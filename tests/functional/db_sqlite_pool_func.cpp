/**
 * @file tests/functional/db_sqlite_pool_func.cpp
 * @brief 数据库模块 · 连接池（`uvcpp_db_pool`）：SQLite 后端。
 *
 * ## 这个文件补的是共用套件补不了的那几块
 *
 * `db_suite.h` 里那一节走的是**单线程、顺序**的借还（三个后端都跑一遍，管的是
 * "池子本身不该有后端差异"）。这里管的是只有单进程多线程 / 事件循环才看得见的
 * 那几件事：
 *
 *   1. **异步池查询**：借还在池线程里做，回调在循环线程上，回来之后
 *      `in_use()` 必须是 0（借了没还的池子会在这一条上红）。
 *   2. **池线程会被借空的池子等死** —— 本文件最要紧的一条。`max=1`、把那唯一
 *      一条借在手里、再投一个异步池查询：那个池线程会去等一条空闲连接。
 *      判据是它**在借出超时后醒过来**并拿到 `NO_CONNECTION`，而**不是**挂住
 *      （看门狗咬到就是失败）。这条同时是「acquire 一律带超时」那层防线的见证。
 *   3. **多线程借还不会把池子撑过 `max`**，而且是**复用**（`created_total()` 停在
 *      `max` 上，不随调用次数涨）。
 *   4. **在途的活遇到池子先走一步**：投出去之后立刻 `delete` 池子，进程不能崩，
 *      回调也不能被调（存活凭证那套的见证）。**这一条同时钉着一处平台相关的
 *      回归，别删**：见下面「第 4 节为什么值钱」。
 *
 * ## 第 4 节为什么值钱
 *
 * 它看着和第 3 节重复（"多个线程一起借还"），其实是**唯一**能发现下面这个 bug
 * 的用例：`uvcpp_db_pool` 的析构要"等 work 相归零"，而那段等待一度写在
 * `~impl()` 里，`impl_` 又是个 `std::unique_ptr` —— 两个标准库的析构顺序**正好
 * 相反**（libc++ 先把成员指针置空再调 deleter，libstdc++ 反过来）。于是那段等待
 * 在 Linux 上保护得严严实实，在 macOS 上等的却是"`impl_` 已经空了"的那扇窗，
 * 工作线程撞在 `impl_->mu_` 上直接 SEGV。
 *
 * 关键在于：**这个 bug 在 Linux 上本地永远跑不出来** —— libstdc++ 恰好掩盖了它，
 * 而且没有任何内存错误可供 ASan / valgrind / TSan 抓（它就是"时序对了"，不是
 * 数据竞争）。当年为了定位它，本机把第 4 节在 `-O0`/`-O3` 下各跑了四百多遍、外加
 * valgrind 与 TSan，全绿。所以这条用例是**跨标准库**的守卫：只有 macOS 那条腿
 * （libc++）真的会红。删了它，这个 bug 会无声无息地回来。
 *
 * ## 为什么文件名带 `db_sqlite`
 *
 * `tests/functional/CMakeLists.txt` 按文件名决定一条用例什么时候该存在，而
 * `db-servers` 那一格显式断言「SQLite 关着 ⇒ 一条 `test_db_sqlite*` 都不许注册」。
 * 叫成 `db_pool_func.cpp` 的话它会在那一格照样注册、然后因为没后端而红 —— 且红得
 * 看起来像池子坏了。池子是纯本库的东西，一个后端就能把上面四条钉死。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_value.h>
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

std::string g_url;
std::string g_path;

/// 建一个干净的库（文件删掉重来），并塞两行进去给查询用。
bool prepare_db() {
  std::remove(g_path.c_str());
  uvcpp::uvcpp_db_client db;
  if (!uvcpp::uvcpp_db_ok(db.open(g_url))) {
    check(false, "准备数据库失败：" + db.last_error());
    return false;
  }
  uvcpp::uvcpp_db_status st = db.execute(
      "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)");
  if (uvcpp::uvcpp_db_ok(st)) {
    st = db.execute("INSERT INTO t (name) VALUES ('甲'), ('乙')");
  }
  if (!uvcpp::uvcpp_db_ok(st)) {
    check(false, "准备数据库失败：" + db.last_error());
    return false;
  }
  db.close();
  return true;
}

// ---------------------------------------------------------------------------
// 1. 异步池查询：借还在池线程里，回调在循环线程上
// ---------------------------------------------------------------------------

int test_async_query_on_loop_thread() {
  std::cout << "-- 异步池查询 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  check(uvcpp::uvcpp_db_ok(pool.init(g_url, 1, 2)), "init(min=1, max=2)");

  uvcpp::uvcpp_loop loop;
  const std::thread::id main_tid = std::this_thread::get_id();
  std::atomic<int> calls(0);
  std::atomic<bool> tid_ok(false);
  std::atomic<int64_t> rows(-1);
  std::atomic<int> status_seen(-1);

  const int rc = pool.query(
      &loop, "SELECT name FROM t ORDER BY id", {},
      [&](uvcpp::uvcpp_db_status st, const uvcpp::uvcpp_db_table& t) {
        calls.fetch_add(1);
        if (std::this_thread::get_id() == main_tid) tid_ok.store(true);
        status_seen.store(static_cast<int>(st));
        if (uvcpp::uvcpp_db_ok(st)) {
          rows.store(static_cast<int64_t>(t.row_count()));
        }
        loop.stop();
      });
  check(rc == 0, "投递成功（返回 0），实得 " + std::to_string(rc));
  check(calls.load() == 0, "投出去时回调还没来");

  uvcpp::uvcpp_timer watchdog(&loop);
  watchdog.start([&loop](uvcpp::uvcpp_timer*) { loop.stop(); }, 5000, 0);
  loop.run(UV_RUN_DEFAULT);

  check(calls.load() == 1, "回调恰好一次，实得 " + std::to_string(calls.load()));
  check(tid_ok.load(), "回调跑在**循环线程**上");
  check(status_seen.load() == static_cast<int>(uvcpp::uvcpp_db_status::OK),
        "状态是 OK");
  check(rows.load() == 2, "读到 2 行，实得 " + std::to_string(rows.load()));
  // 借还在**池线程**里做完了 —— 回调拿着的是结果，不是一个还借着的连接。
  check(pool.in_use() == 0, "回调之后没有留在外面的连接，实得 " +
                                std::to_string(pool.in_use()));

  watchdog.stop();
  uvcpp_test::drain(&loop);
  pool.close();
  return 0;
}

// ---------------------------------------------------------------------------
// 2. 借空池子 ⇒ 池线程在超时后醒，而不是挂住
// ---------------------------------------------------------------------------

int test_borrowed_connection_starves_pool_thread() {
  std::cout << "-- 借空池子：池线程必须醒 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  check(uvcpp::uvcpp_db_ok(pool.init(g_url, 1, 1)), "init(min=1, max=1)：池子就一条");
  // 借出超时给短的：这一条要证的是"到点会醒"，不是"能等多久"。
  // 同时也让这条用例稳稳落在 db-servers 那格的 `ctest --timeout 30` 里。
  pool.set_acquire_timeout_ms(150);

  uvcpp::uvcpp_db_client* held = pool.acquire();
  check(held != nullptr, "把那唯一一条借在手里");
  check(pool.in_use() == 1 && pool.idle() == 0, "池子里一条空闲的都没有了");

  uvcpp::uvcpp_loop loop;
  std::atomic<int> calls(0);
  std::atomic<int> status_seen(-1);
  std::atomic<bool> watchdog_fired(false);

  const int rc = pool.query(
      &loop, "SELECT 1 AS one", {},
      [&](uvcpp::uvcpp_db_status st, const uvcpp::uvcpp_db_table&) {
        calls.fetch_add(1);
        status_seen.store(static_cast<int>(st));
        loop.stop();
      });
  check(rc == 0, "异步池查询投出去了（返回 0），实得 " + std::to_string(rc));

  // 看门狗**咬到就是失败**：池线程要是挂住了，回调永远不来，这条用例只能靠它
  // 收场 —— 而那正是要判红的情形。
  uvcpp::uvcpp_timer watchdog(&loop);
  watchdog.start(
      [&](uvcpp::uvcpp_timer*) {
        watchdog_fired.store(true);
        loop.stop();
      },
      5000, 0);
  loop.run(UV_RUN_DEFAULT);

  check(!watchdog_fired.load(), "没有挂住：池线程在借出超时后自己醒了");
  check(calls.load() == 1, "回调来了一次，实得 " + std::to_string(calls.load()));
  check(status_seen.load() ==
            static_cast<int>(uvcpp::uvcpp_db_status::NO_CONNECTION),
        "拿到的是 NO_CONNECTION（借不出），实得 " +
            std::string(uvcpp::uvcpp_db_status_name(
                static_cast<uvcpp::uvcpp_db_status>(status_seen.load()))));
  check(!pool.last_error().empty(), "池子说得出来为什么借不出：" + pool.last_error());
  check(pool.size() == 1, "被借空期间池子没有偷偷多开连接");

  watchdog.stop();
  uvcpp_test::drain(&loop);

  pool.release(held);
  check(pool.idle() == 1, "还回去之后池子恢复空闲");
  uvcpp::uvcpp_db_table t;
  check(uvcpp::uvcpp_db_ok(pool.query("SELECT 1 AS one", &t)),
        "还回去之后同步池查询又能用了");
  pool.close();
  return 0;
}

// ---------------------------------------------------------------------------
// 3. 多线程借还：不撑过 max，而且是复用
// ---------------------------------------------------------------------------

int test_concurrent_borrow_respects_max() {
  std::cout << "-- 多线程借还 --" << std::endl;

  const size_t kMax = 3;
  const int kThreads = 4;
  const int kPerThread = 100;

  uvcpp::uvcpp_db_pool pool;
  check(uvcpp::uvcpp_db_ok(pool.init(g_url, 1, kMax)), "init(min=1, max=3)");
  pool.set_acquire_timeout_ms(5000);

  std::atomic<int> bad(0);
  std::atomic<size_t> oversize(0);
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.push_back(std::thread([&]() {
      for (int k = 0; k < kPerThread; ++k) {
        uvcpp::uvcpp_db_table t;
        if (!uvcpp::uvcpp_db_ok(pool.query("SELECT 1 AS one", &t))) {
          bad.fetch_add(1);
        }
        if (pool.size() > kMax) oversize.fetch_add(1);
      }
    }));
  }
  for (std::thread& t : threads) t.join();

  check(bad.load() == 0, "4 线程 × 100 次池查询全部成功，失败 " +
                             std::to_string(bad.load()) + " 次");
  check(oversize.load() == 0, "全程 size() 没有超过 max");
  check(pool.size() <= kMax, "结束后 size() <= max，实得 " +
                                 std::to_string(pool.size()));
  // 复用，而不是"每次借都给新开一条"。
  check(pool.created_total() <= kMax,
        "created_total 停在 max 上（复用而不是每次新开），实得 " +
            std::to_string(pool.created_total()));
  check(pool.reused_total() >=
            static_cast<uint64_t>(kThreads * kPerThread) - kMax,
        "绝大多数是复用来的，reused_total = " +
            std::to_string(pool.reused_total()));
  check(pool.in_use() == 0, "全部归还，in_use() == 0");

  // 坏连接不回池子：丢弃之后 size 少一。
  uvcpp::uvcpp_db_client* c = pool.acquire(2000);
  const size_t before = pool.size();
  check(c != nullptr, "再借一条");
  pool.discard(c);
  check(pool.size() == before - 1, "discard 之后那条离开了池子，实得 " +
                                       std::to_string(pool.size()));

  pool.close();
  return 0;
}

// ---------------------------------------------------------------------------
// 4. 在途的活遇到池子先走一步
// ---------------------------------------------------------------------------

int test_pool_destroyed_with_job_in_flight() {
  std::cout << "-- 池子先走一步 --" << std::endl;

  uvcpp::uvcpp_db_pool* pool = new uvcpp::uvcpp_db_pool();
  check(uvcpp::uvcpp_db_ok(pool->init(g_url, 1, 2)), "init");

  uvcpp::uvcpp_loop loop;
  std::atomic<int> calls(0);
  std::atomic<bool> watchdog_fired(false);

  const int rc = pool->query(&loop, "SELECT 1 AS one", {},
                             [&](uvcpp::uvcpp_db_status, const uvcpp::uvcpp_db_table&) {
                               calls.fetch_add(1);
                               loop.stop();
                             });
  check(rc == 0, "投出去了");

  // 池子当场没：析构会等 work 相收完（这一条就是那段等待的见证 —— 不等的话
  // 工作线程会踩在已经释放的连接上，进程在这里就崩了，不可能走到下面）。
  //
  // **这一行是跨标准库的判据，不是随手写的一句 `delete`。** 池线程此刻正按设计
  // 在 `run_pool_job` 里回头调 `pool->acquire()`，而 `pool->impl_` 是个
  // `std::unique_ptr`：那段"等 work 相归零"必须在 `impl_` 被销毁**之前**跑完。
  // libc++（macOS）先置空成员指针再调 deleter，libstdc++（Linux）反过来 —— 所以
  // 放错地方只有 macOS 会红，本机怎么跑都是绿的（见文件头「第 4 节为什么值钱」）。
  delete pool;

  uvcpp::uvcpp_timer watchdog(&loop);
  watchdog.start(
      [&](uvcpp::uvcpp_timer*) {
        watchdog_fired.store(true);
        loop.stop();
      },
      800, 0);
  loop.run(UV_RUN_DEFAULT);

  check(watchdog_fired.load(), "池子没了之后没有任何东西会停这条循环（回调被抑制）");
  check(calls.load() == 0, "池子没了 ⇒ 回调不被调（存活凭证那套），实得 " +
                               std::to_string(calls.load()));

  watchdog.stop();
  uvcpp_test::drain(&loop);
  return 0;
}

}  // namespace

int main() {
  g_path = temp_dir() + "/uvcpp-db-pool.db";
  g_url = "sqlite://" + g_path;

  std::cout << "== uvcpp_db_pool · SQLite ==" << std::endl;
  if (!prepare_db()) return 1;

  test_async_query_on_loop_thread();
  test_borrowed_connection_starves_pool_thread();
  test_concurrent_borrow_respects_max();
  test_pool_destroyed_with_job_in_flight();

  std::remove(g_path.c_str());

  if (failures == 0) {
    std::cout << "db_sqlite_pool_func: 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "db_sqlite_pool_func: " << failures << " 条失败" << std::endl;
  return 1;
}
