/**
 * @file src/db/uvcpp_db_params.h
 * @brief 一次查询的绑定参数（位置参数）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 用法上它就是「一串值」，写法尽量短：
 *
 * @code
 * uvcpp_db_table t;
 * db.query("SELECT * FROM users WHERE age > ? AND city = ?",
 *          {30, "杭州"}, &t);          // initializer_list 直接当实参
 * @endcode
 *
 * ## 为什么坚持参数化
 *
 * 拼接 SQL 是老问题，这里是照着**本仓已有的一次事故**写的：旧的 MySQL 通道
 * 在无参数时走 `mysql_query`、有参数时走 `mysql_stmt_prepare`，两条路的转义
 * 规矩不一样，于是「有参数就安全」这句话并不成立 —— 而参数化只解决**值**，
 * 表名、列名、ORDER BY 方向这些**标识符**仍然只能由代码决定。所以：
 *
 *  * 值一律走 `?`（PostgreSQL 侧本模块自己转成 `$1..$n`）；
 *  * 要拼标识符时用 `uvcpp_db_client::escape_identifier()`，且只在白名单
 *    之后调用（它的作用是加引号，不是消毒）。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_PARAMS_H
#define SRC_DB_UVCPP_DB_PARAMS_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_DB_ENABLE

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <db/uvcpp_db_value.h>

namespace uvcpp {

class UVCPP_API uvcpp_db_params {
 public:
  uvcpp_db_params() = default;

  /// `{1, "a", 2.5}` —— 每个元素按 `uvcpp_db_value` 的构造函数定类型。
  uvcpp_db_params(std::initializer_list<uvcpp_db_value> init);

  /// 追加一个值。`add(1)`、`add("x")`、`add(std::string("x"))`、
  /// `add(uvcpp_db_value::blob(p, n))` 都可以。
  uvcpp_db_params& add(uvcpp_db_value value);

  /// 追加一个 NULL。
  uvcpp_db_params& add_null();

  size_t size() const { return values_.size(); }
  bool empty() const { return values_.empty(); }
  void clear() { values_.clear(); }

  const std::vector<uvcpp_db_value>& values() const { return values_; }
  const uvcpp_db_value& operator[](size_t index) const {
    return index < values_.size() ? values_[index] : null_slot();
  }

 private:
  static const uvcpp_db_value& null_slot();
  std::vector<uvcpp_db_value> values_;
};

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
#endif  // SRC_DB_UVCPP_DB_PARAMS_H
