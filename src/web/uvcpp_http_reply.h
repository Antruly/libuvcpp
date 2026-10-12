/**
 * @file src/web/uvcpp_http_reply.h
 * @brief 「稍后回话」的凭据 —— 推迟应答的处理函数留在手里的那枚句柄。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 处理函数默认**同步答完**：往 `resp` 里填好、返回，框架替你发。异步的处理函数
 * （读盘、查库、调外部服务）答不了那么快，它得留住一样东西，等结果回来时凭它把
 * 响应送出去 —— 那就是本类，`uvcpp_http_server::send_reply()` 收的就是它。
 *
 * 三条传输上它是**同一件事**，只是背后的东西不同：
 *
 * | 传输 | 凭据背后 |
 * |---|---|
 * | HTTP/1.1 | 那条 TCP 连接 |
 * | HTTP/2 | 那条连接 **+ 它的一条流**（一条连接上并发跑着多条流） |
 * | HTTP/3 | 那条 QUIC 连接 **+ 它的一条流** |
 *
 * 处理函数因此不需要知道自己在哪条腿上 —— 这正是它存在的理由。同一件事以前只有
 * h1/h2 做得到（那两条腿上处理函数手里的 `uvcpp_tcp_client*` 就是凭据），h3 那条
 * 腿上没有对应物，于是 `resp.deferred` 只能被**显式拒绝**。
 *
 * ### 为什么它不是 `uvcpp_http_request` 那种带虚表的类
 *
 * 本仓的请求/响应对象都走 `UVCPP_DEFINE_FUNC`（`explicit 构造 + virtual 析构`），
 * 于是各带一个虚表指针。本类**刻意不走那条路**：它是个**值语义的句柄**，没有多态、
 * 不拥有资源、按值进按值出，而它又是 `uvcpp_http_request` 的一个成员 —— 那条结构
 * 在 h1 的**每请求**热路上。多一个虚表指针等于每个请求都白付一次。
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_REPLY_H
#define SRC_WEB_UVCPP_HTTP_REPLY_H

#include <cstdint>
#include <string>

// 显式包含：`.cpp` 里那道 `#if UVCPP_WEB_ENABLE` 靠它，传递断了的话整个 `.cpp`
// 会静默编成空文件 —— 那是本仓最忌讳的「没测表现为通过」的编译期版本。
#include <uvcpp/uvcpp_config.h>
#if UVCPP_WEB_ENABLE

#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>
#include <web/uvcpp_http_common.h>

namespace uvcpp {

class uvcpp_http_server;
class uvcpp_loop;
class uvcpp_tcp_client;
#if UVCPP_HTTP3_ENABLE
class uvcpp_quic_connection;
#endif

/**
 * @brief 一条请求的「稍后回话」凭据。**按值持有**，不拥有任何东西。
 *
 * 拿到它的方式只有一个：处理函数收到的那份请求里，`req.reply`。
 *
 * ### 它不保证"还能回话"
 *
 * 凭据记住的是**这一刻**的那条连接。对端断开、连接被回收、指针被新连接复用 ——
 * 这些都可能在处理函数排队等结果的期间发生。所以"能不能写"要**在写之前**问：
 *
 * @code
 * server.get("/slow", [&server](uvcpp_http_request& req,
 *                               uvcpp_http_response& resp,
 *                               uvcpp_tcp_client*) {
 *   const uvcpp_http_reply reply = req.reply;   // 按值留一份，别留引用
 *   resp.deferred = true;                       // 声明"这条响应由我负责"
 *   // 在别的线程上等结果，再回到 `reply.loop()` 那条线程（本仓的惯用入口是
 *   // `uvcpp_work::queue_work()` 的 after_work 那一半）。第一个闭包在工作线程
 *   // 上跑（读盘 / 查库 / 调外部服务），第二个在循环线程上跑：
 *   uvcpp_work* w = new uvcpp_work();
 *   w->queue_work(reply.loop(),
 *                 [](uvcpp_work*) { ... },
 *                 [&server, reply](uvcpp_work* self, int) {
 *                   delete self;
 *                   if (!server.reply_alive(reply)) return;  // 换人了：丢掉
 *                   uvcpp_http_response out = uvcpp_http_response::ok("done", 4);
 *                   server.send_reply(reply, out);           // 循环线程上写
 *                 });
 * });
 * @endcode
 *
 * 判据是 `reply_alive()`（代次对得上），不是 `valid()` —— 后者只说明"这枚凭据是
 * 从一条真请求上取下来的"，取下来之后它一直是真。`send_reply()` 自己也查一遍，
 * 不一致时**什么都不发**并返回 `UV_ENOENT`：把 A 的响应发给 B 是这条路上唯一
 * 不可接受的结局，宁可丢。
 *
 * ### 线程
 *
 * `send_reply()` 必须跑在 `loop()` 那条循环的线程上。跨线程要用循环自己的投递
 * 入口投回去，别直接调。
 */
class UVCPP_API uvcpp_http_reply {
 public:
  /** @brief 造一枚**无效**凭据：`valid()` 为假，`send_reply()` 一定拒绝。 */
  uvcpp_http_reply() = default;

  // 值语义：默认的拷贝/移动都合适（浅拷贝那几个裸指针，本来就只是"记住是谁"）。
  uvcpp_http_reply(const uvcpp_http_reply&) = default;
  uvcpp_http_reply& operator=(const uvcpp_http_reply&) = default;

  /**
   * @brief 这枚凭据是从一条真请求上取下来的吗。
   *
   * 手搓的请求对象（`uvcpp_http_request req;` 之后自己填字段那种）里那一枚是默认
   * 构造的，因此为假 —— 拿它 `send_reply()` 会返回 `UV_EINVAL`。它**不**说明
   * 连接还在，那件事问 `uvcpp_http_server::reply_alive()`。
   */
  bool valid() const { return server_ != nullptr; }

  /// @brief 这条请求走的传输（`HVER_11` / `HVER_20` / `HVER_30`）。
  uvcpp_http_version version() const { return version_; }

  /**
   * @brief 这条请求在它那条连接上的流号。
   *
   * h1 上恒为 0（一条连接同一时刻只有一条在跑的请求）；h2/h3 上是真流号 ——
   * 一条连接上并发跑着多条流时，它是"我回的是哪一条"的唯一凭据。
   */
  int32_t stream_id() const { return stream_id_; }

  /**
   * @brief 连接代次：同一格连接槽被新连接复用时会变。
   *
   * 单调递增、不复用，0 表示"从未登记"。`reply_alive()` 比的正是这个数。
   */
  uint64_t generation() const { return generation_; }

  /**
   * @brief 这条请求所在的那条循环。
   *
   * 唯一正确的用法是"把工作投回它"：异步结果回来时人可能在任何线程上，而
   * `send_reply()` 只在循环线程上合法。
   *
   * @warning 凭据无效时返回 `nullptr`。循环本身的生命周期不归本类管 ——
   *          服务器停了之后它照旧可能是个野指针，所以**先问 `reply_alive()`**。
   */
  uvcpp_loop* loop() const { return loop_; }

  /**
   * @brief 给人看的诊断串，形如 `HTTP/2 stream 5 (gen 7)`。
   *
   * 只在日志与错误信息里用，别拿去判分支 —— 它的措辞会变。
   */
  std::string describe() const {
    if (!valid()) return "invalid reply";
    std::string s = uvcpp_http_version_str(version_);
    if (stream_id_ != 0) {
      s += " stream ";
      s += std::to_string(stream_id_);
    }
    s += " (gen ";
    s += std::to_string(static_cast<unsigned long long>(generation_));
    s += ")";
    return s;
  }

 private:
  friend class uvcpp_http_server;

  uvcpp_http_server* server_ = nullptr;  ///< 空 = 无效凭据
  uvcpp_tcp_client*  client_ = nullptr;  ///< h1 / h2；h3 上为空
#if UVCPP_HTTP3_ENABLE
  uvcpp_quic_connection* quic_ = nullptr;  ///< h3；别的腿上为空
#endif
  uvcpp_loop*        loop_       = nullptr;
  int                loop_index_ = 0;  ///< `loop_` 在服务器里的序号（h3 查表要用）
  int32_t            stream_id_  = 0;
  uint64_t           generation_ = 0;
  uvcpp_http_version version_    = static_cast<uvcpp_http_version>(1);
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_REPLY_H
