/**
 * @file src/webapp/uvcpp_web_response.h
 * @brief 响应封装：给业务处理器用的、可链式调用的响应构造器。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它解决什么问题
 * --------------
 * `uvcpp_http_response` 是**协议层**的表示，直接用有三个坑：
 *
 * 1. **状态码要手写**。只有 `ok` / `not_found` / `server_error` / `make`
 *    四个工厂，其余几十个常用码都得自己填数字和 reason phrase。
 * 2. **`set_header` 是覆盖语义**，而 `Set-Cookie` 是**允许多条**的
 *    （RFC 6265 §3）。用 `set_header` 发第二个 cookie 会把第一个顶掉 ——
 *    这是个安静的、只在多 cookie 场景才暴露的 bug。
 * 3. **空 body 的 200 不会输出 `Content-Length`**。`to_string()` 里的条件是
 *    `!has_cl && body.size() > 0`，于是 `body.size() == 0` 时一个长度头都不发。
 *    在 HTTP/1.1 下这等于告诉对端「长度未知，读到我关连接为止」，keep-alive
 *    会因此退化甚至挂住。**必须显式发 `Content-Length: 0`。**
 *
 * 所以这一层的核心职责是 **`sync_meta()`**：在任何发送路径之前，把
 * `Content-Length`、以及「这个状态码到底允不允许有 body」这两件事算准。
 *
 * 关于 HEAD
 * ---------
 * HEAD 的语义是「和 GET 一样的头，但没有 body」。所以 `set_head_only(true)`
 * 会在**保留** `Content-Length`（反映 GET 应有的长度）之后丢掉 body 字节。
 * 顺序很关键：先算长度，再丢 body。
 *
 * 线程约定
 * --------
 * 与 `uvcpp_web_request` 相同，只在 loop 线程上访问。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_RESPONSE_H
#define SRC_WEBAPP_UVCPP_WEB_RESPONSE_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_export.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_response.h>
#include <webapp/uvcpp_web_handler.h>
#include <webapp/uvcpp_web_json.h>

namespace uvcpp {

// =========================================================================
// 发送结果
// =========================================================================

/**
 * @brief 一次响应发送的最终结果，交给 `on_sent()` 回调。
 *
 * 存在的意义是给访问日志中间件用：只有在这里才能知道**真正**写出去的状态码
 * 和字节数（handler 里看到的还只是「打算发什么」）。
 */
struct UVCPP_API uvcpp_web_sent_info {
  int      status_code;    ///< 最终状态码
  size_t   body_bytes;     ///< 实际写入连接的 body 字节数（HEAD 时为 0）
  uint64_t connection_id;  ///< 所属连接 id（0 表示未知）
  bool     ok;             ///< 写入是否成功（false = 连接已断/写失败）

  /// 显式构造函数，**不用 NSDMI** —— 那会让本结构体失去聚合初始化资格。
  uvcpp_web_sent_info();
};

/** @brief 响应发出后的回调。 */
typedef std::function<void(const uvcpp_web_sent_info&)> uvcpp_web_sent_cb;

// =========================================================================
// 响应对象
// =========================================================================

/**
 * @brief 响应构造器。
 *
 * 所有 mutator 都返回 `*this`，可以链式写：
 * @code
 *   resp.status(201).set_header("Location", "/users/7").json(u);
 * @endcode
 */
class UVCPP_API uvcpp_web_response {
 public:
  uvcpp_web_response();
  ~uvcpp_web_response();

  // 不可拷贝：它持有 on_sent 回调与「已结束」状态，复制出来的副本语义
  // 必然是错的（谁的回调？谁算已发送？）。框架里它总是按引用传递。
  uvcpp_web_response(const uvcpp_web_response&) = delete;
  uvcpp_web_response& operator=(const uvcpp_web_response&) = delete;

  // -------------------------------------------------------------------
  // 状态行
  // -------------------------------------------------------------------

  /** @brief 设置状态码（reason phrase 自动补全）。 */
  uvcpp_web_response& status(int code);
  /** @brief 设置状态码（枚举版）。 */
  uvcpp_web_response& status(http_status code);
  /** @brief 覆盖 reason phrase（一般不需要）。 */
  uvcpp_web_response& status_message(const std::string& msg);

  int status_code() const;

  // -------------------------------------------------------------------
  // 报头
  // -------------------------------------------------------------------

  /** @brief 设置头（**覆盖**同名头）。 */
  uvcpp_web_response& set_header(const std::string& name,
                                 const std::string& value);

  /**
   * @brief 追加一个头（**不覆盖**同名头）。
   *
   * 这是 `Set-Cookie` 唯一正确的发法。普通单值头请用 `set_header()`，
   * 否则对端会收到两条同名头，多数客户端只认第一条或直接判为畸形。
   */
  uvcpp_web_response& add_header(const std::string& name,
                                 const std::string& value);

  /** @brief 发一条 `Set-Cookie`（内部走 `add_header`，可重复调用）。 */
  uvcpp_web_response& set_cookie(const std::string& name,
                                 const std::string& value,
                                 const std::string& path = std::string("/"),
                                 long max_age = -1,
                                 bool http_only = true,
                                 bool secure = false,
                                 const std::string& same_site =
                                     std::string("Lax"));

  /** @brief 让一个 cookie 立即过期（值为空 + `Max-Age=0`）。 */
  uvcpp_web_response& clear_cookie(const std::string& name,
                                   const std::string& path = std::string("/"));

  std::string get_header(const std::string& name,
                         const std::string& def = std::string()) const;
  bool has_header(const std::string& name) const;
  uvcpp_web_response& remove_header(const std::string& name);

  uvcpp_web_response& set_content_type(const std::string& ct);
  std::string content_type() const;

  // -------------------------------------------------------------------
  // 报文体
  // -------------------------------------------------------------------

  /** @brief 设置 body（二进制安全，会拷贝一份）。 */
  uvcpp_web_response& body(const char* data, size_t len,
                           const std::string& ct = std::string());

  /** @brief 设置 body（二进制安全）。 */
  uvcpp_web_response& body(const std::string& s,
                           const std::string& ct = std::string());

  /**
   * @brief 接管一块已有 buf 的所有权，**不拷贝字节**。
   *
   * 用于把磁盘读来的大文件/大上传结果直接交给响应，避免再复制一遍。
   * 调用后 `src` 为空。
   */
  uvcpp_web_response& body_move(uvcpp_buf& src,
                                const std::string& ct = std::string());

  /// 设置 `text/plain` body。
  uvcpp_web_response& text(const std::string& s);
  /// 设置 `text/html; charset=utf-8` body。
  uvcpp_web_response& html(const std::string& s);
  /// 序列化并设置 `application/json` body。
  uvcpp_web_response& json(const uvcpp_json& j);
  /** @brief 设置**已经序列化好**的 JSON body（跳过再次 dump）。 */
  uvcpp_web_response& json_str(const std::string& s);
  /// 设置 `application/octet-stream`（或指定类型）的二进制 body。
  uvcpp_web_response& binary(const char* data, size_t len,
                             const std::string& ct =
                                 std::string("application/octet-stream"));

  const char* body_data() const;
  size_t body_size() const;
  bool body_empty() const;
  /** @brief 清空 body（`Content-Length` 会在 `sync_meta()` 时重算）。 */
  uvcpp_web_response& clear_body();

  // -------------------------------------------------------------------
  // 常用状态码 helper
  // -------------------------------------------------------------------
  //
  // 语义统一为：**设置状态码；若当前 body 为空，再补一段默认的纯文本说明**。
  // 所以 `.not_found()` 单独调用会给出可直接读的 404 页面；而
  // `.json(err).not_found()` 会保留你自己的 JSON body。
  // 顺序无所谓，但 body 只在你没设过的时候才被填。

  uvcpp_web_response& ok();
  uvcpp_web_response& created();
  uvcpp_web_response& accepted();
  uvcpp_web_response& no_content();          ///< 204，无 body
  uvcpp_web_response& not_modified();        ///< 304，无 body
  uvcpp_web_response& redirect(const std::string& url, int code = 302);

  uvcpp_web_response& bad_request();
  uvcpp_web_response& unauthorized(const std::string& challenge =
                                       std::string("Bearer"));
  uvcpp_web_response& forbidden();
  uvcpp_web_response& not_found();
  /** @brief 405，并带上 `Allow` 头。 */
  uvcpp_web_response& method_not_allowed(const std::string& allow);
  uvcpp_web_response& conflict();
  uvcpp_web_response& payload_too_large();
  uvcpp_web_response& unsupported_media_type();
  /** @brief 416，并带上 `Content-Range: bytes *&#47;total`（total 为 0 则不带）。 */
  uvcpp_web_response& range_not_satisfiable(unsigned long long total = 0);
  uvcpp_web_response& unprocessable();
  uvcpp_web_response& too_many_requests();

  uvcpp_web_response& server_error();
  uvcpp_web_response& service_unavailable();

  // -------------------------------------------------------------------
  // HEAD / 元信息同步
  // -------------------------------------------------------------------

  /**
   * @brief 标记这是 HEAD 请求：保留 `Content-Length`，丢弃 body。
   *
   * **顺序无关** —— 长度是在 `sync_meta()`（发送前）按当时的 body 算的，
   * 再丢 body。所以先设 body 还是先设这个标志，结果一样。
   */
  void set_head_only(bool v);
  bool head_only() const;

  /**
   * @brief 补齐 `Content-Length`、按状态码丢弃不该有的 body。
   *
   * 幂等。框架在发送前一定会调一次；手动调也可以（`raw()` 非 const 版会
   * 顺带调）。规则：
   *  - 1xx / 204 / 304：清空 body，**不自动加** `Content-Length`
   *    （用户显式设了的话保留）；
   *  - 其余：body 为空时补 `Content-Length: 0`（这正是协议层漏掉的那条）；
   *  - 已存在 `Content-Length` 或 `Transfer-Encoding: chunked` 时不动它 ——
   *    使用者可能在做分块/流式，不能覆盖他的决定。
   */
  void sync_meta();

  // -------------------------------------------------------------------
  // 生命周期
  // -------------------------------------------------------------------

  /** @brief 标记响应已构造完成，可以发送。 */
  void end();
  bool ended() const;

  /** @brief 是否要延迟发送（handler 稍后自己 `send()`）。 */
  bool deferred() const;
  void set_deferred(bool v);

  /**
   * @brief 注册「响应真正发出后」的回调。
   *
   * **可叠加**：多次调用会按注册顺序**全部**触发，后注册的不会顶掉先注册的。
   * 覆盖语义在这里是个陷阱 —— 访问日志中间件和业务 handler 都会想挂这个
   * 回调，谁后挂谁生效，另一个就静默失效了，而失效的偏偏是日志（最不该
   * 丢的那个）。
   */
  void on_sent(const uvcpp_web_sent_cb& cb);
  /** @brief 已注册的回调个数。 */
  size_t sent_callback_count() const;
  /**
   * @brief 触发全部 `on_sent` 回调。由框架调用，异常会被逐条吞掉。
   *
   * **只生效一次**：触发后回调列表被清空，重复调用是空操作。响应的"已发出"
   * 是个一次性事件，重复投递会让访问日志记两遍。
   */
  void notify_sent(const uvcpp_web_sent_info& info);

  /**
   * @brief 安装错误处理器（`web_middleware_error_handler()` 用）。
   *
   * 链上抛出的异常由框架捕获后调用它。**只保留最后一个** —— 错误处理器
   * 天然是全局策略，装两个只会让"到底谁处理了"变成需要读代码才能回答的
   * 问题。
   */
  void set_error_handler(const uvcpp_web_error_handler& h);
  const uvcpp_web_error_handler& error_handler() const;
  bool has_error_handler() const;

  // -------------------------------------------------------------------
  // 逃生口
  // -------------------------------------------------------------------

  /**
   * @brief 拿到底层 `uvcpp_http_response`（会先 `sync_meta()`）。
   *
   * 框架把它交给 `uvcpp_http_server::send_response()`。直接改它也行，
   * 但改完记得再调一次 `sync_meta()`。
   */
  uvcpp_http_response& raw();
  const uvcpp_http_response& raw() const;

 private:
  /// 这个状态码是否不允许有 body（1xx / 204 / 304）。
  bool status_forbids_body() const;

  uvcpp_http_response resp_;

  bool head_only_;
  bool ended_;
  bool deferred_;
  std::vector<uvcpp_web_sent_cb> sent_cbs_;
  uvcpp_web_error_handler error_cb_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_RESPONSE_H
