/**
 * @file src/web/uvcpp_http_compress.h
 * @brief HTTP Content-Encoding compression utilities (gzip / deflate).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides transparent body compression / decompression for the HTTP
 * client and server modules.  Enabled only when UVCPP_ZLIB_ENABLE=1
 * (explicit opt-in via cmake -DUVCPP_ENABLE_ZLIB=ON).
 *
 * Key differences from WebSocket Per-Message Deflate (RFC 7692):
 *  - Uses standard gzip (RFC 1952) and deflate (RFC 1950) wrap,
 *    NOT raw deflate.  This means *positive* window_bits values:
 *      gzip:    15 + 16 = 31
 *      deflate: 15       (zlib header/trailer)
 *  - Every call starts from a **fresh deflate/inflate state** — no context
 *    takeover, so each response is an independent stream.  `compress()`
 *    reaches that state by resetting a per-thread kernel instead of building
 *    a new one per call (a level-6 deflate kernel is ~256 KB, and allocating
 *    that on every response is what used to dominate `json-comp`); the bytes
 *    produced are identical either way.  Safe for concurrent use — the kernel
 *    is thread-local, so no two threads ever share one.
 *  - Auto-detection of incoming encoding via inflateInit2(…, 47).
 *  - The compression **level** is a parameter of `compress()`, defaulting to
 *    `k_compress_level_default` (= zlib's `Z_DEFAULT_COMPRESSION`, which today
 *    means level 6).  The level changes *which bytes* come out, never *whether*
 *    compression happens — so on the server side it belongs in the response's
 *    variant-cache **key** (see `uvcpp_http_server`), not in the "should we
 *    compress" decision.
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_COMPRESS_H
#define SRC_WEB_UVCPP_HTTP_COMPRESS_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEB_ENABLE

#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>

#include <string>
#include <vector>

namespace uvcpp {

// =========================================================================
// Compression method
// =========================================================================

/** @brief Content-Encoding values supported by this module. */
enum class http_compress_method : int {
  NONE    = 0,   ///< No compression / identity
  GZIP    = 1,   ///< gzip (RFC 1952)
  DEFLATE = 2,   ///< deflate (RFC 1950, zlib format)
};

// =========================================================================
// Compression result
// =========================================================================

/** @brief Result of a compress or decompress operation.
 *         Uses uvcpp_buf (not std::string) — safe for binary payloads. */
struct http_compress_result {
  bool                success = false;  ///< true if operation completed
  uvcpp_buf           data;             ///< Result payload (empty on failure)
  http_compress_method method = http_compress_method::NONE;  ///< Encoding used
};

// =========================================================================
// Compression utility functions (free functions, no state)
// =========================================================================

namespace http_compress {

#if UVCPP_ZLIB_ENABLE

/**
 * @brief 默认压缩档位。
 *
 * 值就是 zlib 的 `Z_DEFAULT_COMPRESSION`（今天等价第 6 档）。写成字面量而不是
 * 包含 `<zlib.h>`：这个头文件是公开头，不该把 zlib 的类型与宏带进每一个
 * include 它的编译单元。
 */
static const int k_compress_level_default = -1;

/** 档位取值范围的闭区间：`k_compress_level_default`(-1) 与 0..9。 */
static const int k_compress_level_min = -1;
static const int k_compress_level_max = 9;

/**
 * @brief 把任意整数收进 `[k_compress_level_min, k_compress_level_max]`。
 *
 * 夹紧而不是报错：档位错了最坏是"压得多一点或少一点"，没有任何一条路径会因为
 * 它变得不正确，所以没有理由让调用方在运行时处理一个失败返回值。越界值一律
 * 落到最近的端点上（`-5` → `-1`，`99` → `9`）。
 *
 * 服务器侧 `set_compress_level()` 存的就是这里夹过的值 —— 键里的档位字符
 * （见 `uvcpp_http_server.cpp`）依赖"两个不同的档位不会映射到同一个字符"，
 * 而未夹紧的 15 与 5 会撞在一起。
 */
UVCPP_API int clamp_compress_level(int level);

/**
 * @brief Compress input data.
 * @param data   Input buffer.
 * @param len    Input length in bytes.
 * @param method Target encoding (GZIP or DEFLATE).
 * @return Compressed result (check result.success).  Uses
 *         `k_compress_level_default`.
 */
UVCPP_API http_compress_result compress(const char* data, size_t len,
                                         http_compress_method method);

/**
 * @brief Compress input data at an explicit level.
 * @param data   Input buffer.
 * @param len    Input length in bytes.
 * @param method Target encoding (GZIP or DEFLATE).
 * @param level  Deflate level, clamped by `clamp_compress_level()`.  Lower is
 *               faster and bigger; 9 is slowest and smallest.
 * @return Compressed result (check result.success).
 *
 * 与三参数那份的**唯一**区别就是档位，产出的字节因此**不同**（同一份输入、
 * 同一个档位，则与三参数那份逐字节相同 —— 每调用一次都是全新状态）。
 */
UVCPP_API http_compress_result compress(const char* data, size_t len,
                                         http_compress_method method,
                                         int level);

/**
 * @brief Decompress input data.
 *        Auto-detects gzip vs deflate from the data header.
 * @param data  Compressed input.
 * @param len   Input length.
 * @return Decompressed result.
 */
UVCPP_API http_compress_result decompress(const char* data, size_t len);

#endif  // UVCPP_ZLIB_ENABLE

/**
 * @brief Parse Accept-Encoding header and return the best supported method.
 *
 * Quality values (q=…) are respected per RFC 7231 Section 5.3.4.
 * Priority when equal: gzip > deflate.
 *
 * @param header  Value of the Accept-Encoding request header.
 * @return        Best method, or NONE if no supported encoding is acceptable.
 */
UVCPP_API http_compress_method parse_accept_encoding(const std::string& header);

/**
 * @brief Determine whether a response should be compressed.
 * @param content_type   The response Content-Type value (may be empty).
 * @param excluded_types List of MIME types to skip.
 *                       Supports wildcard: "image/*" matches "image/png", etc.
 * @return true if compression is appropriate.
 */
UVCPP_API bool should_compress(const std::string& content_type,
                                const std::vector<std::string>& excluded_types);

/**
 * @brief Default MIME types excluded from HTTP compression.
 *
 * These types are generally already compressed or are binary formats
 * that gain little from gzip.  Callers can use this as a base and
 * customise via set_compress_excluded_types() on the server.
 */
UVCPP_API const std::vector<std::string>& default_excluded_mime_types();

}  // namespace http_compress
}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_COMPRESS_H
