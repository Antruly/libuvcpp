/**
 * @file src/webapp/uvcpp_web_json_reflect.h
 * @brief JSON 字段表反射的**读入侧**：把一棵解析好的 DOM 填进结构体。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么这一半在 webapp，另一半在核心
 * ----------------------------------
 * 写出侧（`uvcpp/uvcpp_json_reflect.h`）只需要 `uvcpp_json_writer`，那是核心模块、
 * 零依赖。但读入侧的输入是**一棵已经解析好的 DOM**，本仓的 DOM 就是
 * `uvcpp_json`（= nlohmann，`webapp/uvcpp_web_json.h`），而它整个挂在 webapp
 * 模块上、`UVCPP_BUILD_WEBAPP` 默认是 **OFF**。所以两个方向天然分在两层，依赖
 * 方向只有 core ← webapp 一个方向：**这一页 include 那一页，不是反过来**。
 *
 * 于是：
 *   - 只写不读的工程（核心模块）**不会**被本页拖进 nlohmann；
 *   - 要读的工程本来就在用 webapp，它已经带着 nlohmann 了。
 *
 * 形状：一个宏标注，两个方向
 * -------------------------
 *     struct user { std::string name; int age;
 *                   UVCPP_JSON_FIELDS(user, name, age) };
 *
 *     // 读
 *     user u;
 *     const char* bad = nullptr;
 *     json_status st = uvcpp_from_json(dom, u, uvcpp_from_json_options(), &bad);
 *
 * 字段类型与写出侧**一一对应**（bool / 八种整型 / float / double / std::string /
 * 嵌套结构体 / 上面任一种的 std::vector），只有一处**故意不对称**：
 *
 *   - `const char*`（与 `char*`）**能写不能读**。它不是个能持有内存的字段：让
 *     它指向 DOM 里的字符串，DOM 一析构就悬垂，而"指向谁"是使用者的事，不是
 *     库能替他决定的。真去读它**编不过**（`static_assert` 会说要改用
 *     `std::string`），不是运行时返错。
 *
 * 三条读入语义（都是有意的，不是实现细节）
 * ----------------------------------------
 * 1. **没给的成员不动目标里的原值。** 缺字段、值是 `null`，两种都算"没给"。
 *    ⇒ 目标结构体可以先放**默认值**，JSON 里出现的字段覆盖它、没出现的保持
 *    原样。这是"部分更新"的形状，也是最不容易让人踩坑的一种（对比：把缺失当
 *    零值填进去，会把"没设"写成"设成了0"）。
 *    想要"缺一个都不行"就打开 `uvcpp_from_json_options(true, ...)`，会返回
 *    `json_status::MISSING` 并把字段名写进 `where`。
 * 2. **类型不对一律报错，不做隐式转换。** `"age":"30"` 是 `MISMATCH`（不是把
 *    字符串解析成数字），`true` 读进 `int` 也是 `MISMATCH`。唯一放宽的是
 *    **整数读进浮点字段**（JSON 本来就不分 int/double，`{"score":1}` 该收）。
 *    整数到整数**逐宽度查上下界**：`70000` 读进 `short` 是 `MISMATCH`，不是
 *    静默截成 `4464`。
 * 3. **多出来的成员默认忽略**，`uvcpp_from_json_options(..., true)` 打开
 *    `unknown_is_error` 才报 `UNKNOWN`。打开之后每读一次都要拿 DOM 的成员去
 *    字段表里逐个比（O(成员 × 字段)），这是**默认关掉它的原因**。
 *
 * 不抛异常
 * --------
 * nlohmann 的 `get<T>()` 在类型不符时会抛，越界也会抛。本页**先看类型再取值**
 * （`is_boolean()` / `is_number_unsigned()` / `is_string()` …），所以那些抛点
 * 根本到不了。剩下唯一可能的异常是**分配失败**（`std::string` 的赋值、
 * `std::vector` 的 `push_back`），公开入口处接住并返回 `json_status::NO_MEMORY`
 * —— 本仓铁律：异常不得穿透 libuv 的回调。
 *
 * 失败时目标可能被**部分修改**（前几个字段已经写进去了）。这不是疏漏：要"全
 * 成功或全不动"就得先建一份临时对象再搬 —— 那是另一层代价。调用方看到非 OK
 * 就该丢弃 `out`，与写出侧"非 OK 就丢弃产出"是同一条规矩。
 */
#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_JSON_REFLECT_H
#define SRC_WEBAPP_UVCPP_WEB_JSON_REFLECT_H

#include <uvcpp/uvcpp_json_reflect.h>
#include <webapp/uvcpp_web_json.h>

#include <exception>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace uvcpp {

/**
 * @brief 读入（JSON → 结构体）的策略。
 *
 * 默认两项都是 **false**：缺字段不动原值、多出来的成员忽略。两个开关都在
 * `uvcpp_from_json_options` 而不是字段描述器上 —— 这一版**没有** per-field 的
 * required/别名（见文档"没做的"）。
 */
struct uvcpp_from_json_options {
  bool missing_is_error;  ///< true：缺字段（或值为 null）⇒ `json_status::MISSING`
  bool unknown_is_error;  ///< true：出现字段表里没有的成员 ⇒ `json_status::UNKNOWN`

  explicit uvcpp_from_json_options(bool missing = false, bool unknown = false)
      : missing_is_error(missing), unknown_is_error(unknown) {}
};

// 前置声明（公开入口；嵌套结构体要递归回它）。默认实参只写在这里一次。
template <typename T>
json_status uvcpp_from_json(const uvcpp_json& j, T& out,
                            const uvcpp_from_json_options& opts = uvcpp_from_json_options(),
                            const char** where = nullptr);

namespace json_detail {

/// 读入过程中要带着走的两样东西（省得每个函数都多两个参数）。
struct uvcpp_read_ctx {
  const uvcpp_from_json_options* opts;
  const char** where;  ///< 出错时写"最深一层出错的字段名"；调用方可以给 nullptr
};

/// 记下出错位置：**只在还没记的时候记**，于是留下的是最深那一层。
inline void uvcpp_note_where(const uvcpp_read_ctx& ctx, const char* name) {
  if (ctx.where != nullptr && *ctx.where == nullptr) *ctx.where = name;
}

// ---------------------------------------------------------------------
// 标量：先判类型，再取值（不靠 get<T> 的隐式转换，因此没有抛点）
// ---------------------------------------------------------------------

inline bool uvcpp_read_bool(const uvcpp_json& j, bool& out) {
  if (!j.is_boolean()) return false;
  out = j.get<bool>();
  return true;
}

/// 目标是有符号整型时：上下界都要比。
template <typename T>
bool uvcpp_read_signed_into(long long s, T& out, std::true_type) {
  if (s < static_cast<long long>(std::numeric_limits<T>::min())) return false;
  if (s > static_cast<long long>(std::numeric_limits<T>::max())) return false;
  out = static_cast<T>(s);
  return true;
}

/// 目标是**无符号**整型时：负数直接不算（这是"值域检查"里最容易漏的一条）。
template <typename T>
bool uvcpp_read_signed_into(long long s, T& out, std::false_type) {
  if (s < 0) return false;
  if (static_cast<unsigned long long>(s) >
      static_cast<unsigned long long>(std::numeric_limits<T>::max()))
    return false;
  out = static_cast<T>(s);
  return true;
}

/**
 * @brief 整数读进整型字段，**逐宽度查上下界**。
 *
 * nlohmann 解析出来的整数分两个存储类型（无符号 / 有符号），这里按**存储类型**
 * 分两支走，`get<...>()` 取的都是那个类型的精确类型 ⇒ 不会越界抛异常。
 */
template <typename T>
bool uvcpp_read_integer(const uvcpp_json& j, T& out) {
  if (j.is_number_unsigned()) {
    const unsigned long long u = j.get<unsigned long long>();
    // 同一句对两种目标都对：有符号目标的 max 是正值，放进 ull 无损。
    if (u > static_cast<unsigned long long>(std::numeric_limits<T>::max())) return false;
    out = static_cast<T>(u);
    return true;
  }
  if (j.is_number_integer()) {
    const long long s = j.get<long long>();
    return uvcpp_read_signed_into<T>(s, out, std::is_signed<T>());
  }
  return false;
}

/// @brief 浮点字段：浮点直接收；整数也收（JSON 不分 int/double）。
template <typename T>
bool uvcpp_read_float(const uvcpp_json& j, T& out) {
  if (j.is_number_float()) {
    out = static_cast<T>(j.get<double>());
    return true;
  }
  if (j.is_number_unsigned()) {
    out = static_cast<T>(j.get<unsigned long long>());
    return true;
  }
  if (j.is_number_integer()) {
    out = static_cast<T>(j.get<long long>());
    return true;
  }
  return false;
}

inline bool uvcpp_read_string(const uvcpp_json& j, std::string& out) {
  if (!j.is_string()) return false;
  // 类型已经确认过，`get_ref` 不会抛（它的抛点是"类型不符"）。
  out = j.get_ref<const std::string&>();
  return true;
}

// ---------------------------------------------------------------------
// 一个值（按类别分派）
// ---------------------------------------------------------------------

template <typename T>
json_status uvcpp_read_value(const uvcpp_json& j, T& out, std::integral_constant<int, -1>,
                             const uvcpp_read_ctx&) {
  static_assert(sizeof(T) == 0,
                "uvcpp_from_json：这个字段类型没有可读入的形状。支持 bool / 整型 / 浮点 / "
                "std::string / 标了 UVCPP_JSON_FIELDS 的结构体，以及上面任一种的 std::vector；"
                "C 数组、std::map、std::optional、枚举、其它指针都不在这一版的范围里。");
  return json_status::MISMATCH;
}

template <typename T>
json_status uvcpp_read_value(const uvcpp_json& j, T& out, std::integral_constant<int, 0>,
                             const uvcpp_read_ctx&) {
  return uvcpp_read_bool(j, out) ? json_status::OK : json_status::MISMATCH;
}

template <typename T>
json_status uvcpp_read_value(const uvcpp_json& j, T& out, std::integral_constant<int, 1>,
                             const uvcpp_read_ctx&) {
  return uvcpp_read_integer(j, out) ? json_status::OK : json_status::MISMATCH;
}

template <typename T>
json_status uvcpp_read_value(const uvcpp_json& j, T& out, std::integral_constant<int, 2>,
                             const uvcpp_read_ctx&) {
  return uvcpp_read_integer(j, out) ? json_status::OK : json_status::MISMATCH;
}

template <typename T>
json_status uvcpp_read_value(const uvcpp_json& j, T& out, std::integral_constant<int, 3>,
                             const uvcpp_read_ctx&) {
  return uvcpp_read_float(j, out) ? json_status::OK : json_status::MISMATCH;
}

/// `std::string` 能读；C 字符串指针**不能**（见文件头）。
template <typename T>
json_status uvcpp_read_string_like(const uvcpp_json& j, T& out, std::true_type,
                                   const uvcpp_read_ctx&) {
  return uvcpp_read_string(j, out) ? json_status::OK : json_status::MISMATCH;
}

template <typename T>
json_status uvcpp_read_string_like(const uvcpp_json&, T&, std::false_type, const uvcpp_read_ctx&) {
  static_assert(sizeof(T) == 0,
                "uvcpp_from_json：`const char*` / `char*` 字段读不进来 —— 它不持有内存，"
                "指向 DOM 里的字符串会在 DOM 析构后悬垂。把那个字段改成 std::string。");
  return json_status::MISMATCH;
}

template <typename T>
json_status uvcpp_read_value(const uvcpp_json& j, T& out, std::integral_constant<int, 4>,
                             const uvcpp_read_ctx& ctx) {
  typedef typename uvcpp_plain<T>::type P;
  return uvcpp_read_string_like(j, out, std::is_same<P, std::string>(), ctx);
}

template <typename T>
json_status uvcpp_read_value(const uvcpp_json& j, T& out, std::integral_constant<int, 5>,
                             const uvcpp_read_ctx& ctx) {
  if (!j.is_object()) return json_status::MISMATCH;
  // 递归回公开入口（那一层还有一遍 static_assert 与 try/catch）。
  return uvcpp_from_json(j, out, *ctx.opts, ctx.where);
}

template <typename T>
json_status uvcpp_read_value(const uvcpp_json& j, T& out, std::integral_constant<int, 6>,
                             const uvcpp_read_ctx& ctx) {
  if (!j.is_array()) return json_status::MISMATCH;
  typedef typename T::value_type element_type;
  out.clear();
  for (uvcpp_json::const_iterator it = j.begin(); it != j.end(); ++it) {
    element_type element = element_type();
    const json_status st = uvcpp_read_value(
        *it, element, std::integral_constant<int, uvcpp_json_kind<element_type>::value>(), ctx);
    if (st != json_status::OK) return st;
    out.push_back(element);  // 这里可能 bad_alloc ⇒ 公开入口处接住
  }
  return json_status::OK;
}

/// @brief 一个值：先算类别，再分派。
template <typename T>
json_status uvcpp_read_value(const uvcpp_json& j, T& out, const uvcpp_read_ctx& ctx) {
  typedef typename uvcpp_plain<T>::type P;
  return uvcpp_read_value(j, out, std::integral_constant<int, uvcpp_json_kind<P>::value>(), ctx);
}

// ---------------------------------------------------------------------
// 一个字段：定位 → 判"给没给" → 转换
// ---------------------------------------------------------------------

/**
 * @brief 读一个字段。第一个参数是**上一步的结果**，非 OK 就原地返回（与写出侧
 * 同一条粘性规矩：错一次就停，不必层层检查）。
 */
template <typename Class, typename FieldType>
json_status uvcpp_read_field(json_status st, const uvcpp_json& j, Class& obj,
                             const uvcpp_json_field<Class, FieldType>& f,
                             const uvcpp_read_ctx& ctx) {
  if (st != json_status::OK) return st;

  const uvcpp_json::const_iterator it = j.find(f.name);
  // "没这个成员"与"值是 null"都算**没给**：不动目标里的原值。
  if (it == j.end() || it->is_null()) {
    if (ctx.opts->missing_is_error) {
      uvcpp_note_where(ctx, f.name);
      return json_status::MISSING;
    }
    return json_status::OK;
  }

  st = uvcpp_read_value(*it, obj.*(f.pointer), ctx);
  if (st != json_status::OK) uvcpp_note_where(ctx, f.name);
  return st;
}

template <typename Class, typename Tuple, std::size_t... I>
json_status uvcpp_read_fields(Class& obj, const uvcpp_json& j, const Tuple& fields,
                              const uvcpp_read_ctx& ctx, uvcpp_index_sequence<I...>) {
  json_status st = json_status::OK;
  int swallow[] = {0, (st = uvcpp_read_field(st, j, obj, std::get<I>(fields), ctx), 0)...};
  (void)swallow;
  return st;
}

/// 字段表里有没有这个名字（只在 `unknown_is_error` 打开时才付这个代价）。
template <typename Tuple, std::size_t... I>
bool uvcpp_fields_contain(const Tuple& fields, const std::string& name, uvcpp_index_sequence<I...>) {
  bool hit = false;
  int swallow[] = {0, (hit = hit || (name == std::get<I>(fields).name), 0)...};
  (void)swallow;
  return hit;
}

}  // namespace json_detail

/**
 * @brief 把一棵 JSON DOM 读进一个标了 `UVCPP_JSON_FIELDS` 的结构体。
 *
 * @param j     已经解析好的 DOM（要是个**对象**）
 * @param out   目标；**成功与失败都可能被改了一部分**（见文件头）
 * @param opts  缺字段 / 多成员算不算错
 * @param where 非 nullptr 时，出错会写入**最深一层出错的字段名**（静态字符串，
 *              来自宏里的 `#字段`；成员表里没有的名字则指向 DOM 里的那个键，
 *              活到 `j` 还在为止）
 * @return `OK` / `MISMATCH`（类型或值域不符、根不是对象）/ `MISSING` /
 *         `UNKNOWN` / `NO_MEMORY`
 *
 * 字段类型不支持、或类型没有字段表时都是**编译期**报错（`static_assert`）。
 */
template <typename T>
json_status uvcpp_from_json(const uvcpp_json& j, T& out, const uvcpp_from_json_options& opts,
                            const char** where) {
  typedef typename json_detail::uvcpp_plain<T>::type plain;
  static_assert(json_detail::uvcpp_has_json_fields<plain>::value,
                "uvcpp_from_json：这个类型没有 `UVCPP_JSON_FIELDS(Type, ...)` 字段表。");
  return uvcpp_from_json(
      j, out, opts, where,
      std::integral_constant<bool, json_detail::uvcpp_has_json_fields<plain>::value>());
}

/// @brief 有字段表的那一支：真读。
template <typename T>
json_status uvcpp_from_json(const uvcpp_json& j, T& out, const uvcpp_from_json_options& opts,
                            const char** where, std::true_type) {
  if (where != nullptr) *where = nullptr;
  if (!j.is_object()) return json_status::MISMATCH;

  try {
    json_detail::uvcpp_read_ctx ctx;
    ctx.opts = &opts;
    ctx.where = where;

    typedef typename json_detail::uvcpp_plain<T>::type plain;
    const typename json_detail::uvcpp_plain<decltype(plain::uvcppJsonFields())>::type& fields =
        plain::uvcppJsonFields();
    typedef typename json_detail::uvcpp_plain<decltype(fields)>::type tuple_type;

    json_status st = json_detail::uvcpp_read_fields(
        out, j, fields, ctx,
        json_detail::uvcpp_make_index_seq<std::tuple_size<tuple_type>::value>());
    if (st != json_status::OK) return st;

    if (opts.unknown_is_error) {
      // 反向查一遍：DOM 里有、字段表里没有的成员。默认关掉（这一步是 O(成员×字段)）。
      for (uvcpp_json::const_iterator it = j.begin(); it != j.end(); ++it) {
        if (!json_detail::uvcpp_fields_contain(
                fields, it.key(),
                json_detail::uvcpp_make_index_seq<std::tuple_size<tuple_type>::value>())) {
          if (where != nullptr) *where = it.key().c_str();
          return json_status::UNKNOWN;
        }
      }
    }
    return json_status::OK;
  } catch (const std::bad_alloc&) {
    return json_status::NO_MEMORY;
  } catch (const std::exception&) {
    // 到不了这里：类型判断都发生在 get 之前（见文件头）。真到了说明 nlohmann
    // 那侧的行为与我们读到的类型不一致 —— 报 MISMATCH，绝不把异常放出去。
    return json_status::MISMATCH;
  }
}

/// @brief 没有字段表的那一支：到不了（上面那句 `static_assert` 先拦下了）。
template <typename T>
json_status uvcpp_from_json(const uvcpp_json&, T&, const uvcpp_from_json_options&, const char**,
                            std::false_type) {
  return json_status::MISMATCH;
}

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_JSON_REFLECT_H
