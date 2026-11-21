/**
 * @file src/uvcpp/uvcpp_cron.h
 * @brief 定时任务：cron 表达式解析 + 挂在 `uvcpp_loop` 上的调度器。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * uvcpp::uvcpp_cron cron(loop);
 * cron.add("0 3 * * *", [] { backup(); });        // 每天 03:00（UTC）
 * cron.add("* * * * * *", [] { tick(); });       // 6 段：每秒（测试/心跳用）
 * cron.start();
 * ```
 *
 * ## 时间基准是 **UTC**
 *
 * 所有匹配与下次触发都按 UTC（`gmtime_r`）。理由：本库跨平台（Windows 没有
 * `tzset` 语义一致的环境变量路径），"本地时区"在容器/CI 里也常常不是部署者
 * 以为的那个；要按本地时间跑，调用方在自己的回调里换算，或表达式按 UTC 写。
 *
 * ## 支持哪些表达式
 *
 * - **5 段**（标准 Vixie cron）：`分 时 日 月 周`；
 * - **6 段**（Quartz 风格扩展）：`秒 分 时 日 月 周`；
 * - 每段支持 `*`、`n`、`a-b`、`a-b/n`、``*`` 配步长（星号斜杠 n）、逗号列表；
 * - 周：`0`/`7` 都是周日；月：`1-12`；日：`1-31`；
 * - **日与周同时受限时按"或"**（Vixie cron 的语义：`0 0 13 * 5` 是"13 号
 *   或周五"，不是"13 号的周五"）；
 * - 不支持名字（`JAN`/`MON`）、`@daily` 这类宏、秒级之外的 L/W/# 扩展 ——
 *   遇到就是解析失败，不猜。
 *
 * ## 调度语义
 *
 * - **错过不补**：回调执行的耗时超过间隔时，下一次从"现在"重新算，不会把
 *   错过的每一拍都补跑（`stats().skipped` 记这种情况）。
 * - 回调抛异常会被吞掉并计 `errors`，不影响后续触发。
 * - 只在构造时给的循环线程上跑；`add()` / `start()` 请在 `loop.run()` 之前或
 *   用 `loop.post()` 投回来。
 * - 每个任务一个 `uvcpp_timer`；任务多（上千）时请合并成更少的定时器再用
 *   表达式的时间字段自己分派 —— 本类没有做定时器堆。
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_CRON_H
#define SRC_UVCPP_UVCPP_CRON_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace uvcpp {

class uvcpp_loop;
class uvcpp_timer;

/**
 * @brief 解析好的 cron 表达式。可用作纯函数（`next_utc()`）不接受调度器。
 */
class UVCPP_API uvcpp_cron_schedule {
 public:
  uvcpp_cron_schedule();

  /**
   * @brief 解析一个表达式（5 段或 6 段）。
   *
   * @param expr 表达式（首尾空白忽略，段间任意空白）。
   * @param out  成功时写入解析结果。
   * @param err  失败时写入人读原因（可为 nullptr）。
   * @return 是否成功。
   */
  static bool parse(const std::string& expr, uvcpp_cron_schedule& out,
                    std::string* err = nullptr);

  bool valid() const { return valid_; }
  /// 是否 6 段（带秒）。
  bool has_seconds() const { return has_seconds_; }
  const std::string& expression() const { return expr_; }

  /// `t`（UTC）是否命中。无效表达式恒 false。
  bool matches_utc(std::time_t t) const;

  /**
   * @brief 严格晚于 @p after 的下一次触发（UTC）。
   *
   * @param next 成功时写入下一次触发时间。
   * @return false = 无效表达式，或搜索上限（5 年）内没有匹配
   *         （例如 `0 0 30 2 *` 这种永远不存在的日期）。
   */
  bool next_utc(std::time_t after, std::time_t& next) const;

 private:
  friend class uvcpp_cron;
  bool bit(const std::vector<uint64_t>& f, int v) const;

  std::vector<uint64_t> seconds_;  ///< 60 位；5 段表达式恒全 1（每秒都可）
  std::vector<uint64_t> minutes_;  ///< 60 位
  std::vector<uint64_t> hours_;    ///< 24 位
  std::vector<uint64_t> days_;     ///< 32 位（1..31 用）
  std::vector<uint64_t> months_;   ///< 13 位
  std::vector<uint64_t> weekdays_; ///< 8 位（0..7，7 归一成 0）
  bool dom_star_ = true;
  bool dow_star_ = true;
  bool has_seconds_ = false;
  bool valid_ = false;
  std::string expr_;
};

struct UVCPP_API uvcpp_cron_stats {
  uint64_t runs = 0;     ///< 成功执行的回调次数
  uint64_t errors = 0;   ///< 回调抛异常的次数
  uint64_t skipped = 0;  ///< 因回调耗时超过间隔而跳过的触发次数
  size_t   jobs = 0;     ///< 当前任务数
};

/**
 * @brief 挂在一条 `uvcpp_loop` 上的 cron 调度器。见文件头。
 */
class UVCPP_API uvcpp_cron {
 public:
  explicit uvcpp_cron(uvcpp_loop* loop);
  ~uvcpp_cron();

  uvcpp_cron(const uvcpp_cron&) = delete;
  uvcpp_cron& operator=(const uvcpp_cron&) = delete;

  /**
   * @brief 加一个任务。解析失败返回 `UV_EINVAL` 并写 @p err。
   *
   * 调度器已经 `start()` 时，新任务当场排上；没 `start()` 时等 `start()`。
   */
  int add(const std::string& expr, std::function<void()> cb,
          std::string* err = nullptr);

  /// 开始触发（幂等）。
  void start();

  /// 暂停全部触发（幂等）；任务保留，`start()` 可再开。
  void stop();

  /// 摘掉全部任务（停掉它们的定时器）。
  void clear();

  size_t size() const;

  uvcpp_cron_stats stats() const;

 private:
  struct job;
  struct impl;
  // impl 用裸指针：`uvcpp_timer` 的句柄必须在循环线程上析构，而析构顺序由
  // 用户在 `loop.run()` 之后控制；这里只按任务生命周期管理。
  impl* impl_;
};

}  // namespace uvcpp

#endif  // SRC_UVCPP_UVCPP_CRON_H
