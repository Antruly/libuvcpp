/**
 * @file src/db/uvcpp_db.cpp
 * @brief `uvcpp_db.h` 的实现：一个连接 = 一个 `uvcpp_db_client`。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "db/uvcpp_db.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE

#include <chrono>
#include <cstdio>
#include <mutex>

#include "db/uvcpp_db_driver.h"

namespace uvcpp {

namespace {

/// SQL 打进日志前压成一行、截断。日志里保留完整多行 SQL 会把一行日志撑成
/// 几十行，而排障时需要的往往是「这条语句 + 耗时」，不是它的缩进。
std::string brief_sql(const std::string& sql) {
  std::string out;
  out.reserve(sql.size());
  bool last_space = false;
  for (size_t i = 0; i < sql.size(); ++i) {
    const char c = sql[i];
    const bool space = (c == '\n' || c == '\r' || c == '\t' || c == ' ');
    if (space) {
      if (!last_space) out.push_back(' ');
      last_space = true;
    } else {
      out.push_back(c);
      last_space = false;
    }
  }
  const size_t kMax = 200;
  if (out.size() > kMax) {
    out.resize(kMax);
    out.append("…");
  }
  return out;
}

}  // namespace

struct uvcpp_db_client::impl {
  /// 递归锁：`insert()` 要调 `execute()` 再调 `last_insert_id()`，都是公开
  /// 方法，非递归锁会在这里自己把自己锁死。
  std::recursive_mutex mutex;
  std::unique_ptr<db_detail::uvcpp_db_driver> driver;
  std::string url;
  std::string last_error;
  int timeout_ms = 5000;
  log_callback log;

  void log_line(const std::string& line) {
    if (log) {
      // 回调里再进库（比如把日志写进库）会撞上这把锁；那是调用方的事，
      // 这里不吞异常、也不加锁 —— 锁已经在手上（递归锁允许）。
      log(line);
    }
  }
};

uvcpp_db_client::uvcpp_db_client() : impl_(new impl()) {}

uvcpp_db_client::~uvcpp_db_client() {
  // `impl_` 是 unique_ptr，析构自己会删；这里只负责在删之前把连接关掉。
  close();
}

uvcpp_db_status uvcpp_db_client::open(const std::string& url) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);

  impl_->driver.reset();
  impl_->url.clear();

  db_detail::uvcpp_db_url parsed;
  std::string err;
  uvcpp_db_status st = db_detail::uvcpp_db_url::parse(url, &parsed, &err);
  if (!uvcpp_db_ok(st)) {
    impl_->last_error = err;
    impl_->log_line(std::string("open failed | ") +
                    uvcpp_db_status_name(st) + " | " + err);
    return st;
  }

  impl_->driver = db_detail::uvcpp_db_make_driver(parsed.scheme);
  if (!impl_->driver) {
    // 「这个后端存在、但这份包没编进来」与「这个 scheme 根本不存在」是两件事：
    // 前者换份包就好，后者是连接串写错了。判的是 driver_known 而不是
    // driver_available —— available 为假时两种情况都是假，分不开。
    st = db_detail::uvcpp_db_driver_known(parsed.scheme)
             ? uvcpp_db_status::NO_DRIVER
             : uvcpp_db_status::BAD_URL;
    impl_->last_error = "没有可用的 " + parsed.scheme +
                        " 驱动（这个库编进来的后端：" +
                        db_detail::uvcpp_db_available_drivers() + "）";
    impl_->driver.reset();
    impl_->log_line(std::string("open failed | ") +
                    uvcpp_db_status_name(st) + " | " + impl_->last_error);
    return st;
  }

  impl_->driver->set_timeout_ms(impl_->timeout_ms);
  err.clear();
  st = impl_->driver->open(parsed, &err);
  if (!uvcpp_db_ok(st)) {
    impl_->last_error = err;
    impl_->driver.reset();
    impl_->log_line(std::string("open failed | ") +
                    uvcpp_db_status_name(st) + " | " + err);
    return st;
  }

  impl_->url = url;
  impl_->last_error.clear();
  impl_->log_line("open ok | " + brief_sql(url) + " | " +
                  std::to_string(impl_->timeout_ms) + " ms 超时");
  return uvcpp_db_status::OK;
}

void uvcpp_db_client::close() {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (impl_->driver) impl_->driver->close();
  impl_->driver.reset();
}

bool uvcpp_db_client::is_open() const {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  return impl_->driver && impl_->driver->is_open();
}

uvcpp_db_status uvcpp_db_client::reconnect() {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (impl_->url.empty()) {
    impl_->last_error = "还没 open() 过，没有可以重连的连接串";
    return uvcpp_db_status::MISUSE;
  }
  const std::string url = impl_->url;
  return open(url);
}

uvcpp_db_status uvcpp_db_client::ping() {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!impl_->driver || !impl_->driver->is_open()) {
    impl_->last_error = "连接没打开";
    return uvcpp_db_status::NOT_CONNECTED;
  }
  std::string err;
  const uvcpp_db_status st = impl_->driver->ping(&err);
  if (!uvcpp_db_ok(st)) impl_->last_error = err;
  return st;
}

const std::string& uvcpp_db_client::url() const { return impl_->url; }

const char* uvcpp_db_client::driver_name() const {
  return impl_->driver ? impl_->driver->name() : "";
}

const std::string& uvcpp_db_client::last_error() const {
  return impl_->last_error;
}

uvcpp_db_status uvcpp_db_client::query(const std::string& sql,
                                       uvcpp_db_table* out) {
  return query(sql, uvcpp_db_params(), out);
}

uvcpp_db_status uvcpp_db_client::query(const std::string& sql,
                                       const uvcpp_db_params& params,
                                       uvcpp_db_table* out) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!out) {
    impl_->last_error = "query() 的 out 是空的";
    return uvcpp_db_status::MISUSE;
  }
  out->clear();
  if (sql.empty()) {
    impl_->last_error = "SQL 是空的";
    return uvcpp_db_status::MISUSE;
  }
  if (!impl_->driver || !impl_->driver->is_open()) {
    impl_->last_error = "连接没打开";
    return uvcpp_db_status::NOT_CONNECTED;
  }

  const auto t0 = std::chrono::steady_clock::now();
  std::string err;
  const uvcpp_db_status st =
      impl_->driver->query(sql, params.values(), out, &err);
  const long ms = static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count());

  if (!uvcpp_db_ok(st)) {
    impl_->last_error = err;
    impl_->log_line(std::string("query failed | ") + uvcpp_db_status_name(st) +
                    " | " + std::to_string(ms) + " ms | " + brief_sql(sql) +
                    " | " + err);
    return st;
  }
  impl_->log_line("query ok | " + std::to_string(out->row_count()) + " 行 | " +
                  std::to_string(ms) + " ms | " + brief_sql(sql) +
                  (params.empty() ? "" : " | 参数 " +
                                             std::to_string(params.size())));
  return uvcpp_db_status::OK;
}

uvcpp_db_status uvcpp_db_client::execute(const std::string& sql,
                                         int64_t* affected) {
  return execute(sql, uvcpp_db_params(), affected);
}

uvcpp_db_status uvcpp_db_client::execute(const std::string& sql,
                                         const uvcpp_db_params& params,
                                         int64_t* affected) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (affected) *affected = -1;
  if (sql.empty()) {
    impl_->last_error = "SQL 是空的";
    return uvcpp_db_status::MISUSE;
  }
  if (!impl_->driver || !impl_->driver->is_open()) {
    impl_->last_error = "连接没打开";
    return uvcpp_db_status::NOT_CONNECTED;
  }

  const auto t0 = std::chrono::steady_clock::now();
  std::string err;
  const uvcpp_db_status st =
      impl_->driver->execute(sql, params.values(), affected, &err);
  const long ms = static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count());

  if (!uvcpp_db_ok(st)) {
    impl_->last_error = err;
    impl_->log_line(std::string("execute failed | ") +
                    uvcpp_db_status_name(st) + " | " + std::to_string(ms) +
                    " ms | " + brief_sql(sql) + " | " + err);
    return st;
  }
  impl_->log_line("execute ok | 影响 " +
                  std::to_string(affected ? *affected : -1) + " 行 | " +
                  std::to_string(ms) + " ms | " + brief_sql(sql) +
                  (params.empty() ? "" : " | 参数 " +
                                             std::to_string(params.size())));
  return uvcpp_db_status::OK;
}

uvcpp_db_status uvcpp_db_client::insert(const std::string& sql,
                                        const uvcpp_db_params& params,
                                        uvcpp_db_value* new_id) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  const uvcpp_db_status st = execute(sql, params, nullptr);
  if (!uvcpp_db_ok(st)) return st;
  uvcpp_db_value id;
  last_insert_id(&id);  // 拿不到不算失败：见头文件里那段
  if (new_id) *new_id = std::move(id);
  return uvcpp_db_status::OK;
}

uvcpp_db_value uvcpp_db_client::last_insert_id() {
  uvcpp_db_value out;
  last_insert_id(&out);
  return out;
}

uvcpp_db_status uvcpp_db_client::last_insert_id(uvcpp_db_value* out) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (out) out->set_null();
  if (!out) {
    impl_->last_error = "last_insert_id() 的 out 是空的";
    return uvcpp_db_status::MISUSE;
  }
  if (!impl_->driver || !impl_->driver->is_open()) {
    impl_->last_error = "连接没打开";
    return uvcpp_db_status::NOT_CONNECTED;
  }
  std::string err;
  const uvcpp_db_status st = impl_->driver->last_insert_id(out, &err);
  if (!uvcpp_db_ok(st)) impl_->last_error = err;
  return st;
}

uvcpp_db_status uvcpp_db_client::begin() {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!impl_->driver || !impl_->driver->is_open()) {
    impl_->last_error = "连接没打开";
    return uvcpp_db_status::NOT_CONNECTED;
  }
  std::string err;
  const uvcpp_db_status st = impl_->driver->begin(&err);
  if (!uvcpp_db_ok(st)) {
    impl_->last_error = err;
    impl_->log_line("begin failed | " + err);
  } else {
    impl_->log_line("begin ok");
  }
  return st;
}

uvcpp_db_status uvcpp_db_client::commit() {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!impl_->driver || !impl_->driver->is_open()) {
    impl_->last_error = "连接没打开";
    return uvcpp_db_status::NOT_CONNECTED;
  }
  std::string err;
  const uvcpp_db_status st = impl_->driver->commit(&err);
  if (!uvcpp_db_ok(st)) {
    impl_->last_error = err;
    impl_->log_line("commit failed | " + err);
  } else {
    impl_->log_line("commit ok");
  }
  return st;
}

uvcpp_db_status uvcpp_db_client::rollback() {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!impl_->driver || !impl_->driver->is_open()) {
    impl_->last_error = "连接没打开";
    return uvcpp_db_status::NOT_CONNECTED;
  }
  std::string err;
  const uvcpp_db_status st = impl_->driver->rollback(&err);
  if (!uvcpp_db_ok(st)) {
    impl_->last_error = err;
    impl_->log_line("rollback failed | " + err);
  } else {
    impl_->log_line("rollback ok");
  }
  return st;
}

uvcpp_db_status uvcpp_db_client::table_names(std::vector<std::string>* out) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!out) {
    impl_->last_error = "table_names() 的 out 是空的";
    return uvcpp_db_status::MISUSE;
  }
  out->clear();
  if (!impl_->driver || !impl_->driver->is_open()) {
    impl_->last_error = "连接没打开";
    return uvcpp_db_status::NOT_CONNECTED;
  }
  std::string err;
  const uvcpp_db_status st = impl_->driver->table_names(out, &err);
  if (!uvcpp_db_ok(st)) impl_->last_error = err;
  return st;
}

uvcpp_db_status uvcpp_db_client::table_schema(const std::string& table,
                                              uvcpp_db_table* out) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!out) {
    impl_->last_error = "table_schema() 的 out 是空的";
    return uvcpp_db_status::MISUSE;
  }
  out->clear();
  if (!impl_->driver || !impl_->driver->is_open()) {
    impl_->last_error = "连接没打开";
    return uvcpp_db_status::NOT_CONNECTED;
  }
  std::string err;
  const uvcpp_db_status st = impl_->driver->table_schema(table, out, &err);
  if (!uvcpp_db_ok(st)) impl_->last_error = err;
  return st;
}

std::string uvcpp_db_client::escape(const std::string& text) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (impl_->driver) return impl_->driver->escape(text);
  // 没连接时的退路：单引号翻倍（SQL 标准的写法，三家都认）。MySQL 的反斜杠
  // 规则拿不到（要连接才知道 NO_BACKSLASH_ESCAPES），所以这时**不要**用它拼 SQL。
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\'') out.push_back('\'');
    out.push_back(text[i]);
  }
  return out;
}

std::string uvcpp_db_client::escape_identifier(const std::string& identifier) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (impl_->driver) return impl_->driver->quote_identifier(identifier);
  std::string out = "\"";
  for (size_t i = 0; i < identifier.size(); ++i) {
    if (identifier[i] == '"') out.push_back('"');
    out.push_back(identifier[i]);
  }
  out.push_back('"');
  return out;
}

void uvcpp_db_client::set_timeout_ms(int timeout_ms) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  impl_->timeout_ms = timeout_ms > 0 ? timeout_ms : 0;
  if (impl_->driver) impl_->driver->set_timeout_ms(impl_->timeout_ms);
}

int uvcpp_db_client::timeout_ms() const { return impl_->timeout_ms; }

void uvcpp_db_client::set_log(log_callback callback) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  impl_->log = std::move(callback);
}

std::unique_ptr<uvcpp_db_client> uvcpp_db_open(const std::string& url,
                                               uvcpp_db_status* status,
                                               std::string* error) {
  std::unique_ptr<uvcpp_db_client> client(new uvcpp_db_client());
  const uvcpp_db_status st = client->open(url);
  if (status) *status = st;
  if (!uvcpp_db_ok(st)) {
    // 失败时 client 会被销毁，`last_error()` 跟着没了 —— 所以这里必须把
    // 原因**取出来再走**。不给这个出口的话，调用方只能拿到一个光秃秃的
    // 返回码（`open_failed`），而"认证失败"和"库不存在"在排障时是两件事。
    if (error) *error = client->last_error();
    return nullptr;
  }
  return client;
}

std::string uvcpp_db_drivers() { return db_detail::uvcpp_db_available_drivers(); }

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
