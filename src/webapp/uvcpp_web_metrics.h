/**
 * @file src/webapp/uvcpp_web_metrics.h
 * @brief webapp 的运维端点：`/health`、`/ready`、`/metrics`。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * uvcpp::uvcpp_metrics_registry reg;
 * uvcpp::uvcpp_web_metrics metrics(reg);
 *
 * uvcpp::uvcpp_web_app app;
 * metrics.install(app);               // 注册三个端点 + 请求计数/耗时
 * metrics.set_ready_check([](std::string& why) {
 *   return db_pool_ready(why);        // false 时 why 里写一句原因
 * });
 * ```
 *
 * ## 三个端点各回答什么
 *
 * | 端点 | 语义 | 不健康时 |
 * |---|---|---|
 * | `GET /health` | **进程活着**（liveness） | 永远 200；进程死了自然没响应 |
 * | `GET /ready`  | **能接业务**（readiness） | `set_ready_check()` 返回 false 时 503 |
 * | `GET /metrics`| Prometheus 文本（OpenMetrics 0.0.4 格式） | 永远 200 |
 *
 * `/health` 故意不查依赖：数据库挂了不该让编排器**重启**进程（重启解决不了
 * 数据库），那正是 `/ready` 的活（把它摘出负载均衡即可）。
 *
 * ## 请求指标
 *
 * `instrument_requests`（默认开）装一个**最外层**中间件，记：
 *
 * - `http_requests_total{method="GET",status="200"}` —— Counter；
 * - `http_request_duration_ms{method="GET"}` —— Histogram（默认桶见注册表）；
 * - `http_requests_in_flight` —— Gauge（响应 `on_sent` 时减回去）。
 *
 * **标签里不放 URL 原串**：那是高基数，一个爬虫就能造出几十万个时间序列。
 * 要按路由看，用 `app.get()` 时自己 `reg.inc("route_requests_total",
 * {{"route", "/user/:id"}})` —— 路由模式是有限集合。
 *
 * ## 关闭时零成本
 *
 * 不构造本对象、不 `install()`：注册表是空内存，框架里没有任何钩子。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_METRICS_H
#define SRC_WEBAPP_UVCPP_WEB_METRICS_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_WEBAPP_ENABLE

#include <functional>
#include <string>

#include <uvcpp/uvcpp_metrics_registry.h>

namespace uvcpp {

class uvcpp_web_app;

/// `install()` 的旋钮。显式构造函数（C++11 聚合初始化资格，见本仓约定）。
struct UVCPP_API uvcpp_web_metrics_options {
  uvcpp_web_metrics_options();

  std::string health_path;        ///< 默认 `/health`
  std::string ready_path;         ///< 默认 `/ready`
  std::string metrics_path;       ///< 默认 `/metrics`
  /// 是否装请求计数 / 耗时 / 在途中间件。默认 true。
  bool instrument_requests;
  /// 是否注册 `uvcpp_web_app_connections`（当前登记表里的连接数）。默认 true。
  bool register_connection_gauge;
};

/**
 * @brief 把注册表接到 `uvcpp_web_app` 上。见文件头。
 */
class UVCPP_API uvcpp_web_metrics {
 public:
  explicit uvcpp_web_metrics(uvcpp_metrics_registry& registry);
  ~uvcpp_web_metrics();

  uvcpp_web_metrics(const uvcpp_web_metrics&) = delete;
  uvcpp_web_metrics& operator=(const uvcpp_web_metrics&) = delete;

  /**
   * @brief 注册端点与（可选）请求中间件。**在 `app.start()` 之前调。**
   *
   * @param app 必须活得比本对象久（连接数 gauge 捕的是它的地址）。
   */
  void install(uvcpp_web_app& app,
               const uvcpp_web_metrics_options& opts = uvcpp_web_metrics_options());

  /**
   * @brief 设置 readiness 检查。回调在**循环线程**上、每个 `/ready` 请求一次。
   *
   * @param check 返回 false 表示未就绪；可通过出参写一句人读原因。
   *              传空函数 = 永远就绪。
   */
  void set_ready_check(std::function<bool(std::string&)> check);

  uvcpp_metrics_registry& registry() { return registry_; }

 private:
  uvcpp_metrics_registry&                 registry_;
  uvcpp_web_app*                          app_ = nullptr;
  std::function<bool(std::string&)>       ready_check_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE
#endif  // SRC_WEBAPP_UVCPP_WEB_METRICS_H
