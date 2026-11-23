/**
 * @file src/db/uvcpp_db_job_store.cpp
 * @brief 作业队列数据库适配器的实现（SQLite / MySQL / PostgreSQL）。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <db/uvcpp_db_job_store.h>

#if UVCPP_DB_ENABLE

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_params.h>
#include <db/uvcpp_db_status.h>
#include <db/uvcpp_db_table.h>
#include <db/uvcpp_db_value.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace uvcpp {

namespace {

bool valid_table_name(const std::string& t) {
  if (t.empty()) return false;
  const char c = t[0];
  if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) {
    return false;
  }
  for (std::size_t i = 1; i < t.size(); ++i) {
    const char x = t[i];
    const bool ok = (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') ||
                    (x >= '0' && x <= '9') || x == '_';
    if (!ok) return false;
  }
  return true;
}

const char* kColumns =
    "id, type, payload, concurrency_key, error, state, attempt, "
    "max_retry, timeout_ms, backoff_ms, not_before_ms";

const char* kUpdates =
    "type = excluded.type, payload = excluded.payload, "
    "concurrency_key = excluded.concurrency_key, error = excluded.error, "
    "state = excluded.state, attempt = excluded.attempt, "
    "max_retry = excluded.max_retry, timeout_ms = excluded.timeout_ms, "
    "backoff_ms = excluded.backoff_ms, not_before_ms = excluded.not_before_ms";

}  // namespace

struct uvcpp_db_job_store::impl {
  explicit impl(const std::string& url, const std::string& t)
      : db_url(url), table(t), ready(false) {}

  uvcpp_db_client      db;
  std::string          db_url;
  std::string          table;
  std::string          err;
  bool                 ready;
  mutable std::mutex   mu;

  bool postgres() const {
    const char* name = db.driver_name();
    return name != nullptr && std::string(name) == "postgres";
  }

  std::string placeholder(int i) const {
    if (!postgres()) return "?";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "$%d", i);
    return std::string(buf);
  }

  /// 11 个占位符，按 `kColumns` 的顺序。
  std::string values_clause() const {
    std::string out;
    for (int i = 1; i <= 11; ++i) {
      if (i > 1) out += ", ";
      out += placeholder(i);
    }
    return out;
  }

  std::string upsert_sql() const {
    const std::string base = "INSERT INTO " + table + " (" + kColumns +
                             ") VALUES (" + values_clause() + ")";
    const char* name = db.driver_name();
    const std::string driver = name != nullptr ? name : "";
    if (driver == "sqlite") {
      // SQLite 的 `INSERT OR REPLACE` 从 3.x 一直有；`ON CONFLICT` 要 3.24+。
      return "INSERT OR REPLACE INTO " + table + " (" + kColumns +
             ") VALUES (" + values_clause() + ")";
    }
    if (driver == "mysql") {
      std::string upd;
      const char* cols[] = {"type",          "payload",  "concurrency_key",
                            "error",         "state",    "attempt",
                            "max_retry",     "timeout_ms", "backoff_ms",
                            "not_before_ms"};
      for (std::size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); ++i) {
        if (i > 0) upd += ", ";
        upd += std::string(cols[i]) + " = VALUES(" + cols[i] + ")";
      }
      return base + " ON DUPLICATE KEY UPDATE " + upd;
    }
    // PostgreSQL（以及将来其它支持标准 upsert 的后端）。
    return base + " ON CONFLICT (id) DO UPDATE SET " + kUpdates;
  }
};

uvcpp_db_job_store::uvcpp_db_job_store(const std::string& db_url,
                                       const std::string& table)
    : impl_(new impl(db_url, table)) {}

uvcpp_db_job_store::~uvcpp_db_job_store() {}

bool uvcpp_db_job_store::is_open() const { return impl_->ready; }

const std::string& uvcpp_db_job_store::last_error() const { return impl_->err; }

bool uvcpp_db_job_store::open(std::string* err) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->ready = false;
  impl_->err.clear();

  if (!valid_table_name(impl_->table)) {
    impl_->err = "表名不合法（只允许 [A-Za-z_][A-Za-z0-9_]*）：" +
                 impl_->table;
    if (err != nullptr) *err = impl_->err;
    return false;
  }

  const uvcpp_db_status st = impl_->db.open(impl_->db_url);
  if (!uvcpp_db_ok(st)) {
    impl_->err = impl_->db.last_error();
    if (err != nullptr) *err = impl_->err;
    return false;
  }

  const std::string ddl =
      "CREATE TABLE IF NOT EXISTS " + impl_->table +
      " (id BIGINT PRIMARY KEY, type TEXT NOT NULL, payload TEXT, "
      "concurrency_key TEXT, error TEXT, state INTEGER NOT NULL, "
      "attempt INTEGER NOT NULL, max_retry INTEGER NOT NULL, "
      "timeout_ms BIGINT NOT NULL, backoff_ms BIGINT NOT NULL, "
      "not_before_ms BIGINT NOT NULL)";
  const uvcpp_db_status cs = impl_->db.execute(ddl);
  if (!uvcpp_db_ok(cs)) {
    impl_->err = impl_->db.last_error();
    if (err != nullptr) *err = impl_->err;
    return false;
  }
  impl_->ready = true;
  return true;
}

bool uvcpp_db_job_store::load_all(std::vector<uvcpp_job_record>& out) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  out.clear();
  if (!impl_->ready) {
    impl_->err = "store 还没 open()";
    return false;
  }
  const std::string sql =
      "SELECT " + std::string(kColumns) + " FROM " + impl_->table +
      " ORDER BY id";
  uvcpp_db_table t;
  const uvcpp_db_status st = impl_->db.query(sql, &t);
  if (!uvcpp_db_ok(st)) {
    impl_->err = impl_->db.last_error();
    return false;
  }
  out.reserve(t.row_count());
  for (std::size_t i = 0; i < t.row_count(); ++i) {
    const uvcpp_db_table& tab = t;
    const uvcpp_db_value& id = tab.at(i, "id");
    uvcpp_job_record rec;
    rec.id              = static_cast<uint64_t>(id.to_int64());
    rec.type            = tab.at(i, "type").to_text();
    rec.payload         = tab.at(i, "payload").to_text();
    rec.concurrency_key = tab.at(i, "concurrency_key").to_text();
    rec.error           = tab.at(i, "error").to_text();
    rec.state           = static_cast<int>(tab.at(i, "state").to_int64());
    rec.attempt         = static_cast<int>(tab.at(i, "attempt").to_int64());
    rec.max_retry       = static_cast<int>(tab.at(i, "max_retry").to_int64());
    rec.timeout_ms  = static_cast<uint64_t>(tab.at(i, "timeout_ms").to_int64());
    rec.backoff_ms  = static_cast<uint64_t>(tab.at(i, "backoff_ms").to_int64());
    rec.not_before_ms = tab.at(i, "not_before_ms").to_int64();
    out.push_back(rec);
  }
  return true;
}

bool uvcpp_db_job_store::save(const uvcpp_job_record& rec) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (!impl_->ready) {
    impl_->err = "store 还没 open()";
    return false;
  }
  uvcpp_db_params p;
  p.add(static_cast<int64_t>(rec.id));
  p.add(rec.type);
  p.add(rec.payload);
  p.add(rec.concurrency_key);
  p.add(rec.error);
  p.add(static_cast<int64_t>(rec.state));
  p.add(static_cast<int64_t>(rec.attempt));
  p.add(static_cast<int64_t>(rec.max_retry));
  p.add(static_cast<int64_t>(rec.timeout_ms));
  p.add(static_cast<int64_t>(rec.backoff_ms));
  p.add(static_cast<int64_t>(rec.not_before_ms));

  const uvcpp_db_status st = impl_->db.execute(impl_->upsert_sql(), p);
  if (!uvcpp_db_ok(st)) {
    impl_->err = impl_->db.last_error();
    return false;
  }
  return true;
}

bool uvcpp_db_job_store::erase(uint64_t id) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (!impl_->ready) {
    impl_->err = "store 还没 open()";
    return false;
  }
  uvcpp_db_params p;
  p.add(static_cast<int64_t>(id));
  const std::string sql = "DELETE FROM " + impl_->table + " WHERE id = " +
                          impl_->placeholder(1);
  const uvcpp_db_status st = impl_->db.execute(sql, p);
  if (!uvcpp_db_ok(st)) {
    impl_->err = impl_->db.last_error();
    return false;
  }
  return true;
}

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
