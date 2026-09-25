/**
 * @file src/web/uvcpp_http_response.h
 * @brief HTTP response object — readable/writable, serializable.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Represents a complete HTTP response message.  Can be:
 * - Built manually and serialised via to_string() for sending back
 *   from a server
 * - Constructed from a uvcpp_http_parser result via from_parser()
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_RESPONSE_H
#define SRC_WEB_UVCPP_HTTP_RESPONSE_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEB_ENABLE

#include <string>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_common.h>

namespace uvcpp {

class uvcpp_http_parser;

class UVCPP_API uvcpp_http_response {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_http_response)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_http_response)

  // -------------------------------------------------------------------
  // Fields
  // -------------------------------------------------------------------

  uvcpp_http_version  version        = static_cast<uvcpp_http_version>(1);
  http_status   status_code    = http_status::OK;
  std::string   status_message = "OK";
  http_headers  headers;
  uvcpp_buf     body;

  /// When true, the server does NOT send this response immediately after the
  /// handler returns; the handler must call uvcpp_http_server::send_response
  /// later (on the loop thread, e.g. from an async completion callback).
  /// Used for deferred/streaming responses (large files, slow async work).
  bool          deferred = false;

  /**
   * @brief 这条响应回答的是哪条 h2 流；**0 表示 HTTP/1.1**。
   *
   * 由服务端在调处理函数**之前**填好，所以处理函数把它整个拷走再延迟发送也
   * 带着走。处理函数若**整对象替换**（`resp = uvcpp_http_response::make(...)`）
   * 就会把它冲掉 —— h2 连接上那种写法必须自己把它设回去，或者改用带
   * `stream_id` 的 `uvcpp_http_server::send_response` 重载。
   */
  int32_t       stream_id = 0;

  /**
   * @brief 对端明确表示**这条请求没被处理过**，重发一次是安全的。
   *
   * 只有 HTTP/2 的 `REFUSED_STREAM`（RFC 9113 §8.7：请求在被处理之前就被关掉
   * 了）会让它为真，其余一切情形都是假 —— 包括 h1（这个协议里没有等价信号）。
   *
   * **假不等于"处理过了"**：连接断掉是"结果未知"，`CANCEL` 是"对端不要这条流
   * 了"（那时它可能已经处理完），两者都为假。所以判据只能是"真 ⇒ 可以重试"，
   * 反过来推不成立。
   */
  bool          retryable = false;

  /**
   * @brief 这份 body 的**可缓存身份**；空 = 不参与压缩变体缓存。
   *
   * 由**内容来源**填（静态文件那条路填 `路径 + mtime + size`），不由压缩层填 ——
   * "这份内容什么时候算变了"只有来源知道，而压缩层只把它当查表的键：不解释
   * 这个串、不假设它唯一、不假设它有格式。
   *
   * 身份一变就必须重新压，所以填的字段必须**完整覆盖**"内容是否已改变"；
   * 漏一个就会拿旧字节去答新文件。
   */
  std::string   cache_tag;

  // -------------------------------------------------------------------
  // Header operations
  // -------------------------------------------------------------------

  /** @brief Set a header (case-insensitive overwrite). */
  void set_header(const std::string& key, const std::string& value);

  /**
   * @brief 同上，但头名收 `const char*`（字面量、`c_str()`）。
   *
   * 存在的理由：`const std::string&` 形参会让每个 > 15 字符的字面量在调用点
   * 构造一个堆串（MSVC SSO 上限 15），而 `"transfer-encoding"` 是 17。详见
   * `uvcpp_http_common.h` 里那族重载的注释。
   */
  void set_header(const char* key, const std::string& value);

  /**
   * @brief 名字与值**都是**字面量。
   *
   * 除了省掉头名的临时串，还省掉**值**的一次拷贝：`push_back({name, value})`
   * 是先构造临时 `http_header`（值的 `std::string` 在这里分配一次）再搬进向量，
   * 这里让两个成员各自从 C 串就地构造再移动 ⇒ 长值从两次分配降到一次。
   * 详见 `uvcpp_http_common.h` 里同名重载的注释。
   */
  void set_header(const char* key, const char* value);

  /** @brief Get a header value by name (case-insensitive). */
  std::string get_header(const std::string& key,
                         const std::string& default_val = "") const;

  /** @copydoc set_header(const char*, const std::string&) */
  std::string get_header(const char* key,
                         const std::string& default_val = "") const;

  /** @brief Check if a header exists (case-insensitive). */
  bool has_header(const std::string& key) const;

  /**
   * @brief 把头表的**缓冲**从连接级的回收槽换进来（容量跨请求留下）。
   *
   * 为什么需要：响应对象每个请求新造一个，头表也从空开始 —— 不回收的话，光
   * 「让表长到 4~5 个位置」就要 `reserve(4)` + 一到两次 `_M_realloc_insert`，
   * 再加上 `resp = uvcpp_http_response::ok(...)` 那次整体拷贝赋值自带的一次
   * 分配。这四笔在 `probe_h` 的调用点普查里合起来是 **5.00 次/请求**（对面
   * hical 同一个量是 3.00），而它们一个字节的头都没多存。
   *
   * `spare_headers` 由**连接**持有（`conn_ctx::resp_hdr_recycle`），所以它的
   * 寿命就是连接的寿命；连接关掉时随 `conn_ctx` 一起释放。
   *
   * ★ 交换进来之后**必须**先把元素清掉再交给处理函数：`headers` 是公开成员，
   *   留着上一条请求的条目就是让处理函数静默读到上一条请求的头。所以清空这一步
   *   放在 `adopt_tables()` 里（而不是 `yield_tables()`）—— 这样"换进来的那块
   *   一定是干净的"这条不变式只在一个地方维护。
   */
  void adopt_tables(http_headers& spare_headers);

  /**
   * @brief 把头表交还给回收槽；元素先清掉，只留容量。
   *
   * 调用点在 `uvcpp_http_server::send_response()` 的 h1 出口：报文已经序列化进
   * 写缓冲、体也交给写队列了，头表到这里再也用不着。见那个函数里的注释。
   */
  void yield_tables(http_headers& spare_headers);

  /** @copydoc set_header(const char*, const std::string&) */
  bool has_header(const char* key) const;

  /** @brief Remove a header by name (case-insensitive). */
  void remove_header(const std::string& key);

  /** @copydoc set_header(const char*, const std::string&) */
  void remove_header(const char* key);

  // -------------------------------------------------------------------
  // Content-Type shortcut
  // -------------------------------------------------------------------

  std::string content_type() const;
  void set_content_type(const std::string& ct);

  /** @copydoc set_header(const char*, const char*) */
  void set_content_type(const char* ct);

  // -------------------------------------------------------------------
  // Serialization
  // -------------------------------------------------------------------

  /**
   * @brief Serialise to wire format.
   *
   * Produces: HTTP/version STATUS_CODE REASON\r\n
   *           Header-Name: value\r\n
   *           ...
   *           \r\n
   *           body
   *
   * @param include_body 为 false 时**只**输出头部（状态行 + 各头 + 那个空行），
   *        body 一个字节都不写。HEAD 响应与流式响应都走这一支 —— 前者的 body
   *        按协议就不该发，后者的 body 由调用方随后逐块写。
   *
   *        **chunked 下这一支连终止块 `0\r\n\r\n` 也不发**。这不是"少发了一
   *        段"，而是这一支的定义：它只序列化头部。流式响应结束时由调用方自己
   *        发终止块（见 `uvcpp_http_server::end_stream`）。
   */
  std::string to_string(bool include_body = true) const;

  // -------------------------------------------------------------------
  // Construction from parser
  // -------------------------------------------------------------------

  static uvcpp_http_response from_parser(const uvcpp_http_parser& parser,
                                          const uvcpp_buf& body);

  // -------------------------------------------------------------------
  // Serialization into a caller-owned buffer
  // -------------------------------------------------------------------

  /**
   * @brief 与 @ref to_string **逐字节相同**的序列化，但写进调用方给的串
   *        （先把 `out` 清空、容量留下）。
   *
   * 给"同一块缓冲跨请求复用"的用法：服务端把它挂在连接上，于是第二次起
   * `est > out.capacity()` 恒假 ⇒ 整条响应**一次分配都没有**。它比 @ref to_string
   * 省下的那一次分配是**纯浪费**：服务端拿到字节之后立刻把它们拷进写请求自己的
   * 头部缓冲（`uvcpp_tcp_client::write_owned` 里的 memcpy），那个临时串随即析构。
   *
   * @param out          目的地；**内容被清空**，容量保留。
   * @param include_body 同 @ref to_string：false 时只写头部块（含末尾空行）。
   */
  void to_string_into(std::string& out, bool include_body = true) const;

  // -------------------------------------------------------------------
  // Quick-response factories (for server use)
  // -------------------------------------------------------------------

  /** @brief Build a minimal 200 OK response with body. */
  static uvcpp_http_response ok(const char* body, size_t len,
                                 const std::string& content_type = "text/plain");

  /** @brief Build a 404 Not Found response. */
  static uvcpp_http_response not_found(const char* body = nullptr, size_t len = 0);

  /** @brief Build a 500 Internal Server Error response. */
  static uvcpp_http_response server_error(const char* body = nullptr, size_t len = 0);

  /** @brief Build a response with a given status code and optional body. */
  static uvcpp_http_response make(http_status code,
                                   const char* body = nullptr, size_t len = 0,
                                   const std::string& content_type = "text/plain");
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_RESPONSE_H
