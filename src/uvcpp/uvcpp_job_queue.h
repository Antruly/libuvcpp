/**
 * @file src/uvcpp/uvcpp_job_queue.h
 * @brief 持久化作业队列：同步 handler、并发键串行、重试退避、超时、取消（issue #67 第一批）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * auto store = std::make_shared<uvcpp::uvcpp_memory_job_store>();
 * uvcpp::uvcpp_job_queue queue(&loop, store);
 * queue.register_handler("media.transcode",
 *                        [](uvcpp::uvcpp_job_context& ctx) { /* 在 worker 上跑 *\/ });
 * queue.start();                         // 恢复上次未完成的任务
 *
 * uvcpp::uvcpp_job_submit req;
 * req.type = "media.transcode";
 * req.concurrency_key = "resource-42";   // 同一 key 严格串行
 * req.payload = "{}";
 * req.timeout_ms = 10 * 60 * 1000;
 * req.max_retry = 3;
 * queue.submit(req, &err);
 * ```
 *
 * ## 语义
 *
 * - **状态**：PENDING / RUNNING / SUCCESS / FAILED / CANCELLED；每次变更写 store
 *   并回调 `on_state()`（loop 线程）。
 * - **handler 在 libuv 线程池里同步执行**（`uvcpp_work`）；不要在里面阻塞循环
 *   线程的东西。异步 handler 是后续（#67 剩余）。
 * - **`concurrency_key`**：同一个非空 key 同时只允许一个 RUNNING；空 = 不限制。
 *   排队顺序按提交 id。
 * - **重试**：失败后 `attempt <= max_retry` 时回到 PENDING，`not_before =
 *   now + backoff_ms * 2^(attempt-1)`（上限 1 小时）。
 * - **超时 / 取消是协作式的**：到点/取消会把 `ctx.cancelled()` 置真；handler
 *   应在长循环里轮询它并尽早返回。**队列不会杀线程**（也杀不了）。
 * - **重启恢复**：`start()` 从 store 读回记录；上次停在 RUNNING 的按 PENDING
 *   处理（`error` 写一句恢复说明），由业务选择是否重试 —— 这是 #67 要求的
 *   "进程崩溃后的恢复语义"的最小明确版本。
 * - **队列满**：`max_queue > 0` 且 PENDING 数达到它时 `submit()` 返回 0 并写
 *   `err`（拒绝，不排队）。`rejected` 统计。
 * - **只在 loop 线程上调用**；handler 在 worker 线程，只能通过 ctx 读
 *   `cancelled()` 与作业快照。
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_JOB_QUEUE_H
#define SRC_UVCPP_UVCPP_JOB_QUEUE_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uvcpp {

class uvcpp_loop;

enum class uvcpp_job_state : int {
  PENDING   = 0,
  RUNNING   = 1,
  SUCCESS   = 2,
  FAILED    = 3,
  CANCELLED = 4,
};

/// 一条作业的完整记录（store 持久化的就是它）。
struct UVCPP_API uvcpp_job_record {
  uvcpp_job_record();

  uint64_t    id;
  std::string type;
  std::string payload;
  std::string concurrency_key;
  std::string error;
  int         state;  ///< `uvcpp_job_state`
  int         attempt;      ///< 已经执行过几次
  int         max_retry;    ///< 失败后最多再试几次
  uint64_t    timeout_ms;   ///< 0 = 不限
  uint64_t    backoff_ms;   ///< 首次退避
  int64_t     not_before_ms; ///< epoch ms；0 = 立刻
};

/// 提交参数。显式构造函数（C++11 聚合初始化资格）。
struct UVCPP_API uvcpp_job_submit {
  uvcpp_job_submit();

  std::string type;
  std::string payload;
  std::string concurrency_key;
  int         max_retry;
  uint64_t    timeout_ms;
  uint64_t    backoff_ms;
};

struct UVCPP_API uvcpp_job_stats {
  uvcpp_job_stats();
  size_t   pending;
  size_t   running;
  uint64_t success;
  uint64_t failed;
  uint64_t cancelled;
  uint64_t retried;
  uint64_t rejected;
};

/// 可插拔持久化。实现要对多线程安全（恢复/写入可能跨循环）。
class UVCPP_API uvcpp_job_store {
 public:
  virtual ~uvcpp_job_store() {}
  virtual bool load_all(std::vector<uvcpp_job_record>& out) = 0;
  virtual bool save(const uvcpp_job_record& rec) = 0;
  virtual bool erase(uint64_t id) = 0;
};

/// 进程内实现。队列已经在内存里维护全量记录，这个 store 只是让"重启恢复"
/// 在同一个 store 对象上可测；真正跨进程持久化需要 SQLite/MySQL 适配器。
class UVCPP_API uvcpp_memory_job_store : public uvcpp_job_store {
 public:
  uvcpp_memory_job_store();
  ~uvcpp_memory_job_store() override;
  bool load_all(std::vector<uvcpp_job_record>& out) override;
  bool save(const uvcpp_job_record& rec) override;
  bool erase(uint64_t id) override;
 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

/// 交给 handler 的上下文。只在 handler 调用期间有效。
class UVCPP_API uvcpp_job_context {
 public:
  uvcpp_job_context();
  ~uvcpp_job_context();
  uvcpp_job_context(const uvcpp_job_context&) = delete;
  uvcpp_job_context& operator=(const uvcpp_job_context&) = delete;

  /// 超时 / 被取消（协作式）。
  bool cancelled() const;
  const uvcpp_job_record& job() const;

 private:
  friend class uvcpp_job_queue;
  struct impl;
  impl* impl_;
};

using uvcpp_job_handler = std::function<void(uvcpp_job_context&)>;
using uvcpp_job_state_cb = std::function<void(const uvcpp_job_record&)>;

/**
 * @brief 作业队列。见文件头。
 *
 * **析构契约**：析构前必须已经 `stop()` 且 `stats().running == 0` —— 执行中的
 * 线程池回调持有本对象的 `impl`，析构时还在跑就是悬垂访问。
 */
class UVCPP_API uvcpp_job_queue {
 public:
  explicit uvcpp_job_queue(
      uvcpp_loop* loop,
      std::shared_ptr<uvcpp_job_store> store = nullptr);
  ~uvcpp_job_queue();

  uvcpp_job_queue(const uvcpp_job_queue&) = delete;
  uvcpp_job_queue& operator=(const uvcpp_job_queue&) = delete;

  /// 注册处理器。同类型重复注册 = 覆盖。没有处理器时任务失败（不吞）。
  void register_handler(const std::string& type,
                        const uvcpp_job_handler& handler);

  /// 最大同时执行的作业数（默认 4）。只在没有 RUNNING 时改动才立即生效。
  void set_max_concurrent(size_t n);
  /// PENDING 上限；0 = 不限（默认）。达到上限时 submit 拒绝。
  void set_max_queue(size_t n);

  /// 提交。成功返回 id（>0）；失败返回 0 并写 @p err。
  uint64_t submit(const uvcpp_job_submit& req, std::string* err = nullptr);

  /// 取消。PENDING 直接 CANCELLED；RUNNING 只置协作标志。未知 id 返回 -1。
  int cancel(uint64_t id);

  /// 恢复 store 里的记录并开始调度（幂等）。
  void start();
  /// 停止调度（在跑的允许跑完；不再启动新的）。
  void stop();

  /// 观察状态变化（loop 线程）。**回调里不要 submit 大批任务**：会重入调度。
  void on_state(const uvcpp_job_state_cb& cb);

  /// 查快照（loop 线程）。未知 id 返回 nullptr。
  const uvcpp_job_record* find(uint64_t id) const;

  /// 丢弃所有已完成（SUCCESS/FAILED/CANCELLED）的记录；返回丢弃数。
  size_t clear_finished();

  uvcpp_job_stats stats() const;

 private:
  struct impl;
  impl* impl_;
};

}  // namespace uvcpp

#endif  // SRC_UVCPP_UVCPP_JOB_QUEUE_H
