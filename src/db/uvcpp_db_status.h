/**
 * @file src/db/uvcpp_db_status.h
 * @brief 数据库模块的返回码。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 本模块**不抛异常**（与库内其余模块一致）：所有可能失败的操作返回
 * `uvcpp_db_status`，失败原因另有一份人读的文本，从 `uvcpp_db_client::last_error()`
 * 取。两个理由：
 *
 *  1. 失败在数据库这条路上是**常态**（唯一键冲突、连接被中间件掐断、SQL 写错），
 *     不是异常情形。用返回码，调用方在写代码时就得处理它。
 *  2. `catch` 跨动态库边界在 Windows 上是另一套 ABI 规则，预编译包里必然踩。
 *
 * 返回码只区分「哪一类失败」，不试图枚举数据库自己的错误号 —— 那属于各家驱动
 * 的私有词汇表。要 SQLSTATE / errno 时看 `last_error()` 里的原文。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_STATUS_H
#define SRC_DB_UVCPP_DB_STATUS_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_DB_ENABLE

#include <cstddef>

namespace uvcpp {

enum class uvcpp_db_status : int {
  /// 成功。
  OK = 0,

  /// 连接串解析不了（缺 scheme、缺 '@'、端口不是数字……）。
  BAD_URL,

  /// 这个 scheme 的后端**没有编进当前这个库**。与 BAD_URL 分开，是因为
  /// 前者是调用方写错了，后者是发行方式的选择 —— 预编译包少了哪个后端，
  /// 使用者应该在这里一眼看见，而不是拿到一句「打不开连接」。
  NO_DRIVER,

  /// 打开连接失败：主机/端口不可达、认证失败、库不存在、超时。
  OPEN_FAILED,

  /// 连接没打开就调了需要连接的操作，或执行途中连接掉了且重连也没成功。
  NOT_CONNECTED,

  /// 语句准备失败：SQL 语法错、表/列不存在。（PostgreSQL 的裸通道也会把
  /// 「没有这个表」报在这里。）
  PREPARE_FAILED,

  /// 语句执行失败：约束冲突、类型不匹配、死锁重试用尽、权限不足。
  EXEC_FAILED,

  /// 参数与占位符对不上（个数、或名字找不到）。
  BIND_FAILED,

  /// 这个后端不支持该操作（例如 SQLite 没有存储过程、没有 `lastval()`）。
  UNSUPPORTED,

  /// 用法错误：空 SQL、空结果集指针、未 open 就 query。
  MISUSE,

  /// 内存不足。
  OUT_OF_MEMORY,
};

/// 返回码的稳定短名（英文、小写下划线），用于日志与测试断言。
/// 稳定意味着：可以写进测试的期望值，改名算破坏性变更。
UVCPP_API const char* uvcpp_db_status_name(uvcpp_db_status status);

/// 是否成功。写成函数而不是宏，是为了让 `if (uvcpp_db_ok(st))` 有类型。
inline bool uvcpp_db_ok(uvcpp_db_status status) {
  return status == uvcpp_db_status::OK;
}

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
#endif  // SRC_DB_UVCPP_DB_STATUS_H
