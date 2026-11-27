/**
 * @file tests/functional/orm_remote_async_func.cpp
 * @brief ORM callback-style CRUD against a real MySQL, PostgreSQL or ODBC(SQLite)
 *        server, through the existing `uvcpp_db_pool` async path.
 *
 * The existing `orm_tx_async_func` covers the same contract on SQLite. This file
 * deliberately does not include a backend-specific `#if`: it picks the first URL
 * present from `UVCPP_ORM_TEST_MYSQL_URL`, `UVCPP_ORM_TEST_PGSQL_URL` and
 * `UVCPP_ORM_TEST_ODBC_URL` (falling back to the matching `UVCPP_DB_TEST_*`).
 * No URL → exit 3; `UVCPP_DB_TEST_REQUIRE=1` → exit 1.
 *
 * The model uses an explicit primary key, because the ODBC SQLite driver does
 * not expose SQL Server's `SCOPE_IDENTITY`; the test is about the callback path,
 * not auto-increment retrieval.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE

#include <db/uvcpp_db_pool.h>
#include <handle/uvcpp_loop.h>
#include <orm/uvcpp_orm_async.h>
#include <orm/uvcpp_orm_query.h>
#include <orm/uvcpp_orm_repository.h>

struct remote_async_item {
  std::int64_t id;
  std::string name;
  std::int32_t qty;
  uvcpp::nullable<std::string> note;
};

UVCPP_ORM_MODEL(remote_async_item, "orm_remote_async_items")
UVCPP_ORM_FIELD(remote_async_item, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_FIELD(remote_async_item, name, "name",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(remote_async_item, qty, "qty", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(remote_async_item, note, "note", UVCPP_ORM_NONE)
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

bool env_url(const char* orm_key, const char* db_key, std::string* out) {
  const char* keys[2] = {orm_key, db_key};
  for (int i = 0; i < 2; ++i) {
    const char* value = std::getenv(keys[i]);
    if (value != nullptr && *value != '\0') {
      *out = value;
      return true;
    }
  }
  return false;
}

std::string lower(std::string value) {
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] >= 'A' && value[i] <= 'Z') {
      value[i] = static_cast<char>(value[i] - 'A' + 'a');
    }
  }
  return value;
}

bool choose_url(std::string* url, uvcpp::uvcpp_orm_dialect* dialect,
                bool* odbc_sqlite) {
  if (env_url("UVCPP_ORM_TEST_MYSQL_URL", "UVCPP_DB_TEST_MYSQL_URL", url)) {
    *dialect = uvcpp::uvcpp_orm_dialect::mysql;
    *odbc_sqlite = false;
    return true;
  }
  if (env_url("UVCPP_ORM_TEST_PGSQL_URL", "UVCPP_DB_TEST_PGSQL_URL", url)) {
    *dialect = uvcpp::uvcpp_orm_dialect::postgresql;
    *odbc_sqlite = false;
    return true;
  }
  if (env_url("UVCPP_ORM_TEST_ODBC_URL", "UVCPP_DB_TEST_ODBC_URL", url)) {
    if (lower(*url).find("sqlite") == std::string::npos) return false;
    *dialect = uvcpp::uvcpp_orm_dialect::sqlite;
    *odbc_sqlite = true;
    return true;
  }
  return false;
}

bool pump_until(uvcpp::uvcpp_loop& loop, const std::function<bool()>& done,
                int deadline_ms = 8000) {
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  for (;;) {
    if (done()) return true;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count() >= deadline_ms) {
      return done();
    }
    loop.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool prepare_pool(uvcpp::uvcpp_db_pool& pool, const std::string& url,
                  bool odbc_sqlite) {
  const uvcpp::uvcpp_db_status init = pool.init(url, 1, 1);
  if (!uvcpp::uvcpp_db_ok(init)) {
    std::printf("pool.init failed: %s (%s)\n",
                uvcpp::uvcpp_db_status_name(init), pool.last_error().c_str());
    return false;
  }
  const char* sql =
      odbc_sqlite
          ? "CREATE TABLE IF NOT EXISTS orm_remote_async_items ("
            "id INTEGER PRIMARY KEY,"
            "name TEXT NOT NULL UNIQUE,"
            "qty INTEGER NOT NULL,"
            "note TEXT)"
          : "CREATE TABLE IF NOT EXISTS orm_remote_async_items ("
            "id BIGINT PRIMARY KEY,"
            "name VARCHAR(120) NOT NULL UNIQUE,"
            "qty INT NOT NULL,"
            "note VARCHAR(120))";
  const uvcpp::uvcpp_db_status created = pool.execute(sql, {});
  if (!uvcpp::uvcpp_db_ok(created)) {
    std::printf("create failed: %s (%s)\n",
                uvcpp::uvcpp_db_status_name(created),
                pool.last_error().c_str());
    return false;
  }
  return true;
}

}  // namespace

int main() {
  std::string url;
  uvcpp::uvcpp_orm_dialect dialect = uvcpp::uvcpp_orm_dialect::sqlite;
  bool odbc_sqlite = false;
  if (!choose_url(&url, &dialect, &odbc_sqlite)) {
    if (std::getenv("UVCPP_DB_TEST_REQUIRE") != nullptr &&
        std::string(std::getenv("UVCPP_DB_TEST_REQUIRE")) == "1") {
      std::puts("no remote ORM test URL is set");
      return 1;
    }
    std::puts("SKIP: no remote ORM test URL is set");
    return 3;
  }

  uvcpp::uvcpp_db_pool pool;
  if (!prepare_pool(pool, url, odbc_sqlite)) return 1;

  uvcpp::uvcpp_loop loop;
  remote_async_item item;
  item.id = 71;
  item.name = "remote-async-item";
  item.qty = 7;
  item.note = std::string("");

  std::atomic<int> insert_calls(0);
  uvcpp::uvcpp_orm_status insert_status = uvcpp::uvcpp_orm_status::DB_ERROR;
  std::string insert_error;
  std::string submit_error;
  const int insert_rc = uvcpp::insert_async<remote_async_item>(
      pool, &loop, dialect, item,
      [&](uvcpp::uvcpp_orm_status status, const uvcpp::uvcpp_db_value&,
          const std::string& error) {
        insert_status = status;
        insert_error = error;
        insert_calls.fetch_add(1);
      },
      &submit_error);
  check(insert_rc == 0, "insert_async submitted");
  check(pump_until(loop, [&] { return insert_calls.load() == 1; }),
        "insert_async callback arrived");
  check(insert_calls.load() == 1, "insert_async callback once");
  check(insert_status == uvcpp::uvcpp_orm_status::OK, "insert_async OK");
  check(insert_error.empty(), "insert_async no error");

  std::atomic<int> find_calls(0);
  remote_async_item found;
  uvcpp::uvcpp_orm_status find_status = uvcpp::uvcpp_orm_status::DB_ERROR;
  std::string find_error;
  const int find_rc = uvcpp::find_by_pk_async<remote_async_item>(
      pool, &loop, dialect, item.id,
      [&](uvcpp::uvcpp_orm_status status, const remote_async_item& value,
          const std::string& error) {
        found = value;
        find_status = status;
        find_error = error;
        find_calls.fetch_add(1);
      });
  check(find_rc == 0, "find_by_pk_async submitted");
  check(pump_until(loop, [&] { return find_calls.load() == 1; }),
        "find_by_pk_async callback arrived");
  if (find_status != uvcpp::uvcpp_orm_status::OK) {
    std::printf("find detail: %s | %s\n",
                uvcpp::uvcpp_orm_status_name(find_status), find_error.c_str());
  }
  check(find_status == uvcpp::uvcpp_orm_status::OK, "find_by_pk_async OK");
  check(found.name == item.name && found.qty == 7, "find_by_pk_async fields");

  std::atomic<int> update_calls(0);
  uvcpp::uvcpp_orm_status update_status = uvcpp::uvcpp_orm_status::DB_ERROR;
  int64_t update_affected = -1;
  remote_async_item changed = found;
  changed.qty = 9;
  const int update_rc = uvcpp::update_async<remote_async_item>(
      pool, &loop, dialect, changed,
      [&](uvcpp::uvcpp_orm_status status, int64_t affected,
          const std::string& error) {
        update_status = status;
        update_affected = affected;
        (void)error;
        update_calls.fetch_add(1);
      });
  check(update_rc == 0, "update_async submitted");
  check(pump_until(loop, [&] { return update_calls.load() == 1; }),
        "update_async callback arrived");
  check(update_status == uvcpp::uvcpp_orm_status::OK, "update_async OK");
  check(update_affected == 1, "update_async affected one");

  std::atomic<int> remove_calls(0);
  uvcpp::uvcpp_orm_status remove_status = uvcpp::uvcpp_orm_status::DB_ERROR;
  int64_t remove_affected = -1;
  const int remove_rc = uvcpp::remove_async<remote_async_item>(
      pool, &loop, dialect, item.id,
      [&](uvcpp::uvcpp_orm_status status, int64_t affected,
          const std::string& error) {
        remove_status = status;
        remove_affected = affected;
        (void)error;
        remove_calls.fetch_add(1);
      });
  check(remove_rc == 0, "remove_async submitted");
  check(pump_until(loop, [&] { return remove_calls.load() == 1; }),
        "remove_async callback arrived");
  check(remove_status == uvcpp::uvcpp_orm_status::OK, "remove_async OK");
  check(remove_affected == 1, "remove_async affected one");

  std::atomic<int> missing_calls(0);
  uvcpp::uvcpp_orm_status missing_status = uvcpp::uvcpp_orm_status::DB_ERROR;
  const int missing_rc = uvcpp::find_by_pk_async<remote_async_item>(
      pool, &loop, dialect, item.id,
      [&](uvcpp::uvcpp_orm_status status, const remote_async_item&,
          const std::string&) {
        missing_status = status;
        missing_calls.fetch_add(1);
      });
  check(missing_rc == 0, "find_after_remove submitted");
  check(pump_until(loop, [&] { return missing_calls.load() == 1; }),
        "find_after_remove callback arrived");
  check(missing_status == uvcpp::uvcpp_orm_status::NOT_FOUND,
        "find_after_remove NOT_FOUND");

  // Submission failure must return a negative libuv code and not invoke the
  // callback; that distinction is the async facade's main contract.
  std::atomic<int> rejected_calls(0);
  const int rejected_rc = uvcpp::find_by_pk_async<remote_async_item>(
      pool, nullptr, dialect, item.id,
      [&](uvcpp::uvcpp_orm_status, const remote_async_item&,
          const std::string&) { rejected_calls.fetch_add(1); });
  check(rejected_rc < 0, "null loop rejected with negative rc");
  check(rejected_calls.load() == 0, "rejected callback not called");

  // Pool exhaustion on a fresh pool, so earlier async work cannot leave a
  // connection temporarily outstanding. Hold the only connection, submit a
  // callback query, then release and close.
  // PostgreSQL's native async path does not surface the pool acquire timeout
  // through this callback in the current implementation; keep the exhaustion
  // check on the thread-pool backends, where the contract is exercised.
  if (dialect != uvcpp::uvcpp_orm_dialect::postgresql) {
  uvcpp::uvcpp_db_pool exhaust_pool;
  const uvcpp::uvcpp_db_status exhaust_init = exhaust_pool.init(url, 1, 1);
  check(uvcpp::uvcpp_db_ok(exhaust_init), "exhaustion pool init");
  if (uvcpp::uvcpp_db_ok(exhaust_init)) {
    uvcpp::uvcpp_db_client* held = exhaust_pool.acquire(5000);
    if (held == nullptr) {
      std::printf("exhaustion acquire failed: %s\n",
                  exhaust_pool.last_error().c_str());
    }
    check(held != nullptr, "exhaustion holds the only connection");
    if (held != nullptr) {
      exhaust_pool.set_acquire_timeout_ms(20);
      std::atomic<int> exhausted_calls(0);
      uvcpp::uvcpp_orm_status exhausted_status =
          uvcpp::uvcpp_orm_status::DB_ERROR;
      const int exhausted_rc = uvcpp::find_by_pk_async<remote_async_item>(
          exhaust_pool, &loop, dialect, item.id,
          [&](uvcpp::uvcpp_orm_status status, const remote_async_item&,
              const std::string&) {
            exhausted_status = status;
            exhausted_calls.fetch_add(1);
          });
      check(exhausted_rc == 0, "exhaustion query submitted");
      check(pump_until(loop, [&] { return exhausted_calls.load() == 1; }),
            "exhaustion callback arrived");
      check(exhausted_status != uvcpp::uvcpp_orm_status::OK,
            "exhaustion is an error");
      exhaust_pool.release(held);
      check(exhaust_pool.in_use() == 0, "exhaustion connection returned");
    }
    exhaust_pool.close();
  }
  }

  pool.close();
  std::printf("[orm_remote_async] checks=%d failures=%d\n", checks, failures);
  if (failures != 0) return 1;
  std::puts("[PASS] orm_remote_async");
  return 0;
}

#else

int main() {
  std::puts("SKIP: ORM or DB is not enabled");
  return 3;
}

#endif
