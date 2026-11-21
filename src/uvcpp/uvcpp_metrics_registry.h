/**
 * @file src/uvcpp/uvcpp_metrics_registry.h
 * @brief 进程内的指标注册表：Counter / Gauge / Histogram + Prometheus 文本。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这是**可观测性那一层的核心**，不依赖 webapp、不依赖任何外部库：
 *
 * ```cpp
 * uvcpp::uvcpp_metrics_registry reg;
 * reg.inc("http_requests_total", {{"method", "GET"}, {"status", "200"}});
 * reg.observe("request_duration_ms", 12.5);
 * reg.register_gauge("db_pool_in_use", "Connections currently borrowed",
 *                    [&pool] { return static_cast<double>(pool.in_use()); });
 *
 * std::string text = reg.render();   // Prometheus/OpenMetrics 文本格式
 * ```
 *
 * ## 三条规矩
 *
 * 1. **线程安全**：所有更新方法内部一把互斥量；`render()` 也是。指标更新出现在
 *    每个请求上，而 `render()` 是运维路径（每秒一次都算密），这把锁不是瓶颈。
 * 2. **标签基数由调用方负责**：本类不阻止你把 URL 原串、用户 ID 塞进标签 —— 那会
 *    让时间序列数爆炸。约定见 `doc/webapp-guide.md`（只用方法、状态码、路由模式
 *    这类有限集合）。
 * 3. **关闭即零成本**：没装指标模块、也没人调这些方法时，本类只是一块空内存；
 *    没有后台线程、没有定时器、没有全局单例。
 *
 * ## 名字与标签
 *
 * 名字原样输出（不做前缀）；标签按名字排序后输出，保证同一组标签的字节稳定
 * （Prometheus 按 `名字{标签}` 识别时间序列）。值里的 `\`、`"`、换行按文本格式
 * 转义。直方图的桶用**绝对上界**（毫秒、字节这类），`_bucket{le="..."}` 是
 * 累计的。
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_METRICS_REGISTRY_H
#define SRC_UVCPP_UVCPP_METRICS_REGISTRY_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace uvcpp {

/**
 * @brief 一条标签。`name` 必须是合法的 Prometheus 标签名（调用方保证，本类不校验）。
 */
struct UVCPP_API uvcpp_metric_label {
  std::string name;
  std::string value;
};

using uvcpp_metric_labels = std::vector<uvcpp_metric_label>;

/**
 * @brief 指标注册表。见文件头。
 */
class UVCPP_API uvcpp_metrics_registry {
 public:
  /// 直方图默认桶（毫秒）：1、5、10、25、50、100、250、500、1000、2500、5000、10000。
  static std::vector<double> default_buckets();

  uvcpp_metrics_registry();
  ~uvcpp_metrics_registry();

  uvcpp_metrics_registry(const uvcpp_metrics_registry&) = delete;
  uvcpp_metrics_registry& operator=(const uvcpp_metrics_registry&) = delete;

  // ---- 写 ----

  /// Counter 加 @p delta（默认 1）。Counter 只增不减。
  void inc(const std::string& name, const uvcpp_metric_labels& labels = {},
           uint64_t delta = 1);

  /// Gauge 设成 @p value。
  void set_gauge(const std::string& name, double value,
                 const uvcpp_metric_labels& labels = {});

  /// Gauge 加减 @p delta。
  void add_gauge(const std::string& name, double delta,
                 const uvcpp_metric_labels& labels = {});

  /**
   * @brief 注册一棵"渲染时才取值"的 Gauge（没有标签）。
   *
   * 给"数值本来就在别处维护"的场合用（连接池的 `in_use()`、线程池深度）——
   * 不需要每请求同步一次，`render()` 那一刻读到什么就报什么。
   *
   * 回调在 `render()` 里、**锁外**调用；回调不得再调本注册表的写方法
   * （那会自我死锁），读方法也请避免。
   */
  void register_gauge(const std::string& name, const std::string& help,
                      std::function<double()> fn);

  /**
   * @brief 记一次观测（Histogram）。桶可用 `configure_histogram()` 换。
   *
   * 未配置过的指标用 `default_buckets()`（毫秒）。
   */
  void observe(const std::string& name, double value,
               const uvcpp_metric_labels& labels = {});

  /// 配置一个 Histogram 的名字 / 帮助文本 / 绝对上界（升序；内部会排序去重）。
  void configure_histogram(const std::string& name, const std::string& help,
                           const std::vector<double>& buckets);

  /// 配一条 HELP 文本（可选的；没配就只有样例行）。
  void set_help(const std::string& name, const std::string& help);

  /// 清空所有指标（测试与重启用；不是线程安全的"热重置"，别在跑服务时调）。
  void clear();

  // ---- 读 ----

  /// 渲染成 Prometheus 文本格式（`# HELP` / `# TYPE` / 样例行）。
  std::string render() const;

 private:
  struct histogram {
    std::vector<uint64_t> counts;  ///< 与 buckets 等长：**小于等于**该上界的累计数
    uint64_t              count = 0;
    double                sum   = 0;
  };

  struct family {
    enum kind_t { K_COUNTER, K_GAUGE, K_HISTOGRAM } kind = K_COUNTER;
    std::string help;

    /// 样例键（标签序列化后的串）→ 值。Counter / Gauge。
    std::map<std::string, double> values;
    /// 样例键 → 标签（render 时输出用）。
    std::map<std::string, uvcpp_metric_labels> labels;

    /// Histogram 专用（按样例键）。
    std::vector<double>     buckets;
    std::map<std::string, histogram> hists;

    /// 回调 Gauge（无标签）。
    std::vector<std::pair<std::string, std::function<double()>>> gauges;
  };

  family& family_for(const std::string& name, family::kind_t kind);

  mutable std::mutex                      mu_;
  std::map<std::string, family>           families_;
};

/// 把一组标签转成 `{a="b",c="d"}`；空标签返回空串。值按文本格式转义。
UVCPP_API std::string uvcpp_metric_label_string(
    const uvcpp_metric_labels& labels);

}  // namespace uvcpp

#endif  // SRC_UVCPP_UVCPP_METRICS_REGISTRY_H
