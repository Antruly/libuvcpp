/**
 * @file src/handle/uvcpp_idle.h
 * @brief C++ wrapper for libuv idle handles (uv_idle_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Idle watchers run when the loop has no other pending events.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_IDLE_H
#define SRC_HANDLE_UVCPP_IDLE_H

#include <handle/uvcpp_loop.h>

namespace uvcpp {
/**
 * @brief Idle handle wrapper that schedules callbacks when the loop is idle.
 */
class UVCPP_API uvcpp_idle : public uvcpp_handle {
public:
  UVCPP_DEFINE_FUNC(uvcpp_idle)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_idle)
  /** @brief Construct idle watcher bound to \p loop. */
  uvcpp_idle(uvcpp_loop *loop);

  /** @brief Initialize idle watcher. */
  int init();
  /** @brief Initialize on specific loop. */
  int init(uvcpp_loop *loop);
  /** @brief Start idle watcher; caller-provided callback optional. */
  int start();
  int start(::std::function<void(uvcpp_idle *)> start_cb);

  /** @brief Stop idle watcher. */
  int stop();
  int stop(::std::function<void(uvcpp_idle *)> stop_cb);

protected:
  ::std::function<void(uvcpp_idle *)> idle_start_cb;
  ::std::function<void(uvcpp_idle *)> idle_stop_cb;

private:
  /** @brief Internal libuv idle callback. */
  static void callback_start(uv_idle_t *handle);
  /** @brief Internal stop helper. */
  void callback_stop(uvcpp_handle *handle);

private:
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_IDLE_H