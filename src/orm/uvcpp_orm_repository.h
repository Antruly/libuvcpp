/**
 * @file src/orm/uvcpp_orm_repository.h
 * @brief 单表 ORM 的执行层：把 `uvcpp_db_table` 的行映射成模型，并在同步
 *        `uvcpp_db_client` 上完成 CRUD。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一层只做两件事：
 *
 *   1. 把 #90 构造出来的 `uvcpp_orm_sql_result`（SQL + 参数）交给
 *      `uvcpp_db_client::query/execute/insert` 执行；
 *   2. 把结果集按**列名**映射回模型。
 *
 * 不新开连接、不拼接值、不绕过 `uvcpp_db_params`、不实现事务/异步/关联。
 *
 * 执行入口有三种，共用同一套 SQL 构造与结果映射：
 *
 *   * `uvcpp_db_client&` —— 同步连接（#91 的路径，语义不变）；
 *   * `uvcpp_db_tx&` —— 事务内。每条语句都过 `uvcpp_db_tx` 自己的门，因此
 *     提交/回滚/异常/超时语义完全由现有事务层保证，ORM 不自己 begin/commit；
 *   * `uvcpp_db_pool&` —— 同步借还一条连接（单语句；`acquire()` 会阻塞）。
 *
 * 异步门面在 `<orm/uvcpp_orm_async.h>`：它只把本文件构造好的 SQL 交给
 * `uvcpp_db_pool` 的现有异步接口，不新开线程、不新开连接池。
 *
 * ## 映射规则
 *
 *   * 列名**精确匹配**（大小写敏感，与 `uvcpp_db_table::column_index()` 一致）；
 *   * 结果集里多出来的列忽略；
 *   * 结果集缺一个**可空**列：给对应 `nullable<T>` 写空态；
 *   * 结果集缺一个**非空**列：`MISSING_COLUMN`，错误串带字段名与列名；
 *   * SQL NULL → `nullable<T>` 是空态；SQL NULL → 非空字段是 `NULL_VIOLATION`；
 *   * 空串是有效 TEXT，不是 NULL；
 *   * 数值宽度/符号不匹配（例如 TEXT 映射到 int、整数超出 `int8_t`）是
 *     `TYPE_MISMATCH`，不做静默截断。
 *
 * 一条结果行的映射先在栈上的临时 `T` 里完成，全部成功后才写进调用方的
 * `out` —— 映射失败不会留下“半个对象”。
 *
 * ## 返回码
 *
 *   * 数据库执行失败统一返回 `DB_ERROR`，并同时给出：
 *       - 可选的 `uvcpp_db_status*` 出参（后端原始返回码）；
 *       - `error` 串里的返回码短名与 `db.last_error()` 原文。
 *   * `update` / `remove` 受影响 0 行**不是**错误：返回 `OK`，`affected`
 *     写 0。执行失败时 `affected` 写 -1，返回非 `OK`。
 *   * `find_by_pk` 没找到返回 `NOT_FOUND`，不碰 `out`。
 *   * 参数错误（空 out、NULL 主键、SQL 方言与连接不匹配）返回
 *     `INVALID_ARGUMENT`。
 *
 * ## 已知边界
 *
 * 模型必须是可默认构造的聚合（`column_for<T>()` 在 #89 已经要求这一点）。
 * 字段偏移来自 `offsetof`，与 #89 同一条已知代价。
 *
 * `find_all(result, ..., total)` 需要从已构造的 SELECT 推导 COUNT 查询：它只
 * 支持 #90 构造器生成的单表 SELECT 形状。`raw()` 写出来的任意 SQL 如果选择
 * 列表里也带占位符，推导会拒绝（返回 `INVALID_ARGUMENT`）—— 那种查询请用
 * `find_all(builder, ..., total)` 或显式传一个 COUNT 查询给 `count<T>()`。
 */

#pragma once
#ifndef SRC_ORM_UVCPP_ORM_REPOSITORY_H
#define SRC_ORM_UVCPP_ORM_REPOSITORY_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_params.h>
#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_status.h>
#include <db/uvcpp_db_table.h>
#include <db/uvcpp_db_transaction.h>
#include <db/uvcpp_db_value.h>
#include <orm/uvcpp_orm_error.h>
#include <orm/uvcpp_orm_model.h>
#include <orm/uvcpp_orm_query.h>
#include <orm/uvcpp_orm_value.h>

namespace uvcpp {
namespace orm_detail {

// ---------------------------------------------------------------------------
// 错误与方言
// ---------------------------------------------------------------------------

inline uvcpp_orm_status set_invalid_argument(std::string* error,
                                             const std::string& message) {
  return set_error(error, uvcpp_orm_status::INVALID_ARGUMENT, message);
}

/// 把一次数据库失败翻译成 `DB_ERROR`：保留后端返回码（出参）与原文
/// （执行器的 `last_error()`，写进 `error`）。不要把所有失败压成一个字符串
/// —— 唯一键冲突与“连接没打开”在调用方眼里是两种处置。
inline uvcpp_orm_status set_db_error(const std::string& last_error,
                                     uvcpp_db_status status,
                                     const std::string& action,
                                     std::string* error,
                                     uvcpp_db_status* db_status) {
  if (db_status != nullptr) *db_status = status;
  if (error != nullptr) {
    *error = action + ": " + uvcpp_db_status_name(status) + ": " + last_error;
  }
  return uvcpp_orm_status::DB_ERROR;
}

/// 驱动名 → ORM 方言。驱动名不认识（或连接没开）时返回 false；调用方据此报
/// `DB_ERROR`，而不是猜一个方言。
inline bool dialect_from_driver_name(const char* name,
                                     uvcpp_orm_dialect* out) {
  if (out == nullptr || name == nullptr || *name == '\0') return false;
  const std::string driver(name);
  if (driver == "sqlite") {
    *out = uvcpp_orm_dialect::sqlite;
    return true;
  }
  if (driver == "mysql") {
    *out = uvcpp_orm_dialect::mysql;
    return true;
  }
  if (driver == "postgres" || driver == "postgresql") {
    *out = uvcpp_orm_dialect::postgresql;
    return true;
  }
  return false;
}

inline bool dialect_from_client(const uvcpp_db_client& db,
                                uvcpp_orm_dialect* out) {
  if (dialect_from_driver_name(db.driver_name(), out)) return true;
  // ODBC is a generic driver manager: the SQL dialect belongs to the data
  // source, not to the manager. The only ORM dialect reachable through the
  // ODBC path today is SQLite; an ODBC SQL Server/Oracle connection must stay
  // UNSUPPORTED instead of being guessed as SQLite.
  if (out != nullptr && std::string(db.driver_name()) == "odbc") {
    std::string url = db.url();
    for (std::size_t i = 0; i < url.size(); ++i) {
      if (url[i] >= 'A' && url[i] <= 'Z') url[i] = static_cast<char>(url[i] - 'A' + 'a');
    }
    if (url.find("sqlite") != std::string::npos) {
      *out = uvcpp_orm_dialect::sqlite;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// 执行适配层（同步连接 / 事务 / 池）
// ---------------------------------------------------------------------------

/// 同步连接：所有方法原样转发给 `uvcpp_db_client`。这是 #91 的既有路径，
/// 行为与错误文案都不变。
class client_executor {
 public:
  explicit client_executor(uvcpp_db_client& db) : db_(&db) {}

  bool valid() const { return true; }
  uvcpp_orm_status invalid_error(const std::string&, std::string*,
                                 uvcpp_db_status*) const {
    return uvcpp_orm_status::OK;
  }

  uvcpp_db_status query(const std::string& sql, const uvcpp_db_params& params,
                        uvcpp_db_table* out) {
    return db_->query(sql, params, out);
  }
  uvcpp_db_status execute(const std::string& sql,
                          const uvcpp_db_params& params,
                          int64_t* affected) {
    return db_->execute(sql, params, affected);
  }
  uvcpp_db_status insert(const std::string& sql, const uvcpp_db_params& params,
                         uvcpp_db_value* new_id) {
    return db_->insert(sql, params, new_id);
  }

  const char* driver_name() const { return db_->driver_name(); }
  const std::string& url() const { return db_->url(); }
  bool is_open() const { return db_->is_open(); }
  std::string last_error() const { return db_->last_error(); }

 private:
  uvcpp_db_client* db_;
};

/// 事务内执行。**每条语句都走 `uvcpp_db_tx` 自己的门**：第一条失败会被
/// 事务记住，之后的语句直接返回同一个错误；ORM 不自己 begin/commit，也不
/// 绕过 `tx.status()` / `tx.error()`。这样事务的提交/回滚/超时语义与直接使用
/// `uvcpp_db_tx` 完全一致。
class tx_executor {
 public:
  explicit tx_executor(uvcpp_db_tx& tx) : tx_(&tx) {}

  bool valid() const { return tx_->client() != nullptr; }
  uvcpp_orm_status invalid_error(const std::string& action,
                                 std::string* error,
                                 uvcpp_db_status* db_status) const {
    if (db_status != nullptr) *db_status = uvcpp_db_status::MISUSE;
    return set_error(error, uvcpp_orm_status::DB_ERROR,
                     action +
                         ": transaction has no client (uvcpp_db_tx must be "
                         "constructed by pool.transaction())");
  }

  uvcpp_db_status query(const std::string& sql, const uvcpp_db_params& params,
                        uvcpp_db_table* out) {
    return tx_->query(sql, params, out);
  }
  uvcpp_db_status execute(const std::string& sql,
                          const uvcpp_db_params& params,
                          int64_t* affected) {
    return tx_->execute(sql, params, affected);
  }
  uvcpp_db_status insert(const std::string& sql, const uvcpp_db_params& params,
                         uvcpp_db_value* new_id) {
    return tx_->insert(sql, params, new_id);
  }

  const char* driver_name() const {
    uvcpp_db_client* client = tx_->client();
    return client != nullptr ? client->driver_name() : nullptr;
  }
  std::string url() const {
    uvcpp_db_client* client = tx_->client();
    return client != nullptr ? client->url() : std::string();
  }
  bool is_open() const {
    uvcpp_db_client* client = tx_->client();
    return client != nullptr && client->is_open();
  }
  /// 事务已经记下第一笔错误时，`tx.error()` 是失败那一刻的原文快照，
  /// 比被后续调用覆盖过的 `client->last_error()` 更可信。
  std::string last_error() const {
    if (!tx_->error().empty()) return tx_->error();
    uvcpp_db_client* client = tx_->client();
    return client != nullptr ? client->last_error() : std::string();
  }

 private:
  uvcpp_db_tx* tx_;
};

/// 池子上的一次**同步**借还。`acquire()` 会阻塞，所以这组重载只给不在
/// 事件循环上的调用方用（与 `pool.query` / `pool.execute` 同一条约定）。
class pool_executor {
 public:
  explicit pool_executor(uvcpp_db_pool& pool)
      : pool_(&pool), client_(pool.acquire()) {
    if (client_ == nullptr) {
      acquire_error_ = pool.last_error();
      if (acquire_error_.empty()) acquire_error_ = "pool acquire failed";
    }
  }

  pool_executor(const pool_executor&) = delete;
  pool_executor& operator=(const pool_executor&) = delete;

  ~pool_executor() {
    if (client_ == nullptr) return;
    if (broken_) {
      pool_->discard(client_);
    } else {
      pool_->release(client_);
    }
  }

  bool valid() const { return client_ != nullptr; }
  uvcpp_orm_status invalid_error(const std::string& action,
                                 std::string* error,
                                 uvcpp_db_status* db_status) const {
    if (db_status != nullptr) *db_status = uvcpp_db_status::NO_CONNECTION;
    return set_error(error, uvcpp_orm_status::DB_ERROR,
                     action + ": pool acquire failed: " + acquire_error_);
  }

  uvcpp_db_status query(const std::string& sql, const uvcpp_db_params& params,
                        uvcpp_db_table* out) {
    const uvcpp_db_status status = client_->query(sql, params, out);
    note(status);
    return status;
  }
  uvcpp_db_status execute(const std::string& sql,
                          const uvcpp_db_params& params,
                          int64_t* affected) {
    const uvcpp_db_status status = client_->execute(sql, params, affected);
    note(status);
    return status;
  }
  uvcpp_db_status insert(const std::string& sql, const uvcpp_db_params& params,
                         uvcpp_db_value* new_id) {
    const uvcpp_db_status status = client_->insert(sql, params, new_id);
    note(status);
    return status;
  }

  const char* driver_name() const { return client_->driver_name(); }
  std::string url() const { return client_->url(); }
  bool is_open() const { return client_->is_open(); }
  std::string last_error() const { return client_->last_error(); }

 private:
  void note(uvcpp_db_status status) {
    // 坏连接不回池子 —— 与池子自己的代借代还同一条规矩。
    if (status == uvcpp_db_status::NOT_CONNECTED) broken_ = true;
  }

  uvcpp_db_pool* pool_;
  uvcpp_db_client* client_;
  bool broken_ = false;
  std::string acquire_error_;
};

/// 从任意执行器取方言。执行器隐藏了“连接从哪里来”，这里只认驱动名。
template <typename Executor>
inline bool dialect_from_executor(const Executor& db, uvcpp_orm_dialect* out) {
  if (dialect_from_driver_name(db.driver_name(), out)) return true;
  if (out != nullptr && std::string(db.driver_name()) == "odbc") {
    std::string url = db.url();
    for (std::size_t i = 0; i < url.size(); ++i) {
      if (url[i] >= 'A' && url[i] <= 'Z') url[i] = static_cast<char>(url[i] - 'A' + 'a');
    }
    if (url.find("sqlite") != std::string::npos) {
      *out = uvcpp_orm_dialect::sqlite;
      return true;
    }
  }
  return false;
}

/// 执行器没有可用方言时的错误。`db_status` 尽量贴近真实原因：没 open 就是
/// `NOT_CONNECTED`，打开了一个本层不认识的驱动就是 `UNSUPPORTED`。
template <typename Executor>
inline uvcpp_orm_status set_no_driver_error(const Executor& db,
                                            const std::string& action,
                                            std::string* error,
                                            uvcpp_db_status* db_status) {
  if (db_status != nullptr) {
    *db_status = db.is_open() ? uvcpp_db_status::UNSUPPORTED
                              : uvcpp_db_status::NOT_CONNECTED;
  }
  return set_error(
      error, uvcpp_orm_status::DB_ERROR,
      action +
          ": database client has no supported driver (open it with "
          "sqlite/mysql/postgres first)");
}

inline std::string column_label(const uvcpp_orm_column& column) {
  const char* field = column.field_name();
  const char* name = column.column_name();
  return std::string("field \"") + (field != nullptr ? field : "?") +
         "\" (column \"" + (name != nullptr ? name : "?") + "\")";
}

inline bool dialect_matches(const uvcpp_orm_sql_result& result,
                            uvcpp_orm_dialect dialect,
                            std::string* error, const std::string& action) {
  if (result.dialect == dialect) return true;
  set_error(error, uvcpp_orm_status::INVALID_ARGUMENT,
            action + ": SQL was built for dialect \"" +
                uvcpp_orm_dialect_name(result.dialect) +
                "\" but the connection is \"" +
                uvcpp_orm_dialect_name(dialect) + "\"");
  return false;
}

// ---------------------------------------------------------------------------
// 类型映射：uvcpp_db_value ↔ 模型成员
// ---------------------------------------------------------------------------

/// `uvcpp_orm_type` 是否能接受这个数据库值标签。NULL 由调用方单独处理。
inline bool db_type_compatible(uvcpp_orm_type type,
                               uvcpp_db_type database_type) {
  switch (type) {
    case uvcpp_orm_type::BOOL:
      // SQLite 的布尔就是 INTEGER；MySQL 的 TINYINT(1) 也走 INT64。
      return database_type == uvcpp_db_type::BOOL ||
             database_type == uvcpp_db_type::INT64 ||
             database_type == uvcpp_db_type::UINT64;

    case uvcpp_orm_type::INT8:
    case uvcpp_orm_type::UINT8:
    case uvcpp_orm_type::INT16:
    case uvcpp_orm_type::UINT16:
    case uvcpp_orm_type::INT32:
    case uvcpp_orm_type::UINT32:
    case uvcpp_orm_type::INT64:
    case uvcpp_orm_type::UINT64:
      return database_type == uvcpp_db_type::INT64 ||
             database_type == uvcpp_db_type::UINT64;

    case uvcpp_orm_type::FLOAT:
    case uvcpp_orm_type::DOUBLE:
      // SQLite 动态类型允许 REAL 列里放整数；读出整数时按数值接受。
      return database_type == uvcpp_db_type::DOUBLE ||
             database_type == uvcpp_db_type::INT64 ||
             database_type == uvcpp_db_type::UINT64 ||
             database_type == uvcpp_db_type::BOOL;

    case uvcpp_orm_type::TEXT:
      // 日期时间在值层本来就是文本语义。
      return database_type == uvcpp_db_type::TEXT ||
             database_type == uvcpp_db_type::DATE ||
             database_type == uvcpp_db_type::TIME ||
             database_type == uvcpp_db_type::DATETIME;

    case uvcpp_orm_type::BLOB:
      return database_type == uvcpp_db_type::BLOB;

    case uvcpp_orm_type::DATE:
    case uvcpp_orm_type::TIME:
    case uvcpp_orm_type::DATETIME:
    case uvcpp_orm_type::UNKNOWN:
      return false;
  }
  return false;
}

/// 有符号目标：INT64 必须落在 `Cpp` 的范围内，UINT64 不能超过有符号上限。
template <typename Cpp>
inline bool convert_integer_value(const uvcpp_db_value& value, Cpp* out,
                                  std::true_type /* is_signed */) {
  if (out == nullptr || value.is_null()) return false;
  if (value.type() == uvcpp_db_type::INT64) {
    const std::int64_t number = value.to_int64();
    if (number < static_cast<std::int64_t>(
                     (std::numeric_limits<Cpp>::min)()) ||
        number > static_cast<std::int64_t>(
                     (std::numeric_limits<Cpp>::max)())) {
      return false;
    }
    *out = static_cast<Cpp>(number);
    return true;
  }
  if (value.type() == uvcpp_db_type::UINT64) {
    const std::uint64_t number = value.to_uint64();
    if (number >
        static_cast<std::uint64_t>((std::numeric_limits<Cpp>::max)())) {
      return false;
    }
    *out = static_cast<Cpp>(number);
    return true;
  }
  return false;
}

/// 无符号目标：负的 INT64 必须拒绝（`to_uint64()` 会把它静默读成 0）。
template <typename Cpp>
inline bool convert_integer_value(const uvcpp_db_value& value, Cpp* out,
                                  std::false_type /* is_signed */) {
  if (out == nullptr || value.is_null()) return false;
  if (value.type() == uvcpp_db_type::INT64) {
    const std::int64_t number = value.to_int64();
    if (number < 0) return false;
    const std::uint64_t unsigned_number = static_cast<std::uint64_t>(number);
    if (unsigned_number >
        static_cast<std::uint64_t>((std::numeric_limits<Cpp>::max)())) {
      return false;
    }
    *out = static_cast<Cpp>(unsigned_number);
    return true;
  }
  if (value.type() == uvcpp_db_type::UINT64) {
    const std::uint64_t number = value.to_uint64();
    if (number >
        static_cast<std::uint64_t>((std::numeric_limits<Cpp>::max)())) {
      return false;
    }
    *out = static_cast<Cpp>(number);
    return true;
  }
  return false;
}

/// 单个（非 nullable）C++ 类型的映射。默认实现拒绝没有映射的类型。
template <typename M>
struct base_mapper {
  static bool assign(const uvcpp_db_value&, M*) { return false; }
};

#define UVCPP_ORM_DETAIL_INT_MAPPER(cpp_type)                              \
  template <>                                                              \
  struct base_mapper<cpp_type> {                                           \
    static bool assign(const uvcpp_db_value& value, cpp_type* out) {       \
      if (out == nullptr || value.is_null()) return false;                 \
      return convert_integer_value(                                        \
          value, out,                                                      \
          std::integral_constant<bool,                                     \
                                 std::is_signed<cpp_type>::value>());       \
    }                                                                      \
  };

UVCPP_ORM_DETAIL_INT_MAPPER(std::int8_t)
UVCPP_ORM_DETAIL_INT_MAPPER(std::uint8_t)
UVCPP_ORM_DETAIL_INT_MAPPER(std::int16_t)
UVCPP_ORM_DETAIL_INT_MAPPER(std::uint16_t)
UVCPP_ORM_DETAIL_INT_MAPPER(std::int32_t)
UVCPP_ORM_DETAIL_INT_MAPPER(std::uint32_t)
UVCPP_ORM_DETAIL_INT_MAPPER(std::int64_t)
UVCPP_ORM_DETAIL_INT_MAPPER(std::uint64_t)

#undef UVCPP_ORM_DETAIL_INT_MAPPER

template <>
struct base_mapper<bool> {
  static bool assign(const uvcpp_db_value& value, bool* out) {
    if (out == nullptr || value.is_null()) return false;
    if (value.type() != uvcpp_db_type::BOOL &&
        value.type() != uvcpp_db_type::INT64 &&
        value.type() != uvcpp_db_type::UINT64) {
      return false;
    }
    *out = value.to_bool();
    return true;
  }
};

template <>
struct base_mapper<float> {
  static bool assign(const uvcpp_db_value& value, float* out) {
    if (out == nullptr || value.is_null()) return false;
    if (value.type() != uvcpp_db_type::DOUBLE &&
        value.type() != uvcpp_db_type::INT64 &&
        value.type() != uvcpp_db_type::UINT64 &&
        value.type() != uvcpp_db_type::BOOL) {
      return false;
    }
    const double number = value.to_double();
    // float 装不下的有限值必须报错，而不是变成 inf。NaN 原样通过。
    const double maximum = static_cast<double>((std::numeric_limits<float>::max)());
    if (number == number && (number > maximum || number < -maximum)) {
      return false;
    }
    *out = static_cast<float>(number);
    return true;
  }
};

template <>
struct base_mapper<double> {
  static bool assign(const uvcpp_db_value& value, double* out) {
    if (out == nullptr || value.is_null()) return false;
    if (value.type() != uvcpp_db_type::DOUBLE &&
        value.type() != uvcpp_db_type::INT64 &&
        value.type() != uvcpp_db_type::UINT64 &&
        value.type() != uvcpp_db_type::BOOL) {
      return false;
    }
    *out = value.to_double();
    return true;
  }
};

template <>
struct base_mapper<std::string> {
  static bool assign(const uvcpp_db_value& value, std::string* out) {
    if (out == nullptr || value.is_null()) return false;
    if (value.type() != uvcpp_db_type::TEXT &&
        value.type() != uvcpp_db_type::DATE &&
        value.type() != uvcpp_db_type::TIME &&
        value.type() != uvcpp_db_type::DATETIME) {
      return false;
    }
    // 空串是 TEXT 值，不是 NULL。
    *out = value.to_text();
    return true;
  }
};

template <>
struct base_mapper<std::vector<std::uint8_t> > {
  static bool assign(const uvcpp_db_value& value,
                     std::vector<std::uint8_t>* out) {
    if (out == nullptr || value.is_null()) return false;
    if (value.type() != uvcpp_db_type::BLOB) return false;
    const std::string& bytes = value.bytes();
    out->assign(bytes.begin(), bytes.end());
    return true;
  }
};

template <typename M>
inline bool assign_nullable_value(const uvcpp_db_value& value, void* field) {
  nullable<M>* out = static_cast<nullable<M>*>(field);
  if (value.is_null()) {
    out->reset();
    return true;
  }
  M temporary;
  if (!base_mapper<M>::assign(value, &temporary)) return false;
  *out = nullable<M>(std::move(temporary));
  return true;
}

/// 把一个**非 NULL** 值写进字段。`field` 指向 `column` 在模型里的成员。
/// 失败只表示类型/范围不接受；NULL 与非空判定在调用方。
inline bool assign_column_value(const uvcpp_orm_column& column, void* field,
                                const uvcpp_db_value& value) {
#define UVCPP_ORM_DETAIL_ASSIGN_CASE(tag, cpp_type)                       \
  case uvcpp_orm_type::tag:                                                \
    return column.is_nullable()                                            \
               ? assign_nullable_value<cpp_type>(value, field)             \
               : base_mapper<cpp_type>::assign(                            \
                     value, static_cast<cpp_type*>(field));

  switch (column.type()) {
    UVCPP_ORM_DETAIL_ASSIGN_CASE(BOOL, bool)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(INT8, std::int8_t)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(UINT8, std::uint8_t)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(INT16, std::int16_t)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(UINT16, std::uint16_t)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(INT32, std::int32_t)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(UINT32, std::uint32_t)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(INT64, std::int64_t)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(UINT64, std::uint64_t)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(FLOAT, float)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(DOUBLE, double)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(TEXT, std::string)
    UVCPP_ORM_DETAIL_ASSIGN_CASE(BLOB, std::vector<std::uint8_t>)

    case uvcpp_orm_type::DATE:
    case uvcpp_orm_type::TIME:
    case uvcpp_orm_type::DATETIME:
    case uvcpp_orm_type::UNKNOWN:
      return false;
  }
#undef UVCPP_ORM_DETAIL_ASSIGN_CASE
  return false;
}

/// 可空列缺列 / 值为 NULL 时写空态。只对 `nullable<T>` 字段调用。
inline void reset_nullable_column(const uvcpp_orm_column& column, void* field) {
#define UVCPP_ORM_DETAIL_RESET_CASE(tag, cpp_type) \
  case uvcpp_orm_type::tag:                        \
    static_cast<nullable<cpp_type>*>(field)->reset(); \
    return;

  switch (column.type()) {
    UVCPP_ORM_DETAIL_RESET_CASE(BOOL, bool)
    UVCPP_ORM_DETAIL_RESET_CASE(INT8, std::int8_t)
    UVCPP_ORM_DETAIL_RESET_CASE(UINT8, std::uint8_t)
    UVCPP_ORM_DETAIL_RESET_CASE(INT16, std::int16_t)
    UVCPP_ORM_DETAIL_RESET_CASE(UINT16, std::uint16_t)
    UVCPP_ORM_DETAIL_RESET_CASE(INT32, std::int32_t)
    UVCPP_ORM_DETAIL_RESET_CASE(UINT32, std::uint32_t)
    UVCPP_ORM_DETAIL_RESET_CASE(INT64, std::int64_t)
    UVCPP_ORM_DETAIL_RESET_CASE(UINT64, std::uint64_t)
    UVCPP_ORM_DETAIL_RESET_CASE(FLOAT, float)
    UVCPP_ORM_DETAIL_RESET_CASE(DOUBLE, double)
    UVCPP_ORM_DETAIL_RESET_CASE(TEXT, std::string)
    UVCPP_ORM_DETAIL_RESET_CASE(BLOB, std::vector<std::uint8_t>)

    case uvcpp_orm_type::DATE:
    case uvcpp_orm_type::TIME:
    case uvcpp_orm_type::DATETIME:
    case uvcpp_orm_type::UNKNOWN:
      return;
  }
#undef UVCPP_ORM_DETAIL_RESET_CASE
}

template <typename M>
inline uvcpp_db_value base_to_db_value(const void* field) {
  return uvcpp_orm_to_db_value(*static_cast<const M*>(field));
}

template <typename M>
inline uvcpp_db_value nullable_to_db_value(const void* field) {
  return uvcpp_orm_to_db_value(*static_cast<const nullable<M>*>(field));
}

/// 模型字段 → 绑定参数。`nullable<T>` 的空态是 NULL。
inline uvcpp_db_value column_to_db_value(const uvcpp_orm_column& column,
                                         const void* field) {
#define UVCPP_ORM_DETAIL_TO_DB_CASE(tag, cpp_type)                    \
  case uvcpp_orm_type::tag:                                           \
    return column.is_nullable()                                       \
               ? nullable_to_db_value<cpp_type>(field)                \
               : base_to_db_value<cpp_type>(field);

  switch (column.type()) {
    UVCPP_ORM_DETAIL_TO_DB_CASE(BOOL, bool)
    UVCPP_ORM_DETAIL_TO_DB_CASE(INT8, std::int8_t)
    UVCPP_ORM_DETAIL_TO_DB_CASE(UINT8, std::uint8_t)
    UVCPP_ORM_DETAIL_TO_DB_CASE(INT16, std::int16_t)
    UVCPP_ORM_DETAIL_TO_DB_CASE(UINT16, std::uint16_t)
    UVCPP_ORM_DETAIL_TO_DB_CASE(INT32, std::int32_t)
    UVCPP_ORM_DETAIL_TO_DB_CASE(UINT32, std::uint32_t)
    UVCPP_ORM_DETAIL_TO_DB_CASE(INT64, std::int64_t)
    UVCPP_ORM_DETAIL_TO_DB_CASE(UINT64, std::uint64_t)
    UVCPP_ORM_DETAIL_TO_DB_CASE(FLOAT, float)
    UVCPP_ORM_DETAIL_TO_DB_CASE(DOUBLE, double)
    UVCPP_ORM_DETAIL_TO_DB_CASE(TEXT, std::string)
    UVCPP_ORM_DETAIL_TO_DB_CASE(BLOB, std::vector<std::uint8_t>)

    case uvcpp_orm_type::DATE:
    case uvcpp_orm_type::TIME:
    case uvcpp_orm_type::DATETIME:
    case uvcpp_orm_type::UNKNOWN:
      return uvcpp_db_value();
  }
#undef UVCPP_ORM_DETAIL_TO_DB_CASE
  return uvcpp_db_value();
}

template <typename T>
inline void* column_address(T* object, const uvcpp_orm_column& column) {
  return reinterpret_cast<unsigned char*>(object) + column.offset();
}

template <typename T>
inline const void* column_address(const T* object,
                                  const uvcpp_orm_column& column) {
  return reinterpret_cast<const unsigned char*>(object) + column.offset();
}

// ---------------------------------------------------------------------------
// SQL 构造（同步 CRUD 与异步门面共用）
// ---------------------------------------------------------------------------
//
// 这一节只把“模型 + 参数”变成 #90 的 `uvcpp_orm_sql_result`，不碰数据库。
// 同步路径拿它执行；异步路径拿它在**投递之前**构造 SQL（回调线程只做映射）。
//
// **SQL 文本缓存。** 同一模型/方言的 SELECT/INSERT/UPDATE/DELETE 形状只由
// 元数据决定，值每次不同。缓存 SQL 文本（不是结果、也不是参数）把点查/更新
// 热路径上的字符串拼接与列查找省掉；参数仍然逐次构造并绑定，线程安全由
// C++11 的函数内 static 初始化保证。

template <typename T>
inline std::string build_pk_select_sql(uvcpp_orm_dialect dialect) {
  const uvcpp_orm_column* primary_key = model_traits<T>::primary_key();
  if (primary_key == nullptr) return std::string();
  const uvcpp_orm_sql_result result =
      select<T>(dialect)
          .where_eq(primary_key->column_name(), uvcpp_db_value())
          .limit(1)
          .build();
  return result.ok() ? result.sql : std::string();
}

template <typename T>
inline const std::string& cached_pk_select_sql(uvcpp_orm_dialect dialect) {
  static const std::string sqlite =
      build_pk_select_sql<T>(uvcpp_orm_dialect::sqlite);
  static const std::string mysql =
      build_pk_select_sql<T>(uvcpp_orm_dialect::mysql);
  static const std::string postgresql =
      build_pk_select_sql<T>(uvcpp_orm_dialect::postgresql);
  switch (dialect) {
    case uvcpp_orm_dialect::sqlite: return sqlite;
    case uvcpp_orm_dialect::mysql: return mysql;
    case uvcpp_orm_dialect::postgresql: return postgresql;
  }
  static const std::string empty;
  return empty;
}

template <typename T>
inline std::string build_insert_sql(uvcpp_orm_dialect dialect) {
  uvcpp_orm_insert_builder<T> builder(dialect);
  const uvcpp_orm_column* columns = model_traits<T>::columns();
  const std::size_t count = model_traits<T>::column_count();
  for (std::size_t i = 0; i < count; ++i) {
    const uvcpp_orm_column& column = columns[i];
    if (column.is_auto_increment() || column.is_read_only()) continue;
    builder.column(column.column_name(), uvcpp_db_value());
  }
  const uvcpp_orm_sql_result result = builder.build();
  return result.ok() ? result.sql : std::string();
}

template <typename T>
inline const std::string& cached_insert_sql(uvcpp_orm_dialect dialect) {
  static const std::string sqlite =
      build_insert_sql<T>(uvcpp_orm_dialect::sqlite);
  static const std::string mysql =
      build_insert_sql<T>(uvcpp_orm_dialect::mysql);
  static const std::string postgresql =
      build_insert_sql<T>(uvcpp_orm_dialect::postgresql);
  switch (dialect) {
    case uvcpp_orm_dialect::sqlite: return sqlite;
    case uvcpp_orm_dialect::mysql: return mysql;
    case uvcpp_orm_dialect::postgresql: return postgresql;
  }
  static const std::string empty;
  return empty;
}

/// 模型是否有自增列。没有自增列的 INSERT 不应该调用 backend 的
/// `last_insert_id`：PostgreSQL 的 `lastval()` 在没有序列时会报错并**中止
/// 当前事务**，即使调用方忽略了返回值，后续 commit 也只会回滚。
template <typename T>
inline bool model_has_auto_increment() {
  const uvcpp_orm_column* columns = model_traits<T>::columns();
  const std::size_t count = model_traits<T>::column_count();
  for (std::size_t i = 0; i < count; ++i) {
    if (columns[i].is_auto_increment()) return true;
  }
  return false;
}

template <typename T>
inline std::string build_update_sql(uvcpp_orm_dialect dialect) {
  const uvcpp_orm_column* primary_key = model_traits<T>::primary_key();
  if (primary_key == nullptr) return std::string();
  uvcpp_orm_update_builder<T> builder(dialect);
  const uvcpp_orm_column* columns = model_traits<T>::columns();
  const std::size_t count = model_traits<T>::column_count();
  for (std::size_t i = 0; i < count; ++i) {
    const uvcpp_orm_column& column = columns[i];
    if (column.is_primary_key() || column.is_read_only()) continue;
    builder.set(column.column_name(), uvcpp_db_value());
  }
  builder.where_eq(primary_key->column_name(), uvcpp_db_value());
  const uvcpp_orm_sql_result result = builder.build();
  return result.ok() ? result.sql : std::string();
}

template <typename T>
inline const std::string& cached_update_sql(uvcpp_orm_dialect dialect) {
  static const std::string sqlite =
      build_update_sql<T>(uvcpp_orm_dialect::sqlite);
  static const std::string mysql =
      build_update_sql<T>(uvcpp_orm_dialect::mysql);
  static const std::string postgresql =
      build_update_sql<T>(uvcpp_orm_dialect::postgresql);
  switch (dialect) {
    case uvcpp_orm_dialect::sqlite: return sqlite;
    case uvcpp_orm_dialect::mysql: return mysql;
    case uvcpp_orm_dialect::postgresql: return postgresql;
  }
  static const std::string empty;
  return empty;
}

template <typename T>
inline std::string build_remove_sql(uvcpp_orm_dialect dialect) {
  const uvcpp_orm_column* primary_key = model_traits<T>::primary_key();
  if (primary_key == nullptr) return std::string();
  const uvcpp_orm_sql_result result =
      delete_from<T>(dialect)
          .where_eq(primary_key->column_name(), uvcpp_db_value())
          .build();
  return result.ok() ? result.sql : std::string();
}

template <typename T>
inline const std::string& cached_remove_sql(uvcpp_orm_dialect dialect) {
  static const std::string sqlite =
      build_remove_sql<T>(uvcpp_orm_dialect::sqlite);
  static const std::string mysql =
      build_remove_sql<T>(uvcpp_orm_dialect::mysql);
  static const std::string postgresql =
      build_remove_sql<T>(uvcpp_orm_dialect::postgresql);
  switch (dialect) {
    case uvcpp_orm_dialect::sqlite: return sqlite;
    case uvcpp_orm_dialect::mysql: return mysql;
    case uvcpp_orm_dialect::postgresql: return postgresql;
  }
  static const std::string empty;
  return empty;
}

/// `SELECT ... WHERE pk = ? LIMIT 1`。
template <typename T>
inline uvcpp_orm_status make_find_by_pk_sql(uvcpp_orm_dialect dialect,
                                            const uvcpp_db_value& id,
                                            uvcpp_orm_sql_result* out,
                                            std::string* error) {
  if (out == nullptr) {
    return set_invalid_argument(error,
                                "find_by_pk: SQL result must not be null");
  }
  *out = uvcpp_orm_sql_result();
  if (id.is_null()) {
    return set_invalid_argument(error,
                                "find_by_pk: primary key must not be NULL");
  }
  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  const uvcpp_orm_column* primary_key = model_traits<T>::primary_key();
  if (primary_key == nullptr) {
    return set_error(error, uvcpp_orm_status::NO_PRIMARY_KEY,
                     "find_by_pk: model has no primary key");
  }

  const std::string& sql = cached_pk_select_sql<T>(dialect);
  if (sql.empty()) {
    return set_error(error, uvcpp_orm_status::MAPPING_FAILED,
                     "find_by_pk: could not cache the SELECT shape");
  }
  out->dialect = dialect;
  out->sql = sql;
  out->params.add(id).add(std::int64_t(1));
  return uvcpp_orm_status::OK;
}

/// `INSERT INTO ... (列...) VALUES (...)`。自增列与 `read_only` 列不进入。
template <typename T>
inline uvcpp_orm_status make_insert_sql(uvcpp_orm_dialect dialect,
                                        const T& value,
                                        uvcpp_orm_sql_result* out,
                                        std::string* error) {
  if (out == nullptr) {
    return set_invalid_argument(error, "insert: SQL result must not be null");
  }
  *out = uvcpp_orm_sql_result();
  const uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  const std::string& sql = cached_insert_sql<T>(dialect);
  if (sql.empty()) {
    return set_error(error, uvcpp_orm_status::EMPTY_COLUMN_LIST,
                     "insert: model has no writable columns");
  }
  out->dialect = dialect;
  out->sql = sql;
  const uvcpp_orm_column* columns = model_traits<T>::columns();
  const std::size_t count = model_traits<T>::column_count();
  for (std::size_t i = 0; i < count; ++i) {
    const uvcpp_orm_column& column = columns[i];
    if (column.is_auto_increment() || column.is_read_only()) continue;
    out->params.add(column_to_db_value(column, column_address(&value, column)));
  }
  return uvcpp_orm_status::OK;
}

/// `UPDATE ... SET ... WHERE pk = ?`。SET 里不含主键与 `read_only` 列。
template <typename T>
inline uvcpp_orm_status make_update_sql(uvcpp_orm_dialect dialect,
                                        const T& value,
                                        uvcpp_orm_sql_result* out,
                                        std::string* error) {
  if (out == nullptr) {
    return set_invalid_argument(error, "update: SQL result must not be null");
  }
  *out = uvcpp_orm_sql_result();
  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  const uvcpp_orm_column* primary_key = model_traits<T>::primary_key();
  if (primary_key == nullptr) {
    return set_error(error, uvcpp_orm_status::NO_PRIMARY_KEY,
                     "update: model has no primary key");
  }

  const uvcpp_db_value primary_key_value =
      column_to_db_value(*primary_key,
                         column_address(&value, *primary_key));
  if (primary_key_value.is_null()) {
    return set_invalid_argument(
        error, "update: primary key is NULL; refusing to build WHERE " +
                   std::string(primary_key->column_name()) + " = NULL");
  }

  const std::string& sql = cached_update_sql<T>(dialect);
  if (sql.empty()) {
    return set_error(error, uvcpp_orm_status::EMPTY_SET,
                     "update: model has no writable columns");
  }
  out->dialect = dialect;
  out->sql = sql;
  const uvcpp_orm_column* columns = model_traits<T>::columns();
  const std::size_t count = model_traits<T>::column_count();
  for (std::size_t i = 0; i < count; ++i) {
    const uvcpp_orm_column& column = columns[i];
    if (column.is_primary_key() || column.is_read_only()) continue;
    out->params.add(column_to_db_value(column, column_address(&value, column)));
  }
  out->params.add(primary_key_value);
  return uvcpp_orm_status::OK;
}

/// `DELETE FROM ... WHERE pk = ?`。
template <typename T>
inline uvcpp_orm_status make_remove_sql(uvcpp_orm_dialect dialect,
                                        const uvcpp_db_value& id,
                                        uvcpp_orm_sql_result* out,
                                        std::string* error) {
  if (out == nullptr) {
    return set_invalid_argument(error, "remove: SQL result must not be null");
  }
  *out = uvcpp_orm_sql_result();
  if (id.is_null()) {
    return set_invalid_argument(error,
                                "remove: primary key must not be NULL");
  }
  const uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  const uvcpp_orm_column* primary_key = model_traits<T>::primary_key();
  if (primary_key == nullptr) {
    return set_error(error, uvcpp_orm_status::NO_PRIMARY_KEY,
                     "remove: model has no primary key");
  }

  const std::string& sql = cached_remove_sql<T>(dialect);
  if (sql.empty()) {
    return set_error(error, uvcpp_orm_status::MAPPING_FAILED,
                     "remove: could not cache the DELETE shape");
  }
  out->dialect = dialect;
  out->sql = sql;
  out->params.add(id);
  return uvcpp_orm_status::OK;
}

/// `SELECT COUNT(*) ...`（保留 WHERE，去掉 LIMIT/OFFSET/ORDER BY）。
template <typename T>
inline uvcpp_orm_status make_count_sql(
    const uvcpp_orm_select_builder<T>& builder, uvcpp_orm_sql_result* out,
    std::string* error) {
  if (out == nullptr) {
    return set_invalid_argument(error, "count: SQL result must not be null");
  }
  *out = uvcpp_orm_sql_result();
  const uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  *out = builder.build_count();
  if (!out->ok()) {
    if (error != nullptr) *error = out->error;
    return out->status;
  }
  return uvcpp_orm_status::OK;
}

// ---------------------------------------------------------------------------
// 结果集映射
// ---------------------------------------------------------------------------

typedef bool (*column_assign_fn)(const uvcpp_db_value&, void*);
typedef void (*column_reset_fn)(void*);

template <typename M>
inline bool assign_base_field(const uvcpp_db_value& value, void* field) {
  return base_mapper<M>::assign(value, static_cast<M*>(field));
}

template <typename M>
inline bool assign_nullable_field(const uvcpp_db_value& value, void* field) {
  return assign_nullable_value<M>(value, field);
}

template <typename M>
inline void reset_nullable_field(void* field) {
  static_cast<nullable<M>*>(field)->reset();
}

/// 每个字段的转换函数只按模型选择一次；逐行映射直接调用函数指针，避免在
/// 每行每个字段上重复做类型 switch。
inline column_assign_fn column_assigner(uvcpp_orm_type type, bool nullable) {
#define UVCPP_ORM_DETAIL_ASSIGNER_CASE(tag, cpp_type)                    \
  case uvcpp_orm_type::tag:                                              \
    return nullable ? &assign_nullable_field<cpp_type>                   \
                    : &assign_base_field<cpp_type>;
  switch (type) {
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(BOOL, bool)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(INT8, std::int8_t)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(UINT8, std::uint8_t)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(INT16, std::int16_t)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(UINT16, std::uint16_t)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(INT32, std::int32_t)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(UINT32, std::uint32_t)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(INT64, std::int64_t)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(UINT64, std::uint64_t)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(FLOAT, float)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(DOUBLE, double)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(TEXT, std::string)
    UVCPP_ORM_DETAIL_ASSIGNER_CASE(BLOB, std::vector<std::uint8_t>)
    case uvcpp_orm_type::DATE:
    case uvcpp_orm_type::TIME:
    case uvcpp_orm_type::DATETIME:
    case uvcpp_orm_type::UNKNOWN:
      return nullptr;
  }
#undef UVCPP_ORM_DETAIL_ASSIGNER_CASE
  return nullptr;
}

inline column_reset_fn column_resetter(uvcpp_orm_type type, bool nullable) {
  if (!nullable) return nullptr;
#define UVCPP_ORM_DETAIL_RESETTER_CASE(tag, cpp_type) \
  case uvcpp_orm_type::tag: return &reset_nullable_field<cpp_type>;
  switch (type) {
    UVCPP_ORM_DETAIL_RESETTER_CASE(BOOL, bool)
    UVCPP_ORM_DETAIL_RESETTER_CASE(INT8, std::int8_t)
    UVCPP_ORM_DETAIL_RESETTER_CASE(UINT8, std::uint8_t)
    UVCPP_ORM_DETAIL_RESETTER_CASE(INT16, std::int16_t)
    UVCPP_ORM_DETAIL_RESETTER_CASE(UINT16, std::uint16_t)
    UVCPP_ORM_DETAIL_RESETTER_CASE(INT32, std::int32_t)
    UVCPP_ORM_DETAIL_RESETTER_CASE(UINT32, std::uint32_t)
    UVCPP_ORM_DETAIL_RESETTER_CASE(INT64, std::int64_t)
    UVCPP_ORM_DETAIL_RESETTER_CASE(UINT64, std::uint64_t)
    UVCPP_ORM_DETAIL_RESETTER_CASE(FLOAT, float)
    UVCPP_ORM_DETAIL_RESETTER_CASE(DOUBLE, double)
    UVCPP_ORM_DETAIL_RESETTER_CASE(TEXT, std::string)
    UVCPP_ORM_DETAIL_RESETTER_CASE(BLOB, std::vector<std::uint8_t>)
    case uvcpp_orm_type::DATE:
    case uvcpp_orm_type::TIME:
    case uvcpp_orm_type::DATETIME:
    case uvcpp_orm_type::UNKNOWN:
      return nullptr;
  }
#undef UVCPP_ORM_DETAIL_RESETTER_CASE
  return nullptr;
}

struct column_binding {
  column_binding()
      : column(nullptr),
        index(0u),
        present(false),
        assign(nullptr),
        reset(nullptr) {}

  const uvcpp_orm_column* column;
  std::size_t index;
  bool present;
  column_assign_fn assign;
  column_reset_fn reset;
};

/// 按列名把模型字段与结果集列对起来。缺非空列在**映射任何一行之前**就报错，
/// 零行结果集也不例外：结果集形状不对是 SQL 的问题，不是“没有数据”。
template <typename T>
inline uvcpp_orm_status prepare_bindings(
    const uvcpp_db_table& table, std::vector<column_binding>* bindings,
    std::string* error) {
  bindings->clear();
  const uvcpp_orm_column* columns = model_traits<T>::columns();
  const std::size_t count = model_traits<T>::column_count();
  for (std::size_t i = 0; i < count; ++i) {
    const uvcpp_orm_column& column = columns[i];
    column_binding binding;
    binding.column = &column;
    binding.assign = column_assigner(column.type(), column.is_nullable());
    binding.reset = column_resetter(column.type(), column.is_nullable());

    const char* raw_name = column.column_name();
    const std::string name = raw_name != nullptr ? raw_name : "";
    const std::size_t index = table.column_index(name);
    if (index == std::string::npos) {
      if (!column.is_nullable()) {
        return set_error(
            error, uvcpp_orm_status::MISSING_COLUMN,
            "result set is missing " + column_label(column) +
                ": the field is not nullable");
      }
      binding.present = false;
    } else {
      binding.present = true;
      binding.index = index;
    }
    bindings->push_back(binding);
  }
  return uvcpp_orm_status::OK;
}

/// 映射一行。先写进临时 `T`，全部成功才赋值给 `out`。
template <typename T>
inline uvcpp_orm_status map_row(const uvcpp_db_row& row,
                                const std::vector<column_binding>& bindings,
                                std::size_t row_index, T* out,
                                std::string* error) {
  T mapped = T();
  // 行号只在出错时格式化；热路径上的每一行都不该为一条不会用到的错误串
  // 付一次 std::string 分配。
  struct row_label_text {
    std::size_t index;
    std::string str() const {
      return std::string("row ") + std::to_string(index) + ": ";
    }
  };
  const row_label_text row_label = {row_index};

  for (std::size_t i = 0; i < bindings.size(); ++i) {
    const column_binding& binding = bindings[i];
    void* field = column_address(&mapped, *binding.column);

    if (!binding.present) {
      // prepare_bindings() 已经保证只有可空字段会走到这里。
      if (binding.reset != nullptr) binding.reset(field);
      continue;
    }

    const uvcpp_db_value& value = row[binding.index];
    if (value.is_null()) {
      if (!binding.column->is_nullable()) {
        return set_error(error, uvcpp_orm_status::NULL_VIOLATION,
                         row_label.str() + column_label(*binding.column) +
                             " is SQL NULL but the field is not nullable");
      }
      if (binding.reset != nullptr) binding.reset(field);
      continue;
    }

    if (!db_type_compatible(binding.column->type(), value.type())) {
      return set_error(
          error, uvcpp_orm_status::TYPE_MISMATCH,
          row_label.str() + column_label(*binding.column) +
              " cannot map database type \"" + value.type_name() +
              "\" into ORM type \"" +
              uvcpp_orm_type_name(binding.column->type()) + "\"");
    }

    if (binding.assign == nullptr || !binding.assign(value, field)) {
      return set_error(
          error, uvcpp_orm_status::TYPE_MISMATCH,
          row_label.str() + column_label(*binding.column) +
              " value is out of range for ORM type \"" +
              uvcpp_orm_type_name(binding.column->type()) + "\"");
    }
  }

  *out = std::move(mapped);
  return uvcpp_orm_status::OK;
}

/// 整表映射。任何一行失败都清空本地结果，不返回半张表。
template <typename T>
inline uvcpp_orm_status map_table(const uvcpp_db_table& table,
                                  std::vector<T>* out, std::string* error) {
  std::vector<column_binding> bindings;
  uvcpp_orm_status status = prepare_bindings<T>(table, &bindings, error);
  if (status != uvcpp_orm_status::OK) return status;

  std::vector<T> rows;
  rows.reserve(table.row_count());
  for (std::size_t i = 0; i < table.row_count(); ++i) {
    T row;
    status = map_row(table.rows()[i], bindings, i, &row, error);
    if (status != uvcpp_orm_status::OK) return status;
    rows.push_back(std::move(row));
  }

  *out = std::move(rows);
  return uvcpp_orm_status::OK;
}

// ---------------------------------------------------------------------------
// COUNT
// ---------------------------------------------------------------------------

/// 执行一条 COUNT(*) 查询并读出第一行第一列。
template <typename Executor>
inline uvcpp_orm_status execute_count_query(
    Executor& db, const uvcpp_orm_sql_result& count_sql,
    int64_t* total, std::string* error, uvcpp_db_status* db_status) {
  uvcpp_db_table table;
  const uvcpp_db_status status =
      db.query(count_sql.sql, count_sql.params, &table);
  if (!uvcpp_db_ok(status)) {
    return set_db_error(db.last_error(), status, "count query", error,
                        db_status);
  }
  if (table.row_count() == 0u || table.column_count() == 0u) {
    return set_error(error, uvcpp_orm_status::MAPPING_FAILED,
                     "COUNT(*) query returned no value");
  }
  const uvcpp_db_value& value = table.at(0u, 0u);
  if (value.is_null()) {
    return set_error(error, uvcpp_orm_status::MAPPING_FAILED,
                     "COUNT(*) query returned NULL");
  }
  *total = value.to_int64();
  return uvcpp_orm_status::OK;
}

// ---- 从已构造 SELECT 推导 COUNT -----------------------------------------

inline bool token_matches_at(const std::string& sql, std::size_t position,
                             const char* token) {
  std::size_t i = 0u;
  while (token[i] != '\0') {
    if (position + i >= sql.size()) return false;
    const unsigned char a = static_cast<unsigned char>(sql[position + i]);
    const unsigned char b = static_cast<unsigned char>(token[i]);
    if (ascii_lower(a) != ascii_lower(b)) return false;
    ++i;
  }
  return true;
}

/// 在“引号/注释之外”找关键字（大小写不敏感）。这是给本层自己生成的 SQL
/// 用的有限扫描，不是完整的 SQL 解析器：PostgreSQL 的 `$tag$...$tag$` 字符串
/// 与非标准方言的转义不在覆盖范围内。
inline std::size_t find_top_level_token(const std::string& sql,
                                        std::size_t start,
                                        const char* token,
                                        uvcpp_orm_dialect dialect) {
  enum scan_state {
    normal,
    single_quote,
    double_quote,
    backtick,
    line_comment,
    block_comment
  };
  scan_state state = normal;

  for (std::size_t i = start; i < sql.size(); ++i) {
    const char c = sql[i];
    const char next = (i + 1u < sql.size()) ? sql[i + 1u] : '\0';

    switch (state) {
      case single_quote:
        if (c == '\'') {
          if (next == '\'') {
            ++i;
          } else {
            state = normal;
          }
        }
        break;
      case double_quote:
        if (c == '"') {
          if (next == '"') {
            ++i;
          } else {
            state = normal;
          }
        }
        break;
      case backtick:
        if (c == '`') {
          if (next == '`') {
            ++i;
          } else {
            state = normal;
          }
        }
        break;
      case line_comment:
        if (c == '\n') state = normal;
        break;
      case block_comment:
        if (c == '*' && next == '/') {
          state = normal;
          ++i;
        }
        break;
      case normal:
      default:
        if (token_matches_at(sql, i, token)) return i;
        if (c == '\'') {
          state = single_quote;
        } else if (c == '"') {
          state = double_quote;
        } else if (c == '`') {
          state = backtick;
        } else if (c == '-' && next == '-') {
          state = line_comment;
          ++i;
        } else if (c == '/' && next == '*') {
          state = block_comment;
          ++i;
        } else if (dialect == uvcpp_orm_dialect::mysql && c == '#') {
          state = line_comment;
        }
        break;
    }
  }
  return std::string::npos;
}

/// 从一条 SELECT 推导 `SELECT COUNT(*) FROM ... [WHERE ...]`：去掉
/// ORDER BY / LIMIT / OFFSET，只保留 FROM 与 WHERE。
///
/// 参数只取 WHERE 那一段（生成器把 WHERE 参数放在最前面），所以 COUNT 查询
/// 的参数是 SELECT 参数的前缀。选择列表里若也有占位符，前缀就不再可靠，
/// 这种情况直接拒绝。
inline uvcpp_orm_status derive_count_query(
    const uvcpp_orm_sql_result& select_sql, uvcpp_orm_sql_result* count_sql,
    std::string* error) {
  if (!uvcpp_orm_dialect_is_valid(select_sql.dialect)) {
    return set_error(error, uvcpp_orm_status::INVALID_DIALECT,
                     "cannot derive COUNT(*) for an unknown SQL dialect");
  }

  const std::size_t from_position =
      find_top_level_token(select_sql.sql, 0u, " FROM ", select_sql.dialect);
  if (from_position == std::string::npos) {
    return set_invalid_argument(
        error,
        "find_all: total requires a single-table SELECT with a FROM clause");
  }

  // 选择列表里的占位符会让“参数前缀”这个前提不成立。
  std::size_t prefix_questions = 0u;
  std::vector<std::size_t> prefix_dollars;
  scan_raw_placeholders(select_sql.sql.substr(0u, from_position),
                        select_sql.dialect, &prefix_questions, &prefix_dollars);
  if (prefix_questions != 0u || !prefix_dollars.empty()) {
    return set_invalid_argument(
        error,
        "find_all: total cannot be derived when the SELECT list contains "
        "placeholders; pass a builder or an explicit COUNT query instead");
  }

  std::size_t end_position = select_sql.sql.size();
  const char* terminators[] = {" ORDER BY ", " LIMIT ", " OFFSET "};
  for (std::size_t i = 0;
       i < sizeof(terminators) / sizeof(terminators[0]); ++i) {
    const std::size_t position = find_top_level_token(
        select_sql.sql, from_position, terminators[i], select_sql.dialect);
    if (position != std::string::npos && position < end_position) {
      end_position = position;
    }
  }

  std::string sql = "SELECT COUNT(*)";
  sql += select_sql.sql.substr(from_position, end_position - from_position);

  std::size_t questions = 0u;
  std::vector<std::size_t> dollars;
  scan_raw_placeholders(sql, select_sql.dialect, &questions, &dollars);

  std::size_t expected = 0u;
  if (select_sql.dialect == uvcpp_orm_dialect::postgresql) {
    if (!contiguous_dollar_placeholders(dollars, &expected)) {
      return set_error(
          error, uvcpp_orm_status::PARAM_COUNT_MISMATCH,
          "find_all: COUNT(*) placeholders are not a contiguous $1..$n "
          "sequence");
    }
  } else {
    expected = questions;
  }

  if (expected > select_sql.params.size()) {
    return set_error(
        error, uvcpp_orm_status::PARAM_COUNT_MISMATCH,
        "find_all: COUNT(*) needs " + std::to_string(expected) +
            " parameter(s) but the SELECT has only " +
            std::to_string(select_sql.params.size()));
  }

  count_sql->status = uvcpp_orm_status::OK;
  count_sql->error.clear();
  count_sql->dialect = select_sql.dialect;
  count_sql->sql = sql;
  count_sql->params = uvcpp_db_params();
  for (std::size_t i = 0; i < expected; ++i) {
    count_sql->params.add(select_sql.params[i]);
  }
  return uvcpp_orm_status::OK;
}

// ---------------------------------------------------------------------------
// 公共执行骨架
// ---------------------------------------------------------------------------

/// 同步/事务/池共用的 `find_by_pk` 骨架。
template <typename T, typename Executor>
inline uvcpp_orm_status find_by_pk_impl(Executor& db,
                                        const uvcpp_db_value& id, T* out,
                                        std::string* error,
                                        uvcpp_db_status* db_status) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;

  if (out == nullptr) {
    return set_invalid_argument(error, "find_by_pk: out must not be null");
  }
  if (!db.valid()) {
    return db.invalid_error("find_by_pk", error, db_status);
  }
  if (id.is_null()) {
    return set_invalid_argument(error,
                                "find_by_pk: primary key must not be NULL");
  }

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!dialect_from_executor(db, &dialect)) {
    return set_no_driver_error(db, "find_by_pk", error, db_status);
  }

  uvcpp_orm_sql_result sql_result;
  status = make_find_by_pk_sql<T>(dialect, id, &sql_result, error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_db_table table;
  const uvcpp_db_status database_status =
      db.query(sql_result.sql, sql_result.params, &table);
  if (!uvcpp_db_ok(database_status)) {
    return set_db_error(db.last_error(), database_status, "find_by_pk query",
                        error, db_status);
  }
  if (table.row_count() == 0u) {
    return set_error(
        error, uvcpp_orm_status::NOT_FOUND,
        std::string("find_by_pk: no row in table \"") +
            model_traits<T>::table_name() + "\" matches the primary key");
  }

  std::vector<column_binding> bindings;
  status = prepare_bindings<T>(table, &bindings, error);
  if (status != uvcpp_orm_status::OK) return status;
  return map_row(table.rows()[0], bindings, 0u, out, error);
}

template <typename T, typename Executor>
inline uvcpp_orm_status find_all_impl(
    Executor& db, const uvcpp_orm_sql_result& select_sql,
    const uvcpp_orm_sql_result* count_sql, std::vector<T>* out,
    std::string* error, int64_t* total, uvcpp_db_status* db_status) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;

  if (out == nullptr) {
    return set_invalid_argument(error, "find_all: out must not be null");
  }
  if (!db.valid()) {
    return db.invalid_error("find_all", error, db_status);
  }
  if (!select_sql.ok()) {
    if (error != nullptr) *error = select_sql.error;
    return select_sql.status;
  }
  if (select_sql.sql.empty()) {
    return set_invalid_argument(error, "find_all: SQL is empty");
  }
  if (total != nullptr && count_sql == nullptr) {
    return set_invalid_argument(
        error, "find_all: a COUNT query is required when total is requested");
  }

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!dialect_from_executor(db, &dialect)) {
    return set_no_driver_error(db, "find_all", error, db_status);
  }
  if (!dialect_matches(select_sql, dialect, error, "find_all")) {
    return uvcpp_orm_status::INVALID_ARGUMENT;
  }

  int64_t counted = 0;
  if (count_sql != nullptr) {
    if (!count_sql->ok()) {
      if (error != nullptr) *error = count_sql->error;
      return count_sql->status;
    }
    if (!dialect_matches(*count_sql, dialect, error, "find_all count")) {
      return uvcpp_orm_status::INVALID_ARGUMENT;
    }
    status = execute_count_query(db, *count_sql, &counted, error, db_status);
    if (status != uvcpp_orm_status::OK) return status;
  }

  uvcpp_db_table table;
  const uvcpp_db_status database_status =
      db.query(select_sql.sql, select_sql.params, &table);
  if (!uvcpp_db_ok(database_status)) {
    return set_db_error(db.last_error(), database_status, "find_all query",
                        error, db_status);
  }

  std::vector<T> rows;
  status = map_table<T>(table, &rows, error);
  if (status != uvcpp_orm_status::OK) return status;

  *out = std::move(rows);
  if (total != nullptr) *total = counted;
  return uvcpp_orm_status::OK;
}

template <typename T, typename Executor>
inline uvcpp_orm_status insert_impl(Executor& db, const T& value,
                                    uvcpp_db_value* new_id,
                                    std::string* error,
                                    uvcpp_db_status* db_status) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (new_id != nullptr) new_id->set_null();

  if (!db.valid()) {
    return db.invalid_error("insert", error, db_status);
  }

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!dialect_from_executor(db, &dialect)) {
    return set_no_driver_error(db, "insert", error, db_status);
  }

  uvcpp_orm_sql_result sql_result;
  status = make_insert_sql<T>(dialect, value, &sql_result, error);
  if (status != uvcpp_orm_status::OK) return status;

  if (new_id != nullptr && model_has_auto_increment<T>()) {
    const uvcpp_db_status database_status =
        db.insert(sql_result.sql, sql_result.params, new_id);
    if (!uvcpp_db_ok(database_status)) {
      new_id->set_null();
      return set_db_error(db.last_error(), database_status, "insert", error,
                          db_status);
    }
    return uvcpp_orm_status::OK;
  }

  const uvcpp_db_status database_status =
      db.execute(sql_result.sql, sql_result.params, nullptr);
  if (!uvcpp_db_ok(database_status)) {
    return set_db_error(db.last_error(), database_status, "insert", error,
                        db_status);
  }
  return uvcpp_orm_status::OK;
}

template <typename T, typename Executor>
inline uvcpp_orm_status update_impl(Executor& db, const T& value,
                                    int64_t* affected, std::string* error,
                                    uvcpp_db_status* db_status) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (affected != nullptr) *affected = -1;

  if (!db.valid()) {
    return db.invalid_error("update", error, db_status);
  }

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!dialect_from_executor(db, &dialect)) {
    return set_no_driver_error(db, "update", error, db_status);
  }

  uvcpp_orm_sql_result sql_result;
  status = make_update_sql<T>(dialect, value, &sql_result, error);
  if (status != uvcpp_orm_status::OK) return status;

  int64_t changed = -1;
  const uvcpp_db_status database_status =
      db.execute(sql_result.sql, sql_result.params, &changed);
  if (!uvcpp_db_ok(database_status)) {
    return set_db_error(db.last_error(), database_status, "update", error,
                        db_status);
  }

  if (affected != nullptr) *affected = changed;
  return uvcpp_orm_status::OK;
}

template <typename T, typename Executor>
inline uvcpp_orm_status remove_impl(Executor& db, const uvcpp_db_value& id,
                                    int64_t* affected, std::string* error,
                                    uvcpp_db_status* db_status) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (affected != nullptr) *affected = -1;

  if (!db.valid()) {
    return db.invalid_error("remove", error, db_status);
  }
  if (id.is_null()) {
    return set_invalid_argument(error,
                                "remove: primary key must not be NULL");
  }

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!dialect_from_executor(db, &dialect)) {
    return set_no_driver_error(db, "remove", error, db_status);
  }

  uvcpp_orm_sql_result sql_result;
  status = make_remove_sql<T>(dialect, id, &sql_result, error);
  if (status != uvcpp_orm_status::OK) return status;

  int64_t changed = -1;
  const uvcpp_db_status database_status =
      db.execute(sql_result.sql, sql_result.params, &changed);
  if (!uvcpp_db_ok(database_status)) {
    return set_db_error(db.last_error(), database_status, "remove", error,
                        db_status);
  }

  if (affected != nullptr) *affected = changed;
  return uvcpp_orm_status::OK;
}

template <typename T, typename Executor>
inline uvcpp_orm_status count_impl(
    Executor& db, const uvcpp_orm_select_builder<T>& builder, int64_t* total,
    std::string* error, uvcpp_db_status* db_status) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (total == nullptr) {
    return set_invalid_argument(error, "count: total must not be null");
  }
  if (!db.valid()) {
    return db.invalid_error("count", error, db_status);
  }

  // 与 #91 的错误顺序一致：先校验模型，再定方言，最后才构造 SQL。
  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!dialect_from_executor(db, &dialect)) {
    return set_no_driver_error(db, "count", error, db_status);
  }

  uvcpp_orm_sql_result count_sql;
  status = make_count_sql<T>(builder, &count_sql, error);
  if (status != uvcpp_orm_status::OK) return status;
  if (!dialect_matches(count_sql, dialect, error, "count")) {
    return uvcpp_orm_status::INVALID_ARGUMENT;
  }

  return execute_count_query(db, count_sql, total, error, db_status);
}

template <typename T, typename Executor>
inline uvcpp_orm_status count_impl(
    Executor& db, const uvcpp_orm_sql_result& count_sql, int64_t* total,
    std::string* error, uvcpp_db_status* db_status) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (total == nullptr) {
    return set_invalid_argument(error, "count: total must not be null");
  }
  if (!db.valid()) {
    return db.invalid_error("count", error, db_status);
  }
  if (!count_sql.ok()) {
    if (error != nullptr) *error = count_sql.error;
    return count_sql.status;
  }
  if (count_sql.sql.empty()) {
    return set_invalid_argument(error, "count: SQL is empty");
  }

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!dialect_from_executor(db, &dialect)) {
    return set_no_driver_error(db, "count", error, db_status);
  }
  if (!dialect_matches(count_sql, dialect, error, "count")) {
    return uvcpp_orm_status::INVALID_ARGUMENT;
  }

  return execute_count_query(db, count_sql, total, error, db_status);
}

/// `find_all` 的“已构造 SQL”入口：`total` 非空时从 SELECT 推导 COUNT。
template <typename T, typename Executor>
inline uvcpp_orm_status find_all_result_impl(
    Executor& db, const uvcpp_orm_sql_result& sql_result,
    std::vector<T>* out, std::string* error, int64_t* total,
    uvcpp_db_status* db_status) {
  if (total == nullptr) {
    return find_all_impl<T>(db, sql_result, nullptr, out, error, total,
                            db_status);
  }

  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (!db.valid()) {
    return db.invalid_error("find_all", error, db_status);
  }
  if (out == nullptr) {
    return set_invalid_argument(error, "find_all: out must not be null");
  }
  if (!sql_result.ok()) {
    if (error != nullptr) *error = sql_result.error;
    return sql_result.status;
  }

  uvcpp_orm_dialect dialect;
  if (!dialect_from_executor(db, &dialect)) {
    return set_no_driver_error(db, "find_all", error, db_status);
  }
  if (!dialect_matches(sql_result, dialect, error, "find_all")) {
    return uvcpp_orm_status::INVALID_ARGUMENT;
  }

  uvcpp_orm_sql_result count_sql;
  const uvcpp_orm_status derived =
      derive_count_query(sql_result, &count_sql, error);
  if (derived != uvcpp_orm_status::OK) return derived;

  return find_all_impl<T>(db, sql_result, &count_sql, out, error, total,
                          db_status);
}

/// `find_all` 的构造器入口：`total` 非空时用同一个 builder 生成 COUNT。
template <typename T, typename Executor>
inline uvcpp_orm_status find_all_builder_impl(
    Executor& db, const uvcpp_orm_select_builder<T>& builder,
    std::vector<T>* out, std::string* error, int64_t* total,
    uvcpp_db_status* db_status) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (!db.valid()) {
    return db.invalid_error("find_all", error, db_status);
  }
  if (out == nullptr) {
    return set_invalid_argument(error, "find_all: out must not be null");
  }

  const uvcpp_orm_sql_result select_sql = builder.build();
  if (!select_sql.ok()) {
    if (error != nullptr) *error = select_sql.error;
    return select_sql.status;
  }

  if (total == nullptr) {
    return find_all_impl<T>(db, select_sql, nullptr, out, error, total,
                            db_status);
  }

  const uvcpp_orm_sql_result count_sql = builder.build_count();
  if (!count_sql.ok()) {
    if (error != nullptr) *error = count_sql.error;
    return count_sql.status;
  }

  return find_all_impl<T>(db, select_sql, &count_sql, out, error, total,
                          db_status);
}

}  // namespace orm_detail

// ---------------------------------------------------------------------------
// 公开 API
// ---------------------------------------------------------------------------

/// 按主键查一行。没找到返回 `NOT_FOUND`，且不修改 `out`。
///
/// `db_status` 是可选的后端原始返回码出参：只有 `DB_ERROR` 会写它。
///
/// 三种执行器（同步连接 / 事务 / 池）共用同一套 SQL 构造与结果映射：
///
///   * `uvcpp_db_client&` —— #91 的路径，行为与错误文案不变；
///   * `uvcpp_db_tx&` —— 每条语句都过 `uvcpp_db_tx` 自己的门：事务内
///     第一条失败会被记住，后续语句直接返回同一个错误；ORM 不自己
///     begin/commit，提交/回滚/超时语义完全由 `uvcpp_db_tx` 保证；
///   * `uvcpp_db_pool&` —— 同步借还一条连接（`acquire()` 会阻塞，只给不在
///     事件循环上的调用方；异步请用 `uvcpp_orm_async.h`）。
template <typename T>
inline uvcpp_orm_status find_by_pk(uvcpp_db_client& db,
                                   const uvcpp_db_value& id, T* out,
                                   std::string* error = nullptr,
                                   uvcpp_db_status* db_status = nullptr) {
  orm_detail::client_executor executor(db);
  return orm_detail::find_by_pk_impl<T>(executor, id, out, error, db_status);
}

template <typename T>
inline uvcpp_orm_status find_by_pk(uvcpp_db_tx& tx,
                                   const uvcpp_db_value& id, T* out,
                                   std::string* error = nullptr,
                                   uvcpp_db_status* db_status = nullptr) {
  orm_detail::tx_executor executor(tx);
  return orm_detail::find_by_pk_impl<T>(executor, id, out, error, db_status);
}

template <typename T>
inline uvcpp_orm_status find_by_pk(uvcpp_db_pool& pool,
                                   const uvcpp_db_value& id, T* out,
                                   std::string* error = nullptr,
                                   uvcpp_db_status* db_status = nullptr) {
  orm_detail::pool_executor executor(pool);
  return orm_detail::find_by_pk_impl<T>(executor, id, out, error, db_status);
}

/// 执行一条**已构造**的 SELECT 并映射成模型。`total` 非空时额外执行一条
/// COUNT(*)；COUNT 从 SELECT 推导（见文件头“已知边界”）。
///
/// 失败时不修改 `out` / `total`。
template <typename T>
inline uvcpp_orm_status find_all(uvcpp_db_client& db,
                                 const uvcpp_orm_sql_result& sql_result,
                                 std::vector<T>* out,
                                 std::string* error = nullptr,
                                 int64_t* total = nullptr,
                                 uvcpp_db_status* db_status = nullptr) {
  orm_detail::client_executor executor(db);
  return orm_detail::find_all_result_impl<T>(executor, sql_result, out, error,
                                             total, db_status);
}

template <typename T>
inline uvcpp_orm_status find_all(uvcpp_db_tx& tx,
                                 const uvcpp_orm_sql_result& sql_result,
                                 std::vector<T>* out,
                                 std::string* error = nullptr,
                                 int64_t* total = nullptr,
                                 uvcpp_db_status* db_status = nullptr) {
  orm_detail::tx_executor executor(tx);
  return orm_detail::find_all_result_impl<T>(executor, sql_result, out, error,
                                             total, db_status);
}

template <typename T>
inline uvcpp_orm_status find_all(uvcpp_db_pool& pool,
                                 const uvcpp_orm_sql_result& sql_result,
                                 std::vector<T>* out,
                                 std::string* error = nullptr,
                                 int64_t* total = nullptr,
                                 uvcpp_db_status* db_status = nullptr) {
  orm_detail::pool_executor executor(pool);
  return orm_detail::find_all_result_impl<T>(executor, sql_result, out, error,
                                             total, db_status);
}

/// 执行一个 SELECT 构造器并映射成模型。`total` 非空时用同一个 WHERE 再生成
/// 一条 `SELECT COUNT(*)`（不受 LIMIT/OFFSET 影响）。
template <typename T>
inline uvcpp_orm_status find_all(uvcpp_db_client& db,
                                 const uvcpp_orm_select_builder<T>& builder,
                                 std::vector<T>* out,
                                 std::string* error = nullptr,
                                 int64_t* total = nullptr,
                                 uvcpp_db_status* db_status = nullptr) {
  orm_detail::client_executor executor(db);
  return orm_detail::find_all_builder_impl<T>(executor, builder, out, error,
                                              total, db_status);
}

template <typename T>
inline uvcpp_orm_status find_all(uvcpp_db_tx& tx,
                                 const uvcpp_orm_select_builder<T>& builder,
                                 std::vector<T>* out,
                                 std::string* error = nullptr,
                                 int64_t* total = nullptr,
                                 uvcpp_db_status* db_status = nullptr) {
  orm_detail::tx_executor executor(tx);
  return orm_detail::find_all_builder_impl<T>(executor, builder, out, error,
                                              total, db_status);
}

template <typename T>
inline uvcpp_orm_status find_all(uvcpp_db_pool& pool,
                                 const uvcpp_orm_select_builder<T>& builder,
                                 std::vector<T>* out,
                                 std::string* error = nullptr,
                                 int64_t* total = nullptr,
                                 uvcpp_db_status* db_status = nullptr) {
  orm_detail::pool_executor executor(pool);
  return orm_detail::find_all_builder_impl<T>(executor, builder, out, error,
                                              total, db_status);
}

/// 插入一行。自增列（以及 `read_only` 列）不进入 INSERT；`new_id` 非空时
/// 回填 `last_insert_id`（后端报不出时写 NULL 值，但返回码仍是 `OK`）。
template <typename T>
inline uvcpp_orm_status insert(uvcpp_db_client& db, const T& value,
                               uvcpp_db_value* new_id = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  orm_detail::client_executor executor(db);
  return orm_detail::insert_impl<T>(executor, value, new_id, error,
                                    db_status);
}

template <typename T>
inline uvcpp_orm_status insert(uvcpp_db_tx& tx, const T& value,
                               uvcpp_db_value* new_id = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  orm_detail::tx_executor executor(tx);
  return orm_detail::insert_impl<T>(executor, value, new_id, error,
                                    db_status);
}

template <typename T>
inline uvcpp_orm_status insert(uvcpp_db_pool& pool, const T& value,
                               uvcpp_db_value* new_id = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  orm_detail::pool_executor executor(pool);
  return orm_detail::insert_impl<T>(executor, value, new_id, error,
                                    db_status);
}

/// 按主键更新。SET 里不含主键（也不含 `read_only` 列）。受影响 0 行不是
/// 错误：返回 `OK`，`affected` 写 0；执行失败时 `affected` 写 -1。
template <typename T>
inline uvcpp_orm_status update(uvcpp_db_client& db, const T& value,
                               int64_t* affected = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  orm_detail::client_executor executor(db);
  return orm_detail::update_impl<T>(executor, value, affected, error,
                                    db_status);
}

template <typename T>
inline uvcpp_orm_status update(uvcpp_db_tx& tx, const T& value,
                               int64_t* affected = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  orm_detail::tx_executor executor(tx);
  return orm_detail::update_impl<T>(executor, value, affected, error,
                                    db_status);
}

template <typename T>
inline uvcpp_orm_status update(uvcpp_db_pool& pool, const T& value,
                               int64_t* affected = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  orm_detail::pool_executor executor(pool);
  return orm_detail::update_impl<T>(executor, value, affected, error,
                                    db_status);
}

/// 按主键删除。受影响 0 行不是错误（返回 `OK`、`affected` 0）。
template <typename T>
inline uvcpp_orm_status remove(uvcpp_db_client& db, const uvcpp_db_value& id,
                               int64_t* affected = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  orm_detail::client_executor executor(db);
  return orm_detail::remove_impl<T>(executor, id, affected, error, db_status);
}

template <typename T>
inline uvcpp_orm_status remove(uvcpp_db_tx& tx, const uvcpp_db_value& id,
                               int64_t* affected = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  orm_detail::tx_executor executor(tx);
  return orm_detail::remove_impl<T>(executor, id, affected, error, db_status);
}

template <typename T>
inline uvcpp_orm_status remove(uvcpp_db_pool& pool, const uvcpp_db_value& id,
                               int64_t* affected = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  orm_detail::pool_executor executor(pool);
  return orm_detail::remove_impl<T>(executor, id, affected, error, db_status);
}

/// 执行一条 COUNT 构造器。`total` 必须非空。
template <typename T>
inline uvcpp_orm_status count(uvcpp_db_client& db,
                              const uvcpp_orm_select_builder<T>& builder,
                              int64_t* total,
                              std::string* error = nullptr,
                              uvcpp_db_status* db_status = nullptr) {
  orm_detail::client_executor executor(db);
  return orm_detail::count_impl<T>(executor, builder, total, error,
                                   db_status);
}

template <typename T>
inline uvcpp_orm_status count(uvcpp_db_tx& tx,
                              const uvcpp_orm_select_builder<T>& builder,
                              int64_t* total,
                              std::string* error = nullptr,
                              uvcpp_db_status* db_status = nullptr) {
  orm_detail::tx_executor executor(tx);
  return orm_detail::count_impl<T>(executor, builder, total, error,
                                   db_status);
}

template <typename T>
inline uvcpp_orm_status count(uvcpp_db_pool& pool,
                              const uvcpp_orm_select_builder<T>& builder,
                              int64_t* total,
                              std::string* error = nullptr,
                              uvcpp_db_status* db_status = nullptr) {
  orm_detail::pool_executor executor(pool);
  return orm_detail::count_impl<T>(executor, builder, total, error,
                                   db_status);
}

/// 执行一条**已构造**的 COUNT 查询。`total` 必须非空。
template <typename T>
inline uvcpp_orm_status count(uvcpp_db_client& db,
                              const uvcpp_orm_sql_result& count_sql,
                              int64_t* total,
                              std::string* error = nullptr,
                              uvcpp_db_status* db_status = nullptr) {
  orm_detail::client_executor executor(db);
  return orm_detail::count_impl<T>(executor, count_sql, total, error,
                                   db_status);
}

template <typename T>
inline uvcpp_orm_status count(uvcpp_db_tx& tx,
                              const uvcpp_orm_sql_result& count_sql,
                              int64_t* total,
                              std::string* error = nullptr,
                              uvcpp_db_status* db_status = nullptr) {
  orm_detail::tx_executor executor(tx);
  return orm_detail::count_impl<T>(executor, count_sql, total, error,
                                   db_status);
}

template <typename T>
inline uvcpp_orm_status count(uvcpp_db_pool& pool,
                              const uvcpp_orm_sql_result& count_sql,
                              int64_t* total,
                              std::string* error = nullptr,
                              uvcpp_db_status* db_status = nullptr) {
  orm_detail::pool_executor executor(pool);
  return orm_detail::count_impl<T>(executor, count_sql, total, error,
                                   db_status);
}

}  // namespace uvcpp

#endif  // UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE
#endif  // SRC_ORM_UVCPP_ORM_REPOSITORY_H
