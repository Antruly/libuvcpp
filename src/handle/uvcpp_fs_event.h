/**
 * @file src/handle/uvcpp_fs_event.h
 * @brief C++ wrapper for libuv fs-event handles (uv_fs_event_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides notifications for filesystem events (file changes).
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_FS_EVENT_H
#define SRC_HANDLE_UVCPP_FS_EVENT_H

#include <handle/uvcpp_loop.h>

namespace uvcpp {
/**
 * @brief Filesystem event watcher wrapper.
 */
class UVCPP_API uvcpp_fs_event : public uvcpp_handle {
public:
  UVCPP_DEFINE_FUNC(uvcpp_fs_event)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_fs_event)
  /** @brief Construct fs-event watcher on \p loop. */
  uvcpp_fs_event(uvcpp_loop *loop);

  /** @brief Initialize watcher. */
  int init();
  /** @brief Initialize on provided loop. */
  int init(uvcpp_loop *loop);
  /** @brief Start watching \p path with \p flags. */
  int start(const char *path, unsigned int flags);
  /** @brief Start watching with a callback receiving filename, events and status. */
  int start(::std::function<void(uvcpp_fs_event *, const char *, int, int)> start_cb,
            const char *path, unsigned int flags);
  /** @brief Stop watching. */
  int stop();
  /** @brief Get the monitored path into \p buffer. */
  int getpath(char *buffer, size_t *size);

protected:
  ::std::function<void(uvcpp_fs_event *, const char *, int, int)> fs_event_start_cb;

private:
  /** @brief Internal libuv fs-event callback. */
  static void callback_start(uv_fs_event_t *handle, const char *filename,
                             int events, int status);

private:
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_FS_EVENT_H