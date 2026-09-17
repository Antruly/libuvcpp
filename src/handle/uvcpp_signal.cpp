#include "uvcpp_signal.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_signal::uvcpp_signal() : uvcpp_handle() {
  uv_signal_t* signal = uvcpp::uvcpp_alloc<uv_signal_t>();
  this->set_handle(signal, true);
  this->init();
}

uvcpp_signal::~uvcpp_signal() { }

uvcpp_signal::uvcpp_signal(uvcpp_loop *loop) : uvcpp_handle() {
  uv_signal_t* signal = uvcpp::uvcpp_alloc<uv_signal_t>();
  this->set_handle(signal, true);
  this->init(loop);
}

int uvcpp_signal::init() {
  // 已接进循环的句柄不能清零，理由见 uvcpp_handle::reset_handle_state。
  this->reset_handle_state(UVCPP_SIGNAL_HANDLE, sizeof(uv_signal_t));
  return 0;
}
int uvcpp_signal::init(uvcpp_loop *loop) {
  int ret = uv_signal_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_SIGNAL_HANDLE);
  this->set_handle_data();
  return ret;
}

int uvcpp_signal::start(std::function<void(uvcpp_signal *, int)> start_cb, int signum) {
  signal_start_cb = start_cb;
  return uv_signal_start(UVCPP_SIGNAL_HANDLE, callback_start, signum);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 12
int uvcpp_signal::start_oneshot(std::function<void(uvcpp_signal *, int)> start_oneshot_cb,
                          int signum) {
  signal_start_oneshot_cb = start_oneshot_cb;

  return uv_signal_start_oneshot(UVCPP_SIGNAL_HANDLE, callback_start_oneshot,
                                 signum);
}
#endif
#endif

int uvcpp_signal::stop() { return uv_signal_stop(UVCPP_SIGNAL_HANDLE); }

void uvcpp_signal::loadavg(double avg[3]) {
  uv_loadavg(avg);
  return;
}

void uvcpp_signal::callback_start(uv_signal_t *handle, int signum) {
  uvcpp_signal *self = reinterpret_cast<uvcpp_signal *>(handle->data);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 句柄回调会重复触发，所以是拷不是搬。
  auto cb = self->signal_start_cb;
  if (cb) {
    cb(self, signum);
  }
}

void uvcpp_signal::callback_start_oneshot(uv_signal_t *handle, int signum) {
  uvcpp_signal *self = reinterpret_cast<uvcpp_signal *>(handle->data);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 句柄回调会重复触发，所以是拷不是搬。
  auto cb = self->signal_start_oneshot_cb;
  if (cb) {
    cb(self, signum);
  }
}
} // namespace uvcpp