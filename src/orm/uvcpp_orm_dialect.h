/**
 * @file src/orm/uvcpp_orm_dialect.h
 * @brief ORM 的 SQL 方言：占位符写法与标识符转义。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 方言由调用方**显式**传一个 `uvcpp_orm_dialect` 枚举进来，不从“当前连接”
 * 猜。这条规矩来自 `src/db/uvcpp_db_params.h` 里记的事故：同一个查询在
 * SQLite/MySQL 上是 `?`、在 PostgreSQL 上是 `$1..$n`，两家的占位符**不能**
 * 互换算（PostgreSQL 的 `?` 是合法操作符，SQLite 又把 `$1` 当命名参数收下）。
 *
 * 这里只做两件事：
 *
 *   * `uvcpp_orm_placeholder(dialect, n)`：第 `n` 个（**从 1 开始**）占位符；
 *   * `uvcpp_orm_quote_identifier(dialect, name)`：给标识符加引号，并把
 *     名字里**同一种引号**翻倍。MySQL 用反引号，SQLite/PostgreSQL 用双引号。
 *
 * 转义只处理“名字里有特殊字符”，不负责“名字是恶意的”。表名/列名必须来自
 * `model_traits<T>`；查询构造器不会接受任意字符串当表名。
 *
 * ## 为什么 SQLite 和 PostgreSQL 分两个枚举值
 *
 * 两者的标识符转义相同，但占位符不同。把 SQLite 合并进 PostgreSQL 会让
 * “SQLite 收下 `$1`”变成一条静默的错误路径；这一层不做这种猜测。
 */

#pragma once
#ifndef SRC_ORM_UVCPP_ORM_DIALECT_H
#define SRC_ORM_UVCPP_ORM_DIALECT_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE

#include <cstddef>
#include <string>

namespace uvcpp {

/// 支持的 SQL 方言。值固定，便于日志与测试断言。
enum class uvcpp_orm_dialect : int {
  sqlite = 0,
  mysql = 1,
  postgresql = 2,
};

/// 稳定的短名："sqlite" / "mysql" / "postgresql"；未知值返回 "unknown"。
inline const char* uvcpp_orm_dialect_name(uvcpp_orm_dialect dialect) {
  switch (dialect) {
    case uvcpp_orm_dialect::sqlite: return "sqlite";
    case uvcpp_orm_dialect::mysql: return "mysql";
    case uvcpp_orm_dialect::postgresql: return "postgresql";
  }
  return "unknown";
}

/// 枚举值是不是那三个之一。`enum class` 仍然可以被 `static_cast` 出界，
/// 查询构造入口会先过这一道。
inline bool uvcpp_orm_dialect_is_valid(uvcpp_orm_dialect dialect) {
  switch (dialect) {
    case uvcpp_orm_dialect::sqlite:
    case uvcpp_orm_dialect::mysql:
    case uvcpp_orm_dialect::postgresql: return true;
  }
  return false;
}

/// 标识符的引号字符：MySQL 是反引号，SQLite/PostgreSQL 是双引号。方言非法
/// 时返回 `'\0'`。
inline char uvcpp_orm_identifier_quote(uvcpp_orm_dialect dialect) {
  switch (dialect) {
    case uvcpp_orm_dialect::mysql: return '`';
    case uvcpp_orm_dialect::sqlite:
    case uvcpp_orm_dialect::postgresql: return '"';
  }
  return '\0';
}

/// 第 `index` 个占位符（**从 1 开始**）：SQLite/MySQL 一律 `?`，
/// PostgreSQL 是 `$index`。方言非法时返回空串 —— 调用方应当先用
/// `uvcpp_orm_dialect_is_valid()`。
inline std::string uvcpp_orm_placeholder(uvcpp_orm_dialect dialect,
                                         std::size_t index) {
  switch (dialect) {
    case uvcpp_orm_dialect::sqlite:
    case uvcpp_orm_dialect::mysql: return "?";
    case uvcpp_orm_dialect::postgresql:
      return std::string("$") + std::to_string(index);
  }
  return std::string();
}

/// 给标识符加引号。名字里与引号字符相同的字符会翻倍：
///
///   * SQLite/PostgreSQL: `a"b` → `"a""b"`；
///   * MySQL: ``a`b`` → `` `a``b` ``。
///
/// 名字里的**另一种**引号原样保留（MySQL 里的 `"`、SQLite/PG 里的反引号都
/// 不是引号字符）。方言非法时返回空串，不返回一个未加引号的名字 —— 那会
/// 把“方言传错了”变成一条能跑但语义不同的 SQL。
inline std::string uvcpp_orm_quote_identifier(uvcpp_orm_dialect dialect,
                                              const std::string& identifier) {
  const char quote = uvcpp_orm_identifier_quote(dialect);
  if (quote == '\0') return std::string();

  std::string out;
  out.reserve(identifier.size() + 2u);
  out.push_back(quote);
  for (std::size_t i = 0; i < identifier.size(); ++i) {
    const char c = identifier[i];
    if (c == quote) out.push_back(c);
    out.push_back(c);
  }
  out.push_back(quote);
  return out;
}

}  // namespace uvcpp

#endif  // UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE
#endif  // SRC_ORM_UVCPP_ORM_DIALECT_H
