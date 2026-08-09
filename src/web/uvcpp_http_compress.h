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
 *  - Each compress/decompress call creates a fresh z_stream —
 *    no context takeover.  Safe for concurrent use.
 *  - Auto-detection of incoming encoding via inflateInit2(…, 47).
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_COMPRESS_H
#define SRC_WEB_UVCPP_HTTP_COMPRESS_H

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
 * @brief Compress input data.
 * @param data   Input buffer.
 * @param len    Input length in bytes.
 * @param method Target encoding (GZIP or DEFLATE).
 * @return Compressed result (check result.success).
 */
UVCPP_API http_compress_result compress(const char* data, size_t len,
                                         http_compress_method method);

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
