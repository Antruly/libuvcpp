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
   */
  std::string to_string() const;

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
