#include "uvcpp_group.h"
#include <uvcpp/uvcpp_alloc.h>
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 45
namespace uvcpp {
uvcpp_group::uvcpp_group() {
  this->group = uvcpp::uvcpp_alloc<uv_group_t>();
  this->init();
}

uvcpp_group::~uvcpp_group() { UVCPP_VFREE(this->group); }

uvcpp_group::uvcpp_group(const uvcpp_group& obj) {
  if (this->group != nullptr) {
    uv_group_t* hd = uvcpp::uvcpp_alloc<uv_group_t>();
    memcpy(hd, this->group, sizeof(uv_group_t));
    this->group = hd;
  } else {
    this->group = nullptr;
  }
}
uvcpp_group& uvcpp_group::operator=(const uvcpp_group& obj) {
  if (this->group != nullptr) {
    uv_group_t* hd = uvcpp::uvcpp_alloc<uv_group_t>();
    memcpy(hd, this->group, sizeof(uv_group_t));
    this->group = hd;
  } else {
    this->group = nullptr;
  }
  return *this;
}

int uvcpp_group::init() {
  memset(this->group, 0, sizeof(uv_group_t));
  return 0;
}
int uvcpp_group::get_os_group(uv_uid_t gid) {
  return uv_os_get_group(this->group, gid);
}
void uvcpp_group::free_group() { uv_os_free_group(this->group); }
} // namespace uvcpp
#endif
#endif