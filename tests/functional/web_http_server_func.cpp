#include <iostream>
#include <cstring>
#include <string>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE
#include <web/uvcpp_http_server.h>
#include <web/uvcpp_http_parser.h>
using namespace uvcpp;

// Test 1: Server construction, bind, listen, status
static bool test_server_lifecycle() {
  uvcpp_http_server server;
  if (server.get_status() != HTTP_SERVER_NONE) return false;
  server.bind("127.0.0.1", 20001);
  server.listen();
  if (!server.has_status(HTTP_SERVER_LISTENING)) return false;
  return true;
}

// Test 2: Route registration (no network)
static bool test_route_registration() {
  uvcpp_http_server server;
  server.bind("127.0.0.1", 20002);

  bool get_called = false;
  server.get("/hello", [&](uvcpp_http_request&, uvcpp_http_response& resp, uvcpp_tcp_client*) {
    get_called = true;
    resp = uvcpp_http_response::ok("hi", 2);
  });

  server.post("/data", [&](uvcpp_http_request&, uvcpp_http_response& resp, uvcpp_tcp_client*) {
    resp = uvcpp_http_response::ok("posted", 6);
  });

  // Routes are stored — can't easily test without real connection,
  // but at least verify no crash during registration
  (void)get_called;
  return true;
}

// Test 3: HTTP response serialization
static bool test_response_format() {
  auto resp = uvcpp_http_response::ok("body", 4);
  resp.set_header("x-test", "value");

  std::string wire = resp.to_string();
  if (wire.find("HTTP/1.1 200") == std::string::npos) return false;
  if (wire.find("content-length: 4") == std::string::npos) return false;
  if (wire.find("x-test: value") == std::string::npos) return false;
  if (wire.find("\r\n\r\nbody") == std::string::npos) return false;
  return true;
}

// Test 4: Status code responses
static bool test_status_codes() {
  auto ok = uvcpp_http_response::ok("ok", 2);
  if (ok.status_code != http_status::OK) return false;

  auto nf = uvcpp_http_response::not_found();
  if (nf.status_code != http_status::NOT_FOUND) return false;

  auto se = uvcpp_http_response::server_error();
  if (se.status_code != http_status::INTERNAL_SERVER_ERROR) return false;

  return true;
}

// =========================================================================
// Test 5: Chunked transfer encoding generation (server send path)
// =========================================================================
static bool test_chunked_response() {
  auto resp = uvcpp_http_response::ok("Hello World!", 12, "text/plain");
  resp.set_header("transfer-encoding", "chunked");

  std::string wire = resp.to_string();
  // Should contain Transfer-Encoding: chunked
  if (wire.find("transfer-encoding: chunked") == std::string::npos) return false;
  // Should NOT contain content-length (chunked replaces it)
  if (wire.find("content-length:") != std::string::npos) return false;
  // Should contain chunked body: hex-size, data, final chunk
  if (wire.find("\r\nc\r\nHello World!\r\n0\r\n\r\n") == std::string::npos) return false;

  // Verify it's still parseable
  uvcpp_http_parser parser(http_parser_mode::PARSE_RESPONSE);
  parser.execute(wire.c_str(), wire.size());
  parser.finish();
  if (!parser.is_complete() || parser.has_error()) return false;
  if (parser.get_status_code() != http_status::OK) return false;

  return true;
}

// =========================================================================
// Test 6: Server compression enabled by default (smoke test)
// =========================================================================
#if UVCPP_ZLIB_ENABLE
#include <web/uvcpp_http_compress.h>

static bool test_server_compress_enabled() {
  uvcpp_http_server server;
  // Default: compression enabled
  if (!server.is_compression_enabled()) return false;
  server.set_compression_enabled(false);
  if (server.is_compression_enabled()) return false;
  server.set_compression_enabled(true);
  if (!server.is_compression_enabled()) return false;
  return true;
}

// =========================================================================
// Test 6: Server MIME exclusion list
// =========================================================================
static bool test_server_mime_exclusion() {
  uvcpp_http_server server;
  server.add_compress_excluded_type("application/octet-stream");
  server.set_compress_min_body_size(512);

  // Compress should still work for text types
  auto& defaults = http_compress::default_excluded_mime_types();
  if (http_compress::should_compress("image/png", defaults)) return false;
  if (!http_compress::should_compress("text/html", defaults)) return false;

  return true;
}

// =========================================================================
// Test 7: Response header removal after compression check
// =========================================================================
static bool test_response_header_ops() {
  uvcpp_http_response resp = uvcpp_http_response::ok("test", 4, "text/plain");
  resp.set_header("content-encoding", "gzip");
  if (!resp.has_header("content-encoding")) return false;
  resp.remove_header("content-encoding");
  if (resp.has_header("content-encoding")) return false;

  // Verify response still serializes correctly after header removal
  std::string wire = resp.to_string();
  if (wire.find("HTTP/1.1 200") == std::string::npos) return false;
  if (wire.find("content-encoding") != std::string::npos) return false;
  // Content-Length should still be present (auto-inserted)
  if (wire.find("content-length") == std::string::npos) return false;

  return true;
}
#endif  // UVCPP_ZLIB_ENABLE

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"server_lifecycle", test_server_lifecycle},
    {"route_registration", test_route_registration},
    {"response_format", test_response_format},
    {"status_codes", test_status_codes},
    {"chunked_response", test_chunked_response},
#if UVCPP_ZLIB_ENABLE
    {"compress_enabled", test_server_compress_enabled},
    {"mime_exclusion", test_server_mime_exclusion},
    {"header_ops", test_response_header_ops},
#endif
  };
  for (const auto& t : tests) {
    std::cout << "[web_http_server] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_http_server] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}
#else
int main() { return 0; }
#endif
