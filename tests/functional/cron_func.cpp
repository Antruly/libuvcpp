/**
 * @file tests/functional/cron_func.cpp
 * @brief cron 表达式解析、UTC 匹配、下次触发时间，以及真循环上的调度。
 *
 * 三层判据：
 *
 * 1. **解析**：合法/非法表达式（越界、倒序、步长 0、段数不对）；
 * 2. **纯函数**：`matches_utc()` 与 `next_utc()` —— 用固定 UTC 时间点比对，
 *    不依赖本机时区；日与周同时受限时按 Vixie 的"或"语义；
 * 3. **调度**：一条真 `uvcpp_loop` 上跑 6 段表达式（每秒），确认回调真的被
 *    触发、抛异常的任务被计数且不影响别的任务。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_cron.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>

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

/// 构造一个 UTC 的 `time_t`（`timegm` 在 Windows 上叫 `_mkgmtime`）。
std::time_t utc(int y, int mon, int d, int h, int mi, int s) {
  struct tm tmv;
  std::memset(&tmv, 0, sizeof(tmv));
  tmv.tm_year = y - 1900;
  tmv.tm_mon  = mon - 1;
  tmv.tm_mday = d;
  tmv.tm_hour = h;
  tmv.tm_min  = mi;
  tmv.tm_sec  = s;
#ifdef _WIN32
  return ::_mkgmtime(&tmv);
#else
  return ::timegm(&tmv);
#endif
}

void test_parse() {
  std::cout << "-- 解析 --" << std::endl;
  uvcpp_cron_schedule sc;
  std::string         err;
  check(uvcpp_cron_schedule::parse("* * * * *", sc, &err), "5 段 * 合法");
  check(!sc.has_seconds(), "5 段没有秒字段");
  check(uvcpp_cron_schedule::parse("*/15 0-6 * * 1-5", sc, &err),
        "带步长与区间的 5 段合法");
  check(uvcpp_cron_schedule::parse("* * * * * *", sc, &err), "6 段合法");
  check(sc.has_seconds(), "6 段有秒字段");

  const char* bad[] = {"", "* * * *", "* * * * * * *", "60 * * * *",
                       "0 24 * * *", "0 0 0 * *", "0 0 * 13 *",
                       "0 0 * * 8", "5-1 * * * *", "*/0 * * * *",
                       "a * * * *", "1,,2 * * * *"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    std::string e;
    check(!uvcpp_cron_schedule::parse(bad[i], sc, &e),
          std::string("拒绝非法表达式 '") + bad[i] + "'（" + e + "）");
  }
}

void test_match_and_next() {
  std::cout << "-- 匹配 / 下次触发（UTC）--" << std::endl;
  uvcpp_cron_schedule sc;

  // 2026-01-05 是周一（周四 1 号 → 5 号周一）。
  const std::time_t mon1030 = utc(2026, 1, 5, 10, 30, 0);
  uvcpp_cron_schedule::parse("30 10 * * 1", sc);
  check(sc.matches_utc(mon1030), "周一 10:30 命中 '30 10 * * 1'");
  check(!sc.matches_utc(mon1030 + 60), "下一分钟不命中");

  // 日/周同时受限：13 号 *或* 周五。
  uvcpp_cron_schedule::parse("0 0 13 * 5", sc);
  const std::time_t tue13 = utc(2026, 1, 13, 0, 0, 0);   // 周二，13 号
  const std::time_t fri16 = utc(2026, 1, 16, 0, 0, 0);   // 周五，16 号
  const std::time_t wed14 = utc(2026, 1, 14, 0, 0, 0);
  check(sc.matches_utc(tue13), "13 号（周二）命中（日的分支）");
  check(sc.matches_utc(fri16), "周五（16 号）命中（周的分支）");
  check(!sc.matches_utc(wed14), "周三 14 号不命中（不是'13 号的周五'）");

  // next_utc：从 10:07 起，*/15 的下一拍是 10:15。
  uvcpp_cron_schedule::parse("*/15 * * * *", sc);
  std::time_t next = 0;
  check(sc.next_utc(utc(2026, 1, 5, 10, 7, 0), next) &&
            next == utc(2026, 1, 5, 10, 15, 0),
        "*/15：10:07 → 10:15");

  // 跨月：每月 1 号 0 点。
  uvcpp_cron_schedule::parse("0 0 1 * *", sc);
  check(sc.next_utc(utc(2026, 1, 5, 10, 7, 0), next) &&
            next == utc(2026, 2, 1, 0, 0, 0),
        "每月 1 号 0 点：1/5 → 2/1");

  // 6 段：严格晚一秒。
  uvcpp_cron_schedule::parse("* * * * * *", sc);
  const std::time_t t = utc(2026, 1, 5, 10, 7, 30);
  check(sc.next_utc(t, next) && next == t + 1, "6 段：下一秒");

  // 永远不存在的日期：搜索上限内返回 false（2 月 30 号）。
  uvcpp_cron_schedule::parse("0 0 30 2 *", sc);
  check(!sc.next_utc(utc(2026, 1, 1, 0, 0, 0), next),
        "2 月 30 号：上限内无解 → false");
}

void test_scheduler() {
  std::cout << "-- 调度（真循环）--" << std::endl;
  uvcpp_loop  loop;
  uvcpp_timer stopper(&loop);
  // 3 秒后停循环（在循环线程上拨 stop，避免跨线程调 uv_stop）。
  stopper.start([&loop](uvcpp_timer*) { loop.stop(); }, 3000, 0);

  uvcpp_cron        cron(&loop);
  std::atomic<int>  ticks(0);
  std::atomic<int>  throws(0);
  std::string       err;
  check(cron.add("* * * * * *", [&ticks] { ++ticks; }, &err) == 0,
        "加任务成功（" + err + "）");
  check(cron.add("* * * * * *",
                 [&throws] {
                   ++throws;
                   throw std::runtime_error("boom");
                 },
                 &err) == 0,
        "加抛异常的任务成功");
  check(cron.size() == 2, "任务数 = 2");
  cron.start();

  std::thread th([&loop] { loop.run(); });
  th.join();
  cron.stop();

  const uvcpp_cron_stats s = cron.stats();
  check(ticks.load() >= 1, "正常任务至少跑了一拍（实际 " +
                               std::to_string(ticks.load()) + "）");
  check(s.errors >= 1 && throws.load() >= 1,
        "抛异常的任务被计数（errors=" + std::to_string(s.errors) + "）");
  check(s.runs >= 1, "stats().runs 有数");
  check(s.jobs == 2, "stats().jobs = 2");
}

}  // namespace

int main() {
  std::cout << "[cron] start" << std::endl;
  test_parse();
  test_match_and_next();
  test_scheduler();
  if (g_fail == 0) {
    std::cout << "[cron] ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << "[cron] " << g_fail << " check(s) failed" << std::endl;
  return 1;
}
