/**
 * @file src/web/uvcpp_http_common.h
 * @brief HTTP common types: methods, status codes, versions, headers.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Defines the fundamental HTTP types used throughout the web module.
 * The http_method values match llhttp's internal numbering so that the
 * uvcpp_http_parser can map directly without translation.
 *
 * uvcpp_http_version pre-defines HTTP_2_0 as a placeholder for future implementation.
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_COMMON_H
#define SRC_WEB_UVCPP_HTTP_COMMON_H

#if UVCPP_WEB_ENABLE

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace uvcpp {

// =========================================================================
// HTTP 方法 — 值与 llhttp 内部编号一致，便于直接映射
// =========================================================================

enum class http_method : uint8_t {
  HTTP_DELETE      = 0,
  HTTP_GET         = 1,
  HTTP_HEAD        = 2,
  HTTP_POST        = 3,
  HTTP_PUT         = 4,
  HTTP_CONNECT     = 5,
  HTTP_OPTIONS     = 6,
  HTTP_TRACE       = 7,
  HTTP_COPY        = 8,
  HTTP_LOCK        = 9,
  HTTP_MKCOL       = 10,
  HTTP_MOVE        = 11,
  HTTP_PROPFIND    = 12,
  HTTP_PROPPATCH   = 13,
  HTTP_SEARCH      = 14,
  HTTP_UNLOCK      = 15,
  HTTP_BIND        = 16,
  HTTP_REBIND      = 17,
  HTTP_UNBIND      = 18,
  HTTP_ACL         = 19,
  HTTP_REPORT      = 20,
  HTTP_MKACTIVITY  = 21,
  HTTP_CHECKOUT    = 22,
  HTTP_MERGE       = 23,
  HTTP_MSEARCH     = 24,
  HTTP_NOTIFY      = 25,
  HTTP_SUBSCRIBE   = 26,
  HTTP_UNSUBSCRIBE = 27,
  HTTP_PATCH       = 28,
  HTTP_PURGE       = 29,
  HTTP_MKCALENDAR  = 30,
  HTTP_LINK        = 31,
  HTTP_UNLINK      = 32,
  HTTP_SOURCE      = 33,
  HTTP_PRI         = 34,  ///< HTTP/2 Priority (reserved for future use)
};

/** @brief Convert http_method to its standard string representation. */
inline const char* http_method_str(http_method m) {
  switch (m) {
    case http_method::HTTP_DELETE:      return "DELETE";
    case http_method::HTTP_GET:         return "GET";
    case http_method::HTTP_HEAD:        return "HEAD";
    case http_method::HTTP_POST:        return "POST";
    case http_method::HTTP_PUT:         return "PUT";
    case http_method::HTTP_CONNECT:     return "CONNECT";
    case http_method::HTTP_OPTIONS:     return "OPTIONS";
    case http_method::HTTP_TRACE:       return "TRACE";
    case http_method::HTTP_COPY:        return "COPY";
    case http_method::HTTP_LOCK:        return "LOCK";
    case http_method::HTTP_MKCOL:       return "MKCOL";
    case http_method::HTTP_MOVE:        return "MOVE";
    case http_method::HTTP_PROPFIND:    return "PROPFIND";
    case http_method::HTTP_PROPPATCH:   return "PROPPATCH";
    case http_method::HTTP_SEARCH:      return "SEARCH";
    case http_method::HTTP_UNLOCK:      return "UNLOCK";
    case http_method::HTTP_BIND:        return "BIND";
    case http_method::HTTP_REBIND:      return "REBIND";
    case http_method::HTTP_UNBIND:      return "UNBIND";
    case http_method::HTTP_ACL:         return "ACL";
    case http_method::HTTP_REPORT:      return "REPORT";
    case http_method::HTTP_MKACTIVITY:  return "MKACTIVITY";
    case http_method::HTTP_CHECKOUT:    return "CHECKOUT";
    case http_method::HTTP_MERGE:       return "MERGE";
    case http_method::HTTP_MSEARCH:     return "MSEARCH";
    case http_method::HTTP_NOTIFY:      return "NOTIFY";
    case http_method::HTTP_SUBSCRIBE:   return "SUBSCRIBE";
    case http_method::HTTP_UNSUBSCRIBE: return "UNSUBSCRIBE";
    case http_method::HTTP_PATCH:       return "PATCH";
    case http_method::HTTP_PURGE:       return "PURGE";
    case http_method::HTTP_MKCALENDAR:  return "MKCALENDAR";
    case http_method::HTTP_LINK:        return "LINK";
    case http_method::HTTP_UNLINK:      return "UNLINK";
    case http_method::HTTP_SOURCE:      return "SOURCE";
    case http_method::HTTP_PRI:         return "PRI";
    default:                            return "UNKNOWN";
  }
}

// =========================================================================
// HTTP 状态码 — RFC 7231 等规范定义的常用状态码
// =========================================================================

enum class http_status : int {
  // 1xx Informational
  CONTINUE                        = 100,
  SWITCHING_PROTOCOLS             = 101,
  PROCESSING                      = 102,
  EARLY_HINTS                     = 103,

  // 2xx Success
  OK                              = 200,
  CREATED                         = 201,
  ACCEPTED                        = 202,
  NON_AUTHORITATIVE_INFORMATION   = 203,
  NO_CONTENT                      = 204,
  RESET_CONTENT                   = 205,
  PARTIAL_CONTENT                 = 206,

  // 3xx Redirection
  MULTIPLE_CHOICES                = 300,
  MOVED_PERMANENTLY               = 301,
  FOUND                           = 302,
  SEE_OTHER                       = 303,
  NOT_MODIFIED                    = 304,
  USE_PROXY                       = 305,
  TEMPORARY_REDIRECT              = 307,
  PERMANENT_REDIRECT              = 308,

  // 4xx Client Error
  BAD_REQUEST                     = 400,
  UNAUTHORIZED                    = 401,
  PAYMENT_REQUIRED                = 402,
  FORBIDDEN                       = 403,
  NOT_FOUND                       = 404,
  METHOD_NOT_ALLOWED              = 405,
  NOT_ACCEPTABLE                  = 406,
  PROXY_AUTHENTICATION_REQUIRED   = 407,
  REQUEST_TIMEOUT                 = 408,
  CONFLICT                        = 409,
  GONE                            = 410,
  LENGTH_REQUIRED                 = 411,
  PRECONDITION_FAILED             = 412,
  PAYLOAD_TOO_LARGE               = 413,
  URI_TOO_LONG                    = 414,
  UNSUPPORTED_MEDIA_TYPE          = 415,
  RANGE_NOT_SATISFIABLE           = 416,
  EXPECTATION_FAILED              = 417,
  IM_A_TEAPOT                     = 418,
  MISDIRECTED_REQUEST             = 421,
  UNPROCESSABLE_ENTITY            = 422,
  UPGRADE_REQUIRED                = 426,
  TOO_MANY_REQUESTS               = 429,
  REQUEST_HEADER_FIELDS_TOO_LARGE = 431,

  // 5xx Server Error
  INTERNAL_SERVER_ERROR           = 500,
  NOT_IMPLEMENTED                 = 501,
  BAD_GATEWAY                     = 502,
  SERVICE_UNAVAILABLE             = 503,
  GATEWAY_TIMEOUT                 = 504,
  HTTP_VERSION_NOT_SUPPORTED      = 505,
};

/** @brief Return the standard reason phrase for an HTTP status code. */
inline const char* http_status_reason(http_status s) {
  switch (s) {
    case http_status::CONTINUE:                        return "Continue";
    case http_status::SWITCHING_PROTOCOLS:             return "Switching Protocols";
    case http_status::PROCESSING:                      return "Processing";
    case http_status::EARLY_HINTS:                     return "Early Hints";
    case http_status::OK:                              return "OK";
    case http_status::CREATED:                         return "Created";
    case http_status::ACCEPTED:                        return "Accepted";
    case http_status::NON_AUTHORITATIVE_INFORMATION:   return "Non-Authoritative Information";
    case http_status::NO_CONTENT:                      return "No Content";
    case http_status::RESET_CONTENT:                   return "Reset Content";
    case http_status::PARTIAL_CONTENT:                 return "Partial Content";
    case http_status::MULTIPLE_CHOICES:                return "Multiple Choices";
    case http_status::MOVED_PERMANENTLY:               return "Moved Permanently";
    case http_status::FOUND:                           return "Found";
    case http_status::SEE_OTHER:                       return "See Other";
    case http_status::NOT_MODIFIED:                    return "Not Modified";
    case http_status::USE_PROXY:                       return "Use Proxy";
    case http_status::TEMPORARY_REDIRECT:              return "Temporary Redirect";
    case http_status::PERMANENT_REDIRECT:              return "Permanent Redirect";
    case http_status::BAD_REQUEST:                     return "Bad Request";
    case http_status::UNAUTHORIZED:                    return "Unauthorized";
    case http_status::PAYMENT_REQUIRED:                return "Payment Required";
    case http_status::FORBIDDEN:                       return "Forbidden";
    case http_status::NOT_FOUND:                       return "Not Found";
    case http_status::METHOD_NOT_ALLOWED:              return "Method Not Allowed";
    case http_status::NOT_ACCEPTABLE:                  return "Not Acceptable";
    case http_status::PROXY_AUTHENTICATION_REQUIRED:   return "Proxy Authentication Required";
    case http_status::REQUEST_TIMEOUT:                 return "Request Timeout";
    case http_status::CONFLICT:                        return "Conflict";
    case http_status::GONE:                            return "Gone";
    case http_status::LENGTH_REQUIRED:                 return "Length Required";
    case http_status::PRECONDITION_FAILED:             return "Precondition Failed";
    case http_status::PAYLOAD_TOO_LARGE:               return "Payload Too Large";
    case http_status::URI_TOO_LONG:                    return "URI Too Long";
    case http_status::UNSUPPORTED_MEDIA_TYPE:          return "Unsupported Media Type";
    case http_status::RANGE_NOT_SATISFIABLE:           return "Range Not Satisfiable";
    case http_status::EXPECTATION_FAILED:              return "Expectation Failed";
    case http_status::IM_A_TEAPOT:                     return "I'm a teapot";
    case http_status::MISDIRECTED_REQUEST:             return "Misdirected Request";
    case http_status::UNPROCESSABLE_ENTITY:            return "Unprocessable Entity";
    case http_status::UPGRADE_REQUIRED:                return "Upgrade Required";
    case http_status::TOO_MANY_REQUESTS:               return "Too Many Requests";
    case http_status::REQUEST_HEADER_FIELDS_TOO_LARGE: return "Request Header Fields Too Large";
    case http_status::INTERNAL_SERVER_ERROR:           return "Internal Server Error";
    case http_status::NOT_IMPLEMENTED:                 return "Not Implemented";
    case http_status::BAD_GATEWAY:                     return "Bad Gateway";
    case http_status::SERVICE_UNAVAILABLE:             return "Service Unavailable";
    case http_status::GATEWAY_TIMEOUT:                 return "Gateway Timeout";
    case http_status::HTTP_VERSION_NOT_SUPPORTED:      return "HTTP Version Not Supported";
    default:                                           return "Unknown";
  }
}

// =========================================================================
// HTTP 版本 — 预埋 HTTP/2 枚举值，当前只实现 1.0 和 1.1
//
// NOTE: Values use "HV_" prefix to avoid collision with Windows SDK
// macros (HTTP_1_1, HTTP_VERSION_1_1, etc.)
// =========================================================================

enum class uvcpp_http_version : uint8_t {
  HVER_10 = 0,  // HTTP/1.0 (RFC 1945)
  HVER_11 = 1,  // HTTP/1.1 (RFC 7230-7235)
  HVER_20 = 2,  // HTTP/2   (RFC 7540/9113) — reserved for future
};

/** @brief Return the protocol string for an HTTP version. */
inline const char* uvcpp_http_version_str(uvcpp_http_version v) {
  switch (v) {
    case uvcpp_http_version::HVER_10: return "HTTP/1.0";
    case uvcpp_http_version::HVER_11: return "HTTP/1.1";
    case uvcpp_http_version::HVER_20: return "HTTP/2.0";
    default:                          return "HTTP/1.1";
  }
}

// =========================================================================
// HTTP 头类型
// =========================================================================

/**
 * @brief A single HTTP header (key-value pair).
 *
 * The name is stored in lower-case for efficient case-insensitive lookup.
 */
struct http_header {
  std::string name;   ///< Header field name (lower-case)
  std::string value;  ///< Header field value
};

/** @brief Ordered list of HTTP headers. */
using http_headers = std::vector<http_header>;

// =========================================================================
// Header 便捷函数
// =========================================================================

/**
 * @brief Case-insensitive string equality for header names.
 *        RFC 7230 Section 3.2: header field names are case-insensitive.
 */
inline bool http_name_equal(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

/**
 * @brief Find a header value by name (case-insensitive).
 * @param hdrs  Header list to search.
 * @param name  Header name to find (case-insensitive).
 * @param def   Default value if not found.
 * @return      The header value, or @p def if not present.
 */
inline std::string http_get_header(const http_headers& hdrs,
                                    const std::string& name,
                                    const std::string& def = "") {
  for (const auto& h : hdrs) {
    if (http_name_equal(h.name, name)) return h.value;
  }
  return def;
}

/**
 * @brief Check whether a header exists (case-insensitive).
 */
inline bool http_has_header(const http_headers& hdrs, const std::string& name) {
  for (const auto& h : hdrs) {
    if (http_name_equal(h.name, name)) return true;
  }
  return false;
}

/**
 * @brief Set or overwrite a header (case-insensitive match on name).
 */
inline void http_set_header(http_headers& hdrs, const std::string& name,
                             const std::string& value) {
  for (auto& h : hdrs) {
    if (http_name_equal(h.name, name)) {
      h.value = value;
      return;
    }
  }
  hdrs.push_back({name, value});
}

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_COMMON_H
