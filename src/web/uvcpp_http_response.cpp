/**
 * @file src/web/uvcpp_http_response.cpp
 * @brief Implementation of uvcpp_http_response.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_http_response.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_http_parser.h>
#include <sstream>

namespace uvcpp {

// =========================================================================
// Construction
// =========================================================================

// **构造函数里不再预留头表容量**（改在 `http_reserve_headers()`：头一次真插入
// 时才一次要到 4 个位置，见 `uvcpp_http_common.h`）。
//
// 原来在这里 `headers.reserve(4)`，理由是"响应对象每个请求新建一个，头表每次
// 都从空开始，不预留就要连撞三次 `push_back` 的 1→2→4 扩容"。那个理由对
// **会放头**的响应仍然成立 —— 而那正是懒预留保留的语义（第一次插入时仍然
// 只要一次 256 B）。
//
// 改的原因是有**一份响应从不放头**：HTTP 层的 `dispatch_h1_request()` /
// `dispatch_h2_request()` 造的栈上那个 `resp`，只是给兜底 handler 写
// `deferred` 一个字段用的（webapp 的响应走自己的 `uvcpp_web_context::resp_`）。
// 构造函数里预留 ⇒ 每请求白付一次 256 B 分配。分配普查里
// `uvcpp_http_response::uvcpp_http_response` 那 2.00 次/请求，有一次就是它。
uvcpp_http_response::uvcpp_http_response() {}

uvcpp_http_response::~uvcpp_http_response() {}

// 两个成员函数是手写的（`UVCPP_DEFINE_COPY_FUNC` 只声明），逐字段列一遍 ——
// **加字段时漏掉一处没有任何编译期提示**，`stream_id` 就这么漏过一次。
//
// `deferred` 是响应的一部分，不是可有可无的运行时标记：漏掉它会让一份
// 拷贝看起来「没设过 deferred」，而它承载的语义是「框架不要立刻发我」。
// `stream_id` / `retryable` 同理，而且更隐蔽：它们是 h2 上仅有的两个**身份**
// 字段 —— 在回调里存一份副本再据此重试是很自然的写法，副本里丢了 `retryable`
// 就会把"这条请求没被处理过"读成"处理过了"，丢了 `stream_id` 就分不清
// 回应的是哪条流。h1 上这两个字段恒为 0 / false，所以在 h1 用例里看不出来。
uvcpp_http_response::uvcpp_http_response(const uvcpp_http_response& other)
    : version(other.version),
      status_code(other.status_code),
      status_message(other.status_message),
      headers(other.headers),
      body(other.body),
      deferred(other.deferred),
      stream_id(other.stream_id),
      retryable(other.retryable) {}

uvcpp_http_response& uvcpp_http_response::operator=(
    const uvcpp_http_response& other) {
  if (this != &other) {
    version        = other.version;
    status_code    = other.status_code;
    status_message = other.status_message;
    headers        = other.headers;
    body.clone(other.body);
    deferred       = other.deferred;
    stream_id      = other.stream_id;
    retryable      = other.retryable;
  }
  return *this;
}

// =========================================================================
// Header operations
// =========================================================================

void uvcpp_http_response::set_header(const std::string& key,
                                      const std::string& value) {
  http_set_header(headers, key, value);
}

std::string uvcpp_http_response::get_header(const std::string& key,
                                             const std::string& default_val) const {
  return http_get_header(headers, key, default_val);
}

bool uvcpp_http_response::has_header(const std::string& key) const {
  return http_has_header(headers, key);
}

// --- `const char*` 形态：一行转发，逻辑全在 http_common 里 ---

void uvcpp_http_response::set_header(const char* key,
                                      const std::string& value) {
  http_set_header(headers, key, value);
}

std::string uvcpp_http_response::get_header(
    const char* key, const std::string& default_val) const {
  return http_get_header(headers, key, default_val);
}

bool uvcpp_http_response::has_header(const char* key) const {
  return http_has_header(headers, key);
}

void uvcpp_http_response::remove_header(const char* key) {
  const size_t n = http_name_len(key);
  for (auto it = headers.begin(); it != headers.end(); ++it) {
    if (http_name_iequal(it->name.data(), it->name.size(), key, n)) {
      headers.erase(it);
      return;
    }
  }
}

void uvcpp_http_response::remove_header(const std::string& key) {
  for (auto it = headers.begin(); it != headers.end(); ++it) {
    if (http_name_equal(it->name, key)) {
      headers.erase(it);
      return;
    }
  }
}

std::string uvcpp_http_response::content_type() const {
  return get_header("content-type");
}

void uvcpp_http_response::set_header(const char* key, const char* value) {
  http_set_header(headers, key, value);
}

void uvcpp_http_response::set_content_type(const char* ct) {
  set_header("content-type", ct);
}

void uvcpp_http_response::set_content_type(const std::string& ct) {
  set_header("content-type", ct);
}

// =========================================================================
// Serialization
// =========================================================================

namespace {

// 十进制/十六进制追加。等价于 `oss << v`，但绕开 num_put 面 —— 后者要经过
// locale、sentry 与虚调用，一个整数上百纳秒，而这里每个响应都要格式化
// 状态码与 content-length 两个数。
void append_dec(std::string& out, unsigned long long v) {
  char tmp[24];
  char* p = tmp + sizeof(tmp);
  do {
    *--p = static_cast<char>('0' + (v % 10));
    v /= 10;
  } while (v != 0);
  out.append(p, static_cast<size_t>(tmp + sizeof(tmp) - p));
}

void append_hex(std::string& out, unsigned long long v) {
  static const char* d = "0123456789abcdef";
  char tmp[20];
  char* p = tmp + sizeof(tmp);
  do {
    *--p = d[v & 0xF];
    v >>= 4;
  } while (v != 0);
  out.append(p, static_cast<size_t>(tmp + sizeof(tmp) - p));
}

}  // namespace

std::string uvcpp_http_response::to_string(bool include_body) const {
  // 造串那个形态：给需要**返回值**的调用方（h2 帧、测试、上一层的辅助函数）
  // 用。服务端热路径不走这里 —— 它走 `to_string_into` 把字节写进连接上那块
  // 复用缓冲，省掉的正是下面 `std::string result;` 那一次分配（那个串的字节
  // 立刻被拷进写请求自己的头部缓冲，然后就被析构了，见 `uvcpp_http_server`
  // 里 `send_response` 的调用点）。
  std::string result;
  to_string_into(result, include_body);
  return result;
}

void uvcpp_http_response::to_string_into(std::string& result,
                                        bool include_body) const {
  // 与 `to_string` **同一份实现**（上面那个就是"造一个空串再调这里"）⇒ 两条
  // 路出来的字节逐字节相同，不存在"两处序列化慢慢漂开"这件事。
  //
  // `clear()` 是"复用得起来"的**全部**前提：它只把长度置 0、容量留着 —— 于是
  // 下面那句 `reserve` 在稳态下（`est` 每次都一样）连进都不进。实测（探针
  // `n1_streambuf_probe.cpp`，libstdc++ 13）：`= std::string()` 在这家实现上恰好
  // 也留容量（空临时串的移动赋值退化成 clear），真丢缓冲的是 `swap`/`shrink_to_fit`。
  result.clear();
  const std::string version_str = uvcpp_http_version_str(version);
  const std::string reason = status_message.empty()
                                 ? http_status_reason(status_code)
                                 : status_message;

  // Detect chunked transfer encoding
  bool chunked = false;
  for (const auto& h : headers) {
    if (http_name_equal(h.name, "transfer-encoding") &&
        h.value.find("chunked") != std::string::npos) {
      chunked = true;
    }
  }

  // --- Headers ---
  bool has_cl = has_header("content-length");

  // 先算一遍长度直接 reserve：拼的过程中不再增长，整条响应只有一次分配。
  // 估算是上界而非精确值 —— 估大了只是多占一点，估小了也只会多一次增长。
  size_t est = 32 + version_str.size() + reason.size();
  for (const auto& h : headers) {
    est += h.name.size() + h.value.size() + 4;
  }
  if (include_body) est += body.size() + 24;

  // 复用形态（`to_string_into` 的调用方）下 `result` 的容量是上一次留下的：只要
  // `est` 没涨过它，这一句连 `reserve` 都不做 ⇒ 整条响应一次分配都没有。
  if (est > result.capacity()) result.reserve(est);

  // --- Status line ---
  result += version_str;
  result += ' ';
  // 状态码按 **int** 打印，与原实现 `oss << static_cast<int>(status_code)` 逐字一致。
  // `append_dec` 收的是无符号，直接传下去的话 `static_cast<http_status>(-1)`
  // 那种非法值会被印成 18446744073709551615，而不是原来的 -1 —— 都是坏报文，
  // 但"这次改动不改字节"这句话就没法无条件成立了。多一个分支换那句话成立。
  const int code = static_cast<int>(status_code);
  if (code < 0) {
    result += '-';
    append_dec(result, static_cast<unsigned long long>(-static_cast<long long>(code)));
  } else {
    append_dec(result, static_cast<unsigned long long>(code));
  }
  result += ' ';
  result += reason;
  result += "\r\n";

  for (const auto& h : headers) {
    result += h.name;
    result += ": ";
    result += h.value;
    result += "\r\n";
  }

  if (chunked) {
    // Chunked: no Content-Length (included)
  } else if (!has_cl && body.size() > 0) {
    result += "content-length: ";
    append_dec(result, static_cast<unsigned long long>(body.size()));
    result += "\r\n";
  }

  // --- Blank line ---
  result += "\r\n";

  // 只序列化头部：到这里已经是一条完整的头部块（状态行 + 各头 + 空行）。
  if (!include_body) return;

  // --- Body ---
  if (chunked) {
    if (body.size() > 0) {
      append_hex(result, static_cast<unsigned long long>(body.size()));
      result += "\r\n";
      result.append(body.get_const_data(), body.size());
      result += "\r\n";
    }
    // **空 body 也必须发终止块。** 原先这一支写成
    // `if (chunked && body.size() > 0) { ...; result += "0\r\n\r\n"; }`，
    // 于是 `transfer-encoding: chunked` 配空 body 时会产出**未终止**的报文：
    // 既没有 content-length 也没有 `0\r\n\r\n`，keep-alive 上对端只能一直等
    // 下一个块，直到自己超时。终止块是 chunked 的**帧**，不是 body 的一部分，
    // 所以它跟 body 是否为空无关。
    result += "0\r\n\r\n";
  } else if (body.size() > 0) {
    result.append(body.get_const_data(), body.size());
  }
}

// =========================================================================
// from_parser
// =========================================================================

uvcpp_http_response uvcpp_http_response::from_parser(
    const uvcpp_http_parser& parser, const uvcpp_buf& body) {
  uvcpp_http_response resp;
  resp.version        = parser.get_uvcpp_http_version();
  resp.status_code    = parser.get_status_code();
  resp.status_message = http_status_reason(resp.status_code);
  resp.headers        = parser.get_headers();
  resp.body.clone(body);
  return resp;
}

// =========================================================================
// Factory helpers
// =========================================================================

uvcpp_http_response uvcpp_http_response::ok(const char* body_data, size_t len,
                                             const std::string& content_type) {
  uvcpp_http_response resp;
  resp.status_code    = http_status::OK;
  resp.status_message = "OK";
  if (!content_type.empty()) {
    resp.set_content_type(content_type);
  }
  if (body_data && len > 0) {
    resp.body.clone_data(body_data, len);
  }
  return resp;
}

uvcpp_http_response uvcpp_http_response::not_found(const char* body_data,
                                                     size_t len) {
  uvcpp_http_response resp;
  resp.status_code    = http_status::NOT_FOUND;
  resp.status_message = "Not Found";
  resp.set_content_type("text/plain");
  if (body_data && len > 0) {
    resp.body.clone_data(body_data, len);
  } else {
    const char* default_body = "404 Not Found";
    resp.body.clone_data(default_body, 13);
  }
  return resp;
}

uvcpp_http_response uvcpp_http_response::server_error(const char* body_data,
                                                       size_t len) {
  uvcpp_http_response resp;
  resp.status_code    = http_status::INTERNAL_SERVER_ERROR;
  resp.status_message = "Internal Server Error";
  resp.set_content_type("text/plain");
  if (body_data && len > 0) {
    resp.body.clone_data(body_data, len);
  } else {
    const char* default_body = "500 Internal Server Error";
    resp.body.clone_data(default_body, 25);
  }
  return resp;
}

uvcpp_http_response uvcpp_http_response::make(http_status code,
                                               const char* body_data, size_t len,
                                               const std::string& content_type) {
  uvcpp_http_response resp;
  resp.status_code    = code;
  resp.status_message = http_status_reason(code);
  if (!content_type.empty()) {
    resp.set_content_type(content_type);
  }
  if (body_data && len > 0) {
    resp.body.clone_data(body_data, len);
  }
  return resp;
}

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
