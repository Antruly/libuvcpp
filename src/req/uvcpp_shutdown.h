/**
 * @file src/req/uvcpp_shutdown.h
 * @brief Wrapper for uv_shutdown_t to gracefully shutdown stream writes.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_REQ_UVCPP_SHUTDOWN_H
#define SRC_REQ_UVCPP_SHUTDOWN_H

#include <handle/uvcpp_stream.h>
#include <req/uvcpp_req.h>

namespace uvcpp {
/**
 * @brief Shutdown request wrapper used to close the write side of a stream.
 */
class UVCPP_API uvcpp_shutdown : public uvcpp_req {
public:
    UVCPP_DEFINE_FUNC(uvcpp_shutdown)
    UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_shutdown)

    /** @brief Initialize the shutdown request. */
    int init();
    
    /**
     * @brief Initiate a graceful shutdown of stream writes.
     * @param handle Stream to shutdown.
     * @param shutdown_cb Completion callback invoked with status.
     */
    int shutdown(uvcpp_stream* handle,
                 ::std::function<void(uvcpp_shutdown*, int)> shutdown_cb)
    {
      m_shutdown_cb = shutdown_cb;
      return uv_shutdown(UVCPP_SHUTDOWN_REQ, OBJ_UVCPP_STREAM_HANDLE(*handle),
                         callback_shutdown);
    }

   protected:
    ::std::function<void(uvcpp_shutdown*, int)> m_shutdown_cb;
 private:
    
  static void callback_shutdown(uv_shutdown_t* req, int status) {
      if (reinterpret_cast<uvcpp_shutdown*>(req->data)->m_shutdown_cb)
        reinterpret_cast<uvcpp_shutdown*>(req->data)->m_shutdown_cb(
            reinterpret_cast<uvcpp_shutdown*>(req->data), status);
    }

   private:

};

} // namespace uvcpp

#endif // SRC_REQ_UVCPP_SHUTDOWN_H
