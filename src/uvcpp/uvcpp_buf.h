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
#include <memory>
#include <string>

namespace uvcpp {

class UVCPP_API uvcpp_buf{
public:
  UVCPP_DEFINE_FUNC(uvcpp_buf)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_buf)

  // 移动：**真正转移**（含共享手柄），源变空。
  //
  // 必须显式写。本类有用户声明的拷贝构造与析构，编译器因此**不会**隐式生成
  // 移动构造/移动赋值 —— 于是 `std::move(x)` 会静默退化成**深拷贝**：`x` 原封
  // 不动，接收方拿到一份拷来的字节。带上共享手柄之后这一点会咬人：把一份
  // "接管来的 body" 装进队列、再 `std::move` 出来，本来零拷贝的两步之间又多出
  // 一整份拷贝，而且不会有任何报错。所以这里是"补上缺的那一半"，不是新语义。
  uvcpp_buf(uvcpp_buf &&obj) noexcept;
  uvcpp_buf &operator=(uvcpp_buf &&obj) noexcept;

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

  // -------------------------------------------------------------------
  // 共享视图（零拷贝接管一份已有的缓冲）
  // -------------------------------------------------------------------

  /**
   * @brief 接管一份**已存在**的共享缓冲，不拷贝字节。
   *
   * 传进来之后本对象**只读地**指向 `*src`，靠引用计数保证那块内存活着 ——
   * 所以它比 `set_data()` 那种裸外部视图多一层保护：调用方把 `shared_ptr`
   * 放掉也不会让它悬空。
   *
   * **既有入口的语义一个都没变**：`clone*` / `append*` / `resize` / `set_data`
   * 仍然是"拷进来"。共享只影响"这份字节从哪来"。任何**写**操作会先把共享视图
   * 物化成一份自有的块（见私有 `materialize()`），所以共享之后又被改写只会
   * 退化成一次拷贝，**不会**写进别人的串、也不会写进只读内存。
   *
   * `src` 为空、或指向一个空串时，结果与 `clear()` 一致（空缓冲）。
   */
  void share(::std::shared_ptr<const ::std::string> src);

  /** @brief 本对象当前是否是一份共享视图（即 `share()` 之后、还没被写过）。 */
  bool is_shared() const;

  /**
   * @brief 取出当前持有的共享缓冲；不是共享视图时返回空指针。
   *
   * 典型用法是"把这份 body 同时交给变体表和这条响应"（两边各持一份引用，
   * 都靠同一个 `shared_ptr` 活着），而不是拿它去改内容。
   */
  ::std::shared_ptr<const ::std::string> shared_ref() const;

  ::std::string to_string() const;

  uv_buf_t *out_uv_buf();
  void in_uv_buf(uv_buf_t* bf);

  static void alloc_buf(uv_buf_t *bf, size_t len);
  static void free_buf(uv_buf_t *bf);

private:
  // 释放自有块并回到"空"。（外部视图不会被释放；共享视图只是解除引用。）
  void free_own();

  // 把共享视图变成一份**自有的**块（写操作的前置条件）。不是共享视图时空操作。
  // 所有会写字节的入口都必须先过这里 —— 否则写的就是别人（可能是只读）的内存。
  // 注意是 **const**：它要在 get_data()/get_udata() 这两个 const 访问器里被
  // 调用。（只把 shared_ 标 mutable 是不够的 —— 那让成员能在 const 上下文里被
  // 换掉，不让非 const 成员函数能在 const 上下文里被调用。）
  void materialize() const;

  // 下面三个成员（buf / capacity_ / shared_）是一套状态，materialize() 三个
  // 都要改，所以要么一起 mutable 要么都不。
  // 这不是新开的口子：本类**既有**的 const 访问器（get_data() / get_udata()）
  // 交出的本来就是可写指针 —— 这个类的 const 只保护「句柄的同一性」，不保护
  // 那些字节。
  mutable uv_buf_t buf;
  // buf.base 指向的块**实际**有多少字节，只有自有块才有意义：
  //   capacity_ > 0  —— 我们自己分配的，resize 可以在 sz <= capacity_ 时就地复用；
  //   capacity_ == 0 —— 要么是空的，要么是外部视图（set_data / share），既不能
  //                     free 也不能就地写，扩容必须先拷进自有块。
  // 只有 resize()/materialize() 会建立容量；out_uv_buf/move_buf 把块交出去之后
  // 必须清零。
  mutable size_t capacity_ = 0;

  // 共享视图的持有者。非空时：buf.base 指向 *shared_ 的内容、buf.len ==
  // shared_->size()、capacity_ == 0（即"外部视图"）。
  //
  // 为什么是 mutable：见上面 buf 那一段（本类的 const 不保护字节）。
  mutable ::std::shared_ptr<const ::std::string> shared_;
};
} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_BUF_H