/**
 * @file tests/functional/web_app_http3_stream_func.cpp
 * @brief `uvcpp_web_app` 在 h3 上的流式响应（`begin_chunked` / `write_chunk` /
 *        `end`，1.9.0 / 1.8.13-dev）。
 *
 * 1.9.0 之前 webapp 的 h3 分支对 `r.streaming()` 是**直接回 500** 的。现在接进
 * 了同一套框架流水线，只是出口换成「稍后回话」凭据（h3 没有 `uvcpp_tcp_client`）：
 *
 *   `begin_chunked()` → `begin_h3_stream()`（HEADERS，不结束发送方向）
 *   `write_chunk()`   → `write_h3_stream()`（QUIC 流上的 DATA，**没有** h1 的
 *                        hex 帧 —— `on_h2_stream()` 判的是 `resp.stream_id != 0`）
 *   `end()`           → `end_h3_stream()`（QUIC 流 FIN）
 *
 * 判据：
 *
 * | # | 场景 | 期望 |
 * |---|---|---|
 * | 1 | 两块 body + end | 200、body 逐字节等于拼接值、无 TE/CL、`on_sent` 一次且 streamed |
 * | 2 | 只有 end（零块） | 200、body 空、干净收场 |
 * | 3 | HEAD 打到同一个流式路由 | 200、body 空（一个 DATA 帧都不发） |
 *
 * 第 1 条顺带钉住"组帧由传输层承担"：若框架按 h1 组帧，body 会变成
 * `5\r\nalpha\r\n...0\r\n\r\n`，逐字节比较当场红。
 */

#include <atomic>
#include <cstring>
#include <iostream>
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

/// `on_sent` 的采样点（第 1 条用）。
struct sent_probe {
  std::atomic<int>  calls{0};
  std::atomic<bool> streamed{false};
  std::atomic<bool> ok{false};
  std::atomic<int>  bytes{0};
};

struct TestApp {
  uvcpp_web_app    app;
  uvcpp_ssl_context server_ctx{tls_mode::SERVER, tls_version::TLS_1_3};
  sent_probe       sent;

  void build() {
    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN);

    app.get("/stream",
            [this](uvcpp_web_request&, uvcpp_web_response& resp,
                   uvcpp_web_next) {
              resp.begin_chunked("text/plain");
              resp.on_sent([this](const uvcpp_web_sent_info& si) {
                sent.calls.fetch_add(1);
                sent.streamed.store(si.streamed);
                sent.ok.store(si.ok);
                sent.bytes.store(static_cast<int>(si.body_bytes));
              });
              resp.write_chunk("alpha");
              resp.write_chunk("beta");
              resp.end();
            });

    app.get("/empty-stream",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.begin_chunked("text/plain");
              resp.end();
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
};

void test_h3_stream() {
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
    const bool sent =
        h3c.send(req, [&](const uvcpp_http_response& resp, int err) {
          r.done    = true;
          r.err     = err;
          r.status  = static_cast<int>(resp.status_code);
          r.headers = resp.headers;
          r.body    = resp.body.to_string();
        }) == 0;
    if (!sent) return false;
    return uvcpp_test::wait_until(loop, [&] { return r.done; }, kDeadlineMs);
  };

  // ---- 1) 两块 body ----------------------------------------------------
  {
    result r;
    check(do_request("GET", "/stream", r), "GET /stream 响应到达");
    check(r.err == 0, "GET /stream 干净收场（err=" + std::to_string(r.err) + "）");
    check(r.status == 200, "GET /stream → 200");
    // 逐字节比较：h1 的组帧（`5\r\nalpha\r\n...`）会在这里当场现形。
    check(r.body == "alphabeta",
          "body 逐字节等于拼接值（实际 " + std::to_string(r.body.size()) +
              " 字节：\"" + r.body + "\"）");
    bool has_te = false;
    bool has_cl = false;
    for (size_t i = 0; i < r.headers.size(); ++i) {
      std::string n = r.headers[i].name;
      for (size_t k = 0; k < n.size(); ++k) {
        if (n[k] >= 'A' && n[k] <= 'Z') n[k] = static_cast<char>(n[k] + 32);
      }
      if (n == "transfer-encoding") has_te = true;
      if (n == "content-length") has_cl = true;
    }
    check(!has_te, "首部里没有 transfer-encoding（h3 没有 chunked）");
    check(!has_cl, "首部里没有 content-length（流式边界是 FIN）");
  }

  // ---- 2) 零块流 -------------------------------------------------------
  {
    result r;
    check(do_request("GET", "/empty-stream", r), "GET /empty-stream 响应到达");
    check(r.err == 0,
          "GET /empty-stream 干净收场（err=" + std::to_string(r.err) + "）");
    check(r.status == 200, "GET /empty-stream → 200");
    check(r.body.empty(), "零块的流式响应 body 为空");
  }

  // ---- 3) HEAD 打在流式路由上 -----------------------------------------
  //
  // 走 `/empty-stream` 而不是 `/stream`：`on_sent` 的采样点只装在 /stream 的
  // 处理函数里，HEAD 也走同一个 GET 路由，会把那个计数器一并 +1。
  {
    result r;
    check(do_request("HEAD", "/empty-stream", r), "HEAD /empty-stream 响应到达");
    check(r.err == 0,
          "HEAD /empty-stream 干净收场（err=" + std::to_string(r.err) + "）");
    check(r.status == 200, "HEAD /empty-stream → 200");
    check(r.body.empty(), "HEAD 一个字节的 body 都没有");
  }

  // ---- on_sent：流式收尾时恰好一次，streamed 为真 ---------------------
  check(uvcpp_test::wait_until(loop, [&] { return t.sent.calls.load() > 0; },
                               kDeadlineMs),
        "on_sent 触发了");
  check(t.sent.calls.load() == 1,
        "on_sent 恰好一次（实测 " + std::to_string(t.sent.calls.load()) + "）");
  check(t.sent.streamed.load(), "on_sent 的 streamed 为真");
  check(t.sent.ok.load(), "on_sent 的 ok 为真");
  check(t.sent.bytes.load() == 9,
        "on_sent 的 body_bytes = 9（实测 " +
            std::to_string(t.sent.bytes.load()) + "）");

  check(uvcpp_test::wait_until(loop, [&] { return t.app.connection_count() == 0; },
                               kDeadlineMs),
        "h3 的临时登记项已摘掉（connection_count = " +
            std::to_string(t.app.connection_count()) + "）");

  t.shutdown();
}

}  // namespace

int main() {
  std::cout << "web_app 在 HTTP/3 上的流式响应" << std::endl;
  test_h3_stream();
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
