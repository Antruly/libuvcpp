/**
 * @file tests/functional/db_odbc_func.cpp
 * @brief 数据库模块 · ODBC 后端：跑共用套件（`db_suite.h`）+ 三条 ODBC 专属判据。
 *
 * ## 这一条要一个**真的数据源**，而"哪个数据源"是给出来的，不是猜的
 *
 * 连接串从环境变量 `UVCPP_DB_TEST_ODBC_URL` 取，例：
 *
 *     # SQL Server（本后端存在的理由）
 *     UVCPP_DB_TEST_ODBC_URL='odbc://sa:pw@127.0.0.1:1433/uvcpp_test?Driver={ODBC Driver 18 for SQL Server}'
 *     # SQLite，走 sqliteodbc —— 不需要任何服务端，适合先跑通一条
 *     UVCPP_DB_TEST_ODBC_URL='odbc:?Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=/tmp/uvcpp.db'
 *
 * 没给 URL 时：
 *
 *   * 没设 `UVCPP_DB_TEST_REQUIRE` → 打印一行说明、**退出码 3**。ctest 那边用
 *     `SKIP_RETURN_CODE 3` 把它报成 `***Skipped`，与 `Passed` 是两个词。
 *   * 设了 `UVCPP_DB_TEST_REQUIRE=1` → **退出码 1（红）**。CI 上库都起了还
 *     跳过，说明是配置写错了，这种时候"绿"是最坏的结果。
 *
 * 退出码 3 是照本仓 gate 脚本的规矩来的：**「未判定」不是「通过」**。
 *
 * ## 为什么这个文件里有**方言分支**，而另外三家没有
 *
 * ODBC 是通用驱动：**方言属于数据源，不属于这个后端**。同一份 `db_odbc_func.cpp`
 * 连 SQL Server 与连 SQLite，`CREATE TABLE` 的写法、自增列的定义、有没有原生时间
 * 类型，全都不一样。所以这里按 URL 里的 `Driver=` 挑一份 `dialect`：
 *
 *   * 认得出 → 用对应的方言跑共用套件；
 *   * 认不出 → **退出码 3（未判定）**，不是拿一份默认方言硬跑。硬跑的结局是把
 *     "这个数据源的 DDL 和我们的套件不一致"报成"ODBC 后端坏了"，而这两件事的
 *     修法完全不同。
 *
 * 目前的方言表：`sqlite`（sqliteodbc）与 `sql server`（`msodbcsql*`）。加一族就是
 * 加一个 `dialect` 子类 + 一条 `driver_family_of()` 分支。
 *
 * ## 这个库会被**改写**
 *
 * 套件里有 `DROP TABLE` / `CREATE TABLE`，所以 URL 必须指向一个**一次性的**库。
 * 用例自己的表名都是 `uvcpp_db_*` 前缀，但别拿生产库试。
 *
 * ## 三条 ODBC 专属判据（共用套件覆盖不到的地方）
 *
 *   1. **四个参数各不相同、且各自是各自的值。** `SQLBindParameter` 只记指针，
 *      缓冲共用一份时每个参数都会读到最后一个的值，**而且不报错** —— 见
 *      `test_binding_is_per_parameter()`。
 *   2. **宽字符参数的来回是逐字节的。** 长度指示器按字符给会静默砍半
 *      （`floor(n/2)`，"张三" 变 "张"），见 `test_wide_text_round_trip()`。
 *   3. **`?Driver=` 真的交给了驱动管理器。** 驱动名不存在时是"打不开"，
 *      不是"悄悄换了另一个驱动"，见 `test_driver_passthrough()`。
 */

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <db/uvcpp_db.h>

#include "db_suite.h"

namespace {

using namespace uvcpp_db_test;

/// URL 里那个 `Driver=`（或 `DSN=`）指向哪一族数据源。
enum driver_family { kFamilySqlite, kFamilySqlServer, kFamilyUnknown };

std::string lower(std::string s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] - 'A' + 'a');
  }
  return s;
}

/// 认族**只看 URL 文本**，不去连。连上再问 `SQLGetInfo(SQL_DBMS_NAME)` 更准，
/// 但那时已经晚了 —— 方言要在建表之前定，而建表是套件做的第一件事。
driver_family driver_family_of(const std::string& url) {
  const std::string u = lower(url);
  if (u.find("sqlite") != std::string::npos) return kFamilySqlite;
  // `msodbcsql18` / `{ODBC Driver 18 for SQL Server}` / `SQL Server Native Client`
  // 都落在这里。
  if (u.find("sql server") != std::string::npos ||
      u.find("msodbcsql") != std::string::npos ||
      u.find("sqlncli") != std::string::npos) {
    return kFamilySqlServer;
  }
  return kFamilyUnknown;
}

/// 长文本那一列怎么声明。SQL Server 上 `TEXT` 已废弃、`VARCHAR` 又会按代码页折
/// 中文，得用 `NVARCHAR(MAX)`；SQLite 只有 `TEXT`。
std::string text_column_decl(driver_family fam) {
  return fam == kFamilySqlServer ? "NVARCHAR(MAX)" : "TEXT";
}

/// 二进制那一列怎么声明。SQL Server 上没有 `BLOB` 这个类型名。
std::string blob_column_decl(driver_family fam) {
  return fam == kFamilySqlServer ? "VARBINARY(MAX)" : "BLOB";
}

/// SQLite，经由 sqliteodbc。
///
/// 与 `db_sqlite_func.cpp` 里那份**本库 SQLite 后端**的方言只差一处：
/// `enforces_foreign_keys()` 在这条路上是 **false**。SQLite 的外键默认是关的，
/// 本库的 SQLite 后端会替连接发一句 `PRAGMA foreign_keys=ON`，而 ODBC 这条路上
/// **没人发这一句** —— ODBC 只有 SQL，没有"pragma"这种概念，驱动也不会替谁开。
/// 同一个数据源、同一个库、同一个 schema，走两条路外键行为不同：这不是 bug，
/// 是"谁负责开那个开关"的差别，而它值得在测试里钉住。
class odbc_sqlite_dialect : public dialect {
 public:
  const char* name() const { return "odbc"; }

  std::string ph(size_t) const { return "?"; }

  /// **false，与"本库 SQLite 后端收 `$1`"相反** —— 这是量出来的：占位符是
  /// **驱动**在数，sqliteodbc 只认 `?`，给它 `$1` 报
  /// `parameter marker count incorrect`（`PREPARE_FAILED`）。所以"SQLite 收
  /// `$1`"是那个后端的性质，不是这个数据库的性质；ODBC 这一层把它盖掉了。
  bool accepts_dollar_placeholder() const { return false; }

  /// **false**：sqliteodbc 的 `SQLPrepare` 是空操作（SQLite 要到第一次 step 才
  /// 解析 SQL），于是语法错、表不存在都要等到执行才报 —— `test_errors()` 那两条
  /// 得到的是 `EXEC_FAILED`。实测的。
  bool prepares_eagerly() const { return false; }

  std::string serial_pk_type() const { return "INTEGER PRIMARY KEY AUTOINCREMENT"; }

  std::string create_types_table(const std::string& t) const {
    // `flag_val` 是 INTEGER：SQLite 没有布尔类型，0/1 就是它的布尔 ——
    // 判的是 `to_bool()` 的值，不是类型标签。
    // `e_val` 特意带 `NOT NULL DEFAULT ''`：把空串折成 NULL 的实现会在这里
    // 撞上非空约束，失败得**响亮**。
    return "CREATE TABLE " + t +
           " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
           " i_val INTEGER, d_val REAL, s_val TEXT, b_val BLOB,"
           " n_val TEXT, e_val TEXT NOT NULL DEFAULT '', flag_val INTEGER,"
           " dt_val TEXT, dd_val TEXT, tt_val TEXT, " + decimal_column_decl() + ")";
  }

  std::string create_serial_table(const std::string& t) const {
    return "CREATE TABLE " + t + " (id " + serial_pk_type() + ", name TEXT)";
  }

  std::string decimal_column_decl() const { return "dec_val TEXT"; }

  bool has_native_temporal() const { return false; }
  bool decimal_is_native() const { return false; }

  /// **本后端的 `last_insert_id()` 在这条路上报不出来**（它发的是 SQL Server 的
  /// `SCOPE_IDENTITY()`，SQLite 不认），如实返回 `UNSUPPORTED`。所以这一条在共用
  /// 套件里是"不适用"，而它**真的**返回什么由
  /// `test_last_insert_id_is_honest()` 单独钉住 —— 不适用不等于没人管。
  bool supports_last_insert_id() const { return false; }

  bool enforces_foreign_keys() const { return false; }
};

/// SQL Server，经由 `msodbcsql*`。
///
/// **这一支在本机没有跑过**（手上没有 SQL Server 实例），所以它是一份照着
/// SQL Server 语法写的方言，不是量出来的读数：`INT IDENTITY(1,1)` 是自增列的
/// 写法、`NVARCHAR(MAX)` 承接长文本、`BIT` 是布尔、`DECIMAL(18,2)` 是原生定点。
/// 拿它去跑真 SQL Server 是**第一次**跑，读数以那次为准；哪条断言红了，先怀疑
/// 这里是照书写错了，而不是先怀疑后端。
class odbc_mssql_dialect : public dialect {
 public:
  const char* name() const { return "odbc"; }

  /// SQL Server 的 ODBC 驱动只认 `?`，不认 `$1`。
  std::string ph(size_t) const { return "?"; }

  std::string serial_pk_type() const { return "INT IDENTITY(1,1) PRIMARY KEY"; }

  std::string create_types_table(const std::string& t) const {
    return "CREATE TABLE " + t +
           " (id INT IDENTITY(1,1) PRIMARY KEY,"
           " i_val BIGINT, d_val FLOAT, s_val " + text_column_decl(kFamilySqlServer) +
           ", b_val VARBINARY(MAX), n_val NVARCHAR(64),"
           " e_val NVARCHAR(64) NOT NULL DEFAULT '', flag_val BIT,"
           " dt_val DATETIME2, dd_val DATE, tt_val TIME, " +
           decimal_column_decl() + ")";
  }

  std::string create_serial_table(const std::string& t) const {
    return "CREATE TABLE " + t + " (id " + serial_pk_type() +
           ", name NVARCHAR(64))";
  }

  std::string decimal_column_decl() const { return "dec_val DECIMAL(18,2)"; }

  bool has_native_temporal() const { return true; }
  bool decimal_is_native() const { return true; }
  bool supports_last_insert_id() const { return true; }
  bool enforces_foreign_keys() const { return true; }
};

// ---------------------------------------------------------------------------
// ODBC 专属判据
// ---------------------------------------------------------------------------

/// 坑五的见证：**每个参数都得有自己的缓冲**。
///
/// `SQLBindParameter` 只记指针（`StrLen_or_IndPtr` 也是一枚指针），而执行发生在
/// **所有**参数都绑完之后。用几个成员变量在循环里反复赋值，等于让每个参数都指向
/// 最后那一轮写进去的数据 —— 实测过：`(?, ?, ?, ?)` 逐行绑
/// (1,2,'t0','uu0')、(11,12,'t1','uu1')、(21,22,'t2','uu2')，库里进去的是
/// (2,2,'t','u')、(12,12,'t','u')、(22,22,'t','u')，**一个错都不报**。
///
/// 四种类型的列都放进来：整数两列、文本两列。只放一列数值的话，共用一份缓冲反而
/// 恰好是对的（只有一个参数），这条判据就白写了。
void test_binding_is_per_parameter(harness& h, driver_family fam) {
  const std::string text_col = text_column_decl(fam);
  const std::string table = "uvcpp_db_odbc_bind";
  h.db->execute("DROP TABLE " + table);
  h.ok(h.db->execute("CREATE TABLE " + table + " (a BIGINT, b BIGINT, c " +
                     text_col + ", d " + text_col + ")"),
       "建绑定判据表");

  const std::string sql =
      "INSERT INTO " + table + " (a, b, c, d) VALUES (?, ?, ?, ?)";
  for (int i = 0; i < 3; ++i) {
    const std::string suffix(1, static_cast<char>('0' + i));
    h.ok(h.db->execute(sql, {int64_t(1 + i * 10), int64_t(2 + i * 10),
                             std::string("t") + suffix,
                             std::string("uu") + suffix}),
         "插第 " + std::to_string(i) + " 行（四个参数各不同）");
  }

  uvcpp_db_table t;
  h.ok(h.db->query("SELECT a, b, c, d FROM " + table + " ORDER BY a",
                   static_cast<uvcpp_db_table*>(&t)),
       "读回四列");
  h.eq_int(static_cast<int64_t>(t.row_count()), 3, "三行都在");
  if (t.row_count() != 3) return;

  // 四列都要对。只对第一列的话，"所有参数都成了最后一个的值"这种错会被漏掉
  // —— 第一列恰好是它自己那一轮写进去的（共用的缓冲里最后是最后一行）。
  const int64_t want_a[] = {1, 11, 21};
  const int64_t want_b[] = {2, 12, 22};
  const char* want_c[] = {"t0", "t1", "t2"};
  const char* want_d[] = {"uu0", "uu1", "uu2"};
  for (size_t r = 0; r < 3; ++r) {
    const std::string row = "第 " + std::to_string(r) + " 行 ";
    h.eq_int(t.at(r, 0).to_int64(), want_a[r], row + "a");
    h.eq_int(t.at(r, 1).to_int64(), want_b[r], row + "b");
    h.eq_text(t.at(r, 2).to_text(), want_c[r], row + "c");
    h.eq_text(t.at(r, 3).to_text(), want_d[r], row + "d");
  }

  h.db->execute("DROP TABLE " + table);
}

/// 坑四的见证：**宽字符参数的长度指示器单位是字节**。
///
/// 按字符给的症状是长文本被砍掉一半（`floor(n/2)` 个字符活下来）："张三" 存成
/// "张"。**ASCII 也中招**（"abcdef" 存成 "abc"），所以"只有中文才出问题"这个判断
/// 会把方向带偏 —— 这条判据里纯 ASCII 与中文都要有。
void test_wide_text_round_trip(harness& h, driver_family fam) {
  const std::string table = "uvcpp_db_odbc_wide";
  h.db->execute("DROP TABLE " + table);
  h.ok(h.db->execute("CREATE TABLE " + table + " (k BIGINT, v " +
                     text_column_decl(fam) + ")"),
       "建宽字符判据表");

  // 字符数取偶数（4/2/6）与奇数（3 字中文按 UTF-8 是 9 字节）都有：砍半在两种
  // 长度上都看得见，奇数的还会掉尾字。
  const char* samples[] = {"abcd", "ab", "张三", "张三丰测试",
                           "混合 mixed 文本 12345"};
  const size_t n = sizeof(samples) / sizeof(samples[0]);
  for (size_t i = 0; i < n; ++i) {
    h.ok(h.db->execute("INSERT INTO " + table + " (k, v) VALUES (?, ?)",
                       {int64_t(i), std::string(samples[i])}),
         std::string("插入 [") + samples[i] + "]");
  }

  uvcpp_db_table t;
  h.ok(h.db->query("SELECT k, v FROM " + table + " ORDER BY k",
                   static_cast<uvcpp_db_table*>(&t)),
       "读回宽字符列");
  h.eq_int(static_cast<int64_t>(t.row_count()), static_cast<int64_t>(n),
           "样本条数对得上");
  if (t.row_count() != n) return;

  for (size_t i = 0; i < n; ++i) {
    h.eq_text(t.at(i, 1).to_text(), samples[i],
              std::string("往返逐字节一致：[") + samples[i] + "]");
  }

  // 参数化的**条件**也要能查回来 —— 上面测的是"存进去对不对"，这一步测的是
  // "拿它当条件找得到吗"，两条路都要过绑定与宽字符编码。
  for (size_t i = 0; i < n; ++i) {
    uvcpp_db_table one;
    h.ok(h.db->query("SELECT k FROM " + table + " WHERE v = ?",
                     {std::string(samples[i])},
                     static_cast<uvcpp_db_table*>(&one)),
         std::string("拿 [") + samples[i] + "] 当条件查");
    h.eq_int(static_cast<int64_t>(one.row_count()), 1,
             std::string("按文本条件查到 1 行：[") + samples[i] + "]");
  }

  h.db->execute("DROP TABLE " + table);
}

/// `last_insert_id()` 报不出来时必须是 `UNSUPPORTED`，不是 `OK` + 一个编出来的 0。
///
/// 共用套件在 `supports_last_insert_id() == false` 时把整组标成"不适用"，所以
/// **没有别的判据**在看这条路 —— 而它恰恰是最容易被写成"返回个 0 算了"的地方。
/// 编出来的 0 会成为调用方 SQL 里的 `WHERE id = 0`，查不到任何行，症状是
/// "刚插进去的数据不见了"。
void test_last_insert_id_is_honest(harness& h, driver_family fam,
                                   const std::string& url) {
  if (fam == kFamilySqlServer) {
    // SQL Server 上这条路是真的实现了的（`SCOPE_IDENTITY()`），归共用套件判。
    h.skip("报不出来时返回 UNSUPPORTED", "SQL Server 上这条能报出来");
    return;
  }

  uvcpp_db_client db;
  if (db.open(url) != uvcpp_db_status::OK) {
    h.check(false, "last_insert_id 判据要一个连接：" + db.last_error());
    return;
  }
  uvcpp_db_value out;
  const uvcpp_db_status st = db.last_insert_id(&out);
  h.eq_status(st, uvcpp_db_status::UNSUPPORTED,
              "SQLite 这一路上 last_insert_id -> UNSUPPORTED");
  h.check(!db.last_error().empty(), "报不出来时必须留下人能读的原因");
  db.close();
}

/// `?Driver=` 是**原样**交给驱动管理器的，所以驱动名写错就是"打不开"。
///
/// 这条判据挡的是另一种实现：把不认识的驱动名悄悄换成某个默认驱动 —— 那样
/// `Driver=MySQL` 会连上一个完全不同的数据源，而调用方以为连对了。驱动名不存在时
/// 驱动管理器返回 `SQL_ERROR`（`IM002` 一类），落在 **`OPEN_FAILED`**，不是
/// `NO_DRIVER`：`NO_DRIVER` 在这个模块里的意思是"这个后端没编进这份库"，而 ODBC
/// 后端明明编进来了，缺的是数据源那头的驱动。两个码混起来，排障第一步就会走到
/// "换个包"上去。
void test_driver_passthrough(harness& h) {
  uvcpp_db_client tmp;
  const uvcpp_db_status st = tmp.open(
      "odbc://u:p@127.0.0.1:1/db?Driver={uvcpp_no_such_driver_xyz}");
  h.eq_status(st, uvcpp_db_status::OPEN_FAILED,
              "不存在的 Driver= -> OPEN_FAILED（不是 NO_DRIVER）");
  h.check(!tmp.is_open(), "打不开之后 is_open() 必须是 false");
  h.check(!tmp.last_error().empty(), "打不开必须留下 last_error");

  // 驱动名给了、但那个 `.so` 路径是假的：同样是打不开，不是"照默认驱动连"。
  uvcpp_db_client tmp2;
  const uvcpp_db_status st2 = tmp2.open(
      "odbc:?Driver=/nonexistent/uvcpp_no_such_driver.so;Database=/tmp/x.db");
  h.check(st2 == uvcpp_db_status::OPEN_FAILED ||
              st2 == uvcpp_db_status::BAD_URL,
          std::string("假的驱动 .so 路径 -> 打不开（实得 ") +
              uvcpp::uvcpp_db_status_name(st2) + "）");
}

/// 分块读的两条边界，共用套件都够不着（它的 blob 只有 300 字节、文本虽然是 3000
/// 字节但全是 ASCII）。
///
///   * **二进制跨块**：`SQL_C_BINARY` 没有结尾 NUL，可以用满整块 —— 与文本那条
///     "每块只用 `kChunk - 1` 个单元"的规矩相反。两条分支搞混就是静默少一个字节
///     或多一个字节，而 300 字节的样本永远碰不到。
///   * **4 字节 UTF-8 字符骑在块边界上**：`U+20000` 这类字符在 UTF-16 里是一对
///     代理，边界切在代理对中间就会读成两个 `U+FFFD`。样本特意把它的位置摆在
///     "第一块正好写到它"的地方（前面 2046 个 ASCII），所以这条不只是"长文本能
///     读回来"，而是"切在代理对中间会怎样"。实测 sqliteodbc 不切代理对，本库的
///     `to_utf8()` 只在**同一块内**拼代理对。
void test_chunk_boundaries(harness& h, driver_family fam) {
  const std::string table = "uvcpp_db_odbc_chunk";
  h.db->execute("DROP TABLE " + table);
  h.ok(h.db->execute("CREATE TABLE " + table + " (k BIGINT, b " +
                     blob_column_decl(fam) + ", s " + text_column_decl(fam) + ")"),
       "建分块边界表");

  // 3000 字节的 blob：> kChunk(2048)，必然跨块。
  std::string blob;
  for (int i = 0; i < 3000; ++i) {
    blob.push_back(static_cast<char>(i % 251));  // 251 是质数：三个周期不重合
  }
  h.ok(h.db->execute("INSERT INTO " + table + " (k, b) VALUES (?, ?)",
                     {int64_t(1), uvcpp_db_value::blob(blob)}),
       "插入 3000 字节 blob");

  // 2046 个 'a' + U+20000（UTF-8 四字节 / UTF-16 一对代理）+ 100 个 'b'
  std::string wide;
  for (int i = 0; i < 2046; ++i) wide += "a";
  wide += "\xF0\xA0\x80\x80";
  for (int i = 0; i < 100; ++i) wide += "b";
  h.ok(h.db->execute("INSERT INTO " + table + " (k, s) VALUES (?, ?)",
                     {int64_t(2), wide}),
       "插入代理对骑在块边界上的样本");

  uvcpp_db_table t;
  h.ok(h.db->query("SELECT b, s FROM " + table + " ORDER BY k",
                   static_cast<uvcpp_db_table*>(&t)),
       "读回两行");
  h.eq_int(static_cast<int64_t>(t.row_count()), 2, "两行都在");
  if (t.row_count() != 2) return;

  h.eq_int(static_cast<int64_t>(t.at(0, 0).bytes().size()), 3000,
           "二进制跨块：长度是 3000 字节");
  h.check(t.at(0, 0).bytes() == blob, "二进制跨块：逐字节相等");
  h.check(t.at(1, 1).to_text() == wide,
          "代理对跨块：逐字节相等（实得 " +
              std::to_string(t.at(1, 1).to_text().size()) + " 字节，真值 " +
              std::to_string(wide.size()) + "）");

  // 拿它当**条件**查：写路径（UTF-8 -> UTF-16 的代理对编码）也要对得上。
  uvcpp_db_table one;
  h.ok(h.db->query("SELECT k FROM " + table + " WHERE s = ?", {wide},
                   static_cast<uvcpp_db_table*>(&one)),
       "拿代理对样本当条件查");
  h.eq_int(static_cast<int64_t>(one.row_count()), 1, "条件命中 1 行");

  h.db->execute("DROP TABLE " + table);
}

}  // namespace

int main() {
  const char* url = std::getenv("UVCPP_DB_TEST_ODBC_URL");
  const char* require = std::getenv("UVCPP_DB_TEST_REQUIRE");

  if (url == nullptr || *url == '\0') {
    if (require != nullptr && *require != '\0') {
      std::cerr << "UVCPP_DB_TEST_REQUIRE 已设，但 UVCPP_DB_TEST_ODBC_URL 是空的"
                << " —— 这是失败，不是跳过（CI 上数据源应该已经起来了）"
                << std::endl;
      return 1;
    }
    std::cout << "SKIP: 没给 UVCPP_DB_TEST_ODBC_URL，ODBC 后端这一条未判定"
              << std::endl;
    return 3;  // ctest 的 SKIP_RETURN_CODE
  }

  // 先确认这个库真的带了 ODBC 后端。CMake 那边已经把"没编进来"的用例摘掉了，
  // 所以走到这里还说没有，就是配置错了 —— 报错，不跳过。
  if (uvcpp::uvcpp_db_drivers().find("odbc") == std::string::npos) {
    std::cerr << "这份构建没有 ODBC 后端（已编入：" << uvcpp::uvcpp_db_drivers()
              << "）—— 这一条用例不该被注册" << std::endl;
    return 1;
  }

  const std::string u(url);
  const driver_family fam = driver_family_of(u);

  if (fam == kFamilyUnknown) {
    // 认不出方言时**退出码 3**：这不是"ODBC 后端坏了"，是"这个数据源的 DDL
    // 本用例还不会写"。硬拿一份默认方言跑，红的会是套件里的建表语句，而那条红
    // 会指向完全错误的方向。
    std::cout << "SKIP: 认不出这个 URL 的方言（" << u
              << "）—— 共用套件要按数据源的 DDL 跑，本用例现在只认 sqlite 与 "
                 "sql server 两族。ODBC 后端这一条**未判定**" << std::endl;
    return 3;
  }

  odbc_sqlite_dialect sqlite_d;
  odbc_mssql_dialect mssql_d;
  const dialect& d = (fam == kFamilySqlite)
                         ? static_cast<const dialect&>(sqlite_d)
                         : static_cast<const dialect&>(mssql_d);

  std::cout << "-- ODBC (" << u << ") --" << std::endl;
  const int suite_rc = run_suite(u, d);

  std::cout << "-- ODBC 专属 --" << std::endl;
  harness h;
  uvcpp_db_client db;
  h.db = &db;
  h.d = &d;
  if (db.open(u) == uvcpp_db_status::OK) {
    test_binding_is_per_parameter(h, fam);
    test_wide_text_round_trip(h, fam);
    test_chunk_boundaries(h, fam);
    test_last_insert_id_is_honest(h, fam, u);
    test_driver_passthrough(h);
    db.close();
  } else {
    h.check(false, "专属判据要一个打开的连接，但 open 失败：" + db.last_error());
  }

  std::cout << "-- ODBC 专属：" << h.passed << " 通过 / " << h.failed << " 失败 / "
            << h.not_applicable << " 不适用 --" << std::endl;
  std::cout << (suite_rc == 0 && h.failed == 0 ? "db_odbc_func: 全部通过"
                                               : "db_odbc_func: 有失败")
            << std::endl;
  return (suite_rc == 0 && h.failed == 0) ? 0 : 1;
}
