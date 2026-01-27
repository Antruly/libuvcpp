/**
 * @file src/req/uvcpp_udp_send.h
 * @brief UDP send request wrapper (uv_udp_send_t).
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_REQ_UVCPP_UDP_SEND_H
#define SRC_REQ_UVCPP_UDP_SEND_H

#include <req/uvcpp_req.h>

namespace uvcpp {
/**
 * @brief Wrapper for uv_udp_send_t to perform asynchronous UDP sends.
 *
 * Use `init()` to initialize the request, then call the stream's send
 * method with this request and provide a completion callback.
 */
class UVCPP_API uvcpp_udp_send : public uvcpp_req {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_udp_send)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_udp_send)

  /** @brief Initialize the udp_send request. */
  int init();

  /** @brief User-provided callback invoked when send completes. */
  ::std::function<void(uvcpp_udp_send*, int)> udp_send_cb;

 public:
  /** @brief libuv completion callback forwarded to the wrapper. */
  static void callback_udp_send(uv_udp_send_t* req, int status);

 private:
};

} // namespace uvcpp

#endif // SRC_REQ_UVCPP_UDP_SEND_H