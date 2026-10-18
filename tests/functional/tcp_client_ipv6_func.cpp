/**
 * @file tests/functional/tcp_client_ipv6_func.cpp
 * @brief `uvcpp_tcp_client` 的 IPv6 回环：数字地址 `::1` 直接连、echo 往返。
 *
 * 这条用例盯的是「数字 IPv6 地址的快速路径」本身（SOCKS5 的 ATYP=IPv6 依赖它），
 * 而不是 SOCKS5 —— 于是它不需要代理、也不会因为代理那条路改动而假装覆盖到。
 *
 * 域名解析仍然只走 `AF_INET`（未改），所以这里**不用**域名：用域名的话
 * `::1` 根本解析不出来，量到的会是"DNS 没给我 IPv6 地址"，与路径无关。
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
#include "handle/uvcpp_tcp.h"
#include "handle/uvcpp_timer.h"
#include "net/uvcpp_tcp_client.h"
#include "req/uvcpp_write.h"
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

/** @brief `::1` 上的 TCP echo 服务端，跑在独立线程里。 */
static void run_echo_server(std::promise<int>& port_promise,
                            std::atomic<bool>& stop_server) {
  uvcpp_loop server_loop;
  uvcpp_test::loop_drain drain(&server_loop);
  server_loop.init();

  uvcpp_tcp server(&server_loop);
  const int brc = server.bindIpv6("::1", 0);
  if (brc != 0) {
    // 机器没有 IPv6 栈：把端口报成 -1，主线程据此跳过而不是假绿。
    port_promise.set_value(-1);
    return;
  }

  struct sockaddr_in6 name;
  std::memset(&name, 0, sizeof(name));
  int namelen = sizeof(name);
  server.getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  port_promise.set_value(ntohs(name.sin6_port));

  server.listen(
      [&](uvcpp_stream* s, int status) {
        if (status < 0) return;
        uvcpp_tcp* peer = new uvcpp_tcp(&server_loop);
        if (s->accept(peer) != 0) {
          delete peer;
          return;
        }
        peer->read_start(
            [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
              uvcpp_buf::alloc_buf(buf, sz > 0 ? sz : 4096);
            },
            [peer](uvcpp_stream* stream, ssize_t nread, const uv_buf_t* buf) {
              if (nread > 0) {
                uvcpp_buf* bufcpp = new uvcpp_buf(buf->base, (size_t)nread);
                uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
                uvcpp_write* w = new uvcpp_write();
                w->set_uv_buf(bufcpp->out_uv_buf(), true);
                stream->write(w, w->get_uv_buf(), 1,
                              [bufcpp](uvcpp_write* wr, int) {
                                delete bufcpp;
                                delete wr;
                              });
              } else if (nread == 0) {
                if (buf->base != nullptr) {
                  uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
                }
              } else {
                if (buf->base != nullptr) {
                  uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
                }
                stream->close([peer](uvcpp_handle*) { delete peer; });
              }
            });
      },
      128);

  // 看门狗 + 停止轮询（与 tcp_client_func.cpp 同一形状）。
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

static void test_ipv6_echo(int port) {
  char msg[64];
  std::memset(msg, 0, sizeof(msg));
  std::strcpy(msg, "ipv6-loopback-echo");

  // 同步路径
  {
    uvcpp_tcp_client client;
    const int crc = client.connect_wait("::1", port, 5000);
    std::printf("  connect_wait(\"::1\") -> %d (%s)\n", crc, uv_strerror(crc));
    CHECK(crc == 0);
    if (crc == 0) {
      CHECK(client.write_wait(msg, std::strlen(msg), 5000) == 0);
      uvcpp_buf out;
      CHECK(client.read_wait(out, 5000) == 0);
      CHECK(out.size() == std::strlen(msg));
      if (out.size() == std::strlen(msg)) {
        CHECK(std::memcmp(out.get_const_data(), msg, out.size()) == 0);
      }
    }
  }

  // 异步路径
  {
    uvcpp_tcp_client client;
    bool done = false;
    int st = -1;
    const int rc = client.connect("::1", port, [&](int s) {
      st = s;
      done = true;
    });
    CHECK(rc == 0);
    while (!done) client.run(UV_RUN_NOWAIT);
    std::printf("  connect(\"::1\") -> %d (%s)\n", st, uv_strerror(st));
    CHECK(st == 0);
  }
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
    // 3 = ctest 的 SKIP_RETURN_CODE（本用例在 CMakeLists 里登记了这一格）
    return 3;
  }

  std::printf("[functional tcp_client_ipv6] port=%d\n", port);
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
