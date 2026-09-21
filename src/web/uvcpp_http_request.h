/**
 * @file src/web/uvcpp_http_request.h
 * @brief HTTP request object — readable/writable, serializable.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Represents a complete HTTP request message.  Can be:
 * - Built manually and serialised via to_string() for sending
 * - Constructed from a uvcpp_http_parser result via from_parser()
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_REQUEST_H
#define SRC_WEB_UVCPP_HTTP_REQUEST_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEB_ENABLE

#include <string>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_common.h>

namespace uvcpp {

// Forward declaration
class uvcpp_http_parser;

class UVCPP_API uvcpp_http_request {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_http_request)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_http_request)

  // -------------------------------------------------------------------
  // Fields
  // -------------------------------------------------------------------

  http_method   method  = http_method::HTTP_GET;
  std::string   url     = "/";
  uvcpp_http_version  version = static_cast<uvcpp_http_version>(1);
  http_headers  headers;
  uvcpp_buf     body;

  /**
   * @brief 这条请求来自哪条 h2 流；**0 表示 HTTP/1.1**。
   *
   * 一条 h2 连接上同时跑着多条流，而处理函数拿到的是裸 `uvcpp_tcp_client*`
   * —— 没有这个字段，一个延迟应答的处理函数就说不清自己在替哪条流回话。
   */
  int32_t       stream_id = 0;

  // -------------------------------------------------------------------
  // Header operations
  // -------------------------------------------------------------------

  /** @brief Set a header (case-insensitive overwrite). */
  void set_header(const std::string& key, const std::string& value);

  /** @brief Get a header value by name (case-insensitive). */
  std::string get_header(const std::string& key,
                         const std::string& default_val = "") const;

  /** @brief Check if a header exists (case-insensitive). */
  bool has_header(const std::string& key) const;

  /** @brief Remove a header by name (case-insensitive). */
  void remove_header(const std::string& key);

  // -------------------------------------------------------------------
  // Content-Type shortcut
  // -------------------------------------------------------------------

  /** @brief Get Content-Type value (convenience). */
  std::string content_type() const;

  /** @brief Set Content-Type header. */
  void set_content_type(const std::string& ct);

  // -------------------------------------------------------------------
  // Serialization
  // -------------------------------------------------------------------

  /**
   * @brief Serialise to a complete HTTP request wire format.
   *
   * Produces: METHOD URL HTTP/version\r\n
   *           Header-Name: value\r\n
   *           ...
   *           \r\n
   *           body
   *
   * The Host header is automatically inserted (from headers or from
   * the URL host portion) if not already present for HTTP/1.1 requests.
   */
  std::string to_string() const;

  // -------------------------------------------------------------------
  // Construction from parser
  // -------------------------------------------------------------------

  /**
   * @brief Build an HTTP request from a completed parser and body data.
   * @param parser  A parser whose is_complete() returns true.
   * @param body    The accumulated body data (from body callbacks).
   * @return        A populated request object.
   */
  static uvcpp_http_request from_parser(const uvcpp_http_parser& parser,
                                         const uvcpp_buf& body);

  // -------------------------------------------------------------------
  // Convenience: build common requests
  // -------------------------------------------------------------------

  /** @brief Build a simple GET request. */
  static uvcpp_http_request make_get(const std::string& url);

  /**
   * @brief Build a HEAD request — the headers of the GET, no body back.
   *
   * The response carries the `Content-Length` the GET would have and no
   * body at all; `uvcpp_http_client` handles that on every send path.
   */
  static uvcpp_http_request make_head(const std::string& url);

  /** @brief Build a simple POST request with body. */
  static uvcpp_http_request make_post(const std::string& url,
                                       const char* body, size_t len,
                                       const std::string& content_type = "application/octet-stream");

  // -------------------------------------------------------------------
  // 移动语义
  // -------------------------------------------------------------------

  /**
   * @brief 真正的移动构造 / 移动赋值 —— **不是**深拷贝。
   *
   * 为什么必须显式写出来：`UVCPP_DEFINE_COPY_FUNC` 只声明了拷贝那一对，而在
   * C++ 里**用户声明了拷贝赋值运算符就会抑制隐式移动赋值运算符的生成**。于是
   * `b = std::move(a)` 会静默地绑到 `operator=(const uvcpp_http_request&)`，
   * 把 `headers` 深拷一遍（1 次 vector 分配 + 每个头 2 个 `std::string`）。
   * 代码里写着 `std::move`、跑起来是拷贝，是最难查的一类"性能没问题"。
   * `uvcpp_buf` 的头注释里记过同一个坑，这里是它的第二例。
   *
   * **契约**：移动之后 `obj` 处于「已搬空」状态 —— `url` 为空串、`headers`
   * 为空、`body` 长度为 0（`uvcpp_buf` 的移动语义保证这一点）。`method` /
   * `version` / `stream_id` 是标量，默认移动赋值按值搬，**保留原值**；不要依赖
   * 它们，需要就在移动前取走。
   *
   * 这两条写成 `= default`，但**不是**"不新增导出符号"：本类是 `UVCPP_API`
   * （dllexport），MSVC 会把内联的 `= default` 成员一并导出。
   *
   * 实测（`dumpbin -exports` 比对改动前后那份 DLL）本类的导出符号 **28 → 30**：
   * 新增的正好是 `??0uvcpp_http_request@uvcpp@@QEAA@$$QEAV01@@Z`（移动构造）与
   * `??4uvcpp_http_request@uvcpp@@QEAAAEAV01@$$QEAV01@@Z`（移动赋值），**删除 0 个**。
   * 所以这是一次只增不减的 ABI 变化：用旧头文件编出来的消费者照样链接得上。
   */
  uvcpp_http_request(uvcpp_http_request &&obj) noexcept = default;
  uvcpp_http_request &operator=(uvcpp_http_request &&obj) noexcept = default;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_REQUEST_H
