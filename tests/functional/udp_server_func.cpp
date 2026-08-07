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
#include "net/uvcpp_udp_server.h"

using namespace uvcpp;

// =========================================================================
// Test 1: echo server + sync client send/recv
// =========================================================================
static bool test_echo_sync() {
  uvcpp_udp_server server;
  server.bind("127.0.0.1", 0);

  struct sockaddr_in name;
  int namelen = sizeof(name);
  server.get_udp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  int port = ntohs(name.sin_port);

  // Echo handler
  server.recv_start([&server](uvcpp_buf* buf, const char* ip, int p) {
    server.send(ip, p, buf->get_data(), buf->size());
  });

  // Start server loop on background thread
  std::atomic<bool> srv_stop{false};
  std::thread srv_thread([&]() {
    while (!srv_stop.load())
      server.run(UV_RUN_NOWAIT);
  });

  // Client
  uvcpp_udp_client client;
  client.bind("127.0.0.1", 0);

  const char* msg = "hello_udp_srv";
  if (client.send_wait("127.0.0.1", port, msg, strlen(msg), 5000) != 0) {
    srv_stop.store(true); srv_thread.join(); return false;
  }

  uvcpp_buf out;
  if (client.recv_wait(out, 5000) != 0) {
    srv_stop.store(true); srv_thread.join(); return false;
  }

  srv_stop.store(true); srv_thread.join();
  return out.to_string() == msg;
}

// =========================================================================
// Test 2: async client against server
// =========================================================================
static bool test_echo_async() {
  uvcpp_udp_server server;
  server.bind("127.0.0.1", 0);

  struct sockaddr_in name;
  int namelen = sizeof(name);
  server.get_udp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  int port = ntohs(name.sin_port);

  server.recv_start([&server](uvcpp_buf* buf, const char* ip, int p) {
    server.send(ip, p, buf->get_data(), buf->size());
  });

  std::atomic<bool> srv_stop{false};
  std::thread srv_thread([&]() {
    while (!srv_stop.load()) server.run(UV_RUN_NOWAIT);
  });

  std::atomic<bool> sent{false}, recvd{false}, ok{false};
  const char* msg = "hello_udp_srv_async";
  uvcpp_udp_client client;
  client.bind("127.0.0.1", 0);

  client.recv_start([&](uvcpp_buf* buf, const char*, int) {
    if (buf && buf->to_string() == msg) ok.store(true);
    recvd.store(true);
    client.stop();
  });

  client.send("127.0.0.1", port, msg, strlen(msg),
              [&](int s) { sent.store(s == 0); });

  client.run(UV_RUN_DEFAULT);

  srv_stop.store(true); srv_thread.join();
  return sent.load() && recvd.load() && ok.load();
}

// =========================================================================
// Test 3: server status transitions
// =========================================================================
static bool test_server_status() {
  uvcpp_udp_server server;
  if (server.get_status() != UDP_SERVER_NONE) return false;

  if (server.bind("127.0.0.1", 20001) != 0) return false;
  if (!server.has_status(UDP_SERVER_BOUND)) return false;

  if (server.recv_start([](uvcpp_buf*, const char*, int) {}) != 0) return false;

  std::atomic<bool> done{false};
  server.stop([&done]() { done.store(true); });
  for (int i = 0; i < 5000 && !done.load(); i++) {
    server.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return done.load() && (server.get_status() & UDP_SERVER_STOPPED);
}

// =========================================================================
// Test 4: send uvcpp_buf* from server (zero-copy)
// =========================================================================
static bool test_server_buf_move() {
  uvcpp_udp_server server;
  server.bind("127.0.0.1", 0);

  struct sockaddr_in name;
  int namelen = sizeof(name);
  server.get_udp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  int port = ntohs(name.sin_port);

  server.recv_start([&server](uvcpp_buf* buf, const char* ip, int p) {
    // Zero-copy: move data into a new buffer and send it
    uvcpp_buf* echo = new uvcpp_buf();
    echo->clone_data(buf->get_data(), buf->size());
    server.send(ip, p, echo, [echo](int) { delete echo; });
  });

  std::atomic<bool> srv_stop{false};
  std::thread srv_thread([&]() {
    while (!srv_stop.load()) server.run(UV_RUN_NOWAIT);
  });

  uvcpp_udp_client client;
  client.bind("127.0.0.1", 0);

  const char* msg = "buf_move_srv";
  if (client.send_wait("127.0.0.1", port, msg, strlen(msg), 5000) != 0) {
    srv_stop.store(true); srv_thread.join(); return false;
  }

  uvcpp_buf out;
  if (client.recv_wait(out, 5000) != 0) {
    srv_stop.store(true); srv_thread.join(); return false;
  }

  srv_stop.store(true); srv_thread.join();
  return out.to_string() == msg;
}

// =========================================================================
// main
// =========================================================================
int main() {
  bool ok = true;

  {
    std::cout << "[udp_server] echo_sync\n";
    bool r = test_echo_sync();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  {
    std::cout << "[udp_server] echo_async\n";
    bool r = test_echo_async();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  {
    std::cout << "[udp_server] server_status\n";
    bool r = test_server_status();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  {
    std::cout << "[udp_server] buf_move\n";
    bool r = test_server_buf_move();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }

  std::cout << "[udp_server] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}
