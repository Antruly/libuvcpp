/**
 * @file src/webapp/uvcpp_web_request.h
 * @brief 请求封装：在 `uvcpp_http_request` 之上给业务处理器用的视图。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它解决什么问题
 * --------------
 * `uvcpp_http_request` 是**协议层**的表示：`url` 是还没拆过的原始请求目标，
 * `headers` 是一个线性 `std::vector`，每次取一个头都要全表扫一遍、每次都做
 * 一次大小写不敏感比较；查询串、Cookie、表单、JSON 都要使用者自己解。
 *
 * 这一层把它变成**应用层**的视图：路径/查询串已经拆好，关心的几个头已经
 * 解析并缓存，JSON/表单/Cookie 有现成的取值接口。
 *
 * 关于拷贝
 * --------
 * 构造函数会**接管**源请求的 body（`uvcpp_buf::move_buf`，真正的所有权转移，
 * 不复制字节），其余字段（method/url/headers）走正常拷贝 —— 它们是小的、
 * 有界的。大 body（上传的文件、大 JSON）因此只被搬一次，不会被复制。
 *
 * **代价**：`take_from()` 之后源 `uvcpp_http_request` 的 body 会是空的。
 * 调用方在交出请求之后不得再读它的 body。
 *
 * 线程约定
 * --------
 * 本对象只在 **loop 线程**上访问（`uvcpp_web_context::post()` 会把跨线程的
 * 使用拉回 loop 线程）。因此懒解析的缓存直接用了 `mutable`，没有加锁 ——
 * 加锁会为了一条单线程路径付出每请求一次的原子操作代价。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_REQUEST_H
#define SRC_WEBAPP_UVCPP_WEB_REQUEST_H

#include <string>
#include <utility>
#include <vector>

#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_export.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_util.h>

namespace uvcpp {

class UVCPP_API uvcpp_web_request {
 public:
  uvcpp_web_request();
  ~uvcpp_web_request();

  // 不可拷贝：持有从源请求搬过来的 body，拷贝语义要么是双重释放、要么是
  // 一次昂贵的大块复制，两种都不该被隐式触发。
  uvcpp_web_request(const uvcpp_web_request&) = delete;
  uvcpp_web_request& operator=(const uvcpp_web_request&) = delete;

  // -------------------------------------------------------------------
  // 填充（框架内部调用）
  // -------------------------------------------------------------------

  /**
   * @brief 从 http 层的请求填充本对象。
   *
   * **会搬走 `src` 的 body**（`src.body` 之后为空）。其余字段是拷贝。
   * 调用之后不得再使用 `src.body`。
   */
  void take_from(uvcpp_http_request& src);

  /** @brief 由派发器填充对端信息。 */
  void set_peer(const std::string& ip, unsigned int port);

  /** @brief 由路由器填充一个路径参数（`:id` / `*path` 绑定到的值）。 */
  void set_param(const std::string& name, const std::string& value);

  // -------------------------------------------------------------------
  // 基本信息
  // -------------------------------------------------------------------

  http_method method() const { return method_; }
  /** @brief 方法名（"GET"、"POST"…），静态字符串。 */
  const char* method_name() const;

  /** @brief 原始请求目标，**含查询串**（可能有百分号编码）。 */
  const std::string& raw_url() const { return raw_url_; }

  /** @brief 解码并归一化后的路径。查询串已剥离。 */
  const std::string& path() const { return path_; }

  /** @brief 未解码的路径（需要原样转发时可以拿这个）。 */
  const std::string& raw_path() const { return raw_path_; }

  /** @brief 原始查询串（不含前导 `?`，未解码）。 */
  const std::string& query_string() const { return query_string_; }

  uvcpp_http_version version() const { return version_; }
  /** @brief 协议字符串（"HTTP/1.1"），静态字符串。 */
  const char* version_name() const;

  /** @brief 对端 IP。未填充时返回空串。 */
  const std::string& peer_ip() const { return peer_ip_; }
  /** @brief 对端端口。未填充时为 0。 */
  unsigned int peer_port() const { return peer_port_; }

  // -------------------------------------------------------------------
  // 报头
  // -------------------------------------------------------------------

  /** @brief 取一个头（大小写不敏感）。不存在返回 `def`。 */
  std::string header(const std::string& name,
                     const std::string& def = std::string()) const;
  /** @brief 头是否存在（大小写不敏感）。 */
  bool has_header(const std::string& name) const;

  /** @brief 全部头，按收到的顺序。 */
  const http_headers& headers() const { return src_.headers; }

  /** @brief `Content-Type` 的完整值（含 `; charset=...`），已缓存。 */
  const std::string& content_type() const { return content_type_; }

  /**
   * @brief `Content-Length` 头声明的字节数。
   *
   * 头缺失或不是合法数字时返回 0。注意这是**声明值**，不是实际收到的
   * 字节数 —— 后者用 `body_size()`。两者不一致本身就是攻击信号。
   */
  size_t content_length() const { return content_length_; }

  /** @brief `Host` 头，已缓存。 */
  const std::string& host() const { return host_; }

  /**
   * @brief 客户端是否接受某种内容编码（如 "gzip"）。
   *
   * 解析 `Accept-Encoding` 并**尊重 q 值**：`gzip;q=0` 表示明确拒绝，
   * 此时返回 false。`*` 作为通配处理。`identity` 在没有显式拒绝时恒为
   * 可接受（RFC 7231 §5.3.4）。
   */
  bool accepts_encoding(const std::string& encoding) const;

  /** @brief 连接在本次响应之后是否应当保持（RFC 7230 §6.3）。 */
  bool is_keep_alive() const;

  // -------------------------------------------------------------------
  // 查询串 / 表单 / Cookie / 路径参数
  // -------------------------------------------------------------------

  /** @brief 取一个查询参数。不存在返回 nullptr（注意：可能是空串值）。 */
  const std::string* query(const std::string& name) const;

  /** @brief 全部查询参数，保序、允许重复键。 */
  const std::vector<std::pair<std::string, std::string> >& query_params() const;

  /** @brief `Content-Type` 是否为 `application/x-www-form-urlencoded`。 */
  bool is_form() const;
  /** @brief `Content-Type` 是否为 `multipart/form-data`。 */
  bool is_multipart() const;

  /** @brief 取一个表单字段（application/x-www-form-urlencoded）。 */
  const std::string* form(const std::string& name) const;
  /** @brief 全部表单字段。 */
  const std::vector<std::pair<std::string, std::string> >& form_params() const;

  /** @brief 取一个 Cookie。不存在返回 nullptr。 */
  const std::string* cookie(const std::string& name) const;

  /** @brief 取一个路径参数（由路由器填充）。 */
  const std::string* param(const std::string& name) const;

  /** @brief 全部路径参数。 */
  const std::vector<std::pair<std::string, std::string> >& params() const {
    return params_;
  }

  // -------------------------------------------------------------------
  // 报文体（二进制安全）
  // -------------------------------------------------------------------

  /** @brief body 起始地址。无 body 时为 nullptr。 */
  const char* body_data() const;
  /** @brief body 字节数（**不是** C 字符串长度，body 里可以有 NUL）。 */
  size_t body_size() const;
  /** @brief body 是否为空。 */
  bool body_empty() const;
  /** @brief 把 body 复制成 `std::string`。二进制安全。 */
  std::string body_str() const;

  // -------------------------------------------------------------------
  // JSON
  // -------------------------------------------------------------------

  /** @brief `Content-Type` 是否为 JSON（`application/json` 或 `+json` 后缀）。 */
  bool is_json() const;

  /**
   * @brief 把 body 解析成 JSON。
   *
   * 不抛异常。失败时 `status` 给出原因（若非 nullptr）。
   */
  bool json(uvcpp_json& out, json_status* status = nullptr) const;

  /** @brief 便捷版：失败时返回 `null`。 */
  uvcpp_json json() const;

  // -------------------------------------------------------------------
  // 逃生口
  // -------------------------------------------------------------------

  /**
   * @brief 拿到本对象持有的那个 `uvcpp_http_request`。
   *
   * 它是**本对象自己的副本**，生命周期与本对象一致，可以安全保存指针
   * （只要不超过本对象的生命周期）。协议层需要的东西（比如完整的头列表、
   * 原始 url）都能从这里取。
   */
  uvcpp_http_request& raw() { return src_; }
  const uvcpp_http_request& raw() const { return src_; }

 private:
  /// 从 `Accept-Encoding` 求某个编码的 q 值；未出现返回 -1。
  double encoding_qvalue(const std::string& encoding) const;

  uvcpp_http_request src_;   ///< 源请求的副本（body 已被搬走）
  uvcpp_buf          body_;  ///< 从 src_ 搬过来的 body，真正持有
  std::string        raw_url_;
  std::string        raw_path_;
  std::string        path_;
  std::string        query_string_;
  http_method        method_;
  uvcpp_http_version version_;

  // 构造时解析一次并缓存的头。它们的值在这一层被反复使用
  // （content_type_ 每次 is_json()/is_form() 都读），而 http_headers 是
  // 线性表，每次重扫都是 O(头数) 次字符串比较。
  std::string content_type_;
  std::string accept_encoding_;
  std::string host_;
  size_t      content_length_;

  std::string peer_ip_;
  unsigned int peer_port_;

  // 懒解析缓存：只在真正被问到的时候才算。静态文件请求从不看查询串，
  // 为它们预先解析是白付一次分配。
  mutable bool query_parsed_;
  mutable bool cookies_parsed_;
  mutable bool form_parsed_;
  mutable std::vector<std::pair<std::string, std::string> > query_params_;
  mutable std::vector<std::pair<std::string, std::string> > cookies_;
  mutable std::vector<std::pair<std::string, std::string> > form_params_;

  std::vector<std::pair<std::string, std::string> > params_;  ///< 路径参数
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_REQUEST_H
