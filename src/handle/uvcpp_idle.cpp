#include "uvcpp_idle.h"
#include <uvcpp/uv_alloc.h>

namespace uvcpp {
uvcpp_idle::uvcpp_idle() : uvcpp_handle() {
  uv_idle_t *idle = uvcpp::uv_alloc<uv_idle_t>();
  this->set_handle(idle, true);
  this->init();
}
uvcpp_idle::~uvcpp_idle() {}

uvcpp_idle::uvcpp_idle(uvcpp_loop *loop) : uvcpp_handle() {
  uv_idle_t *idle = uvcpp::uv_alloc<uv_idle_t>();
  this->set_handle(idle, true);
  this->init(loop);
}

int uvcpp_idle::init() {
  memset(UVCPP_IDLE_HANDLE, 0, sizeof(uv_idle_t));
  this->set_handle_data();
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
  if (reinterpret_cast<uvcpp_idle *>(handle->data)->idle_start_cb)
    reinterpret_cast<uvcpp_idle *>(handle->data)
        ->idle_start_cb(reinterpret_cast<uvcpp_idle *>(handle->data));
}

void uvcpp_idle::callback_stop(uvcpp_handle *handle) {
  if (reinterpret_cast<uvcpp_idle *>(handle)->idle_stop_cb)
    reinterpret_cast<uvcpp_idle *>(handle)->idle_stop_cb(
        reinterpret_cast<uvcpp_idle *>(handle));
}
} // namespace uvcpp