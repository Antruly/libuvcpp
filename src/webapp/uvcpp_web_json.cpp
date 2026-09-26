/**
 * @file src/webapp/uvcpp_web_json.cpp
 * @brief `uvcpp_web_json.h` 的实现。
 */

#include <webapp/uvcpp_web_json.h>

#include <exception>
#include <string>

#include <webapp/uvcpp_log.h>

namespace uvcpp {

const char* json_status_name(json_status status) {
  switch (status) {
    case json_status::OK:        return "OK";
    case json_status::EMPTY:     return "EMPTY";
    case json_status::SYNTAX:    return "SYNTAX";
    case json_status::TOO_DEEP:  return "TOO_DEEP";
    case json_status::TOO_LARGE: return "TOO_LARGE";
    case json_status::MISMATCH:  return "MISMATCH";
    case json_status::MISSING:   return "MISSING";
    case json_status::UNKNOWN:   return "UNKNOWN";
    case json_status::NO_MEMORY: return "NO_MEMORY";
  }
  return "?";
}

json_parse_options::json_parse_options(size_t depth, size_t bytes)
    : max_depth(depth), max_bytes(bytes) {}

// =========================================================================
// 深度预扫描
// =========================================================================

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

// =========================================================================
// 解析
// =========================================================================

json_status uvcpp_json_parse(const char* data, size_t len, uvcpp_json& out) {
  return uvcpp_json_parse(data, len, out, json_parse_options());
}

json_status uvcpp_json_parse(const char* data, size_t len, uvcpp_json& out,
                             const json_parse_options& opts) {
  if (data == nullptr || len == 0) return json_status::EMPTY;
  if (len > opts.max_bytes) return json_status::TOO_LARGE;

  // 先扫地雷再解析：nlohmann 的解析器是递归下降且没有深度上限，
  // 深嵌套输入会让它对每一层递归一次，直接爆栈（拒绝服务）。
  // 这个扫描是 O(n) 且只走一遍，相比解析本身的代价可以忽略。
  if (uvcpp_json_max_nesting(data, len) > opts.max_depth) {
    return json_status::TOO_DEEP;
  }

  // 第 3、4 个参数：不传回调、**不抛异常**。开启允许异常会让非法输入
  // 抛 parse_error，而本函数的调用点都在 libuv 回调里 —— 异常穿过 C 的
  // uv_run 栈帧是未定义行为。改为返回 discarded 值，我们检查它。
  uvcpp_json parsed = uvcpp_json::parse(data, data + len, nullptr, false);
  if (parsed.is_discarded()) return json_status::SYNTAX;

  out = parsed;
  return json_status::OK;
}

json_status uvcpp_json_parse(const std::string& s, uvcpp_json& out) {
  return uvcpp_json_parse(s.data(), s.size(), out);
}

// =========================================================================
// 序列化
// =========================================================================

namespace {

/**
 * @brief 把"序列化抛了"变成一条能被看见的日志。
 *
 * 这个异常**必须**吞掉（调用点在响应发送路径上，放它穿到 libuv 就是一次
 * 崩溃），但吞掉之后返回的那个空串与「一个空的 JSON 文档」**完全不可区分**：
 * 调用方拿到的都是 `""`，于是 `resp.json(j)` 会发出 `200` +
 * `Content-Type: application/json` + **零字节正文**。没有返回值、没有布尔、
 * 没有日志能分辨 —— 所以这条日志是这个失败**唯一**的出口。
 *
 * @param what 异常自报的原因；`nullptr` 表示是非 `std::exception` 的异常。
 * @param fn   哪个重载抛的（两个重载的缩进参数不同，排障时要分得开）。
 */
void log_dump_failure(const char* what, const char* fn) {
  UVCPP_LOG_ERROR(log_category::JSON)
      << fn << " 序列化失败（多半是某个字符串里有非法 UTF-8）："
      << (what != nullptr ? what : "非 std::exception 的异常")
      << " —— 返回空串，调用方会把零字节正文当成成功的 JSON 发出去";
}

}  // namespace

std::string uvcpp_json_dump(const uvcpp_json& j) {
  // dump() 在遇到非法 UTF-8 的字符串时抛 type_error.316。调用点常在响应
  // 发送路径上，那里抛异常同样会穿到 libuv —— 吞掉并返回空串，
  // 由调用方把空串当成序列化失败处理。
  try {
    return j.dump();
  } catch (const std::exception& e) {
    log_dump_failure(e.what(), "uvcpp_json_dump");
    return std::string();
  } catch (...) {
    log_dump_failure(nullptr, "uvcpp_json_dump");
    return std::string();
  }
}

std::string uvcpp_json_dump(const uvcpp_json& j, int indent) {
  try {
    if (indent <= 0) return j.dump();
    return j.dump(indent);
  } catch (const std::exception& e) {
    log_dump_failure(e.what(), "uvcpp_json_dump(indent)");
    return std::string();
  } catch (...) {
    log_dump_failure(nullptr, "uvcpp_json_dump(indent)");
    return std::string();
  }
}

// =========================================================================
// 取值
// =========================================================================

namespace {

/// 查找键；不是对象或键不存在时返回 nullptr。
const uvcpp_json* find_member(const uvcpp_json& j, const char* key) {
  if (key == nullptr) return nullptr;
  if (!j.is_object()) return nullptr;
  // 用 find 而不是 operator[]：后者在缺失时会给**非 const** 对象插入一个
  // null（悄悄改数据），在 const 对象上则抛异常。
  uvcpp_json::const_iterator it = j.find(key);
  if (it == j.end()) return nullptr;
  return &(*it);
}

}  // namespace

std::string uvcpp_json_get_string(const uvcpp_json& j, const char* key,
                                  const std::string& def) {
  const uvcpp_json* v = find_member(j, key);
  if (v == nullptr || !v->is_string()) return def;
  return v->get<std::string>();
}

bool uvcpp_json_get_bool(const uvcpp_json& j, const char* key, bool def) {
  const uvcpp_json* v = find_member(j, key);
  if (v == nullptr || !v->is_boolean()) return def;
  return v->get<bool>();
}

bool uvcpp_json_get_int(const uvcpp_json& j, const char* key, long long& out) {
  const uvcpp_json* v = find_member(j, key);
  if (v == nullptr || !v->is_number()) return false;
  try {
    if (v->is_number_unsigned()) {
      out = static_cast<long long>(v->get<unsigned long long>());
    } else if (v->is_number_integer()) {
      out = v->get<long long>();
    } else {
      // 浮点：截断。JSON 的数字类型不分整浮，`3.0` 也该能当 3 读。
      out = static_cast<long long>(v->get<double>());
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool uvcpp_json_get_double(const uvcpp_json& j, const char* key, double& out) {
  const uvcpp_json* v = find_member(j, key);
  if (v == nullptr || !v->is_number()) return false;
  try {
    out = v->get<double>();
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace uvcpp
