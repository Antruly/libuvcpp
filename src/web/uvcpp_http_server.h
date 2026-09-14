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

#if UVCPP_WEB_ENABLE

#include <functional>
#include <map>
#include <deque>
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
   * @brief Upgrade handler — called BEFORE routing when Upgrade: websocket
   *        is detected.  The handler should send the 101 response, then take
   *        ownership of the TCP client for WebSocket framing.
   */
  using upgrade_handler_t = std::function<void(uvcpp_http_request&, uvcpp_tcp_client*)>;
  void on_upgrade(upgrade_handler_t handler);

  /** @brief Register a handler for GET + exact path. */
  void get(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for POST + exact path. */
  void post(const std::string& path, http_request_handler handler);
  /** @brief Register a streaming-body handler for POST + exact path. */
  void post_stream(const std::string& path, http_stream_handler handler);
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
   */
  void set_max_body_size(size_t max_bytes);
  size_t max_body_size() const;

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
   * alive only until the call returns (it is fully serialized here). When the
   * response is not keep-alive the connection is closed once its write
   * completes.
   *
   * @param close_after_write whether to close the connection after the write
   *        when not keep-alive. Default true (same as a normal request).
   *        A streaming endpoint (writing the body in chunks AFTER the header)
   *        must pass false and close the connection itself once the body is done.
   */
  void send_response(uvcpp_tcp_client* client, uvcpp_http_response& resp,
                     bool close_after_write = true);

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

  struct conn_ctx {
    uvcpp_http_parser* parser = nullptr;

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

    // --- Body accounting for the current message ---
    size_t body_bytes = 0;        // body bytes seen so far
    bool body_overflow = false;   // exceeded max_body_size_; buffering stopped

    // --- Per-message facts captured at request completion ---
    // The parser is reset lazily on the next inbound chunk, so anything
    // send_response() needs later must be copied out here rather than read
    // back off the parser.
    std::string accept_encoding;  // client's Accept-Encoding for this message
    bool is_head = false;         // this message was a HEAD request

    // --- Outbound write serialization ---
    // uvcpp_tcp_client permits only one async write in flight at a time, so
    // responses are queued and drained one at a time.
    bool write_pending = false;
    std::deque<std::string> write_queue;
    bool close_requested = false;  // a response asked for close after its write
    bool closing = false;          // a close has already been issued
  };

  /** @brief Queue bytes for the connection, draining as the socket permits. */
  void enqueue_write(conn_ctx& ctx, uvcpp_tcp_client* client, std::string wire);

  /** @brief Start the next queued write if the connection is idle. */
  void pump_write(uvcpp_tcp_client* client);

  /** @brief Close a connection exactly once and release its context. */
  void close_connection(uvcpp_tcp_client* client);

  /** @brief Apply response compression in place; returns true if applied. */
  bool apply_compression(conn_ctx& ctx, uvcpp_http_response& resp);

  void remove_ctx(uvcpp_tcp_client* client);

  // -------------------------------------------------------------------
  // Member variables
  // -------------------------------------------------------------------

  uvcpp_tcp_server* tcp_server_ = nullptr;
  int status_ = HTTP_SERVER_NONE;

  std::vector<route_entry> routes_;
  http_request_handler default_handler_;
  upgrade_handler_t upgrade_handler_;
  std::function<void(uvcpp_tcp_client*)> close_handler_;
  /** @brief 新连接钩子（`on_connection`），早于该连接上的任何请求。 */
  std::function<void(uvcpp_tcp_client*)> connection_handler_;
  http_raw_data_hook raw_data_hook_;

  /** @brief Request body cap in bytes; 0 means unlimited. */
  size_t max_body_size_ = 0;

  /**
   * @brief 下一个要发的连接代次号。
   *
   * **先自增再赋值**，且**永不复位** —— 0 留给"没这条连接"，复位就等于允许
   * 旧代次复活，那这个机制就失去意义了（见 `conn_ctx::generation`）。
   */
  uint64_t next_generation_ = 0;

  std::map<uvcpp_tcp_client*, conn_ctx> contexts_;

#if UVCPP_ZLIB_ENABLE
  bool compress_enabled_ = true;
  size_t compress_min_body_ = 1024;
  std::vector<std::string> compress_excluded_types_;
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_SERVER_H
