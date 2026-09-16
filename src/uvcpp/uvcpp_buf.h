/**
 * @file src/uvcpp/uvcpp_buf.h
 * @brief Higher-level buffer utility built on top of `uv_buf_t`.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_BUF_H
#define SRC_UVCPP_UVCPP_BUF_H

#include <uvcpp/uvcpp_define.h>
#include <string>

namespace uvcpp {

class UVCPP_API uvcpp_buf{
public:
  UVCPP_DEFINE_FUNC(uvcpp_buf)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_buf)

  explicit uvcpp_buf(const char *bf, size_t sz);
  explicit uvcpp_buf(const ::std::string &str);

  void *operator new(size_t size);
  void operator delete(void *p);

  explicit uvcpp_buf(const uv_buf_t &bf);
  uvcpp_buf &operator=(const uv_buf_t &bf);

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
  // resize 只改**可见长度**：缩小时保留已分配的容量，增大时按容量翻倍复用
  // （见 capacity_），所以 append/append_data 的均摊代价是 O(1) 而不是 O(n)。
  // 新露出来的那一段仍然清零，与旧行为一致。
  void resize(size_t sz);
  // set_data 存的是**外部视图**：不持有这块内存，析构不 free 它，后续扩容
  // 会先把内容拷进自有的块（不会去 free 外部指针）。
  void set_data(const char *bf, size_t sz);
  void clone_data(const char *bf, size_t sz);
  void clone_data(const uv_buf_t &src_buf);
  void append(const uvcpp_buf &srcBuf);
  void append_data(const char *bf, size_t sz);
  // move_buf 真正转移所有权（含容量）；clone_buf 是**深拷贝**，拷贝出来的块
  // 归自己所有，与源再无关系。
  void move_buf(uvcpp_buf &src_buf);
  void clone_buf(const uvcpp_buf &src_buf);

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

  uv_buf_t *out_uv_buf();
  void in_uv_buf(uv_buf_t* bf);

  static void alloc_buf(uv_buf_t *bf, size_t len);
  static void free_buf(uv_buf_t *bf);

private:
  // 释放自有块并回到"空"。（外部视图不会被释放。）
  void free_own();

  uv_buf_t buf;
  // buf.base 指向的块**实际**有多少字节，只有自有块才有意义：
  //   capacity_ > 0  —— 我们自己分配的，resize 可以在 sz <= capacity_ 时就地复用；
  //   capacity_ == 0 —— 要么是空的，要么是外部视图（set_data），既不能 free
  //                     也不能就地写，扩容必须先拷进自有块。
  // 只有 resize() 会建立容量；out_uv_buf/move_buf 把块交出去之后必须清零。
  size_t capacity_ = 0;
};
} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_BUF_H