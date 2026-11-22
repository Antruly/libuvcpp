/**
 * @file tests/functional/web_app_metrics_func.cpp
 * @brief 指标注册表 + webapp 运维端点（`/health` `/ready` `/metrics`）。
 *
 * 两层判据：
 *
 * 1. **注册表本身**（不碰网络）：Counter / Gauge / Histogram 的文本格式、
 *    标签排序与转义、`le` 累计桶、`+Inf`、`_sum` / `_count`、回调 Gauge；
 * 2. **端点**：真 `uvcpp_web_app` + 真 TCP 客户端：
 *    - `/health` 恒 200（liveness 不查依赖）；
 *    - `/ready` 随 `set_ready_check()` 的返回在 200 / 503 之间切换，并带上原因；
 *    - `/metrics` 里能看到请求计数（含 404 那条）、耗时直方图、在途 Gauge 归零、
 *      连接数回调 Gauge。
 *
 * 文件名带 `web_app_` 是**必须的**（webapp 是可选模块、默认关；见
 * `tests/functional/CMakeLists.txt` 的命名约定）。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include <uvcpp/uvcpp_metrics_registry.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_metrics.h>

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "  [FAIL] " << what << std::endl;
  }
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// 1. 注册表
// ---------------------------------------------------------------------------
void test_registry_text() {
  std::cout << "-- 注册表：文本格式 --" << std::endl;
  uvcpp_metrics_registry reg;

  uvcpp_metric_labels l;
  uvcpp_metric_label  a;
  a.name  = "status";
  a.value = "200";
  l.push_back(a);
  uvcpp_metric_label b;
  b.name  = "method";
  b.value = "GET";
  l.push_back(b);  // 故意倒序给，渲染应按名字排好

  reg.inc("requests_total", l);
  reg.inc("requests_total", l);
  reg.set_gauge("in_flight", 3);
  reg.add_gauge("in_flight", -1);
  reg.register_gauge("pool_size", "Pool size", [] { return 7.0; });
  reg.configure_histogram("latency_ms", "Latency", {10, 100});
  reg.observe("latency_ms", 5);
  reg.observe("latency_ms", 50);
  reg.observe("latency_ms", 500);

  const std::string t = reg.render();
  std::cout << t;

  check(contains(t, "# TYPE requests_total counter"),
        "Counter 的 TYPE 行");
  check(contains(t,
                 "requests_total{method=\"GET\",status=\"200\"} 2"),
        "标签按名字排序、值正确");
  check(contains(t, "# TYPE in_flight gauge"), "Gauge 的 TYPE 行");
  check(contains(t, "in_flight 2"), "Gauge 加减小计正确");
  check(contains(t, "pool_size 7"), "回调 Gauge 在 render 时取值");
  check(contains(t, "# TYPE latency_ms histogram"), "Histogram 的 TYPE 行");
  check(contains(t, "latency_ms_bucket{le=\"10\"} 1"),
        "le=10 的累计桶（5 落在这里）");
  check(contains(t, "latency_ms_bucket{le=\"100\"} 2"),
        "le=100 的累计桶（5、50）");
  check(contains(t, "latency_ms_bucket{le=\"+Inf\"} 3"), "+Inf 桶");
  check(contains(t, "latency_ms_sum 555"), "sum = 5+50+500");
  check(contains(t, "latency_ms_count 3"), "count = 3");

  // 标签值转义
  uvcpp_metrics_registry esc;
  uvcpp_metric_labels    el;
  uvcpp_metric_label     e;
  e.name  = "v";
  e.value = "a\"b\nc";
  el.push_back(e);
  esc.inc("esc_total", el);
  check(contains(esc.render(), "esc_total{v=\"a\\\"b\\nc\"} 1"),
        "标签值里的引号与换行被转义");
}

void test_registry_threads() {
  std::cout << "-- 注册表：并发更新 --" << std::endl;
  uvcpp_metrics_registry reg;
  const int              kThreads = 4;
  const int              kPerThread = 5000;
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.push_back(std::thread([&reg, kPerThread] {
      for (int k = 0; k < kPerThread; ++k) reg.inc("conc_total");
    }));
  }
  // 与更新并发地渲染几轮
  std::thread renderer([&reg] {
    for (int i = 0; i < 50; ++i) {
      std::string s = reg.render();
      (void)s;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  for (size_t i = 0; i < threads.size(); ++i) threads[i].join();
  renderer.join();
  check(contains(reg.render(), "conc_total " +
                                   std::to_string(kThreads * kPerThread)),
        "并发自增无丢失（" + std::to_string(kThreads * kPerThread) + "）");
}

// ---------------------------------------------------------------------------
// 2. 端点
// ---------------------------------------------------------------------------
struct TestApp {
  uvcpp_web_app            app;
  uvcpp_metrics_registry   reg;
  uvcpp_web_metrics        metrics{reg};
  std::atomic<bool>       ready{true};

  void build() {
    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN);

    metrics.install(app);
    metrics.set_ready_check([this](std::string& why) {
      if (ready.load()) return true;
      why = "db down";
      return false;
    });

    app.get("/hello",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.text(std::string("hello"));
              resp.end();
            });
  }

  int start() {
    if (app.start_background() != 0) return -1;
    return app.bound_port();
  }
  void shutdown() {
    app.stop();
    app.join();
  }
};

struct reply {
  int         status = 0;
  std::string body;
};

reply get(int port, const std::string& path) {
  uvcpp_http_client c;
  reply             out;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return out;
  uvcpp_http_response resp;
  if (c.send_wait(uvcpp_http_request::make_get(path), resp, 3000) != 0) {
    return out;
  }
  out.status = static_cast<int>(resp.status_code);
  out.body   = resp.body.to_string();
  return out;
}

void test_endpoints() {
  std::cout << "-- 端点：health / ready / metrics --" << std::endl;
  TestApp   t;
  t.build();
  const int port = t.start();
  check(port > 0, "app 起来了");
  if (port <= 0) {
    t.shutdown();
    return;
  }

  {
    const reply r = get(port, "/health");
    check(r.status == 200, "liveness：200");
    check(r.body == "ok\n", "liveness：body 是 ok");
  }

  {
    const reply r = get(port, "/ready");
    check(r.status == 200, "readiness（就绪）：200");
    check(r.body == "ready\n", "readiness（就绪）：body 是 ready");
    t.ready.store(false);
    const reply nr = get(port, "/ready");
    check(nr.status == 503, "readiness（未就绪）：503");
    check(contains(nr.body, "not ready: db down"),
          "readiness（未就绪）：带原因（实际 \"" + nr.body + "\"）");
    t.ready.store(true);
  }

  // 先打几条业务请求（含一条 404），再看 /metrics
  check(get(port, "/hello").status == 200, "业务请求 200");
  check(get(port, "/missing").status == 404, "未命中路由 404");

  // 先直接渲染注册表：没有在途请求时在途 Gauge 必须是 0。
  // **要轮询等待**：客户端收到响应 ≠ 服务端的 `on_sent` 已经跑过（它在下一次
  // 循环迭代里才减回去），CI 上出现过刚收完就断言、偶发读到 1 的假红。
  std::string direct = t.reg.render();
  for (int i = 0; i < 200 && !contains(direct, "http_requests_in_flight 0");
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    direct = t.reg.render();
  }
  check(contains(direct, "http_requests_in_flight 0"),
        "metrics（直接读）：在途 Gauge 归零");

  const reply m = get(port, "/metrics");
  check(m.status == 200, "metrics：200");
  std::cout << m.body;
  check(contains(m.body, "http_requests_total{method=\"GET\",status=\"200\"}"),
        "metrics：有 200 的请求计数");
  check(contains(m.body, "http_requests_total{method=\"GET\",status=\"404\"}"),
        "metrics：404 也被计数");
  check(contains(m.body, "# TYPE http_request_duration_ms histogram"),
        "metrics：耗时直方图在");
  check(contains(m.body, "http_requests_in_flight 1"),
        "metrics：在途 Gauge = 1（渲染它的这条请求自己）");
  check(contains(m.body, "uvcpp_web_app_connections"), "metrics：连接数回调 Gauge");

  t.shutdown();
}

}  // namespace

int main() {
  std::cout << "[web_app_metrics] start" << std::endl;
  test_registry_text();
  test_registry_threads();
  test_endpoints();
  if (g_fail == 0) {
    std::cout << "[web_app_metrics] ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << "[web_app_metrics] " << g_fail << " check(s) failed" << std::endl;
  return 1;
}

#else  // !WEBAPP

int main() {
  std::cout << "SKIP: webapp disabled" << std::endl;
  return 0;
}

#endif
