/**
 * @file bench/orm/bench_orm.cpp
 * @brief ORM 端到端与纯开销量具：同一台机器、同一个 SQLite 文件、同一条
 *        连接上，比较**等价手写 SQL** 与 ORM 的成本。
 * @author zhuweiye
 *
 * 这是 issue #95 的第二阶段（第一阶段是 `bench_db.cpp` 的 db 模块基线）。
 * ORM 的模型、SQL 构造、CRUD、事务/异步在 #89/#90/#91/#92 已落地；这里只
 * 量它相对手写 SQL 多花多少，不改 `src/` 的任何行为，也不用 mock。
 *
 * ## 量什么
 *
 * 端到端（真实 SQLite，同一个 `uvcpp_db_client`、同一个连接）：
 *
 *   * `pk`          —— 按主键点查：raw `SELECT` + 手工读列 vs `find_by_pk<T>`；
 *   * `find_all100` —— 取 100 行：raw query + 手工映射 vs 构造器 +
 *                      `find_all<T>`（不带 total，避免多一条 COUNT）；
 *   * `insert`      —— 插入一行：raw `db.insert` vs `insert<T>`（都回填自增 id）；
 *   * `update`      —— 按主键更新：raw `db.execute` vs `update<T>`；
 *   * `remove`      —— 按主键删除：raw `db.execute` vs `remove<T>`。
 *
 * 纯开销（不碰数据库）：
 *
 *   * `build_select` —— raw SQL 字符串 + `uvcpp_db_params` vs
 *                       `select<T>(dialect).where...build()`；
 *   * `map_rows`     —— 100 行内存结果集的手工映射 vs
 *                       `orm_detail::map_table<T>()`（`find_all` 内部用的
 *                       同一条映射代码路径）。
 *
 * ## A/B 口径（重要）
 *
 *   * raw 臂的 SQL 文本是**提前拼好并复用**的，和手写热路径一致；
 *     ORM 臂每次调用都会重新构造 SQL 与参数 —— 那正是 ORM 的开销。
 *   * 参数里带变化值的（点查的主键、删除的主键）raw 臂每次重建
 *     `uvcpp_db_params`；插入复用一组固定参数，更新在两个预构造的参数集之间
 *     交替（两臂都每轮换值）。ORM 臂一律重建。
 *   * 每一对样本**交替**先 raw 后 ORM / 先 ORM 后 raw，抵消机器漂移。
 *   * 计时单位：端到端一次调用；`build_select` 一次构造；`map_rows` 一行。
 *     纯开销按批计时再除以批内单位数，避免把 `steady_clock` 的几十纳秒
 *     放大成结果的一部分。TSV 的 `samples` 是**计时批数**，`ops` 是单位总数。
 *
 * ## 不量什么
 *
 *   * 只支持 SQLite。DDL 与 raw SQL 按 SQLite 写死；其它后端没有在这里
 *     验证过，脚本不会给出 MySQL/PostgreSQL 数字。
 *   * 不改 SQLite 的 pragma（WAL / synchronous / cache），用的是驱动默认值。
 *     写路径因此每次语句都要 fsync，ORM 的几微秒开销会被磁盘账淹没 ——
 *     这是真实成本，不是量具的毛病，报告里要分开看读/写。
 *   * 不量分配次数、CPU 周期、连接池、事务、异步。那些在 `bench_db.cpp`
 *     与后续 issue 里。
 *
 * ## 用法
 *
 *     uvcpp_bench_orm <sqlite-url> [--label=名字] [--results=path.tsv]
 *                     [--samples=1000] [--pure-ops=100000] ...
 *
 * `--help` 是完整参数表。退出码：0 跑完；1 有操作失败；2 用法错 / 打开失败 /
 * 不是 SQLite。
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <orm/uvcpp_orm_repository.h>

// ---------------------------------------------------------------------------
// 模型：5 列，覆盖整数 / 文本 / 浮点 / 布尔 / 自增主键
// ---------------------------------------------------------------------------

struct bench_user {
  std::int64_t id;
  std::string name;
  std::int32_t age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(bench_user, "bench_user")
UVCPP_ORM_FIELD(bench_user, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(bench_user, name, "name", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(bench_user, age, "age", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(bench_user, score, "score", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(bench_user, active, "active", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

namespace {

using uvcpp::uvcpp_db_client;
using uvcpp::uvcpp_db_params;
using uvcpp::uvcpp_db_status;
using uvcpp::uvcpp_db_table;
using uvcpp::uvcpp_db_value;
using uvcpp::uvcpp_orm_dialect;
using uvcpp::uvcpp_orm_sql_result;
using uvcpp::uvcpp_orm_status;

using steady = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// 与 raw 臂共用、且已与 ORM 生成结果逐字比对过的 SQL（见 verify_sql）
// ---------------------------------------------------------------------------

const char* const kPkSql =
    "SELECT \"id\", \"name\", \"age\", \"score\", \"active\" FROM "
    "\"bench_user\" WHERE \"id\" = ? LIMIT ?";

const char* const kInsertSql =
    "INSERT INTO \"bench_user\" (\"name\", \"age\", \"score\", \"active\") "
    "VALUES (?, ?, ?, ?)";

const char* const kUpdateSql =
    "UPDATE \"bench_user\" SET \"name\" = ?, \"age\" = ?, \"score\" = ?, "
    "\"active\" = ? WHERE \"id\" = ?";

const char* const kRemoveSql =
    "DELETE FROM \"bench_user\" WHERE \"id\" = ?";

const char* const kSelectAllSql =
    "SELECT \"id\", \"name\", \"age\", \"score\", \"active\" FROM "
    "\"bench_user\" ORDER BY \"id\" ASC LIMIT ?";

const char* const kBuildSelectSql =
    "SELECT \"id\", \"name\", \"age\", \"score\", \"active\" FROM "
    "\"bench_user\" WHERE \"age\" >= ? AND \"active\" = ? "
    "ORDER BY \"id\" ASC LIMIT ?";

const char* const kDdl =
    "CREATE TABLE \"bench_user\" ("
    "\"id\" INTEGER PRIMARY KEY AUTOINCREMENT,"
    "\"name\" TEXT NOT NULL,"
    "\"age\" INTEGER NOT NULL,"
    "\"score\" REAL NOT NULL,"
    "\"active\" INTEGER NOT NULL)";

// 结果校验和：让编译器不能把纯 CPU 那两臂整个消掉。
std::uint64_t g_sink = 0u;

double now_us() {
  return std::chrono::duration<double, std::micro>(
             steady::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------------------
// 参数与统计
// ---------------------------------------------------------------------------

struct options {
  options()
      : samples(1000),
        warmup(50),
        pure_ops(100000),
        pure_batch(1000),
        pure_warmup(1000),
        map_rows(100),
        map_batches(2000),
        map_warmup(2),
        seed_extra_warmup(50) {}

  std::string url;
  std::string label;
  std::string results;
  int samples;
  int warmup;
  std::uint64_t pure_ops;
  std::uint64_t pure_batch;
  std::uint64_t pure_warmup;
  std::uint64_t map_rows;
  std::uint64_t map_batches;
  std::uint64_t map_warmup;
  int seed_extra_warmup;
};

struct stats {
  stats() : min_us(0.0), mean_us(0.0), p50_us(0.0), p99_us(0.0) {}
  double min_us;
  double mean_us;
  double p50_us;
  double p99_us;
};

stats summarize(std::vector<double> values, double units_per_sample) {
  stats s;
  if (values.empty() || units_per_sample <= 0.0) return s;
  for (std::size_t i = 0; i < values.size(); ++i) {
    values[i] /= units_per_sample;
  }
  std::sort(values.begin(), values.end());
  double sum = 0.0;
  for (std::size_t i = 0; i < values.size(); ++i) sum += values[i];
  s.min_us = values.front();
  s.mean_us = sum / static_cast<double>(values.size());
  // 与 bench_db.cpp / bench_router.cpp 同一取法：idx = n * p / 100。
  s.p50_us = values[values.size() * 50u / 100u];
  s.p99_us = values[values.size() * 99u / 100u];
  return s;
}

struct record {
  record()
      : ops(0u), samples(0u) {}
  std::string case_name;
  std::string arm;  // "raw" / "orm"
  std::string unit; // "call" / "query" / "row"
  std::uint64_t ops;
  std::uint64_t samples;
  stats s;
};

// ---------------------------------------------------------------------------
// 运行器：每个 sample 做 units_per_sample 个单位，raw/orm 交替
// ---------------------------------------------------------------------------

template <typename RawOp, typename OrmOp>
bool run_units_ab(const char* case_name, const char* unit,
                  std::size_t samples, std::size_t units_per_sample,
                  std::size_t warmup_units, RawOp raw_op, OrmOp orm_op,
                  std::vector<record>* out) {
  const std::size_t warmup_calls =
      units_per_sample > 0u ? (warmup_units + units_per_sample - 1u) /
                                  units_per_sample
                            : 0u;

  for (std::size_t i = 0; i < warmup_calls; ++i) {
    if (!raw_op() || !orm_op()) {
      std::fprintf(stderr, "[FAIL] %s: warm-up operation failed\n", case_name);
      return false;
    }
  }

  std::vector<double> raw_us;
  std::vector<double> orm_us;
  raw_us.reserve(samples);
  orm_us.reserve(samples);

  for (std::size_t i = 0; i < samples; ++i) {
    if ((i & 1u) == 0u) {
      const double raw_t0 = now_us();
      const bool raw_ok = raw_op();
      const double raw_t1 = now_us();
      const double orm_t0 = now_us();
      const bool orm_ok = orm_op();
      const double orm_t1 = now_us();
      raw_us.push_back(raw_t1 - raw_t0);
      orm_us.push_back(orm_t1 - orm_t0);
      if (!raw_ok || !orm_ok) {
        std::fprintf(stderr, "[FAIL] %s: operation failed (sample %zu)\n",
                     case_name, i);
        return false;
      }
    } else {
      const double orm_t0 = now_us();
      const bool orm_ok = orm_op();
      const double orm_t1 = now_us();
      const double raw_t0 = now_us();
      const bool raw_ok = raw_op();
      const double raw_t1 = now_us();
      orm_us.push_back(orm_t1 - orm_t0);
      raw_us.push_back(raw_t1 - raw_t0);
      if (!raw_ok || !orm_ok) {
        std::fprintf(stderr, "[FAIL] %s: operation failed (sample %zu)\n",
                     case_name, i);
        return false;
      }
    }
  }

  record raw_record;
  raw_record.case_name = case_name;
  raw_record.arm = "raw";
  raw_record.unit = unit;
  raw_record.ops = static_cast<std::uint64_t>(samples) *
                   static_cast<std::uint64_t>(units_per_sample);
  raw_record.samples = static_cast<std::uint64_t>(samples);
  raw_record.s = summarize(std::move(raw_us),
                           static_cast<double>(units_per_sample));
  out->push_back(raw_record);

  record orm_record;
  orm_record.case_name = case_name;
  orm_record.arm = "orm";
  orm_record.unit = unit;
  orm_record.ops = raw_record.ops;
  orm_record.samples = raw_record.samples;
  orm_record.s = summarize(std::move(orm_us),
                           static_cast<double>(units_per_sample));
  out->push_back(orm_record);
  return true;
}

// ---------------------------------------------------------------------------
// SQL 等价性自检：raw 字符串必须与 ORM 生成的逐字一致
// ---------------------------------------------------------------------------

bool same_params(const uvcpp_db_params& a, const uvcpp_db_params& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

bool check_sql(const char* what, const std::string& raw,
               const uvcpp_db_params& raw_params,
               const uvcpp_orm_sql_result& orm) {
  if (!orm.ok()) {
    std::fprintf(stderr, "[FAIL] %s: ORM build failed: %s\n", what,
                 orm.error.c_str());
    return false;
  }
  if (orm.sql != raw) {
    std::fprintf(stderr, "[FAIL] %s: SQL mismatch\n  raw: %s\n  orm: %s\n",
                 what, raw.c_str(), orm.sql.c_str());
    return false;
  }
  if (!same_params(raw_params, orm.params)) {
    std::fprintf(stderr, "[FAIL] %s: params mismatch (raw %zu, orm %zu)\n",
                 what, raw_params.size(), orm.params.size());
    return false;
  }
  return true;
}

bool verify_sql() {
  std::string error;

  uvcpp_orm_sql_result pk;
  if (uvcpp::orm_detail::make_find_by_pk_sql<bench_user>(
          uvcpp_orm_dialect::sqlite, uvcpp_db_value(std::int64_t(1)), &pk,
          &error) != uvcpp_orm_status::OK) {
    std::fprintf(stderr, "[FAIL] verify pk build: %s\n", error.c_str());
    return false;
  }
  uvcpp_db_params pk_params;
  pk_params.add(std::int64_t(1)).add(std::int64_t(1));
  if (!check_sql("pk", kPkSql, pk_params, pk)) return false;

  bench_user row;
  row.id = 7;
  row.name = "verify";
  row.age = 33;
  row.score = 1.25;
  row.active = true;

  uvcpp_orm_sql_result ins;
  if (uvcpp::orm_detail::make_insert_sql<bench_user>(
          uvcpp_orm_dialect::sqlite, row, &ins, &error) !=
      uvcpp_orm_status::OK) {
    std::fprintf(stderr, "[FAIL] verify insert build: %s\n", error.c_str());
    return false;
  }
  uvcpp_db_params ins_params;
  ins_params.add(row.name).add(row.age).add(row.score).add(row.active);
  if (!check_sql("insert", kInsertSql, ins_params, ins)) return false;

  uvcpp_orm_sql_result upd;
  if (uvcpp::orm_detail::make_update_sql<bench_user>(
          uvcpp_orm_dialect::sqlite, row, &upd, &error) !=
      uvcpp_orm_status::OK) {
    std::fprintf(stderr, "[FAIL] verify update build: %s\n", error.c_str());
    return false;
  }
  uvcpp_db_params upd_params;
  upd_params.add(row.name).add(row.age).add(row.score).add(row.active).add(
      row.id);
  if (!check_sql("update", kUpdateSql, upd_params, upd)) return false;

  uvcpp_orm_sql_result rem;
  if (uvcpp::orm_detail::make_remove_sql<bench_user>(
          uvcpp_orm_dialect::sqlite, uvcpp_db_value(std::int64_t(7)), &rem,
          &error) != uvcpp_orm_status::OK) {
    std::fprintf(stderr, "[FAIL] verify remove build: %s\n", error.c_str());
    return false;
  }
  uvcpp_db_params rem_params;
  rem_params.add(std::int64_t(7));
  if (!check_sql("remove", kRemoveSql, rem_params, rem)) return false;

  const uvcpp_orm_sql_result all_built =
      uvcpp::select<bench_user>(uvcpp_orm_dialect::sqlite)
          .order_by(&bench_user::id, uvcpp::uvcpp_orm_order::asc)
          .limit(100)
          .build();
  uvcpp_db_params all_params;
  all_params.add(100);
  if (!check_sql("select_all", kSelectAllSql, all_params, all_built)) {
    return false;
  }

  const uvcpp_orm_sql_result build_built =
      uvcpp::select<bench_user>(uvcpp_orm_dialect::sqlite)
          .where_ge(&bench_user::age, 18)
          .where_eq(&bench_user::active, true)
          .order_by(&bench_user::id, uvcpp::uvcpp_orm_order::asc)
          .limit(100)
          .build();
  uvcpp_db_params build_params;
  build_params.add(18).add(true).add(100);
  if (!check_sql("build_select", kBuildSelectSql, build_params, build_built)) {
    return false;
  }

  std::printf("SQL equivalence check: raw and ORM text/params match for "
              "pk/insert/update/remove/select_all/build_select\n");
  return true;
}

// ---------------------------------------------------------------------------
// 建表 / 灌数据
// ---------------------------------------------------------------------------

bool exec_ok(uvcpp_db_client& db, const std::string& sql,
             const uvcpp_db_params& params, const char* what) {
  const uvcpp_db_status status = db.execute(sql, params, nullptr);
  if (!uvcpp_db_ok(status)) {
    std::fprintf(stderr, "[FAIL] %s: %s: %s\n", what,
                 uvcpp::uvcpp_db_status_name(status),
                 db.last_error().c_str());
    return false;
  }
  return true;
}

bool exec_ok(uvcpp_db_client& db, const std::string& sql, const char* what) {
  const uvcpp_db_status status = db.execute(sql, nullptr);
  if (!uvcpp_db_ok(status)) {
    std::fprintf(stderr, "[FAIL] %s: %s: %s\n", what,
                 uvcpp::uvcpp_db_status_name(status),
                 db.last_error().c_str());
    return false;
  }
  return true;
}

bool seed_table(uvcpp_db_client& db, std::size_t rows) {
  if (!exec_ok(db, "DROP TABLE IF EXISTS \"bench_user\"", "drop table")) {
    return false;
  }
  if (!exec_ok(db, kDdl, "create table")) return false;

  // 灌数据是一次性的准备，不量；放进一个事务里，避免每行一次 fsync。
  if (!exec_ok(db, "BEGIN", "seed begin")) return false;
  for (std::size_t i = 0; i < rows; ++i) {
    uvcpp_db_params p;
    p.add(std::string("user-") + std::to_string(i));
    p.add(static_cast<std::int32_t>(20 + static_cast<int>(i % 60u)));
    p.add(static_cast<double>(i) * 0.5);
    p.add((i % 2u) == 0u);
    if (!exec_ok(db, kInsertSql, p, "seed insert")) {
      exec_ok(db, "ROLLBACK", "seed rollback");
      return false;
    }
  }
  return exec_ok(db, "COMMIT", "seed commit");
}

// ---------------------------------------------------------------------------
// 报表
// ---------------------------------------------------------------------------

void print_table_header() {
  std::printf("  %-14s %-4s %-6s %9s %8s %10s %10s %10s %10s %12s\n",
              "case", "arm", "unit", "ops", "samples", "min(us)", "mean(us)",
              "p50(us)", "p99(us)", "ops/s");
}

void print_record(const record& r) {
  const double ops_per_s =
      r.s.mean_us > 0.0 ? 1e6 / r.s.mean_us : 0.0;
  std::printf("  %-14s %-4s %-6s %9llu %8llu %10.3f %10.3f %10.3f "
              "%10.3f %12.0f\n",
              r.case_name.c_str(), r.arm.c_str(), r.unit.c_str(),
              static_cast<unsigned long long>(r.ops),
              static_cast<unsigned long long>(r.samples), r.s.min_us,
              r.s.mean_us, r.s.p50_us, r.s.p99_us, ops_per_s);
}

void print_overhead(const std::vector<record>& records) {
  std::printf("\n  extra cost of ORM vs raw (same run, per-unit):\n");
  std::printf("  %-14s %12s %12s %12s\n", "case", "mean_extra%",
              "p50_extra%", "p99_extra%");
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (records[i].arm != "raw") continue;
    for (std::size_t j = i + 1; j < records.size(); ++j) {
      if (records[j].arm != "orm" ||
          records[j].case_name != records[i].case_name) {
        continue;
      }
      const stats& raw = records[i].s;
      const stats& orm = records[j].s;
      const double mean_pct =
          raw.mean_us > 0.0 ? (orm.mean_us / raw.mean_us - 1.0) * 100.0 : 0.0;
      const double p50_pct =
          raw.p50_us > 0.0 ? (orm.p50_us / raw.p50_us - 1.0) * 100.0 : 0.0;
      const double p99_pct =
          raw.p99_us > 0.0 ? (orm.p99_us / raw.p99_us - 1.0) * 100.0 : 0.0;
      std::printf("  %-14s %12.2f %12.2f %12.2f\n",
                  records[i].case_name.c_str(), mean_pct, p50_pct, p99_pct);
      break;
    }
  }
}

bool write_tsv(const std::string& path, const std::vector<record>& records) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "cannot write results: %s\n", path.c_str());
    return false;
  }
  std::fprintf(f,
               "case\tarm\tunit\tops\tsamples\tmin_us\tmean_us\tp50_us\t"
               "p99_us\tops_per_s\n");
  for (std::size_t i = 0; i < records.size(); ++i) {
    const record& r = records[i];
    const double ops_per_s =
        r.s.mean_us > 0.0 ? 1e6 / r.s.mean_us : 0.0;
    std::fprintf(f, "%s\t%s\t%s\t%llu\t%llu\t%.6f\t%.6f\t%.6f\t%.6f\t%.3f\n",
                 r.case_name.c_str(), r.arm.c_str(), r.unit.c_str(),
                 static_cast<unsigned long long>(r.ops),
                 static_cast<unsigned long long>(r.samples), r.s.min_us,
                 r.s.mean_us, r.s.p50_us, r.s.p99_us, ops_per_s);
  }
  std::fclose(f);
  return true;
}

// ---------------------------------------------------------------------------
// 用法
// ---------------------------------------------------------------------------

void print_usage(const char* argv0) {
  std::printf(
      "usage: %s <sqlite-url> [options]\n"
      "  --label=NAME        label written by the driver script\n"
      "  --results=PATH      write machine-readable TSV here\n"
      "  --samples=N         end-to-end samples per arm (default 1000, min 100)\n"
      "  --warmup=N          end-to-end warm-up calls per arm (default 50)\n"
      "  --pure-ops=N        build_select operations per arm (default 100000)\n"
      "  --pure-batch=N      build_select ops per timed sample (default 1000)\n"
      "  --pure-warmup=N     build_select warm-up ops per arm (default 1000)\n"
      "  --map-rows=N        rows per map_rows sample (default 100)\n"
      "  --map-batches=N     map_rows timed samples per arm (default 2000)\n"
      "  --map-warmup=N      map_rows warm-up full-table maps per arm "
      "(default 2)\n"
      "  --help              show this help\n"
      "\n"
      "Only SQLite URLs are supported by this benchmark. Exit codes:\n"
      "  0 ok, 1 an operation failed, 2 usage/open/driver error.\n",
      argv0);
}

bool parse_u64(const char* text, std::uint64_t* out) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text, &end, 10);
  if (end == nullptr || *end != '\0') return false;
  *out = static_cast<std::uint64_t>(v);
  return true;
}

bool parse_int(const char* text, int* out) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  const long v = std::strtol(text, &end, 10);
  if (end == nullptr || *end != '\0') return false;
  *out = static_cast<int>(v);
  return true;
}

bool parse_args(int argc, char** argv, options* opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (a.rfind("--label=", 0) == 0) {
      opt->label = a.substr(8);
    } else if (a.rfind("--results=", 0) == 0) {
      opt->results = a.substr(10);
    } else if (a.rfind("--samples=", 0) == 0) {
      if (!parse_int(a.c_str() + 10, &opt->samples)) return false;
    } else if (a.rfind("--warmup=", 0) == 0) {
      if (!parse_int(a.c_str() + 9, &opt->warmup)) return false;
    } else if (a.rfind("--pure-ops=", 0) == 0) {
      if (!parse_u64(a.c_str() + 11, &opt->pure_ops)) return false;
    } else if (a.rfind("--pure-batch=", 0) == 0) {
      if (!parse_u64(a.c_str() + 13, &opt->pure_batch)) return false;
    } else if (a.rfind("--pure-warmup=", 0) == 0) {
      if (!parse_u64(a.c_str() + 14, &opt->pure_warmup)) return false;
    } else if (a.rfind("--map-rows=", 0) == 0) {
      if (!parse_u64(a.c_str() + 11, &opt->map_rows)) return false;
    } else if (a.rfind("--map-batches=", 0) == 0) {
      if (!parse_u64(a.c_str() + 14, &opt->map_batches)) return false;
    } else if (a.rfind("--map-warmup=", 0) == 0) {
      if (!parse_u64(a.c_str() + 13, &opt->map_warmup)) return false;
    } else if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
      std::fprintf(stderr, "unknown option: %s\n", a.c_str());
      return false;
    } else if (opt->url.empty()) {
      opt->url = a;
    } else {
      std::fprintf(stderr, "unexpected argument: %s\n", a.c_str());
      return false;
    }
  }

  if (opt->url.empty()) {
    const char* env = std::getenv("UVCPP_ORM_BENCH_URL");
    if (env != nullptr && *env != '\0') opt->url = env;
  }
  if (opt->url.empty()) {
    std::fprintf(stderr, "missing sqlite URL (or UVCPP_ORM_BENCH_URL)\n");
    return false;
  }
  if (opt->samples < 100) {
    std::fprintf(stderr, "--samples must be >= 100\n");
    return false;
  }
  if (opt->warmup < 0) {
    std::fprintf(stderr, "--warmup must be >= 0\n");
    return false;
  }
  if (opt->pure_ops < 1000u || opt->pure_batch == 0u ||
      opt->pure_batch > opt->pure_ops ||
      opt->pure_ops % opt->pure_batch != 0u) {
    std::fprintf(stderr,
                 "--pure-ops must be a multiple of --pure-batch (both > 0, "
                 "pure-ops >= 1000)\n");
    return false;
  }
  if (opt->map_rows == 0u || opt->map_batches == 0u) {
    std::fprintf(stderr, "--map-rows and --map-batches must be > 0\n");
    return false;
  }
  if (opt->seed_extra_warmup < 1) opt->seed_extra_warmup = 1;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  options opt;
  if (!parse_args(argc, argv, &opt)) {
    print_usage(argv[0]);
    return 2;
  }

  uvcpp_db_client db;
  const uvcpp_db_status opened = db.open(opt.url);
  if (!uvcpp_db_ok(opened)) {
    std::fprintf(stderr, "open failed: %s\n  url: %s\n",
                 db.last_error().c_str(), opt.url.c_str());
    return 2;
  }
  if (db.driver_name() == nullptr ||
      std::strcmp(db.driver_name(), "sqlite") != 0) {
    std::fprintf(stderr,
                 "this benchmark only supports SQLite; connected driver is "
                 "\"%s\"\n",
                 db.driver_name() != nullptr ? db.driver_name() : "(none)");
    db.close();
    return 2;
  }

  std::printf("uvcpp_bench_orm\n");
  std::printf("  label:          %s\n",
              opt.label.empty() ? "(none)" : opt.label.c_str());
  std::printf("  url:            %s\n", opt.url.c_str());
  std::printf("  driver:         %s\n", db.driver_name());
  std::printf("  e2e samples:    %d per arm (warm-up %d calls)\n", opt.samples,
              opt.warmup);
  std::printf("  build samples:  %llu ops per arm in %llu-op batches "
              "(warm-up %llu ops)\n",
              static_cast<unsigned long long>(opt.pure_ops),
              static_cast<unsigned long long>(opt.pure_batch),
              static_cast<unsigned long long>(opt.pure_warmup));
  std::printf("  map samples:    %llu rows per sample x %llu samples per arm "
              "(warm-up %llu calls)\n",
              static_cast<unsigned long long>(opt.map_rows),
              static_cast<unsigned long long>(opt.map_batches),
              static_cast<unsigned long long>(opt.map_warmup));

  if (!verify_sql()) {
    db.close();
    return 1;
  }

  // 种子行：pk/find_all 用 1..N，update 用 1..N，remove 用 N..3N 加两段
  // warm-up 行。3N + 2*W 足够。
  const std::size_t samples = static_cast<std::size_t>(opt.samples);
  const std::size_t warmup = static_cast<std::size_t>(opt.warmup);
  const std::size_t remove_warmup = static_cast<std::size_t>(opt.warmup);
  const std::size_t seed_rows =
      3u * samples + 2u * remove_warmup +
      static_cast<std::size_t>(opt.seed_extra_warmup);
  std::printf("  seed rows:      %zu\n", seed_rows);
  if (!seed_table(db, seed_rows)) {
    db.close();
    return 1;
  }

  std::vector<record> records;
  std::string error;
  uvcpp_db_status db_status = uvcpp_db_status::OK;

  // -------------------------------------------------------------------------
  // 1. 按主键点查
  // -------------------------------------------------------------------------
  {
    std::size_t raw_i = 0;
    std::size_t orm_i = 0;
    std::uint64_t pk_sink = 0;

    const auto raw_pk = [&]() -> bool {
      const std::int64_t id =
          static_cast<std::int64_t>((raw_i++ % samples) + 1u);
      uvcpp_db_params p;
      p.add(id).add(std::int64_t(1));
      uvcpp_db_table table;
      if (!uvcpp_db_ok(db.query(kPkSql, p, &table))) return false;
      if (table.row_count() != 1u) return false;
      const uvcpp::uvcpp_db_row& row = table.rows()[0];
      bench_user u;
      u.id = row[0].to_int64();
      u.name = row[1].to_text();
      u.age = static_cast<std::int32_t>(row[2].to_int64());
      u.score = row[3].to_double();
      u.active = row[4].to_bool();
      pk_sink += static_cast<std::uint64_t>(u.id);
      return true;
    };

    const auto orm_pk = [&]() -> bool {
      const std::int64_t id =
          static_cast<std::int64_t>((orm_i++ % samples) + 1u);
      bench_user u;
      if (uvcpp::find_by_pk<bench_user>(db, uvcpp_db_value(id), &u, &error,
                                        &db_status) !=
          uvcpp_orm_status::OK) {
        return false;
      }
      pk_sink += static_cast<std::uint64_t>(u.id);
      return true;
    };

    if (!run_units_ab("pk", "call", samples, 1u, warmup, raw_pk, orm_pk,
                      &records)) {
      db.close();
      return 1;
    }
    g_sink += pk_sink;
  }

  // -------------------------------------------------------------------------
  // 2. find_all：100 行
  // -------------------------------------------------------------------------
  {
    std::uint64_t all_sink = 0;
    const std::size_t limit = 100u;
    uvcpp_db_params all_params;
    all_params.add(static_cast<std::int64_t>(limit));
    const uvcpp::uvcpp_orm_select_builder<bench_user> builder =
        uvcpp::select<bench_user>(uvcpp_orm_dialect::sqlite)
            .order_by(&bench_user::id, uvcpp::uvcpp_orm_order::asc)
            .limit(static_cast<std::int64_t>(limit));

    const auto raw_all = [&]() -> bool {
      uvcpp_db_table table;
      if (!uvcpp_db_ok(db.query(kSelectAllSql, all_params, &table))) {
        return false;
      }
      if (table.row_count() != limit) return false;
      std::vector<bench_user> rows;
      rows.reserve(table.row_count());
      const std::vector<uvcpp::uvcpp_db_row>& src = table.rows();
      for (std::size_t i = 0; i < src.size(); ++i) {
        const uvcpp::uvcpp_db_row& row = src[i];
        bench_user u;
        u.id = row[0].to_int64();
        u.name = row[1].to_text();
        u.age = static_cast<std::int32_t>(row[2].to_int64());
        u.score = row[3].to_double();
        u.active = row[4].to_bool();
        rows.push_back(std::move(u));
      }
      all_sink += static_cast<std::uint64_t>(rows.size());
      return true;
    };

    const auto orm_all = [&]() -> bool {
      std::vector<bench_user> rows;
      if (uvcpp::find_all<bench_user>(db, builder, &rows, &error) !=
          uvcpp_orm_status::OK) {
        return false;
      }
      if (rows.size() != limit) return false;
      all_sink += static_cast<std::uint64_t>(rows.size());
      return true;
    };

    if (!run_units_ab("find_all100", "call", samples, 1u, warmup, raw_all,
                      orm_all, &records)) {
      db.close();
      return 1;
    }
    g_sink += all_sink;
  }

  // -------------------------------------------------------------------------
  // 3. 按主键更新
  //
  // 两臂各更新**自己的一行**，并且每次都换一个值：SQLite 在值没变时可能
  // 不写 journal，那样量到的只是解析 SQL 的几微秒，不是一次真实更新。
  // raw 用两个预构造的参数集交替；ORM 每次重新构造（那正是它的开销）。
  // -------------------------------------------------------------------------
  {
    bench_user raw_row;
    raw_row.id = 1;
    raw_row.name = "updated-name";
    raw_row.age = 44;
    raw_row.active = false;

    uvcpp_db_params raw_params_even;
    raw_row.score = 12.5;
    raw_params_even.add(raw_row.name).add(raw_row.age).add(raw_row.score).add(
        raw_row.active).add(raw_row.id);
    uvcpp_db_params raw_params_odd;
    raw_row.score = 13.5;
    raw_params_odd.add(raw_row.name).add(raw_row.age).add(raw_row.score).add(
        raw_row.active).add(raw_row.id);

    bench_user orm_row;
    orm_row.id = 2;
    orm_row.name = "updated-name";
    orm_row.age = 44;
    orm_row.score = 12.5;
    orm_row.active = false;

    std::size_t raw_i = 0;
    std::size_t orm_i = 0;
    std::uint64_t update_sink = 0;
    const auto raw_update = [&]() -> bool {
      const uvcpp_db_params& params =
          ((raw_i++ & 1u) == 0u) ? raw_params_even : raw_params_odd;
      int64_t affected = -1;
      if (!uvcpp_db_ok(db.execute(kUpdateSql, params, &affected))) {
        return false;
      }
      update_sink += static_cast<std::uint64_t>(affected);
      return affected == 1;
    };
    const auto orm_update = [&]() -> bool {
      orm_row.score = ((orm_i++ & 1u) == 0u) ? 12.5 : 13.5;
      int64_t affected = -1;
      if (uvcpp::update<bench_user>(db, orm_row, &affected, &error, &db_status) !=
          uvcpp_orm_status::OK) {
        return false;
      }
      update_sink += static_cast<std::uint64_t>(affected);
      return affected == 1;
    };

    if (!run_units_ab("update", "call", samples, 1u, warmup, raw_update,
                      orm_update, &records)) {
      db.close();
      return 1;
    }
    g_sink += update_sink;
  }

  // -------------------------------------------------------------------------
  // 4. 插入一行（事务外单条；两边都用 db.insert 回填 last_insert_id）
  // -------------------------------------------------------------------------
  {
    bench_user row;
    row.name = "inserted-name";
    row.age = 30;
    row.score = 1.5;
    row.active = true;

    uvcpp_db_params raw_params;
    raw_params.add(row.name).add(row.age).add(row.score).add(row.active);

    std::uint64_t insert_sink = 0;
    const auto raw_insert = [&]() -> bool {
      uvcpp_db_value new_id;
      if (!uvcpp_db_ok(db.insert(kInsertSql, raw_params, &new_id))) {
        return false;
      }
      if (new_id.is_null()) return false;
      insert_sink += static_cast<std::uint64_t>(new_id.to_int64());
      return true;
    };
    const auto orm_insert = [&]() -> bool {
      uvcpp_db_value new_id;
      if (uvcpp::insert<bench_user>(db, row, &new_id, &error, &db_status) !=
          uvcpp_orm_status::OK) {
        return false;
      }
      if (new_id.is_null()) return false;
      insert_sink += static_cast<std::uint64_t>(new_id.to_int64());
      return true;
    };

    if (!run_units_ab("insert", "call", samples, 1u, warmup, raw_insert,
                      orm_insert, &records)) {
      db.close();
      return 1;
    }
    g_sink += insert_sink;
  }

  // -------------------------------------------------------------------------
  // 5. 按主键删除（两臂删各自的行，warm-up 也用独立行）
  // -------------------------------------------------------------------------
  {
    const std::size_t raw_warm_base = 3u * samples;
    const std::size_t orm_warm_base = raw_warm_base + remove_warmup;
    std::size_t raw_i = 0;
    std::size_t orm_i = 0;
    std::uint64_t remove_sink = 0;

    const auto raw_remove = [&]() -> bool {
      const std::size_t c = raw_i++;
      const std::int64_t id =
          c < remove_warmup
              ? static_cast<std::int64_t>(raw_warm_base + c + 1u)
              : static_cast<std::int64_t>(samples + (c - remove_warmup) + 1u);
      uvcpp_db_params p;
      p.add(id);
      int64_t affected = -1;
      if (!uvcpp_db_ok(db.execute(kRemoveSql, p, &affected))) return false;
      remove_sink += static_cast<std::uint64_t>(affected);
      return affected == 1;
    };
    const auto orm_remove = [&]() -> bool {
      const std::size_t c = orm_i++;
      const std::int64_t id =
          c < remove_warmup
              ? static_cast<std::int64_t>(orm_warm_base + c + 1u)
              : static_cast<std::int64_t>(2u * samples +
                                          (c - remove_warmup) + 1u);
      int64_t affected = -1;
      if (uvcpp::remove<bench_user>(db, uvcpp_db_value(id), &affected, &error,
                                    &db_status) != uvcpp_orm_status::OK) {
        return false;
      }
      remove_sink += static_cast<std::uint64_t>(affected);
      return affected == 1;
    };

    if (!run_units_ab("remove", "call", samples, 1u, remove_warmup,
                      raw_remove, orm_remove, &records)) {
      db.close();
      return 1;
    }
    g_sink += remove_sink;
  }

  // -------------------------------------------------------------------------
  // 6. 纯构造：raw SQL + params vs select<T>(...).where...build()
  // -------------------------------------------------------------------------
  {
    const std::uint64_t batches = opt.pure_ops / opt.pure_batch;
    std::uint64_t build_sink = 0;

    const auto raw_build = [&]() -> bool {
      std::uint64_t local = 0;
      for (std::uint64_t i = 0; i < opt.pure_batch; ++i) {
        std::string sql;
        sql.reserve(192u);
        sql += "SELECT ";
        sql += "\"id\", \"name\", \"age\", \"score\", \"active\"";
        sql += " FROM ";
        sql += "\"bench_user\"";
        sql += " WHERE \"age\" >= ? AND \"active\" = ?";
        sql += " ORDER BY \"id\" ASC LIMIT ?";
        uvcpp_db_params p;
        p.add(18).add(true).add(100);
        local += static_cast<std::uint64_t>(sql.size() + p.size());
      }
      build_sink += local;
      return true;
    };

    const auto orm_build = [&]() -> bool {
      std::uint64_t local = 0;
      for (std::uint64_t i = 0; i < opt.pure_batch; ++i) {
        const uvcpp_orm_sql_result result =
            uvcpp::select<bench_user>(uvcpp_orm_dialect::sqlite)
                .where_ge(&bench_user::age, 18)
                .where_eq(&bench_user::active, true)
                .order_by(&bench_user::id, uvcpp::uvcpp_orm_order::asc)
                .limit(100)
                .build();
        if (!result.ok()) return false;
        local += static_cast<std::uint64_t>(result.sql.size() +
                                            result.params.size());
      }
      build_sink += local;
      return true;
    };

    // unit = 一次构造；每个 sample 做 pure_batch 次。
    const std::uint64_t warmup_units = opt.pure_warmup;
    if (!run_units_ab("build_select", "query",
                      static_cast<std::size_t>(batches),
                      static_cast<std::size_t>(opt.pure_batch), warmup_units,
                      raw_build, orm_build, &records)) {
      db.close();
      return 1;
    }
    g_sink += build_sink;
  }

  // -------------------------------------------------------------------------
  // 7. 纯映射：100 行内存结果集
  // -------------------------------------------------------------------------
  {
    uvcpp_db_table table;
    table.reset_columns(
        std::vector<std::string>{"id", "name", "age", "score", "active"});
    table.reserve_rows(static_cast<std::size_t>(opt.map_rows));
    for (std::uint64_t i = 0; i < opt.map_rows; ++i) {
      uvcpp::uvcpp_db_row& row = table.add_row();
      row.reserve(5u);
      row.push(uvcpp_db_value(static_cast<std::int64_t>(i + 1u)));
      row.push(uvcpp_db_value(std::string("user-") + std::to_string(i)));
      row.push(uvcpp_db_value(static_cast<std::int32_t>(20 + (i % 60u))));
      row.push(uvcpp_db_value(static_cast<double>(i) * 0.5));
      row.push(uvcpp_db_value((i % 2u) == 0u));
    }

    std::vector<bench_user> map_out;
    std::uint64_t map_sink = 0;

    const auto raw_map = [&]() -> bool {
      std::vector<bench_user> rows;
      rows.reserve(table.row_count());
      const std::vector<uvcpp::uvcpp_db_row>& src = table.rows();
      for (std::size_t i = 0; i < src.size(); ++i) {
        const uvcpp::uvcpp_db_row& row = src[i];
        bench_user u;
        u.id = row[0].to_int64();
        u.name = row[1].to_text();
        u.age = static_cast<std::int32_t>(row[2].to_int64());
        u.score = row[3].to_double();
        u.active = row[4].to_bool();
        rows.push_back(std::move(u));
      }
      map_out = std::move(rows);
      map_sink += static_cast<std::uint64_t>(map_out.size());
      return true;
    };

    const auto orm_map = [&]() -> bool {
      error.clear();
      if (uvcpp::orm_detail::map_table<bench_user>(table, &map_out, &error) !=
          uvcpp_orm_status::OK) {
        std::fprintf(stderr, "[FAIL] map_rows: %s\n", error.c_str());
        return false;
      }
      map_sink += static_cast<std::uint64_t>(map_out.size());
      return true;
    };

    // unit = 一行；每个 sample 映射 map_rows 行。warm-up 按“整表映射次数”
    // 计，因此乘回 map_rows 换算成单位数。
    const std::uint64_t map_warmup_units = opt.map_warmup * opt.map_rows;
    if (!run_units_ab("map_rows", "row",
                      static_cast<std::size_t>(opt.map_batches),
                      static_cast<std::size_t>(opt.map_rows),
                      static_cast<std::size_t>(map_warmup_units), raw_map,
                      orm_map, &records)) {
      db.close();
      return 1;
    }
    g_sink += map_sink;
  }

  // -------------------------------------------------------------------------
  // 输出
  // -------------------------------------------------------------------------
  std::printf("\n");
  print_table_header();
  for (std::size_t i = 0; i < records.size(); ++i) {
    print_record(records[i]);
  }
  print_overhead(records);

  std::printf("\n  checksum: %llu (prevents dead-code elimination of the "
              "CPU-only arms)\n",
              static_cast<unsigned long long>(g_sink));

  if (!opt.results.empty()) {
    if (!write_tsv(opt.results, records)) {
      db.close();
      return 1;
    }
    std::printf("  results: %s\n", opt.results.c_str());
  }

  db.close();
  std::printf("完成。\n");
  return 0;
}
