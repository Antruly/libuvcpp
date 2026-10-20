/**
 * @file tests/functional/quic_datagram_func.cpp
 * @brief QUIC DATAGRAM（RFC 9221，#53）：收发、大小上限与错误码。
 *
 * 一条 QUIC 连接上：
 *
 * 1. 握手完成后两端都宣告了 DATAGRAM 支持（`max_datagram_size() > 0`）；
 * 2. 客户端发一条，服务端 `on_datagram` 收到并回一条，客户端收到；
 * 3. 空负载 / 超过对端上限的负载返回可区分的错误（`UV_EINVAL` / `UV_EMSGSIZE`）；
 * 4. DATAGRAM **不进流**：同一条连接上开一条流发数据仍然正常，两种数据面
 *    互不干扰。
 */

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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
  if (!ok) {
    ++g_fail;
    std::cerr << "FAIL: " << what << std::endl;
  } else {
    std::cout << "ok: " << what << std::endl;
  }
}

const int kDeadlineMs = 5000;

void test_datagrams() {
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

  std::string server_dgram;
  int         server_dgram_count = 0;
  size_t      server_max = 0;
  uvcpp_quic_connection* server_conn = nullptr;

  const int lrc = server.listen([&](uvcpp_quic_connection* c) {
    server_conn = c;
    uvcpp_quic_connection::callbacks cbs;
    cbs.on_datagram = [&](uvcpp_quic_connection& conn, const uint8_t* data,
                          size_t len) {
      ++server_dgram_count;
      server_dgram.assign(reinterpret_cast<const char*>(data), len);
      if (server_dgram == "ping-dgram") {
        conn.send_datagram("pong-dgram", 10);
      }
    };
    // 同一条连接上的流回声：证明 DATAGRAM 与 STREAM 两个数据面互不干扰。
    cbs.on_read = [](uvcpp_quic_connection& conn, int64_t id,
                     const net_read_result& r) {
      if (r.event == net_read_event::DATA) {
        conn.write_stream(id, r.data, r.size, r.fin);
      }
    };
    c->set_callbacks(cbs);
  });
  check(lrc == 0, "server listen");
  if (lrc != 0) return;
  const int port = server.configured_port();
  check(port > 0, "server port");

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  std::string client_dgram;
  int         client_dgram_count = 0;
  size_t      client_max = 0;
  size_t      client_stream_bytes = 0;
  bool        connected = false;
  int         connect_status = -999;
  int64_t     sid = -1;

  const int crc = client.connect("127.0.0.1", port, [&](int status) {
    connect_status = status;
    connected = true;
  });
  check(crc == 0, "client connect 受理");
  if (crc != 0) return;

  // 必须在 connect() 返回之后、下一次循环迭代之前装：这是
  // `uvcpp_quic_client::connect()` 写死的唯一安全窗口。
  uvcpp_quic_connection* cli_conn = client.connection();
  check(cli_conn != nullptr, "client.connection()");
  if (cli_conn == nullptr) return;
  uvcpp_quic_connection::callbacks cbs;
  cbs.on_datagram = [&](uvcpp_quic_connection&, const uint8_t* data,
                        size_t len) {
    ++client_dgram_count;
    client_dgram.assign(reinterpret_cast<const char*>(data), len);
  };
  cbs.on_read = [&](uvcpp_quic_connection&, int64_t,
                    const net_read_result& r) {
    if (r.event == net_read_event::DATA) client_stream_bytes += r.size;
  };
  cli_conn->set_callbacks(cbs);

  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "握手完成");
  check(connect_status == 0, "connect status = 0");

  // 对端宣告的 DATAGRAM 帧上限。
  server_max = server_conn != nullptr ? server_conn->max_datagram_size() : 0;
  client_max = cli_conn->max_datagram_size();
  check(server_max > 0, "服务端看到对端的 max_datagram_size > 0（实得 " +
                            std::to_string(server_max) + "）");
  check(client_max > 0, "客户端看到对端的 max_datagram_size > 0（实得 " +
                            std::to_string(client_max) + "）");

  // 空负载与超限：同步错误码。
  check(cli_conn->send_datagram(nullptr, 0) == UV_EINVAL,
        "空负载返回 UV_EINVAL");
  std::vector<char> big(client_max + 1, 'x');
  check(cli_conn->send_datagram(big.data(), big.size()) == UV_EMSGSIZE,
        "超过对端上限返回 UV_EMSGSIZE");

  // 正常收发。
  check(cli_conn->send_datagram("ping-dgram", 10) == 0, "发送 ping-dgram");
  check(uvcpp_test::wait_until(
            &loop,
            [&] { return server_dgram_count > 0 && client_dgram_count > 0; },
            kDeadlineMs),
        "两端都收到了 DATAGRAM");
  check(server_dgram == "ping-dgram",
        "服务端收到 \"" + server_dgram + "\"");
  check(client_dgram == "pong-dgram",
        "客户端收到 \"" + client_dgram + "\"");

  // 同一条连接上流数据照常（DATAGRAM 不进流）。
  sid = cli_conn->open_stream(true);
  check(sid >= 0, "open_stream 成功（实得 " + std::to_string(sid) + "）");
  if (sid >= 0) {
    check(cli_conn->write_stream(sid, "stream-data", 11, true) == 0,
          "write_stream 受理");
    check(uvcpp_test::wait_until(
              &loop, [&] { return client_stream_bytes >= 11; }, kDeadlineMs),
          "同一条连接上的流回声到达（DATAGRAM 不影响 STREAM）");
  }
}

}  // namespace

int main() {
  std::cout << "QUIC DATAGRAM (#53)" << std::endl;
  test_datagrams();
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
