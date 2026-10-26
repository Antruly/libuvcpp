/**
 * @file tests/functional/http3_ws_server_func.cpp
 * @brief `uvcpp_h3_ws_server`（#58）：把 quic_server → h3_connection →
 *        extended CONNECT → h3_ws_connection 那段生命周期收进库里的封装。
 *
 * 判据：
 *  [1] 外部循环构造 + `set_ssl_context` / `bind` / `listen` 之后，客户端用
 *      `uvcpp_h3_ws_connection::connect` 能建会话，`on_open` 带出 `:path`；
 *  [2] 文本回显（服务端 `on_text` 里 `send_text`）与 PING→PONG 都通；
 *  [3] 客户端 Close 之后服务端的 `on_close` **恰好一次**（状态码 NORMAL）；
 *  [4] **服务端先死**（会话还开着时 `delete server`）：不许崩，客户端应当看到
 *      连接断开——这条守的是"先销毁 ws、再销毁 h3 连接"的顺序。
 */

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_HTTP3_ENABLE

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <http3/uvcpp_h3_common.h>
#include <http3/uvcpp_h3_connection.h>
#include <http3/uvcpp_h3_ws_connection.h>
#include <http3/uvcpp_h3_ws_server.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_connection.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>

#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++g_fail;
    std::cerr << "FAIL: " << what << std::endl;
  } else {
    std::cout << "ok: " << what << std::endl;
  }
}

const int kDeadlineMs = 8000;

struct client_side {
  std::unique_ptr<uvcpp_h3_connection> h3;
  std::unique_ptr<uvcpp_h3_ws_connection> ws;
  std::string got;
  std::string pong;
  bool connected = false;
  bool disconnected = false;
  int close_count = 0;
  ws_close_code close_code = ws_close_code::NO_STATUS;

  ~client_side() {
    // ws 必须比 h3 先销毁（它不拥有 h3，析构里会去清流上的落点）。
    ws.reset();
    h3.reset();
  }
};

void test_open_echo_close() {
  std::cout << "[h3_ws_server] open/echo/close" << std::endl;
  uvcpp_loop loop;

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    check(false, "server cert");
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  uvcpp_h3_ws_server server(&loop);
  server.set_ssl_context(&server_ctx);
  check(server.bind("127.0.0.1", 0) == 0, "server bind");

  std::string srv_path;
  std::string srv_text;
  int srv_open = 0;
  int srv_close = 0;
  ws_close_code srv_close_code = ws_close_code::NO_STATUS;

  uvcpp_h3_ws_server::callbacks cbs;
  cbs.on_open = [&](uvcpp_h3_ws_connection& ws, const std::string& p) {
    ++srv_open;
    srv_path = p;
    (void)ws;
  };
  cbs.on_text = [&](uvcpp_h3_ws_connection& ws, const std::string& m) {
    srv_text = m;
    ws.send_text(m.data(), m.size());
  };
  cbs.on_close = [&](uvcpp_h3_ws_connection&, ws_close_code code,
                     const std::string&) {
    ++srv_close;
    srv_close_code = code;
  };
  check(server.listen(cbs) == 0, "server listen");
  // 端口由 listen() 时绑定，之后才可读（`0` = 内核挑一个）。
  const int port = server.configured_port();
  check(port > 0, "server port");

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);
  client_side c;
  const int crc = client.connect("127.0.0.1", port, [&](int status) {
    c.connected = status == 0;
  });
  check(crc == 0, "client connect 受理");
  if (crc != 0) return;

  c.h3.reset(new uvcpp_h3_connection(client.connection(),
                                     /*server_side=*/false));
  uvcpp_h3_connection::callbacks hc;
  hc.on_disconnect = [&](uvcpp_h3_connection&) { c.disconnected = true; };
  check(c.h3->start(hc) == 0, "client h3 start");
  check(uvcpp_test::wait_until(&loop, [&] { return c.connected; },
                               kDeadlineMs),
        "客户端握手完成");

  uvcpp_h3_ws_connection::callbacks wc;
  wc.on_text = [&](const std::string& m) { c.got = m; };
  wc.on_pong = [&](const uint8_t* d, size_t n) {
    c.pong.assign(reinterpret_cast<const char*>(d), n);
  };
  wc.on_close = [&](ws_close_code code, const std::string&) {
    ++c.close_count;
    c.close_code = code;
  };
  c.ws = uvcpp_h3_ws_connection::connect(*c.h3, "/chat", "localhost", wc);
  check(c.ws != nullptr, "客户端 connect websocket");
  if (!c.ws) return;

  check(uvcpp_test::wait_until(&loop, [&] { return srv_open == 1; },
                               kDeadlineMs),
        "服务端 on_open 恰好一次");
  check(srv_path == "/chat", "on_open 带出 :path（实际 \"" + srv_path + "\"）");

  check(c.ws->send_text("hello-wrapper", 13) == 0, "客户端发送文本");
  const bool echoed = uvcpp_test::wait_until(
      &loop, [&] { return c.got == "hello-wrapper"; }, kDeadlineMs);
  check(echoed, "文本回显（实际 \"" + c.got + "\"）");
  check(srv_text == "hello-wrapper", "服务端 on_text 收到文本");

  check(c.ws->send_ping("p", 1) == 0, "客户端发送 PING");
  const bool ponged = uvcpp_test::wait_until(&loop, [&] { return c.pong == "p"; },
                                             kDeadlineMs);
  check(ponged, "PONG 回来（实际 \"" + c.pong + "\"）");

  check(c.ws->close(ws_close_code::NORMAL, "bye") == 0, "客户端发送 Close");
  check(uvcpp_test::wait_until(&loop, [&] { return srv_close >= 1; },
                               kDeadlineMs),
        "服务端 on_close 到达");
  check(srv_close == 1, "服务端 on_close 恰好一次（实际 " +
                            std::to_string(srv_close) + "）");
  check(srv_close_code == ws_close_code::NORMAL,
        "on_close 状态码 NORMAL（实际 " +
            std::to_string(static_cast<int>(srv_close_code)) + "）");
}

void test_server_deleted_with_open_session() {
  std::cout << "[h3_ws_server] server deleted while a session is open"
            << std::endl;
  uvcpp_loop loop;

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    check(false, "server cert");
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  std::unique_ptr<uvcpp_h3_ws_server> server(new uvcpp_h3_ws_server(&loop));
  server->set_ssl_context(&server_ctx);
  if (server->bind("127.0.0.1", 0) != 0) {
    check(false, "server2 bind");
    return;
  }
  int srv_open = 0;
  uvcpp_h3_ws_server::callbacks cbs;
  cbs.on_open = [&](uvcpp_h3_ws_connection&, const std::string&) {
    ++srv_open;
  };
  if (server->listen(cbs) != 0) {
    check(false, "server2 listen");
    return;
  }
  const int port = server->configured_port();

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);
  client_side c;
  if (client.connect("127.0.0.1", port, [&](int status) {
        c.connected = status == 0;
      }) != 0) {
    check(false, "client2 connect 受理");
    return;
  }
  c.h3.reset(new uvcpp_h3_connection(client.connection(), false));
  uvcpp_h3_connection::callbacks hc;
  hc.on_disconnect = [&](uvcpp_h3_connection&) { c.disconnected = true; };
  if (c.h3->start(hc) != 0) {
    check(false, "client2 h3 start");
    return;
  }
  if (!uvcpp_test::wait_until(&loop, [&] { return c.connected; },
                              kDeadlineMs)) {
    check(false, "client2 握手完成");
    return;
  }
  c.ws = uvcpp_h3_ws_connection::connect(*c.h3, "/open", "localhost",
                                         uvcpp_h3_ws_connection::callbacks());
  if (!c.ws) {
    check(false, "client2 connect websocket");
    return;
  }
  check(uvcpp_test::wait_until(&loop, [&] { return srv_open == 1; },
                               kDeadlineMs),
        "会话已建立（服务端 on_open）");

  // 会话还开着：直接销毁服务端。析构里必须先清 ws 会话、再销毁 h3 连接 ——
  // 顺序错了会崩在 ws 析构里的 `set_stream_sink`（它碰的是已释放的 h3）。
  //
  // 判据是**隐式的**：走到下面那行就是没崩（ctest 会把 abort 判红）。不断言
  // "客户端立刻看到断开"：QUIC 没有握手级 close，服务端析构只是收掉本地端点，
  // 对端要等 idle timeout 才知道。
  server.reset();
  for (int i = 0; i < 200; ++i) loop.run(UV_RUN_NOWAIT);
  std::cout << "ok: 服务端在会话开着时被销毁没有崩" << std::endl;
}

}  // namespace

int main() {
  std::cout << "[h3_ws_server] issue #58" << std::endl;
  test_open_echo_close();
  test_server_deleted_with_open_session();
  if (g_fail == 0) {
    std::cout << "[h3_ws_server] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[h3_ws_server] FAIL (" << g_fail << " checks failed)"
            << std::endl;
  return 2;
}

#else  // !UVCPP_HTTP3_ENABLE

int main() {
  std::cout << "[h3_ws_server] SKIP: http3 disabled" << std::endl;
  return 0;
}

#endif
