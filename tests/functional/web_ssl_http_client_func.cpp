/**
 * @file tests/functional/web_ssl_http_client_func.cpp
 * @brief `uvcpp_http_client` 的**异步 HTTPS**：真 TLS、真 HTTP 往返。
 *
 * 这个文件守的是 `uvcpp_http_client.h` 里写下的那句话：
 *
 * > After calling this, connect() will perform a TLS handshake
 * > and **all traffic will be encrypted**.
 *
 * 原先它只对了前半句。`connect()` 确实握了手，然后 `send()` 把**明文请求**
 * 写进这条已经加密的 socket —— 对端按畸形记录丢掉。没有崩溃、没有错误码、
 * 没有日志，只有"响应永远不来"。所以这里的判据只能是**端到端拿到 200 和
 * 响应体**：任何半截实现（握了手但没加密、加了密但没解回明文）都停在这一条上。
 *
 * 另外两条对照是必要的，缺一条就说明不了"加密真的生效"：
 *   - 服务端**解出来的明文**必须等于客户端发的那个请求（证明搬运它的是 TLS，
 *     而不是"客户端根本没加密、服务端根本没解"）；
 *   - 明文客户端打同一个 TLS 端口必须**拿不到**响应体，而且服务端的明文计数
 *     一个字节都不许涨（证明这个端口真的只认 TLS —— 否则"端到端通了"这条
 *     对"两端都在发明文"的实现同样成立）。
 *
 * 服务端用真正的 `uvcpp_tcp_server::set_ssl_context()`（不是 `on_connection`
 * 里手动 `enable_tls`），这样客户端这条路径面对的是框架的正常服务端形态。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_OPENSSL_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>

#include <openssl/ssl.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

const char* kBody = "hello-tls";

/// 服务端固定回这一个响应。`Content-Length` 与正文必须一致 —— 客户端按它
/// 判断响应结束，多一个字节就是"永远等不到完整响应"。
const char kResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 9\r\n"
    "Connection: close\r\n"
    "\r\n"
    "hello-tls";

// =========================================================================
// 服务端
// =========================================================================

struct server_state {
  std::atomic<int> listen_rc{-1};
  std::atomic<int> accepted{0};
  /// @brief 解出来的**明文**字节数。反向对照靠它做非空证明。
  std::atomic<int> plain_bytes_in{0};
  std::atomic<int> reply_ok{0};
  std::atomic<int> reply_fail{0};
  std::mutex       mu;
  std::string      request;  ///< 解出来的全部明文
};

void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                server_state& st, uvcpp_ssl_context* sctx) {
  uvcpp_tcp_server server;

  // 只有服务端线程碰它（回调都在自己的 loop 上跑）。
  std::string pending;

  server.set_read_callback(
      [&st, &pending](uvcpp_tcp_client& c, const net_read_result& r) {
        if (!r.is_data()) return;
        st.plain_bytes_in.fetch_add(static_cast<int>(r.size));
        {
          std::lock_guard<std::mutex> lk(st.mu);
          st.request.append(r.data, r.size);
        }
        pending.append(r.data, r.size);
        // 请求头收全了就回。这里刻意**不**解析 —— 这个文件考的是客户端
        // 有没有把明文送进来，服务端的 HTTP 解析能力由别的用例负责。
        if (pending.find("\r\n\r\n") != std::string::npos) {
          pending.clear();
          const int wrc = c.write(kResponse, sizeof(kResponse) - 1,
                                  [&st](int status) {
                                    if (status == 0) st.reply_ok.fetch_add(1);
                                    else st.reply_fail.fetch_add(1);
                                  });
          if (wrc != 0) st.reply_fail.fetch_add(1);
        }
      });

  server.set_ssl_context(sctx);  // 必须在 listen() 之前

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int bound_port = ntohs(name.sin_port);

  rc = server.listen([&st](uvcpp_tcp_client*) { st.accepted.fetch_add(1); }, 128);
  st.listen_rc.store(rc);
  // 端口必须在 `listen()` **之后**才放行：已 bind、尚未 listen 的 socket 是
  // 拒连的，客户端会拿到 ECONNREFUSED 而不是"服务端起来了"。
  port_promise.set_value(rc != 0 ? -1 : bound_port);
  if (rc != 0) return;

  uvcpp_loop* loop = server.get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 让挂起的关闭事件跑完，再让 server 析构。
  for (int i = 0; i < 400; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// =========================================================================
// 客户端
// =========================================================================

struct client_probe {
  std::atomic<int>  connect_status{-99};
  std::atomic<bool> connect_fired{false};
  std::atomic<bool> resp_fired{false};
  std::atomic<int>  resp_err{-99};
  std::atomic<int>  send_rc{-99};
  /// @brief 非原子成员只在测试线程上读写 —— 回调是 `loop->run()` 在**本线程
  /// 的栈上**跑出来的，没有第二个线程碰它们。
  int         status_code = 0;
  std::string body;
  std::string bytes_seen;  ///< 客户端收到的全部原始字节（给反向对照看）
};

/**
 * @brief 跑一遍 `uvcpp_http_client` 的异步 connect + get。
 *
 * `use_tls` 为假时**不调** `set_ssl_context()` —— 那就是"明文客户端打 TLS
 * 端口"的反向对照，走的是完全相同的代码路径。
 */
void run_http_client(int port, uvcpp_ssl_context* cctx, client_probe& p,
                     bool use_tls, int resp_timeout_ms) {
  uvcpp_http_client client;
  if (use_tls) client.set_ssl_context(cctx);

  const int crc = client.connect("127.0.0.1", port, [&p](int st) {
    p.connect_status.store(st);
    p.connect_fired.store(true);
  });
  if (crc != 0) {
    p.connect_status.store(crc);
    p.connect_fired.store(true);
    return;
  }

  auto pump_until = [&](const std::atomic<bool>& flag, int ms) {
    for (int i = 0; i < ms && !flag.load(); ++i) {
      client.run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load();
  };

  // 连接 + 握手。TLS 下 `connect` 回调被压到握手完成之后才发 —— 这就是
  // "回调到了就能直接发应用数据"这条契约在客户端要成立的原因。
  pump_until(p.connect_fired, 3000);
  if (p.connect_status.load() != 0) {
    // 收尾：让可能挂起的关闭事件跑完再析构。
    for (int i = 0; i < 100; ++i) {
      client.run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return;
  }

  p.send_rc.store(client.get("/", [&p](const uvcpp_http_response& r, int err) {
    p.resp_err.store(err);
    if (err == 0) {
      p.status_code = static_cast<int>(r.status_code);
      if (r.body.size() > 0) {
        p.body.assign(r.body.get_const_data(), r.body.size());
      }
    }
    p.resp_fired.store(true);
  }));

  pump_until(p.resp_fired, resp_timeout_ms);

  // 关闭舞步收尾 —— 不在半途析构。
  for (int i = 0; i < 200; ++i) {
    client.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace

int main() {
  std::cout << "[web_ssl_http_client] OpenSSL "
            << OpenSSL_version(OPENSSL_VERSION_STRING) << std::endl;

  uvcpp_ssl_context sctx(tls_mode::SERVER, tls_version::TLS_1_2);
  if (!sctx.is_ready() || !sctx.generate_self_signed("loopback.test", 2048)) {
    std::cerr << "  [FAIL] server ctx: " << sctx.get_last_error() << std::endl;
    return 2;
  }

  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  if (!cctx.is_ready()) {
    std::cerr << "  [FAIL] client ctx not ready" << std::endl;
    return 2;
  }
  // 自签证书的回环测试必须显式关掉校验。**生产客户端不能这样** ——
  // 默认值就是校验（见 uvcpp_ssl_context::set_default_verify）。
  cctx.set_verify_mode(tls_verify_mode::NONE);

  server_state      st;
  std::atomic<bool> stop{false};
  std::promise<int> port_promise;
  std::future<int>  port_future = port_promise.get_future();

  std::thread srv_thread(run_server, std::ref(port_promise), std::ref(stop),
                         std::ref(st), &sctx);

  const int port = port_future.get();
  if (port <= 0) {
    std::cerr << "  [FAIL] server failed to bind/listen (rc=" << st.listen_rc.load()
              << ")" << std::endl;
    stop.store(true);
    srv_thread.join();
    return 2;
  }
  std::cout << "[web_ssl_http_client] server port " << port << std::endl;

  // ---- 场景 1：HTTPS 异步请求（本文件的核心） ------------------------
  //
  // 判据是**端到端**的：状态行 + 响应体。缺陷 #2 的实现在这里拿到的是
  // "connect 成功、send 返回 0、回调永远不来" —— 也就是下面三条全挂。
  {
    client_probe p;
    run_http_client(port, &cctx, p, /*use_tls=*/true, 5000);

    check(p.connect_status.load() == 0,
          "https: connect status = " + std::to_string(p.connect_status.load()));
    check(p.send_rc.load() == 0,
          "https: send returned " + std::to_string(p.send_rc.load()));
    check(p.resp_fired.load(), "https: response callback never fired");
    check(p.resp_err.load() == 0,
          "https: response error = " + std::to_string(p.resp_err.load()));
    check(p.status_code == 200,
          "https: status code = " + std::to_string(p.status_code));
    check(p.body == std::string(kBody),
          "https: body = '" + p.body + "' (want '" + kBody + "')");

    // 服务端侧：它解出来的明文必须就是客户端发的那份 —— 这一条把
    // "客户端根本没加密"排除掉（那种情况下服务端一个字节都解不出来）。
    std::string seen;
    {
      std::lock_guard<std::mutex> lk(st.mu);
      seen = st.request;
    }
    check(seen.find("GET / HTTP/1.1") != std::string::npos,
          "https: server never decrypted a well-formed request (got " +
              std::to_string(st.plain_bytes_in.load()) + " plain bytes)");
    check(st.reply_ok.load() >= 1,
          "https: server reply write failed (" +
              std::to_string(st.reply_fail.load()) + " failures)");
  }

  // ---- 场景 2：反向对照 —— 明文客户端打同一个 TLS 端口 ----------------
  //
  // 这条是场景 1 的**前提**：如果这个端口对明文也照样回 200，那"HTTPS 通了"
  // 就说明不了任何事 —— 两端都在发明文也能通过。
  {
    const int in_before = st.plain_bytes_in.load();
    const int req_before = static_cast<int>(st.request.size());

    client_probe p;
    run_http_client(port, nullptr, p, /*use_tls=*/false, 1500);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // TCP 层当然连得上 —— TLS 是它上面的一层。所以这里**不要求** connect
    // 失败；要的是"这条连接换了明文进去，什么也换不回来"。把 connect 成功
    // 断言掉，下面那条"没拿到响应体"才不是"连都没连上"的空断言。
    check(p.connect_status.load() == 0,
          "plaintext_vs_tls: TCP connect status = " +
              std::to_string(p.connect_status.load()));
    check(p.body != std::string(kBody),
          "plaintext_vs_tls: a PLAINTEXT client got the HTTPS response body — "
          "the server is not really requiring TLS");
    // 非空证明：服务端**没有**从这条连接解出任何明文。计数器不涨就说明
    // 那些字节确实被 TLS 层挡住了，而不是"解析器没认出来"。
    check(st.plain_bytes_in.load() == in_before,
          "plaintext_vs_tls: server accepted " +
              std::to_string(st.plain_bytes_in.load() - in_before) +
              " bytes of PLAINTEXT as data");
    check(static_cast<int>(st.request.size()) == req_before,
          "plaintext_vs_tls: server request buffer grew by " +
              std::to_string(static_cast<int>(st.request.size()) - req_before));
  }

  stop.store(true);
  srv_thread.join();

  // **恰好一条**被交付给上层，而且必须是 TLS 那条。
  //
  // `on_connection` 是握手完成之后才调的（见 web_ssl_server_func.cpp 钉住的
  // 那条约定），所以明文那条连接**一次都不该**走到这里 —— 一个"accept 就交付"
  // 的实现会让这个计数变成 2，同时留一条没人管的半开连接。
  check(st.accepted.load() == 1,
        "server delivered " + std::to_string(st.accepted.load()) +
            " connections to the upper layer (want exactly 1: the TLS one)");

  if (g_failures == 0) {
    std::cout << "[web_ssl_http_client] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[web_ssl_http_client] FAIL (" << g_failures << ")" << std::endl;
  return 2;
}

#else
int main() {
  std::cout << "[web_ssl_http_client] SKIP (SSL disabled)" << std::endl;
  return 0;
}
#endif
