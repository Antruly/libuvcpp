/**
 * @file tests/functional/web_app_util_func.cpp
 * @brief webapp 工具模块（URL / 查询串 / Cookie / 路径安全 / MIME / 日期）的
 *        功能测试。
 *
 * 重点是 `web_sanitize_path()` 的**拒绝表**：每一种编码绕过手法都有一条
 * 对应用例。这份表就是这个模块的可执行规格 —— 改实现时它必须仍然全绿。
 *
 * 另外用**真实的目录 + junction/符号链接**验证
 * `web_resolve_within_root()`：文本层面完全合法的 `/escape/secret.txt`，
 * 当 `escape` 是指向根目录之外的链接时，必须被判为 TRAVERSAL。
 * 这一条是「纯文本检查」与「真实安全边界」的分水岭。
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <webapp/uvcpp_web_util.h>

#ifdef _WIN32
#  include <direct.h>   // _mkdir / _rmdir
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq(const std::string& got, const std::string& want, const char* what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: [" << want
              << "]\n         实际: [" << got << "]" << std::endl;
    ++g_failures;
  }
}

// =========================================================================
// 1. 字符串小工具
// =========================================================================
void test_string_helpers() {
  std::cout << "[util] string_helpers" << std::endl;

  check_eq(web_to_lower("AbC1-_"), "abc1-_", "web_to_lower");
  check_eq(web_to_upper("AbC1-_"), "ABC1-_", "web_to_upper");
  check_eq(web_trim("  \t x y \r\n "), "x y", "web_trim");
  check_eq(web_trim(""), "", "web_trim 空串");
  check_eq(web_trim("   "), "", "web_trim 全空白");

  check(web_starts_with("abcdef", "abc"), "starts_with 命中");
  check(!web_starts_with("ab", "abc"), "starts_with 前缀比串长");
  check(web_ends_with("abcdef", "def"), "ends_with 命中");
  check(!web_ends_with("abc", "abcdef"), "ends_with 后缀比串长");

  // 头字段名与 MIME 都是大小写不敏感的
  check(web_starts_with_ci("Content-Type", "content-"), "starts_with_ci");
  check(web_equals_ci("TEXT/HTML", "text/html"), "equals_ci");
  check(!web_equals_ci("text/html", "text/plain"), "equals_ci 不等");
  check(!web_equals_ci("abc", "abcd"), "equals_ci 长度不同");

  std::vector<std::string> parts;
  web_split("/a//b/", '/', parts, true);
  check(parts.size() == 5, "web_split keep_empty=true 应保留空段");
  web_split("/a//b/", '/', parts, false);
  check(parts.size() == 2, "web_split keep_empty=false 应丢掉空段");
  check_eq(parts[0], "a", "web_split 段 0");
  check_eq(parts[1], "b", "web_split 段 1");
}

// =========================================================================
// 2. 百分号编解码
// =========================================================================
void test_url_encode_decode() {
  std::cout << "[util] url_encode_decode" << std::endl;

  bool ok = true;
  check_eq(web_url_decode("%41%42", false, &ok), "AB", "解码基本");
  check(ok, "解码合法性应为 true");
  check_eq(web_url_decode("a%2Fb", false, &ok), "a/b", "解码 %2F");
  check(ok, "%2F 是合法序列");

  // '+' 的语义按场合不同 —— 这是 URL 解析最常见的坑
  check_eq(web_url_decode("a+b", true, &ok), "a b", "查询串里 + 是空格");
  check(ok, "查询串 + 合法");
  check_eq(web_url_decode("a+b", false, &ok), "a+b", "路径里 + 是字面加号");
  check(ok, "路径 + 合法");

  // 非法序列：按字面保留 + 报告
  ok = true;
  check_eq(web_url_decode("a%zzb", false, &ok), "a%zzb", "非法序列按字面保留");
  check(!ok, "非法序列应报告 ok=false");
  ok = true;
  check_eq(web_url_decode("a%2", false, &ok), "a%2", "%2 截断按字面保留");
  check(!ok, "%2 截断应报告");
  ok = true;
  check_eq(web_url_decode("a%", false, &ok), "a%", "尾部裸 % 按字面保留");
  check(!ok, "尾部裸 % 应报告");

  // %00 必须能解出来 —— 上游（sanitize_path）负责拒绝它
  ok = true;
  const std::string nul = web_url_decode("a%00b", false, &ok);
  check(ok, "%00 是合法编码");
  check(nul.size() == 3 && nul[1] == '\0', "解码结果里应当有 NUL 字节");

  // 中文 UTF-8 往返
  const std::string cn = "\xE4\xB8\xAD\xE6\x96\x87";  // "中文"
  check_eq(web_url_encode(cn), "%E4%B8%AD%E6%96%87", "中文应被百分号编码");
  check_eq(web_url_decode("%E4%B8%AD%E6%96%87", false, &ok), cn, "中文解码");

  check_eq(web_url_encode("a b"), "a%20b", "空格编码成 %20");
  check_eq(web_url_encode("a b", "", true), "a+b", "空格编码成 +");
  check_eq(web_url_encode("/a/b", "/"), "/a/b", "keep 放行 /");
  check_eq(web_url_encode("/a/b"), "%2Fa%2Fb", "不 keep 时 / 也要编码");
  check_eq(web_url_encode("-._~"), "-._~", "unreserved 集合不编码");

  check(web_hex_value('f') == 15, "hex 'f'");
  check(web_hex_value('F') == 15, "hex 'F'");
  check(web_hex_value('0') == 0, "hex '0'");
  check(web_hex_value('g') == -1, "hex 'g' 非法");
}

// =========================================================================
// 3. URL 拆解 / 查询串
// =========================================================================
void test_split_and_query() {
  std::cout << "[util] split_and_query" << std::endl;

  std::string p, q;
  web_split_path_query("/a/b?x=1", p, q);
  check_eq(p, "/a/b", "拆路径");
  check_eq(q, "x=1", "拆查询串");

  web_split_path_query("/a/b", p, q);
  check_eq(p, "/a/b", "无查询串时路径不变");
  check_eq(q, "", "无查询串时为空");

  // 代理风格（absolute-form）：scheme 与 authority 必须先剥掉，
  // 否则 "http://" 会变成路径的一部分。
  web_split_path_query("http://example.com/a/b?x=1", p, q);
  check_eq(p, "/a/b", "absolute-form 剥掉 scheme://authority");
  check_eq(q, "x=1", "absolute-form 查询串");

  web_split_path_query("https://example.com", p, q);
  check_eq(p, "/", "absolute-form 无路径时补 /");

  web_split_path_query("/a#frag", p, q);
  check_eq(p, "/a", "剥掉 #fragment");

  web_split_path_query("/a?x=1#f", p, q);
  check_eq(q, "x=1", "fragment 在查询串之后也应剥掉");

  typedef std::vector<std::pair<std::string, std::string> > params_t;

  const params_t a = web_parse_query("a=1&b=2");
  check(a.size() == 2, "解析两个键值对");
  check(web_find_param(a, "a") != NULL && *web_find_param(a, "a") == "1", "取 a");
  check(web_find_param(a, "b") != NULL && *web_find_param(a, "b") == "2", "取 b");
  check(web_find_param(a, "zz") == NULL, "不存在的键返回 NULL");

  // 重复键：保序，取第一个
  const params_t dup = web_parse_query("a=1&a=2");
  check(dup.size() == 2, "重复键都应保留");
  check(*web_find_param(dup, "a") == "1", "重复键取第一个");

  // 裸键（无 '='）
  const params_t bare = web_parse_query("flag&b=2");
  check(bare.size() == 2, "裸键也应解析出来");
  check(*web_find_param(bare, "flag") == "", "裸键的值为空串");

  // 值里含 '='
  const params_t eqv = web_parse_query("t=a=b");
  check(*web_find_param(eqv, "t") == "a=b", "值里可以再有 =");

  // '+' 与百分号在查询串里都要解
  const params_t enc = web_parse_query("q=a+b&u=%E4%B8%AD");
  check(*web_find_param(enc, "q") == "a b", "查询串 + 解成空格");
  check(*web_find_param(enc, "u") == "\xE4\xB8\xAD", "查询串百分号解码");

  check(web_parse_query("").empty(), "空查询串");
  check(web_parse_query("a=1&&b=2").size() == 2, "跳过空段");
}

// =========================================================================
// 4. Cookie
// =========================================================================
void test_cookies() {
  std::cout << "[util] cookies" << std::endl;

  typedef std::vector<std::pair<std::string, std::string> > cookies_t;

  const cookies_t c = web_parse_cookies("a=1; b=2; c=x=y");
  check(c.size() == 3, "解析三条 cookie");
  check(*web_find_cookie(c, "a") == "1", "cookie a");
  check(*web_find_cookie(c, "b") == "2", "cookie b");
  check(*web_find_cookie(c, "c") == "x=y", "cookie 值里可以含 =");

  const cookies_t spaced = web_parse_cookies("  a = 1 ;  b=2  ");
  check(spaced.size() == 2, "带空白的三条应被 trim");
  check(*web_find_cookie(spaced, "a") == "1", "名字与值都应 trim");

  const cookies_t quoted = web_parse_cookies("a=\"v\"");
  check(*web_find_cookie(quoted, "a") == "v", "带引号的值应去掉引号");

  check(web_parse_cookies("").empty(), "空 Cookie 头");
  check(web_parse_cookies("novalue").empty(), "没有 = 的段应丢弃");

  // 构造
  const std::string basic = web_build_cookie("sid", "abc", "/", -1, true, false, "Lax");
  check_eq(basic, "sid=abc; Path=/; SameSite=Lax; HttpOnly", "基本 cookie");
  check(web_build_cookie("sid", "abc", "/", 60).find("Max-Age=60") !=
            std::string::npos,
        "Max-Age");

  // SameSite=None 必须自动带上 Secure，否则浏览器整条丢弃
  const std::string none = web_build_cookie("sid", "abc", "/", -1, true, false, "None");
  check(none.find("SameSite=None") != std::string::npos, "SameSite=None");
  check(none.find("Secure") != std::string::npos,
        "SameSite=None 应自动补上 Secure");

  // 值里的分隔符必须被编码，不能拼出额外属性
  const std::string evil = web_build_cookie("sid", "a; Path=/hacked", "/x");
  check(evil.find("hacked") == std::string::npos ||
            evil.find("a%3B") != std::string::npos,
        "值里的分号应被百分号编码");

  // 名字非法（会注入额外属性）→ 直接放弃这条 cookie
  check(web_build_cookie("a;b", "v", "/").empty(), "非法名字应返回空串");
  check(web_build_cookie("", "v", "/").empty(), "空名字应返回空串");

  // 名字是合法 token 时正常
  check(!web_build_cookie("a-b_c.d", "v", "/").empty(), "token 字符的名字应可用");
}

// =========================================================================
// 5. 路径安全 —— 拒绝表（本模块的核心）
// =========================================================================
void test_sanitize_accept() {
  std::cout << "[util] sanitize_accept" << std::endl;

  struct accept_case {
    const char* in;
    const char* want;
  };
  static const accept_case k_cases[] = {
      {"/", "/"},
      {"/a/b", "/a/b"},
      {"/a/./b", "/a/b"},           // `.` 段丢弃
      {"/a/b/", "/a/b"},            // 结尾斜杠丢弃
      {"/a//b", "/a/b"},            // 中间空段丢弃
      {"/a/../b", "/b"},            // `..` 在根之内：合法
      {"/a/b/../c", "/a/c"},
      {"/a/%20b", "/a/ b"},         // 百分号解码
      {"/a%2Fb", "/a/b"},           // 编码的分隔符解出来后仍是分隔符
      {"/lib..min.js", "/lib..min.js"},  // ← 旧实现用 find("..") 会误杀它
      {"/a/%2e%2e/b", "/b"},        // 编码的 .. 但没越界：同样合法
      {"/\xE4\xB8\xAD\xE6\x96\x87", "/\xE4\xB8\xAD\xE6\x96\x87"},  // UTF-8 原样通过
  };
  const size_t n = sizeof(k_cases) / sizeof(k_cases[0]);
  for (size_t i = 0; i < n; ++i) {
    std::string out;
    const web_path_status st = web_sanitize_path(k_cases[i].in, out);
    if (st != web_path_status::OK) {
      std::cerr << "  [FAIL] 应接受 [" << k_cases[i].in
                << "] 但被拒: " << web_path_status_name(st) << std::endl;
      ++g_failures;
      continue;
    }
    check_eq(out, k_cases[i].want, k_cases[i].in);
  }
}

void test_sanitize_reject() {
  std::cout << "[util] sanitize_reject" << std::endl;

  struct reject_case {
    const char* in;
    web_path_status want;
  };
  static const reject_case k_cases[] = {
      {"", web_path_status::EMPTY},
      {"abc", web_path_status::NOT_ABSOLUTE},
      {"a/b", web_path_status::NOT_ABSOLUTE},

      // 非法百分号序列
      {"/a/%zz", web_path_status::BAD_PERCENT},
      {"/a/%2", web_path_status::BAD_PERCENT},
      {"/a/%", web_path_status::BAD_PERCENT},

      // 解码后才是危险字符 —— 这些正是旧实现漏掉的
      {"/a%00b", web_path_status::ENCODED_NUL},
      {"/a\x01" "b", web_path_status::CONTROL_CHAR},
      {"/a%7Fb", web_path_status::CONTROL_CHAR},
      {"/a\\b", web_path_status::BACKSLASH},
      {"/a%5Cb", web_path_status::BACKSLASH},        // 编码的反斜杠
      {"/a%5cb/../..", web_path_status::BACKSLASH},
      {"/C:/windows/win.ini", web_path_status::COLON},
      {"/a%3Ab", web_path_status::COLON},            // 编码的冒号

      // 前导双斜杠：UNC / 语义歧义
      {"//etc/passwd", web_path_status::UNC_PREFIX},
      {"//server/share/x", web_path_status::UNC_PREFIX},

      // ---- 真正的目录穿越 ----
      {"/../etc/passwd", web_path_status::TRAVERSAL},
      {"/..", web_path_status::TRAVERSAL},
      {"/a/../../b", web_path_status::TRAVERSAL},
      {"/a/b/../../../c", web_path_status::TRAVERSAL},
      {"/..%2fetc/passwd", web_path_status::TRAVERSAL},   // 编码的斜杠
      {"/%2e%2e/etc/passwd", web_path_status::TRAVERSAL}, // 编码的点
      {"/%2e%2e%2fetc/passwd", web_path_status::TRAVERSAL},
      {"/%2E%2E%2Fetc/passwd", web_path_status::TRAVERSAL},  // 大写十六进制
      {"/a/%2e%2e/%2e%2e/b", web_path_status::TRAVERSAL},
      {"/\\..\\..\\windows", web_path_status::BACKSLASH},
  };
  const size_t n = sizeof(k_cases) / sizeof(k_cases[0]);
  for (size_t i = 0; i < n; ++i) {
    const std::string in = k_cases[i].in;
    // 失败时不得改动输出（便于调用方复用变量）
    std::string out = "SENTINEL";
    const web_path_status st = web_sanitize_path(in, out);
    if (st != k_cases[i].want) {
      std::cerr << "  [FAIL] [" << in << "] 期望 "
                << web_path_status_name(k_cases[i].want) << "，实际 "
                << web_path_status_name(st) << std::endl;
      ++g_failures;
      continue;
    }
    if (out != "SENTINEL") {
      std::cerr << "  [FAIL] [" << in << "] 失败时不应改动 out" << std::endl;
      ++g_failures;
    }
  }

  // 上限
  std::string long_path = "/";
  long_path.append(5000, 'a');
  std::string out;
  check(web_sanitize_path(long_path, out) == web_path_status::TOO_LONG,
        "超长路径应被拒");

  std::string deep = "/";
  for (int i = 0; i < 100; ++i) deep += "a/";
  check(web_sanitize_path(deep, out) == web_path_status::TOO_DEEP,
        "过深路径应被拒");

  check(web_sanitize_path(deep, out, web_path_options(4096, 4)) ==
            web_path_status::TOO_DEEP,
        "自定义 max_depth 应生效");
  check(web_sanitize_path("/aaaaaaaaaaaaaaaaaaaa", out,
                          web_path_options(16, 64)) == web_path_status::TOO_LONG,
        "自定义 max_length 应生效");

  check(std::strcmp(web_path_status_name(web_path_status::TRAVERSAL),
                    "TRAVERSAL") == 0,
        "状态名映射");
}

// =========================================================================
// 5.5 `web_sanitize_filename()` —— 客户端文件名的清洗（步骤 7）
//
// 与 `web_sanitize_path()` 的分工见头文件：那个**拒绝**越界（URL 越界就是
// 攻击），这个**取叶子**（`C:\a\b.txt` 是常态，不该 400）。所以下面这些输入
// **一条都不该失败** —— 它们全部产出某个可安全展示的元数据。
//
// 这张表是那个函数的可执行规格：头文件里列的 6 条规则各有一组输入，改实现时
// 它必须仍然全绿。**规则之间的次序本身也是被测的**（第 5 步排在第 6 步之后，
// 所以截断**造出来**的设备名也要被保护，见表末 `max_len` 那一节）。
//
// 有一处干扰要在读表之前说清楚：`uvcpp_web_multipart` 对引号内的值做了
// RFC 2616 的 quoted-pair 反转义（`\X` → `X`），所以经由 multipart 走到这里的
// `C:\a\b.txt` 其实已经是 `C:ab.txt`。**本文件直接喂字符串**，测的是函数本身
// 的契约；端到端那一份在 `web_app_upload_func.cpp`，它按解析器真实交出来的
// 值写期望（两边的期望值不同是**对的**，不是谁写错了）。
// =========================================================================

/// 严格校验 UTF-8：每个码点的长度与续字节都对，且**没有截断的尾巴**。
///
/// 用它钉"截断不切出半个码点" —— 只比长度的话，切在码点中间同样得到 255，
/// 那样断言就退化成"长度是 255"，与要钉的性质无关。
bool utf8_complete(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 0;
    if (c < 0x80) {
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
    } else {
      return false;  // 非法首字节（含孤立续字节）
    }
    if (i + len > s.size()) return false;  // 截断的尾巴
    for (size_t k = 1; k < len; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += len;
  }
  return true;
}

void test_sanitize_filename() {
  std::cout << "[util] sanitize_filename" << std::endl;

  struct fn_case {
    const char* in;
    const char* want;
  };
  static const fn_case k_cases[] = {
      // --- 规则 1：`/` 与 `\` **都**当分隔符，取最后一段 ---------------
      {"../../etc/passwd", "passwd"},
      {"..\\..\\x.txt", "x.txt"},        // 反斜杠同样是分隔符
      {"C:\\Users\\a\\b.txt", "b.txt"},  // 盘符不是分隔符，是路径段的一部分
      {"/abs/x", "x"},                   // 前导斜杠
      {"a/b", "b"},
      {"//server/share/n.txt", "n.txt"},  // UNC 前缀不特殊，取叶子即可
      {"..\\..\\..\\", "file"},          // 结尾分隔符之后没有叶子
      {"/", "file"},                     // 只有一个分隔符

      // --- 规则 2：C0 控制字符与 DEL --------------------------------
      {"a\x01" "b\x7f.txt", "ab.txt"},
      {"a\tb", "ab"},  // 制表符也 < 0x20
      {"\x01\x02\x03", "file"},  // 整个名字都是控制字符 ⇒ 退化

      // --- 规则 3：结尾的 `.` 与空格（Win32 会静默截掉） --------------
      {"a.txt.", "a.txt"},
      {"a.txt   ", "a.txt"},
      {"a.txt . .", "a.txt"},  // 点与空格交替，一样削干净
      {"   ", "file"},
      {".hidden", ".hidden"},  // 前导点**不**动：dotfile 是合法的展示名

      // --- 规则 4：空 / `.` / `..` 退化成 "file" ----------------------
      {"", "file"},
      {".", "file"},
      {"..", "file"},
      {"...", "file"},  // 先被规则 3 削空，再退化

      // --- 规则 5：保留设备名（大小写无关、带扩展名也算、含上标变体）----
      {"CON", "CON_"},
      {"con", "con_"},
      {"PRN", "PRN_"},
      {"AUX", "AUX_"},
      {"nul.txt", "nul.txt_"},   // 带扩展名**仍是**设备
      {"aux.TXT", "aux.TXT_"},
      {"COM1", "COM1_"},
      {"com9.txt", "com9.txt_"},
      {"LPT1", "LPT1_"},
      {"LPT\xC2\xB9", "LPT\xC2\xB9_"},          // 上标 ¹
      {"COM\xC2\xB2", "COM\xC2\xB2_"},          // 上标 ²
      {"lpt\xC2\xB3.txt", "lpt\xC2\xB3.txt_"},  // 上标 ³ + 小写
      // 负例：没有被误伤的那些（少了它们，"见到 com/lpt 就加下划线"也能过）
      {"COM0", "COM0"},              // 0 不是设备编号
      {"COM10", "COM10"},            // 只 1..9
      {"console.txt", "console.txt"},  // 以 con 开头但不是设备名
      {"NULL", "NULL"},              // 与 nul 无关

      // --- 通过但不变形的普通名字 ------------------------------------
      {"a.bin", "a.bin"},
      {"报告 2026.pdf", "报告 2026.pdf"},
      {"\xE4\xB8\xAD\xE6\x96\x87.txt", "\xE4\xB8\xAD\xE6\x96\x87.txt"},
  };

  const size_t n = sizeof(k_cases) / sizeof(k_cases[0]);
  for (size_t i = 0; i < n; ++i) {
    char tag[256];
    std::snprintf(tag, sizeof(tag), "sanitize_filename[%s]", k_cases[i].in);
    check_eq(web_sanitize_filename(k_cases[i].in), k_cases[i].want, tag);
  }

  // NUL 单独测：它没法出现在 `const char*` 表里（C 字符串到它为止）。
  // 这一条不是凑数 —— NUL 会让任何 C 字符串 API 在这里被截断，于是"校验过的
  // 名字"和"使用的名字"变成两个不同的串。
  {
    std::string with_nul("a\0b.txt", 7);
    check_eq(web_sanitize_filename(with_nul), "ab.txt", "名字里的 NUL 必须被去掉");
  }

  // --- 规则 6：截断，且**保持 UTF-8 码点完整** -----------------------
  {
    std::string long_cn;
    for (int i = 0; i < 200; ++i) long_cn += "\xE4\xB8\xAD";  // 200 × 中 = 600 字节
    const std::string r = web_sanitize_filename(long_cn);
    check_eq(std::to_string(r.size()), std::to_string(255),
             "超长名字必须截断到 255 字节");
    check(utf8_complete(r), "截断不得切出半个码点");
  }

  // 截断点上正好是续字节 ⇒ 必须**退回**到前一个码点边界，而不是切在中间。
  // `中` 是 3 字节，`max_len = 1` 落在第二个字节（续字节）上。
  check_eq(web_sanitize_filename("\xE4\xB8\xAD", 1), "file",
           "截断到 0 字节 ⇒ 退化成 file");

  // `max_len = 0` = **不限**（与全库其它 size_t 上限的约定一致）。
  {
    std::string s(400, 'a');
    check_eq(std::to_string(web_sanitize_filename(s, 0).size()),
             std::to_string(400), "max_len = 0 表示不限");
  }

  // --- 规则次序：**截断排在第 5 步之后** -----------------------------
  // 这一条钉的是次序本身，不是某一条规则。`"nul.txt"` 截到 3 字节正好是
  // `"nul"` —— 先判设备名就会漏掉它，于是盘上会多出一个叫 `nul` 的展示名。
  check_eq(web_sanitize_filename("nul.txt", 3), "nul_",
           "截断**造出来**的设备名也必须被保护（规则 5 排在规则 6 之后）");
  check_eq(web_sanitize_filename("COM1xyz", 4), "COM1_",
           "同上：截断造出的 COM1");
}

// =========================================================================
// 6. 拼接 / 包含判断
// =========================================================================
void test_join_and_containment() {
  std::cout << "[util] join_and_containment" << std::endl;

  check_eq(web_join_root("/srv/www", "/a/b.txt"), "/srv/www/a/b.txt",
           "join_root 普通");
  check_eq(web_join_root("/srv/www/", "/a/b.txt"), "/srv/www/a/b.txt",
           "join_root 根已带斜杠");
  check_eq(web_join_root("/srv/www", "/"), "/srv/www/", "join_root 根路径");
  check_eq(web_join_root("", "/a"), "/a", "join_root 空根");

  check_eq(web_join_url("/", "/a/b", "index.html"), "/a/b", "join_url 根挂载");
  check_eq(web_join_url("/static", "/static/css/a.css", "index.html"),
           "/css/a.css", "join_url 剥掉挂载前缀");
  check_eq(web_join_url("/static", "/static", "index.html"), "/index.html",
           "join_url 挂载点本身 → 索引");
  check_eq(web_join_url("/static", "/static/", "index.html"), "/index.html",
           "join_url 挂载点带斜杠 → 索引");
  check_eq(web_join_url("/", "/", "index.html"), "/index.html",
           "join_url 根 → 索引");
  check_eq(web_join_url("/static", "/other/x", "index.html"), "",
           "join_url 不在挂载点下应返回空");
  // 前缀相同但不是同一层（/staticx 不在 /static 下）
  check_eq(web_join_url("/static", "/staticx/a", "index.html"), "",
           "join_url 不能被同前缀目录骗过");

  // 包含判断：边界必须是分隔符，不能只做前缀比较
  check(web_is_within_root("/srv/www", "/srv/www/a.txt"), "根内文件");
  check(web_is_within_root("/srv/www", "/srv/www"), "根本身");
  check(web_is_within_root("/srv/www", "/srv/www/sub/a.txt"), "根内子目录");
  check(!web_is_within_root("/srv/www", "/srv/www2/a.txt"),
        "同前缀的兄弟目录必须判为根外");        // ← 只比前缀会在这里出错
  check(!web_is_within_root("/srv/www", "/srv"), "父目录在根外");
  check(!web_is_within_root("/srv/www", "/etc/passwd"), "完全不相干的路径");
  check(!web_is_within_root("/srv/www", ""), "空候选路径");
  check(web_is_within_root("/srv/www/", "/srv/www/a.txt"),
        "根带结尾斜杠也应正确");
}

// =========================================================================
// 7. 真实路径解析 —— 含 junction / 符号链接逃逸
// =========================================================================
static const char* kRootDir = "uvcpp_web_util_tmp_root";
static const char* kOutDir  = "uvcpp_web_util_tmp_outside";

static std::string pth(const std::string& a, const std::string& b) {
  return a + "/" + b;
}

static bool write_text(const std::string& path, const std::string& content) {
  std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
  if (!f.good()) return false;
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
  return f.good();
}

static void make_dir(const std::string& d) {
#ifdef _WIN32
  _mkdir(d.c_str());
#else
  mkdir(d.c_str(), 0755);
#endif
}

static void remove_dir(const std::string& d) {
#ifdef _WIN32
  _rmdir(d.c_str());
#else
  rmdir(d.c_str());
#endif
}

/// 建链接（Windows 用 junction —— 它不需要管理员权限，CI 上也能跑）
static bool make_link(const std::string& link, const std::string& target) {
#ifdef _WIN32
  const std::string cmd = "cmd /c mklink /J \"" + link + "\" \"" + target +
                          "\" >nul 2>&1";
  return std::system(cmd.c_str()) == 0;
#else
  return ::symlink(target.c_str(), link.c_str()) == 0;
#endif
}

static bool prepare_link_fs(std::string& link_path) {
  make_dir(kRootDir);
  make_dir(kOutDir);
  write_text(pth(kRootDir, "index.html"), "<html>INDEX</html>");
  make_dir(pth(kRootDir, "sub"));
  write_text(pth(kRootDir, "sub/a.txt"), "A");
  write_text(pth(kOutDir, "secret.txt"), "TOP-SECRET");

  link_path = pth(kRootDir, "escape");
  return make_link(link_path, kOutDir);
}

static void cleanup_link_fs() {
  std::remove(pth(kRootDir, "index.html").c_str());
  std::remove(pth(kRootDir, "sub/a.txt").c_str());
  remove_dir(pth(kRootDir, "sub"));
  remove_dir(pth(kRootDir, "escape"));   // 删链接本身，不会跟进目标
  std::remove(pth(kOutDir, "secret.txt").c_str());
  remove_dir(kRootDir);
  remove_dir(kOutDir);
}

void test_resolve_within_root() {
  std::cout << "[util] resolve_within_root" << std::endl;

  std::string link_path;
  const bool link_ok = prepare_link_fs(link_path);

  std::string root_real;
  if (!web_real_path(kRootDir, root_real, false)) {
    std::cerr << "  [FAIL] 无法解析临时根目录的真实路径" << std::endl;
    ++g_failures;
    cleanup_link_fs();
    return;
  }

  std::string out;
  check(web_resolve_within_root(root_real, "/index.html", out) ==
            web_path_status::OK,
        "根内已存在文件应解析成功");
  check(web_ends_with(out, "index.html"), "解析结果应指向 index.html");

  check(web_resolve_within_root(root_real, "/sub/a.txt", out) ==
            web_path_status::OK,
        "根内子目录文件应解析成功");

  // 文件不存在但父目录在 → OK（调用方 stat 之后回 404）。这正是静态服务
  // 需要的语义：不存在的文件是 404，不是 500。
  check(web_resolve_within_root(root_real, "/nope.txt", out) ==
            web_path_status::OK,
        "父目录存在时，不存在的文件也应给出可信路径");

  // 父目录都不存在 → NOT_FOUND
  check(web_resolve_within_root(root_real, "/nodir/x.txt", out) ==
            web_path_status::NOT_FOUND,
        "父目录不存在应返回 NOT_FOUND");

  check(web_resolve_within_root(root_real, "relative/x", out) ==
            web_path_status::NOT_ABSOLUTE,
        "相对路径应被拒");

  // 纵深防御：即使调用方漏了 sanitize，这里也必须挡住
  check(web_resolve_within_root(root_real, "/../" + std::string(kOutDir) +
                                             "/secret.txt",
                                out) == web_path_status::TRAVERSAL,
        "未经 sanitize 的 .. 也必须在这里被挡住");

  // ---- 关键用例：junction / 符号链接逃逸 ----
  //
  // "/escape/secret.txt" 在**纯文本**层面完全合法（无 ..、无编码、无控制
  // 字符），只有把 escape 解析成真实路径才发现它指向根之外。
  // 这一条区分了「看起来安全」和「真的安全」。
  if (link_ok) {
    std::string norm;
    check(web_sanitize_path("/escape/secret.txt", norm) == web_path_status::OK,
          "链接路径在文本层面应当是合法的（这正是危险之处）");

    const web_path_status st =
        web_resolve_within_root(root_real, "/escape/secret.txt", out);
    if (st != web_path_status::TRAVERSAL) {
      std::cerr << "  [FAIL] 指向根外的链接必须判为 TRAVERSAL，实际 "
                << web_path_status_name(st) << std::endl;
      ++g_failures;
    }
  } else {
    std::cout << "  [skip] 无法创建 junction/符号链接（权限不足），"
                 "跳过链接逃逸用例" << std::endl;
  }

  cleanup_link_fs();
}

// =========================================================================
// 8. MIME / 状态码
// =========================================================================
void test_mime_and_status() {
  std::cout << "[util] mime_and_status" << std::endl;

  check_eq(web_mime_type("/a/b.html"), "text/html", "mime html");
  check_eq(web_mime_type("index.htm"), "text/html", "mime htm");
  check_eq(web_mime_type("/x/app.js"), "application/javascript", "mime js");
  check_eq(web_mime_type("/x/a.PNG"), "image/png", "mime 扩展名大小写不敏感");
  check_eq(web_mime_type("/x/a.tar.gz"), "application/gzip", "mime 取最后一段");
  check_eq(web_mime_type("/x/noext"), "application/octet-stream", "mime 无扩展名");
  check_eq(web_mime_type("/x/a.unknownext"), "application/octet-stream",
           "mime 未知扩展名");
  check_eq(web_mime_type("/x/.hidden"), "application/octet-stream",
           "mime 点开头无扩展名");
  check_eq(web_mime_type("/video/movie.mp4"), "video/mp4", "mime mp4（断点续传）");

  check(web_mime_is_text("text/html"), "text/html 是文本");
  check(web_mime_is_text("text/html; charset=utf-8"), "带 charset 的文本");
  check(web_mime_is_text("application/json"), "json 是文本");
  check(web_mime_is_text("image/svg+xml"), "svg 是文本");
  check(!web_mime_is_text("image/png"), "png 不是文本");
  check(!web_mime_is_text("video/mp4"), "mp4 不是文本");

  check_eq(web_status_text(200), "OK", "200");
  check_eq(web_status_text(404), "Not Found", "404");
  check_eq(web_status_text(206), "Partial Content", "206");
  check_eq(web_status_text(416), "Range Not Satisfiable", "416");
  check_eq(web_status_text(9999), "Unknown", "未知状态码");
}

// =========================================================================
// 9. HTTP 日期
// =========================================================================
void test_http_date() {
  std::cout << "[util] http_date" << std::endl;

  // RFC 7231 的示例时间
  const time_t t = static_cast<time_t>(784111777);
  check_eq(web_http_date(t), "Sun, 06 Nov 1994 08:49:37 GMT", "IMF-fixdate 格式");

  // 往返
  const time_t round = web_parse_http_date(web_http_date(t));
  check(round == t, "格式化后再解析应回到原值");

  const time_t samples[] = {0, 1, 1000000000, 1700000000, 2147483647};
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    check(web_parse_http_date(web_http_date(samples[i])) == samples[i],
          "多种时间戳应能往返");
  }

  // RFC 7231 要求同时接受三种格式
  check(web_parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT") == t,
        "RFC 850 两位年");
  check(web_parse_http_date("Sun Nov  6 08:49:37 1994") == t, "asctime");
  check(web_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT") == t, "IMF");
  check(web_parse_http_date("Sun, 06 Nov 1994 08:49:37 UTC") == t, "UTC 时区");
  check(web_parse_http_date("Sun, 06 Nov 1994 08:49:37 -0500") ==
            static_cast<time_t>(-1),
        "非 GMT 时区应被拒");  // HTTP 日期按定义必须是 GMT

  check(web_parse_http_date("") == static_cast<time_t>(-1), "空串");
  check(web_parse_http_date("not a date") == static_cast<time_t>(-1), "垃圾输入");
  check(web_parse_http_date("Sun, 99 Nov 1994 08:49:37 GMT") ==
            static_cast<time_t>(-1),
        "越界的日应被拒");  // 不查范围的话 days_from_civil 会算出"合理"日期
  check(web_parse_http_date("Sun, 06 Nov 1994 99:49:37 GMT") ==
            static_cast<time_t>(-1),
        "越界的时应被拒");

  check(!web_http_date_now().empty(), "当前时间应能格式化");
}

}  // namespace

int main() {
  test_string_helpers();
  test_url_encode_decode();
  test_split_and_query();
  test_cookies();
  test_sanitize_accept();
  test_sanitize_reject();
  test_sanitize_filename();
  test_join_and_containment();
  test_resolve_within_root();
  test_mime_and_status();
  test_http_date();

  if (g_failures == 0) {
    std::cout << "[util] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[util] FAIL (" << g_failures << " checks failed)" << std::endl;
  return 2;
}
