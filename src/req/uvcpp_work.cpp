#include "uvcpp_work.h"
#include <uvcpp/uvcpp_alloc.h>
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
  return uv_queue_work(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_WORK_REQ,
                       callback_work, callback_after_work);
}
uvcpp_loop *uvcpp_work::get_loop() { return loop; }
} // namespace uvcpp