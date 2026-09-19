/**
 * @file tests/functional/web_ssl_h2_client_func.cpp
 * @brief HTTP/2 **客户端**：`uvcpp_http_client` 走 ALPN 分流那一条路。
 *
 * 前面那些 h2 用例都是**手工**驱动会话层的：`h2_session_func` 把两条会话面对面
 * 摆好、自己搬字节，`web_ssl_h2_server_func` 用裸 `uvcpp_tcp_client` +
 * `uvcpp_h2_connection` 拼了一个客户端。于是"用户真正会用的那个入口"——
 * `set_http2_enabled()` + `send()` —— 一行覆盖都没有：它多了三块别处没有的状态，
 * 每一块坏掉都表现为"能编、能连、回调不来"：
 *
 *   1. **ALPN 名单随开关变**（关着钉 `http/1.1`，开着发 `{"h2","http/1.1"}`）；
 *   2. **按流归位的应答表**（`h2_streams_`）——一条连接上并发几条流时，
 *      单槽状态会让后发的请求盖掉先发的，被盖的那条**永远等不到回调**；
 *   3. **同步系列的闸门**——`send_wait()` 在 h2 连接上必须报 `UV_ENOTSUP`，
 *      否则它会往一条正跑二进制帧的连接里写 HTTP/1.1 明文，症状是
 *      "写成功了、响应等不到"，一处报错都没有。
 *
 * 场景 1 单独钉**默认关**：不调 `set_http2_enabled` 时 ALPN 里不能有 h2，
 * 而且服务端明明支持 h2 也不能协商上去（服务端的名单是 `{"h2","http/1.1"}`，
 * 客户端不宣告就不会选到它）。这条同时是向后兼容的判据。
 *
 * 场景 3 是**核心**：`/defer` 挂起、`/hello` 插队、`/release` 放行，把"派发序"
 * 与"应答序"确定性地掰开。单槽状态在这里的失败方式是确定的 —— `/hello` 的提交
 * 会盖掉 `/defer` 的槽位，于是 defer 的回调一次都不跑。所以断言里"每个键恰好
 * 回调一次"比"body 对得上"更关键：body 对得上可以靠巧合，回调次数对不上不能。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_OPENSSL_ENABLE && UVCPP_NGHTTP2_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

#include <http2/uvcpp_h2_connection.h>

#include <openssl/ssl.h>

#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

const std::vector<std::string> kServerAlpn{"h2", "http/1.1"};

const char kHelloBody[]    = "hello-over-h2";
const char kNotFoundBody[] = "404 Not Found";
const char kPostBody[]     = "echo-me-please";
/// 裸对端（场景 7）的应答体。与 `kHelloBody` 不同，好让"这条响应来自哪个
/// 服务端"在判据里也看得见。
const char kRawBody[]      = "raw-peer-ok";

/// 场景 3 的挂起应答体。**长度 ≥ `compress_min_body_`（默认 1024）**，
/// 与 `web_ssl_h2_server_func` 保持同一份理由：太短的话"该压 / 不该压"两侧
/// 都不压，gzip 那条判据会恒真。
std::string make_defer_body() {
  std::string s;
  while (s.size() < 2048) s += "deferred-response-payload-";
  s.resize(2048);
  return s;
}

const std::string kDeferBody = make_defer_body();

// =========================================================================
// 服务端线程
// =========================================================================

struct server_state {
  std::atomic<int>  listen_rc{-1};
  std::atomic<int>  connections{0};
  std::atomic<int>  alpn_h2_at_delivery{0};
  std::atomic<bool> saw_defer{false};
  /// 场景 5：`/bye` 里 `begin_h2_goaway()` 的返回值（-1 = 那条路由没跑到）。
  std::atomic<int>  goaway_count{-1};

  std::mutex                                        pend_mu;
  std::vector<std::pair<uvcpp_tcp_client*, int32_t>> pending;
};

void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                server_state& st, uvcpp_ssl_context* sctx) {
  uvcpp_http_server http;
  http.set_http2_enabled(true);

  http.on_connection([&st](uvcpp_tcp_client* client) {
    if (client != nullptr && client->is_tls() &&
        client->tls_alpn_selected() == "h2") {
      st.alpn_h2_at_delivery.fetch_add(1);
    }
    st.connections.fetch_add(1);
  });

  http.get("/hello", [](uvcpp_http_request& req, uvcpp_http_response& resp,
                        uvcpp_tcp_client*) {
    resp = uvcpp_http_response::ok(kHelloBody, sizeof(kHelloBody) - 1,
                                   "text/plain");
    resp.stream_id = req.stream_id;
  });

  http.post("/echo", [](uvcpp_http_request& req, uvcpp_http_response& resp,
                        uvcpp_tcp_client*) {
    resp = uvcpp_http_response::ok(req.body.get_const_data(), req.body.size(),
                                   "text/plain");
    resp.stream_id = req.stream_id;
  });

  // `/defer` 只把身份记下来就返回；`/release` 到了才替它应答。
  http.get("/defer", [&st](uvcpp_http_request& req, uvcpp_http_response& resp,
                           uvcpp_tcp_client* client) {
    resp.deferred = true;
    st.saw_defer.store(true);
    std::lock_guard<std::mutex> lk(st.pend_mu);
    st.pending.push_back(std::make_pair(client, req.stream_id));
  });

  http.get("/release", [&http, &st](uvcpp_http_request&, uvcpp_http_response& resp,
                                    uvcpp_tcp_client*) {
    std::vector<std::pair<uvcpp_tcp_client*, int32_t>> pend;
    {
      std::lock_guard<std::mutex> lk(st.pend_mu);
      pend.swap(st.pending);
    }
    for (size_t i = 0; i < pend.size(); ++i) {
      uvcpp_http_response r = uvcpp_http_response::ok(
          kDeferBody.data(), kDeferBody.size(), "text/plain");
      r.stream_id = pend[i].second;
      http.send_response(pend[i].first, r, /*close_after_write=*/false);
    }
    resp = uvcpp_http_response::ok("released", 8, "text/plain");
  });

  // ---- 场景 5 的触发口：服务端主动道别 ----------------------------------
  //
  // 用一条路由当触发口，而不是让测试线程直接调 `begin_h2_goaway()`：后者是在
  // **另一个线程**上碰服务端的连接表，而那张表归服务端的循环线程所有。
  //
  // 处理函数**自己就是那条在飞的流**：GOAWAY 排在它的响应之前发出去，于是
  // "GOAWAY 说的 last_stream_id 就是它"和"它的响应照常到达"钉在一条流上。
  http.get("/bye", [&http, &st](uvcpp_http_request& req, uvcpp_http_response& resp,
                                uvcpp_tcp_client*) {
    st.goaway_count.store(static_cast<int>(http.begin_h2_goaway()));
    resp = uvcpp_http_response::ok(kHelloBody, sizeof(kHelloBody) - 1,
                                   "text/plain");
    resp.stream_id = req.stream_id;
  });

  // 必须在 listen() 之前装：accept 时读不到就是这个原因。
  http.get_tcp_server()->set_ssl_context(sctx);

  if (http.bindIpv4("127.0.0.1", 0) != 0) {
    port_promise.set_value(-1);
    return;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  http.get_tcp_server()->get_tcp()->getsockname(
      reinterpret_cast<sockaddr*>(&name), &namelen);
  const int bound_port = ntohs(name.sin_port);

  const int rc = http.listen(128);
  st.listen_rc.store(rc);
  port_promise.set_value(rc != 0 ? -1 : bound_port);
  if (rc != 0) return;

  uvcpp_loop* loop = http.get_tcp_server()->get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  uvcpp_test::pump_for(loop, 400);
}

struct scenario_server {
  server_state      st;
  std::atomic<bool> stop{false};
  std::promise<int> port_promise;
  std::future<int>  port_future;
  std::thread       thread;
  int               port = -1;

  explicit scenario_server(uvcpp_ssl_context* sctx)
      : port_future(port_promise.get_future()) {
    thread = std::thread(run_server, std::ref(port_promise), std::ref(stop),
                         std::ref(st), sctx);
    port = port_future.get();
  }

  void shutdown() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }

  ~scenario_server() { shutdown(); }

  bool ok() const { return port > 0 && st.listen_rc.load() == 0; }
};

// =========================================================================
// 客户端侧观测
// =========================================================================

struct resp_rec {
  int32_t            sid   = 0;   ///< 应答里的 `stream_id`
  int                status = 0;
  int                err    = 0;
  bool               retryable = false;
  std::string        body;
  uvcpp_http_version version = uvcpp_http_version::HVER_11;
};

/// `uvcpp_http_client` + 观测。
///
/// **成员次序是有意的**（析构逆序）：`client` 先声明 ⇒ 最后析构，本结构自己的
/// 互斥量与表在它之后才拆 —— 客户端的析构会泵循环，回调仍可能被叫到（h2 那条
/// 路上连接结束会跑一遍"在飞的流全部结算"）。
struct client_obs {
  uvcpp_http_client client;

  std::mutex                 mu;
  std::vector<std::string>   arrival;  ///< key 的**到达序**
  std::map<std::string, int> calls;    ///< 每个 key 的回调次数
  std::map<std::string, resp_rec> by_key;

  std::atomic<int> connect_status{-99};
  std::atomic<bool> connect_fired{false};

  uvcpp_loop* loop() { return client.get_tcp_client()->get_loop(); }

  int send_keyed(const std::string& key, const uvcpp_http_request& req) {
    return client.send(req, [this, key](const uvcpp_http_response& r, int err) {
      resp_rec rec;
      rec.sid       = r.stream_id;
      rec.status    = static_cast<int>(r.status_code);
      rec.err       = err;
      rec.retryable = r.retryable;
      rec.body      = r.body.to_string();
      rec.version   = r.version;
      std::lock_guard<std::mutex> lk(mu);
      arrival.push_back(key);
      calls[key] += 1;
      by_key[key] = rec;
    });
  }

  int call_count(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu);
    std::map<std::string, int>::iterator it = calls.find(key);
    return it == calls.end() ? 0 : it->second;
  }

  bool has(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu);
    return by_key.find(key) != by_key.end();
  }

  resp_rec at(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu);
    return by_key[key];
  }

  /// `key` 的应答在到达序里排第几位（-1 = 还没到）。
  int order_of(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu);
    for (size_t i = 0; i < arrival.size(); ++i) {
      if (arrival[i] == key) return static_cast<int>(i);
    }
    return -1;
  }

  /// 起连接。返回 0 表示**连接回调已经跑过且成功**。
  int connect(int port, uvcpp_ssl_context* cctx, const char* label) {
    client.set_ssl_context(cctx);
    const int rc = client.connect("127.0.0.1", port, [this](int status) {
      connect_status.store(status);
      connect_fired.store(true);
    });
    check(rc == 0, std::string(label) + ": connect() returned " +
                       std::to_string(rc));
    if (rc != 0) return rc;
    if (!uvcpp_test::wait_until(loop(), [this] { return connect_fired.load(); },
                                uvcpp_test::kWaitMs)) {
      return -1;
    }
    return connect_status.load();
  }

  bool wait_key(const std::string& key, int ms = uvcpp_test::kWaitMs) {
    return uvcpp_test::wait_until(loop(), [this, &key] { return has(key); },
                                  ms);
  }
};

// =========================================================================
// 场景
// =========================================================================

/// 场景 1：**默认关**。服务端支持 h2（名单里有 h2），客户端不宣告就不会协商到。
void scenario_default_off(int port, uvcpp_ssl_context* cctx,
                          server_state& st) {
  std::cout << "[scenario 1] 默认关：ALPN 里不能有 h2" << std::endl;

  client_obs c;
  check(c.client.http2_enabled() == false,
        "default_off: http2_enabled() 默认不是 false");

  if (c.connect(port, cctx, "default_off") != 0) {
    check(false, "default_off: connect 失败");
    return;
  }

  check(c.client.negotiated_alpn() == "http/1.1",
        "default_off: negotiated_alpn() = \"" + c.client.negotiated_alpn() +
            "\"，应为 \"http/1.1\"");

  // 客户端**功能正常**的正面证据：这条 h1 请求必须真跑通。只断言"ALPN 不是 h2"
  // 是不够的 —— 一个压根连不上的客户端也满足它。
  check(c.send_keyed("hello", uvcpp_http_request::make_get("/hello")) == 0,
        "default_off: send 失败");
  check(c.wait_key("hello"), "default_off: /hello 的响应没来");
  check(c.at("hello").status == 200,
        "default_off: status=" + std::to_string(c.at("hello").status));
  check(c.at("hello").body == kHelloBody,
        "default_off: body=\"" + c.at("hello").body + "\"");
  check(c.at("hello").version != uvcpp_http_version::HVER_20,
        "default_off: h1 的应答却带着 HVER_20");

  // 既有纪律的回归钉子：**异步 connect + `send_wait()`** 恒为 `UV_ENOTSUP`
  // （两种 I/O 模型不能混）。h2 那道新闸门加在同一个函数里，很容易顺手把这
  // 一条也改了 —— 所以它必须在关着 h2 的路上也钉一遍。
  uvcpp_http_response resp;
  const int wrc =
      c.client.send_wait(uvcpp_http_request::make_get("/hello"), resp,
                         uvcpp_test::kWaitMs);
  check(wrc == UV_ENOTSUP,
        "default_off: 异步 connect 之后 send_wait 返回 " + std::to_string(wrc) +
            "，应为 UV_ENOTSUP");
}

/// 场景 2：打开 h2 —— 协商、GET、版本、流身份。
void scenario_on(int port, uvcpp_ssl_context* cctx) {
  std::cout << "[scenario 2] 打开 h2：协商 + GET + 按流归位" << std::endl;

  client_obs c;
  c.client.set_http2_enabled(true);
  check(c.client.http2_enabled() == true,
        "h2_on: http2_enabled() 读完不是 true");

  if (c.connect(port, cctx, "h2_on") != 0) {
    check(false, "h2_on: connect 失败");
    return;
  }
  check(c.client.negotiated_alpn() == "h2",
        "h2_on: negotiated_alpn() = \"" + c.client.negotiated_alpn() +
            "\"，应为 \"h2\"");

  const int rc = c.send_keyed("hello", uvcpp_http_request::make_get("/hello"));
  check(rc == 0, "h2_on: send rc=" + std::to_string(rc));
  check(c.wait_key("hello"), "h2_on: /hello 的响应没来");

  const resp_rec r = c.at("hello");
  check(r.err == 0, "h2_on: err=" + std::to_string(r.err));
  check(r.status == 200, "h2_on: status=" + std::to_string(r.status));
  check(r.body == kHelloBody, "h2_on: body=\"" + r.body + "\"");
  // h2 的应答必须带上 h2 的版本 —— h1 那条路写的是解析器给的版本，
  // 这条要是漏了就是"用 HTTP/1.1 的身份回 h2 的流"。
  check(r.version == uvcpp_http_version::HVER_20,
        "h2_on: resp.version 不是 HVER_20");
  // `stream_id` 是调用方唯一的"这条回应的是哪次请求"的凭据；h2 下客户端发起的
  // 流 id 从 1 开始、恒为奇数。
  check(r.sid > 0 && (r.sid % 2) == 1,
        "h2_on: resp.stream_id=" + std::to_string(r.sid));

  // ---- POST 带 body：请求体必须真的过桥 -----------------------------
  uvcpp_http_request post = uvcpp_http_request::make_post(
      "/echo", kPostBody, sizeof(kPostBody) - 1, "text/plain");
  check(c.send_keyed("echo", post) == 0, "h2_on: POST send 失败");
  check(c.wait_key("echo"), "h2_on: /echo 的响应没来");
  check(c.at("echo").body == kPostBody,
        "h2_on: echo body=\"" + c.at("echo").body + "\"");
  check(c.at("echo").sid != r.sid,
        "h2_on: 第二条流的 id 与第一条相同（" + std::to_string(r.sid) + "）");

  // ---- 404 ---------------------------------------------------------
  check(c.send_keyed("404", uvcpp_http_request::make_get("/nope")) == 0,
        "h2_on: 404 send 失败");
  check(c.wait_key("404"), "h2_on: 404 的响应没来");
  check(c.at("404").status == 404,
        "h2_on: 404 status=" + std::to_string(c.at("404").status));
  check(c.at("404").body == kNotFoundBody,
        "h2_on: 404 body=\"" + c.at("404").body + "\"");

  // ---- 同步系列必须报错，而不是写明文 -------------------------------
  uvcpp_http_response dead;
  const int wrc = c.client.send_wait(uvcpp_http_request::make_get("/hello"),
                                     dead, uvcpp_test::kWaitMs);
  check(wrc == UV_ENOTSUP,
        "h2_on: send_wait 在 h2 连接上返回 " + std::to_string(wrc) +
            "，应为 UV_ENOTSUP(" + std::to_string(UV_ENOTSUP) + ")");

  // 一把闸门之后连接还得好用：上面那次拒绝不能把连接带坏。
  check(c.send_keyed("after", uvcpp_http_request::make_get("/hello")) == 0,
        "h2_on: 被拒之后 send 失败");
  check(c.wait_key("after"), "h2_on: 被拒之后 /hello 的响应没来");
  check(c.at("after").status == 200, "h2_on: 被拒之后的 status 不是 200");
}

/// 场景 3：**并发两条流**，应答序与派发序故意错开。
///
/// 这是本文件的核心判据。`/defer` 先派发但不作答，`/hello` 后派发立刻作答，
/// `/release` 才把 `/defer` 放出来。若应答表是单槽的，`/hello` 的提交会把
/// `/defer` 的槽位盖掉 —— 于是 `defer` 的回调**一次都不跑**，而不只是 body 串了。
void scenario_concurrent(int port, uvcpp_ssl_context* cctx, server_state& st) {
  std::cout << "[scenario 3] 并发两条流：派发序 ≠ 应答序" << std::endl;

  client_obs c;
  c.client.set_http2_enabled(true);
  if (c.connect(port, cctx, "concurrent") != 0) {
    check(false, "concurrent: connect 失败");
    return;
  }

  check(c.send_keyed("defer", uvcpp_http_request::make_get("/defer")) == 0,
        "concurrent: /defer send 失败");
  // 等**服务端真的挂起**了再发下一条：不然 /hello 可能先到，两个请求就退化
  // 成顺序的，这条用例也就什么都验不到了。
  check(uvcpp_test::wait_until(c.loop(), [&st] { return st.saw_defer.load(); },
                               uvcpp_test::kWaitMs),
        "concurrent: 服务端始终没看到 /defer");

  check(c.send_keyed("hello", uvcpp_http_request::make_get("/hello")) == 0,
        "concurrent: /hello send 失败");
  check(c.wait_key("hello"), "concurrent: /hello 的响应没来");

  // 此刻 /defer 还没被放行 —— 这就是"两条流同时在飞"的正面证据。
  check(!c.has("defer"), "concurrent: /defer 在 /release 之前就应答了");

  check(c.send_keyed("release", uvcpp_http_request::make_get("/release")) == 0,
        "concurrent: /release send 失败");
  check(c.wait_key("defer"), "concurrent: /defer 放行之后仍然没应答");
  check(c.wait_key("release"), "concurrent: /release 的响应没来");

  // 每条恰好一次 —— 单槽状态在这里的表现是被盖掉那条**零次**。
  check(c.call_count("defer") == 1,
        "concurrent: defer 回调了 " + std::to_string(c.call_count("defer")) +
            " 次，应为 1");
  check(c.call_count("hello") == 1,
        "concurrent: hello 回调了 " + std::to_string(c.call_count("hello")) +
            " 次，应为 1");

  check(c.at("defer").body == kDeferBody,
        "concurrent: defer body 长度 " + std::to_string(c.at("defer").body.size()) +
            "，应为 " + std::to_string(kDeferBody.size()));
  check(c.at("defer").status == 200,
        "concurrent: defer status=" + std::to_string(c.at("defer").status));
  check(c.at("hello").body == kHelloBody,
        "concurrent: hello body=\"" + c.at("hello").body + "\"");

  // 两条流各归各的 id，且都是发出去的顺序（h2 的客户端流号从 1 起、逐条 +2）。
  check(c.at("defer").sid > 0 && c.at("hello").sid > c.at("defer").sid,
        "concurrent: 流号 defer=" + std::to_string(c.at("defer").sid) +
            " hello=" + std::to_string(c.at("hello").sid) + "，hello 应更大");

  // 到达序：`/hello` 必须排在 `/defer` 前面。这是"两条流并行、各自归位"最直接
  // 的观测 —— 顺序回来就说明第二条根本没并行，而是被排队了。
  const int hi = c.order_of("hello");
  const int di = c.order_of("defer");
  check(hi >= 0 && di >= 0 && hi < di,
        "concurrent: 到达序 hello=" + std::to_string(hi) + " defer=" +
            std::to_string(di) + "，hello 应在前");
}

/// 场景 4：h2 连接上的"对端断开"**只能结算一次**。
///
/// h2 层用的是 `read_start_events`，那条路在 `nread < 0` 时先回调会话
/// （→ `on_h2_disconnect`，把在飞的流按 `UV_ECANCELED` 结算、并写下这个错误码），
/// **紧接着**同一个读回调里还会跑 `fire_close_callbacks()` —— 也就是本类的关闭
/// 观察者。观察者里那道 `if (!user_cb_ ...) return` 是唯一的拦阻：挡住它的正是
/// "h2 的 send 从不设 `user_cb_`"。少了那道门，第二次就会把错误码覆盖成 h1 那套
/// `UV_ECONNRESET`，`get_last_error()` 开始对 h2 的断开说谎。
///
/// 这个场景排在最后：它要把服务端收摊，而那正是断开的来源。
void scenario_drop_on_close(int port, uvcpp_ssl_context* cctx,
                            scenario_server& srv) {
  std::cout << "[scenario 4] h2 上断开只结算一次" << std::endl;

  client_obs c;
  c.client.set_http2_enabled(true);
  if (c.connect(port, cctx, "drop") != 0) {
    check(false, "drop: connect 失败");
    return;
  }
  // 前置：这条连接必须是 h2。落到 h1 就是另一个场景在做断言了。
  check(c.client.negotiated_alpn() == "h2",
        "drop: ALPN 协商结果是 \"" + c.client.negotiated_alpn() +
            "\"，应为 h2（前置）");

  check(c.send_keyed("hello", uvcpp_http_request::make_get("/hello")) == 0,
        "drop: /hello send 失败");
  check(c.wait_key("hello"), "drop: /hello 的响应没来（前置）");
  check(c.at("hello").err == 0, "drop: 断开之前那次响应是成功的（前置）");
  check(c.client.get_last_error() == 0, "drop: 断开之前 last_error 应为 0（前置）");
  const int calls_before = c.call_count("hello");

  srv.shutdown();

  check(uvcpp_test::wait_until(
            c.loop(), [&c] { return !c.client.has_status(HTTP_CLIENT_CONNECTED); },
            uvcpp_test::kWaitMs),
        "drop: 客户端在墙钟上限内观察到断开（前置）");
  uvcpp_test::pump_for(c.loop(), 50);

  check(c.call_count("hello") == calls_before,
        "drop: 断开让已经交付过的回调又跑了一次");
  check(c.client.get_last_error() == UV_ECANCELED,
        "drop: last_error = " + std::to_string(c.client.get_last_error()) +
            "，应为 UV_ECANCELED（被覆盖成 UV_ECONNRESET 就是报了第二遍）");
}

/// 场景 5：`uvcpp_http_client` 要能回答"对端道别了没有"。
///
/// 这一层存在的理由：GOAWAY 和断线在 `send()` 的回调上长得一模一样（都是
/// `UV_ECANCELED`），含义却完全相反 —— 前者对端明说了处理到哪个流号，比它大的
/// 请求**没被处理**、重试安全；后者是"结果未知"。少了这个可查询的位，调用方
/// 只能一律当断线处理。
///
/// 判据里有三条是**互相咬合**的：GOAWAY 里的 `last_stream_id` 必须恰好是 `/bye`
/// 那条流（证明填的是"已处理的最大流号"而不是随便一个数），`/bye` 自己的响应
/// 必须照常到达（证明 GOAWAY 关的是新流、不是连接），而连接必须**还开着**
/// （证明 `begin_h2_goaway()` 只管道别、不管关 —— 关是停机后面的拍子）。
void scenario_peer_goaway(int port, uvcpp_ssl_context* cctx, server_state& st) {
  std::cout << "[scenario 5] 对端 GOAWAY 可查询，且不是断开" << std::endl;

  client_obs c;
  c.client.set_http2_enabled(true);
  if (c.connect(port, cctx, "bye") != 0) {
    check(false, "bye: connect 失败");
    return;
  }
  check(c.client.negotiated_alpn() == "h2",
        "bye: ALPN 协商结果是 \"" + c.client.negotiated_alpn() +
            "\"，应为 h2（前置）");

  // 前置：先跑完一条正常的流，`last_stream_id` 才有基准，"还没道别"也才有
  // 观察点 —— 一个恒真的 `peer_goaway_received()` 在这个场景里是看不出来的。
  check(c.client.peer_goaway_received() == false,
        "bye[前置]: 还没发 /bye 就报对端道别了");
  check(c.send_keyed("hello", uvcpp_http_request::make_get("/hello")) == 0,
        "bye: /hello send 失败");
  check(c.wait_key("hello"), "bye[前置]: /hello 的响应没来");
  check(c.at("hello").err == 0, "bye[前置]: /hello 出错了");

  check(c.send_keyed("bye", uvcpp_http_request::make_get("/bye")) == 0,
        "bye: /bye send 失败");
  check(c.wait_key("bye"), "bye: /bye 的响应没来（GOAWAY 不该掐掉已有的流）");
  check(c.at("bye").err == 0,
        "bye: /bye 的回调 err = " + std::to_string(c.at("bye").err));
  uvcpp_test::pump_for(c.loop(), 30);

  check(st.goaway_count.load() == 1,
        "bye[前置]: 服务端 /bye 里 begin_h2_goaway() 返回 " +
            std::to_string(st.goaway_count.load()) + "，应为 1");

  check(c.client.peer_goaway_received(),
        "bye: 服务端发了 GOAWAY，客户端却不知道 —— 对端只能按断线处理");
  check(c.client.peer_goaway_error_code() == 0,
        "bye: GOAWAY 错误码 = " +
            std::to_string(c.client.peer_goaway_error_code()) +
            "，正常道别该是 0");
  check(c.client.peer_goaway_last_stream_id() == c.at("bye").sid,
        "bye: GOAWAY 的 last_stream_id = " +
            std::to_string(c.client.peer_goaway_last_stream_id()) +
            "，而 /bye 那条流是 " + std::to_string(c.at("bye").sid));

  // GOAWAY 之后不能再开新流 —— 这一条把"收到了"和"进到会话状态里了"分开。
  const int rc = c.client.send(uvcpp_http_request::make_get("/hello"),
                               [](const uvcpp_http_response&, int) {});
  check(rc == UV_ENOTCONN,
        "bye: GOAWAY 之后 send() 返回 " + std::to_string(rc) +
            "，应为 UV_ENOTCONN");

  // 但连接本身还在：道别 ≠ 关连接。
  check(c.client.has_status(HTTP_CLIENT_CONNECTED),
        "bye: GOAWAY 之后连接就没了 —— 道别被当成关闭了");
  check(c.client.peer_goaway_received(),
        "bye: 上面那次 send 把道别状态弄丢了");
}

/// 场景 6：**在它自己的回调里** `delete` 客户端（h2 这条路上）。
///
/// h1 那条同形状的用例在 `web_http_client_selfdestroy_func.cpp`，那条是真崩过的
/// （析构里泵的那几轮会回头再叫一次读路径，cdb 的栈是
/// `on_tcp_data → parser::execute → llhttp`）。这里补的是 h2 的那一半：回调是从
/// `uvcpp_h2_session::drain` 里**同步**跑出来的，上面还压着 nghttp2 的帧循环与
/// h2 连接的读写回调 —— 析构那条"一个都不拆"的路要把它们一路放回去。
void scenario_selfdestroy(int port, uvcpp_ssl_context* cctx) {
  std::cout << "[scenario 6] 在 h2 回调里 delete 客户端" << std::endl;

  uvcpp_http_client* pc = new uvcpp_http_client();
  pc->set_http2_enabled(true);
  pc->set_ssl_context(cctx);
  uvcpp_loop* loop = pc->get_tcp_client()->get_loop();

  std::atomic<bool> connected{false};
  const int crc = pc->connect("127.0.0.1", port, [&connected](int status) {
    if (status == 0) connected.store(true);
  });
  check(crc == 0, "selfdestroy_h2: connect() 返回 " + std::to_string(crc));
  if (crc != 0) {
    delete pc;
    return;
  }
  if (!uvcpp_test::wait_until(loop, [&] { return connected.load(); },
                              uvcpp_test::kWaitMs)) {
    check(false, "selfdestroy_h2: connect 没在墙钟上限内成功");
    delete pc;
    return;
  }
  // **前置**：这条必须是 h2。协商退回 h1 的话，这条用例测的是另一条路，
  // 而它会照常"通过"。
  check(pc->negotiated_alpn() == "h2",
        "selfdestroy_h2[前置]: ALPN = \"" + pc->negotiated_alpn() +
            "\"，应为 h2");

  std::atomic<bool> fired{false};
  std::atomic<bool> deleted{false};
  std::atomic<int>  err{-99};
  std::atomic<int>  code{0};
  std::atomic<int>  sid{0};

  const int src = pc->send(
      uvcpp_http_request::make_get("/hello"),
      [pc, &fired, &deleted, &err, &code, &sid](
          const uvcpp_http_response& r, int e) {
        err.store(e);
        code.store(static_cast<int>(r.status_code));
        sid.store(r.stream_id);
        delete pc;
        deleted.store(true);
        fired.store(true);
      });
  check(src == 0, "selfdestroy_h2: send() 返回 " + std::to_string(src));
  if (src != 0) {
    delete pc;
    return;
  }

  check(uvcpp_test::wait_until(loop, [&] { return fired.load(); },
                               uvcpp_test::kWaitMs),
        "selfdestroy_h2: 响应回调没在墙钟上限内落地");
  check(deleted.load(),
        "selfdestroy_h2[前置]: 回调执行到了 delete 之后的那一句");
  check(err.load() == 0,
        "selfdestroy_h2: err = " + std::to_string(err.load()));
  check(code.load() == 200,
        "selfdestroy_h2: status = " + std::to_string(code.load()));
  // 上面那次 `delete` 的栈是**在 `loop->run()` 里面**退的，这里再泵几轮，
  // 让 nghttp2 的帧循环与 h2 连接的读写回调也走完各自的收尾。
  uvcpp_test::pump_for(loop, 50);
  check(sid.load() > 0 && (sid.load() % 2) == 1,
        "selfdestroy_h2: resp.stream_id = " + std::to_string(sid.load()));
}

// =========================================================================
// 裸 h2 对端（场景 7）
// =========================================================================
//
// 场景 7 要的是"对端用**指定错误码**关掉一条流"，而 `uvcpp_http_server` 造不出
// 这个：它的 RST 只从会话层自己的校验里出来（错误码写死在 `reject()` 里），没有
// 对外入口。所以这里用裸 `uvcpp_tcp_server` + `uvcpp_h2_connection(server_side=
// true)` 拼一个对端 —— 与 `web_ssl_h2_server_func.cpp` 拼裸客户端是同一个手法，
// 方向相反。

struct raw_peer_state {
  std::atomic<int> listen_rc{-1};
  std::atomic<int> accepted{0};
  std::atomic<int> alpn_h2{0};
  std::atomic<int> requests{0};
  std::atomic<int> rsts_sent{0};

  std::mutex               mu;
  std::vector<std::string> paths;  ///< 收到的 `:path`，按到达序
};

/// 剧本：路径 → 用哪个错误码把这条流 RST 掉。`0xFFFF` = 正常应答。
uint32_t rst_code_for(const std::string& path) {
  if (path == "/refuse") return 7;   // REFUSED_STREAM
  if (path == "/cancel") return 8;   // CANCEL
  if (path == "/noerr")  return 0;   // NO_ERROR
  if (path == "/proto")  return 1;   // PROTOCOL_ERROR
  return 0xFFFFu;
}

void run_raw_peer(std::promise<int>& port_promise, std::atomic<bool>& stop,
                  raw_peer_state& st, uvcpp_ssl_context* sctx) {
  uvcpp_tcp_server srv;
  srv.set_ssl_context(sctx);  // 必须在 listen() 之前

  // 活着的 h2 连接。只在本线程上碰（accept 回调、on_disconnect、收摊）。
  std::vector<uvcpp_h2_connection*> conns;

  if (srv.bindIpv4("127.0.0.1", 0) != 0) {
    port_promise.set_value(-1);
    return;
  }
  sockaddr_in name;
  int namelen = sizeof(name);
  srv.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int bound_port = ntohs(name.sin_port);

  const int lrc = srv.listen([&st, &conns](uvcpp_tcp_client* client) {
    if (client == nullptr) return;
    if (client->is_tls() && client->tls_alpn_selected() == "h2") {
      st.alpn_h2.fetch_add(1);
    }
    st.accepted.fetch_add(1);

    uvcpp_h2_connection* conn =
        new uvcpp_h2_connection(client, /*server_side=*/true);
    conns.push_back(conn);

    uvcpp_h2_session::callbacks h2c;
    h2c.on_request = [&st, conn](uvcpp_h2_session& s, uvcpp_h2_stream& str,
                                 bool) {
      st.requests.fetch_add(1);
      {
        std::lock_guard<std::mutex> lk(st.mu);
        st.paths.push_back(str.request.url);
      }
      const uint32_t code = rst_code_for(str.request.url);
      if (code == 0xFFFFu) {
        uvcpp_http_response r = uvcpp_http_response::ok(
            kRawBody, sizeof(kRawBody) - 1, "text/plain");
        conn->send_response(str.stream_id, r);
        return;
      }
      if (s.submit_rst(str.stream_id, code) == 0) st.rsts_sent.fetch_add(1);
      // 在 `recv()` 的栈上冲字节，与 `uvcpp_http_server` 的处理函数同一形状
      // （`send_response` 内部也是 submit + flush）。这一冲必须被推到
      // `mem_recv` 之外 —— 见 `uvcpp_h2_connection::flush()` 里那道守卫。
      conn->flush();
    };
    // 这个对端没有连接级剧本：致命错误记一笔就走，断开路径交给客户端那边。
    h2c.on_fatal = [](uvcpp_h2_session&, int) {};

    uvcpp_h2_connection::callbacks cc;
    cc.on_disconnect = [&conns, conn](uvcpp_h2_connection&) {
      // 契约（`uvcpp_h2_connection.h`）：持有者在这个回调里销毁本对象。
      for (size_t i = 0; i < conns.size(); ++i) {
        if (conns[i] == conn) {
          conns.erase(conns.begin() + static_cast<long>(i));
          break;
        }
      }
      delete conn;
    };

    if (conn->start(h2c, cc) != 0) {
      // `start()` 失败时 `on_disconnect` 不会来（会话根本没起来），自己收。
      for (size_t i = 0; i < conns.size(); ++i) {
        if (conns[i] == conn) {
          conns.erase(conns.begin() + static_cast<long>(i));
          break;
        }
      }
      delete conn;
    }
  });

  st.listen_rc.store(lrc);
  port_promise.set_value(lrc != 0 ? -1 : bound_port);
  if (lrc != 0) return;

  uvcpp_loop* loop = srv.get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  uvcpp_test::pump_for(loop, 200);

  // 收摊：先让服务端关掉手上的连接 —— 每条连接的 `on_disconnect` 会把它那份
  // h2 层删掉（那是**唯一**保证"删在 client 被释放之前"的时点）。泵完之后如果
  // 还有剩的（断开回调没走到），客户端已经关完并被框架释放了，这时删 h2 层
  // 只剩"撤掉那个 nghttp2 会话"，安全。
  srv.close_all_clients();
  uvcpp_test::pump_for(loop, 200);
  for (size_t i = 0; i < conns.size(); ++i) delete conns[i];
  conns.clear();
}

struct raw_peer {
  raw_peer_state    st;
  std::atomic<bool> stop{false};
  std::promise<int> port_promise;
  std::future<int>  port_future;
  std::thread       thread;
  int               port = -1;

  explicit raw_peer(uvcpp_ssl_context* sctx)
      : port_future(port_promise.get_future()) {
    thread = std::thread(run_raw_peer, std::ref(port_promise), std::ref(stop),
                         std::ref(st), sctx);
    port = port_future.get();
  }

  void shutdown() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }

  ~raw_peer() { shutdown(); }

  bool ok() const { return port > 0 && st.listen_rc.load() == 0; }
};

/// `uvcpp_http_response` 的拷贝构造与赋值必须带上两个**身份字段**
/// （`stream_id` 与 `retryable`）。
///
/// 这两条是手写的（`UVCPP_DEFINE_COPY_FUNC` 只声明），逐字段列一遍 —— 加字段
/// 时漏掉一处不会有任何编译期提示。在回调里存一份副本是很常见的写法，丢了
/// `retryable` 就等于这一批的意义全没了，所以这里钉住。
void scenario_response_copy() {
  std::cout << "[scenario 8] 响应的拷贝构造 / 赋值" << std::endl;

  uvcpp_http_response orig;
  orig.stream_id = 5;
  orig.retryable = true;
  orig.status_code = http_status::NOT_FOUND;

  uvcpp_http_response cpy(orig);
  check(cpy.stream_id == 5, "copy: 拷贝构造丢了 stream_id");
  check(cpy.retryable, "copy: 拷贝构造丢了 retryable");
  check(cpy.status_code == http_status::NOT_FOUND,
        "copy: 拷贝构造丢了 status_code");

  uvcpp_http_response asg;
  asg = orig;
  check(asg.stream_id == 5, "copy: 赋值丢了 stream_id");
  check(asg.retryable, "copy: 赋值丢了 retryable");
}

/// 场景 7：对端**按指定错误码**关掉一条流时，本层报什么。
///
/// `REFUSED_STREAM` 在 RFC 9113 §8.7 里的定义就是"这条流在被处理之前就被关掉
/// 了 ⇒ 那条请求**重发是安全的**"；`CANCEL` 是"对端不要这条流了"，**不保证**
/// 没处理过；其它码是协议层面的失败。三者必须是三个可分辨的结果。
///
/// 串行发、每条都等它落地再发下一条：这样"每条流恰好回调一次"和到达序都是
/// 确定的，不掺并发。中间的 `/hello` 是**连接仍然健康**的判据 —— RST 是流级的，
/// 一条流被拒不许把连接或后面的流带下水。
void scenario_peer_rst(int port, uvcpp_ssl_context* cctx, raw_peer_state& st) {
  std::cout << "[scenario 7] 对端 RST_STREAM 的错误码语义" << std::endl;

  client_obs c;
  c.client.set_http2_enabled(true);
  if (c.connect(port, cctx, "rst") != 0) {
    check(false, "rst: connect 失败");
    return;
  }
  check(c.client.negotiated_alpn() == "h2",
        "rst[前置]: ALPN = \"" + c.client.negotiated_alpn() + "\"，应为 h2");

  struct step {
    const char* key;
    const char* path;
    int         want_err;
    bool        want_retryable;
    int         want_status;  ///< -1 = 不检查（被 RST 掉的流没有状态码）
  };
  const step kSteps[] = {
      {"ok0",    "/hello",  0,            false, 200},
      {"refuse", "/refuse", UV_ECANCELED, true,  -1},
      {"ok1",    "/hello",  0,            false, 200},
      {"cancel", "/cancel", UV_ECANCELED, false, -1},
      {"noerr",  "/noerr",  UV_ECANCELED, false, -1},
      {"proto",  "/proto",  UV_EPROTO,    false, -1},
      {"ok2",    "/hello",  0,            false, 200},
  };

  int32_t last_sid = 0;
  for (size_t i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); ++i) {
    const step&       s   = kSteps[i];
    const std::string key = s.key;
    check(c.send_keyed(key, uvcpp_http_request::make_get(s.path)) == 0,
          key + ": send 返回非 0");
    if (!c.wait_key(key)) {
      check(false, key + ": 回调没在墙钟上限内落地");
      return;
    }
    const resp_rec r = c.at(key);
    check(r.err == s.want_err,
          key + ": err = " + std::to_string(r.err) + "，应为 " +
              std::to_string(s.want_err));
    check(r.retryable == s.want_retryable,
          key + ": retryable = " + (r.retryable ? "true" : "false") +
              "，应为 " + (s.want_retryable ? "true" : "false"));
    if (s.want_status >= 0) {
      check(r.status == s.want_status,
            key + ": status = " + std::to_string(r.status) + "，应为 " +
                std::to_string(s.want_status));
    }
    check(c.call_count(key) == 1,
          key + ": 回调 " + std::to_string(c.call_count(key)) + " 次，应为 1");
    check(r.sid > last_sid && (r.sid % 2) == 1,
          key + ": stream_id = " + std::to_string(r.sid) + "（上一条是 " +
              std::to_string(last_sid) + "），客户端流号应为递增的奇数");
    last_sid = r.sid;
  }

  check(c.client.has_status(HTTP_CLIENT_CONNECTED),
        "rst: 一串流级 RST 之后连接就没了 —— 流级错误被当成连接级处理了");

  // 对端侧的正面证据：这七条请求到过对端，其中四条是被 RST 掉的。**等到**而不是
  // 采一下 —— 计数长在对端线程上，与客户端的回调完成之间没有同步关系。
  check(uvcpp_test::wait_flag([&st] { return st.requests.load() == 7; },
                              uvcpp_test::kWaitMs),
        "rst[前置]: 对端只收到 " + std::to_string(st.requests.load()) +
            " 条请求，应为 7");
  check(uvcpp_test::wait_flag([&st] { return st.rsts_sent.load() == 4; },
                              uvcpp_test::kWaitMs),
        "rst[前置]: 对端只发出 " + std::to_string(st.rsts_sent.load()) +
            " 个 RST，应为 4");
  check(st.alpn_h2.load() >= 1,
        "rst[前置]: 对端侧 ALPN 不是 h2 —— 这条不是 h2 连接，测的是别的路");
}

}  // namespace

int main() {
  std::cout << "[web_ssl_h2_client] OpenSSL "
            << OpenSSL_version(OPENSSL_VERSION_STRING) << std::endl;

  uvcpp_ssl_context sctx(tls_mode::SERVER, tls_version::TLS_1_2);
  if (!sctx.is_ready() || !sctx.generate_self_signed("loopback.test", 2048)) {
    std::cerr << "  [FAIL] server ctx/cert: " << sctx.get_last_error()
              << std::endl;
    return 2;
  }
  sctx.set_alpn_select_protos(kServerAlpn);

  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  // 自签证书的回环用例需要它；生产客户端不能这样。
  cctx.set_verify_mode(tls_verify_mode::NONE);
  if (!cctx.is_ready()) {
    std::cerr << "  [FAIL] client ctx not ready" << std::endl;
    return 2;
  }

  {
    scenario_server srv(&sctx);
    if (!srv.ok()) {
      std::cerr << "  [FAIL] server failed to bind/listen" << std::endl;
      return 2;
    }

    scenario_default_off(srv.port, &cctx, srv.st);
    scenario_on(srv.port, &cctx);
    scenario_concurrent(srv.port, &cctx, srv.st);
    scenario_peer_goaway(srv.port, &cctx, srv.st);
    scenario_selfdestroy(srv.port, &cctx);

    // 服务端侧的正面证据：确实有一条 ALPN=h2 的连接被交付过。（场景 1 那条
    // 是 h1，所以这里不能断言等于用例数。）
    check(srv.st.alpn_h2_at_delivery.load() >= 2,
          "server: ALPN=h2 交付的连接数 = " +
              std::to_string(srv.st.alpn_h2_at_delivery.load()) + "，应 ≥ 2");
    check(srv.st.connections.load() >= 3,
          "server: 连接数 = " + std::to_string(srv.st.connections.load()));

    // 放最后：它会 `srv.shutdown()`，上面那两条服务端汇总要在收摊前读完。
    scenario_drop_on_close(srv.port, &cctx, srv);
  }

  scenario_response_copy();

  // 场景 7 用的是另一个对端（裸 h2，能按剧本发 RST），所以单开一段。
  {
    raw_peer peer(&sctx);
    if (!peer.ok()) {
      std::cerr << "  [FAIL] raw peer failed to bind/listen" << std::endl;
      return 2;
    }
    scenario_peer_rst(peer.port, &cctx, peer.st);
  }

  if (g_failures == 0) {
    std::cout << "[web_ssl_h2_client] ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << "[web_ssl_h2_client] " << g_failures << " FAILURE(S)"
            << std::endl;
  return 1;
}

#else
int main() {
  std::cout << "[web_ssl_h2_client] skipped (needs UVCPP_WEB_ENABLE && "
               "UVCPP_OPENSSL_ENABLE && UVCPP_NGHTTP2_ENABLE)"
            << std::endl;
  return 0;
}
#endif
