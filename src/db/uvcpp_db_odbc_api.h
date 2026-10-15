/**
 * @file src/db/uvcpp_db_odbc_api.h
 * @brief ODBC（驱动管理器）函数表：`UVCPP_DB_RUNTIME_LOAD` 的那一半。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 与 `uvcpp_db_mysql_api.h` / `uvcpp_db_pgsql_api.h` 是同一个形状 —— 成员名就是符号
 * 名、清单只写一遍、类型从 ODBC 头的声明里 `decltype` 反推。为什么这么写、手抄三遍
 * 会怎么坏，`uvcpp_db_mysql_api.h` 那份注释说得更细，这里只说 ODBC 自己的三条：
 *
 *   · **链的是驱动管理器，不是具体驱动。** 这张表里的 24 个符号来自 Linux 的
 *     `libodbc.so.2`、Windows 的 `odbc32.dll`、macOS 的 `libodbc.2.dylib`；真正连
 *     哪个库（SQL Server 的 `msodbcsql18`、SQLite 的 `libsqlite3odbc.so`……）由**管理
 *     器**自己 `dlopen`，与这里无关。所以"管理器在不在"是这一层的事，"要连的数据源
 *     装没装驱动"仍然是 `open()` 才知道的事 —— 两件事别混。
 *   · 清单取自 `nm -u …uvcpp_db_odbc.cpp.o | sort`（**不是**照着源码里的标识符抄：
 *     `SQLGetInfo` 那种只出现在一句注释里的名字抄进来就是个永远找不到的符号 ——
 *     PostgreSQL 那份注释里也踩过同一个坑）。逐个与 `libodbc.so.2` 的导出核过，
 *     24 个全在（unixODBC 2.3.11）。
 *   · 这 24 个全是**真函数**，不是宏（见下面那道 `#error`）。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_ODBC_API_H
#define SRC_DB_UVCPP_DB_ODBC_API_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE && UVCPP_DB_ODBC_ENABLE

// ODBC 头里有两条**条件重定向宏**，它们会把名字换成另一个符号：
// `sqlext.h:2057` 的 `#define SQLAllocHandle SQLAllocHandleStd`（在 `ODBC_STD` 下，
// 而且换了之后**语义也不一样**）、`sqlucode.h:754` 的
// `#define SQLSetStmtAttr SQLSetStmtAttrW`（在 `UNICODE` 下，Windows SDK 里是一整族）。
//
// 本后端是**按名字**说话的：运行时加载那一半用 `#name` 去查符号，而 `#` **不展开**
// 实参（这正是我们要的，也正是 pgsql 那份清单敢用 `#name` 的原因）。于是"调用点被
// 宏重定向到 X、查的却是 Y"会在两种构建方式之间分叉，而且**不报错**。堵死在门口，
// 比事后对着 `nm` 猜便宜：这两个宏谁都不该在编本文件时是开着的。
#if defined(ODBC_STD) || defined(UNICODE)
#error "ODBC 后端按符号名做运行时加载：编 src/db/uvcpp_db_odbc.cpp 时 ODBC_STD 与 UNICODE 都必须未定义（它们会把 SQL* 名字重定向到别的符号，两种构建方式会分叉）。"
#endif

#include <sql.h>
#include <sqlext.h>

// unixODBC 的 `sqltypes.h` 第 75 行有一句 `#define BOOL int`（在没有 `<windows.h>`
// 的平台上补齐 ODBC 那套类型表）。它会把这个文件里唯一一处 `uvcpp_db_type::BOOL`
// 展开成 `uvcpp_db_type::int`，报出来的错是 "expected unqualified-id before 'int'"
// —— 离真正的原因隔着一层。这里直接 `#undef`：ODBC 头已经预处理完，这个宏的使命
// 结束了（理由与观察详见 `uvcpp_db_odbc.cpp` 文件头）。微软那套 `sqltypes.h` 不发
// 这个宏（Windows 上 `BOOL` 是 `<windows.h>` 里的 typedef），所以在 Windows 上这行
// 是空操作。
#undef BOOL

#include <string>

namespace uvcpp {
namespace db_detail {

/// 这张库要用的**全部**驱动管理器符号（24 个）。
/// 顺序与 `nm -u …uvcpp_db_odbc.cpp.o | sort` 一致。
///
/// 全是 `W` 形态的宽接口（或没有 A/W 之分的那些，如 `SQLAllocHandle`）：本后端的
/// 文本一律走 `SQLWCHAR`，见 `uvcpp_db_odbc.cpp` 文件头「坑二」。
#define UVCPP_DB_ODBC_SYMBOLS(X) \
  X(SQLAllocHandle)              \
  X(SQLBindParameter)            \
  X(SQLColumnsW)                 \
  X(SQLDescribeColW)             \
  X(SQLDisconnect)               \
  X(SQLDriverConnectW)           \
  X(SQLEndTran)                  \
  X(SQLExecDirectW)              \
  X(SQLExecute)                  \
  X(SQLFetch)                    \
  X(SQLFreeHandle)               \
  X(SQLGetData)                  \
  X(SQLGetDiagRecW)              \
  X(SQLGetInfoW)                 \
  X(SQLMoreResults)              \
  X(SQLNumParams)                \
  X(SQLNumResultCols)            \
  X(SQLPrepareW)                 \
  X(SQLPrimaryKeysW)             \
  X(SQLRowCount)                 \
  X(SQLSetConnectAttr)           \
  X(SQLSetEnvAttr)               \
  X(SQLSetStmtAttr)              \
  X(SQLTablesW)

/// 函数表。成员名 = 符号名。
struct odbc_api {
#define UVCPP_DB_ODBC_DECLARE(name) decltype(&::name) name;
  UVCPP_DB_ODBC_SYMBOLS(UVCPP_DB_ODBC_DECLARE)
#undef UVCPP_DB_ODBC_DECLARE
};

#if UVCPP_DB_RUNTIME_LOAD

/// 取函数表。永不返回空，理由见 `uvcpp_db_mysql_api.h` 里同一处。
const odbc_api& uvcpp_db_odbc_api();

/// 加载失败的原因（人读的一句话）；成功是空串。进程级、只算一次。
const std::string& uvcpp_db_odbc_load_error();

#else  // 直接链接

/// 编译期就填好的表。
const odbc_api& uvcpp_db_odbc_api();

/// 直接链接形态下永远是空串。见 `uvcpp_db_mysql_api.h` 里同一处的理由。
inline const std::string& uvcpp_db_odbc_load_error() {
  static const std::string kNoError;
  return kNoError;
}

#endif  // UVCPP_DB_RUNTIME_LOAD

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE && UVCPP_DB_ODBC_ENABLE
#endif  // SRC_DB_UVCPP_DB_ODBC_API_H
