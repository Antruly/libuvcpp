/**
 * @file tests/functional/web_ws_client_close_func.cpp
 * @brief `~uvcpp_ws_client` **不许在析构里等**。
 *
 * 清单 #6：析构里的关闭流程是
 *   `for (i < 5000 && !done) { loop_->run(UV_RUN_NOWAIT); sleep(1ms); }`
 * 后面还接 20 次带 sleep 的迭代。形状上就是"在析构点上跑事件循环 + 墙钟等待"：
 * 析构点往往就在循环自己的回调里（会话关闭时把自己一起回收），那等于让那个
 * 回调阻塞 —— 最坏 5 秒，常态也有一二十毫秒的固定睡眠，与"不在循环线程上做
 * 耗时操作、不做密集等待"直接抵触。
 *
 * 判据是**多出来的墙钟**：一个真的连上了的客户端，与一个没连过的客户端，
 * 析构耗时之差就是这段关闭流程的代价。取差是为了把机器快慢、`sessions_
 * .shutdown()`、`loop_close()` 这些两边都付的成本约掉。
 *
 * 修复后这个差值是 0 —— 几十次**不含等待**的 NOWAIT 迭代，微秒级。修复前是
 * 20 次 `sleep_for(1ms)` 打底（Windows 默认时钟粒度 15.6ms，实际常到几百毫秒）。
 *
 * 前提必须被断言：没连上过的话 `raw->is_active()` 为假，那段关闭流程**整段
 * 跳过**，于是"析构很快"是一句空话。所以服务端必须确实 accept 到了一个连接。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <uv.h>

#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_ws_client.h>

#if UVCPP_WEB_ENABLE

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// @brief 连上的那个客户端析构耗时的上限（毫秒），取多轮里的**最小值**。
///
/// 实测：修复前 33~38 ms（光是尾部 20 次 `sleep_for(1ms)` 就有这个量级），
/// 修复后 0.15~0.23 ms —— 差两个数量级，10 ms 落在中间偏修复后的那一侧。
/// 取多轮最小值是为了把"这一轮正好被调度走了"的噪声去掉：睡眠是实打实的
/// 墙钟，最小值也降不下来；而偶然的抢占只会让最小值偏大一点点。
const double kMaxDestroyMs = 10.0;
const int    kRounds      = 5;

}  // namespace

int main() {
  std::cout << "[web_ws_client_close] start" << std::endl;

  uvcpp_tcp_server server;
  std::atomic<int>  accepted{0};

  // 只 accept、不回话：握手停在半路。**这正是需要测的状态** —— 客户端已经
  // 连上（底层句柄 active），但还没走到 WS_CLIENT_CLOSED。
  server.set_read_callback([](uvcpp_tcp_client&, const net_read_result&) {});
  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    std::cerr << "  [FAIL] server bind rc=" << rc << std::endl;
    return 2;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int port = ntohs(name.sin_port);

  rc = server.listen([&accepted](uvcpp_tcp_client*) { accepted.fetch_add(1); }, 128);
  if (rc != 0) {
    std::cerr << "  [FAIL] server listen rc=" << rc << std::endl;
    return 2;
  }

  double baseline_ms = 0.0;
  double connected_ms = 0.0;

  // ---- 基线：一个从没连过的客户端 -------------------------------------
  // 它的底层句柄不 active，析构里那段关闭流程整段不执行。同一台机器、同一份
  // 代码里的这一份成本，正好用来把"两边都付的钱"减掉。
  {
    uvcpp_ws_client* c = new uvcpp_ws_client();
    const auto t0 = std::chrono::steady_clock::now();
    delete c;
    baseline_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
  }

  // ---- 被测：连上之后析构 ---------------------------------------------
  connected_ms = 1e9;
  int connected_rounds = 0;
  for (int r = 0; r < kRounds; ++r) {
    const int before_accept = accepted.load();
    uvcpp_ws_client* c = new uvcpp_ws_client();
    rc = c->connect("ws://127.0.0.1:" + std::to_string(port) + "/",
                    [](uvcpp_ws_connection*, int) {});
    check(rc == 0, "connect returned " + std::to_string(rc));

    // 泵到服务端确实 accept 了 —— 这就是"客户端底层句柄已经 active"的证据。
    for (int i = 0; i < 3000 && accepted.load() == before_accept; ++i) {
      server.get_loop()->run(UV_RUN_NOWAIT);
      c->get_loop()->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (accepted.load() != before_accept + 1) {
      check(false,
            "round " + std::to_string(r) + ": server accepted " +
                std::to_string(accepted.load() - before_accept) +
                " connections — 没连上的话量到的不是关闭流程的代价");
      delete c;
      continue;
    }
    // 让连接稳定下来（TCP 层、握手请求发出去）。
    for (int i = 0; i < 50; ++i) {
      server.get_loop()->run(UV_RUN_NOWAIT);
      c->get_loop()->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 从这里开始**谁都不再泵** —— 析构自己要处理剩下的关闭流程。
    const auto t0 = std::chrono::steady_clock::now();
    delete c;
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    if (ms < connected_ms) connected_ms = ms;
    ++connected_rounds;
  }

  std::cout << "  [note] dtor: never-connected " << baseline_ms
            << " ms, connected(min of " << connected_rounds << ") "
            << connected_ms << " ms" << std::endl;

  check(connected_rounds == kRounds,
        "只量到 " + std::to_string(connected_rounds) + "/" +
            std::to_string(kRounds) + " 轮");
  check(connected_ms < kMaxDestroyMs,
        "~uvcpp_ws_client on a CONNECTED client took " +
            std::to_string(connected_ms) + " ms (上限 " +
            std::to_string(kMaxDestroyMs) +
            " ms) — 析构里在跑墙钟等待");

  if (g_failures == 0) {
    std::cout << "[web_ws_client_close] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[web_ws_client_close] FAIL (" << g_failures << ")" << std::endl;
  return 2;
}

#else
int main() {
  std::cout << "[web_ws_client_close] SKIP (web disabled)" << std::endl;
  return 0;
}
#endif
