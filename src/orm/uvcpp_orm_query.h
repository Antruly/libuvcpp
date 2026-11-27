/**
 * @file src/orm/uvcpp_orm_query.h
 * @brief 单表 SQL 构造：SELECT / INSERT / UPDATE / DELETE，按方言生成占位符。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一层**不执行 SQL、不连接数据库**。它只把 `model_traits<T>` 的元数据变成
 * 一条可审计的 SQL 字符串，并把所有**值**放进 `uvcpp_db_params`。调用方拿
 * `uvcpp_orm_sql_result` 交给 #91 的执行层即可。
 *
 * ## 规矩
 *
 *   * 表名、列名只能来自模型元数据。表名直接取 `model_traits<T>::table_name()`；
 *     列可以用成员指针（`&T::id`）或模型里声明过的列名（`"id"`）。两者都会在
 *     构造期查一遍 `model_traits<T>`，查不到就是 `UNKNOWN_COLUMN`。
 *     **没有接受任意字符串当表名的入口**。
 *   * 值一律走占位符：SQLite/MySQL 是 `?`，PostgreSQL 是 `$1..$n`；值本身永远
 *     不会出现在 SQL 文本里。LIMIT / OFFSET 也是参数，不是字面量。
 *   * 不抛异常。失败返回 `uvcpp_orm_sql_result::status` + 人读 `error`。
 *     `validate_model<T>()` 的检查（无模型、无主键、重复列……）在 `build()`
 *     里先跑。
 *   * UPDATE / DELETE 必须有 WHERE。空 WHERE 是 `EMPTY_WHERE`，不会生成全表写
 *     操作。
 *
 * ## 组合方式
 *
 * `where_*` 把条件 AND 进当前组；`or_where_*` 开一个新的组。渲染时每个 OR 组
 * 带括号、组内用 AND，于是优先级是明确的：
 *
 * ```cpp
 * select<user>(uvcpp_orm_dialect::postgresql)
 *     .where_ge(&user::age, 18)          // 组 1
 *     .where_lt(&user::age, 65)
 *     .or_where_eq(&user::name, "root")  // 组 2
 *     .build();
 * // WHERE ("age" >= $1 AND "age" < $2) OR ("name" = $3)
 * ```
 *
 * ## COUNT
 *
 * `.count()` 生成 `SELECT COUNT(*) FROM ...`，只带 WHERE。它**不能**和列选择 /
 * ORDER BY / LIMIT / OFFSET 组合（`INVALID_COUNT_QUERY`）：`COUNT(*)` 只有一行，
 * 再 ORDER BY / LIMIT 只会让语义变得含糊。
 *
 * ## 逃生舱：raw()
 *
 * `raw(dialect, sql, params)` 原样返回调用方的 SQL，**不转义其中的标识符**，
 * 也不改写值。它只做两件事：方言合法性检查、占位符数量与参数数量的一致性
 * 检查（按词法扫描，跳过引号内的内容；不解析完整 SQL，也不处理 PostgreSQL 的
 * `$tag$...$tag$` 字符串）。SQL 的正确性与安全性由调用方承担 —— 能不用就不要
 * 用。
 */

#pragma once
#ifndef SRC_ORM_UVCPP_ORM_QUERY_H
#define SRC_ORM_UVCPP_ORM_QUERY_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include <db/uvcpp_db_params.h>
#include <db/uvcpp_db_value.h>
#include <orm/uvcpp_orm_dialect.h>
#include <orm/uvcpp_orm_error.h>
#include <orm/uvcpp_orm_model.h>
#include <orm/uvcpp_orm_value.h>

namespace uvcpp {

/// WHERE 的操作符。`in` / `is_null` / `is_not_null` 用的值形态不同，见
/// `where_in()` / `where_is_null()` / `where_is_not_null()`。
enum class uvcpp_orm_operator : int {
  eq = 0,
  ne,
  lt,
  le,
  gt,
  ge,
  like,
  in,
  is_null,
  is_not_null,
};

inline const char* uvcpp_orm_operator_name(uvcpp_orm_operator op) {
  switch (op) {
    case uvcpp_orm_operator::eq: return "eq";
    case uvcpp_orm_operator::ne: return "ne";
    case uvcpp_orm_operator::lt: return "lt";
    case uvcpp_orm_operator::le: return "le";
    case uvcpp_orm_operator::gt: return "gt";
    case uvcpp_orm_operator::ge: return "ge";
    case uvcpp_orm_operator::like: return "like";
    case uvcpp_orm_operator::in: return "in";
    case uvcpp_orm_operator::is_null: return "is_null";
    case uvcpp_orm_operator::is_not_null: return "is_not_null";
  }
  return "unknown";
}

/// ORDER BY 的方向。只有这两个值；不接受字符串。
enum class uvcpp_orm_order : int {
  asc = 0,
  desc = 1,
};

inline const char* uvcpp_orm_order_name(uvcpp_orm_order order) {
  switch (order) {
    case uvcpp_orm_order::asc: return "asc";
    case uvcpp_orm_order::desc: return "desc";
  }
  return "unknown";
}

inline bool uvcpp_orm_order_is_valid(uvcpp_orm_order order) {
  switch (order) {
    case uvcpp_orm_order::asc:
    case uvcpp_orm_order::desc: return true;
  }
  return false;
}

/// 构造结果：最终 SQL + 绑定参数。失败时 `sql` / `params` 为空。
struct uvcpp_orm_sql_result {
  uvcpp_orm_sql_result()
      : status(uvcpp_orm_status::OK), dialect(uvcpp_orm_dialect::sqlite) {}

  bool ok() const { return status == uvcpp_orm_status::OK; }
  explicit operator bool() const { return ok(); }

  uvcpp_orm_status status;
  std::string error;
  uvcpp_orm_dialect dialect;
  std::string sql;
  uvcpp_db_params params;
};

namespace orm_detail {

/// 查询值转换。这里**不能**直接用 `uvcpp_orm_to_db_value()`：模型字段是
/// `std::int64_t`，而调用方写的字面量往往是 `int`；`uvcpp_orm_to_db_value<int>`
/// 是“没有映射”的默认实现（NULL）。查询值走 `uvcpp_db_value` 自己的整型
/// 构造函数，只有 `nullable<T>` / BLOB 这两个 ORM 专属形态额外接管。
template <typename U>
inline uvcpp_db_value to_query_value(const U& value) {
  return uvcpp_db_value(value);
}

template <typename U>
inline uvcpp_db_value to_query_value(const nullable<U>& value) {
  return uvcpp_orm_to_db_value(value);
}

inline uvcpp_db_value to_query_value(
    const std::vector<std::uint8_t>& value) {
  return uvcpp_orm_to_db_value(value);
}

/// `where_in` 不接受单个字符串（`std::string` 有 begin/end，会被当成字符
/// 序列逐字符绑定）。字符数组也一并排除，让误用落在编译期。
template <typename U>
struct is_string_like
    : std::integral_constant<
          bool,
          std::is_same<typename std::decay<U>::type, std::string>::value ||
              std::is_same<typename std::decay<U>::type, const char*>::value ||
              std::is_same<typename std::decay<U>::type, char*>::value> {};

struct uvcpp_orm_condition {
  uvcpp_orm_condition()
      : column(nullptr), op(uvcpp_orm_operator::eq) {}

  const uvcpp_orm_column* column;
  uvcpp_orm_operator op;
  std::vector<uvcpp_db_value> values;
};

struct uvcpp_orm_condition_group {
  std::vector<uvcpp_orm_condition> conditions;
};

struct uvcpp_orm_order_term {
  uvcpp_orm_order_term()
      : column(nullptr), order(uvcpp_orm_order::asc) {}

  const uvcpp_orm_column* column;
  uvcpp_orm_order order;
};

inline bool is_binary_operator(uvcpp_orm_operator op) {
  switch (op) {
    case uvcpp_orm_operator::eq:
    case uvcpp_orm_operator::ne:
    case uvcpp_orm_operator::lt:
    case uvcpp_orm_operator::le:
    case uvcpp_orm_operator::gt:
    case uvcpp_orm_operator::ge:
    case uvcpp_orm_operator::like: return true;
    default: return false;
  }
}

inline bool is_null_operator(uvcpp_orm_operator op) {
  return op == uvcpp_orm_operator::is_null ||
         op == uvcpp_orm_operator::is_not_null;
}

inline bool is_known_operator(uvcpp_orm_operator op) {
  return is_binary_operator(op) || is_null_operator(op) ||
         op == uvcpp_orm_operator::in;
}

inline void append_quoted(std::string* sql, uvcpp_orm_dialect dialect,
                          const char* identifier) {
  *sql += uvcpp_orm_quote_identifier(
      dialect, identifier != nullptr ? std::string(identifier) : std::string());
}

inline void append_placeholder(std::string* sql, uvcpp_orm_dialect dialect,
                               std::size_t* next_index) {
  ++(*next_index);
  *sql += uvcpp_orm_placeholder(dialect, *next_index);
}

/// 渲染一个条件，并把它的值按顺序追加到 `params`。`next_index` 是跨条件、
/// 跨 LIMIT/OFFSET 递增的全局参数序号（PostgreSQL 的 `$n` 用它）。
inline uvcpp_orm_status append_condition_sql(
    uvcpp_orm_dialect dialect, const uvcpp_orm_condition& condition,
    std::string* sql, uvcpp_db_params* params, std::size_t* next_index,
    std::string* error) {
  if (condition.column == nullptr || condition.column->column_name() == nullptr) {
    if (error != nullptr) *error = "condition has no column";
    return uvcpp_orm_status::UNKNOWN_COLUMN;
  }

  const std::string label =
      std::string("column \"") + condition.column->column_name() + "\"";
  append_quoted(sql, dialect, condition.column->column_name());

  switch (condition.op) {
    case uvcpp_orm_operator::eq:
    case uvcpp_orm_operator::ne:
    case uvcpp_orm_operator::lt:
    case uvcpp_orm_operator::le:
    case uvcpp_orm_operator::gt:
    case uvcpp_orm_operator::ge:
    case uvcpp_orm_operator::like: {
      if (condition.values.size() != 1u) {
        if (error != nullptr) {
          *error = label + " requires exactly one value";
        }
        return uvcpp_orm_status::PARAM_COUNT_MISMATCH;
      }
      const char* symbol = "=";
      switch (condition.op) {
        case uvcpp_orm_operator::eq: symbol = "="; break;
        case uvcpp_orm_operator::ne: symbol = "<>"; break;
        case uvcpp_orm_operator::lt: symbol = "<"; break;
        case uvcpp_orm_operator::le: symbol = "<="; break;
        case uvcpp_orm_operator::gt: symbol = ">"; break;
        case uvcpp_orm_operator::ge: symbol = ">="; break;
        case uvcpp_orm_operator::like: symbol = "LIKE"; break;
        default: break;
      }
      *sql += " ";
      *sql += symbol;
      *sql += " ";
      params->add(condition.values[0]);
      append_placeholder(sql, dialect, next_index);
      return uvcpp_orm_status::OK;
    }

    case uvcpp_orm_operator::in: {
      if (condition.values.empty()) {
        if (error != nullptr) *error = label + " has an empty IN list";
        return uvcpp_orm_status::EMPTY_IN_VALUES;
      }
      *sql += " IN (";
      for (std::size_t i = 0; i < condition.values.size(); ++i) {
        if (i != 0u) *sql += ", ";
        params->add(condition.values[i]);
        append_placeholder(sql, dialect, next_index);
      }
      *sql += ")";
      return uvcpp_orm_status::OK;
    }

    case uvcpp_orm_operator::is_null:
    case uvcpp_orm_operator::is_not_null: {
      if (!condition.values.empty()) {
        if (error != nullptr) {
          *error = label + " with " +
                   uvcpp_orm_operator_name(condition.op) +
                   " takes no value";
        }
        return uvcpp_orm_status::PARAM_COUNT_MISMATCH;
      }
      *sql += condition.op == uvcpp_orm_operator::is_null ? " IS NULL"
                                                          : " IS NOT NULL";
      return uvcpp_orm_status::OK;
    }
  }

  if (error != nullptr) {
    *error = std::string("unknown operator ") +
             uvcpp_orm_operator_name(condition.op);
  }
  return uvcpp_orm_status::INVALID_OPERATOR;
}

inline bool has_conditions(
    const std::vector<uvcpp_orm_condition_group>& groups) {
  for (std::size_t i = 0; i < groups.size(); ++i) {
    if (!groups[i].conditions.empty()) return true;
  }
  return false;
}

/// 渲染 WHERE 子句（含前导空格）。没有条件时 `sql` 为空串。
///
/// 一个组内用 AND；多个组之间用 OR，且每个组带括号。这样优先级与 SQL 的
/// AND > OR 一致，不会让调用方去猜。
inline uvcpp_orm_status render_condition_groups(
    uvcpp_orm_dialect dialect,
    const std::vector<uvcpp_orm_condition_group>& groups, std::string* sql,
    uvcpp_db_params* params, std::size_t* next_index, std::string* error) {
  sql->clear();
  if (!has_conditions(groups)) return uvcpp_orm_status::OK;

  std::size_t nonempty_groups = 0u;
  for (std::size_t i = 0; i < groups.size(); ++i) {
    if (!groups[i].conditions.empty()) ++nonempty_groups;
  }
  const bool parenthesize = nonempty_groups > 1u;

  *sql = " WHERE ";
  bool first_group = true;
  for (std::size_t g = 0; g < groups.size(); ++g) {
    const std::vector<uvcpp_orm_condition>& conditions = groups[g].conditions;
    if (conditions.empty()) continue;

    if (!first_group) *sql += " OR ";
    first_group = false;
    if (parenthesize) *sql += "(";

    for (std::size_t c = 0; c < conditions.size(); ++c) {
      if (c != 0u) *sql += " AND ";
      const uvcpp_orm_status status = append_condition_sql(
          dialect, conditions[c], sql, params, next_index, error);
      if (status != uvcpp_orm_status::OK) return status;
    }

    if (parenthesize) *sql += ")";
  }
  return uvcpp_orm_status::OK;
}

/// 查询构造共用的方言 + 模型校验。方言先查，然后复用 `validate_model<T>()`。
template <typename T>
inline uvcpp_orm_status validate_statement(uvcpp_orm_dialect dialect,
                                           std::string* error) {
  if (!uvcpp_orm_dialect_is_valid(dialect)) {
    if (error != nullptr) *error = "unknown SQL dialect";
    return uvcpp_orm_status::INVALID_DIALECT;
  }
  return validate_model<T>(error);
}

/// 所有构造器的公共状态：方言、错误、列解析。
template <typename T>
class uvcpp_orm_statement_base {
 protected:
  explicit uvcpp_orm_statement_base(uvcpp_orm_dialect dialect)
      : dialect_(dialect),
        failed_(false),
        status_(uvcpp_orm_status::OK) {}

  std::string table_label() const {
    const char* table = model_traits<T>::table_name();
    return table != nullptr ? std::string(table) : std::string("?");
  }

  /// 按列名解析。名字必须已经在 `model_traits<T>` 里声明过。
  const uvcpp_orm_column* resolve_column(const char* column_name) {
    if (failed_) return nullptr;
    const uvcpp_orm_column* column =
        model_traits<T>::column_by_name(column_name);
    if (column == nullptr) {
      fail(uvcpp_orm_status::UNKNOWN_COLUMN,
           std::string("column \"") +
               (column_name != nullptr ? column_name : "") +
               "\" is not part of model for table \"" + table_label() + "\"");
    }
    return column;
  }

  /// 按成员指针解析。成员没在模型里声明时返回空指针。
  template <typename M>
  const uvcpp_orm_column* resolve_column(M T::* member) {
    if (failed_) return nullptr;
    const uvcpp_orm_column* column = column_for<T>(member);
    if (column == nullptr) {
      fail(uvcpp_orm_status::UNKNOWN_COLUMN,
           std::string("member pointer is not part of model for table \"") +
               table_label() + "\"");
    }
    return column;
  }

  void fail(uvcpp_orm_status status, const std::string& message) {
    if (failed_) return;
    failed_ = true;
    status_ = status;
    error_ = message;
  }

  uvcpp_orm_dialect dialect_;
  bool failed_;
  uvcpp_orm_status status_;
  std::string error_;
};

/// WHERE 构造的 CRTP 基类。`Derived` 只是为了让链式调用回到具体构造器，
/// 这样 `.where_eq(...).limit(...)` 仍然能编译。
template <typename T, typename Derived>
class uvcpp_orm_where_core : public uvcpp_orm_statement_base<T> {
 protected:
  typedef uvcpp_orm_statement_base<T> base_type;

  explicit uvcpp_orm_where_core(uvcpp_orm_dialect dialect)
      : base_type(dialect) {}

  Derived& self() { return static_cast<Derived&>(*this); }

  static std::vector<uvcpp_db_value> one_value(
      const uvcpp_db_value& value) {
    return std::vector<uvcpp_db_value>(1u, value);
  }

  static std::vector<uvcpp_db_value> no_values() {
    return std::vector<uvcpp_db_value>();
  }

  template <typename Container>
  static std::vector<uvcpp_db_value> to_values(const Container& values) {
    std::vector<uvcpp_db_value> out;
    for (auto it = std::begin(values); it != std::end(values); ++it) {
      out.push_back(to_query_value(*it));
    }
    return out;
  }

  Derived& add_named(uvcpp_orm_operator op, const char* column_name,
                     const std::vector<uvcpp_db_value>& values,
                     bool new_group) {
    const uvcpp_orm_column* column = this->resolve_column(column_name);
    return add_resolved(column, op, values, new_group);
  }

  template <typename M>
  Derived& add_member(uvcpp_orm_operator op, M T::* member,
                      const std::vector<uvcpp_db_value>& values,
                      bool new_group) {
    const uvcpp_orm_column* column = this->resolve_column(member);
    return add_resolved(column, op, values, new_group);
  }

  Derived& add_resolved(const uvcpp_orm_column* column, uvcpp_orm_operator op,
                        const std::vector<uvcpp_db_value>& values,
                        bool new_group) {
    if (this->failed_) return self();
    if (column == nullptr) {
      this->fail(uvcpp_orm_status::UNKNOWN_COLUMN,
                 "condition has no column");
      return self();
    }

    const std::string label =
        std::string("column \"") + column->column_name() + "\"";

    if (!is_known_operator(op)) {
      this->fail(uvcpp_orm_status::INVALID_OPERATOR,
                 std::string("operator ") + uvcpp_orm_operator_name(op) +
                     " is not a known operator");
      return self();
    }
    if (is_binary_operator(op)) {
      if (values.size() != 1u) {
        this->fail(uvcpp_orm_status::PARAM_COUNT_MISMATCH,
                   label + " requires exactly one value");
        return self();
      }
    } else if (op == uvcpp_orm_operator::in) {
      if (values.empty()) {
        this->fail(uvcpp_orm_status::EMPTY_IN_VALUES,
                   label + " has an empty IN list");
        return self();
      }
    } else {
      if (!values.empty()) {
        this->fail(uvcpp_orm_status::PARAM_COUNT_MISMATCH,
                   label + " with " + uvcpp_orm_operator_name(op) +
                       " takes no value");
        return self();
      }
    }

    if (new_group || groups_.empty()) {
      groups_.push_back(uvcpp_orm_condition_group());
    }
    uvcpp_orm_condition condition;
    condition.column = column;
    condition.op = op;
    condition.values = values;
    groups_.back().conditions.push_back(condition);
    return self();
  }

  bool has_where_conditions() const {
    return has_conditions(groups_);
  }

  uvcpp_orm_status render_where(std::string* sql, uvcpp_db_params* params,
                                std::size_t* next_index,
                                std::string* error) const {
    return render_condition_groups(this->dialect_, groups_, sql, params,
                                   next_index, error);
  }

 public:
  // ---- 通用入口（主要用于测试非法枚举值，也可直接按操作符构造）----

  Derived& where(uvcpp_orm_operator op, const char* column_name,
                 const uvcpp_db_value& value) {
    return add_named(op, column_name, one_value(value), false);
  }

  template <typename M>
  Derived& where(uvcpp_orm_operator op, M T::* member,
                 const uvcpp_db_value& value) {
    return add_member(op, member, one_value(value), false);
  }

  Derived& where(uvcpp_orm_operator op, const char* column_name) {
    return add_named(op, column_name, no_values(), false);
  }

  template <typename M>
  Derived& where(uvcpp_orm_operator op, M T::* member) {
    return add_member(op, member, no_values(), false);
  }

  Derived& or_where(uvcpp_orm_operator op, const char* column_name,
                    const uvcpp_db_value& value) {
    return add_named(op, column_name, one_value(value), true);
  }

  template <typename M>
  Derived& or_where(uvcpp_orm_operator op, M T::* member,
                    const uvcpp_db_value& value) {
    return add_member(op, member, one_value(value), true);
  }

  Derived& or_where(uvcpp_orm_operator op, const char* column_name) {
    return add_named(op, column_name, no_values(), true);
  }

  template <typename M>
  Derived& or_where(uvcpp_orm_operator op, M T::* member) {
    return add_member(op, member, no_values(), true);
  }

// 二元比较的命名糖。每个名字生成四个重载：按列名 / 按成员指针，
// 以及它们的 OR 组版本。
#define UVCPP_ORM_DETAIL_BINARY_PREDICATE(Method, Op)                       \
  Derived& Method(const char* column_name, const uvcpp_db_value& value) {   \
    return add_named(Op, column_name, one_value(value), false);             \
  }                                                                         \
  template <typename M>                                                     \
  Derived& Method(M T::* member, const uvcpp_db_value& value) {             \
    return add_member(Op, member, one_value(value), false);                 \
  }                                                                         \
  Derived& or_##Method(const char* column_name,                             \
                       const uvcpp_db_value& value) {                       \
    return add_named(Op, column_name, one_value(value), true);              \
  }                                                                         \
  template <typename M>                                                     \
  Derived& or_##Method(M T::* member, const uvcpp_db_value& value) {        \
    return add_member(Op, member, one_value(value), true);                  \
  }

  UVCPP_ORM_DETAIL_BINARY_PREDICATE(where_eq, uvcpp_orm_operator::eq)
  UVCPP_ORM_DETAIL_BINARY_PREDICATE(where_ne, uvcpp_orm_operator::ne)
  UVCPP_ORM_DETAIL_BINARY_PREDICATE(where_lt, uvcpp_orm_operator::lt)
  UVCPP_ORM_DETAIL_BINARY_PREDICATE(where_le, uvcpp_orm_operator::le)
  UVCPP_ORM_DETAIL_BINARY_PREDICATE(where_gt, uvcpp_orm_operator::gt)
  UVCPP_ORM_DETAIL_BINARY_PREDICATE(where_ge, uvcpp_orm_operator::ge)
  UVCPP_ORM_DETAIL_BINARY_PREDICATE(where_like, uvcpp_orm_operator::like)

#undef UVCPP_ORM_DETAIL_BINARY_PREDICATE

  // ---- IN ----

  template <typename Container>
  typename std::enable_if<!is_string_like<Container>::value, Derived&>::type
  where_in(const char* column_name, const Container& values) {
    return add_named(uvcpp_orm_operator::in, column_name, to_values(values),
                     false);
  }

  template <typename M, typename Container>
  typename std::enable_if<!is_string_like<Container>::value, Derived&>::type
  where_in(M T::* member, const Container& values) {
    return add_member(uvcpp_orm_operator::in, member, to_values(values),
                      false);
  }

  Derived& where_in(const char* column_name,
                    std::initializer_list<uvcpp_db_value> values) {
    return add_named(uvcpp_orm_operator::in, column_name,
                     std::vector<uvcpp_db_value>(values), false);
  }

  template <typename M>
  Derived& where_in(M T::* member,
                    std::initializer_list<uvcpp_db_value> values) {
    return add_member(uvcpp_orm_operator::in, member,
                      std::vector<uvcpp_db_value>(values), false);
  }

  template <typename Container>
  typename std::enable_if<!is_string_like<Container>::value, Derived&>::type
  or_where_in(const char* column_name, const Container& values) {
    return add_named(uvcpp_orm_operator::in, column_name, to_values(values),
                     true);
  }

  template <typename M, typename Container>
  typename std::enable_if<!is_string_like<Container>::value, Derived&>::type
  or_where_in(M T::* member, const Container& values) {
    return add_member(uvcpp_orm_operator::in, member, to_values(values), true);
  }

  Derived& or_where_in(const char* column_name,
                       std::initializer_list<uvcpp_db_value> values) {
    return add_named(uvcpp_orm_operator::in, column_name,
                     std::vector<uvcpp_db_value>(values), true);
  }

  template <typename M>
  Derived& or_where_in(M T::* member,
                       std::initializer_list<uvcpp_db_value> values) {
    return add_member(uvcpp_orm_operator::in, member,
                      std::vector<uvcpp_db_value>(values), true);
  }

  // ---- IS NULL / IS NOT NULL ----

  Derived& where_is_null(const char* column_name) {
    return add_named(uvcpp_orm_operator::is_null, column_name, no_values(),
                     false);
  }

  template <typename M>
  Derived& where_is_null(M T::* member) {
    return add_member(uvcpp_orm_operator::is_null, member, no_values(), false);
  }

  Derived& or_where_is_null(const char* column_name) {
    return add_named(uvcpp_orm_operator::is_null, column_name, no_values(),
                     true);
  }

  template <typename M>
  Derived& or_where_is_null(M T::* member) {
    return add_member(uvcpp_orm_operator::is_null, member, no_values(), true);
  }

  Derived& where_is_not_null(const char* column_name) {
    return add_named(uvcpp_orm_operator::is_not_null, column_name, no_values(),
                     false);
  }

  template <typename M>
  Derived& where_is_not_null(M T::* member) {
    return add_member(uvcpp_orm_operator::is_not_null, member, no_values(),
                      false);
  }

  Derived& or_where_is_not_null(const char* column_name) {
    return add_named(uvcpp_orm_operator::is_not_null, column_name, no_values(),
                     true);
  }

  template <typename M>
  Derived& or_where_is_not_null(M T::* member) {
    return add_member(uvcpp_orm_operator::is_not_null, member, no_values(),
                      true);
  }

 protected:
  std::vector<uvcpp_orm_condition_group> groups_;
};

}  // namespace orm_detail

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------

template <typename T>
class uvcpp_orm_select_builder
    : public orm_detail::uvcpp_orm_where_core<T,
                                              uvcpp_orm_select_builder<T> > {
 public:
  typedef orm_detail::uvcpp_orm_where_core<T,
                                           uvcpp_orm_select_builder<T> >
      base_type;

  explicit uvcpp_orm_select_builder(uvcpp_orm_dialect dialect)
      : base_type(dialect),
        count_(false),
        has_limit_(false),
        limit_(0),
        has_offset_(false),
        offset_(0) {}

  /// 选择一列（按模型里声明的列名）。默认选择全部列；一旦显式选过，就只
  /// 输出选中的列，顺序与调用顺序一致。
  uvcpp_orm_select_builder& column(const char* column_name) {
    const uvcpp_orm_column* column = this->resolve_column(column_name);
    if (column != nullptr) selected_.push_back(column);
    return *this;
  }

  template <typename M>
  uvcpp_orm_select_builder& column(M T::* member) {
    const uvcpp_orm_column* column = this->resolve_column(member);
    if (column != nullptr) selected_.push_back(column);
    return *this;
  }

  uvcpp_orm_select_builder& order_by(const char* column_name,
                                     uvcpp_orm_order order) {
    if (this->failed_) return *this;
    if (!uvcpp_orm_order_is_valid(order)) {
      this->fail(uvcpp_orm_status::INVALID_ORDER_DIRECTION,
                 "order direction must be uvcpp_orm_order::asc or ::desc");
      return *this;
    }
    const uvcpp_orm_column* column = this->resolve_column(column_name);
    if (column != nullptr) add_order(column, order);
    return *this;
  }

  template <typename M>
  uvcpp_orm_select_builder& order_by(M T::* member, uvcpp_orm_order order) {
    if (this->failed_) return *this;
    if (!uvcpp_orm_order_is_valid(order)) {
      this->fail(uvcpp_orm_status::INVALID_ORDER_DIRECTION,
                 "order direction must be uvcpp_orm_order::asc or ::desc");
      return *this;
    }
    const uvcpp_orm_column* column = this->resolve_column(member);
    if (column != nullptr) add_order(column, order);
    return *this;
  }

  /// 负数直接拒绝。LIMIT/OFFSET 会作为参数绑定，不拼字面量。
  uvcpp_orm_select_builder& limit(std::int64_t value) {
    if (this->failed_) return *this;
    if (value < 0) {
      this->fail(uvcpp_orm_status::INVALID_LIMIT,
                 "limit must not be negative");
      return *this;
    }
    has_limit_ = true;
    limit_ = value;
    return *this;
  }

  uvcpp_orm_select_builder& offset(std::int64_t value) {
    if (this->failed_) return *this;
    if (value < 0) {
      this->fail(uvcpp_orm_status::INVALID_OFFSET,
                 "offset must not be negative");
      return *this;
    }
    has_offset_ = true;
    offset_ = value;
    return *this;
  }

  /// 生成 `SELECT COUNT(*) FROM ...`。不能与列选择 / ORDER BY / LIMIT /
  /// OFFSET 组合。
  uvcpp_orm_select_builder& count() {
    count_ = true;
    return *this;
  }

  uvcpp_orm_sql_result build() const {
    uvcpp_orm_sql_result result;
    result.dialect = this->dialect_;

    std::string error;
    const uvcpp_orm_status validated =
        orm_detail::validate_statement<T>(this->dialect_, &error);
    if (validated != uvcpp_orm_status::OK) {
      result.status = validated;
      result.error = error;
      return result;
    }
    if (this->failed_) {
      result.status = this->status_;
      result.error = this->error_;
      return result;
    }

    if (count_) {
      if (!selected_.empty() || !orders_.empty() || has_limit_ ||
          has_offset_) {
        result.status = uvcpp_orm_status::INVALID_COUNT_QUERY;
        result.error =
            "count() cannot be combined with columns, order_by, limit or "
            "offset";
        return result;
      }
    } else if (has_offset_ && !has_limit_) {
      result.status = uvcpp_orm_status::OFFSET_WITHOUT_LIMIT;
      result.error = "offset requires limit";
      return result;
    }

    std::string sql;
    std::size_t next_index = 0u;

    if (count_) {
      sql = "SELECT COUNT(*) FROM ";
    } else {
      sql = "SELECT ";
      if (selected_.empty()) {
        const uvcpp_orm_column* columns = model_traits<T>::columns();
        const std::size_t column_count = model_traits<T>::column_count();
        for (std::size_t i = 0; i < column_count; ++i) {
          if (i != 0u) sql += ", ";
          orm_detail::append_quoted(&sql, this->dialect_,
                                    columns[i].column_name());
        }
      } else {
        for (std::size_t i = 0; i < selected_.size(); ++i) {
          if (i != 0u) sql += ", ";
          orm_detail::append_quoted(&sql, this->dialect_,
                                    selected_[i]->column_name());
        }
      }
      sql += " FROM ";
    }

    orm_detail::append_quoted(&sql, this->dialect_,
                              model_traits<T>::table_name());

    std::string where;
    const uvcpp_orm_status where_status =
        this->render_where(&where, &result.params, &next_index, &error);
    if (where_status != uvcpp_orm_status::OK) {
      result.status = where_status;
      result.error = error;
      return result;
    }
    sql += where;

    if (!count_) {
      if (!orders_.empty()) {
        sql += " ORDER BY ";
        for (std::size_t i = 0; i < orders_.size(); ++i) {
          if (i != 0u) sql += ", ";
          orm_detail::append_quoted(&sql, this->dialect_,
                                    orders_[i].column->column_name());
          sql += orders_[i].order == uvcpp_orm_order::asc ? " ASC" : " DESC";
        }
      }
      if (has_limit_) {
        sql += " LIMIT ";
        result.params.add(limit_);
        orm_detail::append_placeholder(&sql, this->dialect_, &next_index);
      }
      if (has_offset_) {
        sql += " OFFSET ";
        result.params.add(offset_);
        orm_detail::append_placeholder(&sql, this->dialect_, &next_index);
      }
    }

    result.status = uvcpp_orm_status::OK;
    result.sql = sql;
    return result;
  }

  /// 只按 WHERE 生成 `SELECT COUNT(*) FROM ...`，**忽略**已选的列、ORDER BY、
  /// LIMIT 与 OFFSET。给仓库的 `find_all(..., total)` 用：分页查询本身带
  /// LIMIT，而总数必须不计分页。
  ///
  /// 与 `count()` 的区别：`count()` 是“把这个构造器变成 COUNT 查询”，所以
  /// 它会拒绝和列选择/ORDER/LIMIT 组合；`build_count()` 是“从同一个 WHERE
  /// 再生成一条 COUNT 查询”，组合进来的那几项按定义被忽略。两者的 WHERE
  /// 渲染与参数顺序完全一致（复用 `render_where()`）。
  uvcpp_orm_sql_result build_count() const {
    uvcpp_orm_sql_result result;
    result.dialect = this->dialect_;

    std::string error;
    const uvcpp_orm_status validated =
        orm_detail::validate_statement<T>(this->dialect_, &error);
    if (validated != uvcpp_orm_status::OK) {
      result.status = validated;
      result.error = error;
      return result;
    }
    if (this->failed_) {
      result.status = this->status_;
      result.error = this->error_;
      return result;
    }

    std::string sql = "SELECT COUNT(*) FROM ";
    orm_detail::append_quoted(&sql, this->dialect_,
                              model_traits<T>::table_name());

    std::size_t next_index = 0u;
    std::string where;
    const uvcpp_orm_status where_status =
        this->render_where(&where, &result.params, &next_index, &error);
    if (where_status != uvcpp_orm_status::OK) {
      result.status = where_status;
      result.error = error;
      return result;
    }
    sql += where;

    result.status = uvcpp_orm_status::OK;
    result.sql = sql;
    return result;
  }

 private:
  void add_order(const uvcpp_orm_column* column, uvcpp_orm_order order) {
    orm_detail::uvcpp_orm_order_term term;
    term.column = column;
    term.order = order;
    orders_.push_back(term);
  }

  std::vector<const uvcpp_orm_column*> selected_;
  std::vector<orm_detail::uvcpp_orm_order_term> orders_;
  bool count_;
  bool has_limit_;
  std::int64_t limit_;
  bool has_offset_;
  std::int64_t offset_;
};

// ---------------------------------------------------------------------------
// INSERT
// ---------------------------------------------------------------------------

template <typename T>
class uvcpp_orm_insert_builder
    : public orm_detail::uvcpp_orm_statement_base<T> {
 public:
  typedef orm_detail::uvcpp_orm_statement_base<T> base_type;

  explicit uvcpp_orm_insert_builder(uvcpp_orm_dialect dialect)
      : base_type(dialect) {}

  /// 追加一列及其值。列顺序就是 INSERT 的列顺序，参数也按这个顺序绑定。
  uvcpp_orm_insert_builder& column(const char* column_name,
                                   const uvcpp_db_value& value) {
    const uvcpp_orm_column* column = this->resolve_column(column_name);
    if (column != nullptr) add_value(column, value);
    return *this;
  }

  template <typename M>
  uvcpp_orm_insert_builder& column(M T::* member,
                                   const uvcpp_db_value& value) {
    const uvcpp_orm_column* column = this->resolve_column(member);
    if (column != nullptr) add_value(column, value);
    return *this;
  }

  uvcpp_orm_sql_result build() const {
    uvcpp_orm_sql_result result;
    result.dialect = this->dialect_;

    std::string error;
    const uvcpp_orm_status validated =
        orm_detail::validate_statement<T>(this->dialect_, &error);
    if (validated != uvcpp_orm_status::OK) {
      result.status = validated;
      result.error = error;
      return result;
    }
    if (this->failed_) {
      result.status = this->status_;
      result.error = this->error_;
      return result;
    }
    if (values_.empty()) {
      result.status = uvcpp_orm_status::EMPTY_COLUMN_LIST;
      result.error = "insert requires at least one column";
      return result;
    }

    std::string sql = "INSERT INTO ";
    orm_detail::append_quoted(&sql, this->dialect_,
                              model_traits<T>::table_name());
    sql += " (";
    for (std::size_t i = 0; i < values_.size(); ++i) {
      if (i != 0u) sql += ", ";
      orm_detail::append_quoted(&sql, this->dialect_,
                                values_[i].column->column_name());
    }
    sql += ") VALUES (";

    std::size_t next_index = 0u;
    for (std::size_t i = 0; i < values_.size(); ++i) {
      if (i != 0u) sql += ", ";
      result.params.add(values_[i].value);
      orm_detail::append_placeholder(&sql, this->dialect_, &next_index);
    }
    sql += ")";

    result.status = uvcpp_orm_status::OK;
    result.sql = sql;
    return result;
  }

 private:
  struct entry {
    entry() : column(nullptr) {}
    const uvcpp_orm_column* column;
    uvcpp_db_value value;
  };

  void add_value(const uvcpp_orm_column* column,
                 const uvcpp_db_value& value) {
    entry item;
    item.column = column;
    item.value = value;
    values_.push_back(item);
  }

  std::vector<entry> values_;
};

// ---------------------------------------------------------------------------
// UPDATE
// ---------------------------------------------------------------------------

template <typename T>
class uvcpp_orm_update_builder
    : public orm_detail::uvcpp_orm_where_core<T,
                                              uvcpp_orm_update_builder<T> > {
 public:
  typedef orm_detail::uvcpp_orm_where_core<T,
                                           uvcpp_orm_update_builder<T> >
      base_type;

  explicit uvcpp_orm_update_builder(uvcpp_orm_dialect dialect)
      : base_type(dialect) {}

  /// SET 一列。值走参数；顺序就是 SET 子句的顺序。
  uvcpp_orm_update_builder& set(const char* column_name,
                                const uvcpp_db_value& value) {
    const uvcpp_orm_column* column = this->resolve_column(column_name);
    if (column != nullptr) add_assignment(column, value);
    return *this;
  }

  template <typename M>
  uvcpp_orm_update_builder& set(M T::* member,
                                const uvcpp_db_value& value) {
    const uvcpp_orm_column* column = this->resolve_column(member);
    if (column != nullptr) add_assignment(column, value);
    return *this;
  }

  uvcpp_orm_sql_result build() const {
    uvcpp_orm_sql_result result;
    result.dialect = this->dialect_;

    std::string error;
    const uvcpp_orm_status validated =
        orm_detail::validate_statement<T>(this->dialect_, &error);
    if (validated != uvcpp_orm_status::OK) {
      result.status = validated;
      result.error = error;
      return result;
    }
    if (this->failed_) {
      result.status = this->status_;
      result.error = this->error_;
      return result;
    }
    if (assignments_.empty()) {
      result.status = uvcpp_orm_status::EMPTY_SET;
      result.error = "update requires at least one set column";
      return result;
    }
    if (!this->has_where_conditions()) {
      result.status = uvcpp_orm_status::EMPTY_WHERE;
      result.error = "update requires a where condition";
      return result;
    }

    std::string sql = "UPDATE ";
    orm_detail::append_quoted(&sql, this->dialect_,
                              model_traits<T>::table_name());
    sql += " SET ";

    std::size_t next_index = 0u;
    for (std::size_t i = 0; i < assignments_.size(); ++i) {
      if (i != 0u) sql += ", ";
      orm_detail::append_quoted(&sql, this->dialect_,
                                assignments_[i].column->column_name());
      sql += " = ";
      result.params.add(assignments_[i].value);
      orm_detail::append_placeholder(&sql, this->dialect_, &next_index);
    }

    std::string where;
    const uvcpp_orm_status where_status =
        this->render_where(&where, &result.params, &next_index, &error);
    if (where_status != uvcpp_orm_status::OK) {
      result.status = where_status;
      result.error = error;
      return result;
    }
    sql += where;

    result.status = uvcpp_orm_status::OK;
    result.sql = sql;
    return result;
  }

 private:
  struct assignment {
    assignment() : column(nullptr) {}
    const uvcpp_orm_column* column;
    uvcpp_db_value value;
  };

  void add_assignment(const uvcpp_orm_column* column,
                      const uvcpp_db_value& value) {
    assignment item;
    item.column = column;
    item.value = value;
    assignments_.push_back(item);
  }

  std::vector<assignment> assignments_;
};

// ---------------------------------------------------------------------------
// DELETE
// ---------------------------------------------------------------------------

template <typename T>
class uvcpp_orm_delete_builder
    : public orm_detail::uvcpp_orm_where_core<T,
                                              uvcpp_orm_delete_builder<T> > {
 public:
  typedef orm_detail::uvcpp_orm_where_core<T,
                                           uvcpp_orm_delete_builder<T> >
      base_type;

  explicit uvcpp_orm_delete_builder(uvcpp_orm_dialect dialect)
      : base_type(dialect) {}

  uvcpp_orm_sql_result build() const {
    uvcpp_orm_sql_result result;
    result.dialect = this->dialect_;

    std::string error;
    const uvcpp_orm_status validated =
        orm_detail::validate_statement<T>(this->dialect_, &error);
    if (validated != uvcpp_orm_status::OK) {
      result.status = validated;
      result.error = error;
      return result;
    }
    if (this->failed_) {
      result.status = this->status_;
      result.error = this->error_;
      return result;
    }
    if (!this->has_where_conditions()) {
      result.status = uvcpp_orm_status::EMPTY_WHERE;
      result.error = "delete requires a where condition";
      return result;
    }

    std::string sql = "DELETE FROM ";
    orm_detail::append_quoted(&sql, this->dialect_,
                              model_traits<T>::table_name());

    std::size_t next_index = 0u;
    std::string where;
    const uvcpp_orm_status where_status =
        this->render_where(&where, &result.params, &next_index, &error);
    if (where_status != uvcpp_orm_status::OK) {
      result.status = where_status;
      result.error = error;
      return result;
    }
    sql += where;

    result.status = uvcpp_orm_status::OK;
    result.sql = sql;
    return result;
  }
};

// ---------------------------------------------------------------------------
// 构造入口
// ---------------------------------------------------------------------------

template <typename T>
inline uvcpp_orm_select_builder<T> select(uvcpp_orm_dialect dialect) {
  return uvcpp_orm_select_builder<T>(dialect);
}

template <typename T>
inline uvcpp_orm_insert_builder<T> insert(uvcpp_orm_dialect dialect) {
  return uvcpp_orm_insert_builder<T>(dialect);
}

template <typename T>
inline uvcpp_orm_update_builder<T> update(uvcpp_orm_dialect dialect) {
  return uvcpp_orm_update_builder<T>(dialect);
}

template <typename T>
inline uvcpp_orm_delete_builder<T> delete_from(uvcpp_orm_dialect dialect) {
  return uvcpp_orm_delete_builder<T>(dialect);
}

// ---------------------------------------------------------------------------
// raw() 逃生舱
// ---------------------------------------------------------------------------

namespace orm_detail {

/// 逐字符扫描 SQL，跳过引号内的内容与注释，数出占位符。
///
/// 这不是 SQL 解析器：它不懂 PostgreSQL 的 `$tag$...$tag$` 字符串，也不懂
/// MySQL 反斜杠转义。`raw()` 是给调用方自己写的 SQL 用的，调用方要为“这段
/// 扫描没看错”负责。
inline void scan_raw_placeholders(const std::string& sql,
                                  uvcpp_orm_dialect dialect,
                                  std::size_t* question_marks,
                                  std::vector<std::size_t>* dollar_indices) {
  *question_marks = 0u;
  dollar_indices->clear();
  const bool postgres = dialect == uvcpp_orm_dialect::postgresql;
  const bool mysql = dialect == uvcpp_orm_dialect::mysql;

  enum scan_state { normal, single_quote, double_quote, backtick, line_comment,
                    block_comment };
  scan_state state = normal;

  for (std::size_t i = 0; i < sql.size(); ++i) {
    const char c = sql[i];
    const char next = (i + 1u < sql.size()) ? sql[i + 1u] : '\0';

    switch (state) {
      case single_quote:
        if (c == '\'') {
          if (next == '\'') {
            ++i;  // '' 是转义的单引号
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
        } else if (mysql && c == '#') {
          state = line_comment;
        } else if (!postgres && c == '?') {
          ++(*question_marks);
        } else if (postgres && c == '$' && next >= '0' && next <= '9') {
          std::size_t value = 0u;
          std::size_t j = i + 1u;
          while (j < sql.size() && sql[j] >= '0' && sql[j] <= '9') {
            const std::size_t digit =
                static_cast<std::size_t>(sql[j] - '0');
            // Windows headers define a `max` macro; parenthesise the member
            // call so MSVC still parses `numeric_limits<size_t>::max()` as a
            // function and does not expand the macro.
            const std::size_t maximum =
                (std::numeric_limits<std::size_t>::max)();
            if (value > (maximum - digit) / 10u) {
              value = maximum;  // 饱和，后面会被“不可能连续”拒掉
            } else {
              value = value * 10u + digit;
            }
            ++j;
          }
          dollar_indices->push_back(value);
          i = j - 1u;
        }
        break;
    }
  }
}

/// PostgreSQL 的占位符必须是连续的 `$1..$n`。返回期望的参数个数。
/// 不连续（例如只有 `$2`）时返回 false。
inline bool contiguous_dollar_placeholders(
    const std::vector<std::size_t>& indices, std::size_t* count) {
  *count = 0u;
  if (indices.empty()) return true;

  std::size_t maximum = 0u;
  for (std::size_t i = 0; i < indices.size(); ++i) {
    if (indices[i] == 0u) return false;  // $0 不是合法占位符
    // 连续序列里最大的编号不会超过占位符个数（$1..$n 只出现 n 个不同值）。
    // 这条上界同时挡住 `$999999999999...`：既不让它撑爆下面的位图，也不会
    // 因为 size_t 回绕而把巨大编号误当成小编号。
    if (indices[i] > indices.size()) return false;
    if (indices[i] > maximum) maximum = indices[i];
  }

  std::vector<bool> seen(maximum + 1u, false);
  for (std::size_t i = 0; i < indices.size(); ++i) {
    seen[indices[i]] = true;
  }
  for (std::size_t n = 1u; n <= maximum; ++n) {
    if (!seen[n]) return false;
  }

  *count = maximum;
  return true;
}

}  // namespace orm_detail

/// 原样返回调用方的 SQL，不做标识符转义、不改写值。
///
/// **警告**：这是逃生舱。表名/列名不会经过模型白名单，SQL 里的值也不会被
/// 参数化 —— 调用方必须自己保证 SQL 是常量结构、所有外部输入都在 `params`
/// 里。能不用就不要用；普通单表查询用 `select<T>()` / `insert<T>()` /
/// `update<T>()` / `delete_from<T>()`。
///
/// 只做两个检查：方言合法、占位符数量与参数数量一致（SQLite/MySQL 数 `?`，
/// PostgreSQL 要求 `$1..$n` 连续）。
inline uvcpp_orm_sql_result raw(
    uvcpp_orm_dialect dialect, const std::string& sql,
    const uvcpp_db_params& params = uvcpp_db_params()) {
  uvcpp_orm_sql_result result;
  result.dialect = dialect;

  if (!uvcpp_orm_dialect_is_valid(dialect)) {
    result.status = uvcpp_orm_status::INVALID_DIALECT;
    result.error = "unknown SQL dialect";
    return result;
  }
  if (sql.find_first_not_of(" \t\r\n") == std::string::npos) {
    result.status = uvcpp_orm_status::EMPTY_RAW_SQL;
    result.error = "raw SQL is empty";
    return result;
  }

  std::size_t question_marks = 0u;
  std::vector<std::size_t> dollar_indices;
  const bool postgres = dialect == uvcpp_orm_dialect::postgresql;
  orm_detail::scan_raw_placeholders(sql, dialect, &question_marks,
                                    &dollar_indices);

  std::size_t expected = 0u;
  bool contiguous = true;
  if (postgres) {
    contiguous =
        orm_detail::contiguous_dollar_placeholders(dollar_indices, &expected);
  } else {
    expected = question_marks;
  }

  if (!contiguous) {
    result.status = uvcpp_orm_status::PARAM_COUNT_MISMATCH;
    result.error =
        "PostgreSQL placeholders in raw SQL must be a contiguous $1..$n "
        "sequence";
    return result;
  }
  if (expected != params.size()) {
    result.status = uvcpp_orm_status::PARAM_COUNT_MISMATCH;
    result.error = "raw SQL has " + std::to_string(expected) +
                   " placeholder(s) but " + std::to_string(params.size()) +
                   " parameter(s) were supplied";
    return result;
  }

  result.status = uvcpp_orm_status::OK;
  result.sql = sql;
  result.params = params;
  return result;
}

}  // namespace uvcpp

#endif  // UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE
#endif  // SRC_ORM_UVCPP_ORM_QUERY_H
