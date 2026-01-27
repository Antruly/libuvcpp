/**
 * @file src/uvcpp/uv_alloc.h
 * @brief Allocation helpers used across the project.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UV_ALLOC_H
#define SRC_UVCPP_UV_ALLOC_H

#include <cstdlib>
#include <cstring>
#include <new>

namespace uvcpp {

template<typename T>
inline T* uv_alloc() {
  T* p = static_cast<T*>(std::malloc(sizeof(T)));
  if (p == nullptr) throw std::bad_alloc();
  std::memset(p, 0, sizeof(T));
  return p;
}

// allocate raw bytes
inline void* uv_alloc_bytes(size_t sz) {
  void* p = std::malloc(sz);
  if (p == nullptr) throw std::bad_alloc();
  std::memset(p, 0, sz);
  return p;
}

// free raw bytes
inline void uv_free_bytes(void* p) {
  if (p == nullptr) return;
  // Clearing memory before free is optional; we avoid expensive memset for large blocks.
  std::free(p);
  return;
}
}

// Templated free for convenience
namespace uvcpp {
template<typename T>
inline void uv_free(T* p) {
  uv_free_bytes(reinterpret_cast<void*>(p));
}
} // namespace uvcpp

#endif // SRC_UVCPP_UV_ALLOC_H
