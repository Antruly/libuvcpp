/**
 * @file src/webapp/uvcpp_web_metrics.cpp
 * @brief 运维端点与请求指标中间件的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "webapp/uvcpp_web_metrics.h"

#if UVCPP_WEBAPP_ENABLE

#include <chrono>
#include <string>
#include <utility>

#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

namespace uvcpp {

uvcpp_web_metrics_options::uvcpp_web_metrics_options()
    : health_path("/health"),
      ready_path("/ready"),
      metrics_path("/metrics"),
      instrument_requests(true),
      register_connection_gauge(true) {}

uvcpp_web_metrics::uvcpp_web_metrics(uvcpp_metrics_registry& registry)
    : registry_(registry) {}

uvcpp_web_metrics::~uvcpp_web_metrics() = default;

void uvcpp_web_metrics::install(uvcpp_web_app& app,
                                const uvcpp_web_metrics_options& opts) {
  app_ = &app;

  // liveness：不查任何依赖。数据库挂了不该让编排器重启进程。
  app.get(opts.health_path,
          [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
            resp.set_header("content-type", "text/plain; charset=utf-8");
            resp.text(std::string("ok\n"));
            resp.end();
          });

  // readiness：**不缓存**结果 —— 每个请求现问一次。检查通常是一次原子读 /
  // 连接池计数，代价可以忽略；缓存反而会让"刚恢复但还没到 TTL"变成一次误判。
  app.get(opts.ready_path,
          [this](uvcpp_web_request&, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            std::string why;
            bool        ready = true;
            if (ready_check_) ready = ready_check_(why);
            resp.set_header("content-type", "text/plain; charset=utf-8");
            if (ready) {
              resp.text(std::string("ready\n"));
            } else {
              resp.status(http_status::SERVICE_UNAVAILABLE);
              std::string body = "not ready";
              if (!why.empty()) {
                body += ": ";
                body += why;
              }
              body += '\n';
              resp.text(body);
            }
            resp.end();
          });

  app.get(opts.metrics_path,
          [this](uvcpp_web_request&, uvcpp_web_response& resp,
                 uvcpp_web_next) {
            // Prometheus 文本格式的 content-type 带版本号；运维端按它挑解析器。
            resp.set_header("content-type",
                            "text/plain; version=0.0.4; charset=utf-8");
            resp.text(registry_.render());
            resp.end();
          });

  // 请求计数 / 耗时 / 在途。注册在最外层（`install()` 必须早于其它中间件），
  // 这样覆盖整条链（含错误兜底转出来的 500）。
  if (opts.instrument_requests) {
    app.use([this](uvcpp_web_request& req, uvcpp_web_response& resp,
                   uvcpp_web_next next) {
      const std::string method =
          http_method_str(req.method());
      const std::chrono::steady_clock::time_point t0 =
          std::chrono::steady_clock::now();
      registry_.add_gauge("http_requests_in_flight", 1);

      // `on_sent` 在响应真正发出去时触发（整包与 deferred 两条路都有），
      // 是"这条请求结束了"的唯一统一时刻；流式响应也是它先回调一次。
      resp.on_sent([this, method, t0](const uvcpp_web_sent_info& si) {
        registry_.add_gauge("http_requests_in_flight", -1);
        const std::string status = std::to_string(si.status_code);
        uvcpp_metric_labels labels;
        uvcpp_metric_label  m;
        m.name  = "method";
        m.value = method;
        labels.push_back(m);
        uvcpp_metric_label s;
        s.name  = "status";
        s.value = status;
        labels.push_back(s);
        registry_.inc("http_requests_total", labels, 1);

        const double ms = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0)
                .count()) /
                          1000.0;
        uvcpp_metric_labels dl;
        uvcpp_metric_label  dm;
        dm.name  = "method";
        dm.value = method;
        dl.push_back(dm);
        registry_.observe("http_request_duration_ms", ms, dl);
      });

      next();
    });
  }

  if (opts.register_connection_gauge) {
    // 回调在 `render()` 里、锁外调用；`app` 必须活得比本对象久（install 的契约）。
    uvcpp_web_app* const a = &app;
    registry_.register_gauge("uvcpp_web_app_connections",
                             "Connections currently registered by the app",
                             [a]() {
                               return static_cast<double>(a->connection_count());
                             });
  }
}

void uvcpp_web_metrics::set_ready_check(
    std::function<bool(std::string&)> check) {
  ready_check_ = std::move(check);
}

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE
