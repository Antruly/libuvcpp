/**
 * @file src/webapp/uvcpp_web_multipart.cpp
 * @brief uvcpp_web_multipart 的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <webapp/uvcpp_web_multipart.h>

#include <utility>

#include <webapp/uvcpp_web_util.h>

namespace uvcpp {

namespace {

// RFC 2046 §5.1.1：boundary 最长 70 字节。
const size_t kMaxBoundaryLen = 70;

// 边界行里允许的线性空白上限（RFC 2046 §5.1.1 的 `[LWSP]`）。有上限不是为了
// 刁难谁：流式解析不能为"一行空白"无界地留缓冲。超过上限**明确失败**，
// 而不是当成 body —— 后者会让本端与接受该填充的对端对"部件从哪儿切"产生
// 分歧，那正是请求走私（对同一段字节两个解析器切出不同部件划分）的形状。
const size_t kMaxBoundaryPad = 128;

// ---------------------------------------------------------------------------
// 部件头的参数解析
// ---------------------------------------------------------------------------

/**
 * @brief 把 `form-data; name="a"; filename="b;c"` 拆成主值 + 参数表。
 *
 * 两条**必须**做对的事（少了任何一条，真实浏览器发的文件名就会被解析错）：
 *  - **引号内的分号不是参数分隔符** —— `filename="a;b.txt"` 是一个参数；
 *  - **`\"` 是转义引号** —— `filename="a\"b.txt"` 的值是 `a"b.txt`。
 */
bool split_params(const std::string& s, std::string& main_out,
                  std::vector<std::pair<std::string, std::string> >& params_out) {
  std::vector<std::string> segs;
  std::string cur;
  bool in_quote = false;

  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (in_quote) {
      if (c == '\\' && i + 1 < s.size()) {
        cur += c;
        cur += s[i + 1];
        ++i;
        continue;
      }
      if (c == '"') in_quote = false;
      cur += c;
      continue;
    }
    if (c == '"') {
      in_quote = true;
      cur += c;
      continue;
    }
    if (c == ';') {
      segs.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  // 引号没闭合：畸形。不接受 —— 一个没闭合的引号会让"参数边界在哪"变成
  // 实现各说各话的问题，那正是解析差异类漏洞的土壤。
  if (in_quote) return false;
  segs.push_back(cur);

  if (segs.empty()) return false;
  main_out = web_trim(segs[0]);
  if (main_out.empty()) return false;

  for (size_t i = 1; i < segs.size(); ++i) {
    const std::string& seg = segs[i];
    // 允许 `a=1; ; b=2` 这种多余分号，但不允许"有内容却没有等号"。
    if (web_trim(seg).empty()) continue;
    const size_t eq = seg.find('=');
    if (eq == std::string::npos) return false;

    const std::string key = web_trim(seg.substr(0, eq));
    if (key.empty()) return false;
    params_out.push_back(
        std::make_pair(key, web_trim(seg.substr(eq + 1))));
  }
  return true;
}

/**
 * @brief 去掉参数值两侧的引号，处理 `\"` 转义。
 *
 * 没有引号的值是 token（`filename*=UTF-8''x`），原样返回。
 */
bool unquote_value(const std::string& v, std::string& out) {
  if (v.empty() || v[0] != '"') {
    out = v;
    return true;
  }
  if (v.size() < 2 || v[v.size() - 1] != '"') return false;  // 引号没闭合

  out.clear();
  out.reserve(v.size() - 2);
  for (size_t i = 1; i + 1 < v.size(); ++i) {
    if (v[i] == '\\' && i + 2 < v.size()) {
      out += v[i + 1];
      ++i;
      continue;
    }
    out += v[i];
  }
  return true;
}

/**
 * @brief 解码 RFC 5987 的 ext-value：`chardet'lang'pct-encoded`。
 *
 * 只认 UTF-8。**认不出就返回 false，让调用方回退到普通 `filename=`** ——
 * 这是 RFC 6266 §4.3 的行为，也是唯一安全的选择：把一个按未知字符集解出来的
 * 字节串当成文件名，比用那个（可能被截断的）普通值更糟。
 *
 * 注意这里**不能**用 `web_url_decode(..., plus_as_space=true)`：`+` 在
 * RFC 5987 里是**字面加号**，不是空格。文件名 `a+b.txt` 被解成 `a b.txt`
 * 是静默的数据损坏。
 */
bool decode_ext_value(const std::string& v, std::string& out) {
  const size_t q1 = v.find('\'');
  if (q1 == std::string::npos) return false;
  const size_t q2 = v.find('\'', q1 + 1);
  if (q2 == std::string::npos) return false;

  const std::string charset = v.substr(0, q1);
  if (charset.empty() || !web_equals_ci(charset, "UTF-8")) return false;

  bool ok = true;
  out = web_url_decode(v.substr(q2 + 1), /*plus_as_space=*/false, &ok);
  return ok;
}

/**
 * @brief 解析部件头块。
 *
 * @return false = 畸形。**裸 LF 或裸 CR 一律判畸形**（见头文件里的理由：
 *         接受两种行尾的实现会让"边界在哪"与只接受一种的对端产生分歧）。
 */
bool parse_part_headers(const std::string& block, uvcpp_web_part_info& info,
                        bool* is_form_data) {
  *is_form_data = false;
  if (block.empty()) return true;  // 空头块本身不畸形，但会因缺 name= 被拒

  bool have_disposition = false;
  bool have_filename = false;
  std::string plain_filename;
  std::string star_filename;

  size_t pos = 0;
  while (pos < block.size()) {
    const size_t eol = block.find("\r\n", pos);
    if (eol == std::string::npos) return false;  // 最后一行没有 CRLF
    const std::string line = block.substr(pos, eol - pos);
    pos = eol + 2;

    // 裸 CR / 裸 LF：畸形。
    if (line.find('\r') != std::string::npos) return false;
    if (line.find('\n') != std::string::npos) return false;

    if (line.empty()) continue;  // 容忍多余空行

    const size_t colon = line.find(':');
    if (colon == std::string::npos) return false;  // 行内没有冒号

    const std::string name = web_to_lower(web_trim(line.substr(0, colon)));
    const std::string value = web_trim(line.substr(colon + 1));
    if (name.empty()) return false;

    if (name == "content-disposition") {
      std::string main;
      std::vector<std::pair<std::string, std::string> > params;
      if (!split_params(value, main, params)) return false;
      if (!web_equals_ci(main, "form-data")) {
        // 不是 form-data：交给调用方按 ERROR_NOT_FORM_DATA 处理。
        *is_form_data = false;
        have_disposition = true;
        continue;
      }
      *is_form_data = true;
      have_disposition = true;

      for (size_t i = 0; i < params.size(); ++i) {
        const std::string key = web_to_lower(params[i].first);
        std::string decoded;
        if (key == "name") {
          if (!unquote_value(params[i].second, decoded)) return false;
          info.name = decoded;
        } else if (key == "filename") {
          // RFC 7578 §4.2：普通 filename 参数**不做**百分号解码。
          if (!unquote_value(params[i].second, decoded)) return false;
          plain_filename = decoded;
          have_filename = true;
        } else if (key == "filename*") {
          std::string ext;
          // 解不出来就不算"有 filename*" —— 回退到普通 filename。
          if (decode_ext_value(web_trim(params[i].second), ext)) {
            star_filename = ext;
          }
          have_filename = true;
        }
      }
    } else if (name == "content-type") {
      info.content_type = value;
    }
  }

  if (!have_disposition) return true;  // 由调用方判 ERROR_NOT_FORM_DATA

  // RFC 6266 §4.3：`filename*` 优先于 `filename`。
  if (!star_filename.empty()) {
    info.filename = star_filename;
  } else if (have_filename) {
    info.filename = plain_filename;
  }

  // **按"参数在不在"判文件部件，而不是按"名字空不空"**：浏览器对空的
  // file input 会发 `filename=""`，那是一个（匿名的）文件部件，不是字段。
  info.is_file = have_filename;
  (void)plain_filename;
  return true;
}

}  // namespace

// =========================================================================
// 结果名
// =========================================================================

const char* uvcpp_web_multipart_result_name(uvcpp_web_multipart_result r) {
  switch (r) {
    case uvcpp_web_multipart_result::OK: return "OK";
    case uvcpp_web_multipart_result::DONE: return "DONE";
    case uvcpp_web_multipart_result::ERROR_NO_BOUNDARY: return "ERROR_NO_BOUNDARY";
    case uvcpp_web_multipart_result::ERROR_BOUNDARY_NEVER_FOUND:
      return "ERROR_BOUNDARY_NEVER_FOUND";
    case uvcpp_web_multipart_result::ERROR_HEADER_TOO_LONG:
      return "ERROR_HEADER_TOO_LONG";
    case uvcpp_web_multipart_result::ERROR_BAD_HEADER: return "ERROR_BAD_HEADER";
    case uvcpp_web_multipart_result::ERROR_NOT_FORM_DATA:
      return "ERROR_NOT_FORM_DATA";
    case uvcpp_web_multipart_result::ERROR_MISSING_NAME:
      return "ERROR_MISSING_NAME";
    case uvcpp_web_multipart_result::ERROR_TOO_MANY_FILES:
      return "ERROR_TOO_MANY_FILES";
    case uvcpp_web_multipart_result::ERROR_TOO_MANY_FIELDS:
      return "ERROR_TOO_MANY_FIELDS";
    case uvcpp_web_multipart_result::ERROR_FIELD_TOO_LARGE:
      return "ERROR_FIELD_TOO_LARGE";
    case uvcpp_web_multipart_result::ERROR_SINK_ABORTED:
      return "ERROR_SINK_ABORTED";
  }
  return "UNKNOWN";
}

// =========================================================================
// 内存上界
// =========================================================================

size_t uvcpp_web_multipart_max_retain(size_t boundary_len) {
  // 未命中时留下的那半个标记：delim 长 boundary_len + 4，留 d-1。
  const size_t nomatch = boundary_len + 3;
  // 边界行还没判完时留在缓冲里的：delim + 至多两段 `[LWSP]` + 至多两个待判字节。
  const size_t pending = boundary_len + 4 + 2 * kMaxBoundaryPad + 2;
  return pending > nomatch ? pending : nomatch;
}

// =========================================================================
// 生命周期
// =========================================================================

uvcpp_web_multipart::uvcpp_web_multipart()
    : sink_(nullptr),
      state_(uvcpp_web_multipart_state::PREAMBLE),
      result_(uvcpp_web_multipart_result::OK),
      received_(0),
      body_bytes_(0),
      part_bytes_(0),
      header_scan_from_(0),
      max_file_size_(0),
      max_field_size_(0),
      max_part_header_bytes_(8 * 1024),
      max_file_count_(32),
      max_field_count_(128),
      part_index_(0),
      file_count_(0),
      field_count_(0),
      truncated_(false) {
  // **虚拟前置 CRLF**：于是首边界与后续边界形状统一（都找 `\r\n--<b>`），
  // 不必为首边界特判，也天然让"边界前的 CRLF 属于分隔符"。
  retain_ = "\r\n";
}

uvcpp_web_multipart::~uvcpp_web_multipart() {}

// =========================================================================
// 配置
// =========================================================================

void uvcpp_web_multipart::set_sink(uvcpp_web_multipart_sink* sink) {
  sink_ = sink;
}

bool uvcpp_web_multipart::set_boundary(const std::string& boundary) {
  // 空或超长必须**拒绝**，不能截断：截断会让本端与对端对"边界是什么"
  // 产生不同理解，那是切错报文的起点。
  if (boundary.empty() || boundary.size() > kMaxBoundaryLen) {
    fail(uvcpp_web_multipart_result::ERROR_NO_BOUNDARY,
         "boundary 为空或超过 70 字节");
    return false;
  }
  boundary_ = boundary;
  delim_ = "\r\n--" + boundary;
  header_scan_from_ = 0;
  return true;
}

void uvcpp_web_multipart::set_max_part_header_bytes(size_t n) {
  max_part_header_bytes_ = n;
}
void uvcpp_web_multipart::set_max_file_count(size_t n) { max_file_count_ = n; }
void uvcpp_web_multipart::set_max_field_count(size_t n) { max_field_count_ = n; }
void uvcpp_web_multipart::set_max_file_size(uint64_t n) { max_file_size_ = n; }
void uvcpp_web_multipart::set_max_field_size(uint64_t n) { max_field_size_ = n; }

// =========================================================================
// 失败
// =========================================================================

void uvcpp_web_multipart::fail(uvcpp_web_multipart_result r,
                              const std::string& detail) {
  if (state_ == uvcpp_web_multipart_state::ERROR_STATE) return;  // 首个错误胜出
  state_ = uvcpp_web_multipart_state::ERROR_STATE;
  result_ = r;
  error_text_ = detail;
  // 出错之后保留缓冲没有意义了（不会再产出），顺手释放：一个畸形但很大的
  // 上传不该让解析器继续持着缓冲区。
  retain_.clear();
  header_block_.clear();
}

// =========================================================================
// 交付
// =========================================================================

bool uvcpp_web_multipart::deliver(const char* data, size_t len) {
  if (len == 0) return true;

  // 只有 PART_BODY 会交付；PREAMBLE / PART_BODY_SKIP / EPILOGUE 一律丢弃。
  // 丢弃**不等于**停止扫边界 —— 那正是 PART_BODY_SKIP 存在的意义。
  if (state_ != uvcpp_web_multipart_state::PART_BODY) return true;

  if (!part_.is_file) {
    if (max_field_size_ > 0 && part_bytes_ + len > max_field_size_) {
      fail(uvcpp_web_multipart_result::ERROR_FIELD_TOO_LARGE,
           "字段部件超过 max_field_size");
      return false;
    }
  } else if (max_file_size_ > 0 && part_bytes_ + len > max_file_size_) {
    // 截断：只收够上限那部分，然后转 SKIP —— **继续扫边界**，
    // 否则后面所有部件都会被这一块吃掉。
    const size_t keep = static_cast<size_t>(max_file_size_ - part_bytes_);
    if (keep > 0 && sink_ != nullptr) {
      if (!sink_->on_part_data(data, keep)) {
        fail(uvcpp_web_multipart_result::ERROR_SINK_ABORTED, "sink 中止了部件");
        return false;
      }
      part_bytes_ += keep;
      body_bytes_ += keep;
    }
    truncated_ = true;
    state_ = uvcpp_web_multipart_state::PART_BODY_SKIP;
    return true;
  }

  if (sink_ != nullptr && !sink_->on_part_data(data, len)) {
    fail(uvcpp_web_multipart_result::ERROR_SINK_ABORTED, "sink 中止了部件");
    return false;
  }
  part_bytes_ += len;
  body_bytes_ += len;
  return true;
}

// =========================================================================
// 部件边界
// =========================================================================

bool uvcpp_web_multipart::finish_part() {
  const bool was_open =
      (state_ == uvcpp_web_multipart_state::PART_BODY ||
       state_ == uvcpp_web_multipart_state::PART_BODY_SKIP);
  if (!was_open) return true;  // PREAMBLE：还没有部件要收尾

  if (sink_ != nullptr) sink_->on_part_end(truncated_);
  return true;
}

bool uvcpp_web_multipart::begin_part_from_block() {
  part_ = uvcpp_web_part_info();

  bool is_form_data = false;
  if (!parse_part_headers(header_block_, part_, &is_form_data)) {
    fail(uvcpp_web_multipart_result::ERROR_BAD_HEADER, "部件头畸形");
    return false;
  }
  if (!is_form_data) {
    fail(uvcpp_web_multipart_result::ERROR_NOT_FORM_DATA,
         "Content-Disposition 缺失或不是 form-data");
    return false;
  }
  if (part_.name.empty()) {
    fail(uvcpp_web_multipart_result::ERROR_MISSING_NAME,
         "Content-Disposition 里没有 name=");
    return false;
  }

  ++part_index_;
  if (part_.is_file) {
    ++file_count_;
    if (max_file_count_ > 0 && file_count_ > max_file_count_) {
      fail(uvcpp_web_multipart_result::ERROR_TOO_MANY_FILES, "文件部件数超限");
      return false;
    }
  } else {
    ++field_count_;
    if (max_field_count_ > 0 && field_count_ > max_field_count_) {
      fail(uvcpp_web_multipart_result::ERROR_TOO_MANY_FIELDS, "字段部件数超限");
      return false;
    }
  }

  if (sink_ != nullptr && !sink_->on_part_begin(part_)) {
    fail(uvcpp_web_multipart_result::ERROR_SINK_ABORTED, "sink 中止了解析");
    return false;
  }
  return true;
}

// =========================================================================
// 主扫描
// =========================================================================

void uvcpp_web_multipart::scan() {
  // 扫描游标（相对 retain_）。**不边扫边往前 erase**：假命中只前进一个字节，
  // 若每次都从 0 重新 find，一个塞满近似边界的 body 会退化成 O(n²)
  // （每字节扫一遍全缓冲）—— 那是可以拿来打的服务端。游标让 find 从上次
  // 停下的地方继续，总量回到线性。
  size_t cursor = 0;

  for (;;) {
    if (state_ == uvcpp_web_multipart_state::ERROR_STATE ||
        state_ == uvcpp_web_multipart_state::EPILOGUE) {
      return;
    }

    // ---------------------------------------------------------------------
    // 攒部件头
    // ---------------------------------------------------------------------
    if (state_ == uvcpp_web_multipart_state::PART_HEADERS) {
      size_t pos = std::string::npos;
      size_t term = 0;

      // 空头块写成 `\r\n`（就是那个结束头部的空行本身），不写成 `\r\n\r\n`。
      // 先判这一种，否则零头部件的块会被当成"还没结束"一直等下去。
      if (retain_.size() >= 2 && retain_[0] == '\r' && retain_[1] == '\n') {
        pos = 0;
        term = 2;
      } else {
        pos = retain_.find("\r\n\r\n", header_scan_from_);
        term = 4;
      }

      if (pos == std::string::npos) {
        // 与 body 扫描同理：记住扫到哪，回退 3 字节以免把跨块的 `\r\n\r\n`
        // 漏掉。没有这个游标，一次喂一个字节地发 8 KiB 头就是 O(头长²)。
        header_scan_from_ = retain_.size() > 3 ? retain_.size() - 3 : 0;
        // 上限必须在**攒的过程中**判，不能等攒完 ——
        // 否则对端可以一直发头把我们撑爆。
        if (retain_.size() > max_part_header_bytes_) {
          fail(uvcpp_web_multipart_result::ERROR_HEADER_TOO_LONG,
               "部件头超过 max_part_header_bytes");
        }
        return;
      }
      header_scan_from_ = 0;

      // **上限与"终结符到没到"无关。** 只在上面那条"还没攒完"的分支里判是
      // 不够的：整包喂、或终结符恰好和超长头落在同一块里（那才是常态 ——
      // 一个 read 就能把整个头带回来），压根走不到那一条，于是一个 9 KiB 的
      // 部件头畅通无阻地开始解析，`max_part_header_bytes` 形同虚设。
      const size_t header_len = (term == 4) ? pos + 2 : 0;
      if (header_len > max_part_header_bytes_) {
        fail(uvcpp_web_multipart_result::ERROR_HEADER_TOO_LONG,
             "部件头超过 max_part_header_bytes");
        return;
      }

      // **必须把最后一个头行自己的 CRLF 一起收进块里。** `\r\n\r\n` 的前两个
      // 字节是**最后一个头行的行尾**，后两个才是那个空行 —— 只取
      // `substr(0, pos)` 会让块的最后一行没有行尾，而 `parse_part_headers` 是
      // 逐行找 `\r\n` 的，找不到就判畸形：一份完全正常的部件头会得到
      // ERROR_BAD_HEADER，`on_part_begin` 一次都不会被调用。
      //
      // 空头块那一支（`term == 2`）反过来：`pos == 0` 指向那个空行本身，
      // 块就该是空的。
      header_block_ = (term == 4) ? retain_.substr(0, pos + 2) : std::string();
      retain_.erase(0, pos + term);

      if (!begin_part_from_block()) return;

      part_bytes_ = 0;
      truncated_ = false;
      state_ = uvcpp_web_multipart_state::PART_BODY;
      cursor = 0;
      continue;
    }

    // ---------------------------------------------------------------------
    // 找边界（PREAMBLE 与两种 BODY 状态共用同一段扫描）
    //
    // 关键：**PREAMBLE 也走这里**，因为虚拟前置 CRLF 让首边界与后续边界
    // 形状一致。区别只在"命中时有没有部件要收尾"以及 deliver 会不会交付。
    // ---------------------------------------------------------------------
    const size_t d = delim_.size();
    const size_t p = retain_.find(delim_, cursor);

    if (p == std::string::npos) {
      // 没命中 ⇒ 任何命中都只能从 `len - d + 1` 起 ⇒ 之前必然是 body。
      // 交付到那里为止，尾部 d-1 字节留在缓冲里，跨块的半截标记不会丢。
      const size_t keep = d - 1;
      const size_t safe = retain_.size() > keep ? retain_.size() - keep : 0;
      if (safe > cursor) {
        if (!deliver(retain_.data() + cursor, safe - cursor)) return;
        cursor = safe;
      }
      retain_.erase(0, cursor);
      return;  // 等更多数据
    }

    // 命中点之前的部分必然是 body（**即使这个命中稍后被证明是假的**：
    // 假命中的话真边界只会更靠后，那一段仍然在 body 里）。
    if (p > cursor) {
      if (!deliver(retain_.data() + cursor, p - cursor)) return;
      cursor = p;
    }

    // 现在命中在 cursor。判定规则（RFC 2046 §5.1.1 的语法）：
    //     delimiter       := CRLF "--" boundary [LWSP] CRLF
    //     close-delimiter := CRLF "--" boundary [LWSP] "--" [LWSP] CRLF
    // **两个字节不够，整行都要看**：`\r\n--<b>x` 与 `\r\n--<b>--x` 都不是
    // 边界，是 body 里的近似串；把它们当成边界会把报文提前截断，后面的部件
    // 全部消失（比解析失败更坏 —— 它看起来是成功的）。
    size_t i = cursor + d;
    while (i < retain_.size() && (retain_[i] == ' ' || retain_[i] == '\t')) ++i;
    if (i - (cursor + d) > kMaxBoundaryPad) {
      fail(uvcpp_web_multipart_result::ERROR_BAD_HEADER, "边界行的填充过长");
      return;
    }

    bool is_part_delim = false;
    bool is_close_delim = false;
    bool undecided = false;

    if (i >= retain_.size()) {
      undecided = true;  // 连行尾的第一个字节都还没有
    } else if (retain_[i] == '\r') {
      if (i + 1 >= retain_.size()) {
        undecided = true;  // 等 LF
      } else if (retain_[i + 1] == '\n') {
        is_part_delim = true;
      }
    } else if (retain_[i] == '-') {
      if (i + 1 >= retain_.size()) {
        undecided = true;  // 等第二个 '-'
      } else if (retain_[i + 1] == '-') {
        size_t j = i + 2;
        while (j < retain_.size() && (retain_[j] == ' ' || retain_[j] == '\t')) {
          ++j;
        }
        if (j - (i + 2) > kMaxBoundaryPad) {
          fail(uvcpp_web_multipart_result::ERROR_BAD_HEADER, "终边界的填充过长");
          return;
        }
        // 缓冲正好以 `--` 结尾：还判不出来，等更多数据。**客户端可以不发
        // 终边界后面那个 CRLF**（RFC 2046 允许、Go 的 mime/multipart 也接受），
        // 那种情形由 finish() 兜底。
        if (j >= retain_.size()) {
          undecided = true;
        } else if (retain_[j] == '\r' || retain_[j] == '\n') {
          is_close_delim = true;
        }
      }
    }

    // **还没判完就必须把已交付的前缀丢掉再返回。** `cursor` 是本次 scan 的
    // 局部量，而 `find` 下一块数据进来时又是从 0 开始 —— 留着这段缓冲区，
    // 同一段 body 会被**再交付一遍**。这个 bug 只在"切点正好落在边界行中间"
    // 时出现（整包喂永远碰不到），正是 `split_every_offset` 要抓的那一类。
    if (undecided) {
      retain_.erase(0, cursor);
      return;
    }

    if (is_part_delim) {
      retain_.erase(0, i + 2);
      if (!finish_part()) return;
      state_ = uvcpp_web_multipart_state::PART_HEADERS;
      header_scan_from_ = 0;
      cursor = 0;
      continue;
    }

    if (is_close_delim) {
      if (!finish_part()) return;
      // 终边界之后的一切按 RFC 属于 epilogue，丢弃。
      retain_.clear();
      state_ = uvcpp_web_multipart_state::EPILOGUE;
      result_ = uvcpp_web_multipart_result::DONE;
      return;
    }

    // **假命中**：这不是真边界，是 body 里的近似串。第一个字节属于 body，
    // 交付它再往后扫（只前进一个字节 —— 真正的边界可能就从下一个字节开始）。
    if (!deliver(retain_.data() + cursor, 1)) return;
    ++cursor;
  }
}

// =========================================================================
// 驱动
// =========================================================================

uvcpp_web_multipart_result uvcpp_web_multipart::feed(const char* data,
                                                    size_t len) {
  // 终态之后是无害空操作：调用方不必每次 feed 之前查状态。
  // 但 received_ **照常累加** —— 它是"累计喂进来多少字节"的诚实计数器，
  // 上层判"上传总长"要靠它，如果在终态停住，那个数字就是错的。
  if (data != nullptr && len > 0) received_ += len;

  if (state_ == uvcpp_web_multipart_state::ERROR_STATE ||
      state_ == uvcpp_web_multipart_state::EPILOGUE) {
    return result_;
  }
  if (data == nullptr || len == 0) return result_;

  if (delim_.empty()) {
    fail(uvcpp_web_multipart_result::ERROR_NO_BOUNDARY, "还没设置 boundary");
    return result_;
  }

  retain_.append(data, len);
  scan();
  return result_;
}

uvcpp_web_multipart_result uvcpp_web_multipart::finish() {
  if (state_ == uvcpp_web_multipart_state::ERROR_STATE) return result_;
  if (state_ == uvcpp_web_multipart_state::EPILOGUE) return result_;

  // 边界行的判定需要"`--` 之后还有没有字节"，所以缓冲正好停在终边界上时
  // 会一直等下去。**到这里就必须给个说法**：客户端合法地可以不发终边界后面
  // 那个 CRLF（RFC 2046 允许，Go 的 mime/multipart 也接受），所以缓冲里
  // 正好是 `\r\n--<b>--`（+ 可选填充）时按正常结束处理。
  // 少了这一支，一个合法上报文的结尾会被判成"被截断"，整份上传连带临时文件
  // 一起被丢掉。
  if (state_ == uvcpp_web_multipart_state::PART_BODY ||
      state_ == uvcpp_web_multipart_state::PART_BODY_SKIP) {
    const size_t d = delim_.size();
    if (retain_.size() >= d + 2 && retain_.compare(0, d, delim_) == 0 &&
        retain_[d] == '-' && retain_[d + 1] == '-') {
      size_t j = d + 2;
      while (j < retain_.size() && (retain_[j] == ' ' || retain_[j] == '\t')) {
        ++j;
      }
      if (j == retain_.size()) {
        if (!finish_part()) return result_;
        state_ = uvcpp_web_multipart_state::EPILOGUE;
        result_ = uvcpp_web_multipart_result::DONE;
        retain_.clear();
        return result_;
      }
    }
  }

  // 没见过终边界 ⇒ 报文被截断。**这一条必须判出来**：不然一个被掐断的上传
  // 会被当成"正常结束、只是文件小"，而半截文件已经落盘了。
  fail(uvcpp_web_multipart_result::ERROR_BOUNDARY_NEVER_FOUND,
       "报文结束前没有出现终边界（body 被截断）");
  return result_;
}

}  // namespace uvcpp
