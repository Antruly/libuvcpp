/**
 * @file src/req/uvcpp_work.h
 * @brief Work request wrapper for libuv thread-pool work (uv_work_t).
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_REQ_UVCPP_WORK_H
#define SRC_REQ_UVCPP_WORK_H

#include <handle/uvcpp_loop.h>
#include <req/uvcpp_req.h>

namespace uvcpp {
/**
 * @brief Wrapper for uv_work_t to queue work on libuv's thread pool.
 *
 * Use `queue_work` to schedule background work and provide an after-work
 * callback that runs on the loop thread.
 */
class UVCPP_API uvcpp_work : public uvcpp_req {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_work)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_work)

  /** @brief Initialize the work request. */
  int init();

  /**
   * @brief Queue work on the libuv thread pool.
   * @param lp Loop to queue work on.
   * @param work_cb Work callback executed on a worker thread.
   * @param after_work_cb Completion callback executed on the loop thread.
   */
  int queue_work(uvcpp_loop* lp,
                ::std::function<void(uvcpp_work*)> work_cb,
                ::std::function<void(uvcpp_work *, int)> after_work_cb);
  /** @brief Return the loop associated with this request. */
  uvcpp_loop *get_loop();
  
  ::std::function<void(uvcpp_work*)> m_work_cb;
  ::std::function<void(uvcpp_work*, int)> m_after_work_cb;

 private:
  static void callback_work(uv_work_t* req) {
    if (reinterpret_cast<uvcpp_work*>(req->data)->m_work_cb)
      reinterpret_cast<uvcpp_work*>(req->data)->m_work_cb(
          reinterpret_cast<uvcpp_work*>(req->data));
  }
  static void callback_after_work(uv_work_t* req, int status) {
    if (reinterpret_cast<uvcpp_work*>(req->data)->m_after_work_cb)
      reinterpret_cast<uvcpp_work*>(req->data)->m_after_work_cb(
          reinterpret_cast<uvcpp_work*>(req->data), status);
  }

 private:
  uvcpp_loop * loop = nullptr;
};
} // namespace uvcpp

#endif // SRC_REQ_UVCPP_WORK_H
