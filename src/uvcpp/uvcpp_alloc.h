/**
 * @file src/uvcpp/uvcpp_alloc.h
 * @brief Allocation helpers used across the project.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_ALLOC_H
#define SRC_UVCPP_UVCPP_ALLOC_H

#include <cstdlib>
#include <cstring>
#include <new>

namespace uvcpp {

template<typename T>
inline T* uvcpp_alloc() {
  T* p = static_cast<T*>(std::malloc(sizeof(T)));
  if (p == nullptr) throw std::bad_alloc();
  std::memset(p, 0, sizeof(T));
  return p;
}

template<typename T>
inline T* uvcpp_alloc_arry(size_t len) {
  T* p = static_cast<T*>(std::malloc(sizeof(T)*len));
  if (p == nullptr) throw std::bad_alloc();
  std::memset(p, 0, sizeof(T));
  return p;
}

// allocate raw bytes
inline void* uvcpp_alloc_bytes(size_t sz) {
  void* p = std::malloc(sz);
  if (p == nullptr) throw std::bad_alloc();
  std::memset(p, 0, sz);
  return p;
}

inline void *uvcpp_realloc_bytes(void *p, size_t sz) {
  void* np = std::realloc(p, sz);
  if (np == nullptr)
    throw std::bad_alloc();
  return np;
}



// free raw bytes
inline void uvcpp_free_bytes(void* p) {
  if (p == nullptr) return;
  // Clearing memory before free is optional; we avoid expensive memset for large blocks.
  std::free(p);
  return;
}
}

// Templated free for convenience
namespace uvcpp {
template<typename T>
inline void uvcpp_free(T* p) {
  uvcpp_free_bytes(reinterpret_cast<void*>(p));
}
} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_ALLOC_H
