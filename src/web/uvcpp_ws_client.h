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
#include <web/uvcpp_ws_sessions.h>

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

  /** @brief 当前活动的 WebSocket 会话数（0 或 1）。 */
  size_t session_count() const;

  /** @brief 累计已回收（真正 `delete`）的会话数。**单调递增**。 */
  size_t recycled_session_count() const;

  // -------------------------------------------------------------------
  // permessage-deflate 协商（RFC 7692）— 仅 UVCPP_ZLIB_ENABLE=1
  // -------------------------------------------------------------------
#if UVCPP_ZLIB_ENABLE
  /**
   * @brief 配置 permessage-deflate 协商策略。**默认开启**。
   *
   * 开启时握手请求会带上 `Sec-WebSocket-Extensions: permessage-deflate;
   * client_max_window_bits`（无值形态，把窗口选择权交给服务端 —— Chrome
   * 等真实客户端就是这么发的）。
   *
   * 服务端**没应答**这个扩展是正常降级，连接照常建立、不启用压缩。
   * 但服务端**应答了却答得不对**（出现本端没提过的参数、无值形态、
   * 窗口位数超出本端愿意给的范围……）会让握手**失败**（`WS_CLIENT_ERROR`）：
   * 服务端一旦写下这个头就已经认定压缩生效，本端装作没看见等于双方对帧格式
   * 的理解不一致，连接必然错乱 —— RFC 7692 §7.1.2 要求此时让握手失败。
   */
  void set_compression(const uvcpp_ws_deflate_config& cfg);
  uvcpp_ws_deflate_config get_compression() const;
#endif

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

  /// 本客户端建立的会话（0 或 1 个）。终结时交回这里回收。
  uvcpp_ws_sessions sessions_;

  std::string ws_host_;
  std::string ws_path_;
  std::string ws_key_;
  std::string handshake_buf_;  // accumulates partial 101 response
  bool use_tls_ = false;

#if UVCPP_ZLIB_ENABLE
  uvcpp_ws_deflate_config deflate_cfg_;
  /** @brief 握手协商出来的压缩参数；accepted=false 时表示不启用压缩。 */
  uvcpp_ws_deflate_params deflate_params_;
#endif

  std::function<void(uvcpp_ws_connection*, int)> connect_cb_;

#if UVCPP_OPENSSL_ENABLE
  uvcpp_ssl_context* ssl_ctx_ = nullptr;
  class uvcpp_ssl*   ssl_    = nullptr;
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_WS_CLIENT_H
