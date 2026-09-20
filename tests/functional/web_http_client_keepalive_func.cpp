/**
 * @file tests/functional/web_http_client_keepalive_func.cpp
 * @brief `Connection: close` 之后这条连接必须**当场作废**，而且
 *        `set_keep_alive(false)` 要在**线上**兑现。
 *
 * 缺陷：`uvcpp_http_client` 里只有一个 `keep_alive_`，而它**只有写、没有读**
 * （`src/web/uvcpp_http_client.cpp:616` 在响应带 `connection: close` 时置 false，
 * `:701` 是 setter；全仓零读取点）。于是：
 *
 *   - **对端说了 `close`**，本层照旧置着 `HTTP_CLIENT_CONNECTED`。调用方按常规
 *     写法"收完响应接着发第二个请求"，这一发就写进一条对端正要关掉的连接 ——
 *     `write()` 报成功、响应永远不来，而**异步路径上没有任何超时**，调用方只能
 *     等到对端 FIN 才拿到错误。写没写出去、对方收没收到，本层一个字都不说。
 *   - **`set_keep_alive(false)`** 连线上都不体现：请求里照样是
 *     `connection: keep-alive`（`src/web/uvcpp_http_request.cpp:131-132` 兜底加
 *     的那条），这个 setter 的实际效果是**零**。
 *
 * 判据为什么落在"第二个请求**真的没有**写出去"上：光看 `send()` 的返回值，
 * 一个"受理了但悄悄丢掉"的实现同样能返回 `UV_ENOTCONN`。所以每条场景都配一个
 * **服务端侧的计数**，并且取**增量** —— 这是对照组的读法，累计值说明不了问题。
 *
 * 服务端有个刻意的形状：**回完响应不关连接**。对端在响应里说了 `close` 却还挂着
 * 不拆，本层就再也没有"对端断开"这条外部信号可以依赖了（那条路已经由
 * `web_http_client_close_func.cpp` 的 S2 盖住）—— 想靠"等 FIN 到来"蒙混过去的
 * 实现，在这里无处可躲，必须自己把状态结清。
 *
 * 场景：
 *   - K1 响应带 `connection: close` ⇒ 交付之后不许再声称 CONNECTED，第二个请求
 *     必须被拒，且服务端**一个字节都收不到**；
 *   - K2 `set_keep_alive(false)` ⇒ 线上请求必须带 `connection: close`（且不再带
 *     `keep-alive`），交付之后同样不许复用这条连接；
 *   - K3 响应带 `connection: keep-alive, close`（`close` **不在首项**）⇒ 与 K1
 *     逐字同一条判据。RFC 9110 §7.6.1 说 `Connection` 是逗号列表，拿整串去比
 *     `"close"` 会漏判这一条；而漏判的后果就是 K1 那个缺陷原样重演，只是触发
 *     条件更偏。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_client.h>

#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// 响应体的长度必须与 `Content-Length` 一致 —— 客户端按它判断响应结束，
/// 差一个字节就是"永远等不到完整响应"。
const char kCloseResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 4\r\n"
    "Connection: close\r\n"
    "\r\n"
    "pong";

/// 同一个响应，**不说** `close` —— K2 用它把"调用方自己的偏好"和"对端的要求"
/// 分开：这条连接之所以不该被复用，只可能是 `set_keep_alive(false)` 起了作用。
const char kPlainResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "pong";

/// K3 用：`close` **不在首项**的写法。
///
/// RFC 9110 §7.6.1 说 `Connection` 是个逗号列表，`close` 落在哪一项都算数。
/// 拿整串去比 `"close"` 会漏判这一条 —— 而漏判的后果就是 K1 那个缺陷原样重演
/// （往一条对端正要关掉的连接上写第二个请求），只是触发条件更偏。
const char kCloseListResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 4\r\n"
    "Connection: keep-alive, close\r\n"
    "\r\n"
    "pong";

// =========================================================================
// 服务端
// =========================================================================

/// 服务端回哪一条响应。三个场景各用一条 —— K2 那条**不说** `close`，才能把
/// "调用方自己的偏好"和"对端的要求"分开验。
enum class reply_kind {
  PLAIN,       ///< 普通 200（K2）
  CLOSE,       ///< `Connection: close`（K1）
  CLOSE_LIST,  ///< `Connection: keep-alive, close`（K3）
};

const char* reply_body(reply_kind k) {
  switch (k) {
    case reply_kind::CLOSE:      return kCloseResponse;
    case reply_kind::CLOSE_LIST: return kCloseListResponse;
    default:                     return kPlainResponse;
  }
}

struct server_state {
  std::atomic<int> listen_rc{-1};
  std::atomic<int> accepted{0};
  /// @brief 收到的请求字节数。第二个请求"没写出去"的正向证据靠它的**增量**。
  std::atomic<int> bytes_in{0};
  /// @brief 解出来的完整请求头（`\r\n\r\n`）个数。比字节数更硬：字节数涨一点
  ///        可能只是半个头，而它涨一格就意味着**一个完整的请求被对端读到了**。
  std::atomic<int> requests{0};
  std::atomic<int> reply_ok{0};
  std::mutex       mu;
  std::string      raw;  ///< 收到的全部请求字节（K2 判线上报文格式用）
};

void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                server_state& st, reply_kind kind) {
  uvcpp_tcp_server server;

  // 只有服务端线程碰它（回调都在自己的 loop 上跑）。
  std::string pending;

  server.set_read_callback(
      [&st, &pending, kind](uvcpp_tcp_client& c,
                            const net_read_result& r) {
        if (!r.is_data()) return;
        st.bytes_in.fetch_add(static_cast<int>(r.size));
        {
          std::lock_guard<std::mutex> lk(st.mu);
          st.raw.append(r.data, r.size);
        }
        pending.append(r.data, r.size);

        // 一个读事件里可能挤着不止一个请求，所以要**逐个切**，不能只 `find` 一次
        // 就走 —— 只切一次的话第二个请求会被当成"还没收全"，计数停在 1，
        // 那条"第二个请求没到达"的判据就会对着一个假的 1 说话。
        for (;;) {
          const std::size_t pos = pending.find("\r\n\r\n");
          if (pos == std::string::npos) break;
          pending.erase(0, pos + 4);
          st.requests.fetch_add(1);

          const char* body = reply_body(kind);
          // **写完不关。** 见文件头：对端说完 `close` 还挂着不拆，本层就没有
          // "对端断开"这条外部信号可依赖了。
          const int wrc = c.write(body, std::strlen(body), [&st](int status) {
            if (status == 0) st.reply_ok.fetch_add(1);
          });
          if (wrc != 0) {
            c.close();
            return;
          }
        }
      });

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int bound_port = ntohs(name.sin_port);

  rc = server.listen([&st](uvcpp_tcp_client*) { st.accepted.fetch_add(1); }, 128);
  st.listen_rc.store(rc);
  // 端口必须在 `listen()` **之后**才放行：已 bind、尚未 listen 的 socket 是
  // 拒连的，客户端会拿到 ECONNREFUSED 而不是"服务端起来了"。
  port_promise.set_value(rc != 0 ? -1 : bound_port);
  if (rc != 0) return;

  uvcpp_loop* loop = server.get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  uvcpp_test::pump_for(loop, 400);
}

/// 一个跑在独立线程上的服务端，`shutdown()` 幂等。
struct test_server {
  server_state      st;
  std::atomic<bool> stop{false};
  std::promise<int> port_promise;
  std::future<int>  port_future;
  std::thread       thread;
  int               port = -1;

  explicit test_server(reply_kind kind)
      : port_future(port_promise.get_future()) {
    thread = std::thread(run_server, std::ref(port_promise), std::ref(stop),
                         std::ref(st), kind);
    port = port_future.get();
  }

  void shutdown() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }

  ~test_server() { shutdown(); }

  bool ok() const { return port > 0 && st.listen_rc.load() == 0; }

  std::string request_text() {
    std::lock_guard<std::mutex> lk(st.mu);
    return st.raw;
  }
};

// =========================================================================
// 客户端
// =========================================================================

/// 回调都是 `loop->run()` 在**本线程的栈上**跑出来的，没有第二个线程碰它们；
/// 用原子只是为了不让人猜。
struct client_probe {
  std::atomic<bool> connect_fired{false};
  std::atomic<int>  connect_status{-99};
  std::atomic<int>  resp_count{0};
  std::atomic<int>  resp_err{-99};
  std::atomic<int>  resp_status_code{0};
  std::atomic<int>  send_rc{-99};
};

uvcpp_loop* loop_of(uvcpp_http_client& c) {
  return c.get_tcp_client()->get_loop();
}

/// 连上 + 前置断言。连不上就没有下文，"连接被作废"那条判据也无从谈起。
bool connect_or_bail(uvcpp_http_client& client, client_probe& p, int port) {
  client.connect("127.0.0.1", port, [&p](int err) {
    p.connect_status.store(err);
    p.connect_fired.store(true);
  });
  const bool fired = uvcpp_test::wait_until(
      loop_of(client), [&p] { return p.connect_fired.load(); },
      uvcpp_test::kWaitMs);
  check(fired, "connect 回调在墙钟上限内落地");
  if (!fired) return false;
  check(p.connect_status.load() == 0, "connect 成功（前置）");
  return p.connect_status.load() == 0;
}

std::string to_lower(std::string s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c >= 'A' && c <= 'Z') s[i] = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

/// 反空转判据的等待上限。**这是一段墙钟，不是圈数**（见 `wait_util.h`）。
const int kIdleMs = 600;

/// 「第二个请求有没有溜出去」的等待形状。
///
/// **不能写成"泵固定若干毫秒再看一眼"。** 那个数就是拿服务端线程的调度时机当
/// 判据：实测同一份**未修**的库连跑两次，一次绿一次红 —— 红绿取决于运气而不是
/// 被测代码，这种判据在门禁里是负资产。
///
/// 这里的形状把两个方向都钉死：不该发生的事一旦发生就**立刻**返回真（红，
/// 说明它真的溜出去了，且不用等满）；没发生就等满 `kIdleMs` 墙钟再返回假（绿）。
/// 等待上限取的是"服务端线程有充裕机会读到它"，所以绿的结论是"给了 600 ms 都没
/// 读到"，而不是"那一瞬间没读到"。
bool second_request_leaked(test_server& srv, int reqs_before, int bytes_before,
                           uvcpp_loop* loop) {
  return uvcpp_test::wait_until(
      loop,
      [&srv, reqs_before, bytes_before] {
        return srv.st.requests.load() != reqs_before ||
               srv.st.bytes_in.load() != bytes_before;
      },
      kIdleMs);
}

// =========================================================================
// K1 / K3 —— 对端在响应里说了 `close`，这条连接就必须当场作废
// =========================================================================

/// K1 与 K3 逐字同一个形状，只差响应头里 `close` 写在哪一项 —— 所以做成一个
/// 函数。两个场景各有一份**独立**的证据，不是同一条断言跑两遍：
///   - K1 `Connection: close`          —— 基本形状；
///   - K3 `Connection: keep-alive, close` —— RFC 9110 §7.6.1 的逗号列表，
///     `close` 不在首项。整串比较在这里会漏判，而漏判的后果就是本用例要钉的
///     那个缺陷原样重演。
void scenario_peer_says_close(reply_kind kind, const char* label,
                              const char* what) {
  std::cout << "[keepalive " << label << "] " << what << std::endl;

  test_server srv(kind);
  check(srv.ok(), "服务端 listen 成功");
  if (!srv.ok()) return;

  uvcpp_http_client client;
  client_probe      p;
  if (!connect_or_bail(client, p, srv.port)) return;

  p.send_rc.store(client.send(uvcpp_http_request::make_get("/first"),
                              [&p](const uvcpp_http_response& r, int err) {
                                p.resp_err.store(err);
                                p.resp_status_code.store(
                                    static_cast<int>(r.status_code));
                                p.resp_count.fetch_add(1);
                              }));
  check(p.send_rc.load() == 0, "send() 受理了第一个请求（前置）");

  check(uvcpp_test::wait_until(loop_of(client),
                               [&p] { return p.resp_count.load() > 0; },
                               uvcpp_test::kWaitMs),
        "第一个响应在墙钟上限内落地");

  // 前置非空证明：少了这几条，"连接被作废"对"根本没连上/服务端压根没回"的
  // 实现同样成立。
  check(srv.st.accepted.load() == 1, "服务端接受了 1 条连接（前置）");
  check(srv.st.requests.load() == 1, "服务端解出 1 个完整请求（前置）");
  check(srv.st.reply_ok.load() == 1, "服务端把响应写出去了（前置）");
  check(p.resp_err.load() == 0, "第一个回调不带错误（前置）");
  check(p.resp_status_code.load() == 200, "第一个回调拿到 200（前置）");

  // === 判据一 ===
  // 对端在响应里说了 `close`，这条连接就不会再接受第二个请求。本层继续置着
  // CONNECTED，就是在替对端许一个它没有许过的承诺。
  check(!client.has_status(HTTP_CLIENT_CONNECTED),
        "对端说了 close 之后不再声称 CONNECTED");

  const int bytes_before = srv.st.bytes_in.load();
  const int reqs_before  = srv.st.requests.load();

  // === 判据二 ===
  const int rc2 = client.send(uvcpp_http_request::make_get("/second"),
                              [](const uvcpp_http_response&, int) {});
  check(rc2 == UV_ENOTCONN, "作废之后 send() 返回 UV_ENOTCONN");

  // === 反空转 ===
  // 返回值分不出"拒绝"和"受理了但悄悄丢掉"。给服务端 600 ms 墙钟，量它那边
  // 计数的**增量** —— 涨一格就说明那个请求真的出去了。
  const bool leaked =
      second_request_leaked(srv, reqs_before, bytes_before, loop_of(client));
  check(!leaked, "第二个请求没有到达服务端（等满 " + std::to_string(kIdleMs) +
                     " ms，服务端计数零增量；实际 reqs " +
                     std::to_string(reqs_before) + "->" +
                     std::to_string(srv.st.requests.load()) + "，bytes " +
                     std::to_string(bytes_before) + "->" +
                     std::to_string(srv.st.bytes_in.load()) + "）");
}

// =========================================================================
// K2 —— `set_keep_alive(false)` 必须在线上兑现
// =========================================================================

void scenario_set_keep_alive_false_reaches_the_wire() {
  std::cout << "[keepalive K2] set_keep_alive(false)：线上请求必须带 "
               "connection: close"
            << std::endl;

  test_server srv(reply_kind::PLAIN);
  check(srv.ok(), "服务端 listen 成功");
  if (!srv.ok()) return;

  uvcpp_http_client client;
  client_probe      p;
  // 必须在 `connect()` **之前**设：本场景要验的是"调用方自己说了不保持"，
  // 而响应里**没有** `close`（`kPlainResponse`）—— 两者不能混在一起判。
  client.set_keep_alive(false);

  if (!connect_or_bail(client, p, srv.port)) return;

  p.send_rc.store(client.send(uvcpp_http_request::make_get("/first"),
                              [&p](const uvcpp_http_response& r, int err) {
                                p.resp_err.store(err);
                                p.resp_status_code.store(
                                    static_cast<int>(r.status_code));
                                p.resp_count.fetch_add(1);
                              }));
  check(p.send_rc.load() == 0, "send() 受理了第一个请求（前置）");
  check(uvcpp_test::wait_until(loop_of(client),
                               [&p] { return p.resp_count.load() > 0; },
                               uvcpp_test::kWaitMs),
        "第一个响应在墙钟上限内落地");
  check(srv.st.requests.load() == 1, "服务端解出 1 个完整请求（前置）");
  check(srv.st.reply_ok.load() == 1, "服务端把响应写出去了（前置）");

  // === 判据三 ===
  // 头名大小写不敏感，统一压成小写再判。
  const std::string low = to_lower(srv.request_text());
  check(low.find("connection: close") != std::string::npos,
        "线上请求带 connection: close");
  check(low.find("connection: keep-alive") == std::string::npos,
        "线上请求**不再**带 connection: keep-alive");

  // === 判据四 ===
  // 调用方说了不保持，这条连接交付完就不该再被复用 —— 与 K1 同一条规矩，
  // 只是触发条件是偏好而不是响应头。
  check(!client.has_status(HTTP_CLIENT_CONNECTED),
        "set_keep_alive(false) 之后交付完不再声称 CONNECTED");

  const int reqs_before  = srv.st.requests.load();
  const int bytes_before = srv.st.bytes_in.load();
  const int rc2 = client.send(uvcpp_http_request::make_get("/second"),
                              [](const uvcpp_http_response&, int) {});
  check(rc2 == UV_ENOTCONN, "不再复用：第二个 send() 返回 UV_ENOTCONN");
  const bool leaked =
      second_request_leaked(srv, reqs_before, bytes_before, loop_of(client));
  check(!leaked, "第二个请求没有到达服务端（等满 " + std::to_string(kIdleMs) +
                     " ms，服务端计数零增量；实际 reqs " +
                     std::to_string(reqs_before) + "->" +
                     std::to_string(srv.st.requests.load()) + "，bytes " +
                     std::to_string(bytes_before) + "->" +
                     std::to_string(srv.st.bytes_in.load()) + "）");
}

}  // namespace

// =========================================================================
// main
// =========================================================================

int main() {
  std::cout << "[web_http_client_keepalive] start" << std::endl;

  scenario_peer_says_close(reply_kind::CLOSE, "K1",
                           "响应带 connection: close：交付之后不许再复用");
  scenario_set_keep_alive_false_reaches_the_wire();
  scenario_peer_says_close(reply_kind::CLOSE_LIST, "K3",
                           "响应带 connection: keep-alive, close：close 不在首项");

  std::cout << "[web_http_client_keepalive] "
            << (g_failures == 0 ? "ALL PASS" : "FAIL") << " (failures="
            << g_failures << ")" << std::endl;
  return g_failures == 0 ? 0 : 2;
}

#else  // UVCPP_WEB_ENABLE

int main() {
  std::cout << "[web_http_client_keepalive] SKIP (web module disabled)"
            << std::endl;
  return 0;
}

#endif  // UVCPP_WEB_ENABLE
