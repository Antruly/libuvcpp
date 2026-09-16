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
  // 会话 / 收发 / 关闭 —— 都是**转发给当前会话**（0 或 1 个）
  // -------------------------------------------------------------------
  //
  // 这一层不自己实现 WS 语义，只是把调用转到当前会话上，省掉"用户得把会话
  // 指针自己存起来"这一步。需要会话能力的全部（ping/pong、压缩、单条消息
  // 上限、底下的 TCP 客户端）就用 session() 拿指针直接办。

  /** @brief 当前会话；**还没连上、或者已经终结**时返回 nullptr。 */
  uvcpp_ws_connection* session() const;

  /**
   * @brief 发送。没有会话时返回 `UV_ENOTCONN` 并**立刻**回调 `cb(UV_ENOTCONN)`。
   *
   * 不静默丢：调用方一定能从返回值或回调知道这一帧没出去（与连接层一致）。
   */
  int send_text(const char* data, size_t len,
                std::function<void(int)> cb = nullptr);
  int send_binary(const char* data, size_t len,
                  std::function<void(int)> cb = nullptr);

  /**
   * @brief 收消息 / 会话结束 / 协议错误的回调。
   *
   * 回调**存在客户端上**，每建立一个新的会话就装一次 —— 所以 `connect()`
   * 之前调（异步的常见写法：先把 handler 备好再连）和握手完成之后调都行；
   * 关掉再连一次也照旧生效。签名与 `uvcpp_ws_connection` 的**完全一致**
   * （薄转发的意义就在这里：两端同一段代码可以照抄）。
   *
   * 没有会话时调用只是先把回调记下来，不会报错。
   */
  void on_text(std::function<void(const std::string&)> cb);
  void on_binary(std::function<void(const uint8_t*, size_t)> cb);
  void on_close(std::function<void(ws_close_code, const std::string&)> cb);
  void on_error(std::function<void(int, const std::string&)> cb);

  /**
   * @brief 优雅关闭：给当前会话发 Close 帧，并把状态置为 `WS_CLIENT_CLOSING`。
   *
   * 之后的**真正**结束（对端回 Close / 连接关掉 / 会话被回收）会把状态置成
   * `WS_CLIENT_CLOSED`，届时 `session()` 变 nullptr、`on_close` 收到码。
   * 可以重复调，只生效一次。
   *
   * **还没有会话**时（连接建立中）没有 Close 帧可发：直接关掉底层连接、状态
   * 直接落 `WS_CLIENT_CLOSED`（连接建立中就等于取消这次连接 —— 留在 CLOSING
   * 就是个谎：没有任何在途操作能把它推下去）。此时正在等 `connect` 回调的
   * 调用方会**当场**收到 `cb(nullptr, UV_ECANCELED)`：取消也是结果，不悬着。
   */
  void close(ws_close_code code = ws_close_code::NORMAL,
             const std::string& reason = std::string());

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

  /** @brief 把存下来的那组回调装到一个会话上（每建一个新会话调一次）。 */
  void install_callbacks(uvcpp_ws_connection* conn);
  /** @brief 会话终结的落点（`sessions_` 的终结观察者）。 */
  void on_session_retired(uvcpp_ws_connection* conn);

  uvcpp_loop*        loop_ = nullptr;
  uvcpp_tcp_client*  tcp_  = nullptr;
  int status_ = WS_CLIENT_NONE;
  int last_error_ = 0;

  /// 当前会话。**终结观察者负责清空** —— 清空之后才是 nullptr，所以拿到它
  /// 的人只可能在会话还活着的时候拿到（回收是延迟的，指针本身不会悬垂）。
  uvcpp_ws_connection* session_ = nullptr;

  /// 用户通过本层装的回调。存着是为了"connect 之前装、建会话时装上去"。
  std::function<void(const std::string&)>                on_text_;
  std::function<void(const uint8_t*, size_t)>            on_bin_;
  std::function<void(ws_close_code, const std::string&)> on_close_;
  std::function<void(int, const std::string&)>           on_error_;

  /// 本客户端建立的会话（0 或 1 个）。终结时交回这里回收。
  uvcpp_ws_sessions sessions_;

  std::string ws_host_;
  std::string ws_path_;
  std::string ws_key_;
  std::string handshake_buf_;  // accumulates partial 101 response

  /// 101 头部之后、跟着同一批到达的字节（第一帧很可能就在里面）。由
  /// `on_handshake_complete` 补投给新会话 —— 不补投就是丢首帧。
  std::string handshake_tail_;
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
