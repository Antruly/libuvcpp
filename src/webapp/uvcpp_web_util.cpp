/**
 * @file src/webapp/uvcpp_web_util.cpp
 * @brief `uvcpp_web_util.h` 的实现。
 */

#include <webapp/uvcpp_web_util.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace uvcpp {

// =========================================================================
// 字符串小工具
// =========================================================================

std::string web_to_lower(const std::string& s) {
  std::string r(s);
  for (size_t i = 0; i < r.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(r[i]);
    // 只处理 ASCII：非 ASCII 字节在多字节编码里可能是 0x80-0xFF，
    // ::tolower 在带符号 char 下是未定义行为，而且对 UTF-8 也没有意义。
    if (c >= 'A' && c <= 'Z') r[i] = static_cast<char>(c - 'A' + 'a');
  }
  return r;
}

std::string web_to_upper(const std::string& s) {
  std::string r(s);
  for (size_t i = 0; i < r.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(r[i]);
    if (c >= 'a' && c <= 'z') r[i] = static_cast<char>(c - 'a' + 'A');
  }
  return r;
}

std::string web_trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e) {
    const char c = s[b];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++b;
    else break;
  }
  while (e > b) {
    const char c = s[e - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') --e;
    else break;
  }
  return s.substr(b, e - b);
}

bool web_starts_with(const std::string& s, const std::string& prefix) {
  if (prefix.size() > s.size()) return false;
  return s.compare(0, prefix.size(), prefix) == 0;
}

bool web_ends_with(const std::string& s, const std::string& suffix) {
  if (suffix.size() > s.size()) return false;
  return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool web_starts_with_ci(const std::string& s, const std::string& prefix) {
  if (prefix.size() > s.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    unsigned char a = static_cast<unsigned char>(s[i]);
    unsigned char b = static_cast<unsigned char>(prefix[i]);
    if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

bool web_equals_ci(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    unsigned char x = static_cast<unsigned char>(a[i]);
    unsigned char y = static_cast<unsigned char>(b[i]);
    if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

void web_split(const std::string& s, char sep, std::vector<std::string>& out,
               bool keep_empty) {
  out.clear();
  size_t start = 0;
  while (true) {
    const size_t p = s.find(sep, start);
    const size_t end = (p == std::string::npos) ? s.size() : p;
    if (end > start || keep_empty) out.push_back(s.substr(start, end - start));
    if (p == std::string::npos) break;
    start = p + 1;
  }
}

std::vector<std::string> web_split(const std::string& s, char sep,
                                   bool keep_empty) {
  std::vector<std::string> out;
  web_split(s, sep, out, keep_empty);
  return out;
}

// =========================================================================
// 百分号编解码
// =========================================================================

int web_hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string web_url_decode(const std::string& s, bool plus_as_space, bool* ok) {
  std::string out;
  out.reserve(s.size());
  bool good = true;

  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '%') {
      if (i + 2 < s.size()) {
        const int hi = web_hex_value(s[i + 1]);
        const int lo = web_hex_value(s[i + 2]);
        if (hi >= 0 && lo >= 0) {
          out += static_cast<char>((hi << 4) | lo);
          i += 2;
          continue;
        }
      }
      // 非法序列：按字面保留 '%' 并报告。不中断 —— 这样调用方既能拿到
      // 一个可用的近似结果，也能通过 ok 决定要不要拒绝。
      good = false;
      out += '%';
      continue;
    }
    if (c == '+' && plus_as_space) {
      out += ' ';
      continue;
    }
    out += c;
  }

  if (ok != nullptr) *ok = good;
  return out;
}

std::string web_url_encode(const std::string& s, const std::string& keep,
                           bool plus_for_space) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());

  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~';
    if (unreserved || keep.find(static_cast<char>(c)) != std::string::npos) {
      out += static_cast<char>(c);
      continue;
    }
    if (c == ' ' && plus_for_space) {
      out += '+';
      continue;
    }
    // RFC 3986 推荐大写十六进制
    out += '%';
    out += kHex[c >> 4];
    out += kHex[c & 0x0F];
  }
  return out;
}

// =========================================================================
// URL 拆解
// =========================================================================

void web_split_path_query(const std::string& raw_url, std::string& path,
                          std::string& query) {
  std::string u = raw_url;

  // 绝对形式（代理风格）：`GET http://host/path HTTP/1.1`。
  // 必须先把 scheme://authority 剥掉 —— 否则整串会被当成路径，
  // 而且 authority 里的内容会混进路径里（`http://x/../y` 之类）。
  if (web_starts_with_ci(u, "http://") || web_starts_with_ci(u, "https://")) {
    const size_t scheme_end = u.find("://") + 3;
    const size_t slash = u.find('/', scheme_end);
    u = (slash == std::string::npos) ? std::string("/") : u.substr(slash);
  }

  // 片段标识符：HTTP 请求目标里不该有，但客户端库时常带上。
  const size_t hash = u.find('#');
  if (hash != std::string::npos) u.erase(hash);

  const size_t q = u.find('?');
  if (q == std::string::npos) {
    path = u;
    query.clear();
  } else {
    path = u.substr(0, q);
    query = u.substr(q + 1);
  }
}

std::vector<std::pair<std::string, std::string> > web_parse_query(
    const std::string& query) {
  std::vector<std::pair<std::string, std::string> > out;
  if (query.empty()) return out;

  size_t start = 0;
  while (true) {
    const size_t amp = query.find('&', start);
    const size_t end = (amp == std::string::npos) ? query.size() : amp;

    // 跳过空段（"a=1&&b=2" 中间那个）
    if (end > start) {
      const std::string kv = query.substr(start, end - start);
      const size_t eq = kv.find('=');
      std::string k;
      std::string v;
      if (eq == std::string::npos) {
        k = kv;  // 裸键，值为空
      } else {
        k = kv.substr(0, eq);
        v = kv.substr(eq + 1);  // 值里可以再有 '='
      }
      // 查询串里 '+' 是空格，所以 plus_as_space = true
      out.push_back(std::make_pair(web_url_decode(k, true),
                                   web_url_decode(v, true)));
    }

    if (amp == std::string::npos) break;
    start = amp + 1;
  }
  return out;
}

const std::string* web_find_param(
    const std::vector<std::pair<std::string, std::string> >& params,
    const std::string& name) {
  for (size_t i = 0; i < params.size(); ++i) {
    if (params[i].first == name) return &params[i].second;
  }
  return nullptr;
}

// =========================================================================
// Content-Type 参数
// =========================================================================

std::string web_multipart_boundary(const std::string& content_type) {
  // 第一步：按分号切参数，但**引号里的分号不算分隔符**。
  //
  // 手写而不是 `web_split(content_type, ';')`：后者会把
  // `boundary="a;b"` 切成两段，于是值变成 `"a`（连引号都没配成对）。
  // 这条规则与部件头的解析是同一条 —— 两处必须一致，否则同一个 boundary
  // 在"判断这是不是 multipart"和"真的去切部件"两个阶段会不一样。
  //
  // 转义对（`\x`）在这里原样带过，留到第二步统一反转义：在这一步吃掉反斜杠
  // 的话，`\"` 会被当成引号本身、把引号状态弄反。
  std::vector<std::string> parts;
  {
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < content_type.size(); ++i) {
      const char c = content_type[i];
      if (c == '\\' && in_quotes && i + 1 < content_type.size()) {
        cur += c;
        cur += content_type[++i];
        continue;
      }
      if (c == '"') in_quotes = !in_quotes;
      if (c == ';' && !in_quotes) {
        parts.push_back(cur);
        cur.clear();
        continue;
      }
      cur += c;
    }
    parts.push_back(cur);
  }

  // 媒体类型必须**恰好**是 multipart/form-data。其他 multipart 子类型
  // （mixed / byteranges…）不是表单上传，拒绝而不是勉强解析。
  if (!web_equals_ci(web_trim(parts[0]), "multipart/form-data")) {
    return std::string();
  }

  for (size_t i = 1; i < parts.size(); ++i) {
    const std::string p = web_trim(parts[i]);
    const size_t eq = p.find('=');
    if (eq == std::string::npos) continue;  // 没有值的参数（`; charset`）
    if (!web_equals_ci(web_trim(p.substr(0, eq)), "boundary")) continue;

    std::string v = web_trim(p.substr(eq + 1));
    // 成对的引号去掉。只在**首尾都是引号**时去 —— `"abc` 这种畸形保持原样，
    // 让 set_boundary() 去判（它才是唯一该对边界值表态的地方）。
    if (v.size() >= 2 && v[0] == '"' && v[v.size() - 1] == '"') {
      v = v.substr(1, v.size() - 2);
    }

    std::string out;
    out.reserve(v.size());
    for (size_t k = 0; k < v.size(); ++k) {
      if (v[k] == '\\' && k + 1 < v.size()) {
        out += v[++k];
        continue;
      }
      out += v[k];
    }
    // 可能是空串（`boundary=""`）—— 按"没有 boundary"交给调用方，
    // 不在这一层区分"没写"和"写了个空的"：两者都不可用。
    return out;
  }

  return std::string();
}

// =========================================================================
// Cookie
// =========================================================================

std::vector<std::pair<std::string, std::string> > web_parse_cookies(
    const std::string& cookie_header) {
  std::vector<std::pair<std::string, std::string> > out;

  size_t start = 0;
  while (start <= cookie_header.size()) {
    const size_t semi = cookie_header.find(';', start);
    const size_t end = (semi == std::string::npos) ? cookie_header.size() : semi;

    if (end > start) {
      const std::string kv = web_trim(cookie_header.substr(start, end - start));
      const size_t eq = kv.find('=');
      if (eq != std::string::npos) {
        const std::string k = web_trim(kv.substr(0, eq));
        std::string v = web_trim(kv.substr(eq + 1));
        // 有些客户端会给值加引号（RFC 6265 之前的老习惯），去掉。
        if (v.size() >= 2 && v[0] == '"' && v[v.size() - 1] == '"') {
          v = v.substr(1, v.size() - 2);
        }
        if (!k.empty()) out.push_back(std::make_pair(k, v));
      }
      // 没有 '=' 的段直接丢弃：Cookie 头里它不可能是合法条目。
    }

    if (semi == std::string::npos) break;
    start = semi + 1;
  }

  // 注意：这里**不做百分号解码**。浏览器不会对 cookie 值做百分号编码，
  // 原样收发才是正确语义。使用者若要自己编码，自行解码。
  return out;
}

const std::string* web_find_cookie(
    const std::vector<std::pair<std::string, std::string> >& cookies,
    const std::string& name) {
  for (size_t i = 0; i < cookies.size(); ++i) {
    if (cookies[i].first == name) return &cookies[i].second;
  }
  return nullptr;
}

namespace {

/// RFC 7230 的 token 字符集（cookie 名字必须是 token）。
bool is_token_char(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9'))
    return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

/**
 * @brief 按 RFC 6265 的 cookie-octet 编码值。
 *
 * 合法集合是「可打印 ASCII 减去空格、双引号、逗号、分号、反斜杠」。
 * 这里取得更严（只放行 `A-Za-z0-9-._~`），因为严格永远安全，
 * 而且能顺手把 `%` 也编码掉，避免与百分号编码混淆。
 */
std::string encode_cookie_value(const std::string& v) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(v[i]);
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (safe) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

}  // namespace

std::string web_build_cookie(const std::string& name, const std::string& value,
                             const std::string& path, long max_age,
                             bool http_only, bool secure,
                             const std::string& same_site) {
  // 名字非法就直接返回空串：宁可调用方少发一条 cookie，也不能拼出一条
  // 能被注入额外属性的 cookie（名字里塞 `; Path=/` 就是这种攻击）。
  if (name.empty()) return std::string();
  for (size_t i = 0; i < name.size(); ++i) {
    if (!is_token_char(static_cast<unsigned char>(name[i]))) return std::string();
  }

  std::string out;
  out += name;
  out += '=';
  out += encode_cookie_value(value);

  if (!path.empty()) {
    out += "; Path=";
    out += path;
  }
  if (max_age >= 0) {
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%ld", max_age);
    if (n > 0) {
      out += "; Max-Age=";
      out.append(buf, static_cast<size_t>(n));
    }
  }
  if (!same_site.empty()) {
    out += "; SameSite=";
    out += same_site;
    // SameSite=None 必须配 Secure，否则现代浏览器整条丢弃。
    if (web_equals_ci(same_site, "None")) secure = true;
  }
  if (secure) out += "; Secure";
  if (http_only) out += "; HttpOnly";

  return out;
}

// =========================================================================
// 静态文件路径安全核心
// =========================================================================

const char* web_path_status_name(web_path_status status) {
  switch (status) {
    case web_path_status::OK:            return "OK";
    case web_path_status::EMPTY:         return "EMPTY";
    case web_path_status::NOT_ABSOLUTE:  return "NOT_ABSOLUTE";
    case web_path_status::BAD_PERCENT:   return "BAD_PERCENT";
    case web_path_status::ENCODED_NUL:   return "ENCODED_NUL";
    case web_path_status::CONTROL_CHAR:  return "CONTROL_CHAR";
    case web_path_status::BACKSLASH:     return "BACKSLASH";
    case web_path_status::COLON:         return "COLON";
    case web_path_status::UNC_PREFIX:    return "UNC_PREFIX";
    case web_path_status::TRAVERSAL:     return "TRAVERSAL";
    case web_path_status::TOO_LONG:      return "TOO_LONG";
    case web_path_status::TOO_DEEP:      return "TOO_DEEP";
    case web_path_status::NOT_FOUND:     return "NOT_FOUND";
  }
  return "?";
}

web_path_options::web_path_options(size_t max_len, size_t max_dep, bool dec)
    : max_length(max_len), max_depth(max_dep), decode(dec) {}

web_path_status web_sanitize_path(const std::string& raw, std::string& out) {
  return web_sanitize_path(raw, out, web_path_options());
}

web_path_status web_sanitize_path(const std::string& raw, std::string& out,
                                  const web_path_options& opts) {
  if (raw.empty()) return web_path_status::EMPTY;

  // 解码只会让字符串变短（`%41` 三字节 → `A` 一字节），所以先按原始长度
  // 做一次廉价的上限检查，避免为一个超长串白白分配内存。
  if (opts.decode && raw.size() > opts.max_length)
    return web_path_status::TOO_LONG;

  std::string path;
  if (opts.decode) {
    bool ok = false;
    path = web_url_decode(raw, /*plus_as_space=*/false, &ok);
    // 路径里的 '+' 是字面加号，不是空格 —— 这一点与查询串相反。
    if (!ok) return web_path_status::BAD_PERCENT;
  } else {
    path = raw;
  }

  if (path.empty()) return web_path_status::EMPTY;
  if (path.size() > opts.max_length) return web_path_status::TOO_LONG;

  // --- 逐字节检查（必须在解码之后做，否则 %00 / %5c / %3a 全都能溜过去）
  for (size_t i = 0; i < path.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(path[i]);
    if (c == 0x00) return web_path_status::ENCODED_NUL;
    if (c < 0x20 || c == 0x7f) return web_path_status::CONTROL_CHAR;
    // Windows 上 '\' 与 '/' 同样是分隔符，`..\..\` 靠它绕过。
    if (c == '\\') return web_path_status::BACKSLASH;
    // 盘符（`C:`）与 NTFS 备用数据流（`file.txt:evil`）
    if (c == ':') return web_path_status::COLON;
  }

  if (path[0] != '/') return web_path_status::NOT_ABSOLUTE;
  // 前导 `//`：POSIX 下语义由实现定义，Windows 下是 UNC（`//server/share`）。
  // 两种都不该出现在本站路径里，直接拒绝。
  if (path.size() >= 2 && path[1] == '/') return web_path_status::UNC_PREFIX;

  // --- 分段归一化
  std::string norm;
  norm.reserve(path.size());
  size_t depth = 0;
  size_t i = 1;

  while (i <= path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    const size_t len = j - i;

    if (len == 0) {
      // `//` 产生的空段：丢弃
    } else if (len == 1 && path[i] == '.') {
      // `.` 段：丢弃
    } else if (len == 2 && path[i] == '.' && path[i + 1] == '.') {
      // `..`：弹出上一段。栈空说明它要越过根 —— 这就是目录穿越。
      //
      // 这里**不做静默截断**（把 `/../etc` 当成 `/etc`）。截断看起来
      // "更宽容"，实际是把一次攻击变成一条正常响应，日志里再也看不出
      // 有人试过，也让测试无法区分「挡住了」和「碰巧没事」。
      if (depth == 0) return web_path_status::TRAVERSAL;
      --depth;
      norm.erase(norm.rfind('/'));
    } else {
      norm += '/';
      norm.append(path, i, len);
      ++depth;
      if (depth > opts.max_depth) return web_path_status::TOO_DEEP;
    }

    if (j == path.size()) break;
    i = j + 1;
  }

  if (norm.empty()) norm = "/";  // 全部被归一化掉 → 根
  out = norm;
  return web_path_status::OK;
}

std::string web_join_url(const std::string& url_root,
                         const std::string& url_path,
                         const std::string& index_file) {
  std::string root = url_root;
  if (root.empty()) root = "/";
  if (root.size() > 1 && root[root.size() - 1] == '/') root.erase(root.size() - 1);

  std::string rest;
  if (root == "/") {
    rest = url_path;
  } else {
    if (url_path == root) {
      rest = "/";
    } else if (web_starts_with(url_path, root) &&
               url_path.size() > root.size() && url_path[root.size()] == '/') {
      rest = url_path.substr(root.size());
    } else {
      return std::string();  // 不在挂载点下
    }
  }

  if (!rest.empty() && rest[0] != '/') rest = "/" + rest;
  if (rest.empty() || rest == "/") {
    if (index_file.empty()) return std::string();
    rest = "/" + index_file;
  }
  return rest;
}

std::string web_join_root(const std::string& root, const std::string& url_path) {
  if (root.empty()) return url_path;
  std::string r = root;
  const char last = r[r.size() - 1];
  const bool has_sep = (last == '/' || last == '\\');
  if (!has_sep) r += '/';
  if (url_path.empty()) return r;
  if (url_path[0] == '/') return r + url_path.substr(1);
  return r + url_path;
}

namespace {

/// 归一化成「可比较」的形式：统一分隔符、Windows 下统一小写、去掉结尾分隔符。
std::string canonical_for_compare(const std::string& p) {
  std::string s = p;
#if defined(_WIN32)
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '/') {
      s[i] = '\\';
      continue;
    }
    if (c >= 'A' && c <= 'Z') s[i] = static_cast<char>(c - 'A' + 'a');
  }
  // 保留 "c:\" 这种根形式（长度 3），否则会把根本身削成 "c:"
  while (s.size() > 3 && s[s.size() - 1] == '\\') s.erase(s.size() - 1);
#else
  while (s.size() > 1 && s[s.size() - 1] == '/') s.erase(s.size() - 1);
#endif
  return s;
}

#if defined(_WIN32)
const char k_path_sep = '\\';
#else
const char k_path_sep = '/';
#endif

}  // namespace

bool web_is_within_root(const std::string& root_real,
                        const std::string& candidate_real) {
  const std::string r = canonical_for_compare(root_real);
  const std::string c = canonical_for_compare(candidate_real);
  if (r.empty() || c.empty()) return false;
  if (c == r) return true;
  if (c.size() <= r.size()) return false;

  // 逐字符比较前缀。**不能**只写 `c.compare(0, r.size(), r) == 0` 就完事：
  // 那样 `/srv/webroot2` 会被判为在 `/srv/webroot` 之内，因为它是前缀匹配。
  // 边界必须是分隔符。
  if (c.compare(0, r.size(), r) != 0) return false;
  if (r[r.size() - 1] == k_path_sep) return true;  // 根自带分隔符，边界已成立
  return c[r.size()] == k_path_sep;
}

namespace {

#if defined(_WIN32)

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), NULL, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], n);
  return w;
}

std::string wide_to_utf8(const wchar_t* w) {
  if (w == NULL) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  if (n <= 1) return std::string();
  std::string s(static_cast<size_t>(n - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
  return s;
}

/**
 * @brief Windows 上的真实路径解析。
 *
 * 用 `GetFinalPathNameByHandleW` 而不是 `_fullpath`：后者只做 `..`/`.` 的
 * 文本归一化，**不展开重解析点**（junction / 符号链接 / 挂载点）。而静态
 * 服务最需要防的恰恰是「根目录里有个 junction 指向 C:\Windows」这种情形 ——
 * 文本层面完全合法，`_fullpath` 也会说没问题。
 */
bool real_path_impl(const std::string& in, std::string& out) {
  const std::wstring w = utf8_to_wide(in);
  if (w.empty()) return false;

  // 0 作为 desired access：只要路径解析，不需要读数据。
  // FILE_FLAG_BACKUP_SEMANTICS 是打开**目录**的必要条件。
  HANDLE h = ::CreateFileW(w.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                           NULL);
  if (h == INVALID_HANDLE_VALUE) return false;

  // 路径最长可到 32767 个宽字符，放栈上太大（64KB），用堆。
  std::vector<wchar_t> buf(32768, L'\0');
  const DWORD n = ::GetFinalPathNameByHandleW(
      h, &buf[0], static_cast<DWORD>(buf.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  ::CloseHandle(h);
  if (n == 0 || n >= buf.size()) return false;

  out = wide_to_utf8(&buf[0]);
  if (out.empty()) return false;

  // 这个 API 一律返回带 `\\?\` 前缀的路径，剥掉才能和普通路径比较。
  if (web_starts_with(out, "\\\\?\\UNC\\")) {
    out = "\\\\" + out.substr(8);  // \\?\UNC\srv\share → \\srv\share
  } else if (web_starts_with(out, "\\\\?\\")) {
    out = out.substr(4);
  }
  return true;
}

#else  // POSIX

bool real_path_impl(const std::string& in, std::string& out) {
  // realpath(path, NULL) 会自己 malloc（POSIX.1-2008）
  char* r = ::realpath(in.c_str(), NULL);
  if (r == NULL) return false;
  out.assign(r);
  ::free(r);
  return true;
}

#endif

/// 去掉路径的最后一段，返回分隔符的位置；找不到返回 npos。
size_t last_sep_pos(const std::string& p) {
#if defined(_WIN32)
  return p.find_last_of("/\\");
#else
  return p.find_last_of('/');
#endif
}

}  // namespace

bool web_real_path(const std::string& path, std::string& out,
                   bool allow_missing) {
  if (path.empty()) return false;

  if (real_path_impl(path, out)) return true;
  if (!allow_missing) return false;

  // 文件不存在时 realpath 必然失败，但静态服务需要为「不存在的文件」得出
  // 一个可信的绝对路径（好判断它到底是在根内还是根外，然后回 404 而不是 500）。
  //
  // 办法是把最后一段摘下来，先解析**父目录** —— 父目录必须存在，否则这个
  // 请求连落点都没有。符号链接只可能出现在已有的那些层级里，而父目录的
  // 解析已经把那些层级全部展开，所以这个结果对包含判断是可信的。
  const size_t sep = last_sep_pos(path);
  if (sep == std::string::npos) return false;  // 光秃秃一个相对文件名，没法定位

  const std::string tail = path.substr(sep + 1);
  // 最后一段如果是 `.`/`..`/空，说明原路径本身不规范，不接手。
  if (tail.empty() || tail == "." || tail == "..") return false;

  std::string parent;
  if (!real_path_impl(path.substr(0, sep), parent)) return false;

  if (!parent.empty() && parent[parent.size() - 1] != '/' &&
      parent[parent.size() - 1] != '\\') {
    parent += k_path_sep;
  }
  out = parent + tail;
  return true;
}

web_path_status web_resolve_within_root(const std::string& root_real,
                                        const std::string& url_path,
                                        std::string& out) {
  if (root_real.empty()) return web_path_status::EMPTY;
  if (url_path.empty() || url_path[0] != '/')
    return web_path_status::NOT_ABSOLUTE;

  const std::string joined = web_join_root(root_real, url_path);

  std::string real;
  if (!web_real_path(joined, real, /*allow_missing=*/true))
    return web_path_status::NOT_FOUND;

  // 真正的安全边界。走到这里 `url_path` 已经过 web_sanitize_path()，
  // 但那是纯文本的；这一层把符号链接、junction 也一并算清楚。
  if (!web_is_within_root(root_real, real)) return web_path_status::TRAVERSAL;

  out = real;
  return web_path_status::OK;
}

// =========================================================================
// 上传文件名的元数据清洗
// =========================================================================

namespace {

/**
 * @brief Win32 的保留设备名判定（入参**必须已小写**）。
 *
 * 两个容易漏的点：
 *   - **带扩展名也算**：`NUL.txt` 在 Win32 上仍然解析成设备，所以比较只取
 *     第一个 `.` 之前的部分；
 *   - **上标变体**：Windows 把 `COM¹`/`COM²`/`COM³` 与 `LPT¹`/`LPT²`/`LPT³`
 *     也当设备名。它们在 UTF-8 里各占两字节（`C2 B9`/`C2 B2`/`C2 B3`），
 *     所以 `stem` 的长度是 5 而不是 4 —— 这一支单列出来，不要试图用
 *     "第 4 个字符是数字" 那种 ASCII 判据去覆盖它。
 */
bool is_reserved_device_name(const std::string& lower) {
  const size_t dot = lower.find('.');
  const std::string stem =
      (dot == std::string::npos) ? lower : lower.substr(0, dot);

  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") {
    return true;
  }
  if (stem.size() < 4) return false;

  const bool com = stem.compare(0, 3, "com") == 0;
  const bool lpt = stem.compare(0, 3, "lpt") == 0;
  if (!com && !lpt) return false;

  if (stem.size() == 4) {
    const char c = stem[3];
    return c >= '1' && c <= '9';
  }
  if (stem.size() == 5 && static_cast<unsigned char>(stem[3]) == 0xC2) {
    const unsigned char c = static_cast<unsigned char>(stem[4]);
    return c == 0xB9 || c == 0xB2 || c == 0xB3;  // ¹ ² ³
  }
  return false;
}

}  // namespace

std::string web_sanitize_filename(const std::string& raw, size_t max_len) {
  // 1. 取叶子。**这一步必须在去掉结尾的点之前** —— 反过来 `..\..\x` 会先被
  //    第 3 步削成 `..\..\x`（结尾没有点，削不动），然后 `..` 段就留在了
  //    结果里。先取叶子，`..` 段天然被丢掉。
  std::string leaf;
  {
    size_t start = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '/' || raw[i] == '\\') start = i + 1;
    }
    leaf = raw.substr(start);
  }

  // 2. 去控制字符。NUL 尤其要紧：它会让任何 C 字符串 API 在这里被截断，
  //    于是"校验的名字"和"使用的名字"变成两个不同的串。
  std::string clean;
  clean.reserve(leaf.size());
  for (size_t i = 0; i < leaf.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(leaf[i]);
    if (c < 0x20 || c == 0x7f) continue;
    clean.push_back(leaf[i]);
  }

  // 3. 去掉结尾的 `.` 与空格（Win32 会静默截掉，不清洗就会让展示名与落盘名
  //    不一致）。`"..."`/`".."`/`"."` 在这里一并变成空串。
  while (!clean.empty() &&
         (clean[clean.size() - 1] == '.' || clean[clean.size() - 1] == ' ')) {
    clean.erase(clean.size() - 1);
  }

  // 4. 退化。空 / `.` / `..` 都取 `"file"`；后两者其实在第 3 步就已经空了，
  //    显式写出来是为了让契约可读（用例也照这条断言）。
  if (clean.empty() || clean == "." || clean == "..") clean = "file";

  // 5. 截断，保持码点完整。UTF-8 的续字节是 `10xxxxxx`：只要截断点上不是续
  //    字节，就没有切出半个字符。对**非法** UTF-8 输入这是尽力而为（回退到
  //    一个不含续字节的位置），不报错 —— 元数据清洗不该因为编码不规范就失败。
  if (max_len > 0 && clean.size() > max_len) {
    size_t n = max_len;
    while (n > 0 && (static_cast<unsigned char>(clean[n]) & 0xC0) == 0x80) --n;
    clean.resize(n);
    if (clean.empty()) clean = "file";
  }

  // 6. 设备名保护。**排在截断之后**：截断能造出设备名（`"nul.txt"` 截到 3
  //    字节就是 `"nul"`），先判就漏了。
  if (is_reserved_device_name(web_to_lower(clean))) clean += "_";

  return clean;
}

// =========================================================================
// MIME
// =========================================================================

namespace {

// 表不大（几十项），线性扫描足够；真正的大表再考虑排序 + 二分。
// 扩展名一律小写、不含点。
//
// 类型直接用公开的 `web_mime_builtin_entry`，而不是本文件私有结构体：
// `uvcpp_web_mime_map` 要照抄这张表作为初值，用同一个类型就只有一份定义，
// 也就不用做（日后会漂移的）结构体转换。
const web_mime_builtin_entry k_mime_table[] = {
    {"html", "text/html"},          {"htm", "text/html"},
    {"css", "text/css"},            {"js", "application/javascript"},
    {"mjs", "application/javascript"},
    {"json", "application/json"},   {"xml", "application/xml"},
    {"txt", "text/plain"},          {"md", "text/markdown"},
    {"csv", "text/csv"},            {"ics", "text/calendar"},
    {"png", "image/png"},           {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},         {"gif", "image/gif"},
    {"webp", "image/webp"},         {"bmp", "image/bmp"},
    {"svg", "image/svg+xml"},       {"ico", "image/x-icon"},
    {"avif", "image/avif"},         {"tif", "image/tiff"},
    {"tiff", "image/tiff"},
    {"mp3", "audio/mpeg"},          {"wav", "audio/wav"},
    {"ogg", "audio/ogg"},           {"oga", "audio/ogg"},
    {"m4a", "audio/mp4"},           {"flac", "audio/flac"},
    {"aac", "audio/aac"},           {"opus", "audio/opus"},
    {"mp4", "video/mp4"},           {"m4v", "video/mp4"},
    {"webm", "video/webm"},         {"ogv", "video/ogg"},
    {"mov", "video/quicktime"},     {"avi", "video/x-msvideo"},
    {"mkv", "video/x-matroska"},    {"ts", "video/mp2t"},
    {"woff", "font/woff"},          {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},            {"otf", "font/otf"},
    {"eot", "application/vnd.ms-fontobject"},
    {"pdf", "application/pdf"},     {"zip", "application/zip"},
    {"gz", "application/gzip"},     {"tar", "application/x-tar"},
    {"7z", "application/x-7z-compressed"},
    {"rar", "application/vnd.rar"},
    {"bz2", "application/x-bzip2"},
    {"wasm", "application/wasm"},
    {"map", "application/json"},    {"webmanifest", "application/manifest+json"},
    {"doc", "application/msword"},
    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {"xls", "application/vnd.ms-excel"},
    {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"ppt", "application/vnd.ms-powerpoint"},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"bin", "application/octet-stream"},
};

const char* const k_default_mime = "application/octet-stream";

}  // namespace

const web_mime_builtin_entry* web_mime_builtin_table(size_t& count) {
  count = sizeof(k_mime_table) / sizeof(k_mime_table[0]);
  return k_mime_table;
}

const char* web_mime_type(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= path.size()) return k_default_mime;

  // 路径里 `?`/`#` 已被上游剥掉，但保险起见再截一次
  std::string ext = path.substr(dot + 1);
  const size_t cut = ext.find_first_of("?#/\\");
  if (cut != std::string::npos) ext.erase(cut);
  if (ext.empty()) return k_default_mime;

  const std::string low = web_to_lower(ext);
  const size_t n = sizeof(k_mime_table) / sizeof(k_mime_table[0]);
  for (size_t i = 0; i < n; ++i) {
    if (low == k_mime_table[i].ext) return k_mime_table[i].type;
  }
  return k_default_mime;
}

bool web_mime_is_text(const std::string& mime) {
  // 只看类型部分，忽略 `; charset=...`
  const size_t semi = mime.find(';');
  const std::string base =
      web_trim(semi == std::string::npos ? mime : mime.substr(0, semi));

  if (web_starts_with_ci(base, "text/")) return true;
  // 这些 application/* 本质是文本，要带 charset、也可以压缩
  static const char* const k_texty[] = {
      "application/javascript",
      "application/x-javascript",
      "application/json",
      "application/xml",
      "application/xhtml+xml",
      "application/manifest+json",
      "application/ld+json",
      "image/svg+xml",
  };
  const size_t n = sizeof(k_texty) / sizeof(k_texty[0]);
  for (size_t i = 0; i < n; ++i) {
    if (web_equals_ci(base, k_texty[i])) return true;
  }
  return false;
}

// =========================================================================
// 状态码
// =========================================================================

const char* web_status_text(int code) {
  switch (code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Entity";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 507: return "Insufficient Storage";
    default:  return "Unknown";
  }
}

// =========================================================================
// HTTP 日期
// =========================================================================

namespace {

const char* const k_wday_names[] = {"Sun", "Mon", "Tue", "Wed",
                                    "Thu", "Fri", "Sat"};
const char* const k_mon_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

int month_from_name(const std::string& s) {
  for (int i = 0; i < 12; ++i) {
    if (web_equals_ci(s, k_mon_names[i])) return i;
  }
  return -1;
}

/// 严格解析十进制无符号整数。超长（可能溢出）或含非数字都算失败。
bool parse_uint(const std::string& s, size_t pos, size_t len, int& out) {
  if (len == 0 || len > 9) return false;
  if (pos + len > s.size()) return false;
  int v = 0;
  for (size_t i = 0; i < len; ++i) {
    const char c = s[pos + i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}

/// 按任意一个分隔符切分，丢弃空段。
void split_any(const std::string& s, const char* seps,
               std::vector<std::string>& out) {
  out.clear();
  std::string cur;
  for (size_t i = 0; i < s.size(); ++i) {
    if (std::strchr(seps, s[i]) != NULL) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur += s[i];
    }
  }
  if (!cur.empty()) out.push_back(cur);
}

/**
 * @brief 公历日期 → 1970-01-01 起的天数（Howard Hinnant 的 days_from_civil）。
 *
 * 用它而不是 `timegm` / `_mkgmtime`：后两者一个不是 POSIX（glibc 在
 * `-std=c++11` 的严格模式下可能不声明），一个是 Windows 专有，都得加平台
 * 分支。这个算法是纯整数运算，两个平台完全一致，也没有时区/夏令时的干扰
 * —— 而 HTTP 日期按定义就是 GMT。
 *
 * @param m 月份，**1-12**（不是 tm 的 0-11）
 */
long long days_from_civil(long long y, unsigned m, unsigned d) {
  y -= (m <= 2) ? 1 : 0;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);            // [0,399]
  const unsigned doy =
      (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;              // [0,365]
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;         // [0,146096]
  return era * 146097LL + static_cast<long long>(doe) - 719468LL;
}

}  // namespace

std::string web_http_date(time_t t) {
  struct tm tmv;
#if defined(_WIN32)
  if (::gmtime_s(&tmv, &t) != 0) return std::string();
#else
  if (::gmtime_r(&t, &tmv) == NULL) return std::string();
#endif

  char buf[64];
  // 必须是 GMT。用本地时间格式化是这块最经典的历史 bug（缓存因此全乱）。
  const int n = std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                              k_wday_names[tmv.tm_wday % 7], tmv.tm_mday,
                              k_mon_names[tmv.tm_mon % 12], tmv.tm_year + 1900,
                              tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  if (n <= 0) return std::string();
  return std::string(buf, static_cast<size_t>(n));
}

std::string web_http_date_now() { return web_http_date(::time(NULL)); }

time_t web_parse_http_date(const std::string& s) {
  const std::string t = web_trim(s);
  if (t.empty()) return static_cast<time_t>(-1);

  std::vector<std::string> tok;
  // 空格、逗号、连字符、冒号一律当分隔符，三种格式就都能切
  split_any(t, " \t,-:", tok);

  int year = -1, mon = -1, day = -1, hh = -1, mi = -1, ss = -1;

  // 两种形态：
  //   有逗号 → IMF-fixdate(`Sun, 06 Nov 1994 08:49:37 GMT`)
  //            或 RFC 850(`Sunday, 06-Nov-94 08:49:37 GMT`)
  //            共同点是逗号前是星期几，切完之后字段位置一致，只有年份
  //            宽度不同（RFC 850 是两位）。
  //   无逗号 → asctime(`Sun Nov  6 08:49:37 1994`)，字段整体前移一位。
  const bool has_comma = (t.find(',') != std::string::npos);

  if (has_comma) {
    if (tok.size() < 7) return static_cast<time_t>(-1);
    if (!parse_uint(tok[1], 0, tok[1].size(), day)) return static_cast<time_t>(-1);
    mon = month_from_name(tok[2]);
    if (mon < 0) return static_cast<time_t>(-1);
    if (!parse_uint(tok[3], 0, tok[3].size(), year)) return static_cast<time_t>(-1);
    // RFC 850 的两位年。RFC 7231 要求把「看起来超过 50 年后」的解释成过去，
    // 70 这个分界是通行做法。
    if (year < 100) year += (year < 70) ? 2000 : 1900;
    if (!parse_uint(tok[4], 0, tok[4].size(), hh)) return static_cast<time_t>(-1);
    if (!parse_uint(tok[5], 0, tok[5].size(), mi)) return static_cast<time_t>(-1);
    if (!parse_uint(tok[6], 0, tok[6].size(), ss)) return static_cast<time_t>(-1);
    // 时区可选；给了就必须是 GMT/UTC/Z
    if (tok.size() >= 8 && !web_equals_ci(tok[7], "GMT") &&
        !web_equals_ci(tok[7], "UTC") && !web_equals_ci(tok[7], "Z")) {
      return static_cast<time_t>(-1);
    }
  } else {
    if (tok.size() < 7) return static_cast<time_t>(-1);
    mon = month_from_name(tok[1]);
    if (mon < 0) return static_cast<time_t>(-1);
    if (!parse_uint(tok[2], 0, tok[2].size(), day)) return static_cast<time_t>(-1);
    if (!parse_uint(tok[3], 0, tok[3].size(), hh)) return static_cast<time_t>(-1);
    if (!parse_uint(tok[4], 0, tok[4].size(), mi)) return static_cast<time_t>(-1);
    if (!parse_uint(tok[5], 0, tok[5].size(), ss)) return static_cast<time_t>(-1);
    if (!parse_uint(tok[6], 0, tok[6].size(), year)) return static_cast<time_t>(-1);
  }

  // 范围检查：不查的话 `99 Nov` 会被 days_from_civil 算出一个"合理"的日期，
  // 把一个畸形请求变成一次成功的条件请求。
  if (mon < 0 || mon > 11) return static_cast<time_t>(-1);
  if (day < 1 || day > 31) return static_cast<time_t>(-1);
  if (hh < 0 || hh > 23) return static_cast<time_t>(-1);
  if (mi < 0 || mi > 59) return static_cast<time_t>(-1);
  if (ss < 0 || ss > 60) return static_cast<time_t>(-1);  // 60 = 闰秒
  if (year < 1970 || year > 9999) return static_cast<time_t>(-1);

  const long long days = days_from_civil(year, static_cast<unsigned>(mon + 1),
                                         static_cast<unsigned>(day));
  const long long secs = days * 86400LL + hh * 3600LL + mi * 60LL + ss;
  return static_cast<time_t>(secs);
}

}  // namespace uvcpp
