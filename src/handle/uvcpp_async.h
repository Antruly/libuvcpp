/**
 * @file src/handle/uvcpp_async.h
 * @brief C++ wrapper for libuv async handles (uv_async_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Async handles allow cross-thread notifications into the event loop.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_ASYNC_H
#define SRC_HANDLE_UVCPP_ASYNC_H

#include <handle/uvcpp_loop.h>

namespace uvcpp {
/**
 * @brief Async handle wrapper for cross-thread loop notifications.
 */
class UVCPP_API uvcpp_async : public uvcpp_handle {
public:
  UVCPP_DEFINE_FUNC(uvcpp_async)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_async)

  /** @brief Construct async handle bound to \p loop. */
  uvcpp_async(uvcpp_loop *loop);
  /** @brief Initialize the async handle. */
  int init();
  /** @brief Initialize on the provided loop. */
  int init(uvcpp_loop *loop);
  /** @brief Initialize with callback invoked on async events. */
  int init(::std::function<void(uvcpp_async *)> init_cb, uvcpp_loop *loop);

  /** @brief Send an async notification to the loop from another thread. */
  int send() { return uv_async_send(UVCPP_ASYNC_HANDLE); }

protected:
  ::std::function<void(uvcpp_async *)> async_init_cb;

private:
  /** @brief Internal libuv async callback. */
  static void callback_init(uv_async_t *handle);

private:
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_ASYNC_H