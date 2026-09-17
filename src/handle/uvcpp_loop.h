/**
 * @file src/handle/uvcpp_loop.h
 * @brief C++ wrapper for libuv event loop (uv_loop_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides access to the default loop and utilities to run/stop/walk handles.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_LOOP_H
#define SRC_HANDLE_UVCPP_LOOP_H

#include <handle/uvcpp_handle.h>

namespace uvcpp {
/**
 * @brief Event loop wrapper around uv_loop_t.
 */
class UVCPP_API uvcpp_loop : public uvcpp_handle {
public:
  /** @brief Return the process default loop. */
  static uvcpp_loop *default_loop();
  UVCPP_DEFINE_FUNC(uvcpp_loop)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_loop)

  /** @brief Initialize loop resources. */
  int init();
  /** @brief Return whether the loop is alive (has active handles/requests). */
  int loop_alive();
  /** @brief Stop the loop. */
  void stop();
  /** @brief Close the loop and release resources. */
  int loop_close();
  /** @brief Close wrapper handle. */
  int close();
  /** @brief Run the loop with the specified mode (default UV_RUN_DEFAULT). */
  int run(uv_run_mode md = UV_RUN_DEFAULT);

  /**
   * @brief `run()` 正在（本线程）跑，还没返回 —— 也就是"当前就在某个回调里"。
   *
   * `uv_run` **不可重入**，而本库有几条析构路径要在收尾时拨几轮循环把挂起的
   * 关闭回调放掉。那些路径如果是在**本循环自己的回调里**被调到的，就既不能
   * 再拨一次循环，也不能把循环关掉释放 —— 外层那一帧 `uv_run` 返回之后还要
   * 接着用它。判据就是这一个。
   *
   * 计数而非布尔：进来的路不止一条（拨一轮只是加上一层），退的时候要一层一层
   * 退干净，中途任何一层里问都是"在跑"。
   */
  bool is_running() const { return run_depth_ > 0; }
  /** @brief Walk all handles attached to the loop and invoke \p walk_cb. */
  void walk(::std::function<void(uvcpp_handle*, void*)> walk_cb, void* arg);

  protected:
  std::function<void(uvcpp_handle *, void *)> handle_walk_cb;

  static void callback_walk(uv_handle_t *handle, void *arg);

private:
  /**
   * @brief `loop_close()` 成功过。
   *
   * 成功之后 `uv_loop_t` 已经被 libuv 收掉（debug 构建里还会把整块内存填成
   * -1），**不能再碰**：连 `uv_loop_alive()` 都是读垃圾，`uv_run()` 更会去
   * 解引用一堆野指针。析构里靠这一位决定"还能不能收尾"。
   */
  bool closed_ = false;
  void *walk_arg_ = nullptr;

  /** @brief `run()` 的嵌套层数，见 `is_running()`。 */
  int run_depth_ = 0;
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_LOOP_H