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
    uvcpp_work* self = reinterpret_cast<uvcpp_work*>(req->data);
    if (self == nullptr) {
      return;
    }
    // 搬闭包再调用，理由见 uvcpp_req::invoke_completion：回调里 `delete self`
    // 会把**此刻正在执行**的这个 std::function 连同捕获一起拆掉。
    // 这里不走 invoke_completion —— 它按 `self_free_` 删对象，而 work 回调跑在
    // **工作线程**上，对象必须活到 after_work，不该在这里被回收。
    ::std::function<void(uvcpp_work*)> cb = ::std::move(self->m_work_cb);
    // 搬完显式清空源：移动后源只是"有效但未指定"，libc++ 的 SBO 分支不清它，
    // 详见 uvcpp_req::invoke_completion 里的同一处说明。
    self->m_work_cb = nullptr;
    if (cb) {
      cb(self);
    }
  }
  static void callback_after_work(uv_work_t* req, int status) {
    uvcpp_work* self = reinterpret_cast<uvcpp_work*>(req->data);
    if (self == nullptr) {
      return;
    }
    // 同上。这一条是**根因**：少了它，任何在 after_work 里 `delete work` 的
    // 调用点都会踩到已释放的闭包。web_static 当初是用 `retire()` 绕过去的
    // （见 uvcpp_web_static.cpp「绝对不能在回调里 delete w」），绕的正是这里。
    invoke_completion(self->m_after_work_cb, self, status);
  }

 private:
  uvcpp_loop * loop = nullptr;
};
} // namespace uvcpp

#endif // SRC_REQ_UVCPP_WORK_H
