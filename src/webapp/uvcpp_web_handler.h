/**
 * @file src/webapp/uvcpp_web_handler.h
 * @brief 框架里那几种回调的签名。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 单独一个头文件，而不是塞进路由或中间件的头里：路由和中间件**共用同一套
 * 签名**，谁包含谁都会造成一个假的依赖方向（中间件并不依赖路由表，
 * 路由表也不依赖中间件）。
 *
 * 中间件与 handler 是同一个类型
 * -----------------------------
 * `next()` 是「放行到链上的下一个」，所以「中间件」和「最终处理器」在类型上
 * 没有区别 —— 差别只在于前者会调 `next()`。这不是省事，而是让链的组装变成
 * 一件简单事：一个 `std::vector<uvcpp_web_handler>` 就够，不需要两套东西
 * 之间的适配。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_HANDLER_H
#define SRC_WEBAPP_UVCPP_WEB_HANDLER_H

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <string>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

class uvcpp_web_request;
class uvcpp_web_response;
class uvcpp_web_context;

/**
 * @brief 放行到链上的下一个处理器。
 *
 * **什么时候、在哪个线程调都行。**
 *
 * - 在 loop 线程上同步调：立刻接着跑下一环（不递归，见 `advance()`）；
 * - 把它留起来、稍后在**工作线程**里调：框架自动投回 loop 线程再续跑，
 *   调用方不需要自己 `post()`。
 *
 * 所以要写一个异步 handler，只要把它按值捕获进工作线程的完成回调就行 ——
 * 框架看到你把 `next` 留了下来，就知道这一环会异步恢复，挂起等你。
 *
 * @warning 留了它就等于承诺**一定会调**。留了不调，这个请求永远不会有响应
 *          （要等停机时才被强行掐断）。框架会额外持有一份上下文引用，所以
 *          它不会泄漏，只是挂着。
 *
 * ★ 它**不再是 `std::function<void()>`**（`1.3.x` M4 换的，见下面的类）。
 *   换它的原因只有一个：`std::function` 只在可调用对象**平凡可拷贝**时才用
 *   内联存储（GCC 的 `__is_location_invariant` = `is_trivially_copyable`，
 *   **与 sizeof 无关**），而「闭包持一份 `shared_ptr`」天生不是 ⇒ 每处理一环
 *   两次堆分配（造一次 + 按值传参再拷一次）。本仓每请求跑两环，那一笔是 4 次。
 *
 *   代价（**不许含糊**）：`sizeof` 变大，是**源码 + ABI 双重断点** —— 拿旧头
 *   编译的调用方配新库不行。但语义一字未改：留副本 = 挂起、按值拷贝不能 move
 *   （判据就是靠拷贝把引用数抬起来）、空 next 调用照样抛。
 */
class UVCPP_API uvcpp_web_next {
 public:
  /// 空 next：`operator bool` 为假，调它抛 `std::bad_function_call`。
  uvcpp_web_next() {}

  /**
   * @brief 手造一个 next：接受任意**无参可调用对象**。
   *
   * 框架自己造的那个**不走这条路**（见 `bind()`）—— 它只持一份上下文的
   * `shared_ptr`，构造与拷贝都不碰堆。这条路留给「在框架外自己驱动一条
   * 中间件链」的用法（本仓的功能用例里有两处），内部仍然走 `std::function`。
   */
  template <typename F,
            typename = typename std::enable_if<
                !std::is_same<typename std::decay<F>::type,
                              uvcpp_web_next>::value>::type>
  uvcpp_web_next(F&& f) : fn_(std::forward<F>(f)) {}

  uvcpp_web_next(const uvcpp_web_next&) = default;
  uvcpp_web_next(uvcpp_web_next&&) = default;
  uvcpp_web_next& operator=(const uvcpp_web_next&) = default;
  uvcpp_web_next& operator=(uvcpp_web_next&&) = default;

  /**
   * @brief 交换两个 next（含「把凭据从一个搬到另一个」）。
   *
   * `uvcpp_web_stream::take_resume()` 靠它把续跑凭据**取走**而不是拷一份：
   * 拷一份会留下 `ctx → stream → resume_ → ctx` 的引用环。
   */
  void swap(uvcpp_web_next& other) {
    ctx_.swap(other.ctx_);
    fn_.swap(other.fn_);
  }

  /// 非空（框架给的一定非空）。
  explicit operator bool() const { return ctx_ != nullptr || bool(fn_); }

  /// 调用：续跑链。空的抛 `std::bad_function_call`（与原 `std::function` 同）。
  void operator()() const;

 private:
  friend class uvcpp_web_context;

  /**
   * @brief 框架专用：把 next 绑到上下文上。**零堆分配**，整个 M4 的落点。
   *
   * 存进 `ctx_` 的那份 `shared_ptr` 同时管两件事：
   *   1. 用户留着副本期间上下文不会死（异步那份就是它的命）；
   *   2. `advance()` 的「留没留 next」判据量的是它的引用数差。
   */
  static uvcpp_web_next bind(const std::shared_ptr<uvcpp_web_context>& self) {
    uvcpp_web_next n;
    n.ctx_ = self;
    return n;
  }

  /// 上下文（框架形态）。为空 = 手造形态。
  std::shared_ptr<uvcpp_web_context> ctx_;
  /// 手造的调用体（框架形态下恒为空）。
  std::function<void()> fn_;
};

/**
 * @brief 请求处理器。
 *
 * 不调 `next()` 且不结束响应 = 链停在这里，由框架收尾。
 */
using uvcpp_web_handler =
    std::function<void(uvcpp_web_request&, uvcpp_web_response&,
                       uvcpp_web_next)>;

/** @brief 中间件就是「会调 `next()` 的 handler」，类型完全相同。 */
using uvcpp_web_middleware = uvcpp_web_handler;

/**
 * @brief 错误处理器（`error_handler` 中间件用）。
 *
 * @param what 捕获到的异常描述（`what()` 或 "unknown exception"）。
 */
using uvcpp_web_error_handler = std::function<void(
    uvcpp_web_request&, uvcpp_web_response&, const std::string& what)>;

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_HANDLER_H
