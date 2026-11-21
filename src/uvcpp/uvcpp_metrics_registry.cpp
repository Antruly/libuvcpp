/**
 * @file src/uvcpp/uvcpp_metrics_registry.cpp
 * @brief 指标注册表的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "uvcpp/uvcpp_metrics_registry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace uvcpp {

namespace {

void escape_label_value(const std::string& in, std::string& out) {
  out.reserve(out.size() + in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n";  break;
      default:   out += c;      break;
    }
  }
}

/// 标签序列化：**先按名字排序**，再拼 `name="value",...`。同组标签的字节稳定
/// 是 Prometheus 把两行认成"同一个时间序列"的前提。
std::string label_key(const uvcpp_metric_labels& labels) {
  uvcpp_metric_labels sorted = labels;
  std::sort(sorted.begin(), sorted.end(),
            [](const uvcpp_metric_label& a, const uvcpp_metric_label& b) {
              return a.name < b.name;
            });
  std::string out;
  for (size_t i = 0; i < sorted.size(); ++i) {
    if (i != 0) out += ',';
    out += sorted[i].name;
    out += '=';
    out += sorted[i].value;  // 键里不需要转义，只要不歧义
  }
  return out;
}

std::string render_labels(const uvcpp_metric_labels& labels) {
  uvcpp_metric_labels sorted = labels;
  std::sort(sorted.begin(), sorted.end(),
            [](const uvcpp_metric_label& a, const uvcpp_metric_label& b) {
              return a.name < b.name;
            });
  std::string out = "{";
  for (size_t i = 0; i < sorted.size(); ++i) {
    if (i != 0) out += ',';
    out += sorted[i].name;
    out += "=\"";
    escape_label_value(sorted[i].value, out);
    out += '"';
  }
  out += '}';
  return out;
}

/// 数值输出：整数不带小数点（好看且省字节），其余用 10 位有效数字。
std::string fmt_double(double v) {
  char buf[64];
  if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) {
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(v));
  } else if (std::isnan(v)) {
    return "NaN";
  } else if (std::isinf(v)) {
    return v > 0 ? "+Inf" : "-Inf";
  } else {
    std::snprintf(buf, sizeof(buf), "%.10g", v);
  }
  return std::string(buf);
}

}  // namespace

std::string uvcpp_metric_label_string(const uvcpp_metric_labels& labels) {
  if (labels.empty()) return std::string();
  return render_labels(labels);
}

std::vector<double> uvcpp_metrics_registry::default_buckets() {
  const double b[] = {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000,
                      10000};
  return std::vector<double>(b, b + sizeof(b) / sizeof(b[0]));
}

uvcpp_metrics_registry::uvcpp_metrics_registry() {}
uvcpp_metrics_registry::~uvcpp_metrics_registry() {}

uvcpp_metrics_registry::family& uvcpp_metrics_registry::family_for(
    const std::string& name, family::kind_t kind) {
  std::map<std::string, family>::iterator it = families_.find(name);
  if (it == families_.end()) {
    family f;
    f.kind = kind;
    if (kind == family::K_HISTOGRAM) f.buckets = default_buckets();
    it = families_.insert(std::make_pair(name, std::move(f))).first;
  }
  return it->second;
}

void uvcpp_metrics_registry::inc(const std::string& name,
                                 const uvcpp_metric_labels& labels,
                                 uint64_t delta) {
  std::lock_guard<std::mutex> lk(mu_);
  family&                     f = family_for(name, family::K_COUNTER);
  if (f.kind != family::K_COUNTER) return;  // 同名不同类型：忽略（不静默改类型）
  const std::string key = label_key(labels);
  f.values[key] += static_cast<double>(delta);
  if (f.labels.find(key) == f.labels.end()) {
    uvcpp_metric_labels sorted = labels;
    std::sort(sorted.begin(), sorted.end(),
              [](const uvcpp_metric_label& a, const uvcpp_metric_label& b) {
                return a.name < b.name;
              });
    f.labels[key] = sorted;
  }
}

void uvcpp_metrics_registry::set_gauge(const std::string& name, double value,
                                       const uvcpp_metric_labels& labels) {
  std::lock_guard<std::mutex> lk(mu_);
  family&                     f = family_for(name, family::K_GAUGE);
  if (f.kind != family::K_GAUGE) return;
  const std::string key = label_key(labels);
  f.values[key] = value;
  if (f.labels.find(key) == f.labels.end()) {
    uvcpp_metric_labels sorted = labels;
    std::sort(sorted.begin(), sorted.end(),
              [](const uvcpp_metric_label& a, const uvcpp_metric_label& b) {
                return a.name < b.name;
              });
    f.labels[key] = sorted;
  }
}

void uvcpp_metrics_registry::add_gauge(const std::string& name, double delta,
                                       const uvcpp_metric_labels& labels) {
  std::lock_guard<std::mutex> lk(mu_);
  family&                     f = family_for(name, family::K_GAUGE);
  if (f.kind != family::K_GAUGE) return;
  const std::string key = label_key(labels);
  f.values[key] += delta;
  if (f.labels.find(key) == f.labels.end()) {
    uvcpp_metric_labels sorted = labels;
    std::sort(sorted.begin(), sorted.end(),
              [](const uvcpp_metric_label& a, const uvcpp_metric_label& b) {
                return a.name < b.name;
              });
    f.labels[key] = sorted;
  }
}

void uvcpp_metrics_registry::register_gauge(const std::string& name,
                                            const std::string& help,
                                            std::function<double()> fn) {
  std::lock_guard<std::mutex> lk(mu_);
  family&                     f = family_for(name, family::K_GAUGE);
  if (f.kind != family::K_GAUGE) return;
  if (!help.empty()) f.help = help;
  if (fn) f.gauges.push_back(std::make_pair(name, std::move(fn)));
}

void uvcpp_metrics_registry::observe(const std::string& name, double value,
                                     const uvcpp_metric_labels& labels) {
  std::lock_guard<std::mutex> lk(mu_);
  family&                     f = family_for(name, family::K_HISTOGRAM);
  if (f.kind != family::K_HISTOGRAM) return;
  const std::string key = label_key(labels);
  histogram&        h   = f.hists[key];
  if (h.counts.size() != f.buckets.size()) h.counts.assign(f.buckets.size(), 0);
  for (size_t i = 0; i < f.buckets.size(); ++i) {
    if (value <= f.buckets[i]) ++h.counts[i];
  }
  ++h.count;
  h.sum += value;
  if (f.labels.find(key) == f.labels.end()) {
    uvcpp_metric_labels sorted = labels;
    std::sort(sorted.begin(), sorted.end(),
              [](const uvcpp_metric_label& a, const uvcpp_metric_label& b) {
                return a.name < b.name;
              });
    f.labels[key] = sorted;
  }
}

void uvcpp_metrics_registry::configure_histogram(
    const std::string& name, const std::string& help,
    const std::vector<double>& buckets) {
  std::lock_guard<std::mutex> lk(mu_);
  family&                     f = family_for(name, family::K_HISTOGRAM);
  if (f.kind != family::K_HISTOGRAM) return;
  if (!help.empty()) f.help = help;
  std::vector<double> b = buckets;
  std::sort(b.begin(), b.end());
  b.erase(std::unique(b.begin(), b.end()), b.end());
  if (b.empty()) return;
  // 换桶之前已经记过的观测按新桶**重放不了**（只存了累计数）—— 调用方应在
  // 第一次 observe 之前配置。这里把旧计数清零，避免"新桶 + 旧计数"的错配。
  f.buckets = b;
  f.hists.clear();
}

void uvcpp_metrics_registry::set_help(const std::string& name,
                                      const std::string& help) {
  std::lock_guard<std::mutex> lk(mu_);
  std::map<std::string, family>::iterator it = families_.find(name);
  if (it == families_.end()) {
    family f;
    f.help = help;
    families_.insert(std::make_pair(name, std::move(f)));
    return;
  }
  it->second.help = help;
}

void uvcpp_metrics_registry::clear() {
  std::lock_guard<std::mutex> lk(mu_);
  families_.clear();
}

std::string uvcpp_metrics_registry::render() const {
  // 先在锁里拷一份快照，锁外跑回调与格式化：`register_gauge()` 的回调通常读
  // 别的对象（连接池、线程池），在锁里跑会把它们的锁与本注册表的锁叠起来。
  std::map<std::string, family> snapshot;
  {
    std::lock_guard<std::mutex> lk(mu_);
    snapshot = families_;
  }

  std::string out;
  for (std::map<std::string, family>::iterator it = snapshot.begin();
       it != snapshot.end(); ++it) {
    const std::string& name  = it->first;
    family&            f     = it->second;
    if (!f.help.empty()) {
      out += "# HELP ";
      out += name;
      out += ' ';
      out += f.help;
      out += '\n';
    }
    switch (f.kind) {
      case family::K_COUNTER:
        out += "# TYPE " + name + " counter\n";
        break;
      case family::K_GAUGE:
        out += "# TYPE " + name + " gauge\n";
        break;
      case family::K_HISTOGRAM:
        out += "# TYPE " + name + " histogram\n";
        break;
    }

    for (std::map<std::string, double>::iterator v = f.values.begin();
         v != f.values.end(); ++v) {
      out += name;
      const std::map<std::string, uvcpp_metric_labels>::iterator l =
          f.labels.find(v->first);
      if (l != f.labels.end() && !l->second.empty()) {
        out += render_labels(l->second);
      }
      out += ' ';
      out += fmt_double(v->second);
      out += '\n';
    }

    for (size_t i = 0; i < f.gauges.size(); ++i) {
      const double val = f.gauges[i].second ? f.gauges[i].second() : 0.0;
      out += f.gauges[i].first;
      out += ' ';
      out += fmt_double(val);
      out += '\n';
    }

    if (f.kind == family::K_HISTOGRAM) {
      for (std::map<std::string, histogram>::iterator h = f.hists.begin();
           h != f.hists.end(); ++h) {
        const uvcpp_metric_labels* labels = nullptr;
        const std::map<std::string, uvcpp_metric_labels>::iterator l =
            f.labels.find(h->first);
        if (l != f.labels.end()) labels = &l->second;
        for (size_t i = 0; i < f.buckets.size(); ++i) {
          out += name;
          out += "_bucket";
          uvcpp_metric_labels with_le;
          if (labels != nullptr) with_le = *labels;
          uvcpp_metric_label le;
          le.name  = "le";
          le.value = fmt_double(f.buckets[i]);
          with_le.push_back(le);
          out += render_labels(with_le);
          out += ' ';
          out += fmt_double(static_cast<double>(h->second.counts[i]));
          out += '\n';
        }
        {
          out += name;
          out += "_bucket";
          uvcpp_metric_labels with_le;
          if (labels != nullptr) with_le = *labels;
          uvcpp_metric_label le;
          le.name  = "le";
          le.value = "+Inf";
          with_le.push_back(le);
          out += render_labels(with_le);
          out += ' ';
          out += fmt_double(static_cast<double>(h->second.count));
          out += '\n';
        }
        {
          out += name;
          out += "_sum";
          if (labels != nullptr && !labels->empty()) {
            out += render_labels(*labels);
          }
          out += ' ';
          out += fmt_double(h->second.sum);
          out += '\n';
        }
        {
          out += name;
          out += "_count";
          if (labels != nullptr && !labels->empty()) {
            out += render_labels(*labels);
          }
          out += ' ';
          out += fmt_double(static_cast<double>(h->second.count));
          out += '\n';
        }
      }
    }
  }
  return out;
}

}  // namespace uvcpp
