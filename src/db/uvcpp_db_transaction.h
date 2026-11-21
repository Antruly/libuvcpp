/**
 * @file src/db/uvcpp_db_transaction.h
 * @brief 异步事务：一条连接从头用到尾，回滚与归还由池子兜底。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * #include <db/uvcpp_db_pool.h>
 *
 * uvcpp::uvcpp_db_pool pool;
 * pool.init("mysql://root@127.0.0.1:3306/app", 2, 8);
 *
 * pool.transaction(
 *     &loop,
 *     [](uvcpp::uvcpp_db_tx& tx) {
 *       tx.execute("UPDATE account SET balance = balance - ? WHERE id = ?",
 *                  {100, 1});
 *       tx.execute("UPDATE account SET balance = balance + ? WHERE id = ?",
 *                  {100, 2});
 *     },
 *     [](uvcpp::uvcpp_db_status st, const std::string& err) {
 *       // 循环线程上恰好一次
 *     },
 *     uvcpp::uvcpp_db_tx_options opts;   // C++11：带默认成员的聚合不能花括号初始化
 *     opts.timeout_ms = 5000;
 *     pool.transaction(&loop, body, done, opts);
 * ```
 *
 * ## 它替你守住的四件事
 *
 * 1. **事务体在 worker 线程上跑**，不占事件循环；
 * 2. **多条语句固定在同一条连接上** —— 借到的那条在 `commit`/`rollback` 之前
 *    不还，`tx.client()` 自始至终是同一个指针；
 * 3. **正常结束自动 commit，出错/抛异常/超时自动 rollback**，连接一定归还
 *    （借不出来、`begin` 失败、体抛异常、提交失败这几条路都算）；
 * 4. **完成回调只在提交时那条循环的线程上调用一次**；投递失败看返回值，回调
 *    不会来（与 `uvcpp_db_pool` 其余异步接口同一条约定）。
 *
 * ## 超时说清楚
 *
 * `timeout_ms` 是**整个事务的总预算**，在**每条语句之前**检查。阻塞驱动没有
 * "从外部打断一条正在跑的语句"的通用办法，所以单条语句最长能跑多久由连接
 * 自己的语句超时决定（池子的 `set_timeout_ms()`，在建连接时生效）。
 * 超时之后这个事务**不再执行任何语句**，并回滚、归还连接。
 *
 * ## 它占一条线程池线程
 *
 * 事务体是阻塞的（驱动的 API 就是阻塞的），所以整个事务期间占着 libuv 默认
 * 线程池的一条线程。池子默认 4 条，`max=1` 的事务队列里同时只有一条真在跑、
 * 其余的在 `acquire()` 上等 —— 与 `uvcpp_db_async` 同一个已知代价。要跑很多
 * 长事务就调 `uvcpp_set_threadpool_size()`（必须在任何投递之前调）。
 *
 * ## 不重试
 *
 * 事务内**不自动重试**：一次失败（约束冲突、死锁、超时）就是一次失败，由
 * 调用方决定要不要重投一个新事务。自动重试会把"这条 SQL 到底跑了几次"变成
 * 一个调用方看不见的变量。
 *
 * ## 与同步 `begin()/commit()` 的关系
 *
 * 池子的 `acquire()` + `client->begin()` 那条同步路**没有变**：它还是给工作
 * 线程 / 不在循环上的调用方用的。本类是它的一层异步包装，不改变任何既有语义。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_TRANSACTION_H
#define SRC_DB_UVCPP_DB_TRANSACTION_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_DB_ENABLE

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include <db/uvcpp_db_params.h>
#include <db/uvcpp_db_status.h>
#include <db/uvcpp_db_table.h>
#include <db/uvcpp_db_value.h>

namespace uvcpp {

class uvcpp_db_client;

/**
 * @brief 一次异步事务的旋钮（`uvcpp_db_pool::transaction()` 的最后一个实参）。
 */
struct UVCPP_API uvcpp_db_tx_options {
  /**
   * @brief 整个事务的总预算（毫秒）。**0 = 不限**。默认 5000。
   *
   * 在每条语句**之前**检查；超时后不再执行语句并回滚。单条语句自己的上限仍
   * 由连接级语句超时（`uvcpp_db_pool::set_timeout_ms()`）兜着。
   */
  int timeout_ms = 5000;

  /**
   * @brief 借连接的超时（毫秒）。**`< 0` = 用池子的默认值**（默认 5000）。
   *
   * 与 `timeout_ms` 是两段预算：借不到连接时直接 `NO_CONNECTION`，事务体
   * 一次都不执行。
   */
  int acquire_timeout_ms = -1;
};

/**
 * @brief `uvcpp_db_pool::transaction()` 的累计观测值（无锁快照）。
 */
struct UVCPP_API uvcpp_db_tx_stats {
  /// 投出去的事务总数（含借连接失败、begin 失败的那些）。
  uint64_t total = 0;
  /// 成功提交的数。
  uint64_t committed = 0;
  /// 回滚收场的数（显式回滚、语句失败、体抛异常、超时都算）。
  uint64_t rolled_back = 0;
  /// 以错误收场的数（与 `rolled_back` 有交集：回滚通常也意味着失败）。
  uint64_t failed = 0;
  /// 其中因 `timeout_ms` 命中而回滚的数。
  uint64_t timed_out = 0;
  /// 借连接超时的数（此时事务体一次都没跑）。
  uint64_t acquire_timeouts = 0;
  /// 累计耗时（微秒，从借连接开始到连接归还）。
  uint64_t total_us = 0;
  /// 单次最长耗时（微秒）。
  uint64_t max_us = 0;
};

/**
 * @brief 事务体拿到的句柄。**只在事务体执行期间有效**。
 *
 * 事务体跑在 worker 线程上；事务结束后这个对象就被销毁了 —— 别把指针或引用
 * 存下来。第一条失败的语句会被记住：之后的 `query/execute/insert` **不再
 * 发出去**，直接返回同一个错误（这样"后面还有三条语句"不会继续糟蹋连接）。
 */
class UVCPP_API uvcpp_db_tx {
 public:
  /**
   * @brief 由 `uvcpp_db_pool::transaction()` 内部构造。业务代码**不要自己造**。
   *
   * 直接构造出来的对象没有可用的连接，任何语句都会返回 `MISUSE`。
   */
  uvcpp_db_tx(uvcpp_db_client* client, int timeout_ms);

  uvcpp_db_tx(const uvcpp_db_tx&) = delete;
  uvcpp_db_tx& operator=(const uvcpp_db_tx&) = delete;

  // ---- 语句 ----

  uvcpp_db_status query(const std::string& sql, uvcpp_db_table* out);
  uvcpp_db_status query(const std::string& sql, const uvcpp_db_params& params,
                        uvcpp_db_table* out);

  uvcpp_db_status execute(const std::string& sql,
                          int64_t* affected = nullptr);
  uvcpp_db_status execute(const std::string& sql,
                          const uvcpp_db_params& params,
                          int64_t* affected = nullptr);

  uvcpp_db_status insert(const std::string& sql, const uvcpp_db_params& params,
                         uvcpp_db_value* new_id);

  // ---- 收尾 ----

  /**
   * @brief 显式提交。不调就由框架在事务体正常返回后自动提交。
   *
   * 调用之后再发语句是 `MISUSE`。提交失败时框架仍会尝试一次 `rollback`。
   */
  uvcpp_db_status commit();

  /**
   * @brief 显式回滚（业务主动放弃时用）。之后不再自动提交。
   *
   * 返回值是 `rollback` 本身的结果；事务里**之前**那条失败语句的状态仍可从
   * `status()` / `error()` 里读到。
   */
  uvcpp_db_status rollback();

  // ---- 观测 ----

  /// 这次事务用的那条连接（事务体内有效）。多条语句共用它 —— 这正是"连接亲和"。
  uvcpp_db_client* client() const { return client_; }

  /// 第一条失败语句的状态；到目前为止没失败过是 `OK`。
  uvcpp_db_status status() const { return first_error_; }

  /// 失败原因（`last_error()` 在失败那一刻的快照）；成功时是空串。
  const std::string& error() const { return error_; }

  bool committed() const { return committed_; }
  bool rolled_back() const { return rolled_back_; }

  /// 从事务开始到**现在**的毫秒数。
  int64_t elapsed_ms() const;

 private:
  /// 每条语句之前过一遍：已收尾 / 已出错 / 已超时都不再发。
  uvcpp_db_status gate();
  /// 记第一笔错误（以及 `NOT_CONNECTED` 的标记，池子据此 discard）。
  void record(uvcpp_db_status st);

  uvcpp_db_client* client_ = nullptr;
  int              timeout_ms_ = 0;
  std::chrono::steady_clock::time_point start_;
  std::chrono::steady_clock::time_point deadline_;
  bool                                  has_deadline_ = false;

  uvcpp_db_status first_error_ = uvcpp_db_status::OK;
  std::string     error_;
  bool            finished_ = false;
  bool            committed_ = false;
  bool            rolled_back_ = false;
};

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
#endif  // SRC_DB_UVCPP_DB_TRANSACTION_H
