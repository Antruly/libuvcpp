/**
 * @file src/db/uvcpp_db_pool.h
 * @brief 连接池：把「一次 open 2 毫秒」摊掉，并把并发从 1 抬到 N。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * #include <db/uvcpp_db_pool.h>
 *
 * uvcpp::uvcpp_db_pool pool;
 * if (!uvcpp::uvcpp_db_ok(pool.init("mysql://root@127.0.0.1:3306/app", 2, 8))) {
 *   std::fprintf(stderr, "%s\n", pool.last_error().c_str());
 *   return 1;
 * }
 *
 * // ① 代借代还：一次调用借一条、用完立刻还（单条语句用这个）
 * uvcpp::uvcpp_db_table t;
 * pool.query("SELECT id, name FROM users WHERE age > ?", {18}, &t);
 *
 * // ② 借还：借到还之间这条连接**归你独占**，事务只能这么写
 * uvcpp::uvcpp_db_client* c = pool.acquire();
 * if (c) {
 *   c->begin();
 *   c->execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", {100, 1});
 *   c->execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", {100, 2});
 *   c->commit();
 *   pool.release(c);          // 一定要还 —— 不还就是拿池子的一条额度换一次泄漏
 * }
 * ```
 *
 * ## 它买的是什么（都有数）
 *
 * `bench/bench_db.cpp` 第 1 节量到 `open`+`close` 一次 **1.9~2.4 ms**；第 10 节量到
 * 4 条线程共用一个 `uvcpp_db_client` 要 **183 ms**、各开一条只要 **65 ms**
 * （MySQL）—— 差别就是 `uvcpp_db_client` 里那把递归锁。池子把这两件事一起解决：
 * 连接**复用**（不再每次付 open 的钱）、**多条**连接（锁不再互相等）。
 *
 * 反过来说，**单线程串行跑**的场景池子一分钱都省不下来，还会多一层借还 ——
 * 那就别用，直接一个 `uvcpp_db_client`（`uvcpp_db_async` 是它的异步门面）。
 * 池子是给「多线程」或「循环线程不能被 open 卡住」这两件事准备的。
 *
 * ## 三条禁令
 *
 * 1. **`acquire()` 会阻塞，所以绝不能在事件循环线程上调它。** 它要么等到一条空闲
 *    连接，要么等到超时（默认 5 s）。循环线程上等 = 把循环卡住，正是异步要躲开的
 *    事。要在循环上取连接，用下面的异步接口（借还在池线程里做），或者用
 *    `uvcpp_db_async` 配一个你独占的 client。
 * 2. **在事件循环线程上调代借代还的同步方法（`pool.query` 等）同样是阻塞的。**
 *    它们是给工作线程 / 不在循环上的调用方用的。
 * 3. **别在**借出期间**去投异步池查询。** 异步池查询自己要借一条连接：`max=1` 时
 *    那唯一一条在你手里，池线程会一直等到超时。它**会**醒（超时是硬的），但你会
 *    拿到 `NO_CONNECTION` 而不是结果。见下面「池线程会被谁等死」。
 *
 * ## 池线程会被谁等死
 *
 * `uvcpp_db_async` 与池子的异步接口都跑在 **libuv 那条唯一的默认线程池**上
 * （默认 4 条线程，见 `uvcpp_db_async.h`）。如果 4 条池线程都在等一条空闲连接，
 * 而那条连接被**调用方自己**用 `acquire()` 借走了 —— 它们会一直等到超时。缓解是
 * 三层的，缺一不可：`acquire()` 一律带超时（默认 5 s，所以**必然**会醒）；上面那条
 * 禁令；以及用例里那条「借走唯一一条 → 投异步池查询 → 超时拿 `NO_CONNECTION`
 * 且进程不挂」。这不是理论问题，是这套接口形状的直接后果。
 *
 * ## 池子不回收自己（这是有意的）
 *
 * **不**起后台线程、**不**挂定时器 —— 收回只在 `acquire()` / `release()` 顺手做，
 * 或者你显式 `close_idle()`。所以：不调这两个方法、池子就不会缩；也**不会缩到
 * `min` 之下**。一个池子挂着 `min` 条空闲连接是设计，不是漏回收。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_POOL_H
#define SRC_DB_UVCPP_DB_POOL_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_DB_ENABLE

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <db/uvcpp_db_params.h>
#include <db/uvcpp_db_status.h>
#include <db/uvcpp_db_table.h>

namespace uvcpp {

class uvcpp_db_client;
class uvcpp_loop;
class uvcpp_db_value;

/**
 * @brief 一组 `uvcpp_db_client`，按需借出、用完归还。
 *
 * **线程安全**：所有方法都可以从任意线程调（内部一把互斥锁 + 一个条件变量）。
 *
 * **所有权**：池子**拥有**它开的每一条连接。`acquire()` 交出的指针只在
 * `release()` 之前有效；`release()` 之后别再碰它。
 *
 * **起停**：`init()` 之后再 `init()` 会先 `close()` 旧的。析构会自动 `close()`。
 * 析构前请确认 `in_use() == 0` —— 在外的借用会被析构一起释放，那个指针当场变成
 * 野指针（见 `~uvcpp_db_pool`）。
 */
class UVCPP_API uvcpp_db_pool {
 public:
  /// 完成回调的两种形状，与 `uvcpp_db_async` 一致：都**只在 `loop` 的线程上**
  /// 被调一次。借还在池线程里做完了，回调拿到的是结果，不是一个还借着的连接。
  using table_cb = std::function<void(uvcpp_db_status, const uvcpp_db_table&)>;
  using exec_cb = std::function<void(uvcpp_db_status, int64_t affected)>;

  uvcpp_db_pool();
  ~uvcpp_db_pool();

  uvcpp_db_pool(const uvcpp_db_pool&) = delete;
  uvcpp_db_pool& operator=(const uvcpp_db_pool&) = delete;

  // ---- 起停 ----

  /// 建池：**立刻**开 `min` 条，失败当场报出来（`OPEN_FAILED` / `BAD_URL` /
  /// `NO_DRIVER`），此时池子是空的、修好问题再 `init` 一次即可。
  ///
  /// 为什么 min 条要在这里同步开：`open` 一次 1.9~2.4 ms，留到第一次查询再开是把
  /// 那个延迟**藏起来**，不是消掉。`min` 默认 1（`init()` 最坏等 2.4 ms）。
  ///
  /// `min > max` 会被夹到 `max`；`max == 0` 视为 `MISUSE`。
  uvcpp_db_status init(const std::string& url, size_t min = 1, size_t max = 8);

  /// 关掉并释放**空闲**的连接；在外的借用仍然归池子所有，`release()` 时释放。
  /// 关闭后 `acquire()` 一律返回 `nullptr`。可以再 `init()`。
  void close();

  // ---- 借还（要在一条连接上做多次操作时用，**事务必须用这个**）----

  /// 借一条连接，等到默认超时（5 s）为止。借不到返回 `nullptr`。
  ///
  /// 借到的这条在你 `release()` 之前**独占**：`begin`/`commit`/`rollback` 必须在
  /// 同一条上，所以事务只能走这条路 —— 代借代还那组每个语句一条连接，事务在上面
  /// 根本不成立（池子不提供 `begin`，就是这个原因）。
  uvcpp_db_client* acquire();

  /// 同上，但等到 `timeout_ms` 为止（`< 0` 表示一直等）。**到上限且全在外借**时
  /// 返回 `nullptr`，`last_error()` 里是原因。
  uvcpp_db_client* acquire(int timeout_ms);

  /// 还回去。不认识的指针是 no-op（记一行到 `last_error()`），不是崩溃。
  /// 还一条**已经坏掉**的连接请用 `discard()`。
  void release(uvcpp_db_client* client);

  /// 这条连接坏了（比如刚返回 `NOT_CONNECTED`）：**关掉它、不还回池子**，把额度
  /// 让给新开的。与 `release()` 一样解除借用关系。
  ///
  /// 池子自己在代借代还那组方法里就是这么做的（见 `query`），手动
  /// `acquire()` 的调用方拿到 `NOT_CONNECTED` 时也该这么收尾。
  void discard(uvcpp_db_client* client);

  // ---- 代借代还（单条语句用这个；**事务别用**）----

  uvcpp_db_status query(const std::string& sql, uvcpp_db_table* out);
  uvcpp_db_status query(const std::string& sql, const uvcpp_db_params& params,
                        uvcpp_db_table* out);
  uvcpp_db_status execute(const std::string& sql, const uvcpp_db_params& params,
                          int64_t* affected = nullptr);
  uvcpp_db_status insert(const std::string& sql, const uvcpp_db_params& params,
                         uvcpp_db_value* new_id);
  /// 借一条出来 ping 一下再还。（这**不是**健康检查的全部 —— 池子不会定期做，
  /// 详见 `set_check_on_acquire`。）
  uvcpp_db_status ping();

  // ---- 异步（借还在池线程里做，回调在你的循环线程上）----
  //
  // 与 `uvcpp_db_async` 同一个形状、同一条 libuv 默认线程池。区别是这里**每次
  // 调用自己借还一条连接** —— 所以单条语句走它最省事，而事务走不了（事务要独占
  // 一条连接直到 commit，而这里的借还在回调之前就结束了）。
  //
  // 返回值：`0` 投出去了，负值是当场没投出去（没 init、loop 为空）。**投递失败
  // 回调不会被调**，与 `uvcpp_db_async` 同一条约定。

  int query(uvcpp_loop* loop, const std::string& sql, const table_cb& cb);
  int query(uvcpp_loop* loop, const std::string& sql,
            const uvcpp_db_params& params, const table_cb& cb);
  int execute(uvcpp_loop* loop, const std::string& sql,
              const uvcpp_db_params& params, const exec_cb& cb);
  int insert(uvcpp_loop* loop, const std::string& sql,
             const uvcpp_db_params& params, const exec_cb& cb);

  // ---- 调参（都在建池前后皆可调；只影响之后新建的连接/之后的借出）----

  /// 借不到连接时等多久，默认 5000 ms。`0` = 不等待（借不到立刻 `nullptr`）。
  /// **这是池子唯一的防挂保险**，别设成 `< 0`。
  void set_acquire_timeout_ms(int ms);

  /// 空闲连接超过这么久就在 `acquire()`/`release()` 顺手关掉（不影响 `min` 条
  /// 保底），默认 60000 ms。`0` = 不按空闲回收。
  void set_idle_timeout_ms(int ms);

  /// 借出前先 `ping` 一次，默认 **false**。ping 一次约 20 µs —— 每次借出都付
  /// 会吃掉池子省下的一部分。默认关着，改用「坏连接不还回池子」（`discard`）。
  void set_check_on_acquire(bool on);

  /// 池子开的每条连接在 `open()` **之前**设的语句超时（毫秒）。`0` = 用 client
  /// 自己的默认值。池子替你开连接，这是唯一能在连接期生效的地方。
  void set_timeout_ms(int ms);

  // ---- 观测（都是无锁读，可以随便在日志里打）----

  size_t size() const;           ///< 现存连接数（含在借的）
  size_t in_use() const;         ///< 在外借的数
  size_t idle() const;           ///< 池子里歇着的数
  size_t max_size() const;       ///< 上限
  size_t min_size() const;       ///< 保底
  uint64_t created_total() const;  ///< 累计开过多少条（**证明复用**：它不随调用次数涨）
  uint64_t reused_total() const;   ///< 累计借出过多少次是复用来的

  /// 最近一次失败的人读原因（借不出、开连接失败……）。与 `uvcpp_db_client` 同一个
  /// 口径：成功不清理它。
  ///
  /// **返回的是拷贝**（不是 `uvcpp_db_client::last_error()` 那样的引用）：这个类是
  /// 多线程的，返回引用就等于把"读的人得自己保证没人同时在写"这件事塞给调用方 ——
  /// 而池子恰恰是给多线程用的。一次拷贝换掉一整类数据竞争，值。
  std::string last_error() const;

  /// 立刻把空闲连接收到 `min` 条（**不看** `idle_timeout_ms`，就是现在收）。
  /// 返回关掉几条。在借的一条都不动。
  ///
  /// 平时不用调它：`acquire()` / `release()` 会按 `idle_timeout_ms` 顺手回收。
  /// 这个方法是给"我知道接下来长时间没活了，现在就想把连接还给数据库"准备的。
  size_t close_idle();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
#endif  // SRC_DB_UVCPP_DB_POOL_H
