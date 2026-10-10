/**
 * @file tests/functional/db_sqlite_func.cpp
 * @brief 数据库模块 · SQLite 后端：跑共用套件（`db_suite.h`）+ 两条 SQLite 专属判据。
 *
 * ## 为什么这一条**不带** `mysql` / `pgsql` 前缀
 *
 * `tests/functional/CMakeLists.txt` 按文件名把用例和模块开关对齐：文件名里含
 * `db` 的用例需要 `UVCPP_ENABLE_DB`。SQLite 后端只要 `libsqlite3` —— 三个平台
 * 的 runner 上都有 —— 所以它**默认就在跑**，不需要任何环境变量、不需要服务端。
 * MySQL / PostgreSQL 那两条要一个真的服务器，见各自文件开头的说明。
 *
 * ## SQLite 专属的两条（共用套件覆盖不到的地方）
 *
 *   1. **父目录会被创建。** `sqlite3_open_v2` 在父目录不存在时只给一个笼统的
 *      `SQLITE_CANTOPEN`；驱动里的 `make_parent_dirs()` 是手写的递归 mkdir
 *      （C++11 没有 `std::filesystem`）。这一条是它的见证。
 *   2. **`UINT64` 超过 `INT64_MAX` 时拒绝绑定**，而不是截断成负数存进去 ——
 *      SQLite 的整数是有符号 64 位。
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include <db/uvcpp_db.h>

#include "db_suite.h"

namespace {

using namespace uvcpp_db_test;

class sqlite_dialect : public dialect {
 public:
  const char* name() const { return "sqlite"; }

  std::string ph(size_t) const { return "?"; }

  std::string serial_pk_type() const { return "INTEGER PRIMARY KEY AUTOINCREMENT"; }

  std::string create_types_table(const std::string& t) const {
    // `flag_val` 是 INTEGER：SQLite 没有布尔类型，0/1 就是它的布尔 ——
    // 所以共用套件里判的是 `to_bool()` 的值，不是类型标签。
    // `e_val` 特意带 `NOT NULL DEFAULT ''`：把空串折成 NULL 的实现会在这里
    // 撞上非空约束，失败得**响亮**，而不是悄悄存了个 NULL。
    return "CREATE TABLE " + t +
           " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
           " i_val INTEGER, d_val REAL, s_val TEXT, b_val BLOB,"
           " n_val TEXT, e_val TEXT NOT NULL DEFAULT '', flag_val INTEGER,"
           " dt_val TEXT, dd_val TEXT, tt_val TEXT, " + decimal_column_decl() + ")";
  }

  std::string create_serial_table(const std::string& t) const {
    return "CREATE TABLE " + t + " (id " + serial_pk_type() + ", name TEXT)";
  }

  /// SQLite 没有定点类型。所以金额列建成 TEXT —— 这不是"退而求其次"，恰好就是
  /// `doc/db-guide.md` 给三家一起开的方子：要精确的钱就用 TEXT 存字符串。
  std::string decimal_column_decl() const { return "dec_val TEXT"; }

  bool has_native_temporal() const { return false; }
  bool decimal_is_native() const { return false; }
  bool enforces_foreign_keys() const { return true; }
};

/// 测试用的临时目录。CI 的三个平台都有 TMPDIR/TEMP/TMP 之一，兜底给 /tmp。
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

int failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++failures;
  }
}

/// 父目录不存在时也要能开起来 —— 这是 `make_parent_dirs()` 的见证。
///
/// 判据不只是"open 成功"：还要**真的在磁盘上**看到那个文件，并且再开一次
/// （第二次父目录已经在了）也成功。只判第一次成功的话，"某个中间层替我们
/// 建了目录"也能过。
int test_creates_parent_dirs() {
  const std::string base =
      temp_dir() + "/uvcpp-db-parentdir-test/deep/deeper";
  const std::string path = base + "/nested.db";

  std::remove(path.c_str());
  std::remove((base + "/nested.db-journal").c_str());

  uvcpp::uvcpp_db_client db;
  const std::string url = "sqlite://" + path;
  const uvcpp::uvcpp_db_status st = db.open(url);
  check(uvcpp::uvcpp_db_ok(st),
        "父目录不存在时 open 要成功（" + db.last_error() + "）");
  if (!uvcpp::uvcpp_db_ok(st)) return 1;

  std::FILE* f = std::fopen(path.c_str(), "rb");
  check(f != nullptr, "数据库文件真的落在磁盘上了：" + path);
  if (f) std::fclose(f);

  db.close();
  check(uvcpp::uvcpp_db_ok(db.open(url)), "第二次 open（父目录已存在）也要成功");

  std::remove(path.c_str());
  return 0;
}

/// SQLite 的整数是**有符号 64 位**：`UINT64` 超出 `INT64_MAX` 必须拒绝绑定，
/// 不能截断成负数静默存进去。
///
/// 判据是**绑定失败**且**表里没有那一行** —— 只判返回码的话，一个"先报错、
/// 再把截断值存进去"的实现也能过。
int test_uint64_overflow_rejected() {
  uvcpp::uvcpp_db_client db;
  const std::string url = "sqlite://" + temp_dir() + "/uvcpp-db-uint64.db";
  if (!uvcpp_db_ok(db.open(url))) {
    check(false, "open 失败：" + db.last_error());
    return 1;
  }
  db.execute("DROP TABLE IF EXISTS u");
  if (!uvcpp_db_ok(db.execute("CREATE TABLE u (v INTEGER)"))) {
    check(false, "建表失败：" + db.last_error());
    return 1;
  }

  const uint64_t too_big = 18446744073709551615ull;  // UINT64_MAX
  const uvcpp::uvcpp_db_status st =
      db.execute("INSERT INTO u (v) VALUES (?)", {too_big});
  check(st == uvcpp::uvcpp_db_status::BIND_FAILED,
        std::string("UINT64_MAX 绑定必须是 BIND_FAILED，实得 ") +
            uvcpp::uvcpp_db_status_name(st));

  uvcpp::uvcpp_db_table t;
  db.query("SELECT COUNT(*) AS c FROM u", &t);
  check(t.row_count() == 1 && t.rows()[0][0].to_int64() == 0,
        "被拒绝的那一行**没有**落库（截断实现会留下一个 -1）");

  // 边界之内要照常能存。
  if (uvcpp_db_ok(db.execute("INSERT INTO u (v) VALUES (?)",
                             {static_cast<uint64_t>(9223372036854775807ull)}))) {
    uvcpp::uvcpp_db_table t2;
    db.query("SELECT v FROM u ORDER BY v DESC LIMIT 1", &t2);
    check(t2.row_count() == 1 && t2.rows()[0][0].to_int64() == 9223372036854775807ll,
          "INT64_MAX 本身要能存能取");
  } else {
    check(false, "INT64_MAX 应该能存：" + db.last_error());
  }

  db.close();
  std::remove((temp_dir() + "/uvcpp-db-uint64.db").c_str());
  return 0;
}

}  // namespace

int main() {
  sqlite_dialect d;
  const std::string url = "sqlite://" + temp_dir() + "/uvcpp-db-suite.db";

  // 每一次都从**空库**开始：上一条用例留下的表会让 `affected_rows` 这类
  // 断言莫名其妙地不成立。
  std::remove((temp_dir() + "/uvcpp-db-suite.db").c_str());

  int rc = run_suite(url, d);

  std::remove((temp_dir() + "/uvcpp-db-suite.db").c_str());

  std::cout << "-- SQLite 专属 --" << std::endl;
  rc |= test_creates_parent_dirs();
  rc |= test_uint64_overflow_rejected();

  std::cout << (rc == 0 && failures == 0 ? "db_sqlite_func: 全部通过"
                                         : "db_sqlite_func: 有失败")
            << std::endl;
  return (rc == 0 && failures == 0) ? 0 : 1;
}
