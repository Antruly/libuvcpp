/**
 * @file src/orm/uvcpp_orm_error.h
 * @brief ORM 元数据校验的返回码与人读结果。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 与 `src/db/uvcpp_db_status.h` 同一条规矩：**不抛异常**。元数据是编译期
 * 声明的，但校验发生在运行期（`validate_model<T>()`），失败是调用方必须显式
 * 处理的分支，不是异常情形。
 *
 * 返回码只区分“哪一类失败”；具体是哪个字段、哪个名字，写在
 * `uvcpp_orm_validation_result::error`（或 `validate_model<T>(std::string*)`
 * 的 `error` 出参）里。错误串一定带字段名 —— 一个只说“模型不合法”的结果，
 * 在几十个字段的模型上等于没说。
 */

#pragma once
#ifndef SRC_ORM_UVCPP_ORM_ERROR_H
#define SRC_ORM_UVCPP_ORM_ERROR_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE

#include <string>
#include <utility>

namespace uvcpp {

enum class uvcpp_orm_status : int {
  /// 校验通过。
  OK = 0,

  /// 这个类型没有声明过模型（`model_traits<T>::table_name()` 是空指针）。
  NO_MODEL,

  /// 模型没有字段。没有字段就不可能有主键。
  NO_COLUMNS,

  /// 表名是空串。
  EMPTY_TABLE_NAME,

  /// 表名里有控制字符。
  INVALID_TABLE_NAME,

  /// 列名是空串或空指针。
  EMPTY_COLUMN_NAME,

  /// 列名里有控制字符。
  INVALID_COLUMN_NAME,

  /// 两列用了同一个列名（按 ASCII 大小写不敏感比较）。
  DUPLICATE_COLUMN,

  /// 一个主键都没有。
  NO_PRIMARY_KEY,

  /// 有不止一个主键。1.12.0 只支持单列主键。
  MULTIPLE_PRIMARY_KEYS,

  /// 自增标志用错了：不在主键上，或者字段不是整数类型。
  INVALID_AUTO_INCREMENT,

  /// 字段类型没有映射（`char`、自定义结构体等）。
  UNSUPPORTED_TYPE,

  /// 标志互相矛盾（例如 `nullable<T>` 同时标了 `not_null`）。
  CONFLICTING_FLAGS,

  // ---- SQL 构造（#90）----

  /// 方言不是 SQLite / MySQL / PostgreSQL 之一（通常是 `static_cast` 出来的）。
  INVALID_DIALECT,

  /// 列名/成员指针不在 `model_traits<T>` 里。查询构造只认模型元数据。
  UNKNOWN_COLUMN,

  /// SELECT / INSERT 一个列都没有。
  EMPTY_COLUMN_LIST,

  /// 操作符不是 `uvcpp_orm_operator` 里定义的那几个。
  INVALID_OPERATOR,

  /// `IN` 的值集合为空。空集合在 SQL 里没有可移植的写法。
  EMPTY_IN_VALUES,

  /// ORDER BY 方向不是 ASC / DESC 枚举。
  INVALID_ORDER_DIRECTION,

  /// LIMIT 是负数。
  INVALID_LIMIT,

  /// OFFSET 是负数。
  INVALID_OFFSET,

  /// 只给了 OFFSET、没给 LIMIT。SQLite/MySQL 都要求两者同时出现。
  OFFSET_WITHOUT_LIMIT,

  /// UPDATE / DELETE 没有 WHERE。拒绝全表写操作。
  EMPTY_WHERE,

  /// UPDATE 一个 SET 列都没有。
  EMPTY_SET,

  /// COUNT 查询不能和列选择 / ORDER BY / LIMIT / OFFSET 组合。
  INVALID_COUNT_QUERY,

  /// `raw()` 收到空 SQL（或只有空白）。
  EMPTY_RAW_SQL,

  /// SQL 里的占位符数量与 `uvcpp_db_params` 的数量对不上。
  PARAM_COUNT_MISMATCH,

  // ---- 执行与结果映射（#91）----

  /// 查询成功，但一行都没有（目前只由 `find_by_pk<T>()` 返回）。
  /// 与 `DB_ERROR` 分开：主键没找到不是数据库失败，调用方通常要分支处理。
  NOT_FOUND,

  /// 结果集与模型对不上，且不属于下面几种更具体的类别。因为某个字段映射
  /// 失败而整体放弃；错误串里带行号、字段名与列名。
  MAPPING_FAILED,

  /// 结果集缺一个**非空**模型字段的列。可空字段缺列是合法的（写空
  /// `nullable<T>`），非空字段缺列无法凭空造值。错误串里带字段名与列名。
  MISSING_COLUMN,

  /// 结果值的类型不能映射到模型字段（例如 TEXT 值映射到 `std::int32_t`，
  /// 或整数超出字段宽度）。错误串里带字段名、列名与实际类型。
  TYPE_MISMATCH,

  /// SQL NULL 落到了非空字段上。`nullable<T>` 收到 NULL 是成功的空态，
  /// 不算这一条。
  NULL_VIOLATION,

  /// 数据库执行失败。具体后端返回码保存在 `uvcpp_db_status` 出参里（以及
  /// `db.last_error()` 的原文里）；`error` 串里也会带返回码短名与原文。
  DB_ERROR,

  /// 仓库 API 的参数不合法（空 out 指针、空/NULL 主键、SQL 方言与连接
  /// 不匹配等）。这是调用方的用法错误，不是数据库错误。
  INVALID_ARGUMENT,
};

/// 返回码的稳定短名（小写下划线），用于日志与测试断言。
UVCPP_API const char* uvcpp_orm_status_name(uvcpp_orm_status status);

inline bool uvcpp_orm_ok(uvcpp_orm_status status) {
  return status == uvcpp_orm_status::OK;
}

/// `validate_model<T>()` 的完整结果：返回码 + 人读原因。
struct uvcpp_orm_validation_result {
  uvcpp_orm_validation_result() : status(uvcpp_orm_status::OK) {}

  uvcpp_orm_validation_result(uvcpp_orm_status result, std::string message)
      : status(result), error(std::move(message)) {}

  bool ok() const { return status == uvcpp_orm_status::OK; }
  explicit operator bool() const { return ok(); }

  uvcpp_orm_status status;
  std::string error;
};

}  // namespace uvcpp

#endif  // UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE
#endif  // SRC_ORM_UVCPP_ORM_ERROR_H
