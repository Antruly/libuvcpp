#include "uvcpp_work.h"
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_threadpool.h>
// `queue_work` 是本库直投 libuv 线程池的入口，投之前记一笔池账 —— 池子的线程数
// 就是**在这条路上第一次被读进缓存的**（`src/threadpool.c:200` 的 `uv_once`）。
// 见 uvcpp_threadpool.h。
namespace uvcpp {
uvcpp_work::uvcpp_work() : uvcpp_req(),loop(nullptr) {
  uv_work_t* work = uvcpp::uvcpp_alloc<uv_work_t>();
  this->set_req(work);
  this->init();
}

uvcpp_work::~uvcpp_work() {  }
int uvcpp_work::init() {
  memset(UVCPP_WORK_REQ, 0, sizeof(uv_work_t));
  this->set_req_data();
  return 0;
}
int uvcpp_work::queue_work(uvcpp_loop *lp,
                      ::std::function<void(uvcpp_work *)> work_cb,
                      ::std::function<void(uvcpp_work *, int)> after_work_cb) {
  if (lp == nullptr) {
    throw "error queue_work loop is nullptr";
  }
  if (loop != nullptr) {
    throw "error,local loop had value";
  }
  loop = lp;  
  m_work_cb = work_cb;
  m_after_work_cb = after_work_cb;
  // 本库的池账：这一笔会把活儿送进 libuv 线程池（见 uvcpp_threadpool.h）
  uvcpp_threadpool_note_use();
  return uv_queue_work(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_WORK_REQ,
                       callback_work, callback_after_work);
}
uvcpp_loop *uvcpp_work::get_loop() { return loop; }
} // namespace uvcpp