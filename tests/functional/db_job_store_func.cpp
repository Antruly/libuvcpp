/**
 * @file tests/functional/db_job_store_func.cpp
 * @brief 作业队列的数据库持久化（issue #67 剩余）：SQLite / MySQL / PostgreSQL。
 *
 * 判据分三层：
 *
 * 1. **直接读写**：`save` → `load_all` 字段逐项相等；`erase` 之后读不到。
 * 2. **跑过的作业也持久化**：队列提交并成功执行之后，记录仍在表里（状态
 *    SUCCESS），换一个 store 实例仍读得到。
 * 3. **跨实例恢复**：手工写入一条 PENDING，用**新的** store + queue 打开同一个
 *    库，`start()` 之后它被执行并变成 SUCCESS —— 这就是"进程重启后未完成任务
 *    能恢复"的最小闭环。
 *
 * 后端选择：SQLite 用临时文件；如果给了 `UVCPP_DB_TEST_MYSQL_URL` /
 * `UVCPP_DB_TEST_PGSQL_URL`，再对那两个后端各做一遍直接读写（表名带 pid，
 * 跑完 `DROP TABLE`）。一个都没得跑时退 3（SKIP），不伪装成通过。
 */

#include <uvcpp/uvcpp_define.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if UVCPP_DB_ENABLE

#include <uv.h>

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_job_store.h>
#include <handle/uvcpp_loop.h>
#include <uvcpp/uvcpp_job_queue.h>

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::printf("  [ ok ] %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("  [FAIL] %s\n", what.c_str());
  }
  std::fflush(stdout);
}

bool pump(uvcpp_loop& loop, const std::function<bool()>& done, int ms) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    loop.run(UV_RUN_ONCE);
  }
  for (int i = 0; i < 5; ++i) loop.run(UV_RUN_NOWAIT);
  return done();
}

std::string temp_db() {
  char dir[1024];
  std::size_t n = sizeof(dir);
  std::string base = ".";
  if (uv_os_tmpdir(dir, &n) == 0) base.assign(dir, n);
  char name[64];
  std::snprintf(name, sizeof(name), "/uvcpp_jobs_%ld.db",
                static_cast<long>(uv_os_getpid()));
  return base + name;
}

}  // namespace

namespace {

/// 直接把一个后端上的 save/load/erase 走一遍。
void roundtrip(const std::string& url, const std::string& table,
               const std::string& label) {
  uvcpp_db_job_store store(url, table);
  std::string err;
  check(store.open(&err), label + "：open() 并建表（" + err + "）");
  if (!store.is_open()) return;

  uvcpp_job_record rec;
  rec.id              = 42;
  rec.type            = "demo";
  rec.payload         = "a-b";
  rec.concurrency_key = "res-1";
  rec.error           = "boom";
  rec.state           = static_cast<int>(uvcpp_job_state::PENDING);
  rec.attempt         = 2;
  rec.max_retry       = 3;
  rec.timeout_ms      = 9000;
  rec.backoff_ms      = 250;
  rec.not_before_ms   = 123456789;
  check(store.save(rec), label + "：save 一条");

  std::vector<uvcpp_job_record> rows;
  check(store.load_all(rows), label + "：load_all");
  check(rows.size() == 1, label + "：读回 1 条");
  if (rows.size() == 1) {
    check(rows[0].id == 42 && rows[0].type == "demo" &&
              rows[0].payload == "a-b" &&
              rows[0].concurrency_key == "res-1" && rows[0].error == "boom" &&
              rows[0].attempt == 2 && rows[0].max_retry == 3 &&
              rows[0].timeout_ms == 9000 && rows[0].backoff_ms == 250 &&
              rows[0].not_before_ms == 123456789,
          label + "：字段逐项一致");
  }

  rec.state = static_cast<int>(uvcpp_job_state::SUCCESS);
  check(store.save(rec), label + "：同 id 覆盖保存");
  rows.clear();
  store.load_all(rows);
  check(rows.size() == 1 &&
            rows[0].state == static_cast<int>(uvcpp_job_state::SUCCESS),
        label + "：upsert 而不是重复行");

  check(store.erase(42), label + "：erase");
  rows.clear();
  store.load_all(rows);
  check(rows.empty(), label + "：erase 之后读不到");
}

std::string unique_table() {
  char name[64];
  std::snprintf(name, sizeof(name), "uvcpp_jobs_%ld",
                static_cast<long>(uv_os_getpid()));
  return std::string(name);
}

void drop_table(const std::string& url, const std::string& table) {
  uvcpp_db_client db;
  if (!uvcpp_db_ok(db.open(url))) return;
  db.execute("DROP TABLE " + table);
}

}  // namespace

int main() {
  std::printf("[db_job_store] issue #67（数据库持久化）\n");
  bool ran = false;

#if UVCPP_DB_SQLITE_ENABLE
  ran = true;
  const std::string path = temp_db();
  const std::string url  = "sqlite://" + path;
  std::remove(path.c_str());
  roundtrip(url, "uvcpp_jobs", "sqlite");
#else
  const std::string url;
#endif

  const char* mysql_url = std::getenv("UVCPP_DB_TEST_MYSQL_URL");
  if (mysql_url != nullptr && *mysql_url != '\0') {
    ran = true;
    const std::string t = unique_table();
    roundtrip(mysql_url, t, "mysql");
    drop_table(mysql_url, t);
  }
  const char* pgsql_url = std::getenv("UVCPP_DB_TEST_PGSQL_URL");
  if (pgsql_url != nullptr && *pgsql_url != '\0') {
    ran = true;
    const std::string t = unique_table();
    roundtrip(pgsql_url, t, "pgsql");
    drop_table(pgsql_url, t);
  }

#if !UVCPP_DB_SQLITE_ENABLE
  if (!ran) {
    std::printf("SKIP: 没有 SQLite 后端，也没有 MySQL/PostgreSQL URL\n");
    return 3;
  }
#endif

#if UVCPP_DB_SQLITE_ENABLE
  // ---- 队列跑完之后记录仍在 -------------------------------------------------
  {
    uvcpp_loop loop;
    std::shared_ptr<uvcpp_db_job_store> store(new uvcpp_db_job_store(url));
    std::string err;
    check(store->open(&err), "第二个实例 open 同一个库");

    uvcpp_job_queue queue(&loop, store);
    int runs = 0;
    queue.register_handler("job.success",
                           [&runs](uvcpp_job_context&) { ++runs; });
    queue.start();

    uvcpp_job_submit req;
    req.type       = "job.success";
    req.payload    = "{}";
    req.max_retry  = 1;
    req.timeout_ms = 5000;
    std::string  serr;
    const uint64_t id = queue.submit(req, &serr);
    check(id != 0, "submit（" + serr + "）");
    check(pump(loop, [&] { return queue.stats().success >= 1; }, 8000),
          "作业执行成功");
    check(runs == 1, "handler 跑了一次");
    queue.stop();
    pump(loop, [] { return false; }, 50);
  }

  {
    uvcpp_db_job_store store(url);
    std::string err;
    check(store.open(&err), "第三个实例重开（读完成态）");
    std::vector<uvcpp_job_record> rows;
    check(store.load_all(rows), "load_all");
    bool found = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].type == "job.success" &&
          rows[i].state == static_cast<int>(uvcpp_job_state::SUCCESS)) {
        found = true;
      }
    }
    check(found, "成功记录留在库里（状态 SUCCESS）");
  }

  // ---- 跨实例恢复：PENDING 被新的队列捡起来 ---------------------------------
  {
    {
      uvcpp_db_job_store store(url);
      std::string err;
      check(store.open(&err), "写入恢复用记录之前 open");
      uvcpp_job_record rec;
      rec.id         = 4242;
      rec.type       = "job.recover";
      rec.payload    = "from-db";
      rec.state      = static_cast<int>(uvcpp_job_state::PENDING);
      rec.max_retry  = 1;
      rec.timeout_ms = 5000;
      check(store.save(rec), "写入 PENDING 记录（模拟上次进程留下的任务）");
    }

    uvcpp_loop loop;
    std::shared_ptr<uvcpp_db_job_store> store(new uvcpp_db_job_store(url));
    std::string err;
    check(store->open(&err), "重启后的实例 open");

    uvcpp_job_queue queue(&loop, store);
    int         recovered_runs = 0;
    std::string seen_payload;
    queue.register_handler("job.recover", [&](uvcpp_job_context& ctx) {
      ++recovered_runs;
      seen_payload = ctx.job().payload;
    });
    queue.start();
    check(pump(loop,
               [&] {
                 const uvcpp_job_record* r = queue.find(4242);
                 return r != nullptr &&
                        r->state == static_cast<int>(uvcpp_job_state::SUCCESS);
               },
               8000),
          "重启后的队列把 PENDING 捡起来并执行成功");
    check(recovered_runs == 1, "恢复的作业恰好执行一次");
    check(seen_payload == "from-db", "payload 从数据库原样恢复");
    queue.stop();
    pump(loop, [] { return false; }, 50);
  }

  std::remove(path.c_str());
#endif

  if (g_fail == 0) {
    std::printf("[db_job_store] ALL PASS\n");
    return 0;
  }
  std::printf("[db_job_store] %d check(s) failed\n", g_fail);
  return 1;
}


#else  // !UVCPP_DB_ENABLE

int main() {
  std::printf("SKIP: db 模块没编进来\n");
  return 3;
}

#endif
