/**
 * @file tests/functional/tcp_client_multiaddr_func.cpp
 * @brief 域名解析用 AF_UNSPEC，并把解析出的地址按顺序逐个尝试（#44）。
 *
 * 1.8.1 之前两条解析路径都把 `ai_family` 写死成 `AF_INET`：IPv6-only 的主机
 * 通过域名连不上，双栈主机只会走 A、失败也不回退。这里钉三件事：
 *
 * 1. 服务端只监听 `::1` 时，用 "localhost" 能连上（解析结果里有 AAAA）；
 * 2. 服务端只监听 `127.0.0.1` 时，用 "localhost" 也能连上 —— 如果解析器把
 *    `::1` 排在前面，这一次连接必须靠**下一轮尝试**才成功；
 * 3. 连一个关闭的端口时，返回的是最后一次尝试的真实错误（ECONNREFUSED），
 *    而不是笼统的 `EAI_NONAME`。
 *
 * 异步与同步两条路径各覆盖一次。没有 IPv6 解析结果的机器上第 1 条跳过。
 */

#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <req/uvcpp_getaddrinfo.h>

#include <uv.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace uvcpp;

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what.c_str());
  }
}

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct server_state {
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::atomic<int>  accepted{0};
  int               port = 0;
  bool              ipv6 = false;
};

void run_server(server_state& st) {
  uvcpp_tcp_server srv;
  srv.set_read_callback([](uvcpp_tcp_client&, const net_read_result&) {});
  int rc = st.ipv6 ? srv.bindIpv6("::1", 0) : srv.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    st.ready.store(true);
    st.port = -1;
    return;
  }
  if (st.ipv6) {
    struct sockaddr_in6 name;
    int namelen = static_cast<int>(sizeof(name));
    srv.get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&name),
                              &namelen);
    st.port = ntohs(name.sin6_port);
  } else {
    struct sockaddr_in name;
    int namelen = static_cast<int>(sizeof(name));
    srv.get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&name),
                              &namelen);
    st.port = ntohs(name.sin_port);
  }
  rc = srv.listen(
      [&st](uvcpp_tcp_client*) { st.accepted.fetch_add(1); }, 16);
  st.ready.store(true);
  if (rc != 0) {
    st.port = -1;
    return;
  }
  uvcpp_loop* loop = srv.get_loop();
  while (!st.stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 200; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

struct server_thread {
  server_state st;
  std::thread  thread;
  explicit server_thread(bool ipv6) {
    st.ipv6 = ipv6;
    thread = std::thread(run_server, std::ref(st));
  }
  int port() {
    while (!st.ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return st.port;
  }
  ~server_thread() {
    st.stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

bool hostname_has_family(const char* host, int family) {
  uvcpp_loop loop;
  uvcpp_getaddrinfo resolver;
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  bool done = false;
  bool has6 = false;
  resolver.getaddrinfo(
      &loop, host, "0", &hints,
      [&done, &has6, family](uvcpp_getaddrinfo* req, int status, struct addrinfo* res) {
        if (status == 0 && res != nullptr) {
          for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
            if (p->ai_family == family) has6 = true;
          }
        }
        if (res != nullptr) req->freeaddrinfo(res);
        done = true;
      });
  const long long deadline = now_ms() + 5000;
  while (!done && now_ms() < deadline) {
    loop.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return has6;
}

int closed_port() {
  // 绑一个临时监听套接字拿端口，随即放开（析构即关闭）。
  uvcpp_tcp_server srv;
  if (srv.bindIpv4("127.0.0.1", 0) != 0) return 0;
  struct sockaddr_in name;
  int namelen = static_cast<int>(sizeof(name));
  srv.get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&name),
                            &namelen);
  const int port = ntohs(name.sin_port);
  return port;  // srv 析构即关闭监听
}

void test_sync_ipv4_server_by_hostname() {
  server_thread srv(false);
  const int port = srv.port();
  if (port <= 0) {
    check(false, "sync/v4: server listening");
    return;
  }
  uvcpp_tcp_client client;
  const int rc = client.connect_wait("localhost", port, 4000);
  check(rc == 0, "sync/v4: 'localhost' 连到只监听 127.0.0.1 的服务端（rc=" +
                     std::to_string(rc) + "）");
  client.close();
}

void test_sync_ipv6_server_by_hostname(const char* host6) {
  if (host6 == nullptr) {
    std::printf("skip: sync/v6（本机没有只解析到 IPv6 的主机名）\n");
    return;
  }
  server_thread srv(true);
  const int port = srv.port();
  if (port <= 0) {
    std::printf("skip: sync/v6（本机不能绑 ::1）\n");
    return;
  }
  uvcpp_tcp_client client;
  const int rc = client.connect_wait(host6, port, 4000);
  check(rc == 0, std::string("sync/v6: '") + host6 +
                     "' 连到只监听 ::1 的服务端（rc=" + std::to_string(rc) +
                     "）");
  client.close();
}

void test_async_ipv6_server_by_hostname(const char* host6) {
  if (host6 == nullptr) {
    std::printf("skip: async/v6（本机没有只解析到 IPv6 的主机名）\n");
    return;
  }
  server_thread srv(true);
  const int port = srv.port();
  if (port <= 0) {
    std::printf("skip: async/v6（本机不能绑 ::1）\n");
    return;
  }
  uvcpp_tcp_client client;
  std::atomic<int>  status{-999};
  std::atomic<bool> done{false};
  const int start = client.connect(host6, port, [&](int st) {
    status.store(st);
    done.store(true);
  });
  check(start == 0, "async/v6: connect 受理");
  const long long deadline = now_ms() + 5000;
  while (!done.load() && now_ms() < deadline) {
    client.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(done.load(), "async/v6: 回调落地");
  check(status.load() == 0,
        "async/v6: 连接成功（status=" + std::to_string(status.load()) + "）");
  client.close();
}

void test_failure_returns_last_error() {
  const int dead = closed_port();
  if (dead <= 0) {
    check(false, "err: 拿不到关闭端口");
    return;
  }
  uvcpp_tcp_client client;
  const long long t0 = now_ms();
  const int rc = client.connect_wait("localhost", dead, 4000);
  const long long elapsed = now_ms() - t0;
  check(rc == UV_ECONNREFUSED,
        "err: 返回最后一次尝试的真实错误 ECONNREFUSED（rc=" +
            std::to_string(rc) + "）");
  check(elapsed < 4000, "err: 不会因为逐个尝试而拖到总超时");
}

// 只有双栈主机名才能让"第一个地址失败、第二个成功"真正发生。
void test_dual_stack_fallback(bool dual_stack) {
  if (!dual_stack) {
    std::printf("skip: fallback（localhost 不是双栈）\n");
    return;
  }
  server_thread srv(false);  // 只监听 127.0.0.1
  const int port = srv.port();
  if (port <= 0) {
    check(false, "fallback: server listening");
    return;
  }
  uvcpp_tcp_client client;
  const int rc = client.connect_wait("localhost", port, 4000);
  check(rc == 0, "fallback: 双栈主机名回退到 IPv4（rc=" + std::to_string(rc) + "）");
  client.close();
}

void test_async_failure_and_cleanup() {
  const int dead = closed_port();
  if (dead <= 0) {
    check(false, "async/err: 拿不到关闭端口");
    return;
  }
  uvcpp_tcp_client client;
  std::atomic<int>  status{-999};
  std::atomic<bool> done{false};
  const int start = client.connect("localhost", dead, [&](int st) {
    status.store(st);
    done.store(true);
  });
  check(start == 0, "async/err: connect 受理");
  const long long deadline = now_ms() + 5000;
  while (!done.load() && now_ms() < deadline) {
    client.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(done.load(), "async/err: 回调落地");
  check(status.load() == UV_ECONNREFUSED,
        "async/err: 最后一次尝试的真实错误（status=" +
            std::to_string(status.load()) + "）");
  client.close();
  // 析构必须在多地址的连接/失败之后仍然干净（async 句柄与临时 tcp 句柄）。
}

}  // namespace

int main() {
  // 找一个只解析出 IPv6 的主机名：`localhost` 在多数系统上有 AAAA，Debian
  // 系把 `ip6-localhost` 单独指到 ::1。都没有时跳过 v6 两条。
  const char* host6 = nullptr;
  if (hostname_has_family("localhost", AF_INET6)) {
    host6 = "localhost";
  } else if (hostname_has_family("ip6-localhost", AF_INET6)) {
    host6 = "ip6-localhost";
  }
  const bool dual_stack =
      hostname_has_family("localhost", AF_INET6) &&
      hostname_has_family("localhost", AF_INET);
  std::printf("IPv6-only hostname: %s\n", host6 != nullptr ? host6 : "(none)");

  test_sync_ipv4_server_by_hostname();
  test_sync_ipv6_server_by_hostname(host6);
  test_async_ipv6_server_by_hostname(host6);
  test_dual_stack_fallback(dual_stack);
  test_failure_returns_last_error();
  test_async_failure_and_cleanup();

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
