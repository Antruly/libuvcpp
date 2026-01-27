/**
 * @file src/req/uvcpp_getaddrinfo.h
 * @brief Wrapper for uv_getaddrinfo_t to perform asynchronous DNS lookups.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_REQ_UVCPP_GETADDRINFO_H
#define SRC_REQ_UVCPP_GETADDRINFO_H

#include <handle/uvcpp_loop.h>
#include <req/uvcpp_req.h>

namespace uvcpp {
/**
 * @brief Asynchronous getaddrinfo request wrapper.
 *
 * Use `getaddrinfo()` to resolve `node`/`service` into `addrinfo` results
 * asynchronously. Call `freeaddrinfo()` to release results.
 */
class UVCPP_API uvcpp_getaddrinfo : public uvcpp_req {
public:
  UVCPP_DEFINE_FUNC(uvcpp_getaddrinfo)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_getaddrinfo)

  /** @brief Initialize the request object. */
  int init();
  /**
   * @brief Perform an asynchronous getaddrinfo lookup.
   * @param loop Loop to run the request on.
   * @param node Hostname or address string.
   * @param service Service name or port string.
   * @param hints Optional addrinfo hints.
   * @param getaddrinfo_cb Completion callback invoked with status and results.
   */
  int getaddrinfo(
      uvcpp_loop *loop, const char *node, const char *service,
      const struct addrinfo *hints,
      ::std::function<void(uvcpp_getaddrinfo *, int, struct addrinfo *)>
          getaddrinfo_cb);

  /** @brief Free results returned by libuv. */
  void freeaddrinfo(struct addrinfo *ai) { uv_freeaddrinfo(ai); }

protected:
  ::std::function<void(uvcpp_getaddrinfo *, int, struct addrinfo *)>
      m_getaddrinfo_cb;

private:
  /** @brief Internal libuv callback forwarding to m_getaddrinfo_cb. */
  static void callback_getaddrinfo(uv_getaddrinfo_t *req, int status,
                                   struct addrinfo *res);
};

} // namespace uvcpp

#endif // SRC_REQ_UVCPP_GETADDRINFO_H