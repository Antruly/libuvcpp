/**
 * @file src/db/uvcpp_db_runtime_load.h
 * @brief `UVCPP_DB_RUNTIME_LOAD` 用的运行时加载原语。
 * @author zhuweiye
 * @version 1.0.0
 *
 * **私有头**：它是 db 模块内部"客户端库怎么到手"这一层的接缝，不属于公开 API
 * （`package_release.py` 的 `PRIVATE_HEADERS` 与 `install(FILES)` 都把它排除在外），
 * 所以这里出现的东西**不构成任何兼容性承诺**。
 *
 * 只有两个函数，因为只有两件事要跨平台收口：**打开一份动态库**、**从里面取一个
 * 符号**。`dlopen` / `LoadLibraryA` 的差别就只有这两处，收在这里之后
 * `uvcpp_db_mysql.cpp` / `uvcpp_db_pgsql.cpp` 里一行 `#if defined(_WIN32)` 都不用写。
 *
 * 为什么整个文件被 `UVCPP_DB_RUNTIME_LOAD` 包住：不开这个开关时调用点走的是
 * **直接链接**（`nm -u` 里的符号由链接器解析），这两条原语一个都不会被引用 ——
 * 那就干脆不要编进来，免得默认档的 `.so` 上凭空多出 `dlopen`/`dlsym` 两个
 * 未定义符号（在某些精简的静态链接环境里它们并不存在）。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_RUNTIME_LOAD_H
#define SRC_DB_UVCPP_DB_RUNTIME_LOAD_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE && UVCPP_DB_RUNTIME_LOAD

#include <string>

namespace uvcpp {
namespace db_detail {

/// 依次试 `names` 里的候选名（数组以 `nullptr` 结尾），返回**第一个**打开的句柄。
///
/// 三条约定：
///
///   * `env_var` 非空且环境里真的有这个变量时，**只**试它那一个值，不再试候选名 ——
///     显式指定就该是显式的，混进候选名会让"我明明指了路径却还是用了系统那份"
///     变成一种可能。它同时是**确定性地复现"这台机器没装客户端库"**的旋钮
///     （指到一个不存在的路径即可），所以测试不依赖机器上装没装东西。
///   * 全部失败返回 `nullptr`，并把**试过哪些名字、各自为什么不行**写进 `tried`
///     （`tried` 可以为 `nullptr`）。写成"人读得懂的一句"而不是错误码：这句话
///     最终会出现在 `uvcpp_db_client::last_error()` 里，直接给使用者看。
///   * 成功时把**实际打开的那一个**名字写进 `loaded`（可以为 `nullptr`）。留着它是
///     为了另一条错误路径：库打开了、但里面少一个符号时，那句"缺哪个符号"如果不
///     带上是**哪一个文件**，就没法定位（同一个符号在 MySQL 8 与 MariaDB 上的
///     有无是不一样的）。
///   * 打开失败**不是**程序错误：库可能压根没装，那是正常情形，所以不抛、不 abort。
///
/// 句柄**永不释放**：没有对应的 `close` 函数，进程退出时由内核回收。理由是不存在
/// "安全卸载点"—— 只要还有一条活连接（或者有一个 `MYSQL*` 被调用方拿着），
/// `dlclose` 就会把已经解析好的函数指针连同实现一起拿走，之后任何一次调用都是
/// 跳到已卸载的代码上。留着的代价是每进程一张页表项，换来的是"卸载"这个动作
/// 从词汇表里消失。
void* uvcpp_db_open_client_library(const char* const* names,
                                   const char* env_var, std::string* tried,
                                   std::string* loaded);

/// 从已打开的句柄里取一个符号；没有返回 `nullptr`。
///
/// 加载器**逐个符号**取并检查，而不是"取到几个算几个"：这批符号里有些是跨版本
/// 会漂移的（`mysql_stmt_*` 一组在 MySQL 与 MariaDB 之间有过增删），而少一个
/// 符号的后果是**调用点跳到空指针**。所以缺哪个就报哪个，见各自的 `_load_error()`。
void* uvcpp_db_lookup_symbol(void* handle, const char* symbol);

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE && UVCPP_DB_RUNTIME_LOAD
#endif  // SRC_DB_UVCPP_DB_RUNTIME_LOAD_H
