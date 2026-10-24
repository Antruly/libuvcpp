/**
 * @file src/uvcpp/uvcpp_compat.h
 * @brief Small C++11 / CRT compatibility shims for MSVC 2013.
 *
 * VS2013 (MSVC 18.00) is supported for the net+ssl and web-lite source
 * profiles only. It lacks the `constexpr`, `thread_local` and `alignas`
 * keywords, and its C runtime has no C99 `snprintf` / `%zu`. This header
 * gives the rest of the tree one spelling for each:
 *
 *   UVCPP_CONSTEXPR   -> constexpr, or const on VS2013
 *   UVCPP_THREAD_LOCAL-> thread_local, or __declspec(thread) on VS2013
 *   UVCPP_SIZE_FMT    -> "zu", or "Iu" on VS2013
 *   uvcpp_snprintf()  -> C99 snprintf semantics on every CRT
 *
 * Only *trivial* thread-locals are compiled on VS2013 (the web-lite profile
 * turns off WebSocket and gzip, which are the non-trivial ones). This header
 * intentionally does not try to emulate thread_local for objects with a
 * destructor.
 *
 * This file is ASCII-only: it is included by every public header and by
 * translation units whose toolchain and locale we do not control.
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_COMPAT_H
#define SRC_UVCPP_UVCPP_COMPAT_H

#include <cstddef>
#include <cstdarg>
#include <cstdio>

#if defined(_MSC_VER) && _MSC_VER < 1900
#  define UVCPP_CONSTEXPR const
#  define UVCPP_THREAD_LOCAL __declspec(thread)
/* MSVC 2013 has no noexcept specifier at all (C3646), not even an operator. */
#  define UVCPP_NOEXCEPT
/*
 * MSVC's printf understands "Iu" for size_t. The C99 "zu" form only arrived
 * with the Universal CRT in VS2015.
 */
#  define UVCPP_SIZE_FMT "Iu"
#else
#  define UVCPP_CONSTEXPR constexpr
#  define UVCPP_THREAD_LOCAL thread_local
#  define UVCPP_NOEXCEPT noexcept
#  define UVCPP_SIZE_FMT "zu"
#endif

/**
 * @brief C99 snprintf on every supported CRT.
 *
 * The VS2013 CRT only has _vsnprintf(), which does not NUL-terminate on
 * truncation and returns -1 instead of the would-be length. Several call
 * sites (notably the HTTP Date writer) rely on the C99 return value to
 * detect truncation, so forwarding to _vsnprintf directly would be wrong.
 */
inline int uvcpp_snprintf(char *buf, std::size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
#if defined(_MSC_VER) && _MSC_VER < 1900
  int r;
  if (n == 0) {
    r = _vscprintf(fmt, ap);
  } else {
    va_list ap_copy = ap;  /* MSVC's va_list is a pointer; copy is enough */
    r = _vsnprintf(buf, n, fmt, ap);
    if (r < 0) {
      r = _vscprintf(fmt, ap_copy);
      buf[n - 1] = '\0';
    }
  }
#else
  const int r = std::vsnprintf(buf, n, fmt, ap);
#endif
  va_end(ap);
  return r;
}

#endif /* SRC_UVCPP_UVCPP_COMPAT_H */
