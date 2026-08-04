#include <iostream>
#include <thread>
#include <future>
#include <atomic>
#include <cstring>
#include <string>
#include <stdexcept>
#include <uv.h>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "handle/uvcpp_timer.h"
#include "req/uvcpp_connect.h"
#include "req/uvcpp_write.h"
#include "uvcpp/uvcpp_buf.h"
#include "net/uvcpp_tcp_client.h"

using namespace uvcpp;

// =========================================================================
// Persistent echo server: accepts multiple connections, echoes data back.
// Stops only on watchdog timeout or explicit stop signal.
// =========================================================================
static void run_echo_server(std::promise<int>& port_promise,
                            std::atomic<bool>& server_ready,
                            std::atomic<bool>& stop_server) {
  uvcpp_loop server_loop;
  server_loop.init();

  uvcpp_tcp server(&server_loop);
  server.bindIpv4("127.0.0.1", 0);

  sockaddr_in name;
  int namelen = sizeof(name);
  server.getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  int port = ntohs(name.sin_port);
  port_promise.set_value(port);

  server.listen(
      [&](uvcpp_stream* s, int status) {
        if (status < 0) return;

        uvcpp_tcp* peer = new uvcpp_tcp(&server_loop);
        int accept_rc = s->accept(peer);
        if (accept_rc != 0) {
          delete peer;
          return;
        }

        peer->read_start(
            [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
              uvcpp_buf::alloc_buf(buf, sz > 0 ? sz : 4096);
            },
            [peer, &server_loop](uvcpp_stream* stream,
                                  ssize_t nread,
                                  const uv_buf_t* buf) {
              if (nread > 0) {
                // Echo the data back
                uvcpp_buf* bufcpp = new uvcpp_buf(buf->base, (size_t)nread);
                uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));

                uvcpp_write* w = new uvcpp_write();
                w->set_uv_buf(bufcpp->out_uv_buf(), true);

                stream->write(
                    w, w->get_uv_buf(), 1,
                    [bufcpp](uvcpp_write* wr, int /*ws*/) {
                      delete bufcpp;
                      delete wr;
                    });
              } else {
                // EOF or error — close this peer, but keep server running
                if (buf->base != nullptr) {
                  uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
                }
                stream->close([peer](uvcpp_handle*) { delete peer; });
              }
            });
      },
      128);

  server_ready.store(true);

  // Watchdog: stop server after 20 seconds
  uvcpp_timer watchdog(&server_loop);
  watchdog.start(
      [&server_loop](uvcpp_timer* t) {
        std::cout << "[functional tcp_client] server watchdog timeout\n";
        server_loop.stop();
      },
      20000, 0);

  // Poll for stop signal
  uvcpp_timer stop_poller(&server_loop);
  stop_poller.start(
      [&stop_server, &server_loop](uvcpp_timer* t) {
        if (stop_server.load()) {
          server_loop.stop();
        }
      },
      100, 100);  // poll every 100ms

  server_loop.run(UV_RUN_DEFAULT);
}

// =========================================================================
// Test 1: Sync connect + write + read echo
// =========================================================================
static bool test_sync_echo(int port) {
  std::cout << "[functional tcp_client] sync_echo start\n";

  uvcpp_tcp_client client;

  // Connect
  int rc = client.connect_wait("127.0.0.1", port, 5000);
  if (rc != 0) {
    std::cout << "[functional tcp_client] sync_echo connect_wait failed: "
              << uv_err_name(rc) << " " << uv_strerror(rc) << std::endl;
    return false;
  }
  std::cout << "[functional tcp_client] sync_echo connected\n";

  // Verify status
  if (!client.has_status(TCP_CLIENT_CONNECTED)) {
    std::cout << "[functional tcp_client] sync_echo status check failed\n";
    return false;
  }

  // Verify addresses
  std::string local_ip, peer_ip;
  int local_port = 0, peer_port = 0;
  client.getLocalAddrs(local_ip, local_port);
  client.getPeerAddrs(peer_ip, peer_port);
  std::cout << "[functional tcp_client] sync_echo local=" << local_ip << ":"
            << local_port << " peer=" << peer_ip << ":" << peer_port
            << std::endl;

  if (local_ip.empty() || peer_ip.empty()) {
    std::cout << "[functional tcp_client] sync_echo address lookup failed\n";
    return false;
  }

  // Write
  const char* msg = "hello_sync_echo";
  rc = client.write_wait(msg, strlen(msg), 5000);
  if (rc != 0) {
    std::cout << "[functional tcp_client] sync_echo write_wait failed: "
              << uv_err_name(rc) << " " << uv_strerror(rc) << std::endl;
    return false;
  }
  std::cout << "[functional tcp_client] sync_echo wrote " << strlen(msg)
            << " bytes\n";

  // Read echo back
  uvcpp_buf out_buf;
  rc = client.read_wait(out_buf, 5000);
  if (rc != 0) {
    std::cout << "[functional tcp_client] sync_echo read_wait failed: "
              << uv_err_name(rc) << " " << uv_strerror(rc) << std::endl;
    return false;
  }

  std::string received = out_buf.to_string();
  std::cout << "[functional tcp_client] sync_echo received: " << received
            << std::endl;

  if (received != msg) {
    std::cout << "[functional tcp_client] sync_echo data mismatch: expected '"
              << msg << "' got '" << received << "'" << std::endl;
    return false;
  }

  std::cout << "[functional tcp_client] sync_echo done success=true\n";
  return true;
}

// =========================================================================
// Test 2: Sync connect failure (connect to unused port)
// =========================================================================
static bool test_sync_connect_failure() {
  std::cout << "[functional tcp_client] connect_failure start\n";

  uvcpp_tcp_client client;

  int rc = client.connect_wait("127.0.0.1", 19999, 2000);
  if (rc == 0) {
    std::cout << "[functional tcp_client] connect_failure unexpectedly "
              << "succeeded\n";
    return false;
  }

  std::cout << "[functional tcp_client] connect_failure got expected error: "
            << uv_err_name(rc) << " " << uv_strerror(rc) << std::endl;

  if (!client.has_status(TCP_CLIENT_ERROR)) {
    std::cout
        << "[functional tcp_client] connect_failure ERROR flag not set\n";
    return false;
  }

  int last_err = client.get_last_error();
  if (last_err == 0) {
    std::cout
        << "[functional tcp_client] connect_failure last_error not recorded\n";
    return false;
  }
  std::cout << "[functional tcp_client] connect_failure last_error="
            << last_err << " (" << uv_err_name(last_err) << ")\n";

  std::cout << "[functional tcp_client] connect_failure done success=true\n";
  return true;
}

// =========================================================================
// Test 3: Async connect + write + read echo
// =========================================================================
static bool test_async_echo(int port) {
  std::cout << "[functional tcp_client] async_echo start\n";

  std::atomic<bool> connected(false);
  std::atomic<bool> write_done(false);
  std::atomic<bool> read_done(false);
  std::atomic<bool> data_ok(false);

  const char* msg = "hello_async_echo";

  uvcpp_tcp_client client;

  int rc = client.connect(
      "127.0.0.1", port,
      [&](int status) {
        if (status == 0) {
          connected.store(true);
          std::cout << "[functional tcp_client] async_echo connected\n";

          client.write(
              msg, strlen(msg),
              [&](int ws) {
                if (ws == 0) {
                  write_done.store(true);
                  std::cout << "[functional tcp_client] async_echo wrote "
                            << strlen(msg) << " bytes\n";
                } else {
                  std::cout
                      << "[functional tcp_client] async_echo write failed: "
                      << uv_err_name(ws) << std::endl;
                  client.stop();
                }
              });

          client.read_start([&](uvcpp_buf* buf) {
            if (buf != nullptr) {
              std::string data = buf->to_string();
              std::cout << "[functional tcp_client] async_echo received: "
                        << data << std::endl;
              if (data == msg) {
                data_ok.store(true);
              }
              read_done.store(true);
              client.stop();
            }
          });
        } else {
          std::cout << "[functional tcp_client] async_echo connect failed: "
                    << uv_err_name(status) << std::endl;
          client.stop();
        }
      });

  if (rc != 0) {
    std::cout << "[functional tcp_client] async_echo connect start failed: "
              << uv_err_name(rc) << std::endl;
    return false;
  }

  // Run the loop (blocks until client.stop() is called from callback)
  client.run(UV_RUN_DEFAULT);

  bool ok = connected.load() && write_done.load() && read_done.load() &&
            data_ok.load();
  std::cout << "[functional tcp_client] async_echo done success="
            << (ok ? "true" : "false") << std::endl;
  return ok;
}

// =========================================================================
// Test 4: Sync write, then async read mix (allowed — different operations)
// =========================================================================
static bool test_sync_write_async_read(int port) {
  std::cout << "[functional tcp_client] sync_write_async_read start\n";

  std::atomic<bool> read_ok(false);
  const char* msg = "mixed_mode_test";

  uvcpp_tcp_client client;

  // Sync connect
  int rc = client.connect_wait("127.0.0.1", port, 5000);
  if (rc != 0) {
    std::cout
        << "[functional tcp_client] sync_write_async_read connect failed: "
        << uv_err_name(rc) << std::endl;
    return false;
  }

  // Sync write
  rc = client.write_wait(msg, strlen(msg), 5000);
  if (rc != 0) {
    std::cout
        << "[functional tcp_client] sync_write_async_read write failed: "
        << uv_err_name(rc) << std::endl;
    return false;
  }

  // Async read
  client.read_start([&](uvcpp_buf* buf) {
    if (buf != nullptr) {
      std::string data = buf->to_string();
      std::cout << "[functional tcp_client] sync_write_async_read received: "
                << data << std::endl;
      if (data == msg) {
        read_ok.store(true);
      }
    }
    client.stop();
  });

  client.run(UV_RUN_DEFAULT);

  bool ok = read_ok.load();
  std::cout << "[functional tcp_client] sync_write_async_read done success="
            << (ok ? "true" : "false") << std::endl;
  return ok;
}

// =========================================================================
// Test 5: Mode mixing detection
// =========================================================================
static bool test_mode_mixing_detection() {
  std::cout << "[functional tcp_client] mode_mixing start\n";

  uvcpp_tcp_client client;

  // Register async connect callback
  int rc = client.connect(
      "127.0.0.1", 12345,
      [](int /*status*/) {});

  if (rc != 0) {
    // May fail immediately if nothing listening
    std::cout << "[functional tcp_client] mode_mixing async connect rc=" << rc
              << std::endl;
  }

  // Now try sync connect_wait - should throw
  bool threw = false;
  try {
    client.connect_wait("127.0.0.1", 12346, 1000);
  } catch (const std::runtime_error& e) {
    threw = true;
    std::cout << "[functional tcp_client] mode_mixing caught: " << e.what()
              << std::endl;
  }

  if (!threw) {
    std::cout
        << "[functional tcp_client] mode_mixing should have thrown but didn't"
        << std::endl;
    return false;
  }

  std::cout << "[functional tcp_client] mode_mixing done success=true\n";
  return true;
}

// =========================================================================
// Test 6: Status transitions
// =========================================================================
static bool test_status_transitions(int port) {
  std::cout << "[functional tcp_client] status_transitions start\n";

  uvcpp_tcp_client client;

  // Initial status
  if (client.get_status() != TCP_CLIENT_NONE) {
    std::cout << "[functional tcp_client] status_transitions initial status "
              << "not NONE: " << client.get_status() << std::endl;
    return false;
  }

  // After connect
  int rc = client.connect_wait("127.0.0.1", port, 5000);
  if (rc != 0) {
    std::cout << "[functional tcp_client] status_transitions connect failed: "
              << uv_err_name(rc) << std::endl;
    return false;
  }

  int status = client.get_status();
  if (!(status & TCP_CLIENT_CONNECTED)) {
    std::cout << "[functional tcp_client] status_transitions CONNECTED flag "
              << "missing: " << status << std::endl;
    return false;
  }
  if (status & TCP_CLIENT_CONNECTING) {
    std::cout << "[functional tcp_client] status_transitions CONNECTING flag "
              << "still set: " << status << std::endl;
    return false;
  }

  if (!client.has_status(TCP_CLIENT_CONNECTED)) {
    std::cout
        << "[functional tcp_client] status_transitions has_status failed\n";
    return false;
  }

  std::cout << "[functional tcp_client] status_transitions status=0x"
            << std::hex << status << std::dec << std::endl;

  std::cout << "[functional tcp_client] status_transitions done success=true\n";
  return true;
}

// =========================================================================
// main
// =========================================================================
int main() {
  std::cout << "[functional tcp_client] start\n";

  // Start persistent echo server
  std::promise<int> port_promise;
  auto port_future = port_promise.get_future();
  std::atomic<bool> server_ready(false);
  std::atomic<bool> stop_server(false);

  std::thread server_thread(run_echo_server, std::ref(port_promise),
                            std::ref(server_ready), std::ref(stop_server));

  int port = port_future.get();
  std::cout << "[functional tcp_client] echo server on port " << port
            << std::endl;

  while (!server_ready.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  bool all_ok = true;

  // Run all tests
  all_ok = test_sync_echo(port) && all_ok;
  all_ok = test_sync_connect_failure() && all_ok;
  all_ok = test_async_echo(port) && all_ok;
  all_ok = test_sync_write_async_read(port) && all_ok;
  all_ok = test_mode_mixing_detection() && all_ok;
  all_ok = test_status_transitions(port) && all_ok;

  // Stop server
  stop_server.store(true);
  server_thread.join();

  std::cout << "[functional tcp_client] done success="
            << (all_ok ? "true" : "false") << std::endl;
  return all_ok ? 0 : 2;
}
