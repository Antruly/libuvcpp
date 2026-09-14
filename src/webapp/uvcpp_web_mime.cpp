#include "uvcpp_web_mime.h"

#include <cstring>

#include <webapp/uvcpp_web_util.h>

namespace uvcpp {

namespace {

/// 查不到时的兜底。与 `web_mime_type()` 用的是同一个字符串字面量含义，
/// 但两者各自持有常量 —— 这里返回的必须**活得比任何调用方都久**。
const char* const k_fallback = "application/octet-stream";

}  // namespace

uvcpp_web_mime_map::uvcpp_web_mime_map(bool with_builtin) {
  if (with_builtin) reset_to_builtin();
}

void uvcpp_web_mime_map::reset_to_builtin() {
  table_.clear();

  size_t n = 0;
  const web_mime_builtin_entry* table = web_mime_builtin_table(n);
  for (size_t i = 0; i < n; ++i) {
    if (table[i].ext == nullptr || table[i].type == nullptr) continue;
    // 内置表的扩展名已经是小写不含点的规范形式，直接落表；不再走
    // normalize_ext() 做一次多余的分配。
    table_[table[i].ext] = table[i].type;
  }
}

void uvcpp_web_mime_map::clear() { table_.clear(); }

bool uvcpp_web_mime_map::normalize_ext(const std::string& ext,
                                       std::string& out) {
  // 去前导点。**可能有多个**：`..png` 这种输入去掉一个点还剩 `.png`，
  // 那仍然不是合法扩展名 —— 所以这里循环剥，剥完再判空。
  size_t begin = 0;
  while (begin < ext.size() && ext[begin] == '.') ++begin;
  if (begin >= ext.size()) return false;

  out = web_to_lower(web_trim(ext.substr(begin)));

  // 剥完点之后仍可能夹带分隔符（`a/b`）或空白（`p n g`）：这类输入当配置
  // 笔误处理，静默忽略比收下一张永远匹配不到的条目好。
  for (size_t i = 0; i < out.size(); ++i) {
    const char c = out[i];
    if (c == '/' || c == '\\' || c == ' ' || c == '\t' || c == '\r' ||
        c == '\n' || c == '?') {
      return false;
    }
  }
  return !out.empty();
}

bool uvcpp_web_mime_map::set(const std::string& ext, const std::string& mime) {
  std::string key;
  if (!normalize_ext(ext, key)) return false;

  const std::string value = web_trim(mime);
  if (value.empty()) return remove(key);

  table_[key] = value;
  return true;
}

size_t uvcpp_web_mime_map::set_from_string(const std::string& spec) {
  size_t applied = 0;
  size_t pos = 0;

  while (pos <= spec.size()) {
    size_t end = spec.find_first_of(";,", pos);
    if (end == std::string::npos) end = spec.size();

    const std::string item = web_trim(spec.substr(pos, end - pos));
    if (!item.empty()) {
      const size_t eq = item.find('=');
      if (eq != std::string::npos &&
          set(item.substr(0, eq), item.substr(eq + 1))) {
        ++applied;
      }
      // 没有 `=` 的项直接跳过：`"png"` 这种半截配置没法猜出使用者想要什么
      // 类型，猜错比不生效更难查。
    }

    if (end == spec.size()) break;
    pos = end + 1;
  }

  return applied;
}

bool uvcpp_web_mime_map::remove(const std::string& ext) {
  std::string key;
  if (!normalize_ext(ext, key)) return false;
  return table_.erase(key) > 0;
}

const char* uvcpp_web_mime_map::lookup_ext(const std::string& ext) const {
  std::string key;
  if (!normalize_ext(ext, key)) return k_fallback;

  const std::map<std::string, std::string>::const_iterator it =
      table_.find(key);
  return it == table_.end() ? k_fallback : it->second.c_str();
}

const char* uvcpp_web_mime_map::lookup(const std::string& path) const {
  // 取**最后一个**点：`archive.tar.gz` 看 `gz`，`lib..min.js` 看 `js`
  // （中间那个空段不影响），`a.b/c` 看 `c`。
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= path.size()) return k_fallback;

  std::string ext = path.substr(dot + 1);
  // 路径里可能还挂着 `?`/`#`/分隔符（上游正常会剥掉，这里不信任它）。
  const size_t cut = ext.find_first_of("?#/\\");
  if (cut != std::string::npos) ext.erase(cut);

  return lookup_ext(ext);
}

bool uvcpp_web_mime_map::is_text(const std::string& mime) {
  // 直接转调 util 的实现，**不在这里再写一份**。
  //
  // 一开始这里是自己判的（省掉一次裁 `;` 和一次 std::string 构造），但"文本
  // 类型"的判据同时决定要不要补 charset、要不要允许压缩，两处实现一旦漂移，
  // 表现是"某些类型莫名没补 charset" —— 正是上面 MIME 表要避免的同一类问题。
  // 这条路径每次静态响应只走一次，省下的那点开销不值得多一份真相。
  return web_mime_is_text(mime);
}

size_t uvcpp_web_mime_map::builtin_size() {
  size_t n = 0;
  web_mime_builtin_table(n);
  return n;
}

uvcpp_web_mime_map& uvcpp_web_mime_map::default_map() {
  // 函数级静态：C++11 保证初始化线程安全，且只初始化一次。
  static uvcpp_web_mime_map instance(true);
  return instance;
}

}  // namespace uvcpp
