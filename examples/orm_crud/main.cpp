/**
 * @file examples/orm_crud/main.cpp
 * @brief 单表 ORM 的可运行示例：SQLite 上真跑一遍 CRUD。
 *
 * 用法（在仓库根）：
 *
 *   cmake -S . -B build-orm-example \
 *     -DUVCPP_BUILD_EXAMPLES=ON -DUVCPP_ENABLE_DB=ON \
 *     -DUVCPP_ENABLE_DB_SQLITE=ON -DUVCPP_BUILD_ORM=ON
 *   cmake --build build-orm-example --target orm_crud
 *   ./build-orm-example/examples/orm_crud/orm_crud
 *
 * 每一步都检查返回码；任何一步失败都会打印 `uvcpp_orm_status`、人读原因与
 * `uvcpp_db_status`，并以非 0 退出。示例只用公开头 `<db/...>` 与 `<orm/...>`。
 */

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <db/uvcpp_db.h>
#include <orm/uvcpp_orm_model.h>
#include <orm/uvcpp_orm_query.h>
#include <orm/uvcpp_orm_repository.h>

struct account {
  std::int64_t id;
  std::string email;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(account, "example_accounts")
UVCPP_ORM_FIELD(account, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(account, email, "email",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(account, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, score, "score", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(account, active, "active", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

namespace {

using uvcpp::uvcpp_db_client;
using uvcpp::uvcpp_db_status;
using uvcpp::uvcpp_db_value;
using uvcpp::uvcpp_orm_dialect;
using uvcpp::uvcpp_orm_order;
using uvcpp::uvcpp_orm_status;

int g_failures = 0;

void fail(const std::string& what, uvcpp_orm_status status,
          const std::string& error, uvcpp_db_status db_status) {
  ++g_failures;
  std::cerr << "[FAIL] " << what << ": "
            << uvcpp::uvcpp_orm_status_name(status) << " [" << error << "]"
            << " db_status=" << uvcpp::uvcpp_db_status_name(db_status)
            << std::endl;
}

bool orm_ok(const std::string& what, uvcpp_orm_status status,
            const std::string& error, uvcpp_db_status db_status) {
  if (status == uvcpp_orm_status::OK) return true;
  fail(what, status, error, db_status);
  return false;
}

bool db_ok(const std::string& what, uvcpp_db_status status,
           const std::string& last_error) {
  if (uvcpp::uvcpp_db_ok(status)) return true;
  ++g_failures;
  std::cerr << "[FAIL] " << what << ": "
            << uvcpp::uvcpp_db_status_name(status) << " [" << last_error << "]"
            << std::endl;
  return false;
}

account make_account(const std::string& email, bool with_age,
                     std::int32_t age, double score) {
  account row;
  row.id = 0;
  row.email = email;
  if (with_age) {
    row.age = age;
  } else {
    row.age.reset();
  }
  row.score = score;
  row.active = true;
  return row;
}

bool insert_row(uvcpp_db_client& db, const account& row,
                const std::string& label, uvcpp_db_value* new_id) {
  std::string error;
  uvcpp_db_status db_status = uvcpp_db_status::OK;
  const uvcpp_orm_status status =
      uvcpp::insert(db, row, new_id, &error, &db_status);
  if (!orm_ok("insert " + label, status, error, db_status)) return false;
  if (new_id->is_null()) {
    std::cerr << "[FAIL] insert " << label
              << ": 后端没有回填自增主键" << std::endl;
    ++g_failures;
    return false;
  }
  std::cout << "insert " << label << " -> id=" << new_id->to_int64()
            << std::endl;
  return true;
}

bool run() {
  const std::string path = "orm_crud_example.db";
  std::remove(path.c_str());
  std::remove((path + "-journal").c_str());

  uvcpp_db_client db;
  const uvcpp_db_status opened = db.open("sqlite://" + path);
  if (!db_ok("open " + path, opened, db.last_error())) return false;

  if (!db_ok("create table", db.execute(
                 "CREATE TABLE example_accounts ("
                 " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 " email TEXT NOT NULL UNIQUE,"
                 " age INTEGER,"
                 " score REAL NOT NULL,"
                 " active INTEGER NOT NULL)",
                 nullptr),
             db.last_error())) {
    return false;
  }

  // ---- CREATE：三行，其中一行 age 是 NULL ----
  uvcpp_db_value alice_id;
  uvcpp_db_value bob_id;
  uvcpp_db_value carol_id;
  if (!insert_row(db, make_account("alice@example.com", true, 30, 1.5),
                  "alice", &alice_id) ||
      !insert_row(db, make_account("bob@example.com", true, 17, 2.0),
                  "bob", &bob_id) ||
      !insert_row(db, make_account("carol@example.com", false, 0, 3.0),
                  "carol", &carol_id)) {
    return false;
  }

  // ---- READ：按主键 ----
  {
    account loaded;
    std::string error;
    uvcpp_db_status db_status = uvcpp_db_status::OK;
    const uvcpp_orm_status status =
        uvcpp::find_by_pk(db, alice_id, &loaded, &error, &db_status);
    if (!orm_ok("find_by_pk alice", status, error, db_status)) return false;
    if (loaded.email != "alice@example.com" || !loaded.age.has_value() ||
        loaded.age.value() != 30 || loaded.score != 1.5 || !loaded.active) {
      std::cerr << "[FAIL] find_by_pk alice: 字段与写入不一致" << std::endl;
      ++g_failures;
      return false;
    }
    std::cout << "find_by_pk alice -> " << loaded.email << ", age="
              << loaded.age.value() << ", score=" << loaded.score << std::endl;
  }

  // ---- READ：条件查 + 总数 ----
  {
    std::vector<account> page;
    int64_t total = 0;
    std::string error;
    uvcpp_db_status db_status = uvcpp_db_status::OK;
    const uvcpp_orm_status status = uvcpp::find_all(
        db,
        uvcpp::select<account>(uvcpp_orm_dialect::sqlite)
            .where_ge(&account::age, std::int32_t(18))
            .order_by(&account::id, uvcpp_orm_order::asc)
            .limit(20),
        &page, &error, &total, &db_status);
    if (!orm_ok("find_all age >= 18", status, error, db_status)) return false;
    if (page.size() != 1u || total != 1 || page[0].email != "alice@example.com") {
      std::cerr << "[FAIL] find_all age >= 18: rows=" << page.size()
                << " total=" << total << std::endl;
      ++g_failures;
      return false;
    }
    std::cout << "find_all age >= 18 -> " << total << " row(s), first="
              << page[0].email << std::endl;
  }

  // ---- READ：显式 COUNT ----
  {
    int64_t total = 0;
    std::string error;
    uvcpp_db_status db_status = uvcpp_db_status::OK;
    const uvcpp_orm_status status = uvcpp::count(
        db, uvcpp::select<account>(uvcpp_orm_dialect::sqlite),
        &total, &error, &db_status);
    if (!orm_ok("count all", status, error, db_status)) return false;
    if (total != 3) {
      std::cerr << "[FAIL] count all: 期望 3，实得 " << total << std::endl;
      ++g_failures;
      return false;
    }
    std::cout << "count all -> " << total << std::endl;
  }

  // ---- UPDATE：按主键 ----
  {
    account loaded;
    std::string error;
    uvcpp_db_status db_status = uvcpp_db_status::OK;
    uvcpp_orm_status status =
        uvcpp::find_by_pk(db, alice_id, &loaded, &error, &db_status);
    if (!orm_ok("find_by_pk before update", status, error, db_status)) {
      return false;
    }
    loaded.score = 2.5;
    loaded.age = 31;

    int64_t affected = -1;
    status = uvcpp::update(db, loaded, &affected, &error, &db_status);
    if (!orm_ok("update alice", status, error, db_status)) return false;
    if (affected != 1) {
      std::cerr << "[FAIL] update alice: affected=" << affected << std::endl;
      ++g_failures;
      return false;
    }

    account reread;
    status = uvcpp::find_by_pk(db, alice_id, &reread, &error, &db_status);
    if (!orm_ok("find_by_pk after update", status, error, db_status)) {
      return false;
    }
    if (reread.score != 2.5 || !reread.age.has_value() ||
        reread.age.value() != 31) {
      std::cerr << "[FAIL] update alice: 回读不一致" << std::endl;
      ++g_failures;
      return false;
    }
    std::cout << "update alice -> affected=" << affected
              << ", score=" << reread.score << std::endl;
  }

  // ---- DELETE：按主键，然后确认 NOT_FOUND ----
  {
    int64_t affected = -1;
    std::string error;
    uvcpp_db_status db_status = uvcpp_db_status::OK;
    uvcpp_orm_status status =
        uvcpp::remove<account>(db, bob_id, &affected, &error, &db_status);
    if (!orm_ok("remove bob", status, error, db_status)) return false;
    if (affected != 1) {
      std::cerr << "[FAIL] remove bob: affected=" << affected << std::endl;
      ++g_failures;
      return false;
    }

    account gone;
    status = uvcpp::find_by_pk(db, bob_id, &gone, &error, &db_status);
    if (status != uvcpp_orm_status::NOT_FOUND) {
      fail("find_by_pk after remove (期望 NOT_FOUND)", status, error,
           db_status);
      return false;
    }
    std::cout << "remove bob -> affected=" << affected << ", then NOT_FOUND"
              << std::endl;
  }

  // ---- READ：空态与 NULL 的区分 ----
  {
    account carol;
    std::string error;
    uvcpp_db_status db_status = uvcpp_db_status::OK;
    const uvcpp_orm_status status =
        uvcpp::find_by_pk(db, carol_id, &carol, &error, &db_status);
    if (!orm_ok("find_by_pk carol", status, error, db_status)) return false;
    if (carol.age.has_value()) {
      std::cerr << "[FAIL] carol.age: SQL NULL 读成了有值" << std::endl;
      ++g_failures;
      return false;
    }
    std::cout << "find_by_pk carol -> age is NULL" << std::endl;
  }

  db.close();
  std::remove(path.c_str());
  std::remove((path + "-journal").c_str());
  return true;
}

}  // namespace

int main() {
  if (!run()) {
    std::cerr << "orm_crud: 有检查失败" << std::endl;
    return 1;
  }
  std::cout << "orm_crud: all checks passed" << std::endl;
  return 0;
}
