#include "uvcpp_shutdown.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_shutdown::uvcpp_shutdown() : uvcpp_req() {
  uv_shutdown_t* shutdown = uvcpp::uvcpp_alloc<uv_shutdown_t>();
  this->set_req(shutdown);
  this->init();
}
uvcpp_shutdown::~uvcpp_shutdown() { }

int uvcpp_shutdown::init() {
  memset(UVCPP_SHUTDOWN_REQ, 0, sizeof(uv_shutdown_t));
  this->set_req_data();
  return 0;
}
} // namespace uvcpp