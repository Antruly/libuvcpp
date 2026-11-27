/**
 * @file tests/functional/orm_tx_async_func.cpp
 * @brief ORM 的事务与异步集成（真实 SQLite，不 mock）。
 *
 * 覆盖 issue #92 的用户可观察判据：
 *
 *   1. **同步事务**：提交、显式回滚、语句失败自动回滚、事务体抛异常、总超时；
 *      事务内多条 ORM 写操作与直接 `tx.execute` 混用，确认走同一条连接；
 *      事务结束后连接归还、`in_use() == 0`。
 *   2. **异步门面**（`uvcpp_orm_async.h`，SQLite 线程池路径）：find / insert /
 *      update / remove 的回调次数、线程、结果；投递失败与执行失败是两种结果；
 *      连接断开时回调报 `DB_ERROR`，之后的查询能重新开连接。
 *   3. **池**：`max=1` 时混合并发操作不挂死；唯一连接被借走时异步查询在借出
 *      超时后醒过来（`NO_CONNECTION`，不是挂住）；连接归还计数正确。
 *   4. **同步池重载**：`find_by_pk/find_all/insert/update/remove/count` 接受
 *      `uvcpp_db_pool&`，单语句借还，调用后 `in_use() == 0`。
 *
 * 文件名带 `orm_` 前缀，`UVCPP_BUILD_ORM=OFF` 时整个目标不存在。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE && UVCPP_DB_SQLITE_ENABLE

#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_transaction.h>
#include <handle/uvcpp_loop.h>
#include <orm/uvcpp_orm_async.h>
#include <orm/uvcpp_orm_query.h>
#include <orm/uvcpp_orm_repository.h>

// ---------------------------------------------------------------------------
// 测试模型
// ---------------------------------------------------------------------------

struct tx_item {
  std::int64_t id;
  std::string name;
  std::int32_t qty;
  uvcpp::nullable<std::string> note;
};

UVCPP_ORM_MODEL(tx_item, "tx_item")
UVCPP_ORM_FIELD(tx_item, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(tx_item, name, "name", UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(tx_item, qty, "qty", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(tx_item, note, "note", UVCPP_ORM_NONE)
UVCPP_ORM_MODEL_END()

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
  ++g_checks;
  if (condition) {
    std::cout << "  [ ok ] " << what << std::endl;
    return;
  }
  ++g_failures;
  std::cerr << "  [FAIL] " << what << std::endl;
}

void check_orm(uvcpp::uvcpp_orm_status got, uvcpp::uvcpp_orm_status want,
               const std::string& what, const std::string& error) {
  check(got == want, what + "（期望 " +
                         uvcpp::uvcpp_orm_status_name(want) + "，实得 " +
                         uvcpp::uvcpp_orm_status_name(got) + " / [" + error +
                         "]）");
}

void check_db(uvcpp::uvcpp_db_status got, uvcpp::uvcpp_db_status want,
              const std::string& what) {
  check(got == want, what + "（期望 " +
                         uvcpp::uvcpp_db_status_name(want) + "，实得 " +
                         uvcpp::uvcpp_db_status_name(got) + "）");
}

void check_contains(const std::string& text, const std::string& fragment,
                    const std::string& what) {
  check(text.find(fragment) != std::string::npos,
        what + "：期望包含 [" + fragment + "]，实得 [" + text + "]");
}

std::string temp_dir() {
  const char* keys[] = {"TMPDIR", "TEMP", "TMP"};
  for (std::size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    const char* value = std::getenv(keys[i]);
    if (value != nullptr && *value != '\0') {
      std::string path(value);
      while (!path.empty() &&
             (path[path.size() - 1] == '/' || path[path.size() - 1] == '\\')) {
        path.erase(path.size() - 1);
      }
      if (!path.empty()) return path;
    }
  }
  return "/tmp";
}

tx_item make_item(const std::string& name, std::int32_t qty) {
  tx_item item;
  item.id = 0;
  item.name = name;
  item.qty = qty;
  item.note.reset();
  return item;
}

/// 建一个干净的库并建好 `tx_item` 表。
bool prepare_pool(uvcpp::uvcpp_db_pool& pool, const std::string& name,
                  std::size_t min, std::size_t max, std::string* path_out) {
  const std::string path = temp_dir() + "/uvcpp-orm-tx-async-" + name + ".db";
  std::remove(path.c_str());
  std::remove((path + "-journal").c_str());
  if (path_out != nullptr) *path_out = path;

  const uvcpp::uvcpp_db_status init = pool.init("sqlite://" + path, min, max);
  if (!uvcpp::uvcpp_db_ok(init)) {
    check(false, "pool.init(" + name + ")：" + pool.last_error());
    return false;
  }
  const uvcpp::uvcpp_db_status created = pool.execute(
      "CREATE TABLE tx_item ("
      " id INTEGER PRIMARY KEY AUTOINCREMENT,"
      " name TEXT NOT NULL UNIQUE,"
      " qty INTEGER NOT NULL,"
      " note TEXT)",
      {});
  if (!uvcpp::uvcpp_db_ok(created)) {
    check(false, "CREATE TABLE(" + name + ")：" + pool.last_error());
    return false;
  }
  return true;
}

void cleanup_pool(uvcpp::uvcpp_db_pool& pool, const std::string& path) {
  pool.close();
  std::remove(path.c_str());
  std::remove((path + "-journal").c_str());
}

int64_t count_rows(uvcpp::uvcpp_db_pool& pool) {
  uvcpp::uvcpp_db_table table;
  if (!uvcpp::uvcpp_db_ok(
          pool.query("SELECT COUNT(*) AS n FROM tx_item", {}, &table)) ||
      table.row_count() == 0u) {
    return -1;
  }
  return table.rows()[0]["n"].to_int64();
}

/// 有界地泵循环，直到 done() 为真。超时返回 done() 的最终值（通常是 false）。
bool pump_until(uvcpp::uvcpp_loop& loop,
                const std::function<bool()>& done, int deadline_ms = 8000) {
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  for (;;) {
    if (done()) return true;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count() >= deadline_ms) {
      return done();
    }
    loop.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

struct tx_capture {
  tx_capture() : calls(0), status(uvcpp::uvcpp_db_status::MISUSE) {}

  std::atomic<int> calls;
  uvcpp::uvcpp_db_status status;
  std::string error;
};

bool run_transaction(uvcpp::uvcpp_db_pool& pool,
                     const uvcpp::uvcpp_db_pool::tx_body& body,
                     const uvcpp::uvcpp_db_tx_options& options,
                     tx_capture* capture) {
  uvcpp::uvcpp_loop loop;
  capture->calls.store(0);
  capture->status = uvcpp::uvcpp_db_status::MISUSE;
  capture->error.clear();

  const int rc = pool.transaction(
      &loop, body,
      [capture](uvcpp::uvcpp_db_status status, const std::string& error) {
        capture->status = status;
        capture->error = error;
        capture->calls.fetch_add(1);
      },
      options);
  check(rc == 0, "transaction() 投递成功");
  if (rc != 0) return false;
  return pump_until(loop, [capture] { return capture->calls.load() == 1; });
}

// ---------------------------------------------------------------------------
// 1. 事务提交 + 连接亲和 + ORM 与直接 tx.execute 混用
// ---------------------------------------------------------------------------

void test_commit_and_affinity() {
  std::cout << "-- 事务提交 / 连接亲和 / 混用 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "commit", 1, 1, &path)) return;

  const uvcpp::uvcpp_db_tx_stats before = pool.tx_stats();
  uvcpp::uvcpp_db_tx_options options;
  tx_capture capture;

  bool same_client = false;
  bool orm_sees_direct = false;
  bool direct_sees_orm = false;
  bool orm_sees_update = false;
  bool direct_sees_remove = false;
  std::size_t in_use_during = 0;

  const uvcpp::uvcpp_db_pool::tx_body body =
      [&](uvcpp::uvcpp_db_tx& tx) {
        std::string error;
        uvcpp::uvcpp_db_status db_status = uvcpp::uvcpp_db_status::OK;
        uvcpp::uvcpp_db_client* const first_client = tx.client();
        in_use_during = pool.in_use();

        // ORM INSERT
        tx_item alpha = make_item("alpha", 1);
        uvcpp::uvcpp_db_value alpha_id;
        uvcpp::uvcpp_orm_status status = uvcpp::insert<tx_item>(
            tx, alpha, &alpha_id, &error, &db_status);
        check_orm(status, uvcpp::uvcpp_orm_status::OK, "事务内 ORM insert",
                  error);
        check(!alpha_id.is_null(), "事务内 ORM insert 回填自增 id");

        // 直接 tx.execute 再插一条
        uvcpp::uvcpp_db_status direct = tx.execute(
            "INSERT INTO tx_item (name, qty) VALUES (?, ?)", {"beta", 2});
        check_db(direct, uvcpp::uvcpp_db_status::OK,
                 "事务内直接 tx.execute");

        // 直接 tx.query 看到 ORM 写入
        {
          uvcpp::uvcpp_db_table table;
          direct = tx.query("SELECT name FROM tx_item WHERE id = ?",
                            {alpha_id}, &table);
          direct_sees_orm =
              uvcpp::uvcpp_db_ok(direct) && table.row_count() == 1u &&
              table.rows()[0]["name"].to_text() == "alpha";
        }

        // 事务内 ORM find_by_pk 也走 tx.client() 的同一条连接。
        {
          tx_item by_pk;
          status = uvcpp::find_by_pk<tx_item>(tx, alpha_id, &by_pk, &error,
                                              &db_status);
          check_orm(status, uvcpp::uvcpp_orm_status::OK,
                    "事务内 ORM find_by_pk", error);
          check(by_pk.name == "alpha" && by_pk.qty == 1,
                "事务内 ORM find_by_pk 读到自己的未提交写入");
        }

        // 事务内 ORM count 同样看到两条未提交写入。
        {
          int64_t tx_total = -1;
          status = uvcpp::count<tx_item>(
              tx,
              uvcpp::select<tx_item>(uvcpp::uvcpp_orm_dialect::sqlite),
              &tx_total, &error, &db_status);
          check_orm(status, uvcpp::uvcpp_orm_status::OK,
                    "事务内 ORM count", error);
          check(tx_total == 2, "事务内 ORM count = 2");
        }

        // ORM find_all 看到直接 tx.execute 写入（同一事务 → 同一连接）
        {
          std::vector<tx_item> rows;
          status = uvcpp::find_all<tx_item>(
              tx,
              uvcpp::select<tx_item>(uvcpp::uvcpp_orm_dialect::sqlite)
                  .order_by(&tx_item::id, uvcpp::uvcpp_orm_order::asc),
              &rows, &error, nullptr, &db_status);
          check_orm(status, uvcpp::uvcpp_orm_status::OK,
                    "事务内 ORM find_all", error);
          orm_sees_direct = rows.size() == 2u;
        }

        // ORM UPDATE alpha
        alpha = make_item("alpha", 11);
        alpha.id = alpha_id.to_int64();
        alpha.note = std::string("updated");
        int64_t affected = -1;
        status = uvcpp::update<tx_item>(tx, alpha, &affected, &error,
                                        &db_status);
        check_orm(status, uvcpp::uvcpp_orm_status::OK, "事务内 ORM update",
                  error);
        check(affected == 1, "事务内 ORM update 影响 1 行");

        {
          uvcpp::uvcpp_db_table table;
          direct = tx.query("SELECT qty, note FROM tx_item WHERE id = ?",
                            {alpha_id}, &table);
          orm_sees_update =
              uvcpp::uvcpp_db_ok(direct) && table.row_count() == 1u &&
              table.rows()[0]["qty"].to_int64() == 11 &&
              table.rows()[0]["note"].to_text() == "updated";
        }

        // 取 beta 的 id，再用 ORM DELETE
        uvcpp::uvcpp_db_table beta_table;
        direct = tx.query("SELECT id FROM tx_item WHERE name = ?", {"beta"},
                          &beta_table);
        if (uvcpp::uvcpp_db_ok(direct) && beta_table.row_count() == 1u) {
          const uvcpp::uvcpp_db_value beta_id =
              beta_table.rows()[0]["id"];
          affected = -1;
          status = uvcpp::remove<tx_item>(tx, beta_id, &affected, &error,
                                          &db_status);
          check_orm(status, uvcpp::uvcpp_orm_status::OK,
                    "事务内 ORM remove", error);
          check(affected == 1, "事务内 ORM remove 影响 1 行");
          uvcpp::uvcpp_db_table after;
          direct = tx.query("SELECT id FROM tx_item WHERE id = ?", {beta_id},
                            &after);
          direct_sees_remove =
              uvcpp::uvcpp_db_ok(direct) && after.row_count() == 0u;
        }

        same_client = first_client != nullptr &&
                      first_client == tx.client();
      };

  const bool arrived = run_transaction(pool, body, options, &capture);
  check(arrived, "提交事务的回调到达");
  check_db(capture.status, uvcpp::uvcpp_db_status::OK,
           "提交事务回调 OK（" + capture.error + "）");
  check(same_client, "事务体内 tx.client() 自始至终是同一个指针");
  check(in_use_during == 1, "事务执行期间恰好独占一条连接（实际 " +
                                std::to_string(in_use_during) + "）");
  check(orm_sees_direct, "ORM 查询看到直接 tx.execute 的未提交写入");
  check(direct_sees_orm, "直接 tx.query 看到 ORM 的未提交写入");
  check(orm_sees_update, "直接 tx.query 看到 ORM update 的未提交结果");
  check(direct_sees_remove, "直接 tx.query 确认 ORM remove 生效");

  check(count_rows(pool) == 1, "提交后库里只有 alpha 一行");
  {
    tx_item back;
    std::string error;
    const uvcpp::uvcpp_orm_status status = uvcpp::find_by_pk<tx_item>(
        pool, uvcpp::uvcpp_db_value(std::int64_t(1)), &back, &error);
    check_orm(status, uvcpp::uvcpp_orm_status::OK,
              "提交后 find_by_pk 读到 alpha", error);
    check(back.name == "alpha" && back.qty == 11,
          "提交后的 qty 是 ORM update 写的 11");
    check(back.note.has_value() && back.note.value() == "updated",
          "提交后的 nullable note 是 ORM update 写的值");
  }
  check(pool.in_use() == 0, "事务结束后连接已归还");
  const uvcpp::uvcpp_db_tx_stats after = pool.tx_stats();
  check(after.total == before.total + 1, "tx_stats().total 涨 1");
  check(after.committed == before.committed + 1, "tx_stats().committed 涨 1");

  cleanup_pool(pool, path);
}

// ---------------------------------------------------------------------------
// 2. 显式回滚
// ---------------------------------------------------------------------------

void test_explicit_rollback() {
  std::cout << "-- 显式回滚 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "rollback", 1, 1, &path)) return;

  uvcpp::uvcpp_db_tx_options options;
  tx_capture capture;
  const uvcpp::uvcpp_db_pool::tx_body body =
      [&](uvcpp::uvcpp_db_tx& tx) {
        std::string error;
        tx_item item = make_item("rolled-back", 1);
        uvcpp::uvcpp_orm_status status =
            uvcpp::insert<tx_item>(tx, item, nullptr, &error);
        check_orm(status, uvcpp::uvcpp_orm_status::OK,
                  "回滚前 ORM insert", error);
        tx.rollback();
      };

  const bool arrived = run_transaction(pool, body, options, &capture);
  check(arrived, "显式回滚的回调到达");
  check_db(capture.status, uvcpp::uvcpp_db_status::OK,
           "显式回滚不算失败（" + capture.error + "）");
  check(count_rows(pool) == 0, "显式回滚后没有留下行");
  check(pool.in_use() == 0, "显式回滚后连接已归还");

  cleanup_pool(pool, path);
}

// ---------------------------------------------------------------------------
// 3. 语句失败 → 自动回滚；事务错误不被吞掉
// ---------------------------------------------------------------------------

void test_statement_error_rolls_back() {
  std::cout << "-- 语句失败 → 自动回滚 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "stmt-error", 1, 1, &path)) return;

  const uvcpp::uvcpp_db_tx_stats before = pool.tx_stats();
  uvcpp::uvcpp_db_tx_options options;
  tx_capture capture;

  bool first_failed = false;
  bool tx_status_preserved = false;
  bool later_refused = false;
  bool error_text_preserved = false;

  const uvcpp::uvcpp_db_pool::tx_body body =
      [&](uvcpp::uvcpp_db_tx& tx) {
        std::string error;
        uvcpp::uvcpp_db_status db_status = uvcpp::uvcpp_db_status::OK;

        tx_item alpha = make_item("dup", 1);
        uvcpp::uvcpp_orm_status status =
            uvcpp::insert<tx_item>(tx, alpha, nullptr, &error, &db_status);
        check_orm(status, uvcpp::uvcpp_orm_status::OK,
                  "冲突前的 ORM insert", error);

        tx_item duplicate = make_item("dup", 2);
        db_status = uvcpp::uvcpp_db_status::OK;
        status = uvcpp::insert<tx_item>(tx, duplicate, nullptr, &error,
                                        &db_status);
        check_orm(status, uvcpp::uvcpp_orm_status::DB_ERROR,
                  "唯一键冲突透传为 DB_ERROR", error);
        check_db(db_status, uvcpp::uvcpp_db_status::EXEC_FAILED,
                 "唯一键冲突的 db_status");
        check_contains(error, "exec_failed", "ORM 错误串带 DB 返回码短名");
        first_failed = true;

        tx_status_preserved =
            (tx.status() == db_status) && !tx.error().empty();
        error_text_preserved = error.find(tx.error()) != std::string::npos;

        // 事务已经记下第一笔错误：后续语句不再发出去，返回同一个错误。
        db_status = uvcpp::uvcpp_db_status::OK;
        tx_item later = make_item("later", 3);
        status = uvcpp::insert<tx_item>(tx, later, nullptr, &error,
                                        &db_status);
        check_orm(status, uvcpp::uvcpp_orm_status::DB_ERROR,
                  "失败后的 ORM insert 仍返回 DB_ERROR", error);
        check_db(db_status, uvcpp::uvcpp_db_status::EXEC_FAILED,
                 "失败后的语句直接返回第一笔错误");
        later_refused = true;
      };

  const bool arrived = run_transaction(pool, body, options, &capture);
  check(arrived, "失败事务的回调到达");
  check(first_failed, "唯一键冲突语句返回失败");
  check(tx_status_preserved, "uvcpp_db_tx::status()/error() 保留第一笔错误");
  check(error_text_preserved, "ORM 错误串里包含 tx.error() 原文");
  check(later_refused, "第一笔失败之后的 ORM 语句不再执行");
  check_db(capture.status, uvcpp::uvcpp_db_status::EXEC_FAILED,
           "回调拿到语句错误（" + capture.error + "）");
  check(count_rows(pool) == 0, "冲突前的 INSERT 被自动回滚");
  check(pool.in_use() == 0, "失败事务结束后连接已归还");
  const uvcpp::uvcpp_db_tx_stats after = pool.tx_stats();
  check(after.rolled_back == before.rolled_back + 1, "rolled_back 涨 1");
  check(after.failed == before.failed + 1, "failed 涨 1");

  cleanup_pool(pool, path);
}

// ---------------------------------------------------------------------------
// 4. 事务体抛异常 → 回滚
// ---------------------------------------------------------------------------

void test_exception_rolls_back() {
  std::cout << "-- 事务体异常 → 回滚 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "throw", 1, 1, &path)) return;

  uvcpp::uvcpp_db_tx_options options;
  tx_capture capture;
  const uvcpp::uvcpp_db_pool::tx_body body =
      [&](uvcpp::uvcpp_db_tx& tx) {
        std::string error;
        tx_item item = make_item("throwing", 1);
        const uvcpp::uvcpp_orm_status status =
            uvcpp::insert<tx_item>(tx, item, nullptr, &error);
        check_orm(status, uvcpp::uvcpp_orm_status::OK,
                  "异常前 ORM insert", error);
        throw std::runtime_error("boom");
      };

  const bool arrived = run_transaction(pool, body, options, &capture);
  check(arrived, "异常事务的回调到达");
  check_db(capture.status, uvcpp::uvcpp_db_status::EXEC_FAILED,
           "异常被翻译成失败状态");
  check_contains(capture.error, "boom", "回调错误串里带异常原因");
  check(count_rows(pool) == 0, "异常前的 ORM INSERT 被回滚");
  check(pool.in_use() == 0, "异常事务结束后连接已归还");

  cleanup_pool(pool, path);
}

// ---------------------------------------------------------------------------
// 5. 总超时 → 回滚
// ---------------------------------------------------------------------------

void test_timeout_rolls_back() {
  std::cout << "-- 总超时 → 回滚 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "timeout", 1, 1, &path)) return;

  const uvcpp::uvcpp_db_tx_stats before = pool.tx_stats();
  uvcpp::uvcpp_db_tx_options options;
  options.timeout_ms = 1;
  tx_capture capture;

  bool second_refused = false;
  bool status_preserved = false;

  const uvcpp::uvcpp_db_pool::tx_body body =
      [&](uvcpp::uvcpp_db_tx& tx) {
        std::string error;
        uvcpp::uvcpp_db_status db_status = uvcpp::uvcpp_db_status::OK;
        tx_item first = make_item("timeout-a", 1);
        const uvcpp::uvcpp_orm_status status0 =
            uvcpp::insert<tx_item>(tx, first, nullptr, &error, &db_status);
        check_orm(status0, uvcpp::uvcpp_orm_status::OK,
                  "超时前 ORM insert", error);

        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        tx_item second = make_item("timeout-b", 2);
        db_status = uvcpp::uvcpp_db_status::OK;
        const uvcpp::uvcpp_orm_status status = uvcpp::insert<tx_item>(
            tx, second, nullptr, &error, &db_status);
        check_orm(status, uvcpp::uvcpp_orm_status::DB_ERROR,
                  "超时后的 ORM insert 返回 DB_ERROR", error);
        check_db(db_status, uvcpp::uvcpp_db_status::TIMEOUT,
                 "超时后的 db_status 是 TIMEOUT");
        second_refused = true;
        status_preserved = (tx.status() == uvcpp::uvcpp_db_status::TIMEOUT);
      };

  const bool arrived = run_transaction(pool, body, options, &capture);
  check(arrived, "超时事务的回调到达");
  check(second_refused, "超时之后的 ORM 语句直接返回 TIMEOUT");
  check(status_preserved, "uvcpp_db_tx::status() 是 TIMEOUT");
  check_db(capture.status, uvcpp::uvcpp_db_status::TIMEOUT,
           "回调状态是 TIMEOUT（" + capture.error + "）");
  check(count_rows(pool) == 0, "超时前的 INSERT 被回滚");
  check(pool.in_use() == 0, "超时事务结束后连接已归还");
  check(pool.tx_stats().timed_out == before.timed_out + 1, "timed_out 涨 1");

  cleanup_pool(pool, path);
}

// ---------------------------------------------------------------------------
// 6. 同步池重载：单语句借还
// ---------------------------------------------------------------------------

void test_sync_pool_overloads() {
  std::cout << "-- 同步池重载（单语句借还）--" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "pool-sync", 1, 2, &path)) return;
  const std::uint64_t created_before = pool.created_total();

  std::string error;
  uvcpp::uvcpp_db_status db_status = uvcpp::uvcpp_db_status::OK;

  tx_item alpha = make_item("pool-alpha", 5);
  uvcpp::uvcpp_db_value alpha_id;
  check_orm(uvcpp::insert<tx_item>(pool, alpha, &alpha_id, &error,
                                   &db_status),
            uvcpp::uvcpp_orm_status::OK, "pool insert", error);
  check(!alpha_id.is_null(), "pool insert 回填自增 id");
  check(pool.in_use() == 0, "pool insert 之后连接已归还");

  tx_item back;
  check_orm(uvcpp::find_by_pk<tx_item>(pool, alpha_id, &back, &error,
                                       &db_status),
            uvcpp::uvcpp_orm_status::OK, "pool find_by_pk", error);
  check(back.name == "pool-alpha" && back.qty == 5,
        "pool find_by_pk 字段正确");
  check(pool.in_use() == 0, "pool find_by_pk 之后连接已归还");

  std::vector<tx_item> rows;
  int64_t total = -1;
  check_orm(uvcpp::find_all<tx_item>(
                pool,
                uvcpp::select<tx_item>(uvcpp::uvcpp_orm_dialect::sqlite),
                &rows, &error, &total, &db_status),
            uvcpp::uvcpp_orm_status::OK, "pool find_all + total", error);
  check(rows.size() == 1u && total == 1, "pool find_all 返回 1 行 / total=1");

  back.qty = 6;
  int64_t affected = -1;
  check_orm(uvcpp::update<tx_item>(pool, back, &affected, &error,
                                   &db_status),
            uvcpp::uvcpp_orm_status::OK, "pool update", error);
  check(affected == 1, "pool update 影响 1 行");

  int64_t counted = -1;
  check_orm(uvcpp::count<tx_item>(
                pool,
                uvcpp::select<tx_item>(uvcpp::uvcpp_orm_dialect::sqlite),
                &counted, &error, &db_status),
            uvcpp::uvcpp_orm_status::OK, "pool count", error);
  check(counted == 1, "pool count = 1");

  check_orm(uvcpp::remove<tx_item>(pool, alpha_id, &affected, &error,
                                   &db_status),
            uvcpp::uvcpp_orm_status::OK, "pool remove", error);
  check(affected == 1, "pool remove 影响 1 行");
  check(count_rows(pool) == 0, "pool remove 之后表为空");
  check(pool.in_use() == 0, "全部同步池调用之后 in_use == 0");
  check(pool.created_total() <= created_before + pool.max_size(),
        "同步池调用没有越过 max 新开连接（created_total=" +
            std::to_string(pool.created_total()) + "）");
  check(pool.reused_total() > 0, "同步池调用复用了连接");

  cleanup_pool(pool, path);
}

// ---------------------------------------------------------------------------
// 7. 异步：回调次数、线程、结果
// ---------------------------------------------------------------------------

void test_async_find_callbacks() {
  std::cout << "-- 异步 find：回调次数 / 线程 / 结果 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "async-find", 1, 2, &path)) return;

  for (int i = 0; i < 3; ++i) {
    if (!uvcpp::uvcpp_db_ok(pool.execute(
            "INSERT INTO tx_item (name, qty) VALUES (?, ?)",
            {std::string("row-") + std::to_string(i), std::int32_t(i + 1)},
            nullptr))) {
      check(false, "准备异步数据：" + pool.last_error());
      cleanup_pool(pool, path);
      return;
    }
  }

  uvcpp::uvcpp_loop loop;
  const std::thread::id loop_thread = std::this_thread::get_id();
  std::atomic<int> calls(0);
  std::atomic<bool> on_loop_thread(false);
  uvcpp::uvcpp_orm_status status = uvcpp::uvcpp_orm_status::OK;
  std::vector<tx_item> rows;
  std::string error;
  std::string submit_error;

  const int rc = uvcpp::find_all_async<tx_item>(
      pool, &loop,
      uvcpp::select<tx_item>(uvcpp::uvcpp_orm_dialect::sqlite)
          .order_by(&tx_item::id, uvcpp::uvcpp_orm_order::asc),
      [&](uvcpp::uvcpp_orm_status result_status,
          const std::vector<tx_item>& result_rows,
          const std::string& result_error) {
        calls.fetch_add(1);
        on_loop_thread.store(std::this_thread::get_id() == loop_thread);
        status = result_status;
        rows = result_rows;
        error = result_error;
      },
      &submit_error);
  check(rc == 0, "find_all_async 投递成功（submit_error=[" + submit_error +
                     "]）");
  check(calls.load() == 0, "find_all_async 返回时回调还没被调");

  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "find_all_async 回调到达");
  check(calls.load() == 1, "find_all_async 回调恰好一次");
  check(on_loop_thread.load(), "find_all_async 回调在循环线程上");
  check_orm(status, uvcpp::uvcpp_orm_status::OK, "find_all_async 状态 OK",
            error);
  check(rows.size() == 3u, "find_all_async 读到 3 行");
  check(rows.size() == 3u && rows[0].name == "row-0" &&
            rows[2].qty == 3,
        "find_all_async 的行内容正确");
  check(pool.in_use() == 0, "find_all_async 回调之后连接已归还");

  // find_by_pk_async：命中与 NOT_FOUND 都走回调。
  calls.store(0);
  std::string find_error;
  tx_item found;
  const int rc2 = uvcpp::find_by_pk_async<tx_item>(
      pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite,
      uvcpp::uvcpp_db_value(std::int64_t(2)),
      [&](uvcpp::uvcpp_orm_status result_status, const tx_item& value,
          const std::string& result_error) {
        calls.fetch_add(1);
        status = result_status;
        found = value;
        find_error = result_error;
      },
      &submit_error);
  check(rc2 == 0, "find_by_pk_async 投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "find_by_pk_async 回调到达");
  check_orm(status, uvcpp::uvcpp_orm_status::OK, "find_by_pk_async 状态 OK",
            find_error);
  check(found.id == 2 && found.name == "row-1" && found.qty == 2,
        "find_by_pk_async 字段正确");

  calls.store(0);
  const int rc3 = uvcpp::find_by_pk_async<tx_item>(
      pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite,
      uvcpp::uvcpp_db_value(std::int64_t(999999)),
      [&](uvcpp::uvcpp_orm_status result_status, const tx_item&,
          const std::string& result_error) {
        calls.fetch_add(1);
        status = result_status;
        find_error = result_error;
      },
      &submit_error);
  check(rc3 == 0, "find_by_pk_async(NOT_FOUND) 投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "find_by_pk_async(NOT_FOUND) 回调到达");
  check_orm(status, uvcpp::uvcpp_orm_status::NOT_FOUND,
            "find_by_pk_async 不存在返回 NOT_FOUND", find_error);
  check(pool.in_use() == 0, "find_by_pk_async 之后连接已归还");

  cleanup_pool(pool, path);
}

// ---------------------------------------------------------------------------
// 8. 异步 insert / update / remove
// ---------------------------------------------------------------------------

void test_async_write_round_trip() {
  std::cout << "-- 异步 insert / update / remove --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "async-write", 1, 2, &path)) return;

  uvcpp::uvcpp_loop loop;
  const std::thread::id loop_thread = std::this_thread::get_id();
  std::atomic<int> calls(0);
  std::atomic<bool> on_loop_thread(false);
  uvcpp::uvcpp_orm_status status = uvcpp::uvcpp_orm_status::OK;
  uvcpp::uvcpp_db_value new_id;
  std::string error;
  std::string submit_error;

  tx_item item = make_item("async-alpha", 7);
  item.note = std::string("note");

  const int rc = uvcpp::insert_async<tx_item>(
      pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite, item,
      [&](uvcpp::uvcpp_orm_status result_status,
          const uvcpp::uvcpp_db_value& result_id,
          const std::string& result_error) {
        calls.fetch_add(1);
        on_loop_thread.store(std::this_thread::get_id() == loop_thread);
        status = result_status;
        new_id = result_id;
        error = result_error;
      },
      &submit_error);
  check(rc == 0, "insert_async 投递成功（submit_error=[" + submit_error +
                     "]）");
  check(calls.load() == 0, "insert_async 返回时回调还没被调");
  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "insert_async 回调到达");
  check(calls.load() == 1, "insert_async 回调恰好一次");
  check(on_loop_thread.load(), "insert_async 回调在循环线程上");
  check_orm(status, uvcpp::uvcpp_orm_status::OK, "insert_async 状态 OK",
            error);
  check(!new_id.is_null(), "insert_async 回填自增 id");
  check(pool.in_use() == 0, "insert_async 之后连接已归还");

  // 重复 name 走回调报 DB_ERROR，且只报一次。
  calls.store(0);
  status = uvcpp::uvcpp_orm_status::OK;
  const int rc_dup = uvcpp::insert_async<tx_item>(
      pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite, item,
      [&](uvcpp::uvcpp_orm_status result_status,
          const uvcpp::uvcpp_db_value&, const std::string& result_error) {
        calls.fetch_add(1);
        status = result_status;
        error = result_error;
      },
      &submit_error);
  check(rc_dup == 0, "重复 insert_async 仍然投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "重复 insert_async 回调到达");
  check_orm(status, uvcpp::uvcpp_orm_status::DB_ERROR,
            "重复 insert_async 回调 DB_ERROR", error);
  check_contains(error, "exec_failed", "重复 insert_async 错误带返回码短名");

  // update_async：命中一行 / 命中零行都返回 OK。
  tx_item update_value = make_item("async-alpha", 9);
  update_value.id = new_id.to_int64();
  update_value.note.reset();
  calls.store(0);
  int64_t affected = -2;
  const int rc_u = uvcpp::update_async<tx_item>(
      pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite, update_value,
      [&](uvcpp::uvcpp_orm_status result_status, int64_t result_affected,
          const std::string& result_error) {
        calls.fetch_add(1);
        status = result_status;
        affected = result_affected;
        error = result_error;
      },
      &submit_error);
  check(rc_u == 0, "update_async 投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "update_async 回调到达");
  check_orm(status, uvcpp::uvcpp_orm_status::OK, "update_async 状态 OK",
            error);
  check(affected == 1, "update_async 影响 1 行");

  tx_item missing = make_item("missing", 1);
  missing.id = 999999;
  calls.store(0);
  const int rc_u0 = uvcpp::update_async<tx_item>(
      pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite, missing,
      [&](uvcpp::uvcpp_orm_status result_status, int64_t result_affected,
          const std::string& result_error) {
        calls.fetch_add(1);
        status = result_status;
        affected = result_affected;
        error = result_error;
      },
      &submit_error);
  check(rc_u0 == 0, "update_async(0 行) 投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "update_async(0 行) 回调到达");
  check_orm(status, uvcpp::uvcpp_orm_status::OK,
            "update_async 0 行不是失败", error);
  check(affected == 0, "update_async 0 行 affected == 0");

  // 异步改完之后同步读回。
  {
    tx_item back;
    std::string read_error;
    check_orm(uvcpp::find_by_pk<tx_item>(pool, new_id, &back, &read_error),
              uvcpp::uvcpp_orm_status::OK,
              "异步 update 后同步 find_by_pk", read_error);
    check(back.qty == 9 && !back.note.has_value(),
          "异步 update 的值已落库");
  }

  // remove_async
  calls.store(0);
  const int rc_r = uvcpp::remove_async<tx_item>(
      pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite, new_id,
      [&](uvcpp::uvcpp_orm_status result_status, int64_t result_affected,
          const std::string& result_error) {
        calls.fetch_add(1);
        status = result_status;
        affected = result_affected;
        error = result_error;
      },
      &submit_error);
  check(rc_r == 0, "remove_async 投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "remove_async 回调到达");
  check_orm(status, uvcpp::uvcpp_orm_status::OK, "remove_async 状态 OK",
            error);
  check(affected == 1, "remove_async 影响 1 行");
  check(count_rows(pool) == 0, "remove_async 之后表为空");
  check(pool.in_use() == 0, "异步写操作之后 in_use == 0");

  cleanup_pool(pool, path);
}

// ---------------------------------------------------------------------------
// 9. 投递失败 ≠ 执行失败
// ---------------------------------------------------------------------------

void test_async_submit_vs_execution_failure() {
  std::cout << "-- 投递失败 ≠ 执行失败 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "async-fail", 1, 2, &path)) return;

  tx_item item = make_item("fail-item", 1);
  uvcpp::uvcpp_db_value id(std::int64_t(1));
  uvcpp::uvcpp_loop loop;
  std::atomic<int> calls(0);
  uvcpp::uvcpp_orm_status status = uvcpp::uvcpp_orm_status::OK;
  std::string error;
  std::string submit_error;

  const uvcpp::uvcpp_orm_find_by_pk_cb<tx_item> count_cb =
      [&](uvcpp::uvcpp_orm_status, const tx_item&, const std::string&) {
        calls.fetch_add(1);
      };
  const uvcpp::uvcpp_orm_find_all_cb<tx_item> rows_cb =
      [&](uvcpp::uvcpp_orm_status, const std::vector<tx_item>&,
          const std::string&) { calls.fetch_add(1); };
  const uvcpp::uvcpp_orm_insert_cb insert_cb =
      [&](uvcpp::uvcpp_orm_status, const uvcpp::uvcpp_db_value&,
          const std::string&) { calls.fetch_add(1); };
  const uvcpp::uvcpp_orm_exec_cb exec_cb =
      [&](uvcpp::uvcpp_orm_status, int64_t, const std::string&) {
        calls.fetch_add(1);
      };

  // loop == nullptr：五个入口都返回负值，回调一次都不来。
  check(uvcpp::find_by_pk_async<tx_item>(
            pool, nullptr, uvcpp::uvcpp_orm_dialect::sqlite, id, count_cb,
            &submit_error) < 0,
        "find_by_pk_async(loop=null) 返回负值");
  check(!submit_error.empty(), "find_by_pk_async 投递失败有原因");
  check(uvcpp::find_all_async<tx_item>(
            pool, nullptr,
            uvcpp::select<tx_item>(uvcpp::uvcpp_orm_dialect::sqlite),
            rows_cb, &submit_error) < 0,
        "find_all_async(loop=null) 返回负值");
  check(uvcpp::insert_async<tx_item>(
            pool, nullptr, uvcpp::uvcpp_orm_dialect::sqlite, item, insert_cb,
            &submit_error) < 0,
        "insert_async(loop=null) 返回负值");
  check(uvcpp::update_async<tx_item>(
            pool, nullptr, uvcpp::uvcpp_orm_dialect::sqlite, item, exec_cb,
            &submit_error) < 0,
        "update_async(loop=null) 返回负值");
  check(uvcpp::remove_async<tx_item>(
            pool, nullptr, uvcpp::uvcpp_orm_dialect::sqlite, id, exec_cb,
            &submit_error) < 0,
        "remove_async(loop=null) 返回负值");
  check(calls.load() == 0, "投递失败时没有任何回调");

  // 空回调
  const uvcpp::uvcpp_orm_find_all_cb<tx_item> empty_cb;
  check(uvcpp::find_all_async<tx_item>(
            pool, &loop,
            uvcpp::select<tx_item>(uvcpp::uvcpp_orm_dialect::sqlite),
            empty_cb, &submit_error) < 0,
        "空回调返回负值");
  check(!submit_error.empty(), "空回调的投递失败原因");

  // 模型/SQL 构造失败也属于“没投出去”。
  check(uvcpp::find_by_pk_async<tx_item>(
            pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite, uvcpp::uvcpp_db_value(),
            count_cb, &submit_error) < 0,
        "NULL 主键在投递前被拒绝");
  check(!submit_error.empty(), "NULL 主键的投递失败原因");

  // 执行失败：投递返回 0，回调恰好一次并带 DB_ERROR。
  calls.store(0);
  status = uvcpp::uvcpp_orm_status::OK;
  error.clear();
  const uvcpp::uvcpp_orm_sql_result bad_sql = uvcpp::raw(
      uvcpp::uvcpp_orm_dialect::sqlite, "SELECT * FROM no_such_table");
  const int rc_bad = uvcpp::find_all_async<tx_item>(
      pool, &loop, bad_sql,
      [&](uvcpp::uvcpp_orm_status result_status,
          const std::vector<tx_item>& result_rows,
          const std::string& result_error) {
        calls.fetch_add(1);
        status = result_status;
        error = result_error;
        check(result_rows.empty(), "执行失败时结果集是空数组");
      },
      &submit_error);
  check(rc_bad == 0, "坏 SQL 仍然投递成功（错在执行时才知道）");
  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "坏 SQL 的回调到达");
  check_orm(status, uvcpp::uvcpp_orm_status::DB_ERROR,
            "坏 SQL 回调 DB_ERROR", error);
  check_contains(error, "prepare_failed",
                 "坏 SQL 错误串带 DB 返回码短名");

  // 映射失败：SQL 成功但列对不上，回调报 MISSING_COLUMN。
  calls.store(0);
  status = uvcpp::uvcpp_orm_status::OK;
  error.clear();
  const uvcpp::uvcpp_orm_sql_result wrong_columns =
      uvcpp::raw(uvcpp::uvcpp_orm_dialect::sqlite, "SELECT 1 AS bogus");
  const int rc_map = uvcpp::find_all_async<tx_item>(
      pool, &loop, wrong_columns,
      [&](uvcpp::uvcpp_orm_status result_status,
          const std::vector<tx_item>& result_rows,
          const std::string& result_error) {
        calls.fetch_add(1);
        status = result_status;
        error = result_error;
        check(result_rows.empty(), "映射失败时结果集是空数组");
      },
      &submit_error);
  check(rc_map == 0, "列不匹配的查询仍然投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "映射失败的回调到达");
  check_orm(status, uvcpp::uvcpp_orm_status::MISSING_COLUMN,
            "列不匹配回调 MISSING_COLUMN", error);

  // 池子关掉之后投递：负值、无回调。
  pool.close();
  calls.store(0);
  check(uvcpp::find_by_pk_async<tx_item>(
            pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite, id, count_cb,
            &submit_error) < 0,
        "池子关闭后投递返回负值");
  check(calls.load() == 0, "池子关闭后没有回调");

  cleanup_pool(pool, path);
}

// ---------------------------------------------------------------------------
// 10. 连接断开：回调报 DB_ERROR，之后能重新开连接
// ---------------------------------------------------------------------------

void test_async_broken_connection() {
  std::cout << "-- 连接断开 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "async-broken", 1, 1, &path)) return;

  // 借出唯一一条连接并关掉它，再“正常”还回去 —— 池子不知道它已经死了。
  // 下一次借出会拿到这条死连接，异步查询应当回调 NOT_CONNECTED（DB_ERROR），
  // 并且池子会 discard 它，而不是把雷留给下一次。
  uvcpp::uvcpp_db_client* broken = pool.acquire();
  check(broken != nullptr, "借出唯一一条连接");
  if (broken != nullptr) {
    broken->close();
    pool.release(broken);
  }
  check(pool.size() == 1, "死连接仍在池子里（准备条件成立）");

  uvcpp::uvcpp_loop loop;
  std::atomic<int> calls(0);
  uvcpp::uvcpp_orm_status status = uvcpp::uvcpp_orm_status::OK;
  std::string error;
  std::string submit_error;

  const int rc = uvcpp::find_by_pk_async<tx_item>(
      pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite,
      uvcpp::uvcpp_db_value(std::int64_t(1)),
      [&](uvcpp::uvcpp_orm_status result_status, const tx_item&,
          const std::string& result_error) {
        calls.fetch_add(1);
        status = result_status;
        error = result_error;
      },
      &submit_error);
  check(rc == 0, "死连接上的查询仍然投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "死连接的回调到达");
  check(calls.load() == 1, "死连接的回调恰好一次");
  check_orm(status, uvcpp::uvcpp_orm_status::DB_ERROR,
            "死连接回调 DB_ERROR", error);
  check_contains(error, "not_connected", "死连接错误串带 not_connected");
  check(pool.in_use() == 0, "死连接没有被留在外面");

  // 池子丢掉了坏连接；下一条查询应当新开一条并成功。
  calls.store(0);
  status = uvcpp::uvcpp_orm_status::OK;
  const int rc2 = uvcpp::find_all_async<tx_item>(
      pool, &loop,
      uvcpp::select<tx_item>(uvcpp::uvcpp_orm_dialect::sqlite),
      [&](uvcpp::uvcpp_orm_status result_status,
          const std::vector<tx_item>&, const std::string& result_error) {
        calls.fetch_add(1);
        status = result_status;
        error = result_error;
      },
      &submit_error);
  check(rc2 == 0, "坏连接被丢掉后重新查询投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }),
        "重新查询的回调到达");
  check_orm(status, uvcpp::uvcpp_orm_status::OK,
            "池子重新开连接后查询成功", error);
  check(pool.in_use() == 0, "重新查询之后连接已归还");

  cleanup_pool(pool, path);
}

// ---------------------------------------------------------------------------
// 11. 池 max=1：混合并发不挂死；连接归还计数
// ---------------------------------------------------------------------------

void test_async_pool_max_one_concurrency() {
  std::cout << "-- 池 max=1：混合并发不挂死 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "async-max-one", 1, 1, &path)) return;
  const std::uint64_t created_before = pool.created_total();

  uvcpp::uvcpp_loop loop;
  const int kInserts = 8;
  const int kFinds = 8;
  std::atomic<int> done(0);
  std::atomic<int> ok(0);
  std::atomic<int> bad(0);
  const int expected = kInserts + kFinds;

  for (int i = 0; i < kInserts; ++i) {
    tx_item item = make_item("max1-" + std::to_string(i), i);
    const int rc = uvcpp::insert_async<tx_item>(
        pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite, item,
        [&](uvcpp::uvcpp_orm_status status, const uvcpp::uvcpp_db_value& id,
            const std::string& error) {
          if (status == uvcpp::uvcpp_orm_status::OK && !id.is_null()) {
            ok.fetch_add(1);
          } else {
            bad.fetch_add(1);
            std::cerr << "    insert_async failed: "
                      << uvcpp::uvcpp_orm_status_name(status) << " [" << error
                      << "]" << std::endl;
          }
          done.fetch_add(1);
        });
    if (rc != 0) {
      check(false, "max=1 时 insert_async 投递第 " + std::to_string(i) +
                       " 条");
    }
  }
  for (int i = 0; i < kFinds; ++i) {
    const int rc = uvcpp::find_all_async<tx_item>(
        pool, &loop,
        uvcpp::select<tx_item>(uvcpp::uvcpp_orm_dialect::sqlite),
        [&](uvcpp::uvcpp_orm_status status,
            const std::vector<tx_item>&, const std::string& error) {
          if (status == uvcpp::uvcpp_orm_status::OK) {
            ok.fetch_add(1);
          } else {
            bad.fetch_add(1);
            std::cerr << "    find_all_async failed: "
                      << uvcpp::uvcpp_orm_status_name(status) << " [" << error
                      << "]" << std::endl;
          }
          done.fetch_add(1);
        });
    if (rc != 0) {
      check(false, "max=1 时 find_all_async 投递第 " + std::to_string(i) +
                       " 条");
    }
  }

  const bool all_arrived =
      pump_until(loop, [&] { return done.load() == expected; }, 15000);
  check(all_arrived, "max=1 的 " + std::to_string(expected) +
                         " 条混合操作全部回调（" +
                         std::to_string(done.load()) + "/" +
                         std::to_string(expected) + "）");
  check(ok.load() == expected, "全部成功（" + std::to_string(ok.load()) +
                                   "，失败 " + std::to_string(bad.load()) +
                                   "）");
  check(pool.in_use() == 0, "全部完成后 in_use == 0");
  check(pool.created_total() <= created_before + pool.max_size(),
        "max=1 没有被越过（created_total=" +
            std::to_string(pool.created_total()) + "）");
  check(pool.reused_total() > 0, "并发操作复用了同一条连接");
  check(count_rows(pool) == kInserts, "库里的行数等于插入次数");

  cleanup_pool(pool, path);
}

// ---------------------------------------------------------------------------
// 12. 池 max=1：唯一连接被借走时，异步查询在借出超时后醒
// ---------------------------------------------------------------------------

void test_async_acquire_timeout_wakes() {
  std::cout << "-- 池 max=1：借出超时能醒 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  std::string path;
  if (!prepare_pool(pool, "async-acquire-timeout", 1, 1, &path)) return;
  pool.set_acquire_timeout_ms(150);

  uvcpp::uvcpp_db_client* held = pool.acquire();
  check(held != nullptr, "把唯一一条连接借在手里");
  check(pool.in_use() == 1 && pool.idle() == 0,
        "池子里没有空闲连接了");

  uvcpp::uvcpp_loop loop;
  std::atomic<int> calls(0);
  uvcpp::uvcpp_orm_status status = uvcpp::uvcpp_orm_status::OK;
  std::string error;
  std::string submit_error;

  const int rc = uvcpp::find_by_pk_async<tx_item>(
      pool, &loop, uvcpp::uvcpp_orm_dialect::sqlite,
      uvcpp::uvcpp_db_value(std::int64_t(1)),
      [&](uvcpp::uvcpp_orm_status result_status, const tx_item&,
          const std::string& result_error) {
        calls.fetch_add(1);
        status = result_status;
        error = result_error;
      },
      &submit_error);
  check(rc == 0, "借空状态下异步查询仍然投递成功");

  check(pump_until(loop, [&] { return calls.load() == 1; }, 5000),
        "池线程在借出超时后醒了，回调到达（不是挂住）");
  check(calls.load() == 1, "借出超时的回调恰好一次");
  check_orm(status, uvcpp::uvcpp_orm_status::DB_ERROR,
            "借不出连接回调 DB_ERROR", error);
  check_contains(error, "no_connection", "错误串带 no_connection");
  check(!pool.last_error().empty(),
        "池子说得出来为什么借不出：" + pool.last_error());
  check(pool.size() == 1, "被借空期间池子没有偷偷多开连接");

  if (held != nullptr) pool.release(held);
  check(pool.idle() == 1, "还回去之后池子恢复空闲");
  check(pool.in_use() == 0, "还回去之后 in_use == 0");

  cleanup_pool(pool, path);
}

}  // namespace

int main() {
  std::cout << "[orm_tx_async] start" << std::endl;

  test_commit_and_affinity();
  test_explicit_rollback();
  test_statement_error_rolls_back();
  test_exception_rolls_back();
  test_timeout_rolls_back();
  test_sync_pool_overloads();
  test_async_find_callbacks();
  test_async_write_round_trip();
  test_async_submit_vs_execution_failure();
  test_async_broken_connection();
  test_async_pool_max_one_concurrency();
  test_async_acquire_timeout_wakes();

  std::cout << "[orm_tx_async] checks=" << g_checks
            << " failures=" << g_failures << std::endl;
  if (g_failures != 0) {
    std::cerr << "[FAIL] orm_tx_async: " << g_failures
              << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "[PASS] orm_tx_async: all " << g_checks << " checks passed"
            << std::endl;
  return 0;
}

#else  // !(ORM && DB && SQLite)

int main() {
  std::cout << "SKIP: orm/db/sqlite disabled" << std::endl;
  return 0;
}

#endif
