#include "uvcpp_timer.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_timer::uvcpp_timer() : uvcpp_handle() {
  uv_timer_t* timer = uvcpp::uv_alloc<uv_timer_t>();
  this->set_handle(timer, true);
  this->init();
}
uvcpp_timer::uvcpp_timer(uvcpp_loop *loop) : uvcpp_handle() {
  uv_timer_t* timer = uvcpp::uv_alloc<uv_timer_t>();
  this->set_handle(timer, true);
  this->init(loop);
}

uvcpp_timer::~uvcpp_timer() {}

int uvcpp_timer::init() {
  memset(UVCPP_TIMER_HANDLE, 0, sizeof(uv_timer_t));
  this->set_handle_data();
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
  if (reinterpret_cast<uvcpp_timer *>(handle->data)->timer_start_cb)
    reinterpret_cast<uvcpp_timer *>(handle->data)
        ->timer_start_cb(reinterpret_cast<uvcpp_timer *>(handle->data));
}
} // namespace uvcpp