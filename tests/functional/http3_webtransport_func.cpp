/**
 * @file tests/functional/http3_webtransport_func.cpp
 * @brief WebTransport over HTTP/3（draft-ietf-webtrans-http3-13）。
 *
 * 一条会话要把三类东西都走通，判据缺一条都不算"实现了"：
 *
 * | # | 面 | 判据 |
 * |---|---|---|
 * | 1 | 会话建立 | `:protocol = webtransport` 的 extended CONNECT，服务端 2xx、双方拿到会话 |
 * | 2 | 双向流 | 客户端开 `0x41` 流写一段，服务端按同一流回显，客户端收到 |
 * | 3 | 单向流 | 客户端开 `0x54` 流写一段并 FIN，服务端收到数据与结束 |
 * | 4 | datagram | RFC 9297，四分之一流号指向会话流；一来一回 |
 * | 5 | Close capsule | `WT_CLOSE_SESSION` 带上应用错误码与说明，对端 `on_close` 原样收到 |
 *
 * 装置与 `http3_ws_func.cpp` 同源：真 QUIC + 真 TLS，两端在同一条循环上。
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
#include <http3/uvcpp_h3_wt_session.h>
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

void test_webtransport() {
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

  std::unique_ptr<uvcpp_h3_connection>    srv_h3;
  std::unique_ptr<uvcpp_h3_wt_session>    srv_wt;
  bool         server_accepted = false;
  bool         srv_drain_seen  = false;
  bool         srv_bidi_end    = false;
  std::string  srv_bidi_got;
  std::string  srv_uni_got;
  bool         srv_uni_end     = false;
  std::string  srv_dgram_got;
  int          srv_close_code  = -1;
  std::string  srv_close_reason;

  const int lrc = server.listen([&](uvcpp_quic_connection* c) {
    srv_h3.reset(new uvcpp_h3_connection(c, /*server_side=*/true));
    uvcpp_h3_connection::callbacks hcb;
    hcb.on_connect_request = [&](uvcpp_h3_connection& h3,
                                 const h3_request& req) {
      if (req.protocol != "webtransport") return;
      if (req.path == "/reject") {
        // 故意不建会话：客户端应当看到非 2xx 并按失败收场。
        std::vector<h3_header> hs;
        h3_header st;
        st.name  = ":status";
        st.value = "404";
        st.kind  = h3_header_kind::STATUS;
        hs.push_back(st);
        h3.send_stream_response(req.stream_id, hs);
        return;
      }
      uvcpp_h3_wt_session::callbacks wcb;
      wcb.on_stream = [&](uvcpp_h3_wt_session&, uvcpp_h3_wt_stream& s, bool bidi) {
        if (bidi) {
          uvcpp_h3_wt_stream* sp = &s;
          s.on_data([&, sp](const uint8_t* d, size_t n) {
            srv_bidi_got.assign(reinterpret_cast<const char*>(d), n);
            sp->write(d, n);  // 回显
          });
          s.on_end([&]() { srv_bidi_end = true; });
        } else {
          s.on_data([&](const uint8_t* d, size_t n) {
            srv_uni_got.assign(reinterpret_cast<const char*>(d), n);
          });
          s.on_end([&]() { srv_uni_end = true; });
        }
      };
      wcb.on_datagram = [&](uvcpp_h3_wt_session& s, const uint8_t* d,
                            size_t n) {
        srv_dgram_got.assign(reinterpret_cast<const char*>(d), n);
        s.send_datagram(d, n);
      };
      wcb.on_drain = [&](uvcpp_h3_wt_session&) { srv_drain_seen = true; };
      wcb.on_close = [&](uvcpp_h3_wt_session&, uint32_t code,
                         const std::string& why) {
        srv_close_code   = static_cast<int>(code);
        srv_close_reason = why;
      };
      srv_wt = uvcpp_h3_wt_session::accept(h3, req, wcb);
      check(srv_wt != nullptr, "服务端 accept WebTransport 会话");
      server_accepted = true;
    };
    hcb.on_error      = [](uvcpp_h3_connection&, int) {};
    hcb.on_disconnect = [](uvcpp_h3_connection&) {};
    srv_h3->set_h3_datagram_enabled(true);
    check(srv_h3->start(hcb) == 0, "服务端 h3 start");
  });
  check(lrc == 0, "server listen");
  if (lrc != 0) return;
  const int port = server.configured_port();
  check(port > 0, "server port");

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  std::unique_ptr<uvcpp_h3_connection> cli_h3;
  std::unique_ptr<uvcpp_h3_wt_session> cli_wt;
  bool         connected   = false;
  bool         cli_opened  = false;
  std::string  cli_dgram_got;
  int          cli_close_code = -1;
  int          rej_error   = 0;
  int          rej_close   = -1;

  const int crc = client.connect("127.0.0.1", port, [&](int st) {
    connected = st == 0;
    if (!connected) return;
    cli_h3.reset(new uvcpp_h3_connection(client.connection(),
                                         /*server_side=*/false));
    uvcpp_h3_connection::callbacks hcb;
    hcb.on_error      = [](uvcpp_h3_connection&, int) {};
    hcb.on_disconnect = [](uvcpp_h3_connection&) {};
    cli_h3->set_h3_datagram_enabled(true);
    check(cli_h3->start(hcb) == 0, "客户端 h3 start");
  });
  check(crc == 0, "client connect 受理");
  if (crc != 0) return;
  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "QUIC 握手完成");

  uvcpp_h3_wt_session::callbacks wcb;
  wcb.on_datagram = [&](uvcpp_h3_wt_session&, const uint8_t* d, size_t n) {
    cli_dgram_got.assign(reinterpret_cast<const char*>(d), n);
  };
  wcb.on_open  = [&](uvcpp_h3_wt_session&) { cli_opened = true; };
  wcb.on_close = [&](uvcpp_h3_wt_session&, uint32_t code, const std::string&) {
    cli_close_code = static_cast<int>(code);
  };
  check(uvcpp_test::wait_until(
            &loop,
            [&] {
              cli_wt = uvcpp_h3_wt_session::connect(*cli_h3, "/wt",
                                                    "localhost", wcb);
              return cli_wt != nullptr;
            },
            kDeadlineMs),
        "客户端开 WebTransport 会话");
  if (!cli_wt) return;
  // 客户端此刻还没收到 2xx：会话处于 pending，早发的流/datagram 先排队。
  check(cli_wt->is_pending(), "客户端会话处于 pending（等 2xx）");
  uvcpp_h3_wt_stream* early = cli_wt->open_bidi_stream();
  check(early != nullptr, "pending 期间也能开流（先排队）");
  if (early) check(early->is_pending(), "排队的流 is_pending");
  if (early) check(early->write("early-bidi", 10) == 0, "pending 期间写流（排队）");
  check(cli_wt->send_datagram("early-dgram", 11) == 0,
        "pending 期间发 datagram（排队）");

  check(uvcpp_test::wait_until(&loop, [&] { return cli_opened; }, kDeadlineMs),
        "客户端收到 2xx，on_open 到达");
  check(cli_wt->is_open(), "会话 is_open");
  check(uvcpp_test::wait_until(&loop, [&] { return server_accepted; },
                               kDeadlineMs),
        "服务端接受了会话");
  if (early) {
    std::string got;
    early->on_data([&](const uint8_t* d, size_t n) {
      got.assign(reinterpret_cast<const char*>(d), n);
    });
    check(uvcpp_test::wait_until(&loop, [&] { return got == "early-bidi"; },
                                 kDeadlineMs),
          "排队流的回显到达（实际 \"" + got + "\"）");
  }
  check(uvcpp_test::wait_until(
            &loop, [&] { return cli_dgram_got == "early-dgram"; }, kDeadlineMs),
        "排队 datagram 的回显到达（实际 \"" + cli_dgram_got + "\"）");

  // ---- 双向流：开流 → 写 → 回显 --------------------------------------
  uvcpp_h3_wt_stream* bidi = nullptr;
  check(uvcpp_test::wait_until(
            &loop, [&] { bidi = cli_wt->open_bidi_stream(); return bidi != nullptr; },
            kDeadlineMs),
        "客户端开双向流");
  if (bidi) {
    std::string bidi_got;
    bidi->on_data([&](const uint8_t* d, size_t n) {
      bidi_got.assign(reinterpret_cast<const char*>(d), n);
    });
    check(bidi->write("hello-bidi", 10) == 0, "双向流写数据");
    check(uvcpp_test::wait_until(&loop,
                                 [&] { return bidi_got == "hello-bidi"; },
                                 kDeadlineMs),
          "双向流回显到达客户端（实际 \"" + bidi_got + "\"）");
    check(srv_bidi_got == "hello-bidi", "服务端收到双向流数据");
  }

  // ---- 单向流：开流 → 写 → FIN --------------------------------------
  uvcpp_h3_wt_stream* uni = nullptr;
  check(uvcpp_test::wait_until(
            &loop, [&] { uni = cli_wt->open_uni_stream(); return uni != nullptr; },
            kDeadlineMs),
        "客户端开单向流");
  if (uni) {
    check(uni->write("hello-uni", 9) == 0, "单向流写数据");
    check(uni->end() == 0, "单向流 FIN");
    check(uvcpp_test::wait_until(&loop, [&] { return srv_uni_end; },
                                 kDeadlineMs),
          "服务端收到单向流的结束");
    check(srv_uni_got == "hello-uni", "服务端收到单向流数据");
  }

  // ---- datagram -------------------------------------------------------
  check(cli_wt->send_datagram("hello-dgram", 11) == 0, "客户端发 datagram");
  check(uvcpp_test::wait_until(
            &loop, [&] { return cli_dgram_got == "hello-dgram"; }, kDeadlineMs),
        "datagram 回显到达客户端（实际 \"" + cli_dgram_got + "\"）");
  check(srv_dgram_got == "hello-dgram", "服务端收到 datagram");

  // ---- Drain capsule --------------------------------------------------
  check(cli_wt->drain() == 0, "客户端发 WT_DRAIN_SESSION");
  check(uvcpp_test::wait_until(&loop, [&] { return srv_drain_seen; },
                               kDeadlineMs),
        "服务端收到 on_drain");

  // ---- Close capsule --------------------------------------------------
  check(cli_wt->close(42, "bye") == 0, "客户端发 WT_CLOSE_SESSION");
  // 本端 close 必须把关联流按 WT_SESSION_GONE 重置 —— 服务端能看到流收场。
  check(uvcpp_test::wait_until(&loop, [&] { return srv_bidi_end; }, kDeadlineMs),
        "本端 close 之后对端流被重置（on_end）");
  check(uvcpp_test::wait_until(&loop, [&] { return srv_close_code == 42; },
                               kDeadlineMs),
        "服务端 on_close 收到 code=42（实际 " +
            std::to_string(srv_close_code) + "）");
  check(srv_close_reason == "bye",
        "服务端收到关闭说明（实际 \"" + srv_close_reason + "\"）");
  check(cli_close_code == -1, "本端主动关闭不补 on_close");

  cli_wt.reset();

  // ---- 非 2xx：会话建立失败 ------------------------------------------
  {
    uvcpp_h3_wt_session::callbacks rcb;
    rcb.on_open  = [&](uvcpp_h3_wt_session&) { cli_opened = true; };
    rcb.on_error = [&](uvcpp_h3_wt_session&, int code, const std::string&) {
      rej_error = code;
    };
    rcb.on_close = [&](uvcpp_h3_wt_session&, uint32_t code, const std::string&) {
      rej_close = static_cast<int>(code);
    };
    cli_opened = false;
    std::unique_ptr<uvcpp_h3_wt_session> rejected;
    check(uvcpp_test::wait_until(
              &loop,
              [&] {
                rejected = uvcpp_h3_wt_session::connect(*cli_h3, "/reject",
                                                        "localhost", rcb);
                return rejected != nullptr;
              },
              kDeadlineMs),
          "开一条会被拒的会话");
    const bool rejected_ok = uvcpp_test::wait_until(
        &loop, [&] { return rej_error == 404; }, kDeadlineMs);
    check(rejected_ok,
          "非 2xx 触发 on_error(404)（实际 " + std::to_string(rej_error) + "）");
    check(!cli_opened, "被拒的会话不触发 on_open");
    check(uvcpp_test::wait_until(&loop, [&] { return rej_close == 0; },
                                 kDeadlineMs),
          "被拒的会话随后 on_close");
    rejected.reset();
  }
}

}  // namespace

int main() {
  std::cout << "WebTransport over HTTP/3" << std::endl;
  test_webtransport();
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
