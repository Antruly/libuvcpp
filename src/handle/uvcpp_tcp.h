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

// ---------------------------------------------------------------------------
// REUSEPORT 的**版本分叉**：这份 libuv 认得这个标志就给真值，不认得就是 0。
//
// `UV_TCP_REUSEPORT` 是 libuv **1.49** 才加进来的，而且它是 `enum uv_tcp_flags`
// 的枚举量、**不是宏** —— 所以老版本上 `#ifdef UV_TCP_REUSEPORT` 判不出来，
// 只能按版本号分叉。这不是纸面上的讲究：本仓 CI 的 ubuntu 腿装的是**系统
// libuv**（`apt-get install libuv1-dev`，ubuntu-24.04 给的是 1.48），直接用那个
// 名字在那里是**编不过**的（`error: 'UV_TCP_REUSEPORT' was not declared in
// this scope`），而其它腿用的是自带的 1.51 —— 这个差异只有一种腿看得见。
//
// 取 0 的含义是"这个标志用不上，退回既有退路"：`uvcpp_tcp_server::bind_flags_
// for_loops()` 拿到 0 之后，`set_loops(n>1)` 走的就是本仓原本就有的那条形状
// （一条接受者 + socket 转手），不会少开线程、也不会静默改语义。
//
// 这一段放在文件**最末尾**是有意的：本头在 `tests/tools/doc_line_refs.lock`
// 里有一条 59-60 的引用，插在它下面才不会让那条引用位移。
// ---------------------------------------------------------------------------
#if defined(UV_VERSION_HEX) && UV_VERSION_HEX >= 0x013100
#define UVCPP_TCP_REUSEPORT_FLAG UV_TCP_REUSEPORT
#else
#define UVCPP_TCP_REUSEPORT_FLAG 0
#endif

#endif // SRC_HANDLE_UVCPP_TCP_H