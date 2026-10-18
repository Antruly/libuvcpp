/**
 * @file src/redis/uvcpp_redis_resp.cpp
 * @brief RESP2/RESP3 的增量解析、值对象与命令编码。
 * @author zhuweiye
 * @version 1.7.5
 */

#include <redis/uvcpp_redis_resp.h>

#if UVCPP_REDIS_ENABLE

#include <uv.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace uvcpp {

namespace {

/// 解析游标。`off` 只在成功时由调用方提交 —— 中途 `NEED_MORE` 时整段重来。
struct cursor {
  const char* p;
  std::size_t len;
  std::size_t off;
};

/// 在 `c.off` 处找 `\r\n`，返回行起点与该行长度（不含 CRLF）。
bool read_line(const cursor& c, std::size_t& line_off, std::size_t& line_len) {
  for (std::size_t i = c.off; i + 1 < c.len; ++i) {
    if (c.p[i] == '\r' && c.p[i + 1] == '\n') {
      line_off = c.off;
      line_len = i - c.off;
      return true;
    }
  }
  return false;
}

/// 严格十进制（可带一个前导 `-`）。不接受空串、`+`、空格、尾部垃圾。
bool parse_int(const char* s, std::size_t n, long long& v) {
  if (n == 0) return false;
  std::size_t i = 0;
  bool neg = false;
  if (s[0] == '-') {
    neg = true;
    i = 1;
    if (n == 1) return false;
  }
  long long acc = 0;
  for (; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    const int d = s[i] - '0';
    if (acc > (std::numeric_limits<long long>::max() - d) / 10) return false;
    acc = acc * 10 + d;
  }
  v = neg ? -acc : acc;
  return true;
}

typedef std::shared_ptr<uvcpp_redis_value_impl> node_ptr;

node_ptr make_node(uvcpp_redis_type t) {
  node_ptr n(new uvcpp_redis_value_impl());
  n->type = t;
  return n;
}

uvcpp_redis_parser::result parse_value(cursor& c, int depth, bool& push,
                                       uvcpp_redis_parser& P, node_ptr& out);

/**
 * @brief 解析 `*n` / `~n` / `>n` 里的 n 个元素，或 `%n` 的 n 对键值。
 *
 * 抽出来是因为 push、集合、映射共用"先读计数、再递归 n 次"这段；差别只有
 * 落在哪个容器里。
 */
uvcpp_redis_parser::result parse_aggregate(cursor& c, int depth,
                                           uvcpp_redis_parser& P,
                                           uvcpp_redis_type kind, bool is_map,
                                           bool is_push, node_ptr& out) {
  std::size_t line_off = 0;
  std::size_t line_len = 0;
  if (!read_line(c, line_off, line_len)) return uvcpp_redis_parser::NEED_MORE;

  if (line_len == 1 && c.p[line_off] == '?') {
    return P.bad(UV_EPROTO, "不支持 RESP3 的流式聚合（*? / %? / ~? / >?）");
  }

  long long n = 0;
  if (!parse_int(c.p + line_off, line_len, n)) {
    return P.bad(UV_EPROTO, "聚合的长度不是一个合法的整数");
  }
  if (kind == uvcpp_redis_type::ARRAY && !is_push && n == -1) {
    out = make_node(uvcpp_redis_type::NULL_VALUE);
    c.off = line_off + line_len + 2;
    return uvcpp_redis_parser::DONE;
  }
  if (n < 0) {
    return P.bad(UV_EPROTO, "聚合的长度是负数");
  }

  const std::size_t count = static_cast<std::size_t>(n);
  const std::size_t elems = is_map ? count * 2 : count;
  if (elems > P.max_elements) {
    return P.bad(UV_EPROTO, "聚合的元素个数超过上限");
  }

  c.off = line_off + line_len + 2;

  out = make_node(kind);
  if (is_map) {
    out->pairs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      node_ptr k;
      node_ptr v;
      bool dummy = false;
      // NEED_MORE 与 BAD 都要**原样**往上传：把 BAD 吞成 NEED_MORE 会让一个
      // 已经确定坏掉的连接永远等下一段字节。
      uvcpp_redis_parser::result r =
          parse_value(c, depth + 1, dummy, P, k);
      if (r != uvcpp_redis_parser::DONE) return r;
      r = parse_value(c, depth + 1, dummy, P, v);
      if (r != uvcpp_redis_parser::DONE) return r;
      out->pairs.push_back(
          std::make_pair(uvcpp_redis_value_factory::make(k),
                         uvcpp_redis_value_factory::make(v)));
    }
  } else {
    out->elems.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      node_ptr v;
      bool child_push = false;
      const uvcpp_redis_parser::result r =
          parse_value(c, depth + 1, child_push, P, v);
      if (r != uvcpp_redis_parser::DONE) return r;
      out->elems.push_back(uvcpp_redis_value_factory::make(v));
    }
  }
  return uvcpp_redis_parser::DONE;
}

uvcpp_redis_parser::result parse_value(cursor& c, int depth, bool& push,
                                       uvcpp_redis_parser& P, node_ptr& out) {
  if (depth > P.max_depth) {
    return P.bad(UV_EPROTO, "嵌套层数超过上限");
  }
  if (c.off >= c.len) return uvcpp_redis_parser::NEED_MORE;

  const std::size_t start = c.off;
  const char t = c.p[start];

  // ---- 行型：+ - : , ( _ # ----
  switch (t) {
    case '+':
    case '-':
    case ':':
    case ',':
    case '(':
    case '_':
    case '#': {
      c.off = start + 1;
      std::size_t line_off = 0;
      std::size_t line_len = 0;
      if (!read_line(c, line_off, line_len)) return uvcpp_redis_parser::NEED_MORE;
      const char* text = c.p + line_off;
      c.off = line_off + line_len + 2;

      switch (t) {
        case '+': {
          out = make_node(uvcpp_redis_type::STATUS);
          out->text.assign(text, line_len);
          return uvcpp_redis_parser::DONE;
        }
        case '-': {
          out = make_node(uvcpp_redis_type::ERROR);
          out->text.assign(text, line_len);
          return uvcpp_redis_parser::DONE;
        }
        case ':': {
          long long v = 0;
          if (!parse_int(text, line_len, v)) {
            return P.bad(UV_EPROTO, "整数值不合法");
          }
          out = make_node(uvcpp_redis_type::INTEGER);
          out->integer = static_cast<std::int64_t>(v);
          return uvcpp_redis_parser::DONE;
        }
        case ',': {
          // 空文本是非法的；`inf` / `-inf` / `nan` 交给 strtod（它认这三个）。
          if (line_len == 0) return P.bad(UV_EPROTO, "浮点值为空");
          std::string tmp(text, line_len);
          char* end = nullptr;
          const double d = std::strtod(tmp.c_str(), &end);
          if (end == nullptr || *end != '\0') {
            return P.bad(UV_EPROTO, "浮点值不合法");
          }
          out = make_node(uvcpp_redis_type::DOUBLE);
          out->text.assign(text, line_len);
          out->number = d;
          return uvcpp_redis_parser::DONE;
        }
        case '(': {
          out = make_node(uvcpp_redis_type::BULK);
          out->text.assign(text, line_len);
          return uvcpp_redis_parser::DONE;
        }
        case '_': {
          if (line_len != 0) return P.bad(UV_EPROTO, "null 后面不该有内容");
          out = make_node(uvcpp_redis_type::NULL_VALUE);
          return uvcpp_redis_parser::DONE;
        }
        default: {  // '#'
          if (line_len != 1 || (text[0] != 't' && text[0] != 'f')) {
            return P.bad(UV_EPROTO, "布尔值只能是 #t 或 #f");
          }
          out = make_node(uvcpp_redis_type::BOOLEAN);
          out->boolean = (text[0] == 't');
          return uvcpp_redis_parser::DONE;
        }
      }
    }

    // ---- bulk / blob error / verbatim ----
    case '$':
    case '!':
    case '=': {
      c.off = start + 1;
      std::size_t line_off = 0;
      std::size_t line_len = 0;
      if (!read_line(c, line_off, line_len)) return uvcpp_redis_parser::NEED_MORE;

      if (line_len == 1 && c.p[line_off] == '?') {
        return P.bad(UV_EPROTO, "不支持 RESP3 的流式字符串（$?）");
      }

      long long n = 0;
      if (!parse_int(c.p + line_off, line_len, n)) {
        return P.bad(UV_EPROTO, "bulk 的长度不是一个合法的整数");
      }
      if (n == -1) {
        if (t != '$') return P.bad(UV_EPROTO, "只有 bulk 可以是空（-1）");
        out = make_node(uvcpp_redis_type::NULL_VALUE);
        c.off = line_off + line_len + 2;
        return uvcpp_redis_parser::DONE;
      }
      if (n < 0) return P.bad(UV_EPROTO, "bulk 的长度是负数");
      if (static_cast<std::size_t>(n) > P.max_bulk) {
        return P.bad(UV_EPROTO, "bulk 超过上限");
      }

      const std::size_t data_off = line_off + line_len + 2;
      const std::size_t data_len = static_cast<std::size_t>(n);
      if (data_off + data_len + 2 > c.len) {
        return uvcpp_redis_parser::NEED_MORE;
      }
      if (c.p[data_off + data_len] != '\r' ||
          c.p[data_off + data_len + 1] != '\n') {
        return P.bad(UV_EPROTO, "bulk 数据后面缺 CRLF");
      }

      if (t == '=') {
        // 逐字字符串的前 4 个字节是 `xxx:` 格式标签，对外部调用方没有意义。
        if (data_len < 4 || c.p[data_off + 3] != ':') {
          return P.bad(UV_EPROTO, "逐字字符串缺少格式标签");
        }
        out = make_node(uvcpp_redis_type::BULK);
        out->text.assign(c.p + data_off + 4, data_len - 4);
      } else if (t == '!') {
        out = make_node(uvcpp_redis_type::ERROR);
        out->text.assign(c.p + data_off, data_len);
      } else {
        out = make_node(uvcpp_redis_type::BULK);
        out->text.assign(c.p + data_off, data_len);
      }
      c.off = data_off + data_len + 2;
      return uvcpp_redis_parser::DONE;
    }

    // ---- 聚合 ----
    case '*':
      c.off = start + 1;
      return parse_aggregate(c, depth, P, uvcpp_redis_type::ARRAY, false, false,
                             out);
    case '~':
      c.off = start + 1;
      return parse_aggregate(c, depth, P, uvcpp_redis_type::SET, false, false,
                             out);
    case '>':
      c.off = start + 1;
      push = true;
      return parse_aggregate(c, depth, P, uvcpp_redis_type::ARRAY, false, true,
                             out);
    case '%':
      c.off = start + 1;
      return parse_aggregate(c, depth, P, uvcpp_redis_type::MAP, true, false,
                             out);

    // ---- 属性（RESP3）：`|n` 后面跟 n 对键值，再跟真正的值 ----
    case '|': {
      c.off = start + 1;
      std::size_t line_off = 0;
      std::size_t line_len = 0;
      if (!read_line(c, line_off, line_len)) return uvcpp_redis_parser::NEED_MORE;
      long long n = 0;
      if (!parse_int(c.p + line_off, line_len, n) || n < 0) {
        return P.bad(UV_EPROTO, "属性帧的长度不合法");
      }
      if (static_cast<std::size_t>(n) * 2 > P.max_elements) {
        return P.bad(UV_EPROTO, "属性的元素个数超过上限");
      }
      c.off = line_off + line_len + 2;
      // 属性的内容对调用方没有意义，读掉之后继续读真正的值。
      for (long long i = 0; i < n * 2; ++i) {
        node_ptr drop;
        bool dummy = false;
        const uvcpp_redis_parser::result r =
            parse_value(c, depth + 1, dummy, P, drop);
        if (r != uvcpp_redis_parser::DONE) return r;
      }
      return parse_value(c, depth, push, P, out);
    }

    default:
      break;
  }

  return P.bad(UV_EPROTO, "未知的 RESP 类型字节");
}

}  // namespace

uvcpp_redis_parser::uvcpp_redis_parser()
    : max_bulk(static_cast<std::size_t>(512) * 1024 * 1024),
      max_elements(static_cast<std::size_t>(1048576)),
      max_depth(64),
      last_error_(0) {}

uvcpp_redis_parser::result uvcpp_redis_parser::bad(int code,
                                                   const std::string& text) {
  last_error_ = code;
  last_error_text_ = text;
  return BAD;
}

uvcpp_redis_parser::result uvcpp_redis_parser::next(const char* buf,
                                                    std::size_t len,
                                                    std::size_t& off,
                                                    uvcpp_redis_value& out,
                                                    bool* is_push) {
  if (buf == nullptr || off > len) {
    if (buf == nullptr) return NEED_MORE;
    last_error_ = UV_EPROTO;
    last_error_text_ = "偏移量越界";
    return BAD;
  }
  cursor c;
  c.p = buf;
  c.len = len;
  c.off = off;

  bool push = false;
  node_ptr node;
  const result r = parse_value(c, 0, push, *this, node);
  if (r == DONE) {
    off = c.off;
    out = uvcpp_redis_value_factory::make(node);
    if (is_push != nullptr) *is_push = push;
  } else if (is_push != nullptr) {
    *is_push = false;
  }
  return r;
}

void uvcpp_redis_parser::encode_command(const std::vector<std::string>& args,
                                        std::string& out) {
  char head[32];
  std::snprintf(head, sizeof(head), "*%llu\r\n",
                static_cast<unsigned long long>(args.size()));
  out.append(head);
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::snprintf(head, sizeof(head), "$%llu\r\n",
                  static_cast<unsigned long long>(args[i].size()));
    out.append(head);
    out.append(args[i]);
    out.append("\r\n");
  }
}

void uvcpp_redis_parser::encode_commands(
    const std::vector<std::vector<std::string>>& commands, std::string& out) {
  for (std::size_t i = 0; i < commands.size(); ++i) {
    encode_command(commands[i], out);
  }
}

// ---------------------------------------------------------------------------
// uvcpp_redis_value
// ---------------------------------------------------------------------------

uvcpp_redis_value::uvcpp_redis_value() {}

uvcpp_redis_value::uvcpp_redis_value(
    std::shared_ptr<const uvcpp_redis_value_impl> p)
    : p_(p) {}

uvcpp_redis_value::~uvcpp_redis_value() {}

uvcpp_redis_value::uvcpp_redis_value(const uvcpp_redis_value& other)
    : p_(other.p_) {}

uvcpp_redis_value& uvcpp_redis_value::operator=(
    const uvcpp_redis_value& other) {
  p_ = other.p_;
  return *this;
}

uvcpp_redis_value::uvcpp_redis_value(uvcpp_redis_value&& other) noexcept
    : p_(std::move(other.p_)) {}

uvcpp_redis_value& uvcpp_redis_value::operator=(
    uvcpp_redis_value&& other) noexcept {
  p_ = std::move(other.p_);
  return *this;
}

const uvcpp_redis_value& uvcpp_redis_value::null() {
  static const uvcpp_redis_value v;
  return v;
}

uvcpp_redis_type uvcpp_redis_value::type() const {
  return p_ ? p_->type : uvcpp_redis_type::NULL_VALUE;
}

bool uvcpp_redis_value::is_null() const {
  return p_ == nullptr || p_->type == uvcpp_redis_type::NULL_VALUE;
}

bool uvcpp_redis_value::is_error() const {
  return p_ != nullptr && p_->type == uvcpp_redis_type::ERROR;
}

const std::string& uvcpp_redis_value::as_string() const {
  static const std::string kEmpty;
  return p_ ? p_->text : kEmpty;
}

std::int64_t uvcpp_redis_value::as_integer() const {
  if (p_ == nullptr) return 0;
  if (p_->type == uvcpp_redis_type::INTEGER) return p_->integer;
  if (p_->type == uvcpp_redis_type::BOOLEAN) return p_->boolean ? 1 : 0;
  return 0;
}

double uvcpp_redis_value::as_double() const {
  if (p_ == nullptr) return 0.0;
  if (p_->type == uvcpp_redis_type::DOUBLE) return p_->number;
  if (p_->type == uvcpp_redis_type::INTEGER) {
    return static_cast<double>(p_->integer);
  }
  return 0.0;
}

bool uvcpp_redis_value::as_bool() const {
  if (p_ == nullptr) return false;
  if (p_->type == uvcpp_redis_type::BOOLEAN) return p_->boolean;
  if (p_->type == uvcpp_redis_type::INTEGER) return p_->integer != 0;
  return false;
}

std::size_t uvcpp_redis_value::size() const {
  if (p_ == nullptr) return 0;
  if (p_->type == uvcpp_redis_type::MAP) return p_->pairs.size();
  return p_->elems.size();
}

const uvcpp_redis_value& uvcpp_redis_value::at(std::size_t i) const {
  if (p_ == nullptr || i >= p_->elems.size()) return null();
  return p_->elems[i];
}

const std::vector<std::pair<uvcpp_redis_value, uvcpp_redis_value>>&
uvcpp_redis_value::items() const {
  static const std::vector<std::pair<uvcpp_redis_value, uvcpp_redis_value>>
      kEmpty;
  if (p_ == nullptr || p_->type != uvcpp_redis_type::MAP) return kEmpty;
  return p_->pairs;
}

}  // namespace uvcpp

#endif  // UVCPP_REDIS_ENABLE
