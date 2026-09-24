#include "uvcpp_write.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_write::uvcpp_write()
    : uvcpp_req(uvcpp::uvcpp_alloc<uv_write_t>()),
      uv_buf_owner(false),
      src_buf_owner(false) {
  // 那个 uv_write_t 在成员初始化列表里就分好交给基类 —— 原来先让基类分一个
  // uv_req_t、再在这里分一个 uv_write_t 并用 set_req() 顶掉，而 set_req() 的
  // 第一件事就是 free_req()：一次 malloc + 一次 free 是纯白付（见 uvcpp_req.h
  // 那条预分配构造的说明）。
  this->init();
}
uvcpp_write::~uvcpp_write() {
  // 值形态的第 1 块：只放块，`uv_buf` 指向的是成员，不能 uvcpp_free。
  if (first_block_base_ != nullptr) {
    uvcpp::uvcpp_free(first_block_base_);
    first_block_base_ = nullptr;
  }
  if (uv_buf_owner && uv_buf != nullptr) {
    uvcpp_buf::free_buf(uv_buf);
    uvcpp::uvcpp_free(uv_buf);
    uv_buf = nullptr;
  }
  release_second();
  if (src_buf_owner && src_buf != nullptr) {
    UVCPP_VFREE(src_buf);
  }
}

void uvcpp_write::release_second() {
  // 第 2 块只有一个槽位，两种形状共用它 —— 换占用者必须先放掉旧的，
  // 否则 owned 那块的头被覆盖掉就再也没人释放（析构只看 second_owner）。
  if (second_owner != nullptr) {
    uvcpp_buf::free_buf(second_owner);
    uvcpp::uvcpp_free(second_owner);
    second_owner = nullptr;
  }
  if (second_block_base_ != nullptr) {
    uvcpp::uvcpp_free(second_block_base_);
    second_block_base_ = nullptr;
  }
  hold_.reset();
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
  // 两种形态择一：换上指针形态就把值形态那一块按它自己的归属放掉，否则它
  // 再也没人放（析构只看 first_block_base_，会被这一句覆盖掉）。
  if (first_block_base_ != nullptr) {
    uvcpp::uvcpp_free(first_block_base_);
    first_block_base_ = nullptr;
  }
  uv_buf = bf;
  uv_buf_owner = owner;
  // 重新设第 1 块即回到"只有 1 块"：数组是在 append 那一刻快照的，留下旧的
  // 快照与新的第 1 块不一致 —— 那正是"头是新的、体是上一笔的"这种错。
  nbufs_ = 1;
}

void uvcpp_write::set_uv_buf_block(uv_buf_t bf) {
  // 先按各自归属放掉上一个第 1 块（指针形态放包装+块，值形态放块）。
  if (uv_buf_owner && uv_buf != nullptr) {
    uvcpp_buf::free_buf(uv_buf);
    uvcpp::uvcpp_free(uv_buf);
    uv_buf_owner = false;
  }
  if (first_block_base_ != nullptr) {
    uvcpp::uvcpp_free(first_block_base_);
  }
  first_block_      = bf;
  first_block_base_ = bf.base;
  uv_buf            = &first_block_;
  // uv_buf 指向成员 ⇒ 那个指针本身不归本对象释放。
  uv_buf_owner = false;
  // 与 set_uv_buf 一致：重设第 1 块即回到"只有 1 块"。
  nbufs_ = 1;
}

void uvcpp_write::append_uv_buf_block(uv_buf_t bf) {
  release_second();
  second_block_      = bf;
  second_block_base_ = bf.base;
  pair_[0] = (uv_buf != nullptr) ? *uv_buf : uv_buf_init(nullptr, 0);
  pair_[1] = bf;
  nbufs_   = 2;
}

uv_buf_t *uvcpp_write::get_uv_buf() { return uv_buf; }

void uvcpp_write::reset_for_reuse() {
  // 第 2 块：按它自己的归属放掉（自有块 / 第二块的值形态 / 共享视图的引用）。
  release_second();
  // 第 1 块：`set_uv_buf(nullptr, false)` 会按旧占用者的归属放掉（指针形态放
  // 包装 + 块、值形态放块），并把 `nbufs_` 归 1 —— 与"重设第 1 块即回到只有
  // 一块"那条既有规则同形。下一笔写自己会重设。
  set_uv_buf(nullptr, false);
}

void uvcpp_write::adopt_body(uvcpp_buf* body) {
  if (body == nullptr) return;
  // 判据是 `is_shared()` 而不是"有没有引用计数"：
  //   * 共享视图 —— 引用计数接过来（`hold` 由本请求持有到析构）；
  //   * 自有块 —— `release_uv_buf()` 把块连同所有权交出来（**会先 materialize**，
  //     共享视图走错这一支就会当场拷一份，所以顺序不能反）。
  if (body->is_shared()) {
    uv_buf_t vb = uv_buf_init(const_cast<char*>(body->get_const_data()),
                              static_cast<unsigned int>(body->size()));
    append_uv_buf_view(vb, body->shared_ref());
  } else if (body->size() > 0) {
    uv_buf_t vb;
    body->release_uv_buf(&vb);
    append_uv_buf_block(vb);
  }
  // body 为空（且不是共享视图）时不追加第 2 块 —— 这时整条报文就是头部那块，
  // 与 `write(uvcpp_buf*)` 传一个空块是同一形状。
}

void uvcpp_write::append_uv_buf_view(
    uv_buf_t bf, const ::std::shared_ptr<const ::std::string> &hold) {
  release_second();
  pair_[0] = (uv_buf != nullptr) ? *uv_buf : uv_buf_init(nullptr, 0);
  pair_[1] = bf;
  hold_    = hold;
  nbufs_   = 2;
}

void uvcpp_write::append_uv_buf_owned(uv_buf_t *bf) {
  // 同一个头交接两次是调用方的错（所有权只能交一次），但**别把它变成
  // double free** —— 那比泄漏严重得多。先比一下指针再决定放不放。
  if (bf != second_owner) {
    release_second();
    second_owner = bf;
  }
  pair_[0] = (uv_buf != nullptr) ? *uv_buf : uv_buf_init(nullptr, 0);
  pair_[1] = (bf != nullptr) ? *bf : uv_buf_init(nullptr, 0);
  nbufs_   = 2;
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