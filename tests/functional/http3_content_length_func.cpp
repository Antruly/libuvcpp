/**
 * @file tests/functional/http3_content_length_func.cpp
 * @brief HTTP/3 两个方向的 `content-length` 校验（1.9.0 / 1.8.12-dev）。
 *
 * 判据：
 *
 * | # | 场景 | 期望 |
 * |---|---|---|
 * | 1 | 响应 `content-length: 10`、实际 4 字节 | 流错误，**不**交付 200/body |
 * | 2 | 响应 `content-length: 4`、实际 4 字节 | 正常 200 + body |
 * | 3 | HEAD 响应 `content-length: 10`、没有 body | 正常 200、无错误（HEAD 描述的是 GET 的长度）|
 *
 * 为什么值得单开一条：此前这一层**一个方向都没有**校验 —— 一条长度撒谎的
 * 响应会被当正常响应收完，而 h2 那边 1.9.0 起两个方向都校验。
 */

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
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

#include "loop_drain.h"
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

h3_header make_regular(const char* name, const char* value) {
  h3_header h;
  h.name  = name;
  h.value = value;
  h.kind  = h3_header_kind::REGULAR;
  return h;
}

void test_content_length() {
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
  int                                  server_reqs = 0;

  const int lrc = server.listen([&](uvcpp_quic_connection* c) {
    srv_h3.reset(new uvcpp_h3_connection(c, /*server_side=*/true));
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_request = [&](uvcpp_h3_connection& h3, const h3_request& req) {
      ++server_reqs;
      h3_response r;
      r.stream_id = req.stream_id;
      r.status    = 200;
      if (req.path == "/bad") {
        r.headers.push_back(make_regular("content-length", "10"));
        r.body = "abcd";  // 撒谎：只有 4 字节
        check(h3.send_response(r) == 0, "服务端发一条长度撒谎的响应");
      } else if (req.path == "/ok") {
        r.headers.push_back(make_regular("content-length", "4"));
        r.body = "abcd";
        check(h3.send_response(r) == 0, "服务端发一条正常的响应");
      } else {  // HEAD
        r.headers.push_back(make_regular("content-length", "10"));
        check(h3.send_response(r, /*omit_body=*/true) == 0,
              "服务端发 HEAD 的响应（长度描述 GET、无 body）");
      }
    };
    cbs.on_error      = [](uvcpp_h3_connection&, int) {};
    cbs.on_disconnect = [](uvcpp_h3_connection&) {};
    check(srv_h3->start(cbs) == 0, "服务端 h3 start");
  });
  check(lrc == 0, "server listen");
  if (lrc != 0) return;
  const int port = server.configured_port();
  check(port > 0, "server port");

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  std::unique_ptr<uvcpp_h3_connection> cli_h3;
  bool                                 connected = false;
  std::map<int64_t, h3_response>       done;
  std::vector<int32_t>                 order;

  const int crc = client.connect("127.0.0.1", port, [&](int st) {
    connected = st == 0;
    if (!connected) return;
    cli_h3.reset(new uvcpp_h3_connection(client.connection(),
                                         /*server_side=*/false));
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_error      = [](uvcpp_h3_connection&, int code) {
      std::cerr << "  [diag] client h3 fatal: " << code << std::endl;
    };
    cbs.on_disconnect = [](uvcpp_h3_connection&) {
      std::cerr << "  [diag] client h3 disconnect" << std::endl;
    };
    check(cli_h3->start(cbs) == 0, "客户端 h3 start");
  });
  check(crc == 0, "client connect 受理");
  if (crc != 0) return;
  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "QUIC 握手完成");
  check(uvcpp_test::wait_until(&loop, [&] { return cli_h3->ready(); },
                               kDeadlineMs),
        "客户端关键流就绪");

  uvcpp_test::loop_drain drain(&loop);

  // 每发一条就把完成队列抽干，按流号存起来。
  auto send_and_collect = [&](const char* method, const char* path) -> int64_t {
    h3_request req;
    req.method    = method;
    req.path      = path;
    req.authority = "localhost";
    const int64_t sid = cli_h3->send_request(req);
    if (sid < 0) {
      std::cerr << "  [diag] send_request(" << path << ") rc=" << sid << std::endl;
      return sid;
    }
    uvcpp_test::wait_until(
        &loop,
        [&] {
          h3_response r;
          while (cli_h3->take_completed(r)) {
            done[r.stream_id] = r;
            order.push_back(static_cast<int32_t>(r.stream_id));
          }
          return done.find(sid) != done.end();
        },
        kDeadlineMs);
    return sid;
  };

  // ---- 1) 撒谎的响应：流错误 -----------------------------------------
  {
    const int64_t sid = send_and_collect("GET", "/bad");
    check(sid >= 0, "GET /bad 发出去（sid=" + std::to_string(sid) + "）");
    std::map<int64_t, h3_response>::iterator it = done.find(sid);
    check(it != done.end(), "GET /bad 有收场");
    if (it != done.end()) {
      check(it->second.error != 0,
            "长度对不上的响应以错误收场（error=" +
                std::to_string(it->second.error) + "）");
      check(it->second.status != 200 || it->second.body != "abcd",
            "没有把撒谎的响应当正常 200 交付");
    }
  }

  // ---- 2) 正常响应 ----------------------------------------------------
  {
    const int64_t sid = send_and_collect("GET", "/ok");
    std::map<int64_t, h3_response>::iterator it = done.find(sid);
    check(it != done.end() && it->second.error == 0,
          "长度正确的响应干净收场");
    if (it != done.end()) {
      check(it->second.status == 200, "GET /ok → 200");
      check(it->second.body == "abcd", "GET /ok body 正确");
    }
  }

  // ---- 3) HEAD：有长度、没 body ---------------------------------------
  {
    const int64_t sid = send_and_collect("HEAD", "/head");
    std::map<int64_t, h3_response>::iterator it = done.find(sid);
    check(it != done.end() && it->second.error == 0,
          "HEAD 的响应不因 content-length 被判错");
    if (it != done.end()) {
      check(it->second.status == 200, "HEAD /head → 200");
      check(it->second.body.empty(), "HEAD /head body 为空");
    }
  }

  check(server_reqs >= 3,
        "服务端处理了三条请求（实际 " + std::to_string(server_reqs) + "）");

  cli_h3.reset();
}

}  // namespace

int main() {
  std::cout << "HTTP/3 content-length validation" << std::endl;
  test_content_length();
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
