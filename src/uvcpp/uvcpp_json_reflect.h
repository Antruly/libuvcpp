/**
 * @file src/uvcpp/uvcpp_json_reflect.h
 * @brief JSON 字段表（C++11 宏版反射）的**序列化侧**：把一个结构体的字段表变成 JSON。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它解决什么
 * ----------
 * `uvcpp_json_writer`（同目录）把"写 JSON 的语法"包住了（转义、逗号、括号配对），
 * 但每一层结构还是得手写：
 *
 *     w.object_begin();
 *     w.member("name", user.name);
 *     w.member("age", user.age);
 *     w.object_end();
 *
 * 字段表一变（加一个成员），这段代码就得跟着变 —— 而且**漏一个字段是静默的**，
 * 编译器不会提醒。本模块把那几句话换成一次标注：
 *
 *     struct user {
 *       std::string name;
 *       int age;
 *       UVCPP_JSON_FIELDS(user, name, age)
 *     };
 *
 *     uvcpp_json_writer w;
 *     uvcpp_to_json(one_user, w);
 *
 * 于是"字段表"只有一处，序列化按它走。
 *
 * 为什么是宏，不是 `auto` 那套
 * ----------------------------
 * 本仓是 **C++11**（`CMakeLists.txt:90`，且没有 per-target 覆盖）。语言级反射
 * （P2996）要到 **C++26**，`std::void_t` 是 C++17、`std::index_sequence` 与
 * `if constexpr` 是 C++17 —— 一句话，C++11 里能表达"列出成员"的只有**宏**。
 * 所以这是一个**要使用者显式列字段**的宏：它**不是**自动反射，是"一次列清、
 * 处处按这张表走"。这个边界写在文档里，别让人以为它能自己发现字段（它做不到，
 * 也不需要：字段表就在宏的参数里）。
 *
 * 形状借自 hical（`src/core/MetaJson.h`、`src/core/Reflection.h`）的 C++20 回退
 * 路线：**字段描述器 + 值元组 + 按下标展开**。三处必须改掉：
 *
 *   1. hical 用 `std::make_index_sequence` 与折叠表达式；C++11 两者都没有，
 *      这里**手写一个 `uvcpp_index_sequence`**（本模块唯一的技术活），并用
 *      "花括号初始化列表的求值顺序有标准保证"替代折叠表达式。
 *   2. hical 的序列化目标是 `boost::json::object`（**先建树**）；这里直写
 *      `uvcpp_json_writer`，**没有树、没有中间表示**。
 *   3. hical 的错误是异常（`MetaJsonError.h`）；本仓铁律是**异常不得穿透 libuv
 *      回调** ⇒ 这里一律返回 `json_write_status`，一个 `throw` 都没有。
 *
 * 这一页只有**写出**；读入在 webapp
 * ----------------------------------
 * 反序列化的输入是一个**已经解析过的 DOM**（`uvcpp_json`，nlohmann 后端），而
 * 那个类型属于 webapp 模块（`webapp/uvcpp_web_json.h`，`UVCPP_BUILD_WEBAPP`
 * 默认关掉）。所以两个方向天然分在两层：
 *
 *   - **写出（本页）**：核心模块，零依赖，不吃任何 `UVCPP_ENABLE_*` 开关；
 *   - **读入（`webapp/uvcpp_web_json_reflect.h`）**：要 nlohmann 那棵 DOM。
 *
 * 依赖方向只有 core ← webapp 一个方向，所以是**那边 include 本页**，不是反过来。
 *
 * 支持的字段类型（其余一律**编译期**报错）
 * --------------------------------------
 *   - `bool`
 *   - 八种整型（含 `char`；`char` 按**数字**写，与 nlohmann 一致）
 *   - `float` / `double`（`NaN`/`±Inf` 产出 `null`，那是 writer 的契约）
 *   - `std::string`、`const char*`（`nullptr` 产出 `null`）
 *   - 另一个标了 `UVCPP_JSON_FIELDS` 的结构体（递归写成对象）
 *   - **以上任意一种**的 `std::vector`（写成数组；`std::vector<std::vector<int> >`
 *     也成）
 *
 * **不支持**：C 数组（`char buf[32]`）、`std::map`、`std::optional`、指针成员
 * （`const char*` 之外的）、枚举。这不是"忘了"，是这一版的范围（见文档的"没做的"）。
 * 传了不支持的类型**编不过**，而且报错是 `static_assert` 点名的那种，不会静默
 * 写成一个数字或一个空对象。
 *
 * 字段顺序 = **宏里列出的顺序**（不是成员声明的顺序）
 * --------------------------------------------------
 * 元组按宏的参数顺序建，展开按下标升序，而花括号初始化列表的求值顺序是标准
 * 保证的（左到右），逗号运算符又自带序列点 ⇒ 写出顺序严格等于宏里的顺序。
 * 有用例专门判这一条（一个**成员声明顺序与宏顺序相反**的结构体）。
 *
 * 失败是粘性的，而且**不补任何字节**
 * ---------------------------------
 * 任何一步非 OK 就立刻返回那个错，后面的字段不再写。与 `uvcpp_json_writer` 一样：
 * **失败之后缓冲里可能是个半截 JSON**，调用方在非 OK 时应当丢弃产出。
 *
 * 线程
 * ----
 * 字段表是**函数内静态**（C++11 保证只初始化一次），构造完就不再变；本页所有
 * 函数都是纯的、无共享状态。`uvcpp_json_writer` 自己的线程约束照旧（一个 writer
 * 只属于一个线程）。
 */
#pragma once
#ifndef SRC_UVCPP_UVCPP_JSON_REFLECT_H
#define SRC_UVCPP_UVCPP_JSON_REFLECT_H

#include <cstddef>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include <uvcpp/uvcpp_json_writer.h>

namespace uvcpp {

// 前置声明：类别 5/6 的分派要递归回公开入口（那里还有一遍 static_assert）。
// 声明放在 `json_detail` **外面**，是为了让 json_detail 里的非限定名查找只找到
// 这一个候选 —— 两个命名空间里各有一个同名声明就成了二义调用。
template <typename T>
json_write_status uvcpp_to_json(const T& obj, uvcpp_json_writer& w);

namespace json_detail {

// =========================================================================
// index_sequence —— C++11 手写版
// =========================================================================
//
// `std::index_sequence` / `std::make_index_sequence` 是 **C++14** 才有的。C++11
// 里要"按下标展开一个元组"就必须自己造一个 —— 而按下标展开正是本模块的核心
// 手法（字段表是个异构元组，每个字段的类型都不一样，只能靠下标取）。
//
// 这个实现是标准的递归形状：`N` 逐个减到 0，每减一层把当前值**贴到前面**，
// 于是最终的参数包是升序的 `0, 1, ..., N-1`。**升序是要紧的**：降序也能编过，
// 但写出字段的顺序就反了（有用例专门判顺序，还有一个变异专门把这里改成降序）。

/** @brief 编译期的下标序列（C++11 版 `std::index_sequence`）。 */
template <std::size_t... I>
struct uvcpp_index_sequence {};

template <std::size_t N, std::size_t... I>
struct uvcpp_index_sequence_impl : uvcpp_index_sequence_impl<N - 1, N - 1, I...> {};

template <std::size_t... I>
struct uvcpp_index_sequence_impl<0, I...> {
  typedef uvcpp_index_sequence<I...> type;
};

/** @brief 造出 `0, 1, ..., N-1`（C++11 版 `std::make_index_sequence`）。 */
template <std::size_t N>
using uvcpp_make_index_seq = typename uvcpp_index_sequence_impl<N>::type;

// =========================================================================
// 字段描述器
// =========================================================================

/**
 * @brief 一个字段：名字 + 成员指针。
 *
 * `name` 指向**静态存储期**的字符串（宏给的就是字面量 `#字段`），所以读入侧的
 * 错误报告可以把这个名字直接交出去，不必拷贝、不会悬垂。
 *
 * 这一版只有这两样。ignored / required / 别名 / 校验约束都没做（见文档"没做的"）。
 */
template <typename Class, typename FieldType>
struct uvcpp_json_field {
  const char* name;
  FieldType Class::* pointer;
};

/** @brief 造一个字段描述器（宏里用；也可以手写）。 */
template <typename Class, typename FieldType>
uvcpp_json_field<Class, FieldType> uvcpp_make_field(const char* name,
                                                    FieldType Class::* pointer) {
  uvcpp_json_field<Class, FieldType> f;
  f.name = name;
  f.pointer = pointer;
  return f;
}

// =========================================================================
// 类型探测（C++11 版 SFINAE）
// =========================================================================

/// C++11 版的 `std::void_t`（那是 C++17）。
template <typename...>
struct uvcpp_void {
  typedef void type;
};

/** @brief 这个类型有没有 `UVCPP_JSON_FIELDS` 生成的字段表。 */
template <typename T, typename = void>
struct uvcpp_has_json_fields : std::false_type {};

template <typename T>
struct uvcpp_has_json_fields<T, typename uvcpp_void<decltype(T::uvcppJsonFields())>::type>
    : std::true_type {};

/// 去掉 cv 与引用（字段类型、使用者传进来的类型都要先归一）。
template <typename T>
struct uvcpp_plain {
  typedef typename std::decay<T>::type type;
};

/** @brief 是不是字符串形状（`std::string` 或 C 字符串指针）。 */
template <typename T>
struct uvcpp_is_string : std::false_type {};
template <>
struct uvcpp_is_string<std::string> : std::true_type {};
template <>
struct uvcpp_is_string<const char*> : std::true_type {};
template <>
struct uvcpp_is_string<char*> : std::true_type {};

/** @brief 是不是 `std::vector`；是的话把元素类型放进 `element`。 */
template <typename T>
struct uvcpp_is_vector : std::false_type {
  typedef void element;
};
template <typename T, typename A>
struct uvcpp_is_vector<std::vector<T, A> > : std::true_type {
  typedef T element;
};

/**
 * @brief 字段的**类别**（分派的依据，编译期常量）：
 *
 *   | 值 | 含义 |
 *   |---|---|
 *   | 0 | `bool` |
 *   | 1 | 有符号整型 |
 *   | 2 | 无符号整型 |
 *   | 3 | 浮点 |
 *   | 4 | `std::string` / `const char*` |
 *   | 5 | 标了 `UVCPP_JSON_FIELDS` 的结构体 |
 *   | 6 | 上面任一种的 `std::vector` |
 *   | -1 | **不支持**（会被 `static_assert` 拦下并点名） |
 *
 * 顺序要紧：`bool` 必须排在整型前面（`std::is_integral<bool>` 也是真），
 * 字符串必须排在指针类之前（否则 `const char*` 会被当成别的）。
 */
template <typename T>
struct uvcpp_json_kind {
  typedef typename uvcpp_plain<T>::type P;
  static const int value =
      std::is_same<P, bool>::value
          ? 0
          : (uvcpp_is_string<P>::value
                 ? 4
                 : (std::is_integral<P>::value
                        ? (std::is_signed<P>::value ? 1 : 2)
                        : (std::is_floating_point<P>::value
                               ? 3
                               : (uvcpp_is_vector<P>::value
                                      ? 6
                                      : (uvcpp_has_json_fields<P>::value ? 5 : -1)))));
};

// =========================================================================
// 写一个值（按类别分派）
// =========================================================================

// 前置声明：类别 6（vector）要递归调它。带标签的那些重载在下面，不带标签的
// 分部在最后 —— 而模板体里的非限定名要在**定义点**就能找到，所以这里必须先声明。
template <typename T>
json_write_status uvcpp_write_value(const T& v, uvcpp_json_writer& w);

/// 类别 -1：不支持的类型。**只有这一条会走到 `static_assert`**。
template <typename T>
json_write_status uvcpp_write_value(const T&, uvcpp_json_writer&, std::integral_constant<int, -1>) {
  static_assert(sizeof(T) == 0,
                "uvcpp_to_json：这个字段类型没有可序列化的形状。支持 bool / 整型 / 浮点 / "
                "std::string / const char* / 标了 UVCPP_JSON_FIELDS 的结构体，以及上面任一种的 "
                "std::vector；C 数组、std::map、std::optional、枚举、其它指针都不在这一版的范围里。");
  return json_write_status::MISUSE;
}

template <typename T>
json_write_status uvcpp_write_value(const T& v, uvcpp_json_writer& w,
                                    std::integral_constant<int, 0>) {
  return w.value(v);  // bool
}

template <typename T>
json_write_status uvcpp_write_value(const T& v, uvcpp_json_writer& w,
                                    std::integral_constant<int, 1>) {
  // 有符号整型统一走 long long：writer 那八个宽度重载是原类型直写，这里多一次
  // 零扩展/符号扩展的转换，产出仍是同一个十进制数（`char` 也按数字写）。
  return w.value(static_cast<long long>(v));
}

template <typename T>
json_write_status uvcpp_write_value(const T& v, uvcpp_json_writer& w,
                                    std::integral_constant<int, 2>) {
  return w.value(static_cast<unsigned long long>(v));
}

/// @brief `float` 与 `double` 要**分开**送到 writer 的两个重载上。
///
/// 为什么不统一 `static_cast<double>`：writer 的"最短往返"判据是按**原类型**的
/// 精度算的（`float` 用 `%.9g`、`double` 用 `%.17g`）。把 `float` 转成 `double`
/// 再写，`0.1f` 就会写成 `0.10000000149011612` —— 数值没错，但产出又多又难看，
/// 而且与直接 `w.value(0.1f)` 不一致。`long double` 走 `double` 这条路（会掉
/// 精度，文档的"没做的"里写着）。
template <typename T>
json_write_status uvcpp_write_float(const T& v, uvcpp_json_writer& w, std::true_type) {
  return w.value(static_cast<float>(v));
}
template <typename T>
json_write_status uvcpp_write_float(const T& v, uvcpp_json_writer& w, std::false_type) {
  return w.value(static_cast<double>(v));
}

template <typename T>
json_write_status uvcpp_write_value(const T& v, uvcpp_json_writer& w,
                                    std::integral_constant<int, 3>) {
  typedef typename uvcpp_plain<T>::type P;
  return uvcpp_write_float(v, w, std::is_same<P, float>());
}

template <typename T>
json_write_status uvcpp_write_value(const T& v, uvcpp_json_writer& w,
                                    std::integral_constant<int, 4>) {
  return w.value(v);  // std::string / const char*（nullptr 产出 null）
}

template <typename T>
json_write_status uvcpp_write_value(const T& v, uvcpp_json_writer& w,
                                    std::integral_constant<int, 5>) {
  return uvcpp_to_json(v, w);  // 嵌套结构体：递归成对象
}

template <typename T>
json_write_status uvcpp_write_value(const T& v, uvcpp_json_writer& w,
                                    std::integral_constant<int, 6>) {
  json_write_status rc = w.array_begin();
  if (rc != json_write_status::OK) return rc;
  for (typename T::const_iterator it = v.begin(); it != v.end(); ++it) {
    rc = uvcpp_write_value(*it, w);  // 元素再分派一次（嵌套 vector 也终止于类型结构）
    if (rc != json_write_status::OK) return rc;
  }
  return w.array_end();
}

/** @brief 写一个值：先算类别，再分派。 */
template <typename T>
json_write_status uvcpp_write_value(const T& v, uvcpp_json_writer& w) {
  typedef typename uvcpp_plain<T>::type P;
  return uvcpp_write_value(v, w, std::integral_constant<int, uvcpp_json_kind<P>::value>());
}

// =========================================================================
// 写一个结构体（展开字段表）
// =========================================================================

/**
 * @brief 写一个字段：`"名字": 值`。
 *
 * 第一个参数是**上一步的结果** —— 已经错了就原地返回，不再往下写一个字节。
 * 这是"粘性"的实现点：错误不必层层检查，展开的那条链自己会停。
 */
template <typename Class, typename FieldType>
json_write_status uvcpp_write_field(json_write_status rc, const Class& obj,
                                    const uvcpp_json_field<Class, FieldType>& f,
                                    uvcpp_json_writer& w) {
  if (rc != json_write_status::OK) return rc;
  rc = w.key(f.name);
  if (rc != json_write_status::OK) return rc;
  return uvcpp_write_value(obj.*(f.pointer), w);
}

/**
 * @brief 按下标升序展开整张字段表。
 *
 * 展开手法是 `int swallow[] = { 0, (rc = ..., 0)... };`：
 *
 *   - **花括号初始化列表的求值顺序是标准保证的**（左到右），
 *   - 逗号运算符自带序列点，
 *   - 每一项读的是上一项写下的 `rc`。
 *
 * 三件事合起来 ⇒ 字段按宏里的顺序写出，且**错一次就停**。C++11 没有折叠
 * 表达式，这就是它的替代品。
 */
template <typename Class, typename Tuple, std::size_t... I>
json_write_status uvcpp_write_fields(const Class& obj, uvcpp_json_writer& w, const Tuple& fields,
                                     uvcpp_index_sequence<I...>) {
  json_write_status rc = json_write_status::OK;
  int swallow[] = {0, (rc = uvcpp_write_field(rc, obj, std::get<I>(fields), w), 0)...};
  (void)swallow;
  return rc;
}

}  // namespace json_detail

/**
 * @brief 把一个标了 `UVCPP_JSON_FIELDS` 的结构体写成一个 JSON **对象**。
 *
 * @param obj 要写的对象
 * @param w   目标 writer（可以是外部缓冲，也可以是自带的）
 * @return 第一个出错的返回码；成功是 `json_write_status::OK`
 *
 * 只写"这个对象自己"那一层：`{` 由本函数写，字段由字段表写，`}` 由本函数写。
 * 想把它塞进更大的结构里，自己在外面开好容器再调：
 *
 *     w.object_begin();
 *     w.key("user");
 *     uvcpp_to_json(one_user, w);   // 这里写出一个完整的对象
 *     w.object_end();
 *
 * **失败时产出不可用**（可能是半截 JSON），丢弃 `w.str()` 里那一段就是正确做法。
 *
 * 这个类型没有字段表时是**编译期**报错（`static_assert`），不是运行时返错 ——
 * "忘了标注"是个编码错误，不该等到运行时才发现。
 */
template <typename T>
json_write_status uvcpp_to_json(const T& obj, uvcpp_json_writer& w) {
  typedef typename json_detail::uvcpp_plain<T>::type plain;
  static_assert(json_detail::uvcpp_has_json_fields<plain>::value,
                "uvcpp_to_json：这个类型没有 `UVCPP_JSON_FIELDS(Type, ...)` 字段表。");
  return uvcpp_to_json(
      obj, w,
      std::integral_constant<bool, json_detail::uvcpp_has_json_fields<plain>::value>());
}

/// @brief 有字段表的那一支：真写。
template <typename T>
json_write_status uvcpp_to_json(const T& obj, uvcpp_json_writer& w, std::true_type) {
  json_write_status rc = w.object_begin();
  if (rc != json_write_status::OK) return rc;

  const typename json_detail::uvcpp_plain<T>::type& self = obj;
  const typename json_detail::uvcpp_plain<decltype(self.uvcppJsonFields())>::type& fields =
      self.uvcppJsonFields();
  typedef typename json_detail::uvcpp_plain<decltype(fields)>::type tuple_type;

  rc = json_detail::uvcpp_write_fields(
      self, w, fields,
      json_detail::uvcpp_make_index_seq<std::tuple_size<tuple_type>::value>());
  if (rc != json_write_status::OK) return rc;

  return w.object_end();
}

/// @brief 没有字段表的那一支：到不了（上面那句 `static_assert` 先拦下了）。
template <typename T>
json_write_status uvcpp_to_json(const T&, uvcpp_json_writer&, std::false_type) {
  return json_write_status::MISUSE;
}

}  // namespace uvcpp

// =========================================================================
// 给使用者用的两个宏
// =========================================================================
//
// `UVCPP_JSON_FIELDS(Type, a, b, c)` 要放在**类体内部**，并且只能用在被列的
// 成员**都已经声明之后**（宏里要取 `&Type::成员`）：
//
//     struct user {
//       std::string name;
//       int age;
//       std::vector<std::string> roles;
//
//       UVCPP_JSON_FIELDS(user, name, age, roles)   // 不需要分号，多写一个也没事
//     };
//
// 展开出两样东西：一个成员 typedef（元组的类型，供下标展开用）与一个静态成员
// 函数 `uvcppJsonFields()`（整个库靠这个名字认出"这个类型有字段表"）。
//
// 为什么需要下面那一堆 MAP_*：C++11 的预处理器**没有**"遍历可变参数"的原语。
// 惯例做法是按参数个数分派到 N 个手写的展开宏上（Gustedt 的 FOR_EACH 那一套），
// 所以有 16 行几乎一样的定义。**上限 16 个字段**就是从这里来的：超过 16 个会
// 得到一个看不懂的宏报错（不是类型错误）—— 这一点在文档里写明。

#define UVCPP_JSON_CAT2_(a, b) a##b
#define UVCPP_JSON_CAT_(a, b) UVCPP_JSON_CAT2_(a, b)

/// 数出参数个数（1..16）—— 见上面那段，标准的分派技巧。
#define UVCPP_JSON_ARG16_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define UVCPP_JSON_NARG_(...) UVCPP_JSON_ARG16_(__VA_ARGS__)
#define UVCPP_JSON_COUNT_RSEQ_() 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0
#define UVCPP_JSON_NARG(...) UVCPP_JSON_NARG_(__VA_ARGS__, UVCPP_JSON_COUNT_RSEQ_())

/// 一个字段 → 一个字段描述器。
#define UVCPP_JSON_FIELD_(T, f) ::uvcpp::json_detail::uvcpp_make_field<T>(#f, &T::f)

#define UVCPP_JSON_MAP_(m, T, ...) \
  UVCPP_JSON_CAT_(UVCPP_JSON_MAP_, UVCPP_JSON_NARG(__VA_ARGS__))(m, T, __VA_ARGS__)

#define UVCPP_JSON_MAP_1(m, T, a1) m(T, a1)
#define UVCPP_JSON_MAP_2(m, T, a1, a2) m(T, a1), m(T, a2)
#define UVCPP_JSON_MAP_3(m, T, a1, a2, a3) m(T, a1), m(T, a2), m(T, a3)
#define UVCPP_JSON_MAP_4(m, T, a1, a2, a3, a4) m(T, a1), m(T, a2), m(T, a3), m(T, a4)
#define UVCPP_JSON_MAP_5(m, T, a1, a2, a3, a4, a5) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5)
#define UVCPP_JSON_MAP_6(m, T, a1, a2, a3, a4, a5, a6) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5), m(T, a6)
#define UVCPP_JSON_MAP_7(m, T, a1, a2, a3, a4, a5, a6, a7) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5), m(T, a6), m(T, a7)
#define UVCPP_JSON_MAP_8(m, T, a1, a2, a3, a4, a5, a6, a7, a8) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5), m(T, a6), m(T, a7), m(T, a8)
#define UVCPP_JSON_MAP_9(m, T, a1, a2, a3, a4, a5, a6, a7, a8, a9) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5), m(T, a6), m(T, a7), m(T, a8), m(T, a9)
#define UVCPP_JSON_MAP_10(m, T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5), m(T, a6), m(T, a7), m(T, a8), m(T, a9), m(T, a10)
#define UVCPP_JSON_MAP_11(m, T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5), m(T, a6), m(T, a7), m(T, a8), m(T, a9), m(T, a10), \
      m(T, a11)
#define UVCPP_JSON_MAP_12(m, T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5), m(T, a6), m(T, a7), m(T, a8), m(T, a9), m(T, a10), \
      m(T, a11), m(T, a12)
#define UVCPP_JSON_MAP_13(m, T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5), m(T, a6), m(T, a7), m(T, a8), m(T, a9), m(T, a10), \
      m(T, a11), m(T, a12), m(T, a13)
#define UVCPP_JSON_MAP_14(m, T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5), m(T, a6), m(T, a7), m(T, a8), m(T, a9), m(T, a10), \
      m(T, a11), m(T, a12), m(T, a13), m(T, a14)
#define UVCPP_JSON_MAP_15(m, T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5), m(T, a6), m(T, a7), m(T, a8), m(T, a9), m(T, a10), \
      m(T, a11), m(T, a12), m(T, a13), m(T, a14), m(T, a15)
#define UVCPP_JSON_MAP_16(m, T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16) \
  m(T, a1), m(T, a2), m(T, a3), m(T, a4), m(T, a5), m(T, a6), m(T, a7), m(T, a8), m(T, a9), m(T, a10), \
      m(T, a11), m(T, a12), m(T, a13), m(T, a14), m(T, a15), m(T, a16)

/**
 * @brief 给一个结构体登记 JSON 字段表（放在类体内部、被列成员之后）。
 *
 *     UVCPP_JSON_FIELDS(user, name, age, roles)
 *
 * 想要换一个 JSON 名字怎么办？这一版没有别名（见文档"没做的"），成员名就是
 * 键名。要改键名就把成员名改掉，或者在写出之后另行处理 —— 不要指望它猜。
 */
// -------------------------------------------------------------------------
// MSVC 的**传统预处理器**过不去下面这个宏 —— 这里就地拦住，报一句人话。
//
// 为什么：这个宏要把 `__VA_ARGS__` 转给 `UVCPP_JSON_MAP_`、再由它转给
// `UVCPP_JSON_ARG16_`，而 cl 的传统预处理器（没开 `/Zc:preprocessor` 时的
// 默认值）**根本不做可变参数的转发** —— 使用者看到的是一串
// `C4003: not enough arguments for function-like macro invocation`，跟真正的
// 原因（预处理器模式）毫无关系。实测 2026-09-24：cl 14.44 与 14.51 两套工具集
// 上传统模式全红，同一批最小复现加 `/Zc:preprocessor` 全绿。
//
// 走 CMake 的消费者不用管这件事（导出目标上的 INTERFACE 选项会自动带上，
// 见 `CMakeLists.txt`），本仓自己的用例也是这么拿到的。**只有手写 cl 命令行的
// 人**会撞上，那就加 `/Zc:preprocessor`。
// -------------------------------------------------------------------------
#if defined(_MSC_VER) && !defined(__clang__) && defined(_MSVC_TRADITIONAL) && \
    _MSVC_TRADITIONAL
#define UVCPP_JSON_FIELDS(Type, ...)                                          \
  static_assert(false,                                                        \
                "UVCPP_JSON_FIELDS 需要 MSVC 的符合标准预处理器：请加 "       \
                "/Zc:preprocessor（本仓 CMake 导出的目标已自动带上）");       \
  static_assert(true, "");
#else
#define UVCPP_JSON_FIELDS(Type, ...)                                                          \
  typedef decltype(::std::make_tuple(UVCPP_JSON_MAP_(UVCPP_JSON_FIELD_, Type, __VA_ARGS__)))   \
      uvcpp_json_fields_tuple;                                                                 \
  static const uvcpp_json_fields_tuple& uvcppJsonFields() {                                    \
    static const uvcpp_json_fields_tuple fields =                                              \
        ::std::make_tuple(UVCPP_JSON_MAP_(UVCPP_JSON_FIELD_, Type, __VA_ARGS__));               \
    return fields;                                                                             \
  }
#endif

#endif  // SRC_UVCPP_UVCPP_JSON_REFLECT_H
