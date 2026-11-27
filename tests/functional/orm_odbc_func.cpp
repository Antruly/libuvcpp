/**
 * @file tests/functional/orm_odbc_func.cpp
 * @brief ORM 同步 CRUD 的真实 ODBC(SQLite) 用例（不是 mock，也不是 SQLite 方言）。
 *
 * 需要 `UVCPP_ORM_TEST_ODBC_URL`（或 `UVCPP_DB_TEST_ODBC_URL`）指向一个可写的
 * ODBC 数据源（SQLite 驱动）；库名、用户名、密码由 URL 给出。没有 URL 时返回 3（ctest 显示
 * `Skipped`），设了 `UVCPP_DB_TEST_REQUIRE=1` 时缺 URL 是失败（退出 1）。
 *
 * 覆盖与 SQLite 用例相同的对外行为：建表、自增主键回填、点查、条件查询、
 * `nullable` 往返、UPDATE、COUNT、DELETE，以及唯一键冲突不被吞掉。
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <orm/uvcpp_orm_repository.h>

struct orm_odbc_account {
  std::int64_t id;
  std::string email;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(orm_odbc_account, "orm_odbc_accounts")
// ODBC/psqlODBC does not expose a portable last-insert-id; this test supplies
// the primary key explicitly for both SQLite and PostgreSQL data sources.
UVCPP_ORM_FIELD(orm_odbc_account, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_FIELD(orm_odbc_account, email, "email",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(orm_odbc_account, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(orm_odbc_account, score, "score", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(orm_odbc_account, active, "active", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

struct orm_odbc_auto {
  std::int64_t id;
  std::string email;
};

UVCPP_ORM_MODEL(orm_odbc_auto, "orm_odbc_auto")
UVCPP_ORM_FIELD(orm_odbc_auto, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(orm_odbc_auto, email, "email",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_MODEL_END()

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* what) {
  ++checks;
  if (condition) return;
  ++failures;
  std::printf("FAIL: %s\n", what);
}

const char* test_url() {
  const char* url = std::getenv("UVCPP_ORM_TEST_ODBC_URL");
  if (url == nullptr || *url == '\0') {
    url = std::getenv("UVCPP_DB_TEST_ODBC_URL");
  }
  return (url != nullptr && *url != '\0') ? url : nullptr;
}

bool require_server() {
  const char* value = std::getenv("UVCPP_DB_TEST_REQUIRE");
  return value != nullptr && std::string(value) == "1";
}

std::string lower_copy(std::string value) {
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] >= 'A' && value[i] <= 'Z') {
      value[i] = static_cast<char>(value[i] - 'A' + 'a');
    }
  }
  return value;
}

bool odbc_url_is_postgres(const std::string& url) {
  const std::string lower = lower_copy(url);
  return lower.find("psqlodbc") != std::string::npos ||
         lower.find("postgres") != std::string::npos;
}

}  // namespace

int main() {
  const char* url = test_url();
  if (url == nullptr) {
    if (require_server()) {
      std::puts("UVCPP_ORM_TEST_ODBC_URL is required but not set");
      return 1;
    }
    std::puts("SKIP: UVCPP_ORM_TEST_ODBC_URL is not set");
    return 3;
  }

  uvcpp::uvcpp_db_client db;
  uvcpp::uvcpp_db_status status = db.open(url);
  if (!uvcpp::uvcpp_db_ok(status)) {
    std::printf("open failed: %s (%s)\n",
                uvcpp::uvcpp_db_status_name(status),
                db.last_error().c_str());
    return 1;
  }

  uvcpp::uvcpp_db_status db_status = uvcpp::uvcpp_db_status::OK;
  std::string error;
  uvcpp::uvcpp_orm_status orm_status = uvcpp::uvcpp_orm_status::OK;
  const bool postgres = odbc_url_is_postgres(url);
  // ODBC's parameter marker is `?` for every data source (psqlODBC included),
  // so the ORM must use the positional dialect even when the server is
  // PostgreSQL. Identifier quoting stays double-quoted for both.
  const uvcpp::uvcpp_orm_dialect dialect =
      uvcpp::uvcpp_orm_dialect::sqlite;

  status = db.execute("DROP TABLE IF EXISTS orm_odbc_accounts");
  check(uvcpp::uvcpp_db_ok(status), "drop table");
  const char* ddl =
      postgres
          ? "CREATE TABLE orm_odbc_accounts ("
            "id BIGINT PRIMARY KEY,"
            "email VARCHAR(120) NOT NULL UNIQUE,"
            "age INTEGER NULL,"
            "score DOUBLE PRECISION NOT NULL,"
            "active INTEGER NOT NULL)"
          : "CREATE TABLE orm_odbc_accounts ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "email TEXT NOT NULL UNIQUE,"
            "age INTEGER NULL,"
            "score REAL NOT NULL,"
            "active INTEGER NOT NULL)";
  status = db.execute(ddl);
  check(uvcpp::uvcpp_db_ok(status), "create table");
  if (!uvcpp::uvcpp_db_ok(status)) {
    std::printf("create failed: %s (%s)\n",
                uvcpp::uvcpp_db_status_name(status),
                db.last_error().c_str());
    return 1;
  }

  orm_odbc_account alice;
  alice.id = 1;
  alice.email = "alice@example.com";
  alice.age = 30;
  alice.score = 1.5;
  alice.active = true;

  uvcpp::uvcpp_db_value alice_id;
  alice_id.set_int64(1);
  orm_status = uvcpp::insert(db, alice, nullptr, &error, &db_status);
  if (orm_status != uvcpp::uvcpp_orm_status::OK) {
    std::printf("insert detail: %s | %s\n",
                uvcpp::uvcpp_orm_status_name(orm_status), error.c_str());
  }
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "insert");

  orm_odbc_account got;
  orm_status = uvcpp::find_by_pk(db, alice_id, &got, &error, &db_status);
  if (orm_status != uvcpp::uvcpp_orm_status::OK) {
    std::printf("find detail: %s | %s | id=%s\n",
                uvcpp::uvcpp_orm_status_name(orm_status), error.c_str(),
                alice_id.to_text().c_str());
  }
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "find_by_pk");
  check(got.email == alice.email, "find_by_pk email");
  check(got.age.has_value() && got.age.value() == 30, "find_by_pk age");
  check(got.score == 1.5, "find_by_pk score");
  check(got.active, "find_by_pk active");

  got.score = 9.25;
  got.age.reset();
  int64_t affected = -1;
  orm_status = uvcpp::update(db, got, &affected, &error, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "update");
  check(affected == 1, "update affected one row");

  orm_odbc_account after;
  orm_status = uvcpp::find_by_pk(db, alice_id, &after, &error, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "re-read after update");
  check(after.score == 9.25, "update persisted score");
  check(!after.age.has_value(), "update persisted NULL");

  std::vector<orm_odbc_account> rows;
  int64_t total = 0;
  orm_status = uvcpp::find_all(
      db,
      uvcpp::select<orm_odbc_account>(dialect)
          .where_eq(&orm_odbc_account::email,
                    std::string("alice@example.com")),
      &rows, &error, &total, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "find_all");
  check(rows.size() == 1, "find_all row count");
  check(total == 1, "find_all total");

  int64_t counted = -1;
  orm_status = uvcpp::count<orm_odbc_account>(
      db,
      uvcpp::select<orm_odbc_account>(dialect),
      &counted, &error, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "count");
  check(counted == 1, "count value");

  // 事务：回滚不留下行；提交留下。ODBC 的 SQLite 驱动支持标准 BEGIN/COMMIT。
  {
    check(uvcpp::uvcpp_db_ok(db.begin()), "begin rollback tx");
    orm_odbc_account bob;
    bob.id = 2;
    bob.email = "bob-rollback@example.com";
    bob.age = 41;
    bob.score = 3.0;
    bob.active = true;
    orm_status = uvcpp::insert(db, bob, nullptr, &error, &db_status);
    check(orm_status == uvcpp::uvcpp_orm_status::OK, "insert in rollback tx");
    check(uvcpp::uvcpp_db_ok(db.rollback()), "rollback tx");

    int64_t after_rollback = -1;
    orm_status = uvcpp::count<orm_odbc_account>(
        db,
        uvcpp::select<orm_odbc_account>(dialect),
        &after_rollback, &error, &db_status);
    check(orm_status == uvcpp::uvcpp_orm_status::OK, "count after rollback");
    check(after_rollback == 1, "rollback removed the row");

    check(uvcpp::uvcpp_db_ok(db.begin()), "begin commit tx");
    orm_odbc_account carol;
    carol.id = 3;
    carol.email = "carol-commit@example.com";
    carol.age = 42;
    carol.score = 4.0;
    carol.active = false;
    orm_status = uvcpp::insert(db, carol, nullptr, &error, &db_status);
    check(orm_status == uvcpp::uvcpp_orm_status::OK, "insert in commit tx");
    check(uvcpp::uvcpp_db_ok(db.commit()), "commit tx");

    int64_t after_commit = -1;
    orm_status = uvcpp::count<orm_odbc_account>(
        db,
        uvcpp::select<orm_odbc_account>(dialect),
        &after_commit, &error, &db_status);
    check(orm_status == uvcpp::uvcpp_orm_status::OK, "count after commit");
    check(after_commit == 2, "commit kept the row");
  }

  // 唯一键冲突必须从数据库透传，而不是被映射成 0 行/成功。
  orm_odbc_account duplicate;
  duplicate.id = 2;
  duplicate.email = "alice@example.com";
  duplicate.age = 1;
  duplicate.score = 2.0;
  duplicate.active = false;
  uvcpp::uvcpp_db_value duplicate_id;
  orm_status = uvcpp::insert(db, duplicate, &duplicate_id, &error, &db_status);
  check(orm_status != uvcpp::uvcpp_orm_status::OK, "duplicate rejected");
  check(!error.empty(), "duplicate has an error message");

  orm_status = uvcpp::remove<orm_odbc_account>(
      db, alice_id, &affected, &error, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "remove");
  check(affected == 1, "remove affected one row");

  orm_status = uvcpp::find_by_pk(db, alice_id, &got, &error, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::NOT_FOUND, "find after remove");

  // ODBC last-insert-id: SQLite uses last_insert_rowid(), psqlODBC uses lastval().
  db.execute("DROP TABLE IF EXISTS orm_odbc_auto");
  const char* auto_ddl =
      postgres
          ? "CREATE TABLE orm_odbc_auto ("
            "id BIGSERIAL PRIMARY KEY,"
            "email VARCHAR(120) NOT NULL UNIQUE)"
          : "CREATE TABLE orm_odbc_auto ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "email TEXT NOT NULL UNIQUE)";
  status = db.execute(auto_ddl);
  check(uvcpp::uvcpp_db_ok(status), "create auto table");
  if (uvcpp::uvcpp_db_ok(status)) {
    orm_odbc_auto generated;
    generated.email = "auto@example.com";
    uvcpp::uvcpp_db_value generated_id;
    orm_status = uvcpp::insert(db, generated, &generated_id, &error, &db_status);
    check(orm_status == uvcpp::uvcpp_orm_status::OK, "auto increment insert");
    if (generated_id.is_null()) {
      std::printf("auto id detail: direct=%s last_error=%s\n",
                  db.last_insert_id().to_text().c_str(),
                  db.last_error().c_str());
    }
    check(!generated_id.is_null(), "ODBC last-insert-id returned a value");
    if (!generated_id.is_null()) {
      orm_odbc_auto reread;
      orm_status = uvcpp::find_by_pk(db, generated_id, &reread, &error,
                                     &db_status);
      check(orm_status == uvcpp::uvcpp_orm_status::OK,
            "auto increment find_by_pk");
      check(reread.email == generated.email, "auto increment email");
    }
    db.execute("DROP TABLE IF EXISTS orm_odbc_auto");
  }

  db.execute("DROP TABLE IF EXISTS orm_odbc_accounts");
  std::printf("[orm_odbc] checks=%d failures=%d\n", checks, failures);
  if (failures != 0) return 1;
  std::puts("[PASS] orm_odbc");
  return 0;
}
