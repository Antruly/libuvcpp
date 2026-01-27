#include "uvcpp_loop.h"
namespace uvcpp {
uvcpp_loop::uvcpp_loop() : uvcpp_handle() {
  uv_loop_t *loop = uvcpp::uv_alloc<uv_loop_t>();
  if (loop == nullptr)
    throw std::bad_alloc();
  this->set_handle(loop, true);
  this->init();
}
uvcpp_loop::~uvcpp_loop() {
  if (!this->is_closing() && this->is_active())
    this->close();
}
int uvcpp_loop::run(uv_run_mode md) { return uv_run(UVCPP_LOOP_HANDLE, md); }

void uvcpp_loop::walk(::std::function<void(uvcpp_handle *, void *)> walk_cb,
                      void *arg) {
  this->handle_walk_cb = walk_cb;
  this->walk_arg_ = arg;
  uv_walk(UVCPP_LOOP_HANDLE, uvcpp_loop::callback_walk, this);
}

void uvcpp_loop::callback_walk(uv_handle_t *handle, void *arg) {
  if (reinterpret_cast<uvcpp_loop *>(arg)->handle_walk_cb)
    reinterpret_cast<uvcpp_loop *>(arg)->handle_walk_cb(
        reinterpret_cast<uvcpp_handle *>(handle->data),
        reinterpret_cast<uvcpp_loop *>(arg)->walk_arg_);
}

uvcpp_loop *uvcpp_loop::default_loop() {
  static uvcpp_loop default_loop;
  default_loop.init();
  return &default_loop;
}

int uvcpp_loop::init() {
  int ret = uv_loop_init(UVCPP_LOOP_HANDLE);
  this->set_handle_data();
  return ret;
}

int uvcpp_loop::loop_alive() { return uv_loop_alive(UVCPP_LOOP_HANDLE); }

void uvcpp_loop::stop() {
  if (uv_loop_alive(UVCPP_LOOP_HANDLE)) {
    uv_stop(UVCPP_LOOP_HANDLE);
  }
}

int uvcpp_loop::loop_close() {
  stop();
  return uv_loop_close(UVCPP_LOOP_HANDLE);
}

int uvcpp_loop::close() { return loop_close(); }

} // namespace uvcpp