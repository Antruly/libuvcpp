/**
 * @file tests/functional/udp_close_token_func.cpp
 * @brief udp 的析构收尾：不许在析构里等墙钟；析构开始之后不许再碰本对象。
 *
 * 清单 #6（析构里跑墙钟等待）/ #7 的同一族在 udp 上的收口，两件事：
 *
 * 1. **析构里不等墙钟**。`~uvcpp_udp_server` 原来是
 *    `for (i < 5000 && !done) { loop_->run(UV_RUN_NOWAIT); sleep(1ms); }`
 *    后面再跟 20 次带 sleep 的迭代；`~uvcpp_udp_client` 同形。析构点常常就在
 *    循环自己的回调里（会话结束时把自己一起回收），那等于让那个回调阻塞 ——
 *    与"不在循环线程上跑耗时操作、不做密集等待"直接抵触。改成**有界、无
 *    sleep** 的 64 次 NOWAIT：`uv_close` 的收尾回调不需要 I/O 也不需要定时器，
 *    一两轮就到。
 *
 * 2. **析构开始之后不再碰本对象**。udp 的接收回调 / 发送完成回调 / `stop()`
 *    的关闭回调原先都捕获裸 `this`。与 tcp 侧同一套存活令牌：对象活着时
 *    `*token == 0`，析构的第一件事置 1。
 *
 * 判据与前提
 * ----------
 * [1] 判据是**析构的绝对墙钟**，两种实例各取 5 轮最小值：
 *       - 从没 bind 过的（0.001 ms）—— 抓的是**无条件**付的等待：原来那 20 次
 *         `sleep_for(1ms)` 就在 `if` 外面，这份实例一样在睡觉（实测 27 ms）。
 *       - bind + recv_start 过的（0.009 ms）—— 抓的是只有活跃句柄才走的那段
 *         关闭流程里的等待。
 *     **不能只看两者之差**：尾巴是无条件付的，差值算出来接近 0 —— 变异验证里
 *     带 sleep 的收尾就是这么漏过去的（两边的差值 Δ 只有 0~7 ms）。
 *     **前提被断言**：句柄不 active 的话那段关闭流程整段跳过，"析构很快"就是
 *     一句空话，所以 bind + recv_start 之后必须 `is_active() == 1`。
 *
 * [2] 客户端在飞的发送完成。相位 A 先证明"这次 send 的完成回调确实在飞、下一轮
 *     就会到"（泵循环 → 恰好 1 次）；相位 B 才是被测形状：**不泵循环**，收尾
 *     交给析构 —— 此时用户的发送回调一次都不能跑。没有令牌的话它会在析构过程
 *     中被打到（变异验证：把 `*alive_token_ = 1;` 那一行去掉，相位 B 立刻红）。
 *
 * 诚实的边界（别把下面这两条当成"测过了"）
 * --------------------------------------
 *  - [2]/[3] 对**改之前**的代码是"恰好通过"：旧析构在 udp 上会整段跳过关闭流程
 *    （客户端先 `recv_stop()` 把句柄变成不 active，服务端则被 `stopped_` 挡住），
 *    什么回调都不会被送进来。它们守的是**新契约**，判它们的是变异。
 *  - "句柄不 active 就不关"这条旧判据（`!is_closing() && is_active()`）的代价是
 *    **句柄内存与在途请求泄漏、以及在已经释放的 loop 上叫 `uv_close`**（`is_active()`
 *    为假时析构已经把 loop 删了，`free_handle()` 才去 uv_close）。本环境没有可靠
 *    的内存探针（内存池计数不成立、无分配器钩子、Release CRT 不报错），**且**
 *    Windows 上 close 套接字是 `uv_close` 里同步做的、不随收尾回调延后，所以这条
 *    **没有**黑盒判据 —— 它只有 [2] 顺带覆盖到的"收尾确实发生过"。
 *  - **服务端接收回调的守卫（变异 M5）抓不住**，且原因不在用例写得松：libuv 自己
 *    在这条路前面就把回调摘掉了。Windows：`uv__udp_close` 先调
 *    `uv_udp_recv_stop`（清 `UV_HANDLE_READING`）、再 `closesocket`，而接收完成
 *    的处理 `uv__process_udp_recv_req` 在 `UV_HANDLE_ZERO_READ` 分支上先查
 *    `UV_HANDLE_READING`，为假就不碰 `recv_cb`（`src/win/udp.c`）；unix：
 *    `uv__udp_recv_stop` 直接把 `handle->recv_cb` 置成 NULL（`src/unix/udp.c`）。
 *    也就是说析构一开始，接收回调就再也送不进来了。这个守卫是**契约一致**的防御
 *    （与 tcp 侧同一套），不是能在这里量出来的东西 —— 删掉它，本用例照样全绿。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <uv.h>

#include <net/uvcpp_udp_client.h>
#include <net/uvcpp_udp_server.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// @brief 析构耗时的上限（毫秒），**绝对值**，取多轮里的最小值。
///
/// 实测（本机，5 轮最小值）：
///   - 改之后：从没 bind 过的 0.001 ms、bind+recv_start 过的 0.009 ms；
///   - 改之前 / 变异 M3：**两边都是 27 ~ 36 ms** —— 那 20 次 `sleep_for(1ms)`
///     在 `if` 外面，两份都要付。
///
/// **判据只能是绝对值，不能是"被测减基线"的差**：尾巴是无条件付的，基线也一样
/// 在睡觉，差值算出来是 0（M3 第一次就是这么漏掉的）。5 ms 夹在中间：相对改之后
/// 的 0.01 ms 有 500 倍余量，相对改之前的 27 ms 有 5 倍余量。
const double kMaxDtorMs = 5.0;
const int    kRounds       = 5;

/// @brief 取服务端实际绑上的端口。
int bound_port(uvcpp_udp_server& s) {
  sockaddr_in name;
  int namelen = sizeof(name);
  if (s.get_udp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen) !=
      0) {
    return -1;
  }
  return ntohs(name.sin_port);
}

/// @brief 一个只 bind、不读的 udp 落点，用来给客户端当发送目标。
///
/// 发到没人绑的端口在 Windows 上第一次也能成，但 ICMP 端口不可达回来之后同一
/// socket 的下一次发送会拿到 WSAECONNRESET，所以给一个真实绑着的目标。
int make_sink_port(uvcpp_udp_server& sink) {
  if (sink.bindIpv4("127.0.0.1", 0) != 0) return -1;
  return bound_port(sink);
}

// =========================================================================
// [1] 服务端析构不在析构里等墙钟
// =========================================================================
void test_server_dtor_does_not_wait() {
  std::cout << "[1] ~uvcpp_udp_server 不等墙钟" << std::endl;

  // ---- 基线：从没 bind 过的服务端 -------------------------------------
  // 它的句柄不 active，改之前那段关闭流程整段跳过。同一台机器、同一份代码里
  // 的这一份成本，正好把"两边都付的钱"（loop_close、delete loop 之类）减掉。
  double baseline_ms = 1e9;
  for (int r = 0; r < kRounds; ++r) {
    uvcpp_udp_server* s = new uvcpp_udp_server();
    const auto t0 = std::chrono::steady_clock::now();
    delete s;
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    if (ms < baseline_ms) baseline_ms = ms;
  }

  // ---- 被测：bind + recv_start 之后析构 ---------------------------------
  double bound_ms = 1e9;
  int    active_ok = 0;
  int    rounds = 0;
  for (int r = 0; r < kRounds; ++r) {
    uvcpp_udp_server* s = new uvcpp_udp_server();
    if (s->bindIpv4("127.0.0.1", 0) != 0) {
      check(false, "round " + std::to_string(r) + ": bind 失败");
      delete s;
      continue;
    }
    if (s->recv_start([](uvcpp_buf*, const char*, int) {}) != 0) {
      check(false, "round " + std::to_string(r) + ": recv_start 失败");
      delete s;
      continue;
    }
    // **前提**：句柄 active 才有那段关闭流程可跑。不 active 的话这个用例量到的
    // 只是两次空析构之差，"析构很快"就是一句空话。
    if (s->get_udp()->is_active() == 1) ++active_ok;
    ++rounds;

    // 从这里开始谁都不再泵循环 —— 收尾只能由析构自己做。
    const auto t0 = std::chrono::steady_clock::now();
    delete s;
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    if (ms < bound_ms) bound_ms = ms;
  }

  std::cout << "  [note] dtor: never-bound " << baseline_ms
            << " ms, bound+recv_start(min of " << rounds << ") " << bound_ms
            << " ms" << std::endl;

  check(rounds == kRounds, "只量到 " + std::to_string(rounds) + "/" +
                               std::to_string(kRounds) + " 轮");
  check(active_ok == kRounds,
        "前提：每一轮 bind + recv_start 之后句柄都是 active 的（实际 " +
            std::to_string(active_ok) + "/" + std::to_string(kRounds) +
            " —— 不 active 的话关闭流程整段跳过，量不出东西）");
  // 两条都摆出来：基线那条抓的是**无条件**付的墙钟（原来尾巴就在 `if` 外面），
  // 被测那条抓的是只有活跃句柄才走的关闭流程里的墙钟。
  check(baseline_ms < kMaxDtorMs,
        "~uvcpp_udp_server（从没 bind 过）析构花了 " +
            std::to_string(baseline_ms) + " ms（上限 " +
            std::to_string(kMaxDtorMs) + " ms）—— 析构里在跑墙钟等待");
  check(bound_ms < kMaxDtorMs,
        "~uvcpp_udp_server（bind+recv_start 过）析构花了 " +
            std::to_string(bound_ms) + " ms（上限 " +
            std::to_string(kMaxDtorMs) + " ms）—— 析构里在跑墙钟等待");
}

// =========================================================================
// [2] 客户端：在飞的发送完成，析构开始之后不许再调用户回调
// =========================================================================

/// @brief 相位 A —— 同样的形状，但**泵循环**：证明这次 send 的完成回调确实在飞。
void test_client_inflight_send_is_real(int sink_port) {
  std::cout << "[2a] 前提：在飞的发送完成确实会到" << std::endl;

  std::atomic<int> calls{0};
  uvcpp_udp_client* c = new uvcpp_udp_client();
  check(c->bindIpv4("127.0.0.1", 0) == 0, "客户端 bind");
  check(c->recv_start(nullptr) == 0, "客户端 recv_start（同步缓存模式）");

  int rc = c->send("127.0.0.1", sink_port, "ping", 4,
                   [&calls](int) { calls.fetch_add(1); });
  check(rc == 0, "异步 send 提交成功（实际 " + std::to_string(rc) + "）");

  // 不泵的话它不会到；泵一轮就到了 —— 这正是"在飞"的意思。
  c->get_loop()->run(UV_RUN_NOWAIT);
  for (int i = 0; i < 200 && calls.load() == 0; ++i) {
    c->get_loop()->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(calls.load() == 1,
        "前提：这次 send 的完成回调到了恰好 1 次（实际 " +
            std::to_string(calls.load()) +
            "）—— 到不了的话下一相位就是空跑");
  delete c;
}

/// @brief 相位 B —— 被测形状：**不泵循环**，让它挂着，然后析构。
void test_client_inflight_send_not_called_after_dtor_start(int sink_port) {
  std::cout << "[2b] 析构开始之后不再调用户回调" << std::endl;

  std::atomic<int>  calls{0};
  std::atomic<int>  during_dtor{0};
  std::atomic<bool> dtor_begun{false};

  {
    uvcpp_udp_client* c = new uvcpp_udp_client();
    check(c->bindIpv4("127.0.0.1", 0) == 0, "客户端 bind");
    check(c->recv_start(nullptr) == 0, "客户端 recv_start（同步缓存模式）");

    int rc = c->send("127.0.0.1", sink_port, "ping", 4, [&](int) {
      calls.fetch_add(1);
      if (dtor_begun.load()) during_dtor.fetch_add(1);
    });
    check(rc == 0, "异步 send 提交成功（实际 " + std::to_string(rc) + "）");

    // 前提（由 [2a] 证明）：这一次 send 的完成回调此刻还没跑、下一轮就会到。
    // 从这里开始**谁都不泵**：收尾连同那个在途回调，只能落在析构自己的收尾里。
    dtor_begun.store(true);
    delete c;
    dtor_begun.store(false);
  }

  check(during_dtor.load() == 0,
        "**析构开始之后用户的发送回调一次都没跑**（实际 " +
            std::to_string(during_dtor.load()) +
            " 次 —— 没有存活令牌的话它会在析构过程中被打到）");
  check(calls.load() == 0,
        "整个析构结束之后也是一次都没跑（实际 " + std::to_string(calls.load()) +
            " 次）");
}

// =========================================================================
// [3] 服务端：stop() 的关闭回调同样被令牌挡住
// =========================================================================
//
// `stop()` 里 `uv_close` 是延迟的：调用方停在这一句之后就不再泵循环，紧跟其后的
// 析构才是唯一会放那个关闭回调的地方。此时对象已经开拆，回调里既不能写
// `status_`，更不能把用户的 `on_stopped` 打进去。
void test_server_stop_callback_not_called_after_dtor_start() {
  std::cout << "[3] stop() 的关闭回调不在析构里触发" << std::endl;

  std::atomic<int> stopped_cb{0};
  {
    uvcpp_udp_server* s = new uvcpp_udp_server();
    check(s->bindIpv4("127.0.0.1", 0) == 0, "服务端 bind");
    check(s->recv_start([](uvcpp_buf*, const char*, int) {}) == 0,
          "服务端 recv_start");
    s->stop([&stopped_cb]() { stopped_cb.fetch_add(1); });
    check(stopped_cb.load() == 0, "stop() 本身不同步触发 on_stopped");
    delete s;  // 关闭回调还挂着，只有析构会放它
  }
  check(stopped_cb.load() == 0,
        "**析构开始之后 on_stopped 一次都不能触发**（实际 " +
            std::to_string(stopped_cb.load()) + " 次）");
}

// =========================================================================
// [4] 正常路径不许被守卫打坏
// =========================================================================

/// @brief 回显服务端，跑在另一个线程上。
class echo_server {
 public:
  bool start() {
    if (server_.bindIpv4("127.0.0.1", 0) != 0) return false;
    port_ = bound_port(server_);
    if (port_ <= 0) return false;
    if (server_.recv_start([this](uvcpp_buf* buf, const char* ip, int p) {
          server_.send(ip, p, buf->get_data(), buf->size());
        }) != 0) {
      return false;
    }
    ready_.store(true);
    thread_ = std::thread([this]() {
      while (!stop_.load()) {
        server_.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      for (int i = 0; i < 20; ++i) {
        server_.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    return true;
  }
  void stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
  }
  int port() const { return port_; }

 private:
  uvcpp_udp_server server_;
  std::thread      thread_;
  std::atomic<bool> ready_{false};
  std::atomic<bool> stop_{false};
  int port_ = 0;
};

/// @brief 接收守卫不能在活着的对象上误伤 —— 回显要照旧往返。
void test_recv_still_works() {
  std::cout << "[4] 守卫不打坏正常收发（回显往返）" << std::endl;

  echo_server srv;
  check(srv.start(), "回显服务端起来了");
  if (srv.port() <= 0) return;

  uvcpp_udp_client client;
  check(client.bindIpv4("127.0.0.1", 0) == 0, "客户端 bind");

  std::atomic<int> got{0};
  std::string       echoed;
  check(client.recv_start([&](uvcpp_buf* buf, const char*, int) {
          echoed = buf->to_string();
          got.fetch_add(1);
        }) == 0,
        "客户端 recv_start");

  const char* msg = "udp_close_token_ping";
  std::atomic<int> send_status{12345};
  check(client.send("127.0.0.1", srv.port(), msg, std::strlen(msg),
                    [&send_status](int status) { send_status.store(status); }) ==
            0,
        "客户端发送");

  // 两个条件都要等：**回显到了不等于发送完成回调跑过了** —— 数据报是提交那一刻
  // 就出去的，回显可能先回来。只等 `got` 的话 `send_status` 会停在哨兵值上。
  for (int i = 0; i < 3000 &&
                  (got.load() == 0 || send_status.load() == 12345);
       ++i) {
    client.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(got.load() == 1, "客户端收到了回显（实际 " + std::to_string(got.load()) +
                             " 次）");
  check(echoed == msg, "回显内容一致（实际 \"" + echoed + "\"）");
  check(send_status.load() == 0,
        "发送完成回调拿到 0（实际 " + std::to_string(send_status.load()) + "）");

  srv.stop();
}

/// @brief 令牌在**活着**的对象上必须是"活着"：stop() + 泵循环照旧收尾。
void test_stop_while_alive_still_completes() {
  std::cout << "[4] 活着的时候 stop() 照旧收尾" << std::endl;

  uvcpp_udp_server server;
  check(server.bindIpv4("127.0.0.1", 0) == 0, "服务端 bind");
  check(server.recv_start([](uvcpp_buf*, const char*, int) {}) == 0,
        "服务端 recv_start");

  std::atomic<int> stopped_cb{0};
  server.stop([&stopped_cb]() { stopped_cb.fetch_add(1); });

  for (int i = 0; i < 200 && stopped_cb.load() == 0; ++i) {
    server.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  check(stopped_cb.load() == 1,
        "on_stopped 触发了（实际 " + std::to_string(stopped_cb.load()) + " 次）");
  check(server.has_status(UDP_SERVER_STOPPED),
        "状态到了 UDP_SERVER_STOPPED（实际 " +
            std::to_string(server.get_status()) + "）");
  check(!server.has_status(UDP_SERVER_STOPPING),
        "UDP_SERVER_STOPPING 已被清掉（实际 " +
            std::to_string(server.get_status()) + "）");
}

}  // namespace

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  uvcpp_udp_server sink;
  const int sink_port = make_sink_port(sink);
  check(sink_port > 0, "发送目标（只 bind 不读的落点）起来了");

  test_server_dtor_does_not_wait();
  if (sink_port > 0) {
    test_client_inflight_send_is_real(sink_port);
    test_client_inflight_send_not_called_after_dtor_start(sink_port);
  }
  test_server_stop_callback_not_called_after_dtor_start();
  test_recv_still_works();
  test_stop_while_alive_still_completes();

  if (g_failures == 0) {
    std::cout << "[udp_close_token] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[udp_close_token] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}
