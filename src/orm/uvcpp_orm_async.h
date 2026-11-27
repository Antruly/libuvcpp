/**
 * @file src/orm/uvcpp_orm_async.h
 * @brief ORM 的异步门面：回调式单表 CRUD，复用 `uvcpp_db_pool` 的现有异步
 *        路径，不新开线程、不新开连接池、不重写事务。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ## 形状
 *
 * 每条操作都是「调用方线程构造 SQL → 投给池子 → 回调在循环线程上映射结果」：
 *
 * ```cpp
 * uvcpp::uvcpp_db_pool pool;
 * pool.init("sqlite:///tmp/app.db", 1, 4);
 *
 * uvcpp::uvcpp_loop loop;
 *
 * uvcpp::find_by_pk_async<user>(
 *     pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite, id,
 *     [](uvcpp::uvcpp_orm_status st, const user& value,
 *        const std::string& error) {
 *       // 循环线程上恰好一次
 *     });
 *
 * loop.run();
 * ```
 *
 * 回调只在 `loop` 的线程上被调用**恰好一次**（成功、执行失败、映射失败都
 * 是一次）。SQL 在一个操作内部构造完成之后才投递；结果映射（纯 CPU、不碰
 * 网络）也在回调里做完。
 *
 * ## 投递失败 ≠ 执行失败
 *
 *   * **投递失败**：`loop` 为空、回调为空、模型/SQL 构造失败、池子已关 ——
 *     返回**负的 libuv 码**，回调**不会**被调；可选的 `submit_error` 出参会
 *     写一句人读原因（池子自己的 `last_error()` 也会保留借出/关闭的原因）。
 *   * **执行失败**：投递成功（返回 0）之后，数据库拒绝语句或结果集映射失败
 *     —— 回调带着非 `OK` 的 `uvcpp_orm_status` 来**一次**，`error` 里是原因。
 *
 * 这条分界与 `uvcpp_db_async` / `uvcpp_db_pool` 的异步接口完全一致：判
 * 「这一次有没有投出去」看返回值，别看回调来没来。
 *
 * ## 方言为什么是显式参数
 *
 * `uvcpp_db_pool` 不暴露“池里的连接是哪个驱动”，而为了拿驱动名在调用方线程
 * 上 `acquire()` 一次会**阻塞**（借不出时最长等到借出超时）—— 那正好是异步
 * 要躲开的事，而且如果调用发生在循环线程上还会把循环卡住。所以：
 *
 *   * `find_all_async(sql_result)` / `find_all_async(builder)` 从 SQL 自身
 *     的 `dialect` 取值；
 *   * `find_by_pk_async` / `insert_async` / `update_async` / `remove_async`
 *     要求调用方传入建库时用的方言（SQLite/MySQL 是 `?`，PostgreSQL 是
 *     `$1..$n`，传错会在执行时由数据库报错，而不是被悄悄改写）。
 *
 * ## 各操作走哪条现有路径
 *
 *   * `find_by_pk_async` / `find_all_async` / `update_async` /
 *     `remove_async`：`uvcpp_db_pool::query` / `execute`。PostgreSQL 上这会
 *     走池子已有的**真异步**路径（`uv_poll` 盯连接套接字，不占线程）；
 *     SQLite / MySQL / ODBC 走现有的 `uv_queue_work` 线程池路径。
 *   * `insert_async`：`uvcpp_db_pool::transaction` + 一条 INSERT。这么做是
 *     为了拿回 #91 的 `last_insert_id` —— 池子的异步 `insert` 回调只交
 *     `affected`，而“插入 + 取自增值”是两次后端往返，现有 `uvcpp_db_pool`
 *     本来就把 `insert` 排除在真异步之外（见 `uvcpp_db_pool.cpp` 的
 *     `submit_job`）。它仍然跑在现有的 libuv 线程池上，不新增线程。
 *
 * ## 不做什么
 *
 * 协程、自动重试、取消正在执行的 SQL、独立数据库线程池、跨连接事务、
 * 异步迁移、关联加载。
 */

#pragma once
#ifndef SRC_ORM_UVCPP_ORM_ASYNC_H
#define SRC_ORM_UVCPP_ORM_ASYNC_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <uv.h>

#include <db/uvcpp_db_async.h>
#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_status.h>
#include <db/uvcpp_db_transaction.h>
#include <db/uvcpp_db_value.h>
#include <orm/uvcpp_orm_error.h>
#include <orm/uvcpp_orm_query.h>
#include <orm/uvcpp_orm_repository.h>

namespace uvcpp {

/// `find_by_pk_async` 的完成回调。失败时 `value` 是默认构造的 `T`，以
/// `status` 为准。
template <typename T>
using uvcpp_orm_find_by_pk_cb =
    std::function<void(uvcpp_orm_status, const T&, const std::string&)>;

/// `find_all_async` 的完成回调。失败时 `rows` 是空数组。
template <typename T>
using uvcpp_orm_find_all_cb =
    std::function<void(uvcpp_orm_status, const std::vector<T>&,
                       const std::string&)>;

/// `insert_async` 的完成回调。`new_id` 在后端报不出自增值时是 NULL 值
/// （那不算失败，与同步 `insert` 同一条语义）。
using uvcpp_orm_insert_cb = std::function<void(
    uvcpp_orm_status, const uvcpp_db_value&, const std::string&)>;

/// `update_async` / `remove_async` 的完成回调。执行失败时 `affected` 是 -1；
/// 受影响 0 行不是失败（`status` 仍是 `OK`）。
using uvcpp_orm_exec_cb =
    std::function<void(uvcpp_orm_status, int64_t, const std::string&)>;

namespace orm_detail {

inline int async_submit_failed(std::string* submit_error,
                               const std::string& message, int rc) {
  if (submit_error != nullptr) *submit_error = message;
  return rc;
}

inline std::string async_pool_submit_error(uvcpp_db_pool& pool,
                                           const char* action, int rc) {
  std::string message(action);
  message += ": submit failed (libuv ";
  message += std::to_string(rc);
  message += ")";
  const std::string reason = pool.last_error();
  if (!reason.empty()) {
    message += ": ";
    message += reason;
  }
  return message;
}

inline std::string async_db_error(const char* action,
                                  uvcpp_db_status status) {
  std::string message(action);
  message += ": ";
  message += uvcpp_db_status_name(status);
  return message;
}

/// 一次异步 INSERT 的事务体与完成回调之间共享的状态。
struct orm_async_insert_state {
  orm_async_insert_state() : status(uvcpp_orm_status::OK) {}

  uvcpp_orm_status status;
  uvcpp_db_value new_id;
  std::string error;
};

}  // namespace orm_detail

// ---------------------------------------------------------------------------
// find_by_pk_async
// ---------------------------------------------------------------------------

/// 按主键异步查一行。`NOT_FOUND` 走回调（不是投递失败）。
///
/// 返回值：`0` = 已投递，回调会在 `loop` 线程上恰好一次；负值 = 没投出去，
/// 回调不会来，原因见 `submit_error`（可选）。
template <typename T>
inline int find_by_pk_async(
    uvcpp_db_pool& pool, uvcpp_loop* loop, uvcpp_orm_dialect dialect,
    const uvcpp_db_value& id,
    const uvcpp_orm_find_by_pk_cb<T>& cb,
    std::string* submit_error = nullptr) {
  if (submit_error != nullptr) submit_error->clear();
  if (loop == nullptr) {
    return orm_detail::async_submit_failed(
        submit_error, "find_by_pk_async: loop must not be null", UV_EINVAL);
  }
  if (!cb) {
    return orm_detail::async_submit_failed(
        submit_error, "find_by_pk_async: callback is empty", UV_EINVAL);
  }

  uvcpp_orm_sql_result sql_result;
  std::string error;
  const uvcpp_orm_status built = orm_detail::make_find_by_pk_sql<T>(
      dialect, id, &sql_result, &error);
  if (built != uvcpp_orm_status::OK) {
    return orm_detail::async_submit_failed(submit_error, error, UV_EINVAL);
  }

  const int rc = pool.query(
      loop, sql_result.sql, sql_result.params,
      [cb](uvcpp_db_status database_status, const uvcpp_db_table& table) {
        if (!uvcpp_db_ok(database_status)) {
          cb(uvcpp_orm_status::DB_ERROR, T(),
             orm_detail::async_db_error("find_by_pk query",
                                        database_status));
          return;
        }

        std::vector<orm_detail::column_binding> bindings;
        std::string map_error;
        uvcpp_orm_status status =
            orm_detail::prepare_bindings<T>(table, &bindings, &map_error);
        if (status != uvcpp_orm_status::OK) {
          cb(status, T(), map_error);
          return;
        }
        if (table.row_count() == 0u) {
          std::string message = "find_by_pk: no row in table \"";
          message += model_traits<T>::table_name();
          message += "\" matches the primary key";
          cb(uvcpp_orm_status::NOT_FOUND, T(), message);
          return;
        }

        T value;
        status = orm_detail::map_row(table.rows()[0], bindings, 0u, &value,
                                     &map_error);
        if (status != uvcpp_orm_status::OK) {
          cb(status, T(), map_error);
          return;
        }
        cb(uvcpp_orm_status::OK, value, std::string());
      });
  if (rc != 0) {
    return orm_detail::async_submit_failed(
        submit_error,
        orm_detail::async_pool_submit_error(pool, "find_by_pk_async", rc),
        rc);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// find_all_async
// ---------------------------------------------------------------------------

/// 执行一条**已构造**的 SELECT 并异步映射成模型。没有 total 参数：总数查询
/// 需要第二条 SQL，而这里只承诺“一次投递、一次回调”。需要 total 时在回调
/// 里再投一条 COUNT，或使用同步路径。
template <typename T>
inline int find_all_async(
    uvcpp_db_pool& pool, uvcpp_loop* loop,
    const uvcpp_orm_sql_result& sql_result,
    const uvcpp_orm_find_all_cb<T>& cb,
    std::string* submit_error = nullptr) {
  if (submit_error != nullptr) submit_error->clear();
  if (loop == nullptr) {
    return orm_detail::async_submit_failed(
        submit_error, "find_all_async: loop must not be null", UV_EINVAL);
  }
  if (!cb) {
    return orm_detail::async_submit_failed(
        submit_error, "find_all_async: callback is empty", UV_EINVAL);
  }
  if (!sql_result.ok()) {
    return orm_detail::async_submit_failed(submit_error, sql_result.error,
                                           UV_EINVAL);
  }
  if (sql_result.sql.empty()) {
    return orm_detail::async_submit_failed(
        submit_error, "find_all_async: SQL is empty", UV_EINVAL);
  }

  const int rc = pool.query(
      loop, sql_result.sql, sql_result.params,
      [cb](uvcpp_db_status database_status, const uvcpp_db_table& table) {
        if (!uvcpp_db_ok(database_status)) {
          cb(uvcpp_orm_status::DB_ERROR, std::vector<T>(),
             orm_detail::async_db_error("find_all query",
                                        database_status));
          return;
        }

        std::vector<T> rows;
        std::string map_error;
        const uvcpp_orm_status status =
            orm_detail::map_table<T>(table, &rows, &map_error);
        if (status != uvcpp_orm_status::OK) {
          cb(status, std::vector<T>(), map_error);
          return;
        }
        cb(uvcpp_orm_status::OK, rows, std::string());
      });
  if (rc != 0) {
    return orm_detail::async_submit_failed(
        submit_error,
        orm_detail::async_pool_submit_error(pool, "find_all_async", rc), rc);
  }
  return 0;
}

/// 执行一个 SELECT 构造器并异步映射成模型。方言从 builder 自己的 SQL 里取。
template <typename T>
inline int find_all_async(
    uvcpp_db_pool& pool, uvcpp_loop* loop,
    const uvcpp_orm_select_builder<T>& builder,
    const uvcpp_orm_find_all_cb<T>& cb,
    std::string* submit_error = nullptr) {
  if (submit_error != nullptr) submit_error->clear();
  if (loop == nullptr) {
    return orm_detail::async_submit_failed(
        submit_error, "find_all_async: loop must not be null", UV_EINVAL);
  }
  if (!cb) {
    return orm_detail::async_submit_failed(
        submit_error, "find_all_async: callback is empty", UV_EINVAL);
  }

  const uvcpp_orm_sql_result sql_result = builder.build();
  if (!sql_result.ok()) {
    return orm_detail::async_submit_failed(submit_error, sql_result.error,
                                           UV_EINVAL);
  }
  return find_all_async<T>(pool, loop, sql_result, cb, submit_error);
}

// ---------------------------------------------------------------------------
// insert_async
// ---------------------------------------------------------------------------

/// 异步插入一行。`new_id` 走回调回填（自增主键）。
///
/// 实现上复用 `uvcpp_db_pool::transaction`：池子的异步 `insert` 回调只交
/// `affected`，拿不到 `last_insert_id`；而“INSERT + 取自增值”本来就是两次
/// 阻塞往返，现有池子也把它排除在真异步之外。事务体里只有这一条 ORM INSERT，
/// 提交/回滚语义由 `uvcpp_db_tx` 保证。
template <typename T>
inline int insert_async(
    uvcpp_db_pool& pool, uvcpp_loop* loop, uvcpp_orm_dialect dialect,
    const T& value, const uvcpp_orm_insert_cb& cb,
    std::string* submit_error = nullptr,
    const uvcpp_db_tx_options& options = uvcpp_db_tx_options()) {
  if (submit_error != nullptr) submit_error->clear();
  if (loop == nullptr) {
    return orm_detail::async_submit_failed(
        submit_error, "insert_async: loop must not be null", UV_EINVAL);
  }
  if (!cb) {
    return orm_detail::async_submit_failed(
        submit_error, "insert_async: callback is empty", UV_EINVAL);
  }

  uvcpp_orm_sql_result sql_result;
  std::string error;
  const uvcpp_orm_status built =
      orm_detail::make_insert_sql<T>(dialect, value, &sql_result, &error);
  if (built != uvcpp_orm_status::OK) {
    return orm_detail::async_submit_failed(submit_error, error, UV_EINVAL);
  }

  std::shared_ptr<orm_detail::orm_async_insert_state> state(
      new orm_detail::orm_async_insert_state());

  const int rc = pool.transaction(
      loop,
      [state, sql_result](uvcpp_db_tx& tx) {
        uvcpp_db_value new_id;
        uvcpp_db_status database_status;
        if (orm_detail::model_has_auto_increment<T>()) {
          database_status =
              tx.insert(sql_result.sql, sql_result.params, &new_id);
        } else {
          database_status =
              tx.execute(sql_result.sql, sql_result.params, nullptr);
        }
        if (!uvcpp_db_ok(database_status)) {
          state->status = uvcpp_orm_status::DB_ERROR;
          state->error = orm_detail::async_db_error("insert",
                                                    database_status);
          const std::string detail = tx.error();
          if (!detail.empty()) {
            state->error += ": ";
            state->error += detail;
          }
          return;
        }
        state->new_id = std::move(new_id);
      },
      [cb, state](uvcpp_db_status database_status,
                  const std::string& transaction_error) {
        if (state->status != uvcpp_orm_status::OK) {
          cb(state->status, uvcpp_db_value(), state->error);
          return;
        }
        if (!uvcpp_db_ok(database_status)) {
          std::string message =
              orm_detail::async_db_error("insert", database_status);
          if (!transaction_error.empty()) {
            message += ": ";
            message += transaction_error;
          }
          cb(uvcpp_orm_status::DB_ERROR, uvcpp_db_value(), message);
          return;
        }
        cb(uvcpp_orm_status::OK, state->new_id, std::string());
      },
      options);
  if (rc != 0) {
    return orm_detail::async_submit_failed(
        submit_error,
        orm_detail::async_pool_submit_error(pool, "insert_async", rc), rc);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// update_async / remove_async
// ---------------------------------------------------------------------------

/// 异步按主键更新。受影响 0 行不是失败（回调 `OK` + `affected == 0`）。
template <typename T>
inline int update_async(
    uvcpp_db_pool& pool, uvcpp_loop* loop, uvcpp_orm_dialect dialect,
    const T& value, const uvcpp_orm_exec_cb& cb,
    std::string* submit_error = nullptr) {
  if (submit_error != nullptr) submit_error->clear();
  if (loop == nullptr) {
    return orm_detail::async_submit_failed(
        submit_error, "update_async: loop must not be null", UV_EINVAL);
  }
  if (!cb) {
    return orm_detail::async_submit_failed(
        submit_error, "update_async: callback is empty", UV_EINVAL);
  }

  uvcpp_orm_sql_result sql_result;
  std::string error;
  const uvcpp_orm_status built =
      orm_detail::make_update_sql<T>(dialect, value, &sql_result, &error);
  if (built != uvcpp_orm_status::OK) {
    return orm_detail::async_submit_failed(submit_error, error, UV_EINVAL);
  }

  const int rc = pool.execute(
      loop, sql_result.sql, sql_result.params,
      [cb](uvcpp_db_status database_status, int64_t affected) {
        if (!uvcpp_db_ok(database_status)) {
          cb(uvcpp_orm_status::DB_ERROR, -1,
             orm_detail::async_db_error("update", database_status));
          return;
        }
        cb(uvcpp_orm_status::OK, affected, std::string());
      });
  if (rc != 0) {
    return orm_detail::async_submit_failed(
        submit_error,
        orm_detail::async_pool_submit_error(pool, "update_async", rc), rc);
  }
  return 0;
}

/// 异步按主键删除。受影响 0 行不是失败（回调 `OK` + `affected == 0`）。
template <typename T>
inline int remove_async(
    uvcpp_db_pool& pool, uvcpp_loop* loop, uvcpp_orm_dialect dialect,
    const uvcpp_db_value& id, const uvcpp_orm_exec_cb& cb,
    std::string* submit_error = nullptr) {
  if (submit_error != nullptr) submit_error->clear();
  if (loop == nullptr) {
    return orm_detail::async_submit_failed(
        submit_error, "remove_async: loop must not be null", UV_EINVAL);
  }
  if (!cb) {
    return orm_detail::async_submit_failed(
        submit_error, "remove_async: callback is empty", UV_EINVAL);
  }

  uvcpp_orm_sql_result sql_result;
  std::string error;
  const uvcpp_orm_status built =
      orm_detail::make_remove_sql<T>(dialect, id, &sql_result, &error);
  if (built != uvcpp_orm_status::OK) {
    return orm_detail::async_submit_failed(submit_error, error, UV_EINVAL);
  }

  const int rc = pool.execute(
      loop, sql_result.sql, sql_result.params,
      [cb](uvcpp_db_status database_status, int64_t affected) {
        if (!uvcpp_db_ok(database_status)) {
          cb(uvcpp_orm_status::DB_ERROR, -1,
             orm_detail::async_db_error("remove", database_status));
          return;
        }
        cb(uvcpp_orm_status::OK, affected, std::string());
      });
  if (rc != 0) {
    return orm_detail::async_submit_failed(
        submit_error,
        orm_detail::async_pool_submit_error(pool, "remove_async", rc), rc);
  }
  return 0;
}

}  // namespace uvcpp

#endif  // UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE
#endif  // SRC_ORM_UVCPP_ORM_ASYNC_H
