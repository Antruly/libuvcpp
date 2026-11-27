/**
 * @file tests/functional/orm_model_func.cpp
 * @brief ORM 模型元数据：`nullable<T>`、类型映射、声明宏与 `validate_model`。
 *
 * 这条用例**不连接数据库、不需要任何客户端库**：它只编译/运行 `src/orm/`
 * 这一层。文件名带 `orm_` 前缀，`tests/functional/CMakeLists.txt` 在
 * `UVCPP_BUILD_ORM=OFF` 时会把整个目标摘掉 —— “没测”表现为用例不存在，
 * 不是又一个恒绿的 SKIP。
 *
 * 覆盖面按 issue #89 的验收标准拆：
 *
 *   1. `nullable<T>` 的空/有值/空串/拷贝/移动/`value_or`/`reset`；
 *   2. 所有 ORM 类型到 `uvcpp_db_type` 的映射，特别是
 *      `nullable<std::string>`（NULL）与 `std::string`（空串仍是 TEXT）的
 *      区别；
 *   3. 用 `UVCPP_ORM_MODEL` / `UVCPP_ORM_FIELD` / `UVCPP_ORM_MODEL_END`
 *      声明一个包含所有类型的模型，核对顺序、列名、偏移、大小、标志、主键；
 *   4. `validate_model<T>()` 的每一类失败：无主键、双主键、重复列、空表名、
 *      空列名、自增非整数、未知类型、nullable + not_null 冲突。
 */

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <orm/uvcpp_orm_model.h>

// ---------------------------------------------------------------------------
// 测试模型：所有支持的字段类型 + 一个单列自增主键
// ---------------------------------------------------------------------------

struct all_types {
  std::int64_t id;
  bool flag;
  std::int8_t i8;
  std::uint8_t u8;
  std::int16_t i16;
  std::uint16_t u16;
  std::int32_t i32;
  std::uint32_t u32;
  std::uint64_t u64;
  float f32;
  double f64;
  std::string text;
  std::vector<std::uint8_t> blob;
  uvcpp::nullable<std::int32_t> opt_i32;
  uvcpp::nullable<std::string> opt_text;
};

UVCPP_ORM_MODEL(all_types, "all_types")
UVCPP_ORM_FIELD(all_types, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(all_types, flag, "flag", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(all_types, i8, "i8", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types, u8, "u8", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types, i16, "i16", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types, u16, "u16", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types, i32, "i32", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types, u32, "u32", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types, u64, "u64", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types, f32, "f32", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types, f64, "f64", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types, text, "text", UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(all_types, blob, "blob", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types, opt_i32, "opt_i32", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(all_types, opt_text, "opt_text", UVCPP_ORM_NONE)
UVCPP_ORM_MODEL_END()

// ---------------------------------------------------------------------------
// 校验失败用的模型：每个模型只制造一个错误
// ---------------------------------------------------------------------------

struct no_primary_key_model {
  std::int32_t a;
};

UVCPP_ORM_MODEL(no_primary_key_model, "no_primary_key")
UVCPP_ORM_FIELD(no_primary_key_model, a, "a", UVCPP_ORM_NONE)
UVCPP_ORM_MODEL_END()

struct two_primary_keys_model {
  std::int32_t id;
  std::int32_t alt;
};

UVCPP_ORM_MODEL(two_primary_keys_model, "two_primary_keys")
UVCPP_ORM_FIELD(two_primary_keys_model, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_FIELD(two_primary_keys_model, alt, "alt", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_MODEL_END()

struct duplicate_column_model {
  std::int32_t id;
  std::string name;
  std::string alias;
};

UVCPP_ORM_MODEL(duplicate_column_model, "duplicate_column")
UVCPP_ORM_FIELD(duplicate_column_model, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_FIELD(duplicate_column_model, name, "dup", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(duplicate_column_model, alias, "DUP", UVCPP_ORM_NONE)
UVCPP_ORM_MODEL_END()

struct empty_table_name_model {
  std::int32_t id;
};

UVCPP_ORM_MODEL(empty_table_name_model, "")
UVCPP_ORM_FIELD(empty_table_name_model, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_MODEL_END()

struct empty_column_name_model {
  std::int32_t id;
};

UVCPP_ORM_MODEL(empty_column_name_model, "empty_column_name")
UVCPP_ORM_FIELD(empty_column_name_model, id, "", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_MODEL_END()

struct control_table_name_model {
  std::int32_t id;
};

UVCPP_ORM_MODEL(control_table_name_model, "bad\ttable")
UVCPP_ORM_FIELD(control_table_name_model, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_MODEL_END()

struct control_column_name_model {
  std::int32_t id;
};

UVCPP_ORM_MODEL(control_column_name_model, "control_column_name")
UVCPP_ORM_FIELD(control_column_name_model, id, "bad\ncolumn",
                UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_MODEL_END()

struct auto_increment_double_model {
  double id;
};

UVCPP_ORM_MODEL(auto_increment_double_model, "auto_increment_double")
UVCPP_ORM_FIELD(auto_increment_double_model, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_MODEL_END()

struct auto_increment_non_pk_model {
  std::int32_t id;
  std::int32_t seq;
};

UVCPP_ORM_MODEL(auto_increment_non_pk_model, "auto_increment_non_pk")
UVCPP_ORM_FIELD(auto_increment_non_pk_model, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_FIELD(auto_increment_non_pk_model, seq, "seq",
                UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_MODEL_END()

struct unsupported_type_model {
  std::int32_t id;
  char letter;
};

UVCPP_ORM_MODEL(unsupported_type_model, "unsupported_type")
UVCPP_ORM_FIELD(unsupported_type_model, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_FIELD(unsupported_type_model, letter, "letter", UVCPP_ORM_NONE)
UVCPP_ORM_MODEL_END()

struct nullable_not_null_model {
  std::int32_t id;
  uvcpp::nullable<std::int32_t> age;
};

UVCPP_ORM_MODEL(nullable_not_null_model, "nullable_not_null")
UVCPP_ORM_FIELD(nullable_not_null_model, id, "id", UVCPP_ORM_PRIMARY_KEY)
UVCPP_ORM_FIELD(nullable_not_null_model, age, "age", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& what) {
  ++checks;
  if (condition) return;
  ++failures;
  std::cerr << "[FAIL] " << what << std::endl;
}

void check_status(uvcpp::uvcpp_orm_status got, uvcpp::uvcpp_orm_status want,
                  const std::string& what) {
  check(got == want, what + "（期望 " + uvcpp::uvcpp_orm_status_name(want) +
                         "，实得 " + uvcpp::uvcpp_orm_status_name(got) + "）");
}

void check_type(uvcpp::uvcpp_orm_type got, uvcpp::uvcpp_orm_type want,
                const std::string& what) {
  check(got == want, what + "（期望 " + uvcpp::uvcpp_orm_type_name(want) +
                         "，实得 " + uvcpp::uvcpp_orm_type_name(got) + "）");
}

/// 校验一个模型必须失败，并且错误串里出现 `fragment`（空串表示不检查）。
template <typename T>
void expect_invalid(uvcpp::uvcpp_orm_status want, const std::string& fragment,
                    const std::string& what) {
  const uvcpp::uvcpp_orm_validation_result result =
      uvcpp::validate_model<T>();
  check(!result.ok(), what + "：必须被判为非法");
  check_status(result.status, want, what + "：返回码");
  if (!fragment.empty()) {
    check(result.error.find(fragment) != std::string::npos,
          what + "：错误串要提到 " + fragment + "（实得 [" + result.error +
              "]）");
  }
}

// ---------------------------------------------------------------------------
// 1. nullable<T>
// ---------------------------------------------------------------------------

void test_nullable() {
  uvcpp::nullable<std::int32_t> empty;
  check(!empty.has_value(), "nullable 默认是空");
  check(empty.value() == 0, "空 nullable<int> 的 value() 返回 0，不抛");
  check(empty.value_or(42) == 42, "空 nullable 的 value_or 返回兜底值");
  check(!static_cast<bool>(empty), "空 nullable 的 operator bool 为 false");

  empty = std::int32_t(7);
  check(empty.has_value(), "赋值后 nullable 有值");
  check(empty.value() == 7, "赋值后的值正确");
  check(empty.value_or(42) == 7, "有值时 value_or 返回内部值");
  check(static_cast<bool>(empty), "有值时 operator bool 为 true");

  empty.reset();
  check(!empty.has_value() && empty.value() == 0,
        "reset 后回到空态且 value() 回到默认值");

  // 空串是**有值**：与 NULL 的区分是这一层的核心契约。
  uvcpp::nullable<std::string> empty_text(std::string(""));
  check(empty_text.has_value(), "nullable<string> 装空串时 has_value() 为 true");
  check(empty_text.value().empty(), "nullable<string> 装空串时 value() 是空串");
  check(empty_text.value_or("fallback").empty(),
        "空串是有效值，value_or 不应该用兜底值替换它");

  uvcpp::nullable<std::string> text(std::string("hello"));
  check(text.has_value() && text.value() == "hello", "nullable<string> 有值");

  // 拷贝是值语义：改副本不影响原件。
  uvcpp::nullable<std::string> copy = text;
  copy.value() = "world";
  check(text.value() == "hello" && copy.value() == "world",
        "nullable 拷贝后两份互不影响");

  // 移动构造：目标拿到值。
  uvcpp::nullable<std::string> moved(std::move(text));
  check(moved.has_value() && moved.value() == "hello",
        "nullable 移动构造后目标仍有值");
  check(moved.value().size() == 5, "nullable 移动构造后目标内容完整");

  // 拷贝/移动赋值走的是 nullable 自身那两份隐式赋值运算符，不是 `operator=(T)`。
  uvcpp::nullable<std::int32_t> assigned_to;
  const uvcpp::nullable<std::int32_t> assigned_from(9);
  assigned_to = assigned_from;
  check(assigned_to.has_value() && assigned_to.value() == 9,
        "nullable 拷贝赋值保留有值状态");

  uvcpp::nullable<std::string> move_assigned_to;
  move_assigned_to = std::move(moved);
  check(move_assigned_to.has_value() && move_assigned_to.value() == "hello",
        "nullable 移动赋值保留有值状态");
}

// ---------------------------------------------------------------------------
// 2. 类型映射
// ---------------------------------------------------------------------------

void test_type_mapping() {
  using uvcpp::uvcpp_orm_type;
  using uvcpp::uvcpp_orm_type_traits;

  check_type(uvcpp_orm_type_traits<bool>::type(), uvcpp_orm_type::BOOL,
             "bool → BOOL");
  check_type(uvcpp_orm_type_traits<std::int8_t>::type(), uvcpp_orm_type::INT8,
             "int8 → INT8");
  check_type(uvcpp_orm_type_traits<std::uint8_t>::type(), uvcpp_orm_type::UINT8,
             "uint8 → UINT8");
  check_type(uvcpp_orm_type_traits<std::int16_t>::type(), uvcpp_orm_type::INT16,
             "int16 → INT16");
  check_type(uvcpp_orm_type_traits<std::uint16_t>::type(),
             uvcpp_orm_type::UINT16, "uint16 → UINT16");
  check_type(uvcpp_orm_type_traits<std::int32_t>::type(), uvcpp_orm_type::INT32,
             "int32 → INT32");
  check_type(uvcpp_orm_type_traits<std::uint32_t>::type(),
             uvcpp_orm_type::UINT32, "uint32 → UINT32");
  check_type(uvcpp_orm_type_traits<std::int64_t>::type(), uvcpp_orm_type::INT64,
             "int64 → INT64");
  check_type(uvcpp_orm_type_traits<std::uint64_t>::type(),
             uvcpp_orm_type::UINT64, "uint64 → UINT64");
  check_type(uvcpp_orm_type_traits<float>::type(), uvcpp_orm_type::FLOAT,
             "float → FLOAT");
  check_type(uvcpp_orm_type_traits<double>::type(), uvcpp_orm_type::DOUBLE,
             "double → DOUBLE");
  check_type(uvcpp_orm_type_traits<std::string>::type(), uvcpp_orm_type::TEXT,
             "std::string → TEXT（不是 NULL）");
  check_type(uvcpp_orm_type_traits<std::vector<std::uint8_t> >::type(),
             uvcpp_orm_type::BLOB, "vector<uint8_t> → BLOB");
  check_type(uvcpp_orm_type_traits<char>::type(), uvcpp_orm_type::UNKNOWN,
             "char 没有映射");

  check_type(uvcpp_orm_type_traits<uvcpp::nullable<std::string> >::type(),
             uvcpp_orm_type::TEXT, "nullable<string> 仍是 TEXT");
  check(uvcpp_orm_type_traits<std::string>::is_nullable() == false,
        "std::string 本身不是 nullable");
  check(uvcpp_orm_type_traits<uvcpp::nullable<std::string> >::is_nullable(),
        "nullable<string> 是可空类型");
  check(uvcpp_orm_type_traits<uvcpp::nullable<std::int32_t> >::is_integer(),
        "nullable<int32> 的底层类型是整数");
  check(uvcpp_orm_type_traits<bool>::is_integer() == false,
        "bool 不算整数（不能自增）");

  // ORM 类型 → 数据库值标签。
  check(uvcpp::uvcpp_orm_db_type(uvcpp_orm_type::BOOL) ==
            uvcpp::uvcpp_db_type::BOOL,
        "BOOL → db BOOL");
  check(uvcpp::uvcpp_orm_db_type(uvcpp_orm_type::INT8) ==
            uvcpp::uvcpp_db_type::INT64,
        "INT8 → db INT64");
  check(uvcpp::uvcpp_orm_db_type(uvcpp_orm_type::UINT64) ==
            uvcpp::uvcpp_db_type::UINT64,
        "UINT64 → db UINT64");
  check(uvcpp::uvcpp_orm_db_type(uvcpp_orm_type::FLOAT) ==
            uvcpp::uvcpp_db_type::DOUBLE,
        "FLOAT → db DOUBLE");
  check(uvcpp::uvcpp_orm_db_type(uvcpp_orm_type::TEXT) ==
            uvcpp::uvcpp_db_type::TEXT,
        "TEXT → db TEXT");
  check(uvcpp::uvcpp_orm_db_type(uvcpp_orm_type::BLOB) ==
            uvcpp::uvcpp_db_type::BLOB,
        "BLOB → db BLOB");
  check(uvcpp::uvcpp_orm_db_type(uvcpp_orm_type::UNKNOWN) ==
            uvcpp::uvcpp_db_type::NIL,
        "UNKNOWN → db NIL（安全网；校验会先拒绝它）");

  // 值转换：空串与 NULL 必须能分开。
  const uvcpp::uvcpp_db_value empty_text =
      uvcpp::uvcpp_orm_to_db_value(std::string(""));
  check(!empty_text.is_null(), "空 std::string 转成非 NULL 的值");
  check(empty_text.type() == uvcpp::uvcpp_db_type::TEXT,
        "空 std::string 的类型标签是 TEXT");

  const uvcpp::uvcpp_db_value null_text =
      uvcpp::uvcpp_orm_to_db_value(uvcpp::nullable<std::string>());
  check(null_text.is_null(), "空 nullable<string> 转成 NULL");

  const uvcpp::uvcpp_db_value valued_empty_text =
      uvcpp::uvcpp_orm_to_db_value(
          uvcpp::nullable<std::string>(std::string("")));
  check(!valued_empty_text.is_null(),
        "nullable<string> 装空串时转成非 NULL");
  check(valued_empty_text.type() == uvcpp::uvcpp_db_type::TEXT,
        "nullable<string> 装空串时类型标签是 TEXT");

  uvcpp::nullable<std::string> round_trip;
  check(uvcpp::uvcpp_orm_from_db_value(uvcpp::uvcpp_db_value(std::string("")),
                                       &round_trip),
        "空 TEXT 能转回 nullable<string>");
  check(round_trip.has_value() && round_trip.value().empty(),
        "空 TEXT 转回后是“有值的空串”，不是 NULL");
  check(uvcpp::uvcpp_orm_from_db_value(uvcpp::uvcpp_db_value(), &round_trip),
        "NULL 能转回 nullable<string>");
  check(!round_trip.has_value(), "NULL 转回后是空态");

  std::string plain;
  check(!uvcpp::uvcpp_orm_from_db_value(uvcpp::uvcpp_db_value(), &plain),
        "NULL 转给非空 std::string 失败（不静默变成空串）");

  const std::vector<std::uint8_t> bytes = {0u, 1u, 255u};
  const uvcpp::uvcpp_db_value blob = uvcpp::uvcpp_orm_to_db_value(bytes);
  check(blob.type() == uvcpp::uvcpp_db_type::BLOB && blob.size() == 3u,
        "vector<uint8_t> → BLOB，字节数正确");
  std::vector<std::uint8_t> blob_back;
  check(uvcpp::uvcpp_orm_from_db_value(blob, &blob_back) && blob_back == bytes,
        "BLOB 往返逐字节相等");

  const uvcpp::uvcpp_db_value number =
      uvcpp::uvcpp_orm_to_db_value(static_cast<std::int32_t>(-7));
  check(number.type() == uvcpp::uvcpp_db_type::INT64 &&
            number.to_int64() == -7,
        "int32 转成 INT64 值");
}

// ---------------------------------------------------------------------------
// 3. 模型元数据
// ---------------------------------------------------------------------------

void test_model_metadata() {
  using uvcpp::column_flag;
  using uvcpp::column_flags;
  using uvcpp::uvcpp_orm_column;
  using uvcpp::uvcpp_orm_type;
  using uvcpp::model_traits;

  // 标志是强类型位集：`|` 可以链式组合，`has()` 逐位判断。
  const column_flags combined = column_flag::primary_key |
                                column_flag::auto_increment |
                                column_flag::unique;
  check(combined.has(column_flag::primary_key) &&
            combined.has(column_flag::auto_increment) &&
            combined.has(column_flag::unique),
        "column_flags 能组合并逐位判断");
  check(!combined.has(column_flag::not_null), "没有组合进来的标志为 false");

  check(std::string(model_traits<all_types>::table_name()) == "all_types",
        "表名");
  check(model_traits<all_types>::column_count() == 15u, "字段数量");

  const uvcpp_orm_column* cols = model_traits<all_types>::columns();
  check(cols != nullptr, "columns() 不是空指针");

  const char* expected_fields[] = {
      "id",    "flag", "i8",  "u8",    "i16",     "u16",     "i32", "u32",
      "u64",   "f32",  "f64", "text",  "blob",    "opt_i32", "opt_text"};
  const char* expected_columns[] = {
      "id",    "flag", "i8",  "u8",    "i16",     "u16",     "i32", "u32",
      "u64",   "f32",  "f64", "text",  "blob",    "opt_i32", "opt_text"};
  const uvcpp_orm_type expected_types[] = {
      uvcpp_orm_type::INT64, uvcpp_orm_type::BOOL,   uvcpp_orm_type::INT8,
      uvcpp_orm_type::UINT8, uvcpp_orm_type::INT16,  uvcpp_orm_type::UINT16,
      uvcpp_orm_type::INT32, uvcpp_orm_type::UINT32, uvcpp_orm_type::UINT64,
      uvcpp_orm_type::FLOAT, uvcpp_orm_type::DOUBLE, uvcpp_orm_type::TEXT,
      uvcpp_orm_type::BLOB,  uvcpp_orm_type::INT32,  uvcpp_orm_type::TEXT};
  const std::size_t expected_sizes[] = {
      sizeof(std::int64_t),
      sizeof(bool),
      sizeof(std::int8_t),
      sizeof(std::uint8_t),
      sizeof(std::int16_t),
      sizeof(std::uint16_t),
      sizeof(std::int32_t),
      sizeof(std::uint32_t),
      sizeof(std::uint64_t),
      sizeof(float),
      sizeof(double),
      sizeof(std::string),
      sizeof(std::vector<std::uint8_t>),
      sizeof(uvcpp::nullable<std::int32_t>),
      sizeof(uvcpp::nullable<std::string>)};
  const std::size_t expected_offsets[] = {
      offsetof(all_types, id),      offsetof(all_types, flag),
      offsetof(all_types, i8),      offsetof(all_types, u8),
      offsetof(all_types, i16),     offsetof(all_types, u16),
      offsetof(all_types, i32),     offsetof(all_types, u32),
      offsetof(all_types, u64),     offsetof(all_types, f32),
      offsetof(all_types, f64),     offsetof(all_types, text),
      offsetof(all_types, blob),    offsetof(all_types, opt_i32),
      offsetof(all_types, opt_text)};

  const std::size_t expected_count =
      sizeof(expected_fields) / sizeof(expected_fields[0]);
  check(model_traits<all_types>::column_count() == expected_count,
        "字段数量与期望数组一致");

  for (std::size_t i = 0; i < expected_count; ++i) {
    const std::string prefix = "字段[" + std::to_string(i) + "]";
    check(std::string(cols[i].field_name()) == expected_fields[i],
          prefix + " 字段名与声明顺序一致");
    check(std::string(cols[i].column_name()) == expected_columns[i],
          prefix + " 列名");
    check(cols[i].type() == expected_types[i], prefix + " 类型标签");
    check(cols[i].size() == expected_sizes[i], prefix + " sizeof");
    check(cols[i].offset() == expected_offsets[i], prefix + " offsetof");
  }

  // 标志
  check(cols[0].is_primary_key(), "id 是主键");
  check(cols[0].is_auto_increment(), "id 是自增");
  check(!cols[0].is_nullable(), "id 不是 nullable");
  check(!cols[1].is_nullable() && cols[1].is_not_null(),
        "flag 是 not_null 且不是 nullable");
  check(cols[11].is_not_null() && cols[11].is_unique(),
        "text 是 not_null + unique");
  check(cols[13].is_nullable() && !cols[13].is_not_null(),
        "opt_i32 自动带 nullable 标志，且没有 not_null");
  check(cols[14].is_nullable() && !cols[14].is_not_null(),
        "opt_text 自动带 nullable 标志，且没有 not_null");

  // 主键与查找路径
  const uvcpp_orm_column* primary = model_traits<all_types>::primary_key();
  check(primary != nullptr, "primary_key() 找到主键");
  check(primary != nullptr && std::string(primary->field_name()) == "id",
        "primary_key() 指向 id");

  const uvcpp_orm_column* by_column =
      model_traits<all_types>::column_by_name("text");
  check(by_column != nullptr && std::string(by_column->field_name()) == "text",
        "按列名找到字段");

  const uvcpp_orm_column* by_field =
      model_traits<all_types>::column_by_field_name("opt_text");
  check(by_field != nullptr &&
            std::string(by_field->column_name()) == "opt_text",
        "按字段名找到字段");

  check(model_traits<all_types>::column_by_name("missing") == nullptr,
        "不存在的列名返回空指针");
  check(model_traits<all_types>::column_index("blob") == 12u,
        "按列名取下标");
  check(model_traits<all_types>::column_index("missing") ==
            model_traits<all_types>::column_count(),
        "不存在的列名返回 column_count()");

  const uvcpp_orm_column* by_member =
      uvcpp::column_for(&all_types::opt_text);
  check(by_member != nullptr &&
            std::string(by_member->field_name()) == "opt_text",
        "按成员指针找到字段");
  check(uvcpp::column_for(&all_types::id) == primary,
        "按成员指针找到的主键与 primary_key() 一致");
}

// ---------------------------------------------------------------------------
// 4. 校验
// ---------------------------------------------------------------------------

void test_validation() {
  using uvcpp::uvcpp_orm_status;
  using uvcpp::validate_model;

  std::string error;
  check_status(validate_model<all_types>(&error), uvcpp_orm_status::OK,
               "完整模型校验通过");
  check(error.empty(), "成功时 error 为空");
  const uvcpp::uvcpp_orm_validation_result result =
      validate_model<all_types>();
  check(result.ok() && result.error.empty(), "结果结构区分 ok");

  // 没有声明模型的类型
  check_status(validate_model<int>(&error), uvcpp_orm_status::NO_MODEL,
               "没有模型的类型被拒绝");

  expect_invalid<no_primary_key_model>(uvcpp_orm_status::NO_PRIMARY_KEY,
                                       "no primary key", "无主键");
  expect_invalid<two_primary_keys_model>(
      uvcpp_orm_status::MULTIPLE_PRIMARY_KEYS, "id", "双主键");
  expect_invalid<duplicate_column_model>(uvcpp_orm_status::DUPLICATE_COLUMN,
                                         "alias", "重复列名（大小写不敏感）");
  expect_invalid<empty_table_name_model>(uvcpp_orm_status::EMPTY_TABLE_NAME,
                                         "table name", "空表名");
  expect_invalid<empty_column_name_model>(uvcpp_orm_status::EMPTY_COLUMN_NAME,
                                          "id", "空列名");
  expect_invalid<control_table_name_model>(
      uvcpp_orm_status::INVALID_TABLE_NAME, "control character", "表名控制字符");
  expect_invalid<control_column_name_model>(
      uvcpp_orm_status::INVALID_COLUMN_NAME, "id", "列名控制字符");
  expect_invalid<auto_increment_double_model>(
      uvcpp_orm_status::INVALID_AUTO_INCREMENT, "id", "自增字段不是整数");
  expect_invalid<auto_increment_non_pk_model>(
      uvcpp_orm_status::INVALID_AUTO_INCREMENT, "seq", "自增字段不是主键");
  expect_invalid<unsupported_type_model>(uvcpp_orm_status::UNSUPPORTED_TYPE,
                                         "letter", "未知类型");
  expect_invalid<nullable_not_null_model>(uvcpp_orm_status::CONFLICTING_FLAGS,
                                          "age", "nullable 与 not_null 冲突");

  // 返回码短名是稳定值，测试可以断言。
  check(std::string(uvcpp::uvcpp_orm_status_name(
            uvcpp_orm_status::DUPLICATE_COLUMN)) == "duplicate_column",
        "状态短名稳定");
}

}  // namespace

int main() {
  std::cout << "[orm_model] start" << std::endl;

  test_nullable();
  test_type_mapping();
  test_model_metadata();
  test_validation();

  std::cout << "[orm_model] checks=" << checks << " failures=" << failures
            << std::endl;
  if (failures != 0) {
    std::cerr << "[FAIL] orm_model: " << failures << " check(s) failed"
              << std::endl;
    return 1;
  }
  std::cout << "[PASS] orm_model: all " << checks << " checks passed"
            << std::endl;
  return 0;
}
