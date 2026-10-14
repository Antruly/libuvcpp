/**
 * @file src/db/uvcpp_db_pgsql_api.h
 * @brief PostgreSQL（libpq）函数表：`UVCPP_DB_RUNTIME_LOAD` 的那一半。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 与 `uvcpp_db_mysql_api.h` 是同一个形状 —— 成员名就是符号名、三处清单只写一遍、
 * 类型从 `libpq-fe.h` 的声明里 `decltype` 反推。为什么这么写、手抄三遍会怎么坏，
 * 那份注释说得更细，这里不重复。
 *
 * 这份清单比 MySQL 那份短（21 个），而且**稳定得多**：`PQ*` 这一层从 PG 9 至今
 * 没有删改过（新增的都是 `PQresultMemorySize` 这类可选件）。仍然逐个 `dlsym` 并
 * 报缺哪个，理由不是"它会变"，而是"少一个的后果是跳到空指针"。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_PGSQL_API_H
#define SRC_DB_UVCPP_DB_PGSQL_API_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE && UVCPP_DB_PGSQL_ENABLE

#include <libpq-fe.h>

#include <string>

namespace uvcpp {
namespace db_detail {

/// 这张库要用的**全部** libpq 符号（21 个）。
/// 顺序与 `nm -u …uvcpp_db_pgsql.cpp.o | sort` 一致。
///
/// 清单里**没有** `PQunescapeBytea`：它只出现在源码的一句注释里（讲
/// `bytea_output='hex'` 为什么不靠它），`nm -u` 里也没有它。照源码里的标识符
/// 抄清单就会把这种"注释里提到的名字"抄进来，得到一个永远找不到的符号。
#define UVCPP_DB_PGSQL_SYMBOLS(X) \
  X(PQclear)                      \
  X(PQcmdTuples)                  \
  X(PQconnectdbParams)            \
  X(PQerrorMessage)               \
  X(PQescapeIdentifier)           \
  X(PQescapeStringConn)           \
  X(PQexec)                       \
  X(PQexecParams)                 \
  X(PQfinish)                     \
  X(PQfname)                      \
  X(PQfreemem)                    \
  X(PQftype)                      \
  X(PQgetisnull)                  \
  X(PQgetlength)                  \
  X(PQgetvalue)                   \
  X(PQnfields)                    \
  X(PQntuples)                    \
  X(PQresultErrorField)           \
  X(PQresultErrorMessage)         \
  X(PQresultStatus)               \
  X(PQstatus)

/// 函数表。成员名 = 符号名。
struct pgsql_api {
#define UVCPP_DB_PGSQL_DECLARE(name) decltype(&::name) name;
  UVCPP_DB_PGSQL_SYMBOLS(UVCPP_DB_PGSQL_DECLARE)
#undef UVCPP_DB_PGSQL_DECLARE
};

#if UVCPP_DB_RUNTIME_LOAD

/// 取函数表。永不返回空，理由见 `uvcpp_db_mysql_api.h` 里同一处。
const pgsql_api& uvcpp_db_pgsql_api();

/// 加载失败的原因（人读的一句话）；成功是空串。进程级、只算一次。
const std::string& uvcpp_db_pgsql_load_error();

#else  // 直接链接

/// 编译期就填好的表。
const pgsql_api& uvcpp_db_pgsql_api();

/// 直接链接形态下永远是空串。见 `uvcpp_db_mysql_api.h` 里同一处的理由。
inline const std::string& uvcpp_db_pgsql_load_error() {
  static const std::string kNoError;
  return kNoError;
}

#endif  // UVCPP_DB_RUNTIME_LOAD

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE && UVCPP_DB_PGSQL_ENABLE
#endif  // SRC_DB_UVCPP_DB_PGSQL_API_H
