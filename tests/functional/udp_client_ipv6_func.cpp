/**
 * @file tests/functional/udp_client_ipv6_func.cpp
 * @brief `uvcpp_udp_client` 的 IPv6 回环：`::1` 收发与 `send_addr` 两种入口。
 *
 * 与 `tcp_client_ipv6_func.cpp` 一对：
 *  - `send(ip, port, ...)` 的分流规则是"含 `:` ⇒ IPv6"，这是**文本入口**；
 *  - `send_addr_wait(sockaddr*, ...)` 是**已解析入口**（SOCKS5 UDP 中继用它），
 *    传进来的是 `sockaddr_in6`，不允许再被转成字符串。
 *
 * 两条各走一次 echo，避免"文本入口对了就当已解析入口也对"。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <uv.h>

#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_udp.h"
#include "handle/uvcpp_timer.h"
#include "net/uvcpp_udp_client.h"
#include "req/uvcpp_udp_send.h"
#include "uvcpp/uvcpp_buf.h"
#include "loop_drain.h"

using namespace uvcpp;

static int g_fails = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %d: %s\n", __LINE__, #cond);                    \
      ++g_fails;                                                        \
    }                                                                   \
  } while (0)

/** @brief `::1` 上的 UDP echo 服务端，跑在独立线程里。 */
static void run_echo_server(std::promise<int>& port_promise,
                            std::atomic<bool>& stop_server) {
  uvcpp_loop server_loop;
  uvcpp_test::loop_drain drain(&server_loop);
  server_loop.init();

  uvcpp_udp udp(&server_loop);
  struct sockaddr_in6 bind_addr;
  std::memset(&bind_addr, 0, sizeof(bind_addr));
  const int arc = uv_ip6_addr("::1", 0, &bind_addr);
  if (arc != 0 || udp.bind(reinterpret_cast<const sockaddr*>(&bind_addr), 0) != 0) {
    port_promise.set_value(-1);
    return;
  }

  struct sockaddr_in6 name;
  std::memset(&name, 0, sizeof(name));
  int namelen = sizeof(name);
  udp.getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  port_promise.set_value(ntohs(name.sin6_port));

  udp.recv_start(
      [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
        uvcpp_buf::alloc_buf(buf, sz > 0 ? sz : 4096);
      },
      [&udp](uvcpp_udp*, ssize_t nread, const uv_buf_t* buf,
             const sockaddr* addr, unsigned) {
        if (nread > 0) {
          uvcpp_buf echo_buf(buf->base, static_cast<size_t>(nread));
          uv_buf_t* raw = echo_buf.out_uv_buf();
          uvcpp_udp_send* req = new uvcpp_udp_send();
          req->set_uv_buf(raw, true);
          udp.send(req, raw, 1, addr,
                   [](uvcpp_udp_send* r, int) { delete r; });
        }
        if (buf->base) uvcpp_free_bytes(buf->base);
      });

  uvcpp_timer watchdog(&server_loop);
  watchdog.start(
      [&server_loop](uvcpp_timer*) { server_loop.stop(); }, 20000, 0);
  uvcpp_timer stop_poller(&server_loop);
  stop_poller.start(
      [&stop_server, &server_loop](uvcpp_timer*) {
        if (stop_server.load()) server_loop.stop();
      },
      100, 100);

  server_loop.run(UV_RUN_DEFAULT);
}

static std::string g_last_payload;
static std::string g_last_ip;

/** @brief 泵客户端循环直到收到一条数据报（或超时）。 */
static bool pump_until_payload(uvcpp_loop* loop, std::size_t expect_len,
                               int timeout_ms) {
  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  while (g_last_payload.size() != expect_len) {
    loop->run(UV_RUN_NOWAIT);
    const long long elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();
    if (elapsed > timeout_ms) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

static void test_ipv6_echo(int port) {
  uvcpp_udp_client client;
  uvcpp_loop* loop = client.get_loop();

  // 客户端绑 `::1`（与 bind() 的 IPv6 分流规则同一处），再用**文本入口**发。
  CHECK(client.bind("::1", 0) == 0);
  CHECK(client.recv_start([](uvcpp_buf* b, const char* ip, int p) {
    (void)p;
    g_last_payload.assign(b->get_const_data(), b->size());
    g_last_ip = (ip != nullptr) ? ip : "";
  }) == 0);

  const char msg[] = "udp-ipv6-echo";
  const size_t msg_len = sizeof(msg) - 1;

  int send_rc = client.send_wait("::1", port, msg, msg_len, 5000);
  std::printf("  send_wait(\"::1\") -> %d (%s)\n", send_rc,
              uv_strerror(send_rc));
  CHECK(send_rc == 0);
  CHECK(pump_until_payload(loop, msg_len, 5000));
  CHECK(g_last_payload == std::string(msg, msg_len));
  // 发送方地址必须按 IPv6 解回文本（AF_INET6 那一条分支）。
  CHECK(g_last_ip == "::1");

  // **已解析入口**：同一台服务端，这次传 `sockaddr_in6` 而不是字符串。
  g_last_payload.clear();
  struct sockaddr_in6 target;
  std::memset(&target, 0, sizeof(target));
  CHECK(uv_ip6_addr("::1", port, &target) == 0);
  const int addr_rc = client.send_addr_wait(
      reinterpret_cast<const sockaddr*>(&target), msg, msg_len, 5000);
  std::printf("  send_addr_wait(sockaddr_in6) -> %d (%s)\n", addr_rc,
              uv_strerror(addr_rc));
  CHECK(addr_rc == 0);
  CHECK(pump_until_payload(loop, msg_len, 5000));

  // `AF_UNSPEC` 不是一个可发送的地址族：早已解析好的入口必须显式拒绝。
  struct sockaddr_in6 unspec;
  std::memset(&unspec, 0, sizeof(unspec));
  unspec.sin6_family = AF_UNSPEC;
  CHECK(client.send_addr_wait(reinterpret_cast<const sockaddr*>(&unspec), msg,
                              msg_len, 100) == UV_EAFNOSUPPORT);
}

int main() {
  std::promise<int> port_promise;
  std::future<int> port_future = port_promise.get_future();
  std::atomic<bool> stop_server(false);

  std::thread server_thread(
      [&]() { run_echo_server(port_promise, stop_server); });

  const int port = port_future.get();
  if (port < 0) {
    stop_server.store(true);
    server_thread.join();
    std::printf("SKIP: 本机没有可用的 IPv6 回环栈\n");
    return 3;
  }

  std::printf("[functional udp_client_ipv6] port=%d\n", port);
  test_ipv6_echo(port);

  stop_server.store(true);
  server_thread.join();

  if (g_fails == 0) {
    std::printf("ALL OK\n");
    return 0;
  }
  std::printf("%d FAILURES\n", g_fails);
  return 1;
}
