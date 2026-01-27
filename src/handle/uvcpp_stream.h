/**
 * @file src/handle/uvcpp_stream.h
 * @brief C++ wrapper for libuv stream handles (uv_stream_t) and common I/O ops.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides stream operations such as listen, accept, read/write and helpers.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_STREAM_H
#define SRC_HANDLE_UVCPP_STREAM_H

#include <handle/uvcpp_loop.h>
#include <req/uvcpp_write.h>
#include <uvcpp/uv_buf.h>
namespace uvcpp {
/**
 * @brief Wrapper for libuv stream handles providing TCP/pipe stream semantics.
 *
 * This class exposes methods for starting/stopping reads, accepting
 * connections, and performing write operations with callbacks.
 */
class UVCPP_API uvcpp_stream : public uvcpp_handle {
public:
  UVCPP_DEFINE_FUNC(uvcpp_stream)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_stream)

  /** @brief Initialize stream resources; must be called before use. */
  int init();
  /** @brief Start listening for incoming connections. */
  int listen(::std::function<void(uvcpp_stream*, int)> connection_cb, int backlog);
  /** @brief Accept a pending connection into \p client. */
  int accept(uvcpp_stream* client);
  /**
   * @brief Start reading from the stream.
   * @param alloc_cb Allocator callback to produce buffers.
   * @param read_cb Read callback invoked with data or error.
   */
  int read_start(
      ::std::function<void(uvcpp_handle *, size_t, uv_buf *)> alloc_cb,
      ::std::function<void(uvcpp_stream *, ssize_t, const uv_buf *)>
          read_cb);
  /** @brief Stop reading from the stream. */
  int read_stop() { return uv_read_stop(UVCPP_STREAM_HANDLE); }
  /**
   * @brief Queue a write request.
   * @param req Write request wrapper.
   * @param bufs Buffers to write.
   * @param nbufs Number of buffers.
   * @param write_cb Completion callback.
   */
  int write(uvcpp_write *req, const uv_buf bufs[], unsigned int nbufs,
            ::std::function<void(uvcpp_write*, int)> write_cb);
  /**
   * @brief Queue a write request and optionally send a handle.
   * @param send_handle Optional stream whose handle will be sent.
   */
  int write(uvcpp_write *req, const uv_buf bufs[], unsigned int nbufs,
            uvcpp_stream* send_handle,
            ::std::function<void(uvcpp_write*, int)> write_cb);

  /** @brief Try to write immediately without queuing. */
  int try_write(const uv_buf bufs[], unsigned int nbufs);
  int try_write(const uv_buf bufs[], unsigned int nbufs,
               uvcpp_stream *send_handle);

  /** @brief Return non-zero if stream is readable. */
  int is_readable();
  /** @brief Return non-zero if stream is writable. */
  int is_writable();
  /** @brief Set blocking/non-blocking mode on the stream. */
  int stream_set_blocking(int blocking);

protected:
  std::function<void(uvcpp_stream *, int)> stream_connection_cb;
  std::function<void(uvcpp_stream *, ssize_t, const uv_buf *)>
      stream_read_cb;

private:
  /** @brief Internal callback for new connections from libuv. */
  static void callback_connection(uv_stream_t* handle, int status);
  /** @brief Internal callback for read events from libuv. */
  static void callback_read(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf);
};
} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_STREAM_H