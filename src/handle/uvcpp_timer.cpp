#include "uvcpp_timer.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_timer::uvcpp_timer() : uvcpp_handle() {
  uv_timer_t* timer = uvcpp::uvcpp_alloc<uv_timer_t>();
  this->set_handle(timer, true);
  this->init();
}
uvcpp_timer::uvcpp_timer(uvcpp_loop *loop) : uvcpp_handle() {
  uv_timer_t* timer = uvcpp::uvcpp_alloc<uv_timer_t>();
  this->set_handle(timer, true);
  this->init(loop);
}

uvcpp_timer::~uvcpp_timer() {}

int uvcpp_timer::init() {
  // 已接进循环的句柄不能清零，理由见 uvcpp_handle::reset_handle_state。
  this->reset_handle_state(UVCPP_TIMER_HANDLE, sizeof(uv_timer_t));
  return 0;
}

int uvcpp_timer::init(uvcpp_loop *loop) {
  int ret = uv_timer_init((uv_loop_t *)loop->get_handle(),
                          (uv_timer_t *)this->get_handle());
  this->set_handle_data();
  return ret;
}

int uvcpp_timer::start(std::function<void(uvcpp_timer *)> start_cb, uint64_t timeout,
                  uint64_t repeat) {
  timer_start_cb = start_cb;
  return uv_timer_start(UVCPP_TIMER_HANDLE, callback_start, timeout, repeat);
}

int uvcpp_timer::stop() { return uv_timer_stop(UVCPP_TIMER_HANDLE); }

void uvcpp_timer::callback_start(uv_timer_t *handle) {
  uvcpp_timer *self = reinterpret_cast<uvcpp_timer *>(handle->data);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 句柄回调会重复触发，所以是拷不是搬。
  auto cb = self->timer_start_cb;
  if (cb) {
    cb(self);
  }
}
} // namespace uvcpp