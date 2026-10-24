/**
 * @file src/redis/uvcpp_redis.cpp
 * @brief 内置 RESP2/RESP3 的客户端：URL 解析、协商、命令 / 管线 / 订阅。
 * @author zhuweiye
 * @version 1.7.5
 *
 * 传输层直接复用 `uvcpp_tcp_client`，协议层是私有的 `uvcpp_redis_resp.h`。本文件
 * 只做三件事：把 URL 变成连接参数、把 RESP 的"请求/应答"与 `tcp_client` 的
 * "写一笔/读一段"对上、把生命周期收干净。
 *
 * 本类的所有内部回调都只捕获 `weak_ptr`，并且每个入口先把自己锁成 `shared_ptr`。
 * 这不是防御性编程，是必需的：用户完全可以在命令回调里 `delete` 掉客户端，而那时
 * 我们正踩在解析循环的栈帧上 —— 锁住之后，对象活到回调栈解开为止。
 */

#include <redis/uvcpp_redis.h>

#if UVCPP_REDIS_ENABLE

#include <redis/uvcpp_redis_resp.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <net/uvcpp_tcp_client.h>
#include <uvcpp/uvcpp_buf.h>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_context.h>
#endif

#include <uv.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace uvcpp {

namespace {

// ===========================================================================
// URL
// ===========================================================================

struct redis_url {
  redis_url() : tls(false), port(6379), has_auth(false), db(0) {}

  bool        tls;
  std::string host;
  int         port;
  std::string user;
  std::string pass;
  bool        has_auth;
  int         db;
};

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/// RFC 3986 的 `%XX` 解码；非法转义返回 false（不猜用户想要什么）。
bool percent_decode(const std::string& in, std::string& out) {
  out.clear();
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '%') {
      out.push_back(in[i]);
      continue;
    }
    if (i + 2 >= in.size()) return false;
    const int hi = hex_value(in[i + 1]);
    const int lo = hex_value(in[i + 2]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<char>((hi << 4) | lo));
    i += 2;
  }
  return true;
}

bool parse_port(const std::string& s, int& port) {
  if (s.empty() || s.size() > 5) return false;
  int v = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  if (v <= 0 || v > 65535) return false;
  port = v;
  return true;
}

/**
 * @brief 解析 `redis://[user[:pass]@]host[:port][/db]` 与 `rediss://`。
 *
 * 只取连接需要的那部分。查询串忽略：Redis 的 URL 里没有标准化过的查询参数，
 * 猜一个出来比直接不看更坏。
 */
int parse_redis_url(const std::string& raw, redis_url& u, std::string& err) {
  u = redis_url();
  err.clear();

  const std::size_t sep = raw.find("://");
  if (sep == std::string::npos) {
    err = "Redis URL 缺少 scheme（要 redis:// 或 rediss://）: " + raw;
    return UV_EINVAL;
  }

  std::string scheme = raw.substr(0, sep);
  for (std::size_t i = 0; i < scheme.size(); ++i) {
    if (scheme[i] >= 'A' && scheme[i] <= 'Z') {
      scheme[i] = static_cast<char>(scheme[i] + 32);
    }
  }
  if (scheme == "redis") {
    u.tls = false;
  } else if (scheme == "rediss") {
    u.tls = true;
  } else {
    err = "不支持的 scheme（只认 redis:// 与 rediss://）: " + scheme;
    return UV_ENOTSUP;
  }

  const std::size_t pos = sep + 3;
  std::size_t auth_end = raw.find_first_of("/?", pos);
  if (auth_end == std::string::npos) auth_end = raw.size();

  std::string authority = raw.substr(pos, auth_end - pos);

  const std::size_t at = authority.rfind('@');
  if (at != std::string::npos) {
    const std::string userinfo = authority.substr(0, at);
    authority = authority.substr(at + 1);

    const std::size_t colon = userinfo.find(':');
    std::string raw_user;
    std::string raw_pass;
    if (colon == std::string::npos) {
      // `redis://:pass@host` 与 `redis://pass@host` 都按"只有密码"处理：
      // AUTH 对"没有用户名"有自己的默认用户，这里不替服务器编一个。
      raw_pass = userinfo;
    } else {
      raw_user = userinfo.substr(0, colon);
      raw_pass = userinfo.substr(colon + 1);
    }
    if (!percent_decode(raw_user, u.user) ||
        !percent_decode(raw_pass, u.pass)) {
      err = "用户名/密码里的百分号转义不合法";
      return UV_EINVAL;
    }
    u.has_auth = true;
  }

  if (authority.empty()) {
    err = "Redis URL 里没有主机名: " + raw;
    return UV_EINVAL;
  }

  if (authority[0] == '[') {
    const std::size_t rb = authority.find(']');
    if (rb == std::string::npos) {
      err = "IPv6 主机缺右方括号: " + authority;
      return UV_EINVAL;
    }
    u.host = authority.substr(1, rb - 1);
    if (rb + 1 < authority.size()) {
      if (authority[rb + 1] != ':' ||
          !parse_port(authority.substr(rb + 2), u.port)) {
        err = "IPv6 主机后面的端口不合法: " + authority;
        return UV_EINVAL;
      }
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      u.host = authority.substr(0, colon);
      if (!parse_port(authority.substr(colon + 1), u.port)) {
        err = "端口不合法: " + authority;
        return UV_EINVAL;
      }
    } else {
      u.host = authority;
    }
  }
  if (u.host.empty()) {
    err = "Redis URL 里没有主机名: " + raw;
    return UV_EINVAL;
  }

  if (auth_end < raw.size() && raw[auth_end] == '/') {
    std::size_t path_end = raw.find('?', auth_end);
    if (path_end == std::string::npos) path_end = raw.size();
    std::string path = raw.substr(auth_end + 1, path_end - auth_end - 1);
    while (!path.empty() && path[path.size() - 1] == '/') {
      path.erase(path.size() - 1);
    }
    if (!path.empty()) {
      if (path.size() > 6) {
        err = "库号不合法: " + path;
        return UV_EINVAL;
      }
      int db = 0;
      for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] < '0' || path[i] > '9') {
          err = "库号不合法: " + path;
          return UV_EINVAL;
        }
        db = db * 10 + (path[i] - '0');
      }
      u.db = db;
    }
  }
  return 0;
}

typedef std::chrono::steady_clock::time_point deadline_t;

deadline_t deadline_after(int ms) {
  return std::chrono::steady_clock::now() +
         std::chrono::milliseconds(ms > 0 ? ms : 0);
}

bool expired(const deadline_t& d) {
  return std::chrono::steady_clock::now() >= d;
}

/// 同步 `*_wait` 的泵步长。1 ms 与仓库里其它 `*_wait` 一致。
const std::chrono::milliseconds kPumpStep(1);

}  // namespace

// ===========================================================================
// 实现
// ===========================================================================

typedef std::function<void(int, const std::vector<uvcpp_redis_value>&)> reply_cb;

class uvcpp_redis_client_impl
    : public std::enable_shared_from_this<uvcpp_redis_client_impl> {
 public:
  explicit uvcpp_redis_client_impl(uvcpp_loop* external);
  ~uvcpp_redis_client_impl();

  /// 构造完成后调用：此时 `shared_from_this()` 才可用。
  void attach();

  // ---- 配置 ----
  int resp_version;
  int connect_timeout_ms;
  int command_timeout_ms;

  std::unique_ptr<uvcpp_loop>       owned_loop;
  uvcpp_loop*                       loop;
  std::unique_ptr<uvcpp_tcp_client> tcp;
#if UVCPP_OPENSSL_ENABLE
  std::unique_ptr<uvcpp_ssl_context> ssl;
#endif

  enum state_t {
    ST_IDLE,        ///< 还没连，可以直接 connect()
    ST_CONNECTING,  ///< TCP/TLS 已发起，到协商完成为止
    ST_READY,       ///< 可以发命令
    ST_CLOSED       ///< 连接已经关掉；再 connect() 会重建传输
  };
  state_t state;
  bool    shutting_down;

  redis_url                url;
  std::function<void(int)> connect_cb;
  deadline_t               connect_deadline;
  bool                     has_connect_deadline;

  std::vector<std::vector<std::string>> handshake;
  std::size_t                           handshake_idx;

  std::string        in;
  std::size_t        in_off;
  uvcpp_redis_parser parser;

  struct out_chunk {
    std::string data;
    std::size_t sent;
    out_chunk() : sent(0) {}
  };
  std::deque<out_chunk> out_q;
  bool                  writing;

  struct pending_cmd {
    std::size_t                    remaining;
    std::vector<uvcpp_redis_value> values;
    reply_cb                       cb;
    deadline_t                     deadline;
    bool                           has_deadline;
    pending_cmd() : remaining(0), has_deadline(false) {}
  };
  std::deque<pending_cmd> pending_q;

  uvcpp_timer* cmd_timer;
  bool         cmd_timer_running;
  bool         cmd_timer_in_cb;

  std::function<void(const std::string&, const uvcpp_redis_value&)> msg_cb;
  bool        subscribed;
  std::size_t ignored_pushes;

  /// 每次 `connect()` / `close_now()` 加一。异步回调带着自己那一代进来，对不上
  /// 就直接丢弃：否则重连之后，上一条连接的完成回调会去动新连接的状态。
  std::uint64_t gen;

  std::string last_error;

  // ---- 生命周期 ----
  void shutdown();
  void close_now(int code, const std::string& text);
  void rebuild_transport();

  // ---- 连接 ----
  int  connect(const std::string& url_str, std::function<void(int)> cb);
  void on_connect(std::uint64_t g, int status);
  void step_handshake();
  void finish_connect(int status);

  // ---- 命令 ----
  int command(const std::vector<std::string>& args, reply_cb cb,
              bool require_ready);
  int pipeline(const std::vector<std::vector<std::string>>& cmds, reply_cb cb);

  // ---- 传输 ----
  void on_read(std::uint64_t g, uvcpp_buf* b);
  void on_transport_closed();
  void process_in(std::uint64_t g);
  void dispatch(const uvcpp_redis_value& v, bool push);
  void route_message(const uvcpp_redis_value& v);
  void flush_out();
  void fail_pending(int code);
  void clear_io();

  // ---- 超时 ----
  bool ensure_timer();
  void arm_timer();
  void disarm_timer_if_idle();
  void on_tick();
};

uvcpp_redis_client_impl::uvcpp_redis_client_impl(uvcpp_loop* external)
    : resp_version(2),
      connect_timeout_ms(30000),
      command_timeout_ms(0),
      owned_loop(external == nullptr ? new uvcpp_loop() : nullptr),
      loop(external != nullptr ? external : owned_loop.get()),
      state(ST_IDLE),
      shutting_down(false),
      has_connect_deadline(false),
      handshake_idx(0),
      in_off(0),
      writing(false),
      cmd_timer(nullptr),
      cmd_timer_running(false),
      cmd_timer_in_cb(false),
      subscribed(false),
      ignored_pushes(0),
      gen(1) {}

void uvcpp_redis_client_impl::attach() { rebuild_transport(); }

void uvcpp_redis_client_impl::rebuild_transport() {
  // 旧的那份先放掉：`~uvcpp_tcp_client` 自己会把在途句柄的路走对（句柄还活着
  // 就交给它自己的关闭完成回调），这里不需要额外等待。
  tcp.reset(new uvcpp_tcp_client(loop));
  std::weak_ptr<uvcpp_redis_client_impl> w(shared_from_this());
  tcp->add_close_observer([w]() {
    if (std::shared_ptr<uvcpp_redis_client_impl> s = w.lock()) {
      s->on_transport_closed();
    }
  });
}

uvcpp_redis_client_impl::~uvcpp_redis_client_impl() {
  shutting_down = true;
  // 析构里**不回调**待决命令：调用方已经把对象交给我们了，再跑它的闭包没有
  // 意义，而且多半就在它自己的栈帧里。
  pending_q.clear();
  connect_cb = nullptr;
  msg_cb = nullptr;

  if (cmd_timer != nullptr) {
    cmd_timer->stop();
    if (cmd_timer_in_cb) {
      // 正在这个定时器自己的回调栈上（用户回调里删掉了客户端）：包装对象不能
      // 当场释放，否则析构的是正在执行的闭包。句柄单独关掉，包装对象交给关闭
      // 完成回调 —— 那时这一帧早已解开。
      if (!cmd_timer->is_closing()) {
        cmd_timer->close([](uvcpp_handle* wrapper) { delete wrapper; });
      }
    } else {
      delete cmd_timer;
    }
    cmd_timer = nullptr;
  }

  if (state == ST_CONNECTING || state == ST_READY) {
    state = ST_CLOSED;
    if (tcp) tcp->close();
  }
  tcp.reset();

  // 自建循环的收尾：把还在关闭队列里的句柄拨完，否则 `~uvcpp_loop` 的
  // `uv_loop_close` 会 EBUSY，那块内存在它的析构里只能泄漏（见 uvcpp_loop.cpp）。
  // 外部循环这条路上什么都不做 —— 它不是我们的。
  if (owned_loop && loop != nullptr && !loop->is_running()) {
    for (int i = 0; i < 64 && loop->loop_alive(); ++i) {
      loop->run(UV_RUN_NOWAIT);
    }
  }
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void uvcpp_redis_client_impl::shutdown() {
  shutting_down = true;
  connect_cb = nullptr;
  pending_q.clear();
  msg_cb = nullptr;
  if (cmd_timer != nullptr) cmd_timer->stop();
  cmd_timer_running = false;
  if (state == ST_CONNECTING || state == ST_READY) {
    state = ST_CLOSED;
    if (tcp) tcp->close();
  }
}

void uvcpp_redis_client_impl::clear_io() {
  in.clear();
  in_off = 0;
  out_q.clear();
  writing = false;
}

void uvcpp_redis_client_impl::close_now(int code, const std::string& text) {
  if (!text.empty()) last_error = text;
  ++gen;  // 这一代之前发出的所有异步回调从此作废

  if (state == ST_CONNECTING) {
    // 协商途中的失败/主动关闭：连接回调必须收到结果，否则它永远不回来。
    finish_connect(code);
    clear_io();
    return;
  }

  const bool was_open = (state == ST_READY);
  state = ST_CLOSED;
  if (cmd_timer != nullptr) {
    cmd_timer->stop();
    cmd_timer_running = false;
  }
  fail_pending(code);
  clear_io();
  if (was_open && tcp) tcp->close();
}

void uvcpp_redis_client_impl::fail_pending(int code) {
  while (!pending_q.empty()) {
    pending_cmd p = std::move(pending_q.front());
    pending_q.pop_front();
    if (p.cb) p.cb(code, p.values);
  }
}

// ---------------------------------------------------------------------------
// 连接
// ---------------------------------------------------------------------------

int uvcpp_redis_client_impl::connect(const std::string& url_str,
                                     std::function<void(int)> cb) {
  if (shutting_down) return UV_ECANCELED;
  if (state == ST_CONNECTING || state == ST_READY) return UV_EALREADY;
  if (!cb) return UV_EINVAL;

  redis_url u;
  std::string err;
  const int prc = parse_redis_url(url_str, u, err);
  if (prc != 0) {
    last_error = err;
    return prc;
  }

#if UVCPP_OPENSSL_ENABLE
  uvcpp_ssl_context* tls_ctx = nullptr;
  if (u.tls) {
    ssl.reset(new uvcpp_ssl_context(tls_mode::CLIENT));
    if (!ssl->is_ready()) {
      ssl.reset();
      last_error = "rediss:// 需要可用的 OpenSSL 上下文";
      return UV_ENOTSUP;
    }
    // CLIENT 上下文的默认档位就是 `PEER` 且已装入系统信任库（见
    // `uvcpp_ssl_context.h` 的 `set_verify_mode()` 注释），与 `redis-cli --tls`
    // 一致，所以这里**不再**动校验档位。本模块不提供"跳过校验"的开关 ——
    // 需要自签证书的场景应在系统信任库里放 CA。
    tls_ctx = ssl.get();
  }
#else
  if (u.tls) {
    last_error = "这份构建没有 OpenSSL（UVCPP_ENABLE_OPENSSL=OFF）";
    return UV_ENOTSUP;
  }
#endif

  // 上一次留下的传输（失败的、正常关闭的）在这里重建，"改完凭据再连一次"因此
  // 是合法的。
  ++gen;
  rebuild_transport();
  clear_io();

#if UVCPP_OPENSSL_ENABLE
  if (tls_ctx != nullptr) {
    const int trc = tcp->enable_tls(tls_ctx);
    if (trc != 0) {
      last_error = "给连接装 TLS 过滤层失败";
      return trc;
    }
  }
#endif

  url = u;
  connect_cb = std::move(cb);
  state = ST_CONNECTING;
  has_connect_deadline = connect_timeout_ms > 0;
  if (has_connect_deadline) connect_deadline = deadline_after(connect_timeout_ms);

  const std::uint64_t g = gen;
  std::weak_ptr<uvcpp_redis_client_impl> w(shared_from_this());
  const int rc = tcp->connect(url.host.c_str(), url.port, [w, g](int status) {
    if (std::shared_ptr<uvcpp_redis_client_impl> s = w.lock()) {
      s->on_connect(g, status);
    }
  });
  if (rc != 0) {
    state = ST_IDLE;
    last_error = uv_strerror(rc);
    connect_cb = nullptr;
    return rc;
  }
  return 0;
}

void uvcpp_redis_client_impl::on_connect(std::uint64_t g, int status) {
  if (g != gen) return;
  if (status != 0) {
    last_error = uv_strerror(status);
    finish_connect(status);
    return;
  }

  in.clear();
  in_off = 0;
  parser = uvcpp_redis_parser();

  std::weak_ptr<uvcpp_redis_client_impl> w(shared_from_this());
  const int rc = tcp->read_start([w, g](uvcpp_buf* b) {
    if (std::shared_ptr<uvcpp_redis_client_impl> s = w.lock()) {
      s->on_read(g, b);
    }
  });
  if (rc != 0) {
    last_error = uv_strerror(rc);
    finish_connect(rc);
    return;
  }

  handshake.clear();
  handshake_idx = 0;

  if (resp_version == 3) {
    std::vector<std::string> hello;
    hello.push_back("HELLO");
    hello.push_back("3");
    if (url.has_auth) {
      hello.push_back("AUTH");
      hello.push_back(url.user.empty() ? std::string("default") : url.user);
      hello.push_back(url.pass);
    }
    handshake.push_back(hello);
  } else if (url.has_auth) {
    std::vector<std::string> auth;
    auth.push_back("AUTH");
    if (!url.user.empty()) auth.push_back(url.user);
    auth.push_back(url.pass);
    handshake.push_back(auth);
  }
  if (url.db != 0) {
    std::vector<std::string> select;
    select.push_back("SELECT");
    char buf[16];
    uvcpp_snprintf(buf, sizeof(buf), "%d", url.db);
    select.push_back(buf);
    handshake.push_back(select);
  }

  if (handshake.empty()) {
    finish_connect(0);
    return;
  }
  step_handshake();
}

void uvcpp_redis_client_impl::step_handshake() {
  const std::size_t idx = handshake_idx;
  const std::string cmd_name = handshake[idx][0];
  std::weak_ptr<uvcpp_redis_client_impl> w(shared_from_this());

  const int rc = command(
      handshake[idx],
      [w, idx, cmd_name](int status,
                         const std::vector<uvcpp_redis_value>& values) {
        std::shared_ptr<uvcpp_redis_client_impl> s = w.lock();
        if (!s) return;
        if (status != 0) {
          s->finish_connect(status);
          return;
        }
        if (!values.empty() && values[0].is_error()) {
          s->last_error = values[0].as_string();
          // AUTH 失败是凭据问题、HELLO 失败是版本/配置问题、SELECT 失败是库号
          // 问题 —— 调用方该采取的动作不一样，状态码也就不该一样。
          s->finish_connect(cmd_name == "AUTH"
                                ? UV_EACCES
                                : (cmd_name == "HELLO" ? UV_ENOTSUP
                                                       : UV_EPROTO));
          return;
        }
        s->handshake_idx = idx + 1;
        if (s->handshake_idx >= s->handshake.size()) {
          s->finish_connect(0);
        } else {
          s->step_handshake();
        }
      },
      /*require_ready=*/false);
  if (rc != 0) finish_connect(rc);
}

void uvcpp_redis_client_impl::finish_connect(int status) {
  std::function<void(int)> cb = std::move(connect_cb);
  connect_cb = nullptr;

  if (status == 0) {
    state = ST_READY;
    handshake.clear();
    handshake_idx = 0;
    has_connect_deadline = false;
    disarm_timer_if_idle();
  } else {
    if (last_error.empty()) last_error = uv_strerror(status);
    const bool was_open = (state == ST_CONNECTING || state == ST_READY);
    state = ST_CLOSED;
    has_connect_deadline = false;
    if (cmd_timer != nullptr) {
      cmd_timer->stop();
      cmd_timer_running = false;
    }
    fail_pending(status);
    clear_io();
    if (was_open && tcp) tcp->close();
  }
  if (cb) cb(status);
}

// ---------------------------------------------------------------------------
// 命令
// ---------------------------------------------------------------------------

int uvcpp_redis_client_impl::command(const std::vector<std::string>& args,
                                     reply_cb cb, bool require_ready) {
  if (shutting_down) return UV_ECANCELED;
  if (require_ready) {
    if (state != ST_READY) return UV_ENOTCONN;
  } else if (state != ST_CONNECTING && state != ST_READY) {
    return UV_ENOTCONN;
  }
  if (args.empty() || !cb) return UV_EINVAL;

  pending_cmd p;
  p.remaining = 1;
  p.cb = std::move(cb);
  if (command_timeout_ms > 0) {
    p.has_deadline = true;
    p.deadline = deadline_after(command_timeout_ms);
  }
  pending_q.push_back(std::move(p));

  out_chunk chunk;
  uvcpp_redis_parser::encode_command(args, chunk.data);
  out_q.push_back(std::move(chunk));
  flush_out();
  arm_timer();
  return 0;
}

int uvcpp_redis_client_impl::pipeline(
    const std::vector<std::vector<std::string>>& cmds, reply_cb cb) {
  if (shutting_down) return UV_ECANCELED;
  if (state != ST_READY) return UV_ENOTCONN;
  if (cmds.empty() || !cb) return UV_EINVAL;

  pending_cmd p;
  p.remaining = cmds.size();
  p.cb = std::move(cb);
  if (command_timeout_ms > 0) {
    p.has_deadline = true;
    p.deadline = deadline_after(command_timeout_ms);
  }
  pending_q.push_back(std::move(p));

  out_chunk chunk;
  uvcpp_redis_parser::encode_commands(cmds, chunk.data);
  out_q.push_back(std::move(chunk));
  flush_out();
  arm_timer();
  return 0;
}

// ---------------------------------------------------------------------------
// 传输
// ---------------------------------------------------------------------------

void uvcpp_redis_client_impl::on_read(std::uint64_t g, uvcpp_buf* b) {
  if (g != gen) return;
  if (b == nullptr || b->size() == 0 || b->get_const_data() == nullptr) return;
  in.append(b->get_const_data(), b->size());
  process_in(g);
}

void uvcpp_redis_client_impl::process_in(std::uint64_t g) {
  while (g == gen) {
    if (in_off >= in.size()) {
      in.clear();
      in_off = 0;
      return;
    }

    bool push = false;
    uvcpp_redis_value v;
    const uvcpp_redis_parser::result r =
        parser.next(in.data(), in.size(), in_off, v, &push);
    if (r == uvcpp_redis_parser::NEED_MORE) {
      // 已经消费过的前缀别一直留着：一个读块只解析出一半时，下一次 `append`
      // 不该把整块再拷一遍。
      if (in_off > 64 * 1024) {
        in.erase(0, in_off);
        in_off = 0;
      }
      return;
    }
    if (r == uvcpp_redis_parser::BAD) {
      close_now(parser.last_error(), parser.last_error_text());
      return;
    }

    dispatch(v, push);
    if (g != gen) return;  // 回调里关了或重连了：剩下的字节属于旧连接
  }
}

void uvcpp_redis_client_impl::dispatch(const uvcpp_redis_value& v, bool push) {
  if (push) {
    route_message(v);
    return;
  }

  if (!pending_q.empty()) {
    pending_q.front().values.push_back(v);
    if (pending_q.front().values.size() >= pending_q.front().remaining) {
      pending_cmd p = std::move(pending_q.front());
      pending_q.pop_front();
      disarm_timer_if_idle();
      if (p.cb) p.cb(0, p.values);
    }
    return;
  }

  // 没有待决命令时到的应答只可能是订阅消息。RESP2 下它与命令应答是**同一种
  // 编码**（普通数组），所以必须先查待决队列、再查订阅 —— 顺序反了会把命令
  // 应答当消息吞掉。
  if (subscribed) route_message(v);
}

void uvcpp_redis_client_impl::route_message(const uvcpp_redis_value& v) {
  if (v.size() < 2) return;
  const std::string& kind = v.at(0).as_string();

  // 先拷一份：消息回调里删掉客户端是合法的，而那时 `msg_cb` 会被清空 ——
  // 正在执行的正是它的那份存储。
  std::function<void(const std::string&, const uvcpp_redis_value&)> cb = msg_cb;

  if (kind == "message" && v.size() >= 3) {
    if (cb) cb(v.at(1).as_string(), v.at(2));
    return;
  }
  if (kind == "pmessage" && v.size() >= 4) {
    // 与 `redis-cli` 的 `pmessage` 一致：给调用方的是真正的频道名，不是模式。
    if (cb) cb(v.at(2).as_string(), v.at(3));
    return;
  }
  // `subscribe` / `psubscribe` / `unsubscribe` / `punsubscribe` / `pong` 的确认
  // 帧：不携带用户载荷。RESP3 下它们走 push 分支到这里；RESP2 下它们是普通
  // 应答，会先被待决队列吃掉。
  ++ignored_pushes;
}

void uvcpp_redis_client_impl::flush_out() {
  if (writing) return;
  while (!out_q.empty()) {
    out_chunk& c = out_q.front();
    const std::size_t len = c.data.size() - c.sent;
    if (len == 0) {
      out_q.pop_front();
      continue;
    }

    const std::uint64_t g = gen;
    std::weak_ptr<uvcpp_redis_client_impl> w(shared_from_this());
    writing = true;
    const int rc = tcp->write(c.data.data() + c.sent, len, [w, g](int status) {
      std::shared_ptr<uvcpp_redis_client_impl> s = w.lock();
      if (!s || g != s->gen) return;
      s->writing = false;
      if (status != 0) {
        s->close_now(status, uv_strerror(status));
        return;
      }
      if (!s->out_q.empty()) s->out_q.pop_front();
      s->flush_out();
    });

    if (rc == 0) return;  // 完成回调会接着发下一块
    writing = false;
    if (rc == UV_EALREADY) {
      // `uvcpp_tcp_client` 一条连接只允许一笔在途写。正常情况下走不到这里
      // （上面的 `writing` 已经挡住了）；真走到了说明有别人在写这条连接 ——
      // 交给那一笔的完成回调续发，不当失败。
      return;
    }
    close_now(rc, uv_strerror(rc));
    return;
  }
}

void uvcpp_redis_client_impl::on_transport_closed() {
  if (shutting_down) return;
  if (state == ST_CONNECTING) {
    if (last_error.empty()) last_error = "连接在协商完成前被关闭";
    finish_connect(UV_ECONNRESET);
    return;
  }
  if (state == ST_READY) {
    state = ST_CLOSED;
    if (cmd_timer != nullptr) {
      cmd_timer->stop();
      cmd_timer_running = false;
    }
    fail_pending(UV_EOF);
  }
}

// ---------------------------------------------------------------------------
// 超时
// ---------------------------------------------------------------------------

bool uvcpp_redis_client_impl::ensure_timer() {
  if (cmd_timer != nullptr && cmd_timer_running) return true;
  if (cmd_timer == nullptr) cmd_timer = new uvcpp_timer(loop);

  std::weak_ptr<uvcpp_redis_client_impl> w(shared_from_this());
  const int rc = cmd_timer->start(
      [w](uvcpp_timer*) {
        std::shared_ptr<uvcpp_redis_client_impl> s = w.lock();
        if (!s) return;
        s->cmd_timer_in_cb = true;
        s->on_tick();
        s->cmd_timer_in_cb = false;
      },
      100, 100);
  if (rc != 0 && cmd_timer->get_handle() == nullptr) {
    delete cmd_timer;
    cmd_timer = nullptr;
    return false;
  }
  cmd_timer_running = (rc == 0);
  return rc == 0;
}

void uvcpp_redis_client_impl::arm_timer() {
  const bool needed =
      (state == ST_CONNECTING && has_connect_deadline) ||
      (!pending_q.empty() && command_timeout_ms > 0);
  if (!needed) return;
  ensure_timer();
}

void uvcpp_redis_client_impl::disarm_timer_if_idle() {
  if (cmd_timer == nullptr || !cmd_timer_running) return;
  if (state == ST_CONNECTING && has_connect_deadline) return;
  if (!pending_q.empty() && command_timeout_ms > 0) return;
  cmd_timer->stop();
  cmd_timer_running = false;
}

void uvcpp_redis_client_impl::on_tick() {
  if (state == ST_CONNECTING && has_connect_deadline &&
      expired(connect_deadline)) {
    close_now(UV_ETIMEDOUT, "连接/协商超时");
    return;
  }
  if (command_timeout_ms > 0 && !pending_q.empty()) {
    const pending_cmd& p = pending_q.front();
    if (p.has_deadline && expired(p.deadline)) {
      close_now(UV_ETIMEDOUT, "Redis 命令超时");
      return;
    }
  }
  disarm_timer_if_idle();
}

// ===========================================================================
// 公开类
// ===========================================================================

namespace {

/// 同步 `*_wait` 的状态。用堆上的一份而不是栈上的引用捕获：本地超时返回之后，
/// 应答仍可能到达，那时回调要能安全地写进一个还活着的地方。
struct wait_slot {
  wait_slot() : done(false), status(0) {}
  bool                           done;
  int                            status;
  uvcpp_redis_value              value;
  std::vector<uvcpp_redis_value> values;
};

}  // namespace

uvcpp_redis_client::uvcpp_redis_client(uvcpp_loop* loop)
    : impl_(new uvcpp_redis_client_impl(loop)) {
  impl_->attach();
}

uvcpp_redis_client::~uvcpp_redis_client() {
  if (impl_) impl_->shutdown();
}

void uvcpp_redis_client::set_resp_version(int version) {
  if (!impl_) return;
  impl_->resp_version = (version >= 3) ? 3 : 2;
}

void uvcpp_redis_client::set_connect_timeout_ms(int ms) {
  if (!impl_) return;
  impl_->connect_timeout_ms = ms > 0 ? ms : 0;
}

void uvcpp_redis_client::set_command_timeout_ms(int ms) {
  if (!impl_) return;
  impl_->command_timeout_ms = ms > 0 ? ms : 0;
}

int uvcpp_redis_client::connect(const std::string& url,
                                std::function<void(int)> cb) {
  std::shared_ptr<uvcpp_redis_client_impl> s = impl_;
  if (!s) return UV_EINVAL;
  return s->connect(url, std::move(cb));
}

int uvcpp_redis_client::connect_wait(const std::string& url, int timeout_ms) {
  std::shared_ptr<uvcpp_redis_client_impl> s = impl_;
  if (!s) return UV_EINVAL;

  bool done = false;
  int  result = 0;
  const int rc = s->connect(url, [&done, &result](int status) {
    done = true;
    result = status;
  });
  if (rc != 0) return rc;

  const deadline_t stop = deadline_after(timeout_ms);
  while (!done) {
    if (timeout_ms > 0 && expired(stop)) {
      s->close_now(UV_ETIMEDOUT, "连接/协商超时");
      break;
    }
    s->loop->run(UV_RUN_NOWAIT);
    if (done) break;
    std::this_thread::sleep_for(kPumpStep);
  }
  return done ? result : UV_ETIMEDOUT;
}

void uvcpp_redis_client::close() {
  std::shared_ptr<uvcpp_redis_client_impl> s = impl_;
  if (s) s->close_now(UV_ECANCELED, "客户端被关闭");
}

bool uvcpp_redis_client::is_connected() const {
  return impl_ && impl_->state == uvcpp_redis_client_impl::ST_READY;
}

int uvcpp_redis_client::command(
    const std::vector<std::string>& args,
    std::function<void(int, const uvcpp_redis_value&)> cb) {
  std::shared_ptr<uvcpp_redis_client_impl> s = impl_;
  if (!s || !cb) return UV_EINVAL;
  return s->command(
      args,
      [cb](int status, const std::vector<uvcpp_redis_value>& values) {
        cb(status, values.empty() ? uvcpp_redis_value::null() : values[0]);
      },
      /*require_ready=*/true);
}

int uvcpp_redis_client::command_wait(const std::vector<std::string>& args,
                                     uvcpp_redis_value& out, int timeout_ms) {
  std::shared_ptr<uvcpp_redis_client_impl> s = impl_;
  if (!s) return UV_EINVAL;

  std::shared_ptr<wait_slot> slot(new wait_slot());
  const int rc = s->command(
      args,
      [slot](int st, const std::vector<uvcpp_redis_value>& values) {
        slot->status = st;
        slot->value = values.empty() ? uvcpp_redis_value::null() : values[0];
        slot->done = true;
      },
      /*require_ready=*/true);
  if (rc != 0) return rc;

  const deadline_t stop = deadline_after(timeout_ms);
  while (!slot->done) {
    if (timeout_ms > 0 && expired(stop)) return UV_ETIMEDOUT;
    s->loop->run(UV_RUN_NOWAIT);
    if (slot->done) break;
    std::this_thread::sleep_for(kPumpStep);
  }
  out = slot->value;
  return slot->status;
}

int uvcpp_redis_client::pipeline(
    const std::vector<std::vector<std::string>>& commands,
    std::function<void(int, const std::vector<uvcpp_redis_value>&)> cb) {
  std::shared_ptr<uvcpp_redis_client_impl> s = impl_;
  if (!s) return UV_EINVAL;
  return s->pipeline(commands, std::move(cb));
}

int uvcpp_redis_client::pipeline_wait(
    const std::vector<std::vector<std::string>>& commands,
    std::vector<uvcpp_redis_value>& out, int timeout_ms) {
  std::shared_ptr<uvcpp_redis_client_impl> s = impl_;
  if (!s) return UV_EINVAL;

  std::shared_ptr<wait_slot> slot(new wait_slot());
  const int rc = s->pipeline(
      commands,
      [slot](int st, const std::vector<uvcpp_redis_value>& values) {
        slot->status = st;
        slot->values = values;
        slot->done = true;
      });
  if (rc != 0) return rc;

  const deadline_t stop = deadline_after(timeout_ms);
  while (!slot->done) {
    if (timeout_ms > 0 && expired(stop)) return UV_ETIMEDOUT;
    s->loop->run(UV_RUN_NOWAIT);
    if (slot->done) break;
    std::this_thread::sleep_for(kPumpStep);
  }
  out.swap(slot->values);
  return slot->status;
}

int uvcpp_redis_client::subscribe(
    const std::vector<std::string>& channels,
    std::function<void(const std::string&, const uvcpp_redis_value&)> on_message,
    std::function<void(int)> cb) {
  std::shared_ptr<uvcpp_redis_client_impl> s = impl_;
  if (!s) return UV_EINVAL;
  if (channels.empty()) return UV_EINVAL;

  s->msg_cb = std::move(on_message);
  s->subscribed = true;

  std::vector<std::string> args;
  args.push_back("SUBSCRIBE");
  for (std::size_t i = 0; i < channels.size(); ++i) args.push_back(channels[i]);

  reply_cb ack;
  if (cb) {
    ack = [cb](int status, const std::vector<uvcpp_redis_value>&) { cb(status); };
  } else {
    ack = [](int, const std::vector<uvcpp_redis_value>&) {};
  }
  return s->command(args, ack, /*require_ready=*/true);
}

int uvcpp_redis_client::psubscribe(
    const std::vector<std::string>& patterns,
    std::function<void(const std::string&, const uvcpp_redis_value&)> on_message,
    std::function<void(int)> cb) {
  std::shared_ptr<uvcpp_redis_client_impl> s = impl_;
  if (!s) return UV_EINVAL;
  if (patterns.empty()) return UV_EINVAL;

  s->msg_cb = std::move(on_message);
  s->subscribed = true;

  std::vector<std::string> args;
  args.push_back("PSUBSCRIBE");
  for (std::size_t i = 0; i < patterns.size(); ++i) args.push_back(patterns[i]);

  reply_cb ack;
  if (cb) {
    ack = [cb](int status, const std::vector<uvcpp_redis_value>&) { cb(status); };
  } else {
    ack = [](int, const std::vector<uvcpp_redis_value>&) {};
  }
  return s->command(args, ack, /*require_ready=*/true);
}

int uvcpp_redis_client::unsubscribe(const std::vector<std::string>& channels,
                                    std::function<void(int)> cb) {
  std::shared_ptr<uvcpp_redis_client_impl> s = impl_;
  if (!s) return UV_EINVAL;

  std::vector<std::string> args;
  args.push_back("UNSUBSCRIBE");
  for (std::size_t i = 0; i < channels.size(); ++i) args.push_back(channels[i]);

  reply_cb ack;
  if (cb) {
    ack = [cb](int status, const std::vector<uvcpp_redis_value>&) { cb(status); };
  } else {
    ack = [](int, const std::vector<uvcpp_redis_value>&) {};
  }
  // 退订之后（假定没有别的频道）就不再处于订阅模式。服务器在全部退订之后也会
  // 解除 RESP2 的命令限制；这里先把本地状态跟上。
  s->subscribed = false;
  return s->command(args, ack, /*require_ready=*/false);
}

const std::string& uvcpp_redis_client::last_error() const {
  static const std::string kEmpty;
  return impl_ ? impl_->last_error : kEmpty;
}

}  // namespace uvcpp

#endif  // UVCPP_REDIS_ENABLE
