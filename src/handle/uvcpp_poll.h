/**
 * @file src/handle/uvcpp_poll.h
 * @brief C++ wrapper for libuv poll handles (uv_poll_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Allows polling arbitrary file descriptors or sockets for events.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_POLL_H
#define SRC_HANDLE_UVCPP_POLL_H

#include <handle/uvcpp_loop.h>

namespace uvcpp {
class UVCPP_API uvcpp_poll : public uvcpp_handle {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_poll)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_poll)

#ifdef WIN32
  /** @brief Construct poll watcher for Windows using file descriptor. */
  explicit uvcpp_poll(uvcpp_loop *loop, int fd);
#endif
  /** @brief Construct poll watcher for given socket descriptor. */
  explicit uvcpp_poll(uvcpp_loop* loop, uv_os_sock_t socket);

  /** @brief Initialize the poll handle. */
  int init();
#ifdef WIN32
  /** @brief Initialize using Windows fd. */
  int init(uvcpp_loop* loop, int fd);
#endif
  /** @brief Initialize using socket descriptor. */
  int init(uvcpp_loop* loop, uv_os_sock_t socket);
  /** @brief Start polling for \p events and invoke \p start_cb on change. */
  int start(int events, ::std::function<void(uvcpp_poll*, int, int)> start_cb);
  /** @brief Stop polling. */
  int stop();

 protected:
  ::std::function<void(uvcpp_poll*, int, int)> poll_start_cb;
 private:
  /** @brief Internal libuv poll callback. */
  static void callback_start(uv_poll_t* handle, int status, int events);
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_POLL_H