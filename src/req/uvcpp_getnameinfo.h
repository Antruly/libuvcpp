/**
 * @file src/req/uvcpp_getnameinfo.h
 * @brief Wrapper for uv_getnameinfo_t to perform reverse DNS lookups.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_REQ_UVCPP_GETNAMEINFO_H
#define SRC_REQ_UVCPP_GETNAMEINFO_H

#include <handle/uvcpp_loop.h>
#include <req/uvcpp_req.h>

namespace uvcpp {
/**
 * @brief Reverse DNS lookup request wrapper.
 *
 * Call `getnameinfo()` to initiate a hostname/service lookup for a sockaddr.
 */
class UVCPP_API uvcpp_getnameinfo : public uvcpp_req {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_getnameinfo)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_getnameinfo)

  /** @brief Initialize the getnameinfo request. */
  int init();
  /**
   * @brief Perform reverse name lookup for `addr`.
   * @param loop Loop to execute on.
   * @param addr Address to reverse-resolve.
   * @param flags Flags for the lookup.
   * @param getnameinfo_cb Completion callback with status, hostname and service.
   */
  int getnameinfo(uvcpp_loop* loop, const struct sockaddr* addr, int flags,
                  ::std::function<void(uvcpp_getnameinfo*, int, const char*, const char*)> getnameinfo_cb);

  ::std::function<void(uvcpp_getnameinfo *, int, const char *, const char *)> m_getnameinfo_cb;

  /** @brief Internal libuv callback forwarded to `m_getnameinfo_cb`. */
  static void callback_getnameinfo(uv_getnameinfo_t* req, int status,
                                   const char* hostname,
                                   const char* service);
};

} // namespace uvcpp

#endif // SRC_REQ_UVCPP_GETNAMEINFO_H