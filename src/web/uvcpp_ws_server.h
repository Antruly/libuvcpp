/**
 * @file src/web/uvcpp_ws_server.h
 * @brief WebSocket server — standalone or backed by uvcpp_http_server.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Detects WebSocket upgrade requests in the embedded HTTP server,
 * completes the handshake (RFC 6455 Section 4), and delivers new
 * connections as uvcpp_ws_connection instances.
 *
 * Standalone usage:
 * @code
 *   uvcpp_ws_server ws;
 *   ws.bind("0.0.0.0", 9000);
 *   ws.on_connection([](uvcpp_ws_connection* c) {
 *     c->on_text([](const std::string& msg) { ... });
 *   });
 *   ws.run(UV_RUN_DEFAULT);
 * @endcode
 */

#pragma once
#ifndef SRC_WEB_UVCPP_WS_SERVER_H
#define SRC_WEB_UVCPP_WS_SERVER_H

#if UVCPP_WEB_ENABLE

#include <functional>
#include <string>
#include <uvcpp/uvcpp_define.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_ws_frame.h>
#include <web/uvcpp_ws_connection.h>
#include <web/uvcpp_http_server.h>

namespace uvcpp {

class UVCPP_API uvcpp_ws_server {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_ws_server)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_ws_server)

  // -------------------------------------------------------------------
  // Standalone mode: bind + listen
  // -------------------------------------------------------------------

  int bind(const char* ip, int port);
  int listen(int backlog = 128);

  // -------------------------------------------------------------------
  // Attach to existing HTTP server (shared mode)
  // -------------------------------------------------------------------

  /** @brief Handle WS upgrades on an existing HTTP server. */
  void attach(uvcpp_http_server* http);

  // -------------------------------------------------------------------
  // Connection callback
  // -------------------------------------------------------------------

  void on_connection(std::function<void(uvcpp_ws_connection*)> cb);

  // -------------------------------------------------------------------
  // Loop
  // -------------------------------------------------------------------

  int run(uv_run_mode md = UV_RUN_DEFAULT);
  void stop(std::function<void()> on_stopped = nullptr);

  // -------------------------------------------------------------------
  // Accessors
  // -------------------------------------------------------------------

  uvcpp_http_server* get_http_server();

 private:
  void handle_upgrade(uvcpp_http_request& req, uvcpp_tcp_client* client);

  static std::string compute_accept_key(const std::string& client_key);
  static std::string sha1(const std::string& input);

  uvcpp_http_server* http_server_ = nullptr;
  bool owns_http_ = false;

  std::function<void(uvcpp_ws_connection*)> on_conn_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_WS_SERVER_H
