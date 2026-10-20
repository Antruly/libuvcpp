/**
 * @file tests/functional/socks5_http_func.cpp
 * @brief `uvcpp_http_client` 经本仓 SOCKS5 服务端访问本仓 TCP/SSL 服务端：
 *        明文 HTTP、SOCKS 之后接 TLS，以及"代理不可达"的反向对照。
 *
 * 这里钉的是**顺序**：SOCKS5 协商（含认证）必须在应用数据之前完成，TLS
 * 握手又必须发生在隧道**里面**。任何把顺序弄反的实现（先 TLS 再 SOCKS、
 * 或者 CONNECT 还没成功就把请求写出去）都会停在这几条判据上。
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_socks5_server.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_ws_client.h>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_context.h>
#endif

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

const char* kBody = "hello-socks";

const char kResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 11\r\n"
    "Connection: close\r\n"
    "\r\n"
    "hello-socks";

// =========================================================================
// SOCKS5 代理
// =========================================================================

struct proxy_state {
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::atomic<int>  sessions{0};
  std::atomic<int>  last_status{0};
  uvcpp_socks5_server_options opts;  // 默认值即"开放代理"，现有场景不受影响
  int               port = -1;
};

void run_proxy(proxy_state& st) {
  uvcpp_socks5_server srv;
  srv.set_options(st.opts);
  srv.bind("127.0.0.1", 0);
  st.port = srv.port();
  srv.listen([&st](const uvcpp_socks5_session_info& info) {
    st.last_status.store(info.status);
    st.sessions.fetch_add(1);
  });
  st.ready.store(true);

  while (!st.stop.load()) {
    srv.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  srv.close();
  for (int i = 0; i < 50; ++i) {
    srv.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

struct proxy_thread {
  proxy_state st;
  std::thread thread;

  proxy_thread() { thread = std::thread(run_proxy, std::ref(st)); }

  int port() {
    while (!st.ready.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return st.port;
  }

  ~proxy_thread() {
    st.stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// =========================================================================
// 目标 HTTP 服务端（可选 TLS）
// =========================================================================

struct target_state {
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::atomic<int>  listen_rc{-1};
  std::atomic<int>  accepted{0};
  std::atomic<int>  plain_bytes{0};
  std::atomic<int>  reply_ok{0};
  std::atomic<int>  reply_fail{0};
  std::mutex        mu;
  std::string       request;
  int               port = -1;
};

// 这一档的 TLS 是可选的：`UVCPP_OPENSSL_ENABLE=0` 的构建里既没有
// `uvcpp_ssl_context`，`uvcpp_tcp_server` 也没有 `set_ssl_context()`。
#if UVCPP_OPENSSL_ENABLE
void run_target(target_state& st, uvcpp_ssl_context* sctx) {
#else
void run_target(target_state& st) {
#endif
  uvcpp_tcp_server server;

  std::string pending;
  server.set_read_callback(
      [&st, &pending](uvcpp_tcp_client& c, const net_read_result& r) {
        if (!r.is_data()) return;
        st.plain_bytes.fetch_add(static_cast<int>(r.size));
        {
          std::lock_guard<std::mutex> lk(st.mu);
          st.request.append(r.data, r.size);
        }
        pending.append(r.data, r.size);
        if (pending.find("\r\n\r\n") == std::string::npos) return;
        pending.clear();
        const int wrc = c.write(kResponse, sizeof(kResponse) - 1,
                                [&st](int status) {
                                  if (status == 0) st.reply_ok.fetch_add(1);
                                  else st.reply_fail.fetch_add(1);
                                });
        if (wrc != 0) st.reply_fail.fetch_add(1);
      });

#if UVCPP_OPENSSL_ENABLE
  if (sctx != nullptr) server.set_ssl_context(sctx);
#endif

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    st.listen_rc.store(rc);
    st.ready.store(true);
    return;
  }

  struct sockaddr_in name;
  int namelen = static_cast<int>(sizeof(name));
  server.get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&name),
                                &namelen);
  st.port = ntohs(name.sin_port);

  rc = server.listen([&st](uvcpp_tcp_client*) { st.accepted.fetch_add(1); },
                     128);
  st.listen_rc.store(rc);
  // 端口必须在 listen() 之后才放行：只 bind 未 listen 的 socket 是拒连的。
  st.ready.store(true);
  if (rc != 0) return;

  uvcpp_loop* loop = server.get_loop();
  while (!st.stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 200; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

struct target_thread {
  target_state      st;
  std::thread       thread;

#if UVCPP_OPENSSL_ENABLE
  explicit target_thread(uvcpp_ssl_context* sctx = nullptr) {
    thread = std::thread(run_target, std::ref(st), sctx);
  }
#else
  target_thread() { thread = std::thread(run_target, std::ref(st)); }
#endif

  int port() {
    while (!st.ready.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return st.port;
  }

  ~target_thread() {
    st.stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// =========================================================================
// 客户端
// =========================================================================

struct probe {
  std::atomic<int>  connect_status{-99};
  std::atomic<bool> connect_fired{false};
  std::atomic<bool> resp_fired{false};
  std::atomic<int>  resp_err{-99};
  std::atomic<int>  send_rc{-99};
  int               status_code = 0;
  std::string       body;
};

uvcpp_loop* loop_of(uvcpp_http_client& c) {
  return c.get_tcp_client()->get_loop();
}

void run_client(uvcpp_http_client& client, uvcpp_loop* loop, probe& p,
                int timeout_ms) {
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  while (!p.connect_fired.load()) {
    loop->run(UV_RUN_NOWAIT);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count() > timeout_ms) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (p.connect_status.load() != 0) return;

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

  const std::chrono::steady_clock::time_point t2 =
      std::chrono::steady_clock::now();
  while (!p.resp_fired.load()) {
    loop->run(UV_RUN_NOWAIT);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t2)
            .count() > timeout_ms) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 关闭舞步收尾，不在半途析构。
  for (int i = 0; i < 100; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void scenario_plain_http() {
  std::cout << "[socks5-http] plain start" << std::endl;

  proxy_thread  proxy;
  target_thread target;
  check(target.port() > 0, "目标服务端 listen 成功（前置）");
  if (target.port() <= 0) return;

  uvcpp_http_client client;
  const int prc = client.set_socks5_proxy("127.0.0.1", proxy.port());
  check(prc == 0, "set_socks5_proxy 返回 " + std::to_string(prc));

  probe p;
  const int crc = client.connect("127.0.0.1", target.port(), [&p](int st) {
    p.connect_status.store(st);
    p.connect_fired.store(true);
  });
  check(crc == 0, "connect 受理（返回 " + std::to_string(crc) + "）");
  run_client(client, loop_of(client), p, 5000);

  check(p.connect_status.load() == 0,
        "经 SOCKS5 的 connect status = " +
            std::to_string(p.connect_status.load()));
  check(p.send_rc.load() == 0,
        "send 返回 " + std::to_string(p.send_rc.load()));
  check(p.resp_fired.load(), "响应回调没有触发");
  check(p.resp_err.load() == 0,
        "响应错误 = " + std::to_string(p.resp_err.load()));
  check(p.status_code == 200,
        "状态码 = " + std::to_string(p.status_code));
  check(p.body == std::string(kBody),
        "响应体 = '" + p.body + "'（期望 '" + kBody + "'）");

  // 代理与会话的非空证明：没有这几条，上面的 200 也可能是一次直连。
  while (proxy.st.sessions.load() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(proxy.st.sessions.load() == 1,
        "代理记录了 1 条会话（实际 " +
            std::to_string(proxy.st.sessions.load()) + "）");
  check(proxy.st.last_status.load() == 0,
        "代理会话状态 = " + std::to_string(proxy.st.last_status.load()));
  check(target.st.accepted.load() == 1,
        "目标服务端接受了 1 条连接（实际 " +
            std::to_string(target.st.accepted.load()) + "）");

  std::string seen;
  {
    std::lock_guard<std::mutex> lk(target.st.mu);
    seen = target.st.request;
  }
  check(seen.find("GET / HTTP/1.1") != std::string::npos,
        "目标服务端收到了完整的请求行");
}

void scenario_proxy_down() {
  std::cout << "[socks5-http] proxy down start" << std::endl;

  target_thread target;
  if (target.port() <= 0) {
    check(false, "目标服务端 listen 成功（前置）");
    return;
  }

  // 拿一个刚刚还开着、现在没人监听的端口当代理。
  int dead_port = 0;
  {
    uvcpp_loop      loop;
    uvcpp_tcp_server server(&loop);
    server.bind("127.0.0.1", 0);
    struct sockaddr_in name;
    int namelen = static_cast<int>(sizeof(name));
    server.get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&name),
                                  &namelen);
    dead_port = ntohs(name.sin_port);
    server.stop();
    for (int i = 0; i < 20; ++i) {
      loop.run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  uvcpp_http_client client;
  check(client.set_socks5_proxy("127.0.0.1", dead_port) == 0,
        "set_socks5_proxy 受理");

  probe p;
  const int crc = client.connect("127.0.0.1", target.port(), [&p](int st) {
    p.connect_status.store(st);
    p.connect_fired.store(true);
  });
  run_client(client, loop_of(client), p, 5000);

  check(p.connect_status.load() != 0,
        "代理不可达时 connect 必须失败（实际 " +
            std::to_string(p.connect_status.load()) + "）");
  check(!client.socks5_last_error().empty(),
        "失败时 socks5_last_error() 有说明");
  check(target.st.accepted.load() == 0,
        "代理不可达时目标服务端一个连接都不该收到（实际 " +
            std::to_string(target.st.accepted.load()) + "）");
}

#if UVCPP_OPENSSL_ENABLE
void scenario_socks_then_tls(uvcpp_ssl_context& sctx,
                             uvcpp_ssl_context& cctx) {
  std::cout << "[socks5-http] socks+tls start" << std::endl;

  proxy_thread  proxy;
  target_thread target(&sctx);
  check(target.port() > 0, "TLS 目标服务端 listen 成功（前置）");
  if (target.port() <= 0) return;

  uvcpp_http_client client;
  client.set_ssl_context(&cctx);
  check(client.set_socks5_proxy("127.0.0.1", proxy.port()) == 0,
        "set_socks5_proxy 受理");

  probe p;
  const int crc = client.connect("127.0.0.1", target.port(), [&p](int st) {
    p.connect_status.store(st);
    p.connect_fired.store(true);
  });
  check(crc == 0, "connect 受理（返回 " + std::to_string(crc) + "）");
  run_client(client, loop_of(client), p, 8000);

  check(p.connect_status.load() == 0,
        "SOCKS+TLS 的 connect status = " +
            std::to_string(p.connect_status.load()));
  check(p.resp_fired.load(), "响应回调没有触发");
  check(p.resp_err.load() == 0,
        "响应错误 = " + std::to_string(p.resp_err.load()));
  check(p.status_code == 200,
        "状态码 = " + std::to_string(p.status_code));
  check(p.body == std::string(kBody),
        "响应体 = '" + p.body + "'（期望 '" + kBody + "'）");

  // TLS 是**在隧道里**跑的：服务端解出来的明文必须就是那个请求。
  std::string seen;
  {
    std::lock_guard<std::mutex> lk(target.st.mu);
    seen = target.st.request;
  }
  check(seen.find("GET / HTTP/1.1") != std::string::npos,
        "TLS 服务端解出了明文请求（plain_bytes=" +
            std::to_string(target.st.plain_bytes.load()) + "）");
  check(target.st.reply_ok.load() >= 1,
        "TLS 服务端把响应写出去了（failures=" +
            std::to_string(target.st.reply_fail.load()) + "）");
  check(proxy.st.sessions.load() == 1,
        "代理记录了 1 条会话（实际 " +
            std::to_string(proxy.st.sessions.load()) + "）");
}
#endif  // UVCPP_OPENSSL_ENABLE

// =========================================================================
// WebSocket 客户端也要能区分 SOCKS5 的失败阶段（与 tcp/http client 同义）
// =========================================================================

int closed_tcp_port() {
  uvcpp_loop loop;
  uvcpp_tcp tcp(&loop);
  struct sockaddr_in bind_addr;
  if (uv_ip4_addr("127.0.0.1", 0, &bind_addr) != 0) return 0;
  if (tcp.bind(reinterpret_cast<const struct sockaddr*>(&bind_addr), 0) != 0) {
    return 0;
  }
  struct sockaddr_in name;
  int namelen = static_cast<int>(sizeof(name));
  if (tcp.getsockname(reinterpret_cast<struct sockaddr*>(&name), &namelen) != 0) {
    return 0;
  }
  return ntohs(name.sin_port);
}

void scenario_ws_proxy_stage() {
  // --- 代理连不上：阶段是"连代理" ---
  {
    const int dead = closed_tcp_port();
    uvcpp_ws_client ws;
    check(ws.set_socks5_proxy("127.0.0.1", dead) == 0,
          "ws: set_socks5_proxy 受理（代理端口关闭）");
    uvcpp_ws_connection* conn = nullptr;
    const int rc = ws.connect_wait("ws://127.0.0.1:9/", conn, 2000);
    check(rc != 0, "ws: 代理不可达时 connect_wait 失败");
    check(ws.socks5_proxy_connect_failed(),
          "ws: 代理不可达被归类为 proxy-connect 失败");
  }

  // --- 代理已应答、认证失败：不能算"连代理" ---
  {
    // `proxy_thread` 在构造时就起线程，改不了 opts；这里先填好再起。
    proxy_state st;
    st.opts.allow_no_auth = false;
    st.opts.allow_username_password = true;
    st.opts.authenticator = [](const std::string& u, const std::string& p) {
      return u == "alice" && p == "secret";
    };
    std::thread th(run_proxy, std::ref(st));
    while (!st.ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    uvcpp_ws_client ws;
    check(ws.set_socks5_proxy("127.0.0.1", st.port, "alice", "wrong") == 0,
          "ws: set_socks5_proxy 受理（错误口令）");
    uvcpp_ws_connection* conn = nullptr;
    const int rc = ws.connect_wait("ws://127.0.0.1:9/", conn, 3000);
    check(rc != 0, "ws: 认证失败时 connect_wait 失败");
    check(!ws.socks5_proxy_connect_failed(),
          "ws: 认证失败不能算 proxy-connect 失败");

    st.stop.store(true);
    th.join();
  }
}

}  // namespace

int main() {
  std::cout << "[socks5-http] start" << std::endl;

  scenario_plain_http();
  scenario_proxy_down();
  scenario_ws_proxy_stage();

#if UVCPP_OPENSSL_ENABLE
  {
    uvcpp_ssl_context sctx(tls_mode::SERVER, tls_version::TLS_1_2);
    uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
    if (!sctx.is_ready() || !sctx.generate_self_signed("loopback.test", 2048) ||
        !cctx.is_ready()) {
      std::cerr << "  [FAIL] 无法准备 TLS 上下文" << std::endl;
      return 2;
    }
    // 自签证书的回环测试必须显式关掉校验；生产默认是校验。
    cctx.set_verify_mode(tls_verify_mode::NONE);
    scenario_socks_then_tls(sctx, cctx);
  }
#endif

  if (g_failures != 0) {
    std::cerr << "[socks5-http] FAILED (" << g_failures << ")" << std::endl;
    return 1;
  }
  std::cout << "[socks5-http] ALL PASSED" << std::endl;
  return 0;
}

#else  // !UVCPP_WEB_ENABLE

int main() {
  std::cout << "[socks5-http] web disabled, skipped" << std::endl;
  return 0;
}

#endif
