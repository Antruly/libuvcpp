/**
 * @file src/http3/uvcpp_h3_ws_connection.h
 * @brief WebSocket over HTTP/3（RFC 9220，extended CONNECT 上的 WS 帧，#53）。
 *
 * 一条 HTTP/3 双向流（extended CONNECT，`:protocol = websocket`）上跑 RFC 6455
 * 帧。与 h1 的 `uvcpp_ws_connection` 的区别：
 *
 * - **没有 HTTP Upgrade**：握手是 h3 的 extended CONNECT 请求 + 2xx 响应；
 * - **没有掩码**：RFC 8441 §5 明确免除 h2/h3 上的客户端掩码要求；
 * - **没有 permessage-deflate**：这条实现只做文本/二进制/控制帧与分片重组；
 *   扩展协商（`Sec-WebSocket-Extensions`）留到后续。
 *
 * 生命周期：本对象**不拥有** `uvcpp_h3_connection`；h3 连接必须先于本对象销毁，
 * 或者调用方先 `close()`。析构时会摘掉流上的 sink（不会把悬垂的 this 留在
 * h3 连接里）。
 */

#pragma once
#ifndef SRC_HTTP3_UVCPP_H3_WS_CONNECTION_H
#define SRC_HTTP3_UVCPP_H3_WS_CONNECTION_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_HTTP3_ENABLE

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_export.h>

#include "http3/uvcpp_h3_connection.h"
#include "web/uvcpp_ws_frame.h"
#include "web/uvcpp_ws_parser.h"

namespace uvcpp {

/**
 * @brief 一条 h3 上的 WebSocket 会话。
 *
 * 创建之后帧的收发都在这里；`callbacks` 与 `uvcpp_ws_connection` 的消息级回调
 * 同形（没有逐帧接口）。**所有成员只能在 h3 连接所属的循环线程上调用。**
 */
class UVCPP_API uvcpp_h3_ws_connection {
 public:
  struct callbacks {
    std::function<void(const std::string&)>       on_text;
    std::function<void(const uint8_t*, size_t)>   on_binary;
    std::function<void(const uint8_t*, size_t)>   on_ping;
    std::function<void(const uint8_t*, size_t)>   on_pong;
    std::function<void(ws_close_code, const std::string&)> on_close;
    std::function<void(int, const std::string&)>  on_error;
  };

  ~uvcpp_h3_ws_connection();
  uvcpp_h3_ws_connection(const uvcpp_h3_ws_connection&)            = delete;
  uvcpp_h3_ws_connection& operator=(const uvcpp_h3_ws_connection&) = delete;

  /**
   * @brief 服务端：接受一条 extended CONNECT 请求并发 2xx，然后开始收帧。
   *
   * @param req 必须是 `protocol == "websocket"` 的 CONNECT 请求。
   * @return 会话；失败返回 `nullptr`（错误已按 500 的路径交给 h3 层）。
   */
  static std::unique_ptr<uvcpp_h3_ws_connection> accept(
      uvcpp_h3_connection& h3, const h3_request& req, callbacks cbs);

  /**
   * @brief 客户端：开一条 extended CONNECT 并开始收帧。
   *
   * 不等 2xx：帧可以立刻发，服务端拒绝时对流做 RST。
   */
  static std::unique_ptr<uvcpp_h3_ws_connection> connect(
      uvcpp_h3_connection& h3, const std::string& path,
      const std::string& authority, callbacks cbs);

  int send_text(const char* data, size_t len);
  int send_binary(const void* data, size_t len);
  int send_ping(const void* data = nullptr, size_t len = 0);
  int send_pong(const void* data = nullptr, size_t len = 0);
  /** @brief 发 Close 帧并关发送方向。 */
  int close(ws_close_code code = ws_close_code::NORMAL,
            const std::string& reason = std::string());

  bool is_open() const { return !closed_; }
  int64_t stream_id() const { return sid_; }

 private:
  uvcpp_h3_ws_connection(uvcpp_h3_connection* h3, int64_t sid, callbacks cbs);

  void on_data(const char* data, size_t len);
  void on_end();
  void on_frame(const uvcpp_ws_frame& frame);
  int  send_frame(const uvcpp_ws_frame& f);
  void fail(int code, const std::string& why);

  uvcpp_h3_connection* h3_ = nullptr;
  int64_t              sid_ = -1;
  callbacks            cbs_;
  uvcpp_ws_parser      parser_;
  bool                 closed_ = false;
  bool                 close_sent_ = false;

  // 消息重组（同 `uvcpp_ws_connection`，但不做解压）。
  bool      in_message_ = false;
  ws_opcode message_op_ = ws_opcode::TEXT;
  std::string message_;
};

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE

#endif  // SRC_HTTP3_UVCPP_H3_WS_CONNECTION_H
