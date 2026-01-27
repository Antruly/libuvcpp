#include "uvcpp_statfs.h"
#include <uvcpp/uv_alloc.h>
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 29
namespace uvcpp {
uvcpp_statfs::uvcpp_statfs() {
  this->statfs = uvcpp::uv_alloc<uv_statfs_t>();
  this->init();
}
uvcpp_statfs::~uvcpp_statfs() { UVCPP_VFREE(this->statfs); }
uvcpp_statfs::uvcpp_statfs(const uvcpp_statfs& obj) {
  if (this->statfs != nullptr) {
    uv_statfs_t* hd = uvcpp::uv_alloc<uv_statfs_t>();
    memcpy(hd, this->statfs, sizeof(uv_statfs_t));
    this->statfs = hd;
  } else {
    this->statfs = nullptr;
  }
}
uvcpp_statfs& uvcpp_statfs::operator=(const uvcpp_statfs& obj) {
  if (this->statfs != nullptr) {
    uv_statfs_t* hd = uvcpp::uv_alloc<uv_statfs_t>();
    memcpy(hd, this->statfs, sizeof(uv_statfs_t));
    this->statfs = hd;
  } else {
    this->statfs = nullptr;
  }
  return *this;
}
int uvcpp_statfs::init() {
  memset(this->statfs, 0, sizeof(uv_statfs_t));
  return 0;
}
uv_statfs_t *uvcpp_statfs::get_statfs() const{
	return statfs;
}
} // namespace uvcpp
#endif
#endif