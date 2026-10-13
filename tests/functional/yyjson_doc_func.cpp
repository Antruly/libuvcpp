/**
 * @file tests/functional/yyjson_doc_func.cpp
 * @brief `yyjson/uvcpp_json_doc.h` 的功能测试 —— 可选的 JSON 解析后端。
 *
 * ## 这一条测的是什么
 *
 * 这个模块存在的理由是"换后端不用改调用点"，所以判据也照着这句话来：
 *
 *   1. **状态逐条一致**：同一份输入，两个后端必须给出同一个结论。这是一张
 *      手写的期望表（25 条：16 条坏输入 + 边界），**两半都拿它判** —— 本后端
 *      判一次，nlohmann 路径再判一次，两次都对着同一列期望值。
 *   2. **桥的产出逐字节相同**：键序与数字形态都落在两份实现都同意的那一批语料
 *      上时，`uvcpp_json_write` 写出来的字节必须与 nlohmann 的 `dump()` 一样。
 *   3. **自己说不出口的地方如实钉住**：四处已声明（文件头）的不同 —— 成员顺序、
 *      重复键、数字形态、深度 —— 每处都有**直接**断言，而不是只在注释里写着。
 *
 * ## 两半，各有一半必须说清
 *
 * 对照的那一半要求 nlohmann 在场，而 nlohmann 只在 `UVCPP_BUILD_WEBAPP=ON` 时
 * 才被 fetch（`src/webapp/uvcpp_web_json.h` 的 `nlohmann/json.hpp` 没有第二个
 * 来源）。所以那几段整体落在 `#if UVCPP_WEBAPP_ENABLE` 里，**并且会打印一行
 * 显眼的 SKIP**：静默跳过就是本仓最忌讳的那种绿（"没测过"长得和"测过了"一样）。
 * 另一半（后端自证：状态表本身、访问器、迭代器、分配器计数、移动语义）不依赖
 * webapp，任何配置下都真跑 —— 否则 `YYJSON=ON, WEBAPP=OFF` 这棵树上这条用例
 * 就等于不存在。
 *
 * ## 见证是谁：谁判谁
 *
 *   * 状态与产出形态：**手写的期望值**（它就是规范），不是"再跑一遍库"。
 *   * 数字：`strtoll`/`strtoull`/`strtod` 是三个成熟实现，读回来的值必须等于
 *     写进去的原值 —— 这是"这个数在 JSON 里表示得对"的**独立**判据。
 *   * 值相同而字节不同（第 3 处不同）那一族：把两份文本都交给 nlohmann 再
 *     `==` —— 用**第三方**判"语义相同"，而不是说一句"看着一样"。
 *   * 分配器计数：`reset()` 之后 `memory_bytes()` 必须**回到 0**。这是"没有泄漏"
 *     这句话唯一可测的形式（yyjson 的 `free` 回调不给尺寸，所以那个数字是本库
 *     自己拿一块 16 字节的头记出来的 —— 错一点就回不到 0）。
 *
 * ## 诚实边界：吞吐与常驻内存**不在这里**
 *
 * 相对 nlohmann 的解析吞吐与 DOM 常驻内存是**量出来的**，记在 `CHANGELOG.md`
 * 与 `doc/json-guide.md`；把压测数字写成断言，就是造一条会随机变红的门禁。
 *
 * ## 命名
 *
 * 文件名含 `yyjson` ⇒ `tests/functional/CMakeLists.txt` 里那条
 * `/[^/]*yyjson[^/]*\.cpp$` 过滤器会在 `UVCPP_ENABLE_YYJSON=OFF` 时把它从目标
 * 列表里摘掉（关掉模块时"没测"必须表现为**用例不存在**，不是表现为通过）。
 * `[^/]*` 前缀是必须的：`file(GLOB)` 给的是全路径（h2 那条踩过，注释在那边）。
 * 名字**不带** `web_app_` 前缀：本模块在核心层，不需要 webapp。
 */
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_json_scan.h>
#include <yyjson/uvcpp_json_doc.h>

// nlohmann 路径只在 webapp 编进来时才存在（那才是 fetch nlohmann 的条件），
// 所以连 include 都要在里面 —— 关掉 webapp 的树上没有 <nlohmann/json.hpp>。
#if UVCPP_WEBAPP_ENABLE
#include <webapp/uvcpp_web_json.h>
#endif

using namespace uvcpp;

namespace {

int g_failures = 0;

const size_t k_default_bytes = 8u * 1024u * 1024u;

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

void check_eq_u(size_t got, size_t want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_eq_ll(long long got, long long want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_st(json_doc_status got, json_doc_status want,
              const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what
              << "\n         期望: " << json_doc_status_name(want)
              << "\n         实际: " << json_doc_status_name(got) << std::endl;
    ++g_failures;
  }
}

/// @brief 这一步必须是 OK（否则后面那些关于内容的断言都不成立）。
void must_ok(json_doc_status st, const std::string& what) {
  check_st(st, json_doc_status::OK, what);
}

// 输出器那一侧的状态是**另一个**枚举（`json_write_status`），所以这两条是重载
// 而不是同一份实现 —— 解析的状态与写的状态从来不是一回事。
void check_st(json_write_status got, json_write_status want,
              const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what
              << "\n         期望: " << json_write_status_name(want)
              << "\n         实际: " << json_write_status_name(got) << std::endl;
    ++g_failures;
  }
}

void must_ok(json_write_status st, const std::string& what) {
  check_st(st, json_write_status::OK, what);
}

#if UVCPP_WEBAPP_ENABLE
/// @brief nlohmann 路径的"这一步必须 OK"。状态是**另一个**枚举，所以是另一个
/// 帮手 —— 两边的状态类型故意不通用（见头文件那张对照表）。
void must_ok_nl(json_status st, const std::string& what) {
  if (st != json_status::OK) {
    std::cerr << "  [FAIL] " << what << "\n         期望: OK\n         实际: "
              << json_status_name(st) << std::endl;
    ++g_failures;
  }
}
#endif

/// @brief `n` 层 `[` 后面 `n` 层 `]` —— 一份**合法**的深嵌套输入，深度正好是 n。
std::string brackets(size_t n) {
  return std::string(n, '[') + std::string(n, ']');
}

/// @brief 只有开括号：深度是 n，但它**不是**合法 JSON（两件事要分得开）。
std::string opens(size_t n) { return std::string(n, '['); }

// 语料。键一律按升序写：第 1 处不同（成员顺序）不在这一组里考，它有自己的格子。
const char* k_corpus_a =
    "{\"a\":[1,-2,true,false,null,\"x\\ny\\\"z\\\\w\"],"
    "\"b\":{\"c\":0.5,\"d\":\"\xe4\xb8\xad\xe6\x96\x87\"},"
    "\"e\":[],\"f\":{}}";
const char* k_corpus_b =
    "{\"ctrl\":\"\\u0000\\u0001\\u001f\x7f\","
    "\"esc\":\"\\b\\f\\n\\r\\t\","
    "\"slash\":\"http://a/b\","
    "\"uni\":\"\\u4e2d\\u6587\"}";
const char* k_corpus_c =
    "{\"big\":18446744073709551615,\"neg\":-9223372036854775808,"
    "\"pos\":9223372036854775807,\"zero\":0}";
const char* k_corpus_d =
    "{\"a\":0.1,\"b\":-2.25,\"c\":1e+300,\"d\":123456789.5}";
const char* k_corpus_array = "[[],[1],[[2,3]],{},{\"k\":\"v\"}]";

// =========================================================================
// 第 1 组：状态表（25 条，期望值是手写的）
// =========================================================================

struct status_case {
  const char* what;
  std::string text;
  size_t max_depth;
  size_t max_bytes;
  json_doc_status want;
};

std::vector<status_case> make_status_cases() {
  std::vector<status_case> v;
  // --- 16 条坏输入 -------------------------------------------------------
  v.push_back(status_case{"1.1 空输入", std::string(), 64, k_default_bytes,
                          json_doc_status::EMPTY});
  // 空白**不是**"空输入"：EMPTY 那条只认 `len == 0`，纯空白是"不是 JSON"。
  v.push_back(status_case{"1.2 纯空白", " \t\r\n ", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.3 只有左花括号", "{", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.4 只有右花括号", "}", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.5 数组尾随逗号", "[1,]", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.6 对象尾随逗号", "{\"a\":1,}", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.7 单引号", "{'a':1}", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.8 键后面没有值", "{\"a\":}", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.9 截断的字面量", "tru", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.10 前导零", "01", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.11 两个根值", "1 2", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.12 未闭合的数组", "[1,2", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.13 未闭合的字符串", "\"abc", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.14 字符串里的非法 UTF-8", "\"\xff\"", 64,
                          k_default_bytes, json_doc_status::SYNTAX});
  v.push_back(status_case{"1.15 非法转义", "\"a\\x\"", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  v.push_back(status_case{"1.16 NaN 不是 JSON", "NaN", 64, k_default_bytes,
                          json_doc_status::SYNTAX});
  // --- 边界 -------------------------------------------------------------
  v.push_back(status_case{"1.17 BOM 开头的合法对象", "\xef\xbb\xbf{\"a\":1}",
                          64, k_default_bytes, json_doc_status::OK});
  v.push_back(status_case{"1.18 深度正好等于上限（64）", brackets(64), 64,
                          k_default_bytes, json_doc_status::OK});
  v.push_back(status_case{"1.19 深度超过上限一层（65）", brackets(65), 64,
                          k_default_bytes, json_doc_status::TOO_DEEP});
  v.push_back(status_case{"1.20 长度超过 max_bytes", "[\"" + std::string(20, 'x') +
                                                      "\"]",
                          64, 16, json_doc_status::TOO_LARGE});
  v.push_back(status_case{"1.21 又长又深（长度那条先判）", opens(70), 64, 16,
                          json_doc_status::TOO_LARGE});
  v.push_back(status_case{"1.22 又深又坏（深度那条先判）", opens(65) + "x", 64,
                          k_default_bytes, json_doc_status::TOO_DEEP});
  v.push_back(status_case{"1.23 max_depth=0 下的标量根值", "1", 0,
                          k_default_bytes, json_doc_status::OK});
  v.push_back(status_case{"1.24 max_depth=0 下的数组", "[1]", 0, k_default_bytes,
                          json_doc_status::TOO_DEEP});
  v.push_back(status_case{"1.25 重复键两边都接受", "{\"a\":1,\"a\":2}", 64,
                          k_default_bytes, json_doc_status::OK});
  return v;
}

#if UVCPP_WEBAPP_ENABLE

/// @brief 同一份输入交给 nlohmann 路径，返回它的状态（转成 int 便于与期望表对）。
int nlohmann_status_int(const std::string& text, size_t depth, size_t bytes) {
  uvcpp_json j;
  const json_status st = uvcpp_json_parse(
      text.data(), text.size(), j, json_parse_options(depth, bytes));
  return static_cast<int>(st);
}

#endif

void group_status_table() {
  const std::vector<status_case> cases = make_status_cases();
  check_eq_u(cases.size(), 25, "1.0 状态表条数（16 条坏输入 + 9 条边界）");

  for (size_t i = 0; i < cases.size(); ++i) {
    const status_case& c = cases[i];
    uvcpp_json_doc doc;
    const json_doc_status got = doc.parse(
        c.text.data(), c.text.size(), json_doc_options(c.max_depth, c.max_bytes));
    check_st(got, c.want, c.what);
    // 成功/失败与 valid() 必须一致 —— 报 OK 而文档是空的，或者报错而文档还在，
    // 都是"状态与事实不符"。
    check(doc.valid() == (c.want == json_doc_status::OK),
          std::string(c.what) + "（valid() 与状态一致）");
    if (c.want == json_doc_status::OK) {
      check_eq_s(doc.last_error(), "", std::string(c.what) + "（成功时没有错误串）");
    } else {
      check(doc.last_error() != nullptr && doc.last_error()[0] != '\0',
            std::string(c.what) + "（失败时必须有错误串，日志里要看得见）");
    }
  }

  // 空指针那一条单列：`parse(nullptr, 0)` 与 `parse("", 0)` 必须同一个答案。
  {
    uvcpp_json_doc doc;
    check_st(doc.parse(static_cast<const char*>(nullptr), 0),
             json_doc_status::EMPTY, "1.26 parse(nullptr, 0)");
  }

#if UVCPP_WEBAPP_ENABLE
  // 前五个枚举成员**数值相同**（头文件的对照表就是这么声明的）；不同的话，
  // 下面那张表里两个后端就只是"碰巧都对"，而不是"同一条线"。
  check(static_cast<int>(json_doc_status::OK) == static_cast<int>(json_status::OK) &&
            static_cast<int>(json_doc_status::EMPTY) ==
                static_cast<int>(json_status::EMPTY) &&
            static_cast<int>(json_doc_status::SYNTAX) ==
                static_cast<int>(json_status::SYNTAX) &&
            static_cast<int>(json_doc_status::TOO_DEEP) ==
                static_cast<int>(json_status::TOO_DEEP) &&
            static_cast<int>(json_doc_status::TOO_LARGE) ==
                static_cast<int>(json_status::TOO_LARGE),
        "1.27 前五个状态在两个枚举里数值相同");
  check_eq_s(json_doc_status_name(json_doc_status::OK),
             json_status_name(json_status::OK), "1.28 状态名 OK");
  check_eq_s(json_doc_status_name(json_doc_status::EMPTY),
             json_status_name(json_status::EMPTY), "1.28 状态名 EMPTY");
  check_eq_s(json_doc_status_name(json_doc_status::SYNTAX),
             json_status_name(json_status::SYNTAX), "1.28 状态名 SYNTAX");
  check_eq_s(json_doc_status_name(json_doc_status::TOO_DEEP),
             json_status_name(json_status::TOO_DEEP), "1.28 状态名 TOO_DEEP");
  check_eq_s(json_doc_status_name(json_doc_status::TOO_LARGE),
             json_status_name(json_status::TOO_LARGE), "1.28 状态名 TOO_LARGE");

  // ★ 同一列期望值，第二个后端再判一遍。这一条才是"两个后端行为一致"的判据。
  size_t agree = 0;
  for (size_t i = 0; i < cases.size(); ++i) {
    const status_case& c = cases[i];
    const int nl =
        nlohmann_status_int(c.text, c.max_depth, c.max_bytes);
    if (nl != static_cast<int>(c.want)) {
      std::cerr << "  [FAIL] 1.29 " << c.what << "：nlohmann 侧是 "
                << json_status_name(static_cast<json_status>(nl)) << "，期望是 "
                << json_doc_status_name(c.want) << std::endl;
      ++g_failures;
    } else {
      ++agree;
    }
  }
  check_eq_u(agree, cases.size(), "1.29 两个后端在同一张表上逐条一致");
#else
  std::cout << "  [SKIP] 1.27–1.29 与 nlohmann 的对照需要 webapp"
               "（UVCPP_BUILD_WEBAPP=OFF）" << std::endl;
#endif
}

// =========================================================================
// 第 2 组：两处已声明的不同（成员顺序、重复键）
// =========================================================================

void group_documented_divergences() {
  // 第 1 处：成员顺序 = **文档顺序**（不是按键排序）。这一格用一份乱序的文档，
  // 顺序断言是逐位写的 —— 谁哪天把它改成"顺手排个序"，这里当场红。
  {
    uvcpp_json_doc doc;
    must_ok(doc.parse("{\"b\":1,\"a\":2,\"c\":3}"), "2.1 乱序文档");
    std::vector<std::string> keys;
    uvcpp_json_object_iter it(doc.root());
    std::string key;
    uvcpp_json_value v;
    while (it.next(key, v)) keys.push_back(key);
    check_eq_u(keys.size(), 3, "2.1 三个成员都迭代到了");
    check(keys.size() == 3 && keys[0] == "b" && keys[1] == "a" && keys[2] == "c",
          "2.1 迭代顺序 = 文档顺序（b, a, c）");
  }

  // 第 2 处：重复键两边都**接受**，但取值不同 —— 这里取**第一个**。
  {
    uvcpp_json_doc doc;
    must_ok(doc.parse("{\"a\":1,\"a\":2}"), "2.2 重复键：本后端接受");
    long long got = 0;
    check(uvcpp_json_get_int(doc.root(), "a", got), "2.2 取得到 a");
    check_eq_ll(got, 1, "2.2 本后端取第一个（yyjson 的 obj_get 语义）");
  }

#if UVCPP_WEBAPP_ENABLE
  {
    // 同一个键序，交回 nlohmann 侧：它是**排序**的，而本后端是文档序 —— 这就是
    // 第 1 处不同被**量出来**的形式，而不是只在注释里写着。
    uvcpp_json j;
    must_ok_nl(uvcpp_json_parse("{\"b\":1,\"a\":2,\"c\":3}", j), "2.3 nlohmann 侧");
    std::vector<std::string> nl_keys;
    for (uvcpp_json::const_iterator it = j.begin(); it != j.end(); ++it) {
      nl_keys.push_back(it.key());
    }
    check_eq_u(nl_keys.size(), 3, "2.3 nlohmann 侧三个成员");
    check(nl_keys.size() == 3 && nl_keys[0] == "a" && nl_keys[1] == "b" &&
              nl_keys[2] == "c",
          "2.3 nlohmann 侧是按键排序（a, b, c）");
  }
  {
    // 第 2 处：nlohmann 取**最后**一个。两边都接受、取值不同，就是这么回事。
    uvcpp_json j;
    must_ok_nl(uvcpp_json_parse("{\"a\":1,\"a\":2}", j), "2.4 nlohmann 侧接受重复键");
    long long got = 0;
    check(uvcpp_json_get_int(j, "a", got), "2.4 nlohmann 侧取得到 a");
    check_eq_ll(got, 2, "2.4 nlohmann 侧取最后一个");
  }
#else
  std::cout << "  [SKIP] 2.3–2.4 与 nlohmann 的对照需要 webapp" << std::endl;
#endif
}

// =========================================================================
// 第 3 组：访问器与迭代器
// =========================================================================

void group_accessors() {
  const std::string text =
      "{\"n\":null,\"b\":true,\"i\":-7,\"u\":18446744073709551615,"
      "\"d\":2.5,\"s\":\"hi\",\"a\":[1,2,3],\"o\":{\"x\":1}}";
  uvcpp_json_doc doc;
  must_ok(doc.parse(text), "3.0 解析语料");
  const uvcpp_json_value root = doc.root();

  check(root.valid(), "3.1 根视图有效");
  check(root.is_object(), "3.1 根是对象");
  check_eq_s(root.type_name(), "object", "3.1 根的类型名");
  check_eq_u(root.size(), 8, "3.1 根的成员数");

  // 六种类型各判一次；同时把"不是这个类型"的那一侧也判掉（`is_*` 只判一侧的
  // 实现很容易全对）。
  check(root.get("n").is_null() && !root.get("n").is_bool(), "3.2 null");
  check(root.get("b").is_bool() && root.get("b").as_bool(), "3.2 bool = true");
  check(!root.get("i").as_bool(true) == false, "3.2 非布尔给默认值（bool）");
  check(root.get("i").is_int() && !root.get("i").is_double(), "3.2 int");
  check_eq_ll(root.get("i").as_int(), -7, "3.2 int 值");
  check(root.get("u").is_int(), "3.2 无符号整数也算 is_int()");
  check_eq_s(root.get("u").type_name(), "int", "3.2 无符号整数的类型名是 int");
  check(root.get("d").is_double() && root.get("d").is_number(), "3.2 double");
  check(root.get("d").as_double() == 2.5, "3.2 double 值");
  check(root.get("s").is_string() && !root.get("s").is_object(), "3.2 string");
  check_eq_s(root.get("s").as_string(), "hi", "3.2 string 值");
  check(root.get("a").is_array() && root.get("o").is_object(), "3.2 容器");

  // 整数读浮点、浮点读整数的两条**有意的**宽松（与 nlohmann 路径同形）。
  check_eq_ll(root.get("d").as_int(), 2, "3.3 浮点当整数读是**截断**（2.5 → 2）");
  check(root.get("i").as_double() == -7.0, "3.3 整数当浮点读");
  check_eq_ll(root.get("u").as_int(), -1,
              "3.3 超 int64 的无符号数当有符号读 = 截断（与 nlohmann 侧同形）");

  // 零拷贝取串与 `as_string()` 必须一致；长度不含结尾的 NUL。
  {
    size_t len = 0;
    const char* p = root.get("s").as_cstr(&len);
    check(p != nullptr && len == 2 && std::string(p, len) == "hi",
          "3.4 as_cstr 与 as_string 一致");
    check(root.get("i").as_cstr() == nullptr, "3.4 非字符串的 as_cstr 是空指针");
  }

  // 越界、类型不符、缺失：一律**无效视图**（不是 null —— 那是有值的）。
  {
    const uvcpp_json_value missing = root.get("nope");
    check(!missing.valid(), "3.5 缺失键是无效视图");
    check(!missing.is_null(), "3.5 无效视图不是 null（null 是有值的）");
    check_eq_s(missing.type_name(), "invalid", "3.5 无效视图的类型名");
    check_eq_u(missing.size(), 0, "3.5 无效视图的元素数是 0");
    check(!root.get("s").at(0).valid(), "3.5 非数组上 at() 是无效视图");
    check(!root.get("a").get("x").valid(), "3.5 非对象上 get() 是无效视图");
    check(!root.get("a").at(3).valid(), "3.5 数组越界");
    check(root.get("a").at(2).as_int() == 3, "3.5 数组最后一个元素可读");
    check(!root.get(static_cast<const char*>(nullptr)).valid(),
          "3.5 get(nullptr) 是无效视图");
    check_eq_s(root.get("s").as_string("D"), "hi", "3.5 取到就不给默认值");
    check_eq_s(root.get("i").as_string("D"), "D",
               "3.5 类型不符给默认值（数字**不**被写成字符串）");
    check(root.get("s").as_int(99) == 99, "3.5 类型不符给默认值（int）");
    check(root.get("nope").as_bool(true), "3.5 无效视图给默认值（bool）");
  }

  // 带长度的键重载：含 NUL 的键只有它能取到。
  {
    uvcpp_json_doc d2;
    must_ok(d2.parse("{\"a\\u0000b\":5}"), "3.6 含 NUL 的键");
    const std::string k("a\0b", 3);
    check_eq_ll(d2.root().get(k.data(), k.size()).as_int(), 5,
                "3.6 get(key, len) 取含 NUL 的键");
    check(!d2.root().get(k.data()).valid(),
          "3.6 只给 NUL 结尾的键取不到它（键是二进制的）");
  }

  // 无用例的键那两个重载也别漏：空对象、空数组、标量的 size() 都是 0。
  {
    uvcpp_json_doc d3;
    must_ok(d3.parse("[{},[],0]"), "3.7 空容器");
    check_eq_u(d3.root().at(0).size(), 0, "3.7 空对象 size 是 0");
    check_eq_u(d3.root().at(1).size(), 0, "3.7 空数组 size 是 0");
    check_eq_u(d3.root().at(2).size(), 0, "3.7 标量 size 是 0");
  }
}

void group_iterators() {
  // 对象的迭代顺序 = 文档顺序（第 1 处不同）；两种 `next` 重载必须给出同一串
  // 键 —— 零拷贝那条路上长度算错是最容易发生的事。
  uvcpp_json_doc doc;
  must_ok(doc.parse("{\"b\":1,\"a\":\"two\",\"c\":[10,20]}"), "4.0 语料");

  std::vector<std::string> zero_copy;
  {
    uvcpp_json_object_iter it(doc.root());
    const char* k = nullptr;
    size_t klen = 0;
    uvcpp_json_value v;
    while (it.next(&k, &klen, v)) {
      check(k != nullptr, "4.1 键指针非空");
      zero_copy.push_back(std::string(k, klen));
      check_eq_u(v.size(), v.is_array() ? 2 : 0, "4.1 值的形状对得上键");
    }
  }
  std::vector<std::string> own_copy;
  {
    uvcpp_json_object_iter it(doc.root());
    std::string k;
    uvcpp_json_value v;
    while (it.next(k, v)) own_copy.push_back(k);
  }
  check_eq_u(zero_copy.size(), 3, "4.2 迭代到三个成员");
  check(zero_copy == own_copy, "4.2 两种 next 重载给出同一串键");
  check(zero_copy.size() == 3 && zero_copy[0] == "b" && zero_copy[1] == "a" &&
            zero_copy[2] == "c",
        "4.2 顺序是文档顺序");

  // 迭代器建在"不是对象/不是数组"的东西上：零次迭代，不崩（yyjson 那两个
  // `*_iter_init` 对 NULL 与非容器都返回空迭代器 —— 这条钉的是我们没把返回值
  // 当成"必须成功"）。
  {
    uvcpp_json_object_iter it(doc.root().get("nope"));
    std::string k;
    uvcpp_json_value v;
    check(!it.next(k, v), "4.3 无效视图上迭代零次");
  }
  {
    uvcpp_json_doc d2;
    must_ok(d2.parse("{\"a\":[]}"), "4.3 语料");
    uvcpp_json_object_iter it(d2.root().get("a"));
    std::string k;
    uvcpp_json_value v;
    check(!it.next(k, v), "4.3 数组上建对象迭代器：零次");
  }
  {
    uvcpp_json_doc d3;
    must_ok(d3.parse("[10,20,30]"), "4.4 数组语料");
    uvcpp_json_array_iter it(d3.root());
    std::vector<long long> got;
    uvcpp_json_value v;
    while (it.next(v)) got.push_back(v.as_int());
    check_eq_u(got.size(), 3, "4.4 三个元素");
    check(got.size() == 3 && got[0] == 10 && got[1] == 20 && got[2] == 30,
          "4.4 数组按顺序迭代");
  }
  {
    uvcpp_json_doc d4;
    must_ok(d4.parse("{\"a\":[],\"o\":{}}"), "4.5 空容器语料");
    uvcpp_json_array_iter ai(d4.root().get("a"));
    uvcpp_json_value v;
    check(!ai.next(v), "4.5 空数组零次");
    uvcpp_json_object_iter oi(d4.root().get("o"));
    std::string k;
    check(!oi.next(k, v), "4.5 空对象零次");
    uvcpp_json_array_iter ai2(d4.root().get("o"));
    check(!ai2.next(v), "4.5 对象上建数组迭代器：零次");
  }
}

// =========================================================================
// 第 4 组：与 nlohmann 同名的取值入口
// =========================================================================

void group_get_helpers() {
  const std::string text =
      "{\"s\":\"hi\",\"b\":true,\"i\":7,\"f\":3.9,\"u\":18446744073709551615}";
  uvcpp_json_doc doc;
  must_ok(doc.parse(text), "5.0 解析语料");
  const uvcpp_json_value root = doc.root();

  check_eq_s(uvcpp_json_get_string(root, "s", "D"), "hi", "5.1 字符串取到");
  check_eq_s(uvcpp_json_get_string(root, "i", "D"), "D", "5.1 类型不符给默认值");
  check_eq_s(uvcpp_json_get_string(root, "nope", "D"), "D", "5.1 缺失给默认值");
  check(uvcpp_json_get_bool(root, "b", false), "5.2 布尔取到");
  check(uvcpp_json_get_bool(root, "i", true), "5.2 类型不符给默认值");
  check(!uvcpp_json_get_bool(root, "nope", false), "5.2 缺失给默认值");

  long long iv = -1;
  check(uvcpp_json_get_int(root, "i", iv) && iv == 7, "5.3 整数取到");
  iv = -1;
  check(uvcpp_json_get_int(root, "f", iv) && iv == 3,
        "5.3 浮点截断成整数（3.9 → 3，与 nlohmann 侧一致）");
  iv = -1;
  check(uvcpp_json_get_int(root, "u", iv) && iv == -1,
        "5.3 超 int64 的无符号数截断（与 nlohmann 侧一致）");
  iv = 12345;
  check(!uvcpp_json_get_int(root, "s", iv), "5.3 类型不符返回 false");
  // ★ 失败时**不写 out**：写 0 会让调用方分不清"取到了 0"和"没取到"。
  check_eq_ll(iv, 12345, "5.3 失败时不改 out");
  check(!uvcpp_json_get_int(root, "nope", iv), "5.3 缺失返回 false");

  double dv = -1.0;
  check(uvcpp_json_get_double(root, "f", dv) && dv == 3.9, "5.4 浮点取到");
  check(uvcpp_json_get_double(root, "i", dv) && dv == 7.0, "5.4 整数当浮点读");
  check(uvcpp_json_get_double(root, "u", dv) && dv == 18446744073709551615.0,
        "5.4 大无符号数的浮点值");
  dv = 1.0;
  check(!uvcpp_json_get_double(root, "s", dv), "5.4 类型不符返回 false");
  check(dv == 1.0, "5.4 失败时不改 out");

  // 自由函数入口与成员入口是同一件事（`uvcpp_json_parse(..., out)` 就是
  // `out.parse(...)`），这条钉住两者不许漂移。
  {
    uvcpp_json_doc d2;
    must_ok(uvcpp_json_parse(text.data(), text.size(), d2), "5.5 自由函数入口");
    must_ok(uvcpp_json_parse(std::string(text), d2), "5.5 std::string 重载");
    check_eq_s(uvcpp_json_get_string(d2.root(), "s", "D"), "hi", "5.5 内容一致");
    uvcpp_json_doc d3;
    must_ok(uvcpp_json_parse(text.data(), text.size(), d3,
                             json_doc_options(64, k_default_bytes)),
           "5.5 带限制的重载");
  }

#if UVCPP_WEBAPP_ENABLE
  // 同一份文本，两个后端的**同名**取值函数必须给出同一个结果 —— 包括默认值与
  // 那个"失败不写 out"的约定。这是"换后端只改变量类型"那句话的可测形式。
  {
    uvcpp_json j;
    must_ok_nl(uvcpp_json_parse(text, j), "5.6 nlohmann 侧");
    check_eq_s(uvcpp_json_get_string(j, "s", "D"), "hi", "5.6 字符串");
    check_eq_s(uvcpp_json_get_string(j, "i", "D"), "D", "5.6 字符串：类型不符");
    check(uvcpp_json_get_bool(j, "b", false) ==
              uvcpp_json_get_bool(root, "b", false),
          "5.6 布尔");
    long long a = 0;
    long long b = 0;
    check(uvcpp_json_get_int(j, "f", a) == uvcpp_json_get_int(root, "f", b) &&
              a == b,
          "5.6 浮点截断（3.9 → 3）两边一致");
    check(uvcpp_json_get_int(j, "u", a) == uvcpp_json_get_int(root, "u", b) &&
              a == b,
          "5.6 超 int64 的无符号数两边一致");
    check(uvcpp_json_get_double(j, "u", dv) ==
              uvcpp_json_get_double(root, "u", dv) &&
              dv == 18446744073709551615.0,
          "5.6 大无符号数的浮点值两边一致");
    a = 12345;
    b = 12345;
    check(!uvcpp_json_get_int(j, "s", a) && !uvcpp_json_get_int(root, "s", b) &&
              a == b,
          "5.6 失败且都不改 out");
  }
#else
  std::cout << "  [SKIP] 5.6 与 nlohmann 的同名入口对照需要 webapp" << std::endl;
#endif
}

// =========================================================================
// 第 5 组：桥（DOM → uvcpp_json_writer）
// =========================================================================

/// @brief 把一份文档写进本库的输出器，返回产出的字节。
std::string write_doc(const uvcpp_json_doc& doc, const std::string& what) {
  uvcpp_json_writer w;
  check_st(uvcpp_json_write(doc.root(), w), json_write_status::OK,
           what + "：uvcpp_json_write 的状态");
  must_ok(w.finish(), what + "：finish");
  return w.str();
}

void group_bridge_self() {
  // 后端自证：产出必须是**能被解析回来的 JSON**，而且转一圈之后值不变。
  // （不需要 nlohmann —— 这一条在 WEBAPP=OFF 的树上也要真跑。）
  struct corpus {
    const char* what;
    const char* text;
    const char* must_contain;
  };
  const corpus cs[] = {
      {"corpus_a", k_corpus_a, "x\\ny\\\"z\\\\w\""},
      {"corpus_b", k_corpus_b, "\\u0000"},
      {"corpus_c", k_corpus_c, "18446744073709551615"},
      {"corpus_d", k_corpus_d, "1e+300"},
      {"corpus_array", k_corpus_array, "[[2,3]]"},
  };
  for (size_t i = 0; i < sizeof(cs) / sizeof(cs[0]); ++i) {
    uvcpp_json_doc doc;
    must_ok(doc.parse(cs[i].text), std::string("6.1 ") + cs[i].what + " 解析");
    const std::string out = write_doc(doc, cs[i].what);
    // 非空洞判据：产出的长度必须**接近**输入（不是空串、不是 "null"），并且
    // 含一个只有真写出内容才会出现的片段。少了这一条，"两边都失败"会让下面
    // 那些比对全部空转成绿。
    check(out.size() >= std::string(cs[i].text).size() / 2 &&
              out.find(cs[i].must_contain) != std::string::npos,
          std::string("6.1 ") + cs[i].what + " 的产出非空洞（含 " +
              cs[i].must_contain + "）");
    uvcpp_json_doc back;
    must_ok(back.parse(out), std::string("6.1 ") + cs[i].what + " 产出可再解析");
    // 再解析一遍之后，整棵树再写一次必须**逐字节相同**（写→读→写是幂等的）。
    const std::string out2 = write_doc(back, cs[i].what);
    check_eq_s(out2, out, std::string("6.1 ") + cs[i].what + " 写→读→写幂等");
  }

  // 无效视图写成 `null`：与 `value((const char*)nullptr)` 同一个理由 —— 没有
  // 这个值在 JSON 里有确切表示，不该让整份响应构造失败。
  {
    uvcpp_json_doc doc;
    must_ok(doc.parse("{\"a\":1}"), "6.2 语料");
    uvcpp_json_writer w;
    check_st(uvcpp_json_write(doc.root().get("nope"), w), json_write_status::OK,
             "6.2 缺失键写出去不是错误");
    must_ok(w.finish(), "6.2 finish");
    check_eq_s(w.str(), "null", "6.2 缺失键写出 null");

    const uvcpp_json_value nothing;
    uvcpp_json_writer w2;
    check_st(uvcpp_json_write(nothing, w2), json_write_status::OK,
             "6.2 默认构造的无效视图也写 null");
    must_ok(w2.finish(), "6.2 finish");
    check_eq_s(w2.str(), "null", "6.2 默认构造的无效视图写出 null");
  }

  // 标量根值、字符串根值、数组根值都能直接当根写。
  {
    uvcpp_json_doc d1, d2, d3;
    must_ok(d1.parse("42"), "6.3 标量根");
    must_ok(d2.parse("\"hi\""), "6.3 字符串根");
    must_ok(d3.parse("[1,2]"), "6.3 数组根");
    check_eq_s(write_doc(d1, "6.3"), "42", "6.3 标量根写出 42");
    check_eq_s(write_doc(d2, "6.3"), "\"hi\"", "6.3 字符串根写出 \"hi\"");
    check_eq_s(write_doc(d3, "6.3"), "[1,2]", "6.3 数组根写出 [1,2]");
  }
}

#if UVCPP_WEBAPP_ENABLE
void group_bridge_vs_nlohmann() {
  // ★ 逐字节相同。前提是那份语料的两边都已声明同意的部分：键序、数字形态。
  // 每个语料都先断言 nlohmann 侧**解析成功且产出非空** —— 否则一个失败的
  // nlohmann 侧会 dump 出 "null"，比对面要是也恰好是 "null" 就假绿了。
  const char* corpora[] = {k_corpus_a, k_corpus_b, k_corpus_c, k_corpus_d,
                           k_corpus_array};
  for (size_t i = 0; i < sizeof(corpora) / sizeof(corpora[0]); ++i) {
    const std::string text(corpora[i]);
    uvcpp_json_doc doc;
    must_ok(doc.parse(text), "7.1 本后端解析语料 " + std::to_string(i));

    uvcpp_json j;
    must_ok_nl(uvcpp_json_parse(text, j), "7.1 nlohmann 解析语料 " + std::to_string(i));
    const std::string theirs = uvcpp_json_dump(j);
    check(!theirs.empty() && theirs != "null",
          "7.1 nlohmann 侧产出非空洞（语料 " + std::to_string(i) + "）");

    const std::string ours = write_doc(doc, "7.1 语料 " + std::to_string(i));
    check_eq_s(ours, theirs,
               "7.1 语料 " + std::to_string(i) + "：桥的产出与 dump() 逐字节相同");
  }
}
#endif

// =========================================================================
// 第 6 组：数字（第 3 处不同）
// =========================================================================

void group_numbers() {
  // 形态是手写的（`uvcpp_json_writer` 的最短往返，见它文件头）：整数值的浮点会
  // 掉小数点、整数保持整数、`1e2` 只留下 `100`。**值相同、字节可能不同** 这句
  // 话在这张表里就是逐条的数。
  struct num_case {
    const char* text;   ///< 输入（一个数字当根值）
    const char* want;   ///< 本库写出来的字节；空串表示"形态不写死，只判值"
    double want_d;      ///< 再用 strtod 读回来必须等于它
  };
  const num_case cases[] = {
      {"1", "1", 1.0},
      {"-1", "-1", -1.0},
      {"1.0", "1", 1.0},
      {"0.0", "0", 0.0},
      {"-0.0", "-0", -0.0},
      {"0.1", "0.1", 0.1},
      {"1e2", "100", 100.0},
      {"1E+2", "100", 100.0},
      {"2.5", "2.5", 2.5},
      {"1e+300", "1e+300", 1e300},
      // π 的形态**不写死**（15 位还是 17 位是实现的最短往返算法的自由），
      // 但它必须能往返 —— 见下面那条 strtod 断言。
      {"3.141592653589793", "", 3.141592653589793},
      {"18446744073709551615", "18446744073709551615",
       18446744073709551615.0},
      {"-9223372036854775808", "-9223372036854775808",
       -9223372036854775808.0},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    uvcpp_json_doc doc;
    must_ok(doc.parse(cases[i].text), std::string("8.1 解析 ") + cases[i].text);
    const std::string out = write_doc(doc, std::string("8.1 ") + cases[i].text);
    if (cases[i].want[0] != '\0') {
      check_eq_s(out, cases[i].want,
                 std::string("8.1 ") + cases[i].text + " 写出来的形态");
    }
    // 独立见证：真解析器读回来必须是同一个值（"最短往返"就是这句话）。
    char* end = nullptr;
    errno = 0;
    const double back = std::strtod(out.c_str(), &end);
    check(end != nullptr && *end == '\0',
          std::string("8.1 ") + out + " 被 strtod 整串吃掉");
    check(back == cases[i].want_d,
          std::string("8.1 ") + out + " 往返之后的值相同");
  }

  // 整数的往返用 `strtoll`/`strtoull` 当见证（浮点那条见证不了 2^64-1）。
  {
    const char* texts[] = {"0", "-1", "9223372036854775807",
                           "-9223372036854775808", "4294967295"};
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) {
      uvcpp_json_doc doc;
      must_ok(doc.parse(texts[i]), std::string("8.2 解析 ") + texts[i]);
      const std::string out = write_doc(doc, std::string("8.2 ") + texts[i]);
      char* end = nullptr;
      errno = 0;
      const long long back = std::strtoll(out.c_str(), &end, 10);
      check(end != nullptr && *end == '\0' && errno == 0,
            std::string("8.2 ") + out + " 整串是整数、不溢出");
      check_eq_ll(back, std::strtoll(texts[i], nullptr, 10),
                  std::string("8.2 ") + texts[i] + " 往返");
    }
  }
  {
    uvcpp_json_doc doc;
    must_ok(doc.parse("18446744073709551615"), "8.2 解析 2^64-1");
    const std::string out = write_doc(doc, "8.2 2^64-1");
    char* end = nullptr;
    errno = 0;
    const unsigned long long back = std::strtoull(out.c_str(), &end, 10);
    check(end != nullptr && *end == '\0' && errno == 0,
          "8.2 无符号那一路也要 strtoull 吃得下");
    check(back == 18446744073709551615ull, "8.2 2^64-1 的往返");
  }

  // 浮点值当整数读：**截断**（不是四舍五入、不是报错）。
  {
    uvcpp_json_doc doc;
    must_ok(doc.parse("{\"a\":3.9,\"b\":-3.9,\"c\":3.0}"), "8.3 浮点语料");
    check_eq_ll(doc.root().get("a").as_int(), 3, "8.3 3.9 → 3");
    check_eq_ll(doc.root().get("b").as_int(), -3, "8.3 -3.9 → -3");
    check_eq_ll(doc.root().get("c").as_int(), 3, "8.3 3.0 → 3");
    check(doc.root().get("c").is_double(), "8.3 3.0 仍然是浮点（JSON 不分整浮）");
  }

#if UVCPP_WEBAPP_ENABLE
  // ★ 第 3 处不同的判据：**值相同、字节可能不同**。两边的产出都交给 nlohmann
  // 再比 `==` —— 用第三方判语义，而不是"看着一样"。
  const char* values[] = {
      "1.0",     "0.0",       "-0.0",     "0.1",
      "3.141592653589793", "1e300", "1.7976931348623157e308", "5e-324",
      "18446744073709551615", "-9223372036854775808", "9007199254740993",
  };
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    const std::string text = std::string("{\"v\":") + values[i] + "}";
    uvcpp_json_doc doc;
    must_ok(doc.parse(text), std::string("8.4 解析 ") + values[i]);
    const std::string ours = write_doc(doc, std::string("8.4 ") + values[i]);

    uvcpp_json a;
    uvcpp_json b;
    must_ok_nl(uvcpp_json_parse(text, a), std::string("8.4 原文 ") + values[i]);
    must_ok_nl(uvcpp_json_parse(ours, b),
           std::string("8.4 本后端的产出 ") + ours);
    check(a == b, std::string("8.4 ") + values[i] + "：两边的**值**相同（字节：" +
                      ours + " vs " + text + "）");
  }
  // 而且这股不同是**真实存在**的（不是"碰巧一样"）：`1.0` 在 nlohmann 那边
  // 是 `1.0`，在桥这边是 `1`。
  {
    uvcpp_json j;
    must_ok_nl(uvcpp_json_parse("{\"v\":1.0}", j), "8.5 nlohmann 侧");
    check_eq_s(uvcpp_json_dump(j), "{\"v\":1.0}",
               "8.5 nlohmann 写回小数点在（第 3 处不同确实存在）");
    uvcpp_json_doc doc;
    must_ok(doc.parse("{\"v\":1.0}"), "8.5 本后端");
    check_eq_s(write_doc(doc, "8.5"), "{\"v\":1}", "8.5 桥这边写 1");
  }
#else
  std::cout << "  [SKIP] 8.4–8.5 与 nlohmann 的数字语义对照需要 webapp" << std::endl;
#endif
}

// =========================================================================
// 第 7 组：分配器计数、失败不改动、移动语义
// =========================================================================

void group_memory_and_move() {
  // 空文档：三个计数都是 0，且没有文档。
  {
    uvcpp_json_doc doc;
    check(!doc.valid(), "9.1 空文档 invalid");
    check_eq_u(doc.memory_bytes(), 0, "9.1 空文档 memory_bytes");
    check_eq_u(doc.peak_bytes(), 0, "9.1 空文档 peak_bytes");
    check_eq_u(doc.allocation_count(), 0, "9.1 空文档 allocation_count");
    check(!doc.root().valid(), "9.1 空文档的根是无效视图");
    check_eq_s(doc.last_error(), "", "9.1 空文档没有错误串");
    check_eq_u(doc.last_error_position(), 0, "9.1 空文档错误偏移是 0");
  }

  // 解析成功：有值、有分配、峰值 ≥ 当前。
  {
    uvcpp_json_doc doc;
    must_ok(doc.parse(k_corpus_a), "9.2 解析语料");
    check(doc.valid(), "9.2 valid");
    check(doc.memory_bytes() > 0, "9.2 memory_bytes > 0");
    check(doc.allocation_count() >= 1, "9.2 allocation_count >= 1");
    check(doc.peak_bytes() >= doc.memory_bytes(), "9.2 peak >= live");

    // ★ "没有泄漏"的可测形式：reset 之后必须**回到 0**（yyjson 的 free 回调不给
    // 尺寸，这个数字是本库自己记的 —— 记错一点就回不到 0）。
    const size_t live = doc.memory_bytes();
    doc.reset();
    check(!doc.valid(), "9.2 reset 之后 invalid");
    check_eq_u(doc.memory_bytes(), 0, "9.2 reset 之后 memory_bytes 回到 0");
    check_eq_u(doc.allocation_count(), 0, "9.2 reset 之后计数块也放掉了");
    check(live > 0, "9.2 （上一步确实有过占用，否则这一格是空的）");
  }

  // 同一个对象上换一份文档：占用必须是**新的那一份**的，不是两份之和 ——
  // 期望值取"另起一个对象解析同一份文本"，两边走的是同一条路，所以这个数
  // 是确定的（不是"大概不一样"）。
  {
    uvcpp_json_doc doc;
    must_ok(doc.parse(k_corpus_b), "9.3 第一份");
    check(doc.memory_bytes() > 0, "9.3 （第一份确实占了）");
    must_ok(doc.parse(k_corpus_a), "9.3 第二份（同一对象上再解析）");
    uvcpp_json_doc fresh;
    must_ok(fresh.parse(k_corpus_a), "9.3 另起一个对象解析第二份");
    check_eq_u(doc.memory_bytes(), fresh.memory_bytes(),
               "9.3 换文档之后占用就是新那份的（旧的那份放掉了）");
  }

  // ★ 解析失败**不改动本对象**：旧文档、旧的值视图、旧的占用全部照旧。
  {
    uvcpp_json_doc doc;
    must_ok(doc.parse(k_corpus_b), "9.4 先解析一份好的");
    const size_t before = doc.memory_bytes();
    const uvcpp_json_value slash = doc.root().get("slash");
    check_eq_s(slash.as_string(), "http://a/b", "9.4 失败前值可读");

    const std::string bad = "[1,2";
    check_st(doc.parse(bad), json_doc_status::SYNTAX, "9.4 坏输入报 SYNTAX");
    check(doc.valid(), "9.4 失败之后文档还在");
    check_eq_u(doc.memory_bytes(), before, "9.4 失败之后占用没变");
    check_eq_s(slash.as_string(), "http://a/b", "9.4 失败之前的视图还能读");
    check(doc.last_error() != nullptr && doc.last_error()[0] != '\0',
          "9.4 失败留下了错误串");
    check(doc.last_error_position() <= bad.size(),
          "9.4 错误偏移落在输入范围内");

    // 再成功一次，错误串要清掉（否则日志里那句会一直挂着，指着一份已经不存在的
    // 失败）。
    must_ok(doc.parse(k_corpus_a), "9.4 再成功一次");
    check_eq_s(doc.last_error(), "", "9.4 成功之后错误串清空");
    check_eq_u(doc.last_error_position(), 0, "9.4 成功之后错误偏移归零");
  }

  // 失败的那一次里，临时分配器必须已经还干净（不然每失败一次漏一份输入的量）。
  {
    uvcpp_json_doc doc;
    must_ok(doc.parse(k_corpus_a), "9.5 先解析一份");
    const size_t before = doc.memory_bytes();
    for (int i = 0; i < 200; ++i) {
      uvcpp_json_doc scratch;
      check_st(scratch.parse("[1,2"), json_doc_status::SYNTAX, "9.5 反复失败");
      check_eq_u(scratch.memory_bytes(), 0, "9.5 失败的文档占用为 0");
    }
    check_eq_u(doc.memory_bytes(), before, "9.5 反复失败不影响别的文档");
  }

  // 移动：所有权只有一份 —— 源对象变"没有文档"，计数器（分配器要用的那块）
  // 必须**跟着走**，否则目标那份文档之后每次 free 都在写别人家的内存。
  {
    uvcpp_json_doc a;
    must_ok(a.parse(k_corpus_a), "9.6 源文档");
    const size_t live = a.memory_bytes();
    uvcpp_json_value v = a.root().get("a");
    check_eq_ll(v.at(0).as_int(), 1, "9.6 移动之前视图可读");

    uvcpp_json_doc b(std::move(a));
    check(!a.valid(), "9.6 移动之后源对象 invalid");
    check_eq_u(a.memory_bytes(), 0, "9.6 移动之后源对象占用 0");
    check_eq_u(a.allocation_count(), 0, "9.6 移动之后源对象的计数块也没了");
    check(b.valid(), "9.6 目标对象有文档");
    check_eq_u(b.memory_bytes(), live, "9.6 占用跟着文档一起搬走了");
    check_eq_ll(v.at(0).as_int(), 1, "9.6 视图跟着文档一起有效");
    check_eq_ll(b.root().get("a").at(3).as_bool(true) ? 1 : 0, 0,
                "9.6 搬过去之后内容没变（第 4 个元素是 false）");
  }
  {
    // 移动赋值到一份**已有文档**的对象上：旧的那份必须先放掉，否则占用会叠加
    // （这一条钉的就是"先 reset 再接管"）。
    uvcpp_json_doc a;
    must_ok(a.parse(k_corpus_b), "9.7 源文档");
    const size_t live = a.memory_bytes();
    uvcpp_json_doc c;
    must_ok(c.parse(k_corpus_d), "9.7 目标已有文档");
    const size_t old = c.memory_bytes();
    check(old > 0, "9.7 （目标确实有占用）");

    c = std::move(a);
    check_eq_u(c.memory_bytes(), live, "9.7 移动赋值之后占用 = 源的（不是两份之和）");
    check(!a.valid(), "9.7 源对象 invalid");
    check_eq_s(c.root().get("slash").as_string(), "http://a/b",
               "9.7 接管的文档内容对");
  }
  {
    // 自移动：不许把自己清空。
    uvcpp_json_doc a;
    must_ok(a.parse(k_corpus_c), "9.8 自移动");
    const size_t live = a.memory_bytes();
    uvcpp_json_doc& ref = a;
    a = std::move(ref);
    check(a.valid() && a.memory_bytes() == live, "9.8 自移动赋值不破坏自己");
    check_eq_ll(a.root().get("pos").as_int(), 9223372036854775807ll,
                "9.8 自移动之后内容还在");
  }
}

// =========================================================================
// 第 8 组：深度判定（两个后端用的是**同一个**预扫描）
// =========================================================================

void group_depth_scan() {
  // 核心层的那个函数本身（两个后端都调它）。
  check_eq_u(uvcpp_json_max_nesting("{\"a\":\"[[[[[[\"}", 14), 1,
             "10.1 字符串里的括号不算深度");
  check_eq_u(uvcpp_json_max_nesting("]]]]", 4), 0, "10.1 多余的闭括号不减到负数");
  check_eq_u(uvcpp_json_max_nesting("{}", 2), 1, "10.1 一层对象");
  check_eq_u(uvcpp_json_max_nesting("[[]]", 4), 2, "10.1 两层数组");
  check_eq_u(uvcpp_json_max_nesting(static_cast<const char*>(nullptr), 0), 0,
             "10.1 nullptr");
  check_eq_u(uvcpp_json_max_nesting("[1,2,3]", 7), 1, "10.1 扁平数组");
  {
    const std::string deep = opens(1000);
    check_eq_u(uvcpp_json_max_nesting(deep.data(), deep.size()), 1000,
               "10.1 一千层");
  }

  // 深度与状态：64 层过、65 层不过（默认上限 64），两边同一个数。
  for (size_t n = 63; n <= 66; ++n) {
    uvcpp_json_doc doc;
    const std::string text = brackets(n);
    const json_doc_status got = doc.parse(text);
    const json_doc_status want = (n <= 64) ? json_doc_status::OK
                                           : json_doc_status::TOO_DEEP;
    check_st(got, want, "10.2 " + std::to_string(n) + " 层的结论");
  }

  // ★ 预扫描对字符串免疫 —— 这一条能分辨"两个后端各自判深度"与"两个后端调
  // 同一个预扫描"：一份 7 个 `[` 在字符串里的输入，最大深度其实是 1，所以
  // `max_depth = 1` 下**必须通过**。不免疫的实现会说 8 层而报 TOO_DEEP。
  {
    const std::string text = "{\"a\":\"[[[[[[[\"}";
    uvcpp_json_doc doc;
    check_st(doc.parse(text, json_doc_options(1, k_default_bytes)),
             json_doc_status::OK,
             "10.3 max_depth=1：字符串里的括号不算深度");
    check(uvcpp_json_max_nesting(text.data(), text.size()) == 1,
          "10.3 它的深度确实是 1");
  }

#if UVCPP_WEBAPP_ENABLE
  // 同一件事在 nlohmann 侧再判一遍（那条线也用的是同一个预扫描）。
  {
    const std::string text = "{\"a\":\"[[[[[[[\"}";
    uvcpp_json j;
    check(static_cast<int>(uvcpp_json_parse(
              text.data(), text.size(), j,
              json_parse_options(1, k_default_bytes))) ==
              static_cast<int>(json_status::OK),
          "10.4 nlohmann 侧 max_depth=1 也判 OK（同一条线）");
  }
  for (size_t n = 64; n <= 65; ++n) {
    const std::string text = brackets(n);
    uvcpp_json_doc doc;
    uvcpp_json j;
    const json_doc_status a = doc.parse(text);
    const json_status b = uvcpp_json_parse(text, j);
    check(static_cast<int>(a) == static_cast<int>(b),
          "10.4 " + std::to_string(n) + " 层在两边同一个结论");
  }
#else
  std::cout << "  [SKIP] 10.4 与 nlohmann 的深度对照需要 webapp" << std::endl;
#endif

  // ★ 深度上限是**策略**、不是爆栈保护：把上限抬到输入深度之上，这份十万层的
  // 输入必须真的解析成功。本后端的读入器是**迭代**的（容器栈在堆上，不占线程
  // 栈），所以这里没有栈可爆 —— 这一格钉的就是文件头那句"解析器不递归"。
  //
  // 形状说明：哪天换成递归下降的读入器，这一格不是"变红"而是**崩**（线程栈被
  // 十万帧打穿）。留着它是因为"崩"同样是红的，而这条断言守的东西值得一条红的。
  {
    const std::string deep = brackets(100000);
    uvcpp_json_doc doc;
    check_st(doc.parse(deep, json_doc_options(200000, k_default_bytes)),
             json_doc_status::OK,
             "10.5 十万层：上限抬到它之上就真解析（读入器不递归）");
    check(doc.valid() && doc.root().size() == 1, "10.5 十万层的根是单元素数组");
    check(doc.memory_bytes() > 0, "10.5 十万层也真的分配了");
  }
}

}  // namespace

int main() {
  group_status_table();
  group_documented_divergences();
  group_accessors();
  group_iterators();
  group_get_helpers();
  group_bridge_self();
  group_numbers();
  group_memory_and_move();
  group_depth_scan();
#if UVCPP_WEBAPP_ENABLE
  group_bridge_vs_nlohmann();
#else
  std::cout << "  [SKIP] 7.1 桥与 dump() 的逐字节比对需要 webapp" << std::endl;
#endif

  if (g_failures == 0) {
    std::cout << "[yyjson_doc] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[yyjson_doc] FAIL (" << g_failures << ")" << std::endl;
  return 2;
}
