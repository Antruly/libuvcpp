#include "uvcpp_tty.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {

uvcpp_tty::uvcpp_tty() : uvcpp_handle() {
  uv_tty_t* tty = uvcpp::uv_alloc<uv_tty_t>();
  this->set_handle(tty, true);
  this->init();
}
uvcpp_tty::~uvcpp_tty() {}

uvcpp_tty::uvcpp_tty(uvcpp_loop *loop, uv_file fd, int readable)
    : uvcpp_handle() {
  uv_tty_t* tty = uvcpp::uv_alloc<uv_tty_t>();
  this->set_handle(tty, true);
  this->init(loop, fd, readable);
}

int uvcpp_tty::init() {
  memset(UVCPP_TTY_HANDLE, 0, sizeof(uv_tty_t));
  this->set_handle_data();
  return 0;
}

int uvcpp_tty::init(uvcpp_loop *loop, uv_file fd, int readable) {
  int ret =
      uv_tty_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_TTY_HANDLE, fd, readable);
  this->set_handle_data();
  return ret;
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 1
int uvcpp_tty::set_mode(uv_tty_mode_t mode) {
  return uv_tty_set_mode(UVCPP_TTY_HANDLE, mode);
}
#endif
#endif

int uvcpp_tty::reset_mode(void) { return uv_tty_reset_mode(); }

#if UV_VERSION_MAJOR >= 1
int uvcpp_tty::get_winsize(int *width, int *height) {
  return uv_tty_get_winsize(UVCPP_TTY_HANDLE, width, height);
}
#else
int uvcpp_tty::get_winsize(int *width, int *height) {
  (void)width; (void)height;
  return UV_ENOSYS;
}
#endif
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 33
void uvcpp_tty::set_vterm_state(uv_tty_vtermstate_t state) {
  uv_tty_set_vterm_state(state);
  return;
}

#if UV_VERSION_MAJOR >= 1
int uvcpp_tty::get_vterm_state(uv_tty_vtermstate_t *state) {
  return uv_tty_get_vterm_state(state);
}
#else
int uvcpp_tty::get_vterm_state(uv_tty_vtermstate_t *state) {
  (void)state;
  return UV_ENOSYS;
}
#endif
#endif
#endif
} // namespace uvcpp
