/**
 * @file tests/functional/http3_public_endpoint_func.cpp
 * @brief 真实公网 HTTP/3 端点的回归探针（#53，手动/按需跑）。
 *
 * **默认 SKIP。** 设 `UVCPP_TEST_H3_ENDPOINT=host:port` 才真的连，例如：
 *
 * ```
 * UVCPP_TEST_H3_ENDPOINT=cloudflare-quic.com:443 \
 *   ./tests/functional/test_http3_public_endpoint_func
 * ```
 *
 * CI 没有外网保证，所以它不是默认门禁；发布前或怀疑 QUIC/h3 的互操作时手动跑。
 * 判据只有三件：握手完成、GET `/` 拿到一个 2xx/3xx 响应、连接干净结束。
 */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_HTTP3_ENABLE

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

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

const int kDeadlineMs = 15000;

bool split_host_port(const std::string& s, std::string& host, int& port) {
  const size_t c = s.rfind(':');
  if (c == std::string::npos || c == 0 || c + 1 >= s.size()) return false;
  host = s.substr(0, c);
  port = std::atoi(s.c_str() + c + 1);
  return port > 0 && port <= 65535;
}

void run(const std::string& host, int port) {
  uvcpp_loop loop;

  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  // 默认 PEER + 系统信任库（不关校验）。
  if (!cctx.is_ready()) {
    check(false, "client TLS ctx ready");
    return;
  }

  uvcpp_http_client cli;
  cli.set_http3_enabled(true);
  cli.set_ssl_context(&cctx);

  bool connected = false;
  int  connect_status = -999;
  int rc = cli.connect(host.c_str(), port, [&](int status) {
    connected = true;
    connect_status = status;
  });
  check(rc == 0, "connect 受理");
  if (rc != 0) return;
  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "QUIC 握手完成");
  check(connect_status == 0, "connect status = 0（" +
                                 std::to_string(connect_status) + "）");
  if (connect_status != 0) return;
  check(cli.negotiated_alpn() == "h3",
        "ALPN = h3（实际 \"" + cli.negotiated_alpn() + "\"）");

  bool done = false;
  int  err = -999;
  int  status = 0;
  size_t body_len = 0;
  rc = cli.send(uvcpp_http_request::make_get("/"),
                [&](const uvcpp_http_response& resp, int e) {
                  done = true;
                  err = e;
                  status = static_cast<int>(resp.status_code);
                  body_len = resp.body.size();
                });
  check(rc == 0, "send 受理");
  check(uvcpp_test::wait_until(&loop, [&] { return done; }, kDeadlineMs),
        "响应落地");
  check(err == 0, "响应无错误（err=" + std::to_string(err) + "）");
  check(status >= 200 && status < 400,
        "状态码 2xx/3xx（实际 " + std::to_string(status) + "）");
  std::cout << "  [info] body bytes = " << body_len << std::endl;
}

}  // namespace

int main() {
  const char* ep = std::getenv("UVCPP_TEST_H3_ENDPOINT");
  if (ep == nullptr || *ep == '\0') {
    std::cout << "SKIP: 设置 UVCPP_TEST_H3_ENDPOINT=host:port 才连真实端点"
              << std::endl;
    return 0;
  }
  std::string host;
  int port = 0;
  if (!split_host_port(ep, host, port)) {
    std::cerr << "UVCPP_TEST_H3_ENDPOINT 格式应为 host:port（实得 \"" << ep
              << "\"）" << std::endl;
    return 2;
  }
  std::cout << "HTTP/3 public endpoint: " << host << ":" << port << std::endl;
  run(host, port);
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
