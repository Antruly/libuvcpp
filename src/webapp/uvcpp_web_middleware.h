/**
 * @file src/webapp/uvcpp_web_middleware.h
 * @brief 内置中间件：访问日志、错误兜底、CORS、请求 ID、体积上限、标识头。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 中间件是什么
 * ------------
 * 「中间件」和「最终 handler」在类型上是**同一个东西**
 * （`uvcpp_web_handler`），区别只在于前者会调 `next()` 放行。所以本文件里
 * 每个工厂函数返回的都是一条现成的 `uvcpp_web_handler`，直接塞进链里即可：
 *
 * @code
 *   app.use(web_middleware_access_log());
 *   app.use(web_middleware_cors());
 *   app.get("/user/:id", handler);
 * @endcode
 *
 * 不调 `next()` 就是**终止链**（本文件里的 `body_limit` 和 CORS 预检都靠
 * 这个语义短路），这与「链跑完了自然结束」是两回事。
 *
 * 关于异步
 * --------
 * 中间件全程在 loop 线程上跑，这里没有一处阻塞调用。要干重活（读盘、算
 * 哈希）应当在 handler 里交给线程池，而不是塞进中间件 —— 中间件是每个请求
 * 都要过的路径，在这里阻塞等于给整个服务设了个全局闸门。
 *
 * 关于 C++11 捕获
 * ---------------
 * 没有 init-capture（那要 C++14），所以配置对象是**按值整体拷进 lambda**
 * 的。配置结构体都很小，且只在注册时拷一次，不在请求路径上。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_MIDDLEWARE_H
#define SRC_WEBAPP_UVCPP_WEB_MIDDLEWARE_H

#include <cstddef>
#include <string>

#include <uvcpp/uvcpp_export.h>
#include <webapp/uvcpp_web_handler.h>

namespace uvcpp {

// =========================================================================
// 1. 访问日志
// =========================================================================

/**
 * @brief 每个请求打一条 `INFO(REQUEST)` 访问日志。
 *
 * 输出形如：`GET /user/42 -> 200 (128B, 3ms)`。
 *
 * 靠 `response.on_sent()` 拿到**最终**状态码和字节数 —— 在中间件里直接读
 * 是读不准的，因为那时链条还没跑完，后面的 handler 还会改状态码；而且
 * 异步 handler 要等到线程池回调才结束，中间件早就返回了。
 *
 * @warning 它捕获了请求对象的指针，**依赖 response 在请求对象存活期内完成
 *          发送**。框架的 context 同时持有两者，所以成立；但如果你把
 *          response 的发送拖到请求对象析构之后（Phase 3 的流式响应要小心
 *          这一点），这条日志就会读到悬垂指针。
 *
 * 应当注册在**最外层**（第一个），这样它统计到的耗时覆盖整条链。
 */
UVCPP_API uvcpp_web_middleware web_middleware_access_log();

// =========================================================================
// 2. 错误兜底
// =========================================================================

/**
 * @brief 安装一个错误处理器：链上抛出的异常由它转成一个正常响应。
 *
 * 没有它的话，handler 抛异常 = 客户端收到连接被掐断（或者永远等不到响应）。
 * 有了它，异常变成 500。
 *
 * **不接异常会怎样**：框架一定会把异常吞掉（不吞就是整个进程挂掉），所以
 * 这不是"要不要处理"的选择，而是"由谁处理"。默认行为是打一条
 * `ERROR(REQUEST)` 然后回 500；这个中间件让你换成自己的（比如上报到监控、
 * 或者按异常类型回不同状态码）。
 *
 * @param handler 自定义处理器。传空则等价于不注册。
 *
 * @note 处理器本身抛异常会被框架吞掉并记一条错误日志 —— 不会再触发一次
 *       错误处理（那样会无限递归）。
 */
UVCPP_API uvcpp_web_middleware
web_middleware_error_handler(const uvcpp_web_error_handler& handler);

// =========================================================================
// 3. CORS
// =========================================================================

/**
 * @brief 跨域配置。
 *
 * 所有字段都有默认值，用**显式构造函数**赋默认值而不是成员初始化器 ——
 * 成员初始化器（NSDMI）会让结构体失去 C++11 的聚合初始化资格。
 */
struct UVCPP_API web_cors_options {
  /// 允许的来源。`"*"` = 任意来源（默认）。也可以是一个具体来源。
  std::string origin;

  /// 预检响应里 `Access-Control-Allow-Methods` 的值。
  std::string methods;

  /**
   * @brief 预检响应里 `Access-Control-Allow-Headers` 的值。
   *
   * 留空 = **回显**请求的 `Access-Control-Request-Headers`（默认）。这是
   * 实践中最省事的做法：客户端要什么就给什么。想收紧就显式列出允许的头，
   * 那时本字段生效、不再回显。
   */
  std::string headers;

  /// `Access-Control-Expose-Headers` 的值（简单响应里客户端能读到的头）。
  std::string expose_headers;

  /// 是否允许携带凭据（Cookie / Authorization）。默认 false。
  bool credentials;

  /// 预检缓存秒数。**小于 0 表示不发** `Access-Control-Max-Age`（默认 -1）。
  long max_age;

  web_cors_options();
};

/**
 * @brief CORS 中间件：加跨域响应头，并把预检请求就地答掉。
 *
 * 只把带 `Access-Control-Request-Method` 的 OPTIONS 当**预检**处理，直接回
 * 204 并终止链；不带这个头的 OPTIONS 是普通请求，放行给路由层（路由层的
 * 自动 OPTIONS 会回 `Allow`）。这两者混在一起会让普通 OPTIONS 请求收到一个
 * 没有 `Allow` 的 204。
 *
 * **`origin == "*"` 与 `credentials == true` 的组合会被拒绝执行**：Fetch
 * 规范明确禁止这个组合，而"为了让它能跑"去回显请求的 Origin，等于允许任意
 * 站点带凭据读取本服务的响应 —— 那比完全不配 CORS 更危险。这里选择保留
 * `"*"`、丢掉 credentials，并打一条 `WARN(HTTP)`。
 */
UVCPP_API uvcpp_web_middleware
web_middleware_cors(const web_cors_options& options);

/** @brief 用默认配置的 CORS 中间件（`origin = "*"`，不带凭据）。 */
UVCPP_API uvcpp_web_middleware web_middleware_cors();

// =========================================================================
// 4. 请求 ID
// =========================================================================

/**
 * @brief 给每个请求分配一个 ID，写进响应头（默认 `X-Request-Id`）。
 *
 * 客户端带了就用客户端的（分布式追踪需要这个），没带就生成一个。两者都会
 * 回写进响应头，方便用户报障时报这一个值就能定位整条链路。
 *
 * **客户端带来的 ID 会被校验**：只接受长度 1~64 且字符集为 `[A-Za-z0-9._-]`
 * 的值，其余一律丢弃并重新生成。这不是洁癖 —— 这个值会进响应头，未校验
 * 就等于把 CRLF 注入（响应拆分）的口子开在了框架默认配置里；它同时也会进
 * 日志，还多一条日志注入。
 *
 * @param header_name 用作 ID 的头名，默认 `"X-Request-Id"`。
 * @param trust_client 是否采信客户端带来的 ID。
 *        **代价**：采信意味着客户端可以自选 ID，恶意客户端能刻意制造 ID
 *        碰撞来污染日志关联。要做严格审计的场合传 `false`，一律本服务生成。
 */
UVCPP_API uvcpp_web_middleware
web_middleware_request_id(const std::string& header_name = "X-Request-Id",
                          bool trust_client = true);

// =========================================================================
// 5. 请求体上限
// =========================================================================

/**
 * @brief 请求体超过 `max_bytes` 就回 413 并终止链。
 *
 * 这是**应用层**的闸门，和 http 层 `set_max_body_size()` 那道流式闸门是
 * 两件事：后者在解析过程中就停止累积（防止内存被撑爆，但那时请求已经读完
 * 一半），前者是在 chain 起跑前的业务策略（比如"这个接口只收 1MB"）。
 * 两道都要有，只靠任何一道都不完整。
 *
 * 同时看 `Content-Length` 声明值和实际收到的字节数：两者不一致本身就是
 * 攻击信号，取大的那个判。
 *
 * @param max_bytes 上限。**0 表示不限**（直接放行）。
 */
UVCPP_API uvcpp_web_middleware web_middleware_body_limit(size_t max_bytes);

// =========================================================================
// 6. 标识头
// =========================================================================

/**
 * @brief 加一个 `X-Powered-By` 响应头。
 *
 * @warning 这是**信息泄露**：把服务端技术栈主动告诉扫描器，能省掉对方一轮
 *          指纹识别。默认给一个泛化的值就够了，**不要**填版本号。不在意
 *          指纹的先例是它确实方便排查"请求到底有没有到本服务"，所以要开
 *          就开，只是别填细节。
 *
 * @param value 头值，默认 `"uvcpp"`。
 */
UVCPP_API uvcpp_web_middleware
web_middleware_powered_by(const std::string& value = "uvcpp");

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_MIDDLEWARE_H
