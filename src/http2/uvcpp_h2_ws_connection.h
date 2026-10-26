/**
 * @file src/http2/uvcpp_h2_ws_connection.h
 * @brief WebSocket over HTTP/2（RFC 8441，extended CONNECT，#58）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 一条 h2 流上的 WebSocket 会话：帧的收发与 `uvcpp_ws_connection` /
 * `uvcpp_h3_ws_connection` 同形（消息级回调，没有逐帧接口）。
 *
 * 与 h3 那条腿的区别只在传输：这里是 h2 流（`send_data` / `set_stream_sink`），
 * 帧**不需要掩码**（RFC 8441 §5，两个方向都不掩）。
 *
 * **生命周期**：本对象不拥有 `uvcpp_h2_connection`；它会往连接里装一个流落点，
 * 所以**连接必须先于本对象销毁**（或者先 `close()`）。析构会先摘掉落点。
 *
 * **线程**：只能在 h2 连接所属的循环线程上调用。
 */

#pragma once
#ifndef SRC_HTTP2_UVCPP_H2_WS_CONNECTION_H
#define SRC_HTTP2_UVCPP_H2_WS_CONNECTION_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_NGHTTP2_ENABLE

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_export.h>
#include <web/uvcpp_ws_frame.h>
#include <web/uvcpp_ws_parser.h>

namespace uvcpp {

class uvcpp_h2_connection;
class uvcpp_http_request;

class UVCPP_API uvcpp_h2_ws_connection {
 public:
  struct callbacks {
    std::function<void(const std::string&)>       on_text;
    std::function<void(const uint8_t*, size_t)>   on_binary;
    std::function<void(const uint8_t*, size_t)>   on_ping;
    std::function<void(const uint8_t*, size_t)>   on_pong;
    std::function<void(ws_close_code, const std::string&)> on_close;
    std::function<void(int, const std::string&)>  on_error;
  };

  ~uvcpp_h2_ws_connection();
  uvcpp_h2_ws_connection(const uvcpp_h2_ws_connection&)            = delete;
  uvcpp_h2_ws_connection& operator=(const uvcpp_h2_ws_connection&) = delete;

  /**
   * @brief 服务端：接受一条 `:protocol = websocket` 的 extended CONNECT。
   *
   * 发 200（回显子协议，不带 body），装上流落点。
   *
   * @param req 必须是 CONNECT 且 `req.protocol == "websocket"`。
   * @return 会话；失败返回 `nullptr`。
   */
  static std::unique_ptr<uvcpp_h2_ws_connection> accept(
      uvcpp_h2_connection& h2, int32_t stream_id, const uvcpp_http_request& req,
      callbacks cbs);

  /**
   * @brief 客户端：开一条 extended CONNECT 并开始收帧。
   *
   * 对端必须宣告过 `SETTINGS_ENABLE_CONNECT_PROTOCOL`（RFC 8441），否则返回
   * `nullptr`。
   */
  static std::unique_ptr<uvcpp_h2_ws_connection> connect(
      uvcpp_h2_connection& h2, const std::string& path,
      const std::string& authority, callbacks cbs);

  int send_text(const char* data, size_t len);
  int send_binary(const void* data, size_t len);
  int send_ping(const void* data = nullptr, size_t len = 0);
  int send_pong(const void* data = nullptr, size_t len = 0);
  /** @brief 发 Close 帧并结束发送方向。 */
  int close(ws_close_code code = ws_close_code::NORMAL,
            const std::string& reason = std::string());

  bool is_open() const { return !closed_; }
  int32_t stream_id() const { return sid_; }

 private:
  uvcpp_h2_ws_connection(uvcpp_h2_connection* h2, int32_t sid, callbacks cbs);

  void on_data(const char* data, size_t len);
  void on_end();
  void on_frame(const uvcpp_ws_frame& frame);
  int  send_frame(const uvcpp_ws_frame& f);
  void fail(int code, const std::string& why);

  /// h2 连接还在（与 `uvcpp_h2_connection::life_token()` 同一枚令牌）。
  bool linked() const;

  uvcpp_h2_connection*      h2_ = nullptr;
  /// 连接比本对象先销毁也合法：任何回头碰它的地方都先问这一枚令牌。
  std::weak_ptr<const char> life_;
  int32_t                   sid_ = -1;
  callbacks            cbs_;
  uvcpp_ws_parser      parser_;
  bool                 closed_ = false;
  bool                 close_sent_ = false;

  // 消息重组（同 `uvcpp_ws_connection` / `uvcpp_h3_ws_connection`）。
  bool        in_message_ = false;
  ws_opcode   message_op_ = ws_opcode::TEXT;
  std::string message_;
};

}  // namespace uvcpp

#endif  // UVCPP_NGHTTP2_ENABLE
#endif  // SRC_HTTP2_UVCPP_H2_WS_CONNECTION_H
