#include "uvcpp_dir.h"
#include <uvcpp/uv_alloc.h>
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 28

namespace uvcpp {
uvcpp_dir::uvcpp_dir() {
  _dir = uvcpp::uv_alloc<uv_dir_t>();
  init();
}
uvcpp_dir::~uvcpp_dir() { UVCPP_VFREE(_dir); }
uvcpp_dir::uvcpp_dir(uvcpp_dirent *dr) {
  _dir = uvcpp::uv_alloc<uv_dir_t>();
  init();
  _dir->dirents = dr->get_dirent();
}
int uvcpp_dir::init() {
  memset(_dir, 0, sizeof(uv_dir_t));
  return 0;
}
void uvcpp_dir::set_dirent(uvcpp_dirent *dr) { _dir->dirents = dr->get_dirent(); }
uv_dir_t *uvcpp_dir::get_dir() { return _dir; }
} // namespace uvcpp
#endif
#endif
