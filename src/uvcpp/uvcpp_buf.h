/**
 * @file src/uvcpp/uvcpp_buf.h
 * @brief Higher-level buffer utility built on top of `uv_buf`.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_BUF_H
#define SRC_UVCPP_UVCPP_BUF_H

#include <uvcpp/uv_define.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uv_buf.h>
#include <string>

namespace uvcpp {

class UVCPP_API uvcpp_buf : public uv_buf {
public:
  UVCPP_DEFINE_COPY_FUNC(uvcpp_buf)
  explicit uvcpp_buf();
  ~uvcpp_buf();
  explicit uvcpp_buf(const char *bf, size_t sz);
  explicit uvcpp_buf(const ::std::string &str);

  void *operator new(size_t size);
  void operator delete(void *p);

  explicit uvcpp_buf(const uv_buf &bf);
  uvcpp_buf &operator=(const uv_buf &bf);

  uvcpp_buf operator+(const uvcpp_buf &bf) const;

  char operator[](const int num) const;

  bool operator==(const uvcpp_buf &bf) const;
  bool operator!=(const uvcpp_buf &bf) const;
  bool operator>(const uvcpp_buf &bf) const;
  bool operator>=(const uvcpp_buf &bf) const;
  bool operator<(const uvcpp_buf &bf) const;
  bool operator<=(const uvcpp_buf &bf) const;

  int init();

  void set_zero();
  void resize(size_t sz);
  // set_data sets an external view (non-owning) when clean==false; when clean==true
  // it will copy data and take ownership.
  void set_data(const char *bf, size_t sz, bool clean = true);
  void clone_data(const char *bf, size_t sz);
  void append(const uvcpp_buf &srcBuf);
  void append_data(const char *bf, size_t sz);

  void insert_data(uint64_t point, const char *bf, size_t sz);
  void rewrite_data(uint64_t point, const char *bf, size_t sz);
  char *get_data() const;
  const char *get_const_data() const;

  unsigned char *get_udata() const;
  const unsigned char *get_const_udata() const;

  size_t size() const;

  void clear();
  void clone(const uvcpp_buf &cloneBuf);

  ::std::string to_string() const;

private:
  // Indicates whether this object owns base and should free it on destruction.
  bool _owner = true;
};
} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_BUF_H