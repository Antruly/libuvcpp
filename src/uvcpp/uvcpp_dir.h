/**
 * @file src/uvcpp/uvcpp_dir.h
 * @brief Wrapper for libuv directory iteration APIs (uv_dir_t).
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_DIR_H
#define SRC_UVCPP_UVCPP_DIR_H

#include <uvcpp/uv_define.h>

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 28
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_dirent.h>

namespace uvcpp {
class UVCPP_API uvcpp_dir  {
public:
  UVCPP_DEFINE_FUNC(uvcpp_dir)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_dir)

  /** @brief Construct directory wrapper from a dirent helper. */
  uvcpp_dir(uvcpp_dirent *dr);
  /** @brief Initialize internal directory handle. */
  int init();

  /** @brief Set a dirent helper to use with this directory. */
  void set_dirent(uvcpp_dirent *dr);
  /** @brief Access underlying uv_dir_t pointer. */
  uv_dir_t *get_dir();

protected:
private:
  uv_dir_t *_dir = nullptr;
};
} // namespace uvcpp
#endif
#endif

#endif // SRC_UVCPP_UVCPP_DIR_H