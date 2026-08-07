#include <iostream>
#include <cstring>
#include <string>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_parser.h>

using namespace uvcpp;

// =========================================================================
// Test 1: Response parsing via parser directly
// =========================================================================
static bool test_parser_only() {
  uvcpp_http_parser parser(http_parser_mode::PARSE_RESPONSE);
  const char* raw =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 5\r\n"
      "\r\n"
      "hello";
  parser.execute(raw, strlen(raw));
  parser.finish();
  return parser.is_complete() && !parser.has_error() &&
         parser.get_status_code() == http_status::OK &&
         parser.get_headers().size() >= 2;
}

// =========================================================================
// Test 2: Client construction and basic status
// =========================================================================
static bool test_client_basics() {
  uvcpp_http_client client;
  if (client.get_status() != HTTP_CLIENT_NONE) return false;

  // Sending without connecting should fail cleanly
  uvcpp_http_response resp;
  int rc = client.send_wait(uvcpp_http_request::make_get("/"), resp, 500);
  (void)rc;  // No crash is the test

  // Status checks
  if (client.has_status(HTTP_CLIENT_CONNECTED)) return false;

  return true;
}

// =========================================================================
// Test 3: Request round-trip (build → wire → parse)
// =========================================================================
static bool test_request_roundtrip() {
  uvcpp_http_request req = uvcpp_http_request::make_get("/api/test");
  req.set_header("x-custom", "value123");

  std::string wire = req.to_string();
  if (wire.find("GET /api/test HTTP/1.1") == std::string::npos) return false;
  if (wire.find("x-custom: value123") == std::string::npos) return false;

  // Parse it back
  uvcpp_http_parser parser(http_parser_mode::PARSE_REQUEST);
  parser.execute(wire.c_str(), wire.size());
  parser.finish();

  return parser.is_complete() && !parser.has_error() &&
         parser.get_method() == http_method::HTTP_GET &&
         parser.get_url() == "/api/test";
}

// =========================================================================
// Test 4: POST request construction
// =========================================================================
static bool test_post_request() {
  uvcpp_http_request req = uvcpp_http_request::make_post(
      "/api/data", "hello", 5, "text/plain");

  std::string wire = req.to_string();
  if (wire.find("POST /api/data HTTP/1.1") == std::string::npos) return false;
  if (wire.find("content-type: text/plain") == std::string::npos) return false;
  if (wire.find("hello") == std::string::npos) return false;

  return true;
}

// =========================================================================
// Test 5: Response object factory methods
// =========================================================================
static bool test_response_factory() {
  auto resp = uvcpp_http_response::ok("OK", 2);
  if (resp.status_code != http_status::OK) return false;
  if (resp.body.to_string() != "OK") return false;

  auto nf = uvcpp_http_response::not_found();
  if (nf.status_code != http_status::NOT_FOUND) return false;

  auto se = uvcpp_http_response::server_error();
  if (se.status_code != http_status::INTERNAL_SERVER_ERROR) return false;

  return true;
}

// =========================================================================
// main
// =========================================================================
int main() {
  bool ok = true;

  struct { const char* name; bool (*fn)(); } tests[] = {
    {"parser_only", test_parser_only},
    {"client_basics", test_client_basics},
    {"request_roundtrip", test_request_roundtrip},
    {"post_request", test_post_request},
    {"response_factory", test_response_factory},
  };

  for (const auto& t : tests) {
    std::cout << "[web_http_client] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }

  std::cout << "[web_http_client] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}

#else
int main() {
  std::cout << "[web_http_client] SKIP (web module disabled)\n";
  return 0;
}
#endif
