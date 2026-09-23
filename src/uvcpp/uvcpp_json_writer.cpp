/**
 * @file src/uvcpp/uvcpp_json_writer.cpp
 * @brief 见 uvcpp_json_writer.h（形状、转义决策表、粘性失败各是什么意思）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这里只放头文件里塞不下的三件事：**逗号该由谁写**的那张状态表、**转义为什么
 * 要按跨度批量做**、以及**浮点为什么不是一句 `to_string`**。
 *
 * ## 逗号与键：谁在什么时刻写
 *
 * 流式构造器最容易写错的就是"逗号写在元素前面还是后面"以及"这个位置到底该有
 * 值、该有键、还是什么都不该有"。本实现的账是这样算的（对象与数组共用一套
 * `check_value`/`emit_value`）：
 *
 * | 位置 | 逗号什么时候写 | 谁写 |
 * |---|---|---|
 * | 数组里第 2+ 个值 | **值**之前（`emit_value` 里，`count > 0` 时） | 值的插入路径 |
 * | 对象里第 2+ 个成员 | **键**之前（`key()` 里，`count > 0` 时） | 键的写入路径 |
 * | 对象的**第一个**成员 | 不写 | —— |
 * | 容器闭合（`}`/`]`） | 不写 | —— |
 *
 * 对象里"成员"是按**值**计数的（`count` 在值落下时才加一，不是在 `key()` 时），
 * 所以"键已写、值没写"这个中间态必须自己记着 —— 那就是 `frame::key_open`。
 * 它同时是两个错误的判据：**值出现时 `key_open == false`**（值没配键）与
 * **关对象时 `key_open == true`**（键没配值）。
 *
 * 容器自己作为一个**值**，它的逗号/键在 `push_frame()` 里就结算掉了（在写 `{`
 * 之前），所以闭合时不需要再动 —— 这也是为什么 `end_frame()` 里一个逗号都没有。
 *
 * ## 转义为什么按跨度批量做
 *
 * 逐字节"要不要转义 → 追加"的做法在**干净字符占绝大多数**的输入上纯属白付：
 * 每个字节一次函数调用 + 一次容量检查，而绝大多数 JSON 字符串里需要转义的
 * 字符是 0 个。所以这里是**先扫到第一个需要转义的字节，把前面这一整段一次追加
 * 进去**（`append_raw` 一次），再追加那个字节的转义形态，然后从下一个位置接着
 * 扫。一段 4 KiB 的干净字符串因此是 **1 次**追加而不是 4096 次。
 *
 * 这个形状是从 hical 的 `appendJsonString`（`src/core/CompileTimeJson.h:41-87`，
 * 32 项控制字符表 + 整段追加）借来的。转义**集合**与它不同（本模块不转义 `/`，
 * 理由写在头文件那张契约表下面），但"跨度批量"这个手法是共通的。
 *
 * ## 浮点为什么不是一句 `to_string`
 *
 * `std::to_string(double)` 等价于 `sprintf("%f")` —— **固定 6 位小数**：`0.1` 得到
 * `"0.100000"`（能用但难看），`1e-20` 得到 `"0.000000"`（**丢了**），`1e300` 得到
 * 一坨 300 位的数字（还是错的）。JSON 里 double 通常是"某个比率"，人和程序都要
 * 看它，所以这里按**最短能往返**的形态给：先试 15 位有效数字（double）/ 6 位
 * （float），用 `strtod` 读回来比对，对不上才用 17 位 / 9 位 —— 后者是**保证**
 * 能往返的位数（double 17、float 9，这是 IEEE-754 十进制往返的标准结论）。
 * 于是 `0.1` 就是 `0.1`，而不是 `0.10000000000000001`。
 *
 * ## 这个文件里没有任何 `#include <nlohmann/*>`
 *
 * 刻意的：本模块是**核心模块**，webapp 关掉时也要在编。谁往这里加 nlohmann，
 * 谁就把"关掉 webapp 就一个字节都不拉"这条契约弄坏了。
 */
#include <uvcpp/uvcpp_json_writer.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>        // std::bad_alloc（append_raw 的 catch 子句要用它）
#include <stdexcept>  // std::length_error（同上）

namespace uvcpp {

const char* json_write_status_name(json_write_status status) {
  switch (status) {
    case json_write_status::OK:        return "OK";
    case json_write_status::MISUSE:    return "MISUSE";
    case json_write_status::TOO_DEEP:  return "TOO_DEEP";
    case json_write_status::TOO_LARGE: return "TOO_LARGE";
    case json_write_status::UNCLOSED:  return "UNCLOSED";
    case json_write_status::NO_MEMORY: return "NO_MEMORY";
  }
  return "?";
}

json_write_options::json_write_options(size_t depth, size_t bytes)
    : max_depth(depth), max_bytes(bytes) {}

// 类内初始化了 `static const` 整型成员，按标准仍需要一个定义（有人取它地址、
// 或某些编译器在 ODR 使用下要它）。一行的事，别赌。
const size_t uvcpp_json_writer::k_max_depth;

uvcpp_json_writer::frame::frame() : is_object(false), key_open(false), count(0) {}

// =========================================================================
// 转义分类表
// =========================================================================
namespace {

/**
 * @brief 256 项分类表：每个字节给出它"要不要转义、怎么转"。
 *
 * 值的意思（`0` 是绝大多数）：
 *
 * | 值 | 含义 |
 * |---|---|
 * | 0 | 原样输出 |
 * | 1..7 | 转成 `\` + `k_short_escapes[值 - 1]` |
 * | 8 | 转成 `\u00XX`（其余 0x00–0x1F） |
 *
 * 用表而不是 `switch`：转义判定在**每一个字节**上都要做一次，而这个表是常量
 * 数组的下标 + 一次比较。表的构造在函数局部静态里（C++11 起它的初始化是
 * 线程安全的，这叫 magic static），所以第一次调用之后它就是一个只读数组。
 */
struct escape_table {
  unsigned char kind[256];

  escape_table() {
    std::memset(kind, 0, sizeof(kind));
    kind[static_cast<unsigned char>('"')] = 1;
    kind[static_cast<unsigned char>('\\')] = 2;
    kind[0x08] = 3;  // \b
    kind[0x0C] = 4;  // \f
    kind[0x0A] = 5;  // \n
    kind[0x0D] = 6;  // \r
    kind[0x09] = 7;  // \t
    for (int c = 0; c < 0x20; ++c) {
      if (kind[c] == 0) kind[c] = 8;  // 剩下的控制字符走 \u00XX
    }
    // 0x7F（DEL）**不转义**：JSON 不要求，且它是可打印面积之外的常见字节，
    // 转它会把二进制载荷（比如 base64 之外的东西）不必要地放大。
    // 0x80 及以上**不转义**：那是 UTF-8 的多字节序列，逐字节搬运才不会破坏它。
  }
};

const escape_table& escapes() {
  static const escape_table t;
  return t;
}

/// 值 1..7 对应的那个字符（下标 0 不用）。**没有** `/`。
const char k_short_escapes[8] = {'\0', '"', '\\', 'b', 'f', 'n', 'r', 't'};

const char k_hex_digits[17] = "0123456789abcdef";

/**
 * @brief 十进制无符号整数 → `out`，返回写了几字节（不写 NUL）。
 *
 * 不用 `std::to_string`：它每写一个数就分配一次串。不用 `snprintf`：`%lld` 在
 * MinGW 上要 `__USE_MINGW_ANSI_STDIO` 才是对的（本仓栽过 printf 家族的当）。
 * 手写这 8 行没有第三个代价。
 */
size_t utoa10(unsigned long long v, char* out) {
  char tmp[20];  // 2^64-1 是 20 位
  size_t n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + (v % 10u));
    v /= 10u;
  } while (v != 0);
  for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
  return n;
}

/// @brief 有符号：负数先转成无符号再取反，避开 `-INT64_MIN` 的溢出（UB）。
size_t itoa10(long long v, char* out) {
  if (v < 0) {
    const unsigned long long u =
        static_cast<unsigned long long>(-(v + 1)) + 1ull;
    out[0] = '-';
    return 1 + utoa10(u, out + 1);
  }
  return utoa10(static_cast<unsigned long long>(v), out);
}

/**
 * @brief 浮点 → 最短能往返的十进制形态。
 *
 * 见文件头"浮点为什么不是一句 `to_string`"。这里额外做一件兜底：`%g` 的小数点
 * 用的是**当前 locale** 的（本库自己不调 `setlocale`，但使用者可能改过），
 * 所以最后把不属于 `[0-9+-eE]` 的字节统一换成 `.` —— 对 `%g` 的输出来说那个
 * 位置只可能是小数点，于是产出在任何 locale 下都是**合法 JSON**。
 *
 * @return 写进去的字节数；`-1` 表示本地缓冲不够（对 double 来说不可达：
 *         `%.17g` 最长也就 24 字节，而缓冲给了 32）。
 */
int format_fp(double v, bool is_float, char* out, size_t cap) {
  const int short_digits = is_float ? 6 : 15;
  const int long_digits = is_float ? 9 : 17;

  int n = std::snprintf(out, cap, "%.*g", short_digits, v);
  if (n < 0 || static_cast<size_t>(n) >= cap) return -1;

  char* end = nullptr;
  const double back = std::strtod(out, &end);
  const bool round_trips =
      is_float ? (static_cast<float>(back) == static_cast<float>(v)) : (back == v);
  if (!round_trips) {
    n = std::snprintf(out, cap, "%.*g", long_digits, v);
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
  }

  for (int i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(out[i]);
    const bool numeric = (c >= '0' && c <= '9') || c == '-' || c == '+' ||
                         c == 'e' || c == 'E';
    if (!numeric) out[i] = '.';
  }
  return n;
}

}  // namespace

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_json_writer::uvcpp_json_writer()
    : uvcpp_json_writer(owned_, json_write_options()) {}

uvcpp_json_writer::uvcpp_json_writer(const json_write_options& opts)
    : uvcpp_json_writer(owned_, opts) {}

uvcpp_json_writer::uvcpp_json_writer(std::string& out)
    : uvcpp_json_writer(out, json_write_options()) {}

uvcpp_json_writer::uvcpp_json_writer(std::string& out,
                                     const json_write_options& opts)
    : out_(&out),
      base_(0),
      written_(0),
      depth_(0),
      root_done_(false),
      status_(json_write_status::OK),
      // 夹到定长栈的容量：见头文件里 `k_max_depth` 那条说明。
      max_depth_(opts.max_depth > k_max_depth ? k_max_depth : opts.max_depth),
      max_bytes_(opts.max_bytes) {
  // `out` 原有内容一概不动（追加语义），但记下它当时的长度 —— `clear()` 要
  // 截回这里，而不是截成空串。
  base_ = out_->size();
}

uvcpp_json_writer::~uvcpp_json_writer() {}

// =========================================================================
// 缓冲与失败
// =========================================================================

json_write_status uvcpp_json_writer::fail(json_write_status s) {
  // 粘性：只记**第一个**错误。后面的调用本身也都被 `status_` 挡住了，所以
  // 这里再判一次是为了挡住"同一个调用里连着 fail 两次"的情形。
  if (status_ == json_write_status::OK) status_ = s;
  return status_;
}

json_write_status uvcpp_json_writer::append_raw(const char* p, size_t n) {
  if (n == 0) return json_write_status::OK;
  // 写成减法而不是 `written_ + n`：后者在 max_bytes 接近 SIZE_MAX 时会回绕，
  // 于是"超了"被判成"没超"。
  if (n > max_bytes_ - written_) return fail(json_write_status::TOO_LARGE);
  try {
    out_->append(p, n);
  } catch (const std::bad_alloc&) {
    // 本模块不抛异常（异常不得穿透 libuv 回调），所以这里必须接住。
    return fail(json_write_status::NO_MEMORY);
  } catch (const std::length_error&) {
    // `append` 在结果超过 `max_size()` 时抛这个 —— 那是"太大了"，不是"没内存"。
    return fail(json_write_status::TOO_LARGE);
  } catch (...) {
    // 兜底：宁可报一个粗一点的码，也不能让异常逃出这个函数。
    return fail(json_write_status::NO_MEMORY);
  }
  written_ += n;
  return json_write_status::OK;
}

json_write_status uvcpp_json_writer::append_escaped(const char* p, size_t len) {
  if (len == 0) return json_write_status::OK;

  const unsigned char* kind = escapes().kind;
  size_t i = 0;
  size_t span_start = 0;

  while (i < len) {
    const unsigned char c = static_cast<unsigned char>(p[i]);
    const unsigned char k = kind[c];
    if (k == 0) {
      ++i;
      continue;
    }
    // 干净的一段**整段**追加（跨度批量，见文件头）。
    if (i > span_start) {
      json_write_status rc = append_raw(p + span_start, i - span_start);
      if (rc != json_write_status::OK) return rc;
    }
    char esc[6];
    size_t esc_len;
    if (k == 8) {
      esc[0] = '\\';
      esc[1] = 'u';
      esc[2] = '0';
      esc[3] = '0';
      esc[4] = k_hex_digits[(c >> 4) & 0x0Fu];
      esc[5] = k_hex_digits[c & 0x0Fu];
      esc_len = 6;
    } else {
      esc[0] = '\\';
      esc[1] = k_short_escapes[k];
      esc_len = 2;
    }
    json_write_status rc = append_raw(esc, esc_len);
    if (rc != json_write_status::OK) return rc;
    ++i;
    span_start = i;
  }
  if (len > span_start) {
    return append_raw(p + span_start, len - span_start);
  }
  return json_write_status::OK;
}

// =========================================================================
// 值的插入：校验 → 结算 → 写字节
// =========================================================================

json_write_status uvcpp_json_writer::check_value() const {
  if (depth_ == 0) {
    // 根位置只允许一个值。写过之后再来一个就是两个根值 —— 那是拼接，不是 JSON。
    if (root_done_) return json_write_status::MISUSE;
    return json_write_status::OK;
  }
  const frame& f = stack_[depth_ - 1];
  if (f.is_object && !f.key_open) return json_write_status::MISUSE;
  return json_write_status::OK;
}

json_write_status uvcpp_json_writer::emit_value() {
  if (depth_ == 0) {
    root_done_ = true;
    return json_write_status::OK;
  }
  frame& f = stack_[depth_ - 1];
  if (f.is_object) {
    // 对象里：逗号已经在 `key()` 里写了，这里只把"键待配值"消掉。
    f.key_open = false;
    ++f.count;
    return json_write_status::OK;
  }
  json_write_status rc = json_write_status::OK;
  if (f.count > 0) {
    rc = append_raw(",", 1);
    if (rc != json_write_status::OK) return rc;
  }
  ++f.count;
  return json_write_status::OK;
}

json_write_status uvcpp_json_writer::before_value() {
  // 先只校验、不产出：这样"值没配键"这类误用**一个字节都不会写出去**，缓冲里
  // 不会多出一个孤零零的逗号。（校验过了之后 `emit_value()` 就只有可能因为
  // 容量失败，那也是粘性失败、产出本来就要丢弃。）
  const json_write_status bad = check_value();
  if (bad != json_write_status::OK) return fail(bad);
  return emit_value();
}

json_write_status uvcpp_json_writer::push_frame(bool is_object) {
  // 深度先判：容器一推上去就等于放行了"再写 n 层"，所以要在**写 `{` 之前**判，
  // 免得留一个没配对的括号在缓冲里（虽然失败时产出要丢弃，但少留一点垃圾更好）。
  if (depth_ >= max_depth_) return fail(json_write_status::TOO_DEEP);
  const json_write_status rc = before_value();
  if (rc != json_write_status::OK) return rc;
  frame& f = stack_[depth_];
  f.is_object = is_object;
  f.key_open = false;
  f.count = 0;
  ++depth_;
  return append_raw(is_object ? "{" : "[", 1);
}

json_write_status uvcpp_json_writer::end_frame(bool is_object) {
  if (depth_ == 0) return fail(json_write_status::MISUSE);
  const frame& f = stack_[depth_ - 1];
  if (f.is_object != is_object) return fail(json_write_status::MISUSE);
  // 关对象时最后一个键还没配值 —— 那是 `{"a":}`，不是 `{"a":1}`。
  if (f.is_object && f.key_open) return fail(json_write_status::MISUSE);
  --depth_;
  return append_raw(is_object ? "}" : "]", 1);
}

// =========================================================================
// 公开：结构
// =========================================================================

json_write_status uvcpp_json_writer::object_begin() {
  if (status_ != json_write_status::OK) return status_;
  return push_frame(true);
}

json_write_status uvcpp_json_writer::array_begin() {
  if (status_ != json_write_status::OK) return status_;
  return push_frame(false);
}

json_write_status uvcpp_json_writer::object_end() {
  if (status_ != json_write_status::OK) return status_;
  return end_frame(true);
}

json_write_status uvcpp_json_writer::array_end() {
  if (status_ != json_write_status::OK) return status_;
  return end_frame(false);
}

// =========================================================================
// 公开：键
// =========================================================================

json_write_status uvcpp_json_writer::key(const char* k, size_t len) {
  if (status_ != json_write_status::OK) return status_;
  if (k == nullptr) return fail(json_write_status::MISUSE);
  if (depth_ == 0 || !stack_[depth_ - 1].is_object) {
    // 键只能出现在对象的第一层里 —— 根位置和数组里都没有"键"这回事。
    return fail(json_write_status::MISUSE);
  }
  frame& f = stack_[depth_ - 1];
  if (f.key_open) {
    // 上一个键还没等到它的值：`{"a" "b":1}` 这种。
    return fail(json_write_status::MISUSE);
  }
  json_write_status rc = json_write_status::OK;
  if (f.count > 0) {
    rc = append_raw(",", 1);
    if (rc != json_write_status::OK) return rc;
  }
  rc = append_raw("\"", 1);
  if (rc == json_write_status::OK) rc = append_escaped(k, len);
  if (rc == json_write_status::OK) rc = append_raw("\"", 1);
  if (rc == json_write_status::OK) rc = append_raw(":", 1);
  if (rc != json_write_status::OK) return rc;
  f.key_open = true;
  return json_write_status::OK;
}

json_write_status uvcpp_json_writer::key(const char* k) {
  // `key(nullptr)` 走下面那个重载去报 MISUSE —— 不做 `? strlen : 0` 那种
  // "把 null 当空串"的宽容，因为它几乎总是调用方的笔误。
  return key(k, k == nullptr ? 0 : std::strlen(k));
}

json_write_status uvcpp_json_writer::key(const std::string& k) {
  return key(k.data(), k.size());
}

// =========================================================================
// 公开：值
// =========================================================================

json_write_status uvcpp_json_writer::null_value() {
  if (status_ != json_write_status::OK) return status_;
  const json_write_status rc = before_value();
  if (rc != json_write_status::OK) return rc;
  return append_raw("null", 4);
}

json_write_status uvcpp_json_writer::value(bool v) {
  if (status_ != json_write_status::OK) return status_;
  const json_write_status rc = before_value();
  if (rc != json_write_status::OK) return rc;
  return v ? append_raw("true", 4) : append_raw("false", 5);
}

json_write_status uvcpp_json_writer::value_signed(long long v) {
  if (status_ != json_write_status::OK) return status_;
  const json_write_status rc = before_value();
  if (rc != json_write_status::OK) return rc;
  char tmp[24];
  const size_t n = itoa10(v, tmp);
  return append_raw(tmp, n);
}

json_write_status uvcpp_json_writer::value_unsigned(unsigned long long v) {
  if (status_ != json_write_status::OK) return status_;
  const json_write_status rc = before_value();
  if (rc != json_write_status::OK) return rc;
  char tmp[24];
  const size_t n = utoa10(v, tmp);
  return append_raw(tmp, n);
}

json_write_status uvcpp_json_writer::value_fp(double v, bool is_float) {
  if (status_ != json_write_status::OK) return status_;
  const json_write_status rc = before_value();
  if (rc != json_write_status::OK) return rc;

  char tmp[32];
  size_t n;
  if (std::isnan(v) || std::isinf(v)) {
    // JSON 的三个字面量里没有 NaN/Infinity。这里给 `null`（nlohmann 的 `dump()`
    // 也是这么办的）而不是报错：某个字段恰好是 NaN 不该让整份响应构造失败，
    // 而 `null` 是能往下走的、且**不骗人**的表示 —— 报 0 或者报一个巨大的数
    // 才是骗人的。
    std::memcpy(tmp, "null", 4);
    n = 4;
  } else {
    const int m = format_fp(v, is_float, tmp, sizeof(tmp));
    if (m < 0) {
      // 不可达（32 字节对任何 double 的 `%.17g` 都够）。真要到了，报 TOO_LARGE
      // 而不是继续写一段空串 —— 空串会让产出静默地少一个值。
      return fail(json_write_status::TOO_LARGE);
    }
    n = static_cast<size_t>(m);
  }
  return append_raw(tmp, n);
}

json_write_status uvcpp_json_writer::value(double v) {
  return value_fp(v, false);
}

json_write_status uvcpp_json_writer::value(float v) {
  return value_fp(static_cast<double>(v), true);
}

json_write_status uvcpp_json_writer::value(const char* s, size_t len) {
  if (status_ != json_write_status::OK) return status_;
  if (s == nullptr) {
    // `value((const char*)nullptr)` 是 `null`：C 串形态里"没有这个值"就该是
    // JSON 的 null，这比报 MISUSE 有用（也符合使用者的直觉）。长度不为 0 却
    // 给了 nullptr 就是错了。
    if (len != 0) return fail(json_write_status::MISUSE);
    return null_value();
  }
  const json_write_status rc = before_value();
  if (rc != json_write_status::OK) return rc;
  json_write_status r = append_raw("\"", 1);
  if (r == json_write_status::OK) r = append_escaped(s, len);
  if (r == json_write_status::OK) r = append_raw("\"", 1);
  return r;
}

json_write_status uvcpp_json_writer::value(const char* s) {
  return value(s, s == nullptr ? 0 : std::strlen(s));
}

json_write_status uvcpp_json_writer::value(const std::string& s) {
  return value(s.data(), s.size());
}

// =========================================================================
// 数值类型的转发（省得在头文件里铺 12 个实现）
// =========================================================================

json_write_status uvcpp_json_writer::value(short v) { return value_signed(v); }
json_write_status uvcpp_json_writer::value(unsigned short v) {
  return value_unsigned(v);
}
json_write_status uvcpp_json_writer::value(int v) { return value_signed(v); }
json_write_status uvcpp_json_writer::value(unsigned int v) {
  return value_unsigned(v);
}
json_write_status uvcpp_json_writer::value(long v) {
  return value_signed(static_cast<long long>(v));
}
json_write_status uvcpp_json_writer::value(unsigned long v) {
  return value_unsigned(static_cast<unsigned long long>(v));
}
json_write_status uvcpp_json_writer::value(long long v) {
  return value_signed(v);
}
json_write_status uvcpp_json_writer::value(unsigned long long v) {
  return value_unsigned(v);
}

// =========================================================================
// 收尾与查询
// =========================================================================

json_write_status uvcpp_json_writer::finish() {
  if (status_ != json_write_status::OK) return status_;
  if (depth_ != 0) return fail(json_write_status::UNCLOSED);
  if (!root_done_) return fail(json_write_status::MISUSE);
  return json_write_status::OK;
}

json_write_status uvcpp_json_writer::status() const { return status_; }

bool uvcpp_json_writer::ok() const {
  return status_ == json_write_status::OK;
}

size_t uvcpp_json_writer::depth() const { return depth_; }

size_t uvcpp_json_writer::written() const { return written_; }

void uvcpp_json_writer::reserve(size_t bytes) {
  try {
    // 预留在 `base_` 之上，也就是给"追加"留的余量。
    out_->reserve(base_ + bytes);
  } catch (...) {
    // 预留失败**不是**致命错误（真正的写下去那一步还会再试、并且那时才报
    // NO_MEMORY），所以这里只记账，不动产出。
    fail(json_write_status::NO_MEMORY);
  }
}

void uvcpp_json_writer::clear() {
  out_->resize(base_);  // 只截自己写过的那一段，别吃掉调用方的前缀
  written_ = 0;
  depth_ = 0;
  root_done_ = false;
  status_ = json_write_status::OK;
}

const std::string& uvcpp_json_writer::str() const { return *out_; }

const char* uvcpp_json_writer::data() const { return out_->data(); }

size_t uvcpp_json_writer::size() const { return out_->size(); }

}  // namespace uvcpp
