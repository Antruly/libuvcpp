#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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
              } else if (nread == 0) {
                // 空读不是 EOF。libuv 的 win/tcp.c:1076 写得很直白：缓冲区
                // 整个是空的时侯就报一次 0 字节读，等价于 read(2) 的 EAGAIN。
                // 以前这里把 `nread <= 0` 一律当成"对端关了"，于是一次空读
                // 就把连接拆掉 —— 表现为偶发的"回显一个字节都没回来"。
                if (buf->base != nullptr) {
                  uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
                }
              } else {
                // nread < 0：UV_EOF 或真错误 —— 关掉这个 peer，服务端继续跑
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
// Test 7: 零拷贝异步写（`uvcpp_buf*` 重载）连发 —— 不能毒化连接
//
// 缺陷（Phase 3c「观察到但未修」第 2 条）：`write(uvcpp_buf*, cb)` 的成功
// 回调只清 `write_fn_` / `write_arg_`，**从不清 `has_async_write_cb_`**。
// 于是第一次异步写之后就永久毒化：此后每一次异步写都在入口被
// `if (has_async_write_cb_) return UV_EALREADY;` 挡掉 —— 调用方收到的是一个
// 看着像"协议错误"的返回值，不是崩溃，所以静默失效。3c 的 `send_file` 是靠
// 改走 `const char*` 重载绕开的，这条路当时是坏的。
//
// 判据三条，缺一条都不成立：
//   1. 三次连发的返回值都是 0 —— **第二次**正是缺陷版本拿 UV_EALREADY 的地方；
//   2. 三次回调都送到（不是被入口挡掉），全部完成之后 `has_write_callback()`
//      必须回到 false（毒化状态的直接观察点）；
//   3. 服务端回显的字节与三次写入拼起来的**完全一致** —— 前两条只证明
//      "接口没报错"，只有这一条证明字节真的出去了。
//
// 等待用墙钟上界，不靠"回调一定会来"：观测不到就按失败报出。
// =========================================================================
static bool test_async_buf_write_no_poison(int port) {
  std::cout << "[functional tcp_client] async_buf_write start\n";

  uvcpp_tcp_client client;

  const char* parts[3] = {"alpha-", "bravo-", "charlie"};
  const size_t lens[3] = {6, 6, 7};
  const std::string expect = "alpha-bravo-charlie";

  std::atomic<int> wrc[3];
  std::atomic<int> callbacks(0);
  std::atomic<int> write_status(0);
  std::atomic<bool> connected(false);
  /// 对端把连接关了（读回调收到空 buf）—— 与"数据没回来"是两件事，
  /// 失败时必须分得清是哪一件。
  std::atomic<bool> peer_closed(false);
  std::mutex mu;
  std::string echoed;

  wrc[0].store(-1);
  wrc[1].store(-1);
  wrc[2].store(-1);

  // 每次写都在**上一次的写回调里**发起 —— 这正是缺陷版本的死法：回调跑的时候
  // `has_async_write_cb_` 还立着，下一次写当场被判成 UV_EALREADY。
  std::function<void(int)> issue_next = [&](int idx) {
    if (idx >= 3) return;
    // 零拷贝重载：数据所有权随本次写交出去，`uvcpp_buf` 对象本身仍由调用方
    // 销毁（`out_uv_buf()` 已经把数据摘走了，所以这里 delete 是安全的）。
    uvcpp_buf* b = new uvcpp_buf(parts[idx], lens[idx]);
    int wrc_now = client.write(b, [&, idx](int st) {
      if (st != 0) write_status.store(st);
      callbacks.fetch_add(1);
      issue_next(idx + 1);
    });
    wrc[idx].store(wrc_now);
    delete b;
  };

  int rc = client.connect("127.0.0.1", port, [&](int status) {
    if (status != 0) {
      std::cout << "[functional tcp_client] async_buf_write connect failed: "
                << uv_err_name(status) << std::endl;
      client.stop();
      return;
    }
    connected.store(true);
    // 先起读：回显可能比最后一个写回调先到。
    client.read_start([&](uvcpp_buf* buf) {
      if (buf == nullptr) {
        peer_closed.store(true);
        return;
      }
      std::lock_guard<std::mutex> lk(mu);
      echoed += buf->to_string();
    });
    issue_next(0);
  });

  if (rc != 0) {
    std::cout << "[functional tcp_client] async_buf_write connect start failed: "
              << uv_err_name(rc) << std::endl;
    return false;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    client.run(UV_RUN_NOWAIT);
    size_t got = 0;
    {
      std::lock_guard<std::mutex> lk(mu);
      got = echoed.size();
    }
    if (callbacks.load() == 3 && got >= expect.size()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 毒化状态的观察点必须在客户端析构（会关掉连接）之前读。
  const bool cb_flag_left_set = client.has_write_callback();

  std::string got;
  {
    std::lock_guard<std::mutex> lk(mu);
    got = echoed;
  }

  bool ok = true;
  if (!connected.load()) {
    std::cout << "[functional tcp_client] async_buf_write never connected\n";
    ok = false;
  }
  for (int i = 0; i < 3; ++i) {
    if (wrc[i].load() != 0) {
      std::cout << "[functional tcp_client] async_buf_write #" << i
                << " returned " << wrc[i].load() << " ("
                << uv_err_name(wrc[i].load()) << ")\n";
      ok = false;
    }
  }
  if (callbacks.load() != 3) {
    std::cout << "[functional tcp_client] async_buf_write callbacks="
              << callbacks.load() << " (expect 3)\n";
    ok = false;
  }
  if (cb_flag_left_set) {
    std::cout << "[functional tcp_client] async_buf_write "
                 "has_write_callback() still true after all writes\n";
    ok = false;
  }
  if (write_status.load() != 0) {
    std::cout << "[functional tcp_client] async_buf_write a write failed: "
              << uv_err_name(write_status.load()) << std::endl;
    ok = false;
  }
  if (got != expect) {
    std::cout << "[functional tcp_client] async_buf_write echo mismatch: got '"
              << got << "' (" << got.size() << "), expect '" << expect << "' ("
              << expect.size() << "), peer_closed=" << (peer_closed.load() ? 1 : 0)
              << ", client_status=0x" << std::hex << client.get_status()
              << std::dec << ", last_error=" << client.get_last_error() << "\n";
    ok = false;
  }

  std::cout << "[functional tcp_client] async_buf_write done success="
            << (ok ? "true" : "false") << std::endl;
  return ok;
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
  all_ok = test_async_buf_write_no_poison(port) && all_ok;

  // Stop server
  stop_server.store(true);
  server_thread.join();

  std::cout << "[functional tcp_client] done success="
            << (all_ok ? "true" : "false") << std::endl;
  return all_ok ? 0 : 2;
}
