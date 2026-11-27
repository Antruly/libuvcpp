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
#include <db/uvcpp_db_status.h>
#include <db/uvcpp_db_table.h>
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
/// （`db.last_error()`，写进 `error`）。不要把所有失败压成一个字符串 ——
/// 唯一键冲突与“连接没打开”在调用方眼里是两种处置。
inline uvcpp_orm_status set_db_error(uvcpp_db_client& db,
                                     uvcpp_db_status status,
                                     const std::string& action,
                                     std::string* error,
                                     uvcpp_db_status* db_status) {
  if (db_status != nullptr) *db_status = status;
  if (error != nullptr) {
    *error = action + ": " + uvcpp_db_status_name(status) + ": " +
             db.last_error();
  }
  return uvcpp_orm_status::DB_ERROR;
}

/// 从**已打开**的同步连接上取 SQL 方言。驱动名不认识（或连接没开）时返回
/// false；调用方据此报 `DB_ERROR`，而不是猜一个方言。
inline bool dialect_from_client(const uvcpp_db_client& db,
                                uvcpp_orm_dialect* out) {
  if (out == nullptr) return false;
  const char* name = db.driver_name();
  if (name == nullptr || *name == '\0') return false;
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

/// 连接没有可用方言时的错误。`db_status` 尽量贴近真实原因：没 open 就是
/// `NOT_CONNECTED`，打开了一个本层不认识的驱动就是 `UNSUPPORTED`。
inline uvcpp_orm_status set_no_driver_error(const uvcpp_db_client& db,
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
// 结果集映射
// ---------------------------------------------------------------------------

struct column_binding {
  column_binding() : column(nullptr), index(0u), present(false) {}

  const uvcpp_orm_column* column;
  std::size_t index;
  bool present;
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
  const std::string row_label = "row " + std::to_string(row_index) + ": ";

  for (std::size_t i = 0; i < bindings.size(); ++i) {
    const column_binding& binding = bindings[i];
    void* field = column_address(&mapped, *binding.column);

    if (!binding.present) {
      // prepare_bindings() 已经保证只有可空字段会走到这里。
      reset_nullable_column(*binding.column, field);
      continue;
    }

    const uvcpp_db_value& value = row[binding.index];
    if (value.is_null()) {
      if (!binding.column->is_nullable()) {
        return set_error(error, uvcpp_orm_status::NULL_VIOLATION,
                         row_label + column_label(*binding.column) +
                             " is SQL NULL but the field is not nullable");
      }
      reset_nullable_column(*binding.column, field);
      continue;
    }

    if (!db_type_compatible(binding.column->type(), value.type())) {
      return set_error(
          error, uvcpp_orm_status::TYPE_MISMATCH,
          row_label + column_label(*binding.column) +
              " cannot map database type \"" + value.type_name() +
              "\" into ORM type \"" +
              uvcpp_orm_type_name(binding.column->type()) + "\"");
    }

    if (!assign_column_value(*binding.column, field, value)) {
      return set_error(
          error, uvcpp_orm_status::TYPE_MISMATCH,
          row_label + column_label(*binding.column) +
              " value is out of range for ORM type \"" +
              uvcpp_orm_type_name(binding.column->type()) + "\"");
    }
  }

  *out = mapped;
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
inline uvcpp_orm_status execute_count_query(
    uvcpp_db_client& db, const uvcpp_orm_sql_result& count_sql,
    int64_t* total, std::string* error, uvcpp_db_status* db_status) {
  uvcpp_db_table table;
  const uvcpp_db_status status =
      db.query(count_sql.sql, count_sql.params, &table);
  if (!uvcpp_db_ok(status)) {
    return set_db_error(db, status, "count query", error, db_status);
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

template <typename T>
inline uvcpp_orm_status find_all_impl(
    uvcpp_db_client& db, const uvcpp_orm_sql_result& select_sql,
    const uvcpp_orm_sql_result* count_sql, std::vector<T>* out,
    std::string* error, int64_t* total, uvcpp_db_status* db_status) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;

  if (out == nullptr) {
    return set_invalid_argument(error, "find_all: out must not be null");
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
  if (!dialect_from_client(db, &dialect)) {
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
    return set_db_error(db, database_status, "find_all query", error,
                        db_status);
  }

  std::vector<T> rows;
  status = map_table<T>(table, &rows, error);
  if (status != uvcpp_orm_status::OK) return status;

  *out = std::move(rows);
  if (total != nullptr) *total = counted;
  return uvcpp_orm_status::OK;
}

}  // namespace orm_detail

// ---------------------------------------------------------------------------
// 公开 API
// ---------------------------------------------------------------------------

/// 按主键查一行。没找到返回 `NOT_FOUND`，且不修改 `out`。
///
/// `db_status` 是可选的后端原始返回码出参：只有 `DB_ERROR` 会写它。
template <typename T>
inline uvcpp_orm_status find_by_pk(uvcpp_db_client& db,
                                   const uvcpp_db_value& id, T* out,
                                   std::string* error = nullptr,
                                   uvcpp_db_status* db_status = nullptr) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;

  if (out == nullptr) {
    return orm_detail::set_invalid_argument(
        error, "find_by_pk: out must not be null");
  }
  if (id.is_null()) {
    return orm_detail::set_invalid_argument(
        error, "find_by_pk: primary key must not be NULL");
  }

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!orm_detail::dialect_from_client(db, &dialect)) {
    return orm_detail::set_no_driver_error(db, "find_by_pk", error,
                                           db_status);
  }

  const uvcpp_orm_column* primary_key = model_traits<T>::primary_key();
  if (primary_key == nullptr) {
    return orm_detail::set_error(error, uvcpp_orm_status::NO_PRIMARY_KEY,
                                 "find_by_pk: model has no primary key");
  }

  const uvcpp_orm_sql_result sql_result =
      select<T>(dialect)
          .where_eq(primary_key->column_name(), id)
          .limit(1)
          .build();
  if (!sql_result.ok()) {
    if (error != nullptr) *error = sql_result.error;
    return sql_result.status;
  }

  uvcpp_db_table table;
  const uvcpp_db_status database_status =
      db.query(sql_result.sql, sql_result.params, &table);
  if (!uvcpp_db_ok(database_status)) {
    return orm_detail::set_db_error(db, database_status, "find_by_pk query",
                                    error, db_status);
  }
  if (table.row_count() == 0u) {
    return orm_detail::set_error(
        error, uvcpp_orm_status::NOT_FOUND,
        std::string("find_by_pk: no row in table \"") +
            model_traits<T>::table_name() + "\" matches the primary key");
  }

  std::vector<orm_detail::column_binding> bindings;
  status = orm_detail::prepare_bindings<T>(table, &bindings, error);
  if (status != uvcpp_orm_status::OK) return status;
  return orm_detail::map_row(table.rows()[0], bindings, 0u, out, error);
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
  if (total == nullptr) {
    return orm_detail::find_all_impl<T>(db, sql_result, nullptr, out, error,
                                        total, db_status);
  }

  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (out == nullptr) {
    return orm_detail::set_invalid_argument(
        error, "find_all: out must not be null");
  }
  if (!sql_result.ok()) {
    if (error != nullptr) *error = sql_result.error;
    return sql_result.status;
  }

  uvcpp_orm_dialect dialect;
  if (!orm_detail::dialect_from_client(db, &dialect)) {
    return orm_detail::set_no_driver_error(db, "find_all", error, db_status);
  }
  if (!orm_detail::dialect_matches(sql_result, dialect, error, "find_all")) {
    return uvcpp_orm_status::INVALID_ARGUMENT;
  }

  uvcpp_orm_sql_result count_sql;
  const uvcpp_orm_status derived =
      orm_detail::derive_count_query(sql_result, &count_sql, error);
  if (derived != uvcpp_orm_status::OK) return derived;

  return orm_detail::find_all_impl<T>(db, sql_result, &count_sql, out, error,
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
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;

  if (out == nullptr) {
    return orm_detail::set_invalid_argument(
        error, "find_all: out must not be null");
  }

  const uvcpp_orm_sql_result select_sql = builder.build();
  if (!select_sql.ok()) {
    if (error != nullptr) *error = select_sql.error;
    return select_sql.status;
  }

  if (total == nullptr) {
    return orm_detail::find_all_impl<T>(db, select_sql, nullptr, out, error,
                                        total, db_status);
  }

  const uvcpp_orm_sql_result count_sql = builder.build_count();
  if (!count_sql.ok()) {
    if (error != nullptr) *error = count_sql.error;
    return count_sql.status;
  }

  return orm_detail::find_all_impl<T>(db, select_sql, &count_sql, out, error,
                                      total, db_status);
}

/// 插入一行。自增列（以及 `read_only` 列）不进入 INSERT；`new_id` 非空时
/// 回填 `last_insert_id`（后端报不出时写 NULL 值，但返回码仍是 `OK`）。
template <typename T>
inline uvcpp_orm_status insert(uvcpp_db_client& db, const T& value,
                               uvcpp_db_value* new_id = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (new_id != nullptr) new_id->set_null();

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!orm_detail::dialect_from_client(db, &dialect)) {
    return orm_detail::set_no_driver_error(db, "insert", error, db_status);
  }

  uvcpp_orm_insert_builder<T> builder(dialect);
  const uvcpp_orm_column* columns = model_traits<T>::columns();
  const std::size_t count = model_traits<T>::column_count();
  for (std::size_t i = 0; i < count; ++i) {
    const uvcpp_orm_column& column = columns[i];
    if (column.is_auto_increment() || column.is_read_only()) continue;
    builder.column(column.column_name(),
                   orm_detail::column_to_db_value(
                       column, orm_detail::column_address(&value, column)));
  }

  const uvcpp_orm_sql_result sql_result = builder.build();
  if (!sql_result.ok()) {
    if (error != nullptr) *error = sql_result.error;
    return sql_result.status;
  }

  if (new_id != nullptr) {
    const uvcpp_db_status database_status =
        db.insert(sql_result.sql, sql_result.params, new_id);
    if (!uvcpp_db_ok(database_status)) {
      new_id->set_null();
      return orm_detail::set_db_error(db, database_status, "insert", error,
                                      db_status);
    }
    return uvcpp_orm_status::OK;
  }

  const uvcpp_db_status database_status =
      db.execute(sql_result.sql, sql_result.params, nullptr);
  if (!uvcpp_db_ok(database_status)) {
    return orm_detail::set_db_error(db, database_status, "insert", error,
                                    db_status);
  }
  return uvcpp_orm_status::OK;
}

/// 按主键更新。SET 里不含主键（也不含 `read_only` 列）。受影响 0 行不是
/// 错误：返回 `OK`，`affected` 写 0；执行失败时 `affected` 写 -1。
template <typename T>
inline uvcpp_orm_status update(uvcpp_db_client& db, const T& value,
                               int64_t* affected = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (affected != nullptr) *affected = -1;

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!orm_detail::dialect_from_client(db, &dialect)) {
    return orm_detail::set_no_driver_error(db, "update", error, db_status);
  }

  const uvcpp_orm_column* primary_key = model_traits<T>::primary_key();
  if (primary_key == nullptr) {
    return orm_detail::set_error(error, uvcpp_orm_status::NO_PRIMARY_KEY,
                                 "update: model has no primary key");
  }

  const uvcpp_db_value primary_key_value = orm_detail::column_to_db_value(
      *primary_key, orm_detail::column_address(&value, *primary_key));
  if (primary_key_value.is_null()) {
    return orm_detail::set_invalid_argument(
        error, "update: primary key is NULL; refusing to build WHERE " +
                   std::string(primary_key->column_name()) + " = NULL");
  }

  uvcpp_orm_update_builder<T> builder(dialect);
  const uvcpp_orm_column* columns = model_traits<T>::columns();
  const std::size_t count = model_traits<T>::column_count();
  for (std::size_t i = 0; i < count; ++i) {
    const uvcpp_orm_column& column = columns[i];
    if (column.is_primary_key() || column.is_read_only()) continue;
    builder.set(column.column_name(),
                orm_detail::column_to_db_value(
                    column, orm_detail::column_address(&value, column)));
  }
  builder.where_eq(primary_key->column_name(), primary_key_value);

  const uvcpp_orm_sql_result sql_result = builder.build();
  if (!sql_result.ok()) {
    if (error != nullptr) *error = sql_result.error;
    return sql_result.status;
  }

  int64_t changed = -1;
  const uvcpp_db_status database_status =
      db.execute(sql_result.sql, sql_result.params, &changed);
  if (!uvcpp_db_ok(database_status)) {
    return orm_detail::set_db_error(db, database_status, "update", error,
                                    db_status);
  }

  if (affected != nullptr) *affected = changed;
  return uvcpp_orm_status::OK;
}

/// 按主键删除。受影响 0 行不是错误（返回 `OK`、`affected` 0）。
template <typename T>
inline uvcpp_orm_status remove(uvcpp_db_client& db, const uvcpp_db_value& id,
                               int64_t* affected = nullptr,
                               std::string* error = nullptr,
                               uvcpp_db_status* db_status = nullptr) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (affected != nullptr) *affected = -1;

  if (id.is_null()) {
    return orm_detail::set_invalid_argument(
        error, "remove: primary key must not be NULL");
  }

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!orm_detail::dialect_from_client(db, &dialect)) {
    return orm_detail::set_no_driver_error(db, "remove", error, db_status);
  }

  const uvcpp_orm_column* primary_key = model_traits<T>::primary_key();
  if (primary_key == nullptr) {
    return orm_detail::set_error(error, uvcpp_orm_status::NO_PRIMARY_KEY,
                                 "remove: model has no primary key");
  }

  const uvcpp_orm_sql_result sql_result =
      delete_from<T>(dialect)
          .where_eq(primary_key->column_name(), id)
          .build();
  if (!sql_result.ok()) {
    if (error != nullptr) *error = sql_result.error;
    return sql_result.status;
  }

  int64_t changed = -1;
  const uvcpp_db_status database_status =
      db.execute(sql_result.sql, sql_result.params, &changed);
  if (!uvcpp_db_ok(database_status)) {
    return orm_detail::set_db_error(db, database_status, "remove", error,
                                    db_status);
  }

  if (affected != nullptr) *affected = changed;
  return uvcpp_orm_status::OK;
}

/// 执行一条 COUNT 构造器。`total` 必须非空。
template <typename T>
inline uvcpp_orm_status count(uvcpp_db_client& db,
                              const uvcpp_orm_select_builder<T>& builder,
                              int64_t* total,
                              std::string* error = nullptr,
                              uvcpp_db_status* db_status = nullptr) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (total == nullptr) {
    return orm_detail::set_invalid_argument(
        error, "count: total must not be null");
  }

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!orm_detail::dialect_from_client(db, &dialect)) {
    return orm_detail::set_no_driver_error(db, "count", error, db_status);
  }

  const uvcpp_orm_sql_result count_sql = builder.build_count();
  if (!count_sql.ok()) {
    if (error != nullptr) *error = count_sql.error;
    return count_sql.status;
  }
  if (!orm_detail::dialect_matches(count_sql, dialect, error, "count")) {
    return uvcpp_orm_status::INVALID_ARGUMENT;
  }

  return orm_detail::execute_count_query(db, count_sql, total, error,
                                         db_status);
}

/// 执行一条**已构造**的 COUNT 查询。`total` 必须非空。
template <typename T>
inline uvcpp_orm_status count(uvcpp_db_client& db,
                              const uvcpp_orm_sql_result& count_sql,
                              int64_t* total,
                              std::string* error = nullptr,
                              uvcpp_db_status* db_status = nullptr) {
  if (error != nullptr) error->clear();
  if (db_status != nullptr) *db_status = uvcpp_db_status::OK;
  if (total == nullptr) {
    return orm_detail::set_invalid_argument(
        error, "count: total must not be null");
  }
  if (!count_sql.ok()) {
    if (error != nullptr) *error = count_sql.error;
    return count_sql.status;
  }
  if (count_sql.sql.empty()) {
    return orm_detail::set_invalid_argument(error, "count: SQL is empty");
  }

  uvcpp_orm_status status = validate_model<T>(error);
  if (status != uvcpp_orm_status::OK) return status;

  uvcpp_orm_dialect dialect;
  if (!orm_detail::dialect_from_client(db, &dialect)) {
    return orm_detail::set_no_driver_error(db, "count", error, db_status);
  }
  if (!orm_detail::dialect_matches(count_sql, dialect, error, "count")) {
    return uvcpp_orm_status::INVALID_ARGUMENT;
  }

  return orm_detail::execute_count_query(db, count_sql, total, error,
                                         db_status);
}

}  // namespace uvcpp

#endif  // UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE
#endif  // SRC_ORM_UVCPP_ORM_REPOSITORY_H
