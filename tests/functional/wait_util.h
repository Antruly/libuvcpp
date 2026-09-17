/**
 * @file tests/functional/wait_util.h
 * @brief 测试侧等待原语：上限一律是**墙钟**，不是迭代次数。
 * @author zhuweiye
 * @version 1.0.0
 */

#ifndef UVCPP_TESTS_FUNCTIONAL_WAIT_UTIL_H_
#define UVCPP_TESTS_FUNCTIONAL_WAIT_UTIL_H_

#include <chrono>
#include <thread>

#include <uv.h>

#include "handle/uvcpp_loop.h"

namespace uvcpp_test {

/// 单个事件（连接、写完、关完）的默认等待上限。事件本身是毫秒级的，这个
/// 上限只在真出问题时才起作用。
const int kWaitMs = 2000;

/**
 * @brief `t0` 到现在过了多少毫秒。
 */
inline long long elapsed_ms(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

/**
 * @brief 泵循环直到 \p pred 为真，或超过 \p deadline_ms 墙钟。
 * @return 期限内是否满足。
 *
 * **上限必须是墙钟，不能是迭代次数。** 常见写法
 * `for (int i = 0; i < N; ++i) { run(); sleep_for(1ms); }` 里那个 `N` 看着像
 * 毫秒，实际是「N 圈」，而一圈的代价就是**系统定时器粒度**：
 *
 * | 环境 | `sleep_for(1ms)` 实测 |
 * |---|---|
 * | 本机（`NtQueryTimerResolution` current=1.0000 ms） | 1.86 ms |
 * | Windows 无请求者时的默认粒度 | 15.6 ms |
 *
 * `test_tcp_read_func` 原本关键路径上攒了 ~3200 个无条件圈，于是同一个用例
 * 本机 8.4 s、Windows runner 上 51 s，被 `ctest --timeout 30` 判成 Timeout ——
 * **库里一行没错，是用例把机器参数写进了等待**。换成墙钟后摆幅 6.4× → 8%。
 */
template <typename Pred>
inline bool wait_until(uvcpp::uvcpp_loop* loop, Pred pred, int deadline_ms) {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (pred()) return true;
    if (elapsed_ms(t0) >= deadline_ms) return false;
    if (loop != nullptr) loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

/**
 * @brief 同 `wait_until`，但一次泵**两个**循环（事件分属两边时用，例如
 * accept 在服务端循环、connect 在客户端循环）。
 */
template <typename Pred>
inline bool wait_until_pair(uvcpp::uvcpp_loop* a, uvcpp::uvcpp_loop* b,
                            Pred pred, int deadline_ms) {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (pred()) return true;
    if (elapsed_ms(t0) >= deadline_ms) return false;
    if (a != nullptr) a->run(UV_RUN_NOWAIT);
    if (b != nullptr) b->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

/**
 * @brief 同 `wait_until`，但泵的是裸 `uv_loop_t`（自己 `uv_loop_init` 的调用点）。
 */
template <typename Pred>
inline bool wait_until(uv_loop_t* loop, Pred pred, int deadline_ms) {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (pred()) return true;
    if (elapsed_ms(t0) >= deadline_ms) return false;
    uv_run(loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

/**
 * @brief 不泵循环，只等一个跨线程标志。
 * @return 期限内是否置位。
 */
template <typename Pred>
inline bool wait_flag(Pred pred, int deadline_ms) {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  while (!pred()) {
    if (elapsed_ms(t0) >= deadline_ms) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

/**
 * @brief 什么都不等，纯粹泵 \p ms 毫秒（墙钟）。
 */
template <typename LoopT>
inline void pump_for(LoopT* loop, int ms) {
  wait_until(loop, [] { return false; }, ms);
}

/**
 * @brief 同 `pump_for`，但一次泵**两个**循环。
 */
template <typename LoopT>
inline void pump_for_pair(LoopT* a, LoopT* b, int ms) {
  wait_until_pair(a, b, [] { return false; }, ms);
}

}  // namespace uvcpp_test

#endif  // UVCPP_TESTS_FUNCTIONAL_WAIT_UTIL_H_
