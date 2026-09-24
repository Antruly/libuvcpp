/**
 * @file tests/functional/json_writer_func.cpp
 * @brief `uvcpp_json_writer.h` 的功能测试 —— 应用层 JSON 构造器。
 *
 * ## 这一条测的是什么
 *
 * 一个流式构造器的产出只有两种坏法：**字节不对**（少个逗号、该转义的没转义、
 * 数字写成另一种值）和**状态不对**（误用之后还继续写、深度/大小上限不生效）。
 * 所以这里的断言分两族：
 *
 *   - **逐字节比对**：期望串是手写的（它就是规范），而不是"再跑一遍库的结果"。
 *     自己跟自己对账没有意义。
 *   - **返回码 + 连带效应**：误用必须报 `MISUSE`，而且**报完之后不许再动缓冲**
 *     —— 后半句比前半句重要。只断言返回码的话，"返回了错误码但缓冲区已经被
 *     写坏了"这种实现照样能全绿，而那正是调用方最可能踩到的形状。
 *
 * ## 数字那组有真解析器当见证
 *
 * 字符串的转义只能靠手写期望串判（没有一个独立实现能当参照），但**数字可以**：
 * `strtoll`/`strtoull`/`strtod`/`strtof` 是四个成熟实现，把它们读回来的值与
 * 写进去的原值比对，就是"这个数字在 JSON 里表示得对不对"的**独立**判据。所以
 * 那组里每个数都要过一遍往返，而不只看字符串长得像不像。
 *
 * ## 诚实边界：跨度批量的**效率**没有独立见证
 *
 * `append_escaped` 是"扫到第一个要转义的字节、把前面整段一次追加"实现的（见
 * `src/uvcpp/uvcpp_json_writer.cpp` 的文件头），但**从外部区分不出**它和"逐字节
 * 追加"—— 两条路的产出逐字节相同，而分配次数/调用次数不是这个进程能观测的。
 * 这里能判的只有**逐字节正确**：1 MiB 干净串往返相等、4096 个换行转义后长度
 * 正好翻倍、含各种控制字符的输入产出里**不存在裸控制字节**。谁把跨度逻辑写坏
 * 了，这三条里至少有一条会红。
 *
 * ## 命名
 *
 * 文件名不带 `web_` / `web_app_` / `web_ssl_` / `h2_` 任一前缀：本用例只用
 * **核心模块**，与 web / webapp / OpenSSL / nghttp2 四个开关都无关，所以四条
 * 过滤器都不该命中它。反过来，一旦改名成 `web_json_writer_func.cpp`，"关掉
 * web 时它被摘掉"就会表现为"这条用例从此不存在"—— 那是
 * `tests/functional/CMakeLists.txt` 里记着的"没测表现为通过"老坑。
 */
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include <uvcpp/uvcpp_json_writer.h>

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

void check_eq_u(size_t got, size_t want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_st(json_write_status got, json_write_status want,
              const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what
              << "\n         期望: " << json_write_status_name(want)
              << "\n         实际: " << json_write_status_name(got) << std::endl;
    ++g_failures;
  }
}

/// @brief 这一步必须是 OK（否则后面那些关于内容的断言都不成立）。
void must_ok(json_write_status st, const std::string& what) {
  check_st(st, json_write_status::OK, what);
}

// =========================================================================
// 第 1 组：结构与逗号
// =========================================================================

void group_structure() {
  // 空容器：逗号逻辑最容易在这里露馅（`count == 0` 的那个分支）。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "1.1 空对象：object_begin");
    must_ok(w.object_end(), "1.1 空对象：object_end");
    must_ok(w.finish(), "1.1 空对象：finish");
    check_eq_s(w.str(), "{}", "1.1 空对象的产出");
  }
  {
    uvcpp_json_writer w;
    must_ok(w.array_begin(), "1.2 空数组：array_begin");
    must_ok(w.array_end(), "1.2 空数组：array_end");
    must_ok(w.finish(), "1.2 空数组：finish");
    check_eq_s(w.str(), "[]", "1.2 空数组的产出");
  }

  // 多成员：逗号写在**键**之前（不是值之后），且第一个成员不写逗号。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "1.3 object_begin");
    must_ok(w.key("a"), "1.3 key a");
    must_ok(w.value(1), "1.3 value a");
    must_ok(w.key("b"), "1.3 key b");
    must_ok(w.value(2), "1.3 value b");
    must_ok(w.key("c"), "1.3 key c");
    must_ok(w.value(3), "1.3 value c");
    must_ok(w.object_end(), "1.3 object_end");
    must_ok(w.finish(), "1.3 finish");
    check_eq_s(w.str(), "{\"a\":1,\"b\":2,\"c\":3}", "1.3 三个成员的产出");
  }
  {
    uvcpp_json_writer w;
    must_ok(w.array_begin(), "1.4 array_begin");
    must_ok(w.value(1), "1.4 v1");
    must_ok(w.value(2), "1.4 v2");
    must_ok(w.value(3), "1.4 v3");
    must_ok(w.array_end(), "1.4 array_end");
    must_ok(w.finish(), "1.4 finish");
    check_eq_s(w.str(), "[1,2,3]", "1.4 三个元素的产出");
  }

  // 嵌套：容器作为**值**的逗号结算发生在写 `{` 之前 —— 混着来一遍。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "1.5 object_begin");
    must_ok(w.key("o"), "1.5 key o");
    must_ok(w.object_begin(), "1.5 内层 object_begin");
    must_ok(w.key("x"), "1.5 内层 key");
    must_ok(w.value(true), "1.5 内层 value");
    must_ok(w.object_end(), "1.5 内层 object_end");
    must_ok(w.key("a"), "1.5 key a");
    must_ok(w.array_begin(), "1.5 array_begin");
    must_ok(w.value(1), "1.5 a1");
    must_ok(w.array_begin(), "1.5 数组套数组");
    must_ok(w.array_end(), "1.5 空内层数组");
    must_ok(w.value(2), "1.5 a2");
    must_ok(w.array_end(), "1.5 array_end");
    must_ok(w.key("n"), "1.5 key n");
    must_ok(w.null_value(), "1.5 null_value");
    must_ok(w.object_end(), "1.5 object_end");
    must_ok(w.finish(), "1.5 finish");
    check_eq_s(w.str(), "{\"o\":{\"x\":true},\"a\":[1,[],2],\"n\":null}",
               "1.5 嵌套的产出");
  }

  // `member()` 是 `key` + `value` 的合并形态，产出必须与分开写**逐字节相同**。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "1.6 object_begin");
    must_ok(w.member("s", std::string("x")), "1.6 member string");
    must_ok(w.member("i", 5), "1.6 member int");
    must_ok(w.member("b", true), "1.6 member bool");
    must_ok(w.object_end(), "1.6 object_end");
    must_ok(w.finish(), "1.6 finish");
    check_eq_s(w.str(), "{\"s\":\"x\",\"i\":5,\"b\":true}",
               "1.6 member() 的产出（bool 必须是 true，不是 1）");
  }

  // 标量也能单独当根值。
  {
    uvcpp_json_writer w;
    must_ok(w.value(42), "1.7 标量根值");
    must_ok(w.finish(), "1.7 finish");
    check_eq_s(w.str(), "42", "1.7 标量根值的产出");
  }
  {
    uvcpp_json_writer w;
    must_ok(w.value("hi"), "1.8 字符串根值");
    must_ok(w.finish(), "1.8 finish");
    check_eq_s(w.str(), "\"hi\"", "1.8 字符串根值的产出");
  }
}

// =========================================================================
// 第 2 组：转义
// =========================================================================

void group_escape() {
  // 两个必转的字节。
  {
    uvcpp_json_writer w;
    must_ok(w.value("a\"b"), "2.1 引号");
    must_ok(w.finish(), "2.1 finish");
    check_eq_s(w.str(), "\"a\\\"b\"", "2.1 `\"` 转成 `\\\"`");
  }
  {
    uvcpp_json_writer w;
    must_ok(w.value("a\\b"), "2.2 反斜杠");
    must_ok(w.finish(), "2.2 finish");
    check_eq_s(w.str(), "\"a\\\\b\"", "2.2 `\\` 转成 `\\\\`");
  }

  // 五个短形态。
  {
    uvcpp_json_writer w;
    must_ok(w.value("\b\f\n\r\t"), "2.3 五个短形态");
    must_ok(w.finish(), "2.3 finish");
    check_eq_s(w.str(), "\"\\b\\f\\n\\r\\t\"", "2.3 五个短形态的产出");
  }

  // 其余控制字符走 `\u00XX`（小写十六进制）；0x7F **不**转义（JSON 不要求）。
  {
    uvcpp_json_writer w;
    const char ctrl[4] = {0x00, 0x01, 0x1F, 0x7F};
    must_ok(w.value(ctrl, sizeof(ctrl)), "2.4 控制字符");
    must_ok(w.finish(), "2.4 finish");
    const std::string want = "\"\\u0000\\u0001\\u001f\x7f\"";
    check_eq_s(w.str(), want, "2.4 `\\u00XX` 是小写、0x7F 原样");
  }

  // ★ 与 hical 的分歧点：`/` **不**转义。这条专门占一格，因为它是一处刻意的
  // 不同 —— 谁哪天"顺手补上"了斜杠转义，这里会红。
  {
    uvcpp_json_writer w;
    must_ok(w.value("http://a/b"), "2.5 斜杠");
    must_ok(w.finish(), "2.5 finish");
    check_eq_s(w.str(), "\"http://a/b\"", "2.5 `/` 不转义（与 hical 的有意分歧）");
  }

  // UTF-8 多字节：逐字节搬运，一个字节都不许动。
  {
    uvcpp_json_writer w;
    must_ok(w.value("\xe4\xb8\xad\xe6\x96\x87"), "2.6 UTF-8");
    must_ok(w.finish(), "2.6 finish");
    check_eq_s(w.str(), "\"\xe4\xb8\xad\xe6\x96\x87\"", "2.6 UTF-8 原样输出");
  }

  // 键也要转义 —— 这一条容易漏（值的转义做了，键的忘了）。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "2.7 object_begin");
    must_ok(w.key("a\"b"), "2.7 含引号的键");
    must_ok(w.value(1), "2.7 value");
    must_ok(w.object_end(), "2.7 object_end");
    must_ok(w.finish(), "2.7 finish");
    check_eq_s(w.str(), "{\"a\\\"b\":1}", "2.7 键里的 `\"` 也要转义");
  }

  // 空串与空键都是合法的（不是 MISUSE）。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "2.8 object_begin");
    must_ok(w.key(""), "2.8 空键");
    must_ok(w.value(""), "2.8 空值");
    must_ok(w.object_end(), "2.8 object_end");
    must_ok(w.finish(), "2.8 finish");
    check_eq_s(w.str(), "{\"\":\"\"}", "2.8 空键与空串");
  }

  // 跨度批量：干净段整段追加，转义点只打断一次 —— 正确性用逐字节比对判。
  {
    uvcpp_json_writer w;
    const std::string clean(4096, 'a');
    must_ok(w.value(clean + "\"" + clean), "2.9 长干净串夹一个引号");
    must_ok(w.finish(), "2.9 finish");
    const std::string want = "\"" + clean + "\\\"" + clean + "\"";
    check_eq_s(w.str(), want, "2.9 段前/段中/段后三段都要逐字节对");
  }

  // 1 MiB 干净串：产出长度必须正好是 len + 2（多一个字节都说明实现乱动过）。
  {
    uvcpp_json_writer w;
    const std::string big(1024 * 1024, 'x');
    must_ok(w.value(big), "2.10 1 MiB 干净串");
    must_ok(w.finish(), "2.10 finish");
    check_eq_u(w.str().size(), big.size() + 2, "2.10 1 MiB 的产出长度");
    check(w.str() == "\"" + big + "\"", "2.10 1 MiB 的产出逐字节相等");
  }

  // 全是换行：转义后长度**正好翻倍**（`\n` → `\` + `n`）。
  {
    uvcpp_json_writer w;
    const std::string nl(4096, '\n');
    must_ok(w.value(nl), "2.11 4096 个换行");
    must_ok(w.finish(), "2.11 finish");
    check_eq_u(w.str().size(), nl.size() * 2 + 2, "2.11 转义后长度正好翻倍");
    check(w.str().substr(1, 2) == "\\n", "2.11 头两个字节是 `\\n`");
    check(w.str().substr(w.str().size() - 3, 2) == "\\n", "2.11 尾部也是 `\\n`");
  }

  // 不变量：产出里**不存在**裸的 0x00–0x1F。把 32 个控制字符全灌一遍再扫产出，
  // 比逐个字符对期望串更能说明"没有漏网的控制字节"（漏一个也是一个坏字节）。
  {
    std::string all_ctrl;
    for (int c = 0; c < 0x20; ++c) all_ctrl.push_back(static_cast<char>(c));
    uvcpp_json_writer w;
    must_ok(w.value(all_ctrl), "2.12 全部控制字符");
    must_ok(w.finish(), "2.12 finish");
    bool has_raw_ctrl = false;
    for (size_t i = 1; i + 1 < w.str().size(); ++i) {  // 跳过两端的引号
      if (static_cast<unsigned char>(w.str()[i]) < 0x20) {
        has_raw_ctrl = true;
        break;
      }
    }
    check(!has_raw_ctrl, "2.12 产出里不该有裸控制字节");
    // 顺带把长度也算准：5 个短形态各 2 字节、其余 27 个各 6 字节，加两个引号。
    check_eq_u(w.str().size(), 5 * 2 + 27 * 6 + 2, "2.12 转义后的总长度");
  }
}

// =========================================================================
// 第 3 组：数字（每个数都过一遍真解析器的往返）
// =========================================================================

void group_numbers() {
  // 下面那几处 `(std::numeric_limits<...>::max)()` 的括号**不是装饰**：
  // `windows.h` 把 `min`/`max` 定义成宏，`...::max()` 里的 `max` 会被当成一次
  // 函数式宏调用拆掉（`C4003`，`/Zc:preprocessor` 下是硬错）。这个 TU 现在没有
  // `windows.h`，所以是**预防性**的；同一件事的正例见
  // `src/webapp/uvcpp_web_json_reflect.h` 与 `web_app_static_func.cpp`。
  // --- 有符号整数：期望串手写，并且 `strtoll` 读回来必须等于原值 -----------
  {
    const long long vals[] = {
        0,
        -1,
        (std::numeric_limits<long long>::min)(),
        (std::numeric_limits<long long>::max)(),
        -32768,
    };
    const char* want[] = {
        "0",
        "-1",
        "-9223372036854775808",
        "9223372036854775807",
        "-32768",
    };
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); ++i) {
      uvcpp_json_writer w;
      must_ok(w.value(vals[i]), std::string("3.1 有符号整数 ") + want[i]);
      must_ok(w.finish(), std::string("3.1 finish ") + want[i]);
      check_eq_s(w.str(), want[i], std::string("3.1 产出 ") + want[i]);
      errno = 0;
      char* end = nullptr;
      const long long back = std::strtoll(w.str().c_str(), &end, 10);
      const std::string what = std::string("3.1 往返 ") + want[i];
      check(end != nullptr && *end == '\0' && errno == 0, what + "（整串被吃、无溢出）");
      check(back == vals[i], what + "（读回来等于原值）");
    }
  }

  // --- 无符号整数：同上，见证是 `strtoull` ---------------------------------
  {
    const unsigned long long vals[] = {
        0ull,
        65535ull,
        4294967295ull,
        (std::numeric_limits<unsigned long long>::max)(),
    };
    const char* want[] = {"0", "65535", "4294967295", "18446744073709551615"};
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); ++i) {
      uvcpp_json_writer w;
      must_ok(w.value(vals[i]), std::string("3.2 无符号整数 ") + want[i]);
      must_ok(w.finish(), std::string("3.2 finish ") + want[i]);
      check_eq_s(w.str(), want[i], std::string("3.2 产出 ") + want[i]);
      errno = 0;
      char* end = nullptr;
      const unsigned long long back = std::strtoull(w.str().c_str(), &end, 10);
      const std::string what = std::string("3.2 往返 ") + want[i];
      check(end != nullptr && *end == '\0' && errno == 0, what + "（整串被吃、无溢出）");
      check(back == vals[i], what + "（读回来等于原值）");
    }
  }

  // --- 八个宽度重载各自可达且写法正确（少一个转发就会在这些格里红） --------
  {
    uvcpp_json_writer w0;
    w0.value(static_cast<short>(-32768));
    w0.finish();
    check_eq_s(w0.str(), "-32768", "3.3 short");

    uvcpp_json_writer w1;
    w1.value(static_cast<unsigned short>(65535));
    w1.finish();
    check_eq_s(w1.str(), "65535", "3.3 unsigned short");

    uvcpp_json_writer w2;
    w2.value(static_cast<int>(-2147483647 - 1));
    w2.finish();
    check_eq_s(w2.str(), "-2147483648", "3.3 int");

    uvcpp_json_writer w3;
    w3.value(static_cast<unsigned int>(4294967295u));
    w3.finish();
    check_eq_s(w3.str(), "4294967295", "3.3 unsigned int");

    uvcpp_json_writer w4;
    w4.value(static_cast<long>(-123456789L));
    w4.finish();
    check_eq_s(w4.str(), "-123456789", "3.3 long");

    uvcpp_json_writer w5;
    w5.value(static_cast<unsigned long>(4000000000ul));
    w5.finish();
    check_eq_s(w5.str(), "4000000000", "3.3 unsigned long");

    uvcpp_json_writer w6;
    w6.value(static_cast<long long>(-9223372036854775807ll - 1));
    w6.finish();
    check_eq_s(w6.str(), "-9223372036854775808", "3.3 long long");

    uvcpp_json_writer w7;
    w7.value(static_cast<unsigned long long>(18446744073709551615ull));
    w7.finish();
    check_eq_s(w7.str(), "18446744073709551615", "3.3 unsigned long long");
  }

  // 整数产出**不含**小数点与指数 —— 这条防的是"整数走了浮点那条路"。
  {
    const char* ints[] = {"0",    "-1",  "-9223372036854775808",
                          "9223372036854775807", "-32768",
                          "18446744073709551615"};
    for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); ++i) {
      const std::string s(ints[i]);
      check(s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
                s.find('E') == std::string::npos,
            std::string("3.4 整数不该出现小数点或指数：") + s);
    }
  }

  // --- 浮点：**最短能往返**的形态，每个数都过 `strtod`/`strtof` 往返 -------
  //
  // 这一组不是在测"像不像"，是在测"读回来是不是同一个值"。`0.1f` 那一格还多
  // 一条：`float` 的往返目标是 `float` 而不是 `double`，所以 6 位有效数字就够
  // —— 谁把 `float` 也按 17 位写，产出会变成 `0.100000001...`，这里会红。
  struct fp_case {
    double v;
    bool is_float;
    const char* want;
  };
  const fp_case fcases[] = {
      {0.1, false, "0.1"},
      {0.5, false, "0.5"},
      {1.0, false, "1"},
      {-0.0, false, "-0"},
      {1e300, false, "1e+300"},
      {1e-20, false, "1e-20"},
      {static_cast<double>(0.1f), true, "0.1"},
      {static_cast<double>(3.14159265f), true, "3.14159274"},
      {static_cast<double>(1.5f), true, "1.5"},
  };
  for (size_t i = 0; i < sizeof(fcases) / sizeof(fcases[0]); ++i) {
    uvcpp_json_writer w;
    if (fcases[i].is_float) {
      must_ok(w.value(static_cast<float>(fcases[i].v)), "3.5 float 写入");
    } else {
      must_ok(w.value(fcases[i].v), "3.5 double 写入");
    }
    must_ok(w.finish(), "3.5 finish");
    const std::string got = w.str();
    check_eq_s(got, fcases[i].want, std::string("3.5 产出形态 ") + fcases[i].want);
    char* end = nullptr;
    if (fcases[i].is_float) {
      const float back = std::strtof(got.c_str(), &end);
      check(end != nullptr && *end == '\0', "3.5 float 整串被吃");
      check(back == static_cast<float>(fcases[i].v),
            std::string("3.5 float 往返 ") + got);
    } else {
      const double back = std::strtod(got.c_str(), &end);
      check(end != nullptr && *end == '\0', "3.5 double 整串被吃");
      check(back == fcases[i].v, std::string("3.5 double 往返 ") + got);
    }
  }

  // π：15 位有效数字不够往返，所以必须退到 17 位 —— 这一格的**形态**不写死
  // （那是实现的自由），但往返是硬判据。
  {
    const double pi = 3.141592653589793;
    uvcpp_json_writer w;
    must_ok(w.value(pi), "3.6 pi 写入");
    must_ok(w.finish(), "3.6 finish");
    char* end = nullptr;
    const double back = std::strtod(w.str().c_str(), &end);
    check(end != nullptr && *end == '\0', "3.6 pi 整串被吃");
    check(back == pi, std::string("3.6 pi 往返（15 位不够时要退 17 位）：") + w.str());
  }

  // NaN / ±Inf ⇒ `null`。JSON 里没有这三个字面量，而报 0 或者报一个巨大的数
  // 是**骗人**的；`null` 是能往下走且不骗人的表示。
  {
    uvcpp_json_writer w;
    must_ok(w.value(std::nan("")), "3.7 NaN");
    must_ok(w.finish(), "3.7 finish");
    check_eq_s(w.str(), "null", "3.7 NaN ⇒ null");
  }
  {
    uvcpp_json_writer w;
    must_ok(w.value(std::numeric_limits<double>::infinity()), "3.8 +Inf");
    must_ok(w.finish(), "3.8 finish");
    check_eq_s(w.str(), "null", "3.8 +Inf ⇒ null");
  }
  {
    uvcpp_json_writer w;
    must_ok(w.value(-std::numeric_limits<double>::infinity()), "3.9 -Inf");
    must_ok(w.finish(), "3.9 finish");
    check_eq_s(w.str(), "null", "3.9 -Inf ⇒ null");
  }
}

// =========================================================================
// 第 4 组：误用（返回码 + 报完之后不许再动缓冲）
// =========================================================================

/**
 * @brief 断言"这一步报 `MISUSE`，而且**缓冲区一个字节都没变**"。
 *
 * 后半句是这一组存在的理由：只判返回码的话，"返回了错误但那一下已经把逗号写
 * 出去了"这种实现全绿，而调用方拿到的是一份坏产出。
 */
void expect_misuse_and_frozen(uvcpp_json_writer& w, json_write_status st,
                              const std::string& what) {
  check_st(st, json_write_status::MISUSE, what + "（返回码）");
  const size_t before = w.size();
  const json_write_status next = w.value(999);
  check_eq_u(w.size(), before, what + "（报错之后缓冲区不许再变）");
  check_st(next, json_write_status::MISUSE, what + "（粘性：后续调用仍报这个错）");
  check(!w.ok(), what + "（ok() 应当为假）");
}

void group_misuse() {
  // 4.1 根位置写两个值（那是拼接，不是 JSON）。
  {
    uvcpp_json_writer w;
    must_ok(w.value(1), "4.1 第一个根值");
    check_eq_u(w.size(), 1, "4.1 第一个根值已经写下");
    expect_misuse_and_frozen(w, w.value(2), "4.1 第二个根值");
  }
  // 4.2 根位置写键。
  {
    uvcpp_json_writer w;
    expect_misuse_and_frozen(w, w.key("a"), "4.2 根位置写键");
  }
  // 4.3 数组里写键。
  {
    uvcpp_json_writer w;
    must_ok(w.array_begin(), "4.3 array_begin");
    expect_misuse_and_frozen(w, w.key("a"), "4.3 数组里写键");
  }
  // 4.4 对象里值没配键。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "4.4 object_begin");
    expect_misuse_and_frozen(w, w.value(1), "4.4 对象里值没配键");
  }
  // 4.5 键没配值就关对象（那会产出 `{"a":}`）。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "4.5 object_begin");
    must_ok(w.key("a"), "4.5 key");
    check_st(w.object_end(), json_write_status::MISUSE, "4.5 键没配值就关对象");
  }
  // 4.6 连着写两个键。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "4.6 object_begin");
    must_ok(w.key("a"), "4.6 第一个键");
    check_st(w.key("b"), json_write_status::MISUSE, "4.6 连着写两个键");
  }
  // 4.7 容器不配对：对象还没关就来一个 `]`。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "4.7 object_begin");
    check_st(w.array_end(), json_write_status::MISUSE, "4.7 对象里来 `]`");
  }
  // 4.8 多余的 `}`（栈已空）。
  {
    uvcpp_json_writer w;
    check_st(w.object_end(), json_write_status::MISUSE, "4.8 多余的 `}`");
  }
  // 4.9 `key(nullptr)`。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "4.9 object_begin");
    check_st(w.key(static_cast<const char*>(nullptr)), json_write_status::MISUSE,
             "4.9 key(nullptr)");
  }
  // 4.10 空构造器的 `finish()`：一个值都没写，那不是 JSON。
  {
    uvcpp_json_writer w;
    check_st(w.finish(), json_write_status::MISUSE, "4.10 空构造器 finish()");
  }
  // 4.11 还有容器没关就 `finish()` ⇒ UNCLOSED（不是 MISUSE）。
  {
    uvcpp_json_writer w;
    must_ok(w.object_begin(), "4.11 object_begin");
    check_st(w.finish(), json_write_status::UNCLOSED, "4.11 没关容器就 finish()");
    // 而且它**没有**替你把括号补上 —— 补了就是静默猜意图。
    check_eq_s(w.str(), "{", "4.11 finish() 不许替你补括号");
  }
  // 4.12 值那里给了 nullptr 但长度不为 0 ⇒ MISUSE（不许"当空串"糊过去）。
  {
    uvcpp_json_writer w;
    check_st(w.value(static_cast<const char*>(nullptr), 3),
             json_write_status::MISUSE, "4.12 value(nullptr, 3)");
  }
  // 4.13 `value((const char*)nullptr)` 是 `null`（这是**合法**用法，不是误用）。
  {
    uvcpp_json_writer w;
    must_ok(w.value(static_cast<const char*>(nullptr)), "4.13 value(nullptr)");
    must_ok(w.finish(), "4.13 finish");
    check_eq_s(w.str(), "null", "4.13 value(nullptr) ⇒ null");
  }
}

// =========================================================================
// 第 5 组：深度上限
// =========================================================================

void group_depth() {
  // 默认上限 64：64 层容器写得进去，第 65 层不行。
  //
  // 这里用**对象套对象**而不是数组，因为那样每一层都要先给一个键 —— 顺带把
  // "键的状态（`key_open`）在深嵌套下也跟着走"这件事一起测了。（顺带记一笔：
  // 最初这一格写的是连着 64 个 `object_begin()`，那是 `{{{{…`，**本来就该**
  // MISUSE —— 红的是测试不是实现。）
  {
    uvcpp_json_writer w;
    for (size_t i = 0; i < uvcpp_json_writer::k_max_depth; ++i) {
      if (i > 0) must_ok(w.key("k"), "5.1 第 " + std::to_string(i + 1) + " 层的键");
      must_ok(w.object_begin(), "5.1 开第 " + std::to_string(i + 1) + " 层");
    }
    check_eq_u(w.depth(), uvcpp_json_writer::k_max_depth, "5.1 depth() 到顶");
    must_ok(w.key("k"), "5.1 第 65 层的键（键本身还写得进去）");
    check_st(w.object_begin(), json_write_status::TOO_DEEP,
             "5.1 第 65 层应当 TOO_DEEP");
    check_eq_u(w.depth(), uvcpp_json_writer::k_max_depth,
               "5.1 失败之后 depth 不该变");
    // 粘性：接下来全是 TOO_DEEP，关也关不回去。
    check_st(w.object_end(), json_write_status::TOO_DEEP, "5.1 粘性：object_end");
    check_st(w.finish(), json_write_status::TOO_DEEP, "5.1 粘性：finish");
  }
  // 正好 64 层能写完、能收尾。
  {
    uvcpp_json_writer w;
    for (size_t i = 0; i < uvcpp_json_writer::k_max_depth; ++i) {
      must_ok(w.array_begin(), "5.2 开第 " + std::to_string(i + 1) + " 层");
    }
    for (size_t i = 0; i < uvcpp_json_writer::k_max_depth; ++i) {
      must_ok(w.array_end(), "5.2 关第 " + std::to_string(i + 1) + " 层");
    }
    must_ok(w.finish(), "5.2 64 层收尾");
    check(w.str() == std::string(uvcpp_json_writer::k_max_depth, '[') +
                        std::string(uvcpp_json_writer::k_max_depth, ']'),
          "5.2 64 层的产出形状");
  }
  // ★ `max_depth` 超过编译期上限时**被夹到 64**，而不是报错、也不是偷偷多给。
  //   判据是"第 65 层仍然 TOO_DEEP" —— 夹没夹在这里能看出来。
  {
    std::string body;
    uvcpp_json_writer w(body, json_write_options(1000, 8u << 20));
    for (size_t i = 0; i < uvcpp_json_writer::k_max_depth; ++i) {
      must_ok(w.array_begin(), "5.3 夹取后仍能开 64 层");
    }
    check_st(w.array_begin(), json_write_status::TOO_DEEP,
             "5.3 max_depth=1000 被夹到 64（第 65 层仍 TOO_DEEP）");
  }
  // `max_depth = 0`：容器一律 TOO_DEEP，但**标量根值**仍然可以（它不占栈）。
  {
    uvcpp_json_writer w(json_write_options(0, 8u << 20));
    must_ok(w.value(1), "5.4 max_depth=0 下标量根值");
    must_ok(w.finish(), "5.4 finish");
    check_eq_s(w.str(), "1", "5.4 max_depth=0 的标量根值");
  }
  {
    uvcpp_json_writer w(json_write_options(0, 8u << 20));
    check_st(w.object_begin(), json_write_status::TOO_DEEP,
             "5.5 max_depth=0 下容器应当 TOO_DEEP");
  }
}

// =========================================================================
// 第 6 组：大小上限
// =========================================================================

void group_size_limit() {
  const std::string exact = "{\"a\":1}";  // 7 字节
  // 上限正好等于产出长度 ⇒ 通过（边界是"不超过"，不是"小于"）。
  {
    uvcpp_json_writer w(json_write_options(64, exact.size()));
    must_ok(w.object_begin(), "6.1 object_begin");
    must_ok(w.key("a"), "6.1 key");
    must_ok(w.value(1), "6.1 value");
    must_ok(w.object_end(), "6.1 最后那个 `}` 正好用满上限");
    must_ok(w.finish(), "6.1 finish");
    check_eq_s(w.str(), exact, "6.1 上限等于长度时应当通过");
    check_eq_u(w.written(), exact.size(), "6.1 written()");
  }
  // 上限少 1 ⇒ 最后那个 `}` 触界，报 TOO_LARGE。
  {
    uvcpp_json_writer w(json_write_options(64, exact.size() - 1));
    must_ok(w.object_begin(), "6.2 object_begin");
    must_ok(w.key("a"), "6.2 key");
    must_ok(w.value(1), "6.2 value");
    check_st(w.object_end(), json_write_status::TOO_LARGE, "6.2 触界应当 TOO_LARGE");
    check_eq_u(w.written(), exact.size() - 1, "6.2 触界时写下去的长度");
    check_eq_u(w.str().size(), exact.size() - 1, "6.2 触界时的缓冲长度");
    check_st(w.value(0), json_write_status::TOO_LARGE, "6.2 粘性：后续仍报错");
  }
  // 字符串值走的是另一条插入路径，受同一把尺子管；而且**先判后写**：触界那一
  // 下不许把半个值留在缓冲里。
  {
    uvcpp_json_writer w(json_write_options(64, 8));
    must_ok(w.array_begin(), "6.3 array_begin（1 字节）");
    check_st(w.value("0123456"), json_write_status::TOO_LARGE,
             "6.3 字符串值触界（1 + 1 引号 + 7 = 9 > 8）");
    // 已经写下去的是 `[` 和那个开引号 —— 值是**整段**被拒的，不是写了 6 个字节
    // 才发现不够。
    check_eq_s(w.str(), "[\"", "6.3 触界时缓冲里只有 `[` 与开引号");
    check_eq_u(w.written(), 2, "6.3 触界时的 written()");
  }
  // 键也受同一把尺子管。
  {
    uvcpp_json_writer w(json_write_options(64, 4));
    must_ok(w.object_begin(), "6.4 object_begin（1 字节）");
    check_st(w.key("abcdef"), json_write_status::TOO_LARGE,
             "6.4 键触界（1 + 8 = 9 > 4）");
  }
}

// =========================================================================
// 第 7 组：直写调用方缓冲
// =========================================================================

void group_external_buffer() {
  const std::string prefix = "PREFIX:";
  {
    std::string body = prefix;
    uvcpp_json_writer w(body);
    must_ok(w.object_begin(), "7.1 object_begin");
    must_ok(w.key("k"), "7.1 key");
    must_ok(w.value(std::string("v")), "7.1 value");
    must_ok(w.object_end(), "7.1 object_end");
    must_ok(w.finish(), "7.1 finish");

    check_eq_s(w.str(), prefix + "{\"k\":\"v\"}", "7.1 产出追加在前缀之后");
    check(w.str() == body, "7.1 str() 就是调用方那个串");
    check_eq_u(w.size(), prefix.size() + 9, "7.1 size() 是整串长度");
    // ★ `written()` 只算**自己**写的那一段 —— 上限比的也是它，所以前缀不会
    // 把额度吃掉。
    check_eq_u(w.written(), 9, "7.1 written() 不含前缀");
  }
  // `clear()` 截回**构造时**的长度，而不是清空整串。
  {
    std::string body = prefix;
    uvcpp_json_writer w(body);
    must_ok(w.value(1), "7.2 value");
    must_ok(w.finish(), "7.2 finish");
    check_eq_s(body, prefix + "1", "7.2 追加");
    w.clear();
    check_eq_s(body, prefix, "7.2 clear() 截回构造时的长度（不许吃掉前缀）");
    check_eq_u(w.written(), 0, "7.2 clear() 之后 written() 归零");
    check_eq_u(w.depth(), 0, "7.2 clear() 之后 depth() 归零");
    check(w.ok(), "7.2 clear() 清掉错误状态");
    // 同一个 writer 复用：再写一份不同的 JSON。
    must_ok(w.array_begin(), "7.2 复用 array_begin");
    must_ok(w.value(7), "7.2 复用 value");
    must_ok(w.array_end(), "7.2 复用 array_end");
    must_ok(w.finish(), "7.2 复用 finish");
    check_eq_s(body, prefix + "[7]", "7.2 复用之后的产出");
  }
  // 上限只算自己写的字节：前缀比上限还长，也不该触界。
  {
    std::string body(1000, 'x');
    uvcpp_json_writer w(body, json_write_options(64, 7));
    must_ok(w.object_begin(), "7.3 object_begin");
    must_ok(w.key("a"), "7.3 key");
    must_ok(w.value(1), "7.3 value");
    must_ok(w.object_end(), "7.3 object_end");
    must_ok(w.finish(), "7.3 finish（上限 7 只比自己的 7 字节）");
    check_eq_u(w.written(), 7, "7.3 written()");
    check_eq_u(w.size(), 1007, "7.3 整串长度 = 前缀 + 产出");
  }
  // 自带缓冲与外部缓冲两条路必须产出**完全相同**的字节（走的是同一段实现，
  // 但缓冲的所有权不同、账不同，所以值得对一遍）。
  {
    std::string ext;
    uvcpp_json_writer we(ext);
    uvcpp_json_writer wi;
    for (int k = 0; k < 2; ++k) {
      uvcpp_json_writer& w = (k == 0) ? we : wi;
      must_ok(w.array_begin(), "7.4 array_begin");
      must_ok(w.value(std::string("a\"b")), "7.4 value");
      must_ok(w.value(0.25), "7.4 value 2");
      must_ok(w.array_end(), "7.4 array_end");
    }
    must_ok(we.finish(), "7.4 外部缓冲 finish");
    must_ok(wi.finish(), "7.4 自带缓冲 finish");
    check_eq_s(ext, wi.str(), "7.4 两条路的产出逐字节相同");
    check_eq_s(ext, "[\"a\\\"b\",0.25]", "7.4 两条路的产出都对");
  }
}

// =========================================================================
// 第 8 组：一份"像真的响应"的产出（逐字节）
// =========================================================================

void group_response_like() {
  // 形状取自 webapp 里最常见的那种响应：一个列表 + 分页元信息 + 一个可能含
  // 特殊字符的字段。期望串是手写的。
  uvcpp_json_writer w;
  must_ok(w.object_begin(), "8.1 object_begin");
  must_ok(w.member("ok", true), "8.1 ok");
  must_ok(w.key("total"), "8.1 key total");
  must_ok(w.value(2), "8.1 total");
  must_ok(w.key("items"), "8.1 key items");
  must_ok(w.array_begin(), "8.1 array_begin");
  for (int i = 0; i < 2; ++i) {
    must_ok(w.object_begin(), "8.1 item object_begin");
    must_ok(w.key("id"), "8.1 key id");
    must_ok(w.value(i + 1), "8.1 id");
    must_ok(w.key("name"), "8.1 key name");
    // 这一条值里同时有双引号、反斜杠和换行 —— 手写 json_str 最容易漏的就是它。
    must_ok(w.value(i == 0 ? "a\"b\\c" : "line1\nline2"), "8.1 name");
    must_ok(w.object_end(), "8.1 item object_end");
  }
  must_ok(w.array_end(), "8.1 array_end");
  must_ok(w.key("ratio"), "8.1 key ratio");
  must_ok(w.value(0.5), "8.1 ratio");
  must_ok(w.object_end(), "8.1 object_end");
  must_ok(w.finish(), "8.1 finish");

  const std::string want =
      "{\"ok\":true,\"total\":2,\"items\":["
      "{\"id\":1,\"name\":\"a\\\"b\\\\c\"},"
      "{\"id\":2,\"name\":\"line1\\nline2\"}],"
      "\"ratio\":0.5}";
  check_eq_s(w.str(), want, "8.1 一份完整响应的逐字节产出");
}

}  // namespace

int main() {
  group_structure();
  group_escape();
  group_numbers();
  group_misuse();
  group_depth();
  group_size_limit();
  group_external_buffer();
  group_response_like();

  if (g_failures == 0) {
    std::cout << "[json_writer] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[json_writer] FAIL (" << g_failures << ")" << std::endl;
  return 2;
}
