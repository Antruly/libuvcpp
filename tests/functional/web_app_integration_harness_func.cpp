/**
 * @file tests/functional/web_app_integration_harness_func.cpp
 * @brief 集成测试 harness（issue #75）：把**跨模块**的那几条真路径串起来跑。
 *
 * 与各模块自己的功能用例是**互补**关系，不是替代：那些钉的是单模块的语义
 * （事务的连接亲和、队列的重试退避、房间的隔离……），这里钉的是"它们放在同一个
 * 进程里、共用一条循环/一个线程池时仍然成立"，并且每一格都带**资源收尾检查**。
 *
 * ## 四格能力
 *
 * | # | 能力 | 判据（失败能定位到哪一格） |
 * |---|---|---|
 * | I1 | 数据库异步事务（**外部依赖，默认跳过**） | 提交/回滚/并发提交；`in_use()` 归零、`tx_stats()` 对账 |
 * | I2 | 作业队列 | 并发键串行、失败重试、取消不执行、队列清空 |
 * | I3 | WebSocket 房间 + 运维端点 | 三连接广播都到达、断线退订、`/health` `/ready` `/metrics` |
 * | I4 | 进程监管 | 超时/取消收尾、树终止不拖住、`running()` 归零 |
 *
 * ## 外部依赖的开关（与 DB 用例同一套变量）
 *
 * - `UVCPP_DB_TEST_MYSQL_URL` / `UVCPP_DB_TEST_PGSQL_URL`：任一存在才跑 I1；
 * - `UVCPP_DB_TEST_REQUIRE=1`：**给了 URL 却不许退 3**（CI 的 db-servers 格子）；
 * - 不给 URL 就打印 `SKIP (没有 URL)`，**不算失败** —— 这是"不把 MySQL/Redis
 *   变成所有测试的强依赖"那条边界的落地。I2/I3/I4 不依赖任何外部服务。
 *
 * ## 一条命令
 *
 * ```
 * ctest -R test_web_app_integration_harness_func --output-on-failure
 * ```
 *
 * 退出码：0 = 已判过的格全过（I1 因环境缺失被跳过**不算失败**，会单独印一行
 * SKIP）；1 = 有格失败。`UVCPP_DB_TEST_REQUIRE=1` 时缺 URL 直接算失败 ——
 * CI 的 db-servers 格子用它关掉"静默跳过"。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_WEB_WS_ENABLE

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_process_supervisor.h>
#include <net/uvcpp_tcp_client.h>
#include <uvcpp/uvcpp_cron.h>
#include <uvcpp/uvcpp_job_queue.h>
#include <uvcpp/uvcpp_metrics_registry.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_ws_client.h>
#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_metrics.h>
#include <webapp/uvcpp_ws_rooms.h>

#if UVCPP_DB_ENABLE
#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_table.h>
#endif

using namespace uvcpp;

namespace {

int g_fail = 0;  ///< 只在 main 里汇总时用（end_cap 里加）

int         cap_fail = 0;
std::string cap_name;

void begin_cap(const char* name) {
  cap_name = name;
  cap_fail = 0;
  std::printf("== %s\n", name);
  std::fflush(stdout);
}

void check(bool ok, const std::string& what) {
  if (ok) {
    std::printf("  [ ok ] %s\n", what.c_str());
  } else {
    ++cap_fail;
    std::printf("  [FAIL] %s\n", what.c_str());
  }
  std::fflush(stdout);
}

/// 结束一格：打印 PASS/FAIL 并把失败数并进总账。
void end_cap() {
  if (cap_fail == 0) {
    std::printf("== %s: PASS\n", cap_name.c_str());
  } else {
    std::printf("== %s: FAIL (%d)\n", cap_name.c_str(), cap_fail);
    g_fail += cap_fail;
  }
  std::fflush(stdout);
}

void skip_cap(const char* why) {
  std::printf("== %s: SKIP (%s)\n", cap_name.c_str(), why);
  std::fflush(stdout);
}

long long ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

bool pump(uvcpp_loop& loop, const std::function<bool()>& done, int ms) {
  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  while (!done()) {
    loop.run(UV_RUN_NOWAIT);
    if (done()) return true;
    if (ms_since(t0) >= ms) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

std::string http_get(int port, const std::string& path, int* status = nullptr) {
  uvcpp_http_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return std::string();
  uvcpp_http_response r;
  if (c.send_wait(uvcpp_http_request::make_get(path), r, 3000) != 0) {
    return std::string();
  }
  if (status != nullptr) *status = static_cast<int>(r.status_code);
  return r.body.to_string();
}

// =========================================================================
// I1：数据库异步事务（外部依赖，默认跳过）
// =========================================================================

void cap_db_transaction() {
  begin_cap("I1 数据库异步事务");
#if !UVCPP_DB_ENABLE
  skip_cap("这一份构建没有编 db 模块");
  return;
#else
  const char* url = std::getenv("UVCPP_DB_TEST_MYSQL_URL");
  if (url == nullptr || *url == '\0') {
    url = std::getenv("UVCPP_DB_TEST_PGSQL_URL");
  }
  // 本机没有 MySQL/PostgreSQL 时用这一条跑同一格（SQLite 也走同一个池子/事务
  // 入口，适合"一条命令跑完"的本地自检）。
  if (url == nullptr || *url == '\0') {
    url = std::getenv("UVCPP_DB_TEST_URL");
  }
  const char* require = std::getenv("UVCPP_DB_TEST_REQUIRE");
  const bool  required = require != nullptr && std::strcmp(require, "1") == 0;
  if (url == nullptr || *url == '\0') {
    skip_cap(required ? "REQUIRE=1 但没有 URL" : "没有 UVCPP_DB_TEST_*_URL");
    if (required) check(false, "外部依赖缺失但 REQUIRE=1");
    end_cap();
    return;
  }

  // SQLite 是单写者：并发事务降到 3 个、池上限 1（仍是同一个 `transaction()`
  // 路径，只是不制造 busy 重试）。MySQL/PostgreSQL 走 6 并发 / 池上限 2。
  const bool sqlite_backend =
      std::strncmp(url, "sqlite:", 7) == 0 || std::strncmp(url, "sqlite3:", 8) == 0;
  const int  pool_max     = sqlite_backend ? 1 : 2;
  const int  concurrent_n = sqlite_backend ? 3 : 6;
  const int  expect_rows  = 2 + concurrent_n;

  uvcpp_loop loop;
  uvcpp_db_pool pool;
  const uvcpp_db_status init_st = pool.init(url, 1, pool_max);
  check(init_st == uvcpp_db_status::OK,
        "pool.init 成功（" + pool.last_error() + "）");
  if (init_st != uvcpp_db_status::OK) {
    end_cap();
    return;
  }

  char tab[80];
  std::snprintf(tab, sizeof(tab), "uvcpp_itest_%ld_%llu",
                static_cast<long>(uv_os_getpid()),
                static_cast<unsigned long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count()));

  const auto exec = [&](const std::string& sql) {
    int64_t           n = 0;
    uvcpp_db_params   p;
    return pool.execute(sql, p, &n);
  };
  const auto count = [&]() -> int64_t {
    uvcpp_db_table    t;
    uvcpp_db_params   p;
    const std::string sql = std::string("SELECT COUNT(*) AS n FROM ") + tab;
    if (pool.query(sql, p, &t) != uvcpp_db_status::OK) return -1;
    return t.value("n").to_int64();
  };

  check(exec(std::string("DROP TABLE IF EXISTS ") + tab) ==
            uvcpp_db_status::OK,
        "清理同名表");
  check(exec(std::string("CREATE TABLE ") + tab +
             " (k VARCHAR(64) PRIMARY KEY, v VARCHAR(64))") ==
            uvcpp_db_status::OK,
        "建表");

  // ---- 成功路径：自动提交 ----
  {
    bool             done = false;
    uvcpp_db_status  st   = uvcpp_db_status::NOT_CONNECTED;
    std::string      msg;
    const int rc = pool.transaction(
        &loop,
        [&](uvcpp_db_tx& tx) {
          tx.execute(std::string("INSERT INTO ") + tab +
                     " (k,v) VALUES ('a','1')");
          tx.execute(std::string("INSERT INTO ") + tab +
                     " (k,v) VALUES ('b','2')");
        },
        [&](uvcpp_db_status s, const std::string& m) {
          st   = s;
          msg  = m;
          done = true;
        });
    check(rc == 0, "事务受理");
    check(pump(loop, [&] { return done; }, 8000), "事务收尾");
    check(st == uvcpp_db_status::OK, "提交成功（" + msg + "）");
    check(count() == 2, "两行落地（实际 " + std::to_string(count()) + "）");
    check(pool.in_use() == 0, "连接已归还（in_use=0）");
  }

  // ---- 失败注入：约束冲突 → 回滚 ----
  {
    bool             done = false;
    uvcpp_db_status  st   = uvcpp_db_status::OK;
    std::string      msg;
    const int rc = pool.transaction(
        &loop,
        [&](uvcpp_db_tx& tx) {
          tx.execute(std::string("INSERT INTO ") + tab +
                     " (k,v) VALUES ('c','3')");
          // 主键冲突：这一句失败之后事务体应当停下，框架回滚。
          tx.execute(std::string("INSERT INTO ") + tab +
                     " (k,v) VALUES ('a','dup')");
        },
        [&](uvcpp_db_status s, const std::string& m) {
          st   = s;
          msg  = m;
          done = true;
        });
    check(rc == 0, "失败注入事务受理");
    check(pump(loop, [&] { return done; }, 8000), "失败事务收尾");
    check(st != uvcpp_db_status::OK, "约束冲突以错误收场");
    check(count() == 2,
          "回滚生效：'c' 没有留下（实际 " + std::to_string(count()) + "）");
    check(pool.in_use() == 0, "回滚后连接已归还");
  }

  // ---- 并发事务：max=2 的池上 6 个全部落地 ----
  {
    int done = 0;
    int ok   = 0;
    for (int i = 0; i < concurrent_n; ++i) {
      const std::string key = std::string("p") + std::to_string(i);
      pool.transaction(
          &loop,
          [&, key](uvcpp_db_tx& tx) {
            tx.execute(std::string("INSERT INTO ") + tab + " (k,v) VALUES ('" +
                       key + "','x')");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          },
          [&](uvcpp_db_status s, const std::string&) {
            if (s == uvcpp_db_status::OK) ++ok;
            ++done;
          });
    }
    check(pump(loop, [&] { return done == concurrent_n; }, 15000),
          std::to_string(concurrent_n) + " 个并发事务收尾");
    check(ok == concurrent_n,
          "全部提交（实际 " + std::to_string(ok) + "）");
    check(count() == expect_rows,
          "表里共 " + std::to_string(expect_rows) + " 行（实际 " +
              std::to_string(count()) + "）");
    check(pool.in_use() == 0, "并发之后连接池归零");
    check(pool.tx_stats().committed >= static_cast<uint64_t>(1 + concurrent_n),
          "tx_stats.committed 对账（" +
              std::to_string(pool.tx_stats().committed) + "）");
  }

  exec(std::string("DROP TABLE ") + tab);
  pool.close();
  check(pool.in_use() == 0, "关闭后 in_use=0");
  end_cap();
#endif  // UVCPP_DB_ENABLE
}

// =========================================================================
// I2：作业队列（并发键 / 重试 / 取消）
// =========================================================================

void cap_job_queue() {
  begin_cap("I2 作业队列");
  uvcpp_loop loop;

  std::shared_ptr<uvcpp_memory_job_store> store(
      new uvcpp_memory_job_store());
  uvcpp_job_queue q(&loop, store);
  q.set_max_concurrent(2);

  std::mutex              mu;
  std::map<std::string, int> active;   ///< 每个并发键当前在跑几个
  int                     max_active = 0;
  std::atomic<int>        runs(0);
  std::atomic<int>        retries_seen(0);
  std::atomic<int>        cancelled_hits(0);

  q.register_handler("itest", [&](uvcpp_job_context& ctx) {
    const std::string key = ctx.job().concurrency_key;
    {
      std::lock_guard<std::mutex> lk(mu);
      const int a = ++active[key];
      if (a > max_active) max_active = a;
    }
    if (ctx.job().payload == "fail-once" && ctx.job().attempt < 2) {
      {
        std::lock_guard<std::mutex> lk(mu);
        --active[key];
      }
      ++retries_seen;
      throw std::runtime_error("injected failure");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(
        ctx.job().payload == "block" ? 100 : 15));
    {
      std::lock_guard<std::mutex> lk(mu);
      --active[key];
      if (ctx.job().payload == "cancel-me") ++cancelled_hits;
    }
    ++runs;
  });

  q.start();
  std::string err;
  int         submitted = 0;

  const auto submit = [&](const char* key, const char* payload,
                          int max_retry) -> uint64_t {
    uvcpp_job_submit s;
    s.type             = "itest";
    s.concurrency_key  = key;
    s.payload          = payload;
    s.max_retry        = max_retry;
    s.backoff_ms       = 10;
    const uint64_t id  = q.submit(s, &err);
    if (id != 0) ++submitted;
    return id;
  };

  for (int i = 0; i < 4; ++i) submit("serial", "ok", 0);
  submit("parallel-a", "ok", 0);
  submit("parallel-b", "ok", 0);
  submit("retry", "fail-once", 1);

  // 取消：**先占满两个执行位**，再在第三条键上排两条 —— 第二条只可能还在
  // PENDING，`cancel()` 因此是确定的"取消掉了"而不是"给在跑的那条置个标志"。
  submit("blocker-a", "block", 0);
  submit("blocker-b", "block", 0);
  const uint64_t c1 = submit("cancel-key", "ok", 0);
  const uint64_t c2 = submit("cancel-key", "cancel-me", 0);
  check(c1 != 0 && c2 != 0, "取消场景的两个作业都提交了");
  if (c2 != 0) {
    check(q.cancel(c2) == 0, "第二个作业取消成功");
  }

  const bool drained =
      pump(loop,
           [&] {
             const uvcpp_job_stats s = q.stats();
             return s.pending == 0 && s.running == 0;
           },
           15000);
  check(drained, "队列排空");
  if (!drained) {
    const uvcpp_job_stats s = q.stats();
    std::printf("  [diag] pending=%zu running=%zu success=%llu failed=%llu "
                "cancelled=%llu retried=%llu\n",
                s.pending, s.running,
                static_cast<unsigned long long>(s.success),
                static_cast<unsigned long long>(s.failed),
                static_cast<unsigned long long>(s.cancelled),
                static_cast<unsigned long long>(s.retried));
  }

  const uvcpp_job_stats st = q.stats();
  check(st.success == 10,
        "成功 10 条（实际 " + std::to_string(st.success) + "）");
  check(st.cancelled == 1, "取消 1 条（实际 " +
                               std::to_string(st.cancelled) + "）");
  check(st.retried >= 1, "重试计数涨（实际 " +
                             std::to_string(st.retried) + "）");
  check(retries_seen.load() == 1, "失败注入确实触发了一次重试");
  check(runs.load() == 10, "handler 跑了 10 次（取消那条没跑）");
  check(cancelled_hits.load() == 0, "被取消的作业一次都没执行");
  check(max_active == 1, "同一并发键从不同时执行（实际最大 " +
                             std::to_string(max_active) + "）");

  q.stop();
  q.clear_finished();
  check(q.stats().success == 0 && q.stats().pending == 0,
        "stop + clear_finished 之后账本归零");
  end_cap();
}

// =========================================================================
// I3：WebSocket 房间 + 运维端点（真端口、真客户端）
// =========================================================================

void cap_ws_and_metrics() {
  begin_cap("I3 WebSocket 房间 + /health /ready /metrics");

  uvcpp_ws_rooms            rooms;
  uvcpp_metrics_registry    reg;
  uvcpp_web_metrics         metrics(reg);
  uvcpp_web_app             app;

  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);

  app.websocket("/chat/:room", [&rooms](uvcpp_web_ws_request& ws_req) {
    const std::string* room = ws_req.param("room");
    uvcpp_ws_connection* c  = ws_req.connection();
    if (room == nullptr || c == nullptr) return;
    rooms.join(c, *room);
    const std::string r = *room;
    c->on_text([&rooms, r](const std::string& m) {
      rooms.broadcast_text(r, m.data(), m.size());
    });
  });

  app.get("/room_size/:room",
          [&rooms](uvcpp_web_request& req, uvcpp_web_response& resp,
                   uvcpp_web_next) {
            const std::string* room = req.param("room");
            resp.text(std::to_string(room ? rooms.room_size(*room) : 0));
            resp.end();
          });

  metrics.install(app);
  metrics.set_ready_check([](std::string&) { return true; });

  if (app.start_background() != 0) {
    check(false, "app.start_background");
    end_cap();
    return;
  }
  const int port = app.bound_port();
  check(port > 0, "真实端口起来了");

  // 三个协议客户端；每个自己拥有一条循环，测试线程轮流拨。
  std::vector<std::shared_ptr<uvcpp_ws_client> > clients;
  std::vector<int>                             got;
  for (int i = 0; i < 3; ++i) {
    std::shared_ptr<uvcpp_ws_client> c(new uvcpp_ws_client());
    c->on_text([&got, i](const std::string& m) {
      if (m == "sync-hello") got[i] += 1;
    });
    const std::string url =
        "ws://127.0.0.1:" + std::to_string(port) + "/chat/room-it";
    uvcpp_ws_connection* conn = nullptr;
    check(c->connect_wait(url, conn, 5000) == 0 && conn != nullptr,
          "客户端 " + std::to_string(i) + " 握手成功");
    clients.push_back(c);
    got.push_back(0);
  }

  // 三个都进房间（服务端按连接记；HTTP 读的是同一条循环上的表）。
  {
    const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    std::string size;
    while (ms_since(t0) < 4000) {
      size = http_get(port, "/room_size/room-it");
      if (size == "3") break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(size == "3", "房间里有 3 个成员（实际 '" + size + "'）");
  }

  check(clients[0]->send_text("sync-hello", 10) == 0, "客户端 0 发出消息");
  {
    const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    while (ms_since(t0) < 5000 && (got[0] == 0 || got[1] == 0 || got[2] == 0)) {
      for (size_t i = 0; i < clients.size(); ++i) clients[i]->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  check(got[0] > 0 && got[1] > 0 && got[2] > 0,
        "三条连接都收到了广播（" + std::to_string(got[0]) + "/" +
            std::to_string(got[1]) + "/" + std::to_string(got[2]) + "）");

  // ---- 运维端点 -------------------------------------------------------
  {
    int health = 0;
    const std::string h = http_get(port, "/health", &health);
    check(health == 200, "/health 200（实际 " + std::to_string(health) + "）");
    check(!h.empty(), "/health 有正文");

    int ready = 0;
    http_get(port, "/ready", &ready);
    check(ready == 200, "/ready 200（实际 " + std::to_string(ready) + "）");

    const std::string m = http_get(port, "/metrics");
    check(m.find("http_requests_total") != std::string::npos,
          "/metrics 含请求计数");
    check(m.find("http_requests_in_flight") != std::string::npos,
          "/metrics 含在途 Gauge");
    check(m.find("http_request_duration_ms") != std::string::npos,
          "/metrics 含耗时直方图");
  }

  // ---- 断线自动退订 ---------------------------------------------------
  for (size_t i = 0; i < clients.size(); ++i) clients[i]->close();
  {
    const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    std::string size = "?";
    while (ms_since(t0) < 5000) {
      size = http_get(port, "/room_size/room-it");
      if (size == "0") break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(size == "0", "断线后房间被摘掉（实际 '" + size + "'）");
  }
  for (size_t i = 0; i < clients.size(); ++i) {
    for (int k = 0; k < 20; ++k) clients[i]->run(UV_RUN_NOWAIT);
  }
  clients.clear();

  app.stop();
  app.join();
  check(app.connection_count() == 0,
        "停机后连接表归零（实际 " +
            std::to_string(app.connection_count()) + "）");
  check(rooms.room_count() == 0,
        "停机后房间表归零（实际 " + std::to_string(rooms.room_count()) + "）");
  end_cap();
}

// =========================================================================
// I4：进程监管（超时 + 取消 + 有界输出）
// =========================================================================

void cap_process_supervisor() {
  begin_cap("I4 进程监管");
  uvcpp_loop loop;
  {
    uvcpp_process_supervisor sup(&loop);

    uvcpp_process_spec spec;
#ifdef _WIN32
    spec.file = "powershell.exe";
    spec.args = std::vector<std::string>{"-NoProfile", "-NonInteractive",
                                         "-Command",
                                         "Start-Sleep -Seconds 30"};
#else
    spec.file = "/bin/sh";
    spec.args = std::vector<std::string>{"-c", "sleep 30"};
#endif
    spec.timeout_ms    = 300;
    spec.stop_grace_ms = 200;
    spec.kill_tree     = true;

    bool                done = false;
    uvcpp_process_result result;
    const uint64_t      id = sup.spawn(
        spec, nullptr, [&](const uvcpp_process_result& r) {
          result = r;
          done   = true;
        });
    check(id != 0, "spawn 受理");
    check(pump(loop, [&] { return done; }, 8000), "超时后收尾");
    check(result.timed_out, "timed_out 标记");
    check(result.elapsed_ms < 6000,
          "没有被 30s 的睡眠拖住（" + std::to_string(result.elapsed_ms) +
              " ms）");
    check(sup.running() == 0, "running() 归零");
    check(sup.stats().timed_out == 1, "stats.timed_out = 1");

    // 取消：另一条立刻取消的进程。
    bool                done2 = false;
    uvcpp_process_result result2;
    spec.timeout_ms = 0;
    const uint64_t id2 = sup.spawn(
        spec, nullptr, [&](const uvcpp_process_result& r) {
          result2 = r;
          done2   = true;
        });
    check(id2 != 0, "第二个 spawn 受理");
    check(sup.cancel(id2) == 0, "cancel 成功");
    check(pump(loop, [&] { return done2; }, 8000), "取消后收尾");
    check(result2.cancelled, "cancelled 标记");
    check(sup.running() == 0, "两次之后 running() 仍归零");
  }
  end_cap();
}

}  // namespace

int main() {
  std::printf("[integration] issue #75\n");
  std::fflush(stdout);

  cap_db_transaction();
  cap_job_queue();
  cap_ws_and_metrics();
  cap_process_supervisor();

  std::printf("----\n");
  if (g_fail == 0) {
    const bool db_env = std::getenv("UVCPP_DB_TEST_MYSQL_URL") != nullptr ||
                        std::getenv("UVCPP_DB_TEST_PGSQL_URL") != nullptr ||
                        std::getenv("UVCPP_DB_TEST_URL") != nullptr;
    std::printf("[integration] ALL PASS%s\n",
                db_env ? "" : "（I1 因没有 DB URL 跳过）");
    return 0;
  }
  std::printf("[integration] %d check(s) failed\n", g_fail);
  return 1;
}

#else  // !(webapp && ws)

int main() {
  std::printf("SKIP: webapp / ws disabled\n");
  return 0;
}

#endif
