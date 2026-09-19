/**
 * @file tests/functional/web_http_client_close_func.cpp
 * @brief 「对端在响应收完之前断开」时，`send()` 的回调**必须**落地。
 *
 * 缺陷（引入于 `7191f2e`，web 模块最初那一提交）：`uvcpp_http_client::on_tcp_close`
 * 有声明、有定义、**零注册点**。本层注册的是传统 `tcp_->read_start()`，而传统式
 * 在 `nread < 0` 时**不调数据回调**，只跑 `fire_close_callbacks()` —— 所以连接没了
 * 就是彻底安静。异步路径上没有任何超时（`timeout_ms` 全在 `*_wait` 系列里），
 * 于是调用方**无限等**。
 *
 * 判据只能是"回调在墙钟上限内确实来了，且带着错误码"：任何半截实现（装了观察者
 * 但没交付、交付了但状态没清）都停在这一条上。
 *
 * 三条场景缺一不可：
 *   - S1 服务端收下请求就关连接、不应答 → 回调必须来，`UV_ECONNRESET`；
 *   - S2 正常收完响应之后服务端再关 → 已经交付过的回调**不许**被叫第二次，
 *     且此后的 `send()` 拿到 `UV_ENOTCONN`（此前是写进一条死 socket、写"成功"）；
 *   - S3 对端回了 404 的头、正文发一半就断 → 交付的状态码必须是**404**，
 *     不是本层默认构造的 200，也不是 `HTTP_STATUS_NONE`。
 *     （h2 上同一次断开会被两个口子看到，见 `web_ssl_h2_client_func.cpp` 场景 4。）
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
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

/// 服务端固定回这一个响应。`Content-Length` 与正文必须一致 —— 客户端按它判断
/// 响应结束，多一个字节就是"永远等不到完整响应"。
const char kResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "pong";

// =========================================================================
// 服务端
// =========================================================================

enum class srv_mode {
  DROP_AFTER_HEAD,   ///< 收全请求头就拆连接，一个字节都不回
  REPLY_THEN_CLOSE,  ///< 回一个完整响应，写完再拆
  HALF_REPLY_THEN_CLOSE,  ///< 回 `404` 的头 + 半个正文就拆（S3）
};

/// S3 用的半截响应：头说 `Content-Length: 4`，正文只给 2 个字节。客户端会一直
/// 等剩下的 2 个字节，等来的是断开 —— 正是"响应没收完"的形状。
const char kHalfResponse[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "no";

struct server_state {
  std::atomic<int> listen_rc{-1};
  std::atomic<int> accepted{0};
  /// @brief 收到的请求字节数。S1 的非空前置靠它 —— 服务端要是没真收到请求，
  /// "客户端等到 ECONNRESET"这条就证明了别的东西。
  std::atomic<int> bytes_in{0};
  std::atomic<int> reply_ok{0};
};

void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                server_state& st, srv_mode mode) {
  uvcpp_tcp_server server;

  // 只有服务端线程碰它（回调都在自己的 loop 上跑）。
  std::string pending;

  server.set_read_callback(
      [&st, &pending, mode](uvcpp_tcp_client& c, const net_read_result& r) {
        if (!r.is_data()) return;
        st.bytes_in.fetch_add(static_cast<int>(r.size));
        pending.append(r.data, r.size);
        if (pending.find("\r\n\r\n") == std::string::npos) return;
        pending.clear();

        if (mode == srv_mode::DROP_AFTER_HEAD) {
          // 请求已经读干净了，所以拆出来的是一次正常 FIN，不是 RST ——
          // 客户端那边看到 `UV_EOF`。两条路都会走到同一个关闭通知。
          c.close();
          return;
        }
        const char*      body = kResponse;
        std::size_t      blen = sizeof(kResponse) - 1;
        if (mode == srv_mode::HALF_REPLY_THEN_CLOSE) {
          body = kHalfResponse;
          blen = sizeof(kHalfResponse) - 1;
        }
        const int wrc = c.write(body, blen, [&st, &c](int status) {
          if (status != 0) return;
          st.reply_ok.fetch_add(1);
          // 写完再拆：响应得真的发出去。
          c.close();
        });
        if (wrc != 0) c.close();
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

  explicit test_server(srv_mode mode)
      : port_future(port_promise.get_future()) {
    thread = std::thread(run_server, std::ref(port_promise), std::ref(stop),
                         std::ref(st), mode);
    port = port_future.get();
  }

  void shutdown() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }

  ~test_server() { shutdown(); }

  bool ok() const { return port > 0 && st.listen_rc.load() == 0; }
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

/// 发起一次异步 GET，`cb` 由调用方给。
void async_get(uvcpp_http_client& client, client_probe& p,
               std::function<void(const uvcpp_http_response&, int)> cb) {
  p.send_rc.store(client.send(uvcpp_http_request::make_get("/"), std::move(cb)));
}

// =========================================================================
// S1 —— 本轮修的那件事
// =========================================================================

void scenario_drop_before_reply() {
  std::cout << "[scenario 1] 响应没来就断开：回调必须在墙钟上限内落地" << std::endl;

  test_server srv(srv_mode::DROP_AFTER_HEAD);
  check(srv.ok(), "服务端 listen 成功");
  if (!srv.ok()) return;

  uvcpp_http_client client;
  client_probe      p;

  client.connect("127.0.0.1", srv.port, [&p](int err) {
    p.connect_status.store(err);
    p.connect_fired.store(true);
  });
  check(uvcpp_test::wait_until(loop_of(client),
                               [&p] { return p.connect_fired.load(); },
                               uvcpp_test::kWaitMs),
        "connect 回调在墙钟上限内落地");
  check(p.connect_status.load() == 0, "connect 成功（前置）");
  if (p.connect_status.load() != 0) return;

  async_get(client, p, [&p](const uvcpp_http_response& r, int err) {
    p.resp_err.store(err);
    p.resp_status_code.store(static_cast<int>(r.status_code));
    p.resp_count.fetch_add(1);
  });
  check(p.send_rc.load() == 0, "send() 受理了这次请求（前置）");

  // **这就是本轮修的那条。** 修之前这里必然超时 —— 回调永远不会来。
  check(uvcpp_test::wait_until(loop_of(client),
                               [&p] { return p.resp_count.load() > 0; },
                               uvcpp_test::kWaitMs),
        "对端断开后 send() 的回调在墙钟上限内落地");

  // 前置非空证明：服务端确实收到了请求、确实接受了连接。少了这两条，
  // "等到 ECONNRESET"对"根本没连上"的实现同样成立。
  check(srv.st.accepted.load() == 1, "服务端接受了 1 条连接（前置）");
  check(srv.st.bytes_in.load() > 0, "服务端确实收到了请求字节（前置）");

  check(p.resp_count.load() == 1, "回调恰好一次");
  check(p.resp_err.load() == UV_ECONNRESET,
        "错误码是 UV_ECONNRESET（本层写死的值，与 EOF/RST 无关）");
  check(!client.has_status(HTTP_CLIENT_CONNECTED),
        "断开之后不再声称 CONNECTED");
}

// =========================================================================
// S2 —— 交付过的不许再叫一次；死连接上的 send() 不能再"成功"
// =========================================================================

void scenario_close_after_reply() {
  std::cout << "[scenario 2] 正常响应之后再断开：不许重复交付，send() 必须报 ENOTCONN"
            << std::endl;

  test_server srv(srv_mode::REPLY_THEN_CLOSE);
  check(srv.ok(), "服务端 listen 成功");
  if (!srv.ok()) return;

  uvcpp_http_client client;
  client_probe      p;

  client.connect("127.0.0.1", srv.port, [&p](int err) {
    p.connect_status.store(err);
    p.connect_fired.store(true);
  });
  check(uvcpp_test::wait_until(loop_of(client),
                               [&p] { return p.connect_fired.load(); },
                               uvcpp_test::kWaitMs),
        "connect 回调在墙钟上限内落地");
  if (p.connect_status.load() != 0) return;

  async_get(client, p, [&p](const uvcpp_http_response& r, int err) {
    p.resp_err.store(err);
    p.resp_status_code.store(static_cast<int>(r.status_code));
    p.resp_count.fetch_add(1);
  });

  check(uvcpp_test::wait_until(loop_of(client),
                               [&p] { return p.resp_count.load() > 0; },
                               uvcpp_test::kWaitMs),
        "正常响应在墙钟上限内落地");
  check(p.resp_err.load() == 0, "第一次回调不带错误");
  check(p.resp_status_code.load() == 200, "第一次回调拿到 200");
  check(srv.st.reply_ok.load() == 1, "服务端确实把响应写出去了（前置）");

  // 服务端写完就拆。等客户端把这次断开处理掉 —— 步骤 1 已经交付过了，
  // 所以这一等不能有任何新的交付。
  check(uvcpp_test::wait_until(
            loop_of(client),
            [&client] { return !client.has_status(HTTP_CLIENT_CONNECTED); },
            uvcpp_test::kWaitMs),
        "客户端在墙钟上限内观察到断开（前置）");
  uvcpp_test::pump_for(loop_of(client), 50);

  check(p.resp_count.load() == 1, "断开之后回调**没有**被叫第二次");
  check(p.resp_err.load() == 0, "回调的错误码没有被改写");

  // 此前这里会写进一条死 socket 并报"成功"，调用方只能等到下一次超时。
  const int rc = client.send(uvcpp_http_request::make_get("/"),
                             [](const uvcpp_http_response&, int) {});
  check(rc == UV_ENOTCONN, "死连接上的 send() 返回 UV_ENOTCONN");
}

// =========================================================================
// S3 —— 错误路径上交付的状态码必须是**对端真的说过的**那个
// =========================================================================

/// 缺陷（与 h2 侧同一个，见 `HTTP_STATUS_NONE`）：`pending_resp_` 只在
/// `on_response_complete()` 里填，而错误路径交付的正是它 —— 于是"对端回了 404、
/// 正文发一半就断"这种情况，调用方拿到的是 `200 OK` 配一个 `UV_ECONNRESET`。
/// 对端从没说过 200，那是本层默认构造出来的。
///
/// 判据反过来更难伪造：**必须是 404**，不是 0（没收到过）也不是 200（默认值）。
void scenario_half_reply_then_close() {
  std::cout << "[scenario 3] 404 的头到了、正文没发完就断：状态码必须是 404"
            << std::endl;

  test_server srv(srv_mode::HALF_REPLY_THEN_CLOSE);
  check(srv.ok(), "服务端 listen 成功");
  if (!srv.ok()) return;

  uvcpp_http_client client;
  client_probe      p;

  client.connect("127.0.0.1", srv.port, [&p](int err) {
    p.connect_status.store(err);
    p.connect_fired.store(true);
  });
  check(uvcpp_test::wait_until(loop_of(client),
                               [&p] { return p.connect_fired.load(); },
                               uvcpp_test::kWaitMs),
        "connect 回调在墙钟上限内落地");
  check(p.connect_status.load() == 0, "connect 成功（前置）");
  if (p.connect_status.load() != 0) return;

  async_get(client, p, [&p](const uvcpp_http_response& r, int err) {
    p.resp_err.store(err);
    p.resp_status_code.store(static_cast<int>(r.status_code));
    p.resp_count.fetch_add(1);
  });
  check(p.send_rc.load() == 0, "send() 受理了这次请求（前置）");

  check(uvcpp_test::wait_until(loop_of(client),
                               [&p] { return p.resp_count.load() > 0; },
                               uvcpp_test::kWaitMs),
        "半截响应 + 断开之后回调在墙钟上限内落地");

  // 前置：服务端确实把那个 404 的头写出去过。少了这条，"拿到 404"可能只是
  // 客户端自己编了另一个数 —— 上下文里根本没有 404 这个值。
  check(srv.st.reply_ok.load() == 1, "服务端确实把半截响应写出去了（前置）");

  check(p.resp_count.load() == 1, "回调恰好一次");
  check(p.resp_err.load() == UV_ECONNRESET,
        "错误码是 UV_ECONNRESET（响应没收完）");
  check(p.resp_status_code.load() == 404,
        "状态码是对端说过的那一个（404），既不是默认的 200 也不是 0 —— 实际是 " +
            std::to_string(p.resp_status_code.load()));
}

}  // namespace

int main() {
  std::cout << "=== web_http_client_close_func ===" << std::endl;

  scenario_drop_before_reply();
  scenario_close_after_reply();
  scenario_half_reply_then_close();

  if (g_failures == 0) {
    std::cout << "ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}

#else  // UVCPP_WEB_ENABLE

int main() {
  std::cout << "[SKIP] web_http_client_close_func: UVCPP_WEB_ENABLE off"
            << std::endl;
  return 0;
}

#endif
