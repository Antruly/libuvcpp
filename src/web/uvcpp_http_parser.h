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

  /** @brief Called at the start of each chunk (parser->content_length
   *         holds the chunk size in bytes). */
  void set_on_chunk_header(std::function<void(size_t)> cb);

  /** @brief Called when a chunk's data has been fully received. */
  void set_on_chunk_complete(std::function<void()> cb);

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

  sz_cb_t   chunk_hdr_fn_  = nullptr;
  void*     chunk_hdr_arg_ = nullptr;

  void_cb_t chunk_done_fn_  = nullptr;
  void*     chunk_done_arg_ = nullptr;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_PARSER_H
