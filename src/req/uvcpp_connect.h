/**
 * @file src/req/uvcpp_connect.h
 * @brief Wrapper for uv_connect_t to perform asynchronous connect operations.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_REQ_UVCPP_CONNECT_H
#define SRC_REQ_UVCPP_CONNECT_H

#include <req/uvcpp_req.h>
#include <handle/uvcpp_stream.h>

namespace uvcpp {
/**
 * @brief Connect request wrapper used to connect streams (TCP/pipe).
 */
class UVCPP_API uvcpp_connect : public uvcpp_req {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_connect)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_connect)

  /** @brief Initialize the connect request. */
  int init();
  /** @brief Get an internal stream used for the connect operation (if any). */
  uvcpp_stream* get_connect_stream();

  ::std::function<void(uvcpp_connect*, int)> m_connect_cb;
 public:
  /** @brief libuv callback forwarded to m_connect_cb. */
  static void callback_connect(uv_connect_t* req, int status) {
    if (reinterpret_cast<uvcpp_connect*>(req->data)->m_connect_cb)
      reinterpret_cast<uvcpp_connect*>(req->data)->m_connect_cb(
          reinterpret_cast<uvcpp_connect*>(req->data), status);
  }
  
 protected:
  
 private:
};

} // namespace uvcpp

#endif // SRC_REQ_UVCPP_CONNECT_H