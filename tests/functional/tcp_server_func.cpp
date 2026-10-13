#include <iostream>
#include <thread>
#include <future>
#include <atomic>
#include <cstring>
#include <string>
#include <uv.h>
#if !defined(_WIN32)
// `TCP_NODELAY` / `IPPROTO_TCP` 在 `<netinet/tcp.h>` 里，`socklen_t` 在
// `<sys/socket.h>` 里 —— libuv 的公开头只带进后者，前者得自己拉。
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "uvcpp/uvcpp_buf.h"
#include "net/uvcpp_tcp_client.h"
#include "net/uvcpp_tcp_server.h"

using namespace uvcpp;

// =========================================================================
// Helper: 从**套接字**上把 `TCP_NODELAY` 读回来。
//
// 判据必须落在这里，不能落在 `server.tcp_nodelay()` 上：那个访问器读的是我们
// 自己存的那个 bool，它绿只说明"存进去了"，说明不了"这个选项真到了 fd 上"。
// 而这条缺陷（两段式应答被 Nagle 压住 ~40 ms）恰恰是"以为设了、其实没设"。
//
// 取 fd 必须走 `fileno()`：`uv_tcp_t::socket` 这个成员只在 Windows 上存在
// （`uv/win.h`），POSIX 上直接写它编译不过（同 `tcp_read_func.cpp` 里的注记）。
// =========================================================================
static bool read_tcp_nodelay(uvcpp_tcp_client* client, int* out) {
  if (client == nullptr || out == nullptr) return false;
  if (client->get_tcp() == nullptr) return false;

  uv_os_sock_t sock = (uv_os_sock_t)-1;
  if (client->get_tcp()->fileno(sock) != 0) return false;
  if (sock == (uv_os_sock_t)-1) return false;

  // **零初始化，而且只看"零 / 非零"**。两件事逼出来这个写法：
  //
  //  - Windows 的 `getsockopt(TCP_NODELAY)` **只写一个字节**（`BOOL`），高三个
  //    字节留着不动。初始化成 -1（0xFFFFFFFF）就会读回 -255（开）/ -256（关）：
  //    看着像第三个取值，其实只是那个 -1 的残渣（MinGW 上量到的就是这个）。
  //  - Darwin 读回来的是内核内部的 `TF_NODELAY` 标志位，值 **4**（macOS 上量到
  //    的就是这个）。POSIX 从没承诺 `getsockopt` 会把你 `setsockopt` 存进去的
  //    那个数原样还给你 —— 它只保证"开"与"关"可分。
  //
  // 判据因此是 `(v != 0) == want`，而"读没读到"由 `-1` 这个哨兵单独表达
  // （调用方 `wait_observed_nodelay` 就是靠它等的）。
  int v = 0;
#if defined(_WIN32)
  int len = static_cast<int>(sizeof(v));
  if (::getsockopt((SOCKET)sock, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<char*>(&v), &len) != 0) {
    return false;
  }
#else
  socklen_t len = static_cast<socklen_t>(sizeof(v));
  if (::getsockopt(static_cast<int>(sock), IPPROTO_TCP, TCP_NODELAY, &v, &len) !=
      0) {
    return false;
  }
#endif
  *out = v;
  return true;
}

// =========================================================================
// Helper: runs an echo server on a background thread.
// =========================================================================
struct EchoServer {
  /// 交给 `set_tcp_nodelay()` 的值。**必须在 `start()` 之前写**
  /// （库对这条顺序有 `@warning`：它是在每次 accept 时被读的）。
  bool want_nodelay = true;
  /// accept 回调里从套接字上读回来的 `TCP_NODELAY`；-1 = 还没读到。
  std::atomic<int> observed_nodelay{-1};

  std::promise<int> port_promise;
  std::atomic<bool> server_ready{false};
  std::atomic<bool> stop_server{false};
  std::thread thread;
  int port = 0;

  void start() {
    thread = std::thread([this]() {
      uvcpp_tcp_server server;
      server.bind("127.0.0.1", 0);
      server.set_tcp_nodelay(want_nodelay);

      sockaddr_in name;
      int namelen = sizeof(name);
      server.get_tcp()->getsockname(
          reinterpret_cast<sockaddr*>(&name), &namelen);
      int p = ntohs(name.sin_port);
      port_promise.set_value(p);

      server.listen(
          [this](uvcpp_tcp_client* client) {
            int seen = -1;
            if (read_tcp_nodelay(client, &seen)) observed_nodelay.store(seen);
            client->read_start([client](uvcpp_buf* buf) {
              if (buf != nullptr && buf->size() > 0) {
                uvcpp_buf* echo = new uvcpp_buf();
                echo->clone_data(buf->get_data(), buf->size());
                uv_buf_t* raw = echo->out_uv_buf();
                uvcpp_write* w = new uvcpp_write();
                w->set_uv_buf(raw, true);
                client->get_tcp()->write(
                    w, w->get_uv_buf(), 1,
                    [echo](uvcpp_write* wr, int) {
                      delete echo;
                      delete wr;
                    });
              }
            });
          },
          128);

      server_ready.store(true);

      int polls = 0;
      while (!stop_server.load() && polls < 2000) {
        server.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        polls++;
      }
    });

    port = port_promise.get_future().get();
    while (!server_ready.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void stop() {
    stop_server.store(true);
    if (thread.joinable()) thread.join();
  }
};

// =========================================================================
// Test 1: Sync echo
// =========================================================================
static bool test_sync_echo(int port) {
  uvcpp_tcp_client client;
  if (client.connect_wait("127.0.0.1", port, 5000) != 0) return false;
  const char* msg = "hello_sync";
  if (client.write_wait(msg, strlen(msg), 5000) != 0) return false;
  uvcpp_buf buf;
  if (client.read_wait(buf, 5000) != 0) return false;
  return buf.to_string() == msg;
}

// =========================================================================
// Test 2: Async echo
// =========================================================================
static bool test_async_echo(int port) {
  std::atomic<bool> connected{false}, wrote{false}, read{false}, ok{false};
  const char* msg = "hello_async";
  uvcpp_tcp_client client;

  if (client.connect("127.0.0.1", port, [&](int st) {
        if (st != 0) { client.stop(); return; }
        connected.store(true);
        client.write(msg, strlen(msg), [&](int ws) {
          if (ws == 0) wrote.store(true);
          else client.stop();
        });
        client.read_start([&](uvcpp_buf* buf) {
          if (buf && buf->to_string() == msg) ok.store(true);
          read.store(true);
          client.stop();
        });
      }) != 0) return false;

  client.run(UV_RUN_DEFAULT);
  return connected.load() && wrote.load() && read.load() && ok.load();
}

// =========================================================================
// Test 3: Multiple clients
// =========================================================================
static bool test_multiple_clients(int port) {
  const int N = 3;
  std::atomic<int> good{0};
  std::thread th[3];
  for (int i = 0; i < N; i++)
    th[i] = std::thread([port, i, &good]() {
      uvcpp_tcp_client cl;
      if (cl.connect_wait("127.0.0.1", port, 5000) != 0) return;
      std::string m = "m_" + std::to_string(i);
      if (cl.write_wait(m.c_str(), m.size(), 5000) != 0) return;
      uvcpp_buf b;
      if (cl.read_wait(b, 5000) != 0) return;
      if (b.to_string() == m) good.fetch_add(1);
    });
  for (int i = 0; i < N; i++) th[i].join();
  return good.load() == N;
}

// =========================================================================
// Test 4: Auto cleanup
// =========================================================================
static bool test_auto_cleanup(int port) {
  uvcpp_tcp_client client;
  if (client.connect_wait("127.0.0.1", port, 5000) != 0) return false;
  const char* msg = "cleanup";
  if (client.write_wait(msg, strlen(msg), 5000) != 0) return false;
  uvcpp_buf buf;
  if (client.read_wait(buf, 5000) != 0) return false;
  return buf.to_string() == msg;
}

// =========================================================================
// Test: write(uvcpp_buf&) sync — auto-copy
// =========================================================================
static bool test_write_buf_copy(int port) {
  uvcpp_tcp_client client;
  if (client.connect_wait("127.0.0.1", port, 5000) != 0) return false;

  uvcpp_buf msg("hello_buf_copy");
  // After write_wait, msg is unchanged (auto-copy)
  if (client.write_wait(msg, 5000) != 0) return false;
  if (msg.to_string() != "hello_buf_copy") return false;  // still intact

  uvcpp_buf out;
  if (client.read_wait(out, 5000) != 0) return false;
  return out.to_string() == "hello_buf_copy";
}

// =========================================================================
// Test: write(uvcpp_buf*) sync — zero-copy, transfers data ownership
// =========================================================================
static bool test_write_buf_move(int port) {
  uvcpp_tcp_client client;
  if (client.connect_wait("127.0.0.1", port, 5000) != 0) return false;

  uvcpp_buf* msg = new uvcpp_buf("hello_buf_move");
  // After write_wait, msg is empty (data moved out)
  if (client.write_wait(msg, 5000) != 0) { delete msg; return false; }
  bool emptied = (msg->size() == 0);  // data was transferred
  delete msg;  // caller still manages the object

  if (!emptied) return false;

  uvcpp_buf out;
  if (client.read_wait(out, 5000) != 0) return false;
  return out.to_string() == "hello_buf_move";
}

// =========================================================================
// Test: write(uvcpp_buf&, cb) async — auto-copy
// =========================================================================
static bool test_write_buf_copy_async(int port) {
  std::atomic<bool> wrote{false}, ok{false};
  uvcpp_buf msg("hello_async_copy");
  uvcpp_tcp_client client;

  if (client.connect("127.0.0.1", port, [&](int st) {
        if (st != 0) { client.stop(); return; }
        client.write(msg, [&](int ws) {
          wrote.store(true);
          if (ws != 0) client.stop();
        });
        client.read_start([&](uvcpp_buf* buf) {
          if (buf && buf->to_string() == "hello_async_copy") ok.store(true);
          client.stop();
        });
      }) != 0) return false;

  client.run(UV_RUN_DEFAULT);
  // msg should still be intact after async write
  return wrote.load() && ok.load() && msg.to_string() == "hello_async_copy";
}

// =========================================================================
// Test: write(uvcpp_buf*, cb) async — zero-copy
// =========================================================================
static bool test_write_buf_move_async(int port) {
  std::atomic<bool> wrote{false}, ok{false};
  uvcpp_buf* msg = new uvcpp_buf("hello_async_move");
  uvcpp_tcp_client client;

  if (client.connect("127.0.0.1", port, [&](int st) {
        if (st != 0) { client.stop(); delete msg; return; }
        client.write(msg, [&](int ws) {
          wrote.store(true);
          if (ws != 0) client.stop();
        });
        client.read_start([&](uvcpp_buf* buf) {
          if (buf && buf->to_string() == "hello_async_move") ok.store(true);
          client.stop();
        });
      }) != 0) { delete msg; return false; }

  client.run(UV_RUN_DEFAULT);
  bool emptied = (msg->size() == 0);
  delete msg;
  return wrote.load() && ok.load() && emptied;
}

// =========================================================================
// Test 5: Server status
// =========================================================================
static bool test_server_status() {
  uvcpp_tcp_server server;
  if (server.get_status() != TCP_SERVER_NONE) return false;
  // 端口随便给：本用例只看状态迁移。原先写死 19999 会与
  // `tcp_client_func` 那条"连 19999 必须失败"的用例抢同一个端口。
  if (server.bind("127.0.0.1", 0) != 0) return false;
  if (!server.has_status(TCP_SERVER_LISTENING)) return false;
  if (server.listen([](uvcpp_tcp_client*) {}, 128) != 0) return false;

  std::atomic<bool> done{false};
  server.stop([&done]() { done.store(true); });
  for (int i = 0; i < 5000 && !done.load(); i++) {
    server.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done.load() && (server.get_status() & TCP_SERVER_STOPPED);
}

// =========================================================================
// Nagle 那一组：默认关（= 套接字上 TCP_NODELAY == 1）
//
// 为什么要有这一组：libuv 的 `uv_tcp_t` **不**设 TCP_NODELAY，于是 Nagle 是开的。
// h1 的应答是"整条报文一次 `uv_write`"，看不见；h2 上 gRPC 那种"先 HEADERS+DATA、
// 再一段 trailers"的两段式应答，第二段会一直等到对端那个 40 ms 的延迟 ACK 才走
// —— 本机实测 h2c gRPC 的 `request` 中位数恒为 40.97 ms 而 TTFB 只有 345 µs，且
// 在 `-m 1..100` 上纹丝不动（这正是"不是负载、是定时器"的指纹）。
// =========================================================================

/// 一次同步回环（顺带把 accept 回调跑完 —— 回程数据到了，说明服务端那条连接
/// 早就被接受了）。
static bool echo_roundtrip(int port) {
  uvcpp_tcp_client client;
  if (client.connect_wait("127.0.0.1", port, 5000) != 0) return false;
  const char* msg = "nodelay";
  if (client.write_wait(msg, strlen(msg), 5000) != 0) return false;
  uvcpp_buf buf;
  if (client.read_wait(buf, 5000) != 0) return false;
  return buf.to_string() == msg;
}

/// 有界等待 accept 回调把套接字上的值记录进来。
static int wait_observed_nodelay(EchoServer& s) {
  for (int i = 0; i < 400 && s.observed_nodelay.load() < 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return s.observed_nodelay.load();
}

/// 访问器那一半：默认 true、设了读得回来。不动套接字（也就不必 listen）。
static bool test_nodelay_accessor() {
  uvcpp_tcp_server server;
  if (!server.tcp_nodelay()) return false;  // 默认必须开

  server.set_tcp_nodelay(false);
  if (server.tcp_nodelay()) return false;

  server.set_tcp_nodelay(true);
  return server.tcp_nodelay();
}

/// 套接字那一半：`want` 是喂给 `set_tcp_nodelay()` 的值，判据是**从 fd 上读回来
/// 的那一个数"零 / 非零"与 `want` 一致**（为什么不是"恰好等于 1"，
/// 见 `read_tcp_nodelay` 上面那段：Darwin 回 4、Windows 只写一个字节）。
///
/// 两个方向都要量。只量 true 的话，"忘了设 false"这条永远测不出来（库默认就是
/// true，什么都不做也绿）；只量 false 的话，把默认翻成 false 这条测不出来。
static bool test_nodelay_on_socket(int* observed_out, bool want) {
  EchoServer s;
  s.want_nodelay = want;
  s.start();
  bool echoed = echo_roundtrip(s.port);
  int got = wait_observed_nodelay(s);
  s.stop();

  if (observed_out != nullptr) *observed_out = got;
  // `got < 0` 是"压根没读到"（accept 回调没把值记进来），别让它借着
  // "非零即开"混过去 —— 那样 `want == true` 就会假绿。
  return echoed && got >= 0 && (got != 0) == want;
}

// =========================================================================
int main() {
  bool ok = true;

  {
    std::cout << "[tcp_server] sync_echo\n";
    EchoServer s; s.start();
    bool r = test_sync_echo(s.port);
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
    s.stop();
  }
  {
    std::cout << "[tcp_server] async_echo\n";
    EchoServer s; s.start();
    bool r = test_async_echo(s.port);
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
    s.stop();
  }
  {
    std::cout << "[tcp_server] write_buf_copy\n";
    EchoServer s; s.start();
    bool r = test_write_buf_copy(s.port);
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
    s.stop();
  }
  {
    std::cout << "[tcp_server] write_buf_move\n";
    EchoServer s; s.start();
    bool r = test_write_buf_move(s.port);
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
    s.stop();
  }
  {
    std::cout << "[tcp_server] write_buf_copy_async\n";
    EchoServer s; s.start();
    bool r = test_write_buf_copy_async(s.port);
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
    s.stop();
  }
  {
    std::cout << "[tcp_server] write_buf_move_async\n";
    EchoServer s; s.start();
    bool r = test_write_buf_move_async(s.port);
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
    s.stop();
  }
  {
    std::cout << "[tcp_server] server_status\n";
    bool r = test_server_status();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  {
    std::cout << "[tcp_server] tcp_nodelay_accessor\n";
    bool r = test_nodelay_accessor();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  {
    std::cout << "[tcp_server] tcp_nodelay_default_on\n";
    int got = -1;
    bool r = test_nodelay_on_socket(&got, true);
    std::cout << "     套接字上读回 TCP_NODELAY = " << got
              << "（非零即开；这边期望 开）\n";
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  {
    std::cout << "[tcp_server] tcp_nodelay_off\n";
    int got = -1;
    bool r = test_nodelay_on_socket(&got, false);
    std::cout << "     套接字上读回 TCP_NODELAY = " << got
              << "（非零即开；这边期望 关）\n";
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }

  std::cout << "[tcp_server] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}
