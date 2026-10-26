/**
 * @file tests/functional/web_app_h3_ws_func.cpp
 * @brief `uvcpp_web_app::websocket_connect()` 的 HTTP/3 那条腿
 *        （RFC 9220 extended CONNECT，#58）。
 *
 * 与 `web_app_h2_ws_func.cpp` 一一对照：同一张路由表、同一个传输无关的
 * `uvcpp_web_ws_session`，只把承载换成 QUIC。判据也刻意相同 —— 路径参数、
 * `transport() == HVER_30`、回显、关闭各一次。
 */

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_HTTP3_ENABLE && UVCPP_WEB_SERVER_ENABLE

#include <handle/uvcpp_loop.h>
#include <http3/uvcpp_h3_common.h>
#include <http3/uvcpp_h3_connection.h>
#include <http3/uvcpp_h3_ws_connection.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_connection.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_ws.h>

#include "loop_drain.h"
#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "ok: " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << what << std::endl;
  }
}

const int kDeadlineMs = 8000;

struct TestApp {
  uvcpp_web_app app;
  uvcpp_ssl_context server_ctx{tls_mode::SERVER, tls_version::TLS_1_3};

  mutable std::mutex mu;
  std::string path_seen;
  std::string room_seen;
  std::string message_seen;
  int         close_seen     = 0;
  int         transport_seen = 0;

  void build() {
    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN);

    app.websocket_connect(
        "/chat/:room",
        [this](uvcpp_web_ws_request& ws, uvcpp_web_ws_session& s) {
          {
            std::lock_guard<std::mutex> lk(mu);
            path_seen = ws.path();
            if (ws.param("room") != nullptr) room_seen = *ws.param("room");
            transport_seen =
                s.transport() == uvcpp_http_version::HVER_30 ? 3 : 2;
          }
          s.on_text([this, &s](const std::string& m) {
            {
              std::lock_guard<std::mutex> lk(mu);
              message_seen = m;
            }
            s.send_text(m.data(), m.size());
          });
          s.on_close([this](ws_close_code, const std::string&) {
            std::lock_guard<std::mutex> lk(mu);
            ++close_seen;
          });
        });
  }

  /// @return QUIC 端口；<= 0 = 没起来。
  int start() {
    if (!server_ctx.generate_self_signed("localhost", 2048)) return -1;
    app.http_server()->set_quic_ssl_context(&server_ctx);
    if (app.http_server()->listen_quic(0, "127.0.0.1") != 0) return -1;
    const int qport = app.http_server()->quic_listen_port();
    if (app.start_background() != 0) return -1;
    return qport;
  }
  void shutdown() {
    app.stop();
    app.join();
  }
};

}  // namespace

namespace {

void test_app_h3_ws() {
  TestApp t;
  t.build();
  const int qport = t.start();
  check(qport > 0, "app 的 QUIC 端点起来（端口 " + std::to_string(qport) + "）");
  if (qport <= 0) {
    t.shutdown();
    return;
  }

  uvcpp_loop loop;
  // 句柄都在本对象后面声明 ⇒ 析构顺序「句柄 → 本守卫 → 循环」，
  // 补上最后那一次把关闭回调拨完的 `uv_run`（否则整个循环对象漏掉）。
  uvcpp_test::loop_drain drain(&loop);
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  bool connected = false;
  check(client.connect("127.0.0.1", qport,
                       [&](int st) { connected = st == 0; }) == 0,
        "QUIC connect 受理");
  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "QUIC 握手完成");

  uvcpp_h3_connection h3(client.connection(), /*server_side=*/false);
  uvcpp_h3_connection::callbacks h3cbs;
  h3cbs.on_error      = [](uvcpp_h3_connection&, int) {};
  h3cbs.on_disconnect = [](uvcpp_h3_connection&) {};
  check(h3.start(h3cbs) == 0, "客户端 h3 start");

  uvcpp_h3_ws_connection::callbacks wc;
  std::string client_got;
  int         client_close = 0;
  wc.on_text  = [&](const std::string& m) { client_got = m; };
  wc.on_close = [&](ws_close_code, const std::string&) { ++client_close; };

  std::unique_ptr<uvcpp_h3_ws_connection> ws =
      uvcpp_h3_ws_connection::connect(h3, "/chat/lobby", "localhost", wc);
  check(ws != nullptr, "extended CONNECT 发出");
  if (ws == nullptr) {
    t.shutdown();
    return;
  }

  const bool routed = uvcpp_test::wait_until(
      &loop,
      [&] {
        std::lock_guard<std::mutex> lk(t.mu);
        return t.path_seen == "/chat/lobby";
      },
      kDeadlineMs);
  {
    std::lock_guard<std::mutex> lk(t.mu);
    check(routed, "处理器看到路径（实际 \"" + t.path_seen + "\"）");
    check(t.room_seen == "lobby", "路径参数 :room（实际 \"" + t.room_seen + "\"）");
    check(t.transport_seen == 3, "会话报告 HVER_30");
  }

  {
    const int rc = ws->send_text("hello-app-h3", 12);
    check(rc == 0, "发送文本（rc=" + std::to_string(rc) + "）");
  }
  const bool echo = uvcpp_test::wait_until(
      &loop, [&] { return client_got == "hello-app-h3"; }, kDeadlineMs);
  check(echo, "回显到达（实际 \"" + client_got + "\"）");
  {
    std::lock_guard<std::mutex> lk(t.mu);
    check(t.message_seen == "hello-app-h3", "服务端收到文本");
  }

  check(ws->close(ws_close_code::NORMAL, "bye") == 0, "发送 Close");
  const bool server_saw_close = uvcpp_test::wait_until(
      &loop,
      [&] {
        std::lock_guard<std::mutex> lk(t.mu);
        return t.close_seen == 1;
      },
      kDeadlineMs);
  {
    std::lock_guard<std::mutex> lk(t.mu);
    check(server_saw_close,
          "服务端 on_close 恰好一次（实际 " + std::to_string(t.close_seen) + "）");
  }
  check(client_close == 0, "本端主动关闭不补 on_close");

  ws.reset();
  t.shutdown();
}

}  // namespace

int main() {
  std::cout << "websocket_connect() over HTTP/3 (#58)" << std::endl;
  test_app_h3_ws();
  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "all checks passed" << std::endl;
  return 0;
}

#else  // !(webapp && http3 && web server)

int main() {
  std::cout << "SKIP: webapp / http3 disabled" << std::endl;
  return 0;
}

#endif
