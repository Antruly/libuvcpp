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
      rec.sid     = r.stream_id;
      rec.status  = static_cast<int>(r.status_code);
      rec.err     = err;
      rec.body    = r.body.to_string();
      rec.version = r.version;
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
