/**
 * @file src/handle/uvcpp_check.h
 * @brief C++ wrapper for libuv check handles (uv_check_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Check watchers run at the end of the loop iteration.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_CHECK_H
#define SRC_HANDLE_UVCPP_CHECK_H

#include <handle/uvcpp_loop.h>

namespace uvcpp {
/**
 * @brief Check handle wrapper executed at end of loop iterations.
 */
class UVCPP_API uvcpp_check : public uvcpp_handle {
public:
  UVCPP_DEFINE_FUNC(uvcpp_check)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_check)

  /** @brief Construct check watcher on \p loop. */
  uvcpp_check(uvcpp_loop *loop);

  /** @brief Initialize check watcher. */
  int init();
  /** @brief Initialize on provided loop. */
  int init(uvcpp_loop *loop);
  /** @brief Start the check watcher; optional callback may be provided. */
  int start();
  int start(::std::function<void(uvcpp_check *)> start_cb);

  /** @brief Stop the check watcher. */
  int stop();

protected:
  ::std::function<void(uvcpp_check *)> check_start_cb;

private:
  /** @brief Internal libuv callback for check events. */
  static void callback_start(uv_check_t *handle);

private:
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_CHECK_H