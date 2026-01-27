/**
 * @file src/handle/uvcpp_fs_poll.h
 * @brief C++ wrapper for libuv fs-poll handles (uv_fs_poll_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Polls filesystem metadata at intervals and notifies on changes.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_FS_POLL_H
#define SRC_HANDLE_UVCPP_FS_POLL_H

#include <handle/uvcpp_loop.h>

namespace uvcpp {
/**
 * @brief Filesystem poll wrapper that invokes a callback when file stats change.
 */
class UVCPP_API uvcpp_fs_poll : public uvcpp_handle {
public:
  UVCPP_DEFINE_FUNC(uvcpp_fs_poll)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_fs_poll)
  /** @brief Construct fs-poll watcher bound to \p loop. */
  uvcpp_fs_poll(uvcpp_loop *loop);

  /** @brief Initialize the fs-poll watcher. */
  int init();
  /** @brief Initialize on a specific loop. */
  int init(uvcpp_loop *loop);

  /** @brief Start polling \p path every \p interval milliseconds. */
  int start(const char *path, unsigned int interval);
  /** @brief Start polling with a callback receiving previous and current stat. */
  int start(::std::function<void(uvcpp_fs_poll *, const uv_stat_t *, const uv_stat_t *)>
                start_cb,
            const char *path, unsigned int interval);
  /** @brief Stop polling. */
  int stop();
  /** @brief Get the polled path into \p buffer with size returned in \p size. */
  int getpath(char *buffer, size_t *size);

protected:
  ::std::function<void(uvcpp_fs_poll *, const uv_stat_t *, const uv_stat_t *)>
      fs_poll_start_cb;

private:
  /** @brief Internal fs-poll callback from libuv. */
  static void callback_start(uv_fs_poll_t *handle, int status,
                             const uv_stat_t *prev, const uv_stat_t *curr);

private:
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_FS_POLL_H