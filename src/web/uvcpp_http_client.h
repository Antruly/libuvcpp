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
#include <ssl/uvcpp_ssl.h>  // 同时也声明了 uvcpp::uvcpp_ssl_context
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
   * @brief 同步纯 HTTP 发送/接收（阻塞式 socket I/O，读至 EOF）。
   *
   * 与 send_wait 的区别：send_wait 走 libuv 异步读 + 事件循环泵，连接关闭时
   * 析构需做 close-dance，存在内存安全隐患；本方法直接对已连接 socket 做同步
   * 收发，绕开 libuv 异步读路径，析构期句柄处于 inactive，安全。
   */
  int send_wait_plain(const uvcpp_http_request& req,
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
  /** @brief Enable/disable automatic Accept-Encoding + response decompression.
   *         Default: OFF — user must opt in explicitly. */
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
   *
   * @note 两条路径各自完整，但**不能交叉**：
   *   - 异步系列（`connect(host, port, cb)` + `send(req, cb)`）：`connect()`
   *     在发起连接**之前**把 TLS 装到 `uvcpp_tcp_client` 上（memory BIO），
   *     握手由读事件推进，握手完成才回调 —— 于是 `send()` 写的是明文、
   *     出网的是密文，读回来的密文也在这里被解回明文。
   *   - 同步系列（`connect_wait()` + `send_wait()`）：沿用阻塞式握手读写。
   *   - 混用（异步 connect + `send_wait()`）会返回 `UV_ENOTSUP` —— 阻塞式
   *     SSL_read/SSL_write 要独占 socket，接管不了 memory BIO 那条会话。
   *     宁可报错，也不把明文写进一条已经加密的连接。
   */
  void set_ssl_context(uvcpp_ssl_context* ctx);
  bool is_ssl_enabled() const;
  /** @brief 同步 SSL 请求路径（阻塞式握手/读写，仅 *_wait 系列使用）。 */
  int send_wait_ssl(const uvcpp_http_request& req,
                    uvcpp_http_response& resp,
                    int timeout_ms);
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
  bool compress_enabled_ = false;   // OFF by default — user must opt in
  size_t compress_min_body_ = 1024;
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_CLIENT_H
