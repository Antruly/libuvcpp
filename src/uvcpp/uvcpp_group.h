/**
 * @file src/uvcpp/uvcpp_group.h
 * @brief Wrapper for uv_group_t to query group information.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_GROUP_H
#define SRC_UVCPP_UVCPP_GROUP_H

#include <uvcpp/uvcpp_define.h>

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 45
namespace uvcpp {
class UVCPP_API uvcpp_group {
public:
  UVCPP_DEFINE_FUNC(uvcpp_group)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_group)

  /** @brief Initialize group wrapper. */
  int init();
  /** @brief Retrieve OS group information for gid. */
  int get_os_group(uv_uid_t gid);
  /** @brief Free allocated group resources. */
  void free_group();

private:
  uv_group_t *group = nullptr;
};
} // namespace uvcpp
#endif
#endif

#endif // SRC_UVCPP_UVCPP_GROUP_H
