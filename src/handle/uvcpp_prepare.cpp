#include "uvcpp_prepare.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_prepare::uvcpp_prepare() : uvcpp_handle() {
  uv_prepare_t* prepare = uvcpp::uvcpp_alloc<uv_prepare_t>();
  this->set_handle(prepare, true);
  this->init();
}
uvcpp_prepare::uvcpp_prepare(uvcpp_loop* loop) : uvcpp_handle() {
  uv_prepare_t* prepare = uvcpp::uvcpp_alloc<uv_prepare_t>();
  this->set_handle(prepare, true);
  this->init(loop);
}

uvcpp_prepare::~uvcpp_prepare() {}

int uvcpp_prepare::init() {
  // 已接进循环的句柄不能清零，理由见 uvcpp_handle::reset_handle_state。
  this->reset_handle_state(UVCPP_PREPARE_HANDLE, sizeof(uv_prepare_t));
  return 0;
}

int uvcpp_prepare::init(uvcpp_loop* loop) {
  int ret = uv_prepare_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_PREPARE_HANDLE);
  this->set_handle_data();
  return ret;
}

 int uvcpp_prepare::start(::std::function<void(uvcpp_prepare*)> start_cb) {
  prepare_start_cb = start_cb;
  return uv_prepare_start(UVCPP_PREPARE_HANDLE, callback);
}

int uvcpp_prepare::stop() {
  return uv_prepare_stop(UVCPP_PREPARE_HANDLE);
}

 void uvcpp_prepare::callback(uv_prepare_t* handle) {
  uvcpp_prepare *self = reinterpret_cast<uvcpp_prepare *>(handle->data);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 句柄回调会重复触发，所以是拷不是搬。
  auto cb = self->prepare_start_cb;
  if (cb) {
    cb(self);
  }
}

 } // namespace uvcpp