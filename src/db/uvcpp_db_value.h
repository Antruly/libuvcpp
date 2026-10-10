/**
 * @file src/db/uvcpp_db_value.h
 * @brief 一个单元格的值：类型标签 + 值本体 + 显式的 NULL。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ## 三条契约
 *
 * 1. **NULL 与空串是两回事。** `is_null()` 为真时读出来是 0/""，但一个
 *    `text` 型、内容是空串的值同样读出 ""，`is_null()` 为假。旧的
 *    `SqlValue` 把空串折叠成 NULL（`SqlValue(const std::string&)` 里
 *    `value.empty()` 就走 Null 分支），于是 `WHERE name = ''` 与
 *    `WHERE name IS NULL` 在结果集里长得一模一样 —— 这一版把这条修掉了，
 *    测试里有一条专门盯着它（`db_sqlite_func.cpp` 的「空串不是 NULL」）。
 *
 * 2. **不抛异常、读不炸。** 任何 `to_*()` 在类型不合适时都返回该类型的安全值
 *    （0 / 0.0 / false / 空串），并且 `to_text()` 对任意类型都有定义。
 *    数值转文本走 `snprintf`，不用 `std::to_string` —— 后者把 3.14 写成
 *    "3.140000"，然后这个字符串会被拿去当 SQL 字面量或喂给人看。
 *
 * 3. **值语义。** 可拷贝、可移动，拷完两份互不影响。旧的 `SqlValue` 删了拷贝
 *    构造、却保留了一个裸的 `refCount` 指针（每次都 `new int()`，除了泄漏
 *    什么也没做），结果上层只能靠 `new SqlValue` + 裸指针传递，所有权散在
 *    三个类里。这一版把值摊平：数值存字段，文本/blob 存 `std::string`。
 *
 * ## 类型与数据库的对应
 *
 * | 本模块 | MySQL | SQLite | PostgreSQL |
 * |---|---|---|---|
 * | `INT64`/`UINT64` | BIGINT 及以下整型 | INTEGER | int2/int4/int8 |
 * | `DOUBLE` | FLOAT/DOUBLE/DECIMAL | REAL | float4/float8/numeric |
 * | `BOOL` | TINYINT(1)/BOOL | INTEGER(0/1) | bool |
 * | `TEXT` | CHAR/VARCHAR/TEXT | TEXT | text/varchar/char |
 * | `BLOB` | BLOB/JSON 列 | BLOB | bytea |
 * | `DATE`/`TIME`/`DATETIME` | DATE/TIME/DATETIME | — | date/time/timestamp |
 *
 * 日期时间一律以**文本**形态保存（`"YYYY-MM-DD"`、`"HH:MM:SS"`、
 * `"YYYY-MM-DD HH:MM:SS"`）。这是唯一一种三家后端都能无损往返、且直接可读的
 * 表示；SQLite 本来就没有日期类型，PostgreSQL 的文本格式恰好就是这个形状。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_VALUE_H
#define SRC_DB_UVCPP_DB_VALUE_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_DB_ENABLE

#include <cstdint>
#include <ctime>
#include <string>
#include <type_traits>

namespace uvcpp {

enum class uvcpp_db_type : int {
  /// SQL NULL。取名 NIL 而不是 NULL：`<windows.h>` 把 NULL 定义成 0，
  /// 而 `<windows.h>` 会经 libuv 进来。
  NIL = 0,
  INT64,
  UINT64,
  DOUBLE,
  BOOL,
  TEXT,
  BLOB,
  DATE,
  TIME,
  DATETIME,
};

class UVCPP_API uvcpp_db_value {
 public:
  /// NULL。
  uvcpp_db_value() = default;

  uvcpp_db_value(bool v) { set_bool(v); }
  uvcpp_db_value(double v) { set_double(v); }
  uvcpp_db_value(float v) { set_double(static_cast<double>(v)); }
  uvcpp_db_value(const char* v) { set_text(v ? v : ""); }
  uvcpp_db_value(const std::string& v) { set_text(v); }
  uvcpp_db_value(std::string&& v) { set_text(std::move(v)); }

  /// 所有整型走这一个模板：不写死 `int`/`long`/`int64_t` 三个重载，
  /// 是因为 `long` 在 LP64 与 Windows 上不是同一种类型 —— 写死重载会让
  /// `uvcpp_db_value(some_long)` 在两平台上落到不同的重载、甚至判成二义。
  template <typename T,
            typename std::enable_if<std::is_integral<T>::value &&
                                        !std::is_same<T, bool>::value,
                                    int>::type = 0>
  uvcpp_db_value(T v) {
    if (std::is_signed<T>::value) {
      set_int64(static_cast<int64_t>(v));
    } else {
      set_uint64(static_cast<uint64_t>(v));
    }
  }

  /// 显式构造 blob（二进制）。不给 `const char*` 这条路 —— 二进制里必然有 0，
  /// 而 `const char*` 会静默截断。
  static uvcpp_db_value blob(const void* data, size_t size);
  static uvcpp_db_value blob(const std::string& bytes);

  /// 显式的日期时间值。传进来的文本按原样保存，不做校验/规范化 ——
  /// 校验属于后端（服务端会拒），这里多一道转换只会多一个不一致的地方。
  static uvcpp_db_value date(const std::string& text);
  static uvcpp_db_value time(const std::string& text);
  static uvcpp_db_value datetime(const std::string& text);

  // ---- 读 ----

  bool is_null() const { return type_ == uvcpp_db_type::NIL; }
  uvcpp_db_type type() const { return type_; }
  /// 类型的稳定短名："nil" / "int64" / "text" / …。
  const char* type_name() const;

  /// 数值、布尔的字符串形态；非数值类型返回**原始字节**（可能含 0，按
  /// `size()` 截）。NULL 返回空串。任何类型都有定义。
  std::string to_text() const;
  int64_t to_int64() const;
  uint64_t to_uint64() const;
  double to_double() const;
  bool to_bool() const;

  /// 日期时间 → `time_t`（本地时区，与 `std::mktime` 一致）。解析不了返回 0。
  /// 只认 `"YYYY-MM-DD[ HH:MM:SS]"`。
  std::time_t to_time_t() const;

  /// 文本/blob/日期时间型的原始字节。数值型返回空串 —— 要文本用 `to_text()`。
  const std::string& bytes() const { return text_; }
  /// 原始字节长度。`to_text()` 的长度可能不同（数值型）。
  size_t size() const { return text_.size(); }

  /// 字节级相等（NULL 与 NULL 相等；`1` 与 `1.0` 不相等，因为类型不同）。
  bool operator==(const uvcpp_db_value& other) const;
  bool operator!=(const uvcpp_db_value& other) const {
    return !(*this == other);
  }

  // ---- 写 ----

  void set_null();
  void set_bool(bool v);
  void set_int64(int64_t v);
  void set_uint64(uint64_t v);
  void set_double(double v);
  void set_text(std::string v);
  void set_blob(std::string bytes);
  void set_date(std::string text);
  void set_time(std::string text);
  void set_datetime(std::string text);

 private:
  uvcpp_db_type type_ = uvcpp_db_type::NIL;
  int64_t int_ = 0;
  uint64_t uint_ = 0;
  double double_ = 0.0;
  bool bool_ = false;
  std::string text_;  // TEXT / BLOB / DATE / TIME / DATETIME
};

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
#endif  // SRC_DB_UVCPP_DB_VALUE_H
