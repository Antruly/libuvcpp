#pragma once
#ifndef SRC_UVCPP_UVCPP_UTSNAME_H
#define SRC_UVCPP_UVCPP_UTSNAME_H

#include <uvcpp/uv_define.h>

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 25
#include <uvcpp/uvcpp_define.h>

namespace uvcpp {
/**
 * @brief Wrapper for uv_utsname_t providing system information.
 */
class UVCPP_API uvcpp_utsname {
public:
  UVCPP_DEFINE_FUNC(uvcpp_utsname)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_utsname)

  /** @brief Initialize internal resources. */
  int init();
  /** @brief Get hostname into \p buffer with length in \p size. */
  int get_hostname(char *buffer, size_t *size);
  /** @brief Populate stored utsname data. */
  int uname();

  /** @brief Access the underlying uv_utsname_t. */
  uv_utsname_t *get_utsname() const;

private:
  uv_utsname_t *utsname = nullptr;
};
} // namespace uvcpp
#endif
#endif

#endif // SRC_UVCPP_UVCPP_UTSNAME_H