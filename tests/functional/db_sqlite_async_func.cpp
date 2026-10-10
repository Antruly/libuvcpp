/**
 * @file tests/functional/db_sqlite_async_func.cpp
 * @brief 数据库模块 · 异步门面（`uvcpp_db_async`）：SQLite 后端。
 *
 * ## 为什么这一条带 `db_sqlite` 前缀
 *
 * `tests/functional/CMakeLists.txt` 按**文件名**决定一条用例什么时候该存在：
 * 含 `db` ⇒ 需要 db 模块，含 `db_sqlite` ⇒ 还需要 SQLite 后端。而 `db-servers`
 * 那一格**显式断言**「SQLite 关着 ⇒ 一条 `test_db_sqlite*` 都不许注册」。所以
 * 这个前缀是**承重的**：叫成 `db_async_func.cpp` 的话，它会在那一格照样被注册、
 * 然后因为跑不起来而红，且红得看起来像门面坏了。
 *
 * ## 每一条判据都在钉什么
 *
 *   1. **回调真的在循环线程上** —— 提交**从另一条线程**发出，循环在 main 上跑，
 *      于是「提交者线程 / 池线程 / 循环线程」三者互不相同，回调的线程身份才有
 *      分辨力。附带一条：`query()` 返回时回调计数必须还是 0（就地同步跑的实现
 *      会在这一条上当场红，而它恰好也能骗过线程身份那一半）。
 *   2. **投递失败时回调不被调** —— 约定是「投递失败看返回值，回调不来」。
 *   3. **回调里 `delete` 门面是安全的** —— 之后在途的那一条不再回调，进程照常
 *      收尾。这是 `alive_token` 那套的见证，同时钉住「在途的活不写已释放的门面」。
 *      「在途」是**量出来的**（`in_flight() == 2`），不是等出来的。
 *   4. **future 糖在无循环的 main 上可用**，且连投多条时全部兑现、账还干净。
 *   5. **失败走状态码，不走异常**：SQL 错时回调拿到非 0 状态 + 一句人读文案。
 *
 * ## 第 3 条：两版修法，和一个真实的死锁
 *
 * 它原本是「投两条 → 在第一条的回调里 `delete` 门面 → 断言第二条的回调不来」，
 * 隐含假定**第一条的 after_work 先于第二条**。libuv 默认池有 4 条线程，两条谁先
 * 跑完**不保证** —— 第二条先回来时，它的回调是在门面**还活着**的时候合法触发的，
 * 断言于是失败，看着像门面漏了一条回调。这是 2026-10 CI 上 Linux 那格红的原因
 * （本机 200 次才翻 1 次，CI 负载高先炸）。
 *
 * 第一版修法是「把顺序钉死」：让第二条的 work 相在**日志钩子**里停住，第一条的
 * 回调等到它进了 work 相才删门面、再放行。**这个修法本身是错的，而且错得更狠** ——
 *
 *   `uvcpp_db.cpp` 的 `query()` 整段攥着 client 的互斥量（`lock_guard` 在函数体
 *   最外层），而日志是在**锁里面**记的（`log_line`）。所以「在钩子里扣住」= 「替
 *   整个 client 攥着锁」。同一个 client 上，第一条那条活的查询会去等这把锁 ——
 *   于是第一条的 `after_work` 永远不来、第一条的回调永远不来、闸门永远不开、
 *   第二条永远不放。**构造性的死锁**，不是偶发。
 *
 * 现场（本机 500 次循环跑到第 N 次挂住 73 分钟）：进程 7 条线程**全部**停在
 * futex 上，最后三行输出是 `第一条回调来过一次，实得 0` / `第二条**真的跑过**`。
 * 「第二条跑过」而「第一条没回来」正是上面那条链的指纹。给它加个 5 秒上界只是把
 * 挂死换成假红，病根没动。
 *
 * 现在的做法是把确定性**换个地方取**：
 *
 *   - **「在途」用账量，不用时序**。`complete_job` 把 `in_flight` 归零放在**用户
 *     回调之后**，两条 `complete_job` 又都在循环线程上串行 —— 所以在**任何一个**
 *     回调里读 `in_flight()`，另一条的账必然还在（恒等于 2）。谁先到都成立。
 *   - **钩子改成纯粹记账**（原子自增，一个字节都不阻塞）—— 于是没有锁可攥，死锁
 *     的来路被拆掉。
 *   - **「被抑制」在 `drain` 之后才断言**。`drain` 会把还挂在循环上的 `after_work`
 *     拨干净；拨完仍是 0 条回调，配合「两条的 SQL 都真的跑过」（日志各记一笔 ⇒
 *     work 相已完成 ⇒ libuv 保证交付 `after_work`），「被抑制」才和「压根没轮到」
 *     分得开。
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_async.h>
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

/// 测试用的临时目录。与 `db_sqlite_func.cpp` 那份同一个兜底（那边是匿名
/// 命名空间里的，这边要自己来一份）。
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

uvcpp::uvcpp_db_status open_fresh(const std::string& path,
                                  uvcpp::uvcpp_db_client* db) {
  std::remove(path.c_str());
  const uvcpp::uvcpp_db_status st = db->open("sqlite://" + path);
  if (!uvcpp::uvcpp_db_ok(st)) {
    check(false, "open 失败：" + db->last_error());
    return st;
  }
  uvcpp::uvcpp_db_status e = db->execute(
      "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)");
  if (!uvcpp::uvcpp_db_ok(e)) {
    check(false, "建表失败：" + db->last_error());
  }
  return e;
}

/// 循环收尾守卫：把挂起的关闭回调拨完，再让循环随作用域销毁。
///
/// 用法是「声明在循环之后、句柄之前」，靠析构逆序拿到「句柄 → 守卫 → 循环」。
/// 这里几条用例的句柄（watchdog）都建在循环之后，所以显式把守卫摆在循环后面
/// 是不够的 —— 直接在每个用例末尾手动 `drain()` 更不容易看错顺序。
void drain_loop(uvcpp::uvcpp_loop* loop) { uvcpp_test::drain(loop); }

// ---------------------------------------------------------------------------
// 1. 回调在循环线程上
// ---------------------------------------------------------------------------

int test_callback_runs_on_loop_thread() {
  std::cout << "-- 回调在循环线程上 --" << std::endl;

  uvcpp::uvcpp_db_client db;
  const std::string path = temp_dir() + "/uvcpp-db-async-cb.db";
  if (!uvcpp::uvcpp_db_ok(open_fresh(path, &db))) return 1;
  db.execute("INSERT INTO t (name) VALUES (?)", {"甲"});
  db.execute("INSERT INTO t (name) VALUES (?)", {"乙"});

  uvcpp::uvcpp_loop loop;

  const std::thread::id main_tid = std::this_thread::get_id();
  std::atomic<int> calls(0);
  std::atomic<bool> tid_ok(false);
  std::atomic<int64_t> rows(-1);
  std::string first_name;

  uvcpp::uvcpp_db_async a(&db);

  // 从**另一条**线程提交：循环在 main 上跑，池线程是第三条 —— 三者互不相同，
  // 回调的线程身份才分得出「真的是循环线程」还是「就地跑了」还是「在池线程上」。
  std::promise<int> submitted;
  std::future<int> submitted_fut = submitted.get_future();
  std::thread submitter([&]() {
    const int rc = a.query(
        &loop, "SELECT name FROM t ORDER BY id", {},
        [&](uvcpp::uvcpp_db_status st, const uvcpp::uvcpp_db_table& t) {
          calls.fetch_add(1);
          if (std::this_thread::get_id() == main_tid) tid_ok.store(true);
          if (uvcpp::uvcpp_db_ok(st)) {
            rows.store(static_cast<int64_t>(t.row_count()));
            if (!t.empty()) first_name = t.rows()[0]["name"].to_text();
          }
          loop.stop();
        });
    submitted.set_value(rc);
  });

  const int rc = submitted_fut.get();
  // 循环还没跑，回调不可能已经来过 —— 于是这一条能把「就地同步跑」的实现钉死。
  check(rc == 0, "投递成功（返回 0），实得 " + std::to_string(rc));
  check(calls.load() == 0,
        "query() 返回时回调**还没**被调（就地同步跑的实现会在这一条上红）");

  uvcpp::uvcpp_timer watchdog(&loop);
  watchdog.start([&loop](uvcpp::uvcpp_timer*) { loop.stop(); }, 5000, 0);

  loop.run(UV_RUN_DEFAULT);

  submitter.join();

  check(calls.load() == 1, "回调恰好被调一次，实得 " +
                               std::to_string(calls.load()));
  check(tid_ok.load(),
        "回调跑在**循环线程**上（不是提交者那条，也不是池线程）");
  check(rows.load() == 2, "读到 2 行，实得 " + std::to_string(rows.load()));
  check(first_name == "甲", "第一行是「甲」，实得「" + first_name + "」");
  check(a.in_flight() == 0, "回调之后在途账归零，实得 " +
                                std::to_string(a.in_flight()));

  watchdog.stop();
  drain_loop(&loop);
  db.close();
  std::remove(path.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// 2. 投递失败 ⇒ 负返回值 + 回调不来
// ---------------------------------------------------------------------------

int test_submit_failure_never_calls_back() {
  std::cout << "-- 投递失败：回调不来 --" << std::endl;

  uvcpp::uvcpp_db_client db;
  const std::string path = temp_dir() + "/uvcpp-db-async-fail.db";
  if (!uvcpp::uvcpp_db_ok(open_fresh(path, &db))) return 1;

  std::atomic<int> calls(0);
  uvcpp::uvcpp_db_async a(&db);

  // (a) loop 是空的。
  const int rc_null_loop = a.query(
      nullptr, "SELECT 1", {},
      [&](uvcpp::uvcpp_db_status, const uvcpp::uvcpp_db_table&) {
        calls.fetch_add(1);
      });
  check(rc_null_loop < 0, "loop 为空 ⇒ 负返回值，实得 " +
                              std::to_string(rc_null_loop));

  // (b) 门面没绑连接。
  uvcpp::uvcpp_db_async unbound;
  uvcpp::uvcpp_loop loop;
  const int rc_unbound = unbound.query(
      &loop, "SELECT 1", {},
      [&](uvcpp::uvcpp_db_status, const uvcpp::uvcpp_db_table&) {
        calls.fetch_add(1);
      });
  check(rc_unbound < 0, "没绑连接 ⇒ 负返回值，实得 " +
                            std::to_string(rc_unbound));

  check(calls.load() == 0, "两次投递失败，回调一次都没被调");
  check(a.in_flight() == 0 && unbound.in_flight() == 0,
        "投递失败不留在途账（失败在投递处就结清了）");

  drain_loop(&loop);
  db.close();
  std::remove(path.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// 3. 回调里 delete 门面
// ---------------------------------------------------------------------------

int test_delete_facade_inside_callback() {
  std::cout << "-- 回调里 delete 门面 --" << std::endl;

  uvcpp::uvcpp_db_client db;
  const std::string path = temp_dir() + "/uvcpp-db-async-del.db";
  if (!uvcpp::uvcpp_db_ok(open_fresh(path, &db))) return 1;

  // 两条活各留一个 SQL 标记，用来在**日志钩子**里见证「它的 work 相真的跑过」。
  //
  // **钩子里一个字节都不许阻塞**（见文件头那段死锁记录）：`query()` 整段攥着
  // client 的互斥量，日志是在**锁里面**记的 —— 谁在钩子里停住，谁就替整个 client
  // 攥着锁，同一个 client 上另一条活的查询会跟着停。所以这里只有原子自增。
  std::atomic<int> a_sql(0);
  std::atomic<int> b_sql(0);
  db.set_log([&](const std::string& line) {
    if (line.find("A_MARK") != std::string::npos) a_sql.fetch_add(1);
    if (line.find("B_MARK") != std::string::npos) b_sql.fetch_add(1);
  });

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_db_async* a = new uvcpp::uvcpp_db_async(&db);

  std::atomic<int> arrived(0);             // 累计到了几个用户回调
  std::atomic<int> first_sql(-1);          // 先到的那个是第几条（0 = A，1 = B）
  std::atomic<int> later_calls(0);         // 后到那条的回调（**期望恒为 0**）
  std::atomic<size_t> live_at_delete(0);
  std::atomic<bool> deleted(false);

  // 两条共用同一个回调体：**先到的那个**负责删门面，后到的那个只记账。谁先到不由
  // libuv 保证（上一版正是把它当成保证了才红），所以这里两边对称。
  auto on_done = [&](int idx) {
    if (arrived.fetch_add(1) != 0) {
      later_calls.fetch_add(1);  // 门面已经在先到那个回调里没了，这里**不该**被走到
      return;
    }
    first_sql.store(idx);

    // **删门面那一刻「另一条确实在途」是量出来的，不是等出来的。** `complete_job`
    // 把 `in_flight` 归零放在**用户回调之后**（`uvcpp_db_async.cpp` 的
    // `release_slot`），两条 `complete_job` 又都在循环线程上串行 —— 所以在任何一个
    // 回调里读，另一条的账必然还在，恒等于 2。这条判据比上一版「等它进 work 相」
    // 结实：那个等法不但要靠时序，还会把它自己等死。
    live_at_delete.store(a->in_flight());

    delete a;  // 门面自己没了 —— 之后在途的活不该再回来碰它
    a = nullptr;
    deleted.store(true);
  };

  const int rc1 = a->query(&loop, "SELECT 'A_MARK' AS m", {},
                           [&](uvcpp::uvcpp_db_status,
                               const uvcpp::uvcpp_db_table&) { on_done(0); });
  const int rc2 = a->query(&loop, "SELECT 'B_MARK' AS m", {},
                           [&](uvcpp::uvcpp_db_status,
                               const uvcpp::uvcpp_db_table&) { on_done(1); });
  check(rc1 == 0 && rc2 == 0, "两条都投递成功");

  // 后到那条的用户回调**不会**来（门面已经在先到那条的回调里没了），所以没有事件
  // 能停这条循环，只能靠看门狗收场 —— 600 ms 对两条毫秒级的 SQLite 查询绰绰有余。
  // 看门狗只负责停，不负责判（它一停，下面 `drain` 还是会把排队的 after_work 拨完）。
  uvcpp::uvcpp_timer watchdog(&loop);
  watchdog.start([&loop](uvcpp::uvcpp_timer*) { loop.stop(); }, 600, 0);
  loop.run(UV_RUN_DEFAULT);

  // **`drain` 是判据的一部分，不是收尾礼节。** 它把还挂在循环上的 `after_work`
  // 拨干净（libuv 在 `uv_run` 返回前清 stop_flag，所以第一轮只是清标志、第二轮起
  // 跑真身）。只有拨过之后回调计数还是 0，「被抑制」才和「循环提前停了、压根没轮到
  // 它」分得开 —— 而下面「两条的 SQL 都跑过」那条判据证明两条 work 相都完成了，
  // libuv 对已完成的 work 保证交付 after_work，于是「没轮到」被排除。
  watchdog.stop();
  drain_loop(&loop);

  check(arrived.load() == 1,
        "总共只有一个用户回调到过（先到那个），实得 " +
            std::to_string(arrived.load()));
  check(deleted.load(),
        "先到的那个回调里门面已经删掉了（先到的是第 " +
            std::to_string(first_sql.load() + 1) + " 条投出去的）");
  check(live_at_delete.load() == 2,
        "删门面那一刻，另一条确实在途：in_flight 实得 " +
            std::to_string(live_at_delete.load()) + "，应为 2");
  check(a_sql.load() >= 1 && b_sql.load() >= 1,
        "两条的 SQL 都真的跑过（日志里各记到一笔：" +
            std::to_string(a_sql.load()) + " / " +
            std::to_string(b_sql.load()) + "）");
  check(later_calls.load() == 0,
        "门面没了之后，后到那条的回调不再来，实得 " +
            std::to_string(later_calls.load()));

  db.set_log(nullptr);
  db.close();
  std::remove(path.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// 4. 失败走状态码
// ---------------------------------------------------------------------------

int test_error_reported_through_status() {
  std::cout << "-- 失败走状态码 --" << std::endl;

  uvcpp::uvcpp_db_client db;
  const std::string path = temp_dir() + "/uvcpp-db-async-err.db";
  if (!uvcpp::uvcpp_db_ok(open_fresh(path, &db))) return 1;

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_db_async a(&db);

  std::atomic<int> calls(0);
  uvcpp::uvcpp_db_status got = uvcpp::uvcpp_db_status::OK;
  size_t got_rows = 999;

  const int rc = a.query(
      &loop, "SELECT * FROM 这张表不存在", {},
      [&](uvcpp::uvcpp_db_status st, const uvcpp::uvcpp_db_table& t) {
        calls.fetch_add(1);
        got = st;
        got_rows = t.row_count();
        loop.stop();
      });
  check(rc == 0, "SQL 错了也照样投得出去（错在读的时候才知道）");

  uvcpp::uvcpp_timer watchdog(&loop);
  watchdog.start([&loop](uvcpp::uvcpp_timer*) { loop.stop(); }, 5000, 0);
  loop.run(UV_RUN_DEFAULT);

  check(calls.load() == 1, "回调照来一次");
  check(got != uvcpp::uvcpp_db_status::OK,
        std::string("回调拿到的是失败状态，实得 ") +
            uvcpp::uvcpp_db_status_name(got));
  check(got_rows == 0, "失败时给的是**空表**（不是上一次的结果）");
  check(!db.last_error().empty(), "人读的原因在 last_error() 里");

  watchdog.stop();
  drain_loop(&loop);
  db.close();
  std::remove(path.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// 5. future 糖
// ---------------------------------------------------------------------------

int test_future_from_thread_without_loop() {
  std::cout << "-- future 糖（调用方不在循环上）--" << std::endl;

  uvcpp::uvcpp_db_client db;
  const std::string path = temp_dir() + "/uvcpp-db-async-fut.db";
  if (!uvcpp::uvcpp_db_ok(open_fresh(path, &db))) return 1;

  uvcpp::uvcpp_db_async a(&db);

  // 这一条从头到尾**没有循环**：future 那条私有循环线程是门面自己起的。
  const uvcpp::uvcpp_db_status e1 =
      a.execute("INSERT INTO t (name) VALUES (?)", {"丙"}).get().status;
  check(uvcpp::uvcpp_db_ok(e1), "execute 的 future 拿到 OK");

  uvcpp::uvcpp_db_result ins =
      a.insert("INSERT INTO t (name) VALUES (?)", {"丁"}).get();
  check(ins.ok(), "insert 的 future 拿到 OK（" + ins.error + "）");
  check(ins.last_id.to_int64() == 2,
        "insert 带回了自增 id（期待 2，实得 " +
            std::to_string(ins.last_id.to_int64()) + "）");

  uvcpp::uvcpp_db_result r = a.query("SELECT id, name FROM t ORDER BY id").get();
  check(r.ok(), "query 的 future 拿到 OK（" + r.error + "）");
  check(r.table.row_count() == 2, "读到 2 行，实得 " +
                                      std::to_string(r.table.row_count()));
  check(r.table.row_count() == 2 && r.table.rows()[1]["name"].to_text() == "丁",
        "第二行是「丁」");

  // 连投 N 条再一起 get()：N 条会**同时**进池子（真并行要看池子那一批，
  // 这里钉的是「都兑现了、顺序无所谓、账还干净」）。
  std::vector<uvcpp::uvcpp_db_async::result_fut> futs;
  for (int i = 0; i < 8; ++i) {
    futs.push_back(a.query("SELECT COUNT(*) AS c FROM t"));
  }
  int ok_count = 0;
  for (size_t i = 0; i < futs.size(); ++i) {
    const uvcpp::uvcpp_db_result rr = futs[i].get();
    if (rr.ok() && rr.table.row_count() == 1 &&
        rr.table.rows()[0]["c"].to_int64() == 2) {
      ++ok_count;
    }
  }
  check(ok_count == 8, "连投 8 条 future 全部兑现且答案一致，实得 " +
                           std::to_string(ok_count));
  check(a.in_flight() == 0, "账还干净，实得 " +
                                std::to_string(a.in_flight()));

  // 没绑连接的门面：future **必须**兑现一个失败，而不是永久挂住。
  uvcpp::uvcpp_db_async unbound;
  const uvcpp::uvcpp_db_result bad = unbound.query("SELECT 1").get();
  check(!bad.ok(), "没绑连接的 future 拿到失败（不是挂着）");
  check(!bad.error.empty(), "而且有原因：「" + bad.error + "」");

  db.close();
  std::remove(path.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// 6. 异步 execute / ping 也要能用
// ---------------------------------------------------------------------------

int test_async_execute_and_ping() {
  std::cout << "-- 异步 execute / ping --" << std::endl;

  uvcpp::uvcpp_db_client db;
  const std::string path = temp_dir() + "/uvcpp-db-async-exec.db";
  if (!uvcpp::uvcpp_db_ok(open_fresh(path, &db))) return 1;
  db.execute("INSERT INTO t (name) VALUES (?), (?), (?)", {"a", "b", "c"});

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_db_async a(&db);

  std::atomic<int> done(0);
  int64_t affected = -999;
  uvcpp::uvcpp_db_status ping_st = uvcpp::uvcpp_db_status::MISUSE;

  const int rc1 = a.execute(
      &loop, "UPDATE t SET name = ? WHERE name = ?", {"z", "a"},
      [&](uvcpp::uvcpp_db_status st, int64_t n) {
        if (uvcpp::uvcpp_db_ok(st)) affected = n;
        if (done.fetch_add(1) == 1) loop.stop();
      });
  const int rc2 = a.ping(&loop, [&](uvcpp::uvcpp_db_status st) {
    ping_st = st;
    if (done.fetch_add(1) == 1) loop.stop();
  });
  check(rc1 == 0 && rc2 == 0, "execute 与 ping 都投递成功");

  uvcpp::uvcpp_timer watchdog(&loop);
  watchdog.start([&loop](uvcpp::uvcpp_timer*) { loop.stop(); }, 5000, 0);
  loop.run(UV_RUN_DEFAULT);

  check(done.load() == 2, "两条回调都来了，实得 " + std::to_string(done.load()));
  check(affected == 1, "UPDATE 影响 1 行，实得 " + std::to_string(affected));
  check(uvcpp::uvcpp_db_ok(ping_st),
        std::string("ping 拿到 OK，实得 ") +
            uvcpp::uvcpp_db_status_name(ping_st));
  check(a.in_flight() == 0, "账还干净");

  watchdog.stop();
  drain_loop(&loop);
  db.close();
  std::remove(path.c_str());
  return 0;
}

}  // namespace

int main() {
  std::cout << "[functional db_sqlite_async] start" << std::endl;

  int rc = 0;
  rc |= test_callback_runs_on_loop_thread();
  rc |= test_submit_failure_never_calls_back();
  rc |= test_delete_facade_inside_callback();
  rc |= test_error_reported_through_status();
  rc |= test_future_from_thread_without_loop();
  rc |= test_async_execute_and_ping();

  std::cout << (rc == 0 && failures == 0
                    ? "db_sqlite_async_func: 全部通过"
                    : "db_sqlite_async_func: 有失败")
            << std::endl;
  return (rc == 0 && failures == 0) ? 0 : 1;
}
