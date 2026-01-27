#include "uvcpp_async.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_async::uvcpp_async() : uvcpp_handle() {
  uv_async_t *async = uvcpp::uv_alloc<uv_async_t>();
  this->set_handle(async, true);
  this->init();
}
uvcpp_async::~uvcpp_async() {}

uvcpp_async::uvcpp_async(uvcpp_loop *loop) : uvcpp_handle() {
  uv_async_t *async = uvcpp::uv_alloc<uv_async_t>();
  this->set_handle(async, true);
  this->init(loop);
}

int uvcpp_async::init() {
  memset(UVCPP_ASYNC_HANDLE, 0, sizeof(uv_async_t));
  this->set_handle_data();
  return 0;
}

int uvcpp_async::init(uvcpp_loop *loop) {
  int ret = uv_async_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_ASYNC_HANDLE, nullptr);
  this->set_handle_data();
  return ret;
}

int uvcpp_async::init(::std::function<void(uvcpp_async *)> init_cb, uvcpp_loop *loop) {
  async_init_cb = init_cb;
  return uv_async_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_ASYNC_HANDLE,
                       callback_init);
}

void uvcpp_async::callback_init(uv_async_t *handle) {
  if (reinterpret_cast<uvcpp_async *>(handle->data)->async_init_cb)
    reinterpret_cast<uvcpp_async *>(handle->data)
        ->async_init_cb(reinterpret_cast<uvcpp_async *>(handle->data));
}


} // namespace uvcpp