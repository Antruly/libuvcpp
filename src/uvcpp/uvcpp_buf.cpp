#include "uvcpp_buf.h"
#include <stdexcept>
#include <new>
#include <cstdint>
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_buf::uvcpp_buf(){
  this->buf.base = nullptr;
  this->buf.len = 0;
  this->capacity_ = 0;
}

uvcpp_buf::~uvcpp_buf() {
  this->free_own();
}

uvcpp_buf::uvcpp_buf(const uvcpp_buf &bf) : uvcpp_buf (){

  if (bf.buf.len > 0 && bf.buf.base != nullptr) {
    this->resize(bf.buf.len);
    memcpy(this->buf.base, bf.buf.base, bf.buf.len);
  }
}

uvcpp_buf &uvcpp_buf::operator=(const uvcpp_buf &bf) {
  if (this == &bf) {
    return *this; // 自赋值检查
  }

  this->free_own();

  if (bf.buf.len > 0 && bf.buf.base != nullptr) {
    this->resize(bf.buf.len);
    if (this->buf.base != nullptr)
    memcpy(this->buf.base, bf.buf.base, bf.buf.len);
  } else {
    this->resize(0);
  }
  return *this;
}

// 移动构造/赋值（见头文件里"必须显式写"那一段）。
//
// 三个成员是一套状态，必须一起搬；搬完把源**归零**——尤其是 `capacity_`：
// 只搬 buf 不搬容量的话源会变成"有指针、容量 0"的外部视图形状，而它明明拥有
// 那块内存，之后既不会复用也会被 free_own 释放一次 —— 接收方那次释放就成了
// 二次释放。共享手柄也要搬走（不是拷一份）：这里要的是"这块字节现在归你了"。
uvcpp_buf::uvcpp_buf(uvcpp_buf &&obj) noexcept : uvcpp_buf() {
  this->buf       = obj.buf;
  this->capacity_ = obj.capacity_;
  this->shared_   = ::std::move(obj.shared_);
  obj.buf.base    = nullptr;
  obj.buf.len     = 0;
  obj.capacity_   = 0;
}

uvcpp_buf &uvcpp_buf::operator=(uvcpp_buf &&obj) noexcept {
  if (this == &obj) {
    return *this;
  }
  this->free_own();
  this->buf       = obj.buf;
  this->capacity_ = obj.capacity_;
  this->shared_   = ::std::move(obj.shared_);
  obj.buf.base    = nullptr;
  obj.buf.len     = 0;
  obj.capacity_   = 0;
  return *this;
}

uvcpp_buf::uvcpp_buf(const char *bf, size_t sz) : uvcpp_buf() {
  if (sz > 0) {
    if (bf == nullptr) {
      throw std::invalid_argument("Input buffer pointer is null");
    }
    this->resize(sz);
    memcpy(this->buf.base, bf, sz);
  }
}

uvcpp_buf::uvcpp_buf(const ::std::string &str) : uvcpp_buf() {
  if (!str.empty()) {
    this->resize(str.size());
    memcpy(this->buf.base, str.c_str(), str.size());
  }
}

void *uvcpp_buf::operator new(size_t size) {
  void *p = uvcpp::uvcpp_alloc_bytes(size);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void uvcpp_buf::operator delete(void *p) {
  UVCPP_VFREE(p)
}

uvcpp_buf::uvcpp_buf(const uv_buf_t &bf) {
  this->buf.base = nullptr;
  this->buf.len = 0;
  this->capacity_ = 0;
  if (bf.len > 0 && bf.base != nullptr) {
    this->resize(bf.len);
    if (this->buf.base != nullptr)
    memcpy(this->buf.base, bf.base, bf.len);
  }
}

uvcpp_buf &uvcpp_buf::operator=(const uv_buf_t &bf) {
  if (this == reinterpret_cast<const uvcpp_buf *>(&bf)) {
    return *this; // 自赋值检查 (防护，尽管不太可能)
  }

  this->free_own();
  if (bf.len > 0 && bf.base != nullptr) {
    this->resize(bf.len);
    if (this->buf.base != nullptr)
    memcpy(this->buf.base, bf.base, bf.len);
  }
  return *this;
}
uvcpp_buf uvcpp_buf::operator+(const uvcpp_buf &bf) const {
  uvcpp_buf vbf;
  size_t total_size = this->buf.len + bf.buf.len;

  if (total_size > 0) {
    vbf.resize(total_size);
    if (this->buf.len > 0 && this->buf.base != nullptr) {
      memcpy(vbf.buf.base, this->buf.base, this->buf.len);
    }
    if (bf.buf.len > 0 && bf.buf.base != nullptr) {
      memcpy(vbf.buf.base + this->buf.len, bf.buf.base, bf.buf.len);
    }
  }

  return vbf;
}
char uvcpp_buf::operator[](const int num) const {
  if (this->buf.base == nullptr) {
    throw std::out_of_range("Buffer is null");
  }
  if (num < 0 || static_cast<size_t>(num) >= this->buf.len) {
    throw std::out_of_range("Index out of bounds");
  }
  return this->buf.base[num];
}
bool uvcpp_buf::operator==(const uvcpp_buf &bf) const {
  if (this->buf.len != bf.buf.len) {
    return false;
  }
  if (this->buf.len == 0) {
    return true; // 两个空缓冲区相等
  }
  if (this->buf.base == nullptr || bf.buf.base == nullptr) {
    return this->buf.base == bf.buf.base; // 都为null则相等，否则不相等
  }
  return memcmp(this->buf.base, bf.buf.base, this->buf.len) == 0;
}
bool uvcpp_buf::operator!=(const uvcpp_buf &bf) const { return !(*this == bf); }
bool uvcpp_buf::operator>(const uvcpp_buf &bf) const {
  return this->buf.len > bf.buf.len;
}
bool uvcpp_buf::operator>=(const uvcpp_buf &bf) const {
  return this->buf.len >= bf.buf.len;
}
bool uvcpp_buf::operator<(const uvcpp_buf &bf) const {
  return this->buf.len < bf.buf.len;
}
bool uvcpp_buf::operator<=(const uvcpp_buf &bf) const {
  return this->buf.len <= bf.buf.len;
}
int uvcpp_buf::init() {
  this->set_zero();
  return 0;
}

void uvcpp_buf::set_zero() {
  if (this->buf.base == nullptr && this->buf.len > 0) {
    throw std::logic_error("Buffer state inconsistency: null pointer with non-zero length");
  }
  if (this->buf.base != nullptr && this->buf.len > 0) {
    // 这一支是直接 memset 的、不经过 resize()，所以物化得自己来 ——
    // 少了这一句，set_zero() 会把别人（可能只读）的共享串清零。
    this->materialize();
    memset(this->buf.base, 0, this->buf.len);
  }
}

void uvcpp_buf::free_own() {
  if (this->buf.base != nullptr && this->capacity_ > 0) {
    UVCPP_VFREE(this->buf.base)
  }
  this->buf.base = nullptr;
  this->buf.len = 0;
  this->capacity_ = 0;
  // 共享视图那条路没有块可 free（capacity_ 恒为 0），但**引用要放掉** ——
  // 漏了这一句，引用计数就只涨不落，那份缓冲再也回收不了。
  this->shared_.reset();
}

void uvcpp_buf::materialize() const {
  if (this->shared_ == nullptr) {
    return;
  }
  // 走到这里 = 一次"先共享又丢"。静默（不断言、不抛，理由见头文件），但要能
  // 看见 —— 这是本类里**唯一**动这个计数的地方。
  share_discard_count_.fetch_add(1, ::std::memory_order_relaxed);
  const size_t n = this->buf.len;
  char *p = nullptr;
  if (n > 0) {
    p = static_cast<char *>(uvcpp_alloc_bytes(n));
    if (p == nullptr) {
      throw std::bad_alloc();
    }
    // **先拷、后放引用**：buf.base 指向的就是 *shared_ 的内容，先把引用放掉
    // 就可能把那个 string 析构掉，接着这一句 memcpy 读的就是已释放的内存
    // （只在"本对象是最后一份引用"时发生，所以它同时是"偶尔崩"和"偶尔读脏"）。
    memcpy(p, this->buf.base, n);
  }
  this->shared_.reset();
  this->buf.base = p;
  this->buf.len = n;
  this->capacity_ = n;
}

void uvcpp_buf::share(::std::shared_ptr<const ::std::string> src) {
  this->free_own();  // 旧的块 + 旧的共享引用一起放掉，buf 归零
  if (src == nullptr || src->empty()) {
    return;  // 空共享与 clear() 之后的状态一致
  }
  this->shared_ = ::std::move(src);
  // 只读地指过去：capacity_ 留 0，于是它按"外部视图"被对待 ——
  // 不能 free、不能就地写，写操作必须先 materialize()。
  this->buf.base = const_cast<char *>(this->shared_->data());
  this->buf.len = this->shared_->size();
  this->capacity_ = 0;
}

bool uvcpp_buf::is_shared() const { return this->shared_ != nullptr; }

::std::shared_ptr<const ::std::string> uvcpp_buf::shared_ref() const {
  return this->shared_;
}

::std::atomic<uint64_t> uvcpp_buf::share_discard_count_{0};

uint64_t uvcpp_buf::share_discard_count() {
  return share_discard_count_.load(::std::memory_order_relaxed);
}

void uvcpp_buf::reset_share_discard_count() {
  share_discard_count_.store(0, ::std::memory_order_relaxed);
}

void uvcpp_buf::resize(size_t sz) {
  if (sz == 0) {
    this->free_own();
    return;
  }
  // 共享视图先物化成自有的块：resize 的调用方拿到的必然是一块可写内存。
  // 不放在 sz == 0 那一支之前是因为那一支根本不要写（free_own 自己会放引用）。
  this->materialize();
  if (this->buf.len == sz) {
    return;
  }

  if (this->buf.base != nullptr && sz <= this->capacity_) {
    // 容量够：原地复用，只改可见长度。新露出来的那段仍然清零。
    if (this->buf.len < sz) {
      memset(this->buf.base + this->buf.len, 0, sz - this->buf.len);
    }
    this->buf.len = sz;
    return;
  }

  // 旧块的有效字节数是 this->buf.len —— 内存池路径靠它决定复制多少，
  // 不能沿用「照新大小复制」的写法（会从旧块后面读越界）。
  const size_t old_len = this->buf.len;
  // 容量翻倍：append 系列每次只涨一点，不翻倍就退化成每次都要新分配 + 全量搬运
  // 的 O(n²)（旧块越大越慢，且每次都向内存池要一块新的）。
  size_t new_cap = sz;
  if (this->capacity_ > 0 && this->capacity_ <= (SIZE_MAX / 2)) {
    const size_t doubled = this->capacity_ * 2;
    if (doubled > new_cap) {
      new_cap = doubled;
    }
  }

  char *new_base = nullptr;
  const bool external_view = (this->capacity_ == 0 && this->buf.base != nullptr);
  if (external_view) {
    // 外部视图：这块内存不是我们的，既不能 free 也不能就地扩容 —— 拷一份自有的。
    // 这里只拷 min(旧长度, 新容量)：视图缩小时旧长度可能比新块还大，照旧长度
    // 拷会写越界。（内存池那条路自己会取 min，这里得手写。）
    new_base = (char *)uvcpp_alloc_bytes(new_cap);
    const size_t copy_len = (old_len < new_cap) ? old_len : new_cap;
    if (new_base != nullptr && copy_len > 0) {
      memcpy(new_base, this->buf.base, copy_len);
    }
  } else {
    new_base = (char *)uvcpp_realloc_bytes(this->buf.base, old_len, new_cap);
  }
  if (new_base == nullptr) {
    throw std::bad_alloc();
  }
  if (old_len < sz) {
    memset(new_base + old_len, 0, sz - old_len);
  }
  this->buf.base = new_base;
  this->buf.len = sz;
  this->capacity_ = new_cap;
}

void uvcpp_buf::set_data(const char *bf, size_t sz) {
    if (bf == nullptr && sz > 0) {
      throw std::invalid_argument(
          "Cannot set null buffer pointer with non-zero size");
    }
    this->free_own();
    // set as external data view; do not take ownership
    this->buf.base = const_cast<char *>(bf);
    this->buf.len = sz;
    this->capacity_ = 0;
}

char *uvcpp_buf::get_data() const {
  // 交回可写指针，所以共享视图必须先物化 —— 否则调用方写的是别人（可能只读）
  // 的内存。不是共享视图时这是空操作，热路径一分钱不付。
  this->materialize();
  return this->buf.base;
}

const char *uvcpp_buf::get_const_data() const { return this->buf.base; }

unsigned char *uvcpp_buf::get_udata() const {
  this->materialize();  // 同 get_data()：可写指针
  return (unsigned char *)this->buf.base;
}

const unsigned char *uvcpp_buf::get_const_udata() const {
  return (unsigned char *)this->buf.base;
}

size_t uvcpp_buf::size() const { return this->buf.len; }

void uvcpp_buf::clear() { this->resize(0); }

void uvcpp_buf::clone(const uvcpp_buf &srcBuf) {

  this->resize(srcBuf.size());
  if (srcBuf.size() > 0) {
    memcpy(this->buf.base, srcBuf.get_data(), srcBuf.size());
  }
}

::std::string uvcpp_buf::to_string() const {
  if (this->buf.base == nullptr) {
    return std::string();
  }
  return ::std::string(this->buf.base, this->buf.len);
}

uv_buf_t *uvcpp_buf::out_uv_buf() {
  // 这条入口把块**连同所有权**交出去，而共享视图的所有权在一份 shared_ptr 上
  // —— 它没法通过裸 uv_buf_t 转移（接手方拿到的就是一个要自己 free 的指针）。
  // 所以先物化成自有的块，交出去的才真的是"这块归你了"。
  this->materialize();
  uv_buf_t *bf = uvcpp_alloc<uv_buf_t>();
  bf->base = buf.base;
  bf->len = buf.len;
  // 整块交出去：连同容量。留下 capacity_ 的话，之后再 append 会就地写进
  // 已经交出去的那块内存（对端正在读的内容被改写）。
  buf.base = nullptr;
  buf.len = 0;
  capacity_ = 0;
  return bf;
}

void uvcpp_buf::in_uv_buf(uv_buf_t *bf) {
  this->free_own();
  // 收下这块内存并**持有**它（与 out_uv_buf 对称）。bf->len 是可见长度，
  // 当作容量用是保守的：真的分配得更大也只是少复用一次，不会写越界。
  buf.base = bf->base;
  buf.len = bf->len;
  capacity_ = bf->len;
}

namespace {

/// 只分配、**不清零**的一块裸字节。
///
/// 与 `uvcpp_alloc_*`（`uvcpp_alloc.h`）的区别只有一条：那一族全是 calloc 语义，
/// 这一个不是。所以它**故意**待在本文件的匿名命名空间里，不进公开头 —— 同前缀、
/// 同形状、唯独不保证清零的兄弟放在一起，是个迟早会被误用的陷阱。真要给它
/// 公开的位置，得先让那一族的名字能一眼区分开。
///
/// 不清零为什么值钱：libuv 对 TCP 流给的 `suggested_size` 是 **64 KiB**，而一条
/// 连接在**一次请求**里会让 alloc 回调跑**两趟** ⇒ 每请求白清 **128 KiB**，而请求
/// 本身只有百来字节。实测合计 −13.66%（见 `doc/benchmark.md` §4.1）。
///
/// 契约：拿到之后、读之前，**不能假设它是 0**。
void* alloc_raw(size_t sz) {
#if UVCPP_ENABLE_MEMORY_POOL
  void* p = uvcpp_enterprise_alloc(sz);
#else
  void* p = std::malloc(sz);
#endif
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

}  // namespace

void uvcpp_buf::alloc_buf(uv_buf_t *bf, size_t len) {
  // 不清零：这块是交给 libuv 的 alloc 回调去**接数据**的，紧接着就被读满，
  // 清它是纯白付。见上面 alloc_raw() 的说明。
  bf->base = static_cast<char*>(alloc_raw(len));
  bf->len = len;
}

void uvcpp_buf::free_buf(uv_buf_t *bf) {
  uvcpp_free(bf->base);
  bf->len = 0;
}

void uvcpp_buf::clone_data(const char *bf, size_t sz) {
  if (bf == nullptr && sz > 0) {
    throw std::invalid_argument("Cannot clone null buffer pointer with non-zero size");
  }
  this->resize(sz);
  if (sz > 0) {
    memcpy(this->buf.base, bf, sz);
  }
}

void uvcpp_buf::append(const uvcpp_buf &srcBuf) {
  const size_t n = srcBuf.buf.len;
  if (n == 0) {
    return;
  }
  const size_t old_len = buf.len;
  this->resize(old_len + n);
  // 自追加时源就是自己：resize 可能已经把块搬走，源指针必须搬完再取，
  // 而且取的是**原内容**（还在 [0, old_len)），否则拷到的是刚清零的那一段。
  const char *src = (&srcBuf == this) ? this->buf.base : srcBuf.buf.base;
  memcpy(this->buf.base + old_len, src, n);
}

void uvcpp_buf::append_data(const char *bf, size_t sz) {
  if (bf == nullptr && sz > 0) {
    throw std::invalid_argument("Cannot append null buffer pointer with non-zero size");
  }
  this->resize(this->buf.len + sz);
  if (sz > 0) {
    memcpy(this->buf.base + this->buf.len - sz, bf, sz);
  }
}

void uvcpp_buf::move_buf(uvcpp_buf &src_buf) {
  this->free_own();
  // 共享视图的**引用**也要跟着块一起走，和 capacity_ 同理：留在源上，
  // 接手方就只是拿到了一个指向"别人的 string"的裸指针，源一旦被析构或
  // 被改写，这边立刻悬空。
  this->shared_ = ::std::move(src_buf.shared_);
  this->buf.base = src_buf.buf.base;
  this->buf.len = src_buf.buf.len;
  // 容量跟着块一起走：留在源上会让它在下次 append 时就地写进已经不属于它的块。
  this->capacity_ = src_buf.capacity_;
  src_buf.buf.base = nullptr;
  src_buf.buf.len = 0;
  src_buf.capacity_ = 0;
}

void uvcpp_buf::clone_buf(const uvcpp_buf &src_buf) {
  // 深拷贝。原来是"只搬 base/len、不持所有权"，于是两份 uvcpp_buf 的析构都会
  // free 同一块内存（双重释放），而且改源会把副本一起改掉 —— 名字与 clone()
  // 一致才是这里该有的语义。
  if (&src_buf == this) {
    return;
  }
  this->resize(src_buf.buf.len);
  if (src_buf.buf.len > 0) {
    memcpy(this->buf.base, src_buf.buf.base, src_buf.buf.len);
  }
}

void uvcpp_buf::clone_data(const uv_buf_t &src_buf) {
  if (src_buf.len > 0 && src_buf.base != nullptr) {
    this->resize(src_buf.len);
    memcpy(this->buf.base, src_buf.base, src_buf.len);
  }
}

void uvcpp_buf::insert_data(uint64_t point, const char *bf, size_t sz) {
  if (bf == nullptr && sz > 0) {
    throw std::invalid_argument("Cannot insert null buffer pointer with non-zero size");
  }
  if (point > this->buf.len) {
    throw std::out_of_range("Insert point beyond buffer length");
  }

  if (point >= this->buf.len) {
    this->append_data(bf, sz);
  } else {
    size_t old_len = this->buf.len;
    this->resize(this->buf.len + sz);
    memmove(this->buf.base + point + sz, this->buf.base + point, old_len - point);
    memcpy(this->buf.base + point, bf, sz);
  }
}

void uvcpp_buf::rewrite_data(uint64_t point, const char *bf, size_t sz) {
  if (bf == nullptr && sz > 0) {
    throw std::invalid_argument("Cannot rewrite with null buffer pointer with non-zero size");
  }
  if (point > this->buf.len) {
    throw std::out_of_range("Rewrite point beyond buffer length");
  }

  if (point + sz > this->buf.len) {
    this->resize(point + sz);
  }
  if (sz > 0) {
    memcpy(this->buf.base + point, bf, sz);
  }
}
} // namespace uvcpp