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
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_LOOP_H