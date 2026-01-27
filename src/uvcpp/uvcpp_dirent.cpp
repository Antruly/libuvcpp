#include "uvcpp_dirent.h"
#include <uvcpp/uv_alloc.h>

namespace uvcpp {
uvcpp_dirent::uvcpp_dirent() {
  this->dirent = uvcpp::uv_alloc<uv_dirent_t>();
  this->init();
}

uvcpp_dirent::~uvcpp_dirent() { UVCPP_VFREE(this->dirent); }

uvcpp_dirent::uvcpp_dirent(const uvcpp_dirent& obj) {
  if (this->dirent != nullptr) {
    uv_dirent_t* hd = uvcpp::uv_alloc<uv_dirent_t>();
    memcpy(hd, this->dirent, sizeof(uv_dirent_t));
    this->dirent = hd;
  } else {
    this->dirent = nullptr;
  }
}
uvcpp_dirent& uvcpp_dirent::operator=(const uvcpp_dirent& obj) {
  if (this->dirent != nullptr) {
    uv_dirent_t* hd = uvcpp::uv_alloc<uv_dirent_t>();
    memcpy(hd, this->dirent, sizeof(uv_dirent_t));
    this->dirent = hd;
  } else {
    this->dirent = nullptr;
  }
  return *this;
}

int uvcpp_dirent::init() {
  memset(this->dirent, 0, sizeof(uv_dirent_t));
  return 0;
}

uv_dirent_t* uvcpp_dirent::get_dirent() { return dirent; }
} // namespace uvcpp
