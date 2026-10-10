/**
 * @file src/db/uvcpp_db_value.cpp
 * @brief `uvcpp_db_value` 的实现：类型转换、日期时间解析与格式化。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "db/uvcpp_db_value.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace uvcpp {

namespace {

/// double → 文本，**最短且能原样读回**的那种写法。
///
/// `%.17g` 总是能读回，但 0.1 会写成 "0.10000000000000001"；`%g`（6 位有效）读
/// 不回来。做法是先按 15 位有效试，读回来 bit 级相等就收工，否则用 17 位兜底
/// —— IEEE754 双精度用 17 位十进制数一定能唯一还原。
std::string double_to_text(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.15g", v);
  if (std::strtod(buf, nullptr) != v) {
    std::snprintf(buf, sizeof(buf), "%.17g", v);
  }
  return std::string(buf);
}

bool parse_time_text(const char* s, std::tm* out) {
  if (!s) return false;
  std::istringstream ss(s);
  ss >> std::get_time(out, "%Y-%m-%d %H:%M:%S");
  if (!ss.fail()) return true;
  ss.clear();
  ss.str(s);
  ss >> std::get_time(out, "%Y-%m-%d");
  return !ss.fail();
}

}  // namespace

uvcpp_db_value uvcpp_db_value::blob(const void* data, size_t size) {
  uvcpp_db_value v;
  if (!data || size == 0) {
    // 空 blob 是有意义的值（与 NULL 不同）：长度 0 的字节串。
    v.set_blob(std::string());
    return v;
  }
  v.set_blob(std::string(static_cast<const char*>(data), size));
  return v;
}

uvcpp_db_value uvcpp_db_value::blob(const std::string& bytes) {
  uvcpp_db_value v;
  v.set_blob(bytes);
  return v;
}

uvcpp_db_value uvcpp_db_value::date(const std::string& text) {
  uvcpp_db_value v;
  v.set_date(text);
  return v;
}

uvcpp_db_value uvcpp_db_value::time(const std::string& text) {
  uvcpp_db_value v;
  v.set_time(text);
  return v;
}

uvcpp_db_value uvcpp_db_value::datetime(const std::string& text) {
  uvcpp_db_value v;
  v.set_datetime(text);
  return v;
}

const char* uvcpp_db_value::type_name() const {
  switch (type_) {
    case uvcpp_db_type::NIL: return "nil";
    case uvcpp_db_type::INT64: return "int64";
    case uvcpp_db_type::UINT64: return "uint64";
    case uvcpp_db_type::DOUBLE: return "double";
    case uvcpp_db_type::BOOL: return "bool";
    case uvcpp_db_type::TEXT: return "text";
    case uvcpp_db_type::BLOB: return "blob";
    case uvcpp_db_type::DATE: return "date";
    case uvcpp_db_type::TIME: return "time";
    case uvcpp_db_type::DATETIME: return "datetime";
  }
  return "unknown";
}

std::string uvcpp_db_value::to_text() const {
  switch (type_) {
    case uvcpp_db_type::NIL:
      return std::string();
    case uvcpp_db_type::INT64:
      return std::to_string(int_);
    case uvcpp_db_type::UINT64:
      return std::to_string(uint_);
    case uvcpp_db_type::DOUBLE:
      return double_to_text(double_);
    case uvcpp_db_type::BOOL:
      return bool_ ? "true" : "false";
    case uvcpp_db_type::TEXT:
    case uvcpp_db_type::BLOB:
    case uvcpp_db_type::DATE:
    case uvcpp_db_type::TIME:
    case uvcpp_db_type::DATETIME:
      return text_;
  }
  return std::string();
}

int64_t uvcpp_db_value::to_int64() const {
  switch (type_) {
    case uvcpp_db_type::NIL: return 0;
    case uvcpp_db_type::INT64: return int_;
    case uvcpp_db_type::UINT64: return static_cast<int64_t>(uint_);
    case uvcpp_db_type::DOUBLE: return static_cast<int64_t>(double_);
    case uvcpp_db_type::BOOL: return bool_ ? 1 : 0;
    default:
      // 文本型：能解析就解析，不能就 0（不抛）。用 strtoll 而不是 stoll ——
      // 后者对 "12abc" 返回 12 但对 "abc" 抛异常。
      return std::strtoll(text_.c_str(), nullptr, 10);
  }
}

uint64_t uvcpp_db_value::to_uint64() const {
  switch (type_) {
    case uvcpp_db_type::NIL: return 0;
    case uvcpp_db_type::INT64:
      return int_ < 0 ? 0 : static_cast<uint64_t>(int_);
    case uvcpp_db_type::UINT64: return uint_;
    case uvcpp_db_type::DOUBLE:
      return double_ < 0 ? 0 : static_cast<uint64_t>(double_);
    case uvcpp_db_type::BOOL: return bool_ ? 1 : 0;
    default:
      return std::strtoull(text_.c_str(), nullptr, 10);
  }
}

double uvcpp_db_value::to_double() const {
  switch (type_) {
    case uvcpp_db_type::NIL: return 0.0;
    case uvcpp_db_type::INT64: return static_cast<double>(int_);
    case uvcpp_db_type::UINT64: return static_cast<double>(uint_);
    case uvcpp_db_type::DOUBLE: return double_;
    case uvcpp_db_type::BOOL: return bool_ ? 1.0 : 0.0;
    default:
      return std::strtod(text_.c_str(), nullptr);
  }
}

bool uvcpp_db_value::to_bool() const {
  switch (type_) {
    case uvcpp_db_type::NIL: return false;
    case uvcpp_db_type::INT64: return int_ != 0;
    case uvcpp_db_type::UINT64: return uint_ != 0;
    case uvcpp_db_type::DOUBLE: return double_ != 0.0;
    case uvcpp_db_type::BOOL: return bool_;
    default: {
      std::string s = text_;
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return s == "true" || s == "1" || s == "t" || s == "y" || s == "yes";
    }
  }
}

std::time_t uvcpp_db_value::to_time_t() const {
  if (type_ == uvcpp_db_type::NIL) return 0;
  if (type_ == uvcpp_db_type::INT64) return static_cast<std::time_t>(int_);
  std::tm tm = {};
  if (!parse_time_text(text_.c_str(), &tm)) return 0;
  tm.tm_isdst = -1;  // 让 mktime 自己判断夏令时
  return std::mktime(&tm);
}

bool uvcpp_db_value::operator==(const uvcpp_db_value& other) const {
  if (type_ != other.type_) return false;
  switch (type_) {
    case uvcpp_db_type::NIL: return true;
    case uvcpp_db_type::INT64: return int_ == other.int_;
    case uvcpp_db_type::UINT64: return uint_ == other.uint_;
    case uvcpp_db_type::DOUBLE: return double_ == other.double_;
    case uvcpp_db_type::BOOL: return bool_ == other.bool_;
    default: return text_ == other.text_;
  }
}

void uvcpp_db_value::set_null() {
  type_ = uvcpp_db_type::NIL;
  int_ = 0;
  uint_ = 0;
  double_ = 0.0;
  bool_ = false;
  text_.clear();
}

void uvcpp_db_value::set_bool(bool v) {
  set_null();
  type_ = uvcpp_db_type::BOOL;
  bool_ = v;
}

void uvcpp_db_value::set_int64(int64_t v) {
  set_null();
  type_ = uvcpp_db_type::INT64;
  int_ = v;
}

void uvcpp_db_value::set_uint64(uint64_t v) {
  set_null();
  type_ = uvcpp_db_type::UINT64;
  uint_ = v;
}

void uvcpp_db_value::set_double(double v) {
  set_null();
  type_ = uvcpp_db_type::DOUBLE;
  double_ = v;
}

void uvcpp_db_value::set_text(std::string v) {
  set_null();
  type_ = uvcpp_db_type::TEXT;
  text_ = std::move(v);
}

void uvcpp_db_value::set_blob(std::string bytes) {
  set_null();
  type_ = uvcpp_db_type::BLOB;
  text_ = std::move(bytes);
}

void uvcpp_db_value::set_date(std::string text) {
  set_null();
  type_ = uvcpp_db_type::DATE;
  text_ = std::move(text);
}

void uvcpp_db_value::set_time(std::string text) {
  set_null();
  type_ = uvcpp_db_type::TIME;
  text_ = std::move(text);
}

void uvcpp_db_value::set_datetime(std::string text) {
  set_null();
  type_ = uvcpp_db_type::DATETIME;
  text_ = std::move(text);
}

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
