#include "uvcpp_connect.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_connect::uvcpp_connect() : uvcpp_req() {
  uv_connect_t* req = uvcpp::uvcpp_alloc<uv_connect_t>();
  this->set_req(req);
  this->init();
}
uvcpp_connect::~uvcpp_connect() {};

int uvcpp_connect::init() {
  memset(UVCPP_CONNECT_REQ, 0, sizeof(uv_connect_t));
  this->set_req_data();
  return 0;
}

uvcpp_stream* uvcpp_connect::get_connect_stream() {
  return (uvcpp_stream*)get_data();
}

/** @brief libuv callback forwarded to m_connect_cb. */
void uvcpp_connect::callback_connect(uv_connect_t* req, int status) {
  uvcpp_connect *c = reinterpret_cast<uvcpp_connect *>(req->data);
  if (c == nullptr) {
    return;
  }
  // 与 uvcpp_write / uvcpp_udp_send 同一条路（清单 #7 的同族）：回调里
  // `delete r` 删的就是 m_connect_cb 里那个**正在执行**的闭包。
  // 见 uvcpp_req::invoke_completion。
  invoke_completion(c->m_connect_cb, c, status);
}

} // namespace uvcpp