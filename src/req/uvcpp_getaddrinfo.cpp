#include "uvcpp_getaddrinfo.h"
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_threadpool.h>
namespace uvcpp {
uvcpp_getaddrinfo::uvcpp_getaddrinfo() : uvcpp_req() {
  uv_getaddrinfo_t* req = uvcpp::uvcpp_alloc<uv_getaddrinfo_t>();
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
  // 本库的池账：这一笔会把活儿送进 libuv 线程池（见 uvcpp_threadpool.h）
  uvcpp_threadpool_note_use();
  return uv_getaddrinfo(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_GETADDRINFO_REQ, callback_getaddrinfo, node, service, hints);
}

void uvcpp_getaddrinfo::callback_getaddrinfo(uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
  uvcpp_getaddrinfo* self = reinterpret_cast<uvcpp_getaddrinfo*>(req->data);
  if (self == nullptr) {
    return;
  }
  // 搬闭包再调用，理由见 uvcpp_req::invoke_completion（回调里可能 `delete self`）。
  invoke_completion(self->m_getaddrinfo_cb, self, status, res);
}
} // namespace uvcpp
