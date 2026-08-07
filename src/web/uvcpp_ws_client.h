/**
 * @file src/web/uvcpp_ws_client.h
 * @brief WebSocket client — HTTP Upgrade handshake + WS frame I/O.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Connects to a ws:// URL, performs the HTTP Upgrade handshake,
 * and wraps the result in a uvcpp_ws_connection for typed message dispatch.
 */

#pragma once
#ifndef SRC_WEB_UVCPP_WS_CLIENT_H
#define SRC_WEB_UVCPP_WS_CLIENT_H

#if UVCPP_WEB_ENABLE

#include <functional>
#include <string>
#include <uvcpp/uvcpp_define.h>
#include <handle/uvcpp_loop.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_ws_frame.h>
#include <web/uvcpp_ws_connection.h>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_context.h>
#endif

namespace uvcpp {

enum ws_client_status : int {
  WS_CLIENT_NONE       = 0x00,
  WS_CLIENT_CONNECTING = 0x01,
  WS_CLIENT_OPEN       = 0x02,
  WS_CLIENT_CLOSING    = 0x04,
  WS_CLIENT_CLOSED     = 0x08,
  WS_CLIENT_ERROR      = 0x10,
};

class UVCPP_API uvcpp_ws_client {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_ws_client)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_ws_client)

  // -------------------------------------------------------------------
  // Connect
  // -------------------------------------------------------------------

  /** @brief Async connect (ws://host:port/path). */
  int connect(const std::string& url,
              std::function<void(uvcpp_ws_connection*, int error)> cb);

  /** @brief Sync connect. */
  int connect_wait(const std::string& url,
                   uvcpp_ws_connection*& out_conn,
                   int timeout_ms = 30000);

  // -------------------------------------------------------------------
  // Loop
  // -------------------------------------------------------------------

  int run(uv_run_mode md = UV_RUN_DEFAULT);
  void stop();
  uvcpp_loop* get_loop();

  // -------------------------------------------------------------------
  // Status
  // -------------------------------------------------------------------

  int get_status() const;
  bool has_status(int flags) const;
  int get_last_error() const;

#if UVCPP_OPENSSL_ENABLE
  void set_ssl_context(uvcpp_ssl_context* ctx);
#endif

 private:
  void do_handshake(const std::string& host, int port,
                    const std::string& path, const std::string& key);
  void on_handshake_data(uvcpp_buf* buf);
  void on_handshake_complete(int error);

  uvcpp_loop*        loop_ = nullptr;
  uvcpp_tcp_client*  tcp_  = nullptr;
  int status_ = WS_CLIENT_NONE;
  int last_error_ = 0;

  std::string ws_host_;
  std::string ws_path_;
  std::string ws_key_;
  std::string handshake_buf_;  // accumulates partial 101 response
  bool use_tls_ = false;

  std::function<void(uvcpp_ws_connection*, int)> connect_cb_;

#if UVCPP_OPENSSL_ENABLE
  uvcpp_ssl_context* ssl_ctx_ = nullptr;
  class uvcpp_ssl*   ssl_    = nullptr;
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_WS_CLIENT_H
