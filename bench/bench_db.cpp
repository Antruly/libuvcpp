/**
 * @file bench/bench_db.cpp
 * @brief 数据库模块的量具：单次调用的成本，以及"同步调用把事件循环卡多久"。
 * @author zhuweiye
 *
 * 为什么要有它
 * ----------
 * `src/db/uvcpp_db.h` 把这条写成了契约：本模块**是同步的，且故意是同步的**
 * （libmysqlclient / libpq / sqlite3 都没有异步 API），异步封装要建在它**上面**
 * （`uv_queue_work` 那一层），而不是把它包在锁里再从别的线程碰事件循环。
 *
 * 那句话有两个可测的后半段，此前一个数都没有：
 *
 *   1. **一次调用多贵** —— 连接、ping、点查、插入、事务、大结果集。异步封装的
 *      每次派发都要付这笔钱，池子开多大也取决于它。
 *   2. **把这条同步调用放在 loop 线程上，循环被卡多久** —— 这才是"要不要异步"
 *      的判据本身，而不是"异步听起来高级"。这里直接量：身上挂着一条 1 ms 的
 *      定时器，同期在 loop 线程上跑 N 条慢查询，记**相邻两次到点的最大间隔**与
 *      **被合并掉的点数**；对照臂把同一批查询交给 `uvcpp_work`（`uv_queue_work`）
 *      扔进线程池，同一个定时器应当回到噪声级。第三条臂把同一批活交给
 *      `uvcpp_db_async`（本库替开发者写掉的那层门面）—— 它和手写 `uvcpp_work`
 *      那条必须落在同一量级，否则门面就是把异步偷换回了同步。三条臂用**同一个
 *      client、同一条连接**，差别只在"在哪个线程上阻塞"。
 *   3. **同一批活怎么分到多条连接上**（第 10 节）—— `uvcpp_db_client` 里那把递归
 *      锁的价钱，以及连接池买到了多少。三条臂：共享 1 个 client / 各开 1 个 /
 *      池子。池子那条是**复用**的证据：`created_total()` 停在 max 上。
 *
 * 怎么读
 * ------
 * 每个用例报 min / mean / p50 / p99 与吞吐。**除了 min 也报 mean**：与
 * `bench_router.cpp` 不同（那里量纯 CPU，min 对抢占免疫），这里的每一步都要穿过
 * 客户端库走进本机回环，抖动是这条路径的一部分，只报 min 会把它藏起来。
 *
 * 表格里的用例名是 ASCII 的，和 `bench_router.cpp` 一个理由：中文是双宽字符，
 * `printf` 按字节数补空格，中文名字会把整张表挤歪。中文解释放在每节标题下面。
 *
 * 这边量不到什么（别把下面的数当线上延迟）
 * --------------------------------------
 *   * 回环上的 MySQL / PostgreSQL **没有网络延迟**。真实部署里一次往返的那部分
 *     （同机房 0.2~0.5 ms，跨机房几毫秒到几十毫秒）这里一条都没有。所以下面的
 *     数字是「客户端库 + 本库封装 + 回环」的成本下界，不是"线上一次查询多久"。
 *   * 倒过来，卡顿那条结论不受影响：循环被卡的时长**等于查询自身的时长**，
 *     网络越慢卡得越久 —— 量不到网络只是让这里的卡顿数偏**乐观**。
 *   * 没有量磁盘冷热、没有量连接数上限、没有量真实数据集上的查询计划。
 *   * 第 9 节的 SQLite 那条慢查询是**本机烧 CPU**（递归 CTE），不是服务端等待；
 *     所以它的异步臂里循环线程会和 worker 抢 CPU，间隔不可能像 MySQL / PG
 *     那两条（服务端睡觉、本机全空）一样干净。这是被测对象的性质，不是量具的毛病。
 *
 * 用法
 * ----
 *     uvcpp_bench_db <url> [--label=名字] [--rows=N]
 *     UVCPP_DB_BENCH_URL='mysql://root@127.0.0.1:33099/uvcpp_test' uvcpp_bench_db
 *
 * 与 `tests/functional/db_*_func.cpp` 共用同一批环境变量口径
 * （`UVCPP_DB_TEST_MYSQL_URL` / `_PG_URL` / `_SQLITE_URL`），现成的服务端直接可用。
 * 退出码：0 跑完；2 用法错 / 连不上。
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_async.h>
#include <db/uvcpp_db_pool.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <req/uvcpp_work.h>

using namespace uvcpp;

namespace {

using steady = std::chrono::steady_clock;

double now_us() {
  return std::chrono::duration<double, std::micro>(steady::now().time_since_epoch())
      .count();
}

/// 一次调用的耗时样本汇总（微秒）。`rows_per_op` > 0 时多报一列"行/秒"。
void report(const char* name, std::vector<double> us, double rows_per_op = 0) {
  if (us.empty()) {
    std::printf("  %-26s %7d  (no samples)\n", name, 0);
    return;
  }
  std::sort(us.begin(), us.end());
  double sum = 0;
  for (double v : us) sum += v;
  const double mean = sum / static_cast<double>(us.size());
  const double p50 = us[us.size() * 50 / 100];
  const double p99 = us[us.size() * 99 / 100];
  std::printf("  %-26s %7zu %10.1f %10.1f %10.1f %10.1f %12.0f", name, us.size(),
              us.front(), mean, p50, p99, 1e6 / mean);
  if (rows_per_op > 0) {
    std::printf(" %14.0f", 1e6 / mean * rows_per_op);
  }
  std::printf("\n");
}

/// 整批操作的耗时（毫秒 / 行每秒）。用在"一次做很多行"的两节上 —— 那里把每行
/// 拆成样本只会掩盖真正想知道的那件事（整批多快）。
void report_bulk(const char* name, double us, double rows) {
  std::printf("  %-26s %22.1f ms %14.0f rows/s  (%.0f rows)\n", name, us / 1000.0,
              rows * 1e6 / us, rows);
}

void print_header() {
  std::printf("  %-26s %7s %10s %10s %10s %10s %12s %14s\n", "case", "n", "min(us)",
              "mean(us)", "p50(us)", "p99(us)", "calls/s", "rows/s");
}

enum class kind { sqlite, mysql, postgres };

kind kind_of(const char* driver) {
  const std::string d = driver ? driver : "";
  if (d == "mysql") return kind::mysql;
  if (d == "postgres") return kind::postgres;
  return kind::sqlite;
}

/// 三家的建表语句。列集刻意一样（k / s / d），只有自增主键的写法不同 —— 与
/// `tests/functional/db_suite.h` 的 `dialect` 同一个理由：差异收敛到一处，
/// 否则量出来的不是同一个东西。
std::string ddl_create(kind k) {
  switch (k) {
    case kind::mysql:
      return "CREATE TABLE uvcpp_bench_rows (id BIGINT PRIMARY KEY AUTO_INCREMENT, "
             "k INT NOT NULL, s VARCHAR(64) NOT NULL, d DOUBLE NOT NULL)";
    case kind::postgres:
      return "CREATE TABLE uvcpp_bench_rows (id BIGSERIAL PRIMARY KEY, "
             "k INT NOT NULL, s VARCHAR(64) NOT NULL, d DOUBLE PRECISION NOT NULL)";
    case kind::sqlite:
      return "CREATE TABLE uvcpp_bench_rows (id INTEGER PRIMARY KEY AUTOINCREMENT, "
             "k INTEGER NOT NULL, s TEXT NOT NULL, d REAL NOT NULL)";
  }
  return "";
}

/// 占位符的写法**是方言差异**：MySQL / SQLite 是 `?`，PostgreSQL 是 `$1..$n`。
/// 驱动**不替调用方换算** —— 这正是 `tests/functional/db_suite.h` 里那个
/// `dialect::ph()` 存在的理由。这里照抄同一条规矩；第 1 次拿 `?` 打 PG 时它报的
/// 是 `syntax error at end of input`。
///
/// （这一处早先与 `src/db/uvcpp_db_params.h`、`doc/db-guide.md` §5 里那句"PG 侧
/// 本模块自己转成 `$1..$n`"对不上 —— 是这支量具撞出来的。**已经收敛**：那两处
/// 改成跟实现一致，`db_suite.h` 也加了 `test_placeholder_dialect()` 钉住
/// "错的写法必须红"。另注：SQLite 把 `$1` 当**命名**参数收下，所以**光测 SQLite
/// 看不到这个差异** —— 这也是为什么量具要在三个后端上各跑一遍。）
std::string ph(kind k, int n) {
  return k == kind::postgres ? "$" + std::to_string(n) : std::string("?");
}

/// 三点插入用的 SQL（`k, s, d`）。
std::string insert_sql(kind k) {
  return "INSERT INTO uvcpp_bench_rows (k, s, d) VALUES (" + ph(k, 1) + ", " +
         ph(k, 2) + ", " + ph(k, 3) + ")";
}

/// 往表里灌 `n` 行（一条事务）。返回耗时微秒；失败返回 -1。
double seed_rows(uvcpp_db_client* db, kind k, int64_t n) {
  const std::string sql = insert_sql(k);
  const double t0 = now_us();
  if (!uvcpp_db_ok(db->begin())) return -1;
  for (int64_t i = 0; i < n; ++i) {
    uvcpp_db_value id;
    const uvcpp_db_status st =
        db->insert(sql, {i, std::string("row ") + std::to_string(i),
                         static_cast<double>(i) * 1.5},
                   &id);
    if (!uvcpp_db_ok(st)) {
      db->rollback();
      std::fprintf(stderr, "  seed failed: %s\n", db->last_error().c_str());
      return -1;
    }
  }
  if (!uvcpp_db_ok(db->commit())) return -1;
  return now_us() - t0;
}

/// "一条慢查询"。要的是**服务端真的在忙**，不是本机睡一觉：MySQL / PG 有现成的
/// 函数，SQLite 没有，用一个递归 CTE 烧 CPU（界由 `calibrate_slow()` 标定到与
/// 另外两家同一个量级，否则三条腿的卡顿数不可比）。
std::string slow_sql(kind k, long bound) {
  switch (k) {
    case kind::mysql:
      return "SELECT SLEEP(0.05)";
    case kind::postgres:
      return "SELECT pg_sleep(0.05)";
    case kind::sqlite:
      return "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < " +
             std::to_string(bound) + ") SELECT count(*) FROM c";
  }
  return "";
}

/// 标定慢查询，返回 SQL；`*out_us` 写实测耗时（<=0 表示标定失败）。
/// MySQL / PG 是现成的 50 ms 睡眠，不用标；SQLite 那条 CTE 起步猜 20 万次，
/// 不够 40 ms 就翻四倍重来。
std::string calibrate_slow(uvcpp_db_client* db, kind k, double* out_us) {
  long bound = 200000;
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::string sql = slow_sql(k, bound);
    uvcpp_db_table t;
    const double t0 = now_us();
    const uvcpp_db_status st = db->query(sql, &t);
    const double dt = now_us() - t0;
    if (!uvcpp_db_ok(st)) {
      std::fprintf(stderr, "  calibrate failed: %s\n", db->last_error().c_str());
      *out_us = 0;
      return sql;
    }
    if (k != kind::sqlite || dt >= 40000.0) {
      *out_us = dt;
      return sql;
    }
    bound *= 4;
  }
  *out_us = -1;
  return slow_sql(k, bound);
}

struct stall_result {
  double max_gap_us = 0;   ///< 相邻两次到点的最大间隔（理想值 1000 us）
  size_t ticks = 0;        ///< 实际到点次数
  size_t expected = 0;     ///< 这段时间本该到点的次数
  double wall_us = 0;      ///< 从 t0 到循环停
  double batch_us = 0;     ///< 那批慢查询自己花了多久
  bool timed_out = false;  ///< 兜底看门狗咬过
};

/// 三条臂：同一批慢查询、同一个 client、同一条连接，差别只在**在哪个线程上
/// 阻塞**（以及异步那两条由谁把活交给线程池）。
enum arm_kind {
  ARM_SYNC,    ///< 直接在定时器回调里跑 —— loop 线程上阻塞
  ARM_WORK,    ///< 手写 `uvcpp_work`（`uv_queue_work`）派到线程池
  ARM_FACADE,  ///< `uvcpp_db_async`，也就是"本库替你把上面那段写掉"的那一层
};

/// 卡顿臂。三条臂都拿同一个 client、同一条连接。ARM_FACADE 与 ARM_WORK 该
/// 给出**同量级**的间隔 —— 门面要是偷偷把那批查询挪回同步执行，这一臂会当场
/// 顶到几百毫秒，和 ARM_SYNC 一样。
///
/// 判据用**相邻到点的间隔**而不是"迟到"：libuv 会把错过的重复定时器合并成一次
/// 回调，合并之后按"第几次到点"去算本该的时刻会一路错下去（迟到量能算出比整段
/// 时长还大的数）。间隔不需要基准 —— 理想值是 1 ms，被卡住的那一次会直接顶到
/// 卡顿时长。
stall_result run_stall_arm(uvcpp_db_client* db, const std::string& sql, int reps,
                           arm_kind kind) {
  const bool async_arm = (kind != ARM_SYNC);
  stall_result r;
  uvcpp_loop loop;
  loop.init();
  // 门面必须活得比循环久（回调跑在循环线程上、句柄投在那条循环上）。声明在
  // `loop` 之后、`beat`/`guard` 之前：析构逆序正好是「定时器 → 门面 → 循环」。
  std::unique_ptr<uvcpp_db_async> facade;

  const steady::time_point t0 = steady::now();
  const auto elapsed_us = [&t0]() {
    return std::chrono::duration<double, std::micro>(steady::now() - t0).count();
  };

  size_t ticks = 0;
  double last_tick = 0;
  bool fired = false;
  int after = 0;          // 同步臂：那批查询之后又数了多少个点
  int remaining = reps;   // 异步臂：还没回来的 work 条数
  int queued = 0;
  double batch0 = 0;

  uvcpp_timer beat(&loop);
  beat.start(
      [&](uvcpp_timer*) {
        const double t = elapsed_us();
        if (last_tick > 0) r.max_gap_us = std::max(r.max_gap_us, t - last_tick);
        last_tick = t;
        ++ticks;

        if (!fired) {
          // 先让定时器跑一个点再动数据库 —— 有基准，间隔才可解释。
          fired = true;
          batch0 = now_us();
          if (kind == ARM_WORK) {
            for (int i = 0; i < reps; ++i) {
              uvcpp_work* w = new uvcpp_work();
              w->init();
              const int rc = w->queue_work(
                  &loop,
                  [db, sql](uvcpp_work*) {
                    // 工作线程：这里阻塞是对的，它不碰事件循环。
                    uvcpp_db_table t;
                    db->query(sql, &t);
                  },
                  [&](uvcpp_work* self, int) {
                    delete self;
                    if (--remaining == 0) {
                      r.batch_us = now_us() - batch0;
                      loop.stop();
                    }
                  });
              if (rc != 0) {
                delete w;
                if (--remaining == 0) loop.stop();
              } else {
                ++queued;
              }
            }
            if (queued == 0) loop.stop();
          } else if (kind == ARM_FACADE) {
            // 门面：同一批活、同一条连接，只是派发 / 搬结果 / 回收 work 三件事
            // 由 `uvcpp_db_async` 包掉。这一臂的意义就是**和上一臂对照** ——
            // 间隔若顶到几百毫秒，说明门面把活偷偷挪回了同步执行。
            facade.reset(new uvcpp_db_async(db));
            for (int i = 0; i < reps; ++i) {
              const int rc = facade->query(
                  &loop, sql,
                  [&](uvcpp_db_status, const uvcpp_db_table&) {
                    if (--remaining == 0) {
                      r.batch_us = now_us() - batch0;
                      loop.stop();
                    }
                  });
              if (rc != 0) {
                if (--remaining == 0) loop.stop();
              } else {
                ++queued;
              }
            }
            if (queued == 0) loop.stop();
          } else {
            for (int i = 0; i < reps; ++i) {
              uvcpp_db_table t;
              db->query(sql, &t);
            }
            r.batch_us = now_us() - batch0;
          }
        } else if (!async_arm && ++after >= 50) {
          // 同步臂：卡顿过去之后再数 50 个点，看循环有没有回到正轨。
          loop.stop();
        }
      },
      1, 1);

  // 兜底看门狗：异步臂若有一条 work 永远回不来，别把量具挂死在这儿。
  const double budget_us = static_cast<double>(reps) * 200000.0 + 5e6;
  uvcpp_timer guard(&loop);
  guard.start(
      [&](uvcpp_timer*) {
        if (elapsed_us() > budget_us) {
          r.timed_out = true;
          loop.stop();
        }
      },
      200, 200);

  loop.run(UV_RUN_DEFAULT);

  r.wall_us = elapsed_us();
  r.ticks = ticks;
  r.expected = static_cast<size_t>(r.wall_us / 1000.0);
  return r;
}

void print_stall(const char* arm, const stall_result& r) {
  std::printf("  %-20s max gap %9.0f us   ticks %4zu / expected %4zu (coalesced %4zu)"
              "   batch %7.0f ms%s\n",
              arm, r.max_gap_us, r.ticks, r.expected,
              r.expected > r.ticks ? r.expected - r.ticks : 0, r.batch_us / 1000.0,
              r.timed_out ? "  [watchdog fired]" : "");
}

/// 一节小标题。
void section(const char* title) { std::printf("\n%s\n", title); }

/// 参数解析：位置参数是 URL，`--label=` / `--rows=`。返回 false 表示用法错。
bool parse_args(int argc, char** argv, std::string* url, std::string* label,
                int64_t* rows) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--label=", 0) == 0) {
      *label = a.substr(8);
    } else if (a.rfind("--rows=", 0) == 0) {
      *rows = std::strtoll(a.c_str() + 7, nullptr, 10);
      if (*rows < 100) return false;
    } else if (a.rfind("--", 0) == 0) {
      return false;
    } else if (url->empty()) {
      *url = a;
    } else {
      return false;
    }
  }
  if (url->empty()) {
    const char* e = std::getenv("UVCPP_DB_BENCH_URL");
    if (e != nullptr) *url = e;
  }
  return !url->empty();
}

}  // namespace

int main(int argc, char** argv) {
  std::string url;
  std::string label;
  int64_t rows = 10000;
  if (!parse_args(argc, argv, &url, &label, &rows)) {
    std::fprintf(stderr,
                 "用法：uvcpp_bench_db <url> [--label=名字] [--rows=N]\n"
                 "  或：UVCPP_DB_BENCH_URL='sqlite:///tmp/bench.sqlite' uvcpp_bench_db\n"
                 "  例：mysql://root@127.0.0.1:33099/uvcpp_test\n"
                 "      postgresql://uvcpp@127.0.0.1:55432/uvcpp_test\n");
    return 2;
  }

  uvcpp_db_client db;
  if (!uvcpp_db_ok(db.open(url))) {
    std::fprintf(stderr, "打开失败：%s\n  本构建可用后端：%s\n", db.last_error().c_str(),
                 uvcpp_db_drivers().c_str());
    return 2;
  }
  const kind k = kind_of(db.driver_name());
  if (label.empty()) label = db.driver_name();

  std::printf("uvcpp 数据库模块量具 —— %s\n", label.c_str());
  std::printf("  URL     %s\n", url.c_str());
  std::printf("  驱动    %s（本构建可用：%s）\n", db.driver_name(),
              uvcpp_db_drivers().c_str());
  std::printf("  行数    %lld\n", static_cast<long long>(rows));
  std::printf("  注意    回环上没有网络延迟，下面的数是「客户端库 + 本库封装 + "
              "回环」的下界\n");

  // ---- 建表 + 灌数据（不计入下面的任何一节）----
  db.execute("DROP TABLE IF EXISTS uvcpp_bench_rows");
  if (!uvcpp_db_ok(db.execute(ddl_create(k)))) {
    std::fprintf(stderr, "建表失败：%s\n", db.last_error().c_str());
    return 2;
  }
  const double seed_us = seed_rows(&db, k, rows);
  if (seed_us < 0) return 2;
  std::printf("  灌 %lld 行  %.1f ms（%.0f 行/秒；一条事务，不计入下面的比较）\n",
              static_cast<long long>(rows), seed_us / 1000.0,
              static_cast<double>(rows) * 1e6 / seed_us);

  print_header();

  // ---- 1. 连接：开一个 client 多贵 ----
  {
    section("1. 连接 —— 一开一关算一笔（200 笔）");
    std::vector<double> us;
    for (int i = 0; i < 200; ++i) {
      uvcpp_db_client c;
      const double t0 = now_us();
      if (!uvcpp_db_ok(c.open(url))) {
        std::fprintf(stderr, "  第 %d 次 open 失败：%s\n", i, c.last_error().c_str());
        break;
      }
      c.close();
      us.push_back(now_us() - t0);
    }
    report("open+close", std::move(us));
  }

  // ---- 2. ping：最轻的一次服务端往返 ----
  {
    section("2. ping —— 最轻的一次往返（2000 次）");
    std::vector<double> us;
    for (int i = 0; i < 2000; ++i) {
      const double t0 = now_us();
      db.ping();
      us.push_back(now_us() - t0);
    }
    report("ping", std::move(us));
  }

  // ---- 3. 无参数查询：走到的是"文本协议"那条路 ----
  {
    section("3. 无参数查询 —— MySQL 这条走的是 mysql_query 文本协议（2000 次）");
    std::vector<double> us;
    uvcpp_db_table t;
    for (int i = 0; i < 2000; ++i) {
      const double t0 = now_us();
      db.query("SELECT 1", &t);
      us.push_back(now_us() - t0);
    }
    report("select 1", std::move(us));
  }

  // ---- 4/5. 点查的两种写法：值拼进 SQL vs 参数化 ----
  // 这一对是本次量具最想给出的对比：本模块**没有语句缓存**，参数化那条路每次
  // 调用都要把语句重新准备一遍（MySQL 是 mysql_stmt_init + prepare + close，
  // PG 是 PQexecParams，SQLite 是 prepare + finalize），这笔开销此前没有数。
  {
    section("4. 点查（无参数路径，值拼进 SQL；2000 次）");
    std::vector<double> us;
    for (int i = 0; i < 2000; ++i) {
      const int64_t id = 1 + (i * 7919) % rows;
      uvcpp_db_table t;
      const double t0 = now_us();
      db.query("SELECT k, s, d FROM uvcpp_bench_rows WHERE id = " + std::to_string(id),
               &t);
      us.push_back(now_us() - t0);
    }
    report("point select (inlined)", std::move(us));
  }
  {
    section("5. 点查（参数化；2000 次）—— 与上一节之差 = 每次重新准备语句的钱");
    const std::string psql =
        "SELECT k, s, d FROM uvcpp_bench_rows WHERE id = " + ph(k, 1);
    std::vector<double> us;
    for (int i = 0; i < 2000; ++i) {
      const int64_t id = 1 + (i * 7919) % rows;
      uvcpp_db_table t;
      const double t0 = now_us();
      db.query(psql, {id}, &t);
      us.push_back(now_us() - t0);
    }
    report("point select (param)", std::move(us));
  }

  // ---- 6. 单行 INSERT，每条自己一个事务 ----
  {
    section("6. 单行 INSERT —— 每条自动提交（1000 次）");
    const std::string isql = insert_sql(k);
    std::vector<double> us;
    for (int i = 0; i < 1000; ++i) {
      uvcpp_db_value id;
      const double t0 = now_us();
      db.insert(isql, {i, std::string("ins"), 1.0}, &id);
      us.push_back(now_us() - t0);
    }
    report("insert autocommit", std::move(us));
  }

  // ---- 7. 同一个事务里 1000 行 ----
  {
    section("7. 事务内 INSERT —— 1000 行 = 一次提交（3 轮）");
    const std::string isql = insert_sql(k);
    for (int round = 0; round < 3; ++round) {
      const double t0 = now_us();
      db.begin();
      for (int i = 0; i < 1000; ++i) {
        uvcpp_db_value id;
        db.insert(isql, {i, std::string("txn"), 2.0}, &id);
      }
      if (!uvcpp_db_ok(db.commit())) {
        std::fprintf(stderr, "  commit 失败：%s\n", db.last_error().c_str());
        break;
      }
      report_bulk("insert in txn", now_us() - t0, 1000);
    }
  }

  // ---- 8. 取回一整批行 ----
  {
    section("8. 结果集取回 —— 1 万行 × 3 列（3 轮）");
    for (int round = 0; round < 3; ++round) {
      uvcpp_db_table t;
      const double t0 = now_us();
      if (!uvcpp_db_ok(db.query("SELECT k, s, d FROM uvcpp_bench_rows LIMIT 10000", &t))) {
        std::fprintf(stderr, "  取回失败：%s\n", db.last_error().c_str());
        break;
      }
      report_bulk("fetch 10k rows", now_us() - t0,
                  static_cast<double>(t.row_count()));
    }
  }

  // ---- 9. 卡顿：同步挂在 loop 线程上 vs 手写 work vs 门面 ----
  {
    double slow_us = 0;
    const std::string slow = calibrate_slow(&db, k, &slow_us);
    section("9. 事件循环卡顿 —— 1 ms 定时器 + 10 条慢查询，同一个 client");
    if (slow_us <= 0) {
      std::printf("  标定失败，跳过（%s）\n", db.last_error().c_str());
    } else {
      std::printf("  慢查询单条 %.1f ms\n", slow_us / 1000.0);
      const stall_result sync = run_stall_arm(&db, slow, 10, ARM_SYNC);
      print_stall("sync (on loop thread)", sync);
      const stall_result work = run_stall_arm(&db, slow, 10, ARM_WORK);
      print_stall("async (via vcpp_work)", work);
      const stall_result fac = run_stall_arm(&db, slow, 10, ARM_FACADE);
      print_stall("async (via db_async)", fac);
      std::printf(
          "  读法  max gap 是这条 1 ms 定时器相邻两次到点的最大间隔（理想 1000 us）。\n"
          "        同步臂在那批查询期间循环整个停住，于是 max gap 直接顶到那批查询的\n"
          "        时长、错过的点被 libuv 合并成一个（ticks 塌到几十）；异步臂的循环\n"
          "        在等数据库时照常转，ticks 应当是满的。两臂的 batch 耗时是同一个\n"
          "        量级 —— 异步不是把查询变快，是不让别的事跟着一起等。\n"
          "        后两条臂的**唯一**差别是「活由谁交给线程池」：手写 `uvcpp_work`\n"
          "        对比 `uvcpp_db_async` 的一次调用。它们的 max gap 必须同量级 ——\n"
          "        门面只是把那段样板收了起来，要是它偷偷改回同步执行，这一臂会当场\n"
          "        顶到和同步臂一样大。\n");
      if (work.max_gap_us > 0 && fac.max_gap_us > 0) {
        const double ratio = fac.max_gap_us / work.max_gap_us;
        std::printf("  门面 / 手写 work 的 max gap 之比：%.2f×（判据：同量级，约 1×）\n",
                    ratio);
      }
    }
  }

  // ---- 10. 并发：一把锁 vs 多条连接 ----
  // `uvcpp_db_client` 内部是一把递归锁：同一个 client 多线程用是安全的，但同一
  // 时刻只有一个在真的用连接。这一节量的是那句话的实际价格。
  {
    section("10. 并发 —— 4 线程 × 500 次点查：共享 1 个 client / 各开 1 个 / 池子");
    const int threads = 4;
    const int per_thread = 500;
    const int64_t nrows = rows;

    const std::string psql =
        "SELECT k FROM uvcpp_bench_rows WHERE id = " + ph(k, 1);
    auto point_select_loop = [per_thread, nrows, &psql](uvcpp_db_client* c) {
      for (int i = 0; i < per_thread; ++i) {
        const int64_t id = 1 + (i * 7919) % nrows;
        uvcpp_db_table t;
        c->query(psql, {id}, &t);
      }
    };

    // 三条臂各自的耗时留着，最后算比值 —— 判据要能一眼看出来，不靠人肉比较。
    double dt_one_client = 0.0;
    double dt_four_clients = 0.0;
    double dt_pool = 0.0;

    {
      std::vector<std::thread> pool;
      const double t0 = now_us();
      for (int i = 0; i < threads; ++i) {
        pool.emplace_back([&db, &point_select_loop]() { point_select_loop(&db); });
      }
      for (auto& th : pool) th.join();
      const double dt = now_us() - t0;
      dt_one_client = dt;
      std::printf("  %-26s %12.1f ms %14.0f calls/s\n", "4 threads / 1 client",
                  dt / 1000.0, threads * per_thread * 1e6 / dt);
    }

    {
      std::vector<std::unique_ptr<uvcpp_db_client>> clients;
      bool ok = true;
      for (int i = 0; i < threads; ++i) {
        std::unique_ptr<uvcpp_db_client> c(new uvcpp_db_client());
        if (!uvcpp_db_ok(c->open(url))) {
          std::fprintf(stderr, "  第 %d 个 client 打开失败：%s\n", i,
                       c->last_error().c_str());
          ok = false;
          break;
        }
        clients.push_back(std::move(c));
      }
      if (ok) {
        std::vector<std::thread> pool;
        const double t0 = now_us();
        for (int i = 0; i < threads; ++i) {
          pool.emplace_back([&clients, i, &point_select_loop]() {
            point_select_loop(clients[i].get());
          });
        }
        for (auto& th : pool) th.join();
        const double dt = now_us() - t0;
        dt_four_clients = dt;
        std::printf("  %-26s %12.1f ms %14.0f calls/s\n", "4 threads / 4 clients",
                    dt / 1000.0, threads * per_thread * 1e6 / dt);
      }
    }
    // 第三条：池子。同一批活、同一个并发度，区别只在连接从哪来 —— 每条线程**每次
    // 查询**借一条、用完立刻还。所以它买的就是上面「4 clients」那条的并行度，代价
    // 是每次多一次借还；而它相对「4 clients」少的，是那 4 次 open（各 1.9~2.4 ms）。
    {
      uvcpp_db_pool pool;
      const uvcpp_db_status ist = pool.init(url, threads, threads);
      if (!uvcpp_db_ok(ist)) {
        std::fprintf(stderr, "  池子起不来：%s\n", pool.last_error().c_str());
      } else {
        std::vector<int> failed(static_cast<size_t>(threads), 0);
        std::vector<std::thread> workers;
        const double t0 = now_us();
        for (int i = 0; i < threads; ++i) {
          workers.emplace_back([&pool, &psql, &failed, i, per_thread, nrows]() {
            for (int k = 0; k < per_thread; ++k) {
              const int64_t id = 1 + (k * 7919) % nrows;
              uvcpp_db_table t;
              if (!uvcpp_db_ok(pool.query(psql, {id}, &t))) ++failed[i];
            }
          });
        }
        for (auto& th : workers) th.join();
        const double dt = now_us() - t0;
        dt_pool = dt;
        int bad = 0;
        for (size_t i = 0; i < failed.size(); ++i) bad += failed[i];
        std::printf(
            "  %-26s %12.1f ms %14.0f calls/s   [created %llu / reused %llu / 借不到 %d 次]\n",
            "4 threads / pool(4)", dt / 1000.0, threads * per_thread * 1e6 / dt,
            static_cast<unsigned long long>(pool.created_total()),
            static_cast<unsigned long long>(pool.reused_total()), bad);
      }
    }

    if (dt_four_clients > 0.0 && dt_pool > 0.0) {
      std::printf(
          "  池子 / 各开一条 之比：%.2f×（判据：同量级，约 1×）   池子 / 共享一条 之比：%.2f×\n",
          dt_pool / dt_four_clients, dt_pool / dt_one_client);
    }

    std::printf(
        "  读法  「共享 1 个 client」比「各开一条」慢多少，就是那把递归锁的实际价格\n"
        "        —— 也就是「连接池」这件事值不值得做的第一个数。反过来，「各开一条」\n"
        "        快多少，是连接数换并行度的上限（服务端的 max_connections 是它的\n"
        "        天花板）。第三条是池子：它买的正是上面那条的并行度，成本只多一次借还\n"
        "        （一把锁 + 一次出队，约 1 µs），所以**判据是它和「各开一条」同量级**\n"
        "        （1× 附近，见上面那行比值）—— 掉了才说明借还那层把并行度吃回去了。\n"
        "        注意它**不是**必然落在两者之间：进程内的 SQLite 上它甚至会略快于\n"
        "        「各开一条」（那边唯一的开销就是线程调度，4 条连接的 open 是白付的）。\n"
        "        方括号里的 created 是**复用的见证**：它等于 max（是 4，不是 2000），\n"
        "        说明连接是借来借去，而不是每次新开；reused 则说明这两千次里有多少次\n"
        "        是白捡的。这一节也顺带量到池子在**并发**上的代价：`query()` 的借还是\n"
        "        一次一条，想更快就得自己 `acquire()` 拿在手里批量用（事务那种用法）。\n");
  }

  // ---- 收尾 ----
  db.execute("DROP TABLE IF EXISTS uvcpp_bench_rows");
  db.close();
  std::printf("\n完成。\n");
  return 0;
}
