/**
 * @file tests/functional/orm_pgsql_func.cpp
 * @brief ORM 同步 CRUD 的真实 PostgreSQL 用例（不是 mock，也不是 SQLite 方言）。
 *
 * 需要 `UVCPP_ORM_TEST_PGSQL_URL`（或 `UVCPP_DB_TEST_PGSQL_URL`）指向一个可写的
 * PostgreSQL 库；库名、用户名、密码由 URL 给出。没有 URL 时返回 3（ctest 显示
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

struct orm_pgsql_account {
  std::int64_t id;
  std::string email;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(orm_pgsql_account, "orm_pgsql_accounts")
UVCPP_ORM_FIELD(orm_pgsql_account, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(orm_pgsql_account, email, "email",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(orm_pgsql_account, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(orm_pgsql_account, score, "score", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(orm_pgsql_account, active, "active", UVCPP_ORM_NOT_NULL)
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
  const char* url = std::getenv("UVCPP_ORM_TEST_PGSQL_URL");
  if (url == nullptr || *url == '\0') {
    url = std::getenv("UVCPP_DB_TEST_PGSQL_URL");
  }
  return (url != nullptr && *url != '\0') ? url : nullptr;
}

bool require_server() {
  const char* value = std::getenv("UVCPP_DB_TEST_REQUIRE");
  return value != nullptr && std::string(value) == "1";
}

}  // namespace

int main() {
  const char* url = test_url();
  if (url == nullptr) {
    if (require_server()) {
      std::puts("UVCPP_ORM_TEST_PGSQL_URL is required but not set");
      return 1;
    }
    std::puts("SKIP: UVCPP_ORM_TEST_PGSQL_URL is not set");
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

  status = db.execute("DROP TABLE IF EXISTS orm_pgsql_accounts");
  check(uvcpp::uvcpp_db_ok(status), "drop table");
  status = db.execute(
      "CREATE TABLE orm_pgsql_accounts ("
      "id BIGSERIAL PRIMARY KEY,"
      "email VARCHAR(120) NOT NULL UNIQUE,"
      "age INT NULL,"
      "score DOUBLE PRECISION NOT NULL,"
      "active BOOLEAN NOT NULL)"
  );
  check(uvcpp::uvcpp_db_ok(status), "create table");
  if (!uvcpp::uvcpp_db_ok(status)) {
    std::printf("create failed: %s (%s)\n",
                uvcpp::uvcpp_db_status_name(status),
                db.last_error().c_str());
    return 1;
  }

  orm_pgsql_account alice;
  alice.email = "alice@example.com";
  alice.age = 30;
  alice.score = 1.5;
  alice.active = true;

  uvcpp::uvcpp_db_value alice_id;
  orm_status = uvcpp::insert(db, alice, &alice_id, &error, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "insert");
  check(!alice_id.is_null(), "insert returns generated id");

  orm_pgsql_account got;
  orm_status = uvcpp::find_by_pk(db, alice_id, &got, &error, &db_status);
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

  orm_pgsql_account after;
  orm_status = uvcpp::find_by_pk(db, alice_id, &after, &error, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "re-read after update");
  check(after.score == 9.25, "update persisted score");
  check(!after.age.has_value(), "update persisted NULL");

  std::vector<orm_pgsql_account> rows;
  int64_t total = 0;
  orm_status = uvcpp::find_all(
      db,
      uvcpp::select<orm_pgsql_account>(uvcpp::uvcpp_orm_dialect::postgresql)
          .where_eq(&orm_pgsql_account::email,
                    std::string("alice@example.com")),
      &rows, &error, &total, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "find_all");
  check(rows.size() == 1, "find_all row count");
  check(total == 1, "find_all total");

  int64_t counted = -1;
  orm_status = uvcpp::count<orm_pgsql_account>(
      db,
      uvcpp::select<orm_pgsql_account>(uvcpp::uvcpp_orm_dialect::postgresql),
      &counted, &error, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "count");
  check(counted == 1, "count value");

  // 唯一键冲突必须从数据库透传，而不是被映射成 0 行/成功。
  orm_pgsql_account duplicate;
  duplicate.email = "alice@example.com";
  duplicate.age = 1;
  duplicate.score = 2.0;
  duplicate.active = false;
  uvcpp::uvcpp_db_value duplicate_id;
  orm_status = uvcpp::insert(db, duplicate, &duplicate_id, &error, &db_status);
  check(orm_status != uvcpp::uvcpp_orm_status::OK, "duplicate rejected");
  check(!error.empty(), "duplicate has an error message");

  orm_status = uvcpp::remove<orm_pgsql_account>(
      db, alice_id, &affected, &error, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::OK, "remove");
  check(affected == 1, "remove affected one row");

  orm_status = uvcpp::find_by_pk(db, alice_id, &got, &error, &db_status);
  check(orm_status == uvcpp::uvcpp_orm_status::NOT_FOUND, "find after remove");

  db.execute("DROP TABLE IF EXISTS orm_pgsql_accounts");
  std::printf("[orm_pgsql] checks=%d failures=%d\n", checks, failures);
  if (failures != 0) return 1;
  std::puts("[PASS] orm_pgsql");
  return 0;
}
