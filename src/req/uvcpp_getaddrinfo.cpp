#include "uvcpp_getaddrinfo.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_getaddrinfo::uvcpp_getaddrinfo() : uvcpp_req() {
  uv_getaddrinfo_t* req = uvcpp::uv_alloc<uv_getaddrinfo_t>();
  this->set_req(req);
  this->init();
}
uvcpp_getaddrinfo::~uvcpp_getaddrinfo() {}

int uvcpp_getaddrinfo::init() {
  memset(UVCPP_GETADDRINFO_REQ, 0, sizeof(uv_getaddrinfo_t));
  this->set_req_data();
  return 0;
}

int uvcpp_getaddrinfo::getaddrinfo(uvcpp_loop* loop, const char* node, const char* service, const struct addrinfo* hints,
                  ::std::function<void(uvcpp_getaddrinfo*, int, struct addrinfo*)> getaddrinfo_cb) {
  m_getaddrinfo_cb = getaddrinfo_cb;
  return uv_getaddrinfo(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_GETADDRINFO_REQ, callback_getaddrinfo, node, service, hints);
}

void uvcpp_getaddrinfo::callback_getaddrinfo(uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
  if (reinterpret_cast<uvcpp_getaddrinfo*>(req->data)->m_getaddrinfo_cb)
    reinterpret_cast<uvcpp_getaddrinfo*>(req->data)->m_getaddrinfo_cb(
        reinterpret_cast<uvcpp_getaddrinfo*>(req->data), status, res);
}
} // namespace uvcpp
