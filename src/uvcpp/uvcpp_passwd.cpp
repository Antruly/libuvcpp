#include "uvcpp_passwd.h"
#include <uvcpp/uvcpp_alloc.h>

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 9
namespace uvcpp {
uvcpp_passwd::uvcpp_passwd() {
  this->passwd = uvcpp::uvcpp_alloc<uv_passwd_t>();
  this->init();
}
uvcpp_passwd::~uvcpp_passwd() { UVCPP_VFREE(this->passwd); }
uvcpp_passwd::uvcpp_passwd(const uvcpp_passwd& obj) {
  if (this->passwd != nullptr) {
    uv_passwd_t* hd = uvcpp::uvcpp_alloc<uv_passwd_t>();
    memcpy(hd, this->passwd, sizeof(uv_passwd_t));
    this->passwd = hd;
  } else {
    this->passwd = nullptr;
  }
}
uvcpp_passwd& uvcpp_passwd::operator=(const uvcpp_passwd& obj) {
  if (this->passwd != nullptr) {
    uv_passwd_t* hd = uvcpp::uvcpp_alloc<uv_passwd_t>();
    memcpy(hd, this->passwd, sizeof(uv_passwd_t));
    this->passwd = hd;
  } else {
    this->passwd = nullptr;
  }
  return *this;
}
int uvcpp_passwd::init() {
  memset(this->passwd, 0, sizeof(uv_passwd_t));
  return 0;
}
int uvcpp_passwd::get_os_passwd() { return uv_os_get_passwd(passwd); }
#if UV_VERSION_MINOR >= 45
int uvcpp_passwd::get_os_passwd(uv_uid_t uid) { return uv_os_get_passwd2(passwd, uid); }
#endif
void uvcpp_passwd::free_passwd() { uv_os_free_passwd(passwd); }
uv_passwd_t *uvcpp_passwd::get_passwd() const { return this->passwd; }
} // namespace uvcpp
#endif
#endif