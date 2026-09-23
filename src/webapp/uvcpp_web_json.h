/**
 * @file src/webapp/uvcpp_web_json.h
 * @brief Web 框架的 JSON 支持（基于 nlohmann/json）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么还要包一层
 * ----------------
 * 不是为了"再抽象一次"，而是有三件具体的事必须做：
 *
 * 1. **不允许异常穿透 libuv 回调。** nlohmann 的 `parse()` 默认抛
 *    `parse_error`，而我们的调用点全在 libuv 的回调里。异常从那里逃出去会
 *    穿过 C 写的 `uv_run` 栈帧 —— 那是未定义行为，MSVC 直接 terminate。
 *    所以这里的解析接口是**不抛异常**的，改用 `is_discarded()`。
 *
 * 2. **深度预扫描。** nlohmann 的解析器是递归下降的，且**没有内建深度上限**。
 *    一个 `[[[[[...` 的请求体能让它对每个 `[` 递归一层，直接爆栈 —— 这是
 *    一个真实存在的拒绝服务手法。这里在解析前先扫一遍最大嵌套深度。
 *
 * 3. **隔离后端。** 全框架只有这一个头文件认识 nlohmann。将来要换 JSON
 *    实现，改动集中在这里。
 *
 * 跨 DLL 的注意事项
 * -----------------
 * nlohmann/json 是 header-only 的，`uvcpp_json` 的对象会跨 DLL 边界传递。
 * 这要求**使用者的翻译单元用同一个版本、同一组配置宏**编译它（默认配置即可）。
 * 如果使用者在自己的工程里覆盖了 `JSON_USE_IMPLICIT_CONVERSIONS`、
 * `JSON_DIAGNOSTICS` 之类的宏，两个 TU 对同一个类型的布局认知就不一致了 ——
 * 这是 header-only 库跨 DLL 的固有约束，不是本库能绕开的。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_JSON_H
#define SRC_WEBAPP_UVCPP_WEB_JSON_H

// 放在最前面：<wingdi.h>（经 <windows.h>）里有 `#define ERROR 0`，宏展开
// 不看命名空间，任何在它之后解析的、用 ERROR 作标识符的代码都会炸。
// nlohmann 本身没有这个标识符，但把它排在前面能把这个风险彻底摘掉。
#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

/**
 * @brief 框架对外的 JSON 类型。
 *
 * 直接就是 nlohmann 的 json，没有包一层 wrapper —— 包装会导致使用者的
 * 代码里到处是转换，而它本身已经足够好用。用别名是为了让「后端是谁」
 * 这件事只在这一个头文件里有定义。
 */
using uvcpp_json = nlohmann::json;

// =========================================================================
// 解析
// =========================================================================

/** @brief 解析结果。 */
enum class json_status : int {
  OK = 0,
  EMPTY,      ///< 输入为空
  SYNTAX,     ///< 语法错误
  TOO_DEEP,   ///< 嵌套超过 max_depth（防爆栈）
  TOO_LARGE,  ///< 超过 max_bytes

  // 下面四个**不来自解析**，来自**反射读入**（webapp/uvcpp_web_json_reflect.h）：
  // 解析成功之后把 DOM 填进结构体时才会出现。放在同一个枚举里，是为了让
  // "解析 + 填结构体"这一条链只返回一个状态类型 —— 使用者一个 switch 就能覆盖
  // 全部失败形状（而"解析失败"与"字段不符"的处理方式往往一样：回 400）。
  MISMATCH,   ///< 反射读入：根不是对象，或某个字段的类型/值域不符合目标
  MISSING,    ///< 反射读入：开了 missing_is_error，而有字段没给（或值为 null）
  UNKNOWN,    ///< 反射读入：开了 unknown_is_error，而有多出来的成员
  NO_MEMORY,  ///< 反射读入：分配失败（std::bad_alloc）
};

/** @brief 状态的可读名称，可直接写日志。 */
UVCPP_API const char* json_status_name(json_status status);

/** @brief 解析的可调限制。 */
struct UVCPP_API json_parse_options {
  size_t max_depth;  ///< 最大嵌套层数，默认 64
  size_t max_bytes;  ///< 最大输入字节数，默认 8 MiB

  /// 显式构造函数（成员初始化器会破坏聚合初始化，与 `uvcpp_log_record` 同理）
  explicit json_parse_options(size_t depth = 64,
                              size_t bytes = 8u * 1024u * 1024u);
};

/**
 * @brief 解析 JSON，**不抛异常**。
 *
 * @param data 输入（可为二进制，不要求 NUL 结尾）
 * @param len  字节数
 * @param out  成功时写入解析结果；**失败时不改动**
 * @return 见 `json_status`
 *
 * 空输入返回 `EMPTY` 而不是 `SYNTAX`：对「没有 body」和「body 不是 JSON」
 * 这两种情况，调用方通常要返回不同的响应（400 vs 415），所以分开报。
 */
UVCPP_API json_status uvcpp_json_parse(const char* data, size_t len,
                                       uvcpp_json& out);

/** @brief 带限制的重载。 */
UVCPP_API json_status uvcpp_json_parse(const char* data, size_t len,
                                       uvcpp_json& out,
                                       const json_parse_options& opts);

/** @brief `std::string` 版本。 */
UVCPP_API json_status uvcpp_json_parse(const std::string& s, uvcpp_json& out);

/**
 * @brief 估算输入的最大嵌套深度。
 *
 * 跳过字符串字面量（含转义），所以 `{"a":"[[[[[["}` 的深度是 1，不是 7。
 * 这个函数是上面第 2 条防护的实现，单独暴露出来是为了能直接测它。
 */
UVCPP_API size_t uvcpp_json_max_nesting(const char* data, size_t len);

// =========================================================================
// 序列化
// =========================================================================

/**
 * @brief 紧凑序列化。
 *
 * 内部吞掉异常：`dump()` 在遇到非法 UTF-8 字符串时会抛 `type_error.316`，
 * 而调用点常常在响应发送路径上，那里绝不能抛。失败返回空串。
 */
UVCPP_API std::string uvcpp_json_dump(const uvcpp_json& j);

/** @brief 带缩进的序列化（`indent` 为空格数；<=0 等价于紧凑）。 */
UVCPP_API std::string uvcpp_json_dump(const uvcpp_json& j, int indent);

// =========================================================================
// 取值
// =========================================================================
//
// 这些函数存在的理由：nlohmann 的 `j["k"]` 在**非 const** 对象上遇到缺失的
// 键会**插入一个 null**，在 const 对象上则会抛。前者会悄悄改掉使用者的
// JSON，后者会抛穿 libuv 回调。两者在请求处理器里都是坑，所以给一组
// 「查不到就给默认值、类型不对也给默认值」的取值函数。

/** @brief 取字符串；键不存在或类型不是字符串时返回 `def`。 */
UVCPP_API std::string uvcpp_json_get_string(const uvcpp_json& j,
                                            const char* key,
                                            const std::string& def);

/** @brief 取布尔；键不存在或类型不是布尔时返回 `def`。 */
UVCPP_API bool uvcpp_json_get_bool(const uvcpp_json& j, const char* key,
                                   bool def);

/**
 * @brief 取整数。
 * @return 取到了返回 true 并写入 `out`；否则返回 false 且不写 `out`。
 *         数值是浮点时按截断处理（`3.9` → `3`）。
 */
UVCPP_API bool uvcpp_json_get_int(const uvcpp_json& j, const char* key,
                                  long long& out);

/** @brief 取浮点；整数也接受。取不到返回 false。 */
UVCPP_API bool uvcpp_json_get_double(const uvcpp_json& j, const char* key,
                                     double& out);

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_JSON_H
