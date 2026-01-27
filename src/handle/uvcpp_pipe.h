/**
 * @file src/handle/uvcpp_pipe.h
 * @brief C++ wrapper for libuv pipe handles (uv_pipe_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * This header provides the `uvcpp_pipe` class which wraps platform pipes
 * and exposes common operations such as binding, connecting and querying
 * peer/local names.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_PIPE_H
#define SRC_HANDLE_UVCPP_PIPE_H

#include <handle/uvcpp_stream.h>
#include <req/uvcpp_connect.h>

namespace uvcpp {
/**
 * @brief Wrapper around libuv uv_pipe_t for pipe (named pipe / IPC) operations.
 *
 * Inherits from `uvcpp_stream` and exposes pipe-specific APIs.
 */
class UVCPP_API uvcpp_pipe : public uvcpp_stream {
public:
  UVCPP_DEFINE_FUNC(uvcpp_pipe)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_pipe)
  /** @brief Construct a pipe wrapper tied to \p loop; \p pic indicates pipe type. */
  uvcpp_pipe(uvcpp_loop *loop, int pic);

  /** @brief Initialize pipe handle. */
  int init();
  /** @brief Initialize pipe handle on provided loop. */
  int init(uvcpp_loop *loop, int pic);
  /** @brief Open an existing file descriptor as a pipe. */
  int open(uv_file file);
  /** @brief Bind the pipe to a name (named pipe / socket path). */
  int bind(const char *name);
  /** @brief Start a connection to a named pipe. */
  void connect(uvcpp_connect *req, const char *name);
  /** @brief Start a connection with completion callback. */
  void connect(uvcpp_connect *req, const char *name,
               std::function<void(uvcpp_connect *, int)> connect_cb);
  /** @brief Retrieve local socket name; \p buffer receives the name and \p size the length. */
  int getsockname(char *buffer, size_t *size);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 2
  /** @brief Retrieve peer socket name into \p buffer. */
  int getpeername(char *buffer, size_t *size);
#endif
#endif

  /** @brief Set the number of pending instances for the pipe. */
  void pendingInstances(int count);
  /** @brief Get the number of pending instances. */
  int pendingCount();
  /** @brief Get the pending handle type for the pipe. */
  uv_handle_type pendingType();
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 16
  /** @brief Change file mode flags for pipe (if supported). */
  int chmod(int flags);
#endif
#endif
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_PIPE_H