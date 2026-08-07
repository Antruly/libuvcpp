/**
 * @file src/net/uvcpp_tcp_server.h
 * @brief Higher-level TCP server with bind/listen convenience and auto client management.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Wraps uvcpp_tcp to provide a simple bind+listen API with automatic
 * callback setup for accepted clients (default write/close handlers,
 * missing-read-callback warnings). Designed for single-threaded event-loop
 * usage — IO-intensive work should be offloaded via uvcpp_work.
 */

#pragma once
#ifndef SRC_NET_UVCPP_TCP_SERVER_H
#define SRC_NET_UVCPP_TCP_SERVER_H

#include <functional>
#include <list>
#include <uv.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_tcp.h>
#include <net/uvcpp_tcp_client.h>

namespace uvcpp {

/**
 * @brief Bitmask status flags for TCP server lifecycle.
 */
enum uvcpp_tcp_server_status : int {
  TCP_SERVER_NONE      = 0x00,  ///< Initial state
  TCP_SERVER_LISTENING = 0x02,  ///< Actively accepting connections
  TCP_SERVER_STOPPING  = 0x04,  ///< stop() called, closing
  TCP_SERVER_STOPPED   = 0x08,  ///< Fully stopped
  TCP_SERVER_ERROR     = 0x10   ///< Error occurred (see last_error_code)
};

/**
 * @brief Application-layer TCP server wrapping uvcpp_tcp.
 *
 * Provides convenience bind (IPv4/IPv6) and listen with automatic
 * per-client callback management.
 *
 * After the user's connection callback returns, the server checks the
 * new client's callback state:
 * - If a read callback was NOT registered, a warning is printed to stderr
 *   and reading is NOT started.
 * - A default close callback is installed that auto-deletes the client
 *   when the connection closes (unless the user already set one).
 * - Write buffers are automatically freed by uvcpp_write regardless of
 *   whether a user callback was provided.
 *
 * IMPORTANT: All callbacks (connection, read, write, close) run on
 * the event-loop thread. Do NOT perform IO-intensive work (file I/O,
 * heavy computation, blocking calls) inside any callback — it will block
 * the entire event loop. Use uvcpp_work to offload heavy tasks:
 *
 * @code
 *   uvcpp_work* w = new uvcpp_work();
 *   w->queue_work(server->get_loop(),
 *     [](uvcpp_work* w) {
 *       // Heavy work here (runs on worker thread)
 *     },
 *     [](uvcpp_work* w, int status) {
 *       // Process results, send response (runs on loop thread)
 *       delete w;
 *     });
 * @endcode
 *
 * Simple data checks and async writes are fine inline. Do NOT use
 * sync write_wait/read_wait inside any callback.
 *
 * Usage:
 * @code
 *   uvcpp_tcp_server server;
 *   server.bind("0.0.0.0", 8080);
 *   server.listen([](uvcpp_tcp_client* client) {
 *     // New connection accepted — register callbacks on client
 *     client->read_start([](uvcpp_buf* buf) {
 *       // Handle received data (keep this fast!)
 *       // Echo back:
 *       client->write(buf->get_data(), buf->size(),
 *         [](int status) { // optional write completion });
 *     });
 *     // write/close callbacks are optional — server provides defaults
 *   });
 *   server.run(UV_RUN_DEFAULT);
 * @endcode
 */
class UVCPP_API uvcpp_tcp_server {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_tcp_server)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_tcp_server)

  // -----------------------------------------------------------------
  // Accessors
  // -----------------------------------------------------------------

  /** @brief Return the underlying uvcpp_tcp handle. */
  uvcpp_tcp* get_tcp();

  /** @brief Return the internal event loop. */
  uvcpp_loop* get_loop();

  /** @brief Return the current status bitmask. */
  int get_status() const;

  /** @brief Check whether all given status flags are set. */
  bool has_status(int flags) const;

  /** @brief Return the last recorded error code (libuv errno). */
  int get_last_error() const;

  // -----------------------------------------------------------------
  // Bind
  // -----------------------------------------------------------------

  /**
   * @brief Bind to an address, auto-detecting IPv4 vs IPv6.
   * @param ip   IPv4 or IPv6 address string.
   * @param port Port number.
   * @return 0 on success, libuv error code on failure.
   */
  int bind(const char* ip, int port);

  /** @brief Bind to an IPv4 address. */
  int bindIpv4(const char* ip, int port);

  /** @brief Bind to an IPv6 address. */
  int bindIpv6(const char* ip, int port);

  // -----------------------------------------------------------------
  // Listen
  // -----------------------------------------------------------------

  /**
   * @brief Start listening for incoming connections.
   *
   * @param connection_cb  Called for each new connection with a
   *                       uvcpp_tcp_client* whose TCP handle has
   *                       already been accepted. The client is in
   *                       CONNECTED | READABLE | WRITABLE state.
   *
   *                       Thread safety: this callback runs on
   *                       the event-loop thread. Do not block, do
   *                       not perform IO-intensive work. Use
   *                       uvcpp_work::queue_work() for heavy tasks.
   *
   *                       Simple data inspection and async writes
   *                       are safe. Do NOT use sync write_wait or
   *                       read_wait inside this or any event-loop
   *                       callback.
   *
   * @param backlog  Listen backlog (default 128).
   * @return 0 on success, libuv error code on failure.
   */
  int listen(std::function<void(uvcpp_tcp_client*)> connection_cb,
             int backlog = 128);

  // -----------------------------------------------------------------
  // Stop
  // -----------------------------------------------------------------

  /**
   * @brief Stop accepting new connections and close the server.
   *
   * Closes all tracked clients, then closes the server TCP handle.
   * @param on_stopped Optional callback invoked when the server has
   *                   fully stopped.
   */
  void stop(std::function<void()> on_stopped = nullptr);

  // -----------------------------------------------------------------
  // Loop control
  // -----------------------------------------------------------------

  /** @brief Run the internal event loop. */
  int run(uv_run_mode md = UV_RUN_DEFAULT);

  /** @brief Stop the internal event loop. */
  void stop_loop();

 private:
  // -----------------------------------------------------------------
  // Internal helpers
  // -----------------------------------------------------------------

  void set_status(int flags);
  void clear_status(int flags);

  /** @brief Post-connection callback: check/set client callbacks. */
  void setup_client_callbacks(uvcpp_tcp_client* client);

  // Trampoline — C-style fn ptr + void* to avoid MSVC std::function
  // copy-chain corruption through libuv's callback layers.
  using connection_callback_t = void(*)(uvcpp_tcp_client* client,
                                        void* arg);

  // -----------------------------------------------------------------
  // Member variables
  // -----------------------------------------------------------------

  uvcpp_loop* loop_ = nullptr;
  uvcpp_tcp*  tcp_  = nullptr;
  bool stopped_     = false;  ///< true if stop() already closed the handle
  int status_       = TCP_SERVER_NONE;
  int last_error_code_ = 0;

  /** @brief Tracked connected clients (for cleanup on stop). */
  std::list<uvcpp_tcp_client*> clients_;

  /** @brief User connection callback stored as trampoline pair. */
  connection_callback_t on_connection_fn_ = nullptr;
  void*                 on_connection_arg_ = nullptr;
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_TCP_SERVER_H
