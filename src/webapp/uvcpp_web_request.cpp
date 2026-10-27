/**
 * @file src/webapp/uvcpp_web_request.cpp
 * @brief `uvcpp_web_request.h` 的实现。
 */

#include <webapp/uvcpp_web_request.h>

#include <cstdlib>
#include <iterator>  // std::make_move_iterator（take_from 搬头表用）

// `json()` 便捷版那条失败日志用（`json_status_name()` 由
// `uvcpp_web_json.h` 提供，那个头经 `uvcpp_web_request.h` 已经在）。
#include <webapp/uvcpp_log.h>

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
      raw_path_(),
      path_(),
      query_string_(),
      method_(http_method::HTTP_GET),
      version_(uvcpp_http_version::HVER_11),
      content_type_(),
      accept_encoding_(),
      host_(),
      content_length_(0),
      keep_alive_cache_(0),
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
  // 顺序是关键：先把 body 搬出来，再搬其余字段。
  //
  // 反过来的话，赋值会先把 body **深拷贝**一份（uvcpp_buf 的拷贝构造是真的
  // 分配 + memcpy），然后我们再从 src 把 body 搬走 —— 等于为一次上传同时持有
  // 两份完整数据。大文件上传时这就是白扔一倍内存。
  body_.move_buf(src.body);

  // 其余字段**逐个搬**，不再走 `src_ = src`：那个赋值会把头向量整条拷一份，
  // 而它是 `std::vector<http_header>`、每个 `http_header` 又是两个
  // `std::string` —— 拷一份就是 **2×头数** 次分配加同样多次 memcpy。搬一次只是
  // 把内部指针换手，于是这条链路上（解析器 → HTTP 层视图 → 这里）头只被**构造**
  // 一次，后面每一跳都是搬家。
  //
  // 代价与 body 那条同源，一并写进契约：`take_from()` 之后源请求的 `headers`
  // 空了（`url` **不搬**，见下面那段：`raw()` 还要从这里读原始目标）。
  // **三个标量（`method` / `version` / `stream_id`）刻意留
  // 原值**：HTTP 层的 h2 路径正是靠 `take_from()` 之后读 `stream_id` 回填流号的。
  src_.method    = src.method;
  src_.version   = src.version;
  src_.stream_id = src.stream_id;
  src_.url       = std::move(src.url);
  // 「稍后回话」的凭据按值接过来（浅句柄，不拥有任何东西）。**h3 上它是框架
  // 唯一能回话的入口** —— `send_response()` 那条路要 `uvcpp_tcp_client*`，而
  // h3 没有；少了这一句，webapp 的 h3 响应只能落到"连接已断开"那条丢弃分支上。
  src_.reply     = src.reply;

  // 头表**逐元素搬进 `src_.headers` 已有的缓冲**，不再移动赋值。
  //
  // 移动赋值把 `src.headers` 那块缓冲**换手**带走 —— 而那个 vector 现在是
  // **连接级**的（HTTP 层的搬移目的地，见 `uvcpp_http_parser::take_headers_into()`）：
  // 带走它，下一条请求就得重新分配一次。这里只搬元素：`insert` 在容量够时一次
  // 分配都不做，`clear()` 只改 size ⇒ 容量留在连接上，`src` 也只丢掉元素。
  //
  // 两句话都不能省：`clear()` 是"上一条请求的头不许留在表里"的守卫（认领时还会
  // 再清一次，见 `adopt_headers()`），末尾那句 `clear()` 维持"搬过就空了"的契约
  // —— 元素本身已经是 moved-from，但**表的大小**不靠 `std::move` 的通常行为保证。
  src_.headers.clear();
  src_.headers.insert(src_.headers.end(),
                      std::make_move_iterator(src.headers.begin()),
                      std::make_move_iterator(src.headers.end()));
  src.headers.clear();

  method_  = src_.method;
  version_ = src_.version;
  // URL 就留在 `src_.url` 里，**不再拷第二份**：`raw_url()` 直接返回它，
  // `raw()` 里看到的也是同一份 ⇒ 不再有"搬走之后源的 url 空了"这种分叉。
  // （以前这里是 `raw_url_ = src_.url`，一过 15 字节的 SSO 线就是每请求一次
  // malloc + free；改成搬走又会把 `raw().url` 清空，违反那个逃生口的契约。）
  web_split_path_query(src_.url, raw_path_, query_string_);

  // 归一化成路由与中间件看到的那条路径（解码 + 折叠）。两条规则的分工与
  // 顺序理由（`+` 是字面加号、`%2F` 解码后充当分隔符、所以**必须先解码再
  // 折叠**）都写在 `web_normalize_path()` 的注释里 —— 那里是这三处
  // （本函数 / `claim_stream()` / `on_ws_upgrade()`）唯一的归一化入口。
  //
  // 先赋值再原地归一，而不是 `path_ = web_normalize_path(...)` 那种形态：
  // `path_` 的容量是上一条请求留下的，够用时这次 `assign` 是纯 memcpy、一次
  // 分配都不做；归一那一步在路径本来就最简时更是连一个字节都不碰。
  path_ = raw_path_;
  web_normalize_path(path_);

  // 关心的几个头**一次遍历**取完。
  //
  // 以前这里是 4 次 `http_get_header()`：4 遍线性表，每格一次大小写不敏感
  // 比较，而且它按**值**返回（`return h.value`）—— 值一过 SSO 线就是每请求
  // 一次 malloc + free。改成赋值到成员里：成员带着上一条请求留下的容量，
  // 够用时一次分配都不做。
  //
  // 判据逐条对齐 `http_get_header()`：同名头出现多次时**先到的那条赢**。
  // 所以每个名字配一个 `seen` 位，命中之后不再覆盖。名字长度先比一次，
  // 长度不对连字节都不用看。
  enum { kSeenType = 1, kSeenEncoding = 2, kSeenHost = 4, kSeenLength = 8 };
  content_type_.clear();
  accept_encoding_.clear();
  host_.clear();
  content_length_ = 0;
  unsigned seen = 0;
  for (size_t i = 0; i < src_.headers.size(); ++i) {
    const http_header& h = src_.headers[i];
    const size_t n = h.name.size();
    if (n == 4) {  // host
      if ((seen & kSeenHost) == 0 &&
          http_name_iequal(h.name.data(), n, "host", 4)) {
        host_ = h.value;
        seen |= kSeenHost;
      }
    } else if (n == 12 && (seen & kSeenType) == 0) {  // content-type
      if (http_name_iequal(h.name.data(), n, "content-type", 12)) {
        content_type_ = h.value;
        seen |= kSeenType;
      }
    } else if (n == 14 && (seen & kSeenLength) == 0) {  // content-length
      if (http_name_iequal(h.name.data(), n, "content-length", 14)) {
        if (!parse_size(h.value, content_length_)) {
          content_length_ = 0;  // 非法声明按 0 处理；不信任这个值做分配
        }
        seen |= kSeenLength;
      }
    } else if (n == 15 && (seen & kSeenEncoding) == 0) {  // accept-encoding
      if (http_name_iequal(h.name.data(), n, "accept-encoding", 15)) {
        accept_encoding_ = h.value;
        seen |= kSeenEncoding;
      }
    }
    if (seen == (kSeenType | kSeenEncoding | kSeenHost | kSeenLength)) break;
  }
  // keep-alive：传输层说了就听它的，没说（`-1`）才留给 `is_keep_alive()` 按
  // 头表算。缓存里 0 = 没算过正是"没说"的编码，所以两边是同一个三态。
  //
  // 「上一条请求的结论不能留到这一条」依旧成立：这里**一定**会被赋一次值
  // （`-1` 也赋），不存在保留旧结论的那条路。
  keep_alive_cache_ = (src.keep_alive == 0 || src.keep_alive == 1)
                          ? src.keep_alive
                          : 0;
}

void uvcpp_web_request::adopt_headers(http_headers& src) {
  // 先清再换：换进来的表若带着上一条请求的头，下面 `take_from()` 会把它一起
  // 搬进 `src_.headers`（`insert` 是**追加**）⇒ 第二条请求看见第一条的头。
  src_.headers.clear();
  src_.headers.swap(src);
}

void uvcpp_web_request::yield_headers(http_headers& dst) {
  // 清空（元素在这里死掉，正是它们本来该死的那一刻），再把**空表**换回去 ——
  // 回收槽里永远是"空的、有容量的"，于是"认领时要不要清"不构成正确性依赖。
  src_.headers.clear();
  src_.headers.swap(dst);
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

bool uvcpp_web_request::has_header(const char* name) const {
  return http_has_header(src_.headers, name);
}

std::string uvcpp_web_request::header(const char* name,
                                      const std::string& def) const {
  return http_get_header(src_.headers, name, def);
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
  // 结论缓存到请求对象的生存期结束：`send_response()` 每条请求问一次，而
  // 「一次线性表扫描 + 按逗号切 token + 每个 token 一次 trim」在 1024 连接的
  // 热路上是能看见的。0 = 没算过（`take_from()` 会归零）。
  if (keep_alive_cache_ != 0) return keep_alive_cache_ == 1;

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

  const bool keep = (version_ == uvcpp_http_version::HVER_11)
                        ? !has_close          // HTTP/1.1 默认保持
                        : has_keep_alive;     // HTTP/1.0 默认关闭
  keep_alive_cache_ = keep ? 1 : 2;
  return keep;
}

// =========================================================================
// 查询串 / 表单 / Cookie / 路径参数
// =========================================================================

const std::vector<std::pair<std::string, std::string> >&
uvcpp_web_request::query_params() const {
  if (!query_parsed_) {
    query_parsed_ = true;
    // 填进**成员自己的**那条向量：同一个请求对象上重复问只解析一次，而且
    // 也不用先造一条临时向量再整体搬过来。
    //
    // **不要以为容量会跨请求留着**：`uvcpp_web_request` 是每请求新建的
    // （`uvcpp_web_context::create()`），上一次请求的向量随上下文一起没了 ——
    // 实测每请求仍是 1 次分配。真正的复用口是 `adopt_tables()` 那族
    // （头表 / 响应表），查询向量没接进去。
    web_parse_query_into(query_string_, query_params_);
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
      web_parse_query_into(body_str(), form_params_);
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
  // 用带 `status` 的那个重载而不是 `json(out, nullptr)`：这个便捷版本把失败
  // 抹成了「一个 null 的 JSON 对象」，调用方**没有任何办法**分辨「body 不是
  // JSON」和「body 就是一个 null」—— 那正是这一层最该说清楚的一件事。
  // 签名不动（这是既有公开 API），改成在这里把状态记一条。
  json_status st = json_status::OK;
  if (!json(out, &st)) {
    UVCPP_LOG_DEBUG(log_category::JSON)
        << "请求体不是可用的 JSON（" << json_status_name(st) << "，body "
        << body_size() << " 字节）：json() 返回 null 对象";
    return uvcpp_json();
  }
  return out;
}

}  // namespace uvcpp
