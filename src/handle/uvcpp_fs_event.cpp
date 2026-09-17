#include "uvcpp_fs_event.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_fs_event::uvcpp_fs_event() : uvcpp_handle() {
  uv_fs_event_t *fs_event = uvcpp::uvcpp_alloc<uv_fs_event_t>();
  this->set_handle(fs_event, true);
  this->init();
}

uvcpp_fs_event::~uvcpp_fs_event() {}

uvcpp_fs_event::uvcpp_fs_event(uvcpp_loop *loop) : uvcpp_handle() {
  uv_fs_event_t *fs_event = uvcpp::uvcpp_alloc<uv_fs_event_t>();
  this->set_handle(fs_event, true);
  this->init(loop);
}

int uvcpp_fs_event::init() {
  // 已接进循环的句柄不能清零，理由见 uvcpp_handle::reset_handle_state。
  this->reset_handle_state(UVCPP_FSEVENT_HANDLE, sizeof(uv_fs_event_t));
  return 0;
}
int uvcpp_fs_event::init(uvcpp_loop *loop) {
  int ret = uv_fs_event_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FSEVENT_HANDLE);
  this->set_handle_data();
  return ret;
}

int uvcpp_fs_event::start(const char *path, unsigned int flags) {
  return uv_fs_event_start(UVCPP_FSEVENT_HANDLE, nullptr, path, flags);
}

int uvcpp_fs_event::start(
    ::std::function<void(uvcpp_fs_event *, const char *, int, int)> start_cb,
    const char *path, unsigned int flags) {
  fs_event_start_cb = start_cb;
  return uv_fs_event_start(UVCPP_FSEVENT_HANDLE, callback_start, path, flags);
}

int uvcpp_fs_event::stop() { return uv_fs_event_stop(UVCPP_FSEVENT_HANDLE); }

int uvcpp_fs_event::getpath(char *buffer, size_t *size) {
  return uv_fs_event_getpath(UVCPP_FSEVENT_HANDLE, buffer, size);
}

void uvcpp_fs_event::callback_start(uv_fs_event_t *handle, const char *filename,
                              int events, int status) {
  uvcpp_fs_event *self = reinterpret_cast<uvcpp_fs_event *>(handle->data);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 句柄回调会重复触发，所以是拷不是搬。
  auto cb = self->fs_event_start_cb;
  if (cb) {
    cb(self, filename, events, status);
  }
}
} // namespace uvcpp