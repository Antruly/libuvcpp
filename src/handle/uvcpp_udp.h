/**
 * @file src/handle/uvcpp_udp.h
 * @brief C++ wrapper for libuv UDP handles (uv_udp_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides UDP send/receive helpers and multicast configuration helpers.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_UDP_H
#define SRC_HANDLE_UVCPP_UDP_H

#include <uvcpp/uvcpp_buf.h>
#include <handle/uvcpp_loop.h>
#include <req/uvcpp_connect.h>
#include <req/uvcpp_udp_send.h>

namespace uvcpp {
/**
 * @brief UDP wrapper exposing send/receive and socket option operations.
 */
class UVCPP_API uvcpp_udp : public uvcpp_handle {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_udp)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_udp)

  /** @brief Construct UDP wrapper bound to \p loop. */
  uvcpp_udp(uvcpp_loop* loop);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 6
  /** @brief Construct with flags when supported by libuv. */
  uvcpp_udp(uvcpp_loop* loop, unsigned int flags);
#endif
#endif

  /** @brief Initialize resources for UDP. */
  int init();
  /** @brief Initialize and bind to loop. */
  int init(uvcpp_loop* loop);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 6
  /** @brief Initialize with flags (optional). */
  int init(uvcpp_loop* loop, unsigned int flags);
#endif
#endif

  /** @brief Open an existing socket descriptor. */
  int open(uv_os_sock_t sock);
  /** @brief Bind to address with \p flags. */
  int bind(const struct sockaddr* addr, unsigned int flags);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 27
  /** @brief Connect UDP socket to a remote peer (optional in newer libuv). */
  int connect(const struct sockaddr* addr);
  /** @brief Get peer socket name for connected UDP. */
  int getpeername(struct sockaddr* name, int* namelen);
#endif
#endif
  /** @brief Get local socket name. */
  int getsockname(struct sockaddr* name, int* namelen);
  /** @brief Join/leave multicast group for \p multicast_addr on \p interface_addr. */
  int set_membership(const char* multicast_addr, const char* interface_addr,
                    uv_membership membership);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 32
  /** @brief Set source-specific multicast membership when supported. */
  int set_source_membership(const char* multicast_addr,
                          const char* interface_addr, const char* source_addr,
                          uv_membership membership);
#endif
#endif

  /** @brief Enable/disable multicast loopback. */
  int set_multicast_loop(int on);
  /** @brief Set multicast TTL. */
  int set_multicast_ttl(int ttl);
  /** @brief Set outgoing multicast interface. */
  int set_multicast_interface(const char* interface_addr);
  /** @brief Enable/disable broadcast on socket. */
  int set_broadcast(int on);
  /** @brief Set IP TTL for outgoing packets. */
  int set_ttl(int ttl);

  /**
   * @brief Send data via UDP asynchronously.
   * @param req Send request wrapper.
   * @param bufs Buffers to send.
   * @param nbufs Buffer count.
   * @param addr Destination address.
   * @param udp_send_cb Completion callback.
   */
  int send(uvcpp_udp_send *req, const uv_buf_t bufs[], unsigned int nbufs,
           const struct sockaddr* addr,
           ::std::function<void(uvcpp_udp_send*, int)> udp_send_cb);
  /** @brief Try to send without queuing; returns error if would block. */
  int try_send(const uv_buf_t bufs[], unsigned int nbufs,
              const struct sockaddr* addr) {
    return uv_udp_try_send(UVCPP_UDP_HANDLE, reinterpret_cast<const ::uv_buf_t*>(bufs), nbufs, addr);
  }

  /**
   * @brief Start receiving UDP datagrams.
   * @param alloc_cb Buffer allocator.
   * @param udp_recv_cb Callback invoked for each received datagram.
   */
  int recv_start(
      ::std::function<void(uvcpp_handle *, size_t, uv_buf_t*)> alloc_cb,
                ::std::function<void(uvcpp_udp*, ssize_t, const uv_buf_t*,
                                   const struct sockaddr*,
                                   unsigned int)> udp_recv_cb_);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 39
  /** @brief Return whether recvmmsg batching is being used (if supported). */
  int using_recvmmsg();
#endif
#endif
  /** @brief Stop receiving datagrams. */
  int recv_stop() { return uv_udp_recv_stop(UVCPP_UDP_HANDLE); }
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 19
  /** @brief Return size of the send queue in bytes. */
  size_t get_send_queue_size();
  /** @brief Return number of queued send requests. */
  size_t get_send_queue_count();
#endif
#endif
 

 protected:
  ::std::function<void(uvcpp_udp *, ssize_t, const uv_buf_t*,
                        const struct sockaddr *, unsigned int)>
      udp_recv_cb;
 private:

  // uv_udp_recv_cb
  static void callback_udp_recv(uv_udp_t* handle, ssize_t nread,
                                const uv_buf_t* buf,
                                const struct sockaddr* addr,
                                unsigned flags);

private:
};

} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_UDP_H