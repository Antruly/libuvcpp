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
  memset(UVCPP_CHECK_HANDLE, 0, sizeof(uv_check_t));
  this->set_handle_data();
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
  if (reinterpret_cast<uvcpp_check *>(handle->data)->check_start_cb)
    reinterpret_cast<uvcpp_check *>(handle->data)
        ->check_start_cb(reinterpret_cast<uvcpp_check *>(handle->data));
}
} // namespace uvcpp