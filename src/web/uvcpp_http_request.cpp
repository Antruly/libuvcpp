/**
 * @file src/web/uvcpp_http_request.cpp
 * @brief Implementation of uvcpp_http_request.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_http_request.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_http_parser.h>
#include <sstream>

namespace uvcpp {

// =========================================================================
// Construction
// =========================================================================

uvcpp_http_request::uvcpp_http_request() {}

uvcpp_http_request::~uvcpp_http_request() {}

uvcpp_http_request::uvcpp_http_request(const uvcpp_http_request& other)
    : method(other.method),
      url(other.url),
      version(other.version),
      headers(other.headers),
      body(other.body) {}

uvcpp_http_request& uvcpp_http_request::operator=(const uvcpp_http_request& other) {
  if (this != &other) {
    method  = other.method;
    url     = other.url;
    version = other.version;
    headers = other.headers;
    body.clone(other.body);
  }
  return *this;
}

// =========================================================================
// Header operations
// =========================================================================

void uvcpp_http_request::set_header(const std::string& key,
                                     const std::string& value) {
  http_set_header(headers, key, value);
}

std::string uvcpp_http_request::get_header(const std::string& key,
                                            const std::string& default_val) const {
  return http_get_header(headers, key, default_val);
}

bool uvcpp_http_request::has_header(const std::string& key) const {
  return http_has_header(headers, key);
}

void uvcpp_http_request::remove_header(const std::string& key) {
  for (auto it = headers.begin(); it != headers.end(); ++it) {
    if (http_name_equal(it->name, key)) {
      headers.erase(it);
      return;
    }
  }
}

std::string uvcpp_http_request::content_type() const {
  return get_header("content-type");
}

void uvcpp_http_request::set_content_type(const std::string& ct) {
  set_header("content-type", ct);
}

// =========================================================================
// Serialization
// =========================================================================

std::string uvcpp_http_request::to_string() const {
  std::ostringstream oss;

  // --- Request line ---
  oss << http_method_str(method) << " "
      << url << " "
      << uvcpp_http_version_str(version) << "\r\n";

  // --- Headers ---
  // Ensure Content-Length matches body
  bool has_host = has_header("host");
  bool has_cl   = has_header("content-length");

  for (const auto& h : headers) {
    oss << h.name << ": " << h.value << "\r\n";
  }

  // Add Host if missing (required for HTTP/1.1)
  if (!has_host) {
    // Try to extract host from URL
    std::string url_str = url;
    std::string host = "localhost";
    if (url_str.size() > 7 &&
        (url_str.compare(0, 7, "http://") == 0)) {
      url_str = url_str.substr(7);
      size_t slash = url_str.find('/');
      size_t colon = url_str.find(':');
      if (colon < slash || slash == std::string::npos) {
        host = url_str.substr(0, colon != std::string::npos ? colon : slash);
      } else {
        host = url_str.substr(0, slash);
      }
    }
    oss << "host: " << host << "\r\n";
  }

  // Add Content-Length if not present and we have a body
  if (!has_cl && body.size() > 0) {
    oss << "content-length: " << body.size() << "\r\n";
  }

  // Add Connection header for HTTP/1.1 default
  if (!has_header("connection")) {
    oss << "connection: keep-alive\r\n";
  }

  // --- Blank line separator ---
  oss << "\r\n";

  // --- Body ---
  std::string result = oss.str();
  if (body.size() > 0) {
    result.append(body.get_const_data(), body.size());
  }

  return result;
}

// =========================================================================
// from_parser
// =========================================================================

uvcpp_http_request uvcpp_http_request::from_parser(
    const uvcpp_http_parser& parser, const uvcpp_buf& body) {
  uvcpp_http_request req;
  req.method  = parser.get_method();
  req.url     = parser.get_url();
  req.version = parser.get_uvcpp_http_version();
  req.headers = parser.get_headers();
  req.body.clone(body);
  return req;
}

// =========================================================================
// Convenience factories
// =========================================================================

uvcpp_http_request uvcpp_http_request::make_get(const std::string& url) {
  uvcpp_http_request req;
  req.method  = http_method::HTTP_GET;
  req.url     = url;
  req.version = static_cast<uvcpp_http_version>(1);
  req.set_header("accept", "*/*");
  return req;
}

uvcpp_http_request uvcpp_http_request::make_post(const std::string& url,
                                                   const char* body_data,
                                                   size_t len,
                                                   const std::string& content_type) {
  uvcpp_http_request req;
  req.method  = http_method::HTTP_POST;
  req.url     = url;
  req.version = static_cast<uvcpp_http_version>(1);
  req.set_content_type(content_type);
  if (body_data && len > 0) {
    req.body.clone_data(body_data, len);
  }
  return req;
}

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
