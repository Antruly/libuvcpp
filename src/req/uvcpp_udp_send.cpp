#include "uvcpp_udp_send.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_udp_send::uvcpp_udp_send() : uvcpp_req() {
  uv_udp_send_t* r = uvcpp::uv_alloc<uv_udp_send_t>();
  this->set_req(r);
  this->init();
}
uvcpp_udp_send::~uvcpp_udp_send() { };

int uvcpp_udp_send::init() {
  memset(UVCPP_UDP_SEND_REQ, 0, sizeof(uv_udp_send_t));
  this->set_req_data();
  return 0;
}

// uv_udp_send_cb
void uvcpp_udp_send::callback_udp_send(uv_udp_send_t* req, int status) {
  if (reinterpret_cast<uvcpp_udp_send*>(req->data)->udp_send_cb)
    reinterpret_cast<uvcpp_udp_send*>(req->data)->udp_send_cb(
        reinterpret_cast<uvcpp_udp_send*>(req->data), status);
}

} // namespace uvcpp