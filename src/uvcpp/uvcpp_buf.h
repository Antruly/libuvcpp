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
#include <atomic>
#include <cstdint>
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

  /**
   * @brief "先共享又丢"的累计次数（进程内，跨对象）。
   *
   * 计数的是这一个事件：本对象是共享视图，而调用方**要写**，于是共享让位 ——
   * 物化成一份自有的块（见 `materialize()`）。它**不是错误**，是一次白拷贝：
   * 共享出去的那份内容没能被用上，还是拷了一遍。
   *
   * 为什么不在这里断言 / 抛：`clear()` 紧接 `clone()` 是**响应对象复用的正常
   * 序列**（keep-alive 上同一个响应对象被反复填），硬断言会把一段合法用法变成
   * 崩溃；而"写坏别人的缓冲"这件事已经被 `shared_ptr<const std::string>` 自己
   * 挡住了 —— **那个 `const` 就是防线**，不需要第二道。
   *
   * 所以要能**看见**而不是要拦住：这个读数在整条热路径上应当是 0（共享之后
   * 只读地发出去，一次都不物化）；非零就说明某处"先共享又丢"，值得逐处解释。
   */
  static uint64_t share_discard_count();

  /** @brief 把上面那个计数清零（给测试用，好让一句判据只覆盖它自己那一段）。 */
  static void reset_share_discard_count();

  ::std::string to_string() const;

  uv_buf_t *out_uv_buf();

  /**
   * @brief `out_uv_buf()` 的**无包装**版本：把块连同所有权填进调用方给的
   *        `uv_buf_t`，不再分配那个 `uv_buf_t` 包装。
   *
   * `out_uv_buf()` 返回的包装是 `uvcpp_alloc<uv_buf_t>()` 出来的，接收方还回来
   * 时要 `free_buf` + `uvcpp_free` **两步**。可接收方往往本来就把 `uv_buf_t`
   * 按值存着（`uvcpp_write` 的 `pair_[]` 就是），那个包装纯属中转 —— 于是每次
   * 发送白付两次分配。这条入口把包装省掉。
   *
   * 块的归属一字不改：交出去的那块归调用方。**但 `out` 不在堆上**，所以调用方
   * 只 `free_buf(out)`（或按自己的方式放 `out->base`），**不要** `uvcpp_free(out)`。
   *
   * 与 `out_uv_buf()` 一样会先 `materialize()`（共享视图要交出去得先物化）。
   */
  void release_uv_buf(uv_buf_t *out);

  void in_uv_buf(uv_buf_t* bf);

  static void alloc_buf(uv_buf_t *bf, size_t len);
  static void free_buf(uv_buf_t *bf);

private:
  // `resize` 的"不保证新露出那段是 0"版本。**故意是 private**：14 个调用点全在
  // 本类自己的成员函数里（见 `uvcpp_buf.cpp`），所以没必要往公开表面上加一个
  // "返回后内容未定义"的脚枪 —— 外面拿不到它，就不存在外部调用方踩它。
  //
  // 只给**紧接着会把新露出的那一段整段写满**的调用方用。`resize_impl` 清的是
  // 且仅是 `[old_len, sz)`（原地复用支清 `[len, sz)`，换块支清 `[old_len, sz)`），
  // 所以只要紧随其后的写操作恰好覆盖这一段，那次清零写的每个字节都会被下一行
  // 原样盖掉。`uvcpp_buf.cpp` 里这类形状都是 `resize(sz); memcpy(base(+off), src, n);`。
  //
  // 实测（服务端装置，**全量**普查：`--wrap=memset`，每调用一次就落一次盘，
  // 所以那份 dump 是完整清单而不是抽样）：
  //   * 100 B 的 GET   —— 每请求  3.4 次 / ~200 B
  //   * 1 MiB 的 POST  —— 每请求 38.0 次 / **3.00 MiB**（2 720 个请求的**全量**落盘：
  //                        103 384 次调用 / 8 557 089 392 字节），全部来自 `resize` 这个清零
  //   * 换成 `resize_uninitialized` 之后，**整个进程**（跑了 6 826 个 1 MiB 请求）
  //     的 memset 调用数是 **24**，全在启动期 —— 每请求 0 次 / 0 字节
  //   * 端到端 CPU：100 B 请求不可分辨（无回归）；1 MiB 请求 −18.7 µs/请求
  //     （−2.45%，4 轮交错可分辨）；8 MiB 请求在合计上不可分辨
  //
  // 契约：返回后 `[0, size())` 的内容**未定义**，调用方必须自己写满。
  //
  // ★ 有一处**刻意不用**它：`insert_data` 的"中间插入"支。那里 memmove 覆盖的是
  //   `[point+sz, old_len+sz)`，而 `point` 可以小于 `old_len-sz`，于是 `resize`
  //   清掉的 `[old_len, old_len+sz)` 未必被覆盖 —— 那一支留着 `resize`。
  void resize_uninitialized(size_t sz);

  // resize 与 resize_uninitialized 的共同实现（zero_tail 只决定"新露出那段
  // 要不要清零"这一件事，其余逻辑必须完全一致 —— 分成两份迟早会漂）。
  void resize_impl(size_t sz, bool zero_tail);

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

  // 见 share_discard_count()。**只**在"共享又丢"那一支里动它，所以非共享的
  // 路上（绝大多数）一个原子操作都不付。
  static ::std::atomic<uint64_t> share_discard_count_;
};
} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_BUF_H