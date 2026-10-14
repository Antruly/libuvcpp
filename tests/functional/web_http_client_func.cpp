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
//
// `parse_accept_encoding()` 的结果**随这一份构建编得出哪些编码而变**：本函数在
// `UVCPP_ZLIB_ENABLE` 里（下面那批 gzip/deflate 期望是它的前提），但 brotli 可以
// 同时编进来，这时 `*` 的兜底结果会整体前移一位。所以凡是靠 `*` 兜底的期望都不
// 写死 gzip，而是读下面这四个常量 —— 兜底序 = br > gzip > deflate，与静态腿选
// 孪生文件的 `http_pick_precompressed_variants()` 同序。
//
// 只编 brotli 的构建（`UVCPP_ZLIB_ENABLE=0`）**不跑本函数**（它的前提不成立）；
// 那一种构建的协商由下面的 `test_accept_encoding_no_zlib()` 与
// `test_compress_brotli()` 里的协商段覆盖。
static const http_compress_method k_star_first =
#if UVCPP_BROTLI_ENABLE
    http_compress_method::BROTLI;
#else
    http_compress_method::GZIP;
#endif
static const http_compress_method k_star_wo_gzip =
#if UVCPP_BROTLI_ENABLE
    http_compress_method::BROTLI;
#else
    http_compress_method::DEFLATE;
#endif
static const http_compress_method k_star_wo_deflate =
#if UVCPP_BROTLI_ENABLE
    http_compress_method::BROTLI;
#else
    http_compress_method::GZIP;
#endif
static const http_compress_method k_star_wo_both =
#if UVCPP_BROTLI_ENABLE
    http_compress_method::BROTLI;
#else
    http_compress_method::NONE;
#endif

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
  // 通配符 → 本构建的首选项（zlib-only 是 gzip，带 brotli 是 br）
  if (http_compress::parse_accept_encoding("*") != k_star_first) return false;
  // q=0 should skip
  if (http_compress::parse_accept_encoding("gzip;q=0, deflate") != http_compress_method::DEFLATE)
    return false;

  // ── `*` 不得把被显式拒掉的编码选回来 ────────────────────────────────
  // RFC 9110 §12.5.3：`*` 只匹配**没有显式列出**的编码。上面那条
  // `gzip;q=0, deflate` 之所以通过，靠的是"deflate 显式出现且 q 更高"，
  // 一旦两侧都只由 `*` 覆盖，原来的实现就会把 gzip 选回来 —— 也就是发一个
  // 客户端刚声明过收不了的编码。下面四条钉住这一点。
  if (http_compress::parse_accept_encoding("*, gzip;q=0") != k_star_wo_gzip)
    return false;
  if (http_compress::parse_accept_encoding("gzip;q=0, *") != k_star_wo_gzip)
    return false;
  if (http_compress::parse_accept_encoding("*, deflate;q=0") != k_star_wo_deflate)
    return false;
  // gzip 与 deflate 都被排掉：zlib-only 的构建里一个候选都不剩 ⇒ NONE；
  // 带 brotli 的构建里 `*` 仍然覆盖 br ⇒ br。
  if (http_compress::parse_accept_encoding("*, gzip;q=0, deflate;q=0") != k_star_wo_both)
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
// Test 7c: brotli（`UVCPP_BROTLI_ENABLE=1` 才有）
// =========================================================================
//
// 这一节**不在** `UVCPP_ZLIB_ENABLE` 里：只编 brotli 的构建（`-DUVCPP_ENABLE_ZLIB=OFF
// -DUVCPP_ENABLE_BROTLI=ON`）也必须跑到它 —— 那正是 `UVCPP_HTTP_COMPRESS_ENABLE`
// 这个派生宏存在的理由，把它挂在 zlib 下面这条用例就永远量不到那条配置。
//
// 判据分三层：
//   ① 往返：解压后与原文**逐字节相同**（含一份 > 16 KiB、输出跨过 4 KiB 内部块
//      的语料 —— 手工流式循环最容易错的就是"最后一块算两遍"，那会让产物变成
//      两倍大，而只解前一半也能"成功"，所以还必须断言字节数）。
//   ② 无跨请求残留：连着压 A、B、A，第二次的 A 必须与第一次**逐字节相同**。
//      brotli 的 API 里没有 `BrotliEncoderReset()`（`encode.h` 里只有
//      `CreateInstance` / `Compress` / `CompressStream`），所以每次都是新状态 ——
//      这条用例把"哪天有人图省事把状态缓存起来"钉红：缓存的窗口跨到上一份响应上，
//      产出的字节就会随前一个请求而变。
//   ③ 协商：`br` 与 gzip/deflate 的优先级、`q=0`、`*` 的兜底序，必须与静态腿选
//      孪生文件那套 `http_pick_precompressed_variants()` **同序** —— 否则同一份
//      资源走孪生发 `br`、走现场压缩发 `gzip`。
#if UVCPP_BROTLI_ENABLE
#include <web/uvcpp_http_compress.h>

/** 可压语料：`json` 那类重复度高的文本。 */
static std::string brotli_text_corpus(size_t n) {
  std::string s;
  s.reserve(n);
  for (size_t i = 0; s.size() < n; ++i) {
    s += "{\"id\":" + std::to_string(i) +
         ",\"name\":\"item\",\"category\":\"tools\",\"active\":true}\n";
  }
  s.resize(n);
  return s;
}

/** 不可压语料：LCG 伪随机字节（确定性）。用来把**输出**顶到 4 KiB 块以上。 */
static std::string brotli_noise_corpus(size_t n) {
  std::string s(n, '\0');
  uint32_t x = 0x12345678u;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1103515245u + 12345u;
    s[i] = static_cast<char>((x >> 16) & 0xff);
  }
  return s;
}

static bool test_compress_brotli() {
  const auto body_of = [](const http_compress_result& r) {
    return std::string(r.data.get_const_data(), r.data.size());
  };

  // ── ① 往返 ────────────────────────────────────────────────────────────
  const std::string text = brotli_text_corpus(40 * 1024);
  {
    const http_compress_result c = http_compress::compress(
        text.data(), text.size(), http_compress_method::BROTLI);
    if (!c.success) return false;
    if (c.data.size() == 0 || c.data.size() >= text.size()) return false;
    if (c.method != http_compress_method::BROTLI) return false;

    const http_compress_result d =
        http_compress::decompress(c.data.get_const_data(), c.data.size());
    if (!d.success) return false;
    // 解不出来时必须落回 GZIP（inflate 那一支），落回去说明 br 那一支根本没接上
    if (d.method != http_compress_method::BROTLI) return false;
    if (body_of(d) != text) return false;
  }
  {
    // 高熵输入：br 的产物会**大于** 4 KiB，流式循环因此必须走到"填满一块就换
    // 下一块"那条路 —— "最后一块重复计入"那个错法正是在这里现形。
    const std::string noise = brotli_noise_corpus(64 * 1024);
    const http_compress_result c = http_compress::compress(
        noise.data(), noise.size(), http_compress_method::BROTLI);
    if (!c.success) return false;
    // 上界：一份高熵输入压不出花样来，产物必须落在"原文 + 一点包装"附近。
    // 少了这条，'把最后一块算了两遍'（产物 ≈ 2×）照样能过往返断言。
    if (c.data.size() > noise.size() + 1024) return false;
    const http_compress_result d =
        http_compress::decompress(c.data.get_const_data(), c.data.size());
    if (!d.success) return false;
    if (body_of(d) != noise) return false;
  }

  // ── ①′ 默认档 = `k_brotli_quality_default`，而**不是** quality 0 ─────
  // 三参数那份（以及把服务器默认档 `k_compress_level_default` 原样透传进来的
  // 四参数那份）必须与显式 q5 逐字节相同。少了这一段，"默认档退化成 q0"照样
  // 能过上面对称性不敏感的往返断言 —— 而它对 `json-comp` 是致命的：本机
  // 32 820 B 的语料 q0 = 1 784 B、q5 = 620 B，q0 比 gzip 的 1 118 B 还大 60%，
  // 也就是"开了 br 反而扣分"。
  {
    const std::string s = brotli_text_corpus(20 * 1024);
    const http_compress_result def = http_compress::compress(
        s.data(), s.size(), http_compress_method::BROTLI);
    const http_compress_result q5 = http_compress::compress(
        s.data(), s.size(), http_compress_method::BROTLI,
        http_compress::k_brotli_quality_default);
    const http_compress_result neg = http_compress::compress(
        s.data(), s.size(), http_compress_method::BROTLI,
        http_compress::k_compress_level_default);
    const http_compress_result q0 = http_compress::compress(
        s.data(), s.size(), http_compress_method::BROTLI, 0);
    if (!def.success || !q5.success || !neg.success || !q0.success) return false;
    if (body_of(def) != body_of(q5)) return false;
    if (body_of(neg) != body_of(q5)) return false;
    // 上面两条恒等断言在"全都退化成同一个档"的实现上也会绿，所以再钉一条
    // **不对称**的：q0 必须严格更大。q0 与 q5 的产物在本机语料上差 2.9×。
    if (q0.data.size() <= def.data.size()) return false;
  }

  // ── ② 无跨请求残留（每次都是全新状态） ───────────────────────────────
  {
    const std::string a = brotli_text_corpus(9000);
    const std::string b = brotli_noise_corpus(9000);
    const http_compress_result a1 = http_compress::compress(
        a.data(), a.size(), http_compress_method::BROTLI);
    const http_compress_result b1 = http_compress::compress(
        b.data(), b.size(), http_compress_method::BROTLI);
    const http_compress_result a2 = http_compress::compress(
        a.data(), a.size(), http_compress_method::BROTLI);
    if (!a1.success || !b1.success || !a2.success) return false;
    if (body_of(a1) != body_of(a2)) return false;
  }

  // ── 档位：q0 与 q11 都要能往返，且 q11 不会更大 ──────────────────────
  {
    const std::string s = brotli_text_corpus(12 * 1024);
    const http_compress_result lo = http_compress::compress(
        s.data(), s.size(), http_compress_method::BROTLI, 0);
    const http_compress_result hi = http_compress::compress(
        s.data(), s.size(), http_compress_method::BROTLI, 11);
    if (!lo.success || !hi.success) return false;
    if (hi.data.size() > lo.data.size()) return false;
    const http_compress_result d =
        http_compress::decompress(hi.data.get_const_data(), hi.data.size());
    if (!d.success || body_of(d) != s) return false;

    // 三参数那份用的是 `k_brotli_quality_default`，必须落在合法区间里
    if (http_compress::k_brotli_quality_default < http_compress::k_brotli_quality_min ||
        http_compress::k_brotli_quality_default > http_compress::k_brotli_quality_max)
      return false;
  }

  // ── 夹紧：与 `clamp_compress_level` 同一条理由（越界落到最近端点） ──
  // 负数 = **哨兵**（"用这个编码器自己的默认档"），不是"很小的质量"：
  // 服务器侧 `set_compress_level()` 的默认值 `k_compress_level_default`(-1)
  // 会**原样**流到 brotli 这一支，收成 0 就等于默认档悄悄变成"最快档"。
  if (http_compress::clamp_brotli_quality(-100) !=
      http_compress::k_brotli_quality_default)
    return false;
  if (http_compress::clamp_brotli_quality(-1) !=
      http_compress::k_brotli_quality_default)
    return false;
  if (http_compress::clamp_brotli_quality(100) != http_compress::k_brotli_quality_max)
    return false;
  if (http_compress::clamp_brotli_quality(5) != 5) return false;
  // 两个夹紧函数**不能合并**成"夹进 [a,b]"一把尺子：上限 9 vs 11 会把 brotli 的
  // 10 悄悄降成 9。deflate 侧"负数 = 默认档"是退化的（它的默认档本身就是 -1）。
  if (http_compress::clamp_compress_level(-1) != -1) return false;
  if (http_compress::clamp_compress_level(-100) != -1) return false;

  // ── ③ 协商 ───────────────────────────────────────────────────────────
  if (http_compress::parse_accept_encoding("br") != http_compress_method::BROTLI)
    return false;
  // 同 q：br 赢（与静态腿的孪生选择同序）
  if (http_compress::parse_accept_encoding("gzip, br") != http_compress_method::BROTLI)
    return false;
  if (http_compress::parse_accept_encoding("br, gzip, deflate") !=
      http_compress_method::BROTLI)
    return false;
  // 显式 q 说了算
#if UVCPP_ZLIB_ENABLE
  if (http_compress::parse_accept_encoding("gzip;q=1, br;q=0.5") !=
      http_compress_method::GZIP)
    return false;
#endif
  if (http_compress::parse_accept_encoding("gzip;q=0.5, br;q=1") !=
      http_compress_method::BROTLI)
    return false;
  // `*` 的兜底序也是 br 优先
  if (http_compress::parse_accept_encoding("*") != http_compress_method::BROTLI)
    return false;
#if UVCPP_ZLIB_ENABLE
  // br 被拒之后落到 gzip；只编 brotli 的构建里没有可落的编码 ⇒ NONE，
  // 那一条由 `test_accept_encoding_no_zlib()` 钉。
  if (http_compress::parse_accept_encoding("*, br;q=0") != http_compress_method::GZIP)
    return false;
#endif
  // 显式拒掉 br 之后不能再被 `*` 选回来
  if (http_compress::parse_accept_encoding("br;q=0") != http_compress_method::NONE)
    return false;
#if UVCPP_ZLIB_ENABLE
  if (http_compress::parse_accept_encoding("br;q=0, *") != http_compress_method::GZIP)
    return false;
#endif
  // 大写编码名（RFC 9110：编码名大小写不敏感）
  if (http_compress::parse_accept_encoding("BR") != http_compress_method::BROTLI)
    return false;
  return true;
}
#endif  // UVCPP_BROTLI_ENABLE

// =========================================================================
// Test 7d: 没编 brotli 的构建里，`br` 必须当作**认不出的编码**
// =========================================================================
//
// 与上面那条互补的负对照。少了它，"协商结果只说得出编得出来的编码"这条规矩
// 只在 brotli 开着的树里被量过 —— 而它真正的用武之地正是 brotli 关着的树。
// `Accept-Encoding: br` 单独出现时必须发原文（`NONE`）：客户端只列了 br，说明
// 它认的就只有 br，退到 gzip 与发一个它没要的编码是一回事。
#if !UVCPP_BROTLI_ENABLE && UVCPP_ZLIB_ENABLE
#include <web/uvcpp_http_compress.h>
static bool test_accept_encoding_no_brotli() {
  if (http_compress::parse_accept_encoding("br") != http_compress_method::NONE)
    return false;
  if (http_compress::parse_accept_encoding("br, gzip") != http_compress_method::GZIP)
    return false;
  // `*` 不会兜到 br 上（这一份构建里 br 不在候选里）
  if (http_compress::parse_accept_encoding("*") != http_compress_method::GZIP)
    return false;
  // 而 gzip/deflate 在 zlib 开着的构建里照旧照常
  if (http_compress::parse_accept_encoding("gzip") != http_compress_method::GZIP)
    return false;
  return true;
}
#endif

// =========================================================================
// Test 8b: 协商在"只编了 brotli"的构建里（`UVCPP_ZLIB_ENABLE=0`）
// =========================================================================
//
// `test_accept_encoding_parse()` 的前提是 zlib 在，`test_accept_encoding_no_brotli()`
// 的前提是 brotli 不在；这两条合起来漏掉了"只编 brotli"这一格。头文件对
// `parse_accept_encoding()` 的承诺是"只返回这一份构建编得出来的编码"，本函数就
// 钉它另一面：gzip/deflate 问不出来。
#if !UVCPP_ZLIB_ENABLE && UVCPP_BROTLI_ENABLE
#include <web/uvcpp_http_compress.h>
static bool test_accept_encoding_no_zlib() {
  if (http_compress::parse_accept_encoding("gzip") != http_compress_method::NONE)
    return false;
  if (http_compress::parse_accept_encoding("deflate") != http_compress_method::NONE)
    return false;
  if (http_compress::parse_accept_encoding("gzip;q=1, br;q=0.5") !=
      http_compress_method::BROTLI)
    return false;
  // gzip 编不出来 ⇒ `gzip, br` 里只有 br 可选（不因为客户端把 gzip 写在前面就
  // 退到 NONE）
  if (http_compress::parse_accept_encoding("gzip, br") != http_compress_method::BROTLI)
    return false;
  // `*` 兜到 br
  if (http_compress::parse_accept_encoding("*") != http_compress_method::BROTLI)
    return false;
  // `*, br;q=0`：`*` 覆盖的候选（只有 br）全被拒 ⇒ 什么都不发
  if (http_compress::parse_accept_encoding("*, br;q=0") != http_compress_method::NONE)
    return false;
  return true;
}
#endif

// =========================================================================
// Test 8c: 一个编码器都没有的构建（`UVCPP_ZLIB_ENABLE=0` 且 `UVCPP_BROTLI_ENABLE=0`）
// =========================================================================
//
// 这一格连 HTTP 层的压缩都关掉了（`UVCPP_HTTP_COMPRESS_ENABLE=(0||0)`），但
// `parse_accept_encoding()` 这个符号仍然在（`uvcpp_http_compress.h` 里它的声明在
// 两个 `_ENABLE` 之外，且定义无条件编译 —— 公共符号表不随构建选项变，
// `check_capi_symbols` 量的就是这件事）。它必须一律回 NONE：协商结果里出现任何
// 一个"编不出来"的编码，就等于对一个只列了它的客户端撒谎。
#if !UVCPP_ZLIB_ENABLE && !UVCPP_BROTLI_ENABLE
#include <web/uvcpp_http_compress.h>
static bool test_accept_encoding_no_encoder() {
  if (http_compress::parse_accept_encoding("*") != http_compress_method::NONE)
    return false;
  if (http_compress::parse_accept_encoding("gzip, deflate") !=
      http_compress_method::NONE)
    return false;
  if (http_compress::parse_accept_encoding("br") != http_compress_method::NONE)
    return false;
  if (http_compress::parse_accept_encoding("gzip;q=1, br;q=1, *") !=
      http_compress_method::NONE)
    return false;
  return true;
}
#endif

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
#if UVCPP_BROTLI_ENABLE
    {"compress_brotli", test_compress_brotli},
#endif
#if !UVCPP_BROTLI_ENABLE && UVCPP_ZLIB_ENABLE
    {"accept_encoding_no_brotli", test_accept_encoding_no_brotli},
#endif
#if !UVCPP_ZLIB_ENABLE && UVCPP_BROTLI_ENABLE
    {"accept_encoding_no_zlib", test_accept_encoding_no_zlib},
#endif
#if !UVCPP_ZLIB_ENABLE && !UVCPP_BROTLI_ENABLE
    {"accept_encoding_no_encoder", test_accept_encoding_no_encoder},
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
