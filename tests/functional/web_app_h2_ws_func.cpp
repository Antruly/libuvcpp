/**
 * @file tests/functional/web_app_h2_ws_func.cpp
 * @brief `uvcpp_web_app::websocket_connect()` —— 浏览器那条 h2 WebSocket
 *        （RFC 8441 extended CONNECT，#58）。
 *
 * 与 `h2_ws_func.cpp` 的分工：那一份测**协议层**（两条 `uvcpp_h2_connection`
 * 面对面，自己 accept/connect）；这一份测**框架层** —— 同一条 CONNECT 请求要
 * 穿过 `uvcpp_http_server` 的钩子、按路径命中 `websocket_connect()` 注册的表、
 * 再回到一个传输无关的 `uvcpp_web_ws_session`。任何一处没接上，症状都是
 * "处理器根本没跑"或"501"，而不是帧错。
 *
 * 装置：`uvcpp_web_app` 跑后台线程（端口 0、h2c 先验知识），客户端是主线程上
 * 一条**阻塞** `uvcpp_tcp_client` + `uvcpp_h2_connection` —— 面板上量到的就是
 * 线上真字节。
 */

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_NGHTTP2_ENABLE && UVCPP_WEB_SERVER_ENABLE

#include <http2/uvcpp_h2_connection.h>
#include <http2/uvcpp_h2_ws_connection.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_ws.h>

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

// =========================================================================
// 服务端：app + 后台线程
// =========================================================================

struct TestApp {
  uvcpp_web_app app;

  mutable std::mutex mu;
  std::string path_seen;
  std::string room_seen;
  std::string message_seen;
  int         close_seen    = 0;
  int         transport_seen = 0;  ///< 2 = h2, 3 = h3

  void build() {
    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN)
        .set_h2c_enabled(true);

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

  int start() {
    if (app.start_background() != 0) return -1;
    return app.bound_port();
  }
  void shutdown() {
    app.stop();
    app.join();
  }
};

// =========================================================================
// 客户端
// =========================================================================

void test_app_h2_ws() {
  TestApp t;
  t.build();
  const int port = t.start();
  check(port > 0, "app 起来（端口 " + std::to_string(port) + "）");
  if (port <= 0) return;

  uvcpp_loop loop;
  uvcpp_tcp_client client(&loop);
  const int crc = client.connect_wait("127.0.0.1", port, 3000);
  check(crc == 0, "客户端连上（rc=" + std::to_string(crc) + "）");
  if (crc != 0) {
    t.shutdown();
    return;
  }

  uvcpp_h2_connection h2(&client, /*server_side=*/false);
  uvcpp_h2_session::callbacks h2cbs;
  uvcpp_h2_connection::callbacks h2conn;
  h2conn.on_disconnect = [](uvcpp_h2_connection&) {};
  check(h2.start(h2cbs, h2conn) == 0, "客户端 h2 会话 start");

  uvcpp_h2_ws_connection::callbacks wc;
  std::string client_got;
  int         client_close = 0;
  wc.on_text  = [&](const std::string& m) { client_got = m; };
  wc.on_close = [&](ws_close_code, const std::string&) { ++client_close; };

  // 对端 SETTINGS 到达之前 `connect()` 返回 nullptr（不会留下半条流），
  // 所以这里像真实客户端一样等它。
  std::unique_ptr<uvcpp_h2_ws_connection> ws;
  const bool connected = uvcpp_test::wait_until(
      &loop,
      [&] {
        ws = uvcpp_h2_ws_connection::connect(h2, "/chat/lobby", "localhost", wc);
        return ws != nullptr;
      },
      kDeadlineMs);
  check(connected, "extended CONNECT 发出");
  if (!connected) {
    t.shutdown();
    return;
  }

  // 服务端把请求交给了 `websocket_connect()` 注册的处理器 —— 判据是路径与
  // 参数都到了（不是"连接建起来了"：建起来只说明 accept 成功）。
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
    check(t.transport_seen == 2, "会话报告 HVER_20");
  }

  {
    const int rc = ws->send_text("hello-app-h2", 12);
    check(rc == 0, "发送文本（rc=" + std::to_string(rc) + "）");
  }
  const bool echo = uvcpp_test::wait_until(
      &loop, [&] { return client_got == "hello-app-h2"; }, kDeadlineMs);
  check(echo, "回显到达（实际 \"" + client_got + "\"）");

  {
    std::lock_guard<std::mutex> lk(t.mu);
    check(t.message_seen == "hello-app-h2", "服务端收到文本");
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
  std::cout << "websocket_connect() over HTTP/2 (#58)" << std::endl;
  test_app_h2_ws();
  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "all checks passed" << std::endl;
  return 0;
}

#else  // !(webapp && nghttp2 && web server)

int main() {
  std::cout << "SKIP: webapp / nghttp2 disabled" << std::endl;
  return 0;
}

#endif
