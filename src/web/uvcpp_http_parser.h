/**
 * @file src/web/uvcpp_http_parser.h
 * @brief C++ wrapper around llhttp — streaming HTTP/1.x message parser.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Wraps nodejs/llhttp in a type-safe C++ API that follows the project's
 * established patterns (trampoline callbacks, streaming I/O, status queries).
 *
 * Supports:
 * - Request and response parsing (selectable by http_parser_mode)
 * - Streaming (partial data) parsing via repeated execute() calls
 * - Chunked transfer encoding (RFC 7230 §4.1)
 * - Connection: keep-alive detection
 * - Upgrade detection (for WebSocket handshake)
 * - Parser reuse via reset() for Keep-Alive connections
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_PARSER_H
#define SRC_WEB_UVCPP_HTTP_PARSER_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEB_ENABLE

#include <functional>
#include <string>
#include <cstdint>

#include <uvcpp/uvcpp_define.h>
#include <web/uvcpp_http_common.h>

// Forward-declare llhttp_t to match llhttp.h's actual typedef.
// llhttp.h does: typedef struct llhttp__internal_s llhttp_t;
// We must match this exactly to avoid C2371 redefinition errors.
struct llhttp__internal_s;
typedef struct llhttp__internal_s llhttp_t;

namespace uvcpp {

// Internal opaque types (defined in .cpp, wrap llhttp C structures)
struct llhttp_raw;
struct llhttp_settings_raw;

// =========================================================================
// 解析器模式
// =========================================================================

/** @brief Whether to parse an HTTP request or response. */
enum class http_parser_mode : uint8_t {
  PARSE_REQUEST  = 0,  ///< Parse HTTP requests (method, URL, headers, body)
  PARSE_RESPONSE = 1,  ///< Parse HTTP responses (status, headers, body)
};

// =========================================================================
// 解析器内部状态
// =========================================================================

/** @brief Current parsing phase. */
enum class http_parser_state : uint8_t {
  IDLE          = 0,  ///< Ready / just reset
  HEADER        = 1,  ///< Parsing start-line + headers
  BODY          = 2,  ///< Parsing message body (Content-Length)
  CHUNK_HEADER  = 3,  ///< Parsing chunk size line
  CHUNK_BODY    = 4,  ///< Parsing chunk data
  TRAILER       = 5,  ///< Parsing trailer headers
  COMPLETE      = 6,  ///< Message fully parsed
  PARSE_ERROR   = 7,  ///< Parse error
};

// =========================================================================
// HTTP 解析器类
// =========================================================================

/**
 * @brief Streaming HTTP/1.x message parser.
 *
 * Wraps llhttp to provide a project-consistent C++ API.
 *
 * Usage (parsing a request):
 * @code
 *   uvcpp_http_parser parser(http_parser_mode::PARSE_REQUEST);
 *   parser.set_on_url([](const char* d, size_t n) { ... });
 *   parser.set_on_headers_complete([]() { ... });
 *   parser.set_on_body([](const char* d, size_t n) { ... });
 *   parser.set_on_message_complete([]() { ... });
 *
 *   // Feed data as it arrives over TCP:
 *   while (tcp_has_data()) {
 *     size_t consumed = parser.execute(data, len);
 *     // unconsumed data goes to next message
 *   }
 *   parser.finish();  // signal EOF
 *
 *   if (parser.is_complete() && !parser.has_error()) {
 *     http_method m = parser.get_method();
 *     // ...
 *     parser.reset();  // reuse for next Keep-Alive request
 *   }
 * @endcode
 */
class UVCPP_API uvcpp_http_parser {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_http_parser)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_http_parser)

  // -------------------------------------------------------------------
  // 构造 / 销毁
  // -------------------------------------------------------------------

  /**
   * @brief Construct a parser for requests or responses.
   * @param mode  PARSE_REQUEST or PARSE_RESPONSE.
   */
  explicit uvcpp_http_parser(http_parser_mode mode);

  // -------------------------------------------------------------------
  // 重置
  // -------------------------------------------------------------------

  /**
   * @brief Reset the parser for a new message on the same connection.
   *
   * Clears internal accumulation buffers and resets the llhttp state.
   * Callbacks are preserved — re-registration is not needed.
   */
  void reset();

  /**
   * @brief Tell a response parser which method the request used.
   *
   * Only `HTTP_HEAD` changes anything.  A response to HEAD carries the
   * headers of the corresponding GET — `Content-Length` included — but no
   * body (RFC 7231 §4.3.2).  llhttp expresses "this response has no body"
   * as `F_SKIPBODY` and **never derives it from `method` itself**
   * (`llhttp__after_headers_complete`, `_local_deps/llhttp/src/http.c:59`
   * and `:138`): the caller sets it.
   *
   * Call after reset() — reset() re-inits llhttp, which clears flags.
   * Without it a client parsing a HEAD response waits forever for a body
   * that the server is never going to send.
   */
  void set_request_method(http_method m);

  // -------------------------------------------------------------------
  // 流式解析
  // -------------------------------------------------------------------

  /**
   * @brief Feed data into the parser.
   *
   * May be called multiple times as data arrives.  Relevant callbacks
   * (on_url, on_header_field, on_body, ...) fire synchronously during
   * this call.
   *
   * @param data  Pointer to received bytes.
   * @param len   Number of bytes available.
   * @return      Number of bytes consumed.  A value less than @p len
   *              means the remaining bytes belong to the next message
   *              (or the parser is in an error/paused state).
   */
  size_t execute(const char* data, size_t len);

  /**
   * @brief Signal that no more data will arrive for this message (EOF).
   * @return 0 on success, non-zero if the message was incomplete.
   */
  int finish();

  // -------------------------------------------------------------------
  // 状态查询
  // -------------------------------------------------------------------

  /** @brief True if the entire message (headers + body) has been parsed. */
  bool is_complete() const;

  /** @brief True if a parse error occurred. */
  bool has_error() const;

  /** @brief Current parser phase. */
  http_parser_state get_state() const;

  /** @brief Human-readable error description from llhttp. */
  const char* error_reason() const;

  // -------------------------------------------------------------------
  // 解析结果（is_complete() 为 true 后读取）
  // -------------------------------------------------------------------

  /** @brief HTTP method (PARSE_REQUEST mode). */
  http_method get_method() const;

  /** @brief HTTP status code (PARSE_RESPONSE mode). */
  http_status get_status_code() const;

  /** @brief HTTP protocol version. */
  uvcpp_http_version get_uvcpp_http_version() const;

  /** @brief Request URL (PARSE_REQUEST mode). */
  const std::string& get_url() const;

  /** @brief Parsed headers. */
  const http_headers& get_headers() const;

  /**
   * @brief 把整条头表**搬走**，本解析器上这条消息的头就空了。
   *
   * 与 `get_headers()` 的差别不只是"搬还是拷"：`http_headers` 是
   * `std::vector<http_header>`、`http_header` 是两个 `std::string`，所以
   * **拷一份** = 一次向量分配（n 个元素）**外加每个非 SSO 的值各一次**；
   * **搬**（本函数）= 同样一次向量分配、n 个元素搬过去，**一个字符串都不分配**。
   * 也就是说"拷"多花的钱在**值的字符串**上，不在向量缓冲 —— 短值（SSO）上两者打平。
   *
   * 搬的是**元素，容量留在解析器上**：`clear()` 之后那块已经长好的缓冲归下一条消息
   * 用，steady state 每请求只有这一次分配。`clear()` 不是装饰 —— `std::move()`
   * 之后"表是空的"只是 moved-from 向量的**通常**行为，标准并不保证；显式清空才是。
   *
   * 只能在这条消息**解析完之后**用（`headers_complete` 回调起）。流水线的下一条
   * 不受影响：`ll_on_message_begin` 本来就会清空 `headers_`。
   * 但**本解析器上这条消息的头就再也读不到了** —— 谁还需要它们，谁就要自己留一份
   * （`uvcpp_http_server` 的做法是让 `message_has_body` 与 `check_expect_header`
   * 改读搬过去的那一份）。
   *
   * ★这块缓冲按**连接**钉住 high-water：它现在活在解析器对象上（旧形状跟着请求对象
   * 一起释放），所以头最多的那条请求决定该连接之后每请求的常驻占用。上限是
   * `set_max_header_bytes()`，但那个值**默认 0 = 不限**。
   *
   * ★ 服务端那条**常规**路径已经不再用它了：那条路走 `take_headers_into()`（目的地
   * 挂在连接上，连这一次向量分配也省掉）。本函数保留成"调用方手上没有可复用的
   * 目的地"时的便利形状。
   */
  http_headers take_headers();

  /**
   * @brief 把整条头表**搬进** `dst`：元素搬过去，`dst` 那块缓冲留着复用。
   *
   * 与 `take_headers()` 只差一件事：**谁出那块 vector 缓冲**。`take_headers()`
   * 的 `out` 是函数内新构造的空 vector ⇒ 那次 `_M_allocate` **每请求必付**；
   * 本函数把元素搬进调用方**已经有的**缓冲里（`clear()` 只改 size、不还容量）
   * ⇒ 容量够时**一次分配都不做**。
   *
   * 前提是 `dst` 活得比请求长 —— 服务端把它放在**连接**上（`conn_ctx::request`
   * 与 `conn_ctx::stream_request`）。放宽成"任意 vector"也成立，只是那样每次都会
   * 退化成一次分配，与 `take_headers()` 等价。
   *
   * `dst` 里原有元素**先被清掉**（本函数不追加到既有内容上）；搬走的后果与
   * `take_headers()` 完全一样：解析器上这条消息的头空了、容量留在解析器上。
   */
  void take_headers_into(http_headers& dst);

  /** @brief Whether the message has Connection: keep-alive semantics. */
  bool should_keep_alive() const;

  /** @brief Content-Length value (0 if no body / chunked). */
  uint64_t get_content_length() const;

  /** @brief Whether the message requests an Upgrade (e.g. WebSocket). */
  bool is_upgrade() const;

  // -------------------------------------------------------------------
  // Callback setters — trampoline model
  //
  // Each setter stores the callback as a C function-pointer + void*
  // pair to avoid MSVC std::function copy-chain corruption through
  // libuv callbacks.
  // -------------------------------------------------------------------

  /** @brief Called with chunks of the request URL. */
  void set_on_url(std::function<void(const char*, size_t)> cb);

  /** @brief Called with the response status reason phrase. */
  void set_on_status(std::function<void(const char*, size_t)> cb);

  /** @brief Called with each header field name chunk. */
  void set_on_header_field(std::function<void(const char*, size_t)> cb);

  /** @brief Called with each header field value chunk. */
  void set_on_header_value(std::function<void(const char*, size_t)> cb);

  /** @brief Called when all headers have been received. */
  void set_on_headers_complete(std::function<void()> cb);

  /** @brief Called with body data chunks (may be called multiple times). */
  void set_on_body(std::function<void(const char*, size_t)> cb);

  /** @brief Called when the complete message has been parsed. */
  void set_on_message_complete(std::function<void()> cb);

  /** @brief Called at the start of each message (once per message, including
   *         the ones that begin mid-buffer behind a completed message). */
  void set_on_message_begin(std::function<void()> cb);

  /** @brief Called at the start of each chunk (parser->content_length
   *         holds the chunk size in bytes). */
  void set_on_chunk_header(std::function<void(size_t)> cb);

  /** @brief Called when a chunk's data has been fully received. */
  void set_on_chunk_complete(std::function<void()> cb);

  // -------------------------------------------------------------------
  // 请求目标 / 头部长度的上限
  //
  // 在 llhttp 的头部回调里**边收边判**，超了就当场返回非 0 让解析停下 ——
  // 放到 `headers_complete` 再判是没用的：那时候 32 MiB 已经解析完了，
  // 挡住的只是内存，CPU 和带宽照付。llhttp 自带的版本没有
  // `HPE_HEADER_OVERFLOW`（9.2.0 的错误枚举里没有这一项），所以这道限只能
  // 自己记。
  // -------------------------------------------------------------------

  /** @brief 撞上的是哪一道限。用于让调用方回 414/431，而不是笼统的 400。 */
  enum class size_limit {
    NONE,
    URL,     ///< 请求目标超过 `set_max_url_bytes()`
    HEADER,  ///< 请求头超过 `set_max_header_bytes()`
  };

  /** @brief 读出撞上的限；`NONE` 表示没撞上。 */
  size_limit limit_hit() const { return limit_hit_; }

  /** @brief 请求目标上限（字节）。0 = 不设限（默认）。 */
  void set_max_url_bytes(size_t n) { max_url_bytes_ = n; }

  /** @brief 请求头上限（字节）。0 = 不设限（默认）。
   *
   * 计的是整个头块在线上占的字节：每个字段的 `名字: 值\r\n`（`": "` 与行尾
   * CRLF 各按 2 字节在字段收尾时一次记入），**不含**请求行、也不含收尾那个空行。
   * 一个字段收完时账是**精确**的；值还在到达的过程中少记了那 2 字节的 `": "`，
   * 所以判定最多晚 2 字节 —— 方向永远是"不早判"，不会误拒一个没超的请求。
   */
  void set_max_header_bytes(size_t n) { max_header_bytes_ = n; }

 private:
  // -------------------------------------------------------------------
  // Trampoline callback types (C function pointers + void* user data)
  // -------------------------------------------------------------------
  using data_cb_t  = void(*)(const char*, size_t, void*);
  using void_cb_t  = void(*)(void*);
  using sz_cb_t    = void(*)(size_t, void*);

  // -------------------------------------------------------------------
  // Static C callbacks — registered with llhttp, forward to the
  // corresponding uvcpp_http_parser instance via parser->data.
  // -------------------------------------------------------------------
  static int ll_on_message_begin(llhttp_t* p);
  static int ll_on_url(llhttp_t* p, const char* at, size_t len);
  static int ll_on_status(llhttp_t* p, const char* at, size_t len);
  static int ll_on_header_field(llhttp_t* p, const char* at, size_t len);
  static int ll_on_header_value(llhttp_t* p, const char* at, size_t len);
  static int ll_on_header_field_complete(llhttp_t* p);
  static int ll_on_header_value_complete(llhttp_t* p);
  static int ll_on_headers_complete(llhttp_t* p);
  static int ll_on_body(llhttp_t* p, const char* at, size_t len);
  static int ll_on_message_complete(llhttp_t* p);
  static int ll_on_chunk_header(llhttp_t* p);
  static int ll_on_chunk_complete(llhttp_t* p);
  static int ll_on_reset(llhttp_t* p);

  // -------------------------------------------------------------------
  // Internal helpers
  // -------------------------------------------------------------------

  /** @brief Bind all llhttp_settings callbacks to static trampolines. */
  void bind_settings();

  /** @brief Called from ll_on_headers_complete to extract method/status/version. */
  void extract_metadata();

  // -------------------------------------------------------------------
  // Member variables
  // -------------------------------------------------------------------

  http_parser_mode  mode_;
  http_parser_state state_ = http_parser_state::IDLE;

  // llhttp internal structures (PIMPL — opaque, defined in .cpp)
  llhttp_raw*          raw_parser_   = nullptr;
  llhttp_settings_raw* raw_settings_ = nullptr;

  // Accumulated parse results
  std::string  url_buf_;
  http_headers headers_;

  // 头部长度上限的账本（API 见上方 `size_limit` 那一节）
  size_t     max_url_bytes_    = 0;
  size_t     max_header_bytes_ = 0;
  size_t     header_bytes_     = 0;  ///< 本条消息累计的头部字节
  size_limit limit_hit_        = size_limit::NONE;

  /** @brief 头部字节超限就置 `limit_hit_`；返回非 0 表示"该停下来了"。 */
  int check_header_limit();

  /** @brief 记一个字段收尾的 `: ` + CRLF 四个字节，再查一次限。 */
  int note_header_done();

  /** @brief 一条消息开始时清账。`reset()` 与 `ll_on_message_begin` 都要调。 */
  void clear_size_limits() {
    header_bytes_ = 0;
    limit_hit_    = size_limit::NONE;
  }

  http_method  method_     = http_method::HTTP_GET;
  http_status  status_code_ = http_status::OK;
  uvcpp_http_version version_    = static_cast<uvcpp_http_version>(1);
  bool         keep_alive_ = true;
  uint64_t     content_length_ = 0;
  bool         upgrade_    = false;

  // Internal header accumulators
  std::string  cur_header_name_;
  std::string  cur_header_value_;

  // Last chunk size (for on_chunk_header callback)
  size_t last_chunk_size_ = 0;

  // Trampoline storage
  data_cb_t url_fn_   = nullptr;
  void*     url_arg_  = nullptr;

  data_cb_t status_fn_  = nullptr;
  void*     status_arg_ = nullptr;

  data_cb_t field_fn_  = nullptr;
  void*     field_arg_ = nullptr;

  data_cb_t value_fn_  = nullptr;
  void*     value_arg_ = nullptr;

  void_cb_t headers_done_fn_  = nullptr;
  void*     headers_done_arg_ = nullptr;

  data_cb_t body_fn_  = nullptr;
  void*     body_arg_ = nullptr;

  void_cb_t msg_done_fn_  = nullptr;
  void*     msg_done_arg_ = nullptr;

  void_cb_t msg_begin_fn_  = nullptr;
  void*     msg_begin_arg_ = nullptr;

  sz_cb_t   chunk_hdr_fn_  = nullptr;
  void*     chunk_hdr_arg_ = nullptr;

  void_cb_t chunk_done_fn_  = nullptr;
  void*     chunk_done_arg_ = nullptr;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_PARSER_H
