/**
 * @file src/uvcpp/uvcpp_json_scan.cpp
 * @brief `uvcpp_json_scan.h` 的实现。
 */

#include <uvcpp/uvcpp_json_scan.h>

namespace uvcpp {

// 这段实现**逐字**来自 `src/webapp/uvcpp_web_json.cpp`（yyjson 后端落地时搬到
// 核心层）：两个 JSON 后端共用它，所以行为必须与搬家之前完全相同 —— 搬家前
// 存在的那些断言（`tests/functional/web_app_request_func.cpp` 的五条）一条不改
// 地继续盯着它，就是为了让"只是搬家"这句话可验证。
size_t uvcpp_json_max_nesting(const char* data, size_t len) {
  if (data == nullptr || len == 0) return 0;

  size_t depth = 0;
  size_t max_depth = 0;
  bool in_string = false;
  bool escaped = false;

  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];

    if (in_string) {
      // 字符串内部一律不计深度，否则 {"a":"[[[[[["} 会被误判成很深。
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }

    switch (c) {
      case '"':
        in_string = true;
        break;
      case '[':
      case '{':
        ++depth;
        if (depth > max_depth) max_depth = depth;
        break;
      case ']':
      case '}':
        // 不配对的多余闭合括号不增加深度；深度为 0 时忽略。
        if (depth > 0) --depth;
        break;
      default:
        break;
    }
  }

  return max_depth;
}

}  // namespace uvcpp
