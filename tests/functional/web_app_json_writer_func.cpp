/**
 * @file tests/functional/web_app_json_writer_func.cpp
 * @brief 拿 **nlohmann** 当独立见证，验证 `uvcpp_json_writer` 写出来的是合法 JSON。
 *
 * ## 为什么这条要挂在 webapp 上
 *
 * `uvcpp_json_writer` 自己是**核心模块**（不吃 webapp 门禁，核心那半的功能测试在
 * `tests/functional/json_writer_func.cpp`）。可"绕出来的是不是合法 JSON"这件事，
 * 自己跟自己对账是不算数的 —— 需要**另一个实现**来读。本仓已有的那个实现就是
 * nlohmann（`webapp/uvcpp_web_json.h` 的 `uvcpp_json_parse`），所以这条验证挂
 * 在 webapp 上，文件名也必须是 `web_app_*`：webapp 关掉时它要被**摘掉**，
 * 而不是变成一个打印 SKIP 的 `main()`（`tests/functional/CMakeLists.txt` 里
 * 记着的"没测表现为通过"）。
 *
 * ## 这条用例里最重要的一组是第 6 组
 *
 * 第 1–5 组全是"解析成功 + 字段相等"。可**只**有这些的话，一个"对什么都说 OK"
 * 的解析器也能让它们全绿 —— 判据得能说出自己在什么情况下是错的。所以第 6 组
 * 是**对照臂**：手写几段坏 JSON，`uvcpp_json_parse` 必须报错；其中最要紧的一条
 * 是"字符串里一个**裸**换行"—— nlohmann 按 RFC 8259 拒它，而写出的转义形态它
 * 收。这两条一起，才说明本模块的转义是**承重**的，而不是装饰。
 */
#include <iostream>
#include <string>

#include <uvcpp/uvcpp_json_writer.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_response.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq_s(const std::string& got, const std::string& want,
                const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

/// @brief 解析**必须成功**，并把结果交回；失败时报红并返回一个空对象。
uvcpp_json must_parse(const std::string& text, const std::string& what) {
  uvcpp_json j;
  const json_status st = uvcpp_json_parse(text, j);
  if (st != json_status::OK) {
    std::cerr << "  [FAIL] " << what << "：解析失败 " << json_status_name(st)
              << "\n         输入: " << text << std::endl;
    ++g_failures;
    return uvcpp_json();
  }
  return j;
}

// =========================================================================
// 第 1 组：结构与各类型往返
// =========================================================================

void group_round_trip() {
  std::string body;
  uvcpp_json_writer w(body);
  w.object_begin();
  w.member("ok", true);
  w.member("no", false);
  w.member("nul", nullptr);
  w.member("i", static_cast<long long>(-42));
  w.member("u", 42ull);
  w.member("d", 0.5);
  w.member("s", std::string("plain"));
  w.key("arr");
  w.array_begin();
  w.value(1);
  w.value(2);
  w.value(3);
  w.array_end();
  w.key("obj");
  w.object_begin();
  w.member("inner", std::string("x"));
  w.object_end();
  w.object_end();
  if (w.finish() != json_write_status::OK) {
    std::cerr << "  [FAIL] 1.0 writer 收尾失败：" << json_write_status_name(w.status())
              << std::endl;
    ++g_failures;
    return;
  }

  const uvcpp_json j = must_parse(body, "1.1 结构往返");
  check(j.is_object(), "1.1 解析出来是对象");
  check_eq_i(j["ok"].get<bool>() ? 1 : 0, 1, "1.2 bool true");
  check_eq_i(j["no"].get<bool>() ? 1 : 0, 0, "1.3 bool false");
  check(j["nul"].is_null(), "1.4 null 还是 null");
  check_eq_i(j["i"].get<long long>(), -42, "1.5 负数往返");
  check_eq_i(static_cast<long long>(j["u"].get<unsigned long long>()), 42,
             "1.6 无符号往返");
  check(j["d"].get<double>() == 0.5, "1.7 double 往返");
  check_eq_s(j["s"].get<std::string>(), "plain", "1.8 字符串往返");
  check(j["arr"].is_array() && j["arr"].size() == 3, "1.9 数组长度");
  check_eq_i(j["arr"][2].get<int>(), 3, "1.10 数组末元素");
  check_eq_s(j["obj"]["inner"].get<std::string>(), "x", "1.11 嵌套对象");
}

// =========================================================================
// 第 2 组：转义往返（逐字节相等）
// =========================================================================

void group_escape_round_trip() {
  // 一段把所有要处理的东西都塞进去的字符串：引号、反斜杠、五个短形态、
  // 一个 `\u00XX` 的、一个斜杠、以及一点 UTF-8。
  std::string tricky;
  tricky += "quote:\" backslash:\\ short:";
  tricky += "\b\f\n\r\t";
  tricky += " ctrl:\x01\x1f";
  tricky += " slash:/path?a=1&b=2";
  tricky += " utf8:\xe4\xb8\xad\xe6\x96\x87";
  tricky += " del:\x7f";

  std::string body;
  uvcpp_json_writer w(body);
  w.object_begin();
  w.member("v", tricky);
  w.object_end();
  if (w.finish() != json_write_status::OK) {
    std::cerr << "  [FAIL] 2.0 writer 收尾失败" << std::endl;
    ++g_failures;
    return;
  }
  const uvcpp_json j = must_parse(body, "2.1 转义往返");
  // 判据是**逐字节**相等：解析回来的字符串必须与当初写进去的那串一个字节不差。
  check_eq_s(j["v"].get<std::string>(), tricky,
             "2.1 转义往返之后逐字节相等（含 0x01/0x1f/0x7f/UTF-8）");
  // `\u0000` 那一路单独一格：NUL 在 `std::string` 里是普通字节，所以能被比对。
  {
    const std::string with_nul("a\0b", 3);
    std::string b2;
    uvcpp_json_writer w2(b2);
    w2.value(with_nul);
    w2.finish();
    const uvcpp_json j2 = must_parse(b2, "2.2 含 NUL 的值");
    check_eq_s(j2.get<std::string>(), with_nul, "2.2 NUL 往返（长度也得对）");
  }
  // 键也走同一条路：解析回来的**键名**必须与写进去的相同。
  {
    const std::string weird_key("k\ney\"x");
    std::string b3;
    uvcpp_json_writer w3(b3);
    w3.object_begin();
    w3.key(weird_key);
    w3.value(1);
    w3.object_end();
    w3.finish();
    const uvcpp_json j3 = must_parse(b3, "2.3 怪键名");
    check(j3.find(weird_key) != j3.end(), "2.3 键名逐字节往返");
  }
}

// =========================================================================
// 第 3 组：数字往返
// =========================================================================

void group_number_round_trip() {
  std::string body;
  uvcpp_json_writer w(body);
  w.object_begin();
  w.member("i64min", static_cast<long long>(-9223372036854775807ll - 1));
  w.member("i64max", static_cast<long long>(9223372036854775807ll));
  w.member("u64max", static_cast<unsigned long long>(18446744073709551615ull));
  w.member("tenth", 0.1);
  w.member("pi", 3.141592653589793);
  w.member("big", 1e300);
  w.member("tiny", 1e-20);
  w.member("neg", -0.0);
  w.object_end();
  w.finish();

  const uvcpp_json j = must_parse(body, "3.1 数字往返");
  check_eq_i(j["i64min"].get<long long>(), -9223372036854775807ll - 1, "3.2 INT64_MIN");
  check_eq_i(j["i64max"].get<long long>(), 9223372036854775807ll, "3.3 INT64_MAX");
  check(j["u64max"].get<unsigned long long>() == 18446744073709551615ull,
        "3.4 UINT64_MAX");
  // 这一条是"最短往返"那套东西的**终局见证**：解析器读回来必须还是同一个 double。
  check(j["tenth"].get<double>() == 0.1, "3.5 0.1 往返");
  // π 要 17 位十进制才能往返，是"最短往返"那套东西的**硬例子**：15 位不够。
  // 这里量的是**位数够不够**（解析器读出同一个 double），与核心用例里量
  // "写出来的字符串长什么样"是两件事。
  check(j["pi"].get<double>() == 3.141592653589793, "3.5b π 往返（要 17 位）");
  check(j["big"].get<double>() == 1e300, "3.6 1e300 往返");
  check(j["tiny"].get<double>() == 1e-20, "3.7 1e-20 往返");
  check(j["neg"].get<double>() == -0.0, "3.8 -0 往返");
}

// =========================================================================
// 第 4 组：规模（分批与深嵌套都要能被解析）
// =========================================================================

void group_scale() {
  // 2000 个元素：跑一遍转义/逗号的两条分支几百次，只有一处写错就解析不过。
  {
    std::string body;
    uvcpp_json_writer w(body);
    w.object_begin();
    w.key("a");
    w.array_begin();
    for (int i = 0; i < 2000; ++i) w.value(i);
    w.array_end();
    w.object_end();
    if (w.finish() != json_write_status::OK) {
      std::cerr << "  [FAIL] 4.0 2000 元素收尾失败" << std::endl;
      ++g_failures;
      return;
    }
    const uvcpp_json j = must_parse(body, "4.1 2000 元素");
    check(j["a"].is_array() && j["a"].size() == 2000, "4.2 元素个数");
    check_eq_i(j["a"][0].get<int>(), 0, "4.3 首元素");
    check_eq_i(j["a"][1999].get<int>(), 1999, "4.4 末元素");
  }
  // 深嵌套：写到 50 层。**上限取 50 而不是 64**，因为解析侧 `json_parse_options`
  // 的默认 `max_depth` 就是 64，而两边的"第几层"未必同口径 —— 挨着边界测就
  // 变成了在测解析器，不是在测 writer。（writer 自己的边界在核心那条用例里。）
  {
    const int kDepth = 50;
    std::string body;
    uvcpp_json_writer w(body);
    for (int i = 0; i < kDepth; ++i) {
      w.array_begin();
    }
    w.value(1);
    for (int i = 0; i < kDepth; ++i) {
      w.array_end();
    }
    if (w.finish() != json_write_status::OK) {
      std::cerr << "  [FAIL] 4.5 深嵌套收尾失败" << std::endl;
      ++g_failures;
      return;
    }
    const uvcpp_json j = must_parse(body, "4.6 50 层");
    check(j.is_array(), "4.7 外层是数组");
    check_eq_s(body.substr(0, 3), "[[[", "4.8 头三层是 `[`");
  }
}

// =========================================================================
// 第 5 组：接到响应上（产出原样进 body）
// =========================================================================

void group_into_response() {
  std::string body;
  uvcpp_json_writer w(body);
  w.object_begin();
  w.member("msg", std::string("hi \"there\""));
  w.object_end();
  w.finish();

  uvcpp_web_response resp;
  resp.json_str(w.str());

  check(resp.body_size() == w.str().size(), "5.1 body 长度等于 writer 产出");
  check_eq_s(std::string(resp.body_data(), resp.body_size()), w.str(),
             "5.2 body 字节与 writer 产出一致（没有被二次加工）");
  // 用 `content_type()` 而不是 `get_header("content-type")`：前者是公开访问器，
  // 与头表里键名的大小写无关。
  check(resp.content_type().find("application/json") != std::string::npos,
        "5.3 content-type 是 application/json（实际：\"" +
            resp.content_type() + "\"）");
  // 再解析一次：走完整条"writer → 响应 body → 解析"的路。
  const uvcpp_json j = must_parse(std::string(resp.body_data(), resp.body_size()),
                                  "5.4 响应 body 里的 JSON");
  check_eq_s(j["msg"].get<std::string>(), "hi \"there\"", "5.5 值往返");
}

// =========================================================================
// 第 6 组：对照臂 —— 坏输入必须被拒（否则前五组等于没判）
// =========================================================================

void group_negative_control() {
  struct bad_case {
    const char* text;
    const char* what;
  };
  const bad_case bad[] = {
      {"{\"a\":}", "6.1 值缺失"},
      {"{\"a\":1,}", "6.2 尾随逗号"},
      {"{\"a\" 1}", "6.3 键值之间没有冒号"},
      {"[1,2", "6.4 括号不配对"},
      {"{\"a\":1}", "对照：这段是好的（下面单独判它 OK）"},
  };
  for (size_t i = 0; i + 1 < sizeof(bad) / sizeof(bad[0]); ++i) {
    uvcpp_json j;
    const json_status st = uvcpp_json_parse(std::string(bad[i].text), j);
    check(st != json_status::OK, std::string(bad[i].what) + "：必须被拒");
  }
  // 最后那条是**反向对照**：同一套代码判一条合法输入必须 OK。少了它，上面
  // 那四条"必须被拒"可能只是因为解析器对什么都拒。
  {
    uvcpp_json j;
    check(uvcpp_json_parse(std::string("{\"a\":1}"), j) == json_status::OK,
          "6.5 反向对照：合法的输入必须通过");
  }

  // ★ 本文件里最要紧的一条：**裸换行**。
  //    字符串里一个真实的 0x0A 字节，RFC 8259 是不允许的，nlohmann 会拒；
  //    而 writer 写出去的转义形态（`\n`）它收。
  //    这两条**必须一起判** —— 只判前一条，就说明不了"转义是承重的"。
  {
    uvcpp_json j;
    const std::string raw = "{\"v\":\"a\nb\"}";  // 里面是真·换行
    check(uvcpp_json_parse(raw, j) != json_status::OK,
          "6.6 裸换行必须被拒（这正是不转义的下场）");

    std::string body;
    uvcpp_json_writer w(body);
    w.object_begin();
    w.member("v", std::string("a\nb"));
    w.object_end();
    w.finish();
    const uvcpp_json j2 = must_parse(body, "6.7 转义之后必须能收");
    check_eq_s(j2["v"].get<std::string>(), "a\nb", "6.8 收回来还是原来那个换行");
    check(body.find('\\') != std::string::npos, "6.9 产出里确实是转义形态");
  }
}

}  // namespace

int main() {
  group_round_trip();
  group_escape_round_trip();
  group_number_round_trip();
  group_scale();
  group_into_response();
  group_negative_control();

  if (g_failures == 0) {
    std::cout << "[web_app_json_writer] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[web_app_json_writer] FAIL (" << g_failures << ")" << std::endl;
  return 2;
}
