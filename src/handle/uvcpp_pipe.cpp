#include "uvcpp_pipe.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_pipe::uvcpp_pipe() : uvcpp_stream() {
  uv_pipe_t *pipe = uvcpp::uvcpp_alloc<uv_pipe_t>();
  this->set_handle(pipe, true);
  this->init();
}

uvcpp_pipe::~uvcpp_pipe() {}

uvcpp_pipe::uvcpp_pipe(uvcpp_loop *loop, int pic) : uvcpp_stream() {
  uv_pipe_t *pipe = uvcpp::uvcpp_alloc<uv_pipe_t>();
  this->set_handle(pipe, true);
  this->init(loop, pic);
}

int uvcpp_pipe::init() {
  memset(UVCPP_PIPE_HANDLE, 0, sizeof(uv_pipe_t));
  this->set_handle_data();
  return 0;
}

int uvcpp_pipe::init(uvcpp_loop *loop, int pic) {
  int ret = uv_pipe_init((uv_loop_t *)loop->get_handle(),
                         (uv_pipe_t *)this->get_handle(), pic);
  this->set_handle_data();
  return ret;
}

int uvcpp_pipe::open(uv_file file) { return uv_pipe_open(UVCPP_PIPE_HANDLE, file); }

int uvcpp_pipe::bind(const char *name) { return uv_pipe_bind(UVCPP_PIPE_HANDLE, name); }

void uvcpp_pipe::connect(uvcpp_connect *req, const char *name) {
  return uv_pipe_connect(OBJ_UVCPP_CONNECT_REQ(*req), UVCPP_PIPE_HANDLE, name, nullptr);
}

void uvcpp_pipe::connect(uvcpp_connect *req, const char *name,
                    std::function<void(uvcpp_connect *, int)> connect_cb) {
  req->m_connect_cb = connect_cb;
  return uv_pipe_connect(OBJ_UVCPP_CONNECT_REQ(*req), UVCPP_PIPE_HANDLE, name,
                         uvcpp_connect::callback_connect);
}

int uvcpp_pipe::getsockname(char *buffer, size_t *size) {
  return uv_pipe_getsockname(UVCPP_PIPE_HANDLE, buffer, size);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 2
int uvcpp_pipe::getpeername(char *buffer, size_t *size) {
  return uv_pipe_getpeername(UVCPP_PIPE_HANDLE, buffer, size);
}
#endif
#endif

void uvcpp_pipe::pendingInstances(int count) {
  uv_pipe_pending_instances(UVCPP_PIPE_HANDLE, count);
}

int uvcpp_pipe::pendingCount() { return uv_pipe_pending_count(UVCPP_PIPE_HANDLE); }

uv_handle_type uvcpp_pipe::pendingType() { return uv_pipe_pending_type(UVCPP_PIPE_HANDLE); }

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 16
int uvcpp_pipe::chmod(int flags) { return uv_pipe_chmod(UVCPP_PIPE_HANDLE, flags); }
#endif
#endif

} // namespace uvcpp