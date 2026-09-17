#include "uvcpp_async.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_async::uvcpp_async() : uvcpp_handle() {
  uv_async_t *async = uvcpp::uvcpp_alloc<uv_async_t>();
  this->set_handle(async, true);
  this->init();
}
uvcpp_async::~uvcpp_async() {}

uvcpp_async::uvcpp_async(uvcpp_loop *loop) : uvcpp_handle() {
  uv_async_t *async = uvcpp::uvcpp_alloc<uv_async_t>();
  this->set_handle(async, true);
  this->init(loop);
}

int uvcpp_async::init() {
  // 已接进循环的句柄不能清零，理由见 uvcpp_handle::reset_handle_state。
  this->reset_handle_state(UVCPP_ASYNC_HANDLE, sizeof(uv_async_t));
  return 0;
}

int uvcpp_async::init(uvcpp_loop *loop) {
  int ret = uv_async_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_ASYNC_HANDLE, nullptr);
  this->set_handle_data();
  return ret;
}

int uvcpp_async::init(::std::function<void(uvcpp_async *)> init_cb, uvcpp_loop *loop) {
  async_init_cb = init_cb;
  return uv_async_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_ASYNC_HANDLE,
                       callback_init);
}

void uvcpp_async::callback_init(uv_async_t *handle) {
  uvcpp_async *self = reinterpret_cast<uvcpp_async *>(handle->data);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 句柄回调会重复触发，所以是拷不是搬。
  auto cb = self->async_init_cb;
  if (cb) {
    cb(self);
  }
}


} // namespace uvcpp