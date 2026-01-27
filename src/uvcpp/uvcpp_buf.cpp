#include "uvcpp_buf.h"
#include <stdexcept>
#include <new>
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_buf::uvcpp_buf() : uv_buf(), _owner(true) {
  base = nullptr;
  len = 0;
}

uvcpp_buf::~uvcpp_buf() {
  if (_owner && base != nullptr) {
    UVCPP_VFREE(base)
  }
  base = nullptr;
  len = 0;
  _owner = true;
}

uvcpp_buf::uvcpp_buf(const uvcpp_buf &bf) : uvcpp_buf() {
  base = nullptr;
  len = 0;
  _owner = true;
  if (bf.len > 0 && bf.base != nullptr) {
    this->resize(bf.len);
    memcpy(base, bf.base, bf.len);
  }
}

uvcpp_buf &uvcpp_buf::operator=(const uvcpp_buf &bf) {
  if (this == &bf) {
    return *this; // 自赋值检查
  }

  if (_owner && base != nullptr) {
    UVCPP_VFREE(base)
  }
  base = nullptr;
  len = 0;
  _owner = true;

  if (bf.len > 0 && bf.base != nullptr) {
    this->resize(bf.len);
    memcpy(base, bf.base, bf.len);
  } else {
    // leave empty buffer
  }
  return *this;
}

uvcpp_buf::uvcpp_buf(const char *bf, size_t sz) : uvcpp_buf() {
  base = nullptr;
  len = 0;
  _owner = true;

  if (sz > 0) {
    if (bf == nullptr) {
      throw std::invalid_argument("Input buffer pointer is null");
    }
    this->resize(sz);
    memcpy(base, bf, sz);
  }
}

uvcpp_buf::uvcpp_buf(const ::std::string &str) {
  base = nullptr;
  len = 0;
  _owner = true;

  if (!str.empty()) {
    this->resize(str.size());
    memcpy(base, str.c_str(), str.size());
  }
}

void *uvcpp_buf::operator new(size_t size) {
  void *p = uvcpp::uv_alloc_bytes(size);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void uvcpp_buf::operator delete(void *p) {
  UVCPP_VFREE(p)
}

uvcpp_buf::uvcpp_buf(const uv_buf &bf) {
  base = nullptr;
  len = 0;
  _owner = true;
  if (bf.len > 0 && bf.base != nullptr) {
    this->resize(bf.len);
    memcpy(base, bf.base, bf.len);
  }
}

uvcpp_buf &uvcpp_buf::operator=(const uv_buf &bf) {
  if (this == reinterpret_cast<const uvcpp_buf *>(&bf)) {
    return *this; // 自赋值检查 (防护，尽管不太可能)
  }

  if (_owner && base != nullptr) {
    UVCPP_VFREE(base)
  }
  base = nullptr;
  len = 0;
  _owner = true;
  if (bf.len > 0 && bf.base != nullptr) {
    this->resize(bf.len);
    memcpy(base, bf.base, bf.len);
  }
  return *this;
}
uvcpp_buf uvcpp_buf::operator+(const uvcpp_buf &bf) const {
  uvcpp_buf vbf;
  size_t total_size = len + bf.len;

  if (total_size > 0) {
    vbf.resize(total_size);
    if (len > 0 && base != nullptr) {
      memcpy(vbf.base, base, len);
    }
    if (bf.len > 0 && bf.base != nullptr) {
      memcpy(vbf.base + len, bf.base, bf.len);
    }
  }

  return vbf;
}
char uvcpp_buf::operator[](const int num) const {
  if (base == nullptr) {
    throw std::out_of_range("Buffer is null");
  }
  if (num < 0 || static_cast<size_t>(num) >= len) {
    throw std::out_of_range("Index out of bounds");
  }
  return base[num];
}
bool uvcpp_buf::operator==(const uvcpp_buf &bf) const {
  if (len != bf.len) {
    return false;
  }
  if (this->len == 0) {
    return true; // 两个空缓冲区相等
  }
  if (base == nullptr || bf.base == nullptr) {
    return base == bf.base; // 都为null则相等，否则不相等
  }
  return memcmp(base, bf.base, len) == 0;
}
bool uvcpp_buf::operator!=(const uvcpp_buf &bf) const { return !(*this == bf); }
bool uvcpp_buf::operator>(const uvcpp_buf &bf) const { return this->len > bf.len; }
bool uvcpp_buf::operator>=(const uvcpp_buf &bf) const { return this->len >= bf.len; }
bool uvcpp_buf::operator<(const uvcpp_buf &bf) const { return this->len < bf.len; }
bool uvcpp_buf::operator<=(const uvcpp_buf &bf) const { return this->len <= bf.len; }
int uvcpp_buf::init() {
  this->set_zero();
  return 0;
}

void uvcpp_buf::set_zero() {
  if (base == nullptr && len > 0) {
    throw std::logic_error("Buffer state inconsistency: null pointer with non-zero length");
  }
  if (base != nullptr && len > 0) {
    memset(base, 0, len);
  }
}

void uvcpp_buf::resize(size_t sz) {
  if (sz == 0) {
    if (base != nullptr && _owner) {
      UVCPP_VFREE(base)
    }
    base = nullptr;
    len = 0;
    _owner = true;
    return;
  } else if (len == sz) {
    return;
  }

  if (base != nullptr) {
    if (!_owner) {
      // currently pointing to external memory; allocate new owned buffer
    char *new_base = (char *)uvcpp::uv_alloc_bytes(sz);
    if (new_base == nullptr) {
      throw std::bad_alloc();
    }
      if (len > 0 && base != nullptr) {
        memcpy(new_base, base, (std::min)(len, sz));
      }
      base = new_base;
      _owner = true;
      if (len < sz) {
        memset(base + len, 0, sz - len);
      }
      len = sz;
    } else {
      char *new_base = (char *)realloc(base, sz);
      if (new_base == nullptr) {
        throw std::bad_alloc();
      }
      base = new_base;
      if (len < sz) {
        memset(base + len, 0, sz - len);
      }
      len = sz;
    }
  } else {
    base = (char *)uvcpp::uv_alloc_bytes(sz);
    len = sz;
    _owner = true;
  }
}

void uvcpp_buf::set_data(const char *bf, size_t sz, bool clean) {
  if (bf == nullptr && sz > 0) {
    throw std::invalid_argument("Cannot set null buffer pointer with non-zero size");
  }
  if (clean && base != nullptr && _owner) {
    this->resize(0);
  }
  // set as external data view; do not take ownership
  base = const_cast<char *>(bf);
  len = sz;
  _owner = false;
}

char *uvcpp_buf::get_data() const { return base; }

const char *uvcpp_buf::get_const_data() const { return base; }

unsigned char *uvcpp_buf::get_udata() const { return (unsigned char *)base; }

const unsigned char *uvcpp_buf::get_const_udata() const {
  return (unsigned char *)base;
}

size_t uvcpp_buf::size() const { return len; }

void uvcpp_buf::clear() { this->resize(0); }

void uvcpp_buf::clone(const uvcpp_buf &srcBuf) {

  this->resize(srcBuf.size());
  memcpy(base, srcBuf.get_data(), srcBuf.size());
}

::std::string uvcpp_buf::to_string() const {
  if (base == nullptr) {
    return std::string();
  }
  return ::std::string(base, len);
}

void uvcpp_buf::clone_data(const char *bf, size_t sz) {
  if (bf == nullptr && sz > 0) {
    throw std::invalid_argument("Cannot clone null buffer pointer with non-zero size");
  }
  this->resize(sz);
  if (sz > 0) {
    memcpy(base, bf, sz);
  }
}

void uvcpp_buf::append(const uvcpp_buf &srcBuf) {
  this->resize(len + srcBuf.len);
  memcpy(base + len - srcBuf.len, srcBuf.base, srcBuf.len);
}

void uvcpp_buf::append_data(const char *bf, size_t sz) {
  if (bf == nullptr && sz > 0) {
    throw std::invalid_argument("Cannot append null buffer pointer with non-zero size");
  }
  this->resize(len + sz);
  if (sz > 0) {
    memcpy(base + len - sz, bf, sz);
  }
}

void uvcpp_buf::insert_data(uint64_t point, const char *bf, size_t sz) {
  if (bf == nullptr && sz > 0) {
    throw std::invalid_argument("Cannot insert null buffer pointer with non-zero size");
  }
  if (point > len) {
    throw std::out_of_range("Insert point beyond buffer length");
  }

  if (point >= this->len) {
    this->append_data(bf, sz);
  } else {
    size_t old_len = len;
    this->resize(len + sz);
    memmove(base + point + sz, base + point, old_len - point);
    memcpy(base + point, bf, sz);
  }
}

void uvcpp_buf::rewrite_data(uint64_t point, const char *bf, size_t sz) {
  if (bf == nullptr && sz > 0) {
    throw std::invalid_argument("Cannot rewrite with null buffer pointer with non-zero size");
  }
  if (point > len) {
    throw std::out_of_range("Rewrite point beyond buffer length");
  }

  if (point + sz > len) {
    this->resize(point + sz);
  }
  if (sz > 0) {
    memcpy(base + point, bf, sz);
  }
}
} // namespace uvcpp