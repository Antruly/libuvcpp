#include "uvcpp_write.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_write::uvcpp_write() : uvcpp_req() {
  uv_write_t* write = uvcpp::uv_alloc<uv_write_t>();
  this->set_req(write);
  this->init();
}
uvcpp_write::~uvcpp_write() {
  if (buf_owner && buf != nullptr) {
    UVCPP_VFREE(buf)
  }
  if (src_buf_owner && src_buf != nullptr) {
    UVCPP_VFREE(src_buf)
  }
}
 
int uvcpp_write::init() {
  memset(UVCPP_WRITE_REQ, 0, sizeof(uv_write_t));
  this->set_req_data();
  return 0;
}

void uvcpp_write::set_buf(const uv_buf *bf, bool owner) {
  if (buf_owner && buf != nullptr) {
    UVCPP_VFREE(buf)
  }
  buf = bf;
  buf_owner = owner;
}

const uv_buf *uvcpp_write::get_buf() { return buf; }

void uvcpp_write::set_src_buf(const uv_buf *bf, bool owner) {
  if (src_buf_owner && src_buf != nullptr) {
    UVCPP_VFREE(src_buf)
  }
  src_buf = bf;
  src_buf_owner = owner;
}

const uv_buf *uvcpp_write::get_src_buf() { return src_buf; }


} // namespace uvcpp