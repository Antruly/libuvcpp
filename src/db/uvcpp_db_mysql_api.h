/**
 * @file src/db/uvcpp_db_mysql_api.h
 * @brief MySQL 客户端函数表：`UVCPP_DB_RUNTIME_LOAD` 的那一半。
 * @author zhuweiye
 * @version 1.0.0
 *
 * **私有头**，理由同 `uvcpp_db_runtime_load.h`。
 *
 * 这里只有一件东西：一张**成员名就是符号名**的函数表（`mysql_query` 成员对应
 * `mysql_query` 符号）。于是调用点从
 *
 *     mysql_query(conn_, sql.c_str())
 *     改成
 *     api().mysql_query(conn_, sql.c_str())
 *
 * 除此之外一个字都不用动，而 `grep -n mysql_query src/db/` 照样能把两边的用法
 * 一起找出来 —— 这比给 `mysql_*` 加一层宏伪装好：那些名字在 `mysql.h` 里
 * 有自己的声明甚至宏（`mysql_library_init` 就是一个），伪装会和它们打架。
 *
 * ## 为什么是 X 宏
 *
 * 这批符号要出现在**三个**地方，而且三处的顺序/集合必须完全一致：
 *
 *   1. 结构体的成员声明（类型从 `mysql.h` 的声明里 `decltype` 反推，不手抄）；
 *   2. 直接链接形态下那张表的初始化列表（位置初始化）；
 *   3. 运行时加载形态下逐个 `dlsym` 的绑定清单。
 *
 * 手抄三遍的后果不对称，所以不能靠人看：
 *
 *   * (1) 与 (2) 对不上大多会被编译器抓住（`mysql_init` 返回 `MYSQL*`、
 *     `mysql_error` 返回 `const char*`，错位就类型不符）—— **但**相邻两项
 *     类型恰好相同时不会（`mysql_stmt_errno` 与 `mysql_stmt_execute` 就都是
 *     `int(MYSQL_STMT*)`），那时错位是静默的、跑起来才发现调错了函数。
 *   * (2) 与 (3) 对不上更糟：运行时加载形态下**没有链接器兜底**（这批符号一个
 *     都不出现在 `nm -u` 里了，这正是这个开关的目的），漏绑一个成员就是表里
 *     留了一个空指针，第一次调用它时跳到 0。
 *
 * 所以符号清单**只写一遍**（`UVCPP_DB_MYSQL_SYMBOLS`），三处都由它展开。清单本身
 * 是从 `nm -u` 抄的，不是从源码里的标识符抄的 —— 差别是实的：源码里写的是
 * `mysql_library_init` 而符号叫 `mysql_server_init`（`mysql.h:421` 的宏），
 * 照源码抄会得到一个永远找不到的名字。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_MYSQL_API_H
#define SRC_DB_UVCPP_DB_MYSQL_API_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE && UVCPP_DB_MYSQL_ENABLE

// 与 `uvcpp_db_mysql.cpp` 同一套两级查找：Debian/Ubuntu 装在 `<mysql/mysql.h>`，
// 官方 tarball 与 vcpkg 是扁平的 `<mysql.h>`。放在这里（而不是各 .cpp 各写一遍）
// 是因为这个头就是"要 mysql.h 的类型"的那个头。
#if defined(__has_include)
#if __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#else
#include <mysql.h>
#endif
#else
#include <mysql.h>
#endif

#include <string>

namespace uvcpp {
namespace db_detail {

/// 这张库要用的**全部** MySQL C API 符号（33 个，含 `mysql_server_init`）。
/// 顺序与 `nm -u …uvcpp_db_mysql.cpp.o | sort` 一致，方便逐行对照。
#define UVCPP_DB_MYSQL_SYMBOLS(X)  \
  X(mysql_affected_rows)           \
  X(mysql_close)                   \
  X(mysql_errno)                   \
  X(mysql_error)                   \
  X(mysql_fetch_fields)            \
  X(mysql_fetch_lengths)           \
  X(mysql_fetch_row)               \
  X(mysql_free_result)             \
  X(mysql_init)                    \
  X(mysql_insert_id)               \
  X(mysql_num_fields)              \
  X(mysql_options)                 \
  X(mysql_ping)                    \
  X(mysql_query)                   \
  X(mysql_real_connect)            \
  X(mysql_real_escape_string)      \
  X(mysql_server_init)             \
  X(mysql_set_character_set)       \
  X(mysql_stmt_affected_rows)      \
  X(mysql_stmt_bind_param)         \
  X(mysql_stmt_bind_result)        \
  X(mysql_stmt_close)              \
  X(mysql_stmt_errno)              \
  X(mysql_stmt_error)              \
  X(mysql_stmt_execute)            \
  X(mysql_stmt_fetch)              \
  X(mysql_stmt_fetch_column)       \
  X(mysql_stmt_init)               \
  X(mysql_stmt_param_count)        \
  X(mysql_stmt_prepare)            \
  X(mysql_stmt_result_metadata)    \
  X(mysql_stmt_store_result)       \
  X(mysql_store_result)

/// 函数表。成员名 = 符号名；类型一律 `decltype(&::符号)`，**不手抄函数签名** ——
/// 手抄的签名在 `STDCALL`（Windows 32 位下 mysql.h 会给这些声明加上调用约定）
/// 和各家版本之间会漂移，而 `decltype` 是编译器从那份真的头文件里读出来的。
struct mysql_api {
#define UVCPP_DB_MYSQL_DECLARE(name) decltype(&::name) name;
  UVCPP_DB_MYSQL_SYMBOLS(UVCPP_DB_MYSQL_DECLARE)
#undef UVCPP_DB_MYSQL_DECLARE
};

#if UVCPP_DB_RUNTIME_LOAD

/// 取函数表。**永不返回空**：加载失败时返回的是一张全空的表，调用点靠下面那个
/// 错误串先挡一道（`mysql_driver::open()` 里的第一件事），所以走到任何一次
/// `api().xxx(...)` 时它必然是满的。
const mysql_api& uvcpp_db_mysql_api();

/// 加载失败的原因，人读的一句话（试过哪些名字、各自为什么不行）；成功是空串。
///
/// 结论是**进程级且只算一次**的，所以可以反复问、结果一样（装在别处的库不会
/// 因为多问几次就被发现）。
const std::string& uvcpp_db_mysql_load_error();

#else  // 直接链接

/// 表在**编译期**就填好（每一项都是指向已链接符号的地址常量，落在
/// `.data.rel.ro` 里，没有运行时初始化，也没有 magic static 的那道检查）。
const mysql_api& uvcpp_db_mysql_api();

/// 直接链接的形态下不存在"加载失败"：符号在链接期就解析完了，缺一个的话
/// 这份库根本生不出来。留着这个函数只是为了让调用点的形状在两种形态下**完全
/// 一样**（`open()` 里那句 `if (!e.empty())` 不用按形态分叉）。
inline const std::string& uvcpp_db_mysql_load_error() {
  static const std::string kNoError;
  return kNoError;
}

#endif  // UVCPP_DB_RUNTIME_LOAD

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE && UVCPP_DB_MYSQL_ENABLE
#endif  // SRC_DB_UVCPP_DB_MYSQL_API_H
