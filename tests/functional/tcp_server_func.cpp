#include <iostream>
#include <thread>
#include <future>
#include <atomic>
#include <cstring>
#include <string>
#include <uv.h>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "uvcpp/uvcpp_buf.h"
#include "net/uvcpp_tcp_client.h"
#include "net/uvcpp_tcp_server.h"

using namespace uvcpp;

// =========================================================================
// Helper: runs an echo server on a background thread.
// =========================================================================
struct EchoServer {
  std::promise<int> port_promise;
  std::atomic<bool> server_ready{false};
  std::atomic<bool> stop_server{false};
  std::thread thread;
  int port = 0;

  void start() {
    thread = std::thread([this]() {
      uvcpp_tcp_server server;
      server.bind("127.0.0.1", 0);

      sockaddr_in name;
      int namelen = sizeof(name);
      server.get_tcp()->getsockname(
          reinterpret_cast<sockaddr*>(&name), &namelen);
      int p = ntohs(name.sin_port);
      port_promise.set_value(p);

      server.listen(
          [](uvcpp_tcp_client* client) {
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
  if (server.bind("127.0.0.1", 19999) != 0) return false;
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

  std::cout << "[tcp_server] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}
