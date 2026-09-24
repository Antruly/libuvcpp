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

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEB_ENABLE

#include <atomic>
#include <functional>
#include <map>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <uv.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

namespace uvcpp {

#if UVCPP_NGHTTP2_ENABLE
// 只前向声明。`uvcpp_h2_connection` 的头是 pimpl 形状的（nghttp2 只在 .cpp 里），
// 这里连它的头都不引 —— 引了就把"关掉 nghttp2 也能编 web 模块"作废。
class uvcpp_h2_connection;
#endif

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

// Streaming-body delivery: instead of accumulating the whole body in memory,
// the server hands each body chunk to the handler as it is parsed.
enum class http_stream_event : uint8_t {
  HEADERS = 0,  // headers done; req has method/url/version/headers (body empty)
  BODY    = 1,  // a body chunk (data/len; may fire many times)
  END     = 2,  // message complete; handler finishes and calls send_response
};

using http_stream_handler = std::function<void(
    http_stream_event ev,
    const char* data, size_t len,
    uvcpp_http_request& req,
    uvcpp_tcp_client* client)>;

/**
 * @brief Streaming claim hook — decides, at headers time, who owns the body.
 *
 * Called once per message, after the headers are parsed but **before any body
 * byte is delivered**. Returning an **empty** handler means "not claimed": the
 * request takes the ordinary path (body accumulated into the request, then the
 * route handler runs). Returning a **non-empty** handler means "claimed": body
 * bytes are handed to it as @ref http_stream_event::BODY, the message end as
 * @ref http_stream_event::END, and **`on_request` / the route handler is never
 * called for this message**.
 *
 * How it differs from @ref post_stream: that one is a convenience registration
 * for "POST + exact path" and is matched first. This hook is consulted for
 * every request and constrains neither the method nor the path shape, which is
 * what a caller with its own router (patterns, params, wildcards) needs — it
 * does the matching itself and claims selectively.
 *
 * @param req     Headers-only view of the request. Valid only for the duration
 *                of the call: copy anything you need to keep.
 * @param client  The connection, for the rare case the decision depends on it.
 * @return The stream handler that takes over this request's body, or an empty
 *         function to leave the request alone.
 *
 * @warning Runs on the event-loop thread, inside the parser's header callback.
 *          Keep it fast — no I/O, no blocking. A claimed request's body is
 *          **not** subject to @ref set_max_body_size (the server never buffers
 *          it); a claimer that needs a cap enforces its own.
 */
using http_stream_claim = std::function<http_stream_handler(
    uvcpp_http_request& req, uvcpp_tcp_client* client)>;

/**
 * @brief Raw TCP data hook — sees every inbound chunk BEFORE the HTTP parser.
 *
 * Lets an application observe or take over a connection at the byte level,
 * which is not otherwise possible: the server owns the socket's read callback,
 * so nothing outside it can be inserted ahead of the parser.
 *
 * @param client  The connection the data arrived on.
 * @param data    The received bytes. **Read-only** — the hook must not modify
 *                them. The parser's buffering arithmetic assumes the bytes it
 *                was handed are the bytes that arrived, so rewriting them in
 *                place corrupts the remaining parse.
 * @param len     Number of bytes in @p data.
 * @return true   the chunk also goes on to the HTTP parser (observe only);
 *         false  the hook consumed the chunk — it is NOT parsed as HTTP.
 *
 * Runs on the event-loop thread: keep it fast, do not block. For heavy
 * inspection (pcap-style dumping, signature scans) copy the bytes and hand
 * them to uvcpp_work.
 */
using http_raw_data_hook = std::function<bool(
    uvcpp_tcp_client* client, const char* data, size_t len)>;

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
   * @brief 让 ALPN 协商出 `h2` 的连接走 HTTP/2 服务。
   *
   * **默认 false** —— 不显式打开就恒为 HTTP/1.1，与既有行为逐字一致。
   * 打开之后也只对"TLS 握手协商出 h2"的连接生效：明文连接、以及 ALPN 没协商出
   * h2 的连接，照旧走 llhttp。服务端要真的能协商出 h2，还得在
   * `uvcpp_ssl_context` 上 `set_alpn_select_protos({"h2","http/1.1"})` ——
   * 这一层不做代理，两条都得显式给。
   */
  void set_http2_enabled(bool on) { http2_enabled_ = on; }
  bool http2_enabled() const { return http2_enabled_; }

  /**
   * @brief 给**本循环上**的 h2 连接发 GOAWAY，但一条都不关。
   *
   * 停机前该先调它，再走关连接那一套：GOAWAY 排进队列之后要几轮循环才出网，
   * 而关连接是立刻生效的 —— 顺序反了，对端只看到"连接断了"，分不清服务端在
   * 停机还是网络挂了，也就没法把在飞的请求安全地挪到别的连接上。
   *
   * `last_stream_id` 是本端已处理的最大流号，所以对端能据此分辨哪几条它发过、
   * 我们**没处理**（那些可以安全重试）。已有的流一条都不受影响，照跑完。
   *
   * @return 真的把 GOAWAY 排出去（且成功冲网）的连接条数。h2 没开时恒为 0。
   *
   * @note **"本循环上"是刻意的**（多循环）：连接上下文是按循环切的，从一条循环
   *       上去翻另一条循环的表就是数据竞争。所以调用方要在**每条**循环上各调
   *       一次（webapp 的停机就是这样按循环推进的），而不是在 0 号循环上指望
   *       一句管全进程。单循环下"本循环"就是"全部"，行为与从前逐字相同。
   *
   * @note 不关连接是**有意的**：调用方还得留出时间让字节出网，之后自己走
   *       `close_connection()` / `uvcpp_tcp_server::close_all_clients()`。
   */
  size_t begin_h2_goaway();

#if UVCPP_NGHTTP2_ENABLE
  /**
   * @brief 在指定的 h2 流上回一个响应。
   *
   * 处理函数**整对象替换**过 `resp` 时用这个：那种写法会把 `resp.stream_id`
   * 冲成 0，于是普通 `send_response` 会把它当成 HTTP/1.1 的应答。
   */
  int send_h2_response(uvcpp_tcp_client* client, int32_t stream_id,
                       uvcpp_http_response& resp);
#endif


  /**
   * @brief Upgrade handler — called BEFORE routing when Upgrade: websocket
   *        is detected.  The handler should send the 101 response, then take
   *        ownership of the TCP client for WebSocket framing.
   */
  using upgrade_handler_t = std::function<void(uvcpp_http_request&, uvcpp_tcp_client*)>;
  void on_upgrade(upgrade_handler_t handler);

  /**
   * @brief 取走"跟着升级请求一起到达的多余字节"（**升级方专用**）。
   *
   * 升级请求和第一帧挤在同一个 TCP 段里是常态，而那批字节在 HTTP 解析器
   * 吃完请求之后就已经被这次读取走了 —— 升级方如果不取，重新 `read_start`
   * 是读不回来的，表现就是"新协议的第一帧凭空消失"。
   *
   * **必须在升级回调之后取**（升级回调是在解析器 `execute()` 里面同步调的，
   * 那时剩余字节还没算出来），比如在自己的应答写完成回调里 —— WS 层就是这么
   * 用的（`uvcpp_ws_server.cpp` 的 101 写完成回调）。
   *
   * 取走即清空；不是升级连接 / 连接已经没了 / 没有多余字节都返回空串。
   */
  std::string take_upgrade_leftover(uvcpp_tcp_client* client);

  /** @brief Register a handler for GET + exact path. */
  void get(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for POST + exact path. */
  void post(const std::string& path, http_request_handler handler);
  /** @brief Register a streaming-body handler for POST + exact path. */
  void post_stream(const std::string& path, http_stream_handler handler);

  /**
   * @brief Install the streaming claim hook (see @ref http_stream_claim).
   *
   * Single slot — installing a new hook replaces the previous one; passing an
   * empty std::function is equivalent to @ref clear_stream_claim.
   *
   * Matching order per message: a `post_stream` route is tried first (it is a
   * registration, not a policy); only if nothing matched is the claim hook
   * asked. So a hook cannot accidentally shadow an explicitly registered
   * streaming route.
   */
  void set_stream_claim(http_stream_claim hook);

  /** @brief Remove the claim hook (no request is ever claimed). */
  void clear_stream_claim();

  /** @brief Whether a claim hook is currently installed. */
  bool has_stream_claim() const;
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
  // Request size limit
  // -------------------------------------------------------------------

  /**
   * @brief Cap the request body size (bytes). 0 = unlimited (default).
   *
   * Without a cap, a single large (or malicious) upload is buffered in full
   * in memory. When a body exceeds the cap the server stops buffering it and
   * answers 413 with `Connection: close` without routing the request.
   *
   * **Read live.** Unlike the two limits below, this one is compared against
   * every body chunk as it arrives, so changing it takes effect on connections
   * that are already open.
   */
  void set_max_body_size(size_t max_bytes);
  size_t max_body_size() const;

  /**
   * @brief Cap the request header block (bytes). 0 = unlimited (default).
   *
   * Counts the whole header block as it appears on the wire — every field's
   * `name: value\r\n`, excluding the request line and the final empty line.
   * Checked **while** the headers arrive, not after: the parser stops as soon as
   * the running count passes the cap, so an oversized header block costs the
   * server a bounded amount of CPU and bandwidth rather than all of it. The
   * request is answered 431 with `Connection: close` and never routed.
   *
   * **Unlike `max_body_size()`, a claimed request is not exempt**: the running
   * count is enforced by the parser as the headers arrive, so an oversized block
   * is rejected 431 with `Connection: close` before `headers_complete` — and the
   * claim hook is therefore never asked about it.
   *
   * **Takes effect for new connections only.** The value is copied into each
   * connection's parser when that connection is accepted, so changing it here
   * does not affect a connection that is already open.
   */
  void set_max_header_bytes(size_t max_bytes);
  size_t max_header_bytes() const;

  /**
   * @brief Cap the request target / URL (bytes). 0 = unlimited (default).
   *
   * Same early-stop behaviour and the same per-connection timing as
   * `set_max_header_bytes()`; the request is answered 414 with
   * `Connection: close` and never routed.
   */
  void set_max_url_bytes(size_t max_bytes);
  size_t max_url_bytes() const;

  // -------------------------------------------------------------------
  // Raw TCP data interception
  // -------------------------------------------------------------------

  /**
   * @brief Install a hook that sees inbound bytes before the HTTP parser.
   *
   * Single slot — installing a new hook replaces the previous one. The hook
   * runs on the event-loop thread ahead of parsing; returning false means the
   * hook took the chunk and the parser never sees it.
   *
   * @note Passing an empty std::function is equivalent to clear_raw_data_hook().
   */
  void set_raw_data_hook(http_raw_data_hook hook);

  /** @brief Remove the raw data hook (inbound bytes go straight to the parser). */
  void clear_raw_data_hook();

  /** @brief Whether a raw data hook is currently installed. */
  bool has_raw_data_hook() const;

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

  /**
   * @brief 压缩变体缓存的只读计数。
   *
   * 存在的理由与 `uvcpp_tcp_client::try_write_stats()` 逐字相同：没有它，
   * "缓存根本没被用上"与"缓存命中了"在用例里**完全同形** —— 两种情况发出去
   * 的字节一模一样（那正是缓存的定义），只比响应体的话，把缓存整个拆掉也全绿。
   *
   * 与 `apply_compression` 一样声明上不设 `UVCPP_ZLIB_ENABLE` 条件（zlib 关掉
   * 时返回全零），这样调用方不必跟着条件编译。
   */
  struct compress_variant_stat {
    uint64_t hits   = 0;  ///< 查表命中，省掉了一次 deflate
    uint64_t misses = 0;  ///< 带 cache_tag 但表里没有（压完后按需存入）
    uint64_t stored = 0;  ///< 实际存进表的次数（不可压、压完更大时不存）
    size_t   entries = 0; ///< 当前表内条数
    size_t   bytes   = 0; ///< 当前表内字节
  };
  compress_variant_stat compress_variant_stats() const;

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

  /**
   * @brief Send a fully-populated response after a delay (deferred response).
   *
   * The handler calls this on the loop thread (e.g. from an async completion
   * callback) instead of letting on_request_complete send immediately. The
   * handler must have set resp.deferred = true so on_request_complete skips
   * its automatic send. Semantics match the inline send in on_request_complete:
   * apply compression, set keep-alive/close header, serialize, and write back.
   *
   * The write is asynchronous and returns immediately — the caller must not
   * assume the bytes have left by the time this returns, and must keep @p resp
   * alive only until the call returns. When the response is not keep-alive the
   * connection is closed once its write completes.
   *
   * **`resp` 的内容在这一句之后不保证还在**：头/体分开走（`nbufs = 2`，见
   * `enqueue_write`）时体会被**移动**出去 —— 移动的是句柄不是字节，所以不产生
   * 拷贝，但 `resp.body` 之后是空的。要记"上线了多少 body"就用本函数的返回值，
   * **不要**回头读 `resp.body.size()`（那正是返回值取代掉的那次读）。
   *
   * @param close_after_write whether to close the connection after the write
   *        when not keep-alive. Default true (same as a normal request).
   *        A streaming endpoint (writing the body in chunks AFTER the header)
   *        must pass false and close the connection itself once the body is done.
   * @return 这次交给写路径的 body 字节数（压缩之后、HEAD 时 `resp.body` 已在
   *         序列化前清空所以为 0），或早退时为 0。
   */
  size_t send_response(uvcpp_tcp_client* client, uvcpp_http_response& resp,
                       bool close_after_write = true);

  /**
   * @brief 流式响应：只把**头部**入队，body 由调用方随后分块送。
   *
   * 与 `send_response()` 的分工是**结构性**的，不是参数式的：本函数
   * **不走压缩**（`apply_compression` 根本不被调用），因为此时 body 还不存在，
   * 而按 `content_type()` 判定并打上 `content-encoding: gzip` 等于**声明了一件
   * 没发生的事**。真正的守卫是"哪条函数在跑"，`apply_compression` 里那句
   * `transfer-encoding` 早返回是第二道。
   *
   * 头部里**不补** `content-length: 0`，也**不**动 `content-length` /
   * `transfer-encoding` —— 长度语义由调用方在两个入口之间选：
   * 长度已知就自己设 `content-length`，未知就设 `transfer-encoding: chunked`
   * （chunked 的组帧同样由调用方负责，本层不猜）。
   *
   * 调用之后这条连接进入**流式模式**：写队列排空**不会**触发关闭，直到
   * `end_stream()` 把模式关掉。HEAD 请求走的就是 `to_string(false)`，
   * 所以头部与 GET 逐字节相同而 body 一个字节都不发。
   */
  void begin_stream(uvcpp_tcp_client* client, uvcpp_http_response& resp);

  /**
   * @brief 流式头部，**指名是哪条流**。h2 下的正路。
   *
   * h2 上一条连接上可以同时有好几条流式响应，所以"这条连接正在流"不是个
   * 充分的身份 —— 必须带 `stream_id`。`stream_id == 0` 即 HTTP/1.1，走
   * 上面那个两参版本。
   *
   * 与 h1 的三点差别（都是 h2 的协议事实，不是取舍）：
   *   - 头部**不补** `transfer-encoding`：那个字段在 h2 里根本不合法；
   *   - 长度由"直到 END_STREAM"表达，所以调用方**不必**设 `content-length`；
   *     设了也照发（已知长度时那是有用信息），但不会被校验；
   *   - 不走 `to_string()`，由会话层折成 HEADERS 帧。
   */
  void begin_stream(uvcpp_tcp_client* client, int32_t stream_id,
                    uvcpp_http_response& resp);

  /**
   * @brief 流式写一块字节（调用方**自己组好帧**：chunked 的 hex 前缀、
   *        CRLF、终止块都不由本层添加）。
   *
   * @param done 这一块**真正写完**之后调用一次，参数是写的状态（0 = 成功）。
   *             **入队之后保证恰好一次**，包括连接在本块还排在队列里时就被关
   *             的场合（那时状态非 0，由收尾路径统一唤醒 —— 见
   *             `queued_done_cancelled_on_close`）。一个永远不回调的 `done`
   *             会让等它的人（`uvcpp_web_response` 的 `pending_bytes_`）永久
   *             挂起。
   *             **但只有 `return 0` 才作这个保证**：见返回值那行。
   * @return 0 = 已入队（`done` 会被调，恰好一次）；非 0 = 连接已不再受管，
   *         **`done` 不会被调用**，调用方必须自己按失败结算这一块
   *         （`uvcpp_web_response::flush_stream` 就是这么做的）。
   *         **非 0 时不要指望 `done` 兜底，也不要两边都调** —— 第二次结算
   *         可能落在响应对象已经被销毁之后。
   */
  int write_stream(uvcpp_tcp_client* client, std::string bytes,
                   std::function<void(int)> done = std::function<void(int)>());

  /**
   * @brief 流式写一块字节，**指名是哪条流**。h2 下的正路。
   *
   * @param bytes 在 h2 上是**裸 body 字节**，不是 chunked 帧 —— h2 的分块由
   *              DATA 帧本身承担，没有 hex 长度前缀那回事。调用方在两条路上
   *              给的东西因此不一样，这一点由调用方按 `stream_id != 0` 区分。
   * @param done  与 h1 逐条相同：**只有返回 0 才保证恰好回调一次**，而且
   *              **绝不在本函数返回之前同步跑**（h2 侧由写完成路径执行）。
   */
  int write_stream(uvcpp_tcp_client* client, int32_t stream_id,
                   std::string bytes,
                   std::function<void(int)> done = std::function<void(int)>());

  /**
   * @brief 结束流式响应。
   *
   * `close_after` 为真时，队列排空后关闭连接（这也是唯一诚实的收尾方式：
   * 中途失败时头部已经上线、状态码改不了了，只能截断 body 并关连接，
   * 让客户端从"长度对不上"看出传输失败）。
   */
  void end_stream(uvcpp_tcp_client* client, bool close_after);

  /**
   * @brief 结束一条流的流式响应。h2 下的正路。
   *
   * h2 里 `close_after` **不关连接**而只发 END_STREAM：连接是长命的，为一条
   * 流去关掉整条连接会把同一条连接上别的流一起打断，那不是调用方要的语义。
   * 要关连接请用 `close_connection()`。
   */
  void end_stream(uvcpp_tcp_client* client, int32_t stream_id, bool close_after);

  /**
   * @brief Register a callback fired when a connection is removed (closed).
   *        Lets a streaming handler free per-connection state (open file, queued
   *        buffers) on an abrupt disconnect where no END event will fire.
   */
  void on_connection_close(std::function<void(uvcpp_tcp_client*)> cb);

  /**
   * @brief 注册「新连接被接受」回调 —— **早于该连接上的任何请求**。
   *
   * 给上层框架用的入口：要在连接级别做簿记（发连接 id、记对端地址、按连接
   * 计数）的场合，这是唯一能在第一个请求之前动手的地方。回调里抛异常会被
   * 吞掉并打一条 stderr，不影响这个连接继续服务。
   *
   * 钩子跑在该连接**已登记之后**，所以回调里 `connection_generation(client)`
   * 已经是这条连接的有效代次号（可以用来把上层 id 和底层连接对上）。
   *
   * @warning 在 loop 线程上调用；**不要**在这里做耗时操作。也**不要**在这里
   *          自己 `read_start()` —— 读路径由本服务器接管（`set_raw_data_hook`
   *          是观察原始字节的正规入口）。
   */
  void on_connection(std::function<void(uvcpp_tcp_client*)> cb);

  /**
   * @brief 这条连接当前的身份代号；**未连接返回 0**。
   *
   * 存在的唯一理由是让**异步**工作能安全地确认"我要写的还是当初那条连接"。
   * 光有 `uvcpp_tcp_client*` 回答不了这个问题：连接关闭后对象被 delete，新连接
   * 完全可能分配在同一个地址上，于是"地址一样"既可能是同一条连接，也可能是
   * 另一条 —— 拿着旧指针往新连接上写，就是把 A 的响应发给 B。
   *
   * 代次号**单调递增、永不复用**，所以正确的用法是：
   * @code
   *   const uint64_t gen = server->connection_generation(client);  // 开始时记下
   *   // ... 异步读盘/查库 ...
   *   // 完成回调里（loop 线程）：
   *   if (server->connection_generation(client) != gen) return;    // 换人了，丢弃
   * @endcode
   * 地址被新连接复用时 `gen` 对不上（新连接是新号），所以丢弃；连接还活着时
   * 号不变，正常发送。
   *
   * @note 返回 0 表示"这条连接不在服务器的登记表里"，此时**不该**写它。
   */
  uint64_t connection_generation(uvcpp_tcp_client* client) const;

  /** @brief 这条连接是否还挂在服务器上（= `connection_generation() != 0`）。 */
  bool is_connected(uvcpp_tcp_client* client) const;

 private:
  // -------------------------------------------------------------------
  // Route matching
  // -------------------------------------------------------------------

  struct route_entry {
    http_method method;
    std::string path;
    http_request_handler handler;
    http_stream_handler stream_handler;  // set for streaming routes (empty otherwise)
  };

  http_request_handler find_handler(http_method method, const std::string& path);
  http_stream_handler find_stream_handler(http_method method, const std::string& path);

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

  /**
   * @brief 一块待写出的字节 + 它写完之后的回调。
   *
   * `done` 可空（普通响应用不上它），实现上只在**流式写**里被填。
   *
   * `body` 是 #17 加的**第二块**：非空时 `bytes` 只是**头部**，体在 `body` 里
   * 且**不拷**，两块一起交给 `uv_write`（`nbufs = 2`）。为什么要分成两块：一条
   * 响应天然是两段，先合并成一条再发就要把体拷一遍；分开写则由写请求持有第二块
   * 直到完成回调（libuv 的契约是缓冲要活到回调）。
   *
   * 用 `uvcpp_buf` 装而不是裸指针：它既能装**共享视图**（静态层整份命中那份），
   * 也能装**自有块**（压缩产出那份），两种都由写请求按同一条契约接手。能用
   * `uvcpp_buf` 按值放进队列，前提是它**有移动语义**（见 `uvcpp_buf` 里的说明：
   * 少了移动构造，`std::move` 会静默退化成深拷贝，那正是这里要消掉的一份）。
   */
  struct queued_write {
    std::string bytes;
    uvcpp_buf body;
    bool has_body = false;
    std::function<void(int)> done;
  };

  /**
   * @brief 一次性结算器：**在途**那一块的 `done` 由它持有。
   *
   * 排队中的块由 `write_queue` 持有，连接关掉时 `close_connection` /
   * `remove_ctx` 能把它们逐个唤醒。**在途那一块不在队列里** —— 它的 `done`
   * 已经交给 `uvcpp_tcp_client::write` 的完成回调了，而那个回调有一条早返回：
   *
   *     if (!token_alive(life)) { delete wr; return; }   // 客户端已析构
   *
   * 服务端连接是**被 tcp_server 的 close manager 删掉**的（`fire_close_callbacks`
   * 里，而它由对端 EOF 直接触发），于是"对端在写入途中断开"这条最普通的路
   * 会让在途那一块的 `done` **永远不响** —— 等它的人（`send_file` 的传输对象）
   * 连同 fd 和缓冲一起永久挂着。
   *
   * 所以这一块的结算权归 `conn_ctx`：完成回调和关闭路径**都**可以唤醒它，而
   * `fired` 保证恰好一次。谁先来谁说了算 —— 正常路径上先到的是完成回调，
   * 于是调用方仍拿到真实的写错误（`ECONNRESET` 之类）而不是一律的
   * `UV_ECANCELED`。
   */
  struct write_done {
    std::function<void(int)> fn;
    bool fired;
    write_done() : fired(false) {}
  };

  /** @brief 唤醒一个结算器；已经响过就什么都不做（幂等）。 */
  void fire_write_done(const std::shared_ptr<write_done>& d, int status);

  /**
   * @brief 一次写完成后的**唯一**结算路径：一块那条路与两块那条路共用它。
   *
   * 「重入次序铁律」就落在这个函数里（`start_write` 那段注释）：只在这一步碰
   * `contexts_`，把要用的值拷出来再离开；不持有任何引用时调用 `done`；之后再
   * 重新 find 决定下一步。抽成函数是因为**两条路必须逐字同一份**逻辑 ——
   * 抄成两份就是给"以后只改了一份"留口子（本仓的 ws 那族就是这么来的）。
   *
   * @param recycle 复用形态那个写请求（见 @ref conn_ctx::wrecycle）；一元素形态
   *                传 nullptr。非空时在本函数里还回 `ctx.wrecycle`，或按
   *                "表项已经没了"那条路直接放掉。
   */
  void settle_write(uvcpp_tcp_client* client,
                    const std::shared_ptr<write_done>& wd,
                    uvcpp_write* recycle, int status);

  /**
   * @brief 复用形态的写完成回调：**无捕获 ⇒ 函数指针**，`std::function` 就地
   *        存储，这一笔的堆分配就是这么省掉的（见 @ref settle_write）。
   */
  static void recycle_write_done(uvcpp_write* w, uvcpp_tcp_client* client,
                                 int status, void* arg);

  struct conn_ctx {
    uvcpp_http_parser* parser = nullptr;

#if UVCPP_NGHTTP2_ENABLE
    /**
     * @brief 这条连接上的 h2 驱动层；非 h2 连接恒为 nullptr。
     *
     * 与 @ref parser **互斥**：h2 连接没有 llhttp 解析器，h1 连接没有 h2 层。
     * 所有原先只看 `parser` 的地方都要先判这条连接走的是哪条路。
     */
    uvcpp_h2_connection* h2 = nullptr;

    /**
     * @brief h2 下**按流**记的状态，键是 stream_id。
     *
     * 不能复用 `body_buf` / `body_overflow` / `is_head` / `accept_encoding` 那四个
     * **连接级**单槽：h2 的一条连接上并发跑着多条流，共用一个标量不是"少判一次"
     * 而是**判错**。三类后果的触发难度不一样，分开写清楚：
     *
     * - **`is_head` / `accept_encoding` —— 确定性的**，只要有延迟应答就必然踩到。
     *   `dispatch_h2_request` 按到达序写这两个字段，`send_h2_response` 按**发送**序
     *   读它们，两者在并发流上不是一个顺序。于是"流 1 是 GET（延迟）、流 2 是
     *   HEAD"会让流 1 的响应被当成 HEAD **掐掉 body**；同构地，流 1 带
     *   `accept-encoding: gzip` 而流 2 不带，流 1 的响应会**不压缩**发出去。
     *   这正是 `web_ssl_h2_server_func.cpp` 场景 7 钉的两条。
     *
     * - **`body_overflow` —— 需要客户端配合一个特定交织。** 旧代码里
     *   `on_request`（**任何**流）会把标志清成 false，所以要踩到它，流 B 的请求头
     *   必须落在"流 A 已经溢出"和"流 A 的 END_STREAM"之间，**并且** A 剩下的
     *   数据块每一块都 ≤ `max_body_size_`（旧 `on_body` 溢出时把 body 清空，之后
     *   每来一块都拿空 buffer 重新比，块一大就又把标志置回去）。交织本身在
     *   `mem_recv` 里是合法的 —— 帧按线序逐帧处理，B 的 HEADERS 完全可以卡在
     *   A 的两个 DATA 帧中间。所以它是**真缺陷**，只是不像上面两条那样随手可复现。
     *
     * 生命周期 = 流的生命周期：请求头到达时建立，`on_close` 时删除。**不能**在
     * 请求收完时删 —— 延迟应答（`resp.deferred`）还要回来读 `is_head`。
     */
    struct h2_stream_state {
      std::string body;             ///< 收集中；请求收完时被 swap 走
      std::string accept_encoding;  ///< 派发时从这条流自己的请求头里取
      bool        overflow = false; ///< 这条流超过了 max_body_size_
      bool        is_head  = false; ///< 这条流是 HEAD
    };
    std::map<int32_t, h2_stream_state> h2_streams;
#endif

    /**
     * @brief Connection generation — monotonic, never reused. 0 = never assigned.
     *
     * Exists because a `uvcpp_tcp_client*` alone cannot answer "is this still
     * the same connection?". Once a connection closes, its client object is
     * deleted and a NEW connection can be allocated at the same address, so an
     * async completion holding only the pointer would happily write connection
     * A's file to connection B. An async operation captures the generation when
     * it starts and re-checks it before touching the socket; a recycled address
     * carries a different generation, so the write is dropped.
     *
     * This is the same idea as `uvcpp_web_conn_id` one layer up; the http layer
     * needs its own because handlers here get raw pointers, not contexts.
     */
    uint64_t generation = 0;

    uvcpp_http_request request;
    uvcpp_buf body_buf;
    bool headers_done = false;
    bool msg_done = false;
    http_stream_handler stream_handler;  // non-empty once a stream route matched
    uvcpp_http_request stream_request;   // request view handed to the stream handler

    /**
     * @brief Whether @ref stream_request was built for this message.
     *
     * A claim hook (the framework installs one for every connection) forces the
     * headers to be copied at headers time even though most requests are never
     * claimed. Without this flag, on_request_complete would rebuild the same
     * request — paying a second full copy of the header vector per request.
     * When set, the completion path reuses @ref stream_request instead.
     */
    bool stream_view_built = false;

    // --- Body accounting for the current message ---
    size_t body_bytes = 0;        // body bytes seen so far
    bool body_overflow = false;   // exceeded max_body_size_; buffering stopped

    /**
     * @brief A response was already sent during the headers callback.
     *
     * Set by the early-rejection paths (417 unknown Expect, 413 by declared
     * Content-Length). The message is still going to be parsed to completion —
     * we cannot make llhttp skip the body — so every later stage has to keep
     * quiet: on_request_complete must not route it (that would send a second
     * response, and the body_overflow branch would send a third), and the body
     * callbacks must not accumulate anything.
     */
    bool rejected = false;

    /**
     * @brief Close the connection once this message ends, not right now.
     *
     * The early rejections answer while the client is still sending the body.
     * Closing at that moment is a real problem on Windows: closing a socket
     * that still has unread data in its receive buffer makes the stack send an
     * RST, and an RST discards whatever the peer has buffered but not yet read
     * — including the 413 we just wrote. So the write is queued, the flag is
     * set, and the close happens in on_request_complete, once the client has
     * stopped sending.
     */
    bool close_after_message = false;

    /**
     * @brief A close was decided while the body was still arriving; do it at
     *        message end instead.
     *
     * @ref close_after_message is set by the headers-time rejections, which know
     * up front that they are answering early. This flag is the *runtime*
     * version of the same rule: a claimed stream handler is free to answer (and
     * ask for the connection to close) from any BODY callback, since it may
     * decide to stop reading an upload the moment it sees enough of it. That
     * response is written while the peer is still sending, so closing right
     * after the write hits exactly the Windows RST problem described on
     * @ref close_after_message — the response we just wrote sits unread in the
     * peer's receive buffer and an RST discards it.
     *
     * pump_write() therefore parks the close here and on_request_complete()
     * honours it once the parser reports the message complete (the peer has
     * stopped sending by then). Only ever set while @ref stream_handler is
     * installed, so a malformed message — which never completes and so has no
     * one to hand the close to — can still close immediately.
     */
    bool defer_close_to_message_end = false;

    /**
     * @brief The message carried `Expect: 100-continue` (HTTP/1.1 only).
     *
     * Recorded at headers time and acted on there — the interim response is
     * queued before the parser returns, so the client can start sending the
     * body without waiting for its own timer.
     */
    bool expect_continue = false;

    /**
     * @brief 这条连接正在升级（`upgrade_handler_` 已经接走）。
     *
     * 升级请求和第一帧常常挤在同一个 TCP 段里到达：HTTP 解析器吃完请求就把
     * 剩下的字节留给了"下一条消息"，而升级之后根本没有下一条 HTTP 消息 ——
     * 那批字节必须交给新协议，否则升级方看到的第一个 WS 帧凭空消失。升级回调
     * 是在 `execute()` **里面**同步调的（那时还不知道剩下多少），所以先立这个
     * 旗标，等 `execute()` 返回后在 @ref pending 里存下来。
     */
    bool upgrading = false;

    /**
     * @brief 升级请求之后多余的那批字节，等升级方来取
     *        （`uvcpp_http_server::take_upgrade_leftover`）。
     */
    std::string pending;

    // --- Per-message facts captured at request completion ---
    // The parser is reset lazily on the next inbound chunk, so anything
    // send_response() needs later must be copied out here rather than read
    // back off the parser.
    std::string accept_encoding;  // client's Accept-Encoding for this message
    bool is_head = false;         // this message was a HEAD request

    // --- Outbound write serialization ---
    // uvcpp_tcp_client permits only one async write in flight at a time, so
    // responses are queued. Draining one queued write at a time would make a
    // pipelined burst cost one syscall (and one packet) per response, so
    // pump_write() gathers a batch of them into a single write instead — see
    // kCoalesceMaxBytes in the .cpp for why that is a per-syscall win.
    bool write_pending = false;
    std::deque<queued_write> write_queue;

    /**
     * @brief **在途**那一块的结算器（见 @ref write_done）。队列非空或正在写
     * 时它必然非空 —— 它是关闭路径唤醒在途那一块的唯一入口。
     */
    std::shared_ptr<write_done> inflight;

    /**
     * @brief 按连接复用的那个写请求（@ref uvcpp_tcp_client::write_owned）。
     *
     * 稳态下这里是 **nullptr**：`start_write` 取走它、写完后 `settle_write`
     * 还回来，中间只在"写请求正在网络层手里"那一刻不在这格。所以它既不与
     * @ref inflight 重叠，也不与 @ref write_queue 重叠。
     *
     * **只有这一张表认它**（库里只在完成回调那一刻认），所以两个抹除表项的地方
     * 都必须放掉它：`remove_ctx`（对端断开、关闭收尾都经它）与析构函数里那个
     * `tbl.clear()` 循环。少一处就是每条连接漏一个写请求对象加它的头部缓冲。
     *
     * 普通响应走这条路是因为它**一次堆分配都不发生**：请求对象（连同里面的
     * `uv_write_t`）与头部缓冲都按连接复用，完成回调是个函数指针。有 `done`
     * 的调用点仍走原来那条 —— 结算器要唤醒那个闭包，而闭包的去向只有它知道。
     */
    uvcpp_write* wrecycle = nullptr;

    bool close_requested = false;  // a response asked for close after its write
    bool closing = false;          // a close has already been issued

    /**
     * @brief 这条连接正处于**出站**流式响应中（`begin_stream` 到 `end_stream`）。
     *
     * 与入站的 @ref stream_handler 是两码事：那个是"请求体正在流进来"，
     * 这个是"响应体正在流出去"。存在的唯一理由是让 pump_write 在队列**暂时**
     * 排空时不要收尾 —— 头部写完到第一块 body 入队之间总有一个空窗，
     * 没有这个标志，一个非 keep-alive 的流会在第一块 body 之前就被关掉。
     */
    bool out_streaming = false;
  };

  /** @brief Queue bytes for the connection, draining as the socket permits. */
  void enqueue_write(conn_ctx& ctx, uvcpp_tcp_client* client, std::string wire,
                     std::function<void(int)> done = std::function<void(int)>());

  /**
   * @brief 头部与体**分开**入队（体不拷），出队时按 `nbufs = 2` 写出去。
   *
   * `head` 必须与 `body` 拼起来正好是那条报文 —— 这个前提由调用方保证
   * （今天只有 `send_response` 用，判据写在那里：`to_string(false)` 接上 body
   * 与 `to_string(true)` 逐字节相同）。`body` 按值收，调用方用 `std::move` 交
   * 出来即可 —— 移动的是句柄，不是字节。
   */
  void enqueue_write(conn_ctx& ctx, uvcpp_tcp_client* client, std::string head,
                     uvcpp_buf body,
                     std::function<void(int)> done = std::function<void(int)>());

  /**
   * @brief Drain the queue if the connection is idle: **the whole queue in one
   * write** when it fits, otherwise the front block alone.
   */
  void pump_write(uvcpp_tcp_client* client);

  /**
   * @brief @ref pump_write 的**已知上下文**版本 —— 省掉一次 `contexts_` 查找。
   *
   * `ctx` 必须就是 `contexts_` 里 `client` 那一项。调用方必须保证**从拿到 `ctx`
   * 到这一句之间没有跑过用户代码**：`close_connection` 会把表项摘掉，之后这个引用
   * 就悬垂了。`on_written` 尾部的调用处是现查现用，`end_stream` 与
   * `enqueue_write` 的调用处中间只有赋值。
   */
  void pump_write(conn_ctx& ctx, uvcpp_tcp_client* client);

  /**
   * @brief 把**已经拿在手上**的这一块立刻发出去（不经过 `write_queue`）。
   *
   * 前置条件：`ctx.write_pending == false && ctx.closing == false`。这两条成立时
   * 队列**必然**是空的 —— 往队列里放的只有 @ref enqueue_write，而它放完必定泵
   * 一次，于是"队列非空"永远蕴含"有一次写在途"。所以这条路与"入队再泵"逐字等价，
   * 省下的是每个响应一次 `queued_write` 构造 + 一次入队 + 一次出队 + 两次移动。
   *
   * `body` 为 `nullptr` 时走单块写；非空时那块的内容按 @ref uvcpp_tcp_client::write
   * 的消费语义交出去（共享视图接引用计数、自有块接所有权），**返回后 `*body` 即
   * 可析构**。
   */
  void start_write(conn_ctx& ctx, uvcpp_tcp_client* client, std::string wire,
                   uvcpp_buf* body, bool has_body, std::function<void(int)> done);

  /** @brief Close a connection exactly once and release its context. */
  void close_connection(uvcpp_tcp_client* client);

  /** @brief Apply response compression in place; returns true if applied. */
  bool apply_compression(conn_ctx& ctx, uvcpp_http_response& resp);

  /**
   * @brief Answer during the headers callback and stop caring about the rest.
   *
   * Queues a `Connection: close` response, marks the context rejected so no
   * later stage answers again, and arranges for the close to happen at message
   * end rather than now (see @ref conn_ctx::close_after_message).
   *
   * @param message_will_complete false when the parse was abandoned mid-message
   *        (an oversized URL/header block), where "at message end" never comes
   *        and the close has to ride on the write instead — otherwise the
   *        connection sits there for good, because **this layer has no idle
   *        sweep**: the one named elsewhere is the webapp layer's (60 s default)
   *        and only exists when a webapp sits above this server.
   *        See @ref pump_write.
   */
  void reject_early(conn_ctx& ctx, uvcpp_tcp_client* client, http_status status,
                    bool message_will_complete = true);

  /**
   * @brief Validate `Expect` and record `100-continue` if asked for.
   *
   * @return false if the request was answered (417) and the caller must stop;
   *         true to carry on.
   *
   * HTTP/1.0 is exempt: Expect has no meaning there (RFC 7231 §5.1.1), and
   * failing a 1.0 message over a header it never defined would break clients
   * talking through proxies that forward it blindly.
   *
   * @param msg_headers 这条消息的头表。**必须是调用方手上那一份**：建请求视图
   *        的那两条路会把头从解析器上搬走（`take_headers()`），搬过之后
   *        `parser->get_headers()` 就是空表了。
   */
  bool check_expect_header(conn_ctx& ctx, uvcpp_tcp_client* client,
                           const http_headers& msg_headers);

  /**
   * @brief Refuse, before the body arrives, a message whose declared
   *        Content-Length is over @ref max_body_size_.
   *
   * Chunked messages have no declared length and cannot be refused this way;
   * they still hit the message-complete check once the cap is exceeded.
   *
   * @return false if the request was answered (413) and the caller must stop;
   *         true to carry on.
   */
  bool reject_oversized_declared(conn_ctx& ctx, uvcpp_tcp_client* client);

  void remove_ctx(uvcpp_tcp_client* client);

  // -------------------------------------------------------------------
  // Member variables
  // -------------------------------------------------------------------

#if UVCPP_NGHTTP2_ENABLE
  /// h2 连接的分流入口（`on_tcp_connection` 里判完 ALPN 后进来）。
  void on_tcp_connection_h2(uvcpp_tcp_client* client);

  /// 把一条**已收全**的 h2 请求交给路由，并在对应的流上回话。
  void dispatch_h2_request(uvcpp_tcp_client* client, int32_t stream_id,
                           uvcpp_http_request& req);
#endif

  /// 没有显式打开就恒 false。
  bool http2_enabled_ = false;

  uvcpp_tcp_server* tcp_server_ = nullptr;
  int status_ = HTTP_SERVER_NONE;

  std::vector<route_entry> routes_;
  http_request_handler default_handler_;
  upgrade_handler_t upgrade_handler_;
  std::function<void(uvcpp_tcp_client*)> close_handler_;
  /** @brief 新连接钩子（`on_connection`），早于该连接上的任何请求。 */
  std::function<void(uvcpp_tcp_client*)> connection_handler_;
  http_raw_data_hook raw_data_hook_;
  http_stream_claim stream_claim_;

  /** @brief Request body cap in bytes; 0 means unlimited. */
  size_t max_body_size_ = 0;
  size_t max_header_bytes_ = 0;
  size_t max_url_bytes_ = 0;

  /**
   * @brief 下一个要发的连接代次号。
   *
   * **先自增再赋值**，且**永不复位** —— 0 留给"没这条连接"，复位就等于允许
   * 旧代次复活，那这个机制就失去意义了（见 `conn_ctx::generation`）。
   *
   * **原子**：多循环下每一条工作循环都在自己线程上给新连接发号（代次号是
   * **全服务**一个序列，不按循环切 —— 调用方拿它当"这条连接的身份"比对，
   * 两条循环各自从 1 数起的话身份就不唯一了）。
   */
  std::atomic<uint64_t> next_generation_{0};

  /**
   * @brief 连接上下文登记表，**按循环切**（下标 = `uvcpp_tcp_client::loop_index()`）。
   *
   * 这正是多循环的闸门：一条连接的全部回调都在它自己那条循环的线程上跑，
   * 而这张表是**每请求**都要碰的（53 处引用），所以它必须是 per-loop 的 ——
   * 一张全局表在两条循环上并发 insert/erase 是 UB，不是"慢一点"。
   *
   * 尺寸在 @ref listen 里定死（`uvcpp_tcp_server::loop_count()`），此后
   * **只读**，所以取表这一路不需要锁，也没有扩容。单循环下恒为一个元素。
   */
  std::vector<std::map<uvcpp_tcp_client*, conn_ctx> > contexts_;

  /**
   * @brief 取 \p client 那条连接所在的登记表。
   *
   * **必须在这条连接自己的循环线程上调用** —— 它返回的是一张别的线程可能正在
   * 动的表的引用；跨循环调用等于把它交出去给竞争。调用点全是连接自己的回调，
   * 所以这条约束天然成立。
   *
   * @warning **查完之后要拿这张表自己的 `end()` 比**，不要把 `find()` 的结果去和
   *          `ctxs_here().end()` 比：两张表不是同一个容器时，跨容器比较迭代器是
   *          未定义行为（`std::map` 的 `end()` 是各自表头节点的地址，于是"找不到"
   *          这条早退**不生效**，后面紧跟着的 `it->second` 就解引用了 `end()`）。
   *          n > 1 时两张表不同这件事，正是 `-1`（不在循环线程上）与"回调跑在
   *          别的循环的线程上"这两条路会造出来的。
   *
   *          ```cpp
   *          std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
   *          auto it = tbl.find(client);
   *          if (it == tbl.end()) return;
   *          ```
   */
  std::map<uvcpp_tcp_client*, conn_ctx>& ctxs_of(uvcpp_tcp_client* client);

  /**
   * @brief 取**本线程**所在循环的登记表。
   *
   * 给手上没有连接对象的入口用（停机时"把本循环上的 h2 连接都道别"）。
   * 判"我在几号"用 `uvcpp_loop_index_of_this_thread()`，所以它同样只能在
   * 循环线程上调（n > 1 时在别的线程上调会 `abort`，见 @ref ctxs_at）。
   */
  std::map<uvcpp_tcp_client*, conn_ctx>& ctxs_here();

  /**
   * @brief @ref ctxs_of / @ref ctxs_here 共用的取值。
   *
   * 越界（表外的循环号）夹回 0 号 —— 它至少是越界安全的，而按循环号取表的唯一
   * 来源就是连接自己的循环号与线程自己的循环号，两者都落在表内。
   *
   * @note **`-1` 不是越界，是"不在任何循环线程上"**，在多循环下直接 `abort`：
   *       夹回 0 号只处理 n 张表里的一张，却让调用方（`begin_h2_goaway()` 那类
   *       "每条循环各调一次"的入口）拿到一个看着正常的返回值 —— 错得比崩还安静。
   *       单循环下表只有一张，夹回 0 号与从前逐字相同，那条路原样保留。
   */
  std::map<uvcpp_tcp_client*, conn_ctx>& ctxs_at(int loop_index);

  /// @brief @ref ctxs_of 的 const 版（`connection_generation()` 这类只读入口用）。
  const std::map<uvcpp_tcp_client*, conn_ctx>& ctxs_of(
      uvcpp_tcp_client* client) const;
  const std::map<uvcpp_tcp_client*, conn_ctx>& ctxs_at(int loop_index) const;

#if UVCPP_ZLIB_ENABLE
  bool compress_enabled_ = true;
  size_t compress_min_body_ = 1024;
  std::vector<std::string> compress_excluded_types_;

  // -------------------------------------------------------------------
  // 压缩变体缓存（#11 第 3 条）：`(cache_tag, 编码) -> 压好的字节`
  //
  // **只记"算过的"，不决定"要不要算"**：门槛、MIME 表、accept-encoding 协商
  // 仍然只在 `apply_compression` 一处判，而且永远是**先判完、确定要压**才来
  // 查表。所以本缓存没有可能与配置打架 —— 它不参与任何决策。
  //
  // 只有填了 `cache_tag` 的响应有资格进来（今天只有静态文件那条路填），
  // 于是动态响应的行为与本缓存逐字无关，爆炸半径圈死在一个路由族。
  //
  // 键是 `编码字符 + cache_tag`，**编码在前**是有意的：单字符前缀使
  // `(编码, tag) -> 键` 单射，后缀则不然（tag 自己含分隔符时会撞成同一个键）。
  // -------------------------------------------------------------------
  struct compress_variant {
    /// 压缩产物**只有这一份**：表与响应共用同一个句柄（`uvcpp_buf::share()`）。
    /// 命中时把句柄递出去、不再拷一份回来；存表时也只是存同一个句柄。表这边
    /// 是 `shared_ptr<const std::string>` —— 拿不到可写指针，那个 `const` 就是
    /// 「共享出去之后被人改了」的防线（有人要写，`uvcpp_buf` 会先物化出私有的
    /// 一份，表里这份纹丝不动）。
    ::std::shared_ptr<const ::std::string> data;
    uint64_t  last_used = 0;
    /// 这一条占多少字节。淘汰与上限都按它算（以前是 `uvcpp_buf::size()`）——
    /// 换成句柄之后**必须**换过来，否则字节账恒为 0、淘汰只剩条数那一条腿。
    size_t bytes() const {
      return this->data != nullptr ? this->data->size() : 0;
    }
  };
  std::map<std::string, compress_variant> compress_variants_;
  uint64_t compress_variant_clock_  = 0;
  uint64_t compress_variant_hits_   = 0;
  uint64_t compress_variant_misses_ = 0;
  uint64_t compress_variant_stored_ = 0;

  /// 护 `compress_variants_` 与上面那四个计数 —— **多循环下本表不是按循环切的**。
  ///
  /// 它是**跨循环共用的缓存**：切了就退回"每条循环各自 deflate 一遍同一个静态
  /// 文件"，命中率按循环数目线性掉（n 条循环 ⇒ 同一个文件被压 n 次、各存一份、
  /// 各占一份字节）。所以这里要的是**互斥**，不是分片。
  ///
  /// 加锁点只有三个**外层入口**（`..._locked()` 那几个帮手不自已加锁，
  /// 因为它们互相调用：淘汰要算字节总量，同一条非递归锁不能进两次）：
  /// 命中那条路、存入（含淘汰）那条路、`compress_variant_stats()`。
  /// 临界区里只碰表与计数，不调任何用户代码 —— `finish_headers()` 在锁外。
  ///
  /// `mutable`：`compress_variant_stats()` 是 const。
  mutable std::mutex compress_mu_;

  /// 表内字节总量。**从表里算出来，不存**：以前有一个 `compress_variants_bytes_`
  /// 计数字段，加在存入那条路上、减在淘汰那条路上 —— 而淘汰那句 `-=` 是要被删掉
  /// 也照样编得过、且**静默**失效的（条数那条腿还在照常淘汰，只有大文件会因为字节
  /// 上限永远触发不了而悄悄不再命中）。账一旦不存在，这种漂移就写不出来了。
  ///
  /// 代价：淘汰循环每轮都要重算一次（`compress_variant_evict_locked()`），而每轮只删一条。
  /// 原来那句线性找 LRU 本来就是每轮 O(n)，所以这里没有换复杂度类，只是常数翻倍。
  ///
  /// 调用方**必须已持有 `compress_mu_`**。
  size_t compress_variant_total_bytes_locked() const;

  /// 按字节淘汰最久未用的若干条；单条可能很大，条数上限卡不住内存。
  /// 调用方**必须已持有 `compress_mu_`**。
  void compress_variant_evict_locked();
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_SERVER_H
