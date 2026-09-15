/**
 * @file src/web/uvcpp_http_response.h
 * @brief HTTP response object — readable/writable, serializable.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Represents a complete HTTP response message.  Can be:
 * - Built manually and serialised via to_string() for sending back
 *   from a server
 * - Constructed from a uvcpp_http_parser result via from_parser()
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_RESPONSE_H
#define SRC_WEB_UVCPP_HTTP_RESPONSE_H

#if UVCPP_WEB_ENABLE

#include <string>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_common.h>

namespace uvcpp {

class uvcpp_http_parser;

class UVCPP_API uvcpp_http_response {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_http_response)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_http_response)

  // -------------------------------------------------------------------
  // Fields
  // -------------------------------------------------------------------

  uvcpp_http_version  version        = static_cast<uvcpp_http_version>(1);
  http_status   status_code    = http_status::OK;
  std::string   status_message = "OK";
  http_headers  headers;
  uvcpp_buf     body;

  /// When true, the server does NOT send this response immediately after the
  /// handler returns; the handler must call uvcpp_http_server::send_response
  /// later (on the loop thread, e.g. from an async completion callback).
  /// Used for deferred/streaming responses (large files, slow async work).
  bool          deferred = false;

  // -------------------------------------------------------------------
  // Header operations
  // -------------------------------------------------------------------

  /** @brief Set a header (case-insensitive overwrite). */
  void set_header(const std::string& key, const std::string& value);

  /** @brief Get a header value by name (case-insensitive). */
  std::string get_header(const std::string& key,
                         const std::string& default_val = "") const;

  /** @brief Check if a header exists (case-insensitive). */
  bool has_header(const std::string& key) const;

  /** @brief Remove a header by name (case-insensitive). */
  void remove_header(const std::string& key);

  // -------------------------------------------------------------------
  // Content-Type shortcut
  // -------------------------------------------------------------------

  std::string content_type() const;
  void set_content_type(const std::string& ct);

  // -------------------------------------------------------------------
  // Serialization
  // -------------------------------------------------------------------

  /**
   * @brief Serialise to wire format.
   *
   * Produces: HTTP/version STATUS_CODE REASON\r\n
   *           Header-Name: value\r\n
   *           ...
   *           \r\n
   *           body
   *
   * @param include_body 为 false 时**只**输出头部（状态行 + 各头 + 那个空行），
   *        body 一个字节都不写。HEAD 响应与流式响应都走这一支 —— 前者的 body
   *        按协议就不该发，后者的 body 由调用方随后逐块写。
   *
   *        **chunked 下这一支连终止块 `0\r\n\r\n` 也不发**。这不是"少发了一
   *        段"，而是这一支的定义：它只序列化头部。流式响应结束时由调用方自己
   *        发终止块（见 `uvcpp_http_server::end_stream`）。
   */
  std::string to_string(bool include_body = true) const;

  // -------------------------------------------------------------------
  // Construction from parser
  // -------------------------------------------------------------------

  static uvcpp_http_response from_parser(const uvcpp_http_parser& parser,
                                          const uvcpp_buf& body);

  // -------------------------------------------------------------------
  // Quick-response factories (for server use)
  // -------------------------------------------------------------------

  /** @brief Build a minimal 200 OK response with body. */
  static uvcpp_http_response ok(const char* body, size_t len,
                                 const std::string& content_type = "text/plain");

  /** @brief Build a 404 Not Found response. */
  static uvcpp_http_response not_found(const char* body = nullptr, size_t len = 0);

  /** @brief Build a 500 Internal Server Error response. */
  static uvcpp_http_response server_error(const char* body = nullptr, size_t len = 0);

  /** @brief Build a response with a given status code and optional body. */
  static uvcpp_http_response make(http_status code,
                                   const char* body = nullptr, size_t len = 0,
                                   const std::string& content_type = "text/plain");
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_RESPONSE_H
