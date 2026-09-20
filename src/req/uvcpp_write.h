/**
 * @file src/req/uvcpp_write.h
 * @brief Wrapper for uv_write_t to perform asynchronous stream writes.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_REQ_UVCPP_WRITE_H
#define SRC_REQ_UVCPP_WRITE_H

#include <req/uvcpp_req.h>
#include <uvcpp/uvcpp_buf.h>

namespace uvcpp {
/**
 * @brief Write request wrapper for stream write operations.
 *
 * Use `set_buf` / `set_src_buf` to configure buffers. Provide `m_write_cb`
 * to receive completion notifications.
 */
class UVCPP_API uvcpp_write : public uvcpp_req {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_write)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_write)

  /** @brief Initialize the write request. */
  int init();

  /** @brief Set the main buffer to write; if owner=true wrapper will free it. */
  void set_uv_buf(uv_buf_t *bf, bool owner = false);
  /** @brief Get the buffer previously set. */
  uv_buf_t *get_uv_buf();

  // -----------------------------------------------------------------
  // 第二块（头/体分开写，`uv_write` 的 `nbufs = 2`）
  // -----------------------------------------------------------------
  //
  // 为什么要两块：一条 HTTP 响应天然是**两段**（头部块 + 体），把两段先合并成
  // 一条再交下来，合并那一步就得把体整份拷一遍。`uv_write` 本来就收**数组**，
  // 所以"分开交"不需要协议层做任何事 —— 需要的是有人**保证第二块活到完成
  // 回调**，因为 libuv 的契约是"缓冲在回调之前一直有效"，它不会替你拷一份。
  //
  // 上面两个方法就是这份保证的两种形状：那块字节要么是**别人的**（靠引用计数
  // 借来用），要么是**本请求接管下来的**（自己释放）。两者与 `set_uv_buf` 的
  // 差别都在所有权，不在字节。

  /**
   * @brief 追加第二块：一份**共享视图**。
   *
   * @param bf   第二块的字节范围（通常是共享串的整段）。
   * @param hold 让这块存活的那个引用。本请求把它持到析构，于是 `uv_write`
   *             那句"缓冲要活到回调"由引用计数履行。`hold` 为空仍是合法的
   *             （等价于本请求不负责第二块的存活，调用方自己保证）。
   *
   * 必须在 `set_uv_buf` **之后**调用（第 1 块是在这一刻被快照进数组的）。
   *
   * 第 2 块**只有一个**：再调一次（两种入口混着调也算）会把上一个占用者换掉，
   * 旧的按它自己的所有权放掉（owned 的释放、view 的放引用）。
   */
  void append_uv_buf_view(uv_buf_t bf,
                          const ::std::shared_ptr<const ::std::string> &hold);

  /**
   * @brief 追加第二块：一块**自有**的块，所有权转移给本请求。
   *
   * @param bf 必须来自 `uvcpp_buf::out_uv_buf()`（它把块**连同所有权**交出来）；
   *           本请求析构时按 `uvcpp_buf::free_buf` + `uvcpp_free` 放掉。
   *           注意释放用的是**这个原指针**，所以调用方不要先给 `bf->base`
   *           加偏移再传进来（`free_buf` 释放的正是 `bf->base`）。
   *
   * 第 2 块**只有一个**：再调一次会把上一个占用者换掉（见 `append_uv_buf_view`）。
   * 同一个 `bf` 交两次不会双释放，但那是调用方的错 —— 所有权只能交一次。
   */
  void append_uv_buf_owned(uv_buf_t *bf);

  /**
   * @brief 本次写要交给 `uv_write` 的缓冲数组。
   *
   * 只有 1 块时返回 `get_uv_buf()`（与既有形状逐字一致）；有 2 块时返回内部
   * 那个连续数组。libuv 会在 `uv_write` 里把 `uv_buf_t` 数组自身拷走，但**不拷
   * 数据** —— 数据要活到回调，这正是上面两个 append 存在的理由。
   */
  uv_buf_t *get_uv_bufs();
  /** @brief 本次写的块数（1 或 2）。 */
  size_t get_uv_nbufs() const;
  /** @brief Set a source buffer (used for send_handle scenarios). */
  void set_src_buf(const uvcpp_buf *bf, bool owner = false);
  /** @brief Get the source buffer previously set. */
  const uvcpp_buf *get_src_buf();
  ::std::function<void(uvcpp_write*,int)> m_write_cb;
 public:

  /** @brief libuv uv_write_cb forwarded to m_write_cb. */
  static void callback_write(uv_write_t *req, int status);

  private:
  uv_buf_t *uv_buf = nullptr;
  const uvcpp_buf *src_buf = nullptr;
  bool uv_buf_owner = false;
  bool src_buf_owner = false;

  // 第 1 块仍由 uv_buf 持有（它可能是 out_uv_buf() 交出来的，也可能是空块），
  // 所以两块要交给 uv_write 时先把第 1 块**快照**进这个连续数组 —— `uv_write`
  // 收的是 `const uv_buf_t bufs[]`，两块必须连续。数组本身只要活到 `uv_write`
  // 返回（libuv 会把 uv_buf_t 数组拷进请求里），但**数据**要活到完成回调。
  size_t nbufs_ = 1;
  uv_buf_t pair_[2];
  // 第 2 块只有一个槽位，两种占用形状共用它；换占用者（含析构）一律走这里，
  // 否则被覆盖掉的那个再也没人放。
  void release_second();

  // 第 2 块若是自有块，这里记着它的**原始**头：释放必须用原指针（free_buf 释放
  // 的是 base），所以接收方不许先把 base 加偏移再传进来。
  uv_buf_t *second_owner = nullptr;
  // 第 2 块若是共享视图，靠它活着 —— 见 append_uv_buf_view。
  ::std::shared_ptr<const ::std::string> hold_;
};
} // namespace uvcpp

#endif // SRC_REQ_UVCPP_WRITE_H