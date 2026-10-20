/**
 * @file tests/functional/http3_goaway_func.cpp
 * @brief HTTP/3 GOAWAY（#53）：shutdown notice → 最终 GOAWAY → drained。
 *
 * 一条 QUIC 连接上跑一次 h3 GET；服务端在请求处理里先发 shutdown notice
 * （`shutdown_notice()`），流关闭后再发最终 GOAWAY（`shutdown()`）。判据：
 *
 * 1. 客户端至少收到 **两次** `on_goaway`：第一次是 notice（id 是一个极大的
 *    哨兵值），第二次是最终值，且 `id` 不会变大（RFC 9114 §5.2）；
 * 2. `peer_goaway_received()` / `peer_goaway_last_stream_id()` 与回调一致；
 * 3. 响应照常到达（GOAWAY 不打断在途流）；
 * 4. 服务端 `is_drained()` 最终为真（所有流收场）。
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

void test_goaway() {
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
  int  notice_rc = -999;
  int  shutdown_rc = -999;

  const int lrc = server.listen([&](uvcpp_quic_connection* c) {
    srv_h3.reset(new uvcpp_h3_connection(c, /*server_side=*/true));
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_request = [&](uvcpp_h3_connection& h3, const h3_request& req) {
      // 第一封：只是通知，在途流照常。第二封紧跟着发（测试里不等 RTT）。
      notice_rc = h3.shutdown_notice();
      shutdown_rc = h3.shutdown();
      h3_response resp;
      resp.stream_id = req.stream_id;
      resp.status    = 200;
      resp.body      = "bye-h3";
      h3.send_response(resp);
    };
    cbs.on_stream_close = [&](uvcpp_h3_connection& h3,
                              const h3_stream_close_info&) {
      (void)h3;
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
  int      goaway_count = 0;
  int64_t  notice_id = -1;
  int64_t  final_id  = -1;
  bool     monotonic = true;
  int64_t  prev_id   = 0;
  std::vector<h3_response> done;
  bool     connected = false;
  int64_t  req_id = -1;

  auto pump_done = [&]() {
    h3_response r;
    while (cli_h3 && cli_h3->take_completed(r)) done.push_back(r);
  };

  int connect_status = -999;
  const int crc = client.connect("127.0.0.1", port, [&](int status) {
    connect_status = status;
    connected = true;
    if (status != 0) return;
    h3_request req;
    req.method    = "GET";
    req.scheme    = "https";
    req.authority = "localhost";
    req.path      = "/bye";
    req_id        = cli_h3->send_request(req);
  });
  check(crc == 0, "client connect 受理");
  if (crc != 0) return;

  cli_h3.reset(
      new uvcpp_h3_connection(client.connection(), /*server_side=*/false));
  uvcpp_h3_connection::callbacks cc;
  cc.on_stream_close = [&](uvcpp_h3_connection&, const h3_stream_close_info&) {
    pump_done();
  };
  cc.on_goaway = [&](uvcpp_h3_connection&, int64_t id) {
    ++goaway_count;
    if (goaway_count == 1) notice_id = id;
    final_id = id;
    if (goaway_count > 1 && id > prev_id) monotonic = false;
    prev_id = id;
  };
  cc.on_error = [&](uvcpp_h3_connection&, int code) {
    std::cerr << "client h3 fatal: " << code << std::endl;
    ++g_fail;
  };
  cc.on_disconnect = [&](uvcpp_h3_connection&) {};
  check(cli_h3->start(cc) == 0, "client h3 start");

  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "握手完成");
  check(req_id >= 0, "send_request 返回流号（connect=" +
                        std::to_string(connect_status) + "，sid=" +
                        std::to_string(req_id) + "，ready=" +
                        std::to_string(cli_h3 && cli_h3->ready()) + "）");
  if (req_id < 0) return;

  check(uvcpp_test::wait_until(&loop, [&] { return !done.empty(); },
                               kDeadlineMs),
        "响应到达（GOAWAY 不打断在途流）");
  if (!done.empty()) {
    check(done[0].status == 200, "响应 200");
    check(done[0].body == "bye-h3", "响应体");
  }

  check(uvcpp_test::wait_until(&loop, [&] { return goaway_count >= 2; },
                               kDeadlineMs),
        "收到至少两封 GOAWAY（notice + 最终）");
  check(notice_rc == 0, "服务端 shutdown_notice() 返回 0");
  check(shutdown_rc == 0, "服务端 shutdown() 返回 0");
  check(goaway_count >= 2,
        "on_goaway 次数 = " + std::to_string(goaway_count));
  check(monotonic, "GOAWAY 的 id 不会变大");
  check(notice_id > (int64_t(1) << 60),
        "第一封是 shutdown notice（哨兵 id）");
  check(final_id >= 0 && final_id < (int64_t(1) << 60),
        "最后一封是最终 GOAWAY（实得 " + std::to_string(final_id) + "）");
  check(cli_h3->peer_goaway_received(), "peer_goaway_received()");
  check(cli_h3->peer_goaway_last_stream_id() == final_id,
        "peer_goaway_last_stream_id() 与回调一致");

  // 服务端所有流收场、GOAWAY 发完并拿到确认之后 drained 为真。QUIC 的 ACK
  // 还要几轮；每一轮顺手冲一下 h3，让它有机会推进自己的收场记账。
  const bool drained_ok = uvcpp_test::wait_until(
      &loop,
      [&] {
        if (!srv_h3) return false;
        srv_h3->flush();
        return srv_h3->is_drained();
      },
      kDeadlineMs);
  check(drained_ok, "服务端 is_drained() 最终为真");
}

}  // namespace

int main() {
  std::cout << "HTTP/3 GOAWAY (#53)" << std::endl;
  test_goaway();
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
