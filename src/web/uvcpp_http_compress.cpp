/**
 * @file src/web/uvcpp_http_compress.cpp
 * @brief HTTP Content-Encoding compression implementation.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_http_compress.h>

#if UVCPP_WEB_ENABLE

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace uvcpp {

// =========================================================================
// parse_accept_encoding
// =========================================================================

http_compress_method http_compress::parse_accept_encoding(
    const std::string& header) {
  if (header.empty()) return http_compress_method::NONE;

  // Parse comma-separated encodings with optional q= weights
  struct candidate { http_compress_method method; double q; };
  candidate best = { http_compress_method::NONE, 0.0 };

  std::string tok;
  std::istringstream ss(header);
  while (std::getline(ss, tok, ',')) {
    // Trim whitespace
    size_t s = 0, e = tok.size();
    while (s < e && (tok[s] == ' ' || tok[s] == '\t')) s++;
    while (e > s && (tok[e-1] == ' ' || tok[e-1] == '\t')) e--;
    std::string enc = tok.substr(s, e - s);

    // Lower-case for matching
    for (auto& c : enc) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Parse quality value
    double q = 1.0;
    size_t qpos = enc.find(";q=");
    if (qpos != std::string::npos) {
      q = std::stod(enc.substr(qpos + 3));
      enc = enc.substr(0, qpos);
      // Re-trim
      while (!enc.empty() && enc.back() == ' ') enc.pop_back();
    }

    if (q == 0.0) continue;  // "not acceptable"

    http_compress_method method = http_compress_method::NONE;
    if (enc == "gzip" || enc == "x-gzip")
      method = http_compress_method::GZIP;
    else if (enc == "deflate" || enc == "x-deflate")
      method = http_compress_method::DEFLATE;
    else if (enc == "*")
      method = http_compress_method::GZIP;  // wildcard → prefer gzip

    if (method != http_compress_method::NONE) {
      if (q > best.q ||
          (q == best.q && static_cast<int>(method) < static_cast<int>(best.method))) {
        best.method = method;
        best.q = q;
      }
    }
  }

  return best.method;
}

// =========================================================================
// should_compress
// =========================================================================

bool http_compress::should_compress(const std::string& content_type,
                                     const std::vector<std::string>& excluded_types) {
  if (content_type.empty()) return true;  // no content-type → compress

  // Lower-case for matching
  std::string ct = content_type;
  for (auto& c : ct) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  for (const auto& excl : excluded_types) {
    std::string excl_lower = excl;
    for (auto& c : excl_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (!excl_lower.empty() && excl_lower.back() == '/') {
      // Prefix match: "image/" matches "image/png", "image/jpeg", etc.
      if (ct.compare(0, excl_lower.size(), excl_lower) == 0)
        return false;
    } else {
      // Exact match
      if (ct == excl_lower) return false;
    }
  }
  return true;
}

// =========================================================================
// default_excluded_mime_types
// =========================================================================

const std::vector<std::string>& http_compress::default_excluded_mime_types() {
  static const std::vector<std::string> types = {
    "image/",
    "video/",
    "audio/",
    "application/zip",
    "application/gzip",
    "application/x-gzip",
    "application/x-rar-compressed",
    "application/x-7z-compressed",
    "application/x-bzip2",
    "application/x-xz",
    "application/x-compress",
    "application/x-compressed",
    "application/zstd",
    "application/pdf",
    "application/octet-stream",
  };
  return types;
}

// =========================================================================
// Zlib-based compress / decompress (only when UVCPP_ZLIB_ENABLE=1)
// =========================================================================

#if UVCPP_ZLIB_ENABLE

#include <zlib.h>

http_compress_result http_compress::compress(const char* data, size_t len,
                                               http_compress_method method) {
  http_compress_result result;
  result.method = method;
  if (!data || len == 0) { result.success = true; return result; }

  int window_bits;
  switch (method) {
    case http_compress_method::GZIP:    window_bits = 15 + 16; break;  // gzip wrap
    case http_compress_method::DEFLATE: window_bits = 15;       break;  // zlib wrap
    default: result.success = false; return result;
  }

  z_stream strm;
  std::memset(&strm, 0, sizeof(strm));

  if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                   window_bits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    result.success = false;
    return result;
  }

  strm.next_in   = reinterpret_cast<const Bytef*>(data);
  strm.avail_in  = static_cast<uInt>(len);

  int ret;
  const size_t C = 4096;
  do {
    unsigned char tmp[C];
    strm.next_out  = tmp;
    strm.avail_out = C;
    ret = deflate(&strm, Z_FINISH);
    size_t written = C - strm.avail_out;
    if (written > 0) result.data.append(reinterpret_cast<const char*>(tmp), written);
  } while (ret == Z_OK);

  result.success = (ret == Z_STREAM_END);
  deflateEnd(&strm);
  return result;
}

http_compress_result http_compress::decompress(const char* data, size_t len) {
  http_compress_result result;
  if (!data || len == 0) { result.success = true; return result; }

  // Auto-detect: try with window_bits=47 (32+15) which handles gzip, zlib, and raw
  z_stream strm;
  std::memset(&strm, 0, sizeof(strm));

  if (inflateInit2(&strm, 15 + 32) != Z_OK) {
    // Fallback: try zlib-wrap only
    std::memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 15) != Z_OK) {
      result.success = false;
      return result;
    }
  }

  strm.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
  strm.avail_in = static_cast<uInt>(len);

  int ret;
  const size_t C = 4096;
  do {
    unsigned char tmp[C];
    strm.next_out  = tmp;
    strm.avail_out = C;
    ret = inflate(&strm, Z_FINISH);
    size_t written = C - strm.avail_out;
    if (written > 0) result.data.append(reinterpret_cast<const char*>(tmp), written);
  } while (ret == Z_OK);

  // Determine which encoding was detected
  if (ret == Z_STREAM_END) {
    // strm.data_type & 0x08: gzip header detected
    // But zlib doesn't reliably expose this; just report success
    result.success = true;
    result.method = http_compress_method::GZIP;  // default assumption
    inflateEnd(&strm);
  } else {
    // Try raw deflate as fallback (some servers send bare deflate)
    inflateEnd(&strm);
    std::memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, -15) == Z_OK) {
      result.data.clear();
      strm.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
      strm.avail_in = static_cast<uInt>(len);
      do {
        unsigned char tmp[C];
        strm.next_out  = tmp;
        strm.avail_out = C;
        ret = inflate(&strm, Z_FINISH);
        size_t written = C - strm.avail_out;
        if (written > 0) result.data.append(reinterpret_cast<const char*>(tmp), written);
      } while (ret == Z_OK);
      result.success = (ret == Z_STREAM_END);
      result.method = http_compress_method::DEFLATE;
      inflateEnd(&strm);
    } else {
      result.success = false;
    }
  }

  return result;
}

#endif  // UVCPP_ZLIB_ENABLE

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
