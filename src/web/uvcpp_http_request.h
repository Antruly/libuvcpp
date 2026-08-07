/**
 * @file src/web/uvcpp_http_request.h
 * @brief HTTP request object — readable/writable, serializable.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Represents a complete HTTP request message.  Can be:
 * - Built manually and serialised via to_string() for sending
 * - Constructed from a uvcpp_http_parser result via from_parser()
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_REQUEST_H
#define SRC_WEB_UVCPP_HTTP_REQUEST_H

#if UVCPP_WEB_ENABLE

#include <string>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_common.h>

namespace uvcpp {

// Forward declaration
class uvcpp_http_parser;

class UVCPP_API uvcpp_http_request {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_http_request)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_http_request)

  // -------------------------------------------------------------------
  // Fields
  // -------------------------------------------------------------------

  http_method   method  = http_method::HTTP_GET;
  std::string   url     = "/";
  uvcpp_http_version  version = static_cast<uvcpp_http_version>(1);
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

  /** @brief Remove a header by name (case-insensitive). */
  void remove_header(const std::string& key);

  // -------------------------------------------------------------------
  // Content-Type shortcut
  // -------------------------------------------------------------------

  /** @brief Get Content-Type value (convenience). */
  std::string content_type() const;

  /** @brief Set Content-Type header. */
  void set_content_type(const std::string& ct);

  // -------------------------------------------------------------------
  // Serialization
  // -------------------------------------------------------------------

  /**
   * @brief Serialise to a complete HTTP request wire format.
   *
   * Produces: METHOD URL HTTP/version\r\n
   *           Header-Name: value\r\n
   *           ...
   *           \r\n
   *           body
   *
   * The Host header is automatically inserted (from headers or from
   * the URL host portion) if not already present for HTTP/1.1 requests.
   */
  std::string to_string() const;

  // -------------------------------------------------------------------
  // Construction from parser
  // -------------------------------------------------------------------

  /**
   * @brief Build an HTTP request from a completed parser and body data.
   * @param parser  A parser whose is_complete() returns true.
   * @param body    The accumulated body data (from body callbacks).
   * @return        A populated request object.
   */
  static uvcpp_http_request from_parser(const uvcpp_http_parser& parser,
                                         const uvcpp_buf& body);

  // -------------------------------------------------------------------
  // Convenience: build common requests
  // -------------------------------------------------------------------

  /** @brief Build a simple GET request. */
  static uvcpp_http_request make_get(const std::string& url);

  /** @brief Build a simple POST request with body. */
  static uvcpp_http_request make_post(const std::string& url,
                                       const char* body, size_t len,
                                       const std::string& content_type = "application/octet-stream");
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_REQUEST_H
