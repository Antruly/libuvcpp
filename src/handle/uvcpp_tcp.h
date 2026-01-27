/**
 * @file src/handle/uvcpp_tcp.h
 * @brief C++ wrapper for libuv TCP handles (uv_tcp_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Exposes TCP-specific operations like bind, connect and socket options.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_TCP_H
#define SRC_HANDLE_UVCPP_TCP_H

#include <list>
#include <req/uvcpp_connect.h>
#include <handle/uvcpp_stream.h>

namespace uvcpp {
/**
 * @brief TCP stream wrapper providing socket-level operations.
 */
class UVCPP_API uvcpp_tcp : public uvcpp_stream {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_tcp)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_tcp)
  /** @brief Construct TCP wrapper bound to \p loop. */
  uvcpp_tcp(uvcpp_loop* loop);
  /** @brief Construct with flags supported by libuv. */
  uvcpp_tcp(uvcpp_loop* loop, unsigned int flags);

  /** @brief Initialize internal tcp resources. */
  int init();
  /** @brief Initialize on provided loop instance. */
  int init(uvcpp_loop* loop);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 7
  /** @brief Initialize with libuv flags (if supported). */
  int init(uvcpp_loop* loop, unsigned int flags);
#endif
#endif

  /** @brief Open an existing OS socket descriptor. */
  int open(uv_os_sock_t sock);
  /** @brief Enable/disable TCP_NODELAY. */
  int nodelay(int enable);
  /** @brief Enable keepalive with \p delay (seconds). */
  int keepalive(int enable, unsigned int delay);
  /** @brief Enable or disable simultaneous accepts on Windows. */
  int simultaneousAccepts(int enable);

  /** @brief Bind socket to address \p addr with \p flags. */
  int bind(const struct sockaddr* addr, unsigned int flags);
  /** @brief Get local socket name. */
  int getsockname(struct sockaddr* name, int* namelen);
  /** @brief Get peer socket name. */
  int getpeername(struct sockaddr* name, int* namelen);

  /** @brief Convenience helpers to bind IPv4/IPv6 addresses. */
  int bindIpv4(const char* addripv4, int port, int flags = 0);
  int bindIpv6(const char* addripv6, int port, int flags = 0);

  /**
   * @brief Initiate an asynchronous connect.
   * @param req Connect request wrapper.
   * @param addr Remote sockaddr.
   * @param connect_cb Completion callback called with status.
   */
  int connect(uvcpp_connect* req,
              const struct sockaddr* addr,
              ::std::function<void(uvcpp_connect*, int)> connect_cb);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 32
  /** @brief Close the connection and reset state, invoking \p close_cb when done. */
  int closeReset(::std::function<void(uvcpp_handle*)> close_cb);
#endif
#endif

 protected:

 private:
  ::std::list<sockaddr*> addrs;
   
};

} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_TCP_H