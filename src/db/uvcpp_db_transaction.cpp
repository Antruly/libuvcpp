/**
 * @file src/db/uvcpp_db_transaction.cpp
 * @brief `uvcpp_db_tx` 的实现（池子那一半在 `uvcpp_db_pool.cpp`）。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "db/uvcpp_db_transaction.h"

#if UVCPP_DB_ENABLE

#include "db/uvcpp_db.h"

#include <chrono>

namespace uvcpp {

uvcpp_db_tx::uvcpp_db_tx(uvcpp_db_client* client, int timeout_ms)
    : client_(client),
      timeout_ms_(timeout_ms > 0 ? timeout_ms : 0),
      start_(std::chrono::steady_clock::now()) {
  if (timeout_ms_ > 0) {
    deadline_     = start_ + std::chrono::milliseconds(timeout_ms_);
    has_deadline_ = true;
  }
}

int64_t uvcpp_db_tx::elapsed_ms() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start_)
      .count();
}

uvcpp_db_status uvcpp_db_tx::gate() {
  // 直接构造出来的对象没有连接：每一条语句都要在这里说清楚，而不是把空指针
  // 交给 `uvcpp_db_client` 去解引用。
  if (client_ == nullptr) {
    if (first_error_ == uvcpp_db_status::OK) {
      first_error_ = uvcpp_db_status::MISUSE;
      error_       = "事务没有可用连接（uvcpp_db_tx 只能由 pool.transaction() 构造）";
    }
    return first_error_;
  }
  if (finished_) return uvcpp_db_status::MISUSE;
  // 第一条失败的语句之后不再往下发：继续发只会让同一个事务在更糟的状态上
  // 再失败几次，而调用方拿到的诊断信息并不会更多。
  if (first_error_ != uvcpp_db_status::OK) return first_error_;
  if (has_deadline_ && std::chrono::steady_clock::now() >= deadline_) {
    first_error_ = uvcpp_db_status::TIMEOUT;
    error_       = "事务超过 timeout_ms 预算（" +
                   std::to_string(timeout_ms_) + " ms），不再执行后续语句";
    return first_error_;
  }
  return uvcpp_db_status::OK;
}

void uvcpp_db_tx::record(uvcpp_db_status st) {
  if (st == uvcpp_db_status::OK) return;
  if (first_error_ != uvcpp_db_status::OK) return;
  first_error_ = st;
  if (client_ != nullptr) error_ = client_->last_error();
  if (error_.empty()) error_ = uvcpp_db_status_name(st);
}

// ---------------------------------------------------------------------------
// 语句
// ---------------------------------------------------------------------------

uvcpp_db_status uvcpp_db_tx::query(const std::string& sql,
                                   uvcpp_db_table* out) {
  return query(sql, uvcpp_db_params(), out);
}

uvcpp_db_status uvcpp_db_tx::query(const std::string& sql,
                                   const uvcpp_db_params& params,
                                   uvcpp_db_table* out) {
  const uvcpp_db_status g = gate();
  if (g != uvcpp_db_status::OK) return g;
  const uvcpp_db_status st = client_->query(sql, params, out);
  record(st);
  return st;
}

uvcpp_db_status uvcpp_db_tx::execute(const std::string& sql,
                                     int64_t* affected) {
  return execute(sql, uvcpp_db_params(), affected);
}

uvcpp_db_status uvcpp_db_tx::execute(const std::string& sql,
                                     const uvcpp_db_params& params,
                                     int64_t* affected) {
  const uvcpp_db_status g = gate();
  if (g != uvcpp_db_status::OK) return g;
  const uvcpp_db_status st = client_->execute(sql, params, affected);
  record(st);
  return st;
}

uvcpp_db_status uvcpp_db_tx::insert(const std::string& sql,
                                    const uvcpp_db_params& params,
                                    uvcpp_db_value* new_id) {
  const uvcpp_db_status g = gate();
  if (g != uvcpp_db_status::OK) return g;
  const uvcpp_db_status st = client_->insert(sql, params, new_id);
  record(st);
  return st;
}

// ---------------------------------------------------------------------------
// 收尾
// ---------------------------------------------------------------------------

uvcpp_db_status uvcpp_db_tx::commit() {
  if (client_ == nullptr) return uvcpp_db_status::MISUSE;
  if (finished_) return uvcpp_db_status::MISUSE;
  // 已经有一条语句失败：这个事务**不能**提交。返回第一笔错误，让调用方看到
  // 真正的原因（而不是一个笼统的"提交失败"）。
  if (first_error_ != uvcpp_db_status::OK) return first_error_;
  // 提交也算事务的一部分：deadline 到了就先回滚，不再"抢着提交一个已经超预算
  // 的事务"。`rollback()` 故意**不**看 deadline —— 收尾永远要有出路。
  if (has_deadline_ && std::chrono::steady_clock::now() >= deadline_) {
    first_error_ = uvcpp_db_status::TIMEOUT;
    error_       = "事务超过 timeout_ms 预算（" + std::to_string(timeout_ms_) +
                   " ms），不再提交";
    finished_    = true;
    return first_error_;
  }

  const uvcpp_db_status st = client_->commit();
  finished_ = true;
  if (st == uvcpp_db_status::OK) {
    committed_ = true;
  } else {
    record(st);
  }
  return st;
}

uvcpp_db_status uvcpp_db_tx::rollback() {
  if (client_ == nullptr) return uvcpp_db_status::MISUSE;
  if (finished_) return uvcpp_db_status::MISUSE;

  const uvcpp_db_status st = client_->rollback();
  finished_    = true;
  rolled_back_ = true;
  if (st != uvcpp_db_status::OK) record(st);
  return st;
}

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
