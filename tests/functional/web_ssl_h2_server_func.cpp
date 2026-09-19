/**
 * @file tests/functional/web_ssl_h2_server_func.cpp
 * @brief HTTP/2 **服务端**：真 TLS + 真 ALPN + 真 socket + 真路由表。
 *
 * 前两个文件各测了一半：`h2_session_func` 把两条会话面对面摆好、靠 `drain()`
 * 手工搬字节（判据落在**转换层**上，不掺 socket），`web_ssl_alpn_func` 测
 * ALPN 协商但**故意**不把 h2 连接交给服务器路由（那时还没有分流器，让一条
 * 已协商 h2 的连接去跑 HTTP/1.1 报文只是当下成立的事实）。于是"分流之后到底
 * 通不通"这一段没人碰过：`uvcpp_http_server::on_tcp_connection` 判完 ALPN 走进
 * 去的那条路，一行覆盖都没有。
 *
 * 判据**刻意分成两处，缺一不可**：
 *   - 服务端处理函数看到了什么（方法/路径/body/content-length/**stream_id**）；
 *   - 客户端在哪条流上收到了什么。
 * 只看客户端是把不住关的：会话层按 id 查表，响应体正确完全不代表它回在了对的
 * 那条流上。所以每条用例都把 stream_id 对号入座，而不是只等"收到一个 200"
 * —— 后者在"响应落在别的流上"时表现为超时，而超时的用例加一句
 * `if (timeout) pass` 就绿了。
 *
 * 客户端侧用**同一条连接**顺序发五个请求（GET / POST / HEAD / 404 / 再来一次
 * GET）：h2 的连接是长命的，stream_id 必须从 1 开始单调递增且都是奇数。第二个
 * 请求能在同一条 TCP 上跑通才是"流簿记做对了"的证据 —— 服务端的
 * `on_connection` 钩子恰好被叫过一次，就是这条 TCP 连接被**复用**的直接观测。
 *
 * 场景 6 判的是"关掉时不能装作能用"：服务端一次都不调 `set_http2_enabled`
 * （钉**默认 false**），客户端照样宣告 h2。这条用例的结论是"什么都没发生"，
 * 而"什么都没发生"与"客户端压根没连上"长得一模一样，所以它必须附上一串
 * **正面证据**（客户端确实写出了字节、服务端确实受理了一条 ALPN=h2 的连接、
 * 会话层确实以错误或断开收场）才不是废话。
 *
 * 场景 7 单独开一条连接，钉的是**按流**而不是按连接记状态：`/defer` 挂起、
 * `/release` 放行，把"派发序"和"应答序"确定性地掰开，中间夹一条会改连接级
 * 判定的流。见该场景自己的说明。
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
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

#include <http2/uvcpp_h2_connection.h>

#include <openssl/ssl.h>

#include "loop_drain.h"
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

/// 单个事件的上限，**墙钟毫秒**（见 `wait_util.h` 的说明）。
const int kWaitMs = 5000;

/// 服务端宣告的名单。h2 排第一是 `SSL_select_next_proto` 的既成事实（没交集时
/// 它会写这一项），这里与 `web_ssl_alpn_func` 保持同一份，免得两个文件里
/// 的"服务端行为"出现只在某一个文件里成立的差别。
const std::vector<std::string> kServerAlpn{"h2", "http/1.1"};

const char kHelloBody[] = "hello-over-h2";
const char kPostBody[]  = "echo-me-please";

const char kNotFoundBody[] = "404 Not Found";

/// 场景 7 延迟应答用的 body。**长度必须 ≥ `compress_min_body_`（默认 1024）**，
/// 否则 gzip 那条判据在"该压"和"不该压"两侧都不压缩，断言恒真。
std::string make_defer_body() {
  std::string s;
  while (s.size() < 2048) s += "deferred-response-payload-";
  s.resize(2048);
  return s;
}
const std::string kDeferBody = make_defer_body();

// =========================================================================
// 服务端侧观测
// =========================================================================

/// 一条被**路由到**的请求，处理函数当场记下来的东西。
///
/// 半个判据在这上面：客户端的响应对不对，证明不了服务端看懂了多少。stream_id
/// 没传进来、请求体被丢、`content-length` 被剥掉 —— 这些在响应上都可能"恰好"
/// 看不出来（响应体是处理函数自己拼的）。
struct seen_request {
  http_method        method = http_method::HTTP_GET;
  std::string        url;
  std::string        body;
  std::string        content_length;
  int32_t            stream_id = 0;
  uvcpp_http_version version   = uvcpp_http_version::HVER_11;
};

struct server_state {
  std::atomic<int> listen_rc{-1};
  /// `on_connection` 钩子跑了几次 —— **这条 TCP 连接被复用的直接观测**。
  std::atomic<int> connections{0};
  std::atomic<int> closes{0};
  /// 交付时 ALPN 已经是 h2 的连接数。
  std::atomic<int> alpn_h2_at_delivery{0};
  /// 服务端线程读到的 `http2_enabled()`（-1 = 还没读）。
  std::atomic<int> h2_flag{-1};
  std::atomic<int> final_count{-1};
  /// `uvcpp_tcp_server::get_last_error()` 的收尾快照（0 = 无错）。
  std::atomic<int> final_last_error{0};

  std::mutex                mu;
  std::vector<seen_request> seen;

  /// 场景 7：挂起的延迟应答。`/release` 到了才发 —— 这样"按到达序派发、
  /// 按发送序应答"这个错位是**确定性**造出来的，不靠抢时序。
  std::mutex                                        pend_mu;
  std::vector<std::pair<uvcpp_tcp_client*, int32_t>> pending;
};

void record_request(server_state& st, uvcpp_http_request& req,
                    http_method method) {
  seen_request r;
  r.method         = method;
  r.url            = req.url;
  r.body           = req.body.to_string();
  r.content_length = req.get_header("content-length");
  r.stream_id      = req.stream_id;
  r.version        = req.version;
  std::lock_guard<std::mutex> lk(st.mu);
  st.seen.push_back(r);
}

std::vector<seen_request> snapshot_seen(server_state& st) {
  std::lock_guard<std::mutex> lk(st.mu);
  return st.seen;
}

// =========================================================================
// 服务端线程
// =========================================================================

/// `enable_h2` 为假时**不调** `set_http2_enabled` —— 场景 6 钉的就是文档写的
/// 那个默认值（默认 false），而不是"显式关掉"这个更弱的事实。
void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                server_state& st, uvcpp_ssl_context* sctx, bool enable_h2) {
  uvcpp_http_server http;

  if (enable_h2) http.set_http2_enabled(true);
  st.h2_flag.store(http.http2_enabled() ? 1 : 0);

  http.on_connection([&st](uvcpp_tcp_client* client) {
    if (client != nullptr && client->is_tls() &&
        client->tls_alpn_selected() == "h2") {
      st.alpn_h2_at_delivery.fetch_add(1);
    }
    st.connections.fetch_add(1);
  });
  http.on_connection_close([&st](uvcpp_tcp_client*) { st.closes.fetch_add(1); });

  http.get("/hello", [&st](uvcpp_http_request& req, uvcpp_http_response& resp,
                           uvcpp_tcp_client*) {
    record_request(st, req, http_method::HTTP_GET);
    resp = uvcpp_http_response::ok(kHelloBody, sizeof(kHelloBody) - 1,
                                   "text/plain");
    // 整对象替换会把 `stream_id` 冲成 0（响应头里写着这条），延迟应答的处理
    // 函数不设回去就等于"拿 HTTP/1.1 的身份回 h2 的流"。
    resp.stream_id = req.stream_id;
  });

  // HEAD 与 GET 各注册一条：本服务器的路由是 method + path 精确匹配，不给
  // HEAD 单独注册的话它落到兜底分支变成 404，"HEAD 的 content-length 等于
  // GET 的长度"就无从谈起了。
  http.head("/hello", [&st](uvcpp_http_request& req, uvcpp_http_response& resp,
                            uvcpp_tcp_client*) {
    record_request(st, req, http_method::HTTP_HEAD);
    resp = uvcpp_http_response::ok(kHelloBody, sizeof(kHelloBody) - 1,
                                   "text/plain");
    resp.stream_id = req.stream_id;
  });

  http.post("/echo", [&st](uvcpp_http_request& req, uvcpp_http_response& resp,
                           uvcpp_tcp_client*) {
    record_request(st, req, http_method::HTTP_POST);
    // 这条**故意不还原** `stream_id`：inline 路径用的是 `dispatch_h2_request`
    // 显式传下来的那个 id，不是响应对象里的。实现若改成读 `resp.stream_id`，
    // 这条路由的响应就会落到流 0 上，客户端当场拿到连接级错误。
    resp = uvcpp_http_response::ok(req.body.get_const_data(), req.body.size(),
                                   "text/plain");
  });

  // ---- 场景 7 的两条路由：延迟挂起 + 外部放行 ------------------------
  //
  // `/defer` 把 (client, stream_id) 记下来就 `deferred = true` 返回；`/release`
  // 到了才用 `http.send_response()` 把它发出去。于是"派发顺序"（arrival）与
  // "应答顺序"（send）被**确定性地**掰开——不需要靠两个请求抢时序。
  //
  // 延迟应答走的是 `send_response(uvcpp_tcp_client*, resp, ...)`，它手上只有
  // `resp.stream_id` 这一个流的身份（见该函数里 h2 那一段），所以下面必须把
  // `r.stream_id` 设回去。
  auto defer_handler = [&st](uvcpp_http_request& req, uvcpp_http_response& resp,
                             uvcpp_tcp_client* client) {
    record_request(st, req, req.method);
    resp.deferred = true;
    std::lock_guard<std::mutex> lk(st.pend_mu);
    st.pending.push_back(std::make_pair(client, req.stream_id));
  };
  http.get("/defer", defer_handler);
  http.head("/defer", defer_handler);

  auto release_handler = [&http, &st](uvcpp_http_request&, uvcpp_http_response& resp,
                                      uvcpp_tcp_client*) {
    std::vector<std::pair<uvcpp_tcp_client*, int32_t>> pend;
    {
      std::lock_guard<std::mutex> lk(st.pend_mu);
      pend.swap(st.pending);
    }
    for (size_t i = 0; i < pend.size(); ++i) {
      uvcpp_http_response r =
          uvcpp_http_response::ok(kDeferBody.data(), kDeferBody.size(),
                                  "text/plain");
      r.stream_id = pend[i].second;
      http.send_response(pend[i].first, r, /*close_after_write=*/false);
    }
    // 这条自己的响应：inline 路径由 `dispatch_h2_request` 显式传 id，
    // **不看** `resp.stream_id`（见 `/echo` 路由上的说明）。
    resp = uvcpp_http_response::ok("released", 8, "text/plain");
  };
  http.get("/release", release_handler);
  // HEAD **必须**也注册：场景 7 子用例 A 就是靠"放行那条流自己是 HEAD"把
  // 连接级 `is_head` 留成 true 的（理由见那里的说明）。
  http.head("/release", release_handler);

  // 必须在 listen() 之前装：accept 时读不到就是这个原因
  // （见 `web_ssl_server_func.cpp` 场景 2 的说明）。
  http.get_tcp_server()->set_ssl_context(sctx);

  if (http.bindIpv4("127.0.0.1", 0) != 0) {
    port_promise.set_value(-1);
    return;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  http.get_tcp_server()->get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name),
                                                &namelen);
  const int bound_port = ntohs(name.sin_port);

  const int rc = http.listen(128);
  st.listen_rc.store(rc);
  // 端口必须在 listen() **之后**才放行：已 bind、尚未 listen 的 socket 是拒连的
  // （RST → ECONNREFUSED），窗口很小但真实存在。
  port_promise.set_value(rc != 0 ? -1 : bound_port);
  if (rc != 0) return;

  uvcpp_loop* loop = http.get_tcp_server()->get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  uvcpp_test::pump_for(loop, 400);
  st.final_count.store(static_cast<int>(http.get_tcp_server()->client_count()));
  // 收尾之后读：握手失败时 `on_tls_handshake_done` 把错误码记在这里，
  // 它是"连接为什么没交付"的唯一线索（见本文件里那条断言）。
  st.final_last_error.store(http.get_tcp_server()->get_last_error());
}

struct scenario_server {
  server_state      st;
  std::atomic<bool> stop{false};
  std::promise<int> port_promise;
  std::future<int>  port_future;
  std::thread       thread;
  int               port = -1;

  scenario_server(uvcpp_ssl_context* sctx, bool enable_h2)
      : port_future(port_promise.get_future()) {
    thread = std::thread(run_server, std::ref(port_promise), std::ref(stop),
                         std::ref(st), sctx, enable_h2);
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

/// 客户端收到的一条完整响应。`sid` 是判据的核心 —— 见文件头的说明。
struct seen_response {
  int32_t     sid                   = 0;
  int         status                = 0;
  bool        end_stream_at_headers = false;  ///< HEADERS 自带 END_STREAM
  bool        end                   = false;  ///< 整条响应收完
  std::string body;
  std::string content_type;
  std::string content_length;
  std::string content_encoding;  ///< 场景 7 的 gzip 判据落在这上面
};

struct client_probe {
  std::atomic<int>    connect_status{-99};
  std::atomic<bool>   connect_fired{false};
  std::atomic<bool>   hs_done_at_connect{false};
  std::atomic<int>    start_rc{-999};
  std::atomic<int>    fatal_count{0};
  std::atomic<int>    disconnect_count{0};
  std::atomic<size_t> bytes_out{0};

  std::mutex                     mu;
  std::string                    client_alpn;
  std::vector<int32_t>           submitted;  ///< `submit_request` 返回的 id，按序
  std::vector<seen_response>     responses;  ///< 收全的响应，按到达序
  std::map<int32_t, std::string> partial;    ///< 正在收的 body，键是流 id
};

/// 一条 h2 客户端连接。
///
/// **成员的声明次序是有意的**（析构逆序）：
///   - `p` 先声明 ⇒ 最后析构：回调可能在客户端析构的过程中还被叫到
///     （`~uvcpp_tcp_client` 会走关闭收尾），那会儿它碰的正是 `p`；
///   - `conn` 在 `client` 之前声明 ⇒ 在 `client` 之后析构：h2 层把自己的读
///     回调挂在客户端上（`start()` 里的 `read_start_events`），客户端还没拆
///     干净就先删 h2 层，那之后的任何一次读都会进已释放的对象；
///   - `drain_` 最后声明 ⇒ 最先析构，在循环还活着的时候把收尾队列拨干净
///     （见 `loop_drain.h`）。
struct h2_client {
  client_probe                         p;
  std::unique_ptr<uvcpp_h2_connection> conn;
  uvcpp_tcp_client                     client;
  uvcpp_test::loop_drain               drain_;
  bool                                 began = false;

  h2_client() : drain_(client.get_loop()) {}

  uvcpp_loop*  loop() { return client.get_loop(); }
  client_probe& probe() { return p; }

  bool begin(int port, uvcpp_ssl_context* cctx, const char* label) {
    const int trc = client.enable_tls(cctx);
    check(trc == 0, std::string(label) + ": enable_tls returned " +
                        std::to_string(trc));
    // enable_tls 之后设：握手要到 connect 完成回调里才起。
    check(client.set_tls_alpn_protos({"h2"}),
          std::string(label) + ": set_tls_alpn_protos failed");

    const int crc = client.connect("127.0.0.1", port, [this, label](int status) {
      p.connect_status.store(status);
      p.hs_done_at_connect.store(client.is_tls_handshake_done());
      p.connect_fired.store(true);
      if (status != 0) return;
      {
        std::lock_guard<std::mutex> lk(p.mu);
        p.client_alpn = client.tls_alpn_selected();
      }

      // h2 层必须在连接**可用之后**才建：它自己装读回调，而 socket 没连上时
      // `read_start_events` 返回 UV_ENOTCONN，那个失败是静默的（没人再 arm）。
      conn.reset(new uvcpp_h2_connection(&client, /*server_side=*/false));

      uvcpp_h2_session::callbacks h2c;
      h2c.on_response = [this](uvcpp_h2_session&, uvcpp_h2_stream& st,
                               bool end_stream) {
        std::lock_guard<std::mutex> lk(p.mu);
        seen_response& r  = open_response(st.stream_id);
        r.status          = static_cast<int>(st.response.status_code);
        r.end_stream_at_headers = end_stream;
        r.content_type   = http_get_header(st.response.headers, "content-type");
        r.content_length =
            http_get_header(st.response.headers, "content-length");
        r.content_encoding =
            http_get_header(st.response.headers, "content-encoding");
      };
      h2c.on_body = [this](uvcpp_h2_session&, uvcpp_h2_stream& st,
                           const char* d, size_t n) {
        std::lock_guard<std::mutex> lk(p.mu);
        p.partial[st.stream_id].append(d, n);
      };
      h2c.on_response_end = [this](uvcpp_h2_session&, uvcpp_h2_stream& st) {
        std::lock_guard<std::mutex> lk(p.mu);
        seen_response& r = open_response(st.stream_id);
        r.body           = p.partial[st.stream_id];
        p.partial.erase(st.stream_id);
        r.end = true;
      };
      h2c.on_fatal = [this, label](uvcpp_h2_session&, int code) {
        std::cerr << "  [note] " << label << ": client h2 fatal " << code
                  << std::endl;
        p.fatal_count.fetch_add(1);
      };

      uvcpp_h2_connection::callbacks cc;
      cc.on_disconnect = [this](uvcpp_h2_connection&) {
        p.disconnect_count.fetch_add(1);
      };

      const int rv = conn->start(h2c, cc);
      p.start_rc.store(rv);
      check(rv == 0, std::string(label) + ": h2 conn start returned " +
                         std::to_string(rv));
      began = true;
    });
    check(crc == 0, std::string(label) + ": connect() start failed");
    return crc == 0;
  }

  /// 提交一个请求并把待发字节冲出去。
  ///
  /// `submit_request` 只把帧排进会话（`drain()` 才吐字节），而 `uvcpp_h2_connection`
  /// 只有响应侧的合并入口（`send_response`），没有请求侧的 —— 所以 `flush()`
  /// 必须自己调。忘了它就是"请求提交了但一个字节都没发"这种最难查的静默失败。
  /// `extra` 是逐条追加的附加请求头 —— 场景 7 用它给**一条流**单独挂
  /// `accept-encoding`，而另一条流不带。
  int32_t submit(http_method method, const std::string& url,
                 const std::string& body,
                 const std::string& content_type = "",
                 const std::vector<std::pair<std::string, std::string>>& extra =
                     std::vector<std::pair<std::string, std::string>>()) {
    if (conn == nullptr) return -1;
    uvcpp_http_request req;
    req.method = method;
    req.url    = url;
    req.set_header("host", "loopback.test");
    for (size_t i = 0; i < extra.size(); ++i) {
      req.set_header(extra[i].first, extra[i].second);
    }
    if (!content_type.empty()) req.set_content_type(content_type);
    if (!body.empty()) {
      req.set_header("content-length", std::to_string(body.size()));
    }
    const int32_t sid = conn->session().submit_request(req, body);
    if (sid <= 0) return sid;
    conn->flush();
    {
      std::lock_guard<std::mutex> lk(p.mu);
      p.submitted.push_back(sid);
    }
    p.bytes_out.store(conn->bytes_out());
    return sid;
  }

  /// 顺序发一个请求并等它整条收完。
  bool submit_and_wait(
      http_method method, const std::string& url, const std::string& body,
      const std::string& content_type = "",
      const std::vector<std::pair<std::string, std::string>>& extra =
          std::vector<std::pair<std::string, std::string>>()) {
    const size_t want = response_count() + 1;
    if (submit(method, url, body, content_type, extra) <= 0) return false;
    return uvcpp_test::wait_until(
        loop(), [this, want] { return response_count() >= want; }, kWaitMs);
  }

  /// 等响应数到达 `want`。场景 7 一次提交三条流，不能用 `submit_and_wait`。
  bool wait_responses(size_t want) {
    return uvcpp_test::wait_until(
        loop(), [this, want] { return response_count() >= want; }, kWaitMs);
  }

  /// 按流号取一条已收完的响应；没有就返回 false。
  bool find_response(int32_t sid, seen_response& out) {
    std::lock_guard<std::mutex> lk(p.mu);
    for (size_t i = 0; i < p.responses.size(); ++i) {
      if (p.responses[i].sid == sid) {
        out = p.responses[i];
        return true;
      }
    }
    return false;
  }

  size_t response_count() {
    std::lock_guard<std::mutex> lk(p.mu);
    return p.responses.size();
  }

  /// 关连接并等关闭**完成**。关完之后再不拆 h2 层也不会漏掉什么 ——
  /// 真正的拆除次序由成员声明次序保证（见本结构的说明）。
  bool finish() {
    bool closed = false;
    client.close([&closed] { closed = true; });
    // `close()` 在句柄已经没了（对端先断、或 h2 层已经关过）时也会把收尾跑
    // 一次，所以这个等待不会白等。
    const bool ok = uvcpp_test::wait_until(
        loop(), [&closed] { return closed; }, kWaitMs);
    uvcpp_test::pump_for(loop(), 60);
    return ok;
  }

 private:
  /// 找这条流的响应槽；没有就补一个，保持"按到达序"。
  seen_response& open_response(int32_t sid) {
    for (size_t i = 0; i < p.responses.size(); ++i) {
      if (p.responses[i].sid == sid) return p.responses[i];
    }
    seen_response r;
    r.sid = sid;
    p.responses.push_back(r);
    return p.responses.back();
  }
};

std::vector<seen_response> snapshot_responses(client_probe& p) {
  std::lock_guard<std::mutex> lk(p.mu);
  return p.responses;
}

std::vector<int32_t> snapshot_submitted(client_probe& p) {
  std::lock_guard<std::mutex> lk(p.mu);
  return p.submitted;
}

std::string snapshot_alpn(client_probe& p) {
  std::lock_guard<std::mutex> lk(p.mu);
  return p.client_alpn;
}

/// 连接已经建立且 h2 层起来了 —— 后面所有断言的前提。
bool wait_connected(h2_client& c, const char* label) {
  const bool ok =
      uvcpp_test::wait_until(c.loop(), [&c] { return c.began; }, kWaitMs);
  check(ok, std::string(label) + ": h2 connection never started");
  return ok;
}

void verify_connected(h2_client& c, const char* label) {
  const std::string tag(label);
  check(c.probe().connect_fired.load(), tag + ": connect cb never fired");
  check(c.probe().connect_status.load() == 0,
        tag + ": connect status = " +
            std::to_string(c.probe().connect_status.load()));
  check(c.probe().hs_done_at_connect.load(),
        tag + ": connect cb fired BEFORE handshake completed");
  check(c.probe().start_rc.load() == 0,
        tag + ": h2 start rc = " + std::to_string(c.probe().start_rc.load()));
  const std::string alpn = snapshot_alpn(c.probe());
  check(alpn == "h2", tag + ": client ALPN = \"" + alpn + "\", want \"h2\"");
}

}  // namespace

int main() {
  std::cout << "[web_ssl_h2_server] OpenSSL "
            << OpenSSL_version(OPENSSL_VERSION_STRING) << std::endl;

  uvcpp_ssl_context sctx(tls_mode::SERVER, tls_version::TLS_1_2);
  if (!sctx.is_ready() || !sctx.generate_self_signed("loopback.test", 2048)) {
    std::cerr << "  [FAIL] server ctx/cert: " << sctx.get_last_error()
              << std::endl;
    return 2;
  }
  sctx.set_alpn_select_protos(kServerAlpn);

  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  // 自签证书的回环用例需要它；生产客户端不能这样（默认接受任何证书 =
  // 中间人可以直接接管）。显式写出来就是要让它显眼。
  cctx.set_verify_mode(tls_verify_mode::NONE);
  if (!cctx.is_ready()) {
    std::cerr << "  [FAIL] client ctx not ready" << std::endl;
    return 2;
  }

  // =====================================================================
  // 场景 1..5：h2 打开，一条 TCP 连接跑完五个请求
  // =====================================================================
  {
    scenario_server srv(&sctx, /*enable_h2=*/true);
    if (!srv.ok()) {
      std::cerr << "  [FAIL] h2 server failed to bind/listen" << std::endl;
      return 2;
    }
    check(srv.st.h2_flag.load() == 1, "h2_on: http2_enabled() reads false");

    h2_client c;
    if (!c.begin(srv.port, &cctx, "h2_on") || !wait_connected(c, "h2_on")) {
      std::cerr << "  [FAIL] h2_on: client never became usable" << std::endl;
      return 2;
    }
    verify_connected(c, "h2_on");

    // ---- 场景 1：GET /hello -----------------------------------------
    check(c.submit_and_wait(http_method::HTTP_GET, "/hello", ""),
          "get_hello: no response within " + std::to_string(kWaitMs) + "ms");

    // ---- 场景 2：POST 带 body ---------------------------------------
    check(c.submit_and_wait(http_method::HTTP_POST, "/echo", kPostBody,
                            "text/plain"),
          "post_echo: no response within " + std::to_string(kWaitMs) + "ms");

    // ---- 场景 3：HEAD -----------------------------------------------
    check(c.submit_and_wait(http_method::HTTP_HEAD, "/hello", ""),
          "head_hello: no response within " + std::to_string(kWaitMs) + "ms");

    // ---- 场景 4：未注册的路径 ---------------------------------------
    check(c.submit_and_wait(http_method::HTTP_GET, "/nope", ""),
          "not_found: no response within " + std::to_string(kWaitMs) + "ms");

    // ---- 场景 5：同一条连接上的第二个流 -----------------------------
    check(c.submit_and_wait(http_method::HTTP_GET, "/hello", ""),
          "second_on_same_conn: no response within " +
              std::to_string(kWaitMs) + "ms");

    const std::vector<int32_t>       sids = snapshot_submitted(c.probe());
    const std::vector<seen_response> resp = snapshot_responses(c.probe());
    const std::vector<seen_request>  seen = snapshot_seen(srv.st);

    // ---- 前提：五个请求、五个流号对号入座 ---------------------------
    check(sids.size() == 5,
          "streams: submitted " + std::to_string(sids.size()) + ", want 5");
    check(resp.size() == 5,
          "streams: got " + std::to_string(resp.size()) + " responses, want 5");
    if (sids.size() == 5 && resp.size() == 5) {
      // 客户端发起的 h2 流号是**奇数、从 1 开始、单调递增**（RFC 9113 §5.1.1）。
      // 钉住这一串是有意义的：服务端若把某条流的 id 记错，响应就落到别的号
      // 上，而这在"我只看到一条流的响应"的用例里查不出来。
      const int32_t want[5] = {1, 3, 5, 7, 9};
      for (int i = 0; i < 5; ++i) {
        check(sids[i] == want[i],
              "streams: submitted[" + std::to_string(i) + "] = " +
                  std::to_string(sids[i]) + ", want " + std::to_string(want[i]));
        check(resp[i].sid == want[i],
              "streams: responses[" + std::to_string(i) + "] landed on stream " +
                  std::to_string(resp[i].sid) + ", want " +
                  std::to_string(want[i]));
      }
    }

    // ---- 服务端侧：五个请求里有四个被路由 ---------------------------
    check(seen.size() == 4,
          "server saw " + std::to_string(seen.size()) +
              " routed requests, want 4 (the 404 one must not be routed)");
    for (size_t i = 0; i < seen.size(); ++i) {
      check(seen[i].stream_id != 0,
            "server saw request " + std::to_string(i) + " with stream_id 0");
      check(seen[i].version == uvcpp_http_version::HVER_20,
            "server saw request " + std::to_string(i) + " as " +
                uvcpp_http_version_str(seen[i].version) + ", want HTTP/2");
    }

    if (resp.size() == 5 && sids.size() == 5) {
      // ---- 场景 1 的断言 --------------------------------------------
      check(resp[0].status == 200,
            "get_hello: status = " + std::to_string(resp[0].status));
      check(resp[0].body == kHelloBody,
            "get_hello: body = \"" + resp[0].body + "\", want \"" +
                std::string(kHelloBody) + "\"");
      check(resp[0].content_type == "text/plain",
            "get_hello: content-type = \"" + resp[0].content_type + "\"");
      check(resp[0].end, "get_hello: response never completed");
      check(resp[0].content_length == std::to_string(sizeof(kHelloBody) - 1),
            "get_hello: content-length = \"" + resp[0].content_length + "\"");
      if (seen.size() >= 1) {
        check(seen[0].url == "/hello" &&
                  seen[0].method == http_method::HTTP_GET,
              "get_hello: handler saw the wrong route");
        check(seen[0].stream_id == sids[0],
              "get_hello: handler saw stream_id " +
                  std::to_string(seen[0].stream_id) + ", want " +
                  std::to_string(sids[0]));
      }

      // ---- 场景 2 的断言 --------------------------------------------
      check(resp[1].status == 200,
            "post_echo: status = " + std::to_string(resp[1].status));
      check(resp[1].body == kPostBody,
            "post_echo: body = \"" + resp[1].body + "\", want \"" +
                std::string(kPostBody) + "\"");
      check(resp[1].end, "post_echo: response never completed");
      if (seen.size() >= 2) {
        // 处理函数看到的**必须是逐字节相同的请求体**：h2 的 body 是分块来的，
        // 连接级一个标量累加（或者干脆不累加）在这里就会露出来。
        check(seen[1].body == kPostBody,
              "post_echo: handler saw body \"" + seen[1].body + "\", want \"" +
                  std::string(kPostBody) + "\"");
        check(seen[1].content_length == std::to_string(sizeof(kPostBody) - 1),
              "post_echo: handler saw content-length \"" +
                  seen[1].content_length + "\", want " +
                  std::to_string(sizeof(kPostBody) - 1));
        check(seen[1].stream_id == sids[1],
              "post_echo: handler saw stream_id " +
                  std::to_string(seen[1].stream_id) + ", want " +
                  std::to_string(sids[1]));
      }

      // ---- 场景 3 的断言 --------------------------------------------
      check(resp[2].status == 200,
            "head_hello: status = " + std::to_string(resp[2].status));
      // **没有 DATA 帧**：HEADERS 自带 END_STREAM，body 一个字节都不该有。
      check(resp[2].body.empty(),
            "head_hello: got " + std::to_string(resp[2].body.size()) +
                " body bytes, want 0");
      check(resp[2].end_stream_at_headers,
            "head_hello: HEADERS did not carry END_STREAM");
      check(resp[2].end, "head_hello: response never completed");
      // content-length 是"GET 会有多长"，不是 0 —— 这是 HEAD 最容易做错的地方
      // （不做就等于告诉客户端这个资源是空的）。
      check(resp[2].content_length == resp[0].content_length,
            "head_hello: content-length = \"" + resp[2].content_length +
                "\", want the GET one (\"" + resp[0].content_length + "\")");
      check(!resp[2].content_length.empty() && resp[2].content_length != "0",
            "head_hello: content-length = \"" + resp[2].content_length + "\"");
      if (seen.size() >= 3) {
        check(seen[2].method == http_method::HTTP_HEAD,
              "head_hello: handler saw method " +
                  std::string(http_method_str(seen[2].method)));
        check(seen[2].stream_id == sids[2],
              "head_hello: handler saw stream_id " +
                  std::to_string(seen[2].stream_id));
      }

      // ---- 场景 4 的断言 --------------------------------------------
      check(resp[3].status == 404,
            "not_found: status = " + std::to_string(resp[3].status));
      check(resp[3].body == kNotFoundBody,
            "not_found: body = \"" + resp[3].body + "\"");
      check(resp[3].end, "not_found: response never completed");
      // 兜底分支是 `dispatch_h2_request` 自己拼的 404 —— 它**也**得带上流号，
      // 否则这条流永远等不到应答。
      check(resp[3].sid == sids[3], "not_found: answered on the wrong stream");

      // ---- 场景 5 的断言 --------------------------------------------
      check(resp[4].status == 200 && resp[4].body == kHelloBody,
            "second_on_same_conn: status = " + std::to_string(resp[4].status) +
                ", body = \"" + resp[4].body + "\"");
      check(resp[4].end, "second_on_same_conn: response never completed");
    }

    // ---- 场景 5 的**连接复用**判据 ----------------------------------
    // 五个请求、服务端只在 `on_connection` 里被叫过一次 —— 这条 TCP 连接从头
    // 用到尾。计数器比"看着像"强：客户端要是每条请求重连一次，这个数会是 5。
    check(srv.st.connections.load() == 1,
          "reuse: server accepted " + std::to_string(srv.st.connections.load()) +
              " connections, want 1");
    check(srv.st.alpn_h2_at_delivery.load() == 1,
          "reuse: ALPN=h2 at delivery for " +
              std::to_string(srv.st.alpn_h2_at_delivery.load()) +
              " connections, want 1");

    check(c.finish(), "h2_on: client close never completed");
    srv.shutdown();

    // 一条连接都没交付时，把**下面那一层**的证据打出来。没有这行提示，读日志
    // 的人会从 h2 分发开始猜，而真正发生的事在服务端 TLS 那一层：
    // `uvcpp_tcp_server::on_tls_handshake_done` 的 status 非 0 时连接**根本
    // 不交给上层**，只把它记进 `get_last_error()`。
    if (srv.st.connections.load() == 0) {
      std::cerr << "  [note] h2_on: the server never delivered the connection "
                   "(listen_rc="
                << srv.st.listen_rc.load()
                << ", tcp_server last_error=" << srv.st.final_last_error.load()
                << ") — so this is not an h2 routing problem" << std::endl;
    }

    // 关闭事件跑完之后登记表该是空的（连接确实被摘了，没有漏账）。
    check(srv.st.final_count.load() == 0,
          "h2_on: client_count() after shutdown = " +
              std::to_string(srv.st.final_count.load()));
    check(srv.st.closes.load() == 1,
          "h2_on: on_connection_close fired " +
              std::to_string(srv.st.closes.load()) + " times, want 1");
  }

  // =====================================================================
  // 场景 6：h2 关闭（默认值），客户端照样宣告 h2
  // =====================================================================
  {
    scenario_server srv(&sctx, /*enable_h2=*/false);
    if (!srv.ok()) {
      std::cerr << "  [FAIL] h1 server failed to bind/listen" << std::endl;
      return 2;
    }
    // 默认值就在文档里写着（`set_http2_enabled` 的注释：**默认 false**）。
    // 这个场景故意一次都不调 setter，钉的就是那个默认。
    check(srv.st.h2_flag.load() == 0,
          "h2_off: http2_enabled() default reads true");

    h2_client c;
    if (!c.begin(srv.port, &cctx, "h2_off") || !wait_connected(c, "h2_off")) {
      std::cerr << "  [FAIL] h2_off: client never became usable" << std::endl;
      return 2;
    }
    verify_connected(c, "h2_off");

    // 与场景 1 **逐字相同**的请求，唯一的差别是服务端那个开关。
    const int32_t sid = c.submit(http_method::HTTP_GET, "/hello", "");
    check(sid > 0, "h2_off: submit_request returned " + std::to_string(sid));
    check(c.probe().bytes_out.load() > 0,
          "h2_off: client wrote nothing — the request never left");

    // 等到"有结论"为止：要么客户端把这条流当 h1 报文解析出致命错误/断开，
    // 要么它（错误地）拿到了一条 h2 响应。**不做"睡两秒然后断言没有响应"**：
    // 那种写法在"客户端压根没发出去"时也照样绿。
    uvcpp_test::wait_until(
        c.loop(),
        [&c] {
          return c.probe().fatal_count.load() > 0 ||
                 c.probe().disconnect_count.load() > 0 ||
                 c.response_count() > 0;
        },
        kWaitMs);
    // 这一条是**反向**判据（期望一直为空），窗口给足比给少安全。
    uvcpp_test::pump_for(c.loop(), 300);

    const std::vector<seen_response> resp = snapshot_responses(c.probe());
    const std::vector<seen_request>  seen = snapshot_seen(srv.st);

    // 核心判据：**没有一条 h2 请求在这条连接上完成过。**
    check(resp.empty(),
          "h2_off: server answered an h2 request on stream " +
              (resp.empty() ? std::string("?") : std::to_string(resp[0].sid)));
    check(seen.empty(),
          "h2_off: server routed " + std::to_string(seen.size()) +
              " request(s) over a connection the client spoke h2 on");
    // 正面对照：这条连接**真的到了服务端**、而且服务端看到的 ALPN 就是 h2。
    // 少了这两条，"拒绝当成 h2 服务"与"没连上"分不开，整条用例就成了废话。
    check(srv.st.connections.load() == 1,
          "h2_off: server accepted " + std::to_string(srv.st.connections.load()) +
              " connections, want 1");
    check(srv.st.alpn_h2_at_delivery.load() == 1,
          "h2_off: ALPN=h2 at delivery for " +
              std::to_string(srv.st.alpn_h2_at_delivery.load()) +
              " connections, want 1 (the client did offer h2)");
    // 服务端把这份字节流当 HTTP/1.1 解析，回的是 h1 的 400，而它对 h2 是帧长
    // 错误 —— 于是会话层要么报致命错误、要么直接断开。两者都算"没被当成 h2
    // 服务"，但**必须有一个**：都没有就说明这条用例只观测到了沉默。
    check(c.probe().fatal_count.load() > 0 ||
              c.probe().disconnect_count.load() > 0,
          "h2_off: the h2 layer neither errored nor disconnected — "
          "the outcome was silence, which proves nothing");

    check(c.finish(), "h2_off: client close never completed");
    srv.shutdown();
    check(srv.st.final_count.load() == 0,
          "h2_off: client_count() after shutdown = " +
              std::to_string(srv.st.final_count.load()));
  }

  // =====================================================================
  // 场景 7：**按流**的状态 —— 延迟应答不能读另一条流的判定
  // =====================================================================
  //
  // 前六个场景里每条流都是"派发即应答"，于是"按流"和"按连接"看起来一样。
  // 这一条把两者掰开：`/defer` 挂起、`/release` 放行，**派发序与应答序故意
  // 错开**，中间夹一条会改连接级判定的流。
  //
  // 两条子用例各钉一个连接级标量，都是确定性的（不靠抢时序）：
  //   A. `is_head`：流 1 = 延迟的 **GET**，流 3（放行）是 **HEAD**。旧实现里
  //      `send_h2_response` 读连接级的 `is_head`，此时它就是流 3 刚写下的
  //      true ⇒ 流 1 的 GET 被当成 HEAD 应答：`content-length: 2048` 配
  //      HEADERS+END_STREAM、一个 DATA 帧都没有。
  //
  //      **失败的样子不是"body 被掐掉"，而是"这条响应整个不见了"**（实测）：
  //      客户端 nghttp2 知道流 1 的请求是 GET，"声明了 2048 字节却在 HEADERS
  //      上就 END_STREAM"属于协议违例，它直接把整条流 RST 掉 —— 所以判据落在
  //      "延迟流必须有响应"，`!end_stream_at_headers` 只是二次防线。
  //
  //      **放行那条流必须是 HEAD，这一条不能"简化"成 GET。** 原先写成 GET，
  //      于是一串派发下来：流 2 的 HEAD 把连接级 `is_head` 置成 true、流 3 的
  //      GET 立刻又置回 false，而延迟响应恰恰是在流 3 的派发**里面**构造的 ——
  //      读到的是 false。子用例看着在钉 `is_head`，其实一次都没撞上过（实测：
  //      把 `send_h2_response` 改回读连接级，A 全绿、只有 B 变红）。流 2 那个
  //      干扰项因此也是**摆设**，留着只为对照 HEAD 响应本身的形状。
  //   B. `accept_encoding`：流 3 带 `accept-encoding: gzip` 且延迟，流 4 不带。
  //      旧实现按流 4 的空串判定 ⇒ 该压的没压。判据是 `content-encoding: gzip`。
  //      （这一条天然成立：放行流自己不带 `accept-encoding`，正好把它抹平。）
  {
    scenario_server srv(&sctx, /*enable_h2=*/true);
    if (!srv.ok()) {
      std::cerr << "  [FAIL] per_stream server failed to bind/listen" << std::endl;
      return 2;
    }

    h2_client c;
    if (!c.begin(srv.port, &cctx, "per_stream") ||
        !wait_connected(c, "per_stream")) {
      std::cerr << "  [FAIL] per_stream: client never became usable" << std::endl;
      return 2;
    }
    verify_connected(c, "per_stream");

    // ---- 子用例 A：延迟的 GET 不能被并发流当成 HEAD -------------------
    const int32_t a_defer = c.submit(http_method::HTTP_GET, "/defer", "");
    check(a_defer > 0, "per_stream/A: 延迟 GET 没提交上");
    // 干扰项，但**不是**钉住 `is_head` 的那一条（它后面的 GET 会把它抹掉，
    // 见上面的说明）。留着只为对照 HEAD 响应本身的形状。
    const int32_t a_head = c.submit(http_method::HTTP_HEAD, "/hello", "");
    check(a_head > 0, "per_stream/A: 干扰 HEAD 没提交上");
    // 真正钉住 `is_head` 的是这一条：**HEAD** 放行 ⇒ 延迟响应构造时连接级
    // `is_head` 是 true。改成 GET 这条子用例就退化成空的。
    const int32_t a_rel = c.submit(http_method::HTTP_HEAD, "/release", "");
    check(a_rel > 0, "per_stream/A: /release 没提交上");

    if (!c.wait_responses(3)) {
      // 收不齐时**自报家门**：只报"没到 3 条"分不出"延迟流压根没发"和
      // "发了但落到别的流号上"。
      std::vector<seen_response> rs = snapshot_responses(c.probe());
      std::vector<int32_t>       subs = snapshot_submitted(c.probe());
      std::cerr << "  [diag] per_stream/A: 只收到 " << rs.size() << " 条;";
      for (size_t i = 0; i < rs.size(); ++i) {
        std::cerr << " {sid=" << rs[i].sid << " st=" << rs[i].status
                  << " end=" << rs[i].end << " esh=" << rs[i].end_stream_at_headers
                  << "}";
      }
      std::cerr << " 已提交:";
      for (size_t i = 0; i < subs.size(); ++i) std::cerr << " " << subs[i];
      std::cerr << " (defer=" << a_defer << " head=" << a_head
                << " rel=" << a_rel << ")" << std::endl;
      check(false, "per_stream/A: 3 条响应没在 " + std::to_string(kWaitMs) +
                       "ms 内收齐");
    }
    uvcpp_test::pump_for(c.loop(), 100);

    seen_response ra;
    const bool a_has = c.find_response(a_defer, ra);
    check(a_has, "per_stream/A: 延迟流没有响应");
    if (a_has) {
      check(ra.end, "per_stream/A: 延迟流响应没收完");
      check(ra.status == 200, "per_stream/A: 状态 " +
                                  std::to_string(ra.status) + "，要 200");
      // **核心判据**：延迟的 GET 必须把 body 发出来。旧实现会给它一个
      // `HEADERS + END_STREAM`（body 被当成 HEAD 掐了）。
      check(!ra.end_stream_at_headers,
            "per_stream/A: 延迟的 GET 收到 HEADERS+END_STREAM —— body 被"
            "另一条流（HEAD）的 `is_head` 掐掉了");
      check(ra.body == kDeferBody,
            "per_stream/A: 延迟流 body 长度 " + std::to_string(ra.body.size()) +
                "，要 " + std::to_string(kDeferBody.size()));
    }

    // 干扰项自己也必须是对的：HEAD 就该是 HEADERS+END_STREAM、无 body。
    // 少了这条，"两条流都没 body"也能让上面那组断言看起来有道理。
    seen_response rh;
    const bool a_head_has = c.find_response(a_head, rh);
    check(a_head_has, "per_stream/A: 干扰 HEAD 没有响应");
    if (a_head_has) {
      check(rh.end_stream_at_headers,
            "per_stream/A: HEAD 的响应没有在 HEADERS 上 END_STREAM");
      check(rh.body.empty(),
            "per_stream/A: HEAD 的响应带了 " + std::to_string(rh.body.size()) +
                " 字节 body");
    }

    // ---- 子用例 B：gzip 按**这条流**的 accept-encoding 决定 -----------
    const std::vector<std::pair<std::string, std::string>> wants_gzip{
        {"accept-encoding", "gzip"}};
    const int32_t b_defer = c.submit(http_method::HTTP_GET, "/defer", "", "",
                                     wants_gzip);
    check(b_defer > 0, "per_stream/B: 带 gzip 的延迟 GET 没提交上");
    // 干扰项：**不带** accept-encoding，inline 应答。
    const int32_t b_plain = c.submit(http_method::HTTP_GET, "/hello", "");
    check(b_plain > 0, "per_stream/B: 干扰 GET 没提交上");
    const int32_t b_rel = c.submit(http_method::HTTP_GET, "/release", "");
    check(b_rel > 0, "per_stream/B: /release 没提交上");

    check(c.wait_responses(6), "per_stream/B: 6 条响应没在 " +
                                   std::to_string(kWaitMs) + "ms 内收齐");
    uvcpp_test::pump_for(c.loop(), 100);

    seen_response rb;
    const bool b_has = c.find_response(b_defer, rb);
    check(b_has, "per_stream/B: 延迟流没有响应");
    if (b_has) {
      // **核心判据**：要了 gzip 的那条流必须拿到 gzip。
      check(rb.content_encoding == "gzip",
            "per_stream/B: 要了 gzip 的流拿到 content-encoding=\"" +
                rb.content_encoding +
                "\" —— 压缩方式是按另一条流的 accept-encoding 定的");
      // `content-encoding` 是个**声明**，光有它证明不了 body 真被压过。
      // 压过的长度必须明显小于原文 —— 这条把它钉死。
      check(rb.body.size() < kDeferBody.size(),
            "per_stream/B: 声称 gzip 但 body 有 " +
                std::to_string(rb.body.size()) + " 字节，原文才 " +
                std::to_string(kDeferBody.size()) + " 字节");
    }
    // 干扰项对照：**没要** gzip 的那条流不该被压。
    seen_response rp;
    const bool b_plain_has = c.find_response(b_plain, rp);
    check(b_plain_has, "per_stream/B: 干扰 GET 没有响应");
    if (b_plain_has) {
      check(rp.content_encoding.empty(),
            "per_stream/B: 没要 gzip 的流被压了（content-encoding=\"" +
                rp.content_encoding + "\"）");
    }

    check(c.finish(), "per_stream: client close never completed");
    srv.shutdown();
    check(srv.st.final_count.load() == 0,
          "per_stream: client_count() after shutdown = " +
              std::to_string(srv.st.final_count.load()));
  }

  if (g_failures != 0) {
    std::cerr << "[web_ssl_h2_server] " << g_failures << " failure(s)"
              << std::endl;
    return 1;
  }
  std::cout << "[web_ssl_h2_server] all scenarios OK" << std::endl;
  return 0;
}

#else
int main() {
  std::cout << "[web_ssl_h2_server] skipped (needs UVCPP_WEB_ENABLE && "
               "UVCPP_OPENSSL_ENABLE && UVCPP_NGHTTP2_ENABLE)"
            << std::endl;
  return 0;
}
#endif
