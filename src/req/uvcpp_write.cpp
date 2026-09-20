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
  if (second_owner != nullptr) {
    uvcpp_buf::free_buf(second_owner);
    uvcpp::uvcpp_free(second_owner);
    second_owner = nullptr;
  }
  // hold_ 自己会放（第 2 块靠它活着）。
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
  // 重新设第 1 块即回到"只有 1 块"：数组是在 append 那一刻快照的，留下旧的
  // 快照与新的第 1 块不一致 —— 那正是"头是新的、体是上一笔的"这种错。
  nbufs_ = 1;
}

uv_buf_t *uvcpp_write::get_uv_buf() { return uv_buf; }

void uvcpp_write::append_uv_buf_view(
    uv_buf_t bf, const ::std::shared_ptr<const ::std::string> &hold) {
  pair_[0] = (uv_buf != nullptr) ? *uv_buf : uv_buf_init(nullptr, 0);
  pair_[1] = bf;
  hold_    = hold;
  nbufs_   = 2;
}

void uvcpp_write::append_uv_buf_owned(uv_buf_t *bf) {
  pair_[0] = (uv_buf != nullptr) ? *uv_buf : uv_buf_init(nullptr, 0);
  pair_[1] = (bf != nullptr) ? *bf : uv_buf_init(nullptr, 0);
  second_owner = bf;
  nbufs_       = 2;
}

uv_buf_t *uvcpp_write::get_uv_bufs() { return (nbufs_ < 2) ? uv_buf : pair_; }

size_t uvcpp_write::get_uv_nbufs() const { return nbufs_; }

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
  if (w == nullptr) {
    return;
  }
  // 搬闭包 + 事后自我释放，见 uvcpp_req::invoke_completion。
  invoke_completion(w->m_write_cb, w, status);
}


} // namespace uvcpp