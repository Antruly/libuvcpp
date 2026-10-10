/**
 * @file src/db/uvcpp_db_async.h
 * @brief 异步门面：不写 `uvcpp_work`，直接 `async_query`。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * #include <db/uvcpp_db.h>
 * #include <db/uvcpp_db_async.h>
 *
 * uvcpp::uvcpp_db_client db;
 * db.open("sqlite:///tmp/app.db");
 *
 * uvcpp::uvcpp_db_async a(&db);
 * a.query(&loop, "SELECT id, name FROM users WHERE city = ?", {"杭州"},
 *         [](uvcpp::uvcpp_db_status st, const uvcpp::uvcpp_db_table& t) {
 *           if (!uvcpp::uvcpp_db_ok(st)) return;   // 失败时 t 是空表
 *           for (const uvcpp::uvcpp_db_row& row : t.rows()) {
 *             std::printf("%s\n", row["name"].to_text().c_str());
 *           }
 *         });
 * loop.run();   // 回调在**这条循环的线程**上被调
 * ```
 *
 * ## 它替你写掉的那段样板
 *
 * 数据库客户端库（libmysqlclient / libpq / sqlite3）全是阻塞 API，要在事件
 * 循环上用就只剩"丢线程池"这一条路（见 `uvcpp_db.h` 顶上那段）。裸写是：
 * `new uvcpp_work` → `queue_work(loop, 工作回调, 完成后回调)` → 在工作回调里
 * 调 `client->query` → 在完成后回调里搬结果、删 work。本类把这一整套收成一次
 * 调用，并且**替你守住三件容易错的事**：完成后回调一定在循环线程上、
 * 投递失败时回调不会被调、回调里 `delete` 门面之后不会有第二次回调。
 *
 * ## 返回值：投递成功与否看 `int`，**别看回调来没来**
 *
 * 下面每个 `query`/`execute`/`insert`/`ping` 都返回一个 `int`：`0` 是投出去了，
 * **负值（libuv 码）是当场没投出去**（loop 为空、或者这个 client 还没绑）。
 * 投递失败时**回调不会被调**，等回调就是等一个永远不来的东西 —— 判"这一次行不行"
 * 一律看返回值。future 那条路不适用（它没有落空的余地：投不出去也当场兑现一个
 * 失败结果，`get()` 照样返回）。
 *
 * ## 回调在**循环线程**上跑，是 libuv 保证的，不是这里拼的
 *
 * `uv_queue_work` 的 after-work 回调由 libuv 从 `uv__work_done` 里、
 * **在循环线程上**发出（`src/req/uvcpp_work.h` 的 `callback_after_work`）。
 * 所以这里**不需要** `uv_async` 邮箱来把结果搬回循环 —— 完成回调本来就在
 * 那条线程上，直接调你的闭包。
 *
 * ## 用的是**全库唯一**那条线程池，和 `uv_fs_*` / DNS 共用
 *
 * `uv_queue_work` 丢进的是 libuv 的默认线程池，**默认只有 4 条线程**
 * （`UV_THREADPOOL_SIZE` 可改，1..1024）。同一条池子还被 `uv_fs_*`、
 * `uv_getaddrinfo`、`uv_getnameinfo`、`uv_random` 用着，所以**一条 30 秒的
 * 慢查询会把文件读写和 DNS 一起堵住**。这不是本类能解决的，是这套模型的已知
 * 代价：要变就调 `uvcpp_set_threadpool_size()`（必须**在任何投递之前**调，
 * 池子大小是第一次投递时读进缓存的）。
 *
 * 本类**不做背压**（不限制在途条数）。一条连接本来就是串行的，在途多条只会
 * 排队等那把递归锁，不会更快；真正的并发靠连接池（`uvcpp_db_pool`）。
 *
 * ## 一个连接还是一把锁
 *
 * 异步买的是"循环不被卡住"，**不是**"同一时刻能跑多条 SQL"：同一个
 * `uvcpp_db_client` 内部那把递归锁没变，在途的几条会**排队**。要并行就借多条
 * 连接。
 *
 * ## 别和同步调用混着用来「省时间」
 *
 * 在循环线程上调一次同步 `db.query()`，会去等 worker 手里的那把锁 —— 不会
 * 死锁，但**会把循环卡住**，正好抵消掉异步的全部意义。要并发就交给池子。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_ASYNC_H
#define SRC_DB_UVCPP_DB_ASYNC_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_DB_ENABLE

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>

#include <db/uvcpp_db_params.h>
#include <db/uvcpp_db_status.h>
#include <db/uvcpp_db_table.h>
#include <db/uvcpp_db_value.h>

namespace uvcpp {

class uvcpp_db_client;
class uvcpp_loop;

/// 一次异步操作的结果。**只有 future 糖用它** —— 回调式把这几样拆成闭包的
/// 实参（`(status, table)` / `(status, affected)`），读起来更直接。
///
/// `status` 是默认值 `MISUSE` 而不是 `OK`：一个**没被填过**的结果必须是"错"，
/// 否则"忘了填"会伪装成成功。
struct UVCPP_API uvcpp_db_result {
  uvcpp_db_status status = uvcpp_db_status::MISUSE;

  /// 查询结果集。失败时是空表（不是"上次的"）。
  uvcpp_db_table table;

  /// `execute` / `insert` 的影响行数；查询与失败时是 -1。
  int64_t affected = -1;

  /// `insert` 拿到的自增值；拿不到是 NULL 值（那不算失败，见 `uvcpp_db.h`）。
  uvcpp_db_value last_id;

  /// 失败时的人读原因，等价于那一刻 `client->last_error()` 的快照。
  /// 成功时是空串。
  std::string error;

  bool ok() const { return uvcpp_db_ok(status); }
};

/**
 * @brief 一条连接的异步门面。
 *
 * **绑的是连接，不是所有权**：构造/`bind()` 收一个裸指针，门面不删它、也不
 * 替它保活。调用方必须保证 client 活得比**所有在途操作**久 —— 工作回调跑在
 * 池线程上，client 那时没了就是野指针。
 *
 * 同一个 client 可以被不同门面、或不同时刻被不同循环驱动，这是正当用法：
 * 门面自己不持有循环，每条在途操作记的是**投递时**那一条。
 */
class UVCPP_API uvcpp_db_async {
 public:
  /// 完成回调的三种形状。都**只在循环线程上**被调一次。
  ///
  /// 失败时 `table` 是空表、`affected` 是 -1，原因从实参、或
  /// `client->last_error()` 取。
  using table_cb = std::function<void(uvcpp_db_status, const uvcpp_db_table&)>;
  using exec_cb = std::function<void(uvcpp_db_status, int64_t affected)>;
  using ping_cb = std::function<void(uvcpp_db_status)>;

  /// future 糖的返回类型（`std::future<uvcpp_db_result>` 的短名）。
  using result_fut = std::future<uvcpp_db_result>;

  uvcpp_db_async();
  explicit uvcpp_db_async(uvcpp_db_client* client);
  ~uvcpp_db_async();

  uvcpp_db_async(const uvcpp_db_async&) = delete;
  uvcpp_db_async& operator=(const uvcpp_db_async&) = delete;

  // ---- 绑连接 ----

  /// 换一条连接。**别在途时换** —— 在途的那几条记的是投递时的 client，
  /// 不受影响，但"现在绑的是谁"会立刻变。
  void bind(uvcpp_db_client* client);
  uvcpp_db_client* client() const;

  // ---- 回调式 ----

  /// 查询。`cb` 在 `loop` 的线程上被调。
  int query(uvcpp_loop* loop, const std::string& sql, const table_cb& cb);
  int query(uvcpp_loop* loop, const std::string& sql,
            const uvcpp_db_params& params, const table_cb& cb);

  /// 增删改。`affected` 拿不到时是 -1。
  int execute(uvcpp_loop* loop, const std::string& sql,
              const uvcpp_db_params& params, const exec_cb& cb);

  /// `execute` + 自增值。回调收到的是影响行数（自增 id 走 future 糖，或直接
  /// 在回调里同步 `client->last_insert_id()` —— 那一次不碰网络）。
  int insert(uvcpp_loop* loop, const std::string& sql,
             const uvcpp_db_params& params, const exec_cb& cb);

  /// 连接是否还活着。
  int ping(uvcpp_loop* loop, const ping_cb& cb);

  // ---- future 糖（给不在循环上的调用方）----

  /// 这几条**不碰调用方的循环**：门面懒起一条**私有的循环线程**，把活投到
  /// 那条循环上，`get()` 阻塞调用方自己这条线程直到结果到手。
  ///
  /// 为什么不是"就地同步跑一遍"：那是撒谎，它照样卡住调用方所在的循环，也不
  /// 叫异步。为什么不是"每次调用现起一条循环再 run()"：那还是每条都串行，
  /// 而这里连投 N 条时 N 条会**同时**进池子（真要并行还得是池子，见类注释）。
  ///
  /// 那条线程在第一次用到 future 时才起，析构门面时收掉。
  result_fut query(const std::string& sql,
                   const uvcpp_db_params& params = uvcpp_db_params());
  result_fut execute(const std::string& sql,
                     const uvcpp_db_params& params = uvcpp_db_params());
  result_fut insert(const std::string& sql,
                    const uvcpp_db_params& params = uvcpp_db_params());

  // ---- 观测 ----

  /// 在途条数（回调式 + future 糖都算）。它不涨到 0 之前不能删 client。
  size_t in_flight() const;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
#endif  // SRC_DB_UVCPP_DB_ASYNC_H
