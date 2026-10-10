/**
 * @file tests/functional/db_suite.h
 * @brief 数据库模块的**跨后端共用**测试体：同一份断言，三个后端各跑一遍。
 *
 * ## 为什么是一份而不是三份
 *
 * `doc/db-guide.md` 里有一句承诺：**换连接串不该等于换数据库语义**。这句话
 * 只有"同一组断言在三个后端上跑"才能验证 —— 三份各写各的测试，每份都会不
 * 自觉地把自家后端的脾气写成期望值，于是三份全绿而三家行为各不相同。
 *
 * 所以这里的形状是：断言全部在 `run_suite()` 里，后端差异**只**出现在
 * `dialect` 的虚函数里（建表 DDL、占位符写法、有没有原生时间类型）。
 * 方言里出现一个 `if (backend == ...)` 就说明这个差异还没被收敛。
 *
 * ## 断言分三层，别把三层混着写
 *
 *   1. **契约层**（最强）：`uvcpp_db_status` 的具体值、`is_null()`、
 *      `to_text()` 的逐字节相等、blob 逐字节相等。这些是文档里写死的。
 *   2. **语义层**：`to_int64()` / `to_double()` / `to_bool()` 的**值**。
 *      类型标签允许后端不同（SQLite 没有布尔，`INTEGER(0/1)` 就是它的布尔），
 *      但**读出来的值必须一样**。判 `type()` 的断言在这里是错的。
 *   3. **不适用层**：SQLite 没有定点类型、没有 `lastval()`。这类**记为
 *      `not_applicable` 并打印出来**，不记成通过 —— 一条"因为不适用所以
 *      跳过"如果打印不出来，它和"测过了"在日志里长得一模一样。
 *
 * ## 谁用这个头
 *
 * `db_sqlite_func.cpp` / `db_mysql_func.cpp` / `db_pgsql_func.cpp`。本文件是
 * `.h`，所以不会被 `tests/functional/CMakeLists.txt` 的 `*.cpp` 通配捞进去当
 * 成一条用例（那会编出一个没有 `main()` 的目标）。
 */

#ifndef UVCPP_TESTS_FUNCTIONAL_DB_SUITE_H_
#define UVCPP_TESTS_FUNCTIONAL_DB_SUITE_H_

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_pool.h>

namespace uvcpp_db_test {

using uvcpp::uvcpp_db_client;
using uvcpp::uvcpp_db_params;
using uvcpp::uvcpp_db_row;
using uvcpp::uvcpp_db_status;
using uvcpp::uvcpp_db_table;
using uvcpp::uvcpp_db_value;

/// 后端的**全部**差异。多一个虚函数就多一处"三家不一样"，所以这里每加一条
/// 都值得先问一句：这个差异能不能收敛掉。
class dialect {
 public:
  virtual ~dialect() {}

  /// 驱动名，同时也是 `uvcpp_db_drivers()` 里必须出现的那一项。
  virtual const char* name() const = 0;

  /// 第 n 个（从 **1** 数）参数的占位符。SQLite/MySQL 恒为 `?`，
  /// PostgreSQL 是 `$n`。本模块把 SQL **原样**交给驱动，**不换算** —— 所以
  /// 这个函数是方言的一部分，不是"调用方的偏好"。
  virtual std::string ph(size_t n) const = 0;

  /// 除了 `ph()` 那种写法，这个后端还收不收 `$n`。**只有 SQLite 收**（它把
  /// `$1` 当**命名**参数，于是 `?` 写法的 SQL 在它这儿照样能跑）；MySQL 与
  /// PostgreSQL 都不收。默认 false。
  ///
  /// 它存在的理由是一条**量出来的**事实：`query("... WHERE id = ?", {1}, &t)`
  /// 在 SQLite 上跑绿，**不**代表它在 PostgreSQL 上也行 —— 那边报
  /// `PREPARE_FAILED`（`syntax error at end of input`）。所以"绿了"这件事得按
  /// 后端分开说，见证见 `test_placeholder_dialect()`。
  virtual bool accepts_dollar_placeholder() const { return false; }

  /// 一张"每种类型来一列"的表。`e_val` 三家的声明都必须带
  /// `NOT NULL DEFAULT ''` —— 见 `test_empty_string_is_not_null()`。
  virtual std::string create_types_table(const std::string& t) const = 0;

  /// 一张只有自增主键和文本的表，给 `last_insert_id()` 用。
  virtual std::string create_serial_table(const std::string& t) const = 0;

  /// 自增主键的**列定义**（不含列名）。放进 `CREATE TABLE x (id <这里>, ...)`
  /// 用，也是外键子表主键的定义 —— 三家的写法互不相同，而"能不能当主键"
  /// 这件事本身是共同的。
  virtual std::string serial_pk_type() const = 0;

  /// 有没有**原生**时间类型。SQLite 没有（只能存 TEXT），另外两家有。
  virtual bool has_native_temporal() const { return true; }

  /// 金额那一列的**声明**（含列名）。三家的写法不同，而这件事本身是共同的：
  /// 金额列该建成什么。
  ///
  /// SQLite 只能给 `TEXT`（它没有定点类型），于是 `to_text()` 逐字节往返；
  /// MySQL / PG 给 `DECIMAL` / `NUMERIC`，本库把它们读成 **double** —— 精度
  /// 会在这一步丢掉。这正是 `doc/db-guide.md` 里"金额请存 TEXT"那条建议的
  /// 来由，而这条建议**必须**有断言兜着，否则它就是一句没人验过的话。
  virtual std::string decimal_column_decl() const = 0;

  /// 上面那一列是不是原生定点类型。判"读成 double 会不会丢精度"要用它。
  virtual bool decimal_is_native() const { return true; }

  /// `last_insert_id()` 在这个后端上有没有意义。三家都有各自的实现，
  /// 但**语义**不同（见 `test_last_insert_id()` 里的注释）。
  virtual bool supports_last_insert_id() const { return true; }

  /// 这个后端的外键约束是真生效的吗。
  virtual bool enforces_foreign_keys() const { return true; }
};

/// 断言与计数的收口。三种计数分开：**失败**、**通过**、**不适用** —— 只有
/// 第一种让进程非零退出，第二种保证"跑过了"，第三种保证"跳过是看得见的"。
struct harness {
  uvcpp_db_client* db;
  const dialect* d;
  int failed = 0;
  int passed = 0;
  int not_applicable = 0;

  void fail(const std::string& what, const std::string& detail) {
    ++failed;
    std::cerr << "  [FAIL] " << what;
    if (!detail.empty()) std::cerr << " —— " << detail;
    std::cerr << std::endl;
  }

  void check(bool cond, const std::string& what) {
    if (cond) {
      ++passed;
    } else {
      fail(what, std::string());
    }
  }

  void skip(const std::string& what, const std::string& why) {
    ++not_applicable;
    std::cout << "  [N/A ] " << what << " —— " << why << std::endl;
  }

  void eq_text(const std::string& got, const std::string& want,
               const std::string& what) {
    if (got == want) {
      ++passed;
      return;
    }
    // 逐字节比，并把两边都打出来：文本断言失败时最需要的就是"差在哪个字节"。
    fail(what, "期望 [" + printable(want) + "] 实得 [" + printable(got) + "]");
  }

  void eq_int(int64_t got, int64_t want, const std::string& what) {
    if (got == want) {
      ++passed;
      return;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "期望 %lld 实得 %lld",
                  static_cast<long long>(want), static_cast<long long>(got));
    fail(what, buf);
  }

  void eq_double(double got, double want, const std::string& what) {
    // 浮点只判相对误差：三家后端的文本格式化路径不同（MySQL 给 '3.5'、
    // PG 给 '3.5'、SQLite 走 double 列），要求 bit 级相等是在测格式化器，
    // 不是在测数据库。绝对相等留给 `to_text()` 那类断言。
    const double diff = got > want ? got - want : want - got;
    const double scale = want > 1.0 || want < -1.0 ? (want > 0 ? want : -want) : 1.0;
    if (diff / scale <= 1e-12) {
      ++passed;
      return;
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "期望 %.17g 实得 %.17g", want, got);
    fail(what, buf);
  }

  void eq_status(uvcpp_db_status got, uvcpp_db_status want,
                 const std::string& what) {
    if (got == want) {
      ++passed;
      return;
    }
    fail(what, std::string("期望 ") +
                   uvcpp::uvcpp_db_status_name(want) + " 实得 " +
                   uvcpp::uvcpp_db_status_name(got) + "（last_error: " +
                   db->last_error() + "）");
  }

  /// 期望成功。失败时把 `last_error()` 一起打出来 —— 没有它，CI 日志里只有
  /// 一句 "open_failed"，看不出是认证、超时还是库不存在。
  void ok(uvcpp_db_status st, const std::string& what) {
    eq_status(st, uvcpp_db_status::OK, what);
  }

  static std::string printable(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      if (c == '\n') {
        out.append("\\n");
      } else if (c == '\r') {
        out.append("\\r");
      } else if (c == '\t') {
        out.append("\\t");
      } else if (c < 0x20 || c >= 0x7f) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\x%02x", c);
        out.append(buf);
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
    return out;
  }
};

/// 造一张"每种类型一列"的表用的列名。三家共用，好让断言里能写死列序。
/// 命名空间作用域的 `const` 在 C++ 里是**内部链接**，所以这个头被几个 `.cpp`
/// 同时 include 也不会重复定义（放 `inline` 是 C++17 才有的事，本库是 C++11）。
static const char* const kTypesColumns[] = {
    "id",   "i_val", "d_val",  "s_val", "b_val",  "n_val",
    "e_val", "flag_val", "dt_val", "dd_val", "tt_val", "dec_val"};

static const size_t kTypesColumnCount =
    sizeof(kTypesColumns) / sizeof(kTypesColumns[0]);

// ---------------------------------------------------------------------------
// 各组用例
// ---------------------------------------------------------------------------

/// 1. 驱动名、连接串解析、`NO_DRIVER` 与 `BAD_URL` 的分工。
inline void test_driver_and_url(harness& h) {
  h.check(std::string(h.db->driver_name()) == h.d->name(),
          "driver_name 必须是本后端");

  const std::string drivers = uvcpp::uvcpp_db_drivers();
  h.check(drivers.find(h.d->name()) != std::string::npos,
          std::string("uvcpp_db_drivers() 里要有 ") + h.d->name() +
              "（实得：" + drivers + "）");

  // 「这份包没带这个后端」与「这个 scheme 根本不存在」必须是**两个**返回码。
  // 这条断言不能写死某个 scheme：`mysql://` 在一份带 MySQL 的构建里是能开的，
  // 写死它就会在那些树上变成一条假的失败。所以先从 `uvcpp_db_drivers()` 里
  // 反推出**这一份**构建缺哪个后端，再拿那一个当判据。
  h.db->close();
  const char* kCandidates[] = {"sqlite", "mysql", "postgres"};
  const char* missing = nullptr;
  for (size_t i = 0; i < 3; ++i) {
    if (drivers.find(kCandidates[i]) == std::string::npos) {
      missing = kCandidates[i];
      break;
    }
  }
  if (missing != nullptr) {
    // 不真的去连（它本来就没编进来），只判返回码。
    h.eq_status(h.db->open(std::string(missing) + "://u:p@127.0.0.1:1/db"),
                uvcpp_db_status::NO_DRIVER,
                std::string("没编进来的后端（") + missing + "）-> NO_DRIVER");
  } else {
    h.skip("没编进来的后端 -> NO_DRIVER", "这一份构建三个后端都在");
  }

  // 反过来：谁都没支持过的 scheme 是**调用方写错了**，报 BAD_URL。
  h.eq_status(h.db->open("oracle://u:p@127.0.0.1:1521/x"),
              uvcpp_db_status::BAD_URL, "不认识的 scheme -> BAD_URL");

  h.eq_status(h.db->open("not a url at all"),
              uvcpp_db_status::BAD_URL, "没有 scheme -> BAD_URL");
  h.eq_status(h.db->open("mysql://u@host:notaport/db"),
              uvcpp_db_status::BAD_URL, "端口不是数字 -> BAD_URL");

  h.check(!h.db->is_open(), "open 失败之后 is_open() 必须是 false");
  h.check(!h.db->last_error().empty(), "open 失败必须留下 last_error");
}

/// 2. 建表 → 参数化插入 → 查回来。这一组是整套的地基。
inline void test_ddl_insert_select(harness& h, const std::string& table) {
  h.ok(h.db->execute("DROP TABLE IF EXISTS " + table), "DROP TABLE IF EXISTS");

  h.ok(h.db->execute(h.d->create_types_table(table)), "建表");

  const std::string ins = "INSERT INTO " + table +
                          " (i_val, d_val, s_val, b_val, n_val, e_val,"
                          "  flag_val, dt_val, dd_val, tt_val, dec_val)"
                          " VALUES (" +
                          h.d->ph(1) + ", " + h.d->ph(2) + ", " + h.d->ph(3) +
                          ", " + h.d->ph(4) + ", " + h.d->ph(5) + ", " +
                          h.d->ph(6) + ", " + h.d->ph(7) + ", " + h.d->ph(8) +
                          ", " + h.d->ph(9) + ", " + h.d->ph(10) + ", " +
                          h.d->ph(11) + ")";

  const std::string blob_bytes("\x00\x01\xfe\xff", 4);
  int64_t affected = -1;
  h.ok(h.db->execute(ins,
                     {int64_t(42), 3.5, std::string("hello"),
                      uvcpp_db_value::blob(blob_bytes), uvcpp_db_value(),
                      std::string(""), int64_t(1), std::string("2026-10-10 12:34:56"),
                      std::string("2026-10-10"), std::string("12:34:56"),
                      std::string("1234.56")},
                     &affected),
       "参数化 INSERT");
  h.eq_int(affected, 1, "INSERT 的 affected_rows 是 1");

  uvcpp_db_table t;
  h.ok(h.db->query("SELECT id, i_val, d_val, s_val, b_val, n_val, e_val,"
                   " flag_val, dt_val, dd_val, tt_val, dec_val FROM " +
                       table,
                   &t),
       "整表 SELECT");

  // 查询失败时**必须**在这里停住：下面全是对第 0 行的断言，空表上取
  // `rows()[0]` 是越界（这一段曾经真的段错误过，把后面十几条断言一起带走了 ——
  // 一条前置失败变成进程崩溃，日志里就看不出是哪一条断言先坏的）。
  if (t.row_count() != 1) {
    h.fail("整表 SELECT 之后必须正好一行", "没办法继续判后面那些列");
    return;
  }
  h.eq_int(static_cast<int64_t>(t.row_count()), 1, "一行");
  h.eq_int(static_cast<int64_t>(t.column_count()), 12, "十二列");

  // 列名与列序：逐字节比。列序错了 `at(row, c)` 那套就全错位了。
  for (size_t c = 0; c < kTypesColumnCount && c < t.columns().size(); ++c) {
    h.eq_text(t.columns()[c], kTypesColumns[c], "列名与列序");
  }

  const uvcpp_db_row& row = t.rows()[0];

  // 契约层：值域。
  h.eq_int(row["i_val"].to_int64(), 42, "INT 往返");
  h.eq_double(row["d_val"].to_double(), 3.5, "DOUBLE 往返");
  h.eq_text(row["s_val"].to_text(), "hello", "TEXT 往返");
  // 金额列：**值**必须一样（三家都读得出 1234.56）。**类型**允许不同 ——
  // SQLite 是 TEXT、另外两家是 double，见 `decimal_column_decl()` 的注释。
  h.eq_double(row["dec_val"].to_double(), 1234.56, "金额列读成 1234.56");
  if (!h.d->decimal_is_native()) {
    h.eq_text(row["dec_val"].to_text(), "1234.56",
              "非原生定点列（TEXT）能逐字节往返，不丢精度");
  } else {
    h.check(row["dec_val"].type() == uvcpp::uvcpp_db_type::DOUBLE,
            "原生定点列被读成 DOUBLE（有意的精度取舍，文档里写明）");
  }

  // 契约层：blob 逐字节。含 0x00 与 0xFF —— 这两枚正是"当 C 字符串处理"会
  // 炸的两个字节（前者截断、后者在有符号 char 下变负）。
  h.eq_text(row["b_val"].bytes(), blob_bytes, "BLOB 逐字节往返（含 0x00 / 0xFF）");
  h.eq_int(static_cast<int64_t>(row["b_val"].size()), 4, "BLOB 长度");

  // 语义层：类型标签允许不同（SQLite 的布尔就是 INTEGER），值必须一样。
  h.check(row["flag_val"].to_bool(), "BOOL 列读成 true");
  h.eq_int(row["flag_val"].to_int64(), 1, "BOOL 列也读得出 1");

  h.check(row["n_val"].is_null(), "真 NULL 读回来还是 NULL");
}

/// 3. **空串不是 NULL。**
///
/// 这是原库的一个真缺陷（`SqlValue` 把空串折成 NULL），也是迁移过来之后最
/// 容易悄悄复发的一处：三家的绑定路径各不相同（SQLite 的 `SQLITE_TRANSIENT`、
/// MySQL 的 `MYSQL_BIND` 空缓冲、PG 的文本参数），**任何一条**把长度 0 当成
/// "没给值"就会退化成 NULL。
///
/// 判据用**双向**：空串列必须是 `""` 且 `!is_null()`，NULL 列必须 `is_null()`。
/// 只判一边的话，"全读成 NULL"或"全读成空串"都能过一半。
inline void test_empty_string_is_not_null(harness& h, const std::string& table) {
  uvcpp_db_table t;
  h.ok(h.db->query("SELECT n_val, e_val FROM " + table, &t), "取空串 / NULL 两列");
  if (t.row_count() != 1) {
    h.fail("空串 === NULL 一测：没能拿到那一行", "row_count 不是 1");
    return;
  }
  const uvcpp_db_row& row = t.rows()[0];
  h.check(row["n_val"].is_null(), "NULL 列 is_null()");
  h.check(!row["e_val"].is_null(), "空串列 **不是** NULL");
  h.eq_text(row["e_val"].to_text(), "", "空串列的文本是空串");
  h.check(row["e_val"].type() != uvcpp::uvcpp_db_type::NIL, "空串列的类型不是 NIL");
  h.eq_int(row["e_val"].size(), 0, "空串列长度 0");

  // 反向：拿空串当**参数**发回去，也不能变成 NULL。
  uvcpp_db_table t2;
  h.ok(h.db->query("SELECT s_val FROM " + table + " WHERE e_val = " + h.d->ph(1),
                   {std::string("")}, &t2),
       "用空串作参数能查到（不是 IS NULL）");
  h.eq_int(static_cast<int64_t>(t2.row_count()), 1, "空串参数命中了那一行");
}

/// 4. 长文本与长 blob：驱动层的缓冲区截断就死在这一条上。
///
/// MySQL 的预处理协议会在缓冲不够时给 `MYSQL_DATA_TRUNCATED` 并把 `error`
/// 标志置位 —— 原库直接忽略它，于是长文本**静默截断**。这里用 3000 字节
/// （大于任何默认的 255 / 1024 缓冲）把它钉住。
inline void test_long_values(harness& h, const std::string& table) {
  std::string long_text;
  for (int i = 0; i < 300; ++i) long_text.append("0123456789");
  std::string long_blob;
  for (int i = 0; i < 300; ++i) long_blob.push_back(static_cast<char>(i % 256));

  const std::string ins = "INSERT INTO " + table + " (s_val, b_val) VALUES (" +
                          h.d->ph(1) + ", " + h.d->ph(2) + ")";
  h.ok(h.db->execute(ins, {long_text, uvcpp_db_value::blob(long_blob)}),
       "插入 3000 字节文本 / blob");

  uvcpp_db_table t;
  h.ok(h.db->query("SELECT s_val, b_val FROM " + table +
                       " WHERE s_val = " + h.d->ph(1),
                   {long_text}, &t),
       "按 3000 字节文本查回来");
  if (t.row_count() != 1) {
    h.fail("长文本往返", "没查到那一行");
    return;
  }
  h.eq_int(static_cast<int64_t>(t.rows()[0]["s_val"].size()), 3000,
           "长文本没有被截断");
  h.check(t.rows()[0]["s_val"].bytes() == long_text, "长文本逐字节相等");
  h.check(t.rows()[0]["b_val"].bytes() == long_blob, "长 blob 逐字节相等");

  h.ok(h.db->execute("DELETE FROM " + table + " WHERE s_val = " + h.d->ph(1),
                     {long_text}),
       "清掉长文本那一行");
}

/// 5. 时间三类：能存进去、能原样读回来。
///
/// 期望值一律用 `to_text()` 逐字节判 —— 本库对三类的表示统一是
/// `YYYY-MM-DD` / `HH:MM:SS` / `YYYY-MM-DD HH:MM:SS`，这是三家都能无损往返
/// 的形状（SQLite 本来就没有日期类型，PG 的文本格式恰好就是这个）。
inline void test_temporal(harness& h, const std::string& table) {
  uvcpp_db_table t;
  h.ok(h.db->query("SELECT dt_val, dd_val, tt_val FROM " + table, &t),
       "取时间三类");
  if (t.row_count() != 1) {
    h.fail("时间往返", "没拿到那一行");
    return;
  }
  const uvcpp_db_row& row = t.rows()[0];
  h.eq_text(row["dt_val"].to_text(), "2026-10-10 12:34:56", "DATETIME 往返");
  h.eq_text(row["dd_val"].to_text(), "2026-10-10", "DATE 往返");
  h.eq_text(row["tt_val"].to_text(), "12:34:56", "TIME 往返");

  if (h.d->has_native_temporal()) {
    // 有原生类型时，值本身也要带上类型标签；否则调用方无法把日期和普通
    // 字符串分开（这正是"日期列在读出来之后还能当日期用"的前提）。
    h.check(row["dd_val"].type() == uvcpp::uvcpp_db_type::DATE,
            "原生 DATE 列的类型标签是 DATE");
    h.check(row["dt_val"].type() == uvcpp::uvcpp_db_type::DATETIME,
            "原生 DATETIME 列的类型标签是 DATETIME");
  } else {
    h.skip("原生时间类型标签", "这个后端没有原生时间类型，只能按 TEXT 存");
  }

  // 时间列**当参数发回去**，走的是绑定那条路（与读结果的格式化解耦）。
  uvcpp_db_table t2;
  h.ok(h.db->query("SELECT COUNT(*) AS c FROM " + table + " WHERE dd_val = " +
                       h.d->ph(1),
                   {std::string("2026-10-10")}, &t2),
       "日期当参数发回去");
  if (t2.row_count() == 1) {
    h.eq_int(t2.rows()[0][0].to_int64(), 1, "按日期参数命中一行");
  } else {
    h.fail("按日期参数命中一行", "没拿到计数结果");
  }
}

/// 6. `last_insert_id()`。三家的语义**不一样**，这里要把不一样的地方说清楚，
/// 而不是假装一样：
///
///   * SQLite：`sqlite3_last_insert_rowid()`，**这条连接**上最近一次成功
///     INSERT 的 rowid。
///   * MySQL：`mysql_insert_id()`，同上，是**连接级**的。
///   * PostgreSQL：`SELECT lastval()`，是**会话级**的最近一次 `nextval()`，
///     所以它甚至能被 `SELECT nextval(...)` 单独改掉。
///
/// 共同点是这一条判据：**插一行之后，拿到的 id 能反过来查到那一行**。
/// 用"能查到"而不是"等于 1"，是因为三家的起始值不同（PG 的序列从 1 开始、
/// 但被 DROP/CREATE 过就不一定）。
inline void test_last_insert_id(harness& h, const std::string& table) {
  if (!h.d->supports_last_insert_id()) {
    h.skip("last_insert_id", "这个后端没有该语义");
    return;
  }
  h.ok(h.db->execute("DROP TABLE IF EXISTS " + table), "DROP serial 表");
  h.ok(h.db->execute(h.d->create_serial_table(table)), "建自增表");

  uvcpp_db_value id;
  h.ok(h.db->insert("INSERT INTO " + table + " (name) VALUES (" + h.d->ph(1) + ")",
                    {"first"}, &id),
       "insert() 返回自增 id");
  h.check(!id.is_null(), "insert() 拿到的 id 不是 NULL");

  uvcpp_db_table t;
  h.ok(h.db->query("SELECT name FROM " + table + " WHERE id = " + h.d->ph(1),
                   {id}, &t),
       "拿 id 反查那一行");
  if (t.row_count() == 1) {
    h.eq_text(t.rows()[0]["name"].to_text(), "first", "反查到的就是刚插的那行");
  } else {
    h.fail("反查到刚插的那行", "row_count 不是 1");
  }

  // 再插一行：id 必须变，且比上一个大（自增的**意义**就在这）。
  uvcpp_db_value id2;
  h.ok(h.db->insert("INSERT INTO " + table + " (name) VALUES (" + h.d->ph(1) + ")",
                    {"second"}, &id2),
       "第二次 insert()");
  h.check(id2.to_int64() > id.to_int64(), "第二次的 id 更大");
}

/// 7. 事务：回滚要真的回滚，提交要真的留下来。
///
/// 原库的 `DatabaseManager` 在取连接前 `mysql_ping`、断了就静默重连 —— 那样
/// 一次"看起来成功"的 UPDATE 可能在事务被丢掉之后才返回。所以"事务边界是真
/// 的"这件事必须有一条断言，否则那条静默重连的路径回来了也没人发现。
inline void test_transaction(harness& h, const std::string& table) {
  h.ok(h.db->execute("DROP TABLE IF EXISTS " + table), "DROP tx 表");
  h.ok(h.db->execute(h.d->create_serial_table(table)), "建 tx 表");

  // 回滚
  h.ok(h.db->begin(), "BEGIN");
  h.ok(h.db->execute("INSERT INTO " + table + " (name) VALUES (" + h.d->ph(1) + ")",
                     {"rolled-back"}),
       "事务内 INSERT");
  h.ok(h.db->rollback(), "ROLLBACK");

  uvcpp_db_table t;
  h.ok(h.db->query("SELECT COUNT(*) AS c FROM " + table, &t), "回滚后计数");
  // 用 `at()` 而不是 `rows()[0]`：查询万一失败，`rows()` 是空的，
  // `rows()[0]` 越界会把这个进程直接带走（本套件真的这么崩过一次）。
  // `at()` 越界返回静态 NULL —— 这是 `uvcpp_db_table` 写在头里的契约。
  h.eq_int(t.at(0, 0).to_int64(), 0, "ROLLBACK 之后那一行不存在");

  // 提交
  h.ok(h.db->begin(), "BEGIN（第二次）");
  h.ok(h.db->execute("INSERT INTO " + table + " (name) VALUES (" + h.d->ph(1) + ")",
                     {"committed"}),
       "事务内 INSERT（第二次）");
  h.ok(h.db->commit(), "COMMIT");

  uvcpp_db_table t2;
  h.ok(h.db->query("SELECT name FROM " + table, &t2), "提交后计数");
  h.eq_int(static_cast<int64_t>(t2.row_count()), 1, "COMMIT 之后那一行在");
  h.eq_text(t2.at(0, "name").to_text(), "committed", "提交的是那一行");

  // 提交之后再回滚：必须报错，而不是"悄悄再开一个事务"。
  // 三家对"没有活动事务时 ROLLBACK"的反应不同（有的是警告不是错误），
  // 所以这里**不**判具体状态码，只判"库没崩、连接还活着、数据还在"。
  const uvcpp_db_status st = h.db->rollback();
  h.check(uvcpp::uvcpp_db_ok(st) || st == uvcpp_db_status::EXEC_FAILED,
          "空事务上 ROLLBACK 要么成功要么明确报错");
  h.check(h.db->is_open(), "空事务上 ROLLBACK 之后连接还活着");
}

/// 8. 外键约束：真的生效吗。
///
/// SQLite 的外键默认**是关的**（所以驱动 `open()` 里那句
/// `PRAGMA foreign_keys = ON` 是承重的）。这一条就是它的见证：关着的话插一条
/// 悬空外键会成功，而"成功"是这里最不该出现的结果。
inline void test_foreign_key(harness& h) {
  if (!h.d->enforces_foreign_keys()) {
    h.skip("外键约束", "这个后端不强制外键");
    return;
  }
  const std::string parent = "uvcpp_fk_parent";
  const std::string child = "uvcpp_fk_child";
  h.ok(h.db->execute("DROP TABLE IF EXISTS " + child), "DROP 子表");
  h.ok(h.db->execute("DROP TABLE IF EXISTS " + parent), "DROP 父表");
  h.ok(h.db->execute(h.d->create_serial_table(parent)), "建父表");
  h.ok(h.db->execute("CREATE TABLE " + child + " (id " +
                         h.d->serial_pk_type() +
                         ", parent_id BIGINT, FOREIGN KEY (parent_id)"
                         " REFERENCES " +
                         parent + "(id))"),
       "建带外键的子表");

  const uvcpp_db_status st = h.db->execute(
      "INSERT INTO " + child + " (parent_id) VALUES (999999)");
  h.check(!uvcpp::uvcpp_db_ok(st),
          "插入悬空外键必须失败（SQLite 若 PRAGMA 没打开就会成功）");

  h.ok(h.db->execute("DROP TABLE " + child), "清理子表");
  h.ok(h.db->execute("DROP TABLE " + parent), "清理父表");
}

/// 9. 模式自省：表名列表与列定义。
inline void test_schema(harness& h, const std::string& table) {
  std::vector<std::string> names;
  h.ok(h.db->table_names(&names), "table_names()");
  bool found = false;
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] == table) found = true;
  }
  h.check(found, "table_names() 里有刚建的表");

  uvcpp_db_table schema;
  h.ok(h.db->table_schema(table, &schema), "table_schema()");
  h.check(schema.row_count() >= 12, "模式里至少 12 列");

  // 归一化过的四列：name / type / nullable / key。列名本身就是契约
  // （`uvcpp_db.h` 里写着），三家必须一样。
  const std::vector<std::string>& cols = schema.columns();
  h.check(!cols.empty() && cols[0] == "name", "模式第一列是 name");
  bool has_key = false, has_nullable = false;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (cols[i] == "key") has_key = true;
    if (cols[i] == "nullable") has_nullable = true;
  }
  h.check(has_key, "模式里有 key 列");
  h.check(has_nullable, "模式里有 nullable 列");

  // 主键列必须被认出来（PRI_KEY_FLAG / PRIMARY KEY 约束 / PRAGMA 三条路
  // 各写各的，这里判的是三条路给出同一个答案）。
  bool pk_seen = false;
  for (size_t r = 0; r < schema.row_count(); ++r) {
    const uvcpp_db_row& row = schema.rows()[r];
    if (row["name"].to_text() == "id" && !row["key"].to_text().empty()) {
      pk_seen = true;
    }
  }
  h.check(pk_seen, "id 列被标成主键");
}

/// 10. 错误路径。**每一条都必须"不执行任何语句"地失败**，而不是执行一半。
inline void test_errors(harness& h, const std::string& table) {
  const std::string bad_sql = "SELCT * FROM " + table;
  uvcpp_db_table t;
  h.eq_status(h.db->query(bad_sql, &t), uvcpp_db_status::PREPARE_FAILED,
              "语法错的 SQL -> PREPARE_FAILED");

  uvcpp_db_table t2;
  h.eq_status(h.db->query("SELECT * FROM uvcpp_no_such_table_xyz", &t2),
              uvcpp_db_status::PREPARE_FAILED, "不存在的表 -> PREPARE_FAILED");

  // 参数个数对不上：三家必须在**本地**或**服务端**同一处拒绝，且报同一个码。
  // 不统一的话，调用方就没法写一句 `if (st == BIND_FAILED) 修参数`。
  uvcpp_db_table t3;
  h.eq_status(h.db->query("SELECT * FROM " + table + " WHERE id = " + h.d->ph(1),
                          {}, &t3),
              uvcpp_db_status::BIND_FAILED, "少给参数 -> BIND_FAILED");
  h.eq_status(h.db->query("SELECT * FROM " + table + " WHERE id = " + h.d->ph(1),
                          {int64_t(1), int64_t(2)}, &t3),
              uvcpp_db_status::BIND_FAILED, "多给参数 -> BIND_FAILED");

  // 空 SQL：属于用法错误，不该走到驱动里去。
  h.check(h.db->query("", &t3) != uvcpp_db_status::OK, "空 SQL 非法");
  h.check(h.db->execute("") != uvcpp_db_status::OK, "空 SQL（execute）非法");
  // 空的结果集指针同理。
  h.check(h.db->query("SELECT 1", static_cast<uvcpp_db_table*>(nullptr)) !=
              uvcpp_db_status::OK,
          "空结果集指针非法");

  // 关掉连接之后，需要连接的操作必须报 NOT_CONNECTED，而不是崩。
  h.db->close();
  h.check(!h.db->is_open(), "close 之后 is_open() 是 false");
  h.eq_status(h.db->query("SELECT 1", &t3), uvcpp_db_status::NOT_CONNECTED,
              "关掉之后 query -> NOT_CONNECTED");
  h.eq_status(h.db->execute("SELECT 1"), uvcpp_db_status::NOT_CONNECTED,
              "关掉之后 execute -> NOT_CONNECTED");
  h.check(h.db->last_insert_id().is_null(),
          "关掉之后 last_insert_id() 是 NULL（不是崩）");
}

/// 11. 转义：把一段**装着引号与反斜杠**的文本原样存进去、原样读回来。
///
/// 判据是"往返相等"，不是"转义后的串长什么样" —— 后者的期望值各后端不同
/// （MySQL 用反斜杠、SQLite/PG 用双写单引号），而调用方真正需要的是
/// 「我拼出来的 SQL 不会因为我这段文本而变形」。
inline void test_escaping(harness& h, const std::string& table) {
  h.ok(h.db->execute("DROP TABLE IF EXISTS " + table), "DROP escape 表");
  h.ok(h.db->execute(h.d->create_serial_table(table)), "建 escape 表");

  const std::string nasty = "O'Brien \"quote\" \\back\\ ; DROP TABLE x --";
  const std::string sql = "INSERT INTO " + table + " (name) VALUES ('" +
                          h.db->escape(nasty) + "')";
  h.ok(h.db->execute(sql), "手工拼串 + escape()");

  uvcpp_db_table t;
  h.ok(h.db->query("SELECT name FROM " + table + " WHERE name = " + h.d->ph(1),
                   {nasty}, &t),
       "查回那段文本");
  if (t.row_count() == 1) {
    h.eq_text(t.rows()[0]["name"].to_text(), nasty, "转义文本逐字节往返");
  } else {
    h.fail("转义文本逐字节往返", "没查到（说明拼出来的 SQL 已经变形）");
  }

  // 标识符转义：带引号/反引号/大写的表名。
  const std::string odd = "uvcpp_odd\"name";
  const std::string quoted = h.db->escape_identifier(odd);
  h.check(!quoted.empty(), "escape_identifier() 不是空串");
  h.ok(h.db->execute("DROP TABLE IF EXISTS " + quoted), "用转义后的怪表名 DROP");
  h.ok(h.db->execute("CREATE TABLE " + quoted + " (id " + h.d->serial_pk_type() +
                         ", v TEXT)"),
       "用转义后的怪表名建表");
  std::vector<std::string> names;
  h.ok(h.db->table_names(&names), "怪表名后 table_names()");
  bool found = false;
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] == odd) found = true;
  }
  h.check(found, "怪表名原样出现在 table_names() 里");
  h.ok(h.db->execute("DROP TABLE " + quoted), "清理怪表名");

  h.ok(h.db->execute("DROP TABLE " + table), "清理 escape 表");
}

/// 12. 结果集的行/列访问器：越界与不存在的列名都得给出静态 NULL，不是崩。
inline void test_row_access(harness& h, const std::string& table) {
  h.ok(h.db->execute("DROP TABLE IF EXISTS " + table), "DROP access 表");
  h.ok(h.db->execute(h.d->create_serial_table(table)), "建 access 表");
  h.ok(h.db->execute("INSERT INTO " + table + " (name) VALUES (" + h.d->ph(1) + ")",
                     {"a"}),
       "插 a");
  h.ok(h.db->execute("INSERT INTO " + table + " (name) VALUES (" + h.d->ph(1) + ")",
                     {"b"}),
       "插 b");

  uvcpp_db_table t;
  h.ok(h.db->query("SELECT id, name FROM " + table + " ORDER BY id", &t),
       "取两行");
  if (t.row_count() != 2) {
    h.fail("取两行", "行数不是 2，后面的访问器断言没法判");
    return;
  }

  h.eq_int(static_cast<int64_t>(t.column_index("name")), 1, "column_index(name) == 1");
  h.eq_int(static_cast<int64_t>(t.column_index("nope")), -1,
           "不存在的列名 -> column_index 返回 -1");

  h.eq_text(t.rows()[0]["name"].to_text(), "a", "第一行按列名取");
  h.eq_text(t.rows()[1]["name"].to_text(), "b", "第二行按列名取");
  h.eq_text(t.at(0, 1).to_text(), "a", "at(0,1) 与 row[0][name] 是同一个格子");
  h.eq_text(t.value("name").to_text(), "a", "value(name) 给第一行");

  h.check(t.rows()[0].has("name"), "has(name)");
  h.check(!t.rows()[0].has("nope"), "!has(nope)");

  // 越界 / 不存在：返回静态 NULL，**可以安全地绑到 const& 上**。
  const uvcpp_db_value& oob = t.at(99, 0);
  h.check(oob.is_null(), "行号越界 -> NULL 值");
  const uvcpp_db_value& oob2 = t.at(0, 99);
  h.check(oob2.is_null(), "列号越界 -> NULL 值");
  const uvcpp_db_value& oob3 = t.rows()[0]["nope"];
  h.check(oob3.is_null(), "不存在的列名 -> NULL 值");
  h.eq_int(static_cast<int64_t>(t.row_count()), 2, "越界访问之后行数没变");

  // 零行的 SELECT：列名还是要给出来（否则 `t.columns()` 为空，调用方连
  // "这条查询本来会返回哪几列"都不知道）。
  uvcpp_db_table empty;
  h.ok(h.db->query("SELECT id, name FROM " + table + " WHERE id < 0", &empty),
       "零行的 SELECT");
  h.eq_int(static_cast<int64_t>(empty.row_count()), 0, "零行");
  h.eq_int(static_cast<int64_t>(empty.column_count()), 2, "零行也有两列列名");
  h.eq_text(empty.columns()[1], "name", "零行的列名也是对的");
  h.check(empty.empty(), "empty() 为真");

  h.ok(h.db->execute("DROP TABLE " + table), "清理 access 表");
}

/// 13. JSON / CSV 导出：逐字节判，因为它们是给外部（HTTP 响应、文件）吃的，
/// 差一个空格都算改格式。
inline void test_export(harness& h, const std::string& table) {
  // 用 `types` 表里那一行（`s_val='hello'`、`n_val` 是 NULL）当素材。
  // **不**用 `SELECT ? AS a, ? AS b`：PostgreSQL 在单选列表里遇到无上下文
  // 的 `$1` 会直接报 "could not determine data type of parameter $1" —— 那是
  // PG 的规矩，不是本库的，但写进共用套件就会变成一条"PG 特有失败"。
  uvcpp_db_table t;
  h.ok(h.db->query("SELECT s_val, n_val FROM " + table, &t),
       "造一个 (hello, NULL) 的结果集");
  t.set_table_name("t");

  const std::string want_json =
      "{\n"
      "  \"table\": \"t\",\n"
      "  \"columns\": [\n"
      "    \"s_val\",\n"
      "    \"n_val\"\n"
      "  ],\n"
      "  \"rows\": [\n"
      "    {\n"
      "      \"s_val\": \"hello\",\n"
      "      \"n_val\": null\n"
      "    }\n"
      "  ]\n"
      "}";
  h.eq_text(t.to_json(), want_json, "to_json() 逐字节");
  h.eq_text(t.to_csv(), "s_val,n_val\nhello,NULL\n",
            "to_csv() 逐字节（NULL 写成 NULL）");

  // 零行也要是合法 JSON：`"rows": []` 而不是 `"rows": [`。
  uvcpp_db_table empty;
  h.ok(h.db->query("SELECT s_val FROM " + table + " WHERE 1 = 0", &empty),
       "造一个零行结果集");
  empty.set_table_name("e");
  const std::string ej = empty.to_json();
  h.check(ej.find("\"rows\": []") != std::string::npos,
          "零行 to_json 是 \"rows\": []");
  h.check(ej.size() > 3 && ej[ej.size() - 1] == '}', "零行 to_json 以 } 收尾");
}

/// 14. `uvcpp_db_open()` 工厂 + `timeout_ms` 往返 + 日志回调。
inline void test_factory_and_options(harness& h, const std::string& url) {
  std::string err;
  uvcpp_db_status st = uvcpp_db_status::OK;
  std::unique_ptr<uvcpp_db_client> c = uvcpp::uvcpp_db_open(url, &st, &err);
  h.check(c.get() != nullptr, "uvcpp_db_open 成功（err: " + err + "）");
  if (!c) return;
  h.check(c->is_open(), "工厂造出来的 client 是打开的");
  h.eq_text(c->url(), url, "url() 原样回显");
  h.ok(c->ping(), "ping() 成功");
  h.ok(c->reconnect(), "reconnect() 成功");

  c->set_timeout_ms(1234);
  h.eq_int(c->timeout_ms(), 1234, "timeout_ms 往返");
  // 时间上限要真的下发到驱动层（`SET SESSION max_execution_time` /
  // `statement_timeout` / `sqlite3_busy_timeout`），但那个效果在一条快查询上
  // 看不出来 —— 这里只判"设完之后还能正常查询"，不假装测了超时。
  uvcpp_db_table t;
  h.ok(c->query("SELECT 1 AS one", &t), "改超时之后照样能查");

  int log_lines = 0;
  c->set_log([&log_lines](const std::string&) { ++log_lines; });
  uvcpp_db_table t2;
  c->query("SELECT 1 AS one", &t2);
  h.check(log_lines > 0, "set_log 之后每次查询都留一行日志");
}

/// 连接池。**三家共用同一组断言** —— 池子是纯本库的东西（借还、复用、超时），
/// 底下是什么数据库不该改变它的行为。任何"某个后端不一样"的断言出现在这里，
/// 都说明池子漏了一层抽象。
///
/// 超时一律给小的（几十毫秒）：这一组要证的是"到点会醒"，不是"能等多久"。
inline void test_pool(harness& h, const std::string& url) {
  std::cout << "-- 连接池 --" << std::endl;

  uvcpp::uvcpp_db_pool pool;
  h.eq_status(pool.init(url, 1, 2), uvcpp_db_status::OK, "pool.init(min=1, max=2)");
  h.check(pool.size() == 1, "init 之后立刻就有 1 条（不是懒开：open 的钱摊在 init 上）");
  h.check(pool.idle() == 1 && pool.in_use() == 0, "那条在空闲表里，没有在外借");
  h.check(pool.max_size() == 2 && pool.min_size() == 1, "上下限记的是传进去的数");
  h.check(pool.created_total() == 1, "created_total == 1（只开了 min 条）");

  // ---- 代借代还：一次调用借一条、回来之前还掉 ----
  uvcpp_db_table t;
  h.eq_status(pool.query("SELECT 1 AS one", &t), uvcpp_db_status::OK, "pool.query");
  h.eq_int(static_cast<int64_t>(t.row_count()), 1, "结果集拿回来了");
  h.check(pool.in_use() == 0, "代借代还之后没有留在外面的连接");
  h.check(pool.reused_total() >= 1, "这一条是复用来的（reused_total 涨了）");
  h.check(pool.created_total() == 1, "没有为这一次查询再开连接");

  // ---- 借还：借到还之间这条连接**归调用方独占**，事务只能这么写 ----
  uvcpp_db_client* c1 = pool.acquire(2000);
  h.check(c1 != nullptr, "acquire 借到一条");
  uvcpp_db_client* c2 = pool.acquire(2000);
  h.check(c2 != nullptr && c2 != c1, "第二条 acquire 开了一条新的（不是把同一条借两次）");
  h.check(pool.size() == 2 && pool.in_use() == 2, "两条都在外借");
  h.check(pool.created_total() == 2, "created_total 涨到 2");

  // 到上限了：第三条只能等 —— 给 60 ms，必须**醒过来**并交回 nullptr。
  uvcpp_db_client* c3 = pool.acquire(60);
  h.check(c3 == nullptr, "max 处再借：超时后拿到 nullptr（不是挂住）");
  h.check(!pool.last_error().empty(), "借不出时 last_error() 说得出来为什么");
  h.check(pool.size() == 2, "借不出不会把池子撑过 max");

  // 事务钉在**同一条**借出的连接上。
  h.eq_status(c1->begin(), uvcpp_db_status::OK, "借出的连接上 begin");
  h.eq_status(c1->execute("CREATE TABLE IF NOT EXISTS uvcpp_db_pool_probe (v INTEGER)"),
              uvcpp_db_status::OK, "借出的连接上建表");
  h.eq_status(c1->rollback(), uvcpp_db_status::OK, "借出的连接上 rollback");

  pool.release(c1);
  pool.release(c2);
  h.check(pool.in_use() == 0 && pool.idle() == 2, "还回去之后两条都回到空闲表");
  h.eq_status(pool.ping(), uvcpp_db_status::OK, "还回去的连接还能用");

  // ---- 重复 / 陌生指针都是 no-op，不是崩溃 ----
  pool.release(c1);
  h.check(pool.in_use() == 0, "重复 release 是 no-op");
  pool.release(reinterpret_cast<uvcpp_db_client*>(&pool));
  h.check(pool.size() == 2, "release 一个不属于本池的指针不会动池子");

  // ---- 回收：min 是保底 ----
  h.eq_int(static_cast<int64_t>(pool.close_idle()), 1, "close_idle 把空闲的收到 min");
  h.check(pool.size() == 1 && pool.idle() == 1, "收到 min 就停手");
  h.check(pool.created_total() == 2, "回收之后再借会复用那条，不会重开");

  // ---- 关了之后借不出 ----
  pool.close();
  h.check(pool.acquire(10) == nullptr, "close 之后 acquire 拿不到");
  h.eq_status(pool.query("SELECT 1", &t), uvcpp_db_status::MISUSE,
              "close 之后再 query 报 MISUSE（是「没 init」，不是「连不上」）");

  // ---- 上限/保底的夹取 ----
  uvcpp::uvcpp_db_pool p2;
  h.eq_status(p2.init(url, 5, 2), uvcpp_db_status::OK, "min > max 时夹到 max");
  h.check(p2.size() == 2 && p2.max_size() == 2, "夹取之后确实只开了 2 条");
  h.eq_status(p2.init(url, 1, 0), uvcpp_db_status::MISUSE, "max == 0 是 MISUSE");
}

/// 占位符是**方言**的 —— 本模块把 SQL **原样**交给驱动，**不换算**。
/// （编号只到 14，连接池这一组起就不再编号了，这里跟它一致。）
///
/// 这一条是整个套件里**唯一一条反向判据**：别处都是"对的写法要能跑"，这里是
/// "**错的**写法必须失败"。为什么值得单开一条：其它组一律走 `dialect::ph()`，
/// 它**永远**写不出错的那种写法，所以"驱动会自己把 `?` 换算成 `$1..$n`"这句话
/// 可以在文档里活很久，而没有一个断言反对它 —— 代价由调用方付：一段在 SQLite
/// 上跑绿的 `?`，搬到 PostgreSQL 上是 `PREPARE_FAILED`。
///
/// 三家实测（本机，真服务端；"另一种写法"都**带够参数**，免得报错其实来自
/// "参数个数对不上"而不是"写法不对"）：
///
/// ```
/// PG     `?`  -> prepare_failed（syntax error at end of input）
/// PG     `$1` -> ok
/// MySQL  `?`  -> ok
/// MySQL  `$1` -> prepare_failed（Unknown column '$1' in 'where clause'）
/// SQLite `?`  -> ok
/// SQLite `$1` -> ok          <- 当**命名**参数收下，这就是那个坑
/// ```
inline void test_placeholder_dialect(harness& h) {
  std::cout << "-- 占位符是方言的 --" << std::endl;

  const std::string table = "uvcpp_db_ph";
  h.db->execute("DROP TABLE IF EXISTS " + table);
  h.ok(h.db->execute("CREATE TABLE " + table + " (id INTEGER, name TEXT)"),
       "建占位符用例的表");

  const std::string own = h.d->ph(1);

  // 用**方言自己的**写法插入（PG 上这一步就是 `$1` / `$2`）。两个号都用到，
  // 所以下面断言①取回了正确的行，也就同时证明了"编号按实参次序、从 1 起"。
  h.ok(h.db->execute("INSERT INTO " + table + " (id, name) VALUES (" +
                         h.d->ph(1) + ", " + h.d->ph(2) + ")",
                     {int64_t(11), std::string("ph")}),
       "用方言自己的写法（" + h.d->ph(1) + " / " + h.d->ph(2) + "）插入一行");

  // ① 方言自己的写法：不光要能跑，还要跑**对**。
  uvcpp_db_table t;
  h.eq_status(h.db->query("SELECT name FROM " + table + " WHERE id = " + own,
                          {int64_t(11)}, &t),
              uvcpp_db_status::OK, "ph() 写法的查询成功");
  h.check(t.row_count() == 1 && t.rows()[0][0].to_text() == "ph",
          "ph() 写法真的取到了那一行（也顺带钉住 $n 的编号从 1 起、按实参次序）");

  // ② 另一种写法：只能按后端的脾气来。**这一半才是这条用例的目的。**
  const std::string other = (own == "$1") ? "?" : "$1";
  const bool expect_ok = (other == "$1") && h.d->accepts_dollar_placeholder();

  uvcpp_db_table t2;
  const uvcpp_db_status st =
      h.db->query("SELECT name FROM " + table + " WHERE id = " + other,
                  {int64_t(11)}, &t2);
  if (expect_ok) {
    h.eq_status(st, uvcpp_db_status::OK,
                "SQLite 把 `$1` 当**命名**参数收下，所以它照样能跑");
    h.check(t2.row_count() == 1 && t2.rows()[0][0].to_text() == "ph",
            "SQLite 用 `$1` 取回的是同一行 —— 正因如此，"
            "**只测 SQLite 看不出写法错**");
  } else {
    h.eq_status(st, uvcpp_db_status::PREPARE_FAILED,
                "另一种写法（" + other + "）必须在**准备阶段**就失败，"
                "报 PREPARE_FAILED");
    h.check(t2.row_count() == 0, "失败时结果集是空的（不是半个结果）");
  }
}

/// 把上面所有组跑一遍。`url` 是已经打开好的那一个。
inline int run_suite(const std::string& url, const dialect& d) {
  std::cout << "== 后端 " << d.name() << " ==" << std::endl;

  uvcpp_db_client db;
  harness h;
  h.db = &db;
  h.d = &d;

  h.ok(db.open(url), "open(" + url + ")");
  if (!db.is_open()) {
    std::cerr << "打不开，后面的组全部没法跑：" << db.last_error() << std::endl;
    std::cout << "== " << d.name() << "：0 通过 / 1 失败 ==" << std::endl;
    return 1;
  }

  const std::string types = "uvcpp_db_types";
  const std::string serial = "uvcpp_db_serial";

  test_driver_and_url(h);
  // 上面那组把连接关掉了，后面每组自己重新开。
  h.ok(db.open(url), "重新 open");

  test_ddl_insert_select(h, types);
  test_empty_string_is_not_null(h, types);
  test_long_values(h, types);
  test_temporal(h, types);
  test_last_insert_id(h, serial);
  test_transaction(h, serial);
  test_foreign_key(h);
  test_schema(h, types);
  test_errors(h, types);
  // `test_errors` 最后一步是**故意**把连接关掉，验证 NOT_CONNECTED。所以后面
  // 每一组都要先重新开一次 —— 不重开的话，剩下三组会集体报
  // `not_connected`，看着像被测代码坏了，其实是套件自己忘了插钥匙。
  h.ok(db.open(url), "test_errors 之后重新 open");
  test_escaping(h, serial);
  test_row_access(h, serial);
  test_export(h, types);
  test_placeholder_dialect(h);

  test_factory_and_options(h, url);

  // 池子自己开自己的连接（`url` 现成的），与上面那个 client 互不干扰。
  test_pool(h, url);

  // 金额那一列在三个后端上的**同一条**判据在 `test_ddl_insert_select()` 里，
  // 这里不再重复。这一段原本是"原生定点列 -> double"的专有断言，现在归到上面
  // 那个 `decimal_is_native()` 分支里了 —— 分成两处写，就会出现"测试通过但两个
  // 后端判的不是同一件事"。

  db.close();

  std::cout << "== " << d.name() << "：" << h.passed << " 通过 / " << h.failed
            << " 失败 / " << h.not_applicable << " 不适用 ==" << std::endl;
  return h.failed == 0 ? 0 : 1;
}

}  // namespace uvcpp_db_test

#endif  // UVCPP_TESTS_FUNCTIONAL_DB_SUITE_H_
