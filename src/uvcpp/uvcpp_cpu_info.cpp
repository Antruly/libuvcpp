#include "uvcpp_cpu_info.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_cpu_info::uvcpp_cpu_info() {
  this->cpu_info = uvcpp::uv_alloc<uv_cpu_info_t>();
  this->init();
}

uvcpp_cpu_info::~uvcpp_cpu_info() { UVCPP_VFREE(this->cpu_info); }

uvcpp_cpu_info::uvcpp_cpu_info(const uvcpp_cpu_info &obj) {
  if (this->cpu_info != nullptr) {
    uv_cpu_info_t *hd = uvcpp::uv_alloc<uv_cpu_info_t>();
    memcpy(hd, this->cpu_info, sizeof(uv_cpu_info_t));
    this->cpu_info = hd;
  } else {
    this->cpu_info = nullptr;
  }
}
uvcpp_cpu_info &uvcpp_cpu_info::operator=(const uvcpp_cpu_info &obj) {
  if (this->cpu_info != nullptr) {
    uv_cpu_info_t *hd = uvcpp::uv_alloc<uv_cpu_info_t>();
    memcpy(hd, this->cpu_info, sizeof(uv_cpu_info_t));
    this->cpu_info = hd;
  } else {
    this->cpu_info = nullptr;
  }
  return *this;
}

int uvcpp_cpu_info::init() {
  memset(this->cpu_info, 0, sizeof(uv_cpu_info_t));
  return 0;
}
} // namespace uvcpp