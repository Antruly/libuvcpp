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
// Test 6: Chunked transfer encoding parsing (client receive path)
// =========================================================================
static bool test_chunked_parse() {
  uvcpp_http_parser parser(http_parser_mode::PARSE_RESPONSE);
  const char* raw =
      "HTTP/1.1 200 OK\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Content-Type: text/plain\r\n"
      "\r\n"
      "5\r\n"
      "Hello\r\n"
      "7\r\n"
      " World!\r\n"
      "0\r\n"
      "\r\n";
  parser.execute(raw, strlen(raw));
  parser.finish();
  if (!parser.is_complete() || parser.has_error()) return false;
  if (parser.get_status_code() != http_status::OK) return false;
  // llhttp decodes chunked body — on_body callbacks receive decoded data
  // The parser headers should NOT include transfer-encoding as it's handled internally
  return true;
}

// =========================================================================
// Test 7: HTTP compress/decompress roundtrip (gzip)
// =========================================================================
#if UVCPP_ZLIB_ENABLE
#include <web/uvcpp_http_compress.h>

static bool test_compress_gzip_roundtrip() {
  const char* plain = "Hello HTTP compression! This is some text to compress. "
                      "It should be significantly smaller after gzip encoding.";
  size_t len = strlen(plain);

  auto cresult = http_compress::compress(plain, len, http_compress_method::GZIP);
  if (!cresult.success) return false;
  if (cresult.data.size() == 0) return false;

  // Compressed data should be different from original
  std::string compressed(cresult.data.get_const_data(), cresult.data.size());
  if (compressed == std::string(plain, len)) return false;

  auto dresult = http_compress::decompress(cresult.data.get_const_data(), cresult.data.size());
  if (!dresult.success) return false;
  std::string decompressed(dresult.data.get_const_data(), dresult.data.size());
  if (decompressed != std::string(plain, len)) return false;

  return true;
}

// =========================================================================
// Test 7: HTTP compress/decompress roundtrip (deflate)
// =========================================================================
static bool test_compress_deflate_roundtrip() {
  const char* plain = "This is a test of deflate compression for HTTP bodies.";
  size_t len = strlen(plain);

  auto cresult = http_compress::compress(plain, len, http_compress_method::DEFLATE);
  if (!cresult.success) return false;
  if (cresult.data.size() == 0) return false;

  auto dresult = http_compress::decompress(cresult.data.get_const_data(), cresult.data.size());
  if (!dresult.success) return false;
  std::string decompressed2(dresult.data.get_const_data(), dresult.data.size());
  if (decompressed2 != std::string(plain, len)) return false;

  return true;
}

// =========================================================================
// Test 8: Accept-Encoding parsing
// =========================================================================
static bool test_accept_encoding_parse() {
  // Basic
  if (http_compress::parse_accept_encoding("gzip, deflate") != http_compress_method::GZIP)
    return false;
  if (http_compress::parse_accept_encoding("deflate") != http_compress_method::DEFLATE)
    return false;
  // deflate first → gzip should still win (higher priority)
  if (http_compress::parse_accept_encoding("deflate, gzip") != http_compress_method::GZIP)
    return false;
  // Only gzip
  if (http_compress::parse_accept_encoding("gzip") != http_compress_method::GZIP)
    return false;
  // None supported
  if (http_compress::parse_accept_encoding("identity") != http_compress_method::NONE)
    return false;
  if (http_compress::parse_accept_encoding("") != http_compress_method::NONE)
    return false;
  // x- variants
  if (http_compress::parse_accept_encoding("x-gzip") != http_compress_method::GZIP)
    return false;
  // Wildcard → gzip
  if (http_compress::parse_accept_encoding("*") != http_compress_method::GZIP)
    return false;
  // q=0 should skip
  if (http_compress::parse_accept_encoding("gzip;q=0, deflate") != http_compress_method::DEFLATE)
    return false;
  return true;
}

// =========================================================================
// Test 9: MIME type exclusion
// =========================================================================
static bool test_mime_exclusion() {
  const auto& excl = http_compress::default_excluded_mime_types();

  // Image types should be excluded (prefix match "image/")
  if (http_compress::should_compress("image/png", excl)) return false;
  if (http_compress::should_compress("image/jpeg", excl)) return false;
  if (http_compress::should_compress("image/gif", excl)) return false;

  // Video/audio
  if (http_compress::should_compress("video/mp4", excl)) return false;
  if (http_compress::should_compress("audio/mpeg", excl)) return false;

  // Application types
  if (http_compress::should_compress("application/zip", excl)) return false;
  if (http_compress::should_compress("application/pdf", excl)) return false;
  if (http_compress::should_compress("application/octet-stream", excl)) return false;

  // Text types SHOULD be compressed
  if (!http_compress::should_compress("text/html", excl)) return false;
  if (!http_compress::should_compress("text/plain", excl)) return false;
  if (!http_compress::should_compress("application/json", excl)) return false;
  if (!http_compress::should_compress("text/css", excl)) return false;

  return true;
}
#endif  // UVCPP_ZLIB_ENABLE

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
    {"chunked_parse", test_chunked_parse},
#if UVCPP_ZLIB_ENABLE
    {"compress_gzip", test_compress_gzip_roundtrip},
    {"compress_deflate", test_compress_deflate_roundtrip},
    {"accept_encoding", test_accept_encoding_parse},
    {"mime_exclusion", test_mime_exclusion},
#endif
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
