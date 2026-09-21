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
 * HEAD 的语义是「和 GET 一样的头，但没有 body」。`set_head_only(true)` 只是
 * 把这个事实**标出来**，body 一路留到 HTTP 层：那边的 `to_string()` 以
 * `include_body = !ctx.is_head` 调用，那才是不发字节的地方 —— 而
 * `apply_compression()` 要拿真 body 才能算出 GET 会发的那个
 * `Content-Length` 与 `Content-Encoding`（RFC 9110 §9.3.2）。
 *
 * 所以本层**不**替 HEAD 丢 body —— 曾经丢过，代价是 HEAD 的长度在压缩之前
 * 就被钉死，于是 HEAD 报未压缩长度、GET 报压缩后的长度，两边必然不一致。
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
#include <memory>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_export.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_response.h>
#include <webapp/uvcpp_web_handler.h>
#include <webapp/uvcpp_web_json.h>

namespace uvcpp {

// 前向声明：本头只把这两个当指针用（`stream_loop()` 的返回类型、
// `stream_attach_file()` 的形参、以及私有的 `shared_ptr` 成员），所以不需要
// 拉 `<uvcpp/uvcpp_loop.h>` 与 `<webapp/uvcpp_web_file.h>` —— 与
// `uvcpp_web_file.h:94-95` 同一手法。`shared_ptr<T>` 的析构不要求 T 完整
// （删除器存在控制块里），所以成员声明也不需要完整类型。
class uvcpp_loop;
class uvcpp_web_file_transfer;

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

  /**
   * @brief 这次响应是不是**流式（chunked）**发出去的。
   *
   * 访问日志靠它区分两种"body_bytes"：整包响应的长度是**发送之前**就知道的，
   * 而流式的长度是**发完之后**才成立的 —— 而且流中途失败时那两段会分叉
   * （`ok` 为假，但已经上线的字节仍然照实记在 `body_bytes` 里）。
   *
   * **加法式字段**：本结构体有显式构造函数（不是聚合），既有构造点一律走
   * 默认值 `false`，所以加它不影响任何调用方。
   */
  bool     streamed;

  /// 显式构造函数，**不用 NSDMI** —— 那会让本结构体失去聚合初始化资格。
  uvcpp_web_sent_info();
};

/** @brief 响应发出后的回调。 */
typedef std::function<void(const uvcpp_web_sent_info&)> uvcpp_web_sent_cb;

// =========================================================================
// 流式响应的出口
// =========================================================================

/**
 * @brief 流式响应的字节出口。
 *
 * 存在的意义是把 `uvcpp_web_response` 与「连接、写队列、压缩」隔开：响应只
 * 知道有三个动作 —— **发头、写一块、收尾**；具体怎么落到 socket 上由 http
 * 层实现（`uvcpp_web_app` 里那个适配器转发给 `uvcpp_http_server` 的三个
 * 流式入口）。
 *
 * 生命周期：由 `uvcpp_web_response::set_stream_sink()` **接管所有权**，随响应
 * 一起析构。框架在 `send_response()` 里按连接 id 建一个装上，使用者不碰。
 *
 * 线程约定：全部只在 loop 线程上调用。
 */
class UVCPP_API uvcpp_web_stream_sink {
 public:
  virtual ~uvcpp_web_stream_sink() {}

  /**
   * @brief 把头部序列化并入队（不写 body、不写终止块）。
   *
   * 实现应当走 `resp.to_string(/*include_body=*\/false)` 而不是自己拼 ——
   * HEAD 走的是同一个出口，两条路必须逐字节一致。
   *
   * @return 0 = 成功；非 0 = 这条路已经不可用（连接没了）。
   */
  virtual int stream_begin(uvcpp_http_response& head) = 0;

  /**
   * @brief 写一块**已经组好帧**的字节（调用方负责 hex 长度与 CRLF）。
   *
   * @param done 这一块**真正写完**（或确认失败）之后调用一次。**实现必须
   *             保证它一定会被调用** —— 包括连接已经不在了的场合。否则调用
   *             方（一次流式响应）会永久挂起：它的收尾回调挂在这个 done 上。
   *
   * @warning `done` **不得在本函数返回之前同步调用**。调用方在
   *          `stream_write()` 返回后仍会读 `this`（用于判断返回值），同步
   *          回调会把"这一块还没交出去"和"整条流已经收尾"压进同一个栈。
   *          本仓的 http 层实现满足这条（完成回调来自 libuv 写完成）。
   *
   * @return 0 = 已受理；非 0 = 未受理，此时 `done` **不必**再调用（调用方
   *         会自己按失败结算）。
   */
  virtual int stream_write(const std::string& bytes,
                           const std::function<void(int)>& done) = 0;

  /// 收尾。`close_after` 为真表示这一块写完之后关连接（chunked 的终止块
  /// 由调用方组好、经 `stream_write` 发出，本函数不管组帧）。
  virtual void stream_end(bool close_after) = 0;

  // --- 分片下发（`send_file*`）需要的三件附加能力 ---
  //
  // 三个都是**默认为空操作**的，所以既有的 sink 实现（第 3 步的测试桩、
  // 以及任何外部实现）不需要改一行。分片下发在缺少它们时会明确失败，而不是
  // 静默地什么都不做（见实现里 `start_file_transfer()` 的判据）。

  /**
   * @brief 报告一个正在进行的文件传输，供实现方在连接断开时取消它。
   *
   * 调用时机是**传输提交之前**。实现应当把 `t` 记在一个按连接索引的表里，
   * 在连接断开时对它调 `cancel()` —— 没有这一条，一个中途断开的下载会把
   * fd 与滑动窗口缓冲挂到进程退出为止。
   *
   * 默认空实现 = "本 sink 不关心传输的生命周期"。
   */
  virtual void stream_attach_file(uvcpp_web_file_transfer* t) { (void)t; }

  /// 传输结束（无论成功、出错还是被取消）之后调用，与 `stream_attach_file`
  /// 配对。实现应当把 `t` 从自己的表里摘掉。
  virtual void stream_detach_file(uvcpp_web_file_transfer* t) { (void)t; }

  /**
   * @brief 分片下发该用哪个 loop 提交 `uv_fs_*`。
   *
   * 分片读是**异步 fs**，需要 loop。而响应自己拿不到 loop —— 它是纯数据
   * 对象，loop 归框架所有。所以由 sink（框架那一侧）提供。
   *
   * 返回 `nullptr` = 本 sink 不支持分片下发，`send_file*` 会以错误收场
   * （**不是**静默地发一个空 body）。
   */
  virtual uvcpp_loop* stream_loop() const { return nullptr; }
};

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
   * @brief 同上，但头名收 `const char*`。
   *
   * `"transfer-encoding"`、`"access-control-allow-origin"` 这类 > 15 字符的
   * 字面量走 `const std::string&` 会在调用点各构造一个堆串（MSVC SSO 上限 15）。
   * 理由与覆盖面见 `src/web/uvcpp_http_common.h`。
   */
  uvcpp_web_response& set_header(const char* name, const std::string& value);

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

  /** @copydoc set_header(const char*, const std::string&) */
  std::string get_header(const char* name,
                         const std::string& def = std::string()) const;
  /** @copydoc set_header(const char*, const std::string&) */
  bool has_header(const char* name) const;
  uvcpp_web_response& remove_header(const std::string& name);
  /** @copydoc set_header(const char*, const std::string&) */
  uvcpp_web_response& remove_header(const char* name);

  uvcpp_web_response& set_content_type(const std::string& ct);
  std::string content_type() const;

  /*
   * 名字与值**都是**字面量时的那一组：省掉临时 `std::string`，值那一侧还多省
   * 一次拷贝（就地构造再移动，而不是"建临时 `http_header` 再搬"）。
   * 字面量调用点由重载决议自动选中，**调用点零改动**。
   * 详见 `uvcpp_http_common.h` 里同名重载的注释。
   */
  uvcpp_web_response& set_header(const char* name, const char* value);
  uvcpp_web_response& add_header(const char* name, const char* value);
  uvcpp_web_response& set_content_type(const char* ct);

  // -------------------------------------------------------------------
  // 报文体
  // -------------------------------------------------------------------

  /** @brief 设置 body（二进制安全，会拷贝一份）。 */
  uvcpp_web_response& body(const char* data, size_t len,
                           const std::string& ct = std::string());

  /** @brief 设置 body（二进制安全）。 */
  uvcpp_web_response& body(const std::string& s,
                           const std::string& ct = std::string());

  /*
   * `ct` 走 C 串的两个版本，**刻意不给默认值** —— 给了就和上面两个的默认实参
   * 撞成歧义（`body(s)` 会有两个同样可行的候选）。
   *
   * 为什么要这一对：`text()` / `html()` / `json()` / `json_str()` 都是往
   * `ct` 位置传**长字面量**（24 / 23 / 31 / 31 字符，全在 MSVC 的 15 字符 SSO
   * 之外），而上面两个版本的 `ct` 是 `const std::string&` ⇒ 每个 `resp.text("ok")`
   * 都要先建一个临时串，再被 `set_content_type` 拷进头表。**那是 benchmark
   * 自己在跑的两条路由**（`GET /text`、`GET /json`）。这一对把它们降到一次分配。
   */
  uvcpp_web_response& body(const char* data, size_t len, const char* ct);
  uvcpp_web_response& body(const std::string& s, const char* ct);

  /**
   * @brief 接管一块已有 buf 的所有权，**不拷贝字节**。
   *
   * 用于把磁盘读来的大文件/大上传结果直接交给响应，避免再复制一遍。
   * 调用后 `src` 为空。
   */
  uvcpp_web_response& body_move(uvcpp_buf& src,
                                const std::string& ct = std::string());

  /**
   * @brief **共享**一份已有的缓冲，不拷贝字节；源仍然持有它。
   *
   * 与上面两条的区别是"谁还留着它"：
   *   - `body()`      拷进来，两份互不相干；
   *   - `body_move()` 所有权**转移**，源变空；
   *   - `body_share()` 两边**共用**同一份字节，靠引用计数共同保活 —— 源（比如
   *     静态层的 LRU 条目）之后被淘汰、被析构，本响应手里的这份仍然有效。
   *
   * 所以"源还要继续复用这块内容"时只能用这一条（`body_move()` 会把源掏空，
   * 对缓存条目而言等于每次命中都要重读一次盘）。
   */
  uvcpp_web_response& body_share(const std::shared_ptr<const std::string>& src,
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
   * @brief 标记这是 HEAD 请求：发与 GET 相同的头，但不发 body。
   *
   * **本层不丢 body**（见文件头「关于 HEAD」）。这个标志的作用有两处：让
   * `uvcpp_web_app` 把 `body_bytes` 记成 0（HEAD 一个字节都没上线），以及让
   * HTTP 层在序列化时走"只发头"那一支（h1 的 `include_body=false`、h2 的
   * `omit_body=true`）—— body 正是靠它挡住、不上去的。
   *
   * 与 `sync_meta()` 的先后**顺序无关** —— 长度按发送前的 body 算。
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

  /**
   * @brief 本响应上的一枚**只做记录**的标志，框架里没有任何读取点。
   *
   * 别与 `uvcpp_http_response::deferred` 混为一谈 —— 真正决定"框架替不替你发"
   * 的是**后者**（`src/web/uvcpp_http_response.h:48-49` 那个公开成员）：HTTP 层
   * 在兜底 handler 返回后检查它，置真就不再自行发送，发送权交给框架。
   * 这一枚没有消费者，`set_deferred(true)` 不改变任何发送行为。
   */
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
  // 流式（chunked）响应
  // -------------------------------------------------------------------
  //
  // 典型用法（SSE）：
  // @code
  //   resp.begin_chunked("text/event-stream");
  //   resp.write_chunk("data: 1\n\n");
  //   resp.write_chunk("data: 2\n\n");
  //   resp.end();
  // @endcode
  //
  // **组帧归框架**：`write_chunk("data: 1\n\n")` 在线上的字节是
  // `b\r\ndata: 1\n\n\r\n`。让使用者自己写 hex 长度等于把最容易错的一步
  // 交出去 —— 少了 CRLF、长度按字符数而不是字节数算，都是只在某些负载下
  // 才暴露的错。
  //
  // **h2 上没有那层帧。** 同一句 `write_chunk("data: 1\n\n")` 在一条 h2 流上
  // 写出去的**恰好就是这 10 个字节**：DATA 帧由会话层组，本层给裸字节。组帧的
  // 判据是协议（`stream_id`），不是调用方 —— 对使用者而言 API 完全一样。
  //
  // 流开始之后 `body()` / `text()` / `json()` 这些**不再有意义**，不要再调。

  /**
   * @brief 切到 chunked 流式响应。**必须在任何 body 之前调用一次**。
   *
   * 做三件事：清掉已有 body（WARN）、去掉 `content-length`（CL 与
   * `transfer-encoding` 不得共存，RFC 7230 §3.3.2）、设
   * `transfer-encoding: chunked`。
   *
   * 之后 `end()` 的语义变成「结束这条流」（补终止块），而不是「标记响应
   * 构造完成」。
   *
   * @param content_type 非空则设置 `Content-Type`。
   */
  void begin_chunked(const std::string& content_type =
                         std::string("text/plain; charset=utf-8"));

  /**
   * @brief 追加一块 body。框架负责组帧。
   *
   * @return **false = 缓冲已到高水位**，调用方应当等 `on_drain()` 再继续。
   *         注意数据**仍然被收下了**，返回 false 不是"丢弃"。
   *
   * 空块（`len == 0` / `data == nullptr`）是**无操作**并返回 true：一个
   * 长度为零的帧在字节上与终止块**完全相同**，发出去等于提前结束这条流。
   *
   * HEAD 请求上是**空操作**（发头部、不发 body），但仍计入
   * `stream_bytes_written()` —— 那个数描述的是「GET 本该发多少」。
   *
   * 没调 `begin_chunked` 就调本函数：WARN + 返回 false，什么都不发。
   * 静默发出一个畸形报文比拒绝更坏。
   */
  bool write_chunk(const std::string& data);
  bool write_chunk(const char* data, size_t len);

  /**
   * @brief 缓冲降到低水位时回调一次。
   *
   * 语义是**边沿触发**：从"高于高水位"降到"不高于半水位"时才触发一次，
   * 触发后要等再次越过高水位才会再触发。这样回调里可以放心地一次灌一批。
   *
   * **可叠加**（与 `on_sent` 同族）：多次注册按顺序全部触发。
   */
  void on_drain(const std::function<void()>& cb);

  bool streaming() const;
  /// 已经写出去的 body 字节数（**不含**组帧的 hex 与 CRLF）。
  size_t stream_bytes_written() const;
  /**
   * @brief 还没落地的字节数（含已交给 sink 但尚未确认写完的那一块）。
   *
   * 这是背压的水位计 —— 判据是**字节数**而不是"几块在飞"。
   */
  size_t stream_pending_bytes() const;

  // --- 分片读下发（`send_file*`） -----------------------------------------
  //
  // 与 `write_chunk()` 的分工：那条路是"调用方自己产生字节"，这条是"从一个
  // 文件里异步分片读出来"。两者共用同一套待发缓冲与背压水位 —— 因为终点是
  // 同一个 socket，水位必须是**一个**数，两条路各算一份就失去意义了。
  //
  // 全程不把整个文件读进内存：一片一片地 `uv_fs_read`，每片交给 sink 之后就
  // 不再持有。峰值内存 ≈ 切片 + 高水位，**与文件大小无关**（细节与算式见
  // `uvcpp_web_file.h` 的文件头）。

  /**
   * @brief 分片读下发一个文件，长度**未知**（走 chunked）。
   *
   * 长度未知时只能 chunked —— 没有长度就发不出 `Content-Length`。读盘读到
   * EOF 就是这条流的天然终点。
   *
   * `done(status, bytes_sent)` 在**整个文件读完、或出错、或被取消之后**
   * 恰好调用一次：`status == 0` 表示完整发完。默认不做任何事。
   *
   * **必须有 sink 且 sink 提供了 loop**（`stream_loop()` 非空），否则这条
   * 调用以错误收场并在 `done` 里报出来 —— 不会静默发一个空 body。
   */
  void send_file(const std::string& path,
                 const std::function<void(int, uint64_t)>& done =
                     std::function<void(int, uint64_t)>());

  /**
   * @brief 已知长度时用它：`known_size >= 0` 走 `Content-Length`，
   *        传 `-1` 表示"不知道，用 chunked"。
   *
   * **为什么已知长度优先走 CL 而不是一律 chunked**：CL 更省（没有每块的
   * hex 与 CRLF），客户端能出进度条，而且**中途失败时"长度对不上"是客户端
   * 立刻看得见的信号** —— chunked 少了终止块也是错的，但那要等到对端读超时
   * 才发现。所以长度已知就不该退化成 chunked。
   *
   * 静态服务走的就是这一支：它本来就要 `stat` 来判 ETag/Range，结果直接传
   * 进来即可，本函数**自己不做 stat**（多做一次 stat 既是浪费，也会引入
   * "两次 stat 之间文件被换掉了"这种不一致）。
   *
   * `known_size == 0` 是合法的：表达成空区间（`first = 1, last = 0`），
   * **仍然会去 open**，所以 ENOENT 之类的错误照常以 404 报出来。
   */
  void send_file(const std::string& path, int64_t known_size,
                 const std::function<void(int, uint64_t)>& done =
                     std::function<void(int, uint64_t)>());

  /**
   * @brief 下发 `[first, last]`（**闭区间**）—— 供静态服务的 Range/206 复用。
   *
   * `first > last` 是合法的空区间（仍会 open，见上）。`Content-Length`
   * 按 `last - first + 1` 设。
   */
  void send_file_range(const std::string& path, uint64_t first, uint64_t last,
                       const std::function<void(int, uint64_t)>& done =
                           std::function<void(int, uint64_t)>());

  /**
   * @brief 高水位。默认 1 MiB（`default_max_stream_buffer_bytes()`）。
   *
   * 可以在 `begin_chunked()` **之前**设（那时 `pending` 还是 0）。
   */
  void set_max_stream_buffer_bytes(size_t n);
  size_t max_stream_buffer_bytes() const;
  static size_t default_max_stream_buffer_bytes();

  // -------------------------------------------------------------------
  // 框架内部接口（使用者不需要碰）
  // -------------------------------------------------------------------

  /// 安装字节出口，**接管所有权**。框架在发送路径上按连接建一个装上。
  void set_stream_sink(uvcpp_web_stream_sink* sink);
  /**
   * @brief 整条流结束（成功或失败）之后恰好调用一次。
   *
   * @param status 0 = 成功；非 0 = 失败的状态码。**在 `notify_sent` 之前**
   *               被调用，所以回调里能读到最终状态。
   */
  void set_stream_finished_cb(const std::function<void(int)>& cb);
  /// 把已经攒下的字节交给 sink，必要时发头、必要时收尾。
  void pump_stream();

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

  /**
   * @brief 标上这条响应属于哪条 h2 流（0 = HTTP/1.1）。**由框架在派发之前调**。
   *
   * 刻意不走 `raw()`：那个非 const 版会先 `sync_meta()`，而在这里跑一次是在
   * body 还空着的时候 —— 它会写死一个 `content-length: 0`，之后处理函数设的
   * body 就再也改不动它了（`sync_meta()` 判的是"已经有长度头了吗"）。
   */
  void set_stream_id(int32_t stream_id) { resp_.stream_id = stream_id; }
  int32_t stream_id() const { return resp_.stream_id; }

 private:
  /// 这个状态码是否不允许有 body（1xx / 204 / 304）。
  bool status_forbids_body() const;

  // --- 流式的内部动作 ---
  /// 把 `pending_buf_` 交给 sink（**调用之后不得再碰任何成员** —— 完成
  /// 回调可能是异步的，但 sink 的实现有权同步收尾整条流）。
  void flush_stream();
  /// 一块写完了。`n` 是那一块的字节数，`status` 是写的结果。
  void stream_write_done(size_t n, int status);
  /// 头部已发、流已 end、攒下的都写完、且还没收尾 —— 可以收了。
  bool stream_finish_ready() const;
  /// 满足条件就收尾。**调用之后不得再碰任何成员。**
  void maybe_finish_stream();

  /// 把流式状态复位成「刚开始」的样子。`begin_chunked()` 与 `arm_file_transfer()`
  /// 共用 —— 分成两份写必然漂移，而漂移的后果是某条路带着上一次下发的
  /// `file_started_` 起步，于是 `pump_stream()` 以为已经起步过，**一个字节都不读**。
  void reset_stream_state();

  /// 这条响应挂在一条 h2 流上（`stream_id != 0`）。
  ///
  /// **组帧方式的判据只能是它，不能问 sink。** `write_chunk()` 完全可能出现在
  /// 处理函数体内，而 sink 是处理函数返回之后才装上的 —— 那一刻已经晚了：
  /// 帧已经按 h1 的形状攒进了 `pending_buf_`。所以框架在**派发之前**就把流号
  /// 写进 `raw()`，本层只读它。
  bool on_h2_stream() const { return resp_.stream_id != 0; }

  // --- 分片下发的内部动作 ---
  /// 追加一块 body，**不组帧**（与 `write_chunk` 的唯一区别就是少了 hex 帧）。
  /// 语义、返回值、HEAD 上的行为全部与 `write_chunk` 一致。
  bool append_raw(const char* data, size_t len);
  /// `send_file*` 的共用前置检查：还没发过文件、还没写过任何 body、`end()`
  /// 也还没调。不满足就打 WARN 并返回 false。
  ///
  /// **刻意不要求 `streaming_`。** `send_file*` 的契约是"自足"——调用方既不必
  /// 先 `begin_chunked()`，也不必事后补一个 `end()`（流模式由
  /// `arm_file_transfer()` 内部建立）。要求了它，主用法会当场被拒。
  bool can_send_file(const char* what) const;
  /// `send_file*` 的共用收口：记下意图（路径 / 区间 / 组帧方式 / 回调），
  /// **只记录、不启动** —— 真到 `pump_stream()` 里才起步。
  void arm_file_transfer(const std::string& path, uint64_t first, uint64_t last,
                         bool to_eof, bool raw,
                         const std::function<void(int, uint64_t)>& done);
  /// `send_file*` 只记录意图，真到 `pump_stream()` 里才起步 —— 因为处理函数
  /// 执行期间框架还没装 sink（没有 loop）。
  ///
  /// @return 0 = **传输已经提交、这条流还活着**（`file_pending_` 为真，等 I/O）。
  ///         非 0 = **这条流已经被收尾**（参数非法 / sink 给不出 loop / 提交失败
  ///         / HEAD 上根本不需要读）—— 那几条路径走的都是 `file_finished()`，
  ///         它会把整条流收干净。
  /// @warning **调用方拿到非 0 必须立刻返回，不得再碰任何成员** —— 收尾可能
  ///          已经把 `this` 连同调用方的栈帧一起销毁了。
  int start_file_transfer();
  /// 传输结束的回调（成功 / 出错 / 被取消都走这里）。**不得重置 `file_`**：
  /// 本函数由传输自己的 `on_done` 调用，销毁传输等于在它的回调里释放它。
  void file_finished(int status, uint64_t bytes_sent);

  /// 把头部块交给 sink，**幂等**（`head_sent_` 即闸门）。
  ///
  /// `send_file*` 把头部**推迟**到第一片（或收尾）才发，为的是留出一个能
  /// 如实报错的窗口 —— 见 `fail_stream_before_head()`。
  void send_stream_head();
  /// 第一个字节之前就失败了：**只把这条流改写成错误响应**，收尾由调用方做。
  ///
  /// 头部一旦上线，状态码就锁死了，能做的只剩"截断 body + 关连接"。所以这条
  /// 路径只在 `head_sent_` 为假时可达 —— 那是 `send_file*` 唯一能报出 404/403
  /// 的窗口，也是"文件打不开"这件事**如实告诉对端**的唯一机会。
  ///
  /// **它自己不 flush、也不收尾**（唯一调用者是 `file_finished()`）。理由是
  /// 次序：改写完之后 `file_finished()` 还要调用户的 `done` 回调，而 flush 在
  /// sink 拒收时会**同步**把整条流收干净、连带析构 `this` —— 那样 `done` 就
  /// 再也发不出去了。所以本函数只负责"变成什么样"，"什么时候发出去"留在
  /// 调用方的 `cb` 之后。
  void fail_stream_before_head(int status);

  /// 分片读的接收方。**必须是嵌套类**：它要读写 `file_raw_` / `pending_bytes_`
  /// 这些私有成员，而定义在匿名命名空间里的类访问不到。
  class file_slice_sink;

  uvcpp_http_response resp_;

  bool head_only_;
  bool ended_;
  bool deferred_;
  std::vector<uvcpp_web_sent_cb> sent_cbs_;
  uvcpp_web_error_handler error_cb_;

  // --- 流式状态 ---
  std::unique_ptr<uvcpp_web_stream_sink> sink_;
  std::string pending_buf_;      ///< 攒着还没交给 sink 的字节（已组帧）
  size_t      pending_bytes_;    ///< 上面那份 + 已交给 sink 但未确认写完的
  size_t      stream_bytes_;     ///< 已写出的 body 字节（不含组帧）
  size_t      max_stream_bytes_; ///< 高水位
  bool        streaming_;
  bool        head_sent_;        ///< `stream_begin` 已经跑过
  bool        stream_finished_;  ///< `end()` 已调（终止块已入待发队列）
  bool        stream_done_;      ///< 已经收尾过（幂等闸门）
  bool        drain_armed_;      ///< 越过过水位，等着降到半水位时回调
  int         stream_status_;    ///< 记第一次非 0 的写失败
  std::vector<std::function<void()> > drain_cbs_;
  std::function<void(int)> stream_done_cb_;

  // --- 分片下发的状态 ---
  //
  // 五个 bool 各回答一个问题，混掉任何一个都会让某条收尾路径漏掉：
  //   file_         —— 在途的传输（谁在跑）
  //   file_raw_     —— 组帧方式：true = `append_raw`（CL 模式，body 原样发），
  //                    false = `write_chunk`（chunked，要组 hex 帧）
  //   file_armed_   —— `send_file*` 已经调过（意图已记下，等 `pump_stream`）
  //   file_started_ —— `start_file_transfer()` 已经提交过（幂等闸门）
  //   file_pending_ —— 传输还没结束（`stream_finish_ready()` 的收尾闸门）
  std::shared_ptr<uvcpp_web_file_transfer> file_;
  /// 传输的接收方。析构函数在 .cpp 里（`file_slice_sink` 是嵌套类，这里只有
  /// 前向声明），而 `unique_ptr<不完整类型>` 的析构要求完整类型 —— 所以本类的
  /// 析构也必须定义在 .cpp 里，这是既有约定。
  std::unique_ptr<file_slice_sink> file_sink_;
  bool        file_raw_;
  bool        file_armed_;
  bool        file_started_;
  bool        file_pending_;
  std::string file_path_;
  uint64_t    file_first_;
  uint64_t    file_last_;
  bool        file_to_eof_;
  int         file_status_;
  std::function<void(int, uint64_t)> file_done_cb_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_RESPONSE_H
