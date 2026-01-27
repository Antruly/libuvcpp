#include "uvcpp_random.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 33
uvcpp_random::uvcpp_random() : uvcpp_req() {
  uv_random_t* req = uvcpp::uv_alloc<uv_random_t>();
  this->set_req(req);
  this->init();
}
uvcpp_random::~uvcpp_random() {}

int uvcpp_random::init() {
  memset(UVCPP_RANDOM_REQ, 0, sizeof(uv_random_t));
  this->set_req_data();
  return 0;
}

int uvcpp_random::random(uvcpp_loop* loop, void* buf, size_t buflen, unsigned flags,
             ::std::function<void(uvcpp_random*, int, void*, size_t)> random_cb) {
  m_random_cb = random_cb;
  return uv_random(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_RANDOM_REQ, buf, buflen, flags, callback_random);
}

void uvcpp_random::callback_random(uv_random_t* req, int status, void* buf, size_t buflen) {
  if (reinterpret_cast<uvcpp_random*>(req->data)->m_random_cb)
    reinterpret_cast<uvcpp_random*>(req->data)->m_random_cb(
        reinterpret_cast<uvcpp_random*>(req->data), status, buf, buflen);
}

#endif
#endif

} // namespace uvcpp