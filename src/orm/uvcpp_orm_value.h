/**
 * @file src/orm/uvcpp_orm_value.h
 * @brief ORM 的值层：显式 NULL（`nullable<T>`）、类型标签与数据库类型映射。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一层**不碰数据库、不生成 SQL**。它只回答两个问题：
 *
 *   1. 一个 C++ 字段是什么类型（`uvcpp_orm_type`），以及它落到
 *      `uvcpp_db_value` 的哪个标签上（`uvcpp_orm_db_type()`）；
 *   2. 这个字段能不能是 NULL（`nullable<T>`）。
 *
 * ## 为什么不用 `std::optional`
 *
 * C++11 没有 `std::optional`，而本仓的公开头必须能在 `-std=c++11` 下编译
 * （`CMAKE_CXX_STANDARD 11`）。`nullable<T>` 是最小可用面：
 * `has_value()` / `value()` / `value_or()` / `reset()` / `operator bool`，
 * 值语义、可拷贝可移动。空的时候 `value()` 返回 `T()`，**不抛异常** ——
 * 与 `uvcpp_db_value::to_*()` 的“读不炸”是同一条规矩。
 *
 * ## 空串不是 NULL
 *
 * `nullable<std::string>` 有两种“空”：
 *
 *   * `has_value() == false` —— NULL；
 *   * `has_value() == true` 且 `value().empty()` —— 一个长度为 0 的文本值。
 *
 * 这是从 `uvcpp_db_value` 继承下来的契约（见 `src/db/uvcpp_db_value.h` 的
 * 三条契约之一）。类型映射里 `std::string` 永远映射到 `TEXT`，不是 `NIL`；
 * 空串落库之后仍然是一个非 NULL 的值。
 *
 * ## 类型映射
 *
 * | C++ 类型 | `uvcpp_orm_type` | `uvcpp_db_type` |
 * |---|---|---|
 * | `bool` | `BOOL` | `BOOL` |
 * | `std::int8_t` … `std::int64_t` | `INT8` … `INT64` | `INT64` |
 * | `std::uint8_t` … `std::uint64_t` | `UINT8` … `UINT64` | `UINT64` |
 * | `float` / `double` | `FLOAT` / `DOUBLE` | `DOUBLE` |
 * | `std::string` | `TEXT` | `TEXT` |
 * | `std::vector<std::uint8_t>` | `BLOB` | `BLOB` |
 * | `nullable<T>` | 同 `T` | 同 `T` |
 *
 * 映射按**精确类型**做：`char`、`long double`、自定义结构体都是 `UNKNOWN`，
 * 由 `validate_model<T>()` 拒绝，而不是猜一个“差不多”的数据库类型。
 */

#pragma once
#ifndef SRC_ORM_UVCPP_ORM_VALUE_H
#define SRC_ORM_UVCPP_ORM_VALUE_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <db/uvcpp_db_value.h>

namespace uvcpp {

/// 显式的 NULL / 有值两态。
///
/// 值语义：拷贝一份，两份互不影响。默认构造是 NULL；从 `T` 构造是有值
/// （**包括** `T()` 和空串 —— 那是有值，不是 NULL）。
template <typename T>
class nullable {
 public:
  /// NULL。
  nullable() : has_value_(false), value_() {}

  /// 有值。用转发构造，`nullable<std::string> s("x")` 也能直接写。
  ///
  /// 排除掉 `nullable` 自己，避免这个模板抢走拷贝/移动构造。
  template <typename U,
            typename std::enable_if<
                std::is_constructible<T, U&&>::value &&
                    !std::is_same<typename std::decay<U>::type,
                                  nullable>::value,
                int>::type = 0>
  nullable(U&& value) : has_value_(true), value_(std::forward<U>(value)) {}

  bool has_value() const { return has_value_; }
  explicit operator bool() const { return has_value_; }

  /// 空时返回 `T()`。不抛异常。
  const T& value() const { return value_; }
  T& value() { return value_; }

  /// 空时返回 `fallback`。
  T value_or(const T& fallback) const {
    return has_value_ ? value_ : fallback;
  }

  /// 赋值等于“改成有值”。`operator=(const nullable&)` / 移动赋值仍然由
  /// 编译器隐式生成，针对 nullable 自身的赋值优先于这两个重载。
  nullable& operator=(const T& value) {
    has_value_ = true;
    value_ = value;
    return *this;
  }
  nullable& operator=(T&& value) {
    has_value_ = true;
    value_ = std::move(value);
    return *this;
  }

  /// 回到 NULL，并把内部值恢复成默认值 —— 这样 `value()` 在空态下既能
  /// 返回默认值，也不会把上一次的值留在对象里。
  void reset() {
    has_value_ = false;
    value_ = T();
  }

  bool operator==(const nullable& other) const {
    if (has_value_ != other.has_value_) return false;
    return !has_value_ || value_ == other.value_;
  }
  bool operator!=(const nullable& other) const { return !(*this == other); }

 private:
  bool has_value_;
  T value_;
};

/// ORM 的字段类型标签。与 `uvcpp_db_type` 分开：前者是 C++ 侧的字段类型
/// （宽度、有无符号都保留），后者是数据库值层收口的标签。
enum class uvcpp_orm_type : int {
  /// 没有映射。`validate_model<T>()` 会拒绝这种字段。
  UNKNOWN = 0,
  BOOL,
  INT8,
  UINT8,
  INT16,
  UINT16,
  INT32,
  UINT32,
  INT64,
  UINT64,
  FLOAT,
  DOUBLE,
  TEXT,
  BLOB,
  /// 日期时间沿用 `uvcpp_db_value::date/time/datetime` 的文本语义；
  /// 目前没有 C++ 类型自动映射到这三个标签（`std::string` 仍是 TEXT）。
  DATE,
  TIME,
  DATETIME,
};

/// 稳定短名（小写下划线），可写进测试期望值。
inline const char* uvcpp_orm_type_name(uvcpp_orm_type type) {
  switch (type) {
    case uvcpp_orm_type::UNKNOWN: return "unknown";
    case uvcpp_orm_type::BOOL: return "bool";
    case uvcpp_orm_type::INT8: return "int8";
    case uvcpp_orm_type::UINT8: return "uint8";
    case uvcpp_orm_type::INT16: return "int16";
    case uvcpp_orm_type::UINT16: return "uint16";
    case uvcpp_orm_type::INT32: return "int32";
    case uvcpp_orm_type::UINT32: return "uint32";
    case uvcpp_orm_type::INT64: return "int64";
    case uvcpp_orm_type::UINT64: return "uint64";
    case uvcpp_orm_type::FLOAT: return "float";
    case uvcpp_orm_type::DOUBLE: return "double";
    case uvcpp_orm_type::TEXT: return "text";
    case uvcpp_orm_type::BLOB: return "blob";
    case uvcpp_orm_type::DATE: return "date";
    case uvcpp_orm_type::TIME: return "time";
    case uvcpp_orm_type::DATETIME: return "datetime";
  }
  return "unknown";
}

/// ORM 类型标签 → 数据库值标签。`UNKNOWN` 映射到 `NIL`，但调用方应当先用
/// `uvcpp_orm_is_known_type()` 拒掉它 —— 这个函数只是让映射表是**全覆盖**的。
inline uvcpp_db_type uvcpp_orm_db_type(uvcpp_orm_type type) {
  switch (type) {
    case uvcpp_orm_type::BOOL: return uvcpp_db_type::BOOL;
    case uvcpp_orm_type::INT8:
    case uvcpp_orm_type::INT16:
    case uvcpp_orm_type::INT32:
    case uvcpp_orm_type::INT64: return uvcpp_db_type::INT64;
    case uvcpp_orm_type::UINT8:
    case uvcpp_orm_type::UINT16:
    case uvcpp_orm_type::UINT32:
    case uvcpp_orm_type::UINT64: return uvcpp_db_type::UINT64;
    case uvcpp_orm_type::FLOAT:
    case uvcpp_orm_type::DOUBLE: return uvcpp_db_type::DOUBLE;
    case uvcpp_orm_type::TEXT: return uvcpp_db_type::TEXT;
    case uvcpp_orm_type::BLOB: return uvcpp_db_type::BLOB;
    case uvcpp_orm_type::DATE: return uvcpp_db_type::DATE;
    case uvcpp_orm_type::TIME: return uvcpp_db_type::TIME;
    case uvcpp_orm_type::DATETIME: return uvcpp_db_type::DATETIME;
    case uvcpp_orm_type::UNKNOWN: return uvcpp_db_type::NIL;
  }
  return uvcpp_db_type::NIL;
}

inline bool uvcpp_orm_is_known_type(uvcpp_orm_type type) {
  return type != uvcpp_orm_type::UNKNOWN;
}

/// 能不能当自增列。`bool` 不算整数 —— 自增一个布尔没有意义。
inline bool uvcpp_orm_type_is_integer(uvcpp_orm_type type) {
  switch (type) {
    case uvcpp_orm_type::INT8:
    case uvcpp_orm_type::UINT8:
    case uvcpp_orm_type::INT16:
    case uvcpp_orm_type::UINT16:
    case uvcpp_orm_type::INT32:
    case uvcpp_orm_type::UINT32:
    case uvcpp_orm_type::INT64:
    case uvcpp_orm_type::UINT64: return true;
    default: return false;
  }
}

namespace orm_detail {

/// 精确类型 → 标签。没有覆盖到的类型是 UNKNOWN。
template <typename T>
struct orm_type_of {
  static uvcpp_orm_type get() { return uvcpp_orm_type::UNKNOWN; }
};

template <>
struct orm_type_of<bool> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::BOOL; }
};
template <>
struct orm_type_of<std::int8_t> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::INT8; }
};
template <>
struct orm_type_of<std::uint8_t> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::UINT8; }
};
template <>
struct orm_type_of<std::int16_t> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::INT16; }
};
template <>
struct orm_type_of<std::uint16_t> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::UINT16; }
};
template <>
struct orm_type_of<std::int32_t> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::INT32; }
};
template <>
struct orm_type_of<std::uint32_t> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::UINT32; }
};
template <>
struct orm_type_of<std::int64_t> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::INT64; }
};
template <>
struct orm_type_of<std::uint64_t> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::UINT64; }
};
template <>
struct orm_type_of<float> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::FLOAT; }
};
template <>
struct orm_type_of<double> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::DOUBLE; }
};
template <>
struct orm_type_of<std::string> {
  static uvcpp_orm_type get() { return uvcpp_orm_type::TEXT; }
};
template <>
struct orm_type_of<std::vector<std::uint8_t> > {
  static uvcpp_orm_type get() { return uvcpp_orm_type::BLOB; }
};

template <typename T>
struct orm_type_of<nullable<T> > {
  static uvcpp_orm_type get() { return orm_type_of<T>::get(); }
};

}  // namespace orm_detail

/// 字段类型查询的公开面。`T` 是结构体成员声明的**原样类型**
/// （例如 `nullable<std::int32_t>`，不是 `std::int32_t`）。
template <typename T>
struct uvcpp_orm_type_traits {
  static uvcpp_orm_type type() { return orm_detail::orm_type_of<T>::get(); }
  static bool is_nullable() { return false; }
  static bool is_integer() { return uvcpp_orm_type_is_integer(type()); }
};

template <typename T>
struct uvcpp_orm_type_traits<nullable<T> > {
  static uvcpp_orm_type type() { return orm_detail::orm_type_of<T>::get(); }
  static bool is_nullable() { return true; }
  static bool is_integer() { return uvcpp_orm_type_is_integer(type()); }
};

namespace orm_detail {

/// 单个 C++ 值 ↔ `uvcpp_db_value`。默认实现对应“没有映射”的类型：
/// 转过去是 NULL，转回来失败。正常路径上 `validate_model<T>()` 会先拒绝
/// 这种字段，所以默认实现只是一道安全网。
template <typename T>
struct value_converter {
  static uvcpp_db_value to_db(const T&) { return uvcpp_db_value(); }
  static bool from_db(const uvcpp_db_value&, T*) { return false; }
};

#define UVCPP_ORM_DETAIL_INT_CONVERTER(cpp_type, reader)                    \
  template <>                                                               \
  struct value_converter<cpp_type> {                                        \
    static uvcpp_db_value to_db(const cpp_type& value) {                    \
      return uvcpp_db_value(value);                                         \
    }                                                                       \
    static bool from_db(const uvcpp_db_value& value, cpp_type* out) {       \
      if (out == nullptr || value.is_null()) return false;                  \
      *out = static_cast<cpp_type>(value.reader());                         \
      return true;                                                          \
    }                                                                       \
  };

UVCPP_ORM_DETAIL_INT_CONVERTER(std::int8_t, to_int64)
UVCPP_ORM_DETAIL_INT_CONVERTER(std::uint8_t, to_uint64)
UVCPP_ORM_DETAIL_INT_CONVERTER(std::int16_t, to_int64)
UVCPP_ORM_DETAIL_INT_CONVERTER(std::uint16_t, to_uint64)
UVCPP_ORM_DETAIL_INT_CONVERTER(std::int32_t, to_int64)
UVCPP_ORM_DETAIL_INT_CONVERTER(std::uint32_t, to_uint64)
UVCPP_ORM_DETAIL_INT_CONVERTER(std::int64_t, to_int64)
UVCPP_ORM_DETAIL_INT_CONVERTER(std::uint64_t, to_uint64)

#undef UVCPP_ORM_DETAIL_INT_CONVERTER

template <>
struct value_converter<bool> {
  static uvcpp_db_value to_db(const bool& value) {
    return uvcpp_db_value(value);
  }
  static bool from_db(const uvcpp_db_value& value, bool* out) {
    if (out == nullptr || value.is_null()) return false;
    *out = value.to_bool();
    return true;
  }
};

template <>
struct value_converter<float> {
  static uvcpp_db_value to_db(const float& value) {
    return uvcpp_db_value(value);
  }
  static bool from_db(const uvcpp_db_value& value, float* out) {
    if (out == nullptr || value.is_null()) return false;
    *out = static_cast<float>(value.to_double());
    return true;
  }
};

template <>
struct value_converter<double> {
  static uvcpp_db_value to_db(const double& value) {
    return uvcpp_db_value(value);
  }
  static bool from_db(const uvcpp_db_value& value, double* out) {
    if (out == nullptr || value.is_null()) return false;
    *out = value.to_double();
    return true;
  }
};

template <>
struct value_converter<std::string> {
  static uvcpp_db_value to_db(const std::string& value) {
    // 空串也是 TEXT，不是 NULL。
    return uvcpp_db_value(value);
  }
  static bool from_db(const uvcpp_db_value& value, std::string* out) {
    if (out == nullptr || value.is_null()) return false;
    *out = value.to_text();
    return true;
  }
};

template <>
struct value_converter<std::vector<std::uint8_t> > {
  static uvcpp_db_value to_db(const std::vector<std::uint8_t>& value) {
    return uvcpp_db_value::blob(
        value.empty() ? nullptr : static_cast<const void*>(value.data()),
        value.size());
  }
  static bool from_db(const uvcpp_db_value& value,
                      std::vector<std::uint8_t>* out) {
    if (out == nullptr || value.is_null()) return false;
    const std::string& bytes = value.bytes();
    out->assign(bytes.begin(), bytes.end());
    return true;
  }
};

template <typename T>
struct value_converter<nullable<T> > {
  static uvcpp_db_value to_db(const nullable<T>& value) {
    if (!value.has_value()) return uvcpp_db_value();
    return value_converter<T>::to_db(value.value());
  }
  static bool from_db(const uvcpp_db_value& value, nullable<T>* out) {
    if (out == nullptr) return false;
    if (value.is_null()) {
      out->reset();
      return true;
    }
    T tmp;
    if (!value_converter<T>::from_db(value, &tmp)) return false;
    *out = nullable<T>(std::move(tmp));
    return true;
  }
};

}  // namespace orm_detail

/// `T` → `uvcpp_db_value`。`nullable<T>` 的空态转成 NULL。
template <typename T>
inline uvcpp_db_value uvcpp_orm_to_db_value(const T& value) {
  return orm_detail::value_converter<T>::to_db(value);
}

/// `uvcpp_db_value` → `T`。成功返回 true；NULL 转给非空类型、或者类型没映射
/// 时返回 false（不抛异常）。`nullable<T>` 收到 NULL 是**成功**，结果是空态。
template <typename T>
inline bool uvcpp_orm_from_db_value(const uvcpp_db_value& value, T* out) {
  if (out == nullptr) return false;
  return orm_detail::value_converter<T>::from_db(value, out);
}

}  // namespace uvcpp

#endif  // UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE
#endif  // SRC_ORM_UVCPP_ORM_VALUE_H
