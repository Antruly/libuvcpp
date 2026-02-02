#include "uvcpp_write.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_write::uvcpp_write() : uvcpp_req(),uv_buf_owner(false),src_buf_owner(false) {
  uv_write_t* write = uvcpp::uvcpp_alloc<uv_write_t>();
  this->set_req(write);
  this->init();
}
uvcpp_write::~uvcpp_write() {
  if (uv_buf_owner && uv_buf != nullptr) {
    uvcpp_buf::free_buf(uv_buf);
    uvcpp::uvcpp_free(uv_buf);
    uv_buf = nullptr;
  }
  if (src_buf_owner && src_buf != nullptr) {
    UVCPP_VFREE(src_buf);
  }
}
 
int uvcpp_write::init() {
  memset(UVCPP_WRITE_REQ, 0, sizeof(uv_write_t));
  this->set_req_data();
  return 0;
}

void uvcpp_write::set_uv_buf(uv_buf_t *bf, bool owner) {
  if (uv_buf_owner && uv_buf != nullptr) {
    uvcpp_buf::free_buf(uv_buf);
    uvcpp::uvcpp_free(uv_buf);
  }
  uv_buf = bf;
  uv_buf_owner = owner;
}

uv_buf_t *uvcpp_write::get_uv_buf() { return uv_buf; }

void uvcpp_write::set_src_buf(const uvcpp_buf *bf, bool owner) {
  if (src_buf_owner && src_buf != nullptr) {
    UVCPP_VFREE(src_buf)
  }
  src_buf = bf;
  src_buf_owner = owner;
}

const uvcpp_buf *uvcpp_write::get_src_buf() { return src_buf; }

/** @brief libuv uv_write_cb forwarded to m_write_cb. */
void uvcpp_write::callback_write(uv_write_t *req, int status) {
  uvcpp_write *w = reinterpret_cast<uvcpp_write *>(req->data);
  if (w->m_write_cb) {
    w->m_write_cb(w, status);
  }
}


} // namespace uvcpp