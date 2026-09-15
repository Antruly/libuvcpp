/**
 * @file tests/functional/tcp_server_close_func.cpp
 * @brief 服务端**主动**关闭连接时，客户端必须被摘除并释放。
 *
 * 背景
 * ----
 * 断开有两条路径，而框架的释放原本只挂在其中一条上：
 *
 *  1. **对端断开** —— libuv 的读回调以 `nread < 0` 结束，`fire_close_callbacks()`
 *     在这个分支里跑，服务端的 close manager 摘除 + `delete`。
 *  2. **服务端主动关闭** —— 调用方自己关掉句柄。这条路径上**没有任何人通知
 *     close manager**，于是客户端对象留在 `clients_` 里，永远不会被摘除、
 *     也永远不会被 `delete`；句柄虽然关了，对象和它的簿记一直留在进程里。
 *
 * 谁走第二条路：`uvcpp_http_server::close_connection()`（解析错误、以及每个
 * `Connection: close` 响应写完之后的收尾）、websocket 的 `send_close()`、
 * 框架层的 `abort()`、优雅关闭、闲置超时。也就是说**每一个非 keep-alive 的
 * HTTP 响应都会漏一个客户端对象** —— 跑得越多，进程涨得越快。
 *
 * 修法：`uvcpp_tcp_client::close()` —— 主动关闭的**唯一正确入口**。它在关闭
 * 完成回调里依次跑「调用方的收尾」和框架的关闭回调（摘除 + 释放），于是两
 * 条断开路径在下游完全一致。直接 `get_tcp()->close(...)` 会绕过它，仍然泄漏。
 *
 * 这个文件守的就是它：服务端主动关掉一条连接，转完循环之后 `client_count()`
 * 必须回到 0。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#include <uv.h>

#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "handle/uvcpp_timer.h"
#include "net/uvcpp_tcp_client.h"
#include "net/uvcpp_tcp_server.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// 客户端与用户 on_close 回调的观察结果。
struct close_observation {
  std::atomic<int> connected;
  std::atomic<int> user_close_cb;   ///< 用户 set_on_close 被触发的次数
  std::atomic<int> client_saw_close;  ///< 客户端自己观察到连接关闭

  close_observation()
      : connected(0), user_close_cb(0), client_saw_close(0) {}
};

struct probe_state {
  std::atomic<int> accepted;           ///< on_connection 次数
  std::atomic<int> closed_by_server;   ///< 服务端主动 close 了几次
  std::atomic<int> count_after_loop;   ///< 转完循环之后读到的 client_count
  std::atomic<int> still_owns;         ///< 那之后服务端还认不认这个客户端
  std::atomic<int> owns_during;        ///< close 之前它确实归服务端管

  probe_state()
      : accepted(0),
        closed_by_server(0),
        count_after_loop(-1),
        still_owns(-1),
        owns_during(-1) {}
};

/**
 * @brief 服务端线程：接受一条连接，然后由**服务端**把它关掉。
 */
void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                probe_state* st) {
  uvcpp_tcp_server server;

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int port = ntohs(name.sin_port);

  // 要在 on_connection 之后才能关它，所以先把指针存下来。
  uvcpp_tcp_client* held = nullptr;

  rc = server.listen(
      [&](uvcpp_tcp_client* client) {
        st->accepted.fetch_add(1);
        held = client;
      },
      128);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }
  // **端口必须在 listen() 成功之后才放行**：调用方拿到端口就立刻 connect，
  // 而"已 bind、尚未 listen"的 socket 在内核里是**拒连**的（ECONNREFUSED），
  // 不是排队等 listen。顺带：放行在前的话，listen 失败时这里会第二次
  // set_value，那是 std::future_error —— 在服务线程里抛出去就是 terminate。
  port_promise.set_value(port);

  uvcpp_loop* loop = server.get_loop();

  // 等客户端连上来 —— 不能靠固定圈数，那会和客户端的连接时机赛跑。
  for (int i = 0; i < 5000 && held == nullptr; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (held == nullptr) {
    st->count_after_loop.store(static_cast<int>(server.client_count()));
    while (!stop.load()) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return;
  }

  st->owns_during.store(server.owns_client(held) ? 1 : 0);

  // **被测路径**：服务端主动关闭。
  //
  // 走 `client->close()` 而不是 `client->get_tcp()->close()`：后者不通知
  // close manager，客户端会被漏在登记表里（这正是本次要修的缺陷）。
  st->closed_by_server.fetch_add(1);
  held->close();

  // 转够久，让关闭的完成回调跑完。
  for (int i = 0; i < 300; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 判据：连接已经关了，登记表必须回到 0。
  st->count_after_loop.store(static_cast<int>(server.client_count()));
  st->still_owns.store(server.owns_client(held) ? 1 : 0);

  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

/// 客户端：连上去挂住，等服务端来关它，并记录自己观察到的关闭。
void run_client(int port, close_observation& obs, uvcpp_tcp_client** make_done) {
  uvcpp_tcp_client client;

  client.set_on_close([&obs]() { obs.user_close_cb.fetch_add(1); });

  int rc = client.connect("127.0.0.1", port, [&](int status) {
    if (status != 0) return;
    obs.connected.store(1);
    // **对端关闭只有读得见**：libuv 是在读回调的 `nread < 0` 分支里报告
    // EOF 的，一个不读的连接即使设了 on_close 也永远不会被通知。这是
    // 客户端侧的同一个坑，所以在连上之后注册一个忽略数据的读。
    client.read_start_events(
        [](uvcpp_tcp_client&, const net_read_result&) {});
  });
  if (rc != 0) {
    *make_done = nullptr;
    return;
  }

  uvcpp_loop* loop = client.get_loop();
  // 一直转，直到关闭回调落地或者超时（服务端在另一端等着我们连上去）。
  for (int i = 0; i < 3000 && obs.user_close_cb.load() == 0; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  obs.client_saw_close.store(obs.user_close_cb.load() > 0 ? 1 : 0);

  // 让 close 事件彻底跑完再析构。
  for (int i = 0; i < 50; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// =========================================================================
// [1] 服务端主动关闭 → 客户端被摘除并释放
// =========================================================================

void test_server_side_close_releases_client() {
  std::cout << "[1] 服务端主动关闭连接" << std::endl;

  probe_state st;
  close_observation obs;
  std::atomic<bool> stop(false);

  std::promise<int> port_promise;
  std::future<int> port_future = port_promise.get_future();

  std::thread server_thread(run_server, std::ref(port_promise),
                            std::ref(stop), &st);

  const int port = port_future.get();
  check(port > 0, "服务端绑定成功");
  if (port <= 0) {
    stop.store(true);
    server_thread.join();
    return;
  }

  uvcpp_tcp_client* make_done = nullptr;
  std::thread client_thread(run_client, port, std::ref(obs), &make_done);
  client_thread.join();

  // 服务端那 300 圈转完还要一会儿，等它把结论写下来。
  for (int i = 0; i < 2000 && st.count_after_loop.load() < 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  stop.store(true);
  server_thread.join();

  check(st.accepted.load() == 1, "服务端接受了一条连接");
  check(obs.connected.load() == 1, "客户端连上了");
  check(st.owns_during.load() == 1, "关闭之前该客户端归服务端管");
  check(st.closed_by_server.load() == 1, "服务端主动关掉了它");
  check(obs.client_saw_close.load() == 1,
        "客户端观察到了关闭（说明服务端那次 close 真的生效了）");

  // 这两条是核心判据：关闭之后，对象必须被摘除 + 释放。
  check(st.count_after_loop.load() == 0,
        "**关闭之后 client_count() 回到 0**（实际 " +
            std::to_string(st.count_after_loop.load()) +
            " —— 大于 0 就是每个服务端主动关闭的连接泄漏一个客户端对象）");
  check(st.still_owns.load() == 0,
        "关闭之后服务端不再认为它归自己管（实际 " +
            std::to_string(st.still_owns.load()) + "）");
}

// =========================================================================
// [2] 主动关闭时，用户的 on_close 回调仍然要跑
//
// 这是「用户的观察槽和框架的管理槽分开」的另一半：修好释放不能以跳过用户
// 回调为代价。
// =========================================================================

void test_user_close_callback_still_fires() {
  std::cout << "[2] 主动关闭时用户 on_close 仍然触发" << std::endl;

  probe_state st;
  close_observation obs;
  std::atomic<bool> stop(false);

  std::promise<int> port_promise;
  std::future<int> port_future = port_promise.get_future();

  std::thread server_thread(run_server, std::ref(port_promise),
                            std::ref(stop), &st);

  const int port = port_future.get();
  check(port > 0, "服务端绑定成功");
  if (port <= 0) {
    stop.store(true);
    server_thread.join();
    return;
  }

  uvcpp_tcp_client* make_done = nullptr;
  std::thread client_thread(run_client, port, std::ref(obs), &make_done);
  client_thread.join();

  for (int i = 0; i < 2000 && st.count_after_loop.load() < 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  stop.store(true);
  server_thread.join();

  check(obs.user_close_cb.load() >= 1,
        "客户端的 on_close 触发了（实际 " +
            std::to_string(obs.user_close_cb.load()) + " 次）");
  check(st.count_after_loop.load() == 0, "释放照旧发生");
}

// =========================================================================
// [3] 连着来三条：每一条都要被释放，不是只有第一条
//
// 这条测的是「释放是每连接的，而不是一次性的」。泄漏的实现在这里会看到
// client_count() 一路涨上去（1、2、3），而在最后一条之后仍然非 0。
// =========================================================================

void run_server_rounds(std::promise<int>& port_promise, std::atomic<bool>& stop,
                       std::atomic<int>* round_done,
                       std::atomic<int>* max_count_after_close,
                       std::atomic<int>* accepted) {
  uvcpp_tcp_server server;

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int port = ntohs(name.sin_port);

  uvcpp_tcp_client* held = nullptr;
  rc = server.listen(
      [&](uvcpp_tcp_client* client) {
        accepted->fetch_add(1);
        held = client;
      },
      128);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }
  // **端口必须在 listen() 成功之后才放行**：调用方拿到端口就立刻 connect，
  // 而"已 bind、尚未 listen"的 socket 在内核里是**拒连**的（ECONNREFUSED），
  // 不是排队等 listen。顺带：放行在前的话，listen 失败时这里会第二次
  // set_value，那是 std::future_error —— 在服务线程里抛出去就是 terminate。
  port_promise.set_value(port);

  uvcpp_loop* loop = server.get_loop();

  for (int round = 0; round < 3; ++round) {
    // 等这一轮的客户端连上来。
    for (int i = 0; i < 5000 && accepted->load() <= round; ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (accepted->load() <= round) break;

    held->close();

    // 转够，让关闭 + 释放跑完，然后记录这一轮结束之后的登记数。
    for (int i = 0; i < 200; ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (server.client_count() > static_cast<size_t>(max_count_after_close->load())) {
      max_count_after_close->store(
          static_cast<int>(server.client_count()));
    }
    round_done->store(round + 1);
  }

  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

/// 连上一条，等对方来关，然后走人。
void connect_and_wait_close(int port) {
  uvcpp_tcp_client client;
  std::atomic<bool> closed(false);
  client.set_on_close([&closed]() { closed.store(true); });

  if (client.connect("127.0.0.1", port, [&client](int status) {
        if (status != 0) return;
        client.read_start_events(
            [](uvcpp_tcp_client&, const net_read_result&) {});
      }) != 0) {
    return;
  }

  uvcpp_loop* loop = client.get_loop();
  for (int i = 0; i < 3000 && !closed.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 50; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void test_every_connection_is_released() {
  std::cout << "[3] 三条连接逐条主动关闭，条条都要释放" << std::endl;

  std::atomic<bool> stop(false);
  std::atomic<int> round_done(0);
  std::atomic<int> max_count_after_close(0);
  std::atomic<int> accepted(0);

  std::promise<int> port_promise;
  std::future<int> port_future = port_promise.get_future();

  std::thread server_thread(run_server_rounds, std::ref(port_promise),
                            std::ref(stop), &round_done,
                            &max_count_after_close, &accepted);

  const int port = port_future.get();
  check(port > 0, "服务端绑定成功");
  if (port <= 0) {
    stop.store(true);
    server_thread.join();
    return;
  }

  for (int round = 0; round < 3; ++round) {
    std::thread t(connect_and_wait_close, port);
    t.join();
    // 这一轮的服务端收尾跑完再进下一轮。
    for (int i = 0; i < 2000 && round_done.load() <= round; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  stop.store(true);
  server_thread.join();

  check(accepted.load() == 3, "服务端接受了三条连接（实际 " +
                                  std::to_string(accepted.load()) + "）");
  check(max_count_after_close.load() == 0,
        "**每一轮关闭之后 client_count() 都是 0**（观测到的最大残留 " +
            std::to_string(max_count_after_close.load()) +
            " —— 大于 0 就是对端已经关掉、对象却还留在登记表里）");
}

// =========================================================================
// [4] close_all_clients()：送走全部已登记连接，但**不停监听**
// =========================================================================

void run_server_close_all(std::promise<int>& port_promise,
                          std::atomic<bool>& stop, std::atomic<int>* accepted,
                          std::atomic<int>* closed_first,
                          std::atomic<int>* count_after_first,
                          std::atomic<int>* closed_second,
                          std::atomic<int>* listening_still,
                          std::atomic<int>* round_done) {
  uvcpp_tcp_server server;

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int port = ntohs(name.sin_port);

  rc = server.listen([&](uvcpp_tcp_client*) { accepted->fetch_add(1); }, 128);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }
  // 见 run_server_rounds 里的同一条说明：端口只能在 listen() 之后放行。
  port_promise.set_value(port);

  uvcpp_loop* loop = server.get_loop();

  // --- 第一轮：两条连接一起送走 ---
  for (int i = 0; i < 5000 && accepted->load() < 2; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  closed_first->store(static_cast<int>(server.close_all_clients()));

  for (int i = 0; i < 300; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  count_after_first->store(static_cast<int>(server.client_count()));
  listening_still->store(
      server.has_status(TCP_SERVER_LISTENING) ? 1 : 0);
  round_done->fetch_add(1);

  // --- 第二轮：监听还在，所以还能收新连接 ---
  for (int i = 0; i < 5000 && accepted->load() < 3; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  closed_second->store(static_cast<int>(server.close_all_clients()));

  for (int i = 0; i < 300; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  round_done->fetch_add(1);

  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void test_close_all_clients() {
  std::cout << "[4] close_all_clients：全部送走但不停监听" << std::endl;

  std::atomic<bool> stop(false);
  std::atomic<int> accepted(0);
  std::atomic<int> closed_first(0);
  std::atomic<int> count_after_first(-1);
  std::atomic<int> closed_second(0);
  std::atomic<int> listening_still(0);
  std::atomic<int> round_done(0);

  std::promise<int> port_promise;
  std::future<int> port_future = port_promise.get_future();

  std::thread server_thread(run_server_close_all, std::ref(port_promise),
                            std::ref(stop), &accepted, &closed_first,
                            &count_after_first, &closed_second,
                            &listening_still, &round_done);

  const int port = port_future.get();
  check(port > 0, "服务端绑定成功");
  if (port <= 0) {
    stop.store(true);
    server_thread.join();
    return;
  }

  // 两条连接挂上去，等服务端把它们一起关掉。
  std::thread c1(connect_and_wait_close, port);
  std::thread c2(connect_and_wait_close, port);
  c1.join();
  c2.join();
  for (int i = 0; i < 2000 && round_done.load() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  // 监听没停：再连一条，它照样该被接受。
  std::thread c3(connect_and_wait_close, port);
  c3.join();
  for (int i = 0; i < 2000 && round_done.load() < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  stop.store(true);
  server_thread.join();

  check(accepted.load() == 3,
        "两条关完之后监听还在，第三条仍被接受（实际接受 " +
            std::to_string(accepted.load()) + " 条）");
  check(closed_first.load() == 2,
        "close_all_clients() 报告关掉了 2 条（实际 " +
            std::to_string(closed_first.load()) + "）");
  check(count_after_first.load() == 0,
        "第一轮之后登记表清空（实际 " +
            std::to_string(count_after_first.load()) + "）");
  check(listening_still.load() == 1, "close_all_clients 之后仍在监听");
  check(closed_second.load() == 1,
        "第二轮只关掉新来的那一条（实际 " +
            std::to_string(closed_second.load()) + "）");
}

// =========================================================================
// [5] 登记表里有"句柄已关、人还留着"的残留时，close_all_clients 不能崩
//
// 这种残留是**绕过框架直接关句柄**的产物：[4] 的实测表明，对一个句柄已经
// 关掉的连接再调一次 close，如果实现先去问 `is_closing()`，那就是空指针
// 解引用（`uvcpp_handle::is_closing()` 把句柄指针直接交给 libuv，而关完之后
// 它是 nullptr）—— 当场段错误。
//
// 场景是构造出来的：先用 `get_tcp()->close()` 关掉一条连接（故意绕过框架），
// 再调 `close_all_clients()`。正确行为是**不崩**，并且把这条残留顺手归还。
// =========================================================================

void test_close_all_sweeps_dead_entries() {
  std::cout << "[5] 句柄已关的残留不能让 close_all_clients 崩" << std::endl;

  uvcpp_tcp_server server;
  int rc = server.bindIpv4("127.0.0.1", 0);
  check(rc == 0, "服务端绑定成功");
  if (rc != 0) return;

  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int port = ntohs(name.sin_port);

  std::atomic<int> accepted(0);
  uvcpp_tcp_client* held = nullptr;
  rc = server.listen(
      [&](uvcpp_tcp_client* client) {
        accepted.fetch_add(1);
        held = client;
      },
      128);
  check(rc == 0, "监听成功");
  if (rc != 0) return;

  std::thread client_thread(connect_and_wait_close, port);

  uvcpp_loop* loop = server.get_loop();
  for (int i = 0; i < 5000 && held == nullptr; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(held != nullptr, "服务端接受了一条连接");

  if (held != nullptr) {
    // **故意绕过框架**：直接关句柄。这样 close manager 不会被通知，
    // 客户端对象留在登记表里，而它的句柄已经关完（get_handle() == nullptr）。
    held->get_tcp()->close([](uvcpp_handle*) {});
    for (int i = 0; i < 300; ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    check(server.client_count() == 1,
          "前提成立：这条连接被绕过框架地关掉，人还留在登记表里（实际 " +
              std::to_string(server.client_count()) + "）");

    // 直接对一个句柄已经关掉的连接调 close()：必须**安全拒绝**。
    //
    // 这一句就是空指针解引用的落点 —— `uvcpp_handle::is_closing()` 会把
    // 句柄指针交给 libuv 的 `uv_is_closing()`，而关完之后它是 nullptr。
    check(held->close() == UV_EINVAL,
          "对已经关掉的连接调 close() 安全返回 UV_EINVAL（实际 " +
              std::to_string(held->close()) + "）");

    // 这一句在修好之前是段错误的落点。
    const size_t n = server.close_all_clients();
    check(n == 0, "已经没有活着的句柄可关，返回 0（实际 " + std::to_string(n) +
                      "）");
    check(server.client_count() == 0,
          "残留被顺手归还，登记表清空（实际 " +
              std::to_string(server.client_count()) + "）");
  }

  client_thread.join();
}

}  // namespace

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  test_server_side_close_releases_client();
  test_user_close_callback_still_fires();
  test_every_connection_is_released();
  test_close_all_clients();
  test_close_all_sweeps_dead_entries();

  if (g_failures == 0) {
    std::cout << "[tcp_server_close] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[tcp_server_close] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}
