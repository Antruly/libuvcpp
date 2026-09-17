#include "uvcpp_check.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_check::uvcpp_check() : uvcpp_handle() {
  uv_check_t *check = uvcpp::uvcpp_alloc<uv_check_t>();
  this->set_handle(check, true);
  init();
}

uvcpp_check::~uvcpp_check() {}

uvcpp_check::uvcpp_check(uvcpp_loop *loop) : uvcpp_handle() {
  uv_check_t *check = uvcpp::uvcpp_alloc<uv_check_t>();
  this->set_handle(check, true);
  this->init(loop);
}

int uvcpp_check::init() {
  // 已接进循环的句柄不能清零，理由见 uvcpp_handle::reset_handle_state。
  this->reset_handle_state(UVCPP_CHECK_HANDLE, sizeof(uv_check_t));
  return 0;
}

int uvcpp_check::init(uvcpp_loop *loop) {
  int ret = uv_check_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_CHECK_HANDLE);
  this->set_handle_data();
  return ret;
}

int uvcpp_check::start() { return uv_check_start(UVCPP_CHECK_HANDLE, nullptr); }

int uvcpp_check::start(::std::function<void(uvcpp_check *)> start_cb) {
  check_start_cb = start_cb;
  return uv_check_start(UVCPP_CHECK_HANDLE, callback_start);
}

int uvcpp_check::stop() { return uv_check_stop(UVCPP_CHECK_HANDLE); }

void uvcpp_check::callback_start(uv_check_t *handle) {
  uvcpp_check *self = reinterpret_cast<uvcpp_check *>(handle->data);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 句柄回调会重复触发，所以是拷不是搬。
  auto cb = self->check_start_cb;
  if (cb) {
    cb(self);
  }
}
} // namespace uvcpp