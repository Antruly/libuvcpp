/**
 * @file src/handle/uvcpp_timer.h
 * @brief C++ wrapper for libuv timers (uv_timer_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Timer wrapper that provides start/stop and callback semantics.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_TIMER_H
#define SRC_HANDLE_UVCPP_TIMER_H

#include <handle/uvcpp_loop.h>

namespace uvcpp {
/**
 * @brief Timer handle wrapper for scheduling callbacks after timeout/repeat.
 */
class UVCPP_API uvcpp_timer : public uvcpp_handle {
public:
  UVCPP_DEFINE_FUNC(uvcpp_timer)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_timer)
  /** @brief Construct timer associated with \p loop. */
  uvcpp_timer(uvcpp_loop *loop);

  /** @brief Initialize timer resources. */
  int init();
  /** @brief Initialize timer on a specific loop. */
  int init(uvcpp_loop* loop);
  /** @brief Start timer with callback, initial \p timeout and \p repeat interval. */
  int start(::std::function<void(uvcpp_timer*)> start_cb, uint64_t timeout, uint64_t repeat);
  /** @brief Stop the timer. */
  int stop();

protected:
  std::function<void(uvcpp_timer *)> timer_start_cb;

private:
  /** @brief Internal libuv timer callback. */
  static void callback_start(uv_timer_t* handle);
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_TIMER_H