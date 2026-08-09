/**
 * @file src/web/uvcpp_http_response.cpp
 * @brief Implementation of uvcpp_http_response.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_http_response.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_http_parser.h>
#include <sstream>

namespace uvcpp {

// =========================================================================
// Construction
// =========================================================================

uvcpp_http_response::uvcpp_http_response() {}

uvcpp_http_response::~uvcpp_http_response() {}

uvcpp_http_response::uvcpp_http_response(const uvcpp_http_response& other)
    : version(other.version),
      status_code(other.status_code),
      status_message(other.status_message),
      headers(other.headers),
      body(other.body) {}

uvcpp_http_response& uvcpp_http_response::operator=(
    const uvcpp_http_response& other) {
  if (this != &other) {
    version        = other.version;
    status_code    = other.status_code;
    status_message = other.status_message;
    headers        = other.headers;
    body.clone(other.body);
  }
  return *this;
}

// =========================================================================
// Header operations
// =========================================================================

void uvcpp_http_response::set_header(const std::string& key,
                                      const std::string& value) {
  http_set_header(headers, key, value);
}

std::string uvcpp_http_response::get_header(const std::string& key,
                                             const std::string& default_val) const {
  return http_get_header(headers, key, default_val);
}

bool uvcpp_http_response::has_header(const std::string& key) const {
  return http_has_header(headers, key);
}

void uvcpp_http_response::remove_header(const std::string& key) {
  for (auto it = headers.begin(); it != headers.end(); ++it) {
    if (http_name_equal(it->name, key)) {
      headers.erase(it);
      return;
    }
  }
}

std::string uvcpp_http_response::content_type() const {
  return get_header("content-type");
}

void uvcpp_http_response::set_content_type(const std::string& ct) {
  set_header("content-type", ct);
}

// =========================================================================
// Serialization
// =========================================================================

std::string uvcpp_http_response::to_string() const {
  std::ostringstream oss;

  // --- Status line ---
  std::string reason = status_message.empty()
                           ? http_status_reason(status_code)
                           : status_message;
  oss << uvcpp_http_version_str(version) << " "
      << static_cast<int>(status_code) << " "
      << reason << "\r\n";

  // Detect chunked transfer encoding
  bool chunked = false;
  bool has_te = false;
  for (const auto& h : headers) {
    if (http_name_equal(h.name, "transfer-encoding") &&
        h.value.find("chunked") != std::string::npos) {
      chunked = true;
    }
  }

  // --- Headers ---
  bool has_cl = has_header("content-length");

  for (const auto& h : headers) {
    oss << h.name << ": " << h.value << "\r\n";
  }

  if (chunked) {
    // Chunked: no Content-Length (included)
  } else if (!has_cl && body.size() > 0) {
    oss << "content-length: " << body.size() << "\r\n";
  }

  // --- Blank line ---
  oss << "\r\n";

  // --- Body ---
  std::string result = oss.str();
  if (chunked && body.size() > 0) {
    std::ostringstream hex_oss;
    hex_oss << std::hex << body.size();
    result += hex_oss.str();
    result += "\r\n";
    result.append(body.get_const_data(), body.size());
    result += "\r\n";
    result += "0\r\n\r\n";
  } else if (body.size() > 0) {
    result.append(body.get_const_data(), body.size());
  }

  return result;
}

// =========================================================================
// from_parser
// =========================================================================

uvcpp_http_response uvcpp_http_response::from_parser(
    const uvcpp_http_parser& parser, const uvcpp_buf& body) {
  uvcpp_http_response resp;
  resp.version        = parser.get_uvcpp_http_version();
  resp.status_code    = parser.get_status_code();
  resp.status_message = http_status_reason(resp.status_code);
  resp.headers        = parser.get_headers();
  resp.body.clone(body);
  return resp;
}

// =========================================================================
// Factory helpers
// =========================================================================

uvcpp_http_response uvcpp_http_response::ok(const char* body_data, size_t len,
                                             const std::string& content_type) {
  uvcpp_http_response resp;
  resp.status_code    = http_status::OK;
  resp.status_message = "OK";
  if (!content_type.empty()) {
    resp.set_content_type(content_type);
  }
  if (body_data && len > 0) {
    resp.body.clone_data(body_data, len);
  }
  return resp;
}

uvcpp_http_response uvcpp_http_response::not_found(const char* body_data,
                                                     size_t len) {
  uvcpp_http_response resp;
  resp.status_code    = http_status::NOT_FOUND;
  resp.status_message = "Not Found";
  resp.set_content_type("text/plain");
  if (body_data && len > 0) {
    resp.body.clone_data(body_data, len);
  } else {
    const char* default_body = "404 Not Found";
    resp.body.clone_data(default_body, 13);
  }
  return resp;
}

uvcpp_http_response uvcpp_http_response::server_error(const char* body_data,
                                                       size_t len) {
  uvcpp_http_response resp;
  resp.status_code    = http_status::INTERNAL_SERVER_ERROR;
  resp.status_message = "Internal Server Error";
  resp.set_content_type("text/plain");
  if (body_data && len > 0) {
    resp.body.clone_data(body_data, len);
  } else {
    const char* default_body = "500 Internal Server Error";
    resp.body.clone_data(default_body, 25);
  }
  return resp;
}

uvcpp_http_response uvcpp_http_response::make(http_status code,
                                               const char* body_data, size_t len,
                                               const std::string& content_type) {
  uvcpp_http_response resp;
  resp.status_code    = code;
  resp.status_message = http_status_reason(code);
  if (!content_type.empty()) {
    resp.set_content_type(content_type);
  }
  if (body_data && len > 0) {
    resp.body.clone_data(body_data, len);
  }
  return resp;
}

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
