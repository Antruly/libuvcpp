/**
 * @file src/db/uvcpp_db_mongodb_api.h
 * @brief MongoDB（libmongoc + libbson）函数表：`UVCPP_DB_RUNTIME_LOAD` 的那一半。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 与 `uvcpp_db_pgsql_api.h` / `uvcpp_db_mysql_api.h` / `uvcpp_db_odbc_api.h` 是
 * 同一个形状 —— 成员名就是符号名、清单只写一遍、类型从真的头里 `decltype` 反推。
 * 为什么这么写、手抄三遍会怎么坏，`uvcpp_db_mysql_api.h` 那份注释说得更细。
 *
 * **这一份和另外三份有一处结构性的不同**：MySQL / PostgreSQL / ODBC 的符号全在
 * **一份**客户端库里，这里的符号横跨 `libmongoc-1.0` 与 `libbson-1.0` 两份。
 * 所以表拆成两张（`bson_api` / `mongoc_api`），各自由对应的句柄里取；
 * `UVCPP_DB_RUNTIME_LOAD=ON` 时**两份库都要打开**，缺一份就是 `NO_DRIVER`，
 * 理由见 `uvcpp_db_mongodb.cpp` 里加载那一段。
 *
 * 这个头**不公开**：它拉进 `<mongoc/mongoc.h>` 与 `<bson/bson.h>`，公开面
 * （`<db/uvcpp_mongo.h>`）里一个 mongoc 类型都不出现 —— 使用者不需要装这套头。
 * `install(FILES)` 与 `package_release.py` 的 `PRIVATE_HEADERS` 两处都排除它。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_MONGODB_API_H
#define SRC_DB_UVCPP_DB_MONGODB_API_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE && UVCPP_DB_MONGODB_ENABLE

// `mongoc/mongoc.h` 自己会把 `mongoc.h` 那一层拉进来；bson 的头它是按
// `<bson/bson.h>` 的路径 include 的，所以两个 include 目录都要在编译行上
// （`CMakeLists.txt` 的 MongoDB 发现那一段把两份都追加进
// `UVCPP_DB_INCLUDE_DIRS`）。
#include <bson/bson.h>
#include <mongoc/mongoc.h>

#include <string>

namespace uvcpp {
namespace db_detail {

/// libbson 这边要用的**全部**符号（4 个）。
///
/// 只取「JSON ↔ BSON」与释放走得到的那几个：构造文档用
/// `bson_new_from_json`（接受 Extended JSON，`$oid` / `$date` 这些都能进），
/// 输出用 `bson_as_canonical_extended_json`，中间**不暴露任何 BSON 类型**给
/// 调用方。所以不需要 `bson_new` / `bson_append_*` / `bson_iter_*` 这一整套。
#define UVCPP_DB_BSON_SYMBOLS(X)          \
  X(bson_as_canonical_extended_json)      \
  X(bson_destroy)                         \
  X(bson_free)                            \
  X(bson_new_from_json)

/// libmongoc 这边要用的**全部**符号（15 个）。
///
/// 这一组在 1.5 之后没有删改过（只新增）。仍然逐个 `dlsym` 并报缺哪个：
/// 少一个符号的后果是**调用点跳到空指针**，而不是链接期的一句话。
///
/// 没有 `mongoc_cleanup`：进程退出由内核回收，与另外三份"句柄永不 dlclose"
/// 是同一条理由（见 `uvcpp_db_runtime_load.h`）。
#define UVCPP_DB_MONGOC_SYMBOLS(X)        \
  X(mongoc_client_command_simple)         \
  X(mongoc_client_destroy)                \
  X(mongoc_client_get_collection)         \
  X(mongoc_client_new_from_uri)           \
  X(mongoc_collection_delete_one)         \
  X(mongoc_collection_destroy)            \
  X(mongoc_collection_find_with_opts)     \
  X(mongoc_collection_insert_one)         \
  X(mongoc_collection_update_one)         \
  X(mongoc_cursor_destroy)                \
  X(mongoc_cursor_error)                  \
  X(mongoc_cursor_next)                   \
  X(mongoc_init)                          \
  X(mongoc_uri_destroy)                   \
  X(mongoc_uri_new_with_error)

/// libbson 的函数表。成员名 = 符号名。
struct bson_api {
#define UVCPP_DB_BSON_DECLARE(name) decltype(&::name) name;
  UVCPP_DB_BSON_SYMBOLS(UVCPP_DB_BSON_DECLARE)
#undef UVCPP_DB_BSON_DECLARE
};

/// libmongoc 的函数表。成员名 = 符号名。
struct mongoc_api {
#define UVCPP_DB_MONGOC_DECLARE(name) decltype(&::name) name;
  UVCPP_DB_MONGOC_SYMBOLS(UVCPP_DB_MONGOC_DECLARE)
#undef UVCPP_DB_MONGOC_DECLARE
};

#if UVCPP_DB_RUNTIME_LOAD

/// 取两张函数表。永不返回空；库没打开时表里全是空指针，调用前先看
/// `uvcpp_db_mongodb_load_error()` 是不是空串。
const bson_api& uvcpp_db_bson_api();
const mongoc_api& uvcpp_db_mongoc_api();

/// 加载失败的原因（人读的一句话）；成功是空串。进程级、只算一次。
const std::string& uvcpp_db_mongodb_load_error();

#else  // 直接链接

const bson_api& uvcpp_db_bson_api();
const mongoc_api& uvcpp_db_mongoc_api();

/// 直接链接形态下永远是空串。
inline const std::string& uvcpp_db_mongodb_load_error() {
  static const std::string kNoError;
  return kNoError;
}

#endif  // UVCPP_DB_RUNTIME_LOAD

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE && UVCPP_DB_MONGODB_ENABLE
#endif  // SRC_DB_UVCPP_DB_MONGODB_API_H
