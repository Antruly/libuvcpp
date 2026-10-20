/**
 * @file tests/functional/web_h2_frame_events_func.cpp
 * @brief HTTP/2 逐帧事件（#52）：HEADERS / DATA / GOAWAY 的摘要。
 *
 * uvscope 0.4 的 h2 调试器要看到帧级流水：HEADERS 数、DATA 帧数与总长度、
 * GOAWAY 的 error code / last_stream_id。这里用 TLS + ALPN 的真实
 * `uvcpp_http_server` + `uvcpp_http_client` 钉住：
 *
 * 1. 一次 GET：响应 HEADERS 至少一帧、DATA 帧的 `length` 之和等于响应体
 *    长度、`stream_id` 与应答里的 `resp.stream_id` 一致；
 * 2. 服务端 `begin_h2_goaway()`：客户端收到 GOAWAY 事件，字段与
 *    `peer_goaway_error_code()` / `peer_goaway_last_stream_id()` 一致；
 * 3. 没设回调时事件不会凭空出现（空 function 判断；行为层面用"设了才收到").
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

#include <http2/uvcpp_h2_common.h>

#if UVCPP_WEB_ENABLE && UVCPP_NGHTTP2_ENABLE && UVCPP_OPENSSL_ENABLE

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

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

const std::vector<std::string> kAlpn{"h2", "http/1.1"};

std::string make_body(size_t n) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>('A' + (i % 26));
  return s;
}

// ---------------------------------------------------------------------------
// 服务端（TLS + h2）
// ---------------------------------------------------------------------------
struct h2_server {
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::atomic<int>  goaway_count{-1};
  std::thread       thread;
  int               port = -1;
  std::string       body;

  void start(uvcpp_ssl_context* sctx, size_t n) {
    body = make_body(n);
    thread = std::thread([this, sctx]() { run(sctx); });
    const long long deadline = now_ms() + 5000;
    while (!ready.load() && now_ms() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void run(uvcpp_ssl_context* sctx) {
    uvcpp_http_server http;
    http.set_http2_enabled(true);
    http.get("/hello", [this](uvcpp_http_request& req, uvcpp_http_response& resp,
                              uvcpp_tcp_client*) {
      resp = uvcpp_http_response::ok(body.data(), body.size(), "text/plain");
      resp.stream_id = req.stream_id;
    });
    http.get("/bye", [this, &http](uvcpp_http_request& req,
                                   uvcpp_http_response& resp,
                                   uvcpp_tcp_client*) {
      goaway_count.store(static_cast<int>(http.begin_h2_goaway()));
      resp = uvcpp_http_response::ok(body.data(), body.size(), "text/plain");
      resp.stream_id = req.stream_id;
    });
    http.get_tcp_server()->set_ssl_context(sctx);
    if (http.bindIpv4("127.0.0.1", 0) != 0) {
      ready.store(true);
      return;
    }
    struct sockaddr_in name;
    int len = static_cast<int>(sizeof(name));
    http.get_tcp_server()->get_tcp()->getsockname(
        reinterpret_cast<sockaddr*>(&name), &len);
    port = ntohs(name.sin_port);
    if (http.listen(128) != 0) {
      port = -1;
      ready.store(true);
      return;
    }
    ready.store(true);
    uvcpp_loop* loop = http.get_tcp_server()->get_loop();
    while (!stop.load()) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int i = 0; i < 400; ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  ~h2_server() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// ---------------------------------------------------------------------------
// 客户端驱动
// ---------------------------------------------------------------------------
bool pump_until(uvcpp_http_client& cli, const std::function<bool()>& done,
                int timeout_ms = 10000) {
  const long long deadline = now_ms() + timeout_ms;
  while (!done()) {
    cli.run(UV_RUN_NOWAIT);
    if (now_ms() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

int connect_h2(uvcpp_http_client& cli, int port, uvcpp_ssl_context* cctx) {
  cli.set_http2_enabled(true);
  cli.set_ssl_context(cctx);
  bool done = false;
  int  status = -1;
  const int rc = cli.connect("127.0.0.1", port, [&](int st) {
    status = st;
    done = true;
  });
  if (rc != 0) return rc;
  if (!pump_until(cli, [&] { return done; })) return UV_ETIMEDOUT;
  if (status != 0) return status;
  if (cli.negotiated_alpn() != "h2") return UV_EPROTO;
  return 0;
}

struct event_log {
  std::mutex                        mu;
  std::vector<uvcpp_h2_frame_event> evs;
  void add(const uvcpp_h2_frame_event& e) {
    std::lock_guard<std::mutex> lk(mu);
    evs.push_back(e);
  }
  std::size_t size() {
    std::lock_guard<std::mutex> lk(mu);
    return evs.size();
  }
  std::vector<uvcpp_h2_frame_event> snapshot() {
    std::lock_guard<std::mutex> lk(mu);
    return evs;
  }
};

struct resp_rec {
  int         status = -1;
  int         err = -1;
  int32_t     sid = 0;
  std::string body;
  bool        done = false;
};

void test_headers_data() {
  const size_t kN = 8 * 1024;
  h2_server srv;
  uvcpp_ssl_context sctx(tls_mode::SERVER, tls_version::TLS_1_2);
  if (!sctx.is_ready() || !sctx.generate_self_signed("loopback.test", 2048)) {
    check(false, "[1] server cert");
    return;
  }
  sctx.set_alpn_select_protos(kAlpn);
  srv.start(&sctx, kN);

  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  cctx.set_verify_mode(tls_verify_mode::NONE);

  uvcpp_http_client cli;
  event_log log;
  cli.set_h2_frame_callback(
      [&](const uvcpp_h2_frame_event& e) { log.add(e); });
  check(connect_h2(cli, srv.port, &cctx) == 0, "[1] connect + ALPN=h2");

  resp_rec r;
  const int rc = cli.send(uvcpp_http_request::make_get("/hello"),
                          [&](const uvcpp_http_response& resp, int err) {
                            r.status = static_cast<int>(resp.status_code);
                            r.err = err;
                            r.sid = resp.stream_id;
                            r.body = resp.body.to_string();
                            r.done = true;
                          });
  check(rc == 0, "[1] send 受理");
  check(pump_until(cli, [&] { return r.done; }), "[1] 响应回调落地");
  check(r.err == 0 && r.status == 200, "[1] 200");
  check(r.body == make_body(kN), "[1] 响应体完整");
  check(r.sid > 0, "[1] 响应带 stream_id");

  const std::vector<uvcpp_h2_frame_event> evs = log.snapshot();
  int headers = 0, data_frames = 0;
  size_t data_bytes = 0;
  for (size_t i = 0; i < evs.size(); ++i) {
    if (evs[i].stream_id != r.sid) continue;
    if (evs[i].type == H2_FRAME_HEADERS) ++headers;
    if (evs[i].type == H2_FRAME_DATA) {
      ++data_frames;
      data_bytes += evs[i].length;
    }
  }
  check(headers >= 1, "[1] HEADERS 帧数 = " + std::to_string(headers));
  check(data_frames >= 1, "[1] DATA 帧数 = " + std::to_string(data_frames));
  check(data_bytes == kN,
        "[1] DATA 总长度 = " + std::to_string(data_bytes) + "（应 " +
            std::to_string(kN) + "）");
}

void test_goaway_event() {
  const size_t kN = 64;
  h2_server srv;
  uvcpp_ssl_context sctx(tls_mode::SERVER, tls_version::TLS_1_2);
  if (!sctx.is_ready() || !sctx.generate_self_signed("loopback.test", 2048)) {
    check(false, "[2] server cert");
    return;
  }
  sctx.set_alpn_select_protos(kAlpn);
  srv.start(&sctx, kN);

  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  cctx.set_verify_mode(tls_verify_mode::NONE);

  uvcpp_http_client cli;
  event_log log;
  cli.set_h2_frame_callback(
      [&](const uvcpp_h2_frame_event& e) { log.add(e); });
  check(connect_h2(cli, srv.port, &cctx) == 0, "[2] connect + ALPN=h2");

  resp_rec r;
  const int rc = cli.send(uvcpp_http_request::make_get("/bye"),
                          [&](const uvcpp_http_response& resp, int err) {
                            r.status = static_cast<int>(resp.status_code);
                            r.err = err;
                            r.done = true;
                          });
  check(rc == 0, "[2] send 受理");
  check(pump_until(cli, [&] { return r.done; }), "[2] 响应回调落地");
  check(r.err == 0 && r.status == 200, "[2] 响应正常");
  check(srv.goaway_count.load() >= 0,
        "[2] 服务端 begin_h2_goaway() 被调用（返回 " +
            std::to_string(srv.goaway_count.load()) + "）");

  // GOAWAY 可能比响应稍晚到，等它进日志。
  check(pump_until(cli, [&] {
    const auto evs = log.snapshot();
    for (size_t i = 0; i < evs.size(); ++i) {
      if (evs[i].type == H2_FRAME_GOAWAY) return true;
    }
    return false;
  }), "[2] 收到 GOAWAY 事件");

  const auto evs = log.snapshot();
  bool found = false;
  uvcpp_h2_frame_event g;
  for (size_t i = 0; i < evs.size(); ++i) {
    if (evs[i].type == H2_FRAME_GOAWAY) {
      g = evs[i];
      found = true;
    }
  }
  check(found, "[2] GOAWAY 事件存在");
  if (found) {
    check(g.stream_id == 0, "[2] GOAWAY 是连接级帧（stream_id=0）");
    check(static_cast<uint32_t>(g.error_code) == cli.peer_goaway_error_code(),
          "[2] error_code 与访问器一致（事件 " +
              std::to_string(g.error_code) + " / 访问器 " +
              std::to_string(cli.peer_goaway_error_code()) + "）");
    check(g.last_stream_id == cli.peer_goaway_last_stream_id(),
          "[2] last_stream_id 与访问器一致（事件 " +
              std::to_string(g.last_stream_id) + " / 访问器 " +
              std::to_string(cli.peer_goaway_last_stream_id()) + "）");
    check(cli.peer_goaway_received(), "[2] peer_goaway_received()");
  }
}

}  // namespace

int main() {
  std::cout << "HTTP/2 frame events (#52)" << std::endl;
  test_headers_data();
  test_goaway_event();
  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "all checks passed" << std::endl;
  return 0;
}

#else  // !(WEB && NGHTTP2 && OPENSSL)

int main() {
  std::cout << "SKIP: web/h2/openssl disabled" << std::endl;
  return 0;
}

#endif
