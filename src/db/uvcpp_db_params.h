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
 * db.query("SELECT * FROM users WHERE age > ? AND city = ?",   // `?`：SQLite/MySQL
 *          {30, "杭州"}, &t);          // initializer_list 直接当实参
 * @endcode
 *
 * PostgreSQL 那条连接上同样的查询要写 `age > $1 AND city = $2` —— 理由见下。
 *
 * ## 为什么坚持参数化
 *
 * 拼接 SQL 是老问题，这里是照着**本仓已有的一次事故**写的：旧的 MySQL 通道
 * 在无参数时走 `mysql_query`、有参数时走 `mysql_stmt_prepare`，两条路的转义
 * 规矩不一样，于是「有参数就安全」这句话并不成立 —— 而参数化只解决**值**，
 * 表名、列名、ORDER BY 方向这些**标识符**仍然只能由代码决定。所以：
 *
 *  * 值走**方言自己的**占位符 —— 本模块**原样**把 SQL 交给驱动，不换算：
 *    SQLite / MySQL 是 `?`，PostgreSQL 是 `$1..$n`（`$1`、`$2` 依次对应参数
 *    表里的第 0、1 个值）。共用套件里这件事由 `dialect::ph(n)` 收口；
 *  * 要拼标识符时用 `uvcpp_db_client::escape_identifier()`，且只在白名单
 *    之后调用（它的作用是加引号，不是消毒）。
 *
 * **为什么不换算**：PostgreSQL 里 `?` 本身就是合法的操作符（jsonb / hstore 的
 * 「键存在」），`?` 也可以出现在字符串字面量或带引号的标识符里 —— 盲换会在
 * **合法**的 SQL 上静默改语义。反过来 SQLite 把 `$1` 当**命名参数**收下（能跑
 * 通，但语义与位置参数不同），所以两家的写法也没法归一。只测 SQLite 看不出这个
 * 差异 —— 拿 `?` 去 PG 会报 `PREPARE_FAILED`（`syntax error at end of input`）。
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
