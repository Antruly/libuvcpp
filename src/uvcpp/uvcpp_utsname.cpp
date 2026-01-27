#include "uvcpp_utsname.h"
#include <uvcpp/uv_alloc.h>

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 25
namespace uvcpp {
uvcpp_utsname::uvcpp_utsname() {
  this->utsname = uvcpp::uv_alloc<uv_utsname_t>();
  this->init();
}
uvcpp_utsname::~uvcpp_utsname() { UVCPP_VFREE(this->utsname); }
uvcpp_utsname::uvcpp_utsname(const uvcpp_utsname& obj) {
  if (this->utsname != nullptr) {
    uv_utsname_t* hd = uvcpp::uv_alloc<uv_utsname_t>();
    memcpy(hd, this->utsname, sizeof(uv_utsname_t));
    this->utsname = hd;
  } else {
    this->utsname = nullptr;
  }
}
uvcpp_utsname& uvcpp_utsname::operator=(const uvcpp_utsname& obj) {
  if (this->utsname != nullptr) {
    uv_utsname_t* hd = uvcpp::uv_alloc<uv_utsname_t>();
    memcpy(hd, this->utsname, sizeof(uv_utsname_t));
    this->utsname = hd;
  } else {
    this->utsname = nullptr;
  }
  return *this;
}
int uvcpp_utsname::init() {
  memset(this->utsname, 0, sizeof(uv_utsname_t));
  return 0;
}
int uvcpp_utsname::get_hostname(char *buffer, size_t *size) {
  return uv_os_gethostname(buffer, size);
}
int uvcpp_utsname::uname() { return uv_os_uname(utsname); }
uv_utsname_t *uvcpp_utsname::get_utsname() const { return utsname; }
} // namespace uvcpp
#endif
#endif