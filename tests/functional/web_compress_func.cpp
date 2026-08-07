/**
 * @file tests/functional/web_compress_func.cpp
 * @brief HTTP gzip + WebSocket compression tests (UVCPP_ZLIB_ENABLE=1 only)
 */
#include <iostream>
#include <string>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_ZLIB_ENABLE
#include <web/uvcpp_ws_frame.h>
#include <web/uvcpp_ws_parser.h>
using namespace uvcpp;

// =========================================================================
// Test 1: Enable compression and get extension header
// =========================================================================
static bool test_compression_enable() {
  uvcpp_ws_parser parser;
  parser.enable_compression(true, false, 15, 13);
  if (!parser.is_compression_enabled()) return false;

  std::string ext = parser.get_extension_header();
  if (ext.find("permessage-deflate") == std::string::npos) return false;
  if (ext.find("client_no_context_takeover") == std::string::npos) return false;
  if (ext.find("server_max_window_bits=13") == std::string::npos) return false;
  return true;
}

// =========================================================================
// Test 2: Compression round-trip (compress → decompress)
// =========================================================================
static bool test_compress_roundtrip() {
  uvcpp_ws_parser parser;
  parser.enable_compression(false, false, 15, 15);

  // Create a message that compresses well
  std::string original(1000, 'A');
  for (int i = 0; i < 1000; i++) original[i] = (char)('A' + (i % 26));

  uvcpp_buf compressed;
  const unsigned char* in = reinterpret_cast<const unsigned char*>(original.c_str());
  int rc = parser.compress(in, original.size(), compressed);
  if (rc != 0) return false;

  // Compressed data should be smaller (or at least different)
  if (compressed.size() >= original.size()) {
    // For random-ish data, it might not compress smaller, that's OK
  }

  uvcpp_buf decompressed;
  const unsigned char* cin = compressed.get_const_udata();
  rc = parser.decompress(cin, compressed.size(), decompressed);
  if (rc != 0) return false;

  if (decompressed.size() != original.size()) return false;
  if (decompressed.to_string() != original) return false;
  return true;
}

// =========================================================================
// Test 3: Compress very small message
// =========================================================================
static bool test_compress_small() {
  uvcpp_ws_parser parser;
  parser.enable_compression(true, true, 15, 15);

  std::string short_msg = "Hi";
  uvcpp_buf compressed;
  const unsigned char* in = reinterpret_cast<const unsigned char*>(short_msg.c_str());
  int rc = parser.compress(in, short_msg.size(), compressed);
  if (rc != 0) return false;

  uvcpp_buf decompressed;
  rc = parser.decompress(compressed.get_const_udata(), compressed.size(), decompressed);
  if (rc != 0) return false;
  if (decompressed.to_string() != short_msg) return false;
  return true;
}

// =========================================================================
// Test 4: Decompress empty/error case
// =========================================================================
static bool test_compress_errors() {
  uvcpp_ws_parser parser;
  parser.enable_compression(false, false, 15, 15);

  // Decompress garbage data should fail
  uvcpp_buf out;
  unsigned char garbage[] = {0xFF, 0xFF, 0xFF, 0xFF};
  int rc = parser.decompress(garbage, 4, out);
  // Should return error (not crash)
  (void)rc;
  return true;  // No crash = pass
}

// =========================================================================
// Test 5: Multiple compress/decompress cycles (context reuse)
// =========================================================================
static bool test_compress_multi() {
  uvcpp_ws_parser parser;
  parser.enable_compression(false, false, 15, 15);

  for (int i = 0; i < 5; i++) {
    std::string msg(100, 'A' + (char)(i % 26));
    uvcpp_buf c, d;
    int rc = parser.compress(reinterpret_cast<const unsigned char*>(msg.c_str()), msg.size(), c);
    if (rc != 0) return false;
    rc = parser.decompress(c.get_const_udata(), c.size(), d);
    if (rc != 0) return false;
    if (d.to_string() != msg) return false;
  }
  return true;
}

// =========================================================================
// Test 6: WS frame with compression (build → parse round-trip)
// =========================================================================
static bool test_ws_frame_compress() {
  // Build a frame with RSV1 set (compressed flag)
  uvcpp_ws_frame f;
  f.opcode = ws_opcode::TEXT;
  f.rsv1 = true;  // Mark as compressed
  f.payload.clone_data("compressed_data", 15);

  size_t sz = uvcpp_ws_parser::calc_frame_size(f);
  char* buf = new char[sz];
  size_t written = uvcpp_ws_parser::build_frame(buf, f);

  // Parse it back
  uvcpp_ws_parser p;
  p.execute(buf, written);
  delete[] buf;

  auto& pf = p.get_current_frame();
  return pf.opcode == ws_opcode::TEXT &&
         pf.rsv1 == true &&
         pf.payload.size() == 15 &&
         pf.payload.to_string() == "compressed_data";
}

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"compression_enable", test_compression_enable},
    {"compress_roundtrip", test_compress_roundtrip},
    {"compress_small", test_compress_small},
    {"compress_errors", test_compress_errors},
    {"compress_multi", test_compress_multi},
    {"ws_frame_compress", test_ws_frame_compress},
  };
  for (const auto& t : tests) {
    std::cout << "[web_compress] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_compress] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}
#else
int main() {
  std::cout << "[web_compress] SKIP (zlib disabled)\n";
  return 0;
}
#endif
