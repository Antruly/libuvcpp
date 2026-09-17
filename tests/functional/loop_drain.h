/**
 * @file tests/functional/loop_drain.h
 * @brief 测试侧循环收尾守卫。
 * @author zhuweiye
 * @version 1.0.0
 */

#ifndef UVCPP_TESTS_FUNCTIONAL_LOOP_DRAIN_H_
#define UVCPP_TESTS_FUNCTIONAL_LOOP_DRAIN_H_

#include "handle/uvcpp_loop.h"

namespace uvcpp_test {

/**
 * @brief 在循环还活着的时候把收尾队列拨干净。
 *
 * 契约（见 `src/handle/uvcpp_loop.cpp` 的析构分支）：属主必须在销毁循环**之前**
 * 把句柄关掉、并把已经排进 endgame 的关闭回调拨完。测试里常见写法是
 * `uvcpp_loop loop;` 声明在最前面、句柄都在它后面，析构逆序就成了「句柄先关、
 * 循环后关」—— 句柄的 `uv_close` 全排进循环的收尾队列，而此刻已经没有人再
 * `uv_run` 它了，`uv_loop_close()` 只能返回 `UV_EBUSY`，整个 `uv_loop_t`
 * 内存留下来（每处约 550 字节）。
 *
 * 本守卫声明在**循环之后、句柄之前**，析构顺序即「句柄 → 本守卫 → 循环」，
 * 正好补上那一次拨动。
 *
 * 判据用 `loop_alive()` 而不是 `loop_close()`：后者内部会 `stop()`，而 `uv_run`
 * 的 `while (r != 0 && loop->stop_flag == 0)` 是**进 body 之前**判的，停标志一
 * 立那一轮就整段空转，交替调用等于原地打转。次数有界是为了**绝不卡住**。
 */
/**
 * @brief 拨到收尾队列空（或到 256 轮上限）为止。
 *
 * 给"循环不随作用域走"的调用点用（例如 `new uvcpp_loop()` 之后显式
 * `delete`）。判据与轮数上限的理由同 `loop_drain`。
 */
inline void drain(uvcpp::uvcpp_loop* loop) {
  if (loop == nullptr) return;
  for (int i = 0; i < 256 && loop->loop_alive() != 0; ++i) {
    loop->run(UV_RUN_NOWAIT);
  }
}

class loop_drain {
 public:
  explicit loop_drain(uvcpp::uvcpp_loop* loop) : loop_(loop) {}

  ~loop_drain() { drain(loop_); }

  loop_drain(const loop_drain&) = delete;
  loop_drain& operator=(const loop_drain&) = delete;

 private:
  uvcpp::uvcpp_loop* loop_;
};

}  // namespace uvcpp_test

#endif  // UVCPP_TESTS_FUNCTIONAL_LOOP_DRAIN_H_
