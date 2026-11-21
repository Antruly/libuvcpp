/**
 * @file src/uvcpp/uvcpp_cron.cpp
 * @brief cron 解析与调度器的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "uvcpp/uvcpp_cron.h"

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace uvcpp {

namespace {

typedef std::vector<uint64_t> bitfield;

bitfield make_bits(int hi) { return bitfield((hi / 64) + 1, 0); }
void     set_bit(bitfield& f, int v) { f[v / 64] |= (uint64_t(1) << (v % 64)); }
bool     get_bit(const bitfield& f, int v) {
  return (f[v / 64] & (uint64_t(1) << (v % 64))) != 0;
}

bool all_digits(const std::string& s) {
  if (s.empty()) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

bool to_int(const std::string& s, int& out) {
  if (!all_digits(s)) return false;
  out = static_cast<int>(std::strtol(s.c_str(), nullptr, 10));
  return true;
}

bool parse_field(const std::string& spec, int lo, int hi, bitfield& out,
                 bool dow_norm, std::string* err) {
  out = make_bits(hi);
  std::istringstream items(spec);
  std::string        item;
  bool               any = false;
  while (std::getline(items, item, ',')) {
    if (item.empty()) {
      if (err) *err = "空的列表项";
      return false;
    }
    std::string range = item;
    int         step  = 1;
    const size_t slash = item.find('/');
    if (slash != std::string::npos) {
      range = item.substr(0, slash);
      const std::string ss = item.substr(slash + 1);
      if (!to_int(ss, step) || step <= 0) {
        if (err) *err = "步长不是正整数：'" + ss + "'";
        return false;
      }
    }
    int a = 0, b = 0;
    if (range == "*") {
      a = lo;
      b = hi;
    } else {
      const size_t dash = range.find('-');
      if (dash == std::string::npos) {
        if (!to_int(range, a)) {
          if (err) *err = "不是数字：'" + range + "'";
          return false;
        }
        b = a;
        // `a/n` 是常见写法（从 a 起到上界按步长）；Vixie 只认 `a-b/n`，
        // 这里接受前者，语义按"a..hi"。
        if (slash != std::string::npos) b = hi;
      } else {
        if (!to_int(range.substr(0, dash), a) ||
            !to_int(range.substr(dash + 1), b)) {
          if (err) *err = "区间两端不是数字：'" + range + "'";
          return false;
        }
      }
    }
    if (a < lo || b > hi || a > b) {
      if (err) {
        *err = "越界或倒序：" + std::string("[") + std::to_string(lo) + "," +
               std::to_string(hi) + "] 内的 '" + item + "'";
      }
      return false;
    }
    for (int v = a; v <= b; v += step) {
      set_bit(out, dow_norm && v == 7 ? 0 : v);
      any = true;
    }
  }
  return any;
}

std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream       in(s);
  std::string              w;
  while (in >> w) out.push_back(w);
  return out;
}

void gmtime_utc(std::time_t t, struct tm& tmv) {
#ifdef _WIN32
  ::gmtime_s(&tmv, &t);
#else
  ::gmtime_r(&t, &tmv);
#endif
}

}  // namespace

uvcpp_cron_schedule::uvcpp_cron_schedule() {
  seconds_  = make_bits(59);
  minutes_  = make_bits(59);
  hours_    = make_bits(23);
  days_     = make_bits(31);
  months_   = make_bits(12);
  weekdays_ = make_bits(7);
}

bool uvcpp_cron_schedule::parse(const std::string& expr,
                                uvcpp_cron_schedule& out,
                                std::string* err) {
  const std::vector<std::string> f = split_ws(expr);
  if (f.size() != 5 && f.size() != 6) {
    if (err) *err = "需要 5 段或 6 段，实际 " + std::to_string(f.size());
    return false;
  }

  uvcpp_cron_schedule r;
  r.expr_ = expr;
  size_t i = 0;
  if (f.size() == 6) {
    r.has_seconds_ = true;
    if (!parse_field(f[i++], 0, 59, r.seconds_, false, err)) return false;
  } else {
    r.seconds_ = make_bits(59);
    for (int s = 0; s < 60; ++s) set_bit(r.seconds_, s);
  }
  if (!parse_field(f[i++], 0, 59, r.minutes_, false, err)) return false;
  if (!parse_field(f[i++], 0, 23, r.hours_, false, err)) return false;
  if (!parse_field(f[i++], 1, 31, r.days_, false, err)) return false;
  if (!parse_field(f[i++], 1, 12, r.months_, false, err)) return false;
  if (!parse_field(f[i++], 0, 7, r.weekdays_, true, err)) return false;
  // 原始字段串判"是不是 `*`"：日/周同时受限时 Vixie 语义才用"或"。
  r.dom_star_ = (f.size() == 6 ? f[3] : f[2]) == "*";
  r.dow_star_ = (f.size() == 6 ? f[5] : f[4]) == "*";
  r.valid_    = true;
  out         = r;
  return true;
}

bool uvcpp_cron_schedule::bit(const std::vector<uint64_t>& f, int v) const {
  return get_bit(f, v);
}

bool uvcpp_cron_schedule::matches_utc(std::time_t t) const {
  if (!valid_) return false;
  struct tm tmv;
  gmtime_utc(t, tmv);
  if (!bit(seconds_, tmv.tm_sec)) return false;
  if (!bit(minutes_, tmv.tm_min)) return false;
  if (!bit(hours_, tmv.tm_hour)) return false;
  if (!bit(months_, tmv.tm_mon + 1)) return false;
  const bool dom = bit(days_, tmv.tm_mday);
  const bool dow = bit(weekdays_, tmv.tm_wday);
  if (dom_star_ && dow_star_) return true;
  if (dom_star_) return dow;
  if (dow_star_) return dom;
  return dom || dow;  // Vixie：两者都受限时取"或"
}

bool uvcpp_cron_schedule::next_utc(std::time_t after,
                                   std::time_t& next) const {
  if (!valid_) return false;
  const long long step = has_seconds_ ? 1 : 60;
  // 5 年上限：2 月 29/30 这种"永远不成立"的表达式要能返回 false 而不是死循环。
  const long long cap = step == 1 ? 366LL * 24 * 3600 * 2 : 5LL * 366 * 24 * 60;
  long long       t   = static_cast<long long>(after) + step;
  for (long long k = 0; k < cap; ++k, t += step) {
    if (matches_utc(static_cast<std::time_t>(t))) {
      next = static_cast<std::time_t>(t);
      return true;
    }
  }
  return false;
}

// ===========================================================================
// 调度器
// ===========================================================================

struct uvcpp_cron::job {
  uvcpp_cron_schedule  schedule;
  std::function<void()> cb;
  uvcpp_timer*          timer = nullptr;
  bool                  armed = false;
};

struct uvcpp_cron::impl {
  uvcpp_loop*         loop = nullptr;
  std::vector<job*>   jobs;
  bool                running = false;
  uvcpp_cron_stats    s;

  void arm(job* j) {
    std::time_t next = 0;
    if (!j->schedule.next_utc(std::time(nullptr), next)) {
      j->armed = false;
      return;
    }
    const std::time_t now = std::time(nullptr);
    uint64_t          ms  = next > now
                                ? static_cast<uint64_t>(next - now) * 1000
                                : 1;
    j->timer->start(
        [this, j](uvcpp_timer*) { fire(j); }, ms, 0);
    j->armed = true;
  }

  void fire(job* j) {
    const std::time_t fired_at = std::time(nullptr);
    try {
      if (j->cb) j->cb();
      ++s.runs;
    } catch (...) {
      ++s.errors;
    }
    // 错过不补：从"现在"重新算；若回调耗时跨过了本应触发的下一拍，
    // next_utc 会自然给出更晚的一拍，这里只把 skipped 记下来（可观测性）。
    std::time_t next = 0;
    if (!j->schedule.next_utc(fired_at, next)) {
      j->armed = false;
      return;
    }
    const std::time_t now = std::time(nullptr);
    if (next <= now) {
      // 已经错过的触发不补；从 now 再求一次。
      if (!j->schedule.next_utc(now, next)) {
        j->armed = false;
        return;
      }
      ++s.skipped;
    }
    uint64_t ms = next > now ? static_cast<uint64_t>(next - now) * 1000 : 1;
    j->timer->start([this, j](uvcpp_timer*) { fire(j); }, ms, 0);
    j->armed = true;
  }
};

uvcpp_cron::uvcpp_cron(uvcpp_loop* loop) : impl_(new impl()) {
  impl_->loop = loop;
  if (loop == nullptr) {
    // 无循环 = 只能当解析器用；add() 一律返回 UV_EINVAL。
    return;
  }
}

uvcpp_cron::~uvcpp_cron() {
  clear();
  delete impl_;
}

int uvcpp_cron::add(const std::string& expr, std::function<void()> cb,
                    std::string* err) {
  if (impl_->loop == nullptr) {
    if (err) *err = "调度器没有循环";
    return UV_EINVAL;
  }
  uvcpp_cron_schedule schedule;
  if (!uvcpp_cron_schedule::parse(expr, schedule, err)) return UV_EINVAL;
  if (!cb) {
    if (err) *err = "回调为空";
    return UV_EINVAL;
  }
  job* j       = new job();
  j->schedule  = schedule;
  j->cb        = std::move(cb);
  j->timer     = new uvcpp_timer(impl_->loop);
  impl_->jobs.push_back(j);
  impl_->s.jobs = impl_->jobs.size();
  if (impl_->running) {
    impl_->arm(j);
  }
  return 0;
}

void uvcpp_cron::start() {
  if (impl_->loop == nullptr || impl_->running) return;
  impl_->running = true;
  for (size_t i = 0; i < impl_->jobs.size(); ++i) impl_->arm(impl_->jobs[i]);
}

void uvcpp_cron::stop() {
  impl_->running = false;
  for (size_t i = 0; i < impl_->jobs.size(); ++i) {
    if (impl_->jobs[i]->timer != nullptr) {
      impl_->jobs[i]->timer->stop();
      impl_->jobs[i]->armed = false;
    }
  }
}

void uvcpp_cron::clear() {
  for (size_t i = 0; i < impl_->jobs.size(); ++i) {
    job* j = impl_->jobs[i];
    if (j->timer != nullptr) {
      j->timer->stop();
      delete j->timer;
    }
    delete j;
  }
  impl_->jobs.clear();
  impl_->s.jobs = 0;
}

size_t uvcpp_cron::size() const { return impl_->jobs.size(); }

uvcpp_cron_stats uvcpp_cron::stats() const {
  uvcpp_cron_stats out = impl_->s;
  out.jobs             = impl_->jobs.size();
  return out;
}

}  // namespace uvcpp
