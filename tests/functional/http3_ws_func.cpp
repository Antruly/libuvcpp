/**
 * @file tests/functional/http3_ws_func.cpp
 * @brief WebSocket over HTTP/3（RFC 9220，extended CONNECT，#53）。
 *
 * 客户端开一条 `:protocol = websocket` 的 CONNECT 流，服务端回 200，双方在
 * 这条流上跑 RFC 6455 帧：文本回显、PING→PONG、Close。
 *
 * 本用例只覆盖无压缩的文本/控制帧；permessage-deflate 没有接到这条腿上。
 */

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_HTTP3_ENABLE

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <http3/uvcpp_h3_common.h>
#include <http3/uvcpp_h3_connection.h>
#include <http3/uvcpp_h3_ws_connection.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
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

void test_ws() {
  uvcpp_loop loop;

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    check(false, "server cert");
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  uvcpp_quic_server server(&loop);
  server.set_ssl_context(&server_ctx);
  if (server.bind("127.0.0.1", 0) != 0) {
    check(false, "server bind");
    return;
  }

  std::unique_ptr<uvcpp_h3_connection> srv_h3;
  std::unique_ptr<uvcpp_h3_ws_connection> srv_ws;
  std::string server_got;
  std::string server_protocol;
  std::string server_path;

  const int lrc = server.listen([&](uvcpp_quic_connection* c) {
    srv_h3.reset(new uvcpp_h3_connection(c, /*server_side=*/true));
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_connect_request = [&](uvcpp_h3_connection& h3,
                                 const h3_request& req) {
      server_protocol = req.protocol;
      server_path     = req.path;
      uvcpp_h3_ws_connection::callbacks wc;
      wc.on_text = [&](const std::string& m) {
        server_got = m;
        if (srv_ws) srv_ws->send_text(m.data(), m.size());
      };
      wc.on_ping = [&](const uint8_t*, size_t) {};
      srv_ws = uvcpp_h3_ws_connection::accept(h3, req, wc);
      check(srv_ws != nullptr, "服务端 accept websocket");
    };
    cbs.on_error = [&](uvcpp_h3_connection&, int code) {
      std::cerr << "server h3 fatal: " << code << std::endl;
      ++g_fail;
    };
    cbs.on_disconnect = [&](uvcpp_h3_connection&) {};
    check(srv_h3->start(cbs) == 0, "server h3 start");
  });
  check(lrc == 0, "server listen");
  if (lrc != 0) return;
  const int port = server.configured_port();
  check(port > 0, "server port");

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  std::unique_ptr<uvcpp_h3_connection> cli_h3;
  std::unique_ptr<uvcpp_h3_ws_connection> cli_ws;
  std::string client_got;
  std::string client_pong;
  int  close_count = 0;
  ws_close_code close_code = ws_close_code::NORMAL;
  bool connected = false;

  const int crc = client.connect("127.0.0.1", port, [&](int status) {
    connected = status == 0;
  });
  check(crc == 0, "client connect 受理");
  if (crc != 0) return;

  cli_h3.reset(
      new uvcpp_h3_connection(client.connection(), /*server_side=*/false));
  uvcpp_h3_connection::callbacks cc;
  cc.on_error = [&](uvcpp_h3_connection&, int code) {
    std::cerr << "client h3 fatal: " << code << std::endl;
    ++g_fail;
  };
  cc.on_disconnect = [&](uvcpp_h3_connection&) {};
  check(cli_h3->start(cc) == 0, "client h3 start");

  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "握手完成");

  uvcpp_h3_ws_connection::callbacks wc;
  wc.on_text = [&](const std::string& m) { client_got = m; };
  wc.on_pong = [&](const uint8_t* d, size_t n) {
    client_pong.assign(reinterpret_cast<const char*>(d), n);
  };
  wc.on_close = [&](ws_close_code code, const std::string&) {
    ++close_count;
    close_code = code;
  };
  cli_ws = uvcpp_h3_ws_connection::connect(*cli_h3, "/chat", "localhost", wc);
  check(cli_ws != nullptr, "客户端 connect websocket");
  if (cli_ws == nullptr) return;
  check(cli_ws->stream_id() >= 0, "ws stream id");

  check(cli_ws->send_text("hello-h3", 8) == 0, "发送文本");
  const bool echo_ok = uvcpp_test::wait_until(
      &loop, [&] { return client_got == "hello-h3"; }, kDeadlineMs);
  check(echo_ok, "文本回显（实际 \"" + client_got + "\"）");
  check(server_got == "hello-h3", "服务端收到文本");
  check(server_protocol == "websocket",
        "服务端看到 :protocol = websocket（实际 \"" + server_protocol + "\"）");
  check(server_path == "/chat", "服务端看到 :path");

  check(cli_ws->send_text("second", 6) == 0, "发送第二段文本");
  const bool second_ok = uvcpp_test::wait_until(
      &loop, [&] { return server_got == "second"; }, kDeadlineMs);
  check(second_ok, "第二段文本到达服务端（实际 \"" + server_got + "\"）");
  check(cli_ws->send_ping("p", 1) == 0, "发送 PING");
  const bool pong_ok = uvcpp_test::wait_until(
      &loop, [&] { return client_pong == "p"; }, kDeadlineMs);
  check(pong_ok, "收到 PONG（实际 \"" + client_pong + "\"）");

  check(cli_ws->close(ws_close_code::NORMAL, "bye") == 0, "发送 Close");
  // 服务端会自动回 Close，客户端本地已经标记 closed；这里只确认没有崩。
  check(!cli_ws->is_open(), "本地关闭后 is_open = false");
}

}  // namespace

int main() {
  std::cout << "WebSocket over HTTP/3 (#53)" << std::endl;
  test_ws();
  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "all checks passed" << std::endl;
  return 0;
}

#else  // !UVCPP_HTTP3_ENABLE

int main() {
  std::cout << "SKIP: http3 disabled" << std::endl;
  return 0;
}

#endif
