#include "uvcpp_random.h"
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_threadpool.h>
namespace uvcpp {
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 33
uvcpp_random::uvcpp_random() : uvcpp_req() {
  uv_random_t* req = uvcpp::uvcpp_alloc<uv_random_t>();
  this->set_req(req);
  this->init();
}
uvcpp_random::~uvcpp_random() {}

int uvcpp_random::init() {
  memset(UVCPP_RANDOM_REQ, 0, sizeof(uv_random_t));
  this->set_req_data();
  return 0;
}

int uvcpp_random::random(uvcpp_loop* loop, void* buf, size_t buflen, unsigned flags,
             ::std::function<void(uvcpp_random*, int, void*, size_t)> random_cb) {
  m_random_cb = random_cb;
  // 本库的池账：这一笔会把活儿送进 libuv 线程池（见 uvcpp_threadpool.h）
  uvcpp_threadpool_note_use();
  return uv_random(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_RANDOM_REQ, buf, buflen, flags, callback_random);
}

void uvcpp_random::callback_random(uv_random_t* req, int status, void* buf, size_t buflen) {
  uvcpp_random* self = reinterpret_cast<uvcpp_random*>(req->data);
  if (self == nullptr) {
    return;
  }
  // 搬闭包再调用：回调里常见最后一句 `delete self`，而那个闭包就存在
  // `m_random_cb` 里 —— 不搬走的话，删掉的是**此刻正在执行**的这个
  // std::function（连同它的捕获），是未定义行为。见 uvcpp_req::invoke_completion。
  invoke_completion(self->m_random_cb, self, status, buf, buflen);
}

#endif
#endif

} // namespace uvcpp