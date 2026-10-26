/**
 * @file tests/functional/udp_server_external_loop_func.cpp
 * @brief issue #59：`uvcpp_udp_server` 用**外部循环**构造时的语义。
 *
 * 背景：`uvcpp_tcp_server` / `uvcpp_quic_server` 都支持"外部循环 + 在既有循环上
 * 工作"，`uvcpp_udp_server` 原先只能自带一条循环 —— 想在 `uvcpp_web_app` 所在的
 * 循环上再收一个 UDP 端点（中继数据面）就做不到，回调会被放到另一条线程上，
 * 跨到循环亲和的 WS/TCP/QUIC 连接上就不安全。
 *
 * 判据：
 *  [1] `uvcpp_udp_server(uvcpp_loop*)` 认下这条循环（`get_loop()` 返回同一个
 *      指针），收发都在它上面；**循环由调用方 `run()`**。
 *  [2] `stop()` 只关本端点的句柄，**不停**外部循环：同一条循环上还能再起第二个
 *      端点，并正常收发。
 *  [3] 析构不关、不删外部循环 —— 这里用的是**栈上的** `uvcpp_loop`：真去
 *      `delete`/`loop_close` 它会当场崩（glibc 的 "free(): invalid pointer" 或
 *      ASAN 的 stack-use-after-scope），不需要额外断言。
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <net/uvcpp_udp_server.h>
#include <uvcpp/uvcpp_buf.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

/** 把一条已 bind 的 UDP 端点上的端口取出来。 */
int bound_port(uvcpp_udp_server& srv) {
  struct sockaddr_in name;
  int namelen = sizeof(name);
  if (srv.get_udp()->getsockname(reinterpret_cast<sockaddr*>(&name),
                                 &namelen) != 0) {
    return -1;
  }
  return ntohs(name.sin_port);
}

/** 裸 UDP socket 发一段；不经过 libuv，所以与循环无关。 */
bool send_raw(int port, const char* text) {
#ifdef _WIN32
  // 进程级 Winsock 初始化一次即可；这个测试只发一两个包，不进 I/O 完成端口。
  static const bool wsa_ok = []() {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  if (!wsa_ok) return false;
  SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd == INVALID_SOCKET) return false;
#else
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
#endif
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  const int len = static_cast<int>(std::strlen(text));
  const int sent =
      static_cast<int>(::sendto(fd, text, len, 0,
                                reinterpret_cast<sockaddr*>(&addr),
                                static_cast<int>(sizeof(addr))));
  const bool ok = sent == len;
#ifdef _WIN32
  ::closesocket(fd);
#else
  ::close(fd);
#endif
  return ok;
}

/** 有界地泵调用方的循环，直到 done() 为真。 */
bool pump_until(uvcpp_loop& loop, const std::function<bool()>& done,
                int rounds = 4000) {
  for (int i = 0; i < rounds; ++i) {
    if (done()) return true;
    loop.run(UV_RUN_NOWAIT);
  }
  return done();
}

void test_external_loop_receives_and_survives_stop() {
  std::printf("[udp_server_external] external_loop\n");

  uvcpp_loop loop;  // 栈上：若被 delete 会当场崩
  std::string seen;
  uvcpp_udp_server server(&loop);

  check(server.get_loop() == &loop, "get_loop() 返回传进去的那条循环");
  check(server.bind("127.0.0.1", 0) == 0, "bind 成功");
  const int port = bound_port(server);
  check(port > 0, "拿到了绑定端口");

  server.recv_start([&seen](uvcpp_buf* buf, const char*, int) {
    seen.assign(buf->get_data(), buf->size());
  });

  check(send_raw(port, "hello-external"), "裸 socket 发出第一包");
  check(pump_until(loop, [&seen] { return seen == "hello-external"; }),
        "回调在调用方的循环上被派发（调用方 run）");

  // stop() 只关本端点；外部循环不许被停掉。
  std::atomic<bool> stopped{false};
  server.stop([&stopped] { stopped.store(true); });
  check(pump_until(loop, [&stopped] { return stopped.load(); }),
        "stop() 的关闭回合同样落在外部循环上");

  // 同一条循环上再起一个端点，仍然能收 —— 证明循环没被 stop()/析构带走。
  std::string seen2;
  uvcpp_udp_server server2(&loop);
  check(server2.bind("127.0.0.1", 0) == 0, "同一循环上第二个端点 bind 成功");
  const int port2 = bound_port(server2);
  server2.recv_start([&seen2](uvcpp_buf* buf, const char*, int) {
    seen2.assign(buf->get_data(), buf->size());
  });
  check(send_raw(port2, "second-endpoint"), "发到第二个端点");
  check(pump_until(loop, [&seen2] { return seen2 == "second-endpoint"; }),
        "第二个端点在同一循环上收到（stop() 没有停外部循环）");

  // 外部模式下 stop_loop() 是空操作：调完之后**第一轮** NOWAIT 就要派发。
  //
  // 判据必须是一轮：`uv_run` 在 `stop_flag` 立着时跳过整段轮询，返回前才把
  // 标志清掉 —— "被停过"只吃掉一轮；给多轮的话停与不停都是绿的，判据空转。
  //
  // 用 0ms 定时器而不是 UDP 包：定时器在 `run` 之前就已到期，第一轮必然派发。
  // 环回 UDP 的包在 macOS 的 kqueue 上不保证第一轮就可见（本用例在 macOS 的
  // db 格子上就是这么红的）。定时器只证明"循环还在跑回调"，与这一条判据正好
  // 同一个命题，不牵扯收发时序。
  bool timer_fired = false;
  uvcpp_timer probe(&loop);
  probe.start([&timer_fired](uvcpp_timer*) { timer_fired = true; }, 0, 0);
  server2.stop_loop();
  loop.run(UV_RUN_NOWAIT);
  check(timer_fired,
        "stop_loop() 在外部模式下是空操作（第一轮 NOWAIT 就派发）");
  probe.stop();
}

}  // namespace

int main() {
  std::printf("[udp_server_external] issue #59\n");
  test_external_loop_receives_and_survives_stop();

  if (g_failures == 0) {
    std::printf("[udp_server_external] ALL PASS\n");
    return 0;
  }
  std::printf("[udp_server_external] FAIL (%d checks failed)\n", g_failures);
  return 2;
}
