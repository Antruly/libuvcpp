/**
 * @file src/uvcpp/uvcpp_dirent.h
 * @brief Wrapper for libuv directory entry structure (uv_dirent_t).
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_DIRENT_H
#define SRC_UVCPP_UVCPP_DIRENT_H

#include <uvcpp/uv_define.h>
#include <uvcpp/uvcpp_define.h>

namespace uvcpp {
class UVCPP_API uvcpp_dirent  {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_dirent)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_dirent)

  /** @brief Initialize internal dirent structure. */
  int init();

  /** @brief Access underlying uv_dirent_t pointer. */
  uv_dirent_t* get_dirent();

 private:
  uv_dirent_t* dirent = nullptr;
};
} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_DIRENT_H
