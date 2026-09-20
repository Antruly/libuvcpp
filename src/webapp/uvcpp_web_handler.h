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
#include <string>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

class uvcpp_web_request;
class uvcpp_web_response;

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
 */
using uvcpp_web_next = std::function<void()>;

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
