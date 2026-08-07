/**
 * @file src/web/uvcpp_ws_connection.h
 * @brief WebSocket connection — wraps an established WS-over-TCP link.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Created from an already-upgraded TCP client.  Provides typed send/receive
 * for text, binary, ping, pong, and close frames via the WS frame parser.
 */

#pragma once
#ifndef SRC_WEB_UVCPP_WS_CONNECTION_H
#define SRC_WEB_UVCPP_WS_CONNECTION_H

#if UVCPP_WEB_ENABLE

#include <functional>
#include <string>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_ws_frame.h>
#include <web/uvcpp_ws_parser.h>

namespace uvcpp {

class UVCPP_API uvcpp_ws_connection {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_ws_connection)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_ws_connection)

  /** @brief Wrap an already-upgraded TCP client. Call start() to begin. */
  explicit uvcpp_ws_connection(uvcpp_tcp_client* tcp);

  /**
   * @brief Start reading WS frames. Must be called once, after the
   *        handshake is complete and the caller is no longer inside
   *        a TCP read callback.
   */
  void start();

  // -------------------------------------------------------------------
  // Send
  // -------------------------------------------------------------------
  int send_text(const char* data, size_t len, std::function<void(int)> cb = nullptr);
  int send_binary(const char* data, size_t len, std::function<void(int)> cb = nullptr);
  int send_ping(const char* data = nullptr, size_t len = 0);
  int send_pong(const char* data = nullptr, size_t len = 0);
  int send_close(ws_close_code code = ws_close_code::NORMAL,
                 const std::string& reason = "");

  // -------------------------------------------------------------------
  // Receive callbacks
  // -------------------------------------------------------------------
  void on_text(std::function<void(const std::string&)> cb);
  void on_binary(std::function<void(const uint8_t*, size_t)> cb);
  void on_ping(std::function<void(const uint8_t*, size_t)> cb);
  void on_pong(std::function<void(const uint8_t*, size_t)> cb);
  void on_close(std::function<void(ws_close_code, const std::string&)> cb);

  // -------------------------------------------------------------------
  // Accessors
  // -------------------------------------------------------------------
  uvcpp_tcp_client* get_tcp_client();

 private:
  void on_tcp_data(uvcpp_buf* buf);
  void on_ws_frame(const uvcpp_ws_frame& frame);
  void send_frame(const uvcpp_ws_frame& frame, std::function<void(int)> cb);

  uvcpp_tcp_client* tcp_ = nullptr;
  uvcpp_ws_parser   parser_;
  bool started_ = false;

  std::function<void(const std::string&)>    on_text_;
  std::function<void(const uint8_t*, size_t)> on_bin_;
  std::function<void(const uint8_t*, size_t)> on_ping_;
  std::function<void(const uint8_t*, size_t)> on_pong_;
  std::function<void(ws_close_code, const std::string&)> on_close_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_WS_CONNECTION_H
