#include "uvcpp_getnameinfo.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_getnameinfo::uvcpp_getnameinfo() : uvcpp_req() {
  uv_getnameinfo_t* req = uvcpp::uvcpp_alloc<uv_getnameinfo_t>();
  this->set_req(req);
  this->init();
}
uvcpp_getnameinfo::~uvcpp_getnameinfo() {}

int uvcpp_getnameinfo::init() {
  memset(UVCPP_GETNAMEINFO_REQ, 0, sizeof(uv_getnameinfo_t));
  this->set_req_data();
  return 0;
}

int uvcpp_getnameinfo::getnameinfo(uvcpp_loop* loop, const struct sockaddr* addr, int flags,
                  ::std::function<void(uvcpp_getnameinfo*, int, const char*, const char*)> getnameinfo_cb) {
  m_getnameinfo_cb = getnameinfo_cb;
  return uv_getnameinfo(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_GETNAMEINFO_REQ, callback_getnameinfo, addr, flags);
}

void uvcpp_getnameinfo::callback_getnameinfo(uv_getnameinfo_t* req, int status,
                                   const char* hostname,
                                   const char* service) {
  if (reinterpret_cast<uvcpp_getnameinfo*>(req->data)->m_getnameinfo_cb)
    reinterpret_cast<uvcpp_getnameinfo*>(req->data)->m_getnameinfo_cb(
        reinterpret_cast<uvcpp_getnameinfo*>(req->data), status, hostname, service);
}
} // namespace uvcpp