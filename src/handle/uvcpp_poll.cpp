#include "uvcpp_poll.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_poll::uvcpp_poll() : uvcpp_handle() {
  uv_poll_t* poll = uvcpp::uvcpp_alloc<uv_poll_t>();
  this->set_handle(poll, true);
  this->init();
}

uvcpp_poll::~uvcpp_poll() {}
#ifdef WIN32
uvcpp_poll::uvcpp_poll(uvcpp_loop *loop, int fd) : uvcpp_handle() {
  uv_poll_t* poll = uvcpp::uvcpp_alloc<uv_poll_t>();
  this->set_handle(poll, true);
  init(loop, fd);
}
#endif
uvcpp_poll::uvcpp_poll(uvcpp_loop *loop, uv_os_sock_t socket) : uvcpp_handle() {
  uv_poll_t* poll = uvcpp::uvcpp_alloc<uv_poll_t>();
  this->set_handle(poll, true);
  init(loop, socket);
}

int uvcpp_poll::init() { 
  memset(UVCPP_POLL_HANDLE, 0, sizeof(uv_poll_t));
  this->set_handle_data();
  return 0;
}
#ifdef WIN32
int uvcpp_poll::init(uvcpp_loop* loop, int fd) {
  int ret = uv_poll_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_POLL_HANDLE, fd);
  this->set_handle_data();
  return ret;
}
#endif
int uvcpp_poll::init(uvcpp_loop* loop, uv_os_sock_t socket) {
  int ret = uv_poll_init_socket(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_POLL_HANDLE, socket);
  this->set_handle_data();
  return ret;
}

 int uvcpp_poll::start(int events,
                        ::std::function<void(uvcpp_poll*, int, int)> start_cb) {
  poll_start_cb = start_cb;
  return uv_poll_start(UVCPP_POLL_HANDLE, events, callback_start);
}

int uvcpp_poll::stop() {
  return uv_poll_stop(UVCPP_POLL_HANDLE);
}

 void uvcpp_poll::callback_start(uv_poll_t* handle, int status, int events) {
  if (reinterpret_cast<uvcpp_poll*>(handle->data)->poll_start_cb)
    reinterpret_cast<uvcpp_poll*>(handle->data)
        ->poll_start_cb(reinterpret_cast<uvcpp_poll*>(handle->data), status, events);
}

 } // namespace uvcpp