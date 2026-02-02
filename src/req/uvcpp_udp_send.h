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
  /** @brief Set the main buffer to write; if owner=true wrapper will free it.
   */
  void set_uv_buf(uv_buf_t *bf, bool owner = false);
  /** @brief Get the buffer previously set. */
  uv_buf_t *get_uv_buf();
  /** @brief Set a source buffer (used for send_handle scenarios). */
  void set_src_buf(const uvcpp_buf *bf, bool owner = false);
  /** @brief Get the source buffer previously set. */
  const uvcpp_buf *get_src_buf();
  /** @brief User-provided callback invoked when send completes. */
  ::std::function<void(uvcpp_udp_send*, int)> udp_send_cb;

 public:
  /** @brief libuv completion callback forwarded to the wrapper. */
  static void callback_udp_send(uv_udp_send_t* req, int status);

 private:
  uv_buf_t *uv_buf = nullptr;
  const uvcpp_buf *src_buf = nullptr;
  bool uv_buf_owner = false;
  bool src_buf_owner = false;
};

} // namespace uvcpp

#endif // SRC_REQ_UVCPP_UDP_SEND_H