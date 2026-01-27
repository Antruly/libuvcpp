#pragma once
#ifndef SRC_UVCPP_UV_BUF_H
#define SRC_UVCPP_UV_BUF_H

#include <uv.h>
#include <cstddef>
#include <type_traits>
#include <string>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace uvcpp {

// Lightweight wrapper that is ABI-compatible with libuv's uv_buf_t.
// Keep this type as small POD so it can be passed directly to libuv without copy.
struct uv_buf {
  size_t len;
  char* base;

  // non-owning utilities (do not perform allocation/deallocation)
  char operator[](const int num) const {
    if (base == nullptr) {
      throw std::out_of_range("Buffer is null");
    }
    if (num < 0 || static_cast<size_t>(num) >= len) {
      throw std::out_of_range("Index out of bounds");
    }
    return base[num];
  }

  char *get_data() const { return base; }
  const char *get_const_data() const { return base; }

  unsigned char *get_udata() const { return reinterpret_cast<unsigned char *>(base); }
  const unsigned char *get_const_udata() const { return reinterpret_cast<const unsigned char *>(base); }

  size_t size() const { return len; }

  void set_zero() const {
    if (base == nullptr && len > 0) {
      throw std::logic_error("Buffer state inconsistency: null pointer with non-zero length");
    }
    if (base != nullptr && len > 0) {
      std::memset(base, 0, len);
    }
  }

  ::uv_buf_t to_uv() const { ::uv_buf_t b; b.base = base; b.len = len; return b; }

  bool operator==(const uv_buf &bf) const {
    if (len != bf.len) return false;
    if (len == 0) return true;
    if (base == nullptr || bf.base == nullptr) return base == bf.base;
    return std::memcmp(base, bf.base, len) == 0;
  }
  bool operator!=(const uv_buf &bf) const { return !(*this == bf); }

  ::std::string to_string() const {
    if (base == nullptr) return std::string();
    return ::std::string(base, len);
  }
};

static_assert(sizeof(uv_buf) == sizeof(::uv_buf_t), "uv_buf must be layout-compatible with uv_buf_t");

// helper conversion without making uv_buf non-trivial
static inline ::uv_buf_t to_uv(const uv_buf &b) {
  ::uv_buf_t ub;
  ub.base = b.base;
  ub.len = b.len;
  return ub;
}

} // namespace uvcpp


#endif // SRC_UVCPP_UV_BUF_H