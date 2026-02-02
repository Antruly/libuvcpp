/**
 * @file src/uvcpp/uvcpp_statfs.h
 * @brief Wrapper for uv_statfs_t (filesystem statistics).
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_STATFS_H
#define SRC_UVCPP_UVCPP_STATFS_H

#include <uvcpp/uvcpp_define.h>

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 29

namespace uvcpp {
class UVCPP_API uvcpp_statfs {
public:
  UVCPP_DEFINE_FUNC(uvcpp_statfs)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_statfs)

  /** @brief Initialize statfs request/structure. */
  int init();

  /** @brief Access underlying uv_statfs_t data. */
  uv_statfs_t *get_statfs() const;

private:
  uv_statfs_t *statfs = nullptr;
};
} // namespace uvcpp
#endif
#endif

#endif // SRC_UVCPP_UVCPP_STATFS_H