/**
 * @file src/db/uvcpp_db_driver.h
 * @brief **私有**头：连接串解析 + 各后端驱动接口 + 工厂。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 不安装（CMake 的 `install(FILES ...)` 里按文件名排除），因为里面按定义就是
 * 实现细节：使用者拿到的是 `uvcpp_db_client`，不是驱动。
 *
 * ## 分层
 *
 * ```
 *   uvcpp_db_client       ← 公开：连接管理、重连、日志、事务、SQL 文本
 *        │
 *   uvcpp_db_driver       ← 这里：一个后端一份实现（sqlite / mysql / pgsql）
 *        │
 *   各家的 C 客户端库
 * ```
 *
 * 驱动**不抛异常**：失败返回 `uvcpp_db_status`，人读的原因写进 `err`。
 * 驱动也不管线程 —— 一个连接同一时刻只能一个线程用，那是 `uvcpp_db_client`
 * 的互斥锁在管。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_DRIVER_H
#define SRC_DB_UVCPP_DB_DRIVER_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <db/uvcpp_db_params.h>
#include <db/uvcpp_db_status.h>
#include <db/uvcpp_db_table.h>
#include <db/uvcpp_db_value.h>

namespace uvcpp {
namespace db_detail {

/// 连接串。形状：
///
///     mysql://user:password@host:3306/dbname?charset=utf8mb4
///     postgres://user:password@host:5432/dbname
///     sqlite:///绝对/路径.db          sqlite://相对/路径.db
///     sqlite::memory:                 （进程内临时库）
///
/// 解析规矩与旧版不同的一处：**userinfo 按最后一个 `@` 切**，不是第一个。
/// 密码里带 `@` 是常见的（`p@ssw0rd`），按第一个切会把密码截断、把剩下的
/// 当成主机名 —— 症状是「连接被拒」，而配置看起来完全正常。
struct uvcpp_db_url {
  std::string scheme;    // 规范化后的 "sqlite" / "mysql" / "postgres"
  std::string user;
  std::string password;
  std::string host;
  std::string database;  // mysql/pg 的库名；sqlite 里是文件路径
  unsigned int port = 0; // 0 = 用该后端默认端口
  std::string options;   // '?' 之后的原文，原样交给后端

  static uvcpp_db_status parse(const std::string& text, uvcpp_db_url* out,
                               std::string* err);
};

/// 一个后端。生命周期由 `uvcpp_db_client` 独占持有。
class uvcpp_db_driver {
 public:
  virtual ~uvcpp_db_driver() = default;

  /// 驱动的稳定短名："sqlite" / "mysql" / "postgres"。
  virtual const char* name() const = 0;

  virtual uvcpp_db_status open(const uvcpp_db_url& url, std::string* err) = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;

  /// 连接还活着吗。一次极轻的服务端往返（不是只看本地句柄非空 ——
  /// 连接被防火墙/NAT 掐掉时本地句柄照样非空）。
  virtual uvcpp_db_status ping(std::string* err) = 0;

  /// 查。`out` 不能为空（空 → MISUSE）。无参数时后端可以走各自的裸通道，
  /// 这条通道要能被 DDL 复用（MySQL 的 `mysql_stmt_prepare` 不支持 ALTER/
  /// CREATE，报 1295）。
  virtual uvcpp_db_status query(const std::string& sql,
                                const std::vector<uvcpp_db_value>& params,
                                uvcpp_db_table* out, std::string* err) = 0;

  /// 增删改。`affected` 可为空；后端报不出影响行数时写 -1。
  virtual uvcpp_db_status execute(const std::string& sql,
                                  const std::vector<uvcpp_db_value>& params,
                                  int64_t* affected, std::string* err) = 0;

  /// 最近一次 INSERT 生成的自增值。后端不支持（SQLite 无表时、PG 无序列时）
  /// 返回 NULL 值 + `unsupported`。
  virtual uvcpp_db_status last_insert_id(uvcpp_db_value* out,
                                         std::string* err) = 0;

  virtual uvcpp_db_status begin(std::string* err) = 0;
  virtual uvcpp_db_status commit(std::string* err) = 0;
  virtual uvcpp_db_status rollback(std::string* err) = 0;

  /// 字面量转义（字符串内容，不含两侧引号）。
  virtual std::string escape(const std::string& text) = 0;
  /// 标识符转义（表名/列名，含两侧引号）。
  virtual std::string quote_identifier(const std::string& ident) = 0;

  virtual uvcpp_db_status table_names(std::vector<std::string>* out,
                                      std::string* err) = 0;
  virtual uvcpp_db_status table_schema(const std::string& table,
                                       uvcpp_db_table* out, std::string* err) = 0;

  /// 语句/锁等待超时（毫秒）。后端有自己的下限，实现里会说明。
  virtual void set_timeout_ms(int /*ms*/) {}
};

/// 各后端的构造函数。定义在各自的 .cpp 里，且那个文件整体被
/// `#if UVCPP_DB_<X>_ENABLE` 包着 —— 关掉的后端这里就没有定义，
/// 而下面的工厂也不会引用它（所以不会链接错）。
std::unique_ptr<uvcpp_db_driver> uvcpp_db_make_sqlite();
std::unique_ptr<uvcpp_db_driver> uvcpp_db_make_mysql();
std::unique_ptr<uvcpp_db_driver> uvcpp_db_make_pgsql();

/// 按 scheme 造驱动。没编进来的后端返回 `nullptr`（**不是**抛异常、
/// 也不是造一个打不开的壳）—— 调用方据此报 `NO_DRIVER`。
std::unique_ptr<uvcpp_db_driver> uvcpp_db_make_driver(const std::string& scheme);

/// 这个 scheme 的后端编进本库了吗。
bool uvcpp_db_driver_available(const std::string& scheme);

/// 这个 scheme 是不是**本模块支持过的**后端，不论这一份构建有没有编进来。
///
/// 与上一个函数的差别就是 `NO_DRIVER` 与 `BAD_URL` 的差别，而这个差别对调用方
/// 是要紧的：`oracle://...` 是**你写错了**（我们从来没有过这个后端），
/// `mysql://...` 打不开是**这份预编译包没带 MySQL**（换个包，或者别用这个
/// scheme）。两个都报 `BAD_URL` 的话，第二种情况会被读成"连接串写错了"，
/// 排障从这里就开始跑偏。
///
/// 传进来的必须是 `uvcpp_db_url::parse()` 规范化之后的 scheme。
bool uvcpp_db_driver_known(const std::string& scheme);

/// 编进来的后端名，逗号分隔、按 sqlite,mysql,postgres 顺序。空串表示一个都没有。
std::string uvcpp_db_available_drivers();

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
#endif  // SRC_DB_UVCPP_DB_DRIVER_H
