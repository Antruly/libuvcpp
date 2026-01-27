#include "uvcpp_barrier.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_barrier::uvcpp_barrier() {
  this->barrier = uvcpp::uv_alloc<uv_barrier_t>();
}
uvcpp_barrier::~uvcpp_barrier() { UVCPP_VFREE(this->barrier); }
int uvcpp_barrier::init(int ct) {
  count = ct;
  return uv_barrier_init(this->barrier, ct);
}
int uvcpp_barrier::wait() { return uv_barrier_wait(this->barrier); }
void uvcpp_barrier::destroy() { uv_barrier_destroy(this->barrier); }
int uvcpp_barrier::get_count() { return count; }
void *uvcpp_barrier::get_barrier() const { return this->barrier; }
} // namespace uvcpp
