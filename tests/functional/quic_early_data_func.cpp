/**
 * @file tests/functional/quic_early_data_func.cpp
 * @brief QUIC 0-RTT（#53）：会话票据导出/复用、early data 状态。
 *
 * 第一次连接拿到会话票据；第二次连接带上票据，并在握手完成**之前**开流写
 * 数据。判据：
 *
 * 1. 第一次连接的票据能导出（非空）；
 * 2. 第二次连接带上票据后握手成功；
 * 3. `early_data_status()` 是 ACCEPTED 或 NOT_SENT（不是 REJECTED）；
 * 4. 早写的数据最终到达服务端（无论走 0-RTT 还是握手后）。
 *
 * **这不是 replay 防护测试**：0-RTT 的安全语义由应用决定，本用例只验接线。
 */

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_QUIC_ENABLE

#include <handle/uvcpp_loop.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_common.h>
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

void test_early_data() {
  uvcpp_loop loop;

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    check(false, "server cert");
    return;
  }
  // 0-RTT：服务端必须允许 early data，否则票据里 max_early_data 是 0。
  server_ctx.set_max_early_data(0xffffffffu);
  check(server_ctx.max_early_data() > 0, "服务端允许 early data");

  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  uvcpp_quic_server server(&loop);
  server.set_ssl_context(&server_ctx);
  if (server.bind("127.0.0.1", 0) != 0) {
    check(false, "server bind");
    return;
  }

  std::atomic<int>  server_bytes{0};
  std::string       server_seen;
  std::atomic<int>  server_conns{0};

  const int lrc = server.listen([&](uvcpp_quic_connection* c) {
    server_conns.fetch_add(1);
    uvcpp_quic_connection::callbacks cbs;
    cbs.on_read = [&](uvcpp_quic_connection&, int64_t,
                      const net_read_result& r) {
      if (r.is_data()) {
        server_bytes.fetch_add(static_cast<int>(r.size));
        server_seen.append(r.data, r.size);
      }
    };
    c->set_callbacks(cbs);
  });
  check(lrc == 0, "server listen");
  if (lrc != 0) return;
  const int port = server.configured_port();
  check(port > 0, "server port");

  // ---- 第一次连接：拿票据 ----
  std::string ticket;
  std::string tp;
  {
    uvcpp_quic_client c1(&loop);
    c1.set_ssl_context(&client_ctx);
    bool connected = false;
    int  status = -999;
    const int rc = c1.connect("127.0.0.1", port, [&](int st) {
      connected = true;
      status = st;
    });
    check(rc == 0, "[1] connect 受理");
    if (rc != 0) return;
    check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
          "[1] 握手完成");
    check(status == 0, "[1] connect status = 0");
    // NewSessionTicket 在握手之后才到；等它被 `new_session_cb` 捕获。
    check(uvcpp_test::wait_until(&loop, [&] { return c1.has_session_ticket(); },
                                 kDeadlineMs),
          "[1] 捕获到 NewSessionTicket");
    ticket = c1.export_session_ticket();
    check(!ticket.empty(), "[1] 票据非空（" + std::to_string(ticket.size()) +
                               " 字节）");
    // 0-RTT 还需要对端上一轮的传输参数（放开了多少条流/多大窗口）。
    tp = c1.export_0rtt_transport_params();
    check(!tp.empty(), "[1] 导出了 0-RTT 传输参数（" +
                           std::to_string(tp.size()) + " 字节）");
  }

  // ---- 第二次连接：带票据 + 握手前写数据 ----
  uvcpp_quic_client c2(&loop);
  c2.set_ssl_context(&client_ctx);
  c2.set_session_ticket(ticket);
  c2.set_0rtt_transport_params(tp);

  bool connected2 = false;
  int  status2 = -999;
  const int rc2 = c2.connect("127.0.0.1", port, [&](int st) {
    connected2 = true;
    status2 = st;
  });
  check(rc2 == 0, "[2] connect 受理");
  if (rc2 != 0) return;

  // 立刻开流写：如果 0-RTT 生效，这批字节会在握手完成之前进入 0-RTT 包。
  uvcpp_quic_connection* conn = c2.connection();
  check(conn != nullptr, "[2] connection()");
  if (conn == nullptr) return;
  int64_t sid = conn->open_stream(true);
  check(sid >= 0, "[2] 握手前 open_stream（实得 " + std::to_string(sid) + "）");
  if (sid >= 0) {
    check(conn->write_stream(sid, "early-data", 10, true) == 0,
          "[2] 握手前 write_stream");
  }

  check(uvcpp_test::wait_until(&loop, [&] { return connected2; }, kDeadlineMs),
        "[2] 握手完成");
  check(status2 == 0, "[2] connect status = 0");
  // 0-RTT 是否被接受由服务端决定；被拒（2）是合法结果，客户端必须回退到
  // 1-RTT 并让数据照常到达。
  const int eds = conn->early_data_status();
  check(eds == 0 || eds == 1 || eds == 2,
        "[2] early_data_status 是 0/1/2（实得 " + std::to_string(eds) + "）");
  // OpenSSL: NOT_SENT=0, REJECTED=1, ACCEPTED=2。
  if (eds == 1) {
    check(conn->early_data_rejected(),
          "[2] REJECTED 时 early_data_rejected() 同步为真");
  } else {
    check(!conn->early_data_rejected(), "[2] 未拒绝时 early_data_rejected() 为假");
  }
  check(eds == 2, "[2] 本机 0-RTT 被接受（status=2）");

  check(uvcpp_test::wait_until(&loop, [&] { return server_bytes.load() >= 10; },
                               kDeadlineMs),
        "[2] 早写的字节到达服务端（实际 " +
            std::to_string(server_bytes.load()) + "）");
  check(server_seen == "early-data",
        "[2] 服务端内容（实际 \"" + server_seen + "\"）");
  std::cout << "  [info] early_data_status = " << conn->early_data_status()
            << std::endl;
}

}  // namespace

int main() {
  std::cout << "QUIC 0-RTT (#53)" << std::endl;
  test_early_data();
  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "all checks passed" << std::endl;
  return 0;
}

#else  // !UVCPP_QUIC_ENABLE

int main() {
  std::cout << "SKIP: quic disabled" << std::endl;
  return 0;
}

#endif
