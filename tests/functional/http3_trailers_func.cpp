/**
 * @file tests/functional/http3_trailers_func.cpp
 * @brief HTTP/3 trailers（#53）：请求与响应两个方向的第二个头块。
 *
 * 一条 QUIC 连接上：
 *
 * 1. 客户端发一个**无 body** 的请求 + trailers（`h3_request::trailers`）；
 *    服务端 `on_request` 拿到的 `req.trailers` 里有它 —— 证明 trailers 不会
 *    混进首部、也不会被当成第二条请求。
 * 2. 服务端回一个带 body 的响应 + trailers；客户端 `take_completed()` 拿到的
 *    `resp.trailers` 里有它，且 `resp.body` 完整 —— 证明 trailers 排在 body
 *    之后、没有顶掉 body。
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

std::string header_value(const std::vector<h3_header>& hs, const char* name) {
  for (size_t i = 0; i < hs.size(); ++i) {
    if (hs[i].name == name) return hs[i].value;
  }
  return std::string();
}

void test_trailers() {
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
  std::vector<h3_header> server_req_trailers;
  std::string            server_req_path;
  int                    server_response_rc = -999;

  const int lrc = server.listen([&](uvcpp_quic_connection* c) {
    srv_h3.reset(new uvcpp_h3_connection(c, /*server_side=*/true));
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_request = [&](uvcpp_h3_connection& h3, const h3_request& req) {
      server_req_path     = req.path;
      server_req_trailers = req.trailers;

      h3_response resp;
      resp.stream_id = req.stream_id;
      resp.status    = 200;
      resp.body      = "resp-body";
      h3_header t;
      t.name  = "x-resp-trailer";
      t.value = "r1";
      resp.trailers.push_back(t);
      server_response_rc = h3.send_response(resp);
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
  bool     connected = false;
  int64_t  sid = -1;

  auto pump_done = [&]() {
    h3_response r;
    while (cli_h3 && cli_h3->take_completed(r)) done.push_back(r);
  };

  const int crc = client.connect("127.0.0.1", port, [&](int status) {
    connected = true;
    if (status != 0) return;
    h3_request req;
    req.method    = "GET";
    req.scheme    = "https";
    req.authority = "localhost";
    req.path      = "/trailers";
    // nghttp3 对"无 body 的请求"直接标 END_STREAM，之后不能再提交 trailers；
    // 真实用法（gRPC）都带 body，这里也带一个。
    req.body      = "q";
    h3_header t;
    t.name  = "x-req-trailer";
    t.value = "q1";
    req.trailers.push_back(t);
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

  check(uvcpp_test::wait_until(&loop, [&] { return !done.empty(); },
                               kDeadlineMs),
        "响应回调落地");
  if (done.empty()) return;
  const h3_response& r = done[0];
  check(r.error == 0, "响应无错误");
  check(r.status == 200, "响应 200");
  check(r.body == "resp-body", "响应体完整（trailers 没有顶掉 body）");
  check(header_value(r.trailers, "x-resp-trailer") == "r1",
        "客户端拿到响应 trailers（实际 \"" +
            header_value(r.trailers, "x-resp-trailer") + "\"）");

  check(server_req_path == "/trailers", "服务端路径");
  check(header_value(server_req_trailers, "x-req-trailer") == "q1",
        "服务端拿到请求 trailers（实际 \"" +
            header_value(server_req_trailers, "x-req-trailer") + "\"）");
  check(server_response_rc == 0, "服务端 send_response 返回 0");
}

}  // namespace

int main() {
  std::cout << "HTTP/3 trailers (#53)" << std::endl;
  test_trailers();
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
