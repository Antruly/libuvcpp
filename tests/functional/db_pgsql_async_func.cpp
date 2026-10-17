/**
 * @file tests/functional/db_pgsql_async_func.cpp
 * @brief 数据库模块 · PostgreSQL 的**真异步**查询（不占线程池）。
 *
 * ## 这一条要一个真的 PostgreSQL 服务端
 *
 * 连接串从 `UVCPP_DB_TEST_PGSQL_URL` 取，缺 URL 是退出码 3（未判定）；
 * 设了 `UVCPP_DB_TEST_REQUIRE` 时缺 URL 就是红。口径与 `db_pgsql_func.cpp` 一致。
 *
 * ## 为什么它是独立一条而不是并进 `db_pgsql_func.cpp`
 *
 * `db_suite.h` 那份是**跨后端共用**的断言，而真异步只有 PostgreSQL 有
 * （`uvcpp_db_client::supports_async_query()`）。把"只有一家有"的东西塞进共用
 * 套件，就得在套件里写 `if (backend == ...)` —— 那份套件自己的文件头明写不要
 * 那么做。
 *
 * ## 每一组在钉什么
 *
 *   1. **往返**：投递返回 true、返回时回调**还没**来（就地同步跑的实现当场红）、
 *      回调在循环线程上一次、行内容逐字节对。参数那条走 `$1`（本模块不换算方言）。
 *   2. **一条连接只许一条在飞**：第二条投递当场 false 且 `last_error()` 有人话；
 *      第一条落地之后同一条连接可以再投。
 *   3. **对端死掉 ⇒ `NOT_CONNECTED`**：用第二条连接 `pg_terminate_backend()`
 *      把被测那条会话杀掉，然后投一条 —— 状态必须是 `NOT_CONNECTED`（池子靠它
 *      丢弃连接；`EXEC_FAILED` 会让一条死连接回到池子里）。
 *   4. **池子的真异步 + `detach_loop`**：四条并发全部落地、值对；有在飞/排队时
 *      `detach_loop` 返回 false 且什么都不做，落地之后返回 true。**这条同时钉住
 *      唤醒句柄是 unreferenced 的**：`loop.run(UV_RUN_DEFAULT)` 必须能返回
 *      （referenced 的话它永远不返回，用例会挂到 watchdog 上）。
 *   5. **A/B**：同一组查询，`set_native_async(true)` 与 `false` 两条路的结果
 *      逐字节相同。`insert` 在真异步开着时仍走线程池（两次往返），也验一遍。
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_pool.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>

#include "loop_drain.h"

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++failures;
  } else {
    std::cout << "  [ ok ] " << what << std::endl;
  }
}

using uvcpp::uvcpp_db_client;
using uvcpp::uvcpp_db_pool;
using uvcpp::uvcpp_db_status;
using uvcpp::uvcpp_db_table;
using uvcpp::uvcpp_loop;
using uvcpp::uvcpp_timer;

const char* g_url = nullptr;

bool open_client(uvcpp_db_client* db) {
  const uvcpp_db_status st = db->open(g_url);
  if (!uvcpp::uvcpp_db_ok(st)) {
    check(false, std::string("open 失败：") + db->last_error());
    return false;
  }
  return true;
}

void prepare_table(uvcpp_db_client* db) {
  db->execute("DROP TABLE IF EXISTS uvcpp_async_t");
  db->execute("CREATE TABLE uvcpp_async_t (id INT PRIMARY KEY, name TEXT)");
  db->execute("INSERT INTO uvcpp_async_t (id, name) VALUES (1, '甲')");
  db->execute("INSERT INTO uvcpp_async_t (id, name) VALUES (2, '乙')");
  db->execute("INSERT INTO uvcpp_async_t (id, name) VALUES (3, '丙')");
}

/// 5 秒兜底：任何一条用例挂住都变成红，而不是把 ctest 挂到超时。
void arm_watchdog(uvcpp_loop* loop, uvcpp_timer* timer) {
  timer->start(
      [loop](uvcpp_timer* self) {
        std::cerr << "  [FAIL] watchdog 到点了 —— 查询没有落地" << std::endl;
        ++failures;
        self->stop();
        loop->stop();
      },
      5000, 0);
}

// ---------------------------------------------------------------------------
// 1. 往返 + 参数
// ---------------------------------------------------------------------------

int test_roundtrip() {
  std::cout << "-- 真异步往返 --" << std::endl;

  uvcpp_db_client db;
  if (!open_client(&db)) return 1;
  prepare_table(&db);
  check(db.supports_async_query(), "PostgreSQL 后端报告支持真异步");

  uvcpp_loop loop;
  uvcpp_timer watchdog(&loop);
  arm_watchdog(&loop, &watchdog);

  const std::thread::id loop_tid = std::this_thread::get_id();
  std::atomic<int> calls(0);
  std::atomic<bool> on_loop(false);
  std::atomic<int> rows(-1);
  std::string first;
  std::atomic<int> st_code(-1);

  const bool submitted = db.query_async(
      &loop, "SELECT id, name FROM uvcpp_async_t ORDER BY id", {},
      [&](uvcpp_db_status st, uvcpp_db_table t) {
        calls.fetch_add(1);
        if (std::this_thread::get_id() == loop_tid) on_loop.store(true);
        st_code.store(static_cast<int>(st));
        if (uvcpp::uvcpp_db_ok(st)) {
          rows.store(static_cast<int>(t.row_count()));
          if (!t.empty()) first = t.rows()[0]["name"].to_text();
        }
        loop.stop();
      });
  check(submitted, "投递成功（返回 true）");
  check(calls.load() == 0,
        "query_async() 返回时回调**还没**来（就地同步跑的实现会在这条上红）");

  loop.run(UV_RUN_DEFAULT);

  check(calls.load() == 1,
        "回调恰好一次，实得 " + std::to_string(calls.load()));
  check(on_loop.load(), "回调跑在循环线程上");
  check(st_code.load() == static_cast<int>(uvcpp_db_status::OK),
        "状态是 OK，实得 " +
            std::string(uvcpp::uvcpp_db_status_name(
                static_cast<uvcpp_db_status>(st_code.load()))));
  check(rows.load() == 3, "读到 3 行，实得 " + std::to_string(rows.load()));
  check(first == "甲", "第一行是「甲」，实得「" + first + "」");

  // 参数：本模块把 SQL 原样交给 libpq ⇒ 占位符必须是 `$n`。
  std::atomic<int64_t> doubled(-1);
  const bool submitted2 = db.query_async(
      &loop, "SELECT $1::int * 2 AS v", {21},
      [&](uvcpp_db_status st, uvcpp_db_table t) {
        if (uvcpp::uvcpp_db_ok(st) && !t.empty()) {
          doubled.store(t.rows()[0]["v"].to_int64());
        }
        loop.stop();
      });
  check(submitted2, "带参数的那条也投得出去");
  loop.run(UV_RUN_DEFAULT);
  check(doubled.load() == 42,
        "参数往返：21*2 = 42，实得 " + std::to_string(doubled.load()));

  watchdog.stop();
  uvcpp_test::drain(&loop);
  db.close();
  return 0;
}

// ---------------------------------------------------------------------------
// 2. 一条连接上只许一条在飞
// ---------------------------------------------------------------------------

int test_one_in_flight_only() {
  std::cout << "-- 一条连接只许一条在飞 --" << std::endl;

  uvcpp_db_client db;
  if (!open_client(&db)) return 1;

  uvcpp_loop loop;
  uvcpp_timer watchdog(&loop);
  arm_watchdog(&loop, &watchdog);

  std::atomic<int> calls(0);
  std::atomic<int64_t> got(-1);
  const bool first = db.query_async(
      &loop, "SELECT $1::int AS v", {7},
      [&](uvcpp_db_status st, uvcpp_db_table t) {
        calls.fetch_add(1);
        if (uvcpp::uvcpp_db_ok(st) && !t.empty()) {
          got.store(t.rows()[0]["v"].to_int64());
        }
        loop.stop();
      });
  check(first, "第一条投得出去");

  const bool second = db.query_async(
      &loop, "SELECT $1::int AS v", {9},
      [&](uvcpp_db_status, uvcpp_db_table) { calls.fetch_add(100); });
  check(!second, "同一条连接上的第二条当场被拒（返回 false）");
  check(db.last_error().find("已经有一条") != std::string::npos,
        "拒的理由是人话（实得「" + db.last_error() + "」）");

  loop.run(UV_RUN_DEFAULT);
  check(calls.load() == 1,
        "只有第一条落地（第二条压根没投出去），实得 " +
            std::to_string(calls.load()));
  check(got.load() == 7, "第一条的值是 7，实得 " + std::to_string(got.load()));

  // 落地之后同一条连接可以再用 —— "在飞"是**这一条查询**的状态，不是连接废了。
  std::atomic<int64_t> again(-1);
  const bool third = db.query_async(
      &loop, "SELECT $1::int AS v", {11},
      [&](uvcpp_db_status st, uvcpp_db_table t) {
        if (uvcpp::uvcpp_db_ok(st) && !t.empty()) {
          again.store(t.rows()[0]["v"].to_int64());
        }
        loop.stop();
      });
  check(third, "第一条落地之后同一条连接可以再投");
  loop.run(UV_RUN_DEFAULT);
  check(again.load() == 11, "第三条的值是 11，实得 " + std::to_string(again.load()));

  watchdog.stop();
  uvcpp_test::drain(&loop);
  db.close();
  return 0;
}

// ---------------------------------------------------------------------------
// 3. 对端死掉 ⇒ NOT_CONNECTED
// ---------------------------------------------------------------------------

int test_not_connected_on_peer_death() {
  std::cout << "-- 对端死掉 ⇒ NOT_CONNECTED --" << std::endl;

  uvcpp_db_client db;
  if (!open_client(&db)) return 1;

  // 被测那条会话的 pid。**这一步必须在异步查询之前同步做掉**：投出去之后这条
  // 连接上就不能再发第二条了。
  uvcpp_db_table pid_table;
  if (!uvcpp::uvcpp_db_ok(db.query("SELECT pg_backend_pid() AS p", &pid_table)) ||
      pid_table.empty()) {
    check(false, "拿被测会话的 pid 失败：" + db.last_error());
    return 1;
  }
  const int64_t pid = pid_table.rows()[0]["p"].to_int64();

  uvcpp_db_client killer;
  if (!open_client(&killer)) return 1;
  uvcpp_db_table killed;
  const uvcpp_db_status kst = killer.query(
      "SELECT pg_terminate_backend(" + std::to_string(pid) + ") AS k", &killed);
  check(uvcpp::uvcpp_db_ok(kst), "用另一条连接杀掉被测会话");
  killer.close();

  uvcpp_loop loop;
  uvcpp_timer watchdog(&loop);
  arm_watchdog(&loop, &watchdog);

  std::atomic<int> calls(0);
  std::atomic<int> st_code(0);
  const bool submitted = db.query_async(
      &loop, "SELECT 1 AS v", {},
      [&](uvcpp_db_status st, uvcpp_db_table) {
        calls.fetch_add(1);
        st_code.store(static_cast<int>(st));
        loop.stop();
      });
  check(submitted, "投递本身成功（连接此刻看起来还活着）");

  loop.run(UV_RUN_DEFAULT);

  check(calls.load() == 1, "回调来过一次");
  check(st_code.load() == static_cast<int>(uvcpp_db_status::NOT_CONNECTED),
        "状态是 NOT_CONNECTED（池子靠它丢弃连接），实得 " +
            std::string(uvcpp::uvcpp_db_status_name(
                static_cast<uvcpp_db_status>(st_code.load()))));

  watchdog.stop();
  uvcpp_test::drain(&loop);
  db.close();
  return 0;
}

// ---------------------------------------------------------------------------
// 4. 池子的真异步 + detach_loop
// ---------------------------------------------------------------------------

int test_pool_native_and_detach() {
  std::cout << "-- 池子真异步 + detach_loop --" << std::endl;

  uvcpp_db_pool pool;
  check(uvcpp::uvcpp_db_ok(pool.init(g_url, 1, 4)), "建池：" + pool.last_error());
  check(pool.native_async(), "默认就是真异步");

  uvcpp_loop loop;
  uvcpp_timer watchdog(&loop);
  arm_watchdog(&loop, &watchdog);

  const int kJobs = 4;
  std::atomic<int> done(0);
  std::vector<int64_t> got(kJobs, -1);
  for (int i = 0; i < kJobs; ++i) {
    const int rc = pool.query(
        &loop, "SELECT $1::int * 10 AS v", {i},
        [&, i](uvcpp_db_status st, const uvcpp_db_table& t) {
          if (uvcpp::uvcpp_db_ok(st) && !t.empty()) {
            got[i] = t.rows()[0]["v"].to_int64();
          }
          if (done.fetch_add(1) + 1 == kJobs) loop.stop();
        });
    check(rc == 0, "第 " + std::to_string(i) + " 条投递返回 0");
  }

  // 循环还没跑、四条都还在排队 ⇒ detach_loop 必须拒绝且什么都不做。
  check(!pool.detach_loop(&loop),
        "有排队中的查询时 detach_loop 返回 false");

  loop.run(UV_RUN_DEFAULT);
  check(done.load() == kJobs,
        "四条都落地了，实得 " + std::to_string(done.load()));
  bool values_ok = true;
  for (int i = 0; i < kJobs; ++i) {
    if (got[i] != i * 10) values_ok = false;
  }
  check(values_ok, "四条的值分别是 0/10/20/30");

  // 用完就得摘：不摘的话循环析构之后池子还挂着一个指向已死循环的句柄。
  check(pool.detach_loop(&loop), "全部落地之后 detach_loop 返回 true");
  check(pool.detach_loop(&loop), "再摘一次也安全（没有分片 ⇒ true）");

  watchdog.stop();
  uvcpp_test::drain(&loop);  // 唤醒句柄的关闭回调要拨一次
  return 0;
}

// ---------------------------------------------------------------------------
// 5. A/B：真异步 vs 线程池，结果必须一样
// ---------------------------------------------------------------------------

std::vector<int64_t> run_batch(bool native) {
  std::vector<int64_t> got;
  uvcpp_db_pool pool;
  if (!uvcpp::uvcpp_db_ok(pool.init(g_url, 1, 4))) {
    check(false, "建池失败：" + pool.last_error());
    return got;
  }
  pool.set_native_async(native);
  check(pool.native_async() == native, native ? "这一臂是真异步" : "这一臂是线程池");

  uvcpp_loop loop;
  uvcpp_timer watchdog(&loop);
  arm_watchdog(&loop, &watchdog);

  const int kJobs = 6;
  got.assign(kJobs, -1);
  std::atomic<int> done(0);
  // **所有回调都到齐了才停循环**（六条查询 + 那次 insert）。少算一条的话
  // `loop.stop()` 会把还在飞的查询留在循环上，主循环提前返回 —— 那看着像
  // "只有三条落地"，其实只是没人再拨循环了。
  std::atomic<int> remaining(kJobs + 1);
  auto finish_one = [&]() {
    if (remaining.fetch_sub(1) == 1) loop.stop();
  };
  for (int i = 0; i < kJobs; ++i) {
    const int rc = pool.query(
        &loop, "SELECT $1::int + 100 AS v", {i},
        [&, i](uvcpp_db_status st, const uvcpp_db_table& t) {
          if (uvcpp::uvcpp_db_ok(st) && !t.empty()) {
            got[i] = t.rows()[0]["v"].to_int64();
          }
          done.fetch_add(1);
          finish_one();
        });
    check(rc == 0, "投递返回 0（" + std::string(native ? "native" : "pool") + "）");
  }

  // `insert` 在真异步开着时**仍走线程池**（两次往返）—— 顺手验一遍它没被
  // 路由丢下。
  std::atomic<int64_t> inserted(-1);
  const int irc = pool.insert(
      &loop, "INSERT INTO uvcpp_async_seq (name) VALUES ($1)", {"A/B"},
      [&](uvcpp_db_status st, int64_t affected) {
        if (uvcpp::uvcpp_db_ok(st)) inserted.store(affected);
        finish_one();
      });
  check(irc == 0, "insert 投递返回 0");

  loop.run(UV_RUN_DEFAULT);
  // 兜底再拨几轮：`uv_close` 的收尾回调（每条查询一个 poll 句柄）要拨出来。
  for (int i = 0; i < 64 && remaining.load() > 0; ++i) loop.run(UV_RUN_NOWAIT);

  check(done.load() == kJobs,
        "六条都落地，实得 " + std::to_string(done.load()));
  check(inserted.load() == 1,
        "insert 落地（affected = 1），实得 " + std::to_string(inserted.load()));

  watchdog.stop();
  pool.detach_loop(&loop);
  uvcpp_test::drain(&loop);
  return got;
}

int test_ab_native_vs_threadpool() {
  std::cout << "-- A/B：真异步 vs 线程池 --" << std::endl;

  uvcpp_db_client db;
  if (!open_client(&db)) return 1;
  db.execute("DROP TABLE IF EXISTS uvcpp_async_seq");
  db.execute(
      "CREATE TABLE uvcpp_async_seq (id BIGSERIAL PRIMARY KEY, name TEXT)");
  db.close();

  const std::vector<int64_t> native = run_batch(true);
  const std::vector<int64_t> pooled = run_batch(false);

  check(native.size() == pooled.size(), "两臂条数一样");
  bool same = native.size() == pooled.size();
  for (size_t i = 0; i < native.size() && i < pooled.size(); ++i) {
    if (native[i] != pooled[i]) same = false;
    if (native[i] != static_cast<int64_t>(i) + 100) same = false;
  }
  check(same, "两条路的结果逐格相同（且都等于 i+100）");
  return 0;
}

}  // namespace

int main() {
  const char* url = std::getenv("UVCPP_DB_TEST_PGSQL_URL");
  const char* require = std::getenv("UVCPP_DB_TEST_REQUIRE");

  if (url == nullptr || *url == '\0') {
    if (require != nullptr && *require != '\0') {
      std::cerr << "UVCPP_DB_TEST_REQUIRE 已设，但 UVCPP_DB_TEST_PGSQL_URL 是空的"
                << " —— 这是失败，不是跳过" << std::endl;
      return 1;
    }
    std::cout << "SKIP: 没给 UVCPP_DB_TEST_PGSQL_URL，真异步这一条未判定"
              << std::endl;
    return 3;  // ctest 的 SKIP_RETURN_CODE
  }
  g_url = url;

  if (uvcpp::uvcpp_db_drivers().find("postgres") == std::string::npos) {
    std::cerr << "这份构建没有 PostgreSQL 后端（已编入："
              << uvcpp::uvcpp_db_drivers() << "）—— 这一条用例不该被注册"
              << std::endl;
    return 1;
  }

  std::cout << "-- PostgreSQL 真异步 (" << url << ") --" << std::endl;
  test_roundtrip();
  test_one_in_flight_only();
  test_not_connected_on_peer_death();
  test_pool_native_and_detach();
  test_ab_native_vs_threadpool();

  if (failures != 0) {
    std::cerr << failures << " 处失败" << std::endl;
    return 1;
  }
  std::cout << "全部通过" << std::endl;
  return 0;
}
