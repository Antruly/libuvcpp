/**
 * @file tests/functional/web_ssl_server_func.cpp
 * @brief `uvcpp_tcp_server` 的每连接 TLS（`set_ssl_context`）。
 *
 * `web_ssl_client_func.cpp` 覆盖的是**客户端**那条路径，服务端角色在里面是用
 * `on_connection` 里手动 `enable_tls()` 模拟的 —— 那一步恰好就是本文件要考的
 * 东西，所以它证明不了"服务端内建了 TLS"。本文件用真正的
 * `set_ssl_context()`，并且专门钉住**交付时机**这个约定：
 *
 * > 握手没完成，`on_connection` 就不许被调用。
 *
 * 为什么这条约定值得单独立一个用例：它错起来是**静默**的。如果实现一 accept
 * 就把连接交出去，上层（http 解析器）会在 TLS 还没建立时就开始工作，而
 * `write()` 在握手完成前必然失败（SSL_write 要求握手已完成）。表现是"服务端
 * 收到了连接、然后什么都不发生" —— 没有崩溃、没有错误码，只有一条永远等不到
 * 响应的连接。所以判据必须是**在 `on_connection` 里当场断言握手已完成**，
 * 并且**当场写一个问候语且写成功** —— 后者是行为证据，不只是读标志位。
 *
 * 另一条判据同样必要：**反向对照**。明文客户端打 TLS 服务端时，`on_connection`
 * 必须**一次都不被调用**，而且这条连接必须被回收（`client_count()==0`）。
 * 少了"被回收"这一半，一个"握手失败就把它晾在那儿"的实现照样能让用例通过。
 *
 * 第三个场景是对照组：没设 `ssl_context()` 的服务端必须照旧立即交付、照旧
 * 收发明文 —— 否则"装了 TLS 的那套逻辑"可能被无条件走到了所有连接上。
 *
 * 服务端在两个方向上都只写**一次**（`on_connection` 里的问候语），不做回显：
 * `uvcpp_tcp_client::write` 同一时刻只允许一个在途写，第二笔会拿到
 * `UV_EALREADY` 并被丢弃。回显客户端稍后到的消息就会和这句问候语抢那个槽 ——
 * 那是个真实的竞态（问候语写完没有取决于循环转了几圈），会让用例偶发失败。
 * 客户端 → 服务端这个方向由服务端收到的**明文内容**来断言，一样强。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_OPENSSL_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>

#include <openssl/ssl.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// 客户端在循环里最多转这么久（每次 UV_RUN_NOWAIT + 1ms 睡眠）。
const int kClientTicks = 4000;

/// 服务端在 `on_connection` 里当场写出去的问候语。
///
/// 它同时是"交付时机正确"的**行为证据**：这一句 `write()` 能成功，就说明
/// 握手确实已经完成（SSL_write 在握手完成前不可能成功）。
const char kGreeting[] = "HELLO-FROM-TLS-SERVER\n";

/// 客户端发出去的请求。选一个像样的 HTTP 请求只是为了让"明文被正确解出来"
/// 这条断言读起来自然，内容本身不重要。
const char kRequest[] = "GET / HTTP/1.1\r\nHost: loopback.test\r\n\r\n";

// =========================================================================
// 服务端侧观测
// =========================================================================

struct server_state {
  std::atomic<int>  delivered{0};  ///< on_connection 被调用的次数
  std::atomic<int>  hs_done_at_delivery{0};  ///< 交付时握手已完成的次数
  std::atomic<int>  greeting_ok{0};          ///< 问候语写成功的次数
  std::atomic<int>  greeting_fail{0};
  std::atomic<int>  read_errors{0};          ///< 读侧收到 READ_ERROR 的次数
  std::atomic<int>  plain_bytes_in{0};       ///< 解出来的**明文**字节数
  std::atomic<int>  final_count{-1};         ///< 收尾后的 client_count()
  std::atomic<int>  final_last_error{0};     ///< 收尾后的 server.get_last_error()
  /**
   * @brief 服务端线程采样到的 `client_count()` 峰值。
   *
   * 反向场景靠它证明"这条连接真的到过服务端"——`delivered` 在那边恒为 0，
   * 光看它无法区分"被正确拒绝了"和"压根没连上"。
   */
  std::atomic<int>  max_clients{0};
  /**
   * @brief `listen()` 的返回值。
   *
   * 不能只看"端口拿到了"就当作服务端起来了：`listen()` 失败时 `run_server`
   * 会**直接返回**（服务端对象随之析构、端口随即关闭），而调用方从端口号上
   * 看不出任何异常 —— 客户端连过去只会拿到 ECONNREFUSED，被静默忽略，
   * 于是整个场景变成"什么都没发生"却依然"通过"。
   */
  std::atomic<int>  listen_rc{-1};
  /**
   * @brief 服务端读回调收到的**全部**事件数（含 DATA / PEER_CLOSED / READ_ERROR）。
   *
   * 反向场景靠它做**确定性**的非空证明：这条回调只由 accept 路径
   * （`setup_client_callbacks` → `read_start_events`）安装到每个连接上，
   * 所以它非零就等于"服务端受理过一条连接"。`max_clients` 是采样来的，
   * 采样会看漏（accept 与握手失败可能落在同一个轮询周期里）；这一个不会。
   */
  std::atomic<int>  read_events{0};

  std::mutex  mu;
  std::string received;  ///< 收到的全部明文
};

void on_server_read(server_state& st, const net_read_result& r) {
  st.read_events.fetch_add(1);
  if (!r.is_data()) {
    if (r.event == net_read_event::READ_ERROR) st.read_errors.fetch_add(1);
    return;
  }
  // 收到的是明文才作数：过滤器没接上的话这里会是密文，逐字节比对会失败。
  st.plain_bytes_in.fetch_add(static_cast<int>(r.size));
  std::lock_guard<std::mutex> lk(st.mu);
  st.received.append(r.data, r.size);
}

/// 服务端线程。`use_tls` 为假时**不**装 ssl_context（对照组）。
void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                server_state& st, uvcpp_ssl_context* sctx, bool use_tls) {
  uvcpp_tcp_server server;

  server.set_read_callback(
      [&st](uvcpp_tcp_client&, const net_read_result& r) {
        on_server_read(st, r);
      });

  if (use_tls) server.set_ssl_context(sctx);  // 必须在 listen() 之前

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int bound_port = ntohs(name.sin_port);

  rc = server.listen(
      [&st](uvcpp_tcp_client* client) {
        // **本文件的核心断言。** 走到这里就说明框架认为"可以用了" ——
        // 那么握手必须已经完成，而且必须当场写得出去东西。
        if (client->is_tls()) {
          if (client->is_tls_handshake_done()) st.hs_done_at_delivery.fetch_add(1);
        }
        const int wrc = client->write(
            kGreeting, sizeof(kGreeting) - 1,
            [&st](int status) {
              if (status == 0) {
                st.greeting_ok.fetch_add(1);
              } else {
                st.greeting_fail.fetch_add(1);
              }
            });
        if (wrc != 0) st.greeting_fail.fetch_add(1);

        st.delivered.fetch_add(1);
      },
      128);
  st.listen_rc.store(rc);
  // **端口必须在 `listen()` 之后才放行。**
  //
  // 原先这一句在 `bind()` 之后、`listen()` 之前，于是调用方拿到端口就立刻
  // `connect()` —— 而"已 bind、尚未 listen"的 socket 在内核里是**拒连**的
  // （RST → `ECONNREFUSED`，实测 `connect status = -4078`）。窗口很小但真实
  // 存在：30 次连跑挂 2 次（约 7%），而且因为三个场景各有一次 `run_server`，
  // 挂的是哪一组全看运气（观测到过场景 1 和场景 3 各一次）。
  //
  // 这是本文件里**同一族**的第三个"等 A 就立刻读 B"的问题（前两个是
  // `max_clients` 的采样快照和 `final_last_error` 在 `join()` 之前读）。
  // 它不属于被测代码 —— 放行信号的时机在用例这边。
  port_promise.set_value(rc != 0 ? -1 : bound_port);
  if (rc != 0) return;

  uvcpp_loop* loop = server.get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    // 采样登记表（见 max_clients 的说明）。
    const int live = static_cast<int>(server.client_count());
    if (live > st.max_clients.load()) st.max_clients.store(live);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 让挂起的关闭事件跑完，再读登记表 —— 否则读到的是还没摘除的中间态。
  for (int i = 0; i < 400; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  st.final_count.store(static_cast<int>(server.client_count()));
  st.final_last_error.store(server.get_last_error());
}

// =========================================================================
// 客户端侧驱动
// =========================================================================

struct client_probe {
  std::atomic<int>  connect_status{-99};
  std::atomic<bool> connect_fired{false};
  std::atomic<bool> hs_done_at_connect{false};
  std::atomic<int>  write_status{-99};
  std::atomic<int>  read_error{0};
  std::atomic<int>  read_arm_rc{-999};
  /// @brief `run_plaintext_client` 里"握着之后那笔写"的返回值。
  ///
  /// 它是**明文确实发出去了**的证据：这一笔返回非 0 就说明连接在握手失败
  /// 之前就已经不可写，反向场景考的东西就不是"TLS 拒绝了明文"了。
  std::atomic<int>  hold_write_rc{-999};

  std::mutex  mu;
  std::string received;
};

/// 跑一个客户端（`cctx` 为 nullptr 时不装 TLS），发 `msg`，等 `want_len`
/// 字节回来。返回时连接已关闭。
void run_client(int port, uvcpp_ssl_context* cctx, const std::string& msg,
                size_t want_len, client_probe& p, const char* label) {
  uvcpp_tcp_client client;

  if (cctx != nullptr) {
    const int trc = client.enable_tls(cctx);
    check(trc == 0,
          std::string(label) + ": enable_tls returned " + std::to_string(trc));
  }

  const int crc = client.connect(
      "127.0.0.1", port, [&client, &p, msg](int status) {
        p.connect_status.store(status);
        p.hs_done_at_connect.store(client.is_tls_handshake_done());
        p.connect_fired.store(true);
        if (status != 0) return;

        // **起读必须在这里，不能在 `connect()` 返回之后。**
        //
        // socket 还没连上时 `arm_async_read()` 会返回 `UV_ENOTCONN`（实测
        // -4053，来自 libuv `uv-common.c:929` 的 `UV_HANDLE_READABLE` 检查）。
        // 那个失败是**静默**的：回调被记下了，却没有任何人再去 arm 它 ——
        // 表现是"连接建立正常、请求也发出去了，就是一个字节都收不到"。
        //
        // TLS 那个场景侥幸躲过，是因为它的连接完成路径自己会 arm
        // （`uvcpp_tcp_client.cpp:577`，`arm_async_read()`）兜住了；明文路径
        // 没有那一步，于是这条用例第一次跑就栽在这里。
        p.read_arm_rc.store(client.read_start_events(
            [&p](uvcpp_tcp_client&, const net_read_result& r) {
              if (r.is_data()) {
                std::lock_guard<std::mutex> lk(p.mu);
                p.received.append(r.data, r.size);
              } else if (r.event == net_read_event::READ_ERROR) {
                p.read_error.fetch_add(1);
              }
            }));

        // 契约：拿到成功就应当能直接发明文。
        const int wrc =
            client.write(msg.data(), msg.size(),
                         [&p](int ws) { p.write_status.store(ws); });
        if (wrc != 0) p.write_status.store(wrc);
      });
  check(crc == 0, std::string(label) + ": connect() start failed");

  uvcpp_loop* loop = client.get_loop();
  for (int i = 0; i < kClientTicks; ++i) {
    loop->run(UV_RUN_NOWAIT);
    {
      std::lock_guard<std::mutex> lk(p.mu);
      if (p.received.size() >= want_len) break;
    }
    if (p.connect_fired.load() && p.connect_status.load() != 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  client.close();
  for (int i = 0; i < 60; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

/// 跑一个**明文**客户端打过去：连上 → **握着一会儿不发东西** → 再发明文 → 收尾。
///
/// 中间那段"握着不发"是为了让反向场景**非空**：服务端只要 accept 了就会把
/// 客户端登记进表里（登记排在 TLS 那一段之前），于是服务端线程采样到的
/// `client_count()` 会短暂变成 1 —— 这是"这条连接真的到过服务端"的证据。
/// 没有它，一个"客户端压根没连上"的运行也会让每一条 `delivered == 0` 通过。
///
/// 先发再等是抓不到这个证据的：明文一进 rbio 握手立刻失败，连接几毫秒内就
/// 被回收了，1ms 的采样很可能一次都看不见。
void run_plaintext_client(int port, const std::string& msg, client_probe& p) {
  uvcpp_tcp_client client;

  const int crc = client.connect(
      "127.0.0.1", port, [&p](int status) {
        p.connect_status.store(status);
        p.connect_fired.store(true);
        // **这里绝不能顺手写一笔。** 写了的话明文在连接建立的同一个轮询周期
        // 里就到达服务端：accept 和握手失败落进**同一次** `loop->run()`，而
        // 服务端线程只在两轮 `run()` 之间采样 `client_count()` —— 中间那个
        // "登记了但还没被拒"的窗口一次都看不见，`max_clients` 于是时有时无。
        // 留空才让下面那段"握着不发"真正成立。
      });
  check(crc == 0, "negative: connect() start failed");

  uvcpp_loop* loop = client.get_loop();

  // 先连上、什么都不发，让服务端把它登记进表里。
  for (int i = 0; i < 300; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // 再发明文 —— 握手此刻才失败。
  p.hold_write_rc.store(client.write(msg.data(), msg.size(), [](int) {}));
  for (int i = 0; i < 500; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  client.close();
  for (int i = 0; i < 60; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::string probe_received(client_probe& p) {
  std::lock_guard<std::mutex> lk(p.mu);
  return p.received;
}

std::string state_received(server_state& st) {
  std::lock_guard<std::mutex> lk(st.mu);
  return st.received;
}

}  // namespace

int main() {
  std::cout << "[web_ssl_server] OpenSSL "
            << OpenSSL_version(OPENSSL_VERSION_STRING) << std::endl;

  // ---- 证书 ---------------------------------------------------------
  uvcpp_ssl_context sctx(tls_mode::SERVER, tls_version::TLS_1_2);
  if (!sctx.is_ready() || !sctx.generate_self_signed("loopback.test", 2048)) {
    std::cerr << "  [FAIL] server ctx/cert: " << sctx.get_last_error()
              << std::endl;
    return 2;
  }

  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  if (!cctx.is_ready()) {
    std::cerr << "  [FAIL] client ctx not ready" << std::endl;
    return 2;
  }
  // 自签证书的回环用例需要它；**生产客户端不能这样**（默认接受任何证书等于
  // 中间人可直接接管）。显式写出来正是为了让它显眼。
  cctx.set_verify_mode(tls_verify_mode::NONE);

  const std::string request(kRequest, sizeof(kRequest) - 1);
  const size_t greeting_len = sizeof(kGreeting) - 1;

  // =====================================================================
  // 场景 1：TLS 服务端 —— 握手完成才交付，且当场就能发明文
  // =====================================================================
  {
    server_state      st;
    std::atomic<bool> stop{false};
    std::promise<int> port_promise;
    std::future<int>  port_future = port_promise.get_future();
    std::thread srv(run_server, std::ref(port_promise), std::ref(stop),
                    std::ref(st), &sctx, true);

    const int port = port_future.get();
    if (port <= 0) {
      std::cerr << "  [FAIL] server failed to bind" << std::endl;
      stop.store(true);
      srv.join();
      return 2;
    }

    client_probe p;
    run_client(port, &cctx, request, greeting_len, p, "tls_server_roundtrip");

    check(p.connect_fired.load(), "connect cb never fired");
    check(p.connect_status.load() == 0,
          "connect status = " + std::to_string(p.connect_status.load()));
    check(p.hs_done_at_connect.load(),
          "client: connect cb fired BEFORE handshake completed");
    check(p.write_status.load() == 0,
          "client write status = " + std::to_string(p.write_status.load()));

    const std::string got = probe_received(p);
    check(got == std::string(kGreeting),
          "tls_server_roundtrip: want " + std::to_string(greeting_len) +
              " bytes of greeting, got " + std::to_string(got.size()));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 服务端侧的交付时机。
    check(st.delivered.load() == 1,
          "server delivered " + std::to_string(st.delivered.load()) +
              " connections (want 1)");

    // **逐次**断言，而不是"至少有一次满足"。
    //
    // 写成 `hs_done_at_delivery == 1` 是可以被蒙过去的：一个"accept 就交付
    // 一次、握手完成再交付一次"的实现，第二次那笔照样让计数变成 1（实测
    // 变异 M1 正是如此 —— 它只被上面的 `delivered == 1` 抓住，这两条断言
    // 当时**没有**报错）。把等式两边都系在 `delivered` 上，"每一次交付都
    // 满足契约"才真正被要求，而且 M1 会在这里直接报出来。
    check(st.hs_done_at_delivery.load() == st.delivered.load(),
          "server: a connection was delivered BEFORE the handshake completed (" +
              std::to_string(st.hs_done_at_delivery.load()) + " of " +
              std::to_string(st.delivered.load()) + " deliveries)");
    check(st.greeting_ok.load() == st.delivered.load(),
          "server: a delivered connection could not write immediately (ok=" +
              std::to_string(st.greeting_ok.load()) + " fail=" +
              std::to_string(st.greeting_fail.load()) + " of " +
              std::to_string(st.delivered.load()) + " deliveries)");
    check(st.greeting_fail.load() == 0,
          "server: greeting write failed " +
              std::to_string(st.greeting_fail.load()) + " times");

    // 客户端 → 服务端这个方向：服务端必须收到**明文**。密文长度和内容都对不上。
    check(state_received(st) == request,
          "server received " + std::to_string(st.plain_bytes_in.load()) +
              " bytes of plaintext (want " + std::to_string(request.size()) +
              ")");

    stop.store(true);
    srv.join();

    check(st.final_count.load() == 0,
          "server client table not drained: " +
              std::to_string(st.final_count.load()) + " left");
  }

  // =====================================================================
  // 场景 2：反向对照 —— 明文客户端打 TLS 服务端
  //
  // 断言两件事，缺一不可：
  //   (a) `on_connection` **一次都不许被调用** —— 握手没成功就没有"连接";
  //   (b) 这条连接必须被回收 —— 否则"握手失败就晾在那儿"的实现也能通过。
  // =====================================================================
  {
    server_state      st;
    std::atomic<bool> stop{false};
    std::promise<int> port_promise;
    std::future<int>  port_future = port_promise.get_future();
    std::thread srv(run_server, std::ref(port_promise), std::ref(stop),
                    std::ref(st), &sctx, true);

    const int port = port_future.get();
    if (port <= 0) {
      std::cerr << "  [FAIL] server failed to bind (negative case)" << std::endl;
      stop.store(true);
      srv.join();
      return 2;
    }

    client_probe np;
    run_plaintext_client(port, request, np);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    check(st.listen_rc.load() == 0,
          "negative: server listen() failed with " +
              std::to_string(st.listen_rc.load()));
    check(np.connect_fired.load(), "negative: connect cb never fired");
    check(np.connect_status.load() == 0,
          "negative: connect status = " +
              std::to_string(np.connect_status.load()));
    check(np.hold_write_rc.load() == 0,
          "negative: the plaintext write was refused (" +
              std::to_string(np.hold_write_rc.load()) +
              "), so nothing was ever sent to reject");

    // ---- 先证明"这条连接真的到过服务端"，否则下面是空的 ----------------
    //
    // 两条独立的证据，缺一不可：
    //   (a) `client_count()` 峰值 —— 连接在登记表里待过（握手的 300ms 里）；
    //   (b) 每连接的读回调被触发过 —— 那条回调只由 accept 路径
    //       （`setup_client_callbacks` → `read_start_events`）安装，
    //       所以它非零就等于"服务端受理了这条连接并读了它"。
    // (a) 是采样来的（服务端线程逐轮 `run()` 之间取一次），(b) 是确定性的：
    // 采样可能因为 accept 与握手失败落进同一个轮询周期而看漏，(b) 不会。
    check(st.max_clients.load() >= 1,
          "negative: the server never registered a client — the plaintext "
          "client never reached it, making this scenario vacuous");
    check(st.read_events.load() >= 1,
          "negative: the server's per-client read path never ran, so this "
          "connection was never accepted (read_events=" +
              std::to_string(st.read_events.load()) + ", read_err=" +
              std::to_string(st.read_errors.load()) + ")");

    check(st.delivered.load() == 0,
          "negative: on_connection was called for a PLAINTEXT client (" +
              std::to_string(st.delivered.load()) + " times)");
    check(st.hs_done_at_delivery.load() == 0,
          "negative: handshake reported done for a plaintext client");
    check(st.plain_bytes_in.load() == 0,
          "negative: plaintext bytes were delivered as if decrypted (" +
              std::to_string(st.plain_bytes_in.load()) + " bytes)");

    stop.store(true);
    srv.join();

    // ---- 收尾（服务端线程已经退出，下面两个字段此刻才是最终值）--------
    //
    // **必须在 join() 之后读**：`final_count` / `final_last_error` 由服务端
    // 线程在 `stop` 之后写，join 之前读到的永远是初始值。
    //
    // 断言具体的 `UV_EPROTO` 而不是"非 0"：非 0 太松 —— 对端连上就断开
    // 也会让 `last_error_code_` 变成 `UV_EOF`（`tls_fail(err, peer_closed)`
    // 里 `err != 0` 那一支），那样就分不清"TLS 拒绝了明文"和"对端提前走了"。
    // `UV_EPROTO` 只可能来自 `tls_drive_handshake` 判定 `SSL_accept` 真出错
    // 那一支，正是本场景要钉的东西。
    check(st.final_last_error.load() == UV_EPROTO,
          "negative: server last_error = " +
              std::to_string(st.final_last_error.load()) +
              " (want UV_EPROTO=" + std::to_string(UV_EPROTO) +
              "), so the TLS layer never rejected the plaintext bytes");
    check(st.final_count.load() == 0,
          "negative: connection not reclaimed — " +
              std::to_string(st.final_count.load()) + " left in the table");
  }

  // =====================================================================
  // 场景 3：对照组 —— 不设 ssl_context 的服务端照旧立即交付、收发明文
  //
  // 没有这一条，一个"把所有连接都当 TLS 处理"的实现也能让上面两条通过
  // （场景 1 它照样能过，场景 2 它照样能拒绝）。
  // =====================================================================
  {
    server_state      st;
    std::atomic<bool> stop{false};
    std::promise<int> port_promise;
    std::future<int>  port_future = port_promise.get_future();
    std::thread srv(run_server, std::ref(port_promise), std::ref(stop),
                    std::ref(st), &sctx, false);  // use_tls = false

    const int port = port_future.get();
    if (port <= 0) {
      std::cerr << "  [FAIL] server failed to bind (plain control)" << std::endl;
      stop.store(true);
      srv.join();
      return 2;
    }

    client_probe p;
    run_client(port, nullptr, request, greeting_len, p, "plain_control");

    check(p.connect_status.load() == 0,
          "plain control: connect status = " +
              std::to_string(p.connect_status.load()));
    check(probe_received(p) == std::string(kGreeting),
          "plain control: greeting not received (got " +
              std::to_string(probe_received(p).size()) + " bytes, arm_rc=" +
              std::to_string(p.read_arm_rc.load()) + ", read_err=" +
              std::to_string(p.read_error.load()) + ", write_st=" +
              std::to_string(p.write_status.load()) + ")");

    check(st.delivered.load() == 1,
          "plain control: on_connection not called (got " +
              std::to_string(st.delivered.load()) + ")");
    check(st.greeting_ok.load() == 1,
          "plain control: greeting write did not succeed");
    check(state_received(st) == request,
          "plain control: server got " +
              std::to_string(st.plain_bytes_in.load()) + " bytes (want " +
              std::to_string(request.size()) + ")");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    srv.join();

    check(st.final_count.load() == 0,
          "plain control: client table not drained: " +
              std::to_string(st.final_count.load()) + " left");
  }

  if (g_failures == 0) {
    std::cout << "[web_ssl_server] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[web_ssl_server] FAIL (" << g_failures << " checks)" << std::endl;
  return 2;
}

#else

int main() {
  std::cout << "[web_ssl_server] SKIP (web or OpenSSL disabled)" << std::endl;
  return 0;
}

#endif
