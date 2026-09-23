/**
 * @file tests/functional/web_app_json_reflect_func.cpp
 * @brief JSON 反射**读入侧**（`uvcpp_from_json`）与请求入口
 *        （`uvcpp_web_request::json(结构体, ...)`）的功能测试。
 *
 * 为什么这一半单独立一个文件
 * --------------------------
 * 写出侧（`uvcpp/uvcpp_json_reflect.h`）在核心模块、零依赖，用例在
 * `tests/functional/json_reflect_func.cpp`。读入侧要用 DOM，而本仓的 DOM 就是
 * `uvcpp_json`（nlohmann），整个挂在 **webapp** 模块上、默认 **OFF**。两半的
 * 编译开关不同，所以必须是两个文件：这一个带 `web_app_` 前缀，关掉 webapp 时
 * 会被 `tests/functional/CMakeLists.txt` 的过滤器摘掉（那个文件里记着"注册了、
 * 通过了、一个断言都没跑"的老坑，所以前缀必须与它所属的开关一一对上）。
 *
 * 这一层只有四种坏法，断言就分四族
 * --------------------------------
 *   1. **没给的字段被写成了别的值**。文档承诺的是"缺字段与 `null` 都算没给，
 *      **不动**目标里的原值"，所以每条用例先给目标灌**哨兵**再读 —— 只判
 *      "读出来等于期望值"的话，"缺失被当成零值填进去"这种实现照样绿。
 *   2. **隐式转换**。`"age":"30"` 必须报错而不是被解析成 30；`true` 读进整型
 *      必须报错；整数读进整数要**逐宽度查上下界**（`70000` 进 `short` 报错，
 *      不是静默截成 `4464`）。这几条都只能靠"值域边界的两侧各来一发"来钉。
 *   3. **多出来的成员**默认忽略、开了 `unknown_is_error` 才报。两条都要判，
 *      否则"永远忽略"与"永远报错"各有一半是绿的。
 *   4. **`where` 指向最深那一层**。这是失败路径上唯一给用户的定位信息，
 *      嵌套结构体/vector 里出错时必须给**里层**的字段名，不是外层那个。
 *
 * 独立见证
 * --------
 * 第 1 组的往返用任务 5 的 writer 产出、再读回来；但**期望串是手写的**，
 * 其余各组的输入也全是手写的 JSON 字面量 —— 不拿 writer 的输出当自己的判据。
 *
 * 两处诚实边界
 * ------------
 *   - **失败时目标可能被改了一部分**（文档明写）。这不是缺陷，但很容易被
 *     当成缺陷"修掉"，所以这里**为它写了断言**（3.4 / 5.2 / 6.3）：前几个
 *     字段已经写进去、后一个失败。哪天有人改成"全成功或全不动"，这几条会红,
 *     那时应当**改文档**而不是改回实现 —— 先判清楚是哪种。
 *   - **`const char*` 字段读不进来是编译期错误**（`static_assert`），没法在
 *     一个会跑的用例里判。这里只由文档承担，不为它编运行时断言。
 *
 * 命名必须同时命中两道过滤器
 * --------------------------
 * 这一条用 webapp 的 `uvcpp_json`，所以文件名要命中 `web_app_*`。**故意不加
 * `web_ssl_` / `h2_`**：本用例与 TLS、HTTP/2 都无关，多带一个前缀就会在那些
 * 开关关掉时被摘掉，而它是"webapp 关掉才该被摘掉"的那一类。
 */
#include <iostream>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_json_reflect.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_request.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_i(long long got, long long want, const char* what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_u(size_t got, size_t want, const char* what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_s(const std::string& got, const std::string& want, const char* what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: [" << want
              << "]\n         实际: [" << got << "]" << std::endl;
    ++g_failures;
  }
}

void check_st(json_status got, json_status want, const char* what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: "
              << json_status_name(want) << "\n         实际: " << json_status_name(got)
              << std::endl;
    ++g_failures;
  }
}

/// 判 where 指向的名字。`want == nullptr` 表示"没记位置"。
void check_where(const char* got, const char* want, const char* what) {
  const std::string g = (got == nullptr) ? std::string("<null>") : std::string(got);
  const std::string w = (want == nullptr) ? std::string("<null>") : std::string(want);
  if (g != w) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << w
              << "\n         实际: " << g << std::endl;
    ++g_failures;
  }
}

// =========================================================================
// 结构体（字段表用宏标出；顺序即宏里列出的顺序）
// =========================================================================

struct point {
  int x;
  int y;
  UVCPP_JSON_FIELDS(point, x, y)
};

struct scalars {
  bool flag;
  signed char i8;
  short i16;
  int i32;
  long long i64;
  unsigned char u8;
  unsigned short u16;
  unsigned int u32;
  unsigned long long u64;
  float f32;
  double f64;
  std::string text;
  UVCPP_JSON_FIELDS(scalars, flag, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, text)
};

struct pair_s {
  int a;
  int b;
  UVCPP_JSON_FIELDS(pair_s, a, b)
};

struct profile {
  std::string name;
  int age;
  double score;
  bool active;
  std::vector<std::string> tags;
  point pos;
  UVCPP_JSON_FIELDS(profile, name, age, score, active, tags, pos)
};

struct with_vec {
  std::vector<int> xs;
  UVCPP_JSON_FIELDS(with_vec, xs)
};

struct with_strs {
  std::vector<std::string> names;
  UVCPP_JSON_FIELDS(with_strs, names)
};

struct with_pts {
  std::vector<point> pts;
  UVCPP_JSON_FIELDS(with_pts, pts)
};

struct with_grid {
  std::vector<std::vector<int> > grid;
  UVCPP_JSON_FIELDS(with_grid, grid)
};

struct one_int {
  int v;
  UVCPP_JSON_FIELDS(one_int, v)
};
struct one_short {
  short v;
  UVCPP_JSON_FIELDS(one_short, v)
};
struct one_uchar {
  unsigned char v;
  UVCPP_JSON_FIELDS(one_uchar, v)
};
struct one_uint {
  unsigned int v;
  UVCPP_JSON_FIELDS(one_uint, v)
};
struct one_ll {
  long long v;
  UVCPP_JSON_FIELDS(one_ll, v)
};
struct one_ull {
  unsigned long long v;
  UVCPP_JSON_FIELDS(one_ull, v)
};
struct one_double {
  double v;
  UVCPP_JSON_FIELDS(one_double, v)
};
struct one_float {
  float v;
  UVCPP_JSON_FIELDS(one_float, v)
};
struct one_bool {
  bool v;
  UVCPP_JSON_FIELDS(one_bool, v)
};
struct one_str {
  std::string v;
  UVCPP_JSON_FIELDS(one_str, v)
};

// =========================================================================
// 辅助
// =========================================================================

/// 解析 + 读入，一步到位。解析失败时**读入那一步根本没跑**（`out` 不动）。
template <typename T>
json_status read_json(const std::string& text, T& out,
                      const uvcpp_from_json_options& opts = uvcpp_from_json_options(),
                      const char** where = nullptr) {
  uvcpp_json dom;
  const json_status st = uvcpp_json_parse(text, dom);
  if (st != json_status::OK) return st;
  return uvcpp_from_json(dom, out, opts, where);
}

// =========================================================================
// 1. 往返：writer 写出 → 解析 → 读回
// =========================================================================
void group_roundtrip() {
  profile p;
  p.name = "zhang";
  p.age = 30;
  p.score = 0.1;
  p.active = true;
  p.tags.push_back("a");
  p.tags.push_back("b");
  p.pos.x = -7;
  p.pos.y = 42;

  uvcpp_json_writer w(json_write_options(64, 8192));
  const json_write_status wst = uvcpp_to_json(p, w);
  check(wst == json_write_status::OK, "1.1 写出成功");
  check(w.finish() == json_write_status::OK, "1.2 finish 成功");

  // 期望串**手写**（不拿 writer 的输出当自己的判据）。
  const std::string want =
      "{\"name\":\"zhang\",\"age\":30,\"score\":0.1,\"active\":true,"
      "\"tags\":[\"a\",\"b\"],\"pos\":{\"x\":-7,\"y\":42}}";
  check_s(w.str(), want, "1.3 产出逐字节等于手写期望");

  // 读回来：先灌哨兵，一个字段没被覆盖就能看出来。
  profile q;
  q.name = "SENTINEL";
  q.age = -1;
  q.score = -1.0;
  q.active = false;
  q.tags.push_back("SENTINEL");
  q.pos.x = 101;
  q.pos.y = 102;

  // 故意先塞个残留值：成功时 where 必须被清成 nullptr，否则调用方会把上一次
  // 失败的字段名当成这一次的（这是个"只在第二次调用才现身"的坑）。
  const char* where = "leftover";
  check_st(read_json(w.str(), q, uvcpp_from_json_options(), &where), json_status::OK,
           "1.4 读回成功");
  check_where(where, nullptr, "1.5 成功时 where 被清成 nullptr（上一轮的残留不留下）");

  check_s(q.name, p.name, "1.6 name 往返一致");
  check_i(q.age, p.age, "1.7 age 往返一致");
  check(q.score == p.score, "1.8 score 往返一致");
  check(q.active == p.active, "1.9 active 往返一致");
  check_u(q.tags.size(), 2, "1.10 tags 元素个数一致");
  if (q.tags.size() == 2) {
    check_s(q.tags[0], "a", "1.11 tags[0]");
    check_s(q.tags[1], "b", "1.12 tags[1]");
  }
  check_i(q.pos.x, -7, "1.13 嵌套结构体 x");
  check_i(q.pos.y, 42, "1.14 嵌套结构体 y");
}

// =========================================================================
// 2. 每个类别都要读对（含内嵌 NUL 与非 ASCII）
// =========================================================================
void group_kinds() {
  const std::string json =
      "{"
      "\"flag\":true,"
      "\"i8\":-128,"
      "\"i16\":-32768,"
      "\"i32\":-2147483647,"
      "\"i64\":-9223372036854775807,"
      "\"u8\":255,"
      "\"u16\":65535,"
      "\"u32\":4294967295,"
      "\"u64\":18446744073709551615,"
      "\"f32\":2.5,"
      "\"f64\":0.1,"
      "\"text\":\"a\\u0000b\""
      "}";

  scalars s;
  s.flag = false;
  s.i8 = 1;
  s.i16 = 2;
  s.i32 = 3;
  s.i64 = 4;
  s.u8 = 5;
  s.u16 = 6;
  s.u32 = 7;
  s.u64 = 8;
  s.f32 = 9.0f;
  s.f64 = 10.0;
  s.text = "SENTINEL";

  check_st(read_json(json, s), json_status::OK, "2.1 十二个字段全部读入");

  check(s.flag, "2.2 bool 覆盖了哨兵");
  check_i(s.i8, -128, "2.3 signed char 下界");
  check_i(s.i16, -32768, "2.4 short 下界");
  check_i(s.i32, -2147483647, "2.5 int");
  check_i(s.i64, -9223372036854775807LL, "2.6 long long");
  check_i(s.u8, 255, "2.7 unsigned char 上界");
  check_i(s.u16, 65535, "2.8 unsigned short 上界");
  check_u(s.u32, 4294967295u, "2.9 unsigned int 上界");
  check_u(s.u64, 18446744073709551615ull, "2.10 unsigned long long 上界");
  check(s.f32 == 2.5f, "2.11 float");
  check(s.f64 == 0.1, "2.12 double");
  // 字符串是二进制安全的：`\u0000` 解出来是一个真的 0 字节。
  const std::string want_text("a\0b", 3);
  check_u(s.text.size(), 3, "2.13 内嵌 NUL 的字符串长度是 3");
  check_s(s.text, want_text, "2.14 内嵌 NUL 原样保留");

  // 转义：`\/` 是 JSON 允许的转义，解出来就是 `/`；`\n` 是换行字节。
  one_str es;
  check_st(read_json("{\"v\":\"a\\/b\\nc\\\"d\\\\e\"}", es), json_status::OK, "2.15 转义读入成功");
  check_s(es.v, "a/b\nc\"d\\e", "2.16 转义逐字节正确");

  // 非 ASCII：用 `\uXXXX` 写，期望用**显式字节**比（不依赖源码编码）。
  // U+4E2D U+6587 = "中文" = e4 b8 ad e6 96 87
  one_str us;
  check_st(read_json("{\"v\":\"\\u4e2d\\u6587\"}", us), json_status::OK, "2.17 \\u 转义读入成功");
  check_s(us.v, std::string("\xe4\xb8\xad\xe6\x96\x87"), "2.18 UTF-8 字节正确");
}

// =========================================================================
// 3. 值域：逐宽度查上下界
// =========================================================================
void group_range() {
  const uvcpp_from_json_options def;

  one_uchar c;
  c.v = 7;
  check_st(read_json("{\"v\":255}", c, def), json_status::OK, "3.1 unsigned char 收 255");
  check_i(c.v, 255, "3.2 值正确");
  c.v = 7;
  check_st(read_json("{\"v\":256}", c, def), json_status::MISMATCH, "3.3 unsigned char 拒 256");
  check_i(c.v, 7, "3.4 报错时那个字段保持哨兵（没被截成 0）");
  check_st(read_json("{\"v\":-1}", c, def), json_status::MISMATCH, "3.5 unsigned char 拒 -1");

  one_short sh;
  sh.v = 7;
  check_st(read_json("{\"v\":32767}", sh, def), json_status::OK, "3.6 short 收 32767");
  check_i(sh.v, 32767, "3.7 值正确");
  // ★ 这一条是本组的主角：截断的实现会写进 4464 并返 OK。
  check_st(read_json("{\"v\":70000}", sh, def), json_status::MISMATCH, "3.8 short 拒 70000");
  check_i(sh.v, 32767, "3.9 70000 被拒之后 v 还是上一轮的值");
  check_st(read_json("{\"v\":-32769}", sh, def), json_status::MISMATCH, "3.10 short 拒 -32769");

  one_uint ui;
  ui.v = 7;
  check_st(read_json("{\"v\":4294967295}", ui, def), json_status::OK, "3.11 unsigned int 收上界");
  check_st(read_json("{\"v\":-1}", ui, def), json_status::MISMATCH, "3.12 unsigned int 拒 -1");
  check_st(read_json("{\"v\":4294967296}", ui, def), json_status::MISMATCH, "3.13 unsigned int 拒上界+1");

  // 同一个 JSON 数字，读进有符号与无符号两种目标：两侧都必须自己判边界。
  one_ll ll;
  ll.v = 7;
  check_st(read_json("{\"v\":18446744073709551615}", ll, def), json_status::MISMATCH,
           "3.14 long long 拒 ull 上界");
  check_i(ll.v, 7, "3.15 被拒之后保持哨兵");
  one_ull ull;
  ull.v = 7;
  check_st(read_json("{\"v\":18446744073709551615}", ull, def), json_status::OK,
           "3.16 unsigned long long 收同一个数");
  // ★ 负的有符号数进无符号目标：这一条**只有 64 位那档判得出来**。
  // 对窄目标（unsigned char / short / int），"先查 s<0"与"再把 s 转成无符号比上界"
  // 是**重合**的 —— 少了前者，后者照样把 -1 当成天文数字挡下来。可 64 位那档的
  // 上界就是 ULLONG_MAX，`(unsigned long long)(-1)` 恰好等于它、**不大于它**，
  // 于是少一行检查就会把 -1 静默写成 18446744073709551615。这一条是变异台
  // 拿 R6 试出来的缺口，不是照着实现补的。
  ull.v = 7;
  check_st(read_json("{\"v\":-1}", ull, def), json_status::MISMATCH,
           "3.16b unsigned long long 拒 -1（不是写成 ULLONG_MAX）");
  check_u(ull.v, 7, "3.16c 被拒之后保持哨兵");

  one_int i32;
  i32.v = 7;
  check_st(read_json("{\"v\":2147483648}", i32, def), json_status::MISMATCH, "3.17 int 拒 2^31");
  check_st(read_json("{\"v\":-2147483648}", i32, def), json_status::OK, "3.18 int 收 -2^31");
  check_i(i32.v, -2147483648LL, "3.19 值正确");

  // 唯一放宽的一条：整数可以进浮点（JSON 本来就不分 int/double）。
  one_double d;
  d.v = -1.0;
  check_st(read_json("{\"v\":1}", d, def), json_status::OK, "3.20 整数进 double 收");
  check(d.v == 1.0, "3.21 值是 1.0");
  one_float f;
  f.v = -1.0f;
  check_st(read_json("{\"v\":3}", f, def), json_status::OK, "3.22 整数进 float 收");
  check(f.v == 3.0f, "3.23 值是 3.0f");
  // 但浮点进整型**不收**（那才是需要隐式截断的方向）。
  one_int iv;
  iv.v = 7;
  check_st(read_json("{\"v\":1.5}", iv, def), json_status::MISMATCH, "3.24 浮点进 int 不收");
  check_i(iv.v, 7, "3.25 保持哨兵");
}

// =========================================================================
// 4. 类型不符一律报错，不做隐式转换
// =========================================================================
void group_mismatch() {
  const uvcpp_from_json_options def;

  // 根不是对象：三种都不是。
  one_int root;
  root.v = 7;
  check_st(read_json("[]", root, def), json_status::MISMATCH, "4.1 根是数组 ⇒ MISMATCH");
  check_st(read_json("1", root, def), json_status::MISMATCH, "4.2 根是数字 ⇒ MISMATCH");
  check_st(read_json("null", root, def), json_status::MISMATCH, "4.3 根是 null ⇒ MISMATCH");
  check_st(read_json("\"x\"", root, def), json_status::MISMATCH, "4.4 根是字符串 ⇒ MISMATCH");
  check_i(root.v, 7, "4.5 根不是对象时一个字段都没动");

  // 值是字符串 ⇒ 不解析成数字。
  const char* where = nullptr;
  one_int si;
  si.v = 7;
  check_st(read_json("{\"v\":\"30\"}", si, def, &where), json_status::MISMATCH,
           "4.6 \"30\" 不读进 int");
  check_i(si.v, 7, "4.7 保持哨兵");
  check_where(where, "v", "4.8 where 给出字段名");

  // 字符串字段 ← 数字 ⇒ 不收。
  one_str ss;
  ss.v = "SENTINEL";
  check_st(read_json("{\"v\":30}", ss, def), json_status::MISMATCH, "4.9 数字不读进 string");
  check_s(ss.v, "SENTINEL", "4.10 保持哨兵");

  // bool 与数字**互不转换**（两个方向都判）。
  one_int bi;
  bi.v = 7;
  check_st(read_json("{\"v\":true}", bi, def), json_status::MISMATCH, "4.11 true 不读进 int");
  check_i(bi.v, 7, "4.12 保持哨兵");
  one_bool bb;
  bb.v = false;
  check_st(read_json("{\"v\":1}", bb, def), json_status::MISMATCH, "4.13 1 不读进 bool");
  check(!bb.v, "4.14 保持哨兵");
  check_st(read_json("{\"v\":false}", bb, def), json_status::OK, "4.15 false 能读进 bool");

  // 数组 / 对象进标量。
  check_st(read_json("{\"v\":[1]}", bi, def), json_status::MISMATCH, "4.16 数组不读进 int");
  check_st(read_json("{\"v\":{}}", bi, def), json_status::MISMATCH, "4.17 对象不读进 int");
  // 标量进嵌套结构体、数组进 vector：都是两个方向。
  profile pf;
  pf.name = "SENTINEL";
  check_st(read_json("{\"name\":\"n\",\"pos\":5}", pf, def), json_status::MISMATCH,
           "4.18 标量不读进嵌套结构体");
  check_s(pf.name, "n", "4.19 它前面那个字段已经被写了（见文件头的诚实边界）");
  check_i(pf.pos.x, 0, "4.20 嵌套结构体没被改");
  with_vec wv;
  wv.xs.push_back(9);
  check_st(read_json("{\"xs\":5}", wv, def), json_status::MISMATCH, "4.21 标量不读进 vector");
  check_u(wv.xs.size(), 1, "4.22 不是数组时 vector 连清空都没做（保持原样）");
  check_st(read_json("{\"xs\":{}}", wv, def), json_status::MISMATCH, "4.23 对象不读进 vector");
}

// =========================================================================
// 5. 缺失与 null：默认"没给"，严格模式下报 MISSING
// =========================================================================
void group_missing() {
  const uvcpp_from_json_options def;
  const uvcpp_from_json_options strict(true);

  // 默认：缺字段不动原值。
  pair_s p;
  p.a = -1;
  p.b = -2;
  check_st(read_json("{\"b\":20}", p, def), json_status::OK, "5.1 缺 a 也能成功");
  check_i(p.a, -1, "5.2 a 保持原值（不是被填成 0）");
  check_i(p.b, 20, "5.3 b 被覆盖");

  p.a = -1;
  p.b = -2;
  check_st(read_json("{\"a\":null,\"b\":20}", p, def), json_status::OK, "5.4 显式 null 也算没给");
  check_i(p.a, -1, "5.5 null 不动原值（不是填 0）");
  check_i(p.b, 20, "5.6 其他字段照读");

  // 严格：缺字段报 MISSING 并给出名字。
  const char* where = nullptr;
  p.a = -1;
  p.b = -2;
  check_st(read_json("{}", p, strict, &where), json_status::MISSING, "5.7 严格模式缺字段报 MISSING");
  check_where(where, "a", "5.8 where 是**第一个**缺的字段（按宏里的字段顺序）");
  check_i(p.a, -1, "5.9 缺字段时原值没动");
  check_i(p.b, -2, "5.10 缺字段时原值没动（b）");

  // 部分成功：a 给了、b 没给 ⇒ 报 MISSING，但 a 已经写进去了。
  p.a = -1;
  p.b = -2;
  where = nullptr;
  check_st(read_json("{\"a\":1}", p, strict, &where), json_status::MISSING, "5.11 只给 a ⇒ MISSING");
  check_where(where, "b", "5.12 where 指出缺的是 b");
  check_i(p.a, 1, "5.13 a 已经写进去了（失败**不是**原子回滚，见文件头）");
  check_i(p.b, -2, "5.14 b 保持原值");

  // 严格 + 全给 ⇒ OK。
  p.a = -1;
  p.b = -2;
  check_st(read_json("{\"a\":1,\"b\":2}", p, strict), json_status::OK, "5.15 全给 ⇒ OK");
  check_i(p.a, 1, "5.16 a");
  check_i(p.b, 2, "5.17 b");

  // 严格 + null ⇒ 与"缺"同一条路。
  p.a = -1;
  p.b = -2;
  where = nullptr;
  check_st(read_json("{\"a\":null,\"b\":2}", p, strict, &where), json_status::MISSING,
           "5.18 严格模式下 null 也算缺");
  check_where(where, "a", "5.19 where 是 a");
  // 粘性：第一个字段就报了 MISSING ⇒ 字段展开**当场停**，`b` 根本没被读。
  // （不是"null 跳过 a、继续读 b"—— 那样 b 会变成 2，这条就是专门钉它的。）
  check_i(p.b, -2, "5.20 报错之后后面的字段一个都不读（错一次就停）");
}

// =========================================================================
// 6. 多出来的成员：默认忽略，开了才报
// =========================================================================
void group_unknown() {
  const uvcpp_from_json_options def;
  const uvcpp_from_json_options strict(false, true);

  pair_s p;
  p.a = -1;
  p.b = -2;
  check_st(read_json("{\"a\":1,\"b\":2,\"c\":3}", p, def), json_status::OK,
           "6.1 多一个成员默认不报错");
  check_i(p.a, 1, "6.2 已知字段照读");
  check_i(p.b, 2, "6.3 已知字段照读（b）");

  const char* where = nullptr;
  p.a = -1;
  p.b = -2;
  check_st(read_json("{\"a\":1,\"b\":2,\"c\":3}", p, strict, &where), json_status::UNKNOWN,
           "6.4 开了 unknown_is_error 才报 UNKNOWN");
  check_where(where, "c", "6.5 where 指出是哪个成员多出来了");
  check_i(p.a, 1, "6.6 字段那一步先跑完了（部分写入）");

  p.a = -1;
  p.b = -2;
  check_st(read_json("{\"a\":1,\"b\":2}", p, strict), json_status::OK, "6.7 成员正好齐时不报");

  // 嵌套结构体里的多成员：where 必须给**里层**的字段名。
  profile pf;
  pf.name = "SENTINEL";
  where = nullptr;
  check_st(read_json("{\"name\":\"n\",\"pos\":{\"x\":1,\"y\":2,\"z\":3}}", pf, strict, &where),
           json_status::UNKNOWN, "6.8 嵌套里的多成员也报 UNKNOWN");
  check_where(where, "z", "6.9 where 指里层的 z（不是外层的 pos）");

  // vector 元素里的多成员。
  with_pts wp;
  where = nullptr;
  check_st(read_json("{\"pts\":[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4,\"z\":5}]}", wp, strict, &where),
           json_status::UNKNOWN, "6.10 vector 元素里的多成员也报 UNKNOWN");
  check_where(where, "z", "6.11 where 指 z");
  check_u(wp.pts.size(), 1, "6.12 第一个元素已经进去了，第二个失败（部分写入）");
}

// =========================================================================
// 7. 容器
// =========================================================================
void group_containers() {
  const uvcpp_from_json_options def;

  with_vec v;
  check_st(read_json("{\"xs\":[1,2,3]}", v, def), json_status::OK, "7.1 读三个元素");
  check_u(v.xs.size(), 3, "7.2 个数");
  if (v.xs.size() == 3) {
    check_i(v.xs[0], 1, "7.3 [0]");
    check_i(v.xs[1], 2, "7.4 [1]");
    check_i(v.xs[2], 3, "7.5 [2]");
  }

  // 空数组必须**清掉**原有元素（不清的话就是三倍数据的静默错）。
  with_vec e;
  e.xs.push_back(9);
  e.xs.push_back(9);
  e.xs.push_back(9);
  check_st(read_json("{\"xs\":[]}", e, def), json_status::OK, "7.6 空数组");
  check_u(e.xs.size(), 0, "7.7 空数组把原有的元素清掉了");

  // 重读短的：必须清后重建，不是"覆盖前 n 个"。
  with_vec r;
  r.xs.push_back(9);
  r.xs.push_back(9);
  r.xs.push_back(9);
  check_st(read_json("{\"xs\":[7]}", r, def), json_status::OK, "7.8 重读一个元素");
  check_u(r.xs.size(), 1, "7.9 只剩一个（不是 3 个都还在）");
  check_i(r.xs[0], 7, "7.10 值");

  // 中途失败：前面的元素已经在里面了。
  with_vec m;
  check_st(read_json("{\"xs\":[1,\"x\",3]}", m, def), json_status::MISMATCH, "7.11 中途一个类型不对");
  check_u(m.xs.size(), 1, "7.12 失败前那个元素已经进去了（部分写入）");

  // 字符串与嵌套 vector。
  with_strs s;
  check_st(read_json("{\"names\":[\"a\",\"\"]}", s, def), json_status::OK, "7.13 字符串数组");
  check_u(s.names.size(), 2, "7.14 个数");
  check_s(s.names[0], "a", "7.15 [0]");
  check_s(s.names[1], "", "7.16 空串原样");

  with_grid g;
  check_st(read_json("{\"grid\":[[1,2],[3]]}", g, def), json_status::OK, "7.17 嵌套数组");
  check_u(g.grid.size(), 2, "7.18 行数");
  if (g.grid.size() == 2) {
    check_u(g.grid[0].size(), 2, "7.19 第 0 行");
    check_u(g.grid[1].size(), 1, "7.20 第 1 行");
    check_i(g.grid[0][1], 2, "7.21 值");
  }
  with_grid g2;
  check_st(read_json("{\"grid\":[[1],2]}", g2, def), json_status::MISMATCH, "7.22 行不是数组 ⇒ 报错");

  // 结构体数组。
  with_pts p;
  check_st(read_json("{\"pts\":[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4}]}", p, def), json_status::OK,
           "7.23 结构体数组");
  check_u(p.pts.size(), 2, "7.24 元素个数");
  if (p.pts.size() == 2) {
    check_i(p.pts[1].x, 3, "7.25 第二个元素的 x");
    check_i(p.pts[1].y, 4, "7.26 第二个元素的 y");
  }
}

// =========================================================================
// 8. 状态名：四个新状态都在，且都有名字
// =========================================================================
void group_status_names() {
  check_s(json_status_name(json_status::OK), "OK", "8.1 OK");
  check_s(json_status_name(json_status::EMPTY), "EMPTY", "8.2 EMPTY");
  check_s(json_status_name(json_status::SYNTAX), "SYNTAX", "8.3 SYNTAX");
  check_s(json_status_name(json_status::TOO_DEEP), "TOO_DEEP", "8.4 TOO_DEEP");
  check_s(json_status_name(json_status::TOO_LARGE), "TOO_LARGE", "8.5 TOO_LARGE");
  // 读入侧那四个：漏一个 case 就会拿到 "?"，而那种"能跑但没名字"最容易过关。
  check_s(json_status_name(json_status::MISMATCH), "MISMATCH", "8.6 MISMATCH");
  check_s(json_status_name(json_status::MISSING), "MISSING", "8.7 MISSING");
  check_s(json_status_name(json_status::UNKNOWN), "UNKNOWN", "8.8 UNKNOWN");
  check_s(json_status_name(json_status::NO_MEMORY), "NO_MEMORY", "8.9 NO_MEMORY");
  check_s(json_status_name(static_cast<json_status>(9999)), "?", "8.10 表外的值给 ? 而不是崩");
}

// =========================================================================
// 9. 请求入口：req.json(结构体, &状态, &字段, 选项)
// =========================================================================

// 与 `web_app_request_func.cpp` 里那对辅助同形（那个文件是这一层的既有惯例）。
// 用输出参数而不是返回值：`uvcpp_web_request` 故意删了拷贝构造，而 C++11 的
// 返回值优化不是强制的 —— `return w;` 仍然需要一个可访问的拷贝/移动构造函数。
uvcpp_http_request make_http_req(http_method method, const std::string& url,
                                 const std::string& body = std::string()) {
  uvcpp_http_request r;
  r.method = method;
  r.url = url;
  r.version = uvcpp_http_version::HVER_11;
  if (!body.empty()) {
    r.body.clone_data(body.data(), body.size());
  }
  return r;
}

void make_web_req(uvcpp_web_request& w, http_method method, const std::string& url,
                  const std::string& body = std::string()) {
  uvcpp_http_request src = make_http_req(method, url, body);
  w.take_from(src);
}

void group_request() {
  const std::string url = "/api/user";

  // 1) 正常一条：解析 + 按字段表填进去，一步到位。
  {
    uvcpp_web_request w;
    make_web_req(w, http_method::HTTP_POST, url, "{\"name\":\"n\",\"age\":30}");
    profile u;
    u.name = "SENTINEL";
    u.age = -1;
    json_status st = json_status::EMPTY;
    const char* field = nullptr;
    const bool ok = w.json(u, &st, &field);
    check(ok, "9.1 合法报文返回 true");
    check_st(st, json_status::OK, "9.2 状态是 OK");
    check_where(field, nullptr, "9.3 成功时 field 是 nullptr");
    check_s(u.name, "n", "9.4 name 填进去了");
    check_i(u.age, 30, "9.5 age 填进去了");
  }

  // 2) 字段不符：返回 false，状态与字段名都要给对；**再成功一次后 field 要被清掉**。
  {
    uvcpp_web_request w;
    make_web_req(w, http_method::HTTP_POST, url, "{\"name\":\"n\",\"age\":\"30\"}");
    profile u;
    u.age = -1;
    json_status st = json_status::OK;
    const char* field = nullptr;
    check(!w.json(u, &st, &field), "9.6 类型不符返回 false");
    check_st(st, json_status::MISMATCH, "9.7 状态是 MISMATCH");
    check_where(field, "age", "9.8 field 指出是 age");
    check_s(u.name, "n", "9.9 name 已经填进去了（部分写入，见文件头）");

    // 同一个请求再读一次，这回换成合法报文：field 必须被清空。
    uvcpp_web_request w2;
    make_web_req(w2, http_method::HTTP_POST, url, "{\"name\":\"n\",\"age\":30}");
    profile u2;
    check(w2.json(u2, &st, &field), "9.10 合法报文成功");
    check_where(field, nullptr, "9.11 上一轮的 field 残留被清掉了");
  }

  // 3) 报文本身坏：失败发生在解析那一步，**目标一个字段都没动**。
  {
    uvcpp_web_request w;
    make_web_req(w, http_method::HTTP_POST, url, "not json at all");
    profile u;
    u.name = "SENTINEL";
    u.age = -1;
    json_status st = json_status::OK;
    check(!w.json(u, &st), "9.12 非 JSON 返回 false");
    check_st(st, json_status::SYNTAX, "9.13 状态是 SYNTAX");
    check_s(u.name, "SENTINEL", "9.14 解析失败时目标没被动过");
  }

  // 4) 空 body ⇒ EMPTY（不是 SYNTAX：调用方要能区分 400 与 415）。
  {
    uvcpp_web_request w;
    make_web_req(w, http_method::HTTP_POST, url);
    profile u;
    json_status st = json_status::OK;
    check(!w.json(u, &st), "9.15 空 body 返回 false");
    check_st(st, json_status::EMPTY, "9.16 状态是 EMPTY");
  }

  // 5) 根不是对象：状态是 MISMATCH，且 field 保持空（一个字段都没进到）。
  {
    uvcpp_web_request w;
    make_web_req(w, http_method::HTTP_POST, url, "[1,2]");
    profile u;
    json_status st = json_status::OK;
    const char* field = reinterpret_cast<const char*>(1);  // 故意塞个脏值
    check(!w.json(u, &st, &field), "9.17 根是数组返回 false");
    check_st(st, json_status::MISMATCH, "9.18 状态是 MISMATCH");
    check_where(field, nullptr, "9.19 根不是对象时 field 被清成 nullptr");
  }

  // 6) 严格模式：缺字段 / 多成员各自的状态与字段名。
  {
    uvcpp_web_request w;
    make_web_req(w, http_method::HTTP_POST, url, "{\"name\":\"n\"}");
    profile u;
    json_status st = json_status::OK;
    const char* field = nullptr;
    check(!w.json(u, &st, &field, uvcpp_from_json_options(true)), "9.20 严格模式缺字段返回 false");
    check_st(st, json_status::MISSING, "9.21 状态是 MISSING");
    check_where(field, "age", "9.22 field 指出缺的是 age");

    uvcpp_web_request w2;
    make_web_req(w2, http_method::HTTP_POST, url, "{\"name\":\"n\",\"age\":30,\"nope\":1}");
    profile u2;
    st = json_status::OK;
    field = nullptr;
    check(!w2.json(u2, &st, &field, uvcpp_from_json_options(false, true)), "9.23 严格模式多成员返回 false");
    check_st(st, json_status::UNKNOWN, "9.24 状态是 UNKNOWN");
    check_where(field, "nope", "9.25 field 指出多出来的是 nope");
  }

  // 7) status/where 都能不传（默认实参这条路径也要走一遍）。
  {
    uvcpp_web_request w;
    make_web_req(w, http_method::HTTP_POST, url, "{\"name\":\"n\",\"age\":30}");
    profile u;
    check(w.json(u), "9.26 只传目标也能用");
    check_i(u.age, 30, "9.27 值正确");
  }
}

}  // namespace

int main() {
  group_roundtrip();
  group_kinds();
  group_range();
  group_mismatch();
  group_missing();
  group_unknown();
  group_containers();
  group_status_names();
  group_request();

  if (g_failures == 0) {
    std::cout << "[web_app_json_reflect] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[web_app_json_reflect] FAIL (" << g_failures << ")" << std::endl;
  return 2;
}
