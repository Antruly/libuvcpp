/**
 * @file src/webapp/uvcpp_web_router.h
 * @brief 路由表：模式匹配 + 优先级 + 405/OPTIONS 语义。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么自己实现一套
 * -----------------
 * `uvcpp_http_server` 自带的路由是**精确字符串匹配**：线性扫描、首次命中、
 * 没有参数、没有通配、分不出 404 和 405。它适合「注册几个固定路径」，
 * 撑不起一个应用框架。
 *
 * 本层采用的做法是：**框架只向 `uvcpp_http_server` 注册一个兜底 handler，
 * 全部路由在本层做**。好处是 http 层的 `routes_` 恒为空（它的线性扫描零
 * 成本），而且优先级/参数/405 只有一份实现，不会出现「两层路由谁先生效」
 * 这种要读两处代码才能回答的问题。
 *
 * 模式语法
 * --------
 * | 写法          | 含义                     | 例子                          |
 * |---------------|--------------------------|-------------------------------|
 * | `/user/list`  | 字面量，逐段精确         | 只匹配 `/user/list`           |
 * | `/user/:id`   | 单段参数                 | `/user/7` → `id = "7"`        |
 * | `/files/*fp`  | 通配，吃掉**剩余全部**段 | `/files/a/b` → `fp = "a/b"`   |
 *
 * 通配必须写在**最后一段**，且它**至少吃一段** —— 所以 `/files/*fp` 不匹配
 * `/files`。想要 `/files` 也响应就单独注册一条。这样定是为了让「带不带
 * 通配」这件事不产生歧义，`/files` 可以是一条独立的路由。
 *
 * 路径按 `/` 切段时**连续的斜杠会被折叠**、结尾的斜杠被忽略，所以
 * `/a//b/` 与 `/a/b` 等价（前者本来也过不了 `web_sanitize_path()`）。
 *
 * 优先级：静态 > 参数 > 通配，**与注册顺序无关**
 * ----------------------------------------------
 * 匹配时把所有候选按「逐段的特异度向量」比较，取最大的那个。特异度
 * `字面量(2) > 参数(1) > 通配(0)`，从第一段开始逐段比，第一个不同的位置
 * 就决出胜负。这是一个**全序**，所以结果与注册顺序无关 ——
 * 先注册 `/user/:id` 再注册 `/user/list`，`/user/list` 依然走字面量那条。
 *
 * 索引
 * ----
 * 请求路径先按「段数」和「首段」两个维度缩小候选集，避免上万条路由的
 * 线性扫描：
 *  - 无通配的按**精确段数**分桶；
 *  - 带通配的按**通配符之前的段数**分桶（它能匹配所有 `N >= k+1`）；
 *  - 每个桶内再按**首段字面量**分一次，首段是参数/通配的进「动态首段」桶。
 *
 * 关于分组
 * --------
 * `group("/api")` 返回的是**共享同一张路由表**的另一个视图，只多了个前缀。
 * 它和它的父对象可以随便拷贝、按值返回 —— 拷贝的是句柄，不是路由表。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_ROUTER_H
#define SRC_WEBAPP_UVCPP_WEB_ROUTER_H

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <uvcpp/uvcpp_export.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_web_handler.h>

namespace uvcpp {

// =========================================================================
// 匹配结果
// =========================================================================

/** @brief `match()` 的结果分类。 */
enum class web_route_result : int {
  /** 命中了一条路由，`handler` 有效。 */
  MATCHED = 0,
  /**
   * 路径存在但没注册 OPTIONS，且开了 `set_auto_options(true)`。
   * `handler` 为空，调用方应当回一个 204 + `Allow`。
   * 这是「自动 OPTIONS」的实现位置。
   */
  AUTO_OPTIONS,
  /** 路径存在但方法不对 —— 应当回 405 + `Allow`。 */
  METHOD_NOT_ALLOWED,
  /** 路径本身就不存在 —— 404。 */
  NOT_FOUND,
};

/** @brief 结果分类的可读名称（"MATCHED" 等）。 */
UVCPP_API const char* web_route_result_name(web_route_result r);

/**
 * @brief 一次路由匹配的结果。
 *
 * `handler` 指向路由表内部，**只要不再注册新路由**就一直有效。
 */
struct UVCPP_API web_route_match {
  web_route_result result;

  /** `MATCHED` 时的处理器；其余情况为 nullptr。 */
  const uvcpp_web_handler* handler;

  /**
   * @brief `MATCHED` 时命中的**完整模式串**（含分组前缀）；其余情况为 nullptr。
   *
   * 与 `handler` 同一个生命周期约定：指向路由表内部，**只要不再注册新路由**
   * 就一直有效。
   *
   * 有了它，调用方可以拿模式串本身做索引（例如"按模式把 WS 路由存进一张
   * `std::map<std::string, handler>`"），而不必按 `handler` 指针做身份映射 ——
   * 后者有个隐患：新注册一条路由可能让表重新分配，旧指针被新处理器复用。
   */
  const std::string* pattern;

  /**
   * @brief 本次命中的是「HEAD 回退到 GET」，调用方需要把 body 丢掉。
   *
   * 框架会据此调 `uvcpp_web_response::set_head_only(true)`。
   */
  bool head_of_get;

  /** @brief 路径参数（`:id` / `*path` 绑定的值），保序。 */
  std::vector<std::pair<std::string, std::string> > params;

  /** @brief 该路径实际支持的方法（405/OPTIONS 时有意义），已排序去重。 */
  std::vector<http_method> allowed_methods;

  /** @brief `allowed_methods` 拼好的 `Allow` 头值，如 `"GET, HEAD"`。 */
  std::string allow;

  /// 显式构造函数，**不用 NSDMI**（那会破坏聚合初始化）。
  web_route_match();

  /** @brief 取一个路径参数，不存在返回 nullptr。 */
  const std::string* param(const std::string& name) const;
};

// =========================================================================
// 路由表
// =========================================================================

/**
 * @brief 路由表（值语义的句柄，底层表可以被多个视图共享）。
 */
class UVCPP_API uvcpp_web_router {
 public:
  uvcpp_web_router();
  explicit uvcpp_web_router(const std::string& prefix);
  ~uvcpp_web_router();
  uvcpp_web_router(const uvcpp_web_router& other);
  uvcpp_web_router& operator=(const uvcpp_web_router& other);

  // -------------------------------------------------------------------
  // 注册
  // -------------------------------------------------------------------

  /**
   * @brief 注册一条路由。
   *
   * @param method  方法
   * @param pattern 模式（见文件头的语法表）。不以 `/` 开头时会自动补上。
   * @param handler 处理器
   * @return 模式非法时返回 false 并记一条 `WARN(ROUTER)`；此时**不会**
   *         注册任何东西。
   *
   * 模式非法的情形：空模式、`:name` 名字为空、`*` 不在最后一段、
   * 字面量段里混入了 `:` 或 `*`。
   */
  bool add(http_method method, const std::string& pattern,
           const uvcpp_web_handler& handler);

  bool get(const std::string& pattern, const uvcpp_web_handler& handler);
  bool post(const std::string& pattern, const uvcpp_web_handler& handler);
  bool put(const std::string& pattern, const uvcpp_web_handler& handler);
  bool del(const std::string& pattern, const uvcpp_web_handler& handler);
  bool patch(const std::string& pattern, const uvcpp_web_handler& handler);
  bool head(const std::string& pattern, const uvcpp_web_handler& handler);
  bool options(const std::string& pattern, const uvcpp_web_handler& handler);

  /** @brief 注册一条**任意方法**都命中的路由。 */
  bool any(const std::string& pattern, const uvcpp_web_handler& handler);

  /**
   * @brief 开一个带前缀的分组视图。
   *
   * 返回的对象与 `*this` **共享同一张路由表** —— 在分组上注册的路由，
   * 父对象一样能匹配到。可以嵌套（`group("/api").group("/v1")`）。
   */
  uvcpp_web_router group(const std::string& prefix) const;

  // -------------------------------------------------------------------
  // 匹配
  // -------------------------------------------------------------------

  /** @brief 按方法 + 路径匹配。 */
  web_route_match match(http_method method, const std::string& path) const;

  // -------------------------------------------------------------------
  // 策略开关
  // -------------------------------------------------------------------

  /**
   * @brief HEAD 没有注册时，回退到同名 GET 路由并把 body 丢掉。
   *
   * 默认开启。RFC 7231 §4.3.2 要求 HEAD 与 GET 的头部一致，所以对
   * 「只想探长度」的客户端来说，这个回退是符合直觉的行为。
   */
  void set_head_as_get(bool v);
  bool head_as_get() const;

  /**
   * @brief 没注册 OPTIONS 时自动回 204 + `Allow`。默认开启。
   */
  void set_auto_options(bool v);
  bool auto_options() const;

  // -------------------------------------------------------------------
  // 观测
  // -------------------------------------------------------------------

  /** @brief 表里的路由总数。 */
  size_t route_count() const;
  /** @brief 表里某个方法的路由数（`any()` 注册的算在每个方法里）。 */
  size_t route_count(http_method method) const;
  /** @brief 清空整张表（所有共享视图一起）。 */
  void clear();

  /** @brief 本视图携带的前缀（根视图为空串）。 */
  const std::string& prefix() const { return prefix_; }

  /**
   * @brief 把本视图的前缀和 pattern 拼成**完整模式**（注册时实际用的那个）。
   *
   * 这是 `match()` 结果里 `pattern` 字段会取到的值 —— 调用方想在注册之后按
   * 模式串索引自己的处理器（而不是按 `handler` 指针做身份映射），就得知道
   * 归一化之后长什么样。
   *
   * @note `add()` 会拒绝非法模式，但本函数**不校验**：它只是拼接。所以它
   *       对非法输入也会返回一个字符串，看着像注册成功了。判断注册是否成功
   *       必须看 `add()` / `get()` 那一系列的**返回值**。
   */
  std::string full_pattern(const std::string& pattern) const;

 private:
  struct router_table;

  std::shared_ptr<router_table> table_;
  std::string prefix_;
  bool head_as_get_;
  bool auto_options_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_ROUTER_H
