#include "uvcpp_thread.h"
#include <uvcpp/uv_alloc.h>

namespace uvcpp {
uvcpp_thread::uvcpp_thread() {
  thread = uvcpp::uv_alloc<uv_thread_t>();
}

uvcpp_thread::~uvcpp_thread() { UVCPP_VFREE(this->thread); }

int uvcpp_thread::start(::std::function<void(void *)> start_cb, void *arg) {
  thread_start_cb = start_cb;
  this->vdata = arg;
  return uv_thread_create(thread, callback_start, this);
}

int uvcpp_thread::init() {
  memset(thread, 0, sizeof(uv_thread_t));
  return 0;
}

int uvcpp_thread::join() { return uv_thread_join(thread); }

int uvcpp_thread::equal(const uvcpp_thread *t) {
  return uv_thread_equal(thread, t->thread);
}

int uvcpp_thread::equal(const uvcpp_thread *t1, const uvcpp_thread *t2) {
  return uv_thread_equal(t1->thread, t2->thread);
}

int uvcpp_thread::equal(const uv_thread_t *t1, const uv_thread_t *t2) {
  return uv_thread_equal(t1, t2);
}

int uvcpp_thread::equal(const uv_thread_t *t) {
  return uv_thread_equal(thread, t);
}

uv_thread_t uvcpp_thread::self() { return uv_thread_self(); }

uv_thread_t *uvcpp_thread::get_thread() const { return thread; }
} // namespace uvcpp