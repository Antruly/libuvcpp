/**
 * @file tests/functional/http3_datagram_func.cpp
 * @brief HTTP/3 Datagram（RFC 9297，跑在 QUIC DATAGRAM / RFC 9221 上）。
 *
 * 测的是 h3 层对 "Quarter Stream ID + payload" 那一层的编解码与闸门：
 *
 * | # | 场景 | 判据 |
 * |---|---|---|
 * | 1 | 双方宣告 `SETTINGS_H3_DATAGRAM`、extended CONNECT 流上发一条 | 对方收到、回显回来、payload 逐字节相同 |
 * | 2 | `max_datagram_size()` | 协商后 > 0（QUIC 层也开了 DATAGRAM） |
 * | 3 | 在**普通 GET 流**上发 | `UV_EINVAL`（RFC 9297 只给 CONNECT 免掉 flow-id） |
 * | 4 | payload = `max_datagram_size()` | `UV_EMSGSIZE`（四分之一流号还要占 1+ 字节） |
 * | 5 | `dropped_datagrams()` | 正向路径上一次丢弃都没有 |
 *
 * 装置与 `http3_ws_func.cpp` 同源：真 QUIC + 真 TLS，两端在**同一条循环**上。
 */

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_HTTP3_ENABLE

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
  if (ok) {
    std::cout << "ok: " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << what << std::endl;
  }
}

const int kDeadlineMs = 8000;

h3_header status_header(int status) {
  h3_header h;
  h.name  = ":status";
  h.value = std::to_string(status);
  h.kind  = h3_header_kind::STATUS;
  return h;
}

void test_datagram() {
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
  int64_t      server_stream    = -1;
  bool         server_accepted  = false;
  std::string  server_got;
  int          server_echoes    = 0;

  const int lrc = server.listen([&](uvcpp_quic_connection* c) {
    srv_h3.reset(new uvcpp_h3_connection(c, /*server_side=*/true));
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_connect_request = [&](uvcpp_h3_connection& h3,
                                 const h3_request& req) {
      server_stream   = req.stream_id;
      server_accepted = true;
      std::vector<h3_header> hs;
      hs.push_back(status_header(200));
      check(h3.send_stream_response(req.stream_id, hs) == 0,
            "服务端回 200（extended CONNECT）");
    };
    cbs.on_datagram = [&](uvcpp_h3_connection& h3, int64_t sid,
                          const uint8_t* data, size_t len) {
      server_got.assign(reinterpret_cast<const char*>(data), len);
      ++server_echoes;
      // 原样回显。**同样走 datagram**，不是流上写字节 —— 判据才落在这一层。
      check(h3.send_datagram(sid, data, len) == 0, "服务端回显 datagram");
    };
    cbs.on_error = [&](uvcpp_h3_connection&, int code) {
      std::cerr << "server h3 fatal: " << code << std::endl;
      ++g_fail;
    };
    cbs.on_disconnect = [&](uvcpp_h3_connection&) {};
    srv_h3->set_h3_datagram_enabled(true);
    check(srv_h3->start(cbs) == 0, "服务端 h3 start");
  });
  check(lrc == 0, "server listen");
  if (lrc != 0) return;
  const int port = server.configured_port();
  check(port > 0, "server port");

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  std::unique_ptr<uvcpp_h3_connection> cli_h3;
  std::string client_got;
  bool        connected = false;

  const int crc = client.connect("127.0.0.1", port, [&](int status) {
    connected = status == 0;
    if (!connected) return;
    cli_h3.reset(new uvcpp_h3_connection(client.connection(),
                                         /*server_side=*/false));
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_datagram = [&](uvcpp_h3_connection&, int64_t, const uint8_t* data,
                          size_t len) {
      client_got.assign(reinterpret_cast<const char*>(data), len);
    };
    cbs.on_error      = [&](uvcpp_h3_connection&, int) {};
    cbs.on_disconnect = [&](uvcpp_h3_connection&) {};
    cli_h3->set_h3_datagram_enabled(true);
    check(cli_h3->start(cbs) == 0, "客户端 h3 start");
  });
  check(crc == 0, "client connect 受理");
  if (crc != 0) return;

  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "QUIC 握手完成");

  // 对端 SETTINGS 到达 + 关键流开出来之后 `send_stream_request` 才受理。
  int64_t sid = -1;
  check(uvcpp_test::wait_until(
            &loop,
            [&] {
              h3_request req;
              req.method    = "CONNECT";
              req.protocol  = "dgram-echo";
              req.authority = "localhost";
              req.path      = "/d";
              sid = cli_h3->send_stream_request(req);
              return sid >= 0;
            },
            kDeadlineMs),
        "客户端开 extended CONNECT（等 SETTINGS）");

  check(uvcpp_test::wait_until(&loop, [&] { return server_accepted; },
                               kDeadlineMs),
        "服务端收到 extended CONNECT");
  check(uvcpp_test::wait_until(
            &loop, [&] { return cli_h3->peer_h3_datagram(); }, kDeadlineMs),
        "双方都宣告了 SETTINGS_H3_DATAGRAM");

  const size_t max_frame = cli_h3->max_datagram_size();
  check(max_frame > 0,
        "max_datagram_size > 0（实际 " + std::to_string(max_frame) + "）");
  check(srv_h3 && srv_h3->max_datagram_size() > 0,
        "服务端的 max_datagram_size 也 > 0");

  // ---- 1) 正向：发一条、收回来 ---------------------------------------
  const char* payload = "dgram-payload-1";
  const size_t plen   = std::strlen(payload);
  check(cli_h3->send_datagram(sid, payload, plen) == 0, "客户端发 datagram");
  const bool echo_ok = uvcpp_test::wait_until(
      &loop, [&] { return client_got == payload; }, kDeadlineMs);
  check(echo_ok, "回显到达客户端（实际 \"" + client_got + "\"）");
  check(server_got == payload, "服务端收到原始 payload");
  check(server_stream == sid, "服务端看到的流号与客户端一致");
  check(server_echoes >= 1, "服务端回显过至少一条");

  // 空 payload 也是一条合法 datagram（只剩四分之一流号）。
  check(cli_h3->send_datagram(sid, nullptr, 0) == 0, "发空 payload");
  check(uvcpp_test::wait_until(&loop, [&] { return server_echoes >= 2; },
                               kDeadlineMs),
        "服务端收到空 payload");

  // ---- 3) 普通 GET 流上不许发 ----------------------------------------
  {
    h3_request get;
    get.method    = "GET";
    get.authority = "localhost";
    get.path      = "/x";
    const int64_t gsid = cli_h3->send_request(get);
    check(gsid >= 0, "普通 GET 流开出来了（" + std::to_string(gsid) + "）");
    if (gsid >= 0) {
      check(cli_h3->send_datagram(gsid, "x", 1) == UV_EINVAL,
            "普通流上发 datagram 被拒（UV_EINVAL）");
    }
  }

  // ---- 4) 超长：四分之一流号还要占字节 --------------------------------
  {
    std::string big(max_frame, 'x');
    check(cli_h3->send_datagram(sid, big.data(), big.size()) == UV_EMSGSIZE,
          "payload = max_datagram_size() 时 UV_EMSGSIZE");
  }

  // ---- 5) 正向路径上没有丢弃 -----------------------------------------
  check(cli_h3->dropped_datagrams() == 0,
        "客户端 dropped_datagrams = 0（实际 " +
            std::to_string(cli_h3->dropped_datagrams()) + "）");
  if (srv_h3) {
    check(srv_h3->dropped_datagrams() == 0,
          "服务端 dropped_datagrams = 0（实际 " +
              std::to_string(srv_h3->dropped_datagrams()) + "）");
  }

  cli_h3.reset();
}

}  // namespace

int main() {
  std::cout << "HTTP/3 Datagram (RFC 9297)" << std::endl;
  test_datagram();
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
