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
 * `take_from()` 一律**搬**，不拷：body 走 `uvcpp_buf::move_buf`，`url` 与
 * `headers` 走移动赋值。所以源请求里那几样大的（body、URL 字符串、整条头向量）
 * 一个字节都不会被复制 —— 这也是**唯一**一条能不复制它们的路。
 *
 * 头向量为什么值得单说：它是 `std::vector<http_header>`，而 `http_header` 是
 * 两个 `std::string`，拷一份的价钱是 **2×头数** 次分配加同样多次 memcpy。搬一次
 * 只是把内部指针换手，于是整条链路上（解析器 → HTTP 层视图 → 这里）头只被**构造**
 * 一次，后面每一跳都是搬家。
 *
 * **代价**：`take_from()` 之后源 `uvcpp_http_request` 的 `body`、`url`、`headers`
 * 全是空的，调用方在交出请求之后不得再读这三样。**三个标量刻意留原值**
 * （`method` / `version` / `stream_id`）—— HTTP 层的 h2 路径正是靠 `take_from()`
 * 之后读 `stream_id` 回填流号的。
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
#include <webapp/uvcpp_web_json_reflect.h>
#include <webapp/uvcpp_web_upload.h>
#include <webapp/uvcpp_web_util.h>

namespace uvcpp {

class uvcpp_web_stream;

/**
 * @brief **缓冲式** multipart 里的一个文件部件（内容在内存里，不落盘）。
 *
 * 与 `uvcpp_web_upload_file` 的分工
 * --------------------------------
 * | | `upload_route()`（流式，落盘） | 普通路由（缓冲，在内存） |
 * |---|---|---|
 * | body 上限 | 框架自己的 `max_upload_size`，默认**不限** | 受 `max_body_size` 约束（默认 16 MiB） |
 * | 内容在哪 | 临时文件，`path()` 给出 | 内存，`data` 给出 |
 * | 适合 | 大文件、需要背压/进度 | 小表单里夹带的附件、头像 |
 *
 * 后者是"顺手能收下"的那一类，**不是**大文件上传的替代品：一条普通路由的
 * body 已经被 `max_body_size` 整包收在内存里了，所以再解出这些部件只是
 * 在那份内存上切分，额外的一份拷贝就是 `data` 本身。
 */
struct UVCPP_API uvcpp_web_form_file {
  /** @brief 字段名（`Content-Disposition: form-data; name="..."`）。 */
  std::string name;

  /**
   * @brief **清洗过**的文件名元数据。
   *
   * 与 `uvcpp_web_upload_file::original_filename()` 同源同语义
   * （`web_sanitize_filename()` 的产物）：取了叶子、去掉了控制字符与
   * Windows 的保留设备名。
   *
   * @warning **仍然只能当元数据**（日志、显示、写进数据库）。
   *          **永远不要**拿它拼路径 —— 要落盘请走 `upload_route()`，
   *          那里由框架生成落盘名。
   */
  std::string filename;

  /** @brief 该部件的 `Content-Type` 原始值（可空）。 */
  std::string content_type;

  /** @brief 部件内容。**二进制安全**（可以有 NUL）。 */
  std::string data;

  uvcpp_web_form_file() {}
  size_t size() const { return data.size(); }
};

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
  /** @brief 同上，但头名收 `const char*`（省掉一次临时 `std::string`）。 */
  bool has_header(const char* name) const;
  /** @copydoc has_header(const char*) const */
  std::string header(const char* name,
                     const std::string& def = std::string()) const;

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

  /**
   * @brief 全部**文件**部件（`multipart/form-data`）。
   *
   * 与 `form_params()` 用**同一个**解析器、同一次解析：`multipart/form-data`
   * 的文本字段进 `form_params_`（所以 `form("x")` 对 multipart 也管用），
   * 带 `filename=` 的部件进这里。两者是同一份报文的两半，分开取只是因为
   * 它们的载荷形态不同。
   *
   * 非 multipart 的请求恒为空；报文畸形时也是空（用 `multipart_ok()`
   * 区分这两种"空"）。
   */
  const std::vector<uvcpp_web_form_file>& files() const;

  /** @brief 按字段名取一个文件部件。不存在返回 nullptr。 */
  const uvcpp_web_form_file* file(const std::string& name) const;

  /**
   * @brief multipart 报文是否是**完好**解析出来的。
   *
   * 非 multipart 请求恒为 true（"不适用"不是错误）。只有 `is_multipart()`
   * 为真且解析失败（缺 boundary、部件头畸形、字段超长、报文在终边界前
   * 被截断）时才为 false。
   *
   * 为什么要暴露它：解析失败时 `form_params()` / `files()` 都是空的，而
   * "空表单"和"报文坏了"对业务是两件事 —— 前者可能只是用户什么都没填。
   * 少了这个接口，handler 只能靠"全空"去猜。
   */
  bool multipart_ok() const;

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
  // 流式 body
  // -------------------------------------------------------------------

  /**
   * @brief 流式 body 通道 —— **非流式请求返回 `nullptr`**。
   *
   * 两个问题是互斥的，不会同时为真：
   *
   * | 返回值          | body 在哪                        |
   * |-----------------|----------------------------------|
   * | 非 nullptr      | 正按块送进 `uvcpp_web_stream`    |
   * | nullptr         | 已经完整躺在 `body_*()` 里       |
   *
   * 所以 `body_size()` 在流式请求上**恒为 0**（一个字节都还没攒），要用
   * `stream()->received()` 看进度。
   *
   * @warning 返回的指针生存期等于本请求（也就是上下文），**不要存到请求之外**。
   */
  uvcpp_web_stream* stream() { return stream_; }
  const uvcpp_web_stream* stream() const { return stream_; }

  /** @brief 这个请求是不是流式收体（等价于 `stream() != nullptr`）。 */
  bool body_streaming() const { return stream_ != nullptr; }

  // -------------------------------------------------------------------
  // 上传
  // -------------------------------------------------------------------

  /**
   * @brief 这次上传解析出来的文件与字段 —— **只有 `upload_route()` 上的请求
   *        才可能非空**，其余一律 `nullptr`。
   *
   * 什么时候读它：在 `stream()->on_end()` 里。**在那之前恒为 `nullptr`**，
   * 这不是"还没解析完"而是这个指针根本没有被挂上 —— 解析结果是在 body 收完
   * 之后才存在的，而 `on_end` 是"收完了"的唯一信号。
   *
   * ```cpp
   * app.upload_route(http_method::HTTP_POST, "/upload/:room",
   *                  [](uvcpp_web_request& req, uvcpp_web_response& resp,
   *                     uvcpp_web_next) {
   *   req.stream()->on_end([&req, &resp]() {
   *     const uvcpp_web_upload_result* up = req.upload();
   *     if (up == nullptr) { resp.bad_request().end(); return; }
   *     const uvcpp_web_upload_file* f = up->file("avatar");
   *     if (f == nullptr) { resp.bad_request().text("缺 avatar").end(); return; }
   *     resp.json_str("{\"size\":" + std::to_string(f->size()) + "}");
   *   });
   * });
   * ```
   *
   * `up == nullptr` 的两种场合：① 报文不是 multipart / boundary 缺失（框架
   * 在 body 之前就回 415 或 400，用户的 `on_end` 根本不会被调用 —— 所以这条
   * 分支只需要处理 ②）；② 落盘失败（磁盘满、fsync 出错），那时本次创建的
   * 临时文件已经**全部删掉**了，`up` 为空就是"什么都没有留下"。
   *
   * @warning 返回的指针生存期等于**上下文**（结果由上传会话持有，会话挂在
   *          流对象的框架钩子上）。`on_end` 返回之后这次请求就收尾了，所以
   *          **不要把它存到 `on_end` 之外**。文件本身是另一回事：`on_end`
   *          返回后它们归 handler，框架不再碰（该 `rename` 就 `rename`）。
   */
  const uvcpp_web_upload_result* upload() const { return upload_; }

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

  /**
   * @brief 解析 body **并直接填进一个结构体**（`UVCPP_JSON_FIELDS` 标注过的）。
   *
   * 一步做完"解析 + 按字段表取值"：
   *
   *     struct user { std::string name; int age; UVCPP_JSON_FIELDS(user, name, age) };
   *
   *     user u;
   *     json_status st;
   *     const char* field = nullptr;
   *     if (!req.json(u, &st, &field)) {
   *       // st 说清是哪一类失败：EMPTY/SYNTAX 是报文的问题，
   *       // MISMATCH/MISSING/UNKNOWN 是字段的问题（field 给出字段名）
   *       resp.bad_request().text(json_status_name(st)).end();
   *       return;
   *     }
   *
   * 语义见 `webapp/uvcpp_web_json_reflect.h`：缺字段**不动**结构体里的原值、
   * 类型不符报错而不做隐式转换、多出来的成员默认忽略。
   *
   * DOM 挂在**本请求对象上**（`json_dom_`），不是这个函数的局部量：所有值都**拷进**
   * `out` 了，但 `where` 在多成员那条路上指进 DOM，生存期是**这个请求还在为止**。
   *
   * @warning 返回 false 时 `out` 可能已经被改了**一部分**（前几个字段），
   *          与 `uvcpp_from_json` 的规矩一样：非成功就丢弃 `out`。
   */
  template <typename T>
  typename std::enable_if<uvcpp::json_detail::uvcpp_has_json_fields<typename std::decay<T>::type>::value,
                          bool>::type
  json(T& out, json_status* status = nullptr, const char** where = nullptr,
       const uvcpp_from_json_options& opts = uvcpp_from_json_options()) const {
    const json_status parse_st = uvcpp_json_parse(body_data(), body_size(), json_dom_);
    if (parse_st != json_status::OK) {
      if (status != nullptr) *status = parse_st;
      return false;
    }
    const json_status read_st = uvcpp_from_json(json_dom_, out, opts, where);
    if (status != nullptr) *status = read_st;
    return read_st == json_status::OK;
  }

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
  friend class uvcpp_web_context;
  /// 上传路由的派发路径要把会话的结果挂到请求上（`set_upload()`）。
  friend class uvcpp_web_app;

  /// 从 `Accept-Encoding` 求某个编码的 q 值；未出现返回 -1。
  double encoding_qvalue(const std::string& encoding) const;

  /** @brief 挂上流式通道（由 `uvcpp_web_context` 在认领时调）。 */
  void set_stream(uvcpp_web_stream* s) { stream_ = s; }

  /**
   * @brief 挂上上传结果（由 `uvcpp_web_app` 在派发上传路由时调一次）。
   *
   * 传进来的是 `nullptr` 表示"这次上传什么都没留下"，与不调它的效果一样 ——
   * 两条路都落到 `upload() == nullptr`，用户只需要判这一种情况。
   */
  void set_upload(const uvcpp_web_upload_result* u) { upload_ = u; }

  /**
   * @brief 流式通道；普通请求恒为 nullptr。
   *
   * 裸指针是刻意的：对象由上下文按值持有（`unique_ptr`），本对象只是它的
   * 一个视图 —— 两者同生共死，所以没有悬垂问题，也不该让请求去分担所有权。
   */
  uvcpp_web_stream* stream_;

  /**
   * @brief 上传结果的视图；由上传会话持有，会话挂在流对象的框架钩子上。
   *
   * 同样的裸指针理由：会话的生存期被流对象兜住（钩子按值捕获
   * `shared_ptr<uvcpp_web_upload>`），而流对象的生存期等于上下文。
   */
  const uvcpp_web_upload_result* upload_;

  uvcpp_http_request src_;   ///< 源请求里**搬**过来的那几个字段（见 take_from）
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

  // multipart 的**文件**部件。它们与 form_params_ 由同一次解析（同一个
  // `form_parsed_` 标志）一起填好 —— 分两个标志会让"只问 form()"的调用方
  // 漏掉文件、再问 file() 时又整份重解一遍。
  mutable std::vector<uvcpp_web_form_file> form_files_;
  mutable bool multipart_ok_;

  // `json()` 那次解析出来的 DOM。**必须是成员、不能是那个函数里的局部量**：
  // `where` 在"多出来的成员"那条路上指到的就是 DOM 里的键（契约见
  // `webapp/uvcpp_web_json_reflect.h` 的 `uvcpp_from_json`），局部量一返回就把
  // 那个指针悬空了 —— libstdc++ 上释放后的那几个字节常常还留着原样（看着是对的），
  // libc++（macOS 那条 CI 腿）读出来就是空串。有了这个成员，`where` 对使用者的
  // 生存期就是"这个请求还在为止"。
  //
  // 每次 `json()` 都整份重新解析（`uvcpp_json_parse` 内部是 `out = parsed`，
  // 不复用旧 DOM）：body 可能被管线里的中间件改过，留住旧 DOM 会跟
  // `body_data()` 说的不是一回事。
  //
  // 代价如实说：本类因此**大了一个 `nlohmann::json`**（16 字节），堆分配只在
  // 真正调 `json()` 时发生。
  mutable uvcpp_json json_dom_;

  /// 缓冲式 multipart 的解析（`form_params()` 在 `is_multipart()` 时调它）。
  /// 单独一个函数是因为它要 include `uvcpp_web_multipart.h`，而那个头不该
  /// 被拖进每个包含本头的编译单元。
  void parse_multipart_form() const;

  std::vector<std::pair<std::string, std::string> > params_;  ///< 路径参数
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_REQUEST_H
