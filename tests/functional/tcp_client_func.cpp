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
#include "net/uvcpp_tcp_server.h"
#include "loop_drain.h"

using namespace uvcpp;

// =========================================================================
// Persistent echo server: accepts multiple connections, echoes data back.
// Stops only on watchdog timeout or explicit stop signal.
// =========================================================================
static void run_echo_server(std::promise<int>& port_promise,
                            std::atomic<bool>& server_ready,
                            std::atomic<bool>& stop_server) {
  uvcpp_loop server_loop;

  uvcpp_test::loop_drain drain_server_loop(&server_loop);
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

  // 占住一个端口但**不 listen**：内核对该端口的连接回 RST，于是"被拒"是确定性的；
  // 而端口在守卫存活期间不会被别的用例抢走。原先写死 19999，取拒绝靠的是
  // "这个端口是空的"这个**没有任何断言**的前提，而 tcp_server_func 恰好也要绑它。
  uvcpp_tcp_server guard;
  if (guard.bind("127.0.0.1", 0) != 0) return false;
  sockaddr_in bound;
  int bound_len = sizeof(bound);
  if (guard.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&bound),
                                   &bound_len) != 0) {
    return false;
  }
  int port = ntohs(bound.sin_port);
  // 守卫必须真的拿到了一个端口：拿到 0 的话下面照样会失败，但失败的理由是
  // "端口非法"而不是"被拒"，用例就白跑了。
  if (port == 0) {
    std::cout << "[functional tcp_client] connect_failure guard got no port\n";
    return false;
  }

  uvcpp_tcp_client client;

  int rc = client.connect_wait("127.0.0.1", port, 2000);
  if (rc == 0) {
    std::cout << "[functional tcp_client] connect_failure unexpectedly "
              << "succeeded\n";
    return false;
  }
  // 收紧到"对端没在听"这一族信号，但**不钉死具体码**：绑定未监听时
  // Linux/Windows 回 `ECONNREFUSED`，而 macOS 上实测**拿不到拒绝**，一路
  // 等到 `connect_wait` 的超时才回 `ETIMEDOUT`（CI 报的就是这个 —— 本机
  // 复现不了，所以内核那边到底为什么不回 RST 只是推测，不当结论写）。
  // 钉死某一个码，就是把这条用例绑死在那个平台的行为上。
  //
  // 收紧本身是必要的：端口非法（`EADDRNOTAVAIL`）、地址不可用之类的错误码
  // 说明**前置条件没立住**，而不是被测路径生效了。
  if (rc != UV_ECONNREFUSED && rc != UV_ETIMEDOUT) {
    std::cout << "[functional tcp_client] connect_failure expected a"
              << " no-listener error, got " << uv_err_name(rc) << std::endl;
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
// Accept-then-hangup server: closes every accepted connection immediately.
// 给"对端断开"那条腿用 —— 常驻回声服务端只在出错时才关连接，关不了。
// =========================================================================
static void run_hangup_server(std::promise<int>& port_promise,
                              std::atomic<bool>& server_ready,
                              std::atomic<bool>& stop_server) {
  uvcpp_loop loop;

  uvcpp_test::loop_drain drain_loop(&loop);
  loop.init();

  uvcpp_tcp server(&loop);
  server.bindIpv4("127.0.0.1", 0);

  sockaddr_in name;
  int namelen = sizeof(name);
  server.getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  port_promise.set_value(ntohs(name.sin_port));

  server.listen(
      [&](uvcpp_stream* s, int status) {
        if (status < 0) return;
        uvcpp_tcp* peer = new uvcpp_tcp(&loop);
        if (s->accept(peer) != 0) {
          delete peer;
          return;
        }
        // 立刻关：对端收到的是 EOF（干净 FIN，不是 RST）。
        peer->close([peer](uvcpp_handle*) { delete peer; });
      },
      16);

  server_ready.store(true);

  uvcpp_timer stop_poller(&loop);
  stop_poller.start(
      [&stop_server, &loop](uvcpp_timer*) {
        if (stop_server.load()) loop.stop();
      },
      50, 50);

  loop.run(UV_RUN_DEFAULT);
}

// =========================================================================
// 在**自己的读回调里**析构客户端
// =========================================================================
/**
 * 为什么这条的区分力**完全来自 PageHeap**：
 *
 * `~uvcpp_tcp_client` 收尾要拨循环把挂起的关闭回调放掉、把 `uv_loop_t` 关掉、
 * 把 `uvcpp_loop` 还回去。如果析构点**就在这个循环自己的读回调里**，那之后还
 * 活着（还在被用）的东西至少有三样：
 *
 *   1. 外层那一帧 `uv_run`（以及它周围的 `uvcpp_loop::run` 计数）
 *   2. `uvcpp_tcp` 的读回调闭包 —— 它捕获了客户端的 `this`，而且它自己就存在
 *      那个被删掉的客户端里
 *   3. **正在执行的那个 `std::function` 本身**（`net_read_cb_`），同理
 *
 * 裸跑时那些内存还好端端地在那儿，读到的还是旧值 —— 所以**不崩**；PageHeap
 * 把释放过的块直接 unmap，同样一次读法立刻 SEGFAULT。两条腿各钉一种"回调返回
 * 之后还剩什么"：
 *
 *   腿 1  收到数据时删 —— 返回之后只剩 `uvcpp_free_bytes(base)`（已经是局部）
 *   腿 2  对端断开时删 —— 返回之后还要走 `fire_close_callbacks()`（全是成员）
 *
 * 两条都不许挂死：真不返回就是失败，所以等待都是有界的。
 */
static bool test_delete_client_in_read_cb(int port, int hangup_port) {
  std::cout << "[functional tcp_client] delete_in_read_cb start\n";
  bool ok = true;

  // ---- 腿 1：收到数据时删（`on_read` 里 `delete`）----
  {
    uvcpp_tcp_client* victim = new uvcpp_tcp_client();  // 自持 loop
    int  data_seen = 0;
    bool deleted   = false;
    const char* msg = "delete_probe_1";

    int rc = victim->connect("127.0.0.1", port, [&](int status) {
      if (status != 0) {
        std::cout << "[functional tcp_client] delete_in_read_cb leg1 connect "
                  << uv_err_name(status) << std::endl;
        return;
      }
      victim->read_start_events(
          [&](uvcpp_tcp_client& /*c*/, const net_read_result& r) {
            if (!r.is_data()) return;
            ++data_seen;
            delete victim;  // <-- 就在这个循环的读回调里
            victim = nullptr;
            deleted = true;
          });
      victim->write(msg, strlen(msg), [](int) {});
    });
    if (rc != 0) {
      std::cout << "[functional tcp_client] delete_in_read_cb leg1 connect "
                   "start failed: " << uv_err_name(rc) << std::endl;
      delete victim;
      return false;
    }

    // 从外面一帧一帧拨：回调里已经把 client 删了，删完之后一个字节都不能再
    // 碰它 —— `victim != nullptr` 那个判断就是这件事（它判的是本函数的局部）。
    const auto t0 = std::chrono::steady_clock::now();
    while (victim != nullptr && !deleted &&
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count() < 5000) {
      victim->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 前置：回调必须真的跑过、也必须真的删了 —— 否则这条用例什么都没测。
    if (data_seen != 1 || !deleted || victim != nullptr) {
      std::cout << "[functional tcp_client] delete_in_read_cb leg1 FAIL "
                   "data_seen=" << data_seen << " deleted=" << deleted
                << " victim=" << static_cast<void*>(victim) << std::endl;
      ok = false;
    } else {
      std::cout << "[functional tcp_client] delete_in_read_cb leg1 survived\n";
    }
  }

  // ---- 腿 2：对端断开时删 ----
  {
    uvcpp_tcp_client* victim = new uvcpp_tcp_client();
    int  end_seen = 0;
    bool deleted  = false;

    int rc = victim->connect("127.0.0.1", hangup_port, [&](int status) {
      if (status != 0) {
        std::cout << "[functional tcp_client] delete_in_read_cb leg2 connect "
                  << uv_err_name(status) << std::endl;
        return;
      }
      victim->read_start_events(
          [&](uvcpp_tcp_client& /*c*/, const net_read_result& r) {
            if (!r.is_end()) return;
            ++end_seen;
            delete victim;  // <-- 回调返回之后还要跑 fire_close_callbacks()
            victim = nullptr;
            deleted = true;
          });
    });
    if (rc != 0) {
      std::cout << "[functional tcp_client] delete_in_read_cb leg2 connect "
                   "start failed: " << uv_err_name(rc) << std::endl;
      delete victim;
      return false;
    }

    const auto t0 = std::chrono::steady_clock::now();
    while (victim != nullptr && !deleted &&
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count() < 5000) {
      victim->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (end_seen != 1 || !deleted || victim != nullptr) {
      std::cout << "[functional tcp_client] delete_in_read_cb leg2 FAIL "
                   "end_seen=" << end_seen << " deleted=" << deleted
                << " victim=" << static_cast<void*>(victim) << std::endl;
      ok = false;
    } else {
      std::cout << "[functional tcp_client] delete_in_read_cb leg2 survived\n";
    }
  }

  // 走到这里说明两次析构都从自己的回调里活了下来。再跑一次完整回显：裸跑下
  // 这是冒烟断言，PageHeap 下前面那两段根本到不了。
  {
    uvcpp_tcp_client after;
    const char* msg = "after_delete_probe";
    if (after.connect_wait("127.0.0.1", port, 5000) != 0) {
      std::cout << "[functional tcp_client] delete_in_read_cb after: connect "
                   "failed\n";
      ok = false;
    } else if (after.write_wait(msg, strlen(msg), 5000) != 0) {
      std::cout << "[functional tcp_client] delete_in_read_cb after: write "
                   "failed\n";
      ok = false;
    } else {
      uvcpp_buf out;
      if (after.read_wait(out, 5000) != 0 || out.to_string() != msg) {
        std::cout << "[functional tcp_client] delete_in_read_cb after: echo "
                     "mismatch '" << out.to_string() << "'\n";
        ok = false;
      }
    }
  }

  std::cout << "[functional tcp_client] delete_in_read_cb done success="
            << (ok ? "true" : "false") << std::endl;
  return ok;
}

// =========================================================================
// 析构时**包装对象到底有没有被回收** —— 连接频繁建立/断开那条路
// =========================================================================
/**
 * 形状：服务跑着，连接不停地来、不停地断。**每一次断开都会析构一个
 * `uvcpp_tcp_client`**，而析构的位置几乎总在某个回调里（对端断开的读回调、
 * 关闭的完成回调）—— 也就是 `loop_->is_running()` 恒为真的地方。
 *
 * 改前那条判据是"循环还在跑 ⇒ 一个都不拆"，它区分不了"我正踩在自己的回调
 * 里"和"循环只是恰好跑着"。后者在服务运行期间永远成立，于是 `tcp_` 那个
 * 包装对象**一次也没被拆过**：实测 net 层 371 字节/连接（20000 次连接
 * +7.07 MB，斜率不收敛），webapp 0.74–1.17 MB/min。
 *
 * 判据为什么不取 RSS：那是**间接**量，慢、噪声大、CI 上没法当断言（涨到能
 * 测出来要几千次连接）。改后的判据是**三条支路各走了多少次**
 * （`uvcpp_tcp_client::reclaim_stats()`）—— 析构里那三条支路对外行为完全
 * 一样（连接都断了），从外面看不出来走了哪一条，所以把它记成计数。一次断开
 * 必须让 `released + deferred` 涨 1、且 `skipped` 恒为 0。这样"把回收改回
 * 不回收"就是**确定性**的失败，不用等内存涨起来。
 *
 * 另有一道独立判据：收起尾来 `uv_loop_close()` 必须返回 0。漏掉包装对象时
 * 它的 `uv_handle_t` 还挂在 handle_queue 上（既不活跃也没在关 —— `uv_run`
 * 连 while 体都不进），`uv_loop_close()` 只能 EBUSY。这条**不需要新计数**
 * 就能抓住老写法，是计数器之外的独立证人。
 *
 * 两条腿各钉一种"回调返回之后还剩什么"（用**外部循环**，这样析构不会连循环
 * 一起漏掉，收尾后还能问 `uv_loop_close()`）：
 *
 *   腿 1  对端断开，框架管理槽删客户端 —— 句柄还活着 ⇒ `deferred`
 *   腿 2  自己 `close()`，完成回调里删  —— 句柄已摘 ⇒ `released`
 */
static bool test_reclaim_paths(int port, int hangup_port) {
  std::cout << "[functional tcp_client] reclaim_paths start\n";
  bool ok = true;

  auto pump_until = [](uvcpp_loop* loop, const std::function<bool()>& done,
                       int timeout_ms) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!done() &&
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count() < timeout_ms) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };

  // ---- 腿 1：对端断开时删（句柄还活着 ⇒ deferred）----
  {
    uvcpp_loop loop;

    uvcpp_test::loop_drain drain_loop(&loop);
    loop.init();

    const uvcpp_tcp_client::reclaim_stat before =
        uvcpp_tcp_client::reclaim_stats();

    uvcpp_tcp_client* victim  = new uvcpp_tcp_client(&loop);
    bool              deleted = false;

    // 框架的管理槽 = "连接一关就把这个客户端删掉"，服务端（`uvcpp_tcp_server`）
    // 用的正是这一条。它在**读回调里**跑（`fire_close_callbacks()` 的第 1337 行
    // 那一路），此刻句柄还活着。
    victim->set_close_manager([&victim, &deleted]() {
      delete victim;
      victim  = nullptr;
      deleted = true;
    });

    int rc = victim->connect("127.0.0.1", hangup_port, [&](int status) {
      if (status != 0) {
        std::cout << "[functional tcp_client] reclaim_paths leg1 connect "
                  << uv_err_name(status) << std::endl;
        return;
      }
      // 对端 accept 完立刻 FIN：要读到它才走得到断开那一支。
      victim->read_start_events(
          [](uvcpp_tcp_client& /*c*/, const net_read_result& /*r*/) {});
    });
    if (rc != 0) {
      std::cout << "[functional tcp_client] reclaim_paths leg1 connect start "
                   "failed: " << uv_err_name(rc) << std::endl;
      delete victim;
      return false;
    }

    pump_until(&loop, [&] { return deleted; }, 5000);

    // 先把收尾队列拨干净**再读计数**：`deferred` 那一条是记在延迟的
    // `delete` 里的，没拨完就还是 0（那不是漏，是还没到）。
    uvcpp_test::drain(&loop);

    const uvcpp_tcp_client::reclaim_stat after =
        uvcpp_tcp_client::reclaim_stats();
    const uint64_t d_deferred = after.deferred - before.deferred;
    const uint64_t d_released = after.released - before.released;
    const uint64_t d_skipped  = after.skipped - before.skipped;

    if (victim != nullptr || !deleted) {
      std::cout << "[functional tcp_client] reclaim_paths leg1 FAIL "
                   "链路没走完：deleted=" << deleted
                << " victim=" << static_cast<void*>(victim) << std::endl;
      ok = false;
    } else if (d_deferred != 1 || d_released != 0 || d_skipped != 0) {
      std::cout << "[functional tcp_client] reclaim_paths leg1 FAIL 支路不对："
                   "deferred+=" << d_deferred << " released+=" << d_released
                << " skipped+=" << d_skipped << "（应 1/0/0）" << std::endl;
      ok = false;
    } else {
      std::cout << "[functional tcp_client] reclaim_paths leg1 deferred "
                   "survived\n";
    }

    // 独立证人：包装对象真被拆干净了，handle_queue 才是空的。老写法（一个
    // 都不拆）漏掉的那个 `uv_handle_t` 会一直挂在队列上 —— 既不活跃也没在
    // 关，`uv_run` 连 while 体都不进，这里只能 EBUSY。
    const int lrc = loop.loop_close();
    if (lrc != 0) {
      std::cout << "[functional tcp_client] reclaim_paths leg1 FAIL "
                   "loop_close rc=" << lrc << "（UV_EBUSY = 还有句柄挂在 "
                   "handle_queue 上）" << std::endl;
      ok = false;
    }
  }

  // ---- 腿 2：自己关，完成回调里删（句柄已摘 ⇒ released）----
  {
    uvcpp_loop loop;

    uvcpp_test::loop_drain drain_loop(&loop);
    loop.init();

    const uvcpp_tcp_client::reclaim_stat before =
        uvcpp_tcp_client::reclaim_stats();

    uvcpp_tcp_client* victim    = new uvcpp_tcp_client(&loop);
    bool              deleted   = false;
    bool              connected = false;

    victim->set_close_manager([&victim, &deleted]() {
      delete victim;
      victim  = nullptr;
      deleted = true;
    });

    int rc = victim->connect("127.0.0.1", port,
                             [&](int status) { connected = (status == 0); });
    if (rc != 0) {
      std::cout << "[functional tcp_client] reclaim_paths leg2 connect start "
                   "failed: " << uv_err_name(rc) << std::endl;
      delete victim;
      return false;
    }
    pump_until(&loop, [&] { return connected || deleted; }, 5000);

    if (!connected || victim == nullptr) {
      std::cout << "[functional tcp_client] reclaim_paths leg2 FAIL 没连上："
                   "connected=" << connected << std::endl;
      ok = false;
    } else {
      // 主动关：完成回调里 `fire_close_callbacks()` 会跑管理槽，而那一刻
      // `uvcpp_handle::callback_close` 已经把 `_handle` 置空了。
      victim->close();
      pump_until(&loop, [&] { return deleted; }, 5000);

      uvcpp_test::drain(&loop);

      const uvcpp_tcp_client::reclaim_stat after =
          uvcpp_tcp_client::reclaim_stats();
      const uint64_t d_released = after.released - before.released;
      const uint64_t d_deferred = after.deferred - before.deferred;
      const uint64_t d_skipped  = after.skipped - before.skipped;

      if (victim != nullptr || !deleted) {
        std::cout << "[functional tcp_client] reclaim_paths leg2 FAIL "
                     "链路没走完：deleted=" << deleted
                  << " victim=" << static_cast<void*>(victim) << std::endl;
        ok = false;
      } else if (d_released != 1 || d_deferred != 0 || d_skipped != 0) {
        std::cout << "[functional tcp_client] reclaim_paths leg2 FAIL 支路不对："
                     "released+=" << d_released << " deferred+=" << d_deferred
                  << " skipped+=" << d_skipped << "（应 1/0/0）" << std::endl;
        ok = false;
      } else {
        std::cout << "[functional tcp_client] reclaim_paths leg2 released "
                     "survived\n";
      }
    }

    const int lrc = loop.loop_close();
    if (lrc != 0) {
      std::cout << "[functional tcp_client] reclaim_paths leg2 FAIL "
                   "loop_close rc=" << lrc << std::endl;
      ok = false;
    }
  }

  // ---- 腿 3：传统 `read_start` 上析构（`read_arg_` 非空 ⇒ 释放必须推迟）----
  //
  // 腿 1/2 走的都是 `read_start_events` —— 那条路的数据由成员 `net_read_cb_`
  // 交付，`read_arg_` **恒为空**。而传统 `read_start` 才是 `read_arg_` 非空的
  // 那条路：它存的就是**此刻正在执行的那个 `std::function`**（见
  // `uvcpp_tcp_client.cpp` 原始读路径那段注释）。于是"把 `read_arg_` 的释放
  // 推迟到关闭完成回调"这件事只有这条腿才测得到 —— 而 webapp 那条 churn
  // 复现（读回调里析构、`read_arg_ != nullptr`）走的正是它。
  //
  // 判据与腿 1 相同，但**抓的不是同一个东西**：这条腿上"当场删 `read_arg_`"
  // 是删一个正在执行的对象，PageHeap 下必崩（腿 1 上则完全看不见）。
  {
    uvcpp_loop loop;

    uvcpp_test::loop_drain drain_loop(&loop);
    loop.init();

    const uvcpp_tcp_client::reclaim_stat before =
        uvcpp_tcp_client::reclaim_stats();

    uvcpp_tcp_client* victim  = new uvcpp_tcp_client(&loop);
    bool              deleted = false;

    victim->set_close_manager([&victim, &deleted]() {
      delete victim;
      victim  = nullptr;
      deleted = true;
    });

    int rc = victim->connect("127.0.0.1", hangup_port, [&](int status) {
      if (status != 0) {
        std::cout << "[functional tcp_client] reclaim_paths leg3 connect "
                  << uv_err_name(status) << std::endl;
        return;
      }
      // 传统读注册：这一句就是让 `read_arg_` 变成非空的那件事。
      victim->read_start([](uvcpp_buf* /*b*/) {});
    });
    if (rc != 0) {
      std::cout << "[functional tcp_client] reclaim_paths leg3 connect start "
                   "failed: " << uv_err_name(rc) << std::endl;
      delete victim;
      return false;
    }

    pump_until(&loop, [&] { return deleted; }, 5000);
    uvcpp_test::drain(&loop);

    const uvcpp_tcp_client::reclaim_stat after =
        uvcpp_tcp_client::reclaim_stats();
    const uint64_t d_deferred = after.deferred - before.deferred;
    const uint64_t d_released = after.released - before.released;
    const uint64_t d_skipped  = after.skipped - before.skipped;

    if (victim != nullptr || !deleted) {
      std::cout << "[functional tcp_client] reclaim_paths leg3 FAIL "
                   "链路没走完：deleted=" << deleted
                << " victim=" << static_cast<void*>(victim) << std::endl;
      ok = false;
    } else if (d_deferred != 1 || d_released != 0 || d_skipped != 0) {
      std::cout << "[functional tcp_client] reclaim_paths leg3 FAIL 支路不对："
                   "deferred+=" << d_deferred << " released+=" << d_released
                << " skipped+=" << d_skipped << "（应 1/0/0）" << std::endl;
      ok = false;
    } else {
      std::cout << "[functional tcp_client] reclaim_paths leg3 deferred "
                   "survived (raw read_start)\n";
    }

    const int lrc = loop.loop_close();
    if (lrc != 0) {
      std::cout << "[functional tcp_client] reclaim_paths leg3 FAIL "
                   "loop_close rc=" << lrc << std::endl;
      ok = false;
    }
  }

  std::cout << "[functional tcp_client] reclaim_paths done success="
            << (ok ? "true" : "false") << std::endl;
  return ok;
}

// =========================================================================
// 析构里**不许有睡眠**：判据是绝对墙钟，不是"A 比 B 快"
// =========================================================================
/**
 * 改前 `~uvcpp_tcp_client` 在"句柄还开着"那条路上是
 *
 *     while (!close_done) { run(NOWAIT); 看墙钟; sleep(1ms); }
 *
 * 注意 `sleep(1ms)` 在**条件复查之前** —— 于是每一次"连着、没关就析构"都
 * 至少花 1 毫秒，一次也躲不掉。这不是"慢一点"，是在事件循环线程上做阻塞等待
 * （本仓库的硬约束），而且它把一次终结拖成最多 5000 次系统调用 + 5 秒墙钟。
 *
 * 判据就取那个 1 毫秒硬下界：**多轮里的最小值**（平均值/总和会被负载噪声和
 * 别的东西一起抬高，最小值不会 —— 这正是抓"平坦成本"该用的量），
 * `min < 1ms` 才算过。改后是 64 轮紧挨着的 `UV_RUN_NOWAIT`（微秒级），
 * 留出的余量是几十倍。
 *
 * 计时区**只包住析构**：`connect_wait()` 自己会拨循环（那是同步 API 的本分），
 * 不能算进来。
 */
static bool test_destructor_has_no_sleep(int port) {
  std::cout << "[functional tcp_client] destructor_no_sleep start\n";

  const int rounds = 30;
  double    best_ms = 1e9;
  bool      ok = true;

  for (int i = 0; i < rounds; ++i) {
    uvcpp_tcp_client* c = new uvcpp_tcp_client();  // 自持 loop
    int rc = c->connect_wait("127.0.0.1", port, 5000);
    if (rc != 0) {
      std::cout << "[functional tcp_client] destructor_no_sleep connect failed: "
                << uv_err_name(rc) << std::endl;
      delete c;
      return false;
    }

    const auto t0 = std::chrono::steady_clock::now();
    delete c;  // <-- 连着、没读过、没关过：走的就是那条"关闭 + 泵"的路
    const auto t1 = std::chrono::steady_clock::now();

    const double ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
            t1 - t0)
            .count();
    if (ms < best_ms) best_ms = ms;
  }

  std::cout << "[functional tcp_client] destructor_no_sleep best=" << best_ms
            << "ms over " << rounds << " rounds (阈值 1ms)" << std::endl;
  if (best_ms >= 1.0) {
    std::cout << "[functional tcp_client] destructor_no_sleep FAIL: 析构里"
                 "还有秒级/毫秒级的等待\n";
    ok = false;
  }

  std::cout << "[functional tcp_client] destructor_no_sleep done success="
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

  // 断开用的服务端（只给 delete_in_read_cb 的腿 2 用）
  std::promise<int> hangup_promise;
  auto hangup_future = hangup_promise.get_future();
  std::atomic<bool> hangup_ready(false);
  std::atomic<bool> stop_hangup(false);
  std::thread hangup_thread(run_hangup_server, std::ref(hangup_promise),
                            std::ref(hangup_ready), std::ref(stop_hangup));

  int hangup_port = hangup_future.get();
  std::cout << "[functional tcp_client] hangup server on port " << hangup_port
            << std::endl;

  while (!hangup_ready.load()) {
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
  all_ok = test_delete_client_in_read_cb(port, hangup_port) && all_ok;
  all_ok = test_reclaim_paths(port, hangup_port) && all_ok;
  all_ok = test_destructor_has_no_sleep(port) && all_ok;

  // Stop server
  stop_server.store(true);
  server_thread.join();
  stop_hangup.store(true);
  hangup_thread.join();

  std::cout << "[functional tcp_client] done success="
            << (all_ok ? "true" : "false") << std::endl;
  return all_ok ? 0 : 2;
}
