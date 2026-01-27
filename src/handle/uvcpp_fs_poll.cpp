#include "uvcpp_fs_poll.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_fs_poll::uvcpp_fs_poll() : uvcpp_handle() {
  uv_fs_poll_t *fs_poll = uvcpp::uv_alloc<uv_fs_poll_t>();
  this->set_handle(fs_poll, true);
  this->init();
}
uvcpp_fs_poll::~uvcpp_fs_poll() {}

uvcpp_fs_poll::uvcpp_fs_poll(uvcpp_loop *loop) : uvcpp_handle() {
  uv_fs_poll_t *fs_poll = uvcpp::uv_alloc<uv_fs_poll_t>();
  this->set_handle(fs_poll, true);
  this->init(loop);
}

int uvcpp_fs_poll::init() {
  memset(UVCPP_FSPOLL_HANDLE, 0, sizeof(uv_fs_poll_t));
  this->set_handle_data();
  return 0;
}

int uvcpp_fs_poll::init(uvcpp_loop *loop) {
  int ret = uv_fs_poll_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FSPOLL_HANDLE);
  this->set_handle_data();
  return ret;
}

int uvcpp_fs_poll::start(const char *path, unsigned int interval) {
  return uv_fs_poll_start(UVCPP_FSPOLL_HANDLE, nullptr, path, interval);
}

int uvcpp_fs_poll::start(
    ::std::function<void(uvcpp_fs_poll *, const uv_stat_t *, const uv_stat_t *)>
        start_cb,
    const char *path, unsigned int interval) {
  fs_poll_start_cb = start_cb;
  return uv_fs_poll_start(UVCPP_FSPOLL_HANDLE, callback_start, path, interval);
}

int uvcpp_fs_poll::stop() { return uv_fs_poll_stop(UVCPP_FSPOLL_HANDLE); }

int uvcpp_fs_poll::getpath(char *buffer, size_t *size) {
  return uv_fs_poll_getpath(UVCPP_FSPOLL_HANDLE, buffer, size);
}

void uvcpp_fs_poll::callback_start(uv_fs_poll_t *handle, int status,
                             const uv_stat_t *prev, const uv_stat_t *curr) {
  if (reinterpret_cast<uvcpp_fs_poll *>(handle->data)->fs_poll_start_cb)
    reinterpret_cast<uvcpp_fs_poll *>(handle->data)
        ->fs_poll_start_cb(reinterpret_cast<uvcpp_fs_poll *>(handle->data), prev,
                           curr);
}
} // namespace uvcpp