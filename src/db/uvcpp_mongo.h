/**
 * @file src/db/uvcpp_mongo.h
 * @brief MongoDB 客户端：JSON 进、JSON 出，客户端库按需加载。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * #include <db/uvcpp_mongo.h>   // 需要 -DUVCPP_ENABLE_DB=ON 的构建
 *
 * uvcpp::uvcpp_mongo_client m;
 * if (!uvcpp::uvcpp_db_ok(m.open("mongodb://127.0.0.1:27017"))) {
 *   std::fprintf(stderr, "%s\n", m.last_error().c_str());
 * }
 *
 * m.insert_one("app", "users", "{\"name\": \"ada\", \"age\": 36}");
 *
 * std::vector<std::string> docs;
 * m.find("app", "users", "{\"age\": {\"$gte\": 18}}", "{\"limit\": 10}",
 *        &docs);
 * ```
 *
 * ## 这套接口的形状：JSON 进、JSON 出
 *
 * 公开面里**一个 BSON 类型都不出现**。文档用 **Extended JSON** 的文本传进来
 * （`bson_new_from_json`），结果用 **canonical Extended JSON** 的文本交回去
 * （`bson_as_canonical_extended_json`）。两个理由：
 *
 *  1. 使用者不必把 `<bson/bson.h>` 装进自己的构建 —— 那个头正是本模块
 *     **不随包分发**的那一份（见下）。
 *  2. 本类不重复发明一套 BSON 文档构造器。`bson_new_from_json` 接受的就是
 *     `mongosh` / `mongoexport` 里那套写法（`$oid`、`$date`、`$numberLong`…），
 *     要手写文档时把那段 JSON 贴进来就行。
 *
 * ## 客户端库**不随包分发**，缺库不报进程级错误
 *
 * 这是本模块与 SQLite 那条路最大的不同，也是本仓驱动策略里单独的一档：
 *
 *  * **编译期只要头**（`mongoc.h` + `bson.h`）。头在不在决定
 *    `UVCPP_DB_MONGODB_ENABLE`；找不到时后端不编进来，`backend_available()` 是
 *    `false`，但**本类仍然存在** —— `open()` 返回 `NO_DRIVER`，`last_error()`
 *    说明这份构建没编 MongoDB 后端。进程、HTTP 服务一切照常。
 *  * **链接期什么都不链**（`UVCPP_DB_RUNTIME_LOAD=ON`，发布包就是这一档）：
 *    `libmongoc-1.0` 与 `libbson-1.0` 在第一次 `open()` 时才 `dlopen` /
 *    `LoadLibrary`。两个都要，缺一个就是 `NO_DRIVER`，错误串列出试过的库名。
 *  * **发布包里没有这两个 `.so` / `.dll`。** 要用 MongoDB 的机器自己装
 *    `mongo-c-driver`（Debian/Ubuntu 上是 `libmongoc-1.0-0` + `libbson-1.0-0`，
 *    或者发行版给的 `libmongoc-1.0-0`），或者用环境变量
 *    `UVCPP_DB_MONGOC_LIBRARY` / `UVCPP_DB_BSON_LIBRARY` 指到完整路径。
 *    **政策原文**见 [`doc/db-driver-policy.md`](../../doc/db-driver-policy.md)
 *    与 [`doc/mongodb-guide.md`](../../doc/mongodb-guide.md)。
 *
 * 于是「这台机器没装 mongoc」与「这个库没编 MongoDB 后端」是**两件事**，
 * 错误串把它们分开说 —— 前者装个包就好，后者要换一份构建。
 *
 * ## 同步为核心，异步是同一把锁上的投递
 *
 * `insert_one` / `find` / … 是**同步**的：libmongoc 是阻塞 API，本类不去假装
 * 它不阻塞。要在事件循环上用，走 `*_async` 那几个：它们用 `uv_queue_work`
 * 把同样的调用丢进 libuv 线程池，**完成回调在调用方那条循环的线程上触发**。
 *
 * **一个对象同一时刻只跑一条操作**（内部一把递归锁）。这是有意的：一条
 * `mongoc_client_t` 内部有自己的连接池，从外面再叠一层"同一连接并发多条"只会
 * 把排队藏起来。要并行就开多个 `uvcpp_mongo_client`。
 *
 * ## 生命周期
 *
 *  * 同步调用：调用方保证对象活着。
 *  * 异步调用：完成回调**一定**在那条循环的线程上被调一次；在回调里析构本对象
 *    是合法的（在途的活自己拿住了内部状态，析构不会把它的脚砍掉）。
 *  * 投递失败（`uvcpp_loop*` 为空）返回负的 libuv 码，**回调不会被调** ——
 *    "这一次行不行"看返回值，别等一个不会来的回调。这与
 *    `<db/uvcpp_db_async.h>` 是同一条约定。
 */

#pragma once
#ifndef SRC_DB_UVCPP_MONGO_H
#define SRC_DB_UVCPP_MONGO_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_DB_ENABLE

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <db/uvcpp_db_status.h>

namespace uvcpp {

class uvcpp_loop;

class UVCPP_API uvcpp_mongo_client {
 public:
  uvcpp_mongo_client();
  ~uvcpp_mongo_client();

  uvcpp_mongo_client(const uvcpp_mongo_client&) = delete;
  uvcpp_mongo_client& operator=(const uvcpp_mongo_client&) = delete;

  // ---- 连接 ----

  /// 打开连接。`uri` 就是 libmongoc 认的那套：
  /// `mongodb://[user:pass@]host[:port][/db][?options]` 或 `mongodb+srv://…`。
  ///
  /// **它会真的连一次**（发一个 `ping`）：回来时 `OK` 表示服务器确实应答了，
  /// 不是"连接串语法没问题"。这一步的等待上限是 URI 里的
  /// `serverSelectionTimeoutMS`（libmongoc 默认 30000 毫秒）——
  /// 本机/局域网请写小一点，例如
  /// `mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=2000`。
  ///
  /// 返回值：
  ///  * `NO_DRIVER` —— 这份构建没编 MongoDB 后端，或运行时打不开
  ///    `libmongoc-1.0` / `libbson-1.0` 中的任何一个（`backend_available()`
  ///    能区分前一种）；
  ///  * `BAD_URL` —— 连接串解析不了，`last_error()` 是 libbson 的原话；
  ///  * `OPEN_FAILED` —— 连不上、认证失败、服务器选中不了。
  ///
  /// 已经打开时先关掉旧的再开新的（失败时**不会**退回旧连接）—— 与
  /// `uvcpp_db_client::open()` 同一条。
  uvcpp_db_status open(const std::string& uri);

  /// 关掉连接。在飞的异步操作会**等它跑完**（内部一把锁），不会半路被砍。
  void close();

  /// 手上有没有一条打开的连接（**不代表**它还能用，那个要 `ping()`）。
  bool is_open() const;

  /// 一次 `{ping: 1}` 往返。`OK` = 服务器应答了。
  uvcpp_db_status ping();

  /// 最近一次失败的人读原因。成功不清理它 —— 失败现场比成功更值得留着。
  const std::string& last_error() const;

  /// 最后一次 `open()` 的连接串（没打开时是空串）。
  const std::string& uri() const;

  /// 这份构建里有没有可用的 MongoDB 后端（编译期的事实）。
  ///
  /// `true` **不**意味着客户端库装好了 —— 那是运行时的事，第一次 `open()` 才
  /// 知道。用 `false` 区分「换一份构建」与「装一个包」，这就是它与
  /// `NO_DRIVER` 的 `last_error()` 一起给出的信息。
  static bool backend_available();

  // ---- 写 ----

  /// 插入一条文档。`document_json` 是 Extended JSON。
  uvcpp_db_status insert_one(const std::string& db,
                             const std::string& collection,
                             const std::string& document_json);

  /// 更新第一条匹配的文档。`update_json` 是用 `$set` / `$inc` 这类更新操作符
  /// 写的文档（与驱动一致：这里**不是**整条替换，替换用 `command()` 发
  /// `replaceOne`）。`upsert` 为真时没有匹配就插入。
  uvcpp_db_status update_one(const std::string& db,
                             const std::string& collection,
                             const std::string& filter_json,
                             const std::string& update_json, bool upsert);

  /// 删除第一条匹配的文档。
  uvcpp_db_status delete_one(const std::string& db,
                             const std::string& collection,
                             const std::string& filter_json);

  // ---- 读 ----

  /// 查询。`out` 不能为空；每次调用**先清空**它。
  ///
  /// `filter_json` 为空串时等价于 `{}`。`options_json` 是 `find` 的选项文档，
  /// 原样交给驱动 —— 常用的是 `{"limit": 10, "sort": {"age": -1},
  /// "projection": {"name": 1}}`；空串等价于不给选项。
  ///
  /// 每一条结果是一个 canonical Extended JSON 字符串。结果集**一次性全部取回**
  /// 到 `out`（本方法不提供游标/流式接口，见文件尾的"明确不做"）。
  uvcpp_db_status find(const std::string& db, const std::string& collection,
                       const std::string& filter_json,
                       const std::string& options_json,
                       std::vector<std::string>* out);

  /// 发一条数据库命令（`db` 是这个命令的执行库，通常写 `"admin"` 或业务库名）。
  /// `out_json` 为真时写回命令应答的 canonical Extended JSON。
  uvcpp_db_status command(const std::string& db,
                          const std::string& command_json,
                          std::string* out_json);

  // ---- 异步（回调在 `loop` 的线程上）----
  //
  // 每个方法返回 `0`（已投递）或负的 libuv 错误码（当场没投出去，**回调不会
  // 被调**）。回调的第一个参数是同步版的那个返回码。
  //
  // 回调里可以析构本对象；但**不要**在回调里对同一个对象发同步调用 ——
  // 那会在池线程之外、循环线程上等同一把锁，而锁的持有者正是这条回调的前
  // 一个任务。要串联就再投一次异步。

  using simple_cb =
      std::function<void(uvcpp_db_status, const std::string&)>;
  using find_cb = std::function<void(uvcpp_db_status,
                                     const std::vector<std::string>&,
                                     const std::string&)>;

  /// 失败时第二个参数是 `last_error()` 的快照；成功时是空串。
  int insert_one_async(uvcpp_loop* loop, const std::string& db,
                       const std::string& collection,
                       const std::string& document_json,
                       simple_cb cb = nullptr);

  int update_one_async(uvcpp_loop* loop, const std::string& db,
                       const std::string& collection,
                       const std::string& filter_json,
                       const std::string& update_json, bool upsert,
                       simple_cb cb = nullptr);

  int delete_one_async(uvcpp_loop* loop, const std::string& db,
                       const std::string& collection,
                       const std::string& filter_json,
                       simple_cb cb = nullptr);

  /// 失败时第三个参数是 `last_error()` 的快照；成功时是空串。
  int find_async(uvcpp_loop* loop, const std::string& db,
                 const std::string& collection, const std::string& filter_json,
                 const std::string& options_json, find_cb cb = nullptr);

  int command_async(uvcpp_loop* loop, const std::string& db,
                    const std::string& command_json,
                    simple_cb cb = nullptr);

  int ping_async(uvcpp_loop* loop, simple_cb cb = nullptr);

 private:
  struct impl;
  std::shared_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
#endif  // SRC_DB_UVCPP_MONGO_H
