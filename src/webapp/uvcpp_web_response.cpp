/**
 * @file src/webapp/uvcpp_web_response.cpp
 * @brief uvcpp_web_response 的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <webapp/uvcpp_web_response.h>

#include <cstddef>
#include <cstdio>

#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_util.h>

namespace uvcpp {

namespace {

/**
 * @brief 把无符号整数写成十进制字符串。
 *
 * 不用 `std::to_string`：本库对工具链的要求是「能编 C++11」，而不是
 * 「标准库把 C++11 全实现完了」。
 */
std::string size_to_string(size_t v) {
  char tmp[24];
  int m = 0;
  if (v == 0) {
    tmp[m++] = '0';
  } else {
    while (v > 0 && m < 23) {
      tmp[m++] = static_cast<char>('0' + (v % 10));
      v /= 10;
    }
  }
  std::string s;
  s.reserve(static_cast<size_t>(m));
  while (m > 0) s += tmp[--m];
  return s;
}

/// 默认文本 body 用：把状态码写成 "404 Not Found" 这样的短句。
std::string default_body_for(int code) {
  std::string s;
  s.reserve(32);
  if (code < 0) {
    // `-(code+1)+1` 而不是 `-code`：对 INT_MIN 取负是未定义行为。
    s += '-';
    s += size_to_string(static_cast<size_t>(-(code + 1)) + 1u);
  } else {
    s += size_to_string(static_cast<size_t>(code));
  }
  const char* reason = web_status_text(code);
  if (reason && *reason) {
    s += ' ';
    s += reason;
  }
  return s;
}

/// 判断 `resp` 是否已经声明了 chunked（此时不能自动补 Content-Length）。
bool is_chunked(const uvcpp_http_response& resp) {
  for (size_t i = 0; i < resp.headers.size(); ++i) {
    if (http_name_equal(resp.headers[i].name, "transfer-encoding") &&
        resp.headers[i].value.find("chunked") != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

// =========================================================================
// uvcpp_web_sent_info
// =========================================================================

uvcpp_web_sent_info::uvcpp_web_sent_info()
    : status_code(0), body_bytes(0), connection_id(0), ok(false) {}

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_web_response::uvcpp_web_response()
    : head_only_(false), ended_(false), deferred_(false) {
  // 默认给 200，且 reason phrase 与状态码一致。
  resp_.status_code = http_status::OK;
  resp_.status_message = web_status_text(200);
}

uvcpp_web_response::~uvcpp_web_response() {}

// =========================================================================
// 状态行
// =========================================================================

uvcpp_web_response& uvcpp_web_response::status(int code) {
  resp_.status_code = static_cast<http_status>(code);
  const char* reason = web_status_text(code);
  resp_.status_message = (reason && *reason) ? reason : "Unknown";
  return *this;
}

uvcpp_web_response& uvcpp_web_response::status(http_status code) {
  return status(static_cast<int>(code));
}

uvcpp_web_response& uvcpp_web_response::status_message(const std::string& msg) {
  resp_.status_message = msg;
  return *this;
}

int uvcpp_web_response::status_code() const {
  return static_cast<int>(resp_.status_code);
}

// =========================================================================
// 报头
// =========================================================================

uvcpp_web_response& uvcpp_web_response::set_header(const std::string& name,
                                                   const std::string& value) {
  http_set_header(resp_.headers, name, value);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::add_header(const std::string& name,
                                                   const std::string& value) {
  // 这里**故意**不走 http_set_header —— 那是覆盖语义。Set-Cookie 这类
  // 允许多值的头必须原样追加，否则第二个 cookie 会把第一个顶掉。
  http_header h;
  h.name = name;
  h.value = value;
  resp_.headers.push_back(h);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::set_cookie(
    const std::string& name, const std::string& value, const std::string& path,
    long max_age, bool http_only, bool secure, const std::string& same_site) {
  return add_header("set-cookie",
                    web_build_cookie(name, value, path, max_age, http_only,
                                     secure, same_site));
}

uvcpp_web_response& uvcpp_web_response::clear_cookie(
    const std::string& name, const std::string& path) {
  // 空值 + Max-Age=0 是让浏览器立刻丢弃该 cookie 的标准做法。值给空串，
  // 是为了让「已删除」这件事在报文里也一目了然。
  return add_header(
      "set-cookie",
      web_build_cookie(name, std::string(), path, 0, true, false,
                       std::string("Lax")));
}

std::string uvcpp_web_response::get_header(const std::string& name,
                                           const std::string& def) const {
  return http_get_header(resp_.headers, name, def);
}

bool uvcpp_web_response::has_header(const std::string& name) const {
  return http_has_header(resp_.headers, name);
}

uvcpp_web_response& uvcpp_web_response::remove_header(const std::string& name) {
  for (size_t i = 0; i < resp_.headers.size();) {
    if (http_name_equal(resp_.headers[i].name, name)) {
      resp_.headers.erase(resp_.headers.begin() +
                          static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
  return *this;
}

uvcpp_web_response& uvcpp_web_response::set_content_type(
    const std::string& ct) {
  return set_header("content-type", ct);
}

std::string uvcpp_web_response::content_type() const {
  return get_header("content-type");
}

// =========================================================================
// 报文体
// =========================================================================

uvcpp_web_response& uvcpp_web_response::body(const char* data, size_t len,
                                             const std::string& ct) {
  // clone_data 才是**深拷贝**。set_data() 存的是外部视图（不持所有权），
  // 拿它去接一个临时 string 会在下一行就悬垂 —— 这是本库最容易被误用的
  // 一个 API，这里必须用对的那个。
  resp_.body.clear();
  if (data && len > 0) {
    resp_.body.clone_data(data, len);
  }
  if (!ct.empty()) {
    set_content_type(ct);
  }
  return *this;
}

uvcpp_web_response& uvcpp_web_response::body(const std::string& s,
                                             const std::string& ct) {
  return body(s.data(), s.size(), ct);
}

uvcpp_web_response& uvcpp_web_response::body_move(uvcpp_buf& src,
                                                  const std::string& ct) {
  resp_.body.move_buf(src);  // 所有权转移，src 之后为空
  if (!ct.empty()) {
    set_content_type(ct);
  }
  return *this;
}

uvcpp_web_response& uvcpp_web_response::text(const std::string& s) {
  return body(s, "text/plain; charset=utf-8");
}

uvcpp_web_response& uvcpp_web_response::html(const std::string& s) {
  return body(s, "text/html; charset=utf-8");
}

uvcpp_web_response& uvcpp_web_response::json(const uvcpp_json& j) {
  return body(uvcpp_json_dump(j), "application/json; charset=utf-8");
}

uvcpp_web_response& uvcpp_web_response::json_str(const std::string& s) {
  return body(s, "application/json; charset=utf-8");
}

uvcpp_web_response& uvcpp_web_response::binary(const char* data, size_t len,
                                               const std::string& ct) {
  return body(data, len, ct);
}

const char* uvcpp_web_response::body_data() const {
  return resp_.body.get_const_data();
}

size_t uvcpp_web_response::body_size() const { return resp_.body.size(); }

bool uvcpp_web_response::body_empty() const { return resp_.body.size() == 0; }

uvcpp_web_response& uvcpp_web_response::clear_body() {
  resp_.body.clear();
  return *this;
}

// =========================================================================
// 状态码 helper
// =========================================================================

namespace {

/**
 * @brief helper 的公共实现：设状态码，body 为空时补一段默认说明。
 *
 * 「body 为空才补」这个判断是刻意的：使用者先 `.json(err)` 再
 * `.bad_request()` 时应当保留他自己的 body；而只调 `.bad_request()` 时
 * 又该有一个至少能读的响应体，而不是空报文。
 */
void apply_status(uvcpp_web_response& r, int code, bool with_default_body) {
  r.status(code);
  if (with_default_body && r.body_empty() && !r.has_header("content-type")) {
    r.body(default_body_for(code), "text/plain; charset=utf-8");
  }
}

}  // namespace

uvcpp_web_response& uvcpp_web_response::ok() {
  apply_status(*this, 200, false);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::created() {
  apply_status(*this, 201, false);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::accepted() {
  apply_status(*this, 202, false);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::no_content() {
  apply_status(*this, 204, false);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::not_modified() {
  apply_status(*this, 304, false);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::redirect(const std::string& url,
                                                 int code) {
  // Location 是重定向的全部意义，必须原样发出（不做百分号编码 ——
  // 使用者可能故意带了已编码的查询串）。
  set_header("location", url);
  apply_status(*this, code, false);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::bad_request() {
  apply_status(*this, 400, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::unauthorized(
    const std::string& challenge) {
  if (!challenge.empty()) {
    // 401 不带 WWW-Authenticate 是不合规的（RFC 7235 §4.1）。
    set_header("www-authenticate", challenge);
  }
  apply_status(*this, 401, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::forbidden() {
  apply_status(*this, 403, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::not_found() {
  apply_status(*this, 404, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::method_not_allowed(
    const std::string& allow) {
  if (!allow.empty()) {
    set_header("allow", allow);  // 405 必须带 Allow（RFC 7231 §6.5.5）
  }
  apply_status(*this, 405, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::conflict() {
  apply_status(*this, 409, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::payload_too_large() {
  apply_status(*this, 413, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::unsupported_media_type() {
  apply_status(*this, 415, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::range_not_satisfiable(
    unsigned long long total) {
  if (total > 0) {
    // 416 的 Content-Range 用 "*" 作范围，total 是资源总长（RFC 7233 §4.4）。
    char buf[64];
    std::snprintf(buf, sizeof(buf), "bytes */%llu", total);
    set_header("content-range", buf);
  }
  apply_status(*this, 416, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::unprocessable() {
  apply_status(*this, 422, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::too_many_requests() {
  apply_status(*this, 429, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::server_error() {
  apply_status(*this, 500, true);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::service_unavailable() {
  apply_status(*this, 503, true);
  return *this;
}

// =========================================================================
// HEAD / 元信息同步
// =========================================================================

void uvcpp_web_response::set_head_only(bool v) { head_only_ = v; }
bool uvcpp_web_response::head_only() const { return head_only_; }

bool uvcpp_web_response::status_forbids_body() const {
  const int code = status_code();
  if (code >= 100 && code < 200) return true;  // 1xx：只有状态行，无 body
  if (code == 204) return true;                // 204 明确定义为无 body
  if (code == 304) return true;                // 304 只能带元信息
  return false;
}

void uvcpp_web_response::sync_meta() {
  if (status_forbids_body()) {
    // 204/304/1xx 不允许有 body。**不自动加 Content-Length** ——
    // RFC 7230 §3.3.2 明确禁止 204 带 CL。使用者显式设了的话保留他的
    // （304 常见做法就是回显原始 CL）。
    resp_.body.clear();
    return;
  }

  const bool has_cl = http_has_header(resp_.headers, "content-length");
  const bool chunked = is_chunked(resp_);

  // HEAD：长度按「本该发出的 body」算，再丢掉 body 本身。
  // 顺序不能反 —— 先丢就算不出长度了。
  const size_t len = resp_.body.size();

  if (!has_cl && !chunked) {
    // 关键修复：body 为空时也要发 Content-Length: 0。
    // 协议层 as to_string() 的条件是 `body.size() > 0`，空体时一个长度头
    // 都不发，在 HTTP/1.1 下等于「长度未知，读到连接关闭为止」。
    set_header("content-length", size_to_string(len));
  }

  if (head_only_) {
    resp_.body.clear();
  }
}

// =========================================================================
// 生命周期
// =========================================================================

void uvcpp_web_response::end() { ended_ = true; }
bool uvcpp_web_response::ended() const { return ended_; }

bool uvcpp_web_response::deferred() const { return deferred_; }
void uvcpp_web_response::set_deferred(bool v) { deferred_ = v; }

void uvcpp_web_response::on_sent(const uvcpp_web_sent_cb& cb) {
  // 叠加而不是覆盖：访问日志与业务代码都会挂这个回调。
  if (cb) sent_cbs_.push_back(cb);
}

size_t uvcpp_web_response::sent_callback_count() const {
  return sent_cbs_.size();
}

void uvcpp_web_response::notify_sent(const uvcpp_web_sent_info& info) {
  if (sent_cbs_.empty()) return;

  // 先整体取走再遍历：回调里如果又调 on_sent()，改的是空列表，不会让本次
  // 遍历的迭代器失效。swap 是 O(1)，不分配。
  std::vector<uvcpp_web_sent_cb> cbs;
  cbs.swap(sent_cbs_);

  for (size_t i = 0; i < cbs.size(); ++i) {
    try {
      cbs[i](info);
    } catch (const std::exception& e) {
      // 发送已经完成，这里再抛就只是把一个已经成功的响应搞成崩溃。
      // 注意**不 return**：一个回调炸了不该连累后面那些。
      UVCPP_LOG_ERROR(log_category::RESPONSE)
          << "on_sent callback threw: " << e.what();
    } catch (...) {
      UVCPP_LOG_ERROR(log_category::RESPONSE) << "on_sent callback threw";
    }
  }
}

void uvcpp_web_response::set_error_handler(const uvcpp_web_error_handler& h) {
  error_cb_ = h;
}

const uvcpp_web_error_handler& uvcpp_web_response::error_handler() const {
  return error_cb_;
}

bool uvcpp_web_response::has_error_handler() const {
  return error_cb_ != nullptr;
}

// =========================================================================
// 逃生口
// =========================================================================

uvcpp_http_response& uvcpp_web_response::raw() {
  sync_meta();
  return resp_;
}

const uvcpp_http_response& uvcpp_web_response::raw() const { return resp_; }

}  // namespace uvcpp
