/**
 * @file src/web/uvcpp_http_server.h
 * @brief HTTP/1.1 server — accept connections, route requests, send responses.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Built on uvcpp_tcp_server.  Each TCP connection gets an independent
 * uvcpp_http_parser context.  Incoming requests are matched against
 * registered routes; the matching handler receives the parsed request
 * and a writable response object to fill in.
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_SERVER_H
#define SRC_WEB_UVCPP_HTTP_SERVER_H

#if UVCPP_WEB_ENABLE

#include <functional>
#include <map>
#include <string>
#include <vector>
#include <uv.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

namespace uvcpp {

class uvcpp_http_parser;

// =========================================================================
// Status flags
// =========================================================================

enum uvcpp_http_server_status : int {
  HTTP_SERVER_NONE      = 0x00,  ///< Initial state
  HTTP_SERVER_LISTENING = 0x01,  ///< Bound and listening
  HTTP_SERVER_STOPPING  = 0x04,  ///< Stopping
  HTTP_SERVER_STOPPED   = 0x08,  ///< Fully stopped
  HTTP_SERVER_ERROR     = 0x10,  ///< Error occurred
};

// =========================================================================
// Handler type
// =========================================================================

/**
 * @brief HTTP request handler.
 * @param req     The parsed incoming request.
 * @param resp    The response object to fill in and send back.
 * @param client  The underlying TCP client (for advanced use: close, raw write, etc.)
 */
using http_request_handler = std::function<void(
    uvcpp_http_request& req,
    uvcpp_http_response& resp,
    uvcpp_tcp_client* client)>;

// =========================================================================
// HTTP Server class
// =========================================================================

/**
 * @brief HTTP/1.1 server.
 *
 * Usage:
 * @code
 *   uvcpp_http_server server;
 *   server.bind("0.0.0.0", 8080);
 *   server.get("/hello", [](auto& req, auto& resp, auto* c) {
 *     resp = uvcpp_http_response::ok("world", 5);
 *   });
 *   server.listen();
 *   server.run(UV_RUN_DEFAULT);
 * @endcode
 */
class UVCPP_API uvcpp_http_server {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_http_server)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_http_server)

  // -------------------------------------------------------------------
  // Bind / Listen
  // -------------------------------------------------------------------

  int bind(const char* ip, int port);
  int bindIpv4(const char* ip, int port);
  int bindIpv6(const char* ip, int port);
  int listen(int backlog = 128);

  // -------------------------------------------------------------------
  // Route registration
  // -------------------------------------------------------------------

  /** @brief Default handler (called when no route matches). */
  void on_request(http_request_handler handler);

  /**
   * @brief Upgrade handler — called BEFORE routing when Upgrade: websocket
   *        is detected.  The handler should send the 101 response, then take
   *        ownership of the TCP client for WebSocket framing.
   */
  using upgrade_handler_t = std::function<void(uvcpp_http_request&, uvcpp_tcp_client*)>;
  void on_upgrade(upgrade_handler_t handler);

  /** @brief Register a handler for GET + exact path. */
  void get(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for POST + exact path. */
  void post(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for PUT + exact path. */
  void put(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for DELETE + exact path. */
  void del(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for OPTIONS + exact path. */
  void options(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for PATCH + exact path. */
  void patch(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for HEAD + exact path. */
  void head(const std::string& path, http_request_handler handler);

  // -------------------------------------------------------------------
  // Compression (UVCPP_ZLIB_ENABLE=1 only)
  // -------------------------------------------------------------------

#if UVCPP_ZLIB_ENABLE
  /** @brief Enable/disable automatic response body compression.
   *         Default: enabled when UVCPP_ZLIB_ENABLE=1. */
  void set_compression_enabled(bool enable);
  bool is_compression_enabled() const;

  /** @brief Set the minimum body size (bytes) to trigger compression. Default 1024. */
  void set_compress_min_body_size(size_t min_size);

  /** @brief Override the default MIME exclusion list. */
  void set_compress_excluded_types(const std::vector<std::string>& types);

  /** @brief Add a single MIME type to the exclusion list. */
  void add_compress_excluded_type(const std::string& mime_type);
#endif

  // -------------------------------------------------------------------
  // Loop control
  // -------------------------------------------------------------------

  int run(uv_run_mode md = UV_RUN_DEFAULT);
  void stop(std::function<void()> on_stopped = nullptr);

  // -------------------------------------------------------------------
  // Status / accessors
  // -------------------------------------------------------------------

  int get_status() const;
  bool has_status(int flags) const;
  uvcpp_tcp_server* get_tcp_server();

 private:
  // -------------------------------------------------------------------
  // Route matching
  // -------------------------------------------------------------------

  struct route_entry {
    http_method method;
    std::string path;
    http_request_handler handler;
  };

  http_request_handler find_handler(http_method method, const std::string& path);

  // -------------------------------------------------------------------
  // Connection handling
  // -------------------------------------------------------------------

  /** @brief Called when a TCP connection is accepted. */
  void on_tcp_connection(uvcpp_tcp_client* client);

  /** @brief Called when data arrives on a connection. */
  void on_connection_data(uvcpp_tcp_client* client, uvcpp_buf* buf);

  /** @brief Called when a full HTTP request has been parsed. */
  void on_request_complete(uvcpp_tcp_client* client);

  // -------------------------------------------------------------------
  // Per-connection parser context
  // -------------------------------------------------------------------

  struct conn_ctx {
    uvcpp_http_parser* parser = nullptr;
    uvcpp_http_request request;
    uvcpp_buf body_buf;
    bool headers_done = false;
    bool msg_done = false;
  };

  conn_ctx* get_or_create_ctx(uvcpp_tcp_client* client);
  void remove_ctx(uvcpp_tcp_client* client);

  // -------------------------------------------------------------------
  // Member variables
  // -------------------------------------------------------------------

  uvcpp_tcp_server* tcp_server_ = nullptr;
  int status_ = HTTP_SERVER_NONE;

  std::vector<route_entry> routes_;
  http_request_handler default_handler_;
  upgrade_handler_t upgrade_handler_;

  std::map<uvcpp_tcp_client*, conn_ctx> contexts_;

#if UVCPP_ZLIB_ENABLE
  bool compress_enabled_ = true;
  size_t compress_min_body_ = 1024;
  std::vector<std::string> compress_excluded_types_;
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_SERVER_H
