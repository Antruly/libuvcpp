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
// Test 7b: 解压后**跨过内部输出块边界**（> 4 KiB）的往返
// =========================================================================
//
// 上面两条的原文都只有几十字节，一次 `inflate()` 就 `Z_STREAM_END` —— 于是
// 那个 4 KiB 的输出块**从来没被填满过**，`inflate()` 的参数写错也测不出来。
// 实测（zlib 1.3，本用例就是量具）：输出超过 4 KiB 时 `inflate(…, Z_FINISH)`
// 在**第一块**填满时就返回 `Z_BUF_ERROR`(-5) 而不是 `Z_OK`，于是
// `while (ret == Z_OK)` 立刻退出、`ret != Z_STREAM_END` ⇒ **整条解压被判成失败**。
// 症状是"解压后超过 4 KiB 的响应一律解不开"，而 4 KiB 以内全好 —— 客户端那条
// 路上，解压失败是**静默**的（`uvcpp_http_client.cpp` 只在 `result.success` 时才
// 替换 body），调用方拿到的是还带着 `content-encoding: gzip` 的压缩字节。
//
// 这条用例的原文是 70 KB 量级、要十几块才输出完，正好压住那个边界：把参数改回
// `Z_FINISH` 它就红。两条腿各压一个循环 —— zlib 包装那条（`15+32` 自动识别）与
// raw deflate 那条回退分支。
static bool test_compress_large_roundtrip() {
  std::string plain;
  for (int i = 0; i < 1500; ++i) {
    plain += "line " + std::to_string(i) +
             ": 0123456789 abcdefghijklmnopqrstuvwxyz\n";
  }
  // 前置：原文必须远远超过 4 KiB（那个内部输出块），且多跨几块 —— 只跨一块的话
  // "第二次调用的那条退出条件"还没用上。
  if (plain.size() <= 4096 * 4) return false;

  const auto body_of = [](const http_compress_result& cr) {
    return std::string(cr.data.get_const_data(), cr.data.size());
  };

  const http_compress_method methods[2] = {http_compress_method::GZIP,
                                           http_compress_method::DEFLATE};
  for (int m = 0; m < 2; ++m) {
    const http_compress_result c =
        http_compress::compress(plain.data(), plain.size(), methods[m]);
    if (!c.success) return false;
    const http_compress_result d =
        http_compress::decompress(c.data.get_const_data(), c.data.size());
    if (!d.success) return false;
    if (body_of(d) != plain) return false;
  }

  // raw deflate（没有 zlib 头/尾）走的是 `decompress` 里那条回退分支。去掉 zlib
  // 包装的头 2 字节与尾 4 字节（adler32）就是裸流 —— deflate 的块结构与之无关。
  const http_compress_result z =
      http_compress::compress(plain.data(), plain.size(),
                              http_compress_method::DEFLATE);
  if (!z.success || z.data.size() < 8) return false;
  const std::string wrapped = body_of(z);
  const std::string raw = wrapped.substr(2, wrapped.size() - 6);
  const http_compress_result rd =
      http_compress::decompress(raw.data(), raw.size());
  if (!rd.success) return false;
  if (body_of(rd) != plain) return false;

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

  // ── `*` 不得把被显式拒掉的编码选回来 ────────────────────────────────
  // RFC 9110 §12.5.3：`*` 只匹配**没有显式列出**的编码。上面那条
  // `gzip;q=0, deflate` 之所以通过，靠的是"deflate 显式出现且 q 更高"，
  // 一旦两侧都只由 `*` 覆盖，原来的实现就会把 gzip 选回来 —— 也就是发一个
  // 客户端刚声明过收不了的编码。下面四条钉住这一点。
  if (http_compress::parse_accept_encoding("*, gzip;q=0") != http_compress_method::DEFLATE)
    return false;
  if (http_compress::parse_accept_encoding("gzip;q=0, *") != http_compress_method::DEFLATE)
    return false;
  if (http_compress::parse_accept_encoding("*, deflate;q=0") != http_compress_method::GZIP)
    return false;
  if (http_compress::parse_accept_encoding("*, gzip;q=0, deflate;q=0") !=
      http_compress_method::NONE)
    return false;
  // 注：这里**没有**钉"显式项比 `*` 的 q 更高时该听谁的"（例如
  // `*, gzip;q=0.5` 里 gzip 到底是 1.0 还是 0.5）。按 RFC 该由显式项说了算，
  // 但当前实现不区分这两者 —— 那是一条独立的、尚未处理的偏差，不在这里
  // 顺手固化下来。

  // ── 退回去的那个也必须"本身可接受" ──────────────────────────────────
  // 「没被 `q=0` 拒过」不等于「被接受了」：没有 `*` 的时候，**未被列出的编码
  // 默认不可接受**（RFC 9110 §12.5.3）。拿前者当后者用，就会给一个只列了
  // gzip 的客户端发 `content-encoding: deflate`。
  if (http_compress::parse_accept_encoding("gzip;q=0, gzip;q=0.5") !=
      http_compress_method::NONE)
    return false;  // 自相矛盾的输入：显式拒绝优先，退回"不发编码"
  if (http_compress::parse_accept_encoding("deflate;q=0, deflate;q=0.5") !=
      http_compress_method::NONE)
    return false;
  if (http_compress::parse_accept_encoding("gzip;q=0") != http_compress_method::NONE)
    return false;
  // `*;q=0`：未列出的一律不可接受 ⇒ 只剩显式列出的那些。
  if (http_compress::parse_accept_encoding("*;q=0") != http_compress_method::NONE)
    return false;
  if (http_compress::parse_accept_encoding("*;q=0, gzip") != http_compress_method::GZIP)
    return false;
  // 反向守卫在**上面**：`gzip` / `*` / `deflate` 那三条（本函数开头）钉的是
  // "合法输入照常选中"，收紧过头会把它们打红 —— 所以不在这里重复。
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

  // 字体：woff/woff2 里装的本来就是压缩流，再 deflate 一遍产物**更大**
  // （本仓语料 22 528 → 22 556、18 432 → 18 460 字节）—— 这两条是精确匹配，
  // 不是 `font/` 前缀。
  if (http_compress::should_compress("font/woff", excl)) return false;
  if (http_compress::should_compress("font/woff2", excl)) return false;
  // ...而**未压缩**的字体格式照旧要压：写前缀匹配就会把这两条一起排掉。
  if (!http_compress::should_compress("font/ttf", excl)) return false;
  if (!http_compress::should_compress("font/otf", excl)) return false;

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
    {"compress_large", test_compress_large_roundtrip},
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
