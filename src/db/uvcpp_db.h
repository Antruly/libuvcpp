/**
 * @file src/db/uvcpp_db.h
 * @brief 数据库模块的总入口：一个连接 = 一个 `uvcpp_db_client`。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * #include <db/uvcpp_db.h>            // 需要 -DUVCPP_ENABLE_DB=ON 的构建
 *
 * uvcpp::uvcpp_db_client db;
 * if (!uvcpp::uvcpp_db_ok(db.open("sqlite:///tmp/app.db"))) {
 *   std::fprintf(stderr, "%s\n", db.last_error().c_str());
 * }
 *
 * uvcpp::uvcpp_db_table t;
 * db.query("SELECT id, name FROM users WHERE age > ?", {18}, &t);
 * for (const uvcpp::uvcpp_db_row& row : t.rows()) {
 *   std::printf("%s\n", row["name"].to_text().c_str());
 * }
 * ```
 *
 * ## 同步，且**故意**是同步的
 *
 * 数据库客户端库（libmysqlclient / libpq / sqlite3）全是阻塞 API，没有异步
 * 版本。把它们塞进事件循环只有两条路：丢线程池（`uv_queue_work`），或者自己
 * 实现协议。这里是第一条路的**底座** —— 本类就是一层薄薄的同步访问层，
 * 异步封装建在它上面（`uvcpp_db_async` 的回调 / future，见
 * `<db/uvcpp_db_async.h>`），而不是把它包在互斥锁里再从
 * 别的线程碰事件循环。理由：库内其它模块的线程规矩是「谁创建的谁用」，
 * 而一个跨线程共享的连接对象会把这条规矩捅破。
 *
 * ## 一个 client 一个连接
 *
 * `uvcpp_db_client` 内部有一把递归锁：**同一个 client 可以被多个线程调用，
 * 但同一时刻只有一个在真的用连接**。要并发就开多个 client —— 或者直接用
 * `uvcpp_db_pool`（`<db/uvcpp_db_pool.h>`）把「开多个、复用、借还」一起管掉。
 * 不要指望这一把锁给你并行度。
 *
 * ## 不自动重连
 *
 * 旧实现（`DatabaseManager`）每次取连接前 `mysql_ping`，断了就**静默重连**。
 * 那是错的：事务进行到一半连接掉线时，静默重连会**丢掉整个事务**，而调用方
 * 看到的是「这条 UPDATE 成功了」。这里把重连显式化：`is_open()` 说的是
 * 「手上有没有连接」，连接是不是还活着用 `ping()` 问，断了用 `reconnect()`
 * 接 —— 什么时候接由调用方决定，因为它才知道自己在不在事务里。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_H
#define SRC_DB_UVCPP_DB_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_DB_ENABLE

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <db/uvcpp_db_params.h>
#include <db/uvcpp_db_status.h>
#include <db/uvcpp_db_table.h>
#include <db/uvcpp_db_value.h>

namespace uvcpp {

class UVCPP_API uvcpp_db_client {
 public:
  /// 日志回调：每次 query/execute 之后调一次，内容是 SQL、参数个数、行数/影响
  /// 行数、耗时。开着它跑基准会失真，但它比事后猜「为什么慢」便宜。
  ///
  /// **线程与锁（写这一条是因为踩过）**：它在**发起这次查询的那条线程**上被调
  /// （不是循环线程 —— 异步门面下那就是池线程），而且调用时 client 的互斥量
  /// **还在手上**：日志是 `query()` 体内记的，锁罩着整个函数体。
  ///
  /// 于是钩子里有两件事不能做：
  ///
  ///   1. **别阻塞。** 钩子在等 = 这个 client 的锁在等 —— 同一个 client 上别的
  ///      线程的查询会**全部**跟着停住。异步门面下这足以凑出死锁（本仓
  ///      `db_sqlite_async_func.cpp` 第 3 条用例上一版就是这么挂死的）。
  ///   2. **别在钩子里改用别的线程再用这个 client 并等它回来** —— 同上，锁在
  ///      你手上。同一条线程上递归再用是允许的（那是递归锁）。
  using log_callback = std::function<void(const std::string&)>;

  uvcpp_db_client();
  ~uvcpp_db_client();

  uvcpp_db_client(const uvcpp_db_client&) = delete;
  uvcpp_db_client& operator=(const uvcpp_db_client&) = delete;

  // ---- 连接 ----

  /// 打开连接。连接串形状见 `doc/db-guide.md`：
  /// `sqlite:///path.db`、`mysql://user:pass@host:3306/db`、
  /// `postgres://user:pass@host:5432/db`。
  /// 已经打开时先关掉旧的再开新的（失败时**不会**退回旧连接）。
  uvcpp_db_status open(const std::string& url);
  void close();
  /// 手上有没有连接（**不代表**它还能用，那个要 `ping()`）。
  bool is_open() const;
  /// 用最后一次 `open()` 的连接串重连。事务里调它 = 放弃那个事务。
  uvcpp_db_status reconnect();
  /// 连接是否还活着（一次极轻的服务端往返；SQLite 是文件句柄检查）。
  uvcpp_db_status ping();

  const std::string& url() const;
  /// "sqlite" / "mysql" / "postgres"；没打开时是空串。
  const char* driver_name() const;
  /// 最近一次失败的人读原因。成功不清理它 —— 失败现场比成功更值得留着。
  const std::string& last_error() const;

  // ---- 读写 ----

  /// 查询。`out` 不能为空。
  uvcpp_db_status query(const std::string& sql, uvcpp_db_table* out);
  uvcpp_db_status query(const std::string& sql, const uvcpp_db_params& params,
                        uvcpp_db_table* out);

  /// 增删改。`affected` 可为空。SQL 里**不要**带尾分号（各家处理不一），
  /// 本模块不替你去掉。
  uvcpp_db_status execute(const std::string& sql, int64_t* affected = nullptr);
  uvcpp_db_status execute(const std::string& sql, const uvcpp_db_params& params,
                          int64_t* affected = nullptr);

  /// `execute` + `last_insert_id`，给「插入后要新主键」这条最常见的路。
  /// `new_id` 可为空。后端报不出自增时 `new_id` 写 NULL 值，但返回码仍是 OK
  /// —— 「插进去了」和「拿不到 id」是两件事。
  uvcpp_db_status insert(const std::string& sql, const uvcpp_db_params& params,
                         uvcpp_db_value* new_id);

  /// 最近一次 INSERT 的自增值；拿不到时是 NULL 值。
  uvcpp_db_value last_insert_id();
  uvcpp_db_status last_insert_id(uvcpp_db_value* out);

  // ---- 事务 ----

  /// 开事务。SQLite 上就是 `BEGIN`，MySQL/PG 上是 `START TRANSACTION` / `BEGIN`。
  /// 不嵌套：重复 `begin()` 由后端报错（不判红是因为 MySQL 会静默提交前一个）。
  uvcpp_db_status begin();
  uvcpp_db_status commit();
  uvcpp_db_status rollback();

  // ---- 元信息 ----

  uvcpp_db_status table_names(std::vector<std::string>* out);
  /// 列结构。三家的列集不同（MySQL 用 `SHOW COLUMNS` 的形状，
  /// PG/SQLite 用 information_schema / pragma 的形状），共同列是
  /// name / type / nullable / key / default。
  uvcpp_db_status table_schema(const std::string& table, uvcpp_db_table* out);

  // ---- 转义（拼 SQL 时才需要；能参数化就参数化）----

  /// 字面量内容转义，**不含**两侧引号：`escape("a'b")` → `a''b`。
  std::string escape(const std::string& text);
  /// 标识符转义，**含**两侧引号：MySQL 用反引号，PG/SQLite 用双引号。
  /// 它防的是「名字里有特殊字符」，不是「名字是恶意的」—— 表名来自外部输入时
  /// 仍然要对着白名单查一遍。
  std::string escape_identifier(const std::string& identifier);

  // ---- 配置 ----

  /// 语句/连接超时（毫秒）。必须在 `open()` **之前**设置才影响连接阶段；
  /// 打开之后设置会尽量应用到当前连接（SQLite 是 busy timeout，
  /// MySQL 是读写超时，PG 是 `statement_timeout`）。
  void set_timeout_ms(int timeout_ms);
  int timeout_ms() const;
  /// 装/卸日志回调（`nullptr` 或空 `function` 即关掉）。语义、线程与锁的约束
  /// 见上面 `log_callback` 的注释 —— 一句话：**钩子里别阻塞**。
  void set_log(log_callback callback);

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

/// `open` + 造对象的糖。失败时返回空指针，返回码写进 `status`、人读的原因写进
/// `error`（两者都可为空）。
///
/// 为什么 `error` 是必需的一个出口：失败时这个 client 当场就被销毁了，
/// `last_error()` 跟着没 —— 只给返回码的话，"认证失败"和"库不存在"在调用方
/// 眼里都是同一个 `open_failed`。
UVCPP_API std::unique_ptr<uvcpp_db_client> uvcpp_db_open(const std::string& url,
                                                         uvcpp_db_status* status,
                                                         std::string* error = nullptr);

/// 这个库里编进来了哪些后端，逗号分隔（例："sqlite,mysql,postgres"）。
/// 预编译包缺哪个后端时，这一行是第一个该看的地方。
UVCPP_API std::string uvcpp_db_drivers();

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
#endif  // SRC_DB_UVCPP_DB_H
