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

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEB_ENABLE

#include <functional>
#include <map>
#include <memory>
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

#if UVCPP_NGHTTP2_ENABLE
// 只前置声明，**绝不** include `http2/uvcpp_h2_*.h`：这是公开头，会把
// nghttp2 的 include 路径与 `NGHTTP2_STATICLIB` 扩散给每一个使用者与测试 TU，
// 而本库把 nghttp2 压成静态、链接标 PRIVATE 就是为了不扩散（`src/http2/`
// 自己的头也守同一条）。定义全在 .cpp 里。
class uvcpp_h2_connection;
class uvcpp_h2_session;
class uvcpp_h2_stream;
#endif

#if UVCPP_HTTP3_ENABLE
// 同一条纪律。`http3/uvcpp_h3_connection.h` 自己**不**含 nghttp3 的类型（它
// 装得出去），但本头是每个使用者每个 TU 都要引的，让它多背一层包含关系没有
// 好处 —— 前置声明足够，定义全在 .cpp 里。
class uvcpp_h3_connection;
class uvcpp_quic_client;
#endif

// =========================================================================
// Status flags
// =========================================================================

enum uvcpp_http_client_status : int {
  HTTP_CLIENT_NONE      = 0x00,  ///< Initial state
  HTTP_CLIENT_CONNECTED = 0x01,  ///< TCP connected to host
  HTTP_CLIENT_SENDING   = 0x02,  ///< Request being sent
  HTTP_CLIENT_RECEIVING = 0x04,  ///< Response being received/parsed
  HTTP_CLIENT_COMPLETE  = 0x08,  ///< Response fully received
  HTTP_CLIENT_CLOSING   = 0x10,  ///< Closing connection. **从不置位**：`set_status()` 只被
                                 ///< CONNECTED / SENDING / RECEIVING / COMPLETE / ERROR
                                 ///< 调用，全仓也没有第二处写 `status_` 的地方
  HTTP_CLIENT_CLOSED    = 0x20,  ///< Connection closed. **从不置位** —— 于是
                                 ///< `uvcpp_http_client.cpp:121` 那处 `!has_status(
                                 ///< HTTP_CLIENT_CLOSED)` 恒为真（那里实际只判 `tcp_`）；
                                 ///< `:580`、`:1306` 两处的注释也明写了「不置这个标志」
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
   * @param timeout_ms Timeout in milliseconds. **仅 Windows 生效** —— 这一支的超时
   *                   靠 `SO_RCVTIMEO`/`SO_SNDTIMEO`，POSIX 上没有对应实现（见
   *                   `send_wait_plain` 的说明），那里传入的值会被忽略。
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
   *
   * @note `timeout_ms` **只有 Windows 支生效**：实现里那段
   *       `setsockopt(SO_RCVTIMEO/SO_SNDTIMEO)` 包在 `#ifdef _WIN32` 中，POSIX
   *       既没有等价写法也没有别处兜底，所以 Linux/macOS 上对端不响应时本方法
   *       仍可能**无限阻塞**。这是已知缺口，让调用方误以为有超时会很难查。
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

#if UVCPP_NGHTTP2_ENABLE
  /**
   * @brief 显式开启 HTTP/2（**默认关**，与 `uvcpp_http_server::set_http2_enabled`
   *        同向）。这是"低层手动"那一层：想用 h2 就自己说，不会有任何自动升级。
   *
   * 打开后的效果：`connect()` 把 ALPN 名单设成 `{"h2","http/1.1"}`（而不是钉死
   * `http/1.1`），握手完成后按**实际协商出来的**那个协议走下去。协商不出 h2
   * 就照常走 HTTP/1.1 —— 名字叫"开启"而不是"强制"，服务端不认 h2 不是错误。
   *
   * **只对异步系列有效**（`connect()` + `send()`）。同步系列要独占 socket 做阻塞
   * 式 `SSL_read/SSL_write`，接管不了 h2 那条内存 BIO 会话 —— 混用一律
   * `UV_ENOTSUP`，与既有的"异步 connect + `send_wait()`"同一条纪律。
   *
   * 必须配 TLS：明文上没有 ALPN，本库也不做 h2c/prior-knowledge，所以没调
   * `set_ssl_context()` 时打开它只是**记录意图**，连接照样是 HTTP/1.1。
   *
   * `nghttp2` 没编进来时这是空操作（`http2_enabled()` 恒 false）。
   */
  void set_http2_enabled(bool on);
  bool http2_enabled() const;

  /**
   * @brief 对端在这个连接上发过 GOAWAY 没有。
   *
   * **这是"优雅告别"与"网络挂了"之间唯一的区分手段。** 两者在 `send()` 的
   * 回调上长得一样（都是 `UV_ECANCELED`），而含义完全不同：收到过 GOAWAY 时，
   * 对端明确说了它处理到了哪个流号，`stream_id` 大于那个的请求**没被处理**，
   * 重试是安全的；没收到就是"结果未知"。
   *
   * 连接已经拆掉之后仍然可查 —— 那个问题恰恰是断开之后才问的。
   */
  bool peer_goaway_received() const;
  /// 对端 GOAWAY 里的错误码；`0` 是正常的优雅退出。没收到过时是 0。
  uint32_t peer_goaway_error_code() const;
  /// 对端 GOAWAY 里的 `last_stream_id`；没收到过时是 0。
  int32_t peer_goaway_last_stream_id() const;
#endif

#if UVCPP_NGHTTP2_ENABLE || UVCPP_HTTP3_ENABLE
  /**
   * @brief 本次连接**实际**协商出的 ALPN 协议。
   *
   * - TCP 那条腿（h1/h2）：`"h2"` / `"http/1.1"`，没走 TLS 时是 `""`；
   * - **h3 那条腿**：`"h3"` —— 它谈在 QUIC 自己的 TLS 握手里，与 `tcp_` 毫无
   *   关系（见下面 `set_http3_enabled()` 的说明）。
   *
   * 连接前恒为空。
   */
  const std::string& negotiated_alpn() const;
#endif

#if UVCPP_HTTP3_ENABLE
  /**
   * @brief 让后续连接走 **HTTP/3（QUIC/UDP）** 而不是 TCP（**默认关**）。
   *
   * 与 `set_http2_enabled()` 是**两件不同的事**，别当成"升级链上的下一级"：
   * h2 是"在 TCP 上谈一个更好的协议"，h3 是**另一条传输**（UDP）。所以
   *
   * - **不碰 TCP 那条腿的 ALPN 名单**：`"h3"` 绝不进 `connect()` 里那份
   *   `{"h2","http/1.1"}` 列表 —— 在 TCP 的 ClientHello 里报 `h3` 只会让对端
   *   把 h3 的二进制流喂给 llhttp，症状是"连上了、写成功了、响应永远不来"。
   * - 打开之后 `connect()` 建的是 `uvcpp_quic_client`（ALPN 用它自己的默认
   *   `"h3"`），**不建 TCP socket**；`send()` 走 h3 那条路。
   * - **必须配 TLS**：QUIC 没有明文模式（ALPN 是 TLS 扩展），所以没调
   *   `set_ssl_context()` 时 `connect()` 返回 `UV_EINVAL`。
   * - **只有异步系列**（`connect(host,port,cb)` + `send(req,cb)`）。`connect_wait`
   *   / `send_wait` / `send_wait_plain` 一律 `UV_ENOTSUP`：同步那条路是对
   *   **socket fd** 做阻塞收发，而 QUIC 没有"一条连接的 fd"这回事，它要做的是
   *   泵它自己的循环 —— 那正是 `run()` 的事。
   * - 两个开关同时打开时 **h3 优先**（它换的是传输，不是协议名）。
   *
   * `UVCPP_ENABLE_HTTP3` 没开时这个函数不存在（整个 API 都在
   * `#if UVCPP_HTTP3_ENABLE` 里）。
   */
  void set_http3_enabled(bool on);
  bool http3_enabled() const;
#endif

 private:
  // -------------------------------------------------------------------
  // Internal helpers
  // -------------------------------------------------------------------

  void set_status(int flags);
  void clear_status(int flags);

  /** @brief TCP "data arrived" handler — feeds data to HTTP parser.
   *
   * `tok` 由**调用方**（那条捕了它的读闭包）传进来，本函数只把它当成一个
   * 局部的、不随本对象释放而失效的东西来读。原因见 `alive_token_` 那段：
   * `parser_->execute()` 会同步跑用户回调，用户可以在那里 `delete this`，
   * 而 `execute()` 返回后本函数**还要接着用成员** —— 那时 `this` 已经没了，
   * 唯一还能读的就是这个局部令牌。
   */
  void on_tcp_data(uvcpp_buf* buf, const std::weak_ptr<char>& tok);

  /** @brief TCP "connection closed" handler — 注册成 `tcp_` 的关闭观察者。 */
  void on_tcp_close();

  void on_response_complete();

#if UVCPP_NGHTTP2_ENABLE
  // h2 的回调都在 mem_recv/drain 里**同步**跑，而它们最终会调进用户的
  // `send()` 回调 —— 用户可以在回调里把 client 析构掉。所以习惯与
  // `uvcpp_h2_connection` 那边一致：先把要交付的东西攒齐、把条目从表里摘走，
  // 再碰用户代码。`on_h2_disconnect` 同理，它是唯一被允许析构 h2 对象的地方。
  //
  // 没有"头收全了"那个回调：`uvcpp_h2_session` 已经把 `:status` 与常规头都写进了
  // `st.response`，而本层的交付粒度是**整条响应**（与 h1 一样）。
  void on_h2_body(uvcpp_h2_session& s, uvcpp_h2_stream& st,
                  const char* data, size_t len);
  void on_h2_response_end(uvcpp_h2_session& s, uvcpp_h2_stream& st);
  void on_h2_stream_close(uvcpp_h2_session& s, int32_t stream_id,
                          uint32_t error_code);
  void on_h2_disconnect(uvcpp_h2_connection& c);
  /// 协商出 h2 之后建会话层。只在 `connect()` 的成功回调里调。
  int  start_h2();
  /// `send()` 的 h2 实现：提交一条流并把字节冲出去。
  int  send_h2(const uvcpp_http_request& req,
               std::function<void(const uvcpp_http_response&, int)> cb);
#endif

#if UVCPP_HTTP3_ENABLE
  // h3 的回调跑在 QUIC 那些回调的栈里（可能同步跑进用户的 `send()` 回调），
  // 所以纪律与 h2 那一段相同：**先把要交付的东西攒齐、把条目从表里摘走，
  // 再碰用户代码**。
  /** @brief `connect()` 的 h3 那条路：建端点、装 h3 层、发起握手。 */
  int  connect_quic(const char* host, int port, std::function<void(int)> cb);
  /// 建 h3 层并把 QUIC 的回调接上。**必须在 `quic_->connect()` 返回之后、
  /// 循环转起来之前调**——理由见定义处那段（晚了就收不到 `on_alpn`）。
  int  start_h3();
  /// `send()` 的 h3 实现：提交一条请求并把字节冲出去。
  int  send_h3(const uvcpp_http_request& req,
               std::function<void(const uvcpp_http_response&, int)> cb);
  /// 把 h3 层里已经收全的响应交付给各自的回调（一次一条，见 `take_completed`）。
  void pump_h3_completed();
  /// h3 层报"底层 QUIC 连接结束了"。**持有者在这里销毁 h3 层**。
  void on_h3_disconnect(uvcpp_h3_connection& c);
#endif

  // -------------------------------------------------------------------
  // Member variables
  // -------------------------------------------------------------------

  uvcpp_loop*        loop_ = nullptr;
  uvcpp_tcp_client*  tcp_  = nullptr;
  uvcpp_http_parser* parser_ = nullptr;

  int status_ = HTTP_CLIENT_NONE;
  int last_error_code_ = 0;
  bool keep_alive_ = true;

  /// `add_close_observer()` 的句柄，0 表示没注册（那个 API 的保留值）。
  int close_observer_id_ = 0;

  /// 存活令牌。凡是"由别人持有、指回本对象"的闭包都捕它的 `weak_ptr`。
  ///
  /// 本对象可以被**从它自己的回调里** `delete` —— 在响应回调里删掉客户端是最
  /// 自然的用法，而这正是本类此前会崩的那条路（`~uvcpp_http_client` 里的泵会
  /// 回头再叫一次 `on_tcp_data`，栈上是
  /// `on_tcp_data → parser::execute → llhttp`）。析构函数**第一件事**就 reset
  /// 它，此后那些闭包一律提前返回。
  ///
  /// 之所以必须是令牌而不是一个 `bool` 成员：闭包跑起来时本对象的内存**已经
  /// 还了**，读任何成员都是往释放过的内存上读。令牌是闭包自己的、独立于本对象
  /// 生命周期的一块内存。手法与 `uvcpp_ws_connection::alive_token_` 相同。
  std::shared_ptr<char> alive_token_;

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

#if UVCPP_NGHTTP2_ENABLE
  /// 每条在飞的流各攒一份。**响应按流 id 归位**，不是单个 `pending_resp_` ——
  /// h2 一条连接上可以同时有好几条流在飞，共用一个槽位会互相踩。
  ///
  /// 状态码与响应头不在这里：`uvcpp_h2_session` 已经把它们填进了那条流的
  /// `st.response`，收尾时直接搬过来即可，再存一份就是两份会走散的真值。
  struct h2_stream_state {
    uvcpp_buf           body;   // 与 h1 那条路一样用 uvcpp_buf，收尾时 clone 进去
    std::function<void(const uvcpp_http_response&, int)> cb;
  };

  bool http2_enabled_ = false;   // 用户意图（该不该谈 h2）
  bool h2_active_ = false;       // 谈成了没有（本次连接真的在跑 h2）
  uvcpp_h2_connection* h2_ = nullptr;

  /// 对端 GOAWAY 的三个字段。断开时 `h2_` 就被删了，而"对端道别了没有"正是
  /// 断开**之后**才要问的问题，所以在拆之前抄一份下来。
  bool     peer_goaway_      = false;
  uint32_t peer_goaway_code_ = 0;
  int32_t  peer_goaway_last_ = 0;

  std::map<int32_t, h2_stream_state> h2_streams_;
#endif

#if UVCPP_NGHTTP2_ENABLE || UVCPP_HTTP3_ENABLE
  /// 本次连接协商出来的 ALPN。**两条腿各写各的**：h2 那份来自
  /// `tcp_->tls_alpn_selected()`（TCP 的 TLS 握手），h3 那份来自
  /// `quic_->connection()->alpn_selected()`（QUIC 自己的 TLS 握手）—— 不是同一次
  /// 握手的两种说法，所以不能共用一条写入路径。读取只有 `negotiated_alpn()`。
  std::string negotiated_alpn_;
#endif

#if UVCPP_HTTP3_ENABLE
  /// 一条在飞的 h3 请求各攒一份，与 `h2_stream_state` 逐字同形。**响应按流号
  /// 归位**，理由与 h2 那条相同（一条连接上可以同时有好几条流）。
  struct h3_stream_state {
    std::function<void(const uvcpp_http_response&, int)> cb;
  };

  bool http3_enabled_ = false;   // 用户意图（走不走 h3）
  bool h3_active_ = false;       // 本对象现在真的在跑 h3（h3_ 非空且活着）
  uvcpp_quic_client*   quic_ = nullptr;  // h3 那条腿的端点。本对象拥有它。
  uvcpp_h3_connection* h3_   = nullptr;  // 不拥有它底下那条 QUIC 连接
  /// 键是**流号**。完成的响应本身在 h3 层的队列里（`take_completed`），
  /// 这里只存"这条流要交给谁" —— 与 h2 那边一个道理：两份真值会走散。
  std::map<int64_t, h3_stream_state> h3_streams_;
#endif

#if UVCPP_ZLIB_ENABLE
  bool compress_enabled_ = false;   // OFF by default — user must opt in
  size_t compress_min_body_ = 1024;
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_CLIENT_H
