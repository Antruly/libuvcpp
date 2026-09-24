/**
 * @file src/webapp/uvcpp_web_response.cpp
 * @brief uvcpp_web_response 的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <webapp/uvcpp_web_response.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

// 分片下发要用到传输对象本身（`append_raw` 的接收方、`start_file_transfer()`
// 里的 `set_stop_at_eof`、以及 `file_` 这个 `shared_ptr` 的析构）。
// 头文件里只有前向声明 —— 那是为了让本头不拉 libuv，这里是真正需要完整类型的地方。
#include <webapp/uvcpp_web_file.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_util.h>

// `fail_stream_before_head()` 与 `start_file_transfer()` 要把 errno 风格的状态码
// 翻成 HTTP 状态码，用的是 `UV_ENOENT` / `UV_EACCES` / `UV_EINVAL` / `UV_EOF`
// 这一族常量。本仓的 `src/` 里此前**一处都没用过**这些（`UV_ENOTSUP` /
// `UV_EISDIR` / `UV_ENOTDIR` 同样零引用），所以这个头不是"反正别的头会拉进来"，
// 是真实必需。
#include <uv.h>

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

/// 把长度写成 chunked 帧前缀的小写十六进制。
///
/// 不是 `size_to_string` 换个进制就完事 —— 这条在**每一块**上都要跑，
/// 所以不碰 `ostringstream`（那个会分配一个 locale 相关的流对象）。
/// 与 `size_to_string` 同一手法：手写反向填表再翻过来。
std::string chunk_hex(size_t v) {
  static const char kDigits[] = "0123456789abcdef";
  char tmp[20];  // size_t 最多 16 位十六进制，留够
  int m = 0;
  if (v == 0) {
    tmp[m++] = '0';
  } else {
    while (v > 0 && m < 19) {
      tmp[m++] = kDigits[v & 0xful];
      v >>= 4;
    }
  }
  std::string s;
  s.reserve(static_cast<size_t>(m));
  while (m > 0) s += tmp[--m];
  return s;
}

}  // namespace

// =========================================================================
// 分片读的接收方
// =========================================================================
//
// 它是 `uvcpp_web_file_sink`（传输那一侧要的接口）与 `uvcpp_web_response`
// 之间的适配器：把「从文件里读出来的一片」变成「这条流上的一块 body」。
//
// **必须是嵌套类**：它要读写 `file_raw_` / `pending_bytes_` 这些私有成员，
// 而定义在匿名命名空间里的类访问不到（头文件的成员表里写着这条理由）。
//
// 生命周期由 `unique_ptr<file_slice_sink>` 独占，但**它自己会提前放手**
// —— 见 `~uvcpp_web_response()` 里那段「有条件泄漏」：传输在途时把本对象
// 析构掉，等于在传输的 `sink_` 里留一个指向已释放内存的裸指针。

class uvcpp_web_response::file_slice_sink : public uvcpp_web_file_sink {
 public:
  explicit file_slice_sink(uvcpp_web_response* owner)
      : owner_(owner), in_callback_(false) {}

  /// 属主中途死了：此后所有回调都是空操作。**不析构自己** —— 传输那边还
  /// 握着一个指向本对象的裸指针（`uvcpp_web_file_transfer::sink_`），它会
  /// 一直用到 `on_done` 为止。属主用 `unique_ptr::release()` 把本对象让给
  /// 传输的这段时间（约 48 字节），是本设计明码标价的代价。
  void detach() { owner_ = nullptr; }

  /// 供属主析构时判断：此刻是不是**本对象的回调还在栈上**。是的话就地
  /// delete 会留下一个正在执行的栈帧指向已释放内存。
  bool in_callback() const { return in_callback_; }

  void on_data(const char* data, size_t len) {
    uvcpp_web_response* o = owner_;
    if (o == nullptr) return;

    in_callback_ = true;

    // **头部在这里才上线**：第一片到达时发。这正是 `send_file*` 唯一能
    // 如实报出 404/403 的窗口 —— 头部一上线，状态码就锁死了。
    o->send_stream_head();

    if (o->file_raw_) {
      // Content-Length 模式：body 就是文件字节本身，一个字节都不组帧。
      o->append_raw(data, len);
    } else {
      // 长度未知 ⇒ chunked：每一片组一个 hex 帧。
      o->write_chunk(data, len);
    }

    // **上面两句之后 `o` 可能已经不存在**：`flush_stream()` 在 sink 拒收
    // 时会同步收尾整条流，而收尾会 release 掉上下文里最后一个 hold。
    // 所以这里只碰 `this` 的成员，不碰 `o`。
    in_callback_ = false;
  }

  /// 唯一的背压信号：属主还压着多少字节没送出去。传输每交付完一片问一次。
  ///
  /// 属主已经死了时返回 0 —— 那时传输正被 `cancel()`，下一笔读的完成回调
  /// 就会看到 `abort_requested_` 并走关闭路径。
  size_t backlog() const {
    const uvcpp_web_response* o = owner_;
    return (o == nullptr) ? 0u : o->pending_bytes_;
  }

  void on_done(int status, uint64_t bytes_sent) {
    uvcpp_web_response* o = owner_;
    // **先置空再调**：回调里可能把属主连同上下文一起送走。
    owner_ = nullptr;
    if (o == nullptr) return;

    in_callback_ = true;
    o->file_finished(status, bytes_sent);
    // 同上：这一句之后 `o` 可能已经不在了，只能碰 `this`。
    in_callback_ = false;
  }

 private:
  uvcpp_web_response* owner_;
  bool in_callback_;
};

// =========================================================================
// uvcpp_web_sent_info
// =========================================================================

uvcpp_web_sent_info::uvcpp_web_sent_info()
    : status_code(0), body_bytes(0), connection_id(0), ok(false),
      streamed(false) {}

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_web_response::uvcpp_web_response()
    : head_only_(false),
      ended_(false),
      deferred_(false),
      pending_bytes_(0),
      stream_bytes_(0),
      max_stream_bytes_(default_max_stream_buffer_bytes()),
      streaming_(false),
      head_sent_(false),
      stream_finished_(false),
      stream_done_(false),
      drain_armed_(false),
      stream_status_(0),
      file_raw_(false),
      file_armed_(false),
      file_started_(false),
      file_pending_(false),
      file_first_(0),
      file_last_(0),
      file_to_eof_(false),
      file_status_(0),
      file_gate_(nullptr) {
  // 默认给 200，且 reason phrase 与状态码一致。
  resp_.status_code = http_status::OK;
  resp_.status_message = web_status_text(200);
}

uvcpp_web_response::~uvcpp_web_response() {
  // 分片下发在途时**不能**就地析构接收方：传输那边握着一个指向它的裸指针
  // （`uvcpp_web_file_transfer::sink_`），会一直用到 `on_done` 为止。
  //
  // 两个条件任一成立都要放手：
  // - `file_pending_`：还有字节要交付，
  // - `file_sink_->in_callback()`：此刻正在跑它的回调（那就更不能 delete）。
  //
  // `release()` 是**故意的泄漏**，约 48 字节，只发生在"下载中途连接就没了"
  // 这种情况下。这笔账记在这里：拿一个确定的小泄漏换一次 use-after-free。
  if (file_sink_ && (file_pending_ || file_sink_->in_callback())) {
    file_sink_->detach();
    file_sink_.release();
  }
  // 先取消再放手：`cancel()` 可能同步收尾（`st_paused` 那一支会当场发
  // `on_done`），而那条路会回头调 `file_finished()` —— 那时 `file_` 还在，
  // `stream_detach_file()` 才拿得到要注销的指针。
  if (file_) {
    file_->cancel();
  }
  file_.reset();
}

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
  http_reserve_headers(resp_.headers);
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

// --- `const char*` 形态：一行转发，逻辑全在 http_common 里 ---

uvcpp_web_response& uvcpp_web_response::set_header(const char* name,
                                                   const std::string& value) {
  http_set_header(resp_.headers, name, value);
  return *this;
}

std::string uvcpp_web_response::get_header(const char* name,
                                           const std::string& def) const {
  return http_get_header(resp_.headers, name, def);
}

bool uvcpp_web_response::has_header(const char* name) const {
  return http_has_header(resp_.headers, name);
}

uvcpp_web_response& uvcpp_web_response::remove_header(const char* name) {
  const size_t n = http_name_len(name);
  for (size_t i = 0; i < resp_.headers.size();) {
    if (http_name_iequal(resp_.headers[i].name.data(),
                         resp_.headers[i].name.size(), name, n)) {
      resp_.headers.erase(resp_.headers.begin() +
                          static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
  return *this;
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

uvcpp_web_response& uvcpp_web_response::body(const char* data, size_t len,
                                             const char* ct) {
  resp_.body.clear();
  if (data && len > 0) {
    resp_.body.clone_data(data, len);
  }
  // `ct` 为空指针与空串一视同仁 —— 上面那版的判据是 `!ct.empty()`。
  if (ct != nullptr && *ct != '\0') {
    set_content_type(ct);
  }
  return *this;
}

uvcpp_web_response& uvcpp_web_response::body(const std::string& s,
                                             const char* ct) {
  return body(s.data(), s.size(), ct);
}

uvcpp_web_response& uvcpp_web_response::set_header(const char* name,
                                                   const char* value) {
  http_set_header(resp_.headers, name, value);
  return *this;
}

uvcpp_web_response& uvcpp_web_response::add_header(const char* name,
                                                   const char* value) {
  // 同 `add_header(const std::string&, const std::string&)`：追加语义，
  // 不是覆盖语义 —— Set-Cookie 这类多值头必须原样追加。
  http_reserve_headers(resp_.headers);
  resp_.headers.push_back(http_header{name, value});
  return *this;
}

uvcpp_web_response& uvcpp_web_response::set_content_type(const char* ct) {
  return set_header("content-type", ct);
}

uvcpp_web_response& uvcpp_web_response::body_move(uvcpp_buf& src,
                                                  const std::string& ct) {
  resp_.body.move_buf(src);  // 所有权转移，src 之后为空
  if (!ct.empty()) {
    set_content_type(ct);
  }
  return *this;
}

uvcpp_web_response& uvcpp_web_response::body_share(
    const std::shared_ptr<const std::string>& src, const std::string& ct) {
  // share() 自己会先把旧的放掉（块 + 引用），等价于 body() 里那句 clear()。
  resp_.body.share(src);
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

  const size_t len = resp_.body.size();

  if (!has_cl && !chunked) {
    // 关键修复：body 为空时也要发 Content-Length: 0。
    // 协议层 as to_string() 的条件是 `body.size() > 0`，空体时一个长度头
    // 都不发，在 HTTP/1.1 下等于「长度未知，读到连接关闭为止」。
    set_header("content-length", size_to_string(len));
  }

  // **HEAD 不在这里丢 body**（这里曾经丢）。"HEAD 不发 body"由 HTTP 层的
  // `resp.to_string(/*include_body=*/!ctx.is_head)` 保证，body 本身要留到那
  // 之前 —— 因为**压缩发生在 HTTP 层**（`apply_compression`），它需要真 body
  // 才能算出 GET 会发的那个长度。在这里丢掉的后果是一整条因果链：
  // body 变 0 → 压缩的尺寸门直接返回 → HEAD 报的是**未压缩**的长度，而 GET
  // 报压缩后的长度，RFC 9110 §9.3.2 要的"HEAD 与 GET 相同的头"当场不成立。
  // 拿 HEAD 探长度再按长度读满的客户端会一直等到超时。
}

// =========================================================================
// 生命周期
// =========================================================================

void uvcpp_web_response::end() {
  ended_ = true;

  if (!streaming_ || stream_finished_) return;
  stream_finished_ = true;

  if (!head_only_ && !file_raw_ && !file_armed_) {
    // 终止块是 chunked 的**帧**，不是 body 的一部分 —— 长度 0 的那一块
    // 正是它。所以它跟 body 是否为空无关：没有它这个报文就没有终止，
    // keep-alive 上对端只能一直等下一个块。
    //
    // `file_raw_` 为真 = 这条流是 `Content-Length` 模式（`send_file_range`
    // 那条路）。**没有终止块这回事** —— 长度已经把报文切好了，多发这 5 个
    // 字节就是往对端塞垃圾，而它会算进 body（对端按 CL 读，多出来的字节
    // 留在缓冲里，下一条请求的解析从此错位）。
    //
    // `file_armed_` 为真 = 这次 `end()` 只是**记下了意图**，真正的下发还没
    // 起步（静态服务那条路的必然次序就是"`send_file_range()` → `end()`"）。
    // 终止块**不能在这里发** —— 它必须排在**最后一个分片之后**，而那个位置
    // 只有 `file_finished()` 知道。在这里抢先发一个 `0\r\n\r\n`，对端读到的
    // 是"消息到此为止"，紧随其后的整份文件全成了下一条报文里的垃圾。
    // （`file_finished()` 自己会补终止块，所以这里少发的那一个不会丢。）
    //
    // h2 上**没有终止块这回事**：流的终点是带 END_STREAM 的那一帧，由 http
    // 层在 `end_stream()` 里发。这里补的 5 个字节会原样进响应体。
    if (!on_h2_stream()) {
      pending_buf_.append("0\r\n\r\n");
      pending_bytes_ += 5;
      if (pending_bytes_ > max_stream_bytes_) drain_armed_ = true;
    }
  }

  if (sink_ && !pending_buf_.empty()) {
    // **这一句之后不得再碰任何成员** —— flush_stream() 可能在同步路径上
    // 就把整条流收尾掉，而收尾回调会还掉上下文最后一个 hold。
    flush_stream();
    return;
  }
  maybe_finish_stream();
}

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

  // ★ 把**容量**还给 `sent_cbs_`：上面那次 `swap` 把缓冲搬到了这个局部向量上，
  //   出函数就跟着析构 —— 于是下一条请求又得 `_M_realloc_insert` 一次。清掉
  //   已经跑完的回调（`clear()` 留容量）再换回去即可。
  //   回调里如果又调了 `on_sent()`，`sent_cbs_` 就不是空的 ⇒ **不还**（那次新
  //   挂的回调得留着），拿回容量即可。两种情形的可观察行为一字不动。
  //   ★ 这句 `cbs.clear()` 同时是"回调不串到下一条请求"的**主守卫**
  //   （`yield_tables()` 里还有第二道网）。两句互为备份、单删都观察不到。
  cbs.clear();
  if (sent_cbs_.empty()) sent_cbs_.swap(cbs);
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
// 流式（chunked）响应
// =========================================================================
//
// 状态机（五个布尔量，各自管一件事，别合并）：
//
//   streaming_        —— 这条响应进入了流模式（begin_chunked 之后恒真）
//   head_sent_        —— 头部已经交给 sink（`send_stream_head()` 里置，之后不再发）
//                        它由三条路调 —— `pump_stream()`、`file_slice_sink::on_data()`
//                        的第一片、以及 `file_finished()` 的收尾处 —— 但幂等，
//                        先置位再调，所以三条路都安全。
//   stream_finished_  —— end() 已经调过（终止块已进待发队列）
//   stream_done_      —— 已经收尾过（**幂等闸门**，收尾路径有多条）
//   drain_armed_      —— 越过过高水位，等着降到半水位时回调一次
//
// 收尾的**唯一**判据是 stream_finish_ready()：头部发了、终止块发完了、
// 攒下的和路上的都清了。任何一处状态变了都要问一次它。

size_t uvcpp_web_response::default_max_stream_buffer_bytes() {
  // 1 MiB。与 uvcpp_web_upload 的 kMaxPendingBytes 取同一个量级 —— 那是
  // "收"这一侧的单槽上限，这里是"发"这一侧的水位线。两个数的含义不同
  // （一个是硬上限、一个是触发点），所以不复用同一个常量。
  return 1024u * 1024u;
}

void uvcpp_web_response::set_max_stream_buffer_bytes(size_t n) {
  max_stream_bytes_ = n;
}

size_t uvcpp_web_response::max_stream_buffer_bytes() const {
  return max_stream_bytes_;
}

void uvcpp_web_response::set_stream_sink(uvcpp_web_stream_sink* sink) {
  // reset 而不是再 new 一个：重复安装时旧的那个应当被销毁。
  sink_.reset(sink);
}

void uvcpp_web_response::set_stream_finished_cb(
    const std::function<void(int)>& cb) {
  stream_done_cb_ = cb;
}

bool uvcpp_web_response::streaming() const { return streaming_; }

size_t uvcpp_web_response::stream_bytes_written() const {
  return stream_bytes_;
}

size_t uvcpp_web_response::stream_pending_bytes() const {
  return pending_bytes_;
}

void uvcpp_web_response::begin_chunked(const std::string& content_type) {
  if (streaming_) {
    UVCPP_LOG_WARN(log_category::RESPONSE)
        << "begin_chunked() 重复调用，已忽略（这条响应已经在流式状态）";
    return;
  }
  if (head_sent_) {
    UVCPP_LOG_WARN(log_category::RESPONSE)
        << "begin_chunked() 在头部已经发出之后被调用，已忽略";
    return;
  }
  if (resp_.body.size() > 0) {
    // 清掉而不是留着：留着的话它既不会被写出去（流式的字节只由
    // write_chunk 产出），又会让 sync_meta() 算出一个没有意义的长度。
    // 而**静默**清掉更坏 —— 使用者刚设的 body 会凭空消失。所以 WARN。
    UVCPP_LOG_WARN(log_category::RESPONSE)
        << "begin_chunked() 之前已经设过 body（" << resp_.body.size()
        << " 字节），已清空 —— 流式响应的 body 只能由 write_chunk() 产出";
    resp_.body.clear();
  }

  // CL 与 transfer-encoding 不得共存（RFC 7230 §3.3.2）。**先删后设**：
  // 顺序反过来的话 to_string() 会同时输出两个头。
  //
  // h2 上这个头**不会上线**（`build_response_nv` 把连接专属头一律剥掉），
  // 但仍然要设：它同时是"这条响应没有 content-length"这个内部不变式的载体 ——
  // `sync_meta()` 和 `apply_compression()` 都靠 `is_chunked()` 判它。h2 流式
  // 响应的定界本来就是 END_STREAM，不发 CL 正是对的。
  remove_header("content-length");
  set_header("transfer-encoding", "chunked");
  if (!content_type.empty()) set_content_type(content_type);

  // 六个状态赋值抽成 reset_stream_state()：分片下发那条路（`arm_file_transfer`）
  // 要用**同一份**清单。分成两份写必然漂移，而漏掉哪一项的症状都很隐蔽
  // （比如漏了 `stream_done_`，第二条流一上来就判自己"已经收尾过"）。
  reset_stream_state();
  // pending_buf_ / pending_bytes_ 不清：没 begin 过就 write_chunk 会被
  // 拒绝，所以此刻它们必然为空 —— 写一句"顺手清一下"反而像是在掩盖
  // 一个不该出现的状态。
  //
  // max_stream_bytes_ **不重置**：在 begin_chunked() 之前设水位是合法
  // 用法（那时候还没有 pending，也就没有"设晚了"的问题）。
}

bool uvcpp_web_response::write_chunk(const std::string& data) {
  return write_chunk(data.data(), data.size());
}

bool uvcpp_web_response::write_chunk(const char* data, size_t len) {
  if (!streaming_) {
    UVCPP_LOG_WARN(log_category::RESPONSE)
        << "write_chunk() 在 begin_chunked() 之前被调用，已忽略"
           "（先调 begin_chunked()）";
    return false;
  }
  if (stream_finished_) {
    UVCPP_LOG_WARN(log_category::RESPONSE)
        << "write_chunk() 在 end() 之后被调用，已忽略";
    return false;
  }
  if (len == 0 || data == nullptr) {
    // **零长块不发帧。** 长度为零的 chunked 帧在字节上与终止块**逐字节
    // 相同**（都是 "0\r\n\r\n"），发出去等于提前结束这条流，而对端会
    // 认为消息已经完整 —— 后面的内容全被当成下一个报文的一部分。
    return true;
  }

  // 口径与 uvcpp_web_sent_info::body_bytes 一致：这个数描述「GET 本该
  // 发出多少」，所以 HEAD 上一字节不发也照样计入。
  stream_bytes_ += len;

  if (head_only_) {
    // HEAD 的头部必须与 GET **逐字节相同**（含 transfer-encoding:
    // chunked），而 body 一个字节都不发 —— 这是协议合法且正确的。
    return true;
  }

  if (on_h2_stream()) {
    // h2 上**没有 chunked 帧** —— 边界由 DATA 帧自己划，这一层给的是裸字节。
    // 照 h1 组帧的话，hex 长度与那两组 CRLF 会成为响应体里实打实的内容：
    // 一条 SSE 流的每个事件前面都挂着 `1a\r\n`，而 `\r\n\r\n` 恰好会被
    // `EventSource` 当成事件分隔符 —— 对端不报错，只是收到一堆垃圾。
    pending_buf_.append(data, len);
    pending_bytes_ += len;
  } else {
    const std::string hex = chunk_hex(len);
    pending_buf_.append(hex);
    pending_buf_.append("\r\n");
    pending_buf_.append(data, len);
    pending_buf_.append("\r\n");
    pending_bytes_ += hex.size() + len + 4;
  }

  if (pending_bytes_ > max_stream_bytes_) drain_armed_ = true;
  // 水位判定要在 flush **之前**取 —— flush 之后就不许再读成员了。
  const bool room = pending_bytes_ <= max_stream_bytes_;

  if (sink_ && !pending_buf_.empty()) {
    // **这一句之后不得再碰任何成员。**
    flush_stream();
    return room;
  }
  return room;
}

void uvcpp_web_response::on_drain(const std::function<void()>& cb) {
  // 叠加，与 on_sent 同族。覆盖语义在这里是个陷阱：一个中间件装了水位
  // 回调之后，业务代码再装一个就会把它静默顶掉。
  if (cb) drain_cbs_.push_back(cb);
}

void uvcpp_web_response::flush_stream() {
  if (!sink_) return;
  if (pending_buf_.empty()) return;

  std::string bytes;
  bytes.swap(pending_buf_);  // O(1)，不拷贝
  const size_t n = bytes.size();

  const int rc = sink_->stream_write(
      bytes, [this, n](int status) { stream_write_done(n, status); });
  if (rc != 0) {
    // **sink 说"没受理"时不会调 done**（`uvcpp_http_server::write_stream`
    // 在连接已经不在登记表里时就是直接 return UV_ECANCELED 的，与它自己
    // 头文件里那句承诺相反）。所以这一笔必须由我们**自己**结算，否则
    // pending_bytes_ 永远减不回 0，这条流再也不会收尾，而挂在上面的
    // context 也永远不会被 release。
    stream_write_done(n, rc);
  }
  // rc == 0 时也不能再碰成员：done 有可能是同步调的。
}

void uvcpp_web_response::stream_write_done(size_t n, int status) {
  if (n >= pending_bytes_) {
    pending_bytes_ = 0;
  } else {
    pending_bytes_ -= n;
  }

  // 只记**第一次**失败的状态：收尾时要报的是"为什么这条流断了"，
  // 而后续的失败多半是同一原因（连接已断）的连锁反应。
  if (status != 0 && stream_status_ == 0) stream_status_ = status;

  if (stream_finish_ready()) {
    // **这一句之后不得再碰任何成员。**
    maybe_finish_stream();
    return;
  }

  // 背压唤醒：分片下发因"下游还压着东西"而停读（transfer 每交付完一片问一次
  // `backlog()`，也就是这里的 `pending_bytes_`），现在读方向的缓冲腾空了，
  // 正是叫醒它的时候。
  //
  // **必须在任何用户 drain 回调运行之前抓到手**：那些回调是用户代码，它们
  // 可以把整条流收尾掉，那之后 `this`（连带 `file_` 成员）可能已经不存在。
  // 抓到局部的 `shared_ptr` 之后，后面就只跟这个局部打交道了。
  //
  // 判据是 `pending_bytes_ == 0` 而不是"降到半水位"：transfer 停读的那一刻
  // `backlog()` 已经 >= 高水位，这里只要还有一字节没出去就再等等 —— 唤醒
  // 一次就够，多唤几次只是白白提交空读。
  std::shared_ptr<uvcpp_web_file_transfer> wake;
  if (file_pending_ && file_ && pending_bytes_ == 0) wake = file_;

  // 边沿触发：越过水位时 arm，降到**半水位**才回调一次。回调用半水位而
  // 不是高水位本身，是为了让回调里可以放心地一次灌一批而不是灌一块。
  if (drain_armed_ && pending_bytes_ <= max_stream_bytes_ / 2) {
    drain_armed_ = false;
    // **先换到栈上再调**：回调里重新 on_drain() 会给成员重新赋值，而
    // `std::function::operator=` 会析构旧目标 —— 正在执行的那个闭包
    // （连同它的捕获）会在自己跑完之前被释放。与 uvcpp_fs 那 36 处
    // 是同族，这里用 swap 而不是拷贝（不额外分配）。
    std::vector<std::function<void()> > cbs;
    cbs.swap(drain_cbs_);
    for (size_t i = 0; i < cbs.size(); ++i) {
      try {
        cbs[i]();
      } catch (const std::exception& e) {
        UVCPP_LOG_ERROR(log_category::RESPONSE)
            << "on_drain callback threw: " << e.what();
      } catch (...) {
        UVCPP_LOG_ERROR(log_category::RESPONSE) << "on_drain callback threw";
      }
    }
  }
  // **这里刻意不调 maybe_finish_stream()。** 能让这条流变"可以收尾"的
  // 只有两件事 —— end() 和 flush_stream() —— 它们自己都会问一次。而
  // drain 回调是用户代码，它可以把整条流收尾掉；在那之后再碰 `this`
  // 就是读一个可能已经析构的对象。HEAD 上 drain 永远不会 arm（pending
  // 恒 0），所以那条路今天走不到 —— 但把安全性寄托在这句推理上，不如
  // 让它在结构上成立。
  //
  // **`resume()` 必须是最后一句。** 上面那段推理同时说明了为什么它只能放在
  // 这里：`wake` 是局部的 `shared_ptr`，不再依赖 `this` 还活着。放在 drain
  // 回调**之前**的话，回调里那次收尾会把 `file_` 成员一起带走，唤醒就落空
  // （分片下发从此停在半路，而这条流已经结束，没人会再来叫它）。
  if (wake) wake->resume();
}

bool uvcpp_web_response::stream_finish_ready() const {
  // `!file_pending_` 是分片下发的收尾闸门：传输还在跑的时候，这条流
  // **不许**收尾。少了它，一个「end() 之后传输才交付第一片」的次序
  // （静态服务那条路就是 —— `send_file_range()` 在处理函数里记录意图，
  // 真正的读盘在 `pump_stream()` 里才起步）会当场把流收尾掉：终止块
  // 发出去、context 被 release、而文件还在读 —— 后续的片全写进一条已经
  // 结束的流里。
  return streaming_ && stream_finished_ && !stream_done_ && head_sent_ &&
         !file_pending_ && pending_buf_.empty() && pending_bytes_ == 0;
}

void uvcpp_web_response::maybe_finish_stream() {
  if (!stream_finish_ready()) return;

  // **先落闸再动手。** sink_->stream_end() 会唤醒写队列（`pump_write`），
  // 那些完成回调有可能同步回到本函数；stream_done_ 是先落下的闸门，
  // 让重入变成空操作。
  stream_done_ = true;

  if (sink_) {
    // 失败时**关连接**：body 已经被截断，而 chunked 的帧本身不会告诉
    // 对端"我少了东西"（终止块可能压根没发出去）。关连接是唯一的信号。
    // 成功时不必关 —— 终止块让这条报文自定界，keep-alive 照常可用。
    sink_->stream_end(stream_status_ != 0);
  }

  // 状态与回调先取到栈上：`cb(st)` 里会 notify_sent + release 上下文，
  // 那之后 `this` 可能已经不在了。
  std::function<void(int)> cb = stream_done_cb_;
  const int st = stream_status_;
  if (cb) cb(st);
}

void uvcpp_web_response::pump_stream() {
  if (!streaming_ || !sink_) return;

  if (file_armed_) {
    // **两种结局都在这里返回，都不能由本函数发头部。**
    //
    // 起步成功 ⇒ `file_pending_` 为真，头部由第一片（`file_slice_sink::
    // on_data`）负责发 —— 那正是「文件打不开还能报 404」的窗口所在。
    // 起步失败 ⇒ `start_file_transfer()` 内部已经走完 `file_finished()`，
    // 这条流已经收尾（甚至 `this` 可能已经不在了），一个成员都碰不得。
    //
    // `file_started_` 是幂等闸门：pump_stream() 会被调多次（每次 flush
    // 之后、每次 end() 之后），而重复提交一次传输等于把同一个文件读两遍、
    // 两遍都写进同一条流。
    if (!file_started_) (void)start_file_transfer();
    return;
  }

  send_stream_head();  // 整包流式（write_chunk）那条路
  if (!pending_buf_.empty()) {
    // **这一句之后不得再碰任何成员。**
    flush_stream();
    return;
  }
  maybe_finish_stream();
}

// =========================================================================
// 流式的内部动作（分片下发）
// =========================================================================

void uvcpp_web_response::reset_stream_state() {
  // 只碰"这条流刚开始"的六个量。`file_*` 一族**不在这里** —— 它们由
  // `arm_file_transfer()` 自己管，混进来就会让 `begin_chunked()` 顺手
  // 清掉一条已经 arm 好的下发。
  streaming_ = true;
  head_sent_ = false;
  stream_finished_ = false;
  stream_done_ = false;
  stream_status_ = 0;
  stream_bytes_ = 0;
}

bool uvcpp_web_response::append_raw(const char* data, size_t len) {
  if (!streaming_) {
    UVCPP_LOG_WARN(log_category::RESPONSE)
        << "append_raw() 在流式开始之前被调用，已忽略";
    return false;
  }
  // **判的是 `stream_done_` 而不是 `stream_finished_`。** 这两个差一个词，
  // 后果却是整份文件：静态服务那条路的必然次序是「`send_file_range()` 记下
  // 区间 → `end()` 收尾这条消息」，而 `end()` 会把 `stream_finished_` 置真。
  // 按 `stream_finished_` 判的话，文件的第一片到达时就会撞上守卫 —— **整份
  // 文件被静默丢弃**，线上只剩一个空 body。
  if (stream_done_) {
    UVCPP_LOG_WARN(log_category::RESPONSE)
        << "append_raw() 在这条流已经收尾之后被调用，已忽略";
    return false;
  }
  if (len == 0 || data == nullptr) {
    // 与 write_chunk 同理，但这里更直白：零字节什么都追加不了。
    return true;
  }

  // 口径与 write_chunk 一致：描述「GET 本该发出多少」。
  stream_bytes_ += len;

  if (head_only_) {
    // HEAD：头部与 GET 逐字节相同，body 一个字节都不发。
    return true;
  }

  // **不组帧** —— 这就是它与 write_chunk 的唯一区别。
  pending_buf_.append(data, len);
  pending_bytes_ += len;

  if (pending_bytes_ > max_stream_bytes_) drain_armed_ = true;
  // 水位判定要在 flush **之前**取 —— flush 之后就不许再读成员了。
  const bool room = pending_bytes_ <= max_stream_bytes_;

  if (sink_ && !pending_buf_.empty()) {
    // **这一句之后不得再碰任何成员。**
    flush_stream();
    return room;
  }
  return room;
}

bool uvcpp_web_response::can_send_file(const char* what) const {
  // **刻意不要求 `streaming_`。** `send_file*` 的契约是"自足"——用户批准
  // 的用法里既没有 `begin_chunked()` 也没有 `end()`（见头文件的分片读一节），
  // 流模式由 `arm_file_transfer()` 自己建立。要求了它，主用法会当场被拒。
  if (file_armed_) {
    UVCPP_LOG_WARN(log_category::RESPONSE)
        << what << "() 重复调用，已忽略（这条响应已经在发一个文件）";
    return false;
  }
  if (stream_bytes_ > 0) {
    // 已经发过 body 就不能再改主意：`Content-Length` 与
    // `transfer-encoding` 的取舍在第一个字节之前就定死了。
    UVCPP_LOG_WARN(log_category::RESPONSE)
        << what << "() 在已经写过 " << stream_bytes_
        << " 字节 body 之后被调用，已忽略";
    return false;
  }
  if (stream_finished_) {
    UVCPP_LOG_WARN(log_category::RESPONSE)
        << what << "() 在 end() 之后被调用，已忽略";
    return false;
  }
  return true;
}

void uvcpp_web_response::arm_file_transfer(
    const std::string& path, uint64_t first, uint64_t last, bool to_eof, bool raw,
    const std::function<void(int, uint64_t)>& done) {
  file_path_ = path;
  file_first_ = first;
  file_last_ = last;
  file_to_eof_ = to_eof;
  file_raw_ = raw;
  file_done_cb_ = done;
  file_status_ = 0;
  file_armed_ = true;

  // 头部的两种形状（RFC 7230 §3.3.2：CL 与 TE 不得共存）。**先删后设** ——
  // 顺序反过来的话 `to_string()` 会同时吐出两个头。
  if (raw) {
    // 长度已知 ⇒ body 就是文件字节本身，一个字节都不组帧。
    remove_header("transfer-encoding");
    if (!to_eof) {
      // 空区间（first = 1, last = 0）靠无符号回绕得到 0，正好是"这段是空的"。
      set_header("content-length", size_to_string(last - first + 1u));
    }
  } else {
    // 长度未知 ⇒ chunked，读到 EOF 收尾。
    remove_header("content-length");
    set_header("transfer-encoding", "chunked");
  }

  if (!file_sink_) file_sink_.reset(new file_slice_sink(this));
  reset_stream_state();
}

void uvcpp_web_response::send_stream_head() {
  if (head_sent_) return;
  // **先置位再调**：`stream_begin()` 无论成功失败，头部都不该再试第二次
  // （重试等于往连接里插一段重复的头部块）。
  head_sent_ = true;
  if (!sink_) return;
  const int rc = sink_->stream_begin(resp_);
  if (rc != 0 && stream_status_ == 0) stream_status_ = rc;
}

void uvcpp_web_response::fail_stream_before_head(int status) {
  if (stream_status_ == 0) stream_status_ = status;

  // 只有这一层能做的映射：errno 风格的状态码 → HTTP 语义。
  int code = 500;
  switch (status) {
    case UV_ENOENT:
    case UV_ENOTDIR:
    case UV_EISDIR:  // 目录：对"要一个文件"的请求就是"没有"
      code = 404;
      break;
    case UV_EACCES:
    case UV_EPERM:
      code = 403;
      break;
    default:
      // 含 UV_ENOTSUP（这条 sink 给不出 loop）：那是服务端配置问题，
      // 不是"你要的东西不存在"。
      code = 500;
      break;
  }

  remove_header("content-length");
  remove_header("transfer-encoding");
  set_content_type("text/plain; charset=utf-8");
  resp_.status_code = static_cast<http_status>(code);
  resp_.status_message = web_status_text(code);

  const std::string text = default_body_for(code);
  // HEAD 上也照设 —— HEAD 的头必须与 GET 逐字节相同，只是不发 body。
  set_header("content-length", size_to_string(text.size()));
  // 这条流已经断在半路了，keep-alive 上的"下一个报文从哪儿开始"无从谈起。
  set_header("connection", "close");

  // body **不走 `append_raw()`**：那个函数判的是"这条流还在不在"，而此刻
  // 我们正要把它改写成完好的错误响应 —— 走它反而会被自己的守卫挡下。
  // 直接进待发缓冲，与 `write_chunk` 的记账口径保持一致。
  resp_.body.clear();
  stream_bytes_ += text.size();
  if (!head_only_) {
    pending_buf_.append(text);
    pending_bytes_ += text.size();
  }

  // 这一条流到此为止：终止块不发（这是 CL 模式，本来也没有终止块，
  // 而且"消息完整"的声明与一个 404 的 body 并不矛盾 —— CL 恰好等于
  // 我们发出去的字节数）。
  stream_finished_ = true;
  send_stream_head();

  // **只改写，不 flush、不收尾。** 收尾交给 `file_finished()` 在 `cb`
  // 跑完之后做 —— `flush_stream()` 在 sink 拒收时会同步收尾整条流并
  // release 上下文，那会毁掉"回调必须先于任何 flush"这条纪律。
}

int uvcpp_web_response::start_file_transfer() {
  // **放在最开头**：`file_finished()` 判的是这两个量，任何一条失败路径
  // 都要经过它。
  file_started_ = true;
  file_pending_ = true;

  if (file_path_.empty() || !file_sink_) {
    file_finished(UV_EINVAL, 0);
    return 1;
  }
  if (head_only_) {
    // HEAD 不读盘：头部在这里发出去（`file_finished` 里那一支），
    // `Content-Length` 已经由 `arm_file_transfer()` 按区间设好了。
    file_finished(0, 0);
    return 1;
  }
  if (sink_ == nullptr || sink_->stream_loop() == nullptr) {
    // 这条 sink 没有 loop（基类的默认实现就是返回 nullptr）——
    // 提交不了 `uv_fs_*`，只能明确失败。判据是 `stream_loop()` 而不是
    // "有没有装那三个虚函数"：三个空操作默认实现是**合法**的，
    // 真正的门槛是"能不能拿到一个 loop"。
    file_finished(UV_ENOTSUP, 0);
    return 1;
  }

  std::shared_ptr<uvcpp_web_file_transfer> t(
      new uvcpp_web_file_transfer(sink_->stream_loop(), file_sink_.get()));
  // 必须在 `start()` **之前**挂（`set_chunk_gate` 在起跑后拒绝换闸门 ——
  // 那时手里可能正攥着旧闸门的名额，换掉就没对象可还了）。
  if (file_gate_ != nullptr) t->set_chunk_gate(file_gate_);
  // 必须在 `start()` **之前**设：状态机一开始跑这个标志就只是给已经提交
  // 出去的那些读当判据用了。
  t->set_stop_at_eof(file_to_eof_);
  // 同样必须在 `start()` 之前挂上：sink 那边要按连接登记它，好在对端断开时
  // `cancel()`。start() 之后再挂就晚了 —— 第一片可能已经在路上了。
  sink_->stream_attach_file(t.get());
  file_ = t;

  const int rc = t->start(file_path_, file_first_, file_last_);
  if (rc != 0) {
    // 同步失败：按 transfer 的契约**不会有任何回调**，所以这一笔得自己收尾。
    //
    // **`file_` 保留。** 清掉它会让 `file_finished()` 因 `file_ == nullptr`
    // 而跳过 `stream_detach_file()`，框架那边的登记表里就留下一个指向
    // `file_sink_` 的悬垂裸指针。transfer 已经 st_done，留着它只是一个空的
    // shared_ptr 引用计数，代价远小于悬垂。
    file_finished(rc, 0);
    return 1;
  }
  return 0;
}

void uvcpp_web_response::file_finished(int status, uint64_t bytes_sent) {
  file_pending_ = false;

  // 先把框架那边的登记撤掉：此刻传输很可能还活着（本函数就是从它的
  // `on_done` 里进来的），而它手里握着 `file_sink_` 的裸指针。
  if (sink_ && file_) sink_->stream_detach_file(file_.get());

  // **在任何 flush 之前**把回调和状态取到栈上。`flush_stream()` 在 sink
  // 拒收时会同步收尾整条流，而收尾会 release 掉上下文 —— 那之后本对象
  // 可能已经不存在，成员也就读不得了。
  // （`std::function::operator=` 会析构旧目标，所以是 swap 而不是读一份。）
  std::function<void(int, uint64_t)> cb;
  cb.swap(file_done_cb_);

  if (status != 0 && stream_status_ == 0) stream_status_ = status;

  if (status != 0 && !head_sent_) {
    // 第一个字节之前就失败：这条流还有救，改写成错误响应。
    fail_stream_before_head(status);
  } else {
    if (status == 0 && !head_only_ && !file_raw_ && !on_h2_stream()) {
      // chunked 的终止块。**`end()` 帮不上忙** —— 它在 `stream_finished_`
      // 上提前返回，而 `send_file*` 的契约是"自足"（用户不必补一句 `end()`），
      // 所以这里必须自己补。
      pending_buf_.append("0\r\n\r\n");
      pending_bytes_ += 5;
      if (pending_bytes_ > max_stream_bytes_) drain_armed_ = true;
    }
    // **失败那一支不发终止块**：终止块是"这条消息是完整的"的声明，而此刻
    // body 已经被截断。`stream_status_` 非 0 会让 `maybe_finish_stream()`
    // 让 sink 关掉连接 —— 那才是诚实的信号。
    stream_finished_ = true;
    send_stream_head();
  }

  // **回调必须在任何 flush 之前。** 上面那句 `flush_stream()` 有可能同步
  // 收尾整条流并析构本对象，回调放在它后面就是在赌。
  if (cb) cb(status, bytes_sent);

  if (sink_ && !pending_buf_.empty()) {
    // **这一句之后不得再碰任何成员。**
    flush_stream();
    return;
  }
  maybe_finish_stream();
}

// =========================================================================
// 分片读下发（公开入口）
// =========================================================================
//
// 三个入口都**只记录意图**，不调 `pump_stream()`：真到那时框架还没装 sink
// （没有 loop 就提交不了 `uv_fs_*`）。起步点是 `pump_stream()` 里的
// `file_armed_` 那一支。

void uvcpp_web_response::send_file(
    const std::string& path, const std::function<void(int, uint64_t)>& done) {
  if (!can_send_file("send_file")) return;
  // 长度未知 ⇒ chunked ⇒ 读到 EOF 就是终点。
  //
  // `last` 必须是 `UINT64_MAX - 1` 而**不是** `UINT64_MAX`：
  // `uvcpp_web_file_transfer::submit_read()` 里的 `remain = last_ - offset_ + 1u`
  // 在后者上第一个切片就溢出成 0，于是当场收尾、一个字节都不读。
  arm_file_transfer(path, 0, UINT64_MAX - 1u, true, false, done);
}

void uvcpp_web_response::send_file(
    const std::string& path, int64_t known_size,
    const std::function<void(int, uint64_t)>& done) {
  if (!can_send_file("send_file")) return;

  if (known_size < 0) {
    arm_file_transfer(path, 0, UINT64_MAX - 1u, true, false, done);
    return;
  }
  if (known_size == 0) {
    // 空区间（`first > last`）—— **仍然会去 open**，所以 ENOENT 之类的
    // 错误照常以 404 报出来，而不是静默发出一个没有来源的空 body。
    arm_file_transfer(path, 1, 0, false, true, done);
    return;
  }
  arm_file_transfer(path, 0, static_cast<uint64_t>(known_size) - 1u, false, true,
                    done);
}

void uvcpp_web_response::set_file_chunk_gate(uvcpp_web_work_limit* limit) {
  // 只是记下来 —— 真正的接线在 `start_file_transfer()` 里（那时才有传输对象
  // 可挂）。放在这里而不是让调用方直接够到传输，是因为传输是响应自己 `new`
  // 的，调用方（静态模块）手上没有它。
  file_gate_ = limit;
}

void uvcpp_web_response::send_file_range(
    const std::string& path, uint64_t first, uint64_t last,
    const std::function<void(int, uint64_t)>& done) {
  if (!can_send_file("send_file_range")) return;
  arm_file_transfer(path, first, last, false, true, done);
}

// =========================================================================
// 逃生口
// =========================================================================

uvcpp_http_response& uvcpp_web_response::raw() {
  sync_meta();
  return resp_;
}

const uvcpp_http_response& uvcpp_web_response::raw() const { return resp_; }

void uvcpp_web_response::adopt_tables(
    http_headers& spare_headers, std::vector<uvcpp_web_sent_cb>& spare_sent) {
  // 两边都空才换得干净：回收槽里只会放 `yield_tables()` 清空过的表。
  resp_.headers.swap(spare_headers);
  sent_cbs_.swap(spare_sent);
}

void uvcpp_web_response::yield_tables(
    http_headers& spare_headers, std::vector<uvcpp_web_sent_cb>& spare_sent) {
  // 先清元素、留下容量。响应到这里已经发完了，表里剩的只是"刚才用过的那几个
  // 头"；`clear()` 掉它们之后这块缓冲就是一条干净的、有容量的空表。
  //
  // ★ `sent_cbs_.clear()` 是**第二道网**，不是主守卫。正常路径上
  //   `notify_sent()` 早把表 `swap` 到局部、跑完、又还了个空的回来。
  //   实测（`yield_tables()` 入口无条件打点，全树 ctest 跑一遍）：3593 次
  //   调用**全部** `sent_cbs_ == 0`，同一份打点里 `hdrs` 是 5/6/7 ⇒ 头那半句
  //   确实在干活。而"注册了 `on_sent` 却从没发出去"的路径（扣住 next +
  //   停机强拆）**根本不走 `context_finished`**，所以也到不了这里。
  //   留着是因为它是**构造上的**兜底：哪天多一条"没发就收场"的路，这一句
  //   就能挡住旧回调串到下一条请求上去 —— 代价一行，而串味的现场极难查。
  resp_.headers.clear();
  sent_cbs_.clear();
  resp_.headers.swap(spare_headers);
  sent_cbs_.swap(spare_sent);
}

}  // namespace uvcpp
