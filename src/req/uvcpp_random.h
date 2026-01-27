/**
 * @file src/req/uvcpp_random.h
 * @brief Wrapper for libuv random request (uv_random_t).
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_REQ_UVCPP_RANDOM_H
#define SRC_REQ_UVCPP_RANDOM_H

#include <handle/uvcpp_loop.h>
#include <req/uvcpp_req.h>

namespace uvcpp {
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 33
/**
 * @brief Random number request wrapper (only available on newer libuv).
 *
 * Use `random()` to request random bytes asynchronously.
 */
class UVCPP_API uvcpp_random : public uvcpp_req {
public:
  UVCPP_DEFINE_FUNC(uvcpp_random)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_random)

  /** @brief Initialize the random request. */
  int init();

  /**
   * @brief Request random bytes.
   * @param loop Loop to run on.
   * @param buf Destination buffer.
   * @param buflen Buffer length.
   * @param flags Flags passed to libuv.
   * @param random_cb Completion callback invoked with status and buffer.
   */
  int random(uvcpp_loop *loop, void *buf, size_t buflen, unsigned flags,
             ::std::function<void(uvcpp_random *, int, void *, size_t)> random_cb);

protected:
  ::std::function<void(uvcpp_random *, int, void *, size_t)> m_random_cb;

private:
  static void callback_random(uv_random_t *req, int status, void *buf,
                              size_t buflen);

private:
};
#endif
#endif
} // namespace uvcpp

#endif // SRC_REQ_UVCPP_RANDOM_H