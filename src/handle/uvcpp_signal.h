/**
 * @file src/handle/uvcpp_signal.h
 * @brief C++ wrapper for libuv signal handles (uv_signal_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides signal handling APIs and helpers.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_SIGNAL_H
#define SRC_HANDLE_UVCPP_SIGNAL_H

#include <handle/uvcpp_loop.h>

namespace uvcpp {
/**
 * @brief Signal handle wrapper for registering OS signal callbacks.
 */
class UVCPP_API uvcpp_signal : public uvcpp_handle {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_signal)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_signal)

  /** @brief Construct a signal watcher bound to \p loop. */
  uvcpp_signal(uvcpp_loop* loop);

  /** @brief Initialize internal resources. */
  int init();
  /** @brief Initialize and bind to provided loop. */
  int init(uvcpp_loop* loop);

  /** @brief Start watching for \p signum and call \p start_cb on occurrences. */
  int start(::std::function<void(uvcpp_signal*, int)> start_cb, int signum);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 12
 /** @brief Start a one-shot signal watcher (supported in newer libuv). */
 int start_oneshot(::std::function<void(uvcpp_signal*, int)> start_oneshot_cb,
                   int signum);
#endif
#endif
 
  /** @brief Stop watching for the signal. */
  int stop();

  /** @brief Retrieve system load average into \p avg[3]. */
  void loadavg(double avg[3]);

 protected:
  ::std::function<void(uvcpp_signal*, int)> signal_start_cb;
  ::std::function<void(uvcpp_signal*, int)> signal_start_oneshot_cb;
 private:
  /** @brief Internal start callback from libuv. */
  static void callback_start(uv_signal_t* handle, int signum);

  /** @brief Internal one-shot start callback from libuv. */
  static void callback_start_oneshot(uv_signal_t* handle, int signum);

 private:
};

} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_SIGNAL_H