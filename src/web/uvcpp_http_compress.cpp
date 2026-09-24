/**
 * @file src/web/uvcpp_http_compress.cpp
 * @brief HTTP Content-Encoding compression implementation.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_compress.h>

#if UVCPP_WEB_ENABLE

#include <algorithm>
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

  // 三种"这个编码到底能不能发"的依据，缺一不可：
  //   `*_ok`     —— 没被 `q=0` 显式拒过（**不等于**被接受了 —— 没有 `*` 时
  //                 未被列出的编码默认不可接受，RFC 9110 §12.5.3）；
  //   `*_accept` —— 显式列出且 q>0；
  //   `wildcard` —— `*` 出现过（它覆盖的就是"没被显式列出"的那些）。
  // `*_ok` 单独用会把"没被拒"当成"被接受"，于是给一个只列了 gzip 的客户端
  // 发 `content-encoding: deflate` —— 那正是这个函数要修的那一类错误。
  bool gzip_ok = true, deflate_ok = true;
  bool gzip_accept = false, deflate_accept = false, wildcard = false;

  std::string tok;
  std::istringstream ss(header);
  while (std::getline(ss, tok, ',')) {
    // Trim whitespace
    size_t s = 0, e = tok.size();
    while (s < e && (tok[s] == ' ' || tok[s] == '\t')) s++;
    while (e > s && (tok[e-1] == ' ' || tok[e-1] == '\t')) e--;
    std::string enc = tok.substr(s, e - s);

    // Lower-case for matching（`http_lower_ascii` 是同一张表，见
    // `uvcpp_http_common.h` —— 全模块只剩这一个 ASCII 小写原语）
    for (auto& c : enc) c = static_cast<char>(http_lower_ascii(static_cast<unsigned char>(c)));

    // Parse quality value
    double q = 1.0;
    size_t qpos = enc.find(";q=");
    if (qpos != std::string::npos) {
      try { q = std::stod(enc.substr(qpos + 3)); } catch (...) { q = 0.0; }
      enc = enc.substr(0, qpos);
      // Re-trim
      while (!enc.empty() && enc.back() == ' ') enc.pop_back();
    }

    if (q == 0.0) {
      // "not acceptable" —— 记下来，别只是跳过这一项。
      if (enc == "gzip" || enc == "x-gzip") gzip_ok = false;
      else if (enc == "deflate" || enc == "x-deflate") deflate_ok = false;
      continue;
    }

    http_compress_method method = http_compress_method::NONE;
    if (enc == "gzip" || enc == "x-gzip") {
      gzip_accept = true;
      method      = http_compress_method::GZIP;
    } else if (enc == "deflate" || enc == "x-deflate") {
      deflate_accept = true;
      method         = http_compress_method::DEFLATE;
    } else if (enc == "*") {
      // RFC 9110 §12.5.3：`*` 只匹配**没有显式列出**的编码。所以选择时要把
      // 被 `q=0` 显式拒掉的排掉 —— 否则 `*, gzip;q=0` 会选回 gzip，服务端
      // 于是发了一个客户端刚刚声明过收不了的编码。
      wildcard = true;
      method   = gzip_ok ? http_compress_method::GZIP
                         : (deflate_ok ? http_compress_method::DEFLATE
                                       : http_compress_method::NONE);
    }

    if (method != http_compress_method::NONE) {
      if (q > best.q ||
          (q == best.q && static_cast<int>(method) < static_cast<int>(best.method))) {
        best.method = method;
        best.q = q;
      }
    }
  }

  // 收尾一道：上面按 q 值取最大值，而 `q=0` 那支是"直接跳过"，所以
  // `gzip;q=0, gzip;q=0.5` 这种自相矛盾的输入仍会把 gzip 选回来。
  // 退回去的那个必须**本身可接受** —— 于是先算"这个编码今天到底能不能发"。
  const bool gzip_usable    = gzip_ok && (gzip_accept || wildcard);
  const bool deflate_usable = deflate_ok && (deflate_accept || wildcard);
  if (best.method == http_compress_method::GZIP && !gzip_usable) {
    best.method = deflate_usable ? http_compress_method::DEFLATE
                                 : http_compress_method::NONE;
  } else if (best.method == http_compress_method::DEFLATE && !deflate_usable) {
    best.method = gzip_usable ? http_compress_method::GZIP
                              : http_compress_method::NONE;
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
  for (auto& c : ct) c = static_cast<char>(http_lower_ascii(static_cast<unsigned char>(c)));

  for (const auto& excl : excluded_types) {
    std::string excl_lower = excl;
    for (auto& c : excl_lower)
      c = static_cast<char>(http_lower_ascii(static_cast<unsigned char>(c)));

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

  strm.next_in   = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
  strm.avail_in  = static_cast<uInt>(len);

  int ret;
  const size_t C = 4096;
  do {
    unsigned char tmp[C];
    strm.next_out  = tmp;
    strm.avail_out = C;
    ret = deflate(&strm, Z_FINISH);
    size_t written = C - strm.avail_out;
    if (written > 0)
      result.data.append_data(reinterpret_cast<const char*>(tmp), written);
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
    if (written > 0)
      result.data.append_data(reinterpret_cast<const char*>(tmp), written);
  } while (ret == Z_OK);

  // Determine which encoding was detected
  if (ret == Z_STREAM_END) {
    result.success = true;
    result.method = http_compress_method::GZIP;
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
        if (written > 0)
          result.data.append_data(reinterpret_cast<const char*>(tmp), written);
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
