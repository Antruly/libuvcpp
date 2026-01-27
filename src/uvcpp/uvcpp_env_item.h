/**
 * @file src/uvcpp/uvcpp_env_item.h
 * @brief Wrapper for libuv environment APIs (uv_env_item_t).
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_ENV_ITEM_H
#define SRC_UVCPP_UVCPP_ENV_ITEM_H

#include <uvcpp/uv_define.h>

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 30
#include <uvcpp/uvcpp_define.h>
namespace uvcpp {
class UVCPP_API uvcpp_env_item {
public:
  UVCPP_DEFINE_FUNC(uvcpp_env_item)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_env_item)

  /** @brief Initialize internal resources. */
  int init();
  /** @brief Retrieve environment variables; returns count via \p count. */
  int os_environ(int *count);
  /** @brief Free environment list obtained from os_environ. */
  void free_environ(int count);
  /** @brief Get an environment variable into provided buffer. */
  int getenv(const char *name, char *buffer, size_t *size);
  /** @brief Set an environment variable. */
  int setenv(const char *name, const char *value);
  /** @brief Unset an environment variable. */
  int unsetenv(const char *name);

private:
  uv_env_item_t *env_item = nullptr;
};
} // namespace uvcpp
#endif
#endif

#endif // SRC_UVCPP_UVCPP_ENV_ITEM_H