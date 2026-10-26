/**
 * @file src/http3/uvcpp_h3_ws_server.h
 * @brief HTTP/3 上的 WebSocket 服务端便捷封装（RFC 9220，extended CONNECT）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 把 "`uvcpp_quic_server` → 每条连接一个 `uvcpp_h3_connection` →
 * `on_connect_request` → `uvcpp_h3_ws_connection::accept`" 这段**每条应用都要
 * 手拼一遍**的生命周期收进库里（#58）：
 *
 *   - 每条 QUIC 连接建一个 `uvcpp_h3_connection`，由本类拥有；
 *   - `:protocol = websocket` 的 extended CONNECT 自动 accept 成
 *     `uvcpp_h3_ws_connection`，按流号登记；
 *   - 非 WebSocket 的普通请求与非 websocket 的 CONNECT 一律回 404；
 *   - 连接断开时**先销毁该连接下所有 ws 会话、再销毁 h3 连接**
 *     （`uvcpp_h3_ws_connection` 不拥有 h3 连接，反过来会碰到已释放的连接）。
 *
 * **线程**：所有成员只能在构造时那条循环的线程上调用。回调也都在那条线程上，
 * 而且可能在本类的内部调用还没返回的时候跑起来 —— 回调里可以 `close()` /
 * `stop()`，但**返回之后不要**再假定本对象还活着（本类用存活令牌兜底，析构之后
 * 迟到的回调直接跳过）。
 */

#pragma once
#ifndef SRC_HTTP3_UVCPP_H3_WS_SERVER_H
#define SRC_HTTP3_UVCPP_H3_WS_SERVER_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_HTTP3_ENABLE

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_export.h>
#include <web/uvcpp_ws_frame.h>  // ws_close_code

namespace uvcpp {

class uvcpp_h3_ws_connection;
class uvcpp_loop;
class uvcpp_quic_server;
class uvcpp_ssl_context;

class UVCPP_API uvcpp_h3_ws_server {
 public:
  /**
   * @brief 回调集合。全部在 `get_loop()` 的线程上调用。
   *
   * 每个回调的第一个参数是**本库拥有**的会话，指针在会话关闭之后失效；
   * 回调里可以直接 `send_text()` / `close()`。
   */
  struct callbacks {
    /// 一条 extended CONNECT 被接受、2xx 已发出。`path` 是请求的 `:path`。
    std::function<void(uvcpp_h3_ws_connection&, const std::string& path)>
        on_open;
    std::function<void(uvcpp_h3_ws_connection&, const std::string&)> on_text;
    std::function<void(uvcpp_h3_ws_connection&, const uint8_t*, size_t)>
        on_binary;
    /// 收到 PING；**PONG 由本层自动回**，这里只是通知。
    std::function<void(uvcpp_h3_ws_connection&, const uint8_t*, size_t)>
        on_ping;
    std::function<void(uvcpp_h3_ws_connection&, const uint8_t*, size_t)>
        on_pong;
    /**
     * @brief 会话关闭。**每条会话恰好一次。**
     *
     * 收到 Close 帧时给对端的状态码；流被重置或连接断开、没收到 Close 帧时
     * 给 `ABNORMAL_CLOSE`（`1006`）。
     */
    std::function<void(uvcpp_h3_ws_connection&, ws_close_code,
                       const std::string&)>
        on_close;
    /// 会话级错误（帧解析失败等）。之后还会收到一次 `on_close`。
    std::function<void(uvcpp_h3_ws_connection&, int, const std::string&)>
        on_error;
  };

  /**
   * @brief 共用一个外部循环（与 `uvcpp_quic_server(uvcpp_loop*)` 同一约定）。
   *
   * @param loop 生命周期必须覆盖本对象；本类不会关掉它。
   */
  explicit uvcpp_h3_ws_server(uvcpp_loop* loop);
  ~uvcpp_h3_ws_server();

  uvcpp_h3_ws_server(const uvcpp_h3_ws_server&) = delete;
  uvcpp_h3_ws_server& operator=(const uvcpp_h3_ws_server&) = delete;

  uvcpp_loop* get_loop();
  /** @brief 底下那条 QUIC 服务端（ALPN 固定 `h3`）。 */
  uvcpp_quic_server* get_quic_server();

  /**
   * @brief 设 TLS 上下文。**生命周期必须覆盖本对象**（只存裸指针）。
   *
   * QUIC 没有明文模式：不设就 `listen()` 失败并返回负值。
   */
  void set_ssl_context(uvcpp_ssl_context* ctx);

  /// `:0` 表示由内核挑一个；挑中的端口在 `listen()` 之后用 `configured_port()`。
  int bind(const char* ip, int port);
  int configured_port() const;

  /**
   * @brief 开始收连接。
   *
   * @return 0 = 已在收；负值 = 没起来（没设 TLS、端口占用、地址非法……），
   *         **此时 `cbs` 一个都不会被调**。
   */
  int listen(callbacks cbs);

  /**
   * @brief 停本端点所在的那条循环（导致 `run()` 返回）。语义与
   *        `uvcpp_quic_server::stop()` 一致。
   */
  void stop();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE
#endif  // SRC_HTTP3_UVCPP_H3_WS_SERVER_H
