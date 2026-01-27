/**
 * @file src/handle/uvcpp_prepare.h
 * @brief C++ wrapper for libuv prepare handles (uv_prepare_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Prepare watchers run before the event loop checks for I/O.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_PREPARE_H
#define SRC_HANDLE_UVCPP_PREPARE_H

#include <handle/uvcpp_loop.h>

namespace uvcpp {
class UVCPP_API uvcpp_prepare : public uvcpp_handle {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_prepare)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_prepare)
  /** @brief Construct prepare watcher bound to \p loop. */
  uvcpp_prepare(uvcpp_loop* loop);

  /** @brief Initialize prepare watcher. */
  int init();
  /** @brief Initialize on a specific loop. */
  int init(uvcpp_loop* loop);

  /** @brief Start the prepare watcher with callback invoked each iteration. */
  int start(::std::function<void(uvcpp_prepare*)> start_cb);
  /** @brief Stop the prepare watcher. */
  int stop();

 protected:
  ::std::function<void(uvcpp_prepare*)> prepare_start_cb;
 private:
  /** @brief Internal libuv prepare callback. */
  static void callback(uv_prepare_t* handle);

 private:
};

} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_PREPARE_H