/**
 * @file src/webapp/uvcpp_web_stream.h
 * @brief 流式请求体：按块交付、可暂停、可中止。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么需要它
 * ------------
 * 普通路由的请求体是**先攒完再交给 handler** 的（`uvcpp_web_request::body_*`）。
 * 上传一个大文件时那条路有两个解不掉的问题：
 *
 * 1. **内存**：整个文件必须驻留内存，上限只能靠 `set_max_body_size` 一刀切；
 * 2. **时延**：handler 要等到最后一个字节才被调用，做不了"边收边写盘"。
 *
 * 流式路由把这两个都解开：handler 在 **headers 解析完就被调用**（body 一个
 * 字节都还没到），此后每个数据块都送到 `on_data()`，收完调 `on_end()`。
 *
 * 用户不需要理解 `next`
 * ---------------------
 * 链的续跑由框架替用户留着（`uvcpp_web_context::attach_stream()` 的注释里
 * 讲了机制）。用户在 `on_end()` 里把 `resp` 填好就行 —— 框架在 `on_end()`
 * 返回后接着跑链、把响应发出去。**用户拿到的 `next` 是同一个东西**，想在
 * 中间提前放行也可以，但那是可选动作。
 *
 * 拿不到这个对象意味着什么
 * ------------------------
 * `uvcpp_web_request::stream()` 对**非流式**请求返回 `nullptr`。也就是说
 * 拿到一个 `uvcpp_web_stream*` 就说明这个请求确实是被流式认领的；反过来，
 * `stream() == nullptr` 时 `body_*` 一定有完整的内容。两者互斥，不会同时成立。
 *
 * @warning 全部方法都要在 **loop 线程**上调用。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_STREAM_H
#define SRC_WEBAPP_UVCPP_WEB_STREAM_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include <uvcpp/uvcpp_export.h>
#include <webapp/uvcpp_web_handler.h>

namespace uvcpp {

class uvcpp_web_context;
class uvcpp_web_request;

/**
 * @brief 收到一块 body 时的回调。
 *
 * @return true 继续收；false **中止**（等价于 `abort(413)`：连接会被关闭，
 *         因为剩下的 body 没人消费了）。返回值是被检查的 —— 想拒绝一个
 *         超限的上传就在这里回 false，别只是记个标志然后继续收。
 */
using uvcpp_web_stream_data_cb = std::function<bool(const char* data, size_t len)>;

/** @brief 整个 body 收完时的回调。在这里填响应。 */
using uvcpp_web_stream_end_cb = std::function<void()>;

/**
 * @brief 请求**没收到完整 body 就结束了**时的回调（对端断开 / 框架掐断）。
 *
 * 与 `on_end()` 互斥，且**最多各触发一次**。存在的意义是让落盘的上传能删掉
 * 半截文件 —— 少了它，"客户端传到 90% 断了"会留下一个看起来完整的坏文件。
 */
using uvcpp_web_stream_abort_cb = std::function<void()>;

/**
 * @brief 进度回调：已收字节 + 总长（`total` 在长度未知时为 0）。
 *
 * 每收一块调一次，因此只在有分块边界时才有意义 —— 别在里面做重活。
 */
using uvcpp_web_stream_progress_cb =
    std::function<void(uint64_t received, uint64_t total)>;

/**
 * @brief **框架级**的流钩子：永远在用户槽**之前**跑。
 *
 * 为什么需要额外一层（这是本文件里最容易看漏的一处设计）
 * ------------------------------------------------------
 * `on_data` / `on_end` / `on_abort` 都是**单槽**（各一个 `std::function`），
 * 所以框架要插一手时不能自己去调 `stream->on_end(...)` —— 用户在 handler 里
 * 一注册就把框架那一份**顶掉**了，而且顶掉之后没有任何报错，只是上传悄悄
 * 不工作。
 *
 * 按本仓既有的「框架槽 + 用户槽」分工（`set_close_manager` 与 `set_on_close`
 * 那一对），这里也开一对：框架槽先跑、用户槽后跑，两边互不覆盖。
 *
 * `on_end` 的返回值是这个结构里唯一有语义的一处
 * -------------------------------------------------
 * 只要框架槽存在，**用户槽就默认被推迟**，除非框架槽回 `false`。
 * 回 `true` 的含义是「我先扣着，稍后由 `release_end()` 放出」—— 上传路由
 * 正是这么用的：`on_end` 到达时落盘只写到了内存，`fsync` + `close` 还在
 * 线程池上跑，而用户在 `on_end` 里就要读 `req.upload()` 的结果。**先放他进来
 * 会让他读到一个还没填的结果。**
 *
 * 回 `false`（或框架槽为空）时行为与今天完全一致：立刻调用户的 `on_end`。
 */
struct uvcpp_web_stream_hooks {
  /** @brief 每块 body：框架先吃（上传路由的 multipart 解析器在这里）。 */
  std::function<void(const char*, size_t)> on_data;
  /** @brief 收完：返回 true = 推迟用户的 `on_end`，等 `release_end()`。 */
  std::function<bool()> on_end;
  /** @brief 中止：框架先清自己的状态（删掉半截的临时文件）。 */
  std::function<void()> on_abort;
};

/**
 * @brief 一次流式请求的 body 通道。
 *
 * 由框架创建并挂在请求上（`req.stream()`），生存期与上下文一致 ——
 * **不要保存这个指针到请求之外**。
 */
class UVCPP_API uvcpp_web_stream {
 public:
  ~uvcpp_web_stream();

  // -----------------------------------------------------------------
  // 读状态
  // -----------------------------------------------------------------

  /** @brief 恒为 true —— 拿不到这个对象就不是流式请求。 */
  bool is_streaming() const { return true; }

  /** @brief 已收到的 body 字节数。 */
  uint64_t received() const { return received_; }

  /** @brief 总长是否已知（客户端给了 `Content-Length`）。chunked 上传时为假。 */
  bool has_total() const { return has_total_; }

  /** @brief 声明的总长；未知时为 0。用之前先看 `has_total()`。 */
  uint64_t total() const { return total_; }

  /** @brief 完整 body 是否已经交付完（`on_end` 已触发）。 */
  bool delivered_end() const { return delivered_end_; }

  /** @brief 是否已经中止（对端断开、或自己调了 `abort()`）。 */
  bool aborted() const { return aborted_; }

  // -----------------------------------------------------------------
  // 回调
  // -----------------------------------------------------------------

  /** @brief 装数据回调。**必须在 `on_end()` 之前装好**，否则收不到数据。 */
  void on_data(uvcpp_web_stream_data_cb cb);

  /** @brief 装收完回调。 */
  void on_end(uvcpp_web_stream_end_cb cb);

  /** @brief 装中止回调（对端断开 / 框架掐断）。 */
  void on_abort(uvcpp_web_stream_abort_cb cb);

  /** @brief 装进度回调。 */
  void on_progress(uvcpp_web_stream_progress_cb cb);

  // -----------------------------------------------------------------
  // 背压
  // -----------------------------------------------------------------

  /**
   * @brief 暂停读 —— 内核缓冲区填满之后对端会被 TCP 窗口挡住。
   *
   * **不保证立刻停**：当前这一块里已经在解析器缓冲中的数据还会继续交付完
   * （`llhttp` 是同步吃下一个缓冲区的，中途插不进去）。所以它是"从下一次
   * 读开始生效"，不是"从此一刀两断"。
   *
   * @return 连接还在（暂停已下发）时为 true；已经断开时为 false。
   */
  bool pause();

  /** @brief 恢复读。没暂停过时是空操作。@return 连接还在时为 true。 */
  bool resume();

  bool paused() const { return paused_; }

  // -----------------------------------------------------------------
  // 结束
  // -----------------------------------------------------------------

  /**
   * @brief 放弃这个上传：回一个状态码，并关掉连接。
   *
   * 用在"这个上传不该继续"的场合（超限、格式不对、鉴权失败）。响应会带上
   * `Connection: close`，因为剩下的 body 没人消费了 —— 复用这条连接只会让
   * 下一个请求读到上一个上传的残留字节。
   *
   * 调用之后 `on_end()` **不会**再触发（`aborted()` 为真）。
   *
   * @param status HTTP 状态码（用 413 / 400 / 401 这类）。
   */
  void abort(int status);

  // -----------------------------------------------------------------
  // 逃生口
  // -----------------------------------------------------------------

  /** @brief 这个流所属的请求（只读视图）。 */
  const uvcpp_web_request& request() const;
  uvcpp_web_request& request();

 private:
  friend class uvcpp_web_context;
  /**
   * @brief 流式路由的包装层要能替用户留住 `next`（见 `bind_resume`）。
   *
   * 那个包装层是 `uvcpp_web_app::stream_route()` 里的一小段 lambda，没有
   * 更好的归属 —— 把 `bind_resume` 公开的话，"谁都能替别人改链的续跑凭据"
   * 就成了一个公开能力，而真正需要它的只有框架自己。
   */
  friend class uvcpp_web_app;

  explicit uvcpp_web_stream(uvcpp_web_context& ctx);
  uvcpp_web_stream(const uvcpp_web_stream&);
  uvcpp_web_stream& operator=(const uvcpp_web_stream&);

  // --- 框架内部（uvcpp_web_context 调用）---

  /** @brief 启动前记下总长（`has_total` 为假表示长度未知）。 */
  void prime(uint64_t total, bool has_total);

  /** @brief 交付一块数据。 */
  void deliver_data(const char* data, size_t len);

  /** @brief 交付消息结束。 */
  void deliver_end();

  /** @brief 交付中止（对端断开 / 框架掐断）。 */
  void deliver_abort();

  /**
   * @brief 留住链的续跑凭据。
   *
   * 这一句就是「框架替用户留 next」的全部实现：存下这份 `uvcpp_web_next` 等于
   * 多持有一份上下文引用，`advance()` 据此判定链挂起。
   */
  void bind_resume(const uvcpp_web_next& next);

  /**
   * @brief 把续跑凭据**取走**（不是拷贝）。
   *
   * 取走而不留下副本是必需的：`resume_` 捕获着上下文，而上下文又持有本对象
   * —— 留一份就是 `ctx → stream → resume_ → ctx` 的引用环，链跑完了上下文
   * 也永远不会析构（`inflight_` 摘了号也没用，泄漏是静默的）。
   *
   * @return 取到了（调用方负责调它）为 true；没有凭据（链早就收尾了）为 false。
   */
  bool take_resume(uvcpp_web_next* out);

  /** @brief 丢掉续跑凭据，不调用。用于中止路径（链已经走不下去了）。 */
  void clear_resume();

  /**
   * @brief 装框架级钩子（见 `uvcpp_web_stream_hooks`）。
   *
   * 私有 + `friend uvcpp_web_app`：公开它等于允许任意调用方去**顶掉**框架
   * 已有的接线，而真正需要它的只有框架自己（`upload_route()` 的派发路径）。
   */
  void set_framework_hooks(const uvcpp_web_stream_hooks& hooks);

  /** @brief 框架槽把用户的 `on_end` 扣住了（链正挂在等它）。 */
  bool end_deferred() const { return end_deferred_; }

  /**
   * @brief 放出被扣住的用户 `on_end`，并收口这条链。
   *
   * 由框架在异步落盘（`fsync` + `close`）真正完成之后调用。**幂等** ——
   * 连调两次只有第一次生效（`end_released_` 挡着），因为"放出用户的 `on_end`"
   * 这件事发生两次就是发两个响应。
   *
   * 调用方**存的是 conn id 而不是 `uvcpp_web_stream*`**，进来时再按 id 去
   * `inflight_` 里找"正在收请求体的那条"（`uvcpp_web_app::active_body_ctx()`；
   * 流水线之后一条连接上可能挂着好几条上下文，所以不能按 id 一查一条）：id 单调
   * 不复用，上下文真没了就查不到、什么都不做，于是「落盘还没完成、这条请求已经
   * 因为别的原因收场了」不会变成悬垂写。
   */
  void release_end();

  uvcpp_web_context* ctx_;

  uvcpp_web_stream_data_cb     data_cb_;
  uvcpp_web_stream_end_cb      end_cb_;
  uvcpp_web_stream_abort_cb    abort_cb_;
  uvcpp_web_stream_progress_cb progress_cb_;

  /// 框架级钩子（见 `uvcpp_web_stream_hooks`）。默认三个都空 = 与今天零差异。
  uvcpp_web_stream_hooks hooks_;

  /// 链的续跑凭据（见 `bind_resume()` / `take_resume()`）。
  uvcpp_web_next resume_;

  uint64_t received_;
  uint64_t total_;
  bool     has_total_;
  bool     delivered_end_;
  bool     aborted_;
  bool     paused_;
  /// 框架槽扣着用户的 `on_end`（`deliver_end()` 里由框架槽的返回值置位）。
  bool     end_deferred_;
  /// `release_end()` 已经放过一次（幂等保护）。
  bool     end_released_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_STREAM_H
