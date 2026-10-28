/**
 * @file tests/functional/quic_stats_func.cpp
 * @brief `uvcpp_quic_connection::stats()`：RTT / 拥塞窗口 / 丢包 / 收发计数。
 *
 * 判据分两层：
 *
 * 1. **形状**：没挂到连接上的壳（默认构造）返回 false、`out` 被清零 —— 这是
 *    "不可用"与"全是 0"必须分得开的地方；
 * 2. **真数**：真握手 + 一次往返之后，两端的 `pkt_sent` / `pkt_recv` /
 *    `bytes_sent` / `bytes_recv` / `cwnd` 都 > 0，`smoothed_rtt_us` > 0
 *    （握手本身就会产生 RTT 采样）。丢包计数只断言语义（`>= 0` 恒真，所以不
 *    断言它等于 0 —— 本机回环上它通常就是 0，但那是环境事实，不是契约）。
 */

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_QUIC_ENABLE

#include <uv.h>

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
  if (ok) {
    std::cout << "ok: " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << what << std::endl;
  }
}

const int kDeadlineMs = 8000;

void test_stats() {
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

  uvcpp_quic_connection* server_conn = nullptr;
  std::string            server_got;

  const int lrc = server.listen([&](uvcpp_quic_connection* c) {
    server_conn = c;
    uvcpp_quic_connection::callbacks cbs;
    cbs.on_read = [&](uvcpp_quic_connection& conn, int64_t id,
                      const net_read_result& r) {
      if (r.event != net_read_event::DATA) return;
      server_got.append(r.data, r.size);
      if (server_got == "ping") conn.write_stream(id, "pong", 4, true);
    };
    c->set_callbacks(cbs);
  });
  check(lrc == 0, "server listen");
  if (lrc != 0) return;
  const int port = server.configured_port();
  check(port > 0, "server port");

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  bool    connected = false;
  std::string client_got;
  bool    client_done = false;

  const int crc = client.connect("127.0.0.1", port, [&](int st) {
    connected = st == 0;
    if (!connected) return;
    uvcpp_quic_connection* c = client.connection();
    if (c == nullptr) return;
    uvcpp_quic_connection::callbacks cbs;
    cbs.on_read = [&](uvcpp_quic_connection&, int64_t,
                      const net_read_result& r) {
      if (r.event == net_read_event::DATA) {
        client_got.append(r.data, r.size);
        if (client_got == "pong") client_done = true;
      }
    };
    c->set_callbacks(cbs);
    const int64_t sid = c->open_stream(true);
    if (sid >= 0) c->write_stream(sid, "ping", 4, true);
  });
  check(crc == 0, "client connect 受理");
  if (crc != 0) return;
  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "QUIC 握手完成");
  check(uvcpp_test::wait_until(&loop, [&] { return client_done; }, kDeadlineMs),
        "一次往返完成（pong 收到）");
  check(server_conn != nullptr, "服务端拿到了连接");

  // ---- 客户端统计 -----------------------------------------------------
  uvcpp_quic_stats cs;
  check(client.connection()->stats(cs), "客户端 stats() 返回 true");
  check(cs.pkt_sent > 0, "客户端 pkt_sent > 0（实际 " +
                             std::to_string(cs.pkt_sent) + "）");
  check(cs.pkt_recv > 0, "客户端 pkt_recv > 0（实际 " +
                             std::to_string(cs.pkt_recv) + "）");
  check(cs.bytes_sent > 0, "客户端 bytes_sent > 0");
  check(cs.bytes_recv > 0, "客户端 bytes_recv > 0");
  check(cs.cwnd > 0, "客户端 cwnd > 0（实际 " + std::to_string(cs.cwnd) + "）");
  check(cs.smoothed_rtt_us > 0,
        "客户端 smoothed_rtt_us > 0（实际 " +
            std::to_string(cs.smoothed_rtt_us) + " µs）");

  // ---- 服务端统计 -----------------------------------------------------
  if (server_conn != nullptr) {
    uvcpp_quic_stats ss;
    check(server_conn->stats(ss), "服务端 stats() 返回 true");
    check(ss.pkt_sent > 0 && ss.pkt_recv > 0, "服务端双向都有包");
    check(ss.cwnd > 0, "服务端 cwnd > 0");
    check(ss.smoothed_rtt_us > 0, "服务端 smoothed_rtt_us > 0");
  }

  // ---- 未挂连接的壳：false，且 out 被清零 -----------------------------
  {
    uvcpp_quic_connection shell;
    uvcpp_quic_stats      zero;
    zero.pkt_sent = 12345;  // 哨兵：确认它真的被清零
    check(!shell.stats(zero), "未挂连接的 stats() 返回 false");
    check(zero.pkt_sent == 0, "false 时 out 被清零（不是留脏值）");
  }
}

}  // namespace

int main() {
  std::cout << "QUIC 连接统计（stats）" << std::endl;
  test_stats();
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
