#include "uvcpp_prepare.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_prepare::uvcpp_prepare() : uvcpp_handle() {
  uv_prepare_t* prepare = uvcpp::uv_alloc<uv_prepare_t>();
  this->set_handle(prepare, true);
  this->init();
}
uvcpp_prepare::uvcpp_prepare(uvcpp_loop* loop) : uvcpp_handle() {
  uv_prepare_t* prepare = uvcpp::uv_alloc<uv_prepare_t>();
  this->set_handle(prepare, true);
  this->init(loop);
}

uvcpp_prepare::~uvcpp_prepare() {}

int uvcpp_prepare::init() {
  memset(UVCPP_PREPARE_HANDLE, 0, sizeof(uv_prepare_t));
  this->set_handle_data();
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
  if (reinterpret_cast<uvcpp_prepare*>(handle->data)->prepare_start_cb)
    reinterpret_cast<uvcpp_prepare*>(handle->data)
        ->prepare_start_cb(reinterpret_cast<uvcpp_prepare*>(handle->data));
}

 } // namespace uvcpp