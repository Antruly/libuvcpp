#include "uvcpp_metrics.h"
#include <uvcpp/uvcpp_alloc.h>

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 45
namespace uvcpp {
uvcpp_metrics::uvcpp_metrics() {
  this->metrics = uvcpp::uvcpp_alloc<uv_metrics_t>();
  this->init();
}
uvcpp_metrics::~uvcpp_metrics() { UVCPP_VFREE(this->metrics); }
uvcpp_metrics::uvcpp_metrics(const uvcpp_metrics& obj) {
  if (this->metrics != nullptr) {
    uv_metrics_t* hd = uvcpp::uvcpp_alloc<uv_metrics_t>();
    memcpy(hd, this->metrics, sizeof(uv_metrics_t));
    this->metrics = hd;
  } else {
    this->metrics = nullptr;
  }
}
uvcpp_metrics& uvcpp_metrics::operator=(const uvcpp_metrics& obj) {
  if (this->metrics != nullptr) {
    uv_metrics_t* hd = uvcpp::uvcpp_alloc<uv_metrics_t>();
    memcpy(hd, this->metrics, sizeof(uv_metrics_t));
    this->metrics = hd;
  } else {
    this->metrics = nullptr;
  }
  return *this;
}
int uvcpp_metrics::init() {
  memset(this->metrics, 0, sizeof(uv_metrics_t));
  return 0;
}
int uvcpp_metrics::info(uvcpp_loop *loop) {
  return uv_metrics_info(OBJ_UVCPP_LOOP_HANDLE(*loop), this->metrics);
}
uint64_t uvcpp_metrics::idle_time(uvcpp_loop *loop) {
  return uv_metrics_idle_time(OBJ_UVCPP_LOOP_HANDLE(*loop));
}
uv_metrics_t *uvcpp_metrics::get_metrics() const { return this->metrics; }
} // namespace uvcpp
#endif  // UV_VERSION_MINOR >= 45
#endif  // UV_VERSION_MAJOR >= 1


