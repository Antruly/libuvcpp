/**
 * @file src/uvcpp/uvcpp_json_scan.h
 * @brief JSON 深度预扫描：把"深到能爆栈"的输入在**交给解析器之前**挑出来。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它解决什么
 * ----------
 * `[[[[[...]]]]]` 这种输入对**递归下降且没有深度上限**的解析器是致命的：每进一层
 * 递归一次，几万层就能把线程栈打穿 —— 一行请求体就能触发的拒绝服务。
 * `uvcpp_json_max_nesting()` 只走一遍输入，返回最大嵌套深度（O(n)、无分配、
 * 不构造任何 DOM），调用方拿它与自己的上限比一下，超了就报 TOO_DEEP 走人。
 *
 * 为什么是"预扫描"，而不是让解析器自己管
 * --------------------------------------
 * 两条理由，一条是能力、一条是契约：
 *
 *   * **能力**：nlohmann 的解析器没有深度上限这一个开关（它的上限就是栈）。
 *     yyjson 有，但那个上限是**编译期**常量（`YYJSON_READER_DEPTH_LIMIT`），
 *     改不了每调用一次的上限 —— 而我们的上限是调用方传进来的运行时值。
 *   * **契约**：两个后端必须对同一份输入给出**同一个** `TOO_DEEP` 结论。把
 *     深度判定放在解析器之前、且只有一份实现，这件事就不再靠"两个解析器的
 *     上限恰好一致"来维持。
 *
 * 为什么它落在核心层（`src/uvcpp/`）而不是某个后端里
 * --------------------------------------------------
 * 本库有**两个** JSON 前端：`webapp/uvcpp_web_json.h` 的 nlohmann 路径与
 * `yyjson/uvcpp_json_doc.h` 的 yyjson 路径。它们在各自的 `parse` 里调的是
 * **同一个** `uvcpp_json_max_nesting`。这个名字与签名最早出现在 webapp 里，
 * yyjson 后端落地时搬到核心层（webapp 的公开头仍然把它带出来，所以使用者的
 * `#include <webapp/uvcpp_web_json.h>` + 调用**逐字不变**）—— 复制一份到
 * yyjson 模块才是那个不能做的选择：深度判定是安全边界，两份实现漂移的那一天，
 * "两个后端行为一致"就成了一句没人验得了的话。
 *
 * 判据
 * ----
 * 它对字符串字面量（含转义）免疫：`{"a":"[[[[[["}` 的深度是 **1**，不是 7；
 * `{"a":"\""}` 里的反斜杠不会把它后面的引号当成字符串结尾。五条断言在
 * `tests/functional/web_app_request_func.cpp`；yyjson 后端与 nlohmann 后端
 * 对同一批输入的深度结论相同这一条，在
 * `tests/functional/yyjson_doc_func.cpp`。
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_JSON_SCAN_H
#define SRC_UVCPP_UVCPP_JSON_SCAN_H

#include <cstddef>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

/**
 * @brief 估算输入的最大嵌套深度。
 *
 * 跳过字符串字面量（含转义），所以 `{"a":"[[[[[["}` 的深度是 1，不是 7。
 * 不配对的多余闭合括号不减少到负数（深度 0 时忽略），所以 `"]]]]"` 的深度是 0。
 *
 * @param data 输入（可为二进制，不要求 NUL 结尾）
 * @param len  字节数
 * @return 最大嵌套深度；`data == nullptr` 或 `len == 0` 时为 0
 *
 * 单独暴露出来是为了能直接测它 —— 它是 `parse` 里那条最容易被"优化掉"的防线。
 */
UVCPP_API size_t uvcpp_json_max_nesting(const char* data, size_t len);

}  // namespace uvcpp

#endif  // SRC_UVCPP_UVCPP_JSON_SCAN_H
