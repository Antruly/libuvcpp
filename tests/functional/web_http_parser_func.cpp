#include <iostream>
#include <string>
#include <cstring>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_parser.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

using namespace uvcpp;

static bool test_parse_request_line() {
  uvcpp_http_parser parser(http_parser_mode::PARSE_REQUEST);
  const char* raw =
      "GET /index.html HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Accept: */*\r\n"
      "\r\n";
  parser.execute(raw, strlen(raw));
  parser.finish();
  if (!parser.is_complete() || parser.has_error()) return false;
  if (parser.get_method() != http_method::HTTP_GET) return false;
  if (parser.get_url() != "/index.html") return false;
  if (parser.get_uvcpp_http_version() != uvcpp_http_version::HVER_11) return false;
  if (parser.get_headers().size() < 2) return false;
  return true;
}

static bool test_parse_response_line() {
  uvcpp_http_parser parser(http_parser_mode::PARSE_RESPONSE);
  const char* raw =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 5\r\n"
      "\r\n"
      "Hello";
  parser.execute(raw, strlen(raw));
  parser.finish();
  if (!parser.is_complete() || parser.has_error()) return false;
  if (parser.get_status_code() != http_status::OK) return false;
  return true;
}

static bool test_request_to_string() {
  uvcpp_http_request req;
  req.method = http_method::HTTP_GET;
  req.url = "/api/test";
  req.set_header("accept", "application/json");
  std::string s = req.to_string();
  if (s.find("GET /api/test HTTP/1.1") == std::string::npos) return false;
  if (s.find("accept: application/json") == std::string::npos) return false;
  return true;
}

static bool test_response_to_string() {
  uvcpp_http_response resp;
  resp.status_code = http_status::NOT_FOUND;
  resp.set_header("content-type", "text/plain");
  resp.body.clone_data("Not Found", 9);
  std::string s = resp.to_string();
  if (s.find("HTTP/1.1 404") == std::string::npos) return false;
  if (s.find("Not Found") == std::string::npos) return false;
  return true;
}

static bool test_version_enum() {
  if (static_cast<uint8_t>(uvcpp_http_version::HVER_10) != 0) return false;
  if (static_cast<uint8_t>(uvcpp_http_version::HVER_11) != 1) return false;
  if (static_cast<uint8_t>(uvcpp_http_version::HVER_20) != 2) return false;
  if (std::string(uvcpp_http_version_str(uvcpp_http_version::HVER_10)) != "HTTP/1.0") return false;
  if (std::string(uvcpp_http_version_str(uvcpp_http_version::HVER_11)) != "HTTP/1.1") return false;
  return true;
}

static bool test_header_ops() {
  http_headers hdrs;
  http_set_header(hdrs, "Content-Type", "text/html");
  http_set_header(hdrs, "X-Custom", "value");
  if (http_get_header(hdrs, "content-type") != "text/html") return false;
  if (!http_has_header(hdrs, "CONTENT-TYPE")) return false;
  if (http_has_header(hdrs, "not-exists")) return false;
  return true;
}

static bool test_parser_reset() {
  uvcpp_http_parser parser(http_parser_mode::PARSE_REQUEST);
  const char* req1 = "GET /first HTTP/1.1\r\n\r\n";
  parser.execute(req1, strlen(req1));
  parser.finish();
  if (!parser.is_complete()) return false;
  if (parser.get_url() != "/first") return false;
  parser.reset();
  const char* req2 = "POST /second HTTP/1.1\r\n\r\n";
  parser.execute(req2, strlen(req2));
  parser.finish();
  if (!parser.is_complete()) return false;
  if (parser.get_method() != http_method::HTTP_POST) return false;
  if (parser.get_url() != "/second") return false;
  return true;
}

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"parse_request_line", test_parse_request_line},
    {"parse_response_line", test_parse_response_line},
    {"request_to_string", test_request_to_string},
    {"response_to_string", test_response_to_string},
    {"version_enum", test_version_enum},
    {"header_ops", test_header_ops},
    {"parser_reset", test_parser_reset},
  };
  for (const auto& t : tests) {
    std::cout << "[web_http_parser] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_http_parser] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}

#else  // UVCPP_WEB_ENABLE

int main() {
  std::cout << "[web_http_parser] SKIP (web module disabled)\n";
  return 0;
}

#endif  // UVCPP_WEB_ENABLE
