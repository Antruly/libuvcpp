/**
 * @file src/net/uvcpp_socks5_server.cpp
 * @brief SOCKS5 服务端的实现。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 结构：`uvcpp_socks5_server` 只负责监听与接受；每条连接一个
 * `socks5_server_conn`，它把状态机、上游连接、BIND 监听器与 UDP socket
 * 全部留在**连接自己那条循环**上。连接的生命周期由框架的关闭观察者收尾 ——
 * 观察者在管理槽之前跑，所以在那里 `delete this` 是安全的，而且不需要一张
 * 跨线程的会话表。
 */

#include "net/uvcpp_socks5_server.h"

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_tcp.h>
#include <handle/uvcpp_timer.h>
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_socks5_common.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <net/uvcpp_udp_client.h>
#include <uvcpp/uvcpp_buf.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace uvcpp {

namespace {

/** @brief 单向转发的背压高/低水位（字节）。 */
const std::size_t kHighWaterBytes = 1024u * 1024u;
const std::size_t kLowWaterBytes  = 256u * 1024u;

/** @brief 握手缓冲的硬上限（超过即判为恶意/错位）。 */
const std::size_t kMaxHandshakeBytes = 4096;

/** @brief 存活令牌：析构后回调不许再碰 `this`。 */
typedef std::shared_ptr<char> life_token;

life_token make_life_token() { return std::make_shared<char>(0); }

bool token_alive(const life_token& t) { return t && *t == 0; }

/**
 * @brief 文本地址 → 端点。
 *
 * `classify_host()` 需要完整地址，IPv4 文本必须能被 `uv_ip4_addr` 接受；
 * 拿不到时退回 IPV4（只在本地地址意外为空时发生）。
 */
uvcpp_socks5_endpoint endpoint_from_text(const std::string& ip, int port) {
  uvcpp_socks5_atyp atyp = uvcpp_socks5_atyp::IPV4;
  if (!socks5_detail::classify_host(ip, atyp)) {
    return uvcpp_socks5_endpoint(uvcpp_socks5_atyp::IPV4, std::string(), port);
  }
  return uvcpp_socks5_endpoint(atyp, ip, port);
}

/** @brief 通配端点（应答里"没有绑定地址"用）。 */
uvcpp_socks5_endpoint empty_endpoint() {
  return uvcpp_socks5_endpoint(uvcpp_socks5_atyp::IPV4, "0.0.0.0", 0);
}

bool endpoint_to_sockaddr(const uvcpp_socks5_endpoint& ep,
                          struct sockaddr_storage& out) {
  std::memset(&out, 0, sizeof(out));
  if (ep.port <= 0 || ep.port > 65535) return false;
  if (ep.atyp == uvcpp_socks5_atyp::IPV4) {
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    if (uv_ip4_addr(ep.host.c_str(), ep.port, &sa) != 0) return false;
    std::memcpy(&out, &sa, sizeof(sa));
    return true;
  }
  if (ep.atyp == uvcpp_socks5_atyp::IPV6) {
    struct sockaddr_in6 sa;
    std::memset(&sa, 0, sizeof(sa));
    if (uv_ip6_addr(ep.host.c_str(), ep.port, &sa) != 0) return false;
    std::memcpy(&out, &sa, sizeof(sa));
    return true;
  }
  return false;
}

/**
 * @brief 目标 → `sockaddr_storage`（数字地址直接转，域名走 `getaddrinfo`）。
 *
 * 域名解析是**阻塞**的：UDP 中继每个数据报都要有目的地，而 libuv 的
 * `getaddrinfo` 是异步请求对象，塞进这条同步分派里要额外一层排队。为此
 * 结果按 `host:port` 缓存，同一个目标只有第一个数据报会等一次解析。
 */
bool resolve_target(const uvcpp_socks5_endpoint& target,
                    struct sockaddr_storage& out) {
  if (target.atyp != uvcpp_socks5_atyp::DOMAIN_NAME) {
    return endpoint_to_sockaddr(target, out);
  }
  if (target.host.empty() || target.port <= 0 || target.port > 65535) {
    return false;
  }

  char port_text[16];
  uvcpp_snprintf(port_text, sizeof(port_text), "%d", target.port);

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  struct addrinfo* res = nullptr;
  if (::getaddrinfo(target.host.c_str(), port_text, &hints, &res) != 0 ||
      res == nullptr) {
    return false;
  }

  bool ok = false;
  for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
    if (p->ai_family == AF_INET && p->ai_addrlen <= sizeof(struct sockaddr_in)) {
      std::memset(&out, 0, sizeof(out));
      std::memcpy(&out, p->ai_addr, p->ai_addrlen);
      ok = true;
      break;
    }
    if (p->ai_family == AF_INET6 &&
        p->ai_addrlen <= sizeof(struct sockaddr_in6)) {
      std::memset(&out, 0, sizeof(out));
      std::memcpy(&out, p->ai_addr, p->ai_addrlen);
      ok = true;
      break;
    }
  }
  ::freeaddrinfo(res);
  return ok;
}

}  // namespace

// =========================================================================
// 单条会话
// =========================================================================

/** @brief 一条控制连接的完整状态。 */
struct socks5_server_conn {
  /** @brief 协议阶段。 */
  enum class stage {
    GREETING,
    AUTH,
    REQUEST,
    CONNECTING,  ///< 上游 TCP 正在连
    BIND_WAIT,   ///< BIND 已回第一次应答，等对端
    RELAY,       ///< 已建立双向转发
    UDP,         ///< UDP 关联
    CLOSED
  };

  /** @brief 超时计时器的用途。 */
  enum class timer_kind {
    NONE,
    HANDSHAKE,
    CONNECT,
    BIND,
    UDP_IDLE
  };

  /** @brief 一个方向的发送队列（同一时刻只有一笔异步写在飞）。 */
  struct out_queue {
    out_queue() : bytes(0), in_flight(false), paused(false), dst(nullptr), src(nullptr) {}
    std::deque<std::string> q;
    std::size_t             bytes;
    bool                    in_flight;
    bool                    paused;
    uvcpp_tcp_client*       dst;
    uvcpp_tcp_client*       src;
  };

  socks5_server_conn(std::atomic<std::size_t>* counter,
                     const uvcpp_socks5_server_options& opts,
                     const std::function<void(const uvcpp_socks5_session_info&)>&
                         on_session,
                     const uvcpp_socks5_session_end_cb& on_session_end,
                     uvcpp_tcp_client* ctrl);
  ~socks5_server_conn();

  // --- 控制连接 ---
  void on_ctrl_read(uvcpp_buf* buf);
  void on_ctrl_closed();
  void drive();
  void shutdown();

  // --- 握手 ---
  void send_selection(std::uint8_t method);
  void send_reply(uvcpp_socks5_reply rep, const uvcpp_socks5_endpoint& ep,
                  bool close_after);
  void notify_session(int status);

  // --- 三种命令 ---
  void begin_connect();
  void begin_bind();
  void begin_udp();
  void on_upstream_connected(int status);
  void install_upstream_read();
  void on_upstream_event(const net_read_result& r);
  void on_bind_peer(uvcpp_tcp_client* peer);
  void on_udp_datagram(uvcpp_buf* buf, const std::string& ip, int port);

  // --- 转发 ---
  void enqueue(out_queue& q, const char* data, std::size_t len);
  void flush(out_queue& q);
  void maybe_finish_close();

  // --- 计时 ---
  void arm_timer(timer_kind kind, int ms);
  void cancel_timer();
  void on_timeout();

  std::atomic<std::size_t>* counter_;
  life_token                life_;

  uvcpp_socks5_server_options opts_;
  std::function<void(const uvcpp_socks5_session_info&)> on_session_;
  uvcpp_socks5_session_end_cb on_session_end_;
  uvcpp_socks5_session_stats  stats_;
  int                         session_status_;
  bool                        ended_;
  int                         end_status_;

  uvcpp_loop*       loop_;
  uvcpp_tcp_client* ctrl_;
  bool              closing_;

  stage phase_;
  std::string in_;

  // 当前命令（会话回报用）
  uvcpp_socks5_command  command_;
  uvcpp_socks5_endpoint target_;
  uvcpp_socks5_endpoint peer_;
  bool                  notified_;

  // CONNECT / BIND 的数据对端
  uvcpp_tcp_client* upstream_;

  // BIND 的临时监听器
  uvcpp_tcp_server* binder_;
  uvcpp_socks5_endpoint bind_reply_ep_;

  // UDP 关联
  uvcpp_udp_client* udp_;
  bool              udp_client_locked_;
  std::string       udp_client_ip_;
  int               udp_client_port_;
  std::map<std::string, struct sockaddr_storage> udp_dns_cache_;

  // 计时器
  uvcpp_timer* timer_;
  timer_kind   timer_kind_;
  bool         timer_in_cb_;

  // 两个方向的转发队列
  out_queue to_ctrl_;  ///< upstream / binder peer → ctrl
  out_queue to_up_;    ///< ctrl → upstream / binder peer

  /** 应答发完之后主动关（握手中的失败路径）。 */
  bool close_after_flush_;
};

socks5_server_conn::socks5_server_conn(
    std::atomic<std::size_t>* counter,
    const uvcpp_socks5_server_options& opts,
    const std::function<void(const uvcpp_socks5_session_info&)>& on_session,
    const uvcpp_socks5_session_end_cb& on_session_end,
    uvcpp_tcp_client* ctrl)
    : counter_(counter),
      life_(make_life_token()),
      opts_(opts),
      on_session_(on_session),
      on_session_end_(on_session_end),
      session_status_(0),
      ended_(false),
      end_status_(0),
      loop_(ctrl != nullptr ? ctrl->get_loop() : nullptr),
      ctrl_(ctrl),
      closing_(false),
      phase_(stage::GREETING),
      command_(uvcpp_socks5_command::CONNECT),
      notified_(false),
      upstream_(nullptr),
      binder_(nullptr),
      udp_(nullptr),
      udp_client_locked_(false),
      udp_client_port_(0),
      timer_(nullptr),
      timer_kind_(timer_kind::NONE),
      timer_in_cb_(false),
      close_after_flush_(false) {
  stats_.started_ms =
      static_cast<std::int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
  if (ctrl_ != nullptr) {
    // 先记对端地址 —— 会话回报要用。
    std::string ip;
    int         port = 0;
    ctrl_->getPeerAddrs(ip, port);
    peer_ = endpoint_from_text(ip, port);

    to_ctrl_.dst = ctrl_;

    const life_token life = life_;
    ctrl_->add_close_observer([this, life]() {
      if (!token_alive(life)) return;
      on_ctrl_closed();
      // 观察者之后管理槽会 delete 客户端；会话在**这里**结束。
      ctrl_ = nullptr;
      delete this;
    });

    ctrl_->read_start([this, life](uvcpp_buf* b) {
      if (!token_alive(life)) return;
      on_ctrl_read(b);
    });

    if (opts_.handshake_timeout_ms > 0) {
      arm_timer(timer_kind::HANDSHAKE, opts_.handshake_timeout_ms);
    }
  }

  // 计数由 `on_accepted()` 做（在那里才能把"超限就马上关"和计数放进同一次
  // 原子操作里）；构造函数失败时由它的 catch 补一次减，所以这里不再加。
}

socks5_server_conn::~socks5_server_conn() {
  *life_ = 1;  // 之后任何挂着的回调都直接返回

  timer_kind_ = timer_kind::NONE;
  if (timer_ != nullptr) {
    timer_->stop();
    delete timer_;
    timer_ = nullptr;
  }

  if (upstream_ != nullptr) {
    delete upstream_;
    upstream_ = nullptr;
  }
  if (binder_ != nullptr) {
    binder_->stop();
    delete binder_;
    binder_ = nullptr;
  }
  if (udp_ != nullptr) {
    udp_->recv_stop();
    delete udp_;
    udp_ = nullptr;
  }

  // ctrl_ 归框架所有，不能在这里 delete。
  if (counter_ != nullptr) counter_->fetch_sub(1);

  // 会话结束通知：**恰好一次**，放在资源收尾之后（回调不该看到半拆的会话）。
  if (!ended_) {
    ended_ = true;
    if (on_session_end_) {
      uvcpp_socks5_session_info info;
      info.command = command_;
      info.target  = target_;
      info.peer    = peer_;
      info.status  = session_status_ != 0 ? session_status_ : end_status_;
      const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      stats_.ended_ms    = static_cast<std::int64_t>(now);
      stats_.duration_ms = stats_.ended_ms - stats_.started_ms;
      stats_.end_status  = info.status;
      try {
        on_session_end_(info, stats_);
      } catch (...) {
        // 观察回调抛异常不影响收尾。
      }
    }
  }
}

// -------------------------------------------------------------------------
// 控制连接
// -------------------------------------------------------------------------

void socks5_server_conn::on_ctrl_read(uvcpp_buf* buf) {
  if (buf == nullptr || closing_) return;

  if (phase_ == stage::RELAY) {
    stats_.bytes_client_to_target += buf->size();
    enqueue(to_up_, buf->get_const_data(), buf->size());
    return;
  }
  if (phase_ == stage::CLOSED) return;

  // 握手 / 等上游期间收到的字节：CONNECT 是"对端先说了话"（合法，锁在
  // `in_` 里，接通后原样转给上游）；其余阶段继续累积，等着一起解析。
  if (in_.size() + buf->size() > kMaxHandshakeBytes) {
    // 握手帧都有硬上界，攒到这个量说明对端在乱发。
    shutdown();
    return;
  }
  in_.append(buf->get_const_data(), buf->size());

  if (phase_ == stage::GREETING || phase_ == stage::AUTH ||
      phase_ == stage::REQUEST) {
    drive();
  }
}

void socks5_server_conn::on_ctrl_closed() {
  // 控制连接没了，关联/转发全部结束；这里只把状态定住，真正的释放交给
  // 析构（观察者马上就要调它）。
  closing_ = true;
  phase_  = stage::CLOSED;
}

void socks5_server_conn::drive() {
  for (;;) {
    if (closing_) return;

    if (phase_ == stage::GREETING) {
      std::size_t             consumed = 0;
      std::vector<std::uint8_t> methods;
      const socks5_detail::parse_status st =
          socks5_detail::parse_greeting(in_.data(), in_.size(), consumed,
                                        methods);
      if (st == socks5_detail::parse_status::NEED_MORE) return;
      if (st == socks5_detail::parse_status::MALFORMED) {
        // 问候都解不出来：没有合法的应答可发，直接收摊。
        notify_session(UV_EPROTO);
        shutdown();
        return;
      }
      in_.erase(0, consumed);

      bool has_no_auth = false;
      bool has_userpass = false;
      for (std::size_t i = 0; i < methods.size(); ++i) {
        if (methods[i] == static_cast<std::uint8_t>(
                              uvcpp_socks5_method::NO_AUTH)) {
          has_no_auth = true;
        } else if (methods[i] == static_cast<std::uint8_t>(
                                     uvcpp_socks5_method::USERNAME_PASSWORD)) {
          has_userpass = true;
        }
      }

      std::uint8_t chosen = static_cast<std::uint8_t>(
          uvcpp_socks5_method::NO_ACCEPTABLE);
      if (opts_.allow_no_auth && has_no_auth) {
        chosen = static_cast<std::uint8_t>(uvcpp_socks5_method::NO_AUTH);
      } else if (opts_.allow_username_password && opts_.authenticator &&
                 has_userpass) {
        chosen = static_cast<std::uint8_t>(
            uvcpp_socks5_method::USERNAME_PASSWORD);
      }

      send_selection(chosen);
      if (chosen ==
          static_cast<std::uint8_t>(uvcpp_socks5_method::NO_ACCEPTABLE)) {
        // 方法选择本身就是应答（VER + 0xFF），不再追加 REP 帧。
        notify_session(UV_EACCES);
        close_after_flush_ = true;
        maybe_finish_close();
        return;
      }
      phase_ = (chosen == static_cast<std::uint8_t>(
                              uvcpp_socks5_method::NO_AUTH))
                   ? stage::REQUEST
                   : stage::AUTH;
      continue;
    }

    if (phase_ == stage::AUTH) {
      std::size_t consumed = 0;
      std::string user;
      std::string password;
      const socks5_detail::parse_status st = socks5_detail::parse_auth_request(
          in_.data(), in_.size(), consumed, user, password);
      if (st == socks5_detail::parse_status::NEED_MORE) return;
      if (st == socks5_detail::parse_status::MALFORMED) {
        notify_session(UV_EPROTO);
        shutdown();
        return;
      }
      in_.erase(0, consumed);

      bool ok = false;
      if (opts_.authenticator) {
        try {
          ok = opts_.authenticator(user, password);
        } catch (...) {
          ok = false;
        }
      }

      std::string resp;
      resp.push_back(static_cast<char>(1));  // RFC 1929 的 VER 恒为 1
      resp.push_back(static_cast<char>(ok ? 0 : 1));
      enqueue(to_ctrl_, resp.data(), resp.size());

      if (!ok) {
        notify_session(UV_EACCES);
        close_after_flush_ = true;
        maybe_finish_close();
        return;
      }
      phase_ = stage::REQUEST;
      continue;
    }

    if (phase_ == stage::REQUEST) {
      std::size_t              consumed = 0;
      uvcpp_socks5_command     cmd      = uvcpp_socks5_command::CONNECT;
      uvcpp_socks5_endpoint    target;
      const socks5_detail::parse_status st = socks5_detail::parse_request(
          in_.data(), in_.size(), consumed, cmd, target);
      if (st == socks5_detail::parse_status::NEED_MORE) return;
      if (st == socks5_detail::parse_status::MALFORMED) {
        // 请求头都解不出来：同样没有合法的应答可发。
        notify_session(UV_EPROTO);
        shutdown();
        return;
      }
      in_.erase(0, consumed);

      command_ = cmd;
      target_  = target;

      const bool allowed_command =
          (cmd == uvcpp_socks5_command::CONNECT && opts_.allow_connect) ||
          (cmd == uvcpp_socks5_command::BIND && opts_.allow_bind) ||
          (cmd == uvcpp_socks5_command::UDP_ASSOCIATE &&
           opts_.allow_udp_associate);
      if (!allowed_command) {
        notify_session(UV_EACCES);
        send_reply(uvcpp_socks5_reply::COMMAND_UNSUPPORTED, empty_endpoint(),
                   true);
        return;
      }
      if (opts_.allow_target) {
        bool allow = false;
        try {
          allow = opts_.allow_target(target);
        } catch (...) {
          allow = false;
        }
        if (!allow) {
          notify_session(UV_EACCES);
          send_reply(uvcpp_socks5_reply::NOT_ALLOWED, empty_endpoint(), true);
          return;
        }
      }

      cancel_timer();

      if (cmd == uvcpp_socks5_command::CONNECT) {
        begin_connect();
      } else if (cmd == uvcpp_socks5_command::BIND) {
        begin_bind();
      } else {
        begin_udp();
      }
      return;
    }

    return;
  }
}

void socks5_server_conn::shutdown() {
  if (closing_) return;
  closing_ = true;
  phase_   = stage::CLOSED;
  cancel_timer();
  if (ctrl_ != nullptr) ctrl_->close(nullptr);
}

void socks5_server_conn::send_selection(std::uint8_t method) {
  std::string out;
  out.push_back(static_cast<char>(5));
  out.push_back(static_cast<char>(method));
  enqueue(to_ctrl_, out.data(), out.size());
}

void socks5_server_conn::send_reply(uvcpp_socks5_reply rep,
                                    const uvcpp_socks5_endpoint& ep,
                                    bool close_after) {
  const std::string out = socks5_detail::encode_reply(rep, ep);
  enqueue(to_ctrl_, out.data(), out.size());
  if (close_after) {
    close_after_flush_ = true;
    maybe_finish_close();
  }
}

void socks5_server_conn::notify_session(int status) {
  session_status_ = status;
  if (notified_ || !on_session_) return;
  notified_ = true;

  uvcpp_socks5_session_info info;
  info.command = command_;
  info.target  = target_;
  info.peer    = peer_;
  info.status  = status;
  try {
    on_session_(info);
  } catch (...) {
    // 观察回调抛异常不该影响协议状态机。
  }
}

// -------------------------------------------------------------------------
// CONNECT
// -------------------------------------------------------------------------

void socks5_server_conn::begin_connect() {
  phase_ = stage::CONNECTING;

  upstream_ = new uvcpp_tcp_client(loop_);
  to_up_.dst = upstream_;

  const life_token life = life_;
  int rc = 0;
  if (target_.is_domain()) {
    // 域名交给 `connect()` 那条路解析：它是**异步**的（`uv_getaddrinfo`），
    // 不会在事件循环里阻塞。历史行为是只解析 `AF_INET`，与客户端那条腿一致。
    rc = upstream_->connect(
        target_.host.c_str(), target_.port, [this, life](int status) {
          if (!token_alive(life)) return;
          on_upstream_connected(status);
        });
  } else {
    // 数字地址直接走 `connect_sockaddr()`：IPv4/IPv6 都在这一支上，
    // 不经过任何 DNS。
    struct sockaddr_storage ss;
    if (!endpoint_to_sockaddr(target_, ss)) {
      delete upstream_;
      upstream_ = nullptr;
      to_up_.dst = nullptr;
      notify_session(UV_EAFNOSUPPORT);
      send_reply(uvcpp_socks5_reply::ATYP_UNSUPPORTED, empty_endpoint(), true);
      return;
    }
    rc = upstream_->connect_sockaddr(
        reinterpret_cast<const struct sockaddr*>(&ss),
        [this, life](int status) {
          if (!token_alive(life)) return;
          on_upstream_connected(status);
        });
  }

  if (rc != 0) {
    // 同步就失败了（参数非法之类）：**不要**在 connect 的栈里递归，
    // 交给下一轮循环，让回调路径与异步失败完全一致。
    if (upstream_ != nullptr) {
      delete upstream_;
      upstream_ = nullptr;
      to_up_.dst = nullptr;
    }
    notify_session(rc);
    send_reply(socks5_detail::error_to_reply(rc), empty_endpoint(), true);
    return;
  }

  if (opts_.upstream_connect_timeout_ms > 0) {
    arm_timer(timer_kind::CONNECT, opts_.upstream_connect_timeout_ms);
  }
}

void socks5_server_conn::on_upstream_connected(int status) {
  if (closing_) return;
  cancel_timer();

  if (status != 0) {
    if (upstream_ != nullptr) {
      delete upstream_;
      upstream_ = nullptr;
      to_up_.dst = nullptr;
    }
    notify_session(status);
    send_reply(socks5_detail::error_to_reply(status), empty_endpoint(), true);
    return;
  }

  install_upstream_read();
  to_up_.dst = upstream_;

  std::string ip;
  int         port = 0;
  upstream_->getLocalAddrs(ip, port);
  const uvcpp_socks5_endpoint bound = endpoint_from_text(ip, port);

  send_reply(uvcpp_socks5_reply::SUCCEEDED, bound, false);
  phase_ = stage::RELAY;
  notify_session(0);

  // 握手期间就攒下的应用字节（客户端"边说边握手"）。
  if (!in_.empty()) {
    const std::string pending = in_;
    in_.clear();
    enqueue(to_up_, pending.data(), pending.size());
  }
}

void socks5_server_conn::install_upstream_read() {
  if (upstream_ == nullptr) return;
  const life_token life = life_;
  upstream_->read_start_events(
      [this, life](uvcpp_tcp_client&, const net_read_result& r) {
        if (!token_alive(life)) return;
        on_upstream_event(r);
      });
}

void socks5_server_conn::on_upstream_event(const net_read_result& r) {
  if (closing_) return;
  if (r.event == net_read_event::DATA) {
    if (r.data != nullptr && r.size > 0) {
      stats_.bytes_target_to_client += r.size;
      enqueue(to_ctrl_, r.data, r.size);
    }
    return;
  }
  // 对端结束（FIN 或读错）：控制连接也结束。RFC 1928 的隧道没有半关闭语义，
  // 这里不尝试把两个方向拆开。
  if (r.event == net_read_event::READ_ERROR && end_status_ == 0) {
    end_status_ = r.error != 0 ? r.error : UV_EIO;
  }
  shutdown();
}

// -------------------------------------------------------------------------
// BIND
// -------------------------------------------------------------------------

void socks5_server_conn::begin_bind() {
  // 控制连接自己的地址族决定监听器绑哪一族。
  std::string local_ip;
  int         local_port = 0;
  ctrl_->getLocalAddrs(local_ip, local_port);
  const bool use_ipv6 = local_ip.find(':') != std::string::npos;

  // 监听器必须落在**本会话这条循环**上：只有同一线程能安全地 accept 后
  // 立刻接管对端。所以用外部循环那个构造。
  binder_ = new uvcpp_tcp_server(loop_);
  const int brc = use_ipv6 ? binder_->bindIpv6("::", 0)
                           : binder_->bindIpv4("0.0.0.0", 0);
  if (brc != 0) {
    notify_session(brc);
    send_reply(socks5_detail::error_to_reply(brc), empty_endpoint(), true);
    return;
  }

  struct sockaddr_storage ss;
  std::memset(&ss, 0, sizeof(ss));
  int namelen = static_cast<int>(sizeof(ss));
  int bind_port = 0;
  if (binder_->get_tcp() != nullptr &&
      binder_->get_tcp()->getsockname(
          reinterpret_cast<struct sockaddr*>(&ss), &namelen) == 0) {
    if (ss.ss_family == AF_INET) {
      bind_port = ntohs(reinterpret_cast<const struct sockaddr_in*>(&ss)
                            ->sin_port);
    } else if (ss.ss_family == AF_INET6) {
      bind_port = ntohs(reinterpret_cast<const struct sockaddr_in6*>(&ss)
                            ->sin6_port);
    }
  }

  const life_token life = life_;
  const int lrc = binder_->listen([this, life](uvcpp_tcp_client* peer) {
    if (!token_alive(life)) {
      if (peer != nullptr) peer->close(nullptr);
      return;
    }
    on_bind_peer(peer);
  });
  if (lrc != 0) {
    notify_session(lrc);
    send_reply(socks5_detail::error_to_reply(lrc), empty_endpoint(), true);
    return;
  }

  // 第一次应答：告诉客户端连哪里。地址用**控制连接的本地地址**，不是
  // 监听器上的 0.0.0.0 —— 通配地址对客户端没有意义。
  bind_reply_ep_ = endpoint_from_text(local_ip, bind_port);
  send_reply(uvcpp_socks5_reply::SUCCEEDED, bind_reply_ep_, false);
  phase_ = stage::BIND_WAIT;
  if (opts_.bind_accept_timeout_ms > 0) {
    arm_timer(timer_kind::BIND, opts_.bind_accept_timeout_ms);
  }
  // 会话从"绑定完成"起就算建立了；第二次应答（对端到达）不再回报。
  notify_session(0);
}

void socks5_server_conn::on_bind_peer(uvcpp_tcp_client* peer) {
  if (peer == nullptr) return;
  if (closing_ || phase_ != stage::BIND_WAIT || binder_ == nullptr) {
    peer->close(nullptr);
    return;
  }

  uvcpp_tcp_client* owned = binder_->take_client(peer);
  if (owned == nullptr) {
    // 登记表里没有它（已经走了）：交给框架自己收尾。
    peer->close(nullptr);
    return;
  }

  upstream_  = owned;
  to_up_.dst = upstream_;

  // 只接一个对端：关掉监听器，但对象留到析构再删（在它自己的 accept
  // 回调里 delete 它不安全）。
  binder_->stop();
  cancel_timer();

  install_upstream_read();

  std::string ip;
  int         port = 0;
  upstream_->getPeerAddrs(ip, port);
  send_reply(uvcpp_socks5_reply::SUCCEEDED, endpoint_from_text(ip, port),
             false);
  phase_ = stage::RELAY;

  if (!in_.empty()) {
    const std::string pending = in_;
    in_.clear();
    enqueue(to_up_, pending.data(), pending.size());
  }
}

// -------------------------------------------------------------------------
// UDP ASSOCIATE
// -------------------------------------------------------------------------

void socks5_server_conn::begin_udp() {
  std::string local_ip;
  int         local_port = 0;
  ctrl_->getLocalAddrs(local_ip, local_port);
  const bool use_ipv6 = local_ip.find(':') != std::string::npos;

  udp_ = new uvcpp_udp_client(loop_);
  const int rc = use_ipv6 ? udp_->bindIpv6("::", 0)
                          : udp_->bindIpv4("0.0.0.0", 0);
  if (rc != 0) {
    notify_session(rc);
    send_reply(socks5_detail::error_to_reply(rc), empty_endpoint(), true);
    return;
  }

  std::string udp_ip;
  int         udp_port = 0;
  udp_->getLocalAddrs(udp_ip, udp_port);

  // RFC 1928 §7：客户端把数据报发到 BND.ADDR:BND.PORT。回通配地址会让
  // 不少客户端把包发到 127.0.0.1（甚至 0.0.0.0），所以回**控制连接的
  // 本地地址** —— 那是客户端确实能到达的那个 IP。
  const uvcpp_socks5_endpoint relay = endpoint_from_text(local_ip, udp_port);
  send_reply(uvcpp_socks5_reply::SUCCEEDED, relay, false);

  const life_token life = life_;
  const int rrc = udp_->recv_start(
      [this, life](uvcpp_buf* b, const char* ip, int port) {
        if (!token_alive(life)) return;
        on_udp_datagram(b, ip != nullptr ? std::string(ip) : std::string(),
                        port);
      });
  if (rrc != 0) {
    notify_session(rrc);
    shutdown();
    return;
  }

  phase_ = stage::UDP;
  notify_session(0);
  if (opts_.udp_idle_timeout_ms > 0) {
    arm_timer(timer_kind::UDP_IDLE, opts_.udp_idle_timeout_ms);
  }
}

void socks5_server_conn::on_udp_datagram(uvcpp_buf* buf, const std::string& ip,
                                         int port) {
  if (buf == nullptr || closing_ || phase_ != stage::UDP) return;

  if (opts_.udp_idle_timeout_ms > 0) {
    arm_timer(timer_kind::UDP_IDLE, opts_.udp_idle_timeout_ms);
  }

  // 首个数据报的来源锁定为客户端（RFC 1928 §7 的推荐做法）。此后来
  // 自别处的数据报都是目标的应答。
  if (!udp_client_locked_) {
    udp_client_locked_ = true;
    udp_client_ip_     = ip;
    udp_client_port_   = port;
  } else if (ip != udp_client_ip_ || port != udp_client_port_) {
    // 目标 → 客户端：这里的 data 是**目标发来的裸载荷**（封装在上面做）。
    ++stats_.udp_datagrams;
    stats_.udp_bytes += buf->size();
    uvcpp_socks5_endpoint from;
    uvcpp_socks5_atyp     atyp = uvcpp_socks5_atyp::IPV4;
    if (!socks5_detail::classify_host(ip, atyp)) return;
    from = uvcpp_socks5_endpoint(atyp, ip, port);

    std::string frame;
    if (!socks5_detail::encode_udp_datagram(
            frame, from, buf->get_const_data(), buf->size())) {
      return;
    }

    struct sockaddr_storage client_ss;
    if (!endpoint_to_sockaddr(
            endpoint_from_text(udp_client_ip_, udp_client_port_), client_ss)) {
      return;
    }
    // `uvcpp_udp_client` 同一时刻只允许一笔带回调的发送；被拒就丢包
    // （UDP 本来就是尽力而为，丢的账记在 `uvcpp_socks5_udp` 那一层）。
    udp_->send_addr(
        reinterpret_cast<const struct sockaddr*>(&client_ss), frame.data(),
        frame.size(), [](int) {});
    return;
  }

  // 客户端 → 目标。
  uvcpp_socks5_endpoint target;
  std::size_t           payload_off = 0;
  if (socks5_detail::decode_udp_datagram(buf->get_const_data(), buf->size(),
                                         target, payload_off) !=
      socks5_detail::parse_status::OK) {
    return;  // RSV/FRAG/坏头：丢弃
  }
  if (payload_off > buf->size()) return;

  // 客户端 → 目标：只统计载荷（封装头不算，见 stats 的说明）。
  ++stats_.udp_datagrams;
  stats_.udp_bytes += buf->size() - payload_off;

  struct sockaddr_storage target_ss;
  if (target.is_domain()) {
    char key[512];
    uvcpp_snprintf(key, sizeof(key), "%s:%d", target.host.c_str(), target.port);
    std::map<std::string, struct sockaddr_storage>::iterator it =
        udp_dns_cache_.find(key);
    if (it == udp_dns_cache_.end()) {
      struct sockaddr_storage ss;
      if (!resolve_target(target, ss)) return;
      it = udp_dns_cache_.insert(std::make_pair(key, ss)).first;
    }
    target_ss = it->second;
  } else if (!endpoint_to_sockaddr(target, target_ss)) {
    return;
  }

  const char* payload =
      payload_off < buf->size() ? buf->get_const_data() + payload_off : nullptr;
  const std::size_t payload_len =
      payload_off < buf->size() ? buf->size() - payload_off : 0;

  udp_->send_addr(reinterpret_cast<const struct sockaddr*>(&target_ss), payload,
                  payload_len, [](int) {});
}

// -------------------------------------------------------------------------
// 转发
// -------------------------------------------------------------------------

void socks5_server_conn::enqueue(out_queue& q, const char* data,
                                 std::size_t len) {
  if (closing_ || len == 0) return;
  if (q.dst == nullptr) return;

  q.q.push_back(std::string(data, len));
  q.bytes += len;

  if (!q.paused && q.src != nullptr && q.bytes > kHighWaterBytes) {
    q.src->read_pause();
    q.paused = true;
  }
  flush(q);
}

void socks5_server_conn::flush(out_queue& q) {
  if (q.in_flight || q.q.empty() || q.dst == nullptr) {
    maybe_finish_close();
    return;
  }

  q.in_flight = true;
  std::string chunk = q.q.front();
  q.q.pop_front();
  q.bytes -= chunk.size();

  const life_token life = life_;
  const int rc = q.dst->write(chunk.data(), chunk.size(),
                              [this, life, &q](int status) {
                                if (!token_alive(life)) return;
                                q.in_flight = false;
                                if (status != 0) {
                                  if (end_status_ == 0) end_status_ = status;
                                  shutdown();
                                  return;
                                }
                                if (q.paused && q.bytes <= kLowWaterBytes) {
                                  if (q.src != nullptr) q.src->read_resume();
                                  q.paused = false;
                                }
                                flush(q);
                              });

  if (rc != 0) {
    q.in_flight = false;
    shutdown();
    return;
  }
  maybe_finish_close();
}

void socks5_server_conn::maybe_finish_close() {
  if (!close_after_flush_) return;
  if (to_ctrl_.in_flight || !to_ctrl_.q.empty()) return;
  shutdown();
}

// -------------------------------------------------------------------------
// 计时
// -------------------------------------------------------------------------

void socks5_server_conn::arm_timer(timer_kind kind, int ms) {
  if (ms <= 0 || loop_ == nullptr || closing_) return;
  if (timer_ == nullptr) timer_ = new uvcpp_timer(loop_);

  timer_kind_ = kind;
  const life_token life = life_;
  timer_->start(
      [this, life](uvcpp_timer*) {
        if (!token_alive(life)) return;
        on_timeout();
      },
      static_cast<std::uint64_t>(ms), 0);
}

void socks5_server_conn::cancel_timer() {
  timer_kind_ = timer_kind::NONE;
  if (timer_ != nullptr) timer_->stop();
}

void socks5_server_conn::on_timeout() {
  const timer_kind kind = timer_kind_;
  timer_kind_ = timer_kind::NONE;
  if (closing_) return;

  if (kind == timer_kind::HANDSHAKE) {
    notify_session(UV_ETIMEDOUT);
    send_reply(uvcpp_socks5_reply::GENERAL_FAILURE, empty_endpoint(), true);
    return;
  }
  if (kind == timer_kind::CONNECT) {
    if (upstream_ != nullptr) {
      delete upstream_;
      upstream_ = nullptr;
      to_up_.dst = nullptr;
    }
    notify_session(UV_ETIMEDOUT);
    send_reply(uvcpp_socks5_reply::HOST_UNREACHABLE, empty_endpoint(), true);
    return;
  }
  if (kind == timer_kind::BIND) {
    send_reply(uvcpp_socks5_reply::GENERAL_FAILURE, empty_endpoint(), true);
    return;
  }
  if (kind == timer_kind::UDP_IDLE) {
    shutdown();
    return;
  }
}

// =========================================================================
// 服务端
// =========================================================================

uvcpp_socks5_server::uvcpp_socks5_server()
    : loop_(nullptr),
      tcp_(nullptr),
      listening_(false),
      closed_(false),
      bound_port_(-1),
      conn_count_(0) {
  tcp_  = new uvcpp_tcp_server();
  loop_ = tcp_ != nullptr ? tcp_->get_loop() : nullptr;
}

uvcpp_socks5_server::~uvcpp_socks5_server() {
  // `uvcpp_tcp_server` 的析构会先停工作线程、再逐循环关掉登记的客户端；
  // 关闭观察者在那时删掉各自的会话。这里只要不再接受新连接即可。
  if (tcp_ != nullptr) {
    delete tcp_;
    tcp_  = nullptr;
    loop_ = nullptr;
  }

  // 所有会话都已随循环收尾；若还有残留，说明有人把连接的驱动挪到了别的
  // 线程上 —— 那已经超出本类的约定（见头文件），这里只记账不掩盖。
  if (conn_count_.load() != 0) {
    last_error_ = "sessions outlived the server object";
  }
}

uvcpp_loop* uvcpp_socks5_server::get_loop() { return loop_; }

int uvcpp_socks5_server::run(uv_run_mode md) {
  if (tcp_ == nullptr) return UV_EINVAL;
  return tcp_->run(md);
}

int uvcpp_socks5_server::set_options(
    const uvcpp_socks5_server_options& opts) {
  if (listening_) {
    last_error_ = "set_options() must be called before listen()";
    return UV_EBUSY;
  }
  opts_ = opts;
  return 0;
}

int uvcpp_socks5_server::bind(const std::string& host, int port) {
  if (tcp_ == nullptr) {
    last_error_ = "no event loop";
    return UV_EINVAL;
  }
  if (listening_) {
    last_error_ = "bind() must be called before listen()";
    return UV_EBUSY;
  }

  const int rc = tcp_->bind(host.c_str(), port);
  if (rc != 0) {
    last_error_ = uv_strerror(rc);
    return rc;
  }

  struct sockaddr_storage ss;
  std::memset(&ss, 0, sizeof(ss));
  int namelen = static_cast<int>(sizeof(ss));
  if (tcp_->get_tcp() != nullptr &&
      tcp_->get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&ss),
                                   &namelen) == 0) {
    if (ss.ss_family == AF_INET) {
      const struct sockaddr_in* sa =
          reinterpret_cast<const struct sockaddr_in*>(&ss);
      bound_port_ = ntohs(sa->sin_port);
    } else if (ss.ss_family == AF_INET6) {
      const struct sockaddr_in6* sa =
          reinterpret_cast<const struct sockaddr_in6*>(&ss);
      bound_port_ = ntohs(sa->sin6_port);
    }
  }
  return 0;
}

int uvcpp_socks5_server::set_loops(int n) {
  if (tcp_ == nullptr) {
    last_error_ = "no event loop";
    return UV_EINVAL;
  }
  if (listening_) {
    last_error_ = "set_loops() must be called before listen()";
    return UV_EBUSY;
  }
  const int rc = tcp_->set_loops(n);
  if (rc != 0) last_error_ = uv_strerror(rc);
  return rc;
}

void uvcpp_socks5_server::on_accepted(uvcpp_tcp_client* client) {
  if (client == nullptr) return;
  if (closed_ || !listening_) {
    client->close(nullptr);
    return;
  }

  // 计数与上限检查：先加再判，超过就退回来并立刻关。n>1 时这一步在不同
  // 循环上并发，上限因此是"近似"的（可能短暂多出不超过循环数个会话）。
  const std::size_t now = conn_count_.fetch_add(1) + 1;
  if (opts_.max_connections != 0 && now > opts_.max_connections) {
    conn_count_.fetch_sub(1);
    client->close(nullptr);
    return;
  }

  socks5_server_conn* conn = nullptr;
  try {
    conn = new socks5_server_conn(&conn_count_, opts_, on_session_,
                                  on_session_end_, client);
    (void)conn;
  } catch (...) {
    // 对象没建起来，析构不会跑 —— 这里必须把 `on_accepted()` 预支的那一次
    // 计数退回去，否则每失败一次就永久占掉一个槽位。
    conn_count_.fetch_sub(1);
    client->close(nullptr);
  }
}

void uvcpp_socks5_server::set_session_end_callback(
    uvcpp_socks5_session_end_cb cb) {
  on_session_end_ = std::move(cb);
}

int uvcpp_socks5_server::listen(
    std::function<void(const uvcpp_socks5_session_info&)> on_session,
    int backlog) {
  if (tcp_ == nullptr) {
    last_error_ = "no event loop";
    return UV_EINVAL;
  }
  if (listening_) {
    last_error_ = "listen() already called";
    return UV_EBUSY;
  }

  on_session_ = on_session;

  const int rc = tcp_->listen(
      [this](uvcpp_tcp_client* client) { on_accepted(client); }, backlog);
  if (rc != 0) {
    last_error_ = uv_strerror(rc);
    return rc;
  }
  listening_ = true;
  closed_    = false;
  return 0;
}

int uvcpp_socks5_server::port() const { return bound_port_; }

std::size_t uvcpp_socks5_server::connection_count() const {
  return conn_count_.load();
}

void uvcpp_socks5_server::close() {
  closed_    = true;
  listening_ = false;
  if (tcp_ == nullptr) return;
  tcp_->stop();
  tcp_->close_all_clients();
}

const std::string& uvcpp_socks5_server::last_error() const {
  return last_error_;
}

}  // namespace uvcpp
