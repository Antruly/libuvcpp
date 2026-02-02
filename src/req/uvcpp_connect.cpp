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

} // namespace uvcpp