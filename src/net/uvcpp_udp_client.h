/**
 * @file src/net/uvcpp_udp_client.h
 * @brief Higher-level UDP client with async/sync dual-mode API built on uvcpp_udp.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides both callback-based (async) and blocking-wait (sync) operations
 * for UDP send and receive, with status tracking and address helpers.
 */

#pragma once
#ifndef SRC_NET_UVCPP_UDP_CLIENT_H
#define SRC_NET_UVCPP_UDP_CLIENT_H

#include <functional>
#include <string>
#include <uv.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_udp.h>
#include <uvcpp/uvcpp_buf.h>

namespace uvcpp {

/**
 * @brief Bitmask status flags for UDP client lifecycle.
 */
enum uvcpp_udp_client_status : int {
  UDP_CLIENT_NONE      = 0x00,  ///< Initial state
  UDP_CLIENT_BOUND     = 0x01,  ///< Bound to local address
  UDP_CLIENT_CONNECTED = 0x02,  ///< Connected to remote peer
  UDP_CLIENT_READABLE  = 0x04,  ///< Data available in recv cache
  UDP_CLIENT_CLOSING   = 0x10,  ///< Close in progress
  UDP_CLIENT_CLOSED    = 0x20,  ///< Handle closed
  UDP_CLIENT_ERROR     = 0x40   ///< Error occurred (see last_error_code)
};

/**
 * @brief Application-layer UDP client wrapping uvcpp_udp.
 *
 * Dual-mode API:
 * - async:  provide a callback; the method returns immediately and the
 *           callback fires on the internal event loop when complete.
 * - sync:   omit the callback (or use the *_wait variants); the method
 *           blocks up to timeout_ms (default 30000) pumping the loop.
 *
 * Send supports two forms:
 * - With (ip, port): send to a specific address each time.
 * - Without address: send to a previously connected peer (requires
 *   prior connect() call; libuv >= 1.27).
 *
 * Usage (sync):
 * @code
 *   uvcpp_udp_client client;
 *   client.bind("0.0.0.0", 0);
 *   client.send_wait("127.0.0.1", 9999, "hello", 5);
 *   uvcpp_buf buf;
 *   client.recv_wait(buf);
 * @endcode
 *
 * Usage (async):
 * @code
 *   uvcpp_udp_client client;
 *   client.bind("0.0.0.0", 0);
 *   client.recv_start([](uvcpp_buf* b, const char* ip, int port) {
 *     // handle received data
 *   });
 *   client.send("127.0.0.1", 9999, "ping", 4, [](int s) { ... });
 *   client.run(UV_RUN_DEFAULT);
 * @endcode
 */
class UVCPP_API uvcpp_udp_client {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_udp_client)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_udp_client)

  // -----------------------------------------------------------------
  // Accessors
  // -----------------------------------------------------------------

  /** @brief Return the underlying uvcpp_udp handle. */
  uvcpp_udp* get_udp();

  /** @brief Return the internal event loop. */
  uvcpp_loop* get_loop();

  /** @brief Return the current status bitmask. */
  int get_status() const;

  /** @brief Check whether all given status flags are set. */
  bool has_status(int flags) const;

  /** @brief Return the last recorded error code (libuv errno). */
  int get_last_error() const;

  // -----------------------------------------------------------------
  // Address helpers
  // -----------------------------------------------------------------

  /**
   * @brief Retrieve the local socket address.
   * @param[out] ip   Receives the IP string.
   * @param[out] port Receives the port number in host byte order.
   * @return The raw sockaddr_storage.
   */
  sockaddr_storage getLocalAddrs(std::string& ip, int& port);

  /**
   * @brief Retrieve the remote (peer) socket address.
   * @return The raw sockaddr_storage.
   */
  sockaddr_storage getPeerAddrs(std::string& ip, int& port);

  // -----------------------------------------------------------------
  // Bind / Connect
  // -----------------------------------------------------------------

  /** @brief Bind to local address (auto-detect IPv4/IPv6). */
  int bind(const char* ip, int port);

  /** @brief Bind to IPv4 address. */
  int bindIpv4(const char* ip, int port);

  /** @brief Bind to IPv6 address. */
  int bindIpv6(const char* ip, int port);

  /**
   * @brief "Connect" to a remote peer (libuv >= 1.27).
   *
   * After connecting, send() can be called without specifying an
   * address.  This does NOT establish a real connection — it just
   * sets the default destination for packets.
   */
  int connect(const char* ip, int port);

  // -----------------------------------------------------------------
  // Send (to specific address)
  // -----------------------------------------------------------------

  int send(const char* ip, int port, const char* data, size_t len,
           std::function<void(int)> cb = nullptr);
  int send_wait(const char* ip, int port, const char* data, size_t len,
                int timeout_ms = 30000);
  int send(const char* ip, int port, const uvcpp_buf& buf,
           std::function<void(int)> cb = nullptr);
  int send(const char* ip, int port, uvcpp_buf* buf,
           std::function<void(int)> cb = nullptr);

  // -----------------------------------------------------------------
  // Send (to connected peer — requires prior connect())
  // -----------------------------------------------------------------

  int send(const char* data, size_t len,
           std::function<void(int)> cb = nullptr);
  int send_wait(const char* data, size_t len, int timeout_ms = 30000);
  int send(const uvcpp_buf& buf, std::function<void(int)> cb = nullptr);
  int send(uvcpp_buf* buf, std::function<void(int)> cb = nullptr);

  // -----------------------------------------------------------------
  // Receive
  // -----------------------------------------------------------------

  /**
   * @brief Start receiving datagrams.
   *
   * Async mode (cb != nullptr): cb(buf, sender_ip, sender_port) is
   * called for every received datagram.
   *
   * Sync mode (cb == nullptr): enables the internal recv cache for
   * subsequent recv_wait() calls.
   */
  int recv_start(std::function<void(uvcpp_buf*, const char*, int)> cb = nullptr);

  /** @brief Synchronously wait for and return received data. */
  int recv_wait(uvcpp_buf& out_buf, int timeout_ms = 30000);

  /** @brief Stop receiving datagrams. */
  int recv_stop();

  // -----------------------------------------------------------------
  // Read cache
  // -----------------------------------------------------------------

  void set_max_read_cache_size(size_t max_size);
  size_t get_max_read_cache_size() const;

  // -----------------------------------------------------------------
  // Loop control
  // -----------------------------------------------------------------

  int run(uv_run_mode md = UV_RUN_DEFAULT);
  void stop();

 private:
  void set_status(int flags);
  void clear_status(int flags);

  bool wait_for_condition(std::function<bool()> condition, int timeout_ms);
  void ensure_read_cache();

  /** @brief Resolve ip:port to sockaddr_in (IPv4). */
  static bool resolve_addr(const char* ip, int port, struct sockaddr_in& addr);

  // Internal alloc/recv callbacks
  static void internal_alloc_cb(uvcpp_handle* h, size_t sz, uv_buf_t* buf);
  void on_internal_recv(uvcpp_udp* u, ssize_t nread, const uv_buf_t* buf,
                        const struct sockaddr* addr, unsigned int flags);

  // Trampolines (free functions in .cpp, not class members)
  using send_callback_t = void(*)(int status, void* arg);
  using recv_callback_t  = void(*)(uvcpp_buf* buf, const char* ip,
                                   int port, void* arg);

  // -----------------------------------------------------------------
  // Member variables
  // -----------------------------------------------------------------

  uvcpp_loop* loop_ = nullptr;
  uvcpp_udp*  udp_  = nullptr;
  int status_       = UDP_CLIENT_NONE;
  int last_error_code_ = 0;

  // Read cache
  uvcpp_buf* read_cache_          = nullptr;
  size_t     max_read_cache_size_ = 2 * 1024 * 1024;
  bool       recv_started_        = false;

  // Mode tracking
  bool has_async_send_cb_  = false;
  bool has_async_recv_cb_  = false;

  // Async callbacks (trampoline pattern)
  send_callback_t send_fn_  = nullptr;
  void*           send_arg_ = nullptr;
  recv_callback_t recv_fn_  = nullptr;
  void*           recv_arg_ = nullptr;

  // Sync coordination
  bool sync_send_done_   = false;
  int  sync_send_result_ = 0;
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_UDP_CLIENT_H
