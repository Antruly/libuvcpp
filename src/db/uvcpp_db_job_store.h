/**
 * @file src/db/uvcpp_db_job_store.h
 * @brief 作业队列的数据库持久化适配器（issue #67 剩余：SQLite / MySQL / PostgreSQL）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * `uvcpp_memory_job_store` 只让"重启恢复"在**同一个 store 对象**上可测；真正
 * 跨进程持久化需要把记录落到数据库。本适配器实现 `uvcpp_job_store`：
 *
 * ```cpp
 * auto store = std::make_shared<uvcpp::uvcpp_db_job_store>(
 *     "sqlite:///var/lib/app/jobs.db");
 * std::string err;
 * if (!store->open(&err)) { fail(err); }  // 建表失败：处理它
 * uvcpp::uvcpp_job_queue queue(&loop, store);
 * ```
 *
 * 表结构固定（`table` 可换名，但列不变）：
 *
 * ```sql
 * id BIGINT PRIMARY KEY, type TEXT, payload TEXT, concurrency_key TEXT,
 * error TEXT, state INTEGER, attempt INTEGER, max_retry INTEGER,
 * timeout_ms BIGINT, backoff_ms BIGINT, not_before_ms BIGINT
 * ```
 *
 * ## 边界
 *
 * - **同步**：`save/load_all/erase` 都是阻塞调用，并且在**调用它的线程**上执行。
 *   作业队列在 loop 线程上调它们，所以本适配器适合"提交/完成事件不密集"的
 *   场景；高吞吐请继续用内存 store，或把生命周期事件批量落到你自己的系统。
 * - **一个 store 一条连接**（`uvcpp_db_client` 自带互斥，可多线程调用，但同一
 *   时刻只有一个在用连接）。多实例共享同一个库是支持的 —— 那正是它存在的理由。
 * - 占位符按驱动生成（PostgreSQL `$n`，SQLite/MySQL `?`）；表的 upsert 也按
 *   驱动选（SQLite `INSERT OR REPLACE`、MySQL `ON DUPLICATE KEY`、PostgreSQL
 *   `ON CONFLICT`）。
 * - 需要 `UVCPP_ENABLE_DB=ON`；`open()` 失败返回 false 并把原因放进 `last_error()`，
 *   不静默降级。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_JOB_STORE_H
#define SRC_DB_UVCPP_DB_JOB_STORE_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE

#include <memory>
#include <string>

#include <uvcpp/uvcpp_export.h>
#include <uvcpp/uvcpp_job_queue.h>

namespace uvcpp {

/**
 * @brief 把 `uvcpp_job_record` 存进数据库的 `uvcpp_job_store` 实现。
 *
 * 构造后要调一次 `open()`；`open()` 成功之前 `load_all/save/erase` 都返回 false。
 */
class UVCPP_API uvcpp_db_job_store : public uvcpp_job_store {
 public:
  /**
   * @param db_url `uvcpp_db_client::open()` 认的连接串（如 `sqlite:///tmp/j.db`）
   * @param table  表名；只允许 `[A-Za-z_][A-Za-z0-9_]*`（防拼 SQL 注入）
   */
  explicit uvcpp_db_job_store(const std::string& db_url,
                              const std::string& table = "uvcpp_jobs");
  ~uvcpp_db_job_store() override;

  uvcpp_db_job_store(const uvcpp_db_job_store&) = delete;
  uvcpp_db_job_store& operator=(const uvcpp_db_job_store&) = delete;

  /// 打开连接并建表（`CREATE TABLE IF NOT EXISTS`）。成功后返回 true。
  bool open(std::string* err = nullptr);

  bool is_open() const;
  const std::string& last_error() const;

  // ---- uvcpp_job_store ----
  bool load_all(std::vector<uvcpp_job_record>& out) override;
  bool save(const uvcpp_job_record& rec) override;
  bool erase(uint64_t id) override;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
#endif  // SRC_DB_UVCPP_DB_JOB_STORE_H
