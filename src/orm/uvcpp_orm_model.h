/**
 * @file src/orm/uvcpp_orm_model.h
 * @brief ORM 的模型声明、字段元数据与元数据校验。
 * @author zhuweiye
 * @version 1.0.0
 *
 * C++11 没有反射，所以模型用“手写 traits + 三个宏”声明。这不是代码生成：
 * 宏只展开成静态数组与函数，不产生运行期注册，也不依赖任何数据库客户端库。
 *
 * 用法（宏必须写在**全局作用域**，类型可以用命名空间限定）：
 *
 * ```cpp
 * struct user {
 *   std::int64_t id;
 *   std::string name;
 *   nullable<std::int32_t> age;
 * };
 *
 * UVCPP_ORM_MODEL(user, "users")
 * UVCPP_ORM_FIELD(user, id, "id",
 *                 UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
 * UVCPP_ORM_FIELD(user, name, "name", UVCPP_ORM_NOT_NULL)
 * UVCPP_ORM_FIELD(user, age, "age", UVCPP_ORM_NONE)
 * UVCPP_ORM_MODEL_END()
 * ```
 *
 * `UVCPP_ORM_FIELD` 自己带结尾逗号，所以字段之间不用再手写逗号；少一个
 * 逗号的错误也就没有机会发生。
 *
 * ## 元数据能回答什么
 *
 *   * 表名：`model_traits<T>::table_name()`；
 *   * 字段数量：`model_traits<T>::column_count()`；
 *   * 按声明顺序的字段：`model_traits<T>::columns()[i]`，每一项带字段名、
 *     列名、`uvcpp_orm_type`、`offsetof` 偏移、`sizeof` 大小与标志；
 *   * 主键：`model_traits<T>::primary_key()`；
 *   * 按列名 / 字段名 / 成员指针查找：`column_by_name()` /
 *     `column_by_field_name()` / `column_for(&T::member)`。
 *
 * ## 标志
 *
 * `column_flag` 是 `enum class`，组合成 `column_flags`（一个无符号位集）。
 * C++11 没有 `std::optional`，也没有 C++17 的折叠表达式；这里的位运算全是
 * 手写重载，不依赖更高标准。
 *
 * `uvcpp_orm_type_traits<decltype(T::member)>::is_nullable()` 会在声明时
 * 自动给 `nullable<T>` 字段补上 `column_flag::nullable`。所以“字段是
 * `nullable<T>` 却标了 `not_null`”这种冲突不需要用户再写一遍标志就能被
 * `validate_model<T>()` 抓到。
 *
 * ## 校验
 *
 * ```cpp
 * std::string error;
 * const uvcpp_orm_status status = validate_model<user>(&error);
 * ```
 *
 * 或拿完整结果：
 *
 * ```cpp
 * const uvcpp_orm_validation_result result = validate_model<user>();
 * ```
 *
 * 校验拒绝：空表名 / 空列名、控制字符、重复列名、主键不是恰好一个、自增
 * 不在整数主键上、没有映射的类型、`nullable` 与 `not_null` 同时出现。
 * 错误串里一定带字段名。
 *
 * ## 结构体的形状
 *
 * 字段偏移来自 `offsetof`，所以模型必须是**公开成员、无基类、无虚函数**的
 * 聚合形状。`std::string` 这样的成员让类型在 C++11 的严格定义下不算
 * standard-layout，`offsetof` 属于“条件支持”；GCC/Clang/MSVC 在本仓支持的
 * 平台上都给出正确的成员偏移。这是本仓在 C++11 下选择手写 traits 的已知
 * 代价，后续版本若引入代码生成可以只改这一层。
 */

#pragma once
#ifndef SRC_ORM_UVCPP_ORM_MODEL_H
#define SRC_ORM_UVCPP_ORM_MODEL_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE

#include <cstddef>
#include <string>

#include <orm/uvcpp_orm_error.h>
#include <orm/uvcpp_orm_value.h>

namespace uvcpp {

/// 单个列/字段的标志位。
enum class column_flag : unsigned int {
  none = 0u,
  /// 主键。1.12.0 只支持单列主键。
  primary_key = 1u << 0,
  /// 自增。只能出现在整数主键上。
  auto_increment = 1u << 1,
  /// 非空。`nullable<T>` 字段不能同时带它。
  not_null = 1u << 2,
  /// 唯一约束。
  unique = 1u << 3,
  /// 只读（后续 CRUD 用；元数据层只保留这个声明）。
  read_only = 1u << 4,
  /// 可空。`nullable<T>` 字段会**自动**带上它，用户一般不用手写。
  nullable = 1u << 5,
};

/// `column_flag` 的组合。
///
/// 写成一个小值类型而不是裸 `unsigned`：调用方只能通过 `has()` 读，不能
/// 拿一个不相干的整数当标志传进来。位运算重载在下面。
class column_flags {
 public:
  column_flags() : bits_(0u) {}
  column_flags(column_flag flag) : bits_(static_cast<unsigned int>(flag)) {}

  unsigned int bits() const { return bits_; }
  bool has(column_flag flag) const {
    return (bits_ & static_cast<unsigned int>(flag)) != 0u;
  }
  bool empty() const { return bits_ == 0u; }

  column_flags& operator|=(column_flag flag) {
    bits_ |= static_cast<unsigned int>(flag);
    return *this;
  }
  column_flags& operator|=(const column_flags& other) {
    bits_ |= other.bits_;
    return *this;
  }
  column_flags& operator&=(const column_flags& other) {
    bits_ &= other.bits_;
    return *this;
  }

 private:
  unsigned int bits_;
};

inline column_flags operator|(column_flag a, column_flag b) {
  column_flags result(a);
  result |= b;
  return result;
}
inline column_flags operator|(column_flags a, column_flag b) {
  a |= b;
  return a;
}
inline column_flags operator|(column_flag a, column_flags b) {
  b |= a;
  return b;
}
inline column_flags operator|(column_flags a, column_flags b) {
  a |= b;
  return a;
}
inline column_flags operator&(column_flags a, column_flags b) {
  a &= b;
  return a;
}

/// 一个字段的元数据。非模板的：它只存偏移与大小，不存成员指针类型，
/// 因此可以直接放进静态数组。
class uvcpp_orm_column {
 public:
  uvcpp_orm_column()
      : field_name_(nullptr),
        column_name_(nullptr),
        type_(uvcpp_orm_type::UNKNOWN),
        offset_(0u),
        size_(0u),
        flags_() {}

  uvcpp_orm_column(const char* field_name, const char* column_name,
                   uvcpp_orm_type type, std::size_t offset, std::size_t size,
                   column_flags flags)
      : field_name_(field_name),
        column_name_(column_name),
        type_(type),
        offset_(offset),
        size_(size),
        flags_(flags) {}

  /// 结构体里的成员名（宏用 `#Member` 生成）。
  const char* field_name() const { return field_name_; }
  /// 数据库里的列名。
  const char* column_name() const { return column_name_; }
  uvcpp_orm_type type() const { return type_; }
  /// `offsetof(T, member)`。
  std::size_t offset() const { return offset_; }
  /// `sizeof(member)`。`nullable<T>` 是两态对象的完整大小。
  std::size_t size() const { return size_; }
  column_flags flags() const { return flags_; }

  bool has_flag(column_flag flag) const { return flags_.has(flag); }
  bool is_primary_key() const { return has_flag(column_flag::primary_key); }
  bool is_auto_increment() const {
    return has_flag(column_flag::auto_increment);
  }
  bool is_not_null() const { return has_flag(column_flag::not_null); }
  bool is_unique() const { return has_flag(column_flag::unique); }
  bool is_read_only() const { return has_flag(column_flag::read_only); }
  bool is_nullable() const { return has_flag(column_flag::nullable); }

 private:
  const char* field_name_;
  const char* column_name_;
  uvcpp_orm_type type_;
  std::size_t offset_;
  std::size_t size_;
  column_flags flags_;
};

namespace orm_detail {

/// 宏生成的静态数组的“指针 + 长度”。长度在数组所在的函数里算，
/// 这样 `column_count()` 不用另一处手写数字。
struct uvcpp_orm_column_list {
  const uvcpp_orm_column* data;
  std::size_t size;
};

/// 默认元数据：类型没有声明模型。`validate_model<T>()` 据此报 `NO_MODEL`。
template <typename T>
struct uvcpp_orm_metadata {
  static const char* table_name() { return nullptr; }
  static const uvcpp_orm_column* columns() { return nullptr; }
  static std::size_t column_count() { return 0u; }
};

/// 给 `nullable<T>` 字段自动补上 `nullable` 标志。用户手写的标志原样保留。
template <typename T>
inline column_flags finalize_flags(column_flags flags) {
  if (uvcpp_orm_type_traits<T>::is_nullable()) {
    flags |= column_flag::nullable;
  }
  return flags;
}

}  // namespace orm_detail

/// 模型的公开 traits。宏特化的是内部 `orm_detail::uvcpp_orm_metadata<T>`，
/// 这一层把查找辅助函数一次性写好。
template <typename T>
struct model_traits : orm_detail::uvcpp_orm_metadata<T> {
  /// 主键列；没有声明主键时返回空指针（`validate_model<T>()` 会报错）。
  static const uvcpp_orm_column* primary_key() {
    const uvcpp_orm_column* cols =
        orm_detail::uvcpp_orm_metadata<T>::columns();
    const std::size_t count =
        orm_detail::uvcpp_orm_metadata<T>::column_count();
    for (std::size_t i = 0; i < count; ++i) {
      if (cols[i].is_primary_key()) return &cols[i];
    }
    return nullptr;
  }

  /// 按**列名**找字段；没有时返回空指针。
  static const uvcpp_orm_column* column_by_name(const char* name) {
    const std::size_t index = column_index(name);
    if (index >= orm_detail::uvcpp_orm_metadata<T>::column_count()) {
      return nullptr;
    }
    return &orm_detail::uvcpp_orm_metadata<T>::columns()[index];
  }

  /// 按**结构体成员名**找字段；没有时返回空指针。
  static const uvcpp_orm_column* column_by_field_name(const char* name) {
    if (name == nullptr) return nullptr;
    const uvcpp_orm_column* cols =
        orm_detail::uvcpp_orm_metadata<T>::columns();
    const std::size_t count =
        orm_detail::uvcpp_orm_metadata<T>::column_count();
    for (std::size_t i = 0; i < count; ++i) {
      const char* field = cols[i].field_name();
      if (field != nullptr && std::string(field) == name) return &cols[i];
    }
    return nullptr;
  }

  /// 按列名找下标；没有时返回 `column_count()`（不是 `npos`，免得多一个
  /// 需要定义的静态常量）。
  static std::size_t column_index(const char* name) {
    if (name == nullptr) {
      return orm_detail::uvcpp_orm_metadata<T>::column_count();
    }
    const uvcpp_orm_column* cols =
        orm_detail::uvcpp_orm_metadata<T>::columns();
    const std::size_t count =
        orm_detail::uvcpp_orm_metadata<T>::column_count();
    for (std::size_t i = 0; i < count; ++i) {
      const char* column = cols[i].column_name();
      if (column != nullptr && std::string(column) == name) return i;
    }
    return count;
  }
};

/// 按成员指针找列元数据。用默认构造的 `T` 取实际地址，再与元数据里的
/// `offsetof` 对齐 —— C++11 没有从成员指针直接取偏移的标准手段。
///
/// 代价：`T` 必须可默认构造（1.12.0 的模型都是聚合）。没有匹配时返回空指针。
template <typename T, typename M>
inline const uvcpp_orm_column* column_for(M T::* member) {
  T instance;
  const unsigned char* base =
      reinterpret_cast<const unsigned char*>(&instance);
  const unsigned char* field =
      reinterpret_cast<const unsigned char*>(&(instance.*member));
  const std::size_t offset = static_cast<std::size_t>(field - base);

  const uvcpp_orm_column* cols = model_traits<T>::columns();
  const std::size_t count = model_traits<T>::column_count();
  for (std::size_t i = 0; i < count; ++i) {
    if (cols[i].offset() == offset && cols[i].size() == sizeof(M)) {
      return &cols[i];
    }
  }
  return nullptr;
}

namespace orm_detail {

/// 表名/列名里不允许出现控制字符。允许 UTF-8 的高位字节 —— 中文标识符
/// 在 MySQL/PostgreSQL 上是合法的，拒绝它不属于元数据层的职责。
inline bool has_control_char(const char* text) {
  if (text == nullptr) return false;
  for (const unsigned char* p =
           reinterpret_cast<const unsigned char*>(text);
       *p != 0u; ++p) {
    if (*p < 0x20u || *p == 0x7fu) return true;
  }
  return false;
}

inline unsigned char ascii_lower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A'))
                                : c;
}

/// 列名重复判断。按 ASCII 大小写不敏感：SQL 标识符在 MySQL/PostgreSQL
/// 上默认大小写不敏感，`Name` 与 `name` 同时出现应当被拒绝，而不是等到
/// 建表时才由后端报一句方言相关的话。
inline bool ascii_iequal(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return a == b;
  while (*a != 0 && *b != 0) {
    if (ascii_lower(static_cast<unsigned char>(*a)) !=
        ascii_lower(static_cast<unsigned char>(*b))) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == *b;
}

inline uvcpp_orm_status set_error(std::string* error, uvcpp_orm_status status,
                                  const std::string& message) {
  if (error != nullptr) *error = message;
  return status;
}

inline std::string field_label(const uvcpp_orm_column& column) {
  const char* field = column.field_name();
  return std::string("field \"") + (field != nullptr ? field : "?") + "\"";
}

}  // namespace orm_detail

/// 校验模型元数据。`error` 可以为空；不为空且失败时写入人读原因。
///
/// 检查顺序：表名 → 字段逐个（列名、类型、标志、重复）→ 主键数量。
/// 失败返回非 `OK`；错误串里一定带字段名（表名错误除外）。
template <typename T>
inline uvcpp_orm_status validate_model(std::string* error) {
  const char* table = model_traits<T>::table_name();
  if (table == nullptr) {
    return orm_detail::set_error(error, uvcpp_orm_status::NO_MODEL,
                                 "no model is declared for this type");
  }
  if (*table == '\0') {
    return orm_detail::set_error(error, uvcpp_orm_status::EMPTY_TABLE_NAME,
                                 "table name is empty");
  }
  if (orm_detail::has_control_char(table)) {
    return orm_detail::set_error(
        error, uvcpp_orm_status::INVALID_TABLE_NAME,
        std::string("table name \"") + table +
            "\" contains a control character");
  }

  const uvcpp_orm_column* cols = model_traits<T>::columns();
  const std::size_t count = model_traits<T>::column_count();
  if (cols == nullptr || count == 0u) {
    return orm_detail::set_error(error, uvcpp_orm_status::NO_COLUMNS,
                                 std::string("table \"") + table +
                                     "\" has no columns");
  }

  std::size_t primary_key_count = 0u;
  std::string primary_key_names;

  for (std::size_t i = 0; i < count; ++i) {
    const uvcpp_orm_column& column = cols[i];
    const std::string label = orm_detail::field_label(column);
    const char* name = column.column_name();

    if (name == nullptr || *name == '\0') {
      return orm_detail::set_error(error, uvcpp_orm_status::EMPTY_COLUMN_NAME,
                                   label + " has an empty column name");
    }
    if (orm_detail::has_control_char(name)) {
      return orm_detail::set_error(
          error, uvcpp_orm_status::INVALID_COLUMN_NAME,
          label + " column name \"" + name +
              "\" contains a control character");
    }
    if (!uvcpp_orm_is_known_type(column.type())) {
      return orm_detail::set_error(error, uvcpp_orm_status::UNSUPPORTED_TYPE,
                                   label + " has an unsupported type");
    }

    // `nullable<T>` 在声明时被自动补上 nullable 标志；这里只判冲突。
    if (column.is_nullable() && column.is_not_null()) {
      return orm_detail::set_error(
          error, uvcpp_orm_status::CONFLICTING_FLAGS,
          label + " is nullable but also marked not_null");
    }

    if (column.is_auto_increment()) {
      if (!column.is_primary_key()) {
        return orm_detail::set_error(
            error, uvcpp_orm_status::INVALID_AUTO_INCREMENT,
            label + " is auto_increment but is not a primary key");
      }
      if (!uvcpp_orm_type_is_integer(column.type())) {
        return orm_detail::set_error(
            error, uvcpp_orm_status::INVALID_AUTO_INCREMENT,
            label + " is auto_increment but its type (" +
                uvcpp_orm_type_name(column.type()) + ") is not an integer");
      }
    }

    for (std::size_t j = 0; j < i; ++j) {
      const char* previous = cols[j].column_name();
      if (orm_detail::ascii_iequal(previous, name)) {
        return orm_detail::set_error(
            error, uvcpp_orm_status::DUPLICATE_COLUMN,
            label + " duplicates column name \"" + name +
                "\" already used by " + orm_detail::field_label(cols[j]));
      }
    }

    if (column.is_primary_key()) {
      ++primary_key_count;
      if (!primary_key_names.empty()) primary_key_names += ", ";
      primary_key_names += label;
    }
  }

  if (primary_key_count == 0u) {
    return orm_detail::set_error(
        error, uvcpp_orm_status::NO_PRIMARY_KEY,
        std::string("table \"") + table + "\" has no primary key");
  }
  if (primary_key_count > 1u) {
    return orm_detail::set_error(
        error, uvcpp_orm_status::MULTIPLE_PRIMARY_KEYS,
        std::string("table \"") + table + "\" has " +
            std::to_string(primary_key_count) + " primary keys (" +
            primary_key_names + "); exactly one is required");
  }

  if (error != nullptr) error->clear();
  return uvcpp_orm_status::OK;
}

/// 同上，但把返回码与错误串一起返回。
template <typename T>
inline uvcpp_orm_validation_result validate_model() {
  uvcpp_orm_validation_result result;
  result.status = validate_model<T>(&result.error);
  return result;
}

}  // namespace uvcpp

// ---------------------------------------------------------------------------
// 声明宏
// ---------------------------------------------------------------------------

/// 标志别名。使用处写成 `UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT`。
#define UVCPP_ORM_NONE ::uvcpp::column_flag::none
#define UVCPP_ORM_PRIMARY_KEY ::uvcpp::column_flag::primary_key
#define UVCPP_ORM_AUTO_INCREMENT ::uvcpp::column_flag::auto_increment
#define UVCPP_ORM_NOT_NULL ::uvcpp::column_flag::not_null
#define UVCPP_ORM_UNIQUE ::uvcpp::column_flag::unique
#define UVCPP_ORM_READ_ONLY ::uvcpp::column_flag::read_only
#define UVCPP_ORM_NULLABLE ::uvcpp::column_flag::nullable

/// 开始声明模型。必须在全局作用域调用；`Type` 可以是命名空间限定的类型。
/// `TableName` 必须是字符串字面量（元数据只存指针，不做拷贝）。
#define UVCPP_ORM_MODEL(Type, TableName)                              \
  namespace uvcpp {                                                   \
  namespace orm_detail {                                              \
  template <>                                                         \
  struct uvcpp_orm_metadata<Type> {                                   \
    static const char* table_name() { return TableName; }             \
    static const uvcpp_orm_column* columns() {                        \
      return column_list().data;                                      \
    }                                                                 \
    static std::size_t column_count() { return column_list().size; }  \
                                                                      \
   private:                                                           \
    static const uvcpp_orm_column_list& column_list() {               \
      static const uvcpp_orm_column kColumns[] = {

/// 声明一个字段。`Member` 是结构体成员名，`ColumnName` 是数据库列名，
/// `Flags` 是 `column_flag` 的组合。
///
/// 末尾自带逗号：字段之间不要手写逗号。
#define UVCPP_ORM_FIELD(Type, Member, ColumnName, Flags)                     \
  uvcpp_orm_column(#Member, ColumnName,                                      \
                   uvcpp_orm_type_traits<decltype(Type::Member)>::type(),    \
                   offsetof(Type, Member), sizeof(Type::Member),             \
                   ::uvcpp::orm_detail::finalize_flags<                      \
                       decltype(Type::Member)>(Flags)),

/// 结束模型声明。
#define UVCPP_ORM_MODEL_END()                                             \
      };                                                                  \
      static const uvcpp_orm_column_list kList = {                        \
          kColumns, sizeof(kColumns) / sizeof(kColumns[0])};              \
      return kList;                                                       \
    }                                                                     \
  };                                                                      \
  }                                                                       \
  }

#endif  // UVCPP_ORM_ENABLE && UVCPP_DB_ENABLE
#endif  // SRC_ORM_UVCPP_ORM_MODEL_H
