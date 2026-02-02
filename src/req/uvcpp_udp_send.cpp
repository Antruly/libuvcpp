#include "uvcpp_udp_send.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_udp_send::uvcpp_udp_send() : uvcpp_req() ,uv_buf_owner(false),src_buf_owner(false){
  uv_udp_send_t* r = uvcpp::uvcpp_alloc<uv_udp_send_t>();
  this->set_req(r);
  this->init();
}
uvcpp_udp_send::~uvcpp_udp_send() {
  if (uv_buf_owner && uv_buf != nullptr) {
    uvcpp_buf::free_buf(uv_buf);
    uvcpp::uvcpp_free(uv_buf);
    uv_buf = nullptr;
  }
  if (src_buf_owner && src_buf != nullptr) {
    UVCPP_VFREE(src_buf)
  }
};

int uvcpp_udp_send::init() {
  memset(UVCPP_UDP_SEND_REQ, 0, sizeof(uv_udp_send_t));
  this->set_req_data();
  return 0;
}

void uvcpp_udp_send::set_uv_buf(uv_buf_t *bf, bool owner) {
  if (uv_buf_owner && uv_buf != nullptr) {
    uvcpp_buf::free_buf(uv_buf);
    uvcpp::uvcpp_free(uv_buf);
  }
  uv_buf = bf;
  uv_buf_owner = owner;
}

uv_buf_t *uvcpp_udp_send::get_uv_buf() { return uv_buf; }

void uvcpp_udp_send::set_src_buf(const uvcpp_buf *bf, bool owner) {
  if (src_buf_owner && src_buf != nullptr) {
    UVCPP_VFREE(src_buf)
  }
  src_buf = bf;
  src_buf_owner = owner;
}

const uvcpp_buf *uvcpp_udp_send::get_src_buf() { return src_buf; }

// uv_udp_send_cb
void uvcpp_udp_send::callback_udp_send(uv_udp_send_t* req, int status) {
  uvcpp_udp_send *s = reinterpret_cast<uvcpp_udp_send *>(req->data);
  if (s->udp_send_cb) {
    s->udp_send_cb(s, status);
  }
  
}

} // namespace uvcpp