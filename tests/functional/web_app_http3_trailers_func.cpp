/**
 * @file tests/functional/web_app_http3_trailers_func.cpp
 * @brief `uvcpp_web_app` 在 h3 上的尾随头（1.9.0 / 1.8.13-dev）。
 *
 * 1.9.0 之前 webapp 的 h3 分支对 `set_trailers()` 是**直接回 500** 的
 * （"h3 上不支持流式/尾随头响应"）。h3 连接层其实早就支持（`h3_response::
 * trailers` → 会话层的第二个 HEADERS 块），断的只是框架那道出口。
 *
 * 判据：
 *
 * | # | 场景 | 期望 |
 * |---|---|---|
 * | 1 | 有 body + 尾随头 | 200、body 正确、字段在 `trailers` 而不在 `headers` |
 * | 2 | 空 body + 尾随头（只有尾随块） | 200、body 空、字段仍到 |
 * | 3 | HEAD（`head_only`） | 200、body 空、尾随头被丢弃 |
 * | 4 | 204 + 尾随头 | 204、body 空、字段到、且**不带** `content-length` |
 *
 * 第 2 条是这一批真正的新能力：零长 body 时 `dr == NULL` 会关掉写方向，尾随块
 * 就提交不上去。会话层为此加了 `trailers_follow` —— 空 body 也给一个立刻 EOF
 * 的 data reader，把"末块之后那段字段"的位置留出来。
 *
 * 第 4 条的 `content-length` 断言顺带钉住 h3 那一支的 RFC 9110 §8.6 修正：
 * 空体响应自动补 `content-length: 0` 时要放过 1xx / 204 / 304。
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

http_headers make_trailers() {
  http_headers tr;
  http_header  h;
  h.name  = "x-trailer-1";
  h.value = "alpha";
  tr.push_back(h);
  h.name  = "x-trailer-2";
  h.value = "";
  tr.push_back(h);
  return tr;
}

bool has_named(const http_headers& hs, const std::string& name) {
  for (size_t i = 0; i < hs.size(); ++i) {
    if (hs[i].name == name) return true;
  }
  return false;
}

struct TestApp {
  uvcpp_web_app    app;
  uvcpp_ssl_context server_ctx{tls_mode::SERVER, tls_version::TLS_1_3};

  void build() {
    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN);

    app.get("/tr",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.text("hello");
              resp.set_trailers(make_trailers());
            });

    app.get("/tr-empty",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.set_trailers(make_trailers());
            });

    app.get("/head",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.text("hello");
              resp.set_trailers(make_trailers());
            });

    app.get("/no-body",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.status(http_status::NO_CONTENT);
              resp.set_trailers(make_trailers());
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
  bool         done   = false;
  int          err    = 12345;
  int          status = 0;
  std::string  body;
  http_headers headers;
  http_headers trailers;
};

void test_h3_trailers() {
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

  auto do_request = [&](const char* method, const char* path, result& r) {
    uvcpp_http_request req = uvcpp_http_request::make_get(path);
    req.method = std::strcmp(method, "HEAD") == 0 ? http_method::HTTP_HEAD
                                                  : http_method::HTTP_GET;
    const bool sent = h3c.send(req, [&](const uvcpp_http_response& resp,
                                         int err) {
      r.done     = true;
      r.err      = err;
      r.status   = static_cast<int>(resp.status_code);
      r.headers  = resp.headers;
      r.trailers = resp.trailers;
      r.body     = resp.body.to_string();
    }) == 0;
    if (!sent) return false;
    return uvcpp_test::wait_until(loop, [&] { return r.done; }, kDeadlineMs);
  };

  // ---- 1) 有 body + 尾随头 -------------------------------------------
  {
    result r;
    check(do_request("GET", "/tr", r), "GET /tr 响应到达");
    check(r.err == 0, "GET /tr 干净收场（err=" + std::to_string(r.err) + "）");
    check(r.status == 200, "GET /tr → 200");
    check(r.body == "hello", "GET /tr body（实际 \"" + r.body + "\"）");
    check(has_named(r.trailers, "x-trailer-1"),
          "尾随头 x-trailer-1 到了");
    check(has_named(r.trailers, "x-trailer-2"),
          "空值尾随头 x-trailer-2 也在");
    check(!has_named(r.headers, "x-trailer-1"),
          "尾随头**不在**首部块里");
  }

  // ---- 2) 空 body + 尾随头（只有尾随块） ------------------------------
  {
    result r;
    check(do_request("GET", "/tr-empty", r), "GET /tr-empty 响应到达");
    check(r.err == 0, "GET /tr-empty 干净收场（err=" + std::to_string(r.err) + "）");
    check(r.status == 200, "GET /tr-empty → 200");
    check(r.body.empty(), "GET /tr-empty body 为空");
    check(has_named(r.trailers, "x-trailer-1"),
          "空 body 的尾随头也到了（这一批的新能力）");
    check(!has_named(r.headers, "x-trailer-1"),
          "空 body 时尾随头同样不在首部块里");
  }

  // ---- 3) HEAD：尾随头丢弃 -------------------------------------------
  {
    result r;
    check(do_request("HEAD", "/head", r), "HEAD /head 响应到达");
    check(r.err == 0, "HEAD /head 干净收场（err=" + std::to_string(r.err) + "）");
    check(r.status == 200, "HEAD /head → 200");
    check(r.body.empty(), "HEAD /head body 为空");
    check(!has_named(r.trailers, "x-trailer-1"),
          "HEAD 上尾随头被丢弃（没有末块可挂）");
  }

  // ---- 4) 204：允许带尾随块，但不许带 content-length ------------------
  {
    result r;
    check(do_request("GET", "/no-body", r), "GET /no-body 响应到达");
    check(r.err == 0, "GET /no-body 干净收场（err=" + std::to_string(r.err) + "）");
    check(r.status == 204, "GET /no-body → 204");
    check(r.body.empty(), "204 没有 body");
    check(!has_named(r.headers, "content-length"),
          "204 不带 content-length（RFC 9110 §8.6）");
    check(has_named(r.trailers, "x-trailer-1"),
          "204 的尾随头也到了（h3 的尾随块不是 content）");
  }

  check(uvcpp_test::wait_until(loop, [&] { return t.app.connection_count() == 0; },
                               kDeadlineMs),
        "h3 的临时登记项已摘掉（connection_count = " +
            std::to_string(t.app.connection_count()) + "）");

  t.shutdown();
}

}  // namespace

int main() {
  std::cout << "web_app 在 HTTP/3 上的尾随头" << std::endl;
  test_h3_trailers();
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
