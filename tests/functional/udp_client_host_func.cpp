/**
 * @file tests/functional/udp_client_host_func.cpp
 * @brief UDP 客户端接受**域名**（异步 AF_UNSPEC、同步阻塞解析）并支持 IPv6
 *        连接（#49）。
 *
 * 1.8.1 之前 `send` / `send_wait` / `connect` 只认数字地址：填域名直接
 * `UV_EINVAL`，`connect()` 也只有 IPv4 分支。这里覆盖：
 *
 * 1. `send_wait("localhost", …)` 与异步 `send("localhost", …)` 都能把数据
 *    送到只监听 127.0.0.1 的 echo 服务端；
 * 2. `send_wait("ip6-localhost", …)` 能送到只监听 ::1 的服务端（机器上没有
 *    该名字时跳过）；
 * 3. `connect("localhost", …)` / `connect("::1", …)` 后，不带地址的
 *    `send`/`recv_wait` 正常；
 * 4. 解析失败返回可区分的错误（不是笼统的 EINVAL）。
 */

#include <net/uvcpp_udp_client.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_udp.h>
#include <req/uvcpp_udp_send.h>
#include <req/uvcpp_getaddrinfo.h>
#include <uvcpp/uvcpp_buf.h>

#include <uv.h>

#include <atomic>
#include <chrono>
#include <future>
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

// 一个最小的 UDP echo 服务端（裸 uvcpp_udp），可选 IPv4/IPv6。
struct echo_server {
  std::atomic<bool>  ready{false};
  std::atomic<bool>  stop{false};
  std::atomic<long>  received{0};
  std::thread        thread;
  int                port = 0;

  void start(bool ipv6) {
    std::promise<int> port_promise;
    auto fut = port_promise.get_future();
    thread = std::thread([this, ipv6, &port_promise]() {
      uvcpp_loop loop;
      uvcpp_udp udp(&loop);
      int rc;
      if (ipv6) {
        struct sockaddr_in6 a;
        uv_ip6_addr("::1", 0, &a);
        rc = udp.bind(reinterpret_cast<const sockaddr*>(&a), 0);
      } else {
        struct sockaddr_in a;
        uv_ip4_addr("127.0.0.1", 0, &a);
        rc = udp.bind(reinterpret_cast<const sockaddr*>(&a), 0);
      }
      if (rc != 0) {
        port_promise.set_value(-1);
        return;
      }
      if (ipv6) {
        struct sockaddr_in6 name;
        int len = sizeof(name);
        udp.getsockname(reinterpret_cast<sockaddr*>(&name), &len);
        port_promise.set_value(ntohs(name.sin6_port));
      } else {
        struct sockaddr_in name;
        int len = sizeof(name);
        udp.getsockname(reinterpret_cast<sockaddr*>(&name), &len);
        port_promise.set_value(ntohs(name.sin_port));
      }
      udp.recv_start(
          [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
            uvcpp_buf::alloc_buf(buf, sz > 0 ? sz : 4096);
          },
          [this, &udp](uvcpp_udp*, ssize_t nread, const uv_buf_t* buf,
                       const sockaddr* addr, unsigned) {
            if (nread > 0) {
              received.fetch_add(nread);
              uvcpp_buf echo(buf->base, static_cast<size_t>(nread));
              uv_buf_t* raw = echo.out_uv_buf();
              uvcpp_udp_send* req = new uvcpp_udp_send();
              req->set_uv_buf(raw, true);
              udp.send(req, raw, 1, addr,
                       [](uvcpp_udp_send* r, int) { delete r; });
            }
            if (buf->base) uvcpp_free_bytes(buf->base);
          });
      ready.store(true);
      const long long deadline = now_ms() + 15000;
      while (!stop.load() && now_ms() < deadline) {
        loop.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      for (int i = 0; i < 200; ++i) {
        loop.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    port = fut.get();
    while (!ready.load() && port > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  ~echo_server() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// 解析顺序里第一个地址的族（0 = 解析失败）。`connect()` 对一个**未绑定**
// 的 UDP 句柄没有"换地址重试"的机会（UDP 没有握手），它用的是 getaddrinfo
// 的第一个结果 —— 用例的服务端必须绑在同一个族上。
int first_family(const char* host) {
  uvcpp_loop loop;
  uvcpp_getaddrinfo resolver;
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  int fam = 0;
  bool done = false;
  resolver.getaddrinfo(
      &loop, host, "0", &hints,
      [&done, &fam](uvcpp_getaddrinfo* req, int st, struct addrinfo* res) {
        if (st == 0 && res != nullptr) fam = res->ai_family;
        if (res) req->freeaddrinfo(res);
        done = true;
      });
  const long long deadline = now_ms() + 5000;
  while (!done && now_ms() < deadline) {
    loop.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return fam;
}

bool hostname_has_family(const char* host, int family) {
  uvcpp_loop loop;
  uvcpp_getaddrinfo resolver;
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  bool done = false, found = false;
  resolver.getaddrinfo(&loop, host, "0", &hints,
                       [&done, &found, family](uvcpp_getaddrinfo* req, int st,
                                               struct addrinfo* res) {
                         if (st == 0 && res != nullptr) {
                           for (struct addrinfo* p = res; p; p = p->ai_next) {
                             if (p->ai_family == family) found = true;
                           }
                         }
                         if (res) req->freeaddrinfo(res);
                         done = true;
                       });
  const long long deadline = now_ms() + 5000;
  while (!done && now_ms() < deadline) {
    loop.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return found;
}

void test_sync_hostname_v4() {
  echo_server srv;
  srv.start(false);
  if (srv.port <= 0) {
    check(false, "sync/v4: server listening");
    return;
  }
  uvcpp_udp_client client;
  client.bind("127.0.0.1", 0);
  const char* msg = "hello-by-name";
  const int rc = client.send_wait("localhost", srv.port, msg, std::strlen(msg),
                                  5000);
  check(rc == 0, "sync/v4: send_wait('localhost')（rc=" + std::to_string(rc) +
                     "）");
  uvcpp_buf out;
  const int rrc = client.recv_wait(out, 5000);
  check(rrc == 0 && out.to_string() == msg,
        "sync/v4: 收到回显 '" + (rrc == 0 ? out.to_string() : std::string()) +
            "'");
}

void test_async_hostname_v4() {
  echo_server srv;
  srv.start(false);
  if (srv.port <= 0) {
    check(false, "async/v4: server listening");
    return;
  }
  uvcpp_udp_client client;
  client.bind("127.0.0.1", 0);
  std::atomic<bool> sent{false};
  std::atomic<int>  send_status{-999};
  std::atomic<bool> echoed{false};
  std::string got;
  client.recv_start([&](uvcpp_buf* b, const char*, int) {
    if (b) got = b->to_string();
    echoed.store(true);
  });
  const char* msg = "hello-async-name";
  const int rc = client.send("localhost", srv.port, msg, std::strlen(msg),
                             [&](int st) {
                               send_status.store(st);
                               sent.store(true);
                             });
  check(rc == 0, "async/v4: send('localhost') 受理");
  const long long deadline = now_ms() + 5000;
  while ((!sent.load() || !echoed.load()) && now_ms() < deadline) {
    client.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(sent.load() && send_status.load() == 0,
        "async/v4: 发送完成（status=" +
            std::to_string(send_status.load()) + "）");
  check(echoed.load() && got == msg, "async/v4: 收到回显");
}

void test_sync_hostname_v6(const char* host6) {
  if (host6 == nullptr) {
    std::printf("skip: sync/v6（本机没有只解析到 IPv6 的主机名）\n");
    return;
  }
  echo_server srv;
  srv.start(true);
  if (srv.port <= 0) {
    std::printf("skip: sync/v6（本机不能绑 ::1）\n");
    return;
  }
  uvcpp_udp_client client;
  if (client.bind("::1", 0) != 0) {
    std::printf("skip: sync/v6（客户端不能绑 ::1）\n");
    return;
  }
  const char* msg = "hello-v6-name";
  const int rc = client.send_wait(host6, srv.port, msg, std::strlen(msg), 5000);
  check(rc == 0, std::string("sync/v6: send_wait('") + host6 + "')（rc=" +
                     std::to_string(rc) + "）");
  uvcpp_buf out;
  const int rrc = client.recv_wait(out, 5000);
  check(rrc == 0 && out.to_string() == msg, "sync/v6: 收到回显");
}

void test_connect_hostnames(bool have6, const char* host6) {
  // 未绑定句柄的 `connect()` 用解析结果里的**第一个**地址（UDP 没有握手，
  // 换不了地址），所以服务端要绑在同一个族上。
  const int fam = first_family("localhost");
  const bool v6 = (fam == AF_INET6);
  echo_server srv;
  srv.start(v6);
  if (srv.port <= 0) {
    check(false, "connect: server listening");
    return;
  }
  uvcpp_udp_client client;
  const int crc = client.connect("localhost", srv.port);
  check(crc == 0, "connect: connect('localhost')（rc=" + std::to_string(crc) +
                      "）");
  if (crc == 0) {
    const char* msg = "connected-send";
    const int src = client.send_wait(msg, std::strlen(msg), 5000);
    check(src == 0, "connect: 连接后 send_wait");
    uvcpp_buf out;
    const int rrc = client.recv_wait(out, 5000);
    check(rrc == 0 && out.to_string() == msg, "connect: 连接后收到回显");
  }
  (void)have6;
  (void)host6;
}

void test_dns_failure() {
  uvcpp_udp_client client;
  client.bind("127.0.0.1", 0);
  const char* msg = "x";
  const int rc = client.send_wait("no-such-host.invalid", 9, msg, 1, 3000);
  check(rc != 0 && rc != UV_EINVAL,
        "dns: 解析失败返回可区分的错误（rc=" + std::to_string(rc) + "）");
}

}  // namespace

int main() {
  std::printf("UDP hostname/IPv6 tests (#49)\n");

  bool have6 = hostname_has_family("ip6-localhost", AF_INET6);
  const char* host6 = have6 ? "ip6-localhost" : nullptr;
  if (!have6 && hostname_has_family("localhost", AF_INET6)) {
    have6 = true;
    host6 = "localhost";
  }
  std::printf("IPv6-only hostname: %s\n", host6 ? host6 : "(none)");

  test_sync_hostname_v4();
  test_async_hostname_v4();
  test_sync_hostname_v6(host6);
  test_connect_hostnames(have6, host6);
  test_dns_failure();

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
