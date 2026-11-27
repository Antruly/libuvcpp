/**
 * @file tests/functional/orm_sqlite_func.cpp
 * @brief ORM 结果映射与同步 CRUD（真实 SQLite，不 mock）。
 *
 * 覆盖 issue #91：
 *
 *   1. 建表、INSERT（自增主键回填）、按主键/条件查询、UPDATE、DELETE、COUNT；
 *   2. NULL/空串、整数边界、BLOB、bool、double、`nullable<T>` 往返；
 *   3. 缺列（可空/非空）、多列、类型不匹配、NULL 写非空字段；
 *   4. UPDATE/DELETE 0 行与执行失败的区分、唯一键冲突透传；
 *   5. 结果集列顺序不同时按列名映射；
 *   6. 非法参数、方言不匹配、未打开连接。
 *
 * 文件名带 `orm_` 前缀，`UVCPP_BUILD_ORM=OFF` 时整个目标不存在。
 */

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <orm/uvcpp_orm_repository.h>

// ---------------------------------------------------------------------------
// 测试模型：覆盖 #89 的全部类型 + nullable 版本
// ---------------------------------------------------------------------------

struct all_types_row {
  std::int64_t id;
  bool flag;
  std::int8_t i8;
  std::uint8_t u8;
  std::int16_t i16;
  std::uint16_t u16;
  std::int32_t i32;
  std::uint32_t u32;
  std::int64_t i64;
  std::uint64_t u64;
  float f32;
  double f64;
  std::string text;
  std::vector<std::uint8_t> blob;
  uvcpp::nullable<std::int32_t> opt_i32;
  uvcpp::nullable<std::string> opt_text;
  uvcpp::nullable<bool> opt_bool;
  uvcpp::nullable<std::int64_t> opt_i64;
  uvcpp::nullable<double> opt_f64;
};

UVCPP_ORM_MODEL(all_types_row, "all_types_row")
UVCPP_ORM_FIELD(all_types_row, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(all_types_row, flag, "flag", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, i8, "i8", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, u8, "u8", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, i16, "i16", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, u16, "u16", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, i32, "i32", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, u32, "u32", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, i64, "i64", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, u64, "u64", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, f32, "f32", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, f64, "f64", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, text, "text",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(all_types_row, blob, "blob", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types_row, opt_i32, "opt_i32", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types_row, opt_text, "opt_text", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types_row, opt_bool, "opt_bool", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types_row, opt_i64, "opt_i64", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types_row, opt_f64, "opt_f64", UVCPP_ORM_NONE)
UVCPP_ORM_MODEL_END()

/// 没有声明模型：仓库 API 必须先报 NO_MODEL。
struct undeclared_row {
  std::int64_t id;
};

namespace {

using uvcpp::uvcpp_db_client;
using uvcpp::uvcpp_db_status;
using uvcpp::uvcpp_db_value;
using uvcpp::uvcpp_orm_dialect;
using uvcpp::uvcpp_orm_select_builder;
using uvcpp::uvcpp_orm_sql_result;
using uvcpp::uvcpp_orm_status;

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& what) {
  ++checks;
  if (condition) return;
  ++failures;
  std::cerr << "[FAIL] " << what << std::endl;
}

void check_status(uvcpp_orm_status got, uvcpp_orm_status want,
                  const std::string& what, const std::string& error) {
  check(got == want, what + "（期望 " + uvcpp::uvcpp_orm_status_name(want) +
                         "，实得 " + uvcpp::uvcpp_orm_status_name(got) +
                         " / [" + error + "]）");
}

void check_db_status(uvcpp_db_status got, uvcpp_db_status want,
                     const std::string& what) {
  check(got == want, what + "（期望 " +
                         uvcpp::uvcpp_db_status_name(want) + "，实得 " +
                         uvcpp::uvcpp_db_status_name(got) + "）");
}

void check_contains(const std::string& text, const std::string& fragment,
                    const std::string& what) {
  check(text.find(fragment) != std::string::npos,
        what + "：期望包含 [" + fragment + "]，实得 [" + text + "]");
}

void check_ok(uvcpp_orm_status status, const std::string& what,
              const std::string& error) {
  check_status(status, uvcpp_orm_status::OK, what, error);
}

/// 测试用的临时目录。CI 的三个平台都有 TMPDIR/TEMP/TMP 之一，兜底给 /tmp。
std::string temp_dir() {
  const char* keys[] = {"TMPDIR", "TEMP", "TMP"};
  for (std::size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    const char* value = std::getenv(keys[i]);
    if (value != nullptr && *value != '\0') {
      std::string path(value);
      while (!path.empty() &&
             (path[path.size() - 1] == '/' || path[path.size() - 1] == '\\')) {
        path.erase(path.size() - 1);
      }
      if (!path.empty()) return path;
    }
  }
  return "/tmp";
}

std::string quoted(const std::string& identifier) {
  return uvcpp::uvcpp_orm_quote_identifier(uvcpp_orm_dialect::sqlite,
                                           identifier);
}

all_types_row make_base_row() {
  all_types_row row;
  row.id = 0;
  row.flag = true;
  row.i8 = -12;
  row.u8 = 200;
  row.i16 = -3000;
  row.u16 = 60000;
  row.i32 = -123456;
  row.u32 = 3000000000u;
  row.i64 = -9000000000ll;
  row.u64 = 9000000000ull;
  row.f32 = 1.5f;
  row.f64 = 2.25;
  row.text = "alpha";
  row.blob.push_back(0u);
  row.blob.push_back(1u);
  row.blob.push_back(255u);
  row.blob.push_back(0u);
  row.blob.push_back(128u);
  row.opt_i32 = std::int32_t(-7);
  row.opt_text = std::string("opt");
  row.opt_bool = true;
  row.opt_i64 = std::int64_t(-9000000000ll);
  row.opt_f64 = 0.125;
  return row;
}

bool same_fields(const all_types_row& a, const all_types_row& b) {
  return a.flag == b.flag && a.i8 == b.i8 && a.u8 == b.u8 &&
         a.i16 == b.i16 && a.u16 == b.u16 && a.i32 == b.i32 &&
         a.u32 == b.u32 && a.i64 == b.i64 && a.u64 == b.u64 &&
         a.f32 == b.f32 &&
         a.f64 == b.f64 && a.text == b.text && a.blob == b.blob &&
         a.opt_i32 == b.opt_i32 && a.opt_text == b.opt_text &&
         a.opt_bool == b.opt_bool && a.opt_i64 == b.opt_i64 &&
         a.opt_f64 == b.opt_f64;
}

std::string select_all_sql(const std::string& skip_column,
                           const std::string& replaced_column,
                           const std::string& replacement) {
  const uvcpp::uvcpp_orm_column* columns =
      uvcpp::model_traits<all_types_row>::columns();
  const std::size_t count =
      uvcpp::model_traits<all_types_row>::column_count();

  std::string sql = "SELECT ";
  bool first = true;
  for (std::size_t i = 0; i < count; ++i) {
    const std::string name(columns[i].column_name());
    if (!skip_column.empty() && name == skip_column) continue;
    if (!first) sql += ", ";
    if (!replaced_column.empty() && name == replaced_column) {
      sql += replacement + " AS " + quoted(name);
    } else {
      sql += quoted(name);
    }
    first = false;
  }
  sql += " FROM " + quoted("all_types_row");
  return sql;
}

int create_table(uvcpp_db_client& db) {
  db.execute("DROP TABLE IF EXISTS \"all_types_row\"");

  const char* sql =
      "CREATE TABLE \"all_types_row\" ("
      " id INTEGER PRIMARY KEY AUTOINCREMENT,"
      " flag INTEGER NOT NULL,"
      " i8 INTEGER NOT NULL,"
      " u8 INTEGER NOT NULL,"
      " i16 INTEGER NOT NULL,"
      " u16 INTEGER NOT NULL,"
      " i32 INTEGER NOT NULL,"
      " u32 INTEGER NOT NULL,"
      " i64 INTEGER NOT NULL,"
      " u64 INTEGER NOT NULL,"
      " f32 REAL NOT NULL,"
      " f64 REAL NOT NULL,"
      " text TEXT NOT NULL UNIQUE,"
      " blob BLOB NOT NULL,"
      " opt_i32 INTEGER,"
      " opt_text TEXT,"
      " opt_bool INTEGER,"
      " opt_i64 INTEGER,"
      " opt_f64 REAL)";
  const uvcpp_db_status status = db.execute(sql);
  check_db_status(status, uvcpp_db_status::OK,
                  "建表 all_types_row（" + db.last_error() + "）");
  return uvcpp_db_ok(status) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 1. INSERT / find_by_pk / 类型往返
// ---------------------------------------------------------------------------

void test_insert_and_round_trip(uvcpp_db_client& db,
                                std::vector<uvcpp_db_value>* ids) {
  std::string error;
  uvcpp_db_status database_status = uvcpp_db_status::OK;

  // --- 普通行：所有字段有值 ---
  all_types_row alpha = make_base_row();
  uvcpp_db_value alpha_id;
  uvcpp_orm_status status =
      uvcpp::insert<all_types_row>(db, alpha, &alpha_id, &error,
                                   &database_status);
  check_ok(status, "insert alpha", error);
  check(!alpha_id.is_null(), "insert 回填了自增 id");
  check(alpha_id.to_int64() > 0, "自增 id 是正数");
  check(alpha.id == 0, "insert 不会改 const 模型的 id（只回填 new_id）");

  all_types_row alpha_back;
  status = uvcpp::find_by_pk<all_types_row>(
      db, alpha_id, &alpha_back, &error, &database_status);
  check_ok(status, "find_by_pk alpha", error);
  check(alpha_back.id == alpha_id.to_int64(), "查回的 id 与 new_id 一致");
  check(same_fields(alpha_back, alpha), "alpha 的全部字段往返一致");

  // --- NULL 可空字段：nullable 是空态 ---
  all_types_row beta = make_base_row();
  beta.text = "beta";
  beta.flag = false;
  beta.opt_i32.reset();
  beta.opt_text.reset();
  beta.opt_bool.reset();
  beta.opt_i64.reset();
  beta.opt_f64.reset();

  uvcpp_db_value beta_id;
  status = uvcpp::insert<all_types_row>(db, beta, &beta_id, &error,
                                        &database_status);
  check_ok(status, "insert beta（可空字段全 NULL）", error);

  all_types_row beta_back;
  status = uvcpp::find_by_pk<all_types_row>(
      db, beta_id, &beta_back, &error, &database_status);
  check_ok(status, "find_by_pk beta", error);
  check(!beta_back.flag, "bool false 往返");
  check(!beta_back.opt_i32.has_value(), "NULL int32 → nullable 空态");
  check(!beta_back.opt_text.has_value(), "NULL text → nullable 空态");
  check(!beta_back.opt_bool.has_value(), "NULL bool → nullable 空态");
  check(!beta_back.opt_i64.has_value(), "NULL int64 → nullable 空态");
  check(!beta_back.opt_f64.has_value(), "NULL double → nullable 空态");

  // --- 空串不是 NULL ---
  all_types_row empty = make_base_row();
  empty.text = std::string();          // 非空字段的空串
  empty.opt_text = std::string();      // 有值的空串
  empty.opt_i32 = std::int32_t(0);

  uvcpp_db_value empty_id;
  status = uvcpp::insert<all_types_row>(db, empty, &empty_id, &error,
                                        &database_status);
  check_ok(status, "insert empty（空串不是 NULL）", error);

  all_types_row empty_back;
  status = uvcpp::find_by_pk<all_types_row>(
      db, empty_id, &empty_back, &error, &database_status);
  check_ok(status, "find_by_pk empty", error);
  check(empty_back.text.empty(), "空串非空字段往返仍是空串");
  check(empty_back.opt_text.has_value(), "空串 nullable 是“有值”");
  check(empty_back.opt_text.value().empty(), "空串 nullable 的值长度为 0");
  check(empty_back.opt_i32.has_value() && empty_back.opt_i32.value() == 0,
        "nullable<int32> 存 0 是有值，不是 NULL");

  // --- 整数边界 / BLOB / 浮点 ---
  all_types_row bounds = make_base_row();
  bounds.text = "bounds";
  bounds.i8 = (std::numeric_limits<std::int8_t>::min)();
  bounds.u8 = (std::numeric_limits<std::uint8_t>::max)();
  bounds.i16 = (std::numeric_limits<std::int16_t>::min)();
  bounds.u16 = (std::numeric_limits<std::uint16_t>::max)();
  bounds.i32 = (std::numeric_limits<std::int32_t>::min)();
  bounds.u32 = (std::numeric_limits<std::uint32_t>::max)();
  bounds.i64 = (std::numeric_limits<std::int64_t>::min)();
  bounds.u64 = static_cast<std::uint64_t>(9223372036854775807ll);
  bounds.f32 = 3.5f;
  bounds.f64 = -1.25;
  bounds.blob.push_back(0xdeu);
  bounds.blob.push_back(0xadu);
  bounds.blob.push_back(0xbeu);
  bounds.blob.push_back(0xefu);
  bounds.opt_i32 = (std::numeric_limits<std::int32_t>::max)();
  bounds.opt_text.reset();
  bounds.opt_bool = false;
  bounds.opt_i64 = (std::numeric_limits<std::int64_t>::min)();
  bounds.opt_f64 = 1.0 / 3.0;

  uvcpp_db_value bounds_id;
  status = uvcpp::insert<all_types_row>(db, bounds, &bounds_id, &error,
                                        &database_status);
  check_ok(status, "insert bounds", error);

  all_types_row bounds_back;
  status = uvcpp::find_by_pk<all_types_row>(
      db, bounds_id, &bounds_back, &error, &database_status);
  check_ok(status, "find_by_pk bounds", error);
  check(same_fields(bounds_back, bounds), "整数边界/BLOB/浮点往返一致");
  check(bounds_back.blob == bounds.blob, "BLOB 逐字节往返一致");
  check(bounds_back.opt_bool.has_value() && !bounds_back.opt_bool.value(),
        "nullable<bool> false 是有值，不是 NULL");

  ids->push_back(alpha_id);
  ids->push_back(beta_id);
  ids->push_back(empty_id);
  ids->push_back(bounds_id);
}

// ---------------------------------------------------------------------------
// 2. find_all / count / 列顺序
// ---------------------------------------------------------------------------

void test_find_all_and_count(uvcpp_db_client& db) {
  std::string error;
  uvcpp_db_status database_status = uvcpp_db_status::OK;

  // 条件查询 + LIMIT：total 必须是**不分页**的总数。
  {
    std::vector<all_types_row> rows;
    int64_t total = -1;
    const uvcpp_orm_status status = uvcpp::find_all<all_types_row>(
        db,
        uvcpp::select<all_types_row>(uvcpp_orm_dialect::sqlite)
            .where_eq(&all_types_row::flag, true)
            .order_by(&all_types_row::id, uvcpp::uvcpp_orm_order::asc)
            .limit(2),
        &rows, &error, &total, &database_status);
    check_ok(status, "find_all builder + total", error);
    check(rows.size() == 2u, "LIMIT 2 只返回 2 行");
    check(total == 3, "total 是不受 LIMIT 影响的总数（3 行 flag=true）");
    for (std::size_t i = 0; i < rows.size(); ++i) {
      check(rows[i].flag, "find_all 的 WHERE 条件每行都成立");
      check(rows[i].id > 0, "find_all 映射了主键与其余字段");
    }
  }

  // 结果集列顺序完全反过来：映射必须按列名，不按下标。
  {
    uvcpp_orm_select_builder<all_types_row> reversed(
        uvcpp_orm_dialect::sqlite);
    const uvcpp::uvcpp_orm_column* columns =
        uvcpp::model_traits<all_types_row>::columns();
    const std::size_t count =
        uvcpp::model_traits<all_types_row>::column_count();
    for (std::size_t i = count; i > 0; --i) {
      reversed.column(columns[i - 1].column_name());
    }
    reversed.where_ge(&all_types_row::id, std::int64_t(0))
        .order_by(&all_types_row::id, uvcpp::uvcpp_orm_order::asc)
        .limit(2);

    const uvcpp_orm_sql_result select_sql = reversed.build();
    std::vector<all_types_row> rows;
    int64_t total = -1;
    const uvcpp_orm_status status = uvcpp::find_all<all_types_row>(
        db, select_sql, &rows, &error, &total, &database_status);
    check_ok(status, "find_all 已构造 SQL + 反序列", error);
    check(rows.size() == 2u, "反序列结果集返回 2 行");
    check(total == 4, "反序列 SQL 的 total 是 4（去掉 LIMIT）");
    check(rows.size() == 2u && rows[0].text == "alpha" &&
              rows[1].text == "beta",
          "反序列结果集仍然按列名映射正确");
  }

  // 显式 count 辅助函数（builder 与已构造 COUNT 两种形态）。
  {
    int64_t total = -1;
    const uvcpp_orm_status status = uvcpp::count<all_types_row>(
        db,
        uvcpp::select<all_types_row>(uvcpp_orm_dialect::sqlite)
            .where_eq(&all_types_row::flag, true),
        &total, &error, &database_status);
    check_ok(status, "count(builder)", error);
    check(total == 3, "count(builder) = 3");

    const uvcpp_orm_sql_result count_sql =
        uvcpp::select<all_types_row>(uvcpp_orm_dialect::sqlite)
            .count()
            .where_eq(&all_types_row::flag, true)
            .build();
    total = -1;
    const uvcpp_orm_status status2 = uvcpp::count<all_types_row>(
        db, count_sql, &total, &error, &database_status);
    check_ok(status2, "count(sql_result)", error);
    check(total == 3, "count(sql_result) = 3");
  }

  // 无 LIMIT 时 total 与行数一致。
  {
    std::vector<all_types_row> rows;
    int64_t total = -1;
    const uvcpp_orm_status status = uvcpp::find_all<all_types_row>(
        db, uvcpp::select<all_types_row>(uvcpp_orm_dialect::sqlite),
        &rows, &error, &total, &database_status);
    check_ok(status, "find_all 无条件 + total", error);
    check(rows.size() == 4u && total == 4, "无条件查询 4 行，total=4");
  }
}

// ---------------------------------------------------------------------------
// 3. 映射错误：缺列 / 多列 / 类型 / NULL
// ---------------------------------------------------------------------------

void test_mapping_errors(uvcpp_db_client& db) {
  std::string error;

  // 多出来的列忽略。
  {
    std::vector<all_types_row> rows;
    const uvcpp_orm_sql_result raw_sql = uvcpp::raw(
        uvcpp_orm_dialect::sqlite,
        "SELECT *, 42 AS \"extra\" FROM \"all_types_row\"");
    check_ok(raw_sql.status, "raw 多列 SQL 构造", raw_sql.error);
    const uvcpp_orm_status status = uvcpp::find_all<all_types_row>(
        db, raw_sql, &rows, &error);
    check_ok(status, "结果集多出来的列被忽略", error);
    check(rows.size() == 4u, "多列结果集仍然映射 4 行");
  }

  // 缺可空列：写空 nullable<T>，不是错误。
  {
    std::vector<all_types_row> rows;
    const uvcpp_orm_sql_result raw_sql =
        uvcpp::raw(uvcpp_orm_dialect::sqlite,
                   select_all_sql("opt_i32", std::string(), std::string()));
    const uvcpp_orm_status status = uvcpp::find_all<all_types_row>(
        db, raw_sql, &rows, &error);
    check_ok(status, "缺可空列 opt_i32 不报错", error);
    check(rows.size() == 4u, "缺可空列时其余字段照常映射");
    for (std::size_t i = 0; i < rows.size(); ++i) {
      check(!rows[i].opt_i32.has_value(), "缺可空列写空 nullable<T>");
    }
  }

  // 列名必须精确匹配：大小写不同不算同一列（`TEXT` ≠ `text`）。
  {
    std::vector<all_types_row> rows;
    const uvcpp_orm_sql_result raw_sql = uvcpp::raw(
        uvcpp_orm_dialect::sqlite,
        "SELECT \"id\", \"flag\", \"i8\", \"u8\", \"i16\", \"u16\", "
        "\"i32\", \"u32\", \"i64\", \"u64\", \"f32\", \"f64\", "
        "\"text\" AS \"TEXT\", \"blob\", \"opt_i32\", \"opt_text\", "
        "\"opt_bool\", \"opt_i64\", \"opt_f64\" FROM \"all_types_row\"");
    const uvcpp_orm_status status = uvcpp::find_all<all_types_row>(
        db, raw_sql, &rows, &error);
    check_status(status, uvcpp_orm_status::MISSING_COLUMN,
                 "列名大小写不同不算匹配", error);
    check_contains(error, "text", "精确匹配失败时错误串带模型列名");
  }

  // 缺非空列：MISSING_COLUMN，错误串带字段名/列名。
  {
    std::vector<all_types_row> rows;
    const uvcpp_orm_sql_result raw_sql =
        uvcpp::raw(uvcpp_orm_dialect::sqlite,
                   select_all_sql("text", std::string(), std::string()));
    const uvcpp_orm_status status = uvcpp::find_all<all_types_row>(
        db, raw_sql, &rows, &error);
    check_status(status, uvcpp_orm_status::MISSING_COLUMN,
                 "缺非空列 text", error);
    check_contains(error, "text", "缺非空列的错误串要带列名");
  }

  // 类型不匹配：TEXT 塞进 int32。
  {
    std::vector<all_types_row> rows;
    const uvcpp_orm_sql_result raw_sql = uvcpp::raw(
        uvcpp_orm_dialect::sqlite,
        select_all_sql(std::string(), "i32", "'not-an-int'"));
    const uvcpp_orm_status status = uvcpp::find_all<all_types_row>(
        db, raw_sql, &rows, &error);
    check_status(status, uvcpp_orm_status::TYPE_MISMATCH,
                 "TEXT 映射到 int32", error);
    check_contains(error, "i32", "类型不匹配的错误串要带列名");
  }

  // 整数超宽：999 塞不进 int8。
  {
    std::vector<all_types_row> rows;
    const uvcpp_orm_sql_result raw_sql = uvcpp::raw(
        uvcpp_orm_dialect::sqlite,
        select_all_sql(std::string(), "i8", "999"));
    const uvcpp_orm_status status = uvcpp::find_all<all_types_row>(
        db, raw_sql, &rows, &error);
    check_status(status, uvcpp_orm_status::TYPE_MISMATCH,
                 "999 超出 int8 范围", error);
    check_contains(error, "out of range", "超宽错误要说 out of range");
  }

  // 负数塞进无符号：不允许静默变成 0。
  {
    std::vector<all_types_row> rows;
    const uvcpp_orm_sql_result raw_sql = uvcpp::raw(
        uvcpp_orm_dialect::sqlite,
        select_all_sql(std::string(), "u8", "-1"));
    const uvcpp_orm_status status = uvcpp::find_all<all_types_row>(
        db, raw_sql, &rows, &error);
    check_status(status, uvcpp_orm_status::TYPE_MISMATCH,
                 "负数映射到 uint8", error);
  }

  // SQL NULL 塞进非空字段。
  {
    std::vector<all_types_row> rows;
    const uvcpp_orm_sql_result raw_sql =
        uvcpp::raw(uvcpp_orm_dialect::sqlite,
                   select_all_sql(std::string(), "text", "NULL"));
    const uvcpp_orm_status status = uvcpp::find_all<all_types_row>(
        db, raw_sql, &rows, &error);
    check_status(status, uvcpp_orm_status::NULL_VIOLATION,
                 "NULL 映射到非空 text", error);
    check_contains(error, "text", "NULL 违规的错误串要带列名");
  }
}

// ---------------------------------------------------------------------------
// 4. UPDATE / DELETE / NOT_FOUND
// ---------------------------------------------------------------------------

void test_update_and_remove(uvcpp_db_client& db) {
  std::string error;
  uvcpp_db_status database_status = uvcpp_db_status::OK;

  all_types_row row = make_base_row();
  row.text = "update-me";
  uvcpp_db_value id;
  uvcpp_orm_status status =
      uvcpp::insert<all_types_row>(db, row, &id, &error, &database_status);
  check_ok(status, "insert update-me", error);
  const std::int64_t row_id = id.to_int64();

  all_types_row found;
  status = uvcpp::find_by_pk<all_types_row>(db, id, &found, &error,
                                            &database_status);
  check_ok(status, "update 前 find_by_pk", error);

  found.text = "updated";
  found.flag = false;
  found.opt_i32.reset();
  found.opt_text = std::string("changed");
  found.opt_i64 = std::int64_t(123);
  found.blob.assign(1u, 0xa5u);

  int64_t affected = -1;
  status = uvcpp::update<all_types_row>(db, found, &affected, &error,
                                        &database_status);
  check_ok(status, "update 一行", error);
  check(affected == 1, "update 影响 1 行（不是失败）");

  all_types_row updated;
  status = uvcpp::find_by_pk<all_types_row>(db, id, &updated, &error,
                                            &database_status);
  check_ok(status, "update 后 find_by_pk", error);
  check(updated.id == row_id, "update 不 SET 主键，id 不变");
  check(updated.text == "updated", "update 后的 text");
  check(!updated.flag, "update 后的 bool");
  check(!updated.opt_i32.has_value(), "update 把 nullable 写回 NULL");
  check(updated.opt_text.has_value() &&
            updated.opt_text.value() == "changed",
        "update 后的 nullable text");
  check(updated.opt_i64.has_value() && updated.opt_i64.value() == 123,
        "update 后的 nullable int64");
  check(updated.blob.size() == 1u && updated.blob[0] == 0xa5u,
        "update 后的 BLOB");

  // 不存在的行：0 行受影响不是 DB 错误。
  all_types_row missing = updated;
  missing.id = 999999;
  affected = -1;
  status = uvcpp::update<all_types_row>(db, missing, &affected, &error,
                                        &database_status);
  check_ok(status, "update 不存在的主键", error);
  check(affected == 0, "update 0 行与执行失败可区分（affected=0）");
  check(database_status == uvcpp_db_status::OK,
        "update 0 行不是 DB 错误（db_status=OK）");

  // DELETE 一行。
  affected = -1;
  status = uvcpp::remove<all_types_row>(db, id, &affected, &error,
                                        &database_status);
  check_ok(status, "remove 一行", error);
  check(affected == 1, "remove 影响 1 行");

  all_types_row gone;
  status = uvcpp::find_by_pk<all_types_row>(db, id, &gone, &error,
                                            &database_status);
  check_status(status, uvcpp_orm_status::NOT_FOUND,
               "删除后 find_by_pk 返回 NOT_FOUND", error);

  // 再删一次：0 行，仍是 OK。
  affected = -1;
  status = uvcpp::remove<all_types_row>(db, id, &affected, &error,
                                        &database_status);
  check_ok(status, "remove 已删除的主键", error);
  check(affected == 0, "remove 0 行与执行失败可区分（affected=0）");

  // 不存在的 id 查询。
  status = uvcpp::find_by_pk<all_types_row>(
      db, uvcpp_db_value(std::int64_t(999999)), &gone, &error,
      &database_status);
  check_status(status, uvcpp_orm_status::NOT_FOUND,
               "find_by_pk 不存在的主键返回 NOT_FOUND", error);
}

// ---------------------------------------------------------------------------
// 5. 数据库错误透传 / 非法参数
// ---------------------------------------------------------------------------

void test_db_errors_and_invalid_arguments(uvcpp_db_client& db) {
  std::string error;
  uvcpp_db_status database_status = uvcpp_db_status::OK;

  // 唯一键冲突：DB_ERROR + 原始 uvcpp_db_status + last_error 原文。
  {
    all_types_row duplicate = make_base_row();
    duplicate.text = "alpha";  // 已存在
    uvcpp_db_value new_id;
    new_id.set_int64(12345);
    const uvcpp_orm_status status = uvcpp::insert<all_types_row>(
        db, duplicate, &new_id, &error, &database_status);
    check_status(status, uvcpp_orm_status::DB_ERROR, "唯一键冲突透传", error);
    check_db_status(database_status, uvcpp_db_status::EXEC_FAILED,
                    "唯一键冲突的 db_status");
    check_contains(error, "exec_failed", "DB 错误串带返回码短名");
    check_contains(db.last_error(), "UNIQUE", "db.last_error() 保留原文");
    check(new_id.is_null(), "失败的 insert 把 new_id 清成 NULL");
  }

  // UINT64 超出 SQLite 有符号 64 位：后端 BIND_FAILED 要透传。
  {
    all_types_row too_big = make_base_row();
    too_big.text = "too-big";
    too_big.u64 = (std::numeric_limits<std::uint64_t>::max)();
    const uvcpp_orm_status status = uvcpp::insert<all_types_row>(
        db, too_big, nullptr, &error, &database_status);
    check_status(status, uvcpp_orm_status::DB_ERROR,
                 "UINT64_MAX 插入失败透传", error);
    check_db_status(database_status, uvcpp_db_status::BIND_FAILED,
                    "UINT64_MAX 的 db_status 是 BIND_FAILED");
  }

  // 未打开的连接：DB_ERROR + NOT_CONNECTED。
  {
    uvcpp_db_client closed;
    all_types_row row = make_base_row();
    database_status = uvcpp_db_status::OK;
    const uvcpp_orm_status status = uvcpp::insert<all_types_row>(
        closed, row, nullptr, &error, &database_status);
    check_status(status, uvcpp_orm_status::DB_ERROR, "未打开连接 insert",
                 error);
    check_db_status(database_status, uvcpp_db_status::NOT_CONNECTED,
                    "未打开连接的 db_status");
  }

  // 空 out / NULL 主键。
  {
    all_types_row row;
    check_status(uvcpp::find_by_pk<all_types_row>(
                     db, uvcpp_db_value(std::int64_t(1)), nullptr, &error),
                 uvcpp_orm_status::INVALID_ARGUMENT,
                 "find_by_pk 空 out", error);

    check_status(uvcpp::find_by_pk<all_types_row>(
                     db, uvcpp_db_value(), &row, &error),
                 uvcpp_orm_status::INVALID_ARGUMENT,
                 "find_by_pk NULL 主键", error);

    std::vector<all_types_row> rows;
    check_status(uvcpp::find_all<all_types_row>(
                     db, uvcpp::select<all_types_row>(
                             uvcpp_orm_dialect::sqlite),
                     nullptr, &error),
                 uvcpp_orm_status::INVALID_ARGUMENT,
                 "find_all 空 out", error);

    int64_t affected = 123;
    check_status(uvcpp::remove<all_types_row>(db, uvcpp_db_value(),
                                              &affected, &error),
                 uvcpp_orm_status::INVALID_ARGUMENT,
                 "remove NULL 主键", error);
    check(affected == -1, "remove 参数错误时 affected=-1");

    check_status(uvcpp::count<all_types_row>(
                     db,
                     uvcpp::select<all_types_row>(
                         uvcpp_orm_dialect::sqlite),
                     nullptr, &error),
                 uvcpp_orm_status::INVALID_ARGUMENT,
                 "count 空 total", error);
  }

  // 没有声明模型的类型。
  {
    undeclared_row row;
    row.id = 1;
    check_status(uvcpp::insert<undeclared_row>(db, row, nullptr, &error),
                 uvcpp_orm_status::NO_MODEL, "insert 未声明模型", error);
    check_status(uvcpp::update<undeclared_row>(db, row, nullptr, &error),
                 uvcpp_orm_status::NO_MODEL, "update 未声明模型", error);
    check_status(uvcpp::remove<undeclared_row>(
                     db, uvcpp_db_value(std::int64_t(1)), nullptr, &error),
                 uvcpp_orm_status::NO_MODEL, "remove 未声明模型", error);
  }

  // 构造失败的 SQL 结果原样透传。
  {
    const uvcpp_orm_sql_result bad =
        uvcpp::select<all_types_row>(uvcpp_orm_dialect::sqlite)
            .column("missing")
            .build();
    std::vector<all_types_row> rows;
    const uvcpp_orm_status status =
        uvcpp::find_all<all_types_row>(db, bad, &rows, &error);
    check_status(status, uvcpp_orm_status::UNKNOWN_COLUMN,
                 "find_all 透传构造错误", error);
    check_contains(error, "missing", "构造错误串带列名");
  }

  // 方言与连接不匹配：拒绝，而不是拿错占位符去执行。
  {
    const uvcpp_orm_sql_result postgres =
        uvcpp::select<all_types_row>(uvcpp_orm_dialect::postgresql).build();
    std::vector<all_types_row> rows;
    const uvcpp_orm_status status =
        uvcpp::find_all<all_types_row>(db, postgres, &rows, &error);
    check_status(status, uvcpp_orm_status::INVALID_ARGUMENT,
                 "方言不匹配被拒绝", error);
    check_contains(error, "postgresql", "方言错误串带 SQL 的方言");
  }

  // total 推导拒绝选择列表里的占位符 / 没有 FROM 的 SQL。
  {
    uvcpp::uvcpp_db_params params;
    params.add(std::int64_t(1));
    const uvcpp_orm_sql_result placeholder_in_select = uvcpp::raw(
        uvcpp_orm_dialect::sqlite, "SELECT ? AS \"id\"", params);
    std::vector<all_types_row> rows;
    int64_t total = 0;
    check_status(uvcpp::find_all<all_types_row>(
                     db, placeholder_in_select, &rows, &error, &total),
                 uvcpp_orm_status::INVALID_ARGUMENT,
                 "total 拒绝选择列表里的占位符", error);

    const uvcpp_orm_sql_result no_from =
        uvcpp::raw(uvcpp_orm_dialect::sqlite, "SELECT 1");
    check_status(uvcpp::find_all<all_types_row>(
                     db, no_from, &rows, &error, &total),
                 uvcpp_orm_status::INVALID_ARGUMENT,
                 "total 拒绝没有 FROM 的 SQL", error);
  }
}

/// 新增状态码必须在 `uvcpp_orm_status_name()` 的 switch 里有名字。
void test_status_names() {
  check(std::string(uvcpp::uvcpp_orm_status_name(
            uvcpp_orm_status::NOT_FOUND)) == "not_found",
        "NOT_FOUND 的稳定短名");
  check(std::string(uvcpp::uvcpp_orm_status_name(
            uvcpp_orm_status::MAPPING_FAILED)) == "mapping_failed",
        "MAPPING_FAILED 的稳定短名");
  check(std::string(uvcpp::uvcpp_orm_status_name(
            uvcpp_orm_status::MISSING_COLUMN)) == "missing_column",
        "MISSING_COLUMN 的稳定短名");
  check(std::string(uvcpp::uvcpp_orm_status_name(
            uvcpp_orm_status::TYPE_MISMATCH)) == "type_mismatch",
        "TYPE_MISMATCH 的稳定短名");
  check(std::string(uvcpp::uvcpp_orm_status_name(
            uvcpp_orm_status::NULL_VIOLATION)) == "null_violation",
        "NULL_VIOLATION 的稳定短名");
  check(std::string(uvcpp::uvcpp_orm_status_name(
            uvcpp_orm_status::DB_ERROR)) == "db_error",
        "DB_ERROR 的稳定短名");
  check(std::string(uvcpp::uvcpp_orm_status_name(
            uvcpp_orm_status::INVALID_ARGUMENT)) == "invalid_argument",
        "INVALID_ARGUMENT 的稳定短名");
}

int run_all(uvcpp_db_client& db) {
  test_status_names();
  if (create_table(db) != 0) return 1;

  std::vector<uvcpp_db_value> ids;
  test_insert_and_round_trip(db, &ids);
  test_find_all_and_count(db);
  test_mapping_errors(db);
  test_update_and_remove(db);
  test_db_errors_and_invalid_arguments(db);
  return 0;
}

}  // namespace

int main() {
  std::cout << "[orm_sqlite] start" << std::endl;

  const std::string path = temp_dir() + "/uvcpp-orm-sqlite.db";
  std::remove(path.c_str());
  std::remove((path + "-journal").c_str());

  uvcpp_db_client db;
  const uvcpp_db_status opened = db.open("sqlite://" + path);
  check_db_status(opened, uvcpp_db_status::OK,
                  "打开 SQLite（" + db.last_error() + "）");

  int rc = 0;
  if (uvcpp_db_ok(opened)) {
    rc = run_all(db);
    db.close();
  } else {
    rc = 1;
  }

  std::remove(path.c_str());
  std::remove((path + "-journal").c_str());

  std::cout << "[orm_sqlite] checks=" << checks << " failures=" << failures
            << std::endl;
  if (rc != 0 || failures != 0) {
    std::cerr << "[FAIL] orm_sqlite: " << failures << " check(s) failed"
              << std::endl;
    return 1;
  }
  std::cout << "[PASS] orm_sqlite: all " << checks << " checks passed"
            << std::endl;
  return 0;
}
