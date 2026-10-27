/**
 * @file tests/functional/web_app_h3_http_func.cpp
 * @brief `uvcpp_web_app` 的**普通 HTTP** 请求走 h3（#58 的后半）。
 *
 * 这条腿从 1.4.1 起一直是坏的，两处断点：
 *
 *   1. `start()` 无条件安装 `set_stream_claim()`，而 HTTP 层的 h3 派发器只要
 *      看到"装了认领钩子"就拒绝**整条** h3 请求（500 + stderr）—— 哪怕应用
 *      一条流式路由都没有；
 *   2. 就算把钩子摘掉，应用的响应也走 `send_response()` 里的
 *      `reg_client(connection_id)`，h3 上没有 TCP client ⇒ 响应被丢弃。
 *
 * 所以这里的判据只能是**端到端 200 + 响应体**，两条各钉一次：
 *
 *   - `/hello`：同步处理函数；
 *   - `/deferred`：处理函数留住 `next`、由 `app.post()` 稍后回话 —— 这条走的是
 *     `req.reply` 那条凭据（h3 上唯一的回话入口），顺带钉住 `take_from()` 把
 *     凭据带进 web 层这件事。
 *
 * 另外断言**登记表收得回来**：h3 请求没有真连接，框架为它补登记的是一条
 * "没有 client"的记录，收尾必须摘掉，否则每条请求都留一条。
 */

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_HTTP3_ENABLE && UVCPP_WEB_SERVER_ENABLE

#include <handle/uvcpp_loop.h>
#include <net/uvcpp_tcp_client.h>
#include <quic/uvcpp_quic_client.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>

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

struct TestApp {
  uvcpp_web_app    app;
  uvcpp_ssl_context server_ctx{tls_mode::SERVER, tls_version::TLS_1_3};

  std::mutex mu;
  uvcpp_web_response* deferred_resp = nullptr;

  void build() {
    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN);

    app.get("/hello",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.text("hello-h3");
              resp.end();
            });

    // 留住 `next`（引用计数 +1 ⇒ 链挂起），由 `post()` 稍后回话。
    app.get("/deferred",
            [this](uvcpp_web_request&, uvcpp_web_response& resp,
                   uvcpp_web_next next) {
              std::shared_ptr<uvcpp_web_next> held(
                  new uvcpp_web_next(next));
              {
                std::lock_guard<std::mutex> lk(mu);
                deferred_resp = &resp;
              }
              app.post([this, held]() {
                uvcpp_web_response* r = nullptr;
                {
                  std::lock_guard<std::mutex> lk(mu);
                  r = deferred_resp;
                  deferred_resp = nullptr;
                }
                if (r != nullptr) {
                  r->text("deferred-h3");
                  r->end();
                }
                (*held)();
              });
            });
  }

  int start() {
    if (!server_ctx.generate_self_signed("localhost", 2048)) return -1;
    app.http_server()->set_quic_ssl_context(&server_ctx);
    if (app.http_server()->listen_quic(0, "127.0.0.1") != 0) return -1;
    const int qport = app.http_server()->quic_listen_port();
    if (app.start_background() != 0) return -1;
    return qport;
  }
  void shutdown() {
    app.stop();
    app.join();
  }
};

struct result {
  bool        done   = false;
  int         err    = 12345;
  int         status = 0;
  int         ver    = -1;
  std::string body;
};

void test_h3_http() {
  TestApp t;
  t.build();
  const int qport = t.start();
  check(qport > 0, "app 的 QUIC 端点起来（端口 " + std::to_string(qport) + "）");
  if (qport <= 0) {
    t.shutdown();
    return;
  }

  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  uvcpp_http_client h3c;
  h3c.set_http3_enabled(true);
  h3c.set_ssl_context(&client_ctx);
  uvcpp_loop* loop = h3c.get_tcp_client()->get_loop();

  bool connected = false;
  check(h3c.connect("127.0.0.1", qport, [&](int st) {
          connected = st == 0;
        }) == 0,
        "h3 connect 受理");
  check(uvcpp_test::wait_until(loop, [&] { return connected; }, kDeadlineMs),
        "h3 握手完成");
  if (!connected) {
    t.shutdown();
    return;
  }
  check(h3c.negotiated_alpn() == "h3",
        "ALPN = h3（实际 \"" + h3c.negotiated_alpn() + "\"）");

  // ---- 同步响应 -------------------------------------------------------
  {
    result r;
    check(h3c.send(uvcpp_http_request::make_get("/hello"),
                   [&](const uvcpp_http_response& resp, int err) {
                     r.done   = true;
                     r.err    = err;
                     r.status = static_cast<int>(resp.status_code);
                     r.ver    = static_cast<int>(resp.version);
                     r.body   = resp.body.to_string();
                   }) == 0,
          "GET /hello 受理");
    check(uvcpp_test::wait_until(loop, [&] { return r.done; }, kDeadlineMs),
          "GET /hello 响应到达");
    check(r.err == 0, "GET /hello 干净收场（err=" + std::to_string(r.err) + "）");
    check(r.status == 200, "GET /hello → 200（实际 " + std::to_string(r.status) + "）");
    check(r.body == "hello-h3", "GET /hello 响应体（实际 \"" + r.body + "\"）");
    check(r.ver == static_cast<int>(uvcpp_http_version::HVER_30),
          "GET /hello 版本是 HTTP/3（实际 " + std::to_string(r.ver) + "）");
  }

  // ---- 稍后回话（req.reply 那条路） -----------------------------------
  {
    result r;
    check(h3c.send(uvcpp_http_request::make_get("/deferred"),
                   [&](const uvcpp_http_response& resp, int err) {
                     r.done   = true;
                     r.err    = err;
                     r.status = static_cast<int>(resp.status_code);
                     r.body   = resp.body.to_string();
                   }) == 0,
          "GET /deferred 受理");
    check(uvcpp_test::wait_until(loop, [&] { return r.done; }, kDeadlineMs),
          "GET /deferred 响应到达");
    check(r.err == 0, "GET /deferred 干净收场（err=" + std::to_string(r.err) + "）");
    check(r.status == 200,
          "GET /deferred → 200（实际 " + std::to_string(r.status) + "）");
    check(r.body == "deferred-h3",
          "GET /deferred 响应体（实际 \"" + r.body + "\"）");
  }

  // ---- 登记表收得回来 -------------------------------------------------
  check(uvcpp_test::wait_until(loop, [&] { return t.app.connection_count() == 0; },
                               kDeadlineMs),
        "h3 的临时登记项已摘掉（connection_count = " +
            std::to_string(t.app.connection_count()) + "）");

  t.shutdown();
}

}  // namespace

int main() {
  std::cout << "web_app over HTTP/3 (#58)" << std::endl;
  test_h3_http();
  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "all checks passed" << std::endl;
  return 0;
}

#else  // !(webapp && http3 && web server)

int main() {
  std::cout << "SKIP: webapp / http3 disabled" << std::endl;
  return 0;
}

#endif
