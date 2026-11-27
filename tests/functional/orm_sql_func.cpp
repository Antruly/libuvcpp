/**
 * @file tests/functional/orm_sql_func.cpp
 * @brief ORM SQL 构造：三方方言、标识符转义、参数绑定与错误路径。
 *
 * 这条用例**不连接数据库、不需要任何客户端库**：它只编译/运行 `src/orm/`
 * 这一层，把生成的 SQL 与期望值逐字节比较。文件名带 `orm_` 前缀，
 * `tests/functional/CMakeLists.txt` 在 `UVCPP_BUILD_ORM=OFF` 时会把整个目标
 * 摘掉 —— “没测”表现为用例不存在，不是又一个恒绿的 SKIP。
 *
 * 覆盖 issue #90 的验收标准：
 *
 *   1. 同一模型在 SQLite/MySQL 生成 `?`、在 PostgreSQL 生成 `$1..$n`；
 *   2. SELECT 列选择 / WHERE 各操作符 / ORDER BY / LIMIT / OFFSET / COUNT 的
 *      golden SQL；
 *   3. INSERT 列顺序、UPDATE 的 SET/WHERE、DELETE 的 WHERE；
 *   4. 参数顺序与数量；值（包括注入语料）只进 `uvcpp_db_params`，不出现在
 *      SQL 文本里；
 *   5. 标识符含双引号/反引号/空格/保留字时的转义；模型里没有的列被拒绝；
 *   6. 无模型、无主键、非法操作符、空 IN、负数 LIMIT/OFFSET、空 WHERE、
 *      参数数量不匹配等错误路径；
 *   7. `raw()` 逃生舱的原样返回、占位符计数与不一致时的错误。
 */

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <orm/uvcpp_orm_query.h>

// ---------------------------------------------------------------------------
// 测试模型
// ---------------------------------------------------------------------------

struct user {
  std::int64_t id;
  std::string name;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(user, "users")
UVCPP_ORM_FIELD(user, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(user, name, "name", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(user, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(user, score, "score", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(user, active, "active", UVCPP_ORM_NONE)
UVCPP_ORM_MODEL_END()

/// 表名是保留字，列名里有双引号、反引号、空格。转义只加引号并翻倍**同一种**
/// 引号，不重写名字。
struct odd_ident {
  std::int64_t id;
  std::int32_t a;
  std::int32_t b;
  std::int32_t c;
};

UVCPP_ORM_MODEL(odd_ident, "order")
UVCPP_ORM_FIELD(odd_ident, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_FIELD(odd_ident, a, "va\"lue", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(odd_ident, b, "o`ther", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(odd_ident, c, "has space", UVCPP_ORM_NONE)
UVCPP_ORM_MODEL_END()

/// 表名本身同时含双引号、反引号与空格：表名走的是和列名同一条转义路径。
struct odd_table {
  std::int64_t id;
};

UVCPP_ORM_MODEL(odd_table, "ta\"ble`x")
UVCPP_ORM_FIELD(odd_table, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_MODEL_END()

/// 声明了元数据、但故意漏掉一个成员：按成员指针选它必须是 UNKNOWN_COLUMN。
struct partially_declared {
  std::int64_t id;
  std::int32_t hidden;
};

UVCPP_ORM_MODEL(partially_declared, "partial")
UVCPP_ORM_FIELD(partially_declared, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_MODEL_END()

struct no_pk_model {
  std::int32_t a;
};

UVCPP_ORM_MODEL(no_pk_model, "no_pk")
UVCPP_ORM_FIELD(no_pk_model, a, "a", UVCPP_ORM_NONE)
UVCPP_ORM_MODEL_END()

/// 没有声明模型，但它是 class 类型 —— 可以实例化构造器，`validate_model`
/// 会报 NO_MODEL。
struct undeclared_model {
  std::int32_t id;
};

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& what) {
  ++checks;
  if (condition) return;
  ++failures;
  std::cerr << "[FAIL] " << what << std::endl;
}

void check_sql(const uvcpp::uvcpp_orm_sql_result& result,
               const std::string& expected, const std::string& what) {
  check(result.status == uvcpp::uvcpp_orm_status::OK,
        what + "：构造必须成功（实得 " +
            uvcpp::uvcpp_orm_status_name(result.status) + " / [" +
            result.error + "]）");
  if (result.status != uvcpp::uvcpp_orm_status::OK) return;
  check(result.sql == expected,
        what + "：SQL 逐字节相等\n  期望 [" + expected + "]\n  实得 [" +
            result.sql + "]");
}

void check_error(const uvcpp::uvcpp_orm_sql_result& result,
                 uvcpp::uvcpp_orm_status expected,
                 const std::string& fragment, const std::string& what) {
  check(result.status != uvcpp::uvcpp_orm_status::OK, what + "：必须失败");
  check(result.status == expected,
        what + "：返回码（期望 " +
            uvcpp::uvcpp_orm_status_name(expected) + "，实得 " +
            uvcpp::uvcpp_orm_status_name(result.status) + " / [" +
            result.error + "]）");
  if (!fragment.empty()) {
    check(result.error.find(fragment) != std::string::npos,
          what + "：错误串要提到 [" + fragment + "]（实得 [" + result.error +
              "]）");
  }
}

void check_param_count(const uvcpp::uvcpp_orm_sql_result& result,
                       std::size_t expected, const std::string& what) {
  check(result.params.size() == expected,
        what + "：参数数量（期望 " + std::to_string(expected) + "，实得 " +
            std::to_string(result.params.size()) + "）");
}

void check_param_int(const uvcpp::uvcpp_orm_sql_result& result,
                     std::size_t index, std::int64_t expected,
                     const std::string& what) {
  const bool ok = index < result.params.size() &&
                  !result.params[index].is_null() &&
                  result.params[index].to_int64() == expected;
  check(ok, what + "：参数[" + std::to_string(index) + "] 是整数 " +
                std::to_string(expected));
}

void check_param_double(const uvcpp::uvcpp_orm_sql_result& result,
                        std::size_t index, double expected,
                        const std::string& what) {
  const bool ok = index < result.params.size() &&
                  !result.params[index].is_null() &&
                  result.params[index].to_double() == expected;
  check(ok, what + "：参数[" + std::to_string(index) + "] 是浮点数");
}

void check_param_bool(const uvcpp::uvcpp_orm_sql_result& result,
                      std::size_t index, bool expected,
                      const std::string& what) {
  const bool ok = index < result.params.size() &&
                  !result.params[index].is_null() &&
                  result.params[index].to_bool() == expected;
  check(ok, what + "：参数[" + std::to_string(index) + "] 是布尔值");
}

void check_param_text(const uvcpp::uvcpp_orm_sql_result& result,
                      std::size_t index, const std::string& expected,
                      const std::string& what) {
  const bool ok = index < result.params.size() &&
                  !result.params[index].is_null() &&
                  result.params[index].type() == uvcpp::uvcpp_db_type::TEXT &&
                  result.params[index].to_text() == expected;
  check(ok, what + "：参数[" + std::to_string(index) + "] 是期望文本");
}

// ---------------------------------------------------------------------------
// 1. 方言与标识符
// ---------------------------------------------------------------------------

void test_dialect() {
  using uvcpp::uvcpp_orm_dialect;

  check(std::string(uvcpp::uvcpp_orm_dialect_name(uvcpp_orm_dialect::sqlite)) ==
            "sqlite",
        "方言短名 sqlite");
  check(std::string(uvcpp::uvcpp_orm_dialect_name(uvcpp_orm_dialect::mysql)) ==
            "mysql",
        "方言短名 mysql");
  check(std::string(
            uvcpp::uvcpp_orm_dialect_name(uvcpp_orm_dialect::postgresql)) ==
            "postgresql",
        "方言短名 postgresql");
  check(!uvcpp::uvcpp_orm_dialect_is_valid(
            static_cast<uvcpp_orm_dialect>(99)),
        "越界方言被判为非法");

  check(uvcpp::uvcpp_orm_placeholder(uvcpp_orm_dialect::sqlite, 1) == "?",
        "SQLite 占位符是 ?（第 1 个）");
  check(uvcpp::uvcpp_orm_placeholder(uvcpp_orm_dialect::sqlite, 7) == "?",
        "SQLite 占位符始终是 ?（第 7 个）");
  check(uvcpp::uvcpp_orm_placeholder(uvcpp_orm_dialect::mysql, 3) == "?",
        "MySQL 占位符是 ?");
  check(uvcpp::uvcpp_orm_placeholder(uvcpp_orm_dialect::postgresql, 1) == "$1",
        "PostgreSQL 第 1 个占位符是 $1");
  check(uvcpp::uvcpp_orm_placeholder(uvcpp_orm_dialect::postgresql, 12) ==
            "$12",
        "PostgreSQL 第 12 个占位符是 $12");

  check(uvcpp::uvcpp_orm_quote_identifier(uvcpp_orm_dialect::sqlite, "users") ==
            "\"users\"",
        "SQLite 标识符用双引号");
  check(uvcpp::uvcpp_orm_quote_identifier(uvcpp_orm_dialect::sqlite,
                                          "a\"b") == "\"a\"\"b\"",
        "SQLite 双引号翻倍");
  check(uvcpp::uvcpp_orm_quote_identifier(uvcpp_orm_dialect::mysql, "users") ==
            "`users`",
        "MySQL 标识符用反引号");
  check(uvcpp::uvcpp_orm_quote_identifier(uvcpp_orm_dialect::mysql, "a`b") ==
            "`a``b`",
        "MySQL 反引号翻倍");
  check(uvcpp::uvcpp_orm_quote_identifier(uvcpp_orm_dialect::mysql, "a\"b") ==
            "`a\"b`",
        "MySQL 不翻倍双引号（它不是 MySQL 的引号字符）");
  check(uvcpp::uvcpp_orm_quote_identifier(uvcpp_orm_dialect::postgresql,
                                          "a`b") == "\"a`b\"",
        "PostgreSQL 不翻倍反引号");
  check(uvcpp::uvcpp_orm_quote_identifier(
            static_cast<uvcpp_orm_dialect>(99), "x").empty(),
        "非法方言不返回未加引号的标识符");
}

// ---------------------------------------------------------------------------
// 2. SELECT golden SQL
// ---------------------------------------------------------------------------

void test_select_golden() {
  using uvcpp::uvcpp_orm_dialect;

  // 默认选择全部列，顺序与模型声明一致。
  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::sqlite).build();
    check_sql(r,
              R"sql(SELECT "id", "name", "age", "score", "active" FROM "users")sql",
              "SQLite 默认 SELECT 全部列");
    check_param_count(r, 0u, "无 WHERE 时没有参数");
  }

  // 列子集 + WHERE：三方方言的占位符差异。
  {
    const uvcpp::uvcpp_orm_sql_result sqlite =
        uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
            .column(&user::id)
            .column(&user::name)
            .where_eq(&user::id, std::int64_t(7))
            .build();
    check_sql(sqlite, R"sql(SELECT "id", "name" FROM "users" WHERE "id" = ?)sql",
              "SQLite SELECT 子集 + eq");
    check_param_count(sqlite, 1u, "SQLite eq 一个参数");
    check_param_int(sqlite, 0u, 7, "SQLite eq 参数值");

    const uvcpp::uvcpp_orm_sql_result postgres =
        uvcpp::select<user>(uvcpp_orm_dialect::postgresql)
            .column(&user::id)
            .column(&user::name)
            .where_eq(&user::id, std::int64_t(7))
            .build();
    check_sql(postgres,
              R"sql(SELECT "id", "name" FROM "users" WHERE "id" = $1)sql",
              "PostgreSQL SELECT 子集 + eq");
    check_param_int(postgres, 0u, 7, "PostgreSQL eq 参数值");

    const uvcpp::uvcpp_orm_sql_result mysql =
        uvcpp::select<user>(uvcpp_orm_dialect::mysql)
            .column(&user::id)
            .column(&user::name)
            .where_eq(&user::id, std::int64_t(7))
            .build();
    check_sql(mysql,
              R"sql(SELECT `id`, `name` FROM `users` WHERE `id` = ?)sql",
              "MySQL SELECT 子集 + eq");
    check_param_int(mysql, 0u, 7, "MySQL eq 参数值");
  }

  // 各二元操作符：SQL 里只出现操作符与占位符，值全部在 params。
  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::postgresql)
            .where_ne(&user::name, std::string("bob"))
            .where_lt(&user::score, 1.5)
            .where_le(&user::score, 2.5)
            .where_gt(&user::score, 0.5)
            .where_ge(&user::age, std::int32_t(18))
            .where_like(&user::name, std::string("a%"))
            .build();
    check_sql(
        r,
        R"sql(SELECT "id", "name", "age", "score", "active" FROM "users" WHERE "name" <> $1 AND "score" < $2 AND "score" <= $3 AND "score" > $4 AND "age" >= $5 AND "name" LIKE $6)sql",
        "PostgreSQL 六个二元操作符");
    check_param_count(r, 6u, "六个操作符六个参数");
    check_param_text(r, 0u, "bob", "ne 参数");
    check_param_double(r, 1u, 1.5, "lt 参数");
    check_param_double(r, 2u, 2.5, "le 参数");
    check_param_double(r, 3u, 0.5, "gt 参数");
    check_param_int(r, 4u, 18, "ge 参数");
    check_param_text(r, 5u, "a%", "like 参数（不是 SQL 字面量）");
    check(r.sql.find("a%") == std::string::npos,
          "LIKE 的值不出现在 SQL 文本里");
  }

  // IN / IS NULL / IS NOT NULL 与 OR 组。
  {
    const std::vector<std::int64_t> ids = {1, 2, 3};
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
            .where_in(&user::id, ids)
            .or_where_is_null(&user::age)
            .or_where_is_not_null(&user::name)
            .build();
    check_sql(
        r,
        R"sql(SELECT "id", "name", "age", "score", "active" FROM "users" WHERE ("id" IN (?, ?, ?)) OR ("age" IS NULL) OR ("name" IS NOT NULL))sql",
        "SQLite IN + IS NULL + IS NOT NULL 的 OR 组");
    check_param_count(r, 3u, "IN 的每个元素都进参数");
    check_param_int(r, 0u, 1, "IN 参数 0");
    check_param_int(r, 1u, 2, "IN 参数 1");
    check_param_int(r, 2u, 3, "IN 参数 2");
  }

  // AND 组内结合，组间 OR；括号明确优先级。
  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::postgresql)
            .where_ge(&user::age, std::int32_t(18))
            .where_lt(&user::age, std::int32_t(65))
            .or_where_eq(&user::name, std::string("root"))
            .build();
    check_sql(
        r,
        R"sql(SELECT "id", "name", "age", "score", "active" FROM "users" WHERE ("age" >= $1 AND "age" < $2) OR ("name" = $3))sql",
        "AND 组内、OR 组间与括号");
    check_param_count(r, 3u, "OR 组合并后的参数数量");
  }

  // ORDER BY / LIMIT / OFFSET：方向和整数都不拼字面量。
  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::postgresql)
            .column(&user::id)
            .where_eq(&user::name, std::string("n"))
            .order_by(&user::score, uvcpp::uvcpp_orm_order::desc)
            .order_by(&user::id, uvcpp::uvcpp_orm_order::asc)
            .limit(424242)
            .offset(747474)
            .build();
    check_sql(
        r,
        R"sql(SELECT "id" FROM "users" WHERE "name" = $1 ORDER BY "score" DESC, "id" ASC LIMIT $2 OFFSET $3)sql",
        "PostgreSQL ORDER BY / LIMIT / OFFSET");
    check_param_count(r, 3u, "ORDER/LIMIT/OFFSET 参数数量");
    check_param_text(r, 0u, "n", "WHERE 参数在 LIMIT 之前");
    check_param_int(r, 1u, 424242, "LIMIT 是绑定参数");
    check_param_int(r, 2u, 747474, "OFFSET 是绑定参数");
    check(r.sql.find("424242") == std::string::npos &&
              r.sql.find("747474") == std::string::npos,
          "LIMIT/OFFSET 的值不出现在 SQL 文本里");
  }

  // MySQL 的 ORDER BY / LIMIT / OFFSET。
  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::mysql)
            .column(&user::id)
            .order_by(&user::id, uvcpp::uvcpp_orm_order::asc)
            .limit(3)
            .build();
    check_sql(r,
              R"sql(SELECT `id` FROM `users` ORDER BY `id` ASC LIMIT ?)sql",
              "MySQL ORDER BY / LIMIT");
    check_param_int(r, 0u, 3, "MySQL LIMIT 参数");
  }
}

// ---------------------------------------------------------------------------
// 3. COUNT
// ---------------------------------------------------------------------------

void test_count() {
  using uvcpp::uvcpp_orm_dialect;

  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
            .count()
            .where_eq(&user::active, true)
            .build();
    check_sql(r, R"sql(SELECT COUNT(*) FROM "users" WHERE "active" = ?)sql",
              "SQLite COUNT(*)");
    check_param_count(r, 1u, "COUNT 的 WHERE 参数");
    check_param_bool(r, 0u, true, "COUNT 的 WHERE 参数值");
    check(r.sql.find("FROM \"users\"") != std::string::npos,
          "COUNT 仍然带表名");
    check(r.sql.find(R"sql(SELECT "id")sql") == std::string::npos,
          "COUNT 不返回行数据（没有列清单）");
  }

  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::postgresql).count().build();
    check_sql(r, R"sql(SELECT COUNT(*) FROM "users")sql",
              "PostgreSQL COUNT(*) 无 WHERE");
    check_param_count(r, 0u, "COUNT 无 WHERE 时没有参数");
  }

  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::mysql)
            .count()
            .where_eq(&user::name, std::string("x"))
            .build();
    check_sql(r,
              R"sql(SELECT COUNT(*) FROM `users` WHERE `name` = ?)sql",
              "MySQL COUNT(*)");
    check_param_text(r, 0u, "x", "MySQL COUNT 参数");
  }

  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .count()
                  .limit(10)
                  .build(),
              uvcpp::uvcpp_orm_status::INVALID_COUNT_QUERY, "count",
              "COUNT 不能和 LIMIT 组合");
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .count()
                  .order_by(&user::id, uvcpp::uvcpp_orm_order::asc)
                  .build(),
              uvcpp::uvcpp_orm_status::INVALID_COUNT_QUERY, "count",
              "COUNT 不能和 ORDER BY 组合");
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .column(&user::id)
                  .count()
                  .build(),
              uvcpp::uvcpp_orm_status::INVALID_COUNT_QUERY, "count",
              "COUNT 不能和列选择组合");
}

// ---------------------------------------------------------------------------
// 4. INSERT / UPDATE / DELETE
// ---------------------------------------------------------------------------

void test_insert_golden() {
  using uvcpp::uvcpp_orm_dialect;

  // 列顺序是调用顺序，不是模型声明顺序（模型里 age 在 name 之后）。
  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::insert<user>(uvcpp_orm_dialect::sqlite)
            .column(&user::age, std::int32_t(30))
            .column(&user::name, std::string("Alice"))
            .build();
    check_sql(r,
              R"sql(INSERT INTO "users" ("age", "name") VALUES (?, ?))sql",
              "SQLite INSERT 列顺序");
    check_param_count(r, 2u, "INSERT 参数数量");
    check_param_int(r, 0u, 30, "INSERT 参数 0 跟列顺序");
    check_param_text(r, 1u, "Alice", "INSERT 参数 1 跟列顺序");
    check(r.sql.find("Alice") == std::string::npos,
          "INSERT 的值不出现在 SQL 文本里");
  }

  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::insert<user>(uvcpp_orm_dialect::postgresql)
            .column(&user::name, std::string("Alice"))
            .column(&user::score, 1.25)
            .build();
    check_sql(r,
              R"sql(INSERT INTO "users" ("name", "score") VALUES ($1, $2))sql",
              "PostgreSQL INSERT");
    check_param_text(r, 0u, "Alice", "PostgreSQL INSERT 参数 0");
    check_param_double(r, 1u, 1.25, "PostgreSQL INSERT 参数 1");
  }

  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::insert<user>(uvcpp_orm_dialect::mysql)
            .column(&user::name, std::string("Alice"))
            .build();
    check_sql(r, R"sql(INSERT INTO `users` (`name`) VALUES (?))sql",
              "MySQL INSERT");
    check_param_text(r, 0u, "Alice", "MySQL INSERT 参数");
  }

  check_error(uvcpp::insert<user>(uvcpp_orm_dialect::sqlite).build(),
              uvcpp::uvcpp_orm_status::EMPTY_COLUMN_LIST, "insert",
              "空 INSERT 被拒绝");
  check_error(uvcpp::insert<user>(uvcpp_orm_dialect::sqlite)
                  .column("missing", 1)
                  .build(),
              uvcpp::uvcpp_orm_status::UNKNOWN_COLUMN, "missing",
              "INSERT 不存在的列被拒绝");
}

void test_update_golden() {
  using uvcpp::uvcpp_orm_dialect;

  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::update<user>(uvcpp_orm_dialect::mysql)
            .set(&user::name, std::string("Bob"))
            .set(&user::active, true)
            .where_eq(&user::id, std::int64_t(9))
            .build();
    check_sql(
        r,
        R"sql(UPDATE `users` SET `name` = ?, `active` = ? WHERE `id` = ?)sql",
        "MySQL UPDATE SET/WHERE");
    check_param_count(r, 3u, "UPDATE 参数数量");
    check_param_text(r, 0u, "Bob", "UPDATE SET 参数 0");
    check_param_bool(r, 1u, true, "UPDATE SET 参数 1");
    check_param_int(r, 2u, 9, "UPDATE WHERE 参数在 SET 之后");
  }

  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::update<user>(uvcpp_orm_dialect::postgresql)
            .set(&user::name, std::string("Bob"))
            .where_eq(&user::id, std::int64_t(9))
            .where_eq(&user::active, true)
            .build();
    check_sql(
        r,
        R"sql(UPDATE "users" SET "name" = $1 WHERE "id" = $2 AND "active" = $3)sql",
        "PostgreSQL UPDATE SET/WHERE");
    check_param_count(r, 3u, "PostgreSQL UPDATE 参数数量");
  }

  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::update<user>(uvcpp_orm_dialect::sqlite)
            .set(&user::name, std::string("Bob"))
            .where_eq(&user::id, std::int64_t(9))
            .build();
    check_sql(r,
              R"sql(UPDATE "users" SET "name" = ? WHERE "id" = ?)sql",
              "SQLite UPDATE SET/WHERE");
  }

  check_error(uvcpp::update<user>(uvcpp_orm_dialect::sqlite)
                  .where_eq(&user::id, std::int64_t(9))
                  .build(),
              uvcpp::uvcpp_orm_status::EMPTY_SET, "update",
              "没有 SET 列的 UPDATE 被拒绝");
  check_error(uvcpp::update<user>(uvcpp_orm_dialect::sqlite)
                  .set(&user::name, std::string("Bob"))
                  .build(),
              uvcpp::uvcpp_orm_status::EMPTY_WHERE, "where",
              "空 WHERE 的 UPDATE 被拒绝（不做全表写）");
  check_error(uvcpp::update<user>(uvcpp_orm_dialect::sqlite)
                  .set("missing", 1)
                  .where_eq(&user::id, std::int64_t(9))
                  .build(),
              uvcpp::uvcpp_orm_status::UNKNOWN_COLUMN, "missing",
              "UPDATE 不存在的列被拒绝");
}

void test_delete_golden() {
  using uvcpp::uvcpp_orm_dialect;

  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::delete_from<user>(uvcpp_orm_dialect::sqlite)
            .where_eq(&user::id, std::int64_t(9))
            .where_eq(&user::name, std::string("x"))
            .build();
    check_sql(r,
              R"sql(DELETE FROM "users" WHERE "id" = ? AND "name" = ?)sql",
              "SQLite DELETE WHERE");
    check_param_count(r, 2u, "DELETE 参数数量");
    check_param_int(r, 0u, 9, "DELETE 参数 0");
    check_param_text(r, 1u, "x", "DELETE 参数 1");
  }

  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::delete_from<user>(uvcpp_orm_dialect::postgresql)
            .where_eq(&user::id, std::int64_t(9))
            .build();
    check_sql(r, R"sql(DELETE FROM "users" WHERE "id" = $1)sql",
              "PostgreSQL DELETE WHERE");
    check_param_int(r, 0u, 9, "PostgreSQL DELETE 参数");
  }

  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::delete_from<user>(uvcpp_orm_dialect::mysql)
            .where_eq(&user::id, std::int64_t(9))
            .build();
    check_sql(r, R"sql(DELETE FROM `users` WHERE `id` = ?)sql",
              "MySQL DELETE WHERE");
  }

  check_error(uvcpp::delete_from<user>(uvcpp_orm_dialect::sqlite).build(),
              uvcpp::uvcpp_orm_status::EMPTY_WHERE, "where",
              "空 WHERE 的 DELETE 被拒绝（不做全表删）");
  check_error(uvcpp::delete_from<user>(uvcpp_orm_dialect::sqlite)
                  .where_eq("missing", 1)
                  .build(),
              uvcpp::uvcpp_orm_status::UNKNOWN_COLUMN, "missing",
              "DELETE 不存在的列被拒绝");
}

// ---------------------------------------------------------------------------
// 5. 标识符转义
// ---------------------------------------------------------------------------

void test_identifier_escaping() {
  using uvcpp::uvcpp_orm_dialect;

  // 表名 "order" 是保留字；列名分别含双引号、反引号、空格。
  {
    const uvcpp::uvcpp_orm_sql_result sqlite =
        uvcpp::select<odd_ident>(uvcpp_orm_dialect::sqlite).build();
    check_sql(
        sqlite,
        R"sql(SELECT "id", "va""lue", "o`ther", "has space" FROM "order")sql",
        "SQLite 奇怪标识符转义");

    const uvcpp::uvcpp_orm_sql_result postgres =
        uvcpp::select<odd_ident>(uvcpp_orm_dialect::postgresql).build();
    check_sql(
        postgres,
        R"sql(SELECT "id", "va""lue", "o`ther", "has space" FROM "order")sql",
        "PostgreSQL 奇怪标识符转义");

    const uvcpp::uvcpp_orm_sql_result mysql =
        uvcpp::select<odd_ident>(uvcpp_orm_dialect::mysql).build();
    check_sql(
        mysql,
        R"sql(SELECT `id`, `va"lue`, `o``ther`, `has space` FROM `order`)sql",
        "MySQL 奇怪标识符转义");
  }

  // 表名自己带引号/反引号/空格时，同样只翻倍“本方言的引号字符”。
  {
    const uvcpp::uvcpp_orm_sql_result sqlite =
        uvcpp::select<odd_table>(uvcpp_orm_dialect::sqlite).build();
    check_sql(sqlite, R"sql(SELECT "id" FROM "ta""ble`x")sql",
              "SQLite 表名转义");
    const uvcpp::uvcpp_orm_sql_result mysql =
        uvcpp::select<odd_table>(uvcpp_orm_dialect::mysql).build();
    check_sql(mysql, R"sql(SELECT `id` FROM `ta"ble``x`)sql",
              "MySQL 表名转义");
    const uvcpp::uvcpp_orm_sql_result postgres =
        uvcpp::select<odd_table>(uvcpp_orm_dialect::postgresql).build();
    check_sql(postgres, R"sql(SELECT "id" FROM "ta""ble`x")sql",
              "PostgreSQL 表名转义");
  }

  // 模型里没有的列：选列、WHERE、ORDER BY、INSERT、UPDATE 都必须拒绝。
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .column("missing")
                  .build(),
              uvcpp::uvcpp_orm_status::UNKNOWN_COLUMN, "missing",
              "SELECT 不存在的列被拒绝");
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .where_eq("missing", 1)
                  .build(),
              uvcpp::uvcpp_orm_status::UNKNOWN_COLUMN, "missing",
              "WHERE 不存在的列被拒绝");
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .order_by("missing", uvcpp::uvcpp_orm_order::asc)
                  .build(),
              uvcpp::uvcpp_orm_status::UNKNOWN_COLUMN, "missing",
              "ORDER BY 不存在的列被拒绝");
  check_error(uvcpp::select<partially_declared>(uvcpp_orm_dialect::sqlite)
                  .column(&partially_declared::hidden)
                  .build(),
              uvcpp::uvcpp_orm_status::UNKNOWN_COLUMN, "member pointer",
              "成员指针不在模型里时被拒绝");
}

// ---------------------------------------------------------------------------
// 6. 注入语料：只进参数，不改变 SQL 结构
// ---------------------------------------------------------------------------

void test_injection() {
  using uvcpp::uvcpp_orm_dialect;

  std::vector<std::string> corpus;
  corpus.push_back(std::string("O'Reilly"));
  corpus.push_back(std::string("x'; DROP TABLE users; --"));
  corpus.push_back(std::string("/* comment */"));
  corpus.push_back(std::string("\"quoted\""));
  corpus.push_back(std::string("semi;colon"));
  corpus.push_back(std::string("a\0b", 3));      // 中间有 NUL 字节
  corpus.push_back(std::string("\xE7\x94\xA8\xE6\x88\xB7"));  // “用户”
  corpus.push_back(std::string("line\nbreak"));

  const std::string expected_sql =
      R"sql(SELECT "id" FROM "users" WHERE "name" = ?)sql";

  for (std::size_t i = 0; i < corpus.size(); ++i) {
    const std::string& value = corpus[i];
    const std::string label =
        "注入语料[" + std::to_string(i) + "]（" +
        std::to_string(value.size()) + " 字节）";

    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
            .column(&user::id)
            .where_eq(&user::name, value)
            .build();

    check_sql(r, expected_sql, label + " 不改变 SQL 结构");
    check_param_count(r, 1u, label + " 参数数量");
    check(r.params[0].type() == uvcpp::uvcpp_db_type::TEXT,
          label + " 作为 TEXT 参数");
    check(r.params[0].bytes() == value,
          label + " 参数字节与输入完全一致");
    check(r.sql.find(value) == std::string::npos,
          label + " 原文不出现在 SQL 文本里");
  }

  // IN 的每个元素都进参数；注入语料同样只作为参数。
  {
    const std::vector<std::string> names = {
        std::string("a'"), std::string("b;--"), std::string("c\0d", 3)};
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
            .column(&user::id)
            .where_in(&user::name, names)
            .build();
    check_sql(r,
              R"sql(SELECT "id" FROM "users" WHERE "name" IN (?, ?, ?))sql",
              "IN 注入语料不改变 SQL 结构");
    check_param_count(r, 3u, "IN 注入语料参数数量");
    check(r.params[0].bytes() == names[0], "IN 参数 0");
    check(r.params[1].bytes() == names[1], "IN 参数 1");
    check(r.params[2].bytes() == names[2], "IN 参数 2");
  }

  // 同样的语料走 INSERT：值仍然只在参数里。
  {
    const std::string nasty = "x'); DROP TABLE users; --";
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::insert<user>(uvcpp_orm_dialect::sqlite)
            .column(&user::name, nasty)
            .build();
    check_sql(r, R"sql(INSERT INTO "users" ("name") VALUES (?))sql",
              "INSERT 注入语料不改变 SQL 结构");
    check_param_text(r, 0u, nasty, "INSERT 注入语料参数");
  }
}

// ---------------------------------------------------------------------------
// 7. 错误路径
// ---------------------------------------------------------------------------

void test_errors() {
  using uvcpp::uvcpp_orm_dialect;
  using uvcpp::uvcpp_orm_status;

  // 模型校验复用 validate_model<T>()。
  check_error(uvcpp::select<undeclared_model>(uvcpp_orm_dialect::sqlite)
                  .build(),
              uvcpp_orm_status::NO_MODEL, "no model",
              "没有模型的类型不能构造 SQL");
  check_error(uvcpp::insert<undeclared_model>(uvcpp_orm_dialect::sqlite)
                  .build(),
              uvcpp_orm_status::NO_MODEL, "no model",
              "INSERT 同样先跑模型校验");
  check_error(
      uvcpp::select<no_pk_model>(uvcpp_orm_dialect::sqlite).build(),
      uvcpp_orm_status::NO_PRIMARY_KEY, "no primary key",
      "无主键模型不能构造 SQL");

  // 方言必须显式且合法。
  check_error(
      uvcpp::select<user>(static_cast<uvcpp_orm_dialect>(99)).build(),
      uvcpp_orm_status::INVALID_DIALECT, "dialect",
      "非法方言被拒绝");

  // 非法操作符 / 方向枚举。
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .where(static_cast<uvcpp::uvcpp_orm_operator>(99), "id", 1)
                  .build(),
              uvcpp_orm_status::INVALID_OPERATOR, "operator",
              "非法操作符被拒绝");
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .order_by(&user::id,
                            static_cast<uvcpp::uvcpp_orm_order>(99))
                  .build(),
              uvcpp_orm_status::INVALID_ORDER_DIRECTION, "direction",
              "非法 ORDER BY 方向被拒绝");

  // 空 IN。
  {
    const std::vector<std::int64_t> empty;
    check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                    .where_in(&user::id, empty)
                    .build(),
                uvcpp_orm_status::EMPTY_IN_VALUES, "empty IN",
                "空 IN 集合被拒绝");
  }
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .where_in(&user::id, std::vector<std::int64_t>())
                  .build(),
              uvcpp_orm_status::EMPTY_IN_VALUES, "empty IN",
              "空 IN 集合被拒绝（临时容器）");

  // LIMIT / OFFSET。
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .limit(-1)
                  .build(),
              uvcpp_orm_status::INVALID_LIMIT, "limit",
              "负数 LIMIT 被拒绝");
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .limit(1)
                  .offset(-1)
                  .build(),
              uvcpp_orm_status::INVALID_OFFSET, "offset",
              "负数 OFFSET 被拒绝");
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .offset(1)
                  .build(),
              uvcpp_orm_status::OFFSET_WITHOUT_LIMIT, "limit",
              "只有 OFFSET 没有 LIMIT 被拒绝");

  // 参数数量不匹配（通用 where 形态）。
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .where(uvcpp::uvcpp_orm_operator::eq, "id")
                  .build(),
              uvcpp_orm_status::PARAM_COUNT_MISMATCH, "one value",
              "eq 没给值被拒绝");
  check_error(uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
                  .where(uvcpp::uvcpp_orm_operator::is_null, "age",
                         uvcpp::uvcpp_db_value(1))
                  .build(),
              uvcpp_orm_status::PARAM_COUNT_MISMATCH, "no value",
              "IS NULL 带值被拒绝");

  // LIMIT 0 是合法的，并且仍然进参数。
  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::select<user>(uvcpp_orm_dialect::sqlite)
            .column(&user::id)
            .limit(0)
            .build();
    check_sql(r, R"sql(SELECT "id" FROM "users" LIMIT ?)sql",
              "LIMIT 0 合法");
    check_param_int(r, 0u, 0, "LIMIT 0 作为参数");
  }
}

// ---------------------------------------------------------------------------
// 8. raw() 逃生舱
// ---------------------------------------------------------------------------

void test_raw() {
  using uvcpp::uvcpp_orm_dialect;
  using uvcpp::uvcpp_orm_status;

  // 警告：raw() 不转义标识符、不参数化值。下面这条只验证“原样返回”。
  {
    const std::string sql = R"sql(SELECT * FROM users WHERE name = ?)sql";
    uvcpp::uvcpp_db_params params;
    params.add(std::string("x"));
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::raw(uvcpp_orm_dialect::sqlite, sql, params);
    check_sql(r, sql, "raw 原样返回 SQL（标识符不自动转义）");
    check_param_count(r, 1u, "raw 参数原样带出");
    check_param_text(r, 0u, "x", "raw 参数值");
  }

  // 引号/注释里的 ? 不算占位符。
  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::raw(uvcpp_orm_dialect::sqlite,
                   R"sql(SELECT '?' AS q /* ? */ -- trailing ?)sql");
    check_sql(r, R"sql(SELECT '?' AS q /* ? */ -- trailing ?)sql",
              "raw 跳过引号与注释里的问号");
    check_param_count(r, 0u, "raw 跳过引号/注释后没有参数");
  }
  {
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::raw(uvcpp_orm_dialect::sqlite,
                   R"sql(SELECT "?" FROM "users")sql");
    check_sql(r, R"sql(SELECT "?" FROM "users")sql",
              "raw 跳过带引号标识符里的问号");
  }
  {
    // MySQL 的 `#` 注释也跳过（只对这一种方言生效）。
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::raw(uvcpp_orm_dialect::mysql, R"sql(SELECT 1 # ?)sql");
    check_sql(r, R"sql(SELECT 1 # ?)sql", "raw 跳过 MySQL # 注释里的问号");
    check_param_count(r, 0u, "raw # 注释后没有参数");
  }

  // PostgreSQL 的 $n 连续性检查。
  {
    uvcpp::uvcpp_db_params params;
    params.add(1).add(2);
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::raw(uvcpp_orm_dialect::postgresql,
                   R"sql(SELECT $1, $2)sql", params);
    check_sql(r, R"sql(SELECT $1, $2)sql", "raw PostgreSQL $1..$2");
    check_param_count(r, 2u, "raw PostgreSQL 参数数量");
  }
  {
    uvcpp::uvcpp_db_params params;
    params.add(1);
    const uvcpp::uvcpp_orm_sql_result r =
        uvcpp::raw(uvcpp_orm_dialect::postgresql,
                   R"sql(SELECT $1, $1)sql", params);
    check_sql(r, R"sql(SELECT $1, $1)sql", "raw PostgreSQL 重复 $1");
  }
  check_error(
      uvcpp::raw(uvcpp_orm_dialect::postgresql, R"sql(SELECT $2)sql",
                 uvcpp::uvcpp_db_params{1, 2}),
      uvcpp_orm_status::PARAM_COUNT_MISMATCH, "contiguous",
      "raw PostgreSQL 不连续的 $n 被拒绝");
  check_error(
      uvcpp::raw(uvcpp_orm_dialect::postgresql, R"sql(SELECT '?')sql",
                 uvcpp::uvcpp_db_params{1}),
      uvcpp_orm_status::PARAM_COUNT_MISMATCH, "placeholder",
      "raw PostgreSQL 不把 ? 当占位符");
  check_error(
      uvcpp::raw(uvcpp_orm_dialect::postgresql,
                 R"sql(SELECT $999999999999999999999)sql"),
      uvcpp_orm_status::PARAM_COUNT_MISMATCH, "contiguous",
      "raw PostgreSQL 超大 $n 被拒绝（不分配、不回绕）");

  // 数量不匹配。
  check_error(uvcpp::raw(uvcpp_orm_dialect::sqlite, R"sql(SELECT ?)sql"),
              uvcpp_orm_status::PARAM_COUNT_MISMATCH, "placeholder",
              "raw 占位符多于参数");
  check_error(uvcpp::raw(uvcpp_orm_dialect::sqlite, R"sql(SELECT 1)sql",
                         uvcpp::uvcpp_db_params{1}),
              uvcpp_orm_status::PARAM_COUNT_MISMATCH, "placeholder",
              "raw 参数多于占位符");

  // 空 SQL / 非法方言。
  check_error(uvcpp::raw(uvcpp_orm_dialect::sqlite, "   \n\t"),
              uvcpp_orm_status::EMPTY_RAW_SQL, "empty",
              "raw 空白 SQL 被拒绝");
  check_error(uvcpp::raw(static_cast<uvcpp_orm_dialect>(99), "SELECT 1"),
              uvcpp_orm_status::INVALID_DIALECT, "dialect",
              "raw 非法方言被拒绝");
}

}  // namespace

int main() {
  std::cout << "[orm_sql] start" << std::endl;

  test_dialect();
  test_select_golden();
  test_count();
  test_insert_golden();
  test_update_golden();
  test_delete_golden();
  test_identifier_escaping();
  test_injection();
  test_errors();
  test_raw();

  std::cout << "[orm_sql] checks=" << checks << " failures=" << failures
            << std::endl;
  if (failures != 0) {
    std::cerr << "[FAIL] orm_sql: " << failures << " check(s) failed"
              << std::endl;
    return 1;
  }
  std::cout << "[PASS] orm_sql: all " << checks << " checks passed"
            << std::endl;
  return 0;
}
