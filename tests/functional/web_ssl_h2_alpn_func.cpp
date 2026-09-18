/**
 * @file tests/functional/web_ssl_h2_alpn_func.cpp
 * @brief ALPN 协商:六种「客户端宣告 × 服务端选择」组合。
 *
 * 这批是 HTTP/2 的地基,而地基里最容易错的一处是**没交集时怎么办**。
 * `SSL_select_next_proto` 在两边没有共同协议时**照样会写 out/outlen**
 * (让它指向服务端列表的第一项),所以它的返回值必须按 `OPENSSL_NPN_NEGOTIATED`
 * 判,不能按"有没有写出去"判。判错的后果不是"没协商出 ALPN"那么轻 —— 客户端
 * 要 spdy/3.1、服务端答 h2,握手成功、双方对协议的理解不一致,比直接失败糟得多。
 *
 * 所以场景 6(客户端只宣告 spdy/3.1)是本文件的**核心判据**:两侧都必须读到
 * 空串。写错的那版在这里会读到 "h2"。
 *
 * 另一条判据是**非 h2 客户端不受影响**:不带 ALPN 扩展的、以及只宣告
 * http/1.1 的客户端,必须照样握手成功、照样跑完一个完整的 HTTP/1.1 请求。
 * 服务端的选择回调在没交集时返回 `SSL_TLSEXT_ERR_NOACK`(不是 fatal)正是为了
 * 这个 —— 返回 fatal 会让所有老客户端连握手都完不成。只断言"握手成功"不够,
 * 所以那三条都走真正的 `uvcpp_http_server` 路由并核对响应体。
 *
 * 服务端侧读 ALPN 的两个位置都覆盖到了:tcp_server 的 `on_connection`
 * (HTTP/2 分流点,批 2c 就在这里判)与 http_server 的路由 handler。
 *
 * 客户端侧两条 API 都覆盖:context 上的 `set_alpn_protos` 与 per-connection 的
 * `set_tls_alpn_protos`(后者的存在意义是"要不要 h2"是每条连接各自的决定),
 * 场景 5 钉住后者确实**覆盖**了前者。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_OPENSSL_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>
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

/// 客户端最多等这么多**毫秒**(墙钟)。
const int kWaitMs = 4000;

/// 服务端宣告的协议名单。h2 排第一 —— 场景 6 里"错的那版"会把这一项硬塞给
/// 只认 spdy/3.1 的客户端,所以顺序是判据的一部分,不能随便排。
const std::vector<std::string> kServerAlpn{"h2", "http/1.1"};

const char kRequest[] = "GET /hello HTTP/1.1\r\nHost: loopback.test\r\n\r\n";

/// 路由响应体。客户端拿它证明"这个请求真的走到了业务代码"。
const char kBody[] = "ALPN-OK";

// =========================================================================
// 服务端侧观测
// =========================================================================

struct server_state {
  std::atomic<int> listen_rc{-1};
  std::atomic<int> delivered{0};  ///< on_connection 被调用的次数(仅 tcp 分支)
  /**
   * @brief 交付时握手已完成的次数。
   *
   * 与 `web_ssl_server_func.cpp` 同理由:ALPN 是在**握手之内**协商完的,所以
   * "on_connection 里能读到终局的 ALPN"完全依赖"握手完了才交付"这条约定。
   * 少了这个计数,一个提前交付的实现也能让每条 ALPN 断言通过(读到空串),
   * 只是恰好和期望值一样。
   */
  std::atomic<int> hs_done_at_delivery{0};
  std::atomic<int> final_count{-1};  ///< 收尾后的 client_count()

  std::mutex mu;
  std::vector<std::string> alpn_seen;  ///< 每条连接协商出的协议名
  std::vector<std::string> paths;      ///< 收到的请求路径
};

void record_alpn(server_state& st, uvcpp_tcp_client* client) {
  std::string alpn = client != nullptr ? client->tls_alpn_selected() : std::string();
  std::lock_guard<std::mutex> lk(st.mu);
  st.alpn_seen.push_back(alpn);
}

std::vector<std::string> snapshot_alpn(server_state& st) {
  std::lock_guard<std::mutex> lk(st.mu);
  return st.alpn_seen;
}

std::vector<std::string> snapshot_paths(server_state& st) {
  std::lock_guard<std::mutex> lk(st.mu);
  return st.paths;
}

/// 服务端已经登记过几条连接。
///
/// 客户端握手完成与服务端交付**不是同一次回调**:客户端的 connect 回调可能在
/// 服务端的 `on_connection` 之前就跑完。直接读 `alpn_seen` 会偶发读到"服务端
/// 一条都没看见"。所以每条场景都先按这个计数等到位再断言 —— 服务端两个分支都
/// 是**先** `record_alpn` 再 `delivered++` / 填响应,所以计数到位就等于记录到位。
size_t record_count(server_state& st) {
  std::lock_guard<std::mutex> lk(st.mu);
  return st.alpn_seen.size();
}

// =========================================================================
// 服务端线程
// =========================================================================

/// 起一个装了 TLS 的服务端并在 `port_promise` 上放行端口。
///
/// `with_http` 为真时装一个真正的 `uvcpp_http_server`(路由 `/hello`),否则只起
/// 裸 `uvcpp_tcp_server`。分成两支是因为 HTTP/2 那条路径在批 2a 还没有分流器,
/// 让一个已协商出 h2 的连接去跑 HTTP/1.1 报文只是**当下**成立的事实,写成断言
/// 就是给后面埋雷 —— 所以 h2 的两个场景只用裸 tcp_server 判协商结果。
void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                server_state& st, uvcpp_ssl_context* sctx, bool with_http) {
  std::unique_ptr<uvcpp_tcp_server>  raw;
  std::unique_ptr<uvcpp_http_server> http;
  uvcpp_tcp_server* tcp = nullptr;

  if (with_http) {
    http.reset(new uvcpp_http_server());
    http->get("/hello", [&st](uvcpp_http_request& req, uvcpp_http_response& resp,
                              uvcpp_tcp_client* client) {
      record_alpn(st, client);
      {
        std::lock_guard<std::mutex> lk(st.mu);
        st.paths.push_back(req.url);
      }
      resp = uvcpp_http_response::ok(kBody, sizeof(kBody) - 1);
    });
    tcp = http->get_tcp_server();
  } else {
    raw.reset(new uvcpp_tcp_server());
    tcp = raw.get();
  }

  // 必须在 listen() 之前装:accept 时读不到就是这个原因(见
  // `web_ssl_app_func.cpp` 场景 2 的说明)。
  tcp->set_ssl_context(sctx);

  const int brc = tcp->bindIpv4("127.0.0.1", 0);
  if (brc != 0) {
    port_promise.set_value(-1);
    return;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  tcp->get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int bound_port = ntohs(name.sin_port);

  int rc = 0;
  if (with_http) {
    rc = http->listen(128);
  } else {
    rc = raw->listen(
        [&st](uvcpp_tcp_client* client) {
          if (client->is_tls() && client->is_tls_handshake_done())
            st.hs_done_at_delivery.fetch_add(1);
          record_alpn(st, client);
          st.delivered.fetch_add(1);
        },
        128);
  }
  st.listen_rc.store(rc);

  // 端口必须在 listen() **之后**才放行:已 bind、尚未 listen 的 socket 是拒连的
  // (RST → ECONNREFUSED),那个窗口很小但真实存在。
  port_promise.set_value(rc != 0 ? -1 : bound_port);
  if (rc != 0) return;

  uvcpp_loop* loop = tcp->get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  uvcpp_test::pump_for(loop, 400);
  st.final_count.store(static_cast<int>(tcp->client_count()));
}

// =========================================================================
// 客户端
// =========================================================================

struct client_probe {
  std::atomic<int>  connect_status{-99};
  std::atomic<bool> connect_fired{false};
  std::atomic<bool> hs_done_at_connect{false};
  std::atomic<int>  read_arm_rc{-999};
  std::atomic<int>  write_rc{-999};

  std::mutex  mu;
  std::string client_alpn;  ///< 握手完立刻缓存的那个值
  std::string received;

  /// 跑这条客户端**之前**服务端已登记几条 —— 用来把"这次连接被登记了"变成
  /// 有上界的等待,而不是靠 sleep 凑。
  size_t records_before = 0;
};

/// 跑一个客户端。`want_alpn` 为空表示**不调** per-connection 那条 API。
/// `use_http_branch` 为真时发一个真正的 HTTP/1.1 请求并等响应;为假时只连上、
/// 读到协商结果就收手。
void run_client(int port, uvcpp_ssl_context* cctx,
                const std::vector<std::string>& want_alpn, std::string request,
                client_probe& p, const char* label) {
  uvcpp_tcp_client client;

  const int trc = client.enable_tls(cctx);
  check(trc == 0, std::string(label) + ": enable_tls returned " + std::to_string(trc));

  // enable_tls 之后设:顺便钉住"装好之后设也来得及"(握手要到 connect 完成回调
  // 里才起)。这个时机是批 2e 的 h2 客户端要用的那个。
  if (!want_alpn.empty()) {
    check(client.set_tls_alpn_protos(want_alpn),
          std::string(label) + ": set_tls_alpn_protos failed");
  }

  const int crc = client.connect(
      "127.0.0.1", port, [&client, &p, request](int status) {
        p.connect_status.store(status);
        p.hs_done_at_connect.store(client.is_tls_handshake_done());
        p.connect_fired.store(true);
        if (status != 0) return;

        {
          std::lock_guard<std::mutex> lk(p.mu);
          p.client_alpn = client.tls_alpn_selected();
        }

        // 起读必须在 connect 回调里:socket 还没连上时 read_start_events 返回
        // UV_ENOTCONN,而且那个失败是静默的(没人再去 arm 它)。
        p.read_arm_rc.store(client.read_start_events(
            [&p](uvcpp_tcp_client&, const net_read_result& r) {
              if (r.is_data()) {
                std::lock_guard<std::mutex> lk(p.mu);
                p.received.append(r.data, r.size);
              }
            }));

        if (!request.empty()) {
          p.write_rc.store(client.write(
              request.data(), request.size(), [](int) {}));
        }
      });
  check(crc == 0, std::string(label) + ": connect() start failed");

  uvcpp_loop* loop = client.get_loop();
  uvcpp_test::wait_until(
      loop,
      [&p, &request] {
        std::lock_guard<std::mutex> lk(p.mu);
        if (!request.empty()) return p.received.find(kBody) != std::string::npos;
        return p.connect_fired.load();
      },
      kWaitMs);

  client.close();
  uvcpp_test::pump_for(loop, 60);
}

std::string probe_alpn(client_probe& p) {
  std::lock_guard<std::mutex> lk(p.mu);
  return p.client_alpn;
}

std::string probe_received(client_probe& p) {
  std::lock_guard<std::mutex> lk(p.mu);
  return p.received;
}

/// 一条场景要核的全部东西。`want_server_alpn`/`want_client_alpn` 为空串就是
/// 期望"两侧都没协商出 ALPN" —— 这是场景 3 与场景 6 的判据。
void verify_scenario(server_state& st, client_probe& p,
                     const std::string& want_client_alpn,
                     const std::string& want_server_alpn, bool expect_http,
                     const char* label) {
  const std::string tag(label);
  check(p.connect_fired.load(), tag + ": connect cb never fired");
  check(p.connect_status.load() == 0,
        tag + ": connect status = " + std::to_string(p.connect_status.load()));
  check(p.hs_done_at_connect.load(),
        tag + ": connect cb fired BEFORE handshake completed");

  const std::string got_client = probe_alpn(p);
  check(got_client == want_client_alpn,
        tag + ": client ALPN = \"" + got_client + "\", want \"" + want_client_alpn + "\"");

  // 服务端那条连接刚跑完,它的观测一定已经落进 alpn_seen 的末尾。
  check(uvcpp_test::wait_flag([&st, &p] { return record_count(st) > p.records_before; },
                              1000),
        tag + ": server never recorded this connection");
  const std::vector<std::string> seen = snapshot_alpn(st);
  check(!seen.empty(), tag + ": server saw no connection at all");
  if (!seen.empty()) {
    const std::string got_server = seen.back();
    check(got_server == want_server_alpn,
          tag + ": server ALPN = \"" + got_server + "\", want \"" + want_server_alpn + "\"");
  }

  if (expect_http) {
    check(p.read_arm_rc.load() == 0,
          tag + ": read_start_events returned " + std::to_string(p.read_arm_rc.load()));
    check(p.write_rc.load() == 0,
          tag + ": write returned " + std::to_string(p.write_rc.load()));
    const std::string got = probe_received(p);
    check(got.find("200") != std::string::npos,
          tag + ": no 200 in response (" + std::to_string(got.size()) + " bytes)");
    check(got.find(kBody) != std::string::npos, tag + ": body missing from response");
    const std::vector<std::string> paths = snapshot_paths(st);
    check(!paths.empty() && paths.back() == "/hello",
          tag + ": server did not parse the request path");
  }
}

/// 一个场景一个服务端 —— 每条连接都是独立的一条,断言不必按顺序对号入座。
struct scenario_server {
  server_state      st;
  std::atomic<bool> stop{false};
  std::promise<int> port_promise;
  std::future<int>  port_future;
  std::thread       thread;
  int               port = -1;

  scenario_server(uvcpp_ssl_context* sctx, bool with_http) : port_future(port_promise.get_future()) {
    thread = std::thread(run_server, std::ref(port_promise), std::ref(stop),
                         std::ref(st), sctx, with_http);
    port = port_future.get();
  }

  /// 停服务端并等它把收尾跑完。`final_count` 只在服务端线程末尾写,所以
  /// **必须停了之后才读** —— 停之前读到的是 -1,一个恒假的断言。
  void shutdown() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }

  ~scenario_server() { shutdown(); }

  bool ok() const { return port > 0 && st.listen_rc.load() == 0; }
};

}  // namespace

int main() {
  std::cout << "[web_ssl_h2_alpn] OpenSSL " << OpenSSL_version(OPENSSL_VERSION_STRING)
            << std::endl;

  // ---- 服务端:自签证书 + 宣告名单 --------------------------------------
  uvcpp_ssl_context sctx(tls_mode::SERVER, tls_version::TLS_1_2);
  if (!sctx.is_ready() || !sctx.generate_self_signed("loopback.test", 2048)) {
    std::cerr << "  [FAIL] server ctx/cert: " << sctx.get_last_error() << std::endl;
    return 2;
  }
  sctx.set_alpn_select_protos(kServerAlpn);

  // ---- 客户端:一个不带 ALPN 的 context + 一个带 h2 的 -----------------
  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  // 自签证书的回环用例需要它;生产客户端不能这样(默认接受任何证书 = 中间人
  // 可以直接接管)。显式写出来就是要让它显眼。
  cctx.set_verify_mode(tls_verify_mode::NONE);
  if (!cctx.is_ready()) {
    std::cerr << "  [FAIL] client ctx not ready" << std::endl;
    return 2;
  }

  uvcpp_ssl_context cctx_h2(tls_mode::CLIENT, tls_version::TLS_1_2);
  cctx_h2.set_verify_mode(tls_verify_mode::NONE);
  if (!cctx_h2.set_alpn_protos({"h2", "http/1.1"})) {
    std::cerr << "  [FAIL] ctx-level set_alpn_protos failed" << std::endl;
    return 2;
  }

  // =====================================================================
  // 场景 1 / 4:协商出 h2 —— 裸 tcp_server,只判协商结果
  // =====================================================================
  {
    scenario_server srv(&sctx, false);
    if (!srv.ok()) {
      std::cerr << "  [FAIL] tcp server failed to bind/listen" << std::endl;
      return 2;
    }

    // 场景 1:per-connection 宣告名单。
    client_probe p1;
    p1.records_before = record_count(srv.st);
    run_client(srv.port, &cctx, {"h2", "http/1.1"}, "", p1, "per_conn_h2");
    verify_scenario(srv.st, p1, "h2", "h2", false, "per_conn_h2");
    check(srv.st.delivered.load() == 1 && srv.st.hs_done_at_delivery.load() == 1,
          "per_conn_h2: on_connection delivered " +
              std::to_string(srv.st.delivered.load()) + " times, " +
              std::to_string(srv.st.hs_done_at_delivery.load()) + " after handshake");

    // 场景 4:context 上的宣告名单。
    client_probe p4;
    p4.records_before = record_count(srv.st);
    run_client(srv.port, &cctx_h2, {}, "", p4, "ctx_level_h2");
    verify_scenario(srv.st, p4, "h2", "h2", false, "ctx_level_h2");

    // 让两条连接的关闭事件跑完再停,否则登记表里还留着中间态。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    srv.shutdown();
    check(srv.st.final_count.load() == 0,
          "tcp branch: client_count() = " + std::to_string(srv.st.final_count.load()));
  }

  // =====================================================================
  // 场景 2 / 3 / 5 / 6:非 h2 —— 走真正的 http_server,必须跑完一个完整请求
  // =====================================================================
  {
    scenario_server srv(&sctx, true);
    if (!srv.ok()) {
      std::cerr << "  [FAIL] http server failed to bind/listen" << std::endl;
      return 2;
    }
    const std::string req(kRequest, sizeof(kRequest) - 1);

    // 场景 2:只宣告 http/1.1。
    client_probe p2;
    p2.records_before = record_count(srv.st);
    run_client(srv.port, &cctx, {"http/1.1"}, req, p2, "http11_only");
    verify_scenario(srv.st, p2, "http/1.1", "http/1.1", true, "http11_only");

    // 场景 3:一个字节的 ALPN 都不宣告。服务端的选择回调**根本不会被调用**,
    // 钉的是"没有扩展时握手照常成功",也就是老客户端那条路。
    client_probe p3;
    p3.records_before = record_count(srv.st);
    run_client(srv.port, &cctx, {}, req, p3, "no_alpn_extension");
    verify_scenario(srv.st, p3, "", "", true, "no_alpn_extension");

    // 场景 5:context 上有 h2,per-connection 压成 http/1.1 —— 覆盖必须生效。
    client_probe p5;
    p5.records_before = record_count(srv.st);
    run_client(srv.port, &cctx_h2, {"http/1.1"}, req, p5, "per_conn_override");
    verify_scenario(srv.st, p5, "http/1.1", "http/1.1", true,
                    "per_conn_override");

    // 场景 6:**本文件的核心判据。** 客户端只宣告服务端没有的协议。选择回调
    // 会被调用且拿不到交集,必须答 NOACK 而不是把服务端列表的第一项(h2)塞回去。
    // 写错的那版两侧都会读到 "h2"。(不进 tcp 分支是因为这个连接要发真请求。)
    client_probe p6;
    p6.records_before = record_count(srv.st);
    run_client(srv.port, &cctx, {"spdy/3.1"}, req, p6, "no_overlap");
    verify_scenario(srv.st, p6, "", "", true, "no_overlap");
  }

  if (g_failures != 0) {
    std::cerr << "[web_ssl_h2_alpn] " << g_failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "[web_ssl_h2_alpn] all scenarios OK" << std::endl;
  return 0;
}

#else
int main() {
  std::cout << "[web_ssl_h2_alpn] skipped (needs UVCPP_WEB_ENABLE && UVCPP_OPENSSL_ENABLE)"
            << std::endl;
  return 0;
}
#endif
