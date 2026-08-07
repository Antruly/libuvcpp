/**
 * @file src/net/uvcpp_tcp_client.h
 * @brief Higher-level TCP client with async/sync dual-mode API built on uvcpp_tcp.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides both callback-based (async) and blocking-wait (sync) operations
 * with internal read cache, status tracking, and address helpers.
 */

#pragma once
#ifndef SRC_NET_UVCPP_TCP_CLIENT_H
#define SRC_NET_UVCPP_TCP_CLIENT_H

#include <functional>
#include <string>
#include <uv.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_tcp.h>
#include <uvcpp/uvcpp_buf.h>

namespace uvcpp {

/**
 * @brief Bitmask status flags for TCP client lifecycle.
 *
 * Multiple flags can be combined: a connected, readable, writable client
 * has status TCP_CLIENT_CONNECTED | TCP_CLIENT_READABLE | TCP_CLIENT_WRITABLE.
 */
enum uvcpp_tcp_client_status : int {
  TCP_CLIENT_NONE       = 0x00,  ///< Initial state
  TCP_CLIENT_CONNECTING = 0x01,  ///< Connect in progress
  TCP_CLIENT_CONNECTED  = 0x02,  ///< Connection established
  TCP_CLIENT_READABLE   = 0x04,  ///< Data available to read
  TCP_CLIENT_WRITABLE   = 0x08,  ///< Ready to write
  TCP_CLIENT_CLOSING    = 0x10,  ///< Close in progress
  TCP_CLIENT_CLOSED     = 0x20,  ///< Connection closed
  TCP_CLIENT_ERROR      = 0x40   ///< An error occurred (see last_error_code)
};

/**
 * @brief Application-layer TCP client wrapping uvcpp_tcp.
 *
 * Dual-mode API:
 * - async:  provide a callback; the method returns immediately and the
 *           callback fires on the internal event loop when complete.
 * - sync:   omit the callback (or use the *_wait variants); the method
 *           blocks up to timeout_ms (default 30000) pumping the loop.
 *
 * Mixing async and sync for the same operation (connect/write/read) is
 * not allowed and will throw std::runtime_error.
 *
 * Usage (sync):
 * @code
 *   uvcpp_tcp_client client;
 *   client.connect_wait("127.0.0.1", 8080);
 *   client.write_wait("hello", 5);
 *   uvcpp_buf buf;
 *   client.read_wait(buf);
 * @endcode
 *
 * Usage (async):
 * @code
 *   uvcpp_tcp_client client;
 *   client.connect("127.0.0.1", 8080, [](int status) {
 *     // connected
 *     client.write("data", 4, [](int ws) { ... });
 *     client.read_start([](uvcpp_buf* b) { ... });
 *   });
 *   client.run(UV_RUN_DEFAULT);
 * @endcode
 */
class UVCPP_API uvcpp_tcp_client {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_tcp_client)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_tcp_client)

  /**
   * @brief Construct a client that shares an external event loop.
   *
   * Used by uvcpp_tcp_server to create accepted clients that run on the
   * server's loop. The client does NOT own the loop and will not close it
   * on destruction.
   */
  explicit uvcpp_tcp_client(uvcpp_loop* external_loop);

  // -----------------------------------------------------------------
  // Accessors
  // -----------------------------------------------------------------

  /** @brief Return the underlying uvcpp_tcp handle. */
  uvcpp_tcp* get_tcp();

  /** @brief Return the internal event loop. */
  uvcpp_loop* get_loop();

  /**
   * @brief Return the current status bitmask.
   * @see uvcpp_tcp_client_status
   */
  int get_status() const;

  /** @brief Return the last recorded error code (libuv errno). */
  int get_last_error() const;

  /** @brief Check whether all given status flags are set. */
  bool has_status(int flags) const;

  /** @brief Check whether an async read callback has been registered. */
  bool has_read_callback() const;

  /** @brief Check whether an async write callback has been registered. */
  bool has_write_callback() const;

  /** @brief Check whether a close callback has been registered. */
  bool has_close_callback() const;

  /**
   * @brief Mark the client as accepted (connected without a connect() call).
   *
   * Used by uvcpp_tcp_server to set the initial CONNECTED | READABLE |
   * WRITABLE state on accepted clients so that write() and read_start()
   * work correctly.
   */
  void mark_accepted();

  // -----------------------------------------------------------------
  // Address helpers
  // -----------------------------------------------------------------

  /**
   * @brief Retrieve the local socket address.
   * @param[out] ip  Receives the IP string (IPv4 or IPv6).
   * @param[out] port Receives the port number in host byte order.
   * @return The raw sockaddr_storage for advanced use.
   */
  sockaddr_storage getLocalAddrs(std::string& ip, int& port);

  /**
   * @brief Retrieve the remote (peer) socket address.
   * @param[out] ip  Receives the IP string (IPv4 or IPv6).
   * @param[out] port Receives the port number in host byte order.
   * @return The raw sockaddr_storage for advanced use.
   */
  sockaddr_storage getPeerAddrs(std::string& ip, int& port);

  // -----------------------------------------------------------------
  // Read cache
  // -----------------------------------------------------------------

  /** @brief Set the maximum read cache size in bytes (default 2 MB). */
  void set_max_read_cache_size(size_t max_size);

  /** @brief Get the current maximum read cache size. */
  size_t get_max_read_cache_size() const;

  // -----------------------------------------------------------------
  // Connect
  // -----------------------------------------------------------------

  /**
   * @brief Connect to a remote host.
   *
   * @param ip   IPv4 or IPv6 address string.
   * @param port Port number.
   * @param cb   If non-null, async connect: returns immediately, cb(status)
   *             fires on the internal loop when complete.
   *             If null, behaves like connect_wait(ip, port, 30000).
   * @return 0 on async start, or the connect result for sync mode.
   */
  int connect(const char* ip, int port,
              std::function<void(int)> cb = nullptr);

  /**
   * @brief Synchronous connect with explicit timeout.
   * @throws std::runtime_error if an async connect callback was already
   *         registered.
   */
  int connect_wait(const char* ip, int port, int timeout_ms = 30000);

  // -----------------------------------------------------------------
  // Write
  // -----------------------------------------------------------------

  /**
   * @brief Write data to the connection (raw pointer + length).
   *
   * @param data Pointer to data to send.
   * @param len  Number of bytes to send.
   * @param cb   If non-null, async write: returns immediately, cb(status)
   *             fires when the write completes.
   *             If null, behaves like write_wait(data, len, 30000).
   * @return 0 on async start, or the write result for sync mode.
   */
  int write(const char* data, size_t len,
            std::function<void(int)> cb = nullptr);

  /**
   * @brief Write data from a uvcpp_buf (auto-copy).
   *
   * The buffer's data is copied internally; the caller retains ownership
   * and the original uvcpp_buf is unchanged after the call.
   */
  int write(const uvcpp_buf& buf,
            std::function<void(int)> cb = nullptr);

  /**
   * @brief Write data from a uvcpp_buf pointer (zero-copy, transfers ownership).
   *
   * The buffer's internal data is moved out via out_uv_buf() and passed
   * directly to the write layer.  After the call the uvcpp_buf is empty.
   * The caller still owns the uvcpp_buf object itself (e.g. must delete
   * it if heap-allocated) — only the data payload is transferred.
   *
   * NOTE: do NOT pass the address of a temporary; the pointer must remain
   * valid until the write callback fires (for async) or the call returns
   * (for sync).
   */
  int write(uvcpp_buf* buf,
            std::function<void(int)> cb = nullptr);

  /**
   * @brief Synchronous write with explicit timeout (raw pointer).
   * @throws std::runtime_error if an async write callback was already
   *         registered.
   */
  int write_wait(const char* data, size_t len, int timeout_ms = 30000);

  /**
   * @brief Synchronous write from uvcpp_buf (auto-copy).
   */
  int write_wait(const uvcpp_buf& buf, int timeout_ms = 30000);

  /**
   * @brief Synchronous write from uvcpp_buf pointer (zero-copy).
   */
  int write_wait(uvcpp_buf* buf, int timeout_ms = 30000);

  // -----------------------------------------------------------------
  // Read
  // -----------------------------------------------------------------

  /**
   * @brief Start reading from the connection.
   *
   * Async mode (cb != nullptr): cb is called for every chunk of data
   * received. Use read_stop() to stop.
   *
   * Sync mode (cb == nullptr): enables the internal read cache so that
   * subsequent read_wait() calls can retrieve data.
   */
  int read_start(std::function<void(uvcpp_buf*)> cb = nullptr);

  /**
   * @brief Synchronously wait for and return received data.
   *
   * Blocks until data is available in the internal read cache or timeout.
   * On success the data is moved into out_buf.
   *
   * @throws std::runtime_error if an async read callback was already
   *         registered.
   */
  int read_wait(uvcpp_buf& out_buf, int timeout_ms = 30000);

  /** @brief Stop reading from the connection. */
  int read_stop();

  /**
   * @brief Register a callback to be invoked when the connection closes.
   *
   * Fires on UV_EOF (graceful peer shutdown) or read error.
   * Used by uvcpp_tcp_server to auto-delete disconnected clients.
   */
  void set_on_close(std::function<void()> cb);

  /** @brief Remove any registered close callback. */
  void clear_on_close();

  // -----------------------------------------------------------------
  // Loop control (async mode)
  // -----------------------------------------------------------------

  /** @brief Run the internal event loop (for async mode users). */
  int run(uv_run_mode md = UV_RUN_DEFAULT);

  /** @brief Stop the internal event loop. */
  void stop();

 private:
  // -----------------------------------------------------------------
  // Internal helpers
  // -----------------------------------------------------------------

  void set_status(int flags);
  void clear_status(int flags);
  void reset_status();

  /**
   * @brief Spin-wait with event-loop pumping until condition is true or
   *        timeout expires.
   * @return true if condition became true, false on timeout.
   */
  bool wait_for_condition(std::function<bool()> condition, int timeout_ms);

  /** @brief Ensure the internal read cache buffer is allocated. */
  void ensure_read_cache();

  /** @brief Static alloc callback forwarded to the client instance. */
  static void internal_alloc_cb(uvcpp_handle* h, size_t sz, uv_buf_t* buf);

  /** @brief Internal read callback registered when using sync reads. */
  void on_internal_read(uvcpp_stream* s, ssize_t nread, const uv_buf_t* buf);

  // -----------------------------------------------------------------
  // Member variables
  // -----------------------------------------------------------------

  uvcpp_loop* loop_ = nullptr;
  uvcpp_tcp*  tcp_  = nullptr;
  bool owns_loop_   = true;  ///< false for server-managed clients
  int status_       = TCP_CLIENT_NONE;
  int last_error_code_ = 0;

  // Read cache
  uvcpp_buf* read_cache_          = nullptr;
  size_t     max_read_cache_size_ = 2 * 1024 * 1024;  // 2 MB default
  bool       read_cache_paused_   = false;
  bool       read_started_        = false;  ///< true if tcp_->read_start was called

  // Mode tracking — prevent mixing sync/async
  bool has_async_connect_cb_ = false;
  bool has_async_read_cb_    = false;
  bool has_async_write_cb_   = false;

  // User async callbacks — stored as C-style fn+arg to completely avoid
  // std::function copy-chain issues through libuv's callback layers.
  using connect_callback_t = void(*)(int status, void* arg);
  using write_callback_t   = void(*)(int status, void* arg);
  using read_callback_t    = void(*)(uvcpp_buf* buf, void* arg);
  using close_callback_t   = void(*)(void* arg);

  connect_callback_t connect_fn_  = nullptr;
  void*              connect_arg_ = nullptr;
  write_callback_t   write_fn_    = nullptr;
  void*              write_arg_   = nullptr;
  read_callback_t    read_fn_     = nullptr;
  void*              read_arg_    = nullptr;
  close_callback_t   close_fn_    = nullptr;
  void*              close_arg_   = nullptr;

  // Sync operation coordination
  bool sync_connect_done_   = false;
  int  sync_connect_result_ = 0;
  bool sync_write_done_     = false;
  int  sync_write_result_   = 0;
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_TCP_CLIENT_H
