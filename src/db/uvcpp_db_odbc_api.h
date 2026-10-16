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

// Windows 的 ODBC 头**不是自给自足**的，`<windows.h>` 必须先来。
//
// 微软那套 `sqltypes.h` 是拿 Windows 自己的类型拼出 ODBC 类型表的：`SQLLEN` /
// `SQLULEN` 走 `INT64` / `UINT64`（`winnt.h`），`SQLHWND` 走 `HWND`（`windef.h`），
// `SQLGUID` 走 `GUID`（`guiddef.h`）—— 全在 `<windows.h>` 里。少了它，第一条错是
// `sqltypes.h(64,9): error C4430: missing type specifier` 指着 `typedef INT64 SQLLEN;`，
// 看上去像 Windows SDK 坏了，其实一行关系都没有（真试过：照着这个症状去换 SDK 版本，
// 换到最新的一样红）。
//
// unixODBC 的 `sqltypes.h` 相反：它**自己补一套**类型（`#define BOOL int` 那种就是这套补丁
// 的一部分），所以 Linux 上漏掉这个顺序照样编得过。补不补看的是一个叫
// `ALREADY_HAVE_WINDOWS_TYPE` 的宏、**不是** `<windows.h>` 在不在 —— 于是 mingw 上会多出
// 下面那段只有 mingw 才有的第二层。顺序坑本身只在 Windows 现形 —— 也正因为这样，它在这份
// 代码第一次上 Windows 之前一直没露头。
//
// `WIN32_LEAN_AND_MEAN` + `NOMINMAX` 是本文件自带的两条卫生：这个是**公共头**，谁包含
// 它就等于把 `<windows.h>` 拖进自己的 TU（本仓 `uvcpp_web_json_reflect.h` 那一段注释记
// 的正是跟着来的 `std::numeric_limits<T>::max` 被当成宏调用）。少拉一半 Windows 头，外加
// 不放出 `min` / `max` 两个宏，代价为零。
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

// mingw 上的第二层。unixODBC 那份 `sqltypes.h` 里"补 windows 类型"那一段的开关是
// `ALREADY_HAVE_WINDOWS_TYPE` —— **这个宏谁都不定义**，所以哪怕 `<windows.h>` 刚从上面进来
// 过，它照样补一遍。补出来的 `CHAR` / `WORD` / `DWORD` / `BYTE` / `LPDWORD` 与 `winnt.h`
// 逐字相同（C++ 里重复 typedef 合法，一声不响），但 `WCHAR` 不同：它给 `unsigned short`，
// 而 `winnt.h` 早已 `typedef wchar_t WCHAR` —— **硬冲突，也是两条 mingw 发布腿唯一的编译
// 错误**：
//
//   sqltypes.h 第 109 行: error: conflicting declaration 'typedef short unsigned int WCHAR'
//   winnt.h 第 320 行:    note: previous declaration as 'typedef wchar_t WCHAR'
//
// MSVC 上不冲突：Windows SDK 那份 `sqltypes.h` 是微软自己写的，没有这一整段补丁。这个坑与
// 上面那个顺序坑同源，**只在 mingw 现形**。
//
// 解法就是这段补丁自己的开关 `SQL_WCHART_CONVERT`（`sqltypes.h` 第 106 行）：打开后 `WCHAR` 是
// `wchar_t`，与 `winnt.h` 逐字相同，冲突消失。语义上也更对 —— Windows 的 ODBC 宽字符接口
// 本来就是 `wchar_t`（2 字节），`unsigned short` 那个是 unixODBC 给"没有 `<windows.h>` 的
// 平台"准备的默认值（它自己的注释就这么写的，`sqltypes.h` 第 30-38 行）。`SQLWCHAR` 由
// `typedef WCHAR` 派生（第 467 行），跟着一起对，**尺寸不变、仍是 2 字节**。
//
// 判据写成"Windows 上但不是 MSVC"而不是 `__MINGW32__`：真正的分界是"手上这份 `sqltypes.h`
// 是 unixODBC 的还是微软的"，而 Windows 上非 MSVC 的工具链拿到的就是 unixODBC 那份；万一
// 有人拿 mingw 去配 SDK 的头，这个宏在微软那份里**一次都没被引用**，是空操作、不会误伤。
// Linux 上必须**不**开：那里 `wchar_t` 是 4 字节，开了就把 `SQLWCHAR` 从 2 字节改成 4 字节
// —— 那是改 ABI，不是修 bug。
#if defined(_WIN32) && !defined(_MSC_VER)
#  define SQL_WCHART_CONVERT
// 同一段补丁里的 `#define CALLBACK`（空定义）会盖掉 `minwindef.h` 的
// `#define CALLBACK __stdcall`，GCC 因此报一句 `'CALLBACK' redefined`；而它**出完 include
// 也不复原**，等于把这个宏借走不还。借出去、还回来：`#undef` 之后 unixODBC 那句成了全新
// 定义（不报重定义），`pop_macro` 再把 `minwindef` 的原定义还回去。万一哪个编译器不认这
// 两个 pragma，它们按标准是被忽略的，结果就退回今天这个样子（一句警告 + 宏留在空定义），
// 不更坏。
#  pragma push_macro("CALLBACK")
#  undef CALLBACK
#endif

#include <sql.h>
#include <sqlext.h>

#if defined(_WIN32) && !defined(_MSC_VER)
#  pragma pop_macro("CALLBACK")
#endif

// unixODBC 的 `sqltypes.h` 第 75 行有一句 `#define BOOL int`（在没有 `<windows.h>`
// 的平台上补齐 ODBC 那套类型表）。它会把这个文件里唯一一处 `uvcpp_db_type::BOOL`
// 展开成 `uvcpp_db_type::int`，报出来的错是 "expected unqualified-id before 'int'"
// —— 离真正的原因隔着一层。这里直接 `#undef`：ODBC 头已经预处理完，这个宏的使命
// 结束了（理由与观察详见 `uvcpp_db_odbc.cpp` 文件头）。微软那套 `sqltypes.h` 不发
// 这个宏（Windows 上 `BOOL` 是上面那个 `<windows.h>` 里的 typedef），所以在 Windows
// 上这行是空操作 —— 这也是为什么上面那个 include 不能省：不包含 `<windows.h>` 时
// `BOOL` 既不是 ODBC 头给的宏、也没有 typedef 兜着，`uvcpp_db_type::BOOL` 会直接
// 变成未定义标识符。
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
