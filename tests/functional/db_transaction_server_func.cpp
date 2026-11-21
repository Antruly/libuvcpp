/**
 * @file tests/functional/db_transaction_server_func.cpp
 * @brief 异步事务在**真服务端**上的行为（MySQL / PostgreSQL，环境变量门控）。
 *
 * SQLite 那条（`db_sqlite_transaction_func`）钉的是控制流与账本；这一条补的
 * 是"真驱动 + 真事务"那半，尤其是：
 *
 *   - **连接亲和**：事务体内两次读"后端自己的连接标识"（MySQL 的
 *     `CONNECTION_ID()` / PG 的 `pg_backend_pid()`）必须相等 —— 这是"多条语句
 *     同一条连接"在服务端那侧的**唯一**硬证据（SQLite 单文件看不出来）；
 *   - 真驱动的 `commit` / `rollback` / 唯一键冲突 / 超时收尾；
 *   - 50 个并发事务在 `max=4` 的池子上全部落地、连接归还。
 *
 * 没有给 `UVCPP_DB_TEST_MYSQL_URL` / `UVCPP_DB_TEST_PGSQL_URL` 时退 3（**未判定**，
 * `CMakeLists.txt` 给这条设了 `SKIP_RETURN_CODE`）。CI 的 `db-servers` 那一格
 * 两个 URL 都给，且 `UVCPP_DB_TEST_REQUIRE=1` —— 那时缺 URL 是失败。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_DB_ENABLE

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

/// 两种方言的占位符。模块层**原样**把 SQL 交驱动，不换算（见 db-guide §5）。
std::string ph(const std::string& url, int n) {
  if (url.compare(0, 10, "postgresql") == 0) return "$" + std::to_string(n);
  return "?";
}

bool is_pg(const std::string& url) {
  return url.compare(0, 10, "postgresql") == 0;
}

bool pump_until(uvcpp_loop& loop, const std::function<bool()>& done,
                int deadline_ms = 15000) {
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

int64_t scalar_int(uvcpp_db_pool& pool, const std::string& sql,
                   const uvcpp_db_params& params) {
  uvcpp_db_table t;
  if (!uvcpp_db_ok(pool.query(sql, params, &t)) || t.rows().empty() ||
      t.rows()[0].size() == 0) {
    return -1;
  }
  return t.rows()[0][0].to_int64();
}

/// 一条 URL 上的完整套件。返回非 0 = 有失败。
int run_suite(const std::string& url) {
  std::cout << "-- " << url << " --" << std::endl;
  const int before = g_fail;

  uvcpp_db_pool pool;
  if (!uvcpp_db_ok(pool.init(url, 1, 4))) {
    std::cerr << "pool init: " << pool.last_error() << std::endl;
    return 1;
  }

  const std::string t = "uvcpp_tx_test";
  pool.execute("DROP TABLE IF EXISTS " + t, {});
  if (!uvcpp_db_ok(pool.execute(
          "CREATE TABLE " + t + " (id INT PRIMARY KEY, v INT)", {}))) {
    std::cerr << "create: " << pool.last_error() << std::endl;
    return 1;
  }

  const std::string p1 = ph(url, 1), p2 = ph(url, 2);

  // ---- 1) 提交 --------------------------------------------------------
  {
    uvcpp_loop loop;
    std::atomic<int> calls(0);
    uvcpp_db_status st = uvcpp_db_status::MISUSE;
    const int rc = pool.transaction(
        &loop,
        [&](uvcpp_db_tx& tx) {
          tx.execute("INSERT INTO " + t + " (id, v) VALUES (" + p1 + ", " + p2 +
                         ")",
                     {1, 10});
          tx.execute("INSERT INTO " + t + " (id, v) VALUES (" + p1 + ", " + p2 +
                         ")",
                     {2, 20});
        },
        [&](uvcpp_db_status s, const std::string&) {
          ++calls;
          st = s;
        });
    check(rc == 0 && pump_until(loop, [&] { return calls.load() == 1; }),
          "提交：回调到达");
    check(st == uvcpp_db_status::OK, "提交：整体 OK");
    check(scalar_int(pool, "SELECT COUNT(*) FROM " + t, {}) == 2,
          "提交：两行都在");
    check(pool.in_use() == 0, "提交：连接已归还");
  }

  // ---- 2) 语句失败回滚 ------------------------------------------------
  {
    uvcpp_loop loop;
    std::atomic<int> calls(0);
    uvcpp_db_status st = uvcpp_db_status::OK;
    const int rc = pool.transaction(
        &loop,
        [&](uvcpp_db_tx& tx) {
          tx.execute("UPDATE " + t + " SET v = 99 WHERE id = " + p1, {1});
          tx.execute("INSERT INTO " + t + " (id, v) VALUES (" + p1 + ", " + p2 +
                         ")",
                     {1, 0});  // 主键冲突
        },
        [&](uvcpp_db_status s, const std::string&) {
          ++calls;
          st = s;
        });
    check(rc == 0 && pump_until(loop, [&] { return calls.load() == 1; }),
          "冲突：回调到达");
    check(st != uvcpp_db_status::OK, "冲突：回调非 0");
    check(scalar_int(pool, "SELECT v FROM " + t + " WHERE id = " + p1, {1}) ==
              10,
          "冲突：UPDATE 被回滚");
    check(pool.in_use() == 0, "冲突：连接已归还");
  }

  // ---- 3) 连接亲和（真服务端的连接标识）-------------------------------
  {
    uvcpp_loop loop;
    std::atomic<int> calls(0);
    int64_t id1 = -1, id2 = -1;
    const std::string id_sql =
        is_pg(url) ? "SELECT pg_backend_pid()" : "SELECT CONNECTION_ID()";
    const int rc = pool.transaction(
        &loop,
        [&](uvcpp_db_tx& tx) {
          uvcpp_db_table a, b;
          tx.query(id_sql, {}, &a);
          tx.query(id_sql, {}, &b);
          if (!a.rows().empty() && !a.rows()[0].empty())
            id1 = a.rows()[0][0].to_int64();
          if (!b.rows().empty() && !b.rows()[0].empty())
            id2 = b.rows()[0][0].to_int64();
        },
        [&](uvcpp_db_status, const std::string&) { ++calls; });
    check(rc == 0 && pump_until(loop, [&] { return calls.load() == 1; }),
          "亲和：回调到达");
    check(id1 > 0 && id1 == id2,
          "亲和：事务内两次连接标识相同（" + std::to_string(id1) + "）");
  }

  // ---- 4) 超时回滚 ----------------------------------------------------
  {
    uvcpp_loop loop;
    std::atomic<int> calls(0);
    uvcpp_db_status st = uvcpp_db_status::OK;
    uvcpp_db_tx_options opts;
    opts.timeout_ms = 1;
    const int rc = pool.transaction(
        &loop,
        [&](uvcpp_db_tx& tx) {
          tx.execute("UPDATE " + t + " SET v = 7 WHERE id = " + p1, {1});
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          tx.execute("UPDATE " + t + " SET v = 8 WHERE id = " + p1, {1});
        },
        [&](uvcpp_db_status s, const std::string&) {
          ++calls;
          st = s;
        },
        opts);
    check(rc == 0 && pump_until(loop, [&] { return calls.load() == 1; }),
          "超时：回调到达");
    check(st == uvcpp_db_status::TIMEOUT, "超时：状态是 TIMEOUT");
    check(scalar_int(pool, "SELECT v FROM " + t + " WHERE id = " + p1, {1}) ==
              10,
          "超时：改动被回滚");
    check(pool.in_use() == 0, "超时：连接已归还");
  }

  // ---- 5) 50 个并发事务 ------------------------------------------------
  {
    uvcpp_loop loop;
    const int kCount = 50;
    std::atomic<int> done(0), ok(0);
    for (int i = 0; i < kCount; ++i) {
      const int rc = pool.transaction(
          &loop,
          [&, i](uvcpp_db_tx& tx) {
            tx.execute("INSERT INTO " + t + " (id, v) VALUES (" + p1 + ", " +
                           p2 + ")",
                       {100 + i, i});
          },
          [&](uvcpp_db_status s, const std::string&) {
            if (s == uvcpp_db_status::OK) ok.fetch_add(1);
            done.fetch_add(1);
          });
      if (rc != 0) {
        check(false, "并发：投递第 " + std::to_string(i) + " 个");
        break;
      }
    }
    const bool all = pump_until(loop, [&] { return done.load() == kCount; });
    check(all, "并发：全部回调到达（" + std::to_string(done.load()) + "/" +
                   std::to_string(kCount) + "）");
    check(ok.load() == kCount, "并发：全部成功");
    check(pool.in_use() == 0, "并发：没有连接泄漏");
    check(scalar_int(pool, "SELECT COUNT(*) FROM " + t + " WHERE id >= 100",
                     {}) == kCount,
          "并发：库里正好 50 行");
  }

  pool.execute("DROP TABLE IF EXISTS " + t, {});
  return g_fail == before ? 0 : 1;
}

}  // namespace

int main() {
  const char* my = std::getenv("UVCPP_DB_TEST_MYSQL_URL");
  const char* pg = std::getenv("UVCPP_DB_TEST_PGSQL_URL");
  const char* require = std::getenv("UVCPP_DB_TEST_REQUIRE");

  std::vector<std::string> urls;
  if (my != nullptr && *my != '\0') urls.push_back(my);
  if (pg != nullptr && *pg != '\0') urls.push_back(pg);

  if (urls.empty()) {
    if (require != nullptr && *require != '\0') {
      std::cerr << "UVCPP_DB_TEST_REQUIRE 已设，但两个 URL 都是空的 —— 失败，不跳过"
                << std::endl;
      return 1;
    }
    std::cout << "SKIP: 没给 UVCPP_DB_TEST_MYSQL_URL / UVCPP_DB_TEST_PGSQL_URL"
              << std::endl;
    return 3;  // ctest 的 SKIP_RETURN_CODE
  }

  int rc = 0;
  for (size_t i = 0; i < urls.size(); ++i) {
    std::string scheme = urls[i].substr(0, urls[i].find(':'));
    // 驱动注册表里叫 `postgres`，URL scheme 叫 `postgresql` —— 两个名字都要对。
    if (scheme == "postgresql") scheme = "postgres";
    if (uvcpp_db_drivers().find(scheme) == std::string::npos) {
      std::cerr << "这份构建没有 " << scheme << " 后端（已编入："
                << uvcpp_db_drivers() << "）—— 用例不该被注册" << std::endl;
      rc |= 1;
      continue;
    }
    rc |= run_suite(urls[i]);
  }
  return rc;
}

#else  // !DB

int main() {
  std::cout << "SKIP: db disabled" << std::endl;
  return 0;
}

#endif
