/**
 * @file tests/functional/web_ws_ownership_func.cpp
 * @brief WS 会话所有权：谁回收会话、什么时候回收、回收几次。
 *
 * 这一层要回答的问题只有一个：**`uvcpp_ws_connection` 会不会被回收，以及会
 * 不会被回收两次**。它错起来的样子是安静的 —— 少回收是内存泄漏（用例照样
 * 全绿），多回收是延迟的 UAF（崩在别的用例里，离现场很远）。所以断言必须
 * 落在**可数的证据**上，而不是"没崩就是对的"：
 *
 *   - `uvcpp_ws_sessions::recycled()` —— 真正 `delete` 过的会话数（单调递增）
 *   - `on_close` / `on_error` 的**次数与码**
 *
 * 四种终结路径都要各走一遍（对端断开、对端 Close 帧、协议错误、属主停机），
 * 它们分别落在不同的代码分支上：`recycled()` 对上只说明其中一条通了。
 *
 * 会话注册的回调捕获场景对象的 `this`，所以场景必须**先于会话终结**。这里由
 * 成员声明顺序保证（`sessions` 声明在 `server`/`client` **之后** → 最先析构），
 * 不靠运气。会话本身全程**不由测试 `delete`** —— 那正是本用例要验证的契约。
 *
 * 客户端方向发出的帧按 RFC 6455 §5.3 带掩码。
 */
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include "net/uvcpp_tcp_server.h"
#include "net/uvcpp_tcp_client.h"
#include "web/uvcpp_ws_connection.h"
#include "web/uvcpp_ws_sessions.h"

#include "wait_util.h"

using namespace uvcpp;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const std::string& what) {
  if (ok) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cout << "  [FAIL] " << what << "\n";
  }
}

// =========================================================================
// 线上字节构造
// =========================================================================

static const char k_mask[4] = {0x37, (char)0xfa, 0x21, 0x3d};

/** @brief 构造一个完整的线上帧（客户端方向 = 带掩码）。 */
static std::string raw_frame(int opcode, bool fin, const std::string& payload,
                             unsigned char rsv = 0) {
  std::string f;
  unsigned char b0 = static_cast<unsigned char>(opcode & 0x0F);
  if (fin) b0 |= 0x80;
  b0 |= rsv;
  f.push_back(static_cast<char>(b0));

  const size_t n = payload.size();
  if (n < 126) {
    f.push_back(static_cast<char>(0x80 | n));
  } else if (n <= 0xFFFF) {
    f.push_back(static_cast<char>(0x80 | 126));
    f.push_back(static_cast<char>((n >> 8) & 0xFF));
    f.push_back(static_cast<char>(n & 0xFF));
  } else {
    f.push_back(static_cast<char>(0x80 | 127));
    for (int i = 0; i < 8; i++) {
      f.push_back(static_cast<char>((n >> (56 - i * 8)) & 0xFF));
    }
  }

  for (int i = 0; i < 4; i++) f.push_back(k_mask[i]);
  for (size_t i = 0; i < n; i++) {
    f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^
                                  static_cast<unsigned char>(k_mask[i % 4])));
  }
  return f;
}

/** @brief 带状态码的 Close 帧负载（RFC 6455 §5.5.1）。 */
static std::string close_payload(uint16_t code, const std::string& reason) {
  std::string p;
  p.push_back(static_cast<char>((code >> 8) & 0xFF));
  p.push_back(static_cast<char>(code & 0xFF));
  p += reason;
  return p;
}

static int bound_port(uvcpp_tcp_server& s) {
  struct sockaddr_in name;
  int nl = static_cast<int>(sizeof(name));
  s.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &nl);
  return ntohs(name.sin_port);
}

// =========================================================================
// 场景：一个 server + 一个 raw client，服务端那侧套 uvcpp_ws_connection，
//       由 uvcpp_ws_sessions 持有所有权（不在测试里 new/delete）
// =========================================================================

struct scenario {
  // 声明顺序 = 析构顺序的逆序：`sessions` 最后声明 → 最先析构，
  // 于是兜底回收发生在两个 loop 都还活着的时候。
  uvcpp_tcp_server  server;
  uvcpp_tcp_client  client;
  uvcpp_ws_sessions sessions;

  uvcpp_ws_connection* conn = nullptr;
  int    port      = 0;
  bool   connected = false;
  size_t adopted   = 0;   // 建过多少会话

  std::vector<int> closes;   // on_close 收到的码（按发生顺序）
  std::vector<int> errors;   // on_error 收到的码

  bool start() {
    if (server.bindIpv4("127.0.0.1", 0) != 0) return false;
    port = bound_port(server);

    // 延迟回收用的 async 句柄必须在**循环线程**上建。本线程就是跑循环的
    // 那一个（pump 在主线程里），所以这里建是对的。
    sessions.set_loop(server.get_loop());

    int rc = server.listen([this](uvcpp_tcp_client* c) {
      auto* wc = new uvcpp_ws_connection(c, ws_role::SERVER);
      wc->on_close([this](ws_close_code cd, const std::string&) {
        closes.push_back(static_cast<int>(cd));
      });
      wc->on_error([this](int code, const std::string&) { errors.push_back(code); });
      // **先接管所有权，再 start()**：对端若在 start() 里就断了，会话得知道
      // 该把自己交给谁（见 uvcpp_ws_sessions::adopt 的说明）。
      sessions.adopt(wc);
      wc->start();
      conn = wc;
      ++adopted;
    }, 128);
    if (rc != 0) return false;

    rc = client.connect("127.0.0.1", port, [this](int st) { connected = (st == 0); });
    if (rc != 0) return false;
    return pump_until([this] { return connected && conn != nullptr; }, 3000);
  }

  /** @brief 同时推进两个 loop，直到 done() 为真或超时。 */
  bool pump_until(const std::function<bool()>& done, int timeout_ms) {
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      server.get_loop()->run(UV_RUN_NOWAIT);
      client.get_loop()->run(UV_RUN_NOWAIT);
      if (done()) return true;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= timeout_ms) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  /** @brief 客户端发一段字节并等写完成（同一时刻只能有一个异步写）。 */
  bool send(const std::string& bytes) {
    bool done = false;
    int rc = client.write(bytes.data(), bytes.size(), [&done](int) { done = true; });
    if (rc != 0) return false;
    return pump_until([&done] { return done; }, 3000);
  }

  /** @brief 把两个 loop 空推 \p ms 毫秒（让排队中的关闭/写完成回调落地）。 */
  void settle(int ms) {
    uvcpp_test::pump_for_pair(server.get_loop(), client.get_loop(), ms);
  }

  ~scenario() {
    // 让兜底回收（~uvcpp_ws_sessions → shutdown → recycle_all）之前，先把
    // 两个 loop 里还排着的关闭完成回调跑掉，尽量让"正常路径"而不是兜底路径
    // 收尾 —— 兜底路径另有专门的用例（属主停机）去覆盖。
    settle(50);
  }
};

// =========================================================================
// T1 —— 对端不发 Close 帧就断开
// =========================================================================

static void t_peer_disconnect() {
  std::cout << "[peer_disconnect]\n";
  scenario s;
  check(s.start(), "scenario started");

  s.client.get_tcp()->close();   // 裸 TCP 关闭：一个 WS 字节都不发

  check(s.pump_until([&s] { return s.sessions.recycled() == 1; }, 3000),
        "session was recycled (recycled()==1)");
  // 对端没发 Close 帧就走 = RFC 6455 §7.1.5 的 1006，且**恰好一次**。
  check(s.closes.size() == 1 && s.closes[0] == 1006,
        "on_close fired exactly once with ABNORMAL_CLOSE(1006)");
  check(s.errors.empty(), "no on_error");
  check(s.sessions.size() == 0, "sessions table is empty");
  check(s.sessions.pending() == 0, "nothing left pending");
  check(s.sessions.recycled() == 1, "recycled counter is exactly 1 (no double free)");
}

// =========================================================================
// T2 —— 对端发 Close 帧（1000）
// =========================================================================

static void t_peer_close_frame() {
  std::cout << "[peer_close_frame]\n";
  scenario s;
  check(s.start(), "scenario started");

  check(s.send(raw_frame(0x8, true, close_payload(1000, "bye"))), "close frame written");

  check(s.pump_until([&s] { return s.sessions.recycled() == 1; }, 3000),
        "session was recycled (recycled()==1)");
  // 码来自**帧里**，而不是 1006 —— 这正是"对端是怎么走的"要回答的。
  check(s.closes.size() == 1 && s.closes[0] == 1000,
        "on_close carried the peer's code (1000), not 1006");
  check(s.errors.empty(), "no on_error");
  check(s.sessions.recycled() == 1, "recycled counter is exactly 1");
}

// =========================================================================
// T3 —— 协议错误（RSV2 置位）
// =========================================================================

static void t_protocol_error() {
  std::cout << "[protocol_error]\n";
  scenario s;
  check(s.start(), "scenario started");

  // 没有任何扩展被协商，RSV2 必须为 0（RFC 6455 §5.2）。
  check(s.send(raw_frame(0x1, true, "hi", 0x20)), "bad frame written");

  check(s.pump_until([&s] { return s.sessions.recycled() == 1; }, 3000),
        "session was recycled (recycled()==1)");
  check(s.errors.size() == 1 && s.errors[0] == 1002,
        "on_error fired once with PROTOCOL_ERROR(1002)");
  // **本端判定**的结束不该被说成"对端异常关闭"：1006 必须是 0 次。
  check(s.closes.empty(), "no 1006 on_close for a self-initiated protocol close");
  check(s.sessions.recycled() == 1, "recycled counter is exactly 1");
}

// =========================================================================
// T4 —— 有在途发送时对端断开
// =========================================================================

static void t_inflight_send_then_disconnect() {
  std::cout << "[inflight_send_then_disconnect]\n";
  const int kN = 12;

  for (int i = 0; i < kN; ++i) {
    scenario s;
    if (!s.start()) { check(false, "scenario started (round)"); return; }

    // 客户端**一个字节都不读**，于是服务端这笔大写在 socket 缓冲填满后
    // 停在"在途"状态；此时把连接砍掉，写完成与回收就会撞在一起。
    std::string big(256 * 1024, 'x');
    s.conn->send_binary(big.data(), big.size());

    s.client.get_tcp()->close();

    if (!s.pump_until([&s] { return s.sessions.recycled() == 1; }, 3000)) {
      check(false, "round: session was recycled");
      return;
    }
  }
  check(true, "12 rounds of in-flight-send + disconnect recycled every session");
}

// =========================================================================
// T5 —— 属主停机：全部会话被终结并回收，且析构不再碰它们
// =========================================================================

static void t_owner_shutdown_recycles_all() {
  std::cout << "[owner_shutdown_recycles_all]\n";

  {
    scenario s;
    check(s.start(), "scenario started");

    // 再叠两条连接：三条会话同时在表里，停机要**全部**收掉。
    uvcpp_tcp_client c2, c3;
    bool c2_ok = false, c3_ok = false;
    check(c2.connect("127.0.0.1", s.port, [&c2_ok](int st) { c2_ok = (st == 0); }) == 0,
          "c2 connect dispatched");
    check(c3.connect("127.0.0.1", s.port, [&c3_ok](int st) { c3_ok = (st == 0); }) == 0,
          "c3 connect dispatched");

    // c2/c3 各有自己的 loop，连接完成回调只在**它们自己的** loop 上跑 ——
    // 只推 server/client 两个 loop 的话这两个回调永远不会来。
    auto pump_all = [&](const std::function<bool()>& done, int timeout_ms) {
      auto t0 = std::chrono::steady_clock::now();
      for (;;) {
        s.server.get_loop()->run(UV_RUN_NOWAIT);
        s.client.get_loop()->run(UV_RUN_NOWAIT);
        c2.get_loop()->run(UV_RUN_NOWAIT);
        c3.get_loop()->run(UV_RUN_NOWAIT);
        if (done()) return true;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count() >= timeout_ms) {
          return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    };

    check(pump_all([&] { return c2_ok && c3_ok && s.adopted == 3; }, 3000),
          "all three connections established");
    check(s.sessions.size() == 3, "three sessions in the table");

    // 属主停机：立即终结 + 回收（不发 Close 帧 —— 循环马上要停了）。
    s.sessions.recycle_all();

    check(s.sessions.recycled() == 3, "all three sessions recycled");
    check(s.sessions.size() == 0, "table empty after recycle_all");
    check(s.sessions.pending() == 0, "nothing pending after recycle_all");

    // recycle_all 之后**不再碰**已回收的会话：会话终结时摘掉了关闭观察者，
    // 所以接下来 c2/c3 析构（各自的 tcp 客户端被删）不该有任何回调落到已
    // 释放的会话上 —— 崩了就是这里错了。
    pump_all([] { return false; }, 50);
  }
  check(true, "scenario destroyed without touching recycled sessions");
}

// =========================================================================
// T6 —— 客户端侧会话：在途发送 + 关闭（`alive_token_` 真正起作用的地方）
// =========================================================================
//
// 服务端那侧有 close manager（客户端被删时它持有的闭包一并消失）；**客户端侧
// 没有** —— `uvcpp_tcp_client` 要活到属主析构，所以"会话已回收、写完成才到"
// 在这里是**真的会发生**的。这一组就打在它上面。
static void t_client_side_inflight_then_close() {
  std::cout << "[client_side_inflight_then_close]\n";
  const int kN = 12;

  for (int i = 0; i < kN; ++i) {
    uvcpp_tcp_server  server;
    uvcpp_tcp_client  cli;
    uvcpp_ws_sessions sessions;
    bool connected = false;
    uvcpp_tcp_client* accepted = nullptr;

    if (server.bindIpv4("127.0.0.1", 0) != 0) { check(false, "bind"); return; }
    const int port = bound_port(server);

    // 服务端**不读**：这笔大写在 socket 缓冲填满后停在在途状态。
    // （listen 的兜底读会替我们收字节，所以必须显式停掉它。）
    server.listen([&accepted](uvcpp_tcp_client* c) {
      accepted = c;
      c->read_stop();
    }, 128);

    sessions.set_loop(cli.get_loop());
    cli.connect("127.0.0.1", port, [&connected](int st) { connected = (st == 0); });

    auto pump = [&]() {
      server.get_loop()->run(UV_RUN_NOWAIT);
      cli.get_loop()->run(UV_RUN_NOWAIT);
    };
    uvcpp_test::wait_until_pair(server.get_loop(), cli.get_loop(),
                                [&] { return connected && accepted != nullptr; },
                                uvcpp_test::kWaitMs);
    if (!(connected && accepted != nullptr)) { check(false, "connect"); return; }

    // 客户端侧的会话 —— 它的写完成闭包由 `cli` 持有，而 `cli` 比会话活得久。
    auto* wc = new uvcpp_ws_connection(&cli, ws_role::CLIENT);
    sessions.adopt(wc);
    wc->start();

    std::string big(4 * 1024 * 1024, 'y');
    wc->send_binary(big.data(), big.size());

    // 走**客户端自己的 close()**（而不是裸 `get_tcp()->close()`）：只有这条
    // 路会跑 fire_close_callbacks，而关闭观察者正是会话终结的唯一信号。
    cli.close();

    uvcpp_test::wait_until_pair(server.get_loop(), cli.get_loop(),
                                [&] { return sessions.recycled() != 0; },
                                uvcpp_test::kWaitMs);
    if (sessions.recycled() != 1) { check(false, "client-side session recycled"); return; }

    // 会话已经 **delete 了**，而 `cli` 还活着 —— 继续推进循环，让任何残留的
    // 写完成回调有机会被投递。没有存活令牌的话，这里就是 UAF。
    uvcpp_test::pump_for_pair(server.get_loop(), cli.get_loop(), 200);
  }
  check(true, "12 rounds of client-side in-flight send + close survived");
}

// =========================================================================
// T7 —— 属主**强制**终结一个还有在途写的客户端侧会话
// =========================================================================
//
// 这是 `alive_token_` 唯一真正吃紧的形状：`recycle_all()` 会把会话**当场**
// delete（不像正常路径那样推到下一轮循环），而写完成闭包还攥在 `cli` 手里。
// 连接随后被关掉，libuv 把那次写以 ECANCELED 结算 —— 回调到达时 `this`
// 已经是一块回收过的内存，没有存活令牌就会读脏成员、调垃圾 std::function。
static void t_owner_terminate_with_inflight_write() {
  std::cout << "[owner_terminate_with_inflight_write]\n";
  const int kN = 12;

  for (int i = 0; i < kN; ++i) {
    uvcpp_tcp_server  server;
    uvcpp_tcp_client  cli;
    uvcpp_ws_sessions sessions;
    bool connected = false;
    uvcpp_tcp_client* accepted = nullptr;

    if (server.bindIpv4("127.0.0.1", 0) != 0) { check(false, "bind"); return; }
    const int port = bound_port(server);

    server.listen([&accepted](uvcpp_tcp_client* c) {
      accepted = c;
      c->read_stop();     // 服务端不读：这笔写会一直停在在途状态
    }, 128);

    sessions.set_loop(cli.get_loop());
    cli.connect("127.0.0.1", port, [&connected](int st) { connected = (st == 0); });

    auto pump = [&]() {
      server.get_loop()->run(UV_RUN_NOWAIT);
      cli.get_loop()->run(UV_RUN_NOWAIT);
    };
    uvcpp_test::wait_until_pair(server.get_loop(), cli.get_loop(),
                                [&] { return connected && accepted != nullptr; },
                                uvcpp_test::kWaitMs);
    if (!(connected && accepted != nullptr)) { check(false, "connect"); return; }

    auto* wc = new uvcpp_ws_connection(&cli, ws_role::CLIENT);
    sessions.adopt(wc);
    wc->start();

    // 必须**真的**灌满管道，否则本用例考的东西根本不存在。
    //
    // 这里踩过一个坑：单发 4 MiB **不够** —— 内核发送缓冲直接把它吃下去了，
    // 写完成回调当场就跑完了，于是"在途"只是我的一厢情愿（实测：那版跑下来
    // 是绿的，是补上下面这条前置断言才当场变红）。所以改成多块连发，把连接
    // 灌到写不动为止。
    //
    // 判据是 `done < kChunks`：只要还有没结算的块，队头那块就必然在途
    // （`pump_send` 只有在 `write_in_flight_` 为假时才会发队头，队列非空而
    // 在途为假是不可能的状态）。
    const int kChunks = 32;                       // 32 MiB：远超管道容量
    const size_t kChunk = 1024 * 1024;
    std::string chunk(kChunk, 'z');
    int done = 0;
    for (int c = 0; c < kChunks; ++c) {
      wc->send_binary(chunk.data(), chunk.size(), [&done](int) { ++done; });
    }
    for (int k = 0; k < 5; ++k) pump();
    if (done >= kChunks) {
      check(false, "precondition: some write is still in flight");
      return;
    }

    // 属主拆台：**当场**终结并 delete，不等下一轮循环。
    sessions.recycle_all();
    if (sessions.recycled() != 1) { check(false, "terminate recycled the session"); return; }

    // 关掉连接 —— libuv 把那次在途写以 ECANCELED 结算，写完成回调随后到达，
    // 而它捕获的 `this` 已经是一块回收过的内存。
    cli.get_tcp()->close();
    uvcpp_test::pump_for_pair(server.get_loop(), cli.get_loop(), 200);
  }
  check(true, "12 rounds of owner-terminate with in-flight write survived");
}

// =========================================================================

int main() {
  t_peer_disconnect();
  t_peer_close_frame();
  t_protocol_error();
  t_inflight_send_then_disconnect();
  t_owner_shutdown_recycles_all();
  t_client_side_inflight_then_close();
  t_owner_terminate_with_inflight_write();

  std::cout << "\n" << g_pass << " passed, " << g_fail << " failed\n";
  if (g_fail != 0) return 1;
  std::cout << "ALL PASS\n";
  return 0;
}

#else
int main() {
  std::cout << "SKIP: web disabled\n";
  return 0;
}
#endif
