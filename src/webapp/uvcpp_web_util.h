/**
 * @file src/webapp/uvcpp_web_util.h
 * @brief Web 框架通用工具：URL 编解码、查询串、Cookie、MIME、HTTP 日期、
 *        以及静态文件路径的安全核心。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么单独一个模块
 * ------------------
 * 现有 web 层**完全没有**这些工具（`uvcpp_http_server.cpp` 的注释提到
 * `parse_query`，但那个函数根本不存在）。路由、请求封装、静态服务都要用，
 * 所以先把它独立出来，谁都不依赖。
 *
 * 安全部分是本模块的重点：`web_sanitize_path()` 与 `web_resolve_within_root()`
 * 一起构成「静态文件不越界」的防线。旧实现是
 * `if (url.find("..") != npos) return false;` —— 它两头都不对：
 *   - **误杀**：`/js/lib..min.js` 这种合法文件名会被拒绝；
 *   - **不是安全边界**：它只在 URL 还没解码时做子串匹配，`%2e%2e/`、
 *     `%2e%2e%2f`、`..%5c` 全部原样穿过，交给文件系统时才知道是什么。
 *
 * 本模块的规则是：**先解码，再按段归一化，最后做真实路径的包含判断**。
 * 三个步骤各司其职，缺一不可。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_UTIL_H
#define SRC_WEBAPP_UVCPP_WEB_UTIL_H

#include <cstddef>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

// =========================================================================
// 字符串小工具
// =========================================================================
//
// C++11 没有 std::string_view，所以这几个函数都吃 const std::string&。
// 它们都在每请求路径上被调用，因此不接受 std::string 值参（避免多一次拷贝）。

/** @brief ASCII 小写化（不改动非 ASCII 字节）。 */
UVCPP_API std::string web_to_lower(const std::string& s);

/** @brief ASCII 大写化（不改动非 ASCII 字节）。 */
UVCPP_API std::string web_to_upper(const std::string& s);

/** @brief 去掉首尾的空白（空格、\t、\r、\n）。 */
UVCPP_API std::string web_trim(const std::string& s);

/** @brief `s` 是否以 `prefix` 开头。 */
UVCPP_API bool web_starts_with(const std::string& s, const std::string& prefix);

/** @brief `s` 是否以 `suffix` 结尾。 */
UVCPP_API bool web_ends_with(const std::string& s, const std::string& suffix);

/**
 * @brief 大小写不敏感的前缀比较。
 *
 * HTTP 的**头字段名**和 **MIME 类型**都按大小写不敏感处理，这两处都要用它。
 */
UVCPP_API bool web_starts_with_ci(const std::string& s,
                                  const std::string& prefix);

/** @brief 大小写不敏感的相等比较。 */
UVCPP_API bool web_equals_ci(const std::string& a, const std::string& b);

/**
 * @brief 按分隔符切分。
 * @param keep_empty 是否保留空段。
 *        切 URL 路径时通常要 `true`（`/a//b` 的三个段里中间那个是空的，
 *        路由匹配需要在场）；切查询串时用 `false` 更省事。
 */
UVCPP_API void web_split(const std::string& s, char sep,
                         std::vector<std::string>& out,
                         bool keep_empty = true);

/** @brief 切分并返回新 vector 的便利版本。 */
UVCPP_API std::vector<std::string> web_split(const std::string& s, char sep,
                                             bool keep_empty = true);

// =========================================================================
// 百分号编解码
// =========================================================================

/**
 * @brief 百分号解码。
 *
 * @param s             输入
 * @param plus_as_space **只对查询串置 true**。
 *        `application/x-www-form-urlencoded` 里 `+` 表示空格，但在 **URL 路径**
 *        里 `+` 就是字面意义的加号（`/a+b` 是文件 `a+b`，不是 `a b`）。
 *        搞混这个是最常见的 URL 解析 bug，所以这里必须显式传参、不给默认值。
 * @param ok            可选输出：解码是否合法（`%` 后必须跟两位十六进制）。
 *                      传 nullptr 表示不关心，此时非法序列按字面保留。
 *
 * @note 解码结果里**可能出现 NUL 字节**。调用方若要把结果当 C 字符串用，
 *       必须自己检查 —— `web_sanitize_path()` 会检查。
 */
UVCPP_API std::string web_url_decode(const std::string& s, bool plus_as_space,
                                     bool* ok = nullptr);

/**
 * @brief 百分号编码。
 *
 * 只保留 RFC 3986 的 unreserved 集合（`A-Za-z0-9-._~`），其余全部编码，
 * 这是最保守也最安全的做法。额外的 `keep` 参数用于放行某几个字符
 * （例如构造路径时想保留 `/`）。
 *
 * @param plus_for_space 空格编码成 `+` 还是 `%20`。查询串常用前者，
 *        路径/表单值用后者。
 */
UVCPP_API std::string web_url_encode(const std::string& s,
                                     const std::string& keep = std::string(),
                                     bool plus_for_space = false);

/** @brief 单字节的十六进制字符值；不是十六进制返回 -1。 */
UVCPP_API int web_hex_value(char c);

// =========================================================================
// URL 拆解 / 查询串 / Cookie
// =========================================================================

/**
 * @brief 把原始请求目标拆成「路径」与「查询串」。
 *
 * 只做拆分，**不做解码** —— 解码时机由调用方决定（路径要用
 * `plus_as_space=false`，查询串用 `true`，两者不能共用一次解码）。
 * 查询串不含前导 `?`；没有查询串时 `query` 为空。
 *
 * 同时剥离 `#fragment`：虽然 HTTP 请求目标里不该有它，但客户端库时常带。
 */
UVCPP_API void web_split_path_query(const std::string& raw_url,
                                    std::string& path, std::string& query);

/**
 * @brief 解析查询串为有序键值对。
 *
 * 保序、允许重复键、允许空值。键与值都做百分号解码（`+` → 空格）。
 * 形如 `a=1&b&c=&=d` 的输入会被解析成：
 * `("a","1") ("b","") ("c","") ("","d")` —— 不丢信息是刻意的，
 * 由调用方决定要不要拒绝。
 */
UVCPP_API std::vector<std::pair<std::string, std::string> > web_parse_query(
    const std::string& query);

/** @brief 在键值对数组里查第一个匹配的键。找不到返回 nullptr。 */
UVCPP_API const std::string* web_find_param(
    const std::vector<std::pair<std::string, std::string> >& params,
    const std::string& name);

/**
 * @brief 解析 `Cookie:` 头。
 *
 * 格式是 `k1=v1; k2=v2`（分号分隔，等号右侧可含 `=`）。这是**浏览器发来的**
 * 格式，与 `Set-Cookie`（响应头）不同，别混用解析器。
 */
UVCPP_API std::vector<std::pair<std::string, std::string> > web_parse_cookies(
    const std::string& cookie_header);

/** @brief `web_parse_cookies` 的查值版本；找不到返回 nullptr。 */
UVCPP_API const std::string* web_find_cookie(
    const std::vector<std::pair<std::string, std::string> >& cookies,
    const std::string& name);

/**
 * @brief 构造一条 `Set-Cookie` 的值（不含头名）。
 *
 * @param name       名字
 * @param value      值
 * @param path       作用路径，空则不输出 `Path`
 * @param max_age    秒；< 0 不输出 `Max-Age`
 * @param http_only  是否加 `HttpOnly`
 * @param secure     是否加 `Secure`
 * @param same_site  `"Lax"` / `"Strict"` / `"None"` / 空（不输出）。
 *                   `SameSite=None` 会被自动补上 `Secure` —— 现代浏览器
 *                   要求两者同时出现，否则整条 cookie 被丢弃。
 *
 * 名字与值都会被校验并编码：值里的分号、逗号、空白会被百分号编码，
 * 避免使用者拼出能注入额外属性的 cookie。
 */
UVCPP_API std::string web_build_cookie(
    const std::string& name, const std::string& value,
    const std::string& path = std::string(), long max_age = -1,
    bool http_only = true, bool secure = false,
    const std::string& same_site = std::string("Lax"));

// =========================================================================
// 静态文件路径安全核心
// =========================================================================

/**
 * @brief `web_sanitize_path()` 的失败原因。
 *
 * 每一种都对应一类真实攻击，测试里有逐条的拒绝用例。
 */
enum class web_path_status : int {
  OK = 0,
  EMPTY,           ///< 输入为空（或只有空白）
  NOT_ABSOLUTE,    ///< 解码后不以 `/` 开头
  BAD_PERCENT,     ///< `%` 后面不是两位十六进制
  ENCODED_NUL,     ///< 解码后含 NUL —— C 字符串 API 会在这里被截断
  CONTROL_CHAR,    ///< 含 `\x01`-`\x1f`、`\x7f`
  BACKSLASH,       ///< 含 `\`：Windows 上它同样是分隔符，`..\..\` 由它绕过
  COLON,           ///< 含 `:`：盘符（`C:`）与 NTFS 数据流（`file:stream`）
  UNC_PREFIX,      ///< 以 `//` 开头：UNC（`//server/share`）与语义歧义
  TRAVERSAL,       ///< `..` 越过了根，或真实路径落在了根之外
  TOO_LONG,        ///< 超过 max_length
  TOO_DEEP,        ///< 段数超过 max_depth
  NOT_FOUND,       ///< 真实路径解析不出来（父目录都不存在）—— 调用方当 404
};

/** @brief 失败原因的可读名称（"TRAVERSAL" 等），可直接写日志。 */
UVCPP_API const char* web_path_status_name(web_path_status status);

/** @brief `web_sanitize_path()` 的可调项。 */
struct UVCPP_API web_path_options {
  size_t max_length;  ///< 解码后的最大长度（字节），默认 4096
  size_t max_depth;   ///< 最大段数，默认 64
  bool   decode;      ///< 是否做百分号解码，默认 true

  /// **显式构造函数**。成员初始化器（NSDMI）会让本结构体失去聚合初始化
  /// 资格，与 `uvcpp_log_record` 同理 —— C++11 下 `web_path_options{1024}`
  /// 会编译失败。给默认实参的构造函数才能两种写法都支持。
  explicit web_path_options(size_t max_len = 4096, size_t max_dep = 64,
                            bool dec = true);
};

/**
 * @brief **静态文件防越界的核心**：把原始 URL 路径归一化成安全的绝对路径。
 *
 * 处理流程（顺序不可调换）：
 *   1. 百分号解码（`plus_as_space = false`）—— 必须在任何检查**之前**，
 *      否则 `%2e%2e%2f` 会被当成普通文件名放过去；
 *   2. 逐字节检查 NUL / 控制字符 / `\` / `:`；
 *   3. 必须以 `/` 开头，且不能以 `//` 开头；
 *   4. 按 `/` 分段，丢弃空段与 `.` 段；遇到 `..` 就弹出上一段，
 *      **弹出时栈已空 = 越界，直接失败**（不做静默截断 —— 截断会把攻击
 *      变成「看起来正常」的请求，日志里再也看不出来）；
 *   5. 重新拼成 `/seg1/seg2` 形式。
 *
 * 结果保证：以 `/` 开头、无 `..`、无空段、无 `.` 段、无 `\`、无 `:`。
 *
 * @param raw 原始路径（可以是 `/a/../b`、`/%2e%2e/x` 等任意输入）
 * @param out 成功时写入归一化结果；**失败时不改动**（便于调用方复用变量）
 * @return 见 `web_path_status`
 *
 * @note 这一步是**纯文本**的，不碰文件系统。它挡住了所有的编码绕过，
 *       但挡不住**符号链接**：`root/link -> C:\Windows` 时
 *       `/link/system32/x` 在这里是合法的。那一层由
 *       `web_resolve_within_root()` 用真实路径解决。
 */
UVCPP_API web_path_status web_sanitize_path(const std::string& raw,
                                            std::string& out);

/** @brief 带选项的重载。 */
UVCPP_API web_path_status web_sanitize_path(const std::string& raw,
                                            std::string& out,
                                            const web_path_options& opts);

/** @brief URL 根路径与索引文件拼成最终 URL 路径（处理挂载前缀与结尾斜杠）。 */
UVCPP_API std::string web_join_url(const std::string& url_root,
                                   const std::string& url_path,
                                   const std::string& index_file);

/**
 * @brief 把归一化后的 URL 路径拼到文档根上。
 *
 * @param root    文档根（调用方应先 `web_real_path()` 归一化一次并缓存）
 * @param url_path 必须是 `web_sanitize_path()` 的输出
 *
 * 平台差异：Windows 用 `\` 连接，POSIX 用 `/`。这里只负责拼接，
 * **不做安全判断** —— 判断在 `web_is_within_root()`。
 */
UVCPP_API std::string web_join_root(const std::string& root,
                                    const std::string& url_path);

/**
 * @brief `candidate` 是否在 `root` 之内（按真实路径比较）。
 *
 * 两个参数都必须是已归一化的**绝对真实路径**。判据是「前缀 + 分隔符」，
 * 所以 `/srv/webroot2` 不会被误判为在 `/srv/webroot` 之内 —— 直接比较前缀
 * 字符串是这个检查最常见的写法错误。
 *
 * Windows 下按大小写不敏感比较；POSIX 下敏感。
 */
UVCPP_API bool web_is_within_root(const std::string& root_real,
                                  const std::string& candidate_real);

/**
 * @brief 解析真实路径（`..`/`.`/符号链接都展开）。失败返回 false。
 *
 * 文件不存在时 `realpath` 会失败，这是正常的 —— 所以本函数有
 * `allow_missing` 参数：为 true 时会退回到「解析父目录 + 接上文件名」，
 * 只要**父目录**存在就能得到一个可信的绝对路径。这正是静态服务需要的语义
 * （请求一个不存在的文件要返回 404，而不是 500）。
 *
 * @param allow_missing 允许最后一段不存在
 */
UVCPP_API bool web_real_path(const std::string& path, std::string& out,
                             bool allow_missing = false);

/**
 * @brief 一次做完「拼接 + 符号链接展开 + 包含判断」。
 *
 * 这是静态服务该调用的函数。`root_real` 应当是启动时用
 * `web_real_path(root, root_real)` 算好并缓存的（每次请求都算一遍是浪费）。
 *
 * @param root_real 已归一化的文档根
 * @param url_path  `web_sanitize_path()` 的输出
 * @param out       成功时写入最终可打开的真实路径
 * @return 见 `web_path_status`。越界返回 `TRAVERSAL`；
 *         父目录不存在时返回 `NOT_FOUND`（调用方当 404 处理）。
 */
UVCPP_API web_path_status web_resolve_within_root(const std::string& root_real,
                                                  const std::string& url_path,
                                                  std::string& out);

// =========================================================================
// MIME / 状态码 / HTTP 日期
// =========================================================================

/**
 * @brief 按文件扩展名猜 MIME 类型。未知返回 `application/octet-stream`。
 *
 * 内置表只覆盖常见类型；要增删请用 `uvcpp_web_mime_map`（`uvcpp_web_mime.h`），
 * 它以本表为初值再叠加使用者的覆盖。返回值是**静态字符串**，不需要释放。
 */
UVCPP_API const char* web_mime_type(const std::string& path);

/** @brief 内置 MIME 表的一条。扩展名小写、不含点。 */
struct UVCPP_API web_mime_builtin_entry {
  const char* ext;
  const char* type;
};

/**
 * @brief 拿到内置 MIME 表本体，用于初始化 `uvcpp_web_mime_map`。
 *
 * 存在的意义是**只有一份表**：`uvcpp_web_mime_map` 构造时照抄这里的条目，
 * 而不是自己再维护一份。两张表迟早会漂移，而漂移的表现是"同一个文件在
 * `web_mime_type()` 和静态服务里类型不一样" —— 极难排查。
 *
 * @param count 输出条目数。
 * @return 指向静态数组的指针，进程生存期内有效，**不要释放、不要改**。
 */
UVCPP_API const web_mime_builtin_entry* web_mime_builtin_table(size_t& count);

/** @brief 从 MIME 类型判断是否应当按文本处理（可压缩、要带 charset）。 */
UVCPP_API bool web_mime_is_text(const std::string& mime);

/** @brief 状态码的原因短语（"Not Found"）。未知返回 "Unknown"。 */
UVCPP_API const char* web_status_text(int code);

/**
 * @brief 格式化成 HTTP 日期（RFC 7231 IMF-fixdate）。
 *
 * 形如 `Sun, 06 Nov 1994 08:49:37 GMT`。`Date`、`Last-Modified`、
 * `Expires` 都用这个格式，且**必须是 GMT**（早期实现用本地时间是个经典坑）。
 */
UVCPP_API std::string web_http_date(time_t t);

/** @brief 当前时间的 HTTP 日期。 */
UVCPP_API std::string web_http_date_now();

/**
 * @brief 解析 HTTP 日期，失败返回 `(time_t)-1`。
 *
 * RFC 7231 要求服务端**同时接受**三种格式（用于 `If-Modified-Since`）：
 * IMF-fixdate、RFC 850、asctime。只认第一种会让老客户端的条件请求失效。
 * 全部按 GMT 解释；`GMT`/`UTC`/`Z` 之外的时区名一律拒绝。
 */
UVCPP_API time_t web_parse_http_date(const std::string& s);

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_UTIL_H
