/**
 * @file src/uvcpp/uvcpp_passwd.h
 * @brief Wrapper for uv_passwd_t to obtain user account information from the OS.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_PASSWD_H
#define SRC_UVCPP_UVCPP_PASSWD_H

#include <uvcpp/uv_define.h>

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 9
#include <uvcpp/uvcpp_define.h>
namespace uvcpp {
class UVCPP_API uvcpp_passwd {
public:
  UVCPP_DEFINE_FUNC(uvcpp_passwd)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_passwd)

  /** @brief Initialize internal resources. */
  int init();

  /** @brief Load the current process user password information. */
  int get_os_passwd();
#if UV_VERSION_MINOR >= 45
  /** @brief Load passwd for specific uid (if supported). */
  int get_os_passwd(uv_uid_t uid);
#endif
  /** @brief Free stored passwd resources. */
  void free_passwd();

  /** @brief Access underlying uv_passwd_t. */
  uv_passwd_t *get_passwd() const;

private:
  uv_passwd_t *passwd = nullptr;
};
} // namespace uvcpp
#endif
#endif

#endif // SRC_UVCPP_UVCPP_PASSWD_H

