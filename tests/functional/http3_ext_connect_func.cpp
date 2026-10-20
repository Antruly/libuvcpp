/**
 * @file tests/functional/http3_ext_connect_func.cpp
 * @brief HTTP/3 extended CONNECT 的协议形状（#53）：`:protocol` 与 `CONNECT`。
 *
 * 只钉**协议层**：客户端把 `:protocol` 填上，本层把它钉成 `:method = CONNECT`
 * 并发出；服务端的 `on_request` 看到 `CONNECT` 与 `"websocket"`，并回 200。
 * 这条流上跑 WebSocket 帧（RFC 9220）是另一层的事，不在本用例里。
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

void test_ext_connect() {
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
  std::string server_method;
  std::string server_protocol;
  std::string server_path;
  int         server_rc = -999;

  const int lrc = server.listen([&](uvcpp_quic_connection* c) {
    srv_h3.reset(new uvcpp_h3_connection(c, /*server_side=*/true));
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_request = [&](uvcpp_h3_connection& h3, const h3_request& req) {
      server_method   = req.method;
      server_protocol = req.protocol;
      server_path     = req.path;
      h3_response resp;
      resp.stream_id = req.stream_id;
      resp.status    = 200;
      resp.body      = "ext-ok";
      server_rc      = h3.send_response(resp);
    };
    cbs.on_error = [&](uvcpp_h3_connection&, int code) {
      std::cerr << "server h3 fatal: " << code << std::endl;
      ++g_fail;
    };
    cbs.on_disconnect = [&](uvcpp_h3_connection&) {};
    const int rc = srv_h3->start(cbs);
    check(rc == 0, "server h3 start");
  });
  check(lrc == 0, "server listen");
  if (lrc != 0) return;
  const int port = server.configured_port();
  check(port > 0, "server port");

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  std::unique_ptr<uvcpp_h3_connection> cli_h3;
  std::vector<h3_response>             done;
  bool    connected = false;
  int64_t sid = -1;

  auto pump_done = [&]() {
    h3_response r;
    while (cli_h3 && cli_h3->take_completed(r)) done.push_back(r);
  };

  const int crc = client.connect("127.0.0.1", port, [&](int status) {
    connected = true;
    if (status != 0) return;
    h3_request req;
    // 故意填 GET：`send_request()` 看到 `protocol` 非空必须把它钉成 CONNECT。
    req.method    = "GET";
    req.scheme    = "https";
    req.authority = "localhost";
    req.path      = "/chat";
    req.protocol  = "websocket";
    sid = cli_h3->send_request(req);
  });
  check(crc == 0, "client connect 受理");
  if (crc != 0) return;

  cli_h3.reset(
      new uvcpp_h3_connection(client.connection(), /*server_side=*/false));
  uvcpp_h3_connection::callbacks cc;
  cc.on_stream_close = [&](uvcpp_h3_connection&, const h3_stream_close_info&) {
    pump_done();
  };
  cc.on_error = [&](uvcpp_h3_connection&, int code) {
    std::cerr << "client h3 fatal: " << code << std::endl;
    ++g_fail;
  };
  cc.on_disconnect = [&](uvcpp_h3_connection&) {};
  check(cli_h3->start(cc) == 0, "client h3 start");

  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "握手完成");
  check(sid >= 0, "send_request 返回流号（实得 " + std::to_string(sid) + "）");
  if (sid < 0) return;

  // QPACK / 对端 SETTINGS 观测（#53）：双方都应该收到对方的 SETTINGS，
  // 并且因为本层打开了 `enable_connect_protocol`，这一位在对端看到的是 true。
  check(uvcpp_test::wait_until(
            &loop,
            [&] {
              return srv_h3 && srv_h3->has_remote_settings() &&
                     cli_h3->has_remote_settings();
            },
            kDeadlineMs),
        "双方都收到对端 SETTINGS");
  if (srv_h3 && srv_h3->has_remote_settings() && cli_h3->has_remote_settings()) {
    const uvcpp_h3_remote_settings cs = cli_h3->remote_settings();
    const uvcpp_h3_remote_settings ss = srv_h3->remote_settings();
    // RFC 9220 的 `SETTINGS_ENABLE_CONNECT_PROTOCOL` 是**服务端**宣告的能力；
    // nghttp3 的文档也写明"客户端忽略这一位"，所以只断言客户端看到它。
    check(cs.enable_connect_protocol, "客户端看到对端 enable_connect_protocol");
    check(cs.qpack_max_dtable_capacity > 0,
          "客户端看到对端 QPACK 表容量（" +
              std::to_string(cs.qpack_max_dtable_capacity) + "）");
    check(ss.qpack_max_dtable_capacity > 0,
          "服务端看到对端 QPACK 表容量（" +
              std::to_string(ss.qpack_max_dtable_capacity) + "）");
    check(cs.max_field_section_size > 0 &&
              ss.max_field_section_size > 0,
          "双方都看到 max_field_section_size");
  }

  check(uvcpp_test::wait_until(&loop, [&] { return !done.empty(); },
                               kDeadlineMs),
        "响应回调落地");
  if (done.empty()) return;
  check(done[0].error == 0 && done[0].status == 200, "响应 200");
  check(done[0].body == "ext-ok", "响应体");

  check(server_method == "CONNECT",
        "服务端看到 :method = CONNECT（实际 \"" + server_method + "\"）");
  check(server_protocol == "websocket",
        "服务端看到 :protocol = websocket（实际 \"" + server_protocol + "\"）");
  check(server_path == "/chat", "服务端看到 :path（实际 \"" + server_path + "\"）");
  check(server_rc == 0, "服务端 send_response 返回 0");
}

}  // namespace

int main() {
  std::cout << "HTTP/3 extended CONNECT (#53)" << std::endl;
  test_ext_connect();
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
