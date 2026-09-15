/**
 * @file src/webapp/uvcpp_web_request.cpp
 * @brief `uvcpp_web_request.h` 的实现。
 */

#include <webapp/uvcpp_web_request.h>

#include <cstdlib>

// 只为**缓冲式** multipart（普通路由上的 `form()` / `file()`，见
// `parse_multipart_form()`）。放在 `.cpp` 里而不是头文件里：那个头不必被每个
// 包含请求头的编译单元拖进来。
#include <webapp/uvcpp_web_multipart.h>

namespace uvcpp {

namespace {

/**
 * @brief 解析非负十进制整数。
 *
 * 不用 `strtoull`：它在溢出时返回 ULLONG_MAX 并把 errno 设成 ERANGE，
 * 而 `Content-Length: 99999999999999999999` 这种输入正好走那条路 ——
 * 一个攻击者可控的值被静默夹到"最大值"，不如直接判非法。
 */
bool parse_size(const std::string& s, size_t& out) {
  const std::string t = web_trim(s);
  if (t.empty() || t.size() > 19) return false;
  size_t v = 0;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] < '0' || t[i] > '9') return false;
    v = v * 10 + static_cast<size_t>(t[i] - '0');
  }
  out = v;
  return true;
}

/// 解析 "0.5" / "1" / ".8" 这样的 q 值，夹到 [0,1]。非法返回 1.0（缺省质量）。
double parse_qvalue(const std::string& s) {
  const std::string t = web_trim(s);
  if (t.empty()) return 1.0;
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end == t.c_str()) return 1.0;  // 一个字符都没吃掉 → 不是数字
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

/// `Content-Type` 的基础类型（小写，不含 `; charset=...`）。
std::string mime_base(const std::string& content_type) {
  const size_t semi = content_type.find(';');
  return web_to_lower(web_trim(semi == std::string::npos
                                   ? content_type
                                   : content_type.substr(0, semi)));
}

/**
 * @brief 把一份 multipart 报文收进两个内存容器：字段与文件。
 *
 * 用的是**和上传路径完全同一个** `uvcpp_web_multipart`，只是换了个 sink ——
 * 解析器与"东西收到哪儿去"本来就是分开的两件事（那个头文件里写着它是纯状态机、
 * 无 IO、不碰请求对象）。所以跨块、近似边界、quoted-pair 这些坑不需要在这里
 * 再解一遍，也不会出现"上传路径修好了、表单路径还留着老 bug"这种分叉。
 *
 * 内存上界：**不额外增加**。普通路由的 body 本来就已经整包收在内存里（受
 * `max_body_size` 约束），这里的 `data` 是那份内存的切分。所以这条路径不适合
 * 大文件 —— 那是 `upload_route()` 的活。**上传总长上限那一套（413 + close）
 * 在这里不适用**：请求早就整包读完了，没有"拒收"这个选项；能做的只是别在
 * 上面再叠一份放大。
 *
 * 用下标而不是指针指向"当前部件"：`vector` 的 `push_back` 会让之前取到的
 * `&back()` 失效，而"下一个部件开始时忘掉旧指针"这种事只能靠人记住。下标没有
 * 这个问题。
 */
class form_sink : public uvcpp_web_multipart_sink {
 public:
  form_sink(std::vector<std::pair<std::string, std::string> >& fields,
            std::vector<uvcpp_web_form_file>& files)
      : fields_(fields), files_(files), file_index_(0), field_index_(0) {}

  bool on_part_begin(const uvcpp_web_part_info& info) {
    if (info.is_file) {
      uvcpp_web_form_file f;
      f.name = info.name;
      // **清洗**：与 `uvcpp_web_upload_file::original_filename()` 同一把尺子
      // （`web_sanitize_filename`）。两边给同一个概念两种语义是个陷阱 ——
      // 何况这里返回的客户端原值可以直接是 `..\..\x`。
      f.filename = web_sanitize_filename(info.filename);
      f.content_type = info.content_type;
      files_.push_back(f);
      file_index_ = files_.size();
      field_index_ = 0;
    } else {
      fields_.push_back(std::make_pair(info.name, std::string()));
      field_index_ = fields_.size();
      file_index_ = 0;
    }
    return true;
  }

  bool on_part_data(const char* data, size_t len) {
    if (file_index_ > 0) {
      files_[file_index_ - 1].data.append(data, len);
    } else if (field_index_ > 0) {
      fields_[field_index_ - 1].second.append(data, len);
    }
    return true;
  }

  void on_part_end(bool truncated) {
    // 这个 sink **不配**任何尺寸上限（`max_file_size` / `max_field_size` 保持
    // 默认的 0 = 不限），因为整包本来就已经在内存里了，再截断只是把一份已经在
    // 手上的数据丢掉。所以 `truncated` 在这里构造性地恒为假。
    (void)truncated;
  }

 private:
  std::vector<std::pair<std::string, std::string> >& fields_;
  std::vector<uvcpp_web_form_file>& files_;
  size_t file_index_;   ///< 1 起；0 = 当前部件不是文件
  size_t field_index_;  ///< 1 起；0 = 当前部件不是字段
};

}  // namespace

// =========================================================================
// 生命周期
// =========================================================================

uvcpp_web_request::uvcpp_web_request()
    : stream_(nullptr),
      upload_(nullptr),
      src_(),
      body_(),
      raw_url_(),
      raw_path_(),
      path_(),
      query_string_(),
      method_(http_method::HTTP_GET),
      version_(uvcpp_http_version::HVER_11),
      content_type_(),
      accept_encoding_(),
      host_(),
      content_length_(0),
      peer_ip_(),
      peer_port_(0),
      query_parsed_(false),
      cookies_parsed_(false),
      form_parsed_(false),
      query_params_(),
      cookies_(),
      form_params_(),
      form_files_(),
      multipart_ok_(true),
      params_() {}

uvcpp_web_request::~uvcpp_web_request() {}

void uvcpp_web_request::take_from(uvcpp_http_request& src) {
  // 顺序是关键：先把 body 搬出来，再拷其余字段。
  //
  // 反过来的话，`src_ = src` 会先把 body **深拷贝**一份（uvcpp_buf 的拷贝
  // 构造是真的分配 + memcpy），然后我们再从 src 把 body 搬走 —— 等于为一次
  // 上传同时持有两份完整数据。大文件上传时这就是白扔一倍内存。
  body_.move_buf(src.body);

  src_ = src;  // 此刻 src.body 已经空了，这次赋值不搬大块

  method_  = src.method;
  version_ = src.version;
  raw_url_ = src.url;

  web_split_path_query(raw_url_, raw_path_, query_string_);

  // 路径解码用 plus_as_space = false —— 路径里的 '+' 是字面加号。
  //
  // 注意由此产生的一条规则：**`%2F` 解码成 `/`，所以它充当分隔符**。
  // 也就是说 `/api/a%2Fb` 解出来是 `/api/a/b`，路由会按两段匹配。这是有意
  // 选择的（Express 等框架同样在解码后的路径上匹配），写在文档里，而不是
  // 让 `%2F` 变成一个隐形的、只有攻击者会用的第三态。
  path_ = web_url_decode(raw_path_, /*plus_as_space=*/false);

  // 关心的几个头解析一次缓存下来。http_headers 是线性表，每次 get_header
  // 都是 O(头数) 次大小写不敏感比较，而 content_type 在 is_json()/is_form()
  // 里会被反复问到。
  content_type_ = http_get_header(src_.headers, "content-type");
  accept_encoding_ = http_get_header(src_.headers, "accept-encoding");
  host_ = http_get_header(src_.headers, "host");

  const std::string cl = http_get_header(src_.headers, "content-length");
  if (!cl.empty() && !parse_size(cl, content_length_)) {
    content_length_ = 0;  // 非法声明按 0 处理；不信任这个值做分配
  }
}

void uvcpp_web_request::set_peer(const std::string& ip, unsigned int port) {
  peer_ip_ = ip;
  peer_port_ = port;
}

void uvcpp_web_request::set_param(const std::string& name,
                                  const std::string& value) {
  params_.push_back(std::make_pair(name, value));
}

// =========================================================================
// 基本信息
// =========================================================================

const char* uvcpp_web_request::method_name() const {
  return http_method_str(method_);
}

const char* uvcpp_web_request::version_name() const {
  return uvcpp_http_version_str(version_);
}

// =========================================================================
// 报头
// =========================================================================

std::string uvcpp_web_request::header(const std::string& name,
                                      const std::string& def) const {
  return http_get_header(src_.headers, name, def);
}

bool uvcpp_web_request::has_header(const std::string& name) const {
  return http_has_header(src_.headers, name);
}

bool uvcpp_web_request::accepts_encoding(const std::string& encoding) const {
  const double q = encoding_qvalue(encoding);
  if (q >= 0.0) return q > 0.0;  // 出现了就用它的 q 值（0 表示明确拒绝）

  // 没出现：identity 恒可接受（RFC 7231 §5.3.4 —— 除非被显式拒绝，
  // 而显式拒绝在上面那条分支里已经返回了）。其它编码不接受。
  return web_equals_ci(encoding, "identity");
}

double uvcpp_web_request::encoding_qvalue(const std::string& encoding) const {
  if (accept_encoding_.empty()) return -1.0;

  double wildcard = -1.0;
  size_t start = 0;

  while (start <= accept_encoding_.size()) {
    size_t comma = accept_encoding_.find(',', start);
    if (comma == std::string::npos) comma = accept_encoding_.size();

    std::string item = web_trim(accept_encoding_.substr(start, comma - start));
    if (!item.empty()) {
      // 一个编码项形如 `gzip;q=0.5;x=y`。按 ';' 切开，第一段是编码名，
      // 其余段里找 q=。不能直接在整串里 find("q=") —— 那会被别的参数
      // 值里的 "q=" 骗到。
      std::vector<std::string> parts;
      web_split(item, ';', parts, false);
      if (!parts.empty()) {
        const std::string name = web_trim(parts[0]);
        double q = 1.0;
        for (size_t i = 1; i < parts.size(); ++i) {
          const std::string p = web_trim(parts[i]);
          if (web_starts_with_ci(p, "q=")) {
            q = parse_qvalue(p.substr(2));
            break;
          }
        }
        if (web_equals_ci(name, encoding)) return q;
        if (name == "*") wildcard = q;
      }
    }

    if (comma == accept_encoding_.size()) break;
    start = comma + 1;
  }

  return wildcard;
}

bool uvcpp_web_request::is_keep_alive() const {
  // Connection 可以是逗号列表（"keep-alive, Upgrade"），所以要按 token 找，
  // 不能整串比较。
  const std::string conn = header("connection");
  bool has_close = false;
  bool has_keep_alive = false;

  size_t start = 0;
  while (start <= conn.size()) {
    size_t comma = conn.find(',', start);
    if (comma == std::string::npos) comma = conn.size();
    const std::string tok = web_trim(conn.substr(start, comma - start));
    if (web_equals_ci(tok, "close")) has_close = true;
    if (web_equals_ci(tok, "keep-alive")) has_keep_alive = true;
    if (comma == conn.size()) break;
    start = comma + 1;
  }

  if (version_ == uvcpp_http_version::HVER_11) {
    return !has_close;  // HTTP/1.1 默认保持
  }
  return has_keep_alive;  // HTTP/1.0 默认关闭
}

// =========================================================================
// 查询串 / 表单 / Cookie / 路径参数
// =========================================================================

const std::vector<std::pair<std::string, std::string> >&
uvcpp_web_request::query_params() const {
  if (!query_parsed_) {
    query_parsed_ = true;
    query_params_ = web_parse_query(query_string_);
  }
  return query_params_;
}

const std::string* uvcpp_web_request::query(const std::string& name) const {
  return web_find_param(query_params(), name);
}

bool uvcpp_web_request::is_form() const {
  return mime_base(content_type_) == "application/x-www-form-urlencoded";
}

bool uvcpp_web_request::is_multipart() const {
  return mime_base(content_type_) == "multipart/form-data";
}

const std::vector<std::pair<std::string, std::string> >&
uvcpp_web_request::form_params() const {
  if (!form_parsed_) {
    form_parsed_ = true;
    if (is_form()) {
      // body_str() 在这里才把 body 复制成 string —— 只有真的要解析表单时
      // 才付这次拷贝。表单和查询串的编码规则相同（`+` 是空格、
      // `%XX` 是字节），所以直接复用 web_parse_query。
      form_params_ = web_parse_query(body_str());
    } else if (is_multipart()) {
      // **这里是步骤 8 之前的一个窟窿**：multipart 请求走到这里原本什么都不做，
      // 于是 `req.form("x")` 对一份完全合法的 multipart 表单**静默返回空** ——
      // 不是"没这个字段"，而是"框架装作它是个空表单"。handler 分不出这两者。
      parse_multipart_form();
    }
  }
  return form_params_;
}

const std::string* uvcpp_web_request::form(const std::string& name) const {
  return web_find_param(form_params(), name);
}

const std::vector<uvcpp_web_form_file>& uvcpp_web_request::files() const {
  // 借道 `form_params()` 而不是自己判 `form_parsed_`：**同一次解析**同时填两个
  // 容器，共用一个惰性标志。分成两个标志的话，"只问 form() 的调用方"会让文件
  // 部件永远不被解析，而"先 form() 再 file()"会整份重解第二遍。
  (void)form_params();
  return form_files_;
}

const uvcpp_web_form_file* uvcpp_web_request::file(
    const std::string& name) const {
  const std::vector<uvcpp_web_form_file>& v = files();
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i].name == name) return &v[i];
  }
  return nullptr;
}

bool uvcpp_web_request::multipart_ok() const {
  // 不是 multipart 就**不适用**，不是错误 —— 返回 true。让一个 urlencoded 表单
  // 因为"不是 multipart"而看到 false，会诱使调用方写"if (!multipart_ok())
  // 回 400"，那会把正常的表单全拒掉。
  if (!is_multipart()) return true;
  (void)form_params();
  return multipart_ok_;
}

void uvcpp_web_request::parse_multipart_form() const {
  multipart_ok_ = true;

  // boundary 缺失/畸形 ⇒ 报文本身无法划分。回 400 是**路由**该做的事，这里
  // 只能如实报告"没解出来"（`multipart_ok() == false`），别自作主张回状态码 ——
  // 请求对象没有响应通道。
  const std::string boundary = web_multipart_boundary(content_type_);
  if (boundary.empty()) {
    multipart_ok_ = false;
    return;
  }

  form_sink sink(form_params_, form_files_);
  uvcpp_web_multipart mp;
  mp.set_sink(&sink);
  if (!mp.set_boundary(boundary)) {
    multipart_ok_ = false;
    return;
  }

  // 空 body 直接调 feed(nullptr, 0) 是没必要的边界情形（一份 multipart 至少
  // 得有终边界），但 guard 一下省得依赖 feed 对 nullptr 的容忍度。
  uvcpp_web_multipart_result r =
      (body_size() > 0)
          ? mp.feed(body_data(), body_size())
          : uvcpp_web_multipart_result::OK;
  // 只有"还没结束"才需要 finish()；feed 已经给出终态（含错误）时它的返回值
  // 就是结论。finish() 的语义是"body 收完了"—— 对一份**已经整包**的 body，
  // 它负责把"终边界从没出现"这件事判出来（报文被掐断）。
  if (r == uvcpp_web_multipart_result::OK) r = mp.finish();

  if (r != uvcpp_web_multipart_result::DONE) {
    // **失败时清空**，而不是把半份结果交出去。契约写在头文件里
    // （`files()` 的注释）：解析失败时两个容器都是空的，靠 `multipart_ok()`
    // 区分"空表单"和"坏了"。交出半份更危险 —— handler 读到的是一个**看起来
    // 合法**的截断值（比如金额、数量），而它没有任何迹象表明自己被截断了。
    multipart_ok_ = false;
    form_params_.clear();
    form_files_.clear();
    return;
  }

  multipart_ok_ = true;
}

const std::string* uvcpp_web_request::cookie(const std::string& name) const {
  if (!cookies_parsed_) {
    cookies_parsed_ = true;
    cookies_ = web_parse_cookies(header("cookie"));
  }
  return web_find_cookie(cookies_, name);
}

const std::string* uvcpp_web_request::param(const std::string& name) const {
  for (size_t i = 0; i < params_.size(); ++i) {
    if (params_[i].first == name) return &params_[i].second;
  }
  return nullptr;
}

// =========================================================================
// 报文体
// =========================================================================

const char* uvcpp_web_request::body_data() const { return body_.get_const_data(); }

size_t uvcpp_web_request::body_size() const { return body_.size(); }

bool uvcpp_web_request::body_empty() const { return body_.size() == 0; }

std::string uvcpp_web_request::body_str() const { return body_.to_string(); }

// =========================================================================
// JSON
// =========================================================================

bool uvcpp_web_request::is_json() const {
  const std::string base = mime_base(content_type_);
  if (base == "application/json" || base == "text/json") return true;
  // `application/vnd.api+json` 这类结构化后缀同样按 JSON 处理（RFC 6839）
  return web_ends_with(base, "+json");
}

bool uvcpp_web_request::json(uvcpp_json& out, json_status* status) const {
  // body_data() 在空 body 时是 nullptr，uvcpp_json_parse 会返回 EMPTY 而不是
  // 崩溃 —— 这正是「没有 body」和「body 不是 JSON」需要分开报的原因。
  const json_status st = uvcpp_json_parse(body_data(), body_size(), out);
  if (status != nullptr) *status = st;
  return st == json_status::OK;
}

uvcpp_json uvcpp_web_request::json() const {
  uvcpp_json out;
  if (!json(out, nullptr)) return uvcpp_json();
  return out;
}

}  // namespace uvcpp
