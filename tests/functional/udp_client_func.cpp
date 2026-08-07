#include <iostream>
#include <thread>
#include <future>
#include <atomic>
#include <cstring>
#include <string>
#include <uv.h>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_udp.h"
#include "req/uvcpp_udp_send.h"
#include "uvcpp/uvcpp_buf.h"
#include "net/uvcpp_udp_client.h"

using namespace uvcpp;

// =========================================================================
// UDP echo server — receives data and echoes back to sender.
// =========================================================================
struct UdpEchoServer {
  std::promise<int> port_promise;
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::thread thread;
  int port = 0;

  void start() {
    thread = std::thread([this]() {
      uvcpp_loop loop;
      loop.init();
      uvcpp_udp udp(&loop);

      struct sockaddr_in bind_addr;
      uv_ip4_addr("127.0.0.1", 0, &bind_addr);
      udp.bind(reinterpret_cast<const sockaddr*>(&bind_addr), 0);

      struct sockaddr_in name;
      int namelen = sizeof(name);
      udp.getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
      port_promise.set_value(ntohs(name.sin_port));

      udp.recv_start(
          [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
            uvcpp_buf::alloc_buf(buf, sz > 0 ? sz : 4096);
          },
          [&udp](uvcpp_udp*, ssize_t nread, const uv_buf_t* buf,
                 const sockaddr* addr, unsigned) {
            if (nread > 0) {
              // Echo back
              uvcpp_buf echo_buf(buf->base, static_cast<size_t>(nread));
              uv_buf_t* raw = echo_buf.out_uv_buf();
              uvcpp_udp_send* req = new uvcpp_udp_send();
              req->set_uv_buf(raw, true);
              udp.send(req, raw, 1, addr,
                       [](uvcpp_udp_send* r, int) { delete r; });
            }
            if (buf->base) uvcpp_free_bytes(buf->base);
          });

      ready.store(true);

      // Poll until stop
      int polls = 0;
      while (!stop.load() && polls < 2000) {
        loop.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        polls++;
      }
    });

    port = port_promise.get_future().get();
    while (!ready.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  void stop_server() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// =========================================================================
// Test 1: bind + sync send_wait + sync recv_wait (echo round-trip)
// =========================================================================
static bool test_sync_send_recv(int port) {
  uvcpp_udp_client client;
  client.bind("127.0.0.1", 0);

  const char* msg = "hello_udp_sync";
  if (client.send_wait("127.0.0.1", port, msg, strlen(msg), 5000) != 0)
    return false;

  uvcpp_buf out;
  if (client.recv_wait(out, 5000) != 0) return false;
  return out.to_string() == msg;
}

// =========================================================================
// Test 2: async send + async recv (echo round-trip)
// =========================================================================
static bool test_async_send_recv(int port) {
  std::atomic<bool> sent{false}, recvd{false}, ok{false};
  const char* msg = "hello_udp_async";
  uvcpp_udp_client client;
  client.bind("127.0.0.1", 0);

  client.recv_start([&](uvcpp_buf* buf, const char*, int) {
    if (buf && buf->to_string() == msg) ok.store(true);
    recvd.store(true);
    client.stop();
  });

  int rc = client.send("127.0.0.1", port, msg, strlen(msg),
                        [&](int s) { sent.store(s == 0); });
  if (rc != 0) return false;

  client.run(UV_RUN_DEFAULT);
  return sent.load() && recvd.load() && ok.load();
}

// =========================================================================
// Test 3: send without binding (OS assigns source port)
// =========================================================================
static bool test_send_no_bind(int port) {
  uvcpp_udp_client client;
  // Don't call bind() — send should still work with OS-assigned port

  const char* msg = "no_bind_test";
  if (client.send_wait("127.0.0.1", port, msg, strlen(msg), 5000) != 0)
    return false;

  // Can't receive without binding, so just verify send succeeded
  return true;
}

// =========================================================================
// Test 4: write uvcpp_buf& (auto-copy)
// =========================================================================
static bool test_buf_copy(int port) {
  uvcpp_udp_client client;
  client.bind("127.0.0.1", 0);

  uvcpp_buf msg("hello_buf_copy");
  if (client.send("127.0.0.1", port, msg, nullptr) != 0) return false;
  if (msg.to_string() != "hello_buf_copy") return false;  // unchanged

  uvcpp_buf out;
  if (client.recv_wait(out, 5000) != 0) return false;
  return out.to_string() == "hello_buf_copy";
}

// =========================================================================
// Test 5: write uvcpp_buf* (zero-copy, moves data)
// =========================================================================
static bool test_buf_move(int port) {
  uvcpp_udp_client client;
  client.bind("127.0.0.1", 0);

  uvcpp_buf* msg = new uvcpp_buf("hello_buf_move");
  if (client.send("127.0.0.1", port, msg, nullptr) != 0) {
    delete msg; return false;
  }
  bool emptied = (msg->size() == 0);
  delete msg;
  if (!emptied) return false;

  uvcpp_buf out;
  if (client.recv_wait(out, 5000) != 0) return false;
  return out.to_string() == "hello_buf_move";
}

// =========================================================================
// main
// =========================================================================
int main() {
  bool ok = true;

  {
    std::cout << "[udp_client] sync_send_recv\n";
    UdpEchoServer s; s.start();
    bool r = test_sync_send_recv(s.port);
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok; s.stop_server();
  }
  {
    std::cout << "[udp_client] async_send_recv\n";
    UdpEchoServer s; s.start();
    bool r = test_async_send_recv(s.port);
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok; s.stop_server();
  }
  {
    std::cout << "[udp_client] send_no_bind\n";
    UdpEchoServer s; s.start();
    bool r = test_send_no_bind(s.port);
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok; s.stop_server();
  }
  {
    std::cout << "[udp_client] buf_copy\n";
    UdpEchoServer s; s.start();
    bool r = test_buf_copy(s.port);
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok; s.stop_server();
  }
  {
    std::cout << "[udp_client] buf_move\n";
    UdpEchoServer s; s.start();
    bool r = test_buf_move(s.port);
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok; s.stop_server();
  }

  std::cout << "[udp_client] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}
