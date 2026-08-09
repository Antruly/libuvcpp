/**
 * @file src/web/uvcpp_http_client.h
 * @brief HTTP/1.1 client — send requests and receive responses via TCP.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Built on uvcpp_tcp_client.  Internally streams response data through
 * uvcpp_http_parser and delivers a complete uvcpp_http_response to the
 * user callback when the message is fully received.
 *
 * Dual-mode API (same pattern as net module):
 * - Async:  provide a callback; fires on the internal event loop.
 * - Sync:   use *_wait variants; blocks until response or timeout.
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_CLIENT_H
#define SRC_WEB_UVCPP_HTTP_CLIENT_H

#if UVCPP_WEB_ENABLE

#include <functional>
#include <string>
#include <uv.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <handle/uvcpp_loop.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl.h>
class uvcpp_ssl_context;
#endif

namespace uvcpp {

class uvcpp_http_parser;

// =========================================================================
// Status flags
// =========================================================================

enum uvcpp_http_client_status : int {
  HTTP_CLIENT_NONE      = 0x00,  ///< Initial state
  HTTP_CLIENT_CONNECTED = 0x01,  ///< TCP connected to host
  HTTP_CLIENT_SENDING   = 0x02,  ///< Request being sent
  HTTP_CLIENT_RECEIVING = 0x04,  ///< Response being received/parsed
  HTTP_CLIENT_COMPLETE  = 0x08,  ///< Response fully received
  HTTP_CLIENT_CLOSING   = 0x10,  ///< Closing connection
  HTTP_CLIENT_CLOSED    = 0x20,  ///< Connection closed
  HTTP_CLIENT_ERROR     = 0x40,  ///< An error occurred
};

// =========================================================================
// HTTP Client class
// =========================================================================

/**
 * @brief HTTP/1.1 client.
 *
 * Usage (async):
 * @code
 *   uvcpp_http_client client;
 *   client.connect("example.com", 80);
 *   client.send(uvcpp_http_request::make_get("/api/data"),
 *               [](const uvcpp_http_response& resp, int err) {
 *                 // response ready — do NOT block here!
 *               });
 *   client.run(UV_RUN_DEFAULT);
 * @endcode
 *
 * Usage (sync):
 * @code
 *   uvcpp_http_client client;
 *   client.connect_wait("example.com", 80);
 *   uvcpp_http_response resp;
 *   client.send_wait(uvcpp_http_request::make_get("/api/data"), resp);
 *   // resp is ready
 * @endcode
 */
class UVCPP_API uvcpp_http_client {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_http_client)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_http_client)

  // -------------------------------------------------------------------
  // Connect
  // -------------------------------------------------------------------

  /**
   * @brief Connect to host:port (async).
   * @param cb  Called with (int error) — 0 = success.
   */
  int connect(const char* host, int port,
              std::function<void(int)> cb = nullptr);

  /**
   * @brief Connect to host:port (sync, blocks until connected or timeout).
   */
  int connect_wait(const char* host, int port, int timeout_ms = 30000);

  // -------------------------------------------------------------------
  // Send request
  // -------------------------------------------------------------------

  /**
   * @brief Send an HTTP request and wait for the full response (sync).
   * @param req        Request to send.
   * @param resp       [out] Populated response.
   * @param timeout_ms Timeout in milliseconds.
   * @return 0 on success, libuv error code on failure.
   */
  int send_wait(const uvcpp_http_request& req,
                uvcpp_http_response& resp,
                int timeout_ms = 30000);

  /**
   * @brief Send an HTTP request (async).
   * @param req  Request to send.
   * @param cb   Called with (response, error) when complete.
   * @return 0 if the send was initiated, libuv error code otherwise.
   */
  int send(const uvcpp_http_request& req,
           std::function<void(const uvcpp_http_response&, int)> cb);

  // -------------------------------------------------------------------
  // Convenience: GET / POST
  // -------------------------------------------------------------------

  int get(const std::string& path,
          std::function<void(const uvcpp_http_response&, int)> cb);
  int get_wait(const std::string& path,
               uvcpp_http_response& resp,
               int timeout_ms = 30000);

  int post(const std::string& path, const char* body, size_t len,
           const std::string& content_type,
           std::function<void(const uvcpp_http_response&, int)> cb);
  int post_wait(const std::string& path, const char* body, size_t len,
                const std::string& content_type,
                uvcpp_http_response& resp,
                int timeout_ms = 30000);

  // -------------------------------------------------------------------
  // Loop control
  // -------------------------------------------------------------------

  int run(uv_run_mode md = UV_RUN_DEFAULT);
  void stop();

  // -------------------------------------------------------------------
  // Status / accessors
  // -------------------------------------------------------------------

  int get_status() const;
  bool has_status(int flags) const;
  int get_last_error() const;
  uvcpp_tcp_client* get_tcp_client();

  /** @brief Set Connection: keep-alive behaviour (default true). */
  void set_keep_alive(bool enable);

  // -------------------------------------------------------------------
  // Compression (UVCPP_ZLIB_ENABLE=1 only)
  // -------------------------------------------------------------------

#if UVCPP_ZLIB_ENABLE
  /** @brief Enable automatic Accept-Encoding + response decompression. */
  void set_compression_enabled(bool enable);
  bool is_compression_enabled() const;
#endif

  // -------------------------------------------------------------------
  // SSL / HTTPS support (UVCPP_OPENSSL_ENABLE=1 only)
  // -------------------------------------------------------------------
#if UVCPP_OPENSSL_ENABLE
  /**
   * @brief Enable SSL/TLS for subsequent connections.
   *        After calling this, connect() will perform a TLS handshake
   *        and all traffic will be encrypted.
   */
  void set_ssl_context(uvcpp_ssl_context* ctx);
  bool is_ssl_enabled() const;
#endif

 private:
  // -------------------------------------------------------------------
  // Internal helpers
  // -------------------------------------------------------------------

  void set_status(int flags);
  void clear_status(int flags);

  /** @brief TCP "data arrived" handler — feeds data to HTTP parser. */
  void on_tcp_data(uvcpp_buf* buf);

  /** @brief TCP "connection closed" handler. */
  void on_tcp_close(uvcpp_tcp_client* client);

  void on_response_complete();

  // -------------------------------------------------------------------
  // Member variables
  // -------------------------------------------------------------------

  uvcpp_loop*        loop_ = nullptr;
  uvcpp_tcp_client*  tcp_  = nullptr;
  uvcpp_http_parser* parser_ = nullptr;

  int status_ = HTTP_CLIENT_NONE;
  int last_error_code_ = 0;
  bool keep_alive_ = true;

  std::string host_;
  int port_ = 0;

  uvcpp_buf          body_buf_;
  uvcpp_http_response pending_resp_;
  bool response_headers_done_ = false;

  std::function<void(const uvcpp_http_response&, int)> user_cb_;

#if UVCPP_OPENSSL_ENABLE
  uvcpp_ssl_context* ssl_ctx_ = nullptr;
  uvcpp_ssl*         ssl_    = nullptr;
  bool ssl_enabled_ = false;
  int do_ssl_handshake(int fd);
  int ssl_read(char* buf, size_t len);
  int ssl_write(const char* data, size_t len);
#endif

#if UVCPP_ZLIB_ENABLE
  bool compress_enabled_ = true;
  size_t compress_min_body_ = 1024;
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_CLIENT_H
