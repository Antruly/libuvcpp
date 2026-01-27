#include "uvcpp_signal.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_signal::uvcpp_signal() : uvcpp_handle() {
  uv_signal_t* signal = uvcpp::uv_alloc<uv_signal_t>();
  this->set_handle(signal, true);
  this->init();
}

uvcpp_signal::~uvcpp_signal() { }

uvcpp_signal::uvcpp_signal(uvcpp_loop *loop) : uvcpp_handle() {
  uv_signal_t* signal = uvcpp::uv_alloc<uv_signal_t>();
  this->set_handle(signal, true);
  this->init(loop);
}

int uvcpp_signal::init() {
  memset(UVCPP_SIGNAL_HANDLE, 0, sizeof(uv_signal_t));
  this->set_handle_data();
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
  if (reinterpret_cast<uvcpp_signal *>(handle->data)->signal_start_cb)
    reinterpret_cast<uvcpp_signal *>(handle->data)
        ->signal_start_cb(reinterpret_cast<uvcpp_signal *>(handle->data), signum);
}

void uvcpp_signal::callback_start_oneshot(uv_signal_t *handle, int signum) {
  if (reinterpret_cast<uvcpp_signal *>(handle->data)->signal_start_oneshot_cb)
    reinterpret_cast<uvcpp_signal *>(handle->data)
        ->signal_start_oneshot_cb(reinterpret_cast<uvcpp_signal *>(handle->data),
                                  signum);
}
} // namespace uvcpp