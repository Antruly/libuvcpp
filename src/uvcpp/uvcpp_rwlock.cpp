#include "uvcpp_rwlock.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {

uvcpp_rwlock::uvcpp_rwlock() {
  this->rwlock = uvcpp::uvcpp_alloc<uv_rwlock_t>();
}
uvcpp_rwlock::~uvcpp_rwlock() { UVCPP_VFREE(this->rwlock); }
int uvcpp_rwlock::init() { return uv_rwlock_init(this->rwlock); }
void uvcpp_rwlock::rdlock() { uv_rwlock_rdlock(this->rwlock); }
void uvcpp_rwlock::tryrdlock() { uv_rwlock_tryrdlock(this->rwlock); }
void uvcpp_rwlock::wrlock() { uv_rwlock_wrlock(this->rwlock); }
void uvcpp_rwlock::trywrlock() { uv_rwlock_trywrlock(this->rwlock); }
void uvcpp_rwlock::rdunlock() { uv_rwlock_rdunlock(this->rwlock); }
void uvcpp_rwlock::wrunlock() { uv_rwlock_wrunlock(this->rwlock); }
void uvcpp_rwlock::destroy() { uv_rwlock_destroy(this->rwlock); }
void *uvcpp_rwlock::get_rwlock() const { return this->rwlock; }
} // namespace uvcpp