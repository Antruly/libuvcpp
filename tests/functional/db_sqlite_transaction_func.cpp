/**
 * @file tests/functional/db_sqlite_transaction_func.cpp
 * @brief 异步事务（`uvcpp_db_pool::transaction()`）· SQLite 后端。
 *
 * 判据都落在**结果与账**上，不落在实现细节上：
 *
 *   1. **成功路径自动提交**：事务体改两行 → 回调 `OK` → 库里是新值；`in_use()` 回 0；
 *      `tx_stats().committed` 涨一。
 *   2. **连接亲和**：事务体内两条语句拿到的 `tx.client()` 是**同一个指针**，
 *      且执行期间 `pool.in_use() == 1`（那条连接被独占）。
 *   3. **语句失败自动回滚**：先改一行、再插一条主键冲突 → 回调非 0 → 那一行**没被改**。
 *   4. **事务体抛异常照样回滚**：异常不能穿过池子的边界，回调拿到非 0 + 人读原因，
 *      数据保持原样。
 *   5. **显式 rollback**：业务主动放弃，回调 `OK`，数据保持原样。
 *   6. **总超时**：`timeout_ms` 到点之后不再执行语句、回滚、归还连接，回调 `TIMEOUT`；
 *      `tx_stats().timed_out` 涨一。
 *   7. **投递失败看返回值，回调不来**：`loop == nullptr` 与**已关的池子**两条路。
 *   8. **100 个并发事务**：全部 `OK`、行数正好 100、`in_use()` 回 0（无连接泄漏）。
 *
 * 文件名带 `db_sqlite` 是**承重的**（`tests/functional/CMakeLists.txt` 的命名约定）：
 * 它需要 SQLite 后端，而 `db-servers` 那一格显式关着 SQLite、也不许注册
 * `test_db_sqlite*`。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_DB_ENABLE && UVCPP_DB_SQLITE_ENABLE

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_transaction.h>
#include <handle/uvcpp_loop.h>

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "  [FAIL] " << what << std::endl;
  }
}

std::string temp_dir() {
  const char* keys[] = {"TMPDIR", "TEMP", "TMP"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    const char* v = std::getenv(keys[i]);
    if (v != nullptr && *v != '\0') {
      std::string s(v);
      while (!s.empty() && (s[s.size() - 1] == '/' || s[s.size() - 1] == '\\')) {
        s.erase(s.size() - 1);
      }
      if (!s.empty()) return s;
    }
  }
  return "/tmp";
}

/// 建一个干净的库并建好 `account` 表。
bool fresh_pool(uvcpp_db_pool& pool, const std::string& name) {
  const std::string path = temp_dir() + "/uvcpp-db-tx-" + name + ".db";
  std::remove(path.c_str());
  // SQLite 下 max=1：写事务串行，避免多个连接同时写时 SQLITE_BUSY 的干扰。
  if (!uvcpp_db_ok(pool.init("sqlite://" + path, 1, 1))) {
    std::cerr << "pool init: " << pool.last_error() << std::endl;
    return false;
  }
  if (!uvcpp_db_ok(pool.execute(
          "CREATE TABLE account (id INTEGER PRIMARY KEY, balance INTEGER)",
          {}))) {
    std::cerr << "create: " << pool.last_error() << std::endl;
    return false;
  }
  if (!uvcpp_db_ok(pool.execute(
          "INSERT INTO account (id, balance) VALUES (?, ?)", {1, 100})) ||
      !uvcpp_db_ok(pool.execute(
          "INSERT INTO account (id, balance) VALUES (?, ?)", {2, 100}))) {
    return false;
  }
  return true;
}

int64_t balance(uvcpp_db_pool& pool, int64_t id) {
  uvcpp_db_table t;
  if (!uvcpp_db_ok(pool.query("SELECT balance FROM account WHERE id = ?",
                              {id}, &t)) ||
      t.rows().empty()) {
    return -1;
  }
  return t.rows()[0]["balance"].to_int64();
}

/// 有界地泵循环，直到 done() 为真。
bool pump_until(uvcpp_loop& loop, const std::function<bool()>& done,
                int deadline_ms = 8000) {
  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  for (;;) {
    if (done()) return true;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count() >= deadline_ms) {
      return done();
    }
    loop.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// ---------------------------------------------------------------------------
// 1 + 2：成功提交 + 连接亲和
// ---------------------------------------------------------------------------
void test_commit_and_affinity() {
  std::cout << "-- 成功提交 / 连接亲和 --" << std::endl;
  uvcpp_db_pool pool;
  if (!fresh_pool(pool, "commit")) { check(false, "准备库"); return; }
  const uvcpp_db_tx_stats before = pool.tx_stats();

  uvcpp_loop loop;
  std::atomic<int> calls(0);
  uvcpp_db_status st = uvcpp_db_status::MISUSE;
  std::string err;
  bool same_client = false;
  size_t in_use_during = 0;

  const int rc = pool.transaction(
      &loop,
      [&](uvcpp_db_tx& tx) {
        uvcpp_db_client* c0 = tx.client();
        in_use_during = pool.in_use();
        tx.execute("UPDATE account SET balance = balance - ? WHERE id = ?",
                   {30, 1});
        uvcpp_db_client* c1 = tx.client();
        tx.execute("UPDATE account SET balance = balance + ? WHERE id = ?",
                   {30, 2});
        same_client = (c0 != nullptr && c0 == c1 && c1 == tx.client());
      },
      [&](uvcpp_db_status s, const std::string& e) {
        ++calls;
        st  = s;
        err = e;
      });
  check(rc == 0, "transaction() 投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }), "回调到达");

  check(st == uvcpp_db_status::OK, "回调整体 OK（" + err + "）");
  check(same_client, "事务体内 client() 自始至终是同一个指针");
  check(in_use_during == 1, "事务执行期间恰好独占一条连接（实际 " +
                                std::to_string(in_use_during) + "）");
  check(balance(pool, 1) == 70 && balance(pool, 2) == 130, "两行的余额都已提交");
  check(pool.in_use() == 0, "结束后连接已归还（in_use = " +
                                std::to_string(pool.in_use()) + "）");
  const uvcpp_db_tx_stats after = pool.tx_stats();
  check(after.total == before.total + 1, "tx_stats().total 涨 1");
  check(after.committed == before.committed + 1, "tx_stats().committed 涨 1");
  check(after.failed == before.failed, "没有记成失败");
}

// ---------------------------------------------------------------------------
// 3：语句失败自动回滚
// ---------------------------------------------------------------------------
void test_statement_error_rolls_back() {
  std::cout << "-- 语句失败 → 回滚 --" << std::endl;
  uvcpp_db_pool pool;
  if (!fresh_pool(pool, "stmt-error")) { check(false, "准备库"); return; }
  const uvcpp_db_tx_stats before = pool.tx_stats();

  uvcpp_loop loop;
  std::atomic<int> calls(0);
  uvcpp_db_status st = uvcpp_db_status::OK;

  const int rc = pool.transaction(
      &loop,
      [&](uvcpp_db_tx& tx) {
        tx.execute("UPDATE account SET balance = balance - ? WHERE id = ?",
                   {40, 1});
        // 主键冲突：这一条必定失败。后面那条语句不该再被发出去。
        const uvcpp_db_status bad =
            tx.execute("INSERT INTO account (id, balance) VALUES (?, ?)", {1, 0});
        check(bad != uvcpp_db_status::OK, "冲突语句返回非 0");
        const uvcpp_db_status after =
            tx.execute("UPDATE account SET balance = 999 WHERE id = ?", {1});
        check(after == bad, "失败之后的语句不再执行（直接返回第一笔错误）");
      },
      [&](uvcpp_db_status s, const std::string&) {
        ++calls;
        st = s;
      });
  check(rc == 0, "投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }), "回调到达");
  check(st == uvcpp_db_status::EXEC_FAILED,
        "回调拿到语句错误（" + std::string(uvcpp_db_status_name(st)) + "）");
  check(balance(pool, 1) == 100, "第一条 UPDATE 被回滚（余额仍是 100）");
  check(pool.in_use() == 0, "连接已归还");
  const uvcpp_db_tx_stats after = pool.tx_stats();
  check(after.rolled_back == before.rolled_back + 1, "rolled_back 涨 1");
  check(after.failed == before.failed + 1, "failed 涨 1");
}

// ---------------------------------------------------------------------------
// 4：事务体抛异常
// ---------------------------------------------------------------------------
void test_exception_rolls_back() {
  std::cout << "-- 事务体抛异常 → 回滚 --" << std::endl;
  uvcpp_db_pool pool;
  if (!fresh_pool(pool, "throw")) { check(false, "准备库"); return; }

  uvcpp_loop loop;
  std::atomic<int> calls(0);
  uvcpp_db_status st = uvcpp_db_status::OK;
  std::string err;

  const int rc = pool.transaction(
      &loop,
      [&](uvcpp_db_tx& tx) {
        tx.execute("UPDATE account SET balance = 1 WHERE id = ?", {1});
        throw std::runtime_error("boom");
      },
      [&](uvcpp_db_status s, const std::string& e) {
        ++calls;
        st  = s;
        err = e;
      });
  check(rc == 0, "投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }), "回调到达");
  check(st == uvcpp_db_status::EXEC_FAILED, "异常被翻译成失败状态");
  check(err.find("boom") != std::string::npos,
        "错误文本里带着异常原因（实际 \"" + err + "\"）");
  check(balance(pool, 1) == 100, "异常前那条 UPDATE 被回滚");
  check(pool.in_use() == 0, "连接已归还");
}

// ---------------------------------------------------------------------------
// 5：显式 rollback
// ---------------------------------------------------------------------------
void test_explicit_rollback() {
  std::cout << "-- 显式 rollback --" << std::endl;
  uvcpp_db_pool pool;
  if (!fresh_pool(pool, "explicit")) { check(false, "准备库"); return; }

  uvcpp_loop loop;
  std::atomic<int> calls(0);
  uvcpp_db_status st = uvcpp_db_status::MISUSE;

  const int rc = pool.transaction(
      &loop,
      [&](uvcpp_db_tx& tx) {
        tx.execute("UPDATE account SET balance = 5 WHERE id = ?", {1});
        tx.rollback();
      },
      [&](uvcpp_db_status s, const std::string&) {
        ++calls;
        st = s;
      });
  check(rc == 0, "投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }), "回调到达");
  check(st == uvcpp_db_status::OK, "显式回滚不算失败");
  check(balance(pool, 1) == 100, "改动已回滚");
}

// ---------------------------------------------------------------------------
// 6：总超时
// ---------------------------------------------------------------------------
void test_timeout() {
  std::cout << "-- 总超时 → 回滚 + 归还 --" << std::endl;
  uvcpp_db_pool pool;
  if (!fresh_pool(pool, "timeout")) { check(false, "准备库"); return; }
  const uvcpp_db_tx_stats before = pool.tx_stats();

  uvcpp_loop loop;
  std::atomic<int> calls(0);
  uvcpp_db_status st = uvcpp_db_status::OK;
  bool second_refused = false;

  uvcpp_db_tx_options opts;
  opts.timeout_ms = 1;
  const int rc = pool.transaction(
      &loop,
      [&](uvcpp_db_tx& tx) {
        tx.execute("UPDATE account SET balance = 7 WHERE id = ?", {1});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        second_refused =
            tx.execute("UPDATE account SET balance = 8 WHERE id = ?", {1}) ==
            uvcpp_db_status::TIMEOUT;
      },
      [&](uvcpp_db_status s, const std::string&) {
        ++calls;
        st = s;
      },
      opts);
  check(rc == 0, "投递成功");
  check(pump_until(loop, [&] { return calls.load() == 1; }), "回调到达");
  check(second_refused, "超时之后的语句直接返回 TIMEOUT");
  check(st == uvcpp_db_status::TIMEOUT, "回调状态是 TIMEOUT");
  check(balance(pool, 1) == 100, "超时前那条 UPDATE 被回滚");
  check(pool.in_use() == 0, "连接已归还");
  check(pool.tx_stats().timed_out == before.timed_out + 1, "timed_out 涨 1");
}

// ---------------------------------------------------------------------------
// 7：投递失败
// ---------------------------------------------------------------------------
void test_submit_failures() {
  std::cout << "-- 投递失败：回调不来 --" << std::endl;
  uvcpp_db_pool pool;
  if (!fresh_pool(pool, "submit")) { check(false, "准备库"); return; }

  std::atomic<int> calls(0);
  auto cb = [&](uvcpp_db_status, const std::string&) { ++calls; };
  auto body = [](uvcpp_db_tx&) {};

  check(pool.transaction(nullptr, body, cb) != 0, "loop == nullptr 返回负值");
  check(pool.transaction(nullptr, nullptr, cb) != 0, "空事务体返回负值");

  pool.close();
  check(pool.transaction(nullptr, body, cb) != 0, "池子关了返回负值");
  check(calls.load() == 0, "以上三条都没有回调");
}

// ---------------------------------------------------------------------------
// 8：100 个并发事务
// ---------------------------------------------------------------------------
void test_many_transactions() {
  std::cout << "-- 100 个并发事务 --" << std::endl;
  uvcpp_db_pool pool;
  const std::string path = temp_dir() + "/uvcpp-db-tx-many.db";
  std::remove(path.c_str());
  if (!uvcpp_db_ok(pool.init("sqlite://" + path, 1, 1))) {
    check(false, "准备池");
    return;
  }
  if (!uvcpp_db_ok(pool.execute(
          "CREATE TABLE log (id INTEGER PRIMARY KEY AUTOINCREMENT, n INTEGER)",
          {}))) {
    check(false, "建表");
    return;
  }
  const uvcpp_db_tx_stats before = pool.tx_stats();

  uvcpp_loop loop;
  const int kCount = 100;
  std::atomic<int> done(0);
  std::atomic<int> ok(0);
  for (int i = 0; i < kCount; ++i) {
    const int rc = pool.transaction(
        &loop,
        [i](uvcpp_db_tx& tx) {
          tx.execute("INSERT INTO log (n) VALUES (?)", {i});
        },
        [&](uvcpp_db_status s, const std::string&) {
          if (s == uvcpp_db_status::OK) ok.fetch_add(1);
          done.fetch_add(1);
        });
    if (rc != 0) {
      check(false, "投递第 " + std::to_string(i) + " 个事务");
      break;
    }
  }
  const bool all_done =
      pump_until(loop, [&] { return done.load() == kCount; }, 15000);
  check(all_done, "全部回调到达（" + std::to_string(done.load()) + "/" +
                      std::to_string(kCount) + "）");
  check(ok.load() == kCount, "全部成功（实际 " + std::to_string(ok.load()) + "）");
  check(pool.in_use() == 0, "没有连接泄漏（in_use = " +
                                std::to_string(pool.in_use()) + "）");
  uvcpp_db_table t;
  check(uvcpp_db_ok(pool.query("SELECT COUNT(*) AS n FROM log", {}, &t)) &&
            !t.rows().empty() && t.rows()[0]["n"].to_int64() == kCount,
        "库里正好 " + std::to_string(kCount) + " 行");
  const uvcpp_db_tx_stats after = pool.tx_stats();
  check(after.committed == before.committed + kCount, "committed 涨 " +
                                                         std::to_string(kCount));
}

}  // namespace

int main() {
  std::cout << "[db_sqlite_transaction] start" << std::endl;
  test_commit_and_affinity();
  test_statement_error_rolls_back();
  test_exception_rolls_back();
  test_explicit_rollback();
  test_timeout();
  test_submit_failures();
  test_many_transactions();

  if (g_fail == 0) {
    std::cout << "[db_sqlite_transaction] ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << "[db_sqlite_transaction] " << g_fail << " check(s) failed"
            << std::endl;
  return 1;
}

#else  // !(DB && SQLite)

int main() {
  std::cout << "SKIP: db/sqlite disabled" << std::endl;
  return 0;
}

#endif
