#include "uvcpp_buf.h"
#include <stdexcept>
#include <new>
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_buf::uvcpp_buf(){
  this->buf.base = nullptr;
  this->buf.len = 0;
}

uvcpp_buf::~uvcpp_buf() {
  if (this->buf.base != nullptr) {
    UVCPP_VFREE(this->buf.base)
  }
  this->buf.base = nullptr;
  this->buf.len = 0;
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

  if (this->buf.base != nullptr) {
    UVCPP_VFREE(this->buf.base)
  }
  this->buf.base = nullptr;
  this->buf.len = 0;

  if (bf.buf.len > 0 && bf.buf.base != nullptr) {
    this->resize(bf.buf.len);
    if (this->buf.base != nullptr)
    memcpy(this->buf.base, bf.buf.base, bf.buf.len);
  } else {
    this->resize(0);
  }
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

  if (this->buf.base != nullptr) {
    UVCPP_VFREE(this->buf.base)
  }
  this->buf.base = nullptr;
  this->buf.len = 0;
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
    memset(this->buf.base, 0, this->buf.len);
  }
}

void uvcpp_buf::resize(size_t sz) {
  if (sz == 0) {
    if (this->buf.base != nullptr) {
      UVCPP_VFREE(this->buf.base)
    }
    this->buf.base = nullptr;
    this->buf.len = 0;
    return;
  } else if (this->buf.len == sz) {
    return;
  }

  if (this->buf.base != nullptr) {
    // currently pointing to external memory; allocate new owned buffer
    char *new_base = (char *)uvcpp::uvcpp_alloc_bytes(sz);
    if (new_base == nullptr) {
      throw std::bad_alloc();
    }
    if (this->buf.len > 0 && this->buf.base != nullptr) {
      memcpy(new_base, this->buf.base,
             (std::min)(static_cast<size_t>(this->buf.len), sz));
    }
    this->buf.base = new_base;
    if (this->buf.len < sz) {
      memset(this->buf.base + this->buf.len, 0, sz - this->buf.len);
    }
    this->buf.len = sz;
  } else {
    char *new_base = (char *)uvcpp_realloc_bytes(this->buf.base, sz);
   
    this->buf.base = new_base;
    if (this->buf.len < sz) {
      memset(this->buf.base + this->buf.len, 0, sz - this->buf.len);
    }
    this->buf.len = sz;
  }
}

void uvcpp_buf::set_data(const char *bf, size_t sz) {
    if (bf == nullptr && sz > 0) {
      throw std::invalid_argument(
          "Cannot set null buffer pointer with non-zero size");
    }
    if (this->buf.base != nullptr) {
      this->resize(0);
    }
    // set as external data view; do not take ownership
    this->buf.base = const_cast<char *>(bf);
    this->buf.len = sz;
}

char *uvcpp_buf::get_data() const { return this->buf.base; }

const char *uvcpp_buf::get_const_data() const { return this->buf.base; }

unsigned char *uvcpp_buf::get_udata() const { return (unsigned char *)this->buf.base; }

const unsigned char *uvcpp_buf::get_const_udata() const {
  return (unsigned char *)this->buf.base;
}

size_t uvcpp_buf::size() const { return this->buf.len; }

void uvcpp_buf::clear() { this->resize(0); }

void uvcpp_buf::clone(const uvcpp_buf &srcBuf) {

  this->resize(srcBuf.size());
  memcpy(this->buf.base, srcBuf.get_data(), srcBuf.size());
}

::std::string uvcpp_buf::to_string() const {
  if (this->buf.base == nullptr) {
    return std::string();
  }
  return ::std::string(this->buf.base, this->buf.len);
}

uv_buf_t *uvcpp_buf::out_uv_buf() {
  uv_buf_t *bf = uvcpp_alloc<uv_buf_t>();
  bf->base = buf.base;
  bf->len = buf.len;
  buf.base = nullptr;
  buf.len = 0;
  return bf;
}

void uvcpp_buf::in_uv_buf(uv_buf_t *bf) {
  this->resize(0);
  buf.base = bf->base;
  buf.len = bf->len;
}

void uvcpp_buf::alloc_buf(uv_buf_t *bf, size_t len) {
  bf->base = uvcpp_alloc_arry<char>(len);
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
  this->resize(buf.len + srcBuf.buf.len);
  memcpy(this->buf.base + this->buf.len - srcBuf.buf.len,
         srcBuf.buf.base,
         srcBuf.buf.len);
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
  this->resize(0);
  this->buf.base = src_buf.buf.base;
  this->buf.len = src_buf.buf.len;
  src_buf.buf.base = nullptr;
  src_buf.buf.len = 0;
}

void uvcpp_buf::clone_buf(const uvcpp_buf &src_buf) {
  this->resize(0);
  this->buf.base = src_buf.buf.base;
  this->buf.len = src_buf.buf.len;
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