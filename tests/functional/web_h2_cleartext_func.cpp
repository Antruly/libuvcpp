/**
 * @file tests/functional/web_h2_cleartext_func.cpp
 * @brief 明文 HTTP/2（RFC 9113 §3.4「先验知识」，下称 h2c）：判据全部落在
 *        **确定的字节**上，并且必须把**分界**钉住。
 *
 * 为什么单开一条：`web_ssl_h2_server_func` 把 h2 服务端测得很细，但那条路上
 * 连接是靠 **TLS + ALPN** 认出来的 —— 判据落在 `SSL_get0_alpn_selected()` 上。
 * 明文这条腿没有任何可借用的信号，只能比字节：头 24 个字节是不是
 * `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`。既然判据是字节，本用例的落点也必须
 * 是字节，而且要覆盖这个判定的**两侧与边界**：
 *
 *   1. 判对了 —— 前言完整时服务端真的按 h2 回帧、路由真的跑到（第一、四、六轮）；
 *   2. 判错了 —— 不是前言时一个字节都不能因此丢掉或重复（第二、五轮）；
 *   3. 前言**跨读边界**时不能只看当前这一段（第三、四轮，这是本层唯一的新状态）；
 *   4. 开关关掉时，同一个字节流必须**逐字节**走今天那条 h1 的路（第七轮）。
 *
 * 装置与"为什么这样能把住关"：
 *
 *   - 服务端跑在**后台线程**、绑端口 0，客户端用**阻塞** `uvcpp_tcp_client`
 *     按需分片写、累积读。分片之间留墙钟间隔，于是"服务端确实分段读过"不再
 *     是推断：`set_raw_data_hook`（解析器**之前**、每段原始字节都经过一次）
 *     把每段长度记下来，第三/四/五轮直接断言**段数 ≥ 2**。没有这条自检的话，
 *     "分片前缀"那几轮在最坏情况下退化成"一次性收全"，于是它们判的东西
 *     （累积状态）一个字节都没被走到，而用例照样绿。
 *   - 第一、四、六轮里喂给服务端的字节**不是手写的常量**，是客户端会话
 *     （`uvcpp_h2_session(false)`）自己 `drain()` 出来的真字节，并且断言它
 *     以那 24 个字节开头 —— 装置自检。手写字节能过，不代表线上真客户端能过。
 *   - 判"服务端说了 h2"用两处互不依赖的证据：第四/六轮是响应体 `world`
 *     出现在 DATA 帧的载荷里（h2 的 DATA 载荷是明文，不是 HPACK），
 *     第一轮则是客户端会话按流收到了 200 + `world` —— 只认字节的话，
 *     一个把 h1 响应恰好拼成帧形状的实现也能骗过第一处。
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_NGHTTP2_ENABLE

#include <net/uvcpp_tcp_client.h>
#include <req/uvcpp_work.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

#include <http2/uvcpp_h2_session.h>

using namespace uvcpp;

namespace {

// =========================================================================
// 判据
// =========================================================================

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (cond) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void info(const std::string& what) {
  std::cout << "  [info] " << what << std::endl;
}

/// 一段字节的前 16 个字节的十六进制，排错时看"服务端到底回了什么"用。
std::string hex16(const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  std::string       out;
  const size_t      n = s.size() < 16 ? s.size() : 16;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    out += kHex[c >> 4];
    out += kHex[c & 0x0F];
    out += ' ';
  }
  if (s.size() > n) out += "...";
  return out;
}

/// 把一段 h2 字节按帧头走一遍，打印 `长度/类型/标志/流号`。
///
/// 判据落在字节上（"服务端说了 h2"）时，光说"收到了 44 字节"没有诊断价值：
/// SETTINGS、ACK、PING、HEADERS、DATA 都是 9 字节帧头起头的，只有把它们摊开
/// 才看得出"服务端停在哪一步"—— 只发了自己的 SETTINGS 就再没动静，
/// 与服务端已经回了 HEADERS/DATA 是两件完全不同的事。
std::string frame_dump(const std::string& s) {
  std::string out;
  size_t      off = 0;
  int         n   = 0;
  while (off + 9 <= s.size() && n < 12) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data()) + off;
    const uint32_t len =
        (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) |
        static_cast<uint32_t>(p[2]);
    const uint32_t type = p[3];
    const uint32_t flg  = p[4];
    if (off + 9 + len > s.size()) {
      out += "截断";
      break;
    }
    if (!out.empty()) out += " ";
    out += "len=" + std::to_string(len) + ",type=" + std::to_string(type) +
           ",flag=" + std::to_string(flg);
    off += 9 + len;
    ++n;
  }
  if (out.empty()) out = "没有完整的帧";
  return out;
}

/// RFC 9113 §3.4 的固定前言：一条 h2 客户端连接的**第一段**。
const char  kPreface[]    = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
const size_t kPrefaceLen  = sizeof(kPreface) - 1;  // 24

/// h2 响应体的明文一定出现在 DATA 帧的载荷里（h2 的 DATA 不编码）。
const char kBody[] = "world";

uvcpp_http_request make_req(http_method m, const std::string& url,
                            const std::string& host = "example.com") {
  uvcpp_http_request r;
  r.method = m;
  r.url    = url;
  r.set_header("host", host);
  return r;
}

// =========================================================================
// 服务端：后台线程 + 端口 0
// =========================================================================

struct TestServer {
  std::thread         th;
  std::atomic<bool>   stop{false};
  std::promise<int>   port_promise;
  std::mutex          mtx;
  std::vector<size_t> chunks;  // raw_data_hook_ 看到的每段长度

  /**
   * @param h2c true 同时开 h2 与 h2c 两块开关；false 两块都不开（今天的默认）。
   *
   * 两块都要开是刻意的：`h2c_enabled()` 的判据是
   * 「h2 开着 **且** h2c 开着」—— h2c 是 h2 的一条腿，不是独立协议。
   */
  int start(bool h2c) {
    th = std::thread([this, h2c] {
      uvcpp_http_server server;

      // 解析器**之前**的观测点：每段原始字节到这里一次。只看不接管（返回 true）。
      server.set_raw_data_hook(
          [this](uvcpp_tcp_client*, const char*, size_t len) {
            std::lock_guard<std::mutex> g(mtx);
            chunks.push_back(len);
            return true;
          });

      server.set_http2_enabled(h2c);
      server.set_h2c_enabled(h2c);

      auto plain = [](uvcpp_http_request&, uvcpp_http_response& resp,
                      uvcpp_tcp_client*) {
        resp = uvcpp_http_response::ok(kBody, 5, "text/plain");
      };
      server.get("/plain", plain);
      server.post("/plain", plain);
      server.get("/hello", [](uvcpp_http_request&, uvcpp_http_response& resp,
                              uvcpp_tcp_client*) {
        resp = uvcpp_http_response::ok("hello", 5, "text/plain");
      });

      // 推迟应答（1.6.7）：凭据从 `req.reply` 拿，兑现入口是 `send_reply()`。
      // 这一条钉的是凭据里的**流号**真的落到了 `resp.stream_id` 上 —— h2 的
      // 发送按流号归位，少了那一句，响应会被 `send_h2_response(client, 0, resp)`
      // 丢在 `h2_streams.find(0)` 那一步，而且**一处错误都不打**（客户端看到
      // 的只是超时）。`uvcpp_static_server` 那条路以前正是这个症状。
      //
      // 捕获 `&server` 是安全的：处理函数挂在这个对象上，它长不过 server。
      server.get("/later", [&server](uvcpp_http_request& req,
                                     uvcpp_http_response& resp,
                                     uvcpp_tcp_client*) {
        const uvcpp_http_reply reply = req.reply;  // 按值留一份
        resp.deferred = true;  // 声明"这条响应由我负责"
        uvcpp_work* w = new uvcpp_work();
        w->queue_work(reply.loop(), [](uvcpp_work*) {},
                      [&server, reply](uvcpp_work* self, int) {
                        delete self;
                        if (!server.reply_alive(reply)) return;
                        uvcpp_http_response out =
                            uvcpp_http_response::ok("later", 5, "text/plain");
                        server.send_reply(reply, out);
                      });
      });

      server.bind("127.0.0.1", 0);
      sockaddr_in name;
      int         namelen = sizeof(name);
      server.get_tcp_server()->get_tcp()->getsockname(
          reinterpret_cast<sockaddr*>(&name), &namelen);
      const int port = ntohs(name.sin_port);

      // **先 listen，再发布端口**：反过来的话调用方立刻 connect 会拿到
      // ECONNREFUSED（本用例的客户端没有重试）。
      server.listen();
      port_promise.set_value(port);

      while (!stop.load()) {
        server.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    return port_promise.get_future().get();
  }

  int chunk_count() {
    std::lock_guard<std::mutex> g(mtx);
    return static_cast<int>(chunks.size());
  }

  std::string chunk_sizes() {
    std::lock_guard<std::mutex> g(mtx);
    std::string s;
    for (size_t i = 0; i < chunks.size(); ++i) {
      if (i != 0) s += ",";
      s += std::to_string(chunks[i]);
    }
    return s;
  }

  void reset_chunks() {
    std::lock_guard<std::mutex> g(mtx);
    chunks.clear();
  }

  void shutdown() {
    stop.store(true);
    if (th.joinable()) th.join();
  }
};

// =========================================================================
// 客户端（阻塞）
// =========================================================================

long long ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

/**
 * @brief 依次写出 @p pieces（段与段之间停 @p gap_ms 墙钟），然后**累积读**
 *        直到 @p pred 为真或超过 @p deadline_ms。
 *
 * @return \p pred 最终是否为真。
 *
 * 必须累积读：服务端的 h2 首批字节（我们的 SETTINGS 帧）与响应帧**可能分两次
 * 到达**，而 `read_wait()` 一有数据就返回。只读一次的写法在"响应恰好与前一批
 * 挤在同一段"时绿、否则红 —— 那是把 TCP 分段写进了判据。
 */
bool write_then_read(int port, const std::vector<std::string>& pieces, int gap_ms,
                     std::string& got,
                     const std::function<bool(const std::string&)>& pred,
                     int deadline_ms) {
  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;

  for (size_t i = 0; i < pieces.size(); ++i) {
    if (!pieces[i].empty() &&
        c.write_wait(pieces[i].data(), pieces[i].size(), 3000) != 0) {
      return false;
    }
    if (gap_ms > 0 && i + 1 < pieces.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
    }
  }

  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  for (;;) {
    if (pred(got)) return true;
    if (ms_since(t0) >= deadline_ms) return false;

    uvcpp_buf b;
    const int rv = c.read_wait(b, 30);
    if (rv == 0) {
      if (b.size() > 0 && b.get_const_data() != nullptr) {
        got.append(b.get_const_data(), b.size());
      }
    } else if (rv != UV_ETIMEDOUT) {
      return false;  // 连接没了；后面 pred 也不可能再变真
    }
  }
}

/// 回复是不是一条 **HTTP/1.x** 报文：状态行开头。用来判"这条连接走了 llhttp"。
bool looks_like_h1(const std::string& got) {
  return got.size() >= 9 && got.compare(0, 9, "HTTP/1.1 ") == 0;
}

/**
 * @brief 用客户端会话自己产生一次 h2 客户端连接的**开场字节**：
 *        前言 + SETTINGS + 一条 `GET /plain` 的 HEADERS。
 *
 * 这是装置自检的锚点：第四/五/六轮喂给服务端的必须**就是**这些字节
 * （真客户端会发的那些），不是手写的近似物。
 */
std::string h2_client_hello() {
  uvcpp_h2_session            cli(false);
  uvcpp_h2_session::callbacks cbs;
  cbs.on_fatal = [](uvcpp_h2_session&, int) {};
  if (cli.init(cbs) != 0) return std::string();
  if (cli.submit_request(make_req(http_method::HTTP_GET, "/plain"), "") <= 0) {
    return std::string();
  }
  std::string out;
  if (cli.drain(out) != 0) return std::string();
  return out;
}

/**
 * @brief 第一轮：明文 socket 上一次真正的 h2 往返。
 *
 * 客户端会话（不是 `uvcpp_h2_connection`，本层不需要那条连接对象）产生字节，
 * 经真 socket 打到服务端，服务端回来的字节再喂回会话。判据落在**会话**上：
 * 按流收到了 200 与 `world` —— 这一步同时穿过了前言判定、llhttp 的让位、
 * 与会话的重新武装，任何一处没接上都会表现为超时。
 */
bool h2_roundtrip(int port, int& status, std::string& body, std::string& sent,
                  std::string& raw, const std::string& url = "/plain") {
  uvcpp_h2_session            cli(false);
  uvcpp_h2_session::callbacks cbs;
  bool     done  = false;
  bool     fatal = false;
  int      st    = 0;
  std::string bd;

  cbs.on_response = [&](uvcpp_h2_session&, uvcpp_h2_stream& stream, bool) {
    st = static_cast<int>(stream.response.status_code);
  };
  cbs.on_body = [&](uvcpp_h2_session&, uvcpp_h2_stream&, const char* d, size_t n) {
    bd.append(d, n);
  };
  cbs.on_response_end = [&](uvcpp_h2_session&, uvcpp_h2_stream&) { done = true; };
  cbs.on_fatal        = [&](uvcpp_h2_session&, int) { fatal = true; };

  if (cli.init(cbs) != 0) return false;
  if (cli.submit_request(make_req(http_method::HTTP_GET, url), "") <= 0) {
    return false;
  }

  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;

  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  bool first_batch = true;

  for (;;) {
    std::string out;
    if (cli.drain(out) != 0) break;
    if (!out.empty()) {
      if (first_batch) {
        sent   = out;
        first_batch = false;
      }
      if (c.write_wait(out.data(), out.size(), 3000) != 0) break;
    }
    if (done || fatal) break;
    if (ms_since(t0) >= 4000) break;

    uvcpp_buf b;
    const int rv = c.read_wait(b, 30);
    if (rv == 0) {
      if (b.size() > 0 && b.get_const_data() != nullptr) {
        raw.append(b.get_const_data(), b.size());
        if (cli.recv(b.get_const_data(), b.size()) != 0) break;
      }
    } else if (rv != UV_ETIMEDOUT) {
      break;
    }
  }

  status = st;
  body   = bd;
  return done;
}

// =========================================================================
// 第 1 轮：明文 h2 往返（真 socket、真路由、真会话）
// =========================================================================

void test_h2c_roundtrip() {
  std::cout << "[1] 明文连接上的 h2 往返" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  check(port > 0, "服务端起来了（端口 0 由系统分配）");
  if (port <= 0) return;

  int         status = 0;
  std::string body;
  std::string sent;
  std::string raw;
  const bool  ok = h2_roundtrip(port, status, body, sent, raw);

  // 装置自检：真客户端会话发出来的第一段**必须**以那 24 个字节开头。
  // 不是的话本用例判的不是 h2c —— 它判的是"我们喂了什么"。
  check(sent.size() >= kPrefaceLen &&
            std::memcmp(sent.data(), kPreface, kPrefaceLen) == 0,
        "装置自检：客户端会话发出来的第一段就是 h2 前言");

  info("服务端按 " + std::to_string(srv.chunk_count()) + " 段读入，长度 " +
       srv.chunk_sizes());
  // 服务端回了什么，按帧摊开打出来。判据是"服务端说了 h2"时，光说"收到 44
  // 字节"没有诊断价值 —— 只有摊开才看得出服务端停在哪一步（只发了自己的
  // SETTINGS 就再没动静，与已经回了 HEADERS/DATA 是两件完全不同的事）。
  info("发 " + std::to_string(sent.size()) + " 字节，收 " +
       std::to_string(raw.size()) + " 字节" +
       (raw.empty() ? std::string(" —— 服务端一个字节都没回")
                    : std::string("；帧：" + frame_dump(raw) + "；头 16 字节 " +
                                  hex16(raw))));

  check(ok, "明文连接上收到了完整的 h2 响应");
  check(status == 200, "状态码是 200（实收 " + std::to_string(status) + "）");
  check(body == kBody, "响应体是 world（实收 \"" + body + "\"）");

  srv.shutdown();
}

// =========================================================================
// 第 1 轮之二：明文 h2 上的**推迟应答**（凭据 + send_reply）
// =========================================================================

/**
 * @brief 同一台明文服务端上，走一趟 `resp.deferred` + `req.reply` 的应答。
 *
 * 这一条与第 1 轮打的是同一条腿、同一份前言分流，差别只在处理函数**不在这一次
 * 调用里回话**：它把凭据按值留下、把结果投回循环线程，回来时进度函数早返回了。
 *
 * 判据是"完整的 200 + 那个体"，而**静默丢包正是它的反例**：h2 的发送按
 * `resp.stream_id` 归位，凭据里那个流号要是没搬上去，`send_h2_response()` 会在
 * `h2_streams.find(0)` 上失败并返回 `UV_EINVAL` —— 客户端那边只是一直等到超时，
 * 服务端一处错误都不打。所以这条用例的价值全在"它答出来了"。
 */
void test_h2c_deferred_reply() {
  std::cout << "[1b] 明文 h2 上的推迟应答（req.reply + send_reply）"
            << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  check(port > 0, "服务端起来了（端口 0 由系统分配）");
  if (port <= 0) return;

  int         status = 0;
  std::string body;
  std::string sent;
  std::string raw;
  const bool  ok = h2_roundtrip(port, status, body, sent, raw, "/later");

  info("发 " + std::to_string(sent.size()) + " 字节，收 " +
       std::to_string(raw.size()) + " 字节" +
       (raw.empty() ? std::string(" —— 服务端一个字节都没回")
                    : std::string("；帧：" + frame_dump(raw))));

  check(ok, "推迟那条也收到了完整的 h2 响应（不是静默超时）");
  check(status == 200, "推迟那条的状态码是 200（实收 " +
                           std::to_string(status) + "）");
  check(body == "later", "推迟那条的响应体是 later（实收 \"" + body + "\"）");

  srv.shutdown();
}

// =========================================================================
// 第 2 轮：同一个端口上的 h1 逐字节不变
// =========================================================================

void test_h1_unchanged_on_same_port() {
  std::cout << "[2] 开了 h2c 的同一个端口上，h1 照旧" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  std::string got;
  const bool  ok = write_then_read(
      port, {"GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"}, 0, got,
      [](const std::string& s) { return s.find("hello") != std::string::npos; },
      3000);

  info("回复 " + std::to_string(got.size()) + " 字节");
  check(ok, "h1 请求拿到了响应");
  check(looks_like_h1(got), "回复是一条 HTTP/1.1 报文（不是 h2 帧）");
  check(got.find("200") != std::string::npos, "状态码是 200");
  check(got.find("hello") != std::string::npos, "响应体是 hello");

  srv.shutdown();
}

// =========================================================================
// 第 3 轮：前言一个字一个字地到 —— 不能只看当前这一段
// =========================================================================

void test_preface_byte_at_a_time() {
  std::cout << "[3] 前言按 1 字节分片到达" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  const std::string hello = h2_client_hello();
  check(hello.size() > kPrefaceLen && hello.compare(0, kPrefaceLen, kPreface) == 0,
        "装置自检：客户端会话给出了真字节（前言 + 帧）");
  if (hello.size() <= kPrefaceLen) {
    srv.shutdown();
    return;
  }

  // 24 个 1 字节的段，段间 10 ms。服务端循环 1 ms 一轮 —— 要让这些段全被
  // 合成一次读，得服务端线程连着 23 次被剥夺 10 ms 以上，实际上不可能。
  std::vector<std::string> pieces;
  for (size_t i = 0; i < kPrefaceLen; ++i) pieces.push_back(hello.substr(i, 1));
  pieces.push_back(hello.substr(kPrefaceLen));

  std::string got;
  const bool  ok = write_then_read(
      port, pieces, 10, got,
      [](const std::string& s) { return s.find(kBody) != std::string::npos; },
      4000);

  info("服务端按 " + std::to_string(srv.chunk_count()) + " 段读入，长度 " +
       srv.chunk_sizes());
  // **判据的前提**：分片真的发生了。否则这一轮判的与第 6 轮无异。
  check(srv.chunk_count() >= 2, "前提：前言确实跨了多个读段（否则本轮的判据是空的）");
  check(ok, "分片前言仍然被判成 h2 —— 响应体 world 出现在回包里");

  srv.shutdown();
}

// =========================================================================
// 第 4 轮：边界 —— 分片正好落在第 23/24 字节之间
// =========================================================================

void test_preface_split_at_last_byte() {
  std::cout << "[4] 前言在第 23/24 字节之间被切开" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  const std::string hello = h2_client_hello();
  if (hello.size() <= kPrefaceLen) {
    check(false, "装置自检：客户端会话给出了真字节");
    srv.shutdown();
    return;
  }

  // 攒了 23 个字节（**没比完**）之后，最后那 1 个字节单独到。这一格是
  // `have + matched == kH2cPrefaceLen` 的边界：`have > 0` 且 `matched > 0`，
  // 两处「还没比完就 return」的条件都不能成立。
  std::string got;
  const bool  ok = write_then_read(
      port,
      {hello.substr(0, kPrefaceLen - 1), hello.substr(kPrefaceLen - 1)},
      /*gap_ms=*/20, got,
      [](const std::string& s) { return s.find(kBody) != std::string::npos; },
      4000);

  info("服务端按 " + std::to_string(srv.chunk_count()) + " 段读入，长度 " +
       srv.chunk_sizes());
  check(srv.chunk_count() >= 2, "前提：前言确实跨了多个读段");
  check(ok, "跨在第 23/24 字节之间的前言仍然判成 h2");

  srv.shutdown();
}

// =========================================================================
// 第 5 轮：攒下的前缀不许丢（判岔了要走 h1，且字节一个不少）
// =========================================================================

void test_buffered_prefix_replayed_on_mismatch() {
  std::cout << "[5] 攒下的近似前缀必须原样补喂给 h1 解析器" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  // "P" 与 `PRI...` 的第 1 个字节相同 ⇒ 会被攒进 `h2c_probe` 而**不进解析器**。
  // 第二个段岔开 ⇒ 判定为 h1，这时那个攒下的字节必须**按原样补喂**。
  //
  // 判据为什么是 200：`P` 丢了的话，llhttp 看到的是 `OST /plain HTTP/1.1`，
  // 请求行不是任何已知方法 ⇒ 400（而不是 200）。反过来若把本段也重复喂一次，
  // 请求行就成了 `PPOST ...`，同样不是 200。所以这一格同时钉住"丢了"和"重了"。
  std::string got;
  const bool  ok = write_then_read(
      port,
      {std::string("P"),
       std::string("OST /plain HTTP/1.1\r\nHost: h\r\ncontent-length: 0\r\n\r\n")},
      /*gap_ms=*/20, got,
      [](const std::string& s) { return s.find("\r\n\r\n") != std::string::npos; },
      3000);

  info("服务端按 " + std::to_string(srv.chunk_count()) + " 段读入，长度 " +
       srv.chunk_sizes());
  check(srv.chunk_count() >= 2, "前提：两段确实分开读了");
  info("回复首行 " + got.substr(0, got.find("\r\n") == std::string::npos
                                        ? got.size()
                                        : got.find("\r\n")));
  check(looks_like_h1(got), "岔开之后走的是 h1 那条路");
  check(got.compare(0, 12, "HTTP/1.1 200") == 0,
        "状态行是 200 —— 攒下的 \"P\" 被原样补喂，既没丢也没重");

  srv.shutdown();
}

// =========================================================================
// 第 6 轮：岔在第 24 个字节上
// =========================================================================

void test_preface_diverges_at_last_byte() {
  std::cout << "[6] 前 23 个字节对、第 24 个字节岔开" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  // 与第 4 轮同一格边界，但这次第 24 个字节是岔的：`have = 23`、`matched = 0`，
  // 既没比完也不该攒 —— 定案 h1，并把那 23 个字节补喂给 llhttp。
  std::string got;
  const bool  ok = write_then_read(
      port,
      {std::string(kPreface, kPrefaceLen - 1),
       std::string("XGET /hello HTTP/1.1\r\nHost: h\r\n\r\n")},
      /*gap_ms=*/20, got,
      [](const std::string& s) { return s.find("\r\n\r\n") != std::string::npos; },
      3000);

  info("服务端按 " + std::to_string(srv.chunk_count()) + " 段读入，长度 " +
       srv.chunk_sizes());
  check(srv.chunk_count() >= 2, "前提：两段确实分开读了");
  check(looks_like_h1(got), "岔在第 24 个字节上 ⇒ 走 h1 那条路");
  check(got.find(kBody) == std::string::npos,
        "没有按 h2 派发（响应里不该出现路由的 body）");
  check(ok, "服务端给了回复（没有静默丢连接）");

  srv.shutdown();
}

// =========================================================================
// 第 7 轮：对照组 —— 关掉开关时**逐字节**走今天那条路
// =========================================================================

void test_h2c_disabled_control() {
  std::cout << "[7] 对照组：h2c 关掉，同一个字节流仍走 h1" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/false);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  const std::string hello = h2_client_hello();
  check(hello.size() > kPrefaceLen, "装置自检：客户端会话给出了真字节");

  // 把**完整**的 h2 开场字节一次性打到一个没开 h2c 的服务端上。它必须原样
  // 落到 llhttp 上：请求行 `PRI * HTTP/2.0` 不是任何已知方法 ⇒ 400。
  // 这一轮是前六轮的对照：没有它，"服务端说了 h2"证明不了是那道开关干的。
  std::string got;
  const bool  ok = write_then_read(
      port, {hello}, 0, got,
      [](const std::string& s) { return s.find("\r\n") != std::string::npos; },
      3000);

  info("回复 " + std::to_string(got.size()) + " 字节，首行 " +
       got.substr(0, got.find("\r\n") == std::string::npos ? got.size()
                                                          : got.find("\r\n")));
  check(ok, "关掉开关时服务端仍然回了话");
  check(looks_like_h1(got), "回复是一条 HTTP/1.1 报文 —— 前言被当成畸形请求行");
  check(got.find(kBody) == std::string::npos, "没有按 h2 派发（没有路由的 body）");

  srv.shutdown();
}

}  // namespace

int main() {
  std::cout << "[functional web_h2_cleartext] 明文 HTTP/2（先验知识 h2c）"
            << std::endl;

  test_h2c_roundtrip();
  test_h2c_deferred_reply();
  test_h1_unchanged_on_same_port();
  test_preface_byte_at_a_time();
  test_preface_split_at_last_byte();
  test_buffered_prefix_replayed_on_mismatch();
  test_preface_diverges_at_last_byte();
  test_h2c_disabled_control();

  std::cout << "[functional web_h2_cleartext] done failures=" << g_failures
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}

#else

int main() {
  std::cout << "[functional web_h2_cleartext] [skip] web/nghttp2 disabled"
            << std::endl;
  return 0;
}

#endif  // UVCPP_WEB_ENABLE && UVCPP_NGHTTP2_ENABLE
