#include "uvcpp_idle.h"
#include <uvcpp/uvcpp_alloc.h>

namespace uvcpp {
uvcpp_idle::uvcpp_idle() : uvcpp_handle() {
  uv_idle_t *idle = uvcpp::uvcpp_alloc<uv_idle_t>();
  this->set_handle(idle, true);
  this->init();
}
uvcpp_idle::~uvcpp_idle() {}

uvcpp_idle::uvcpp_idle(uvcpp_loop *loop) : uvcpp_handle() {
  uv_idle_t *idle = uvcpp::uvcpp_alloc<uv_idle_t>();
  this->set_handle(idle, true);
  this->init(loop);
}

int uvcpp_idle::init() {
  // 已接进循环的句柄不能清零，理由见 uvcpp_handle::reset_handle_state。
  this->reset_handle_state(UVCPP_IDLE_HANDLE, sizeof(uv_idle_t));
  return 0;
}

int uvcpp_idle::init(uvcpp_loop *loop) {
  int ret = uv_idle_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_IDLE_HANDLE);
  this->set_handle_data();
  return ret;
}

int uvcpp_idle::start() { return uv_idle_start(UVCPP_IDLE_HANDLE, nullptr); }

int uvcpp_idle::start(std::function<void(uvcpp_idle *)> start_cb) {
  idle_start_cb = start_cb;

  return uv_idle_start(UVCPP_IDLE_HANDLE, callback_start);
}

int uvcpp_idle::stop() {
  if (!this->is_closing()) {
    this->close();
  }
  return 0;
}

int uvcpp_idle::stop(std::function<void(uvcpp_idle *)> stop_cb) {
  idle_stop_cb = stop_cb;
  if (!this->is_closing()) {
    this->close(std::bind(&uvcpp_idle::callback_stop, this, std::placeholders::_1));
  }
  return 0;
}

void uvcpp_idle::callback_start(uv_idle_t *handle) {
  uvcpp_idle *self = reinterpret_cast<uvcpp_idle *>(handle->data);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 句柄回调会重复触发，所以是拷不是搬。
  auto cb = self->idle_start_cb;
  if (cb) {
    cb(self);
  }
}

void uvcpp_idle::callback_stop(uvcpp_handle *handle) {
  uvcpp_idle *self = reinterpret_cast<uvcpp_idle *>(handle);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 句柄回调会重复触发，所以是拷不是搬。
  auto cb = self->idle_stop_cb;
  if (cb) {
    cb(self);
  }
}
} // namespace uvcpp