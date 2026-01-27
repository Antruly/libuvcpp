/**
 * @file src/handle/uvcpp_tty.h
 * @brief C++ wrapper for libuv TTY handles (uv_tty_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides terminal control operations such as mode and window size.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_TTY_H
#define SRC_HANDLE_UVCPP_TTY_H

#include <handle/uvcpp_loop.h>

namespace uvcpp {
/**
 * @brief TTY handle wrapper exposing terminal operations.
 */
class UVCPP_API uvcpp_tty : public uvcpp_handle {
public:
  UVCPP_DEFINE_FUNC(uvcpp_tty)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_tty)
  /** @brief Construct TTY wrapper on \p loop for file descriptor \p fd. */
  uvcpp_tty(uvcpp_loop *loop, uv_file fd, int readable);

  /** @brief Initialize TTY resources. */
  int init();
  /** @brief Initialize TTY on a specific loop and fd. */
  int init(uvcpp_loop *loop, uv_file fd, int readable);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 1
  /** @brief Set terminal mode (e.g., normal, raw). */
  int set_mode(uv_tty_mode_t mode);
#endif
#endif

  /** @brief Reset terminal mode to previous state. */
  int reset_mode(void);
  /** @brief Get terminal window size into \p width and \p height. */
  int get_winsize(int *width, int *height);

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 33
  /** @brief Set virtual terminal state (if supported). */
  void set_vterm_state(uv_tty_vtermstate_t state);
  /** @brief Get virtual terminal state. */
  int get_vterm_state(uv_tty_vtermstate_t *state);
#endif
#endif
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_TTY_H