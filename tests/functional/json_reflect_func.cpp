/**
 * @file tests/functional/json_reflect_func.cpp
 * @brief `uvcpp_json_reflect.h` 的功能测试 —— C++11 宏版反射的**序列化侧**。
 *
 * ## 这一条测的是什么
 *
 * 字段表这套东西只有三种坏法，所以断言也分三族：
 *
 *   1. **展开本身坏**（本模块唯一的技术活是手写的 `uvcpp_index_sequence`）：
 *      下标必须是**升序**的 `0..N-1`。降序也能编过、也能跑，但字段顺序就反了
 *      —— 所以第 1 组单测那个序列（把下标拼成字符串看一眼），第 2 组再判一次
 *      端到端的顺序。
 *   2. **顺序与内容**：产出**逐字节**比对，期望串是手写的。其中一条专门用
 *      "成员声明顺序与宏里列出的顺序**相反**"的结构体，把"顺序来自宏、不是
 *      来自声明"这件事钉死 —— 只判"产出里有没有这些字段"的话，两种实现都绿。
 *   3. **每个类别都要写对**：bool / 八种整型 / 浮点 / 字符串 / 嵌套结构体 /
 *      vector（含空 vector、嵌套 vector、vector 套结构体）。
 *
 * ## 诚实边界一：fold 的"短路"**不可观测**
 *
 * `uvcpp_write_fields` 里那条展开是"错一次就停"的。但**外面看不出来**：writer
 * 自己就是粘性的（第一次失败之后不再动缓冲），所以"停机"与"继续但被 writer
 * 挡住"产出**逐字节相同**、返回码也相同。这里能判的只有**返回码传得对不对**
 * （第 6 组），短路那一层是防御性的，没有独立见证 —— 不为它编一条判据。
 *
 * ## 诚实边界二：`float` 走的是 `float` 重载
 *
 * `ratio = 0.5f` 期望写出 `0.5`。若实现把它转成 `double` 再写，`%.17g` 会给出
 * `0.5`（这个值恰好短），所以这一条**判不出**那个错。真正能判的是 `0.1f`：
 * 转成 double 会写成 `0.10000000149011612`。所以那条断言用的是 `0.1f`。
 *
 * ## 命名
 *
 * 文件名不带 `web_` / `web_app_` / `web_ssl_` / `h2_` 任一前缀：本用例只用
 * **核心模块**（`uvcpp/uvcpp_json_reflect.h` + `uvcpp/uvcpp_json_writer.h`），
 * 与 web / webapp / OpenSSL / nghttp2 四个开关都无关。反过来，一旦改名成
 * `web_json_reflect_func.cpp`，"关掉 web 时它被摘掉"就会变成"这条用例从此不
 * 存在"—— 那是 `tests/functional/CMakeLists.txt` 里记着的老坑。
 */
#include <iostream>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_json_reflect.h>

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

void check_st(json_write_status got, json_write_status want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: "
              << json_write_status_name(want) << "\n         实际: "
              << json_write_status_name(got) << std::endl;
    ++g_failures;
  }
}

void must_ok(json_write_status rc, const std::string& what) {
  check_st(rc, json_write_status::OK, what);
}

/// 把一个 index_sequence 拼成 "0123…"：用来**直接看**下标是不是升序。
template <std::size_t... I>
std::string indices_of(json_detail::uvcpp_index_sequence<I...>) {
  std::string s;
  int swallow[] = {0, (s += std::to_string(static_cast<unsigned long long>(I)), 0)...};
  (void)swallow;
  return s;
}

/// 把字段表里的名字按顺序拼起来：用来判"表里的顺序就是宏里的顺序"。
template <typename Tuple, std::size_t... I>
std::string names_of(const Tuple& t, json_detail::uvcpp_index_sequence<I...>) {
  std::string s;
  int swallow[] = {0, (s += std::string(std::get<I>(t).name) + ";", 0)...};
  (void)swallow;
  return s;
}

// =========================================================================
// 用例里用的结构体
// =========================================================================

struct point {
  int x;
  int y;
  UVCPP_JSON_FIELDS(point, x, y)
};

/// 成员声明顺序与宏顺序**刻意相反**：判"顺序来自宏"。
struct reordered {
  int a;
  int b;
  int c;
  UVCPP_JSON_FIELDS(reordered, c, b, a)
};

/// 每个类别各来一个。
struct all_kinds {
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
  point nested;
  UVCPP_JSON_FIELDS(all_kinds, flag, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, text, nested)
};

/// 容器那一族。
struct containers {
  std::vector<int> empty_ints;
  std::vector<std::string> names;
  std::vector<std::vector<int> > matrix;
  std::vector<point> points;
  UVCPP_JSON_FIELDS(containers, empty_ints, names, matrix, points)
};

/// C 字符串字段（只写得进来，读不走 —— 见 webapp 那页）。
struct cstr {
  const char* note;
  char* plain;
  UVCPP_JSON_FIELDS(cstr, note, plain)
};

struct one_string {
  std::string s;
  UVCPP_JSON_FIELDS(one_string, s)
};

struct three_ints {
  int a;
  int b;
  int c;
  UVCPP_JSON_FIELDS(three_ints, a, b, c)
};

// 字段个数是编译期常量，顺手判一下宏与元组对得上。
static_assert(std::tuple_size<all_kinds::uvcpp_json_fields_tuple>::value == 13,
              "all_kinds 应当有 13 个字段");
static_assert(std::tuple_size<three_ints::uvcpp_json_fields_tuple>::value == 3,
              "three_ints 应当有 3 个字段");

// =========================================================================
// 1. 手写的 index_sequence
// =========================================================================
void group_index_seq() {
  using json_detail::uvcpp_index_sequence;
  using json_detail::uvcpp_make_index_seq;

  check_eq_s(indices_of(uvcpp_make_index_seq<0>()), "", "1.1 空序列");
  check_eq_s(indices_of(uvcpp_make_index_seq<1>()), "0", "1.2 单元素");
  check_eq_s(indices_of(uvcpp_make_index_seq<3>()), "012", "1.3 三个");
  check_eq_s(indices_of(uvcpp_make_index_seq<10>()), "0123456789", "1.4 十个");
  // 30 个：既压一下递归深度，也确认没有在某个位数上退化成降序。
  check_eq_s(indices_of(uvcpp_make_index_seq<30>()),
             "01234567891011121314151617181920212223242526272829", "1.5 三十个（升序）");

  // 同一个 N 两次拿到的必须是同一个类型（手写实现最容易踩的是"多算一层"）。
  check(std::is_same<uvcpp_make_index_seq<4>, uvcpp_index_sequence<0, 1, 2, 3> >::value,
        "1.6 make_index_seq<4> 就是 index_sequence<0,1,2,3>");
}

// =========================================================================
// 2. 字段表本身：个数、名字、顺序
// =========================================================================
void group_field_table() {
  const std::string names =
      names_of(three_ints::uvcppJsonFields(), json_detail::uvcpp_make_index_seq<3>());
  check_eq_s(names, "a;b;c;", "2.1 字段名按宏顺序");

  // 宏顺序 (c, b, a) ≠ 声明顺序 (a, b, c)
  const std::string rn =
      names_of(reordered::uvcppJsonFields(), json_detail::uvcpp_make_index_seq<3>());
  check_eq_s(rn, "c;b;a;", "2.2 名字跟宏走，不跟声明走");

  reordered r;
  r.a = 1;
  r.b = 2;
  r.c = 3;
  uvcpp_json_writer w;
  must_ok(uvcpp_to_json(r, w), "2.3 to_json");
  must_ok(w.finish(), "2.3 finish");
  check_eq_s(w.str(), "{\"c\":3,\"b\":2,\"a\":1}", "2.4 产出顺序 = 宏顺序");

  // 同一份字段表取两次是同一个对象（函数内静态），不是每次重新建一份。
  check(&three_ints::uvcppJsonFields() == &three_ints::uvcppJsonFields(),
        "2.5 字段表是同一个静态对象");
}

// =========================================================================
// 3. 每个类别都写对
// =========================================================================
void group_kinds() {
  all_kinds k;
  k.flag = true;
  k.i8 = -128;
  k.i16 = -32768;
  k.i32 = -2147483647 - 1;
  k.i64 = -9223372036854775807LL - 1;
  k.u8 = 255;
  k.u16 = 65535;
  k.u32 = 4294967295u;
  k.u64 = 18446744073709551615ull;
  k.f32 = 0.1f;
  k.f64 = 0.1;
  k.text = "q\"\\\n";  // 引号、反斜杠、真换行：转义是 writer 的活，这里确认接上了
  k.nested.x = 7;
  k.nested.y = -9;

  uvcpp_json_writer w;
  must_ok(uvcpp_to_json(k, w), "3.1 to_json");
  must_ok(w.finish(), "3.1 finish");

  const std::string want =
      "{"
      "\"flag\":true,"
      "\"i8\":-128,"
      "\"i16\":-32768,"
      "\"i32\":-2147483648,"
      "\"i64\":-9223372036854775808,"
      "\"u8\":255,"
      "\"u16\":65535,"
      "\"u32\":4294967295,"
      "\"u64\":18446744073709551615,"
      "\"f32\":0.1,"
      "\"f64\":0.1,"
      "\"text\":\"q\\\"\\\\\\n\","
      "\"nested\":{\"x\":7,\"y\":-9}"
      "}";
  check_eq_s(w.str(), want, "3.2 十三个字段的逐字节产出");
  // 单独把 f32 那一段再点出来：它若走 double 重载会写成 0.10000000149011612。
  check(w.str().find("\"f32\":0.1,") != std::string::npos,
        "3.3 float 字段按 float 的精度写（不是 double 的）");

  // bool 的 false 与"零值"都要正常写出来（模板分派最容易漏 false）。
  all_kinds z;
  z.flag = false;
  z.i8 = 0;
  z.i16 = 0;
  z.i32 = 0;
  z.i64 = 0;
  z.u8 = 0;
  z.u16 = 0;
  z.u32 = 0;
  z.u64 = 0;
  z.f32 = 0.0f;
  z.f64 = 0.0;
  z.nested.x = 0;
  z.nested.y = 0;
  uvcpp_json_writer w2;
  must_ok(uvcpp_to_json(z, w2), "3.4 零值 to_json");
  check_eq_s(w2.str(),
             "{\"flag\":false,\"i8\":0,\"i16\":0,\"i32\":0,\"i64\":0,\"u8\":0,\"u16\":0,"
             "\"u32\":0,\"u64\":0,\"f32\":0,\"f64\":0,\"text\":\"\",\"nested\":{\"x\":0,\"y\":0}}",
             "3.5 零值产出的逐字节");
}

/// 字符串字段里的非法字节原样搬（writer 不校验 UTF-8，这里确认不拦、不改）。
void group_string_bytes() {
  one_string o;
  o.s.assign("a\0b", 3);
  uvcpp_json_writer w;
  must_ok(uvcpp_to_json(o, w), "3.6 NUL 串");
  check_eq_s(w.str(), "{\"s\":\"a\\u0000b\"}", "3.7 NUL 转成 \\u0000（小写十六进制）");

  // 键名里的怪字符：字段名是标识符，宏取的是 #字段，所以这里只能验值那一侧。
  one_string o2;
  o2.s = "\x01\x1f";
  uvcpp_json_writer w2;
  must_ok(uvcpp_to_json(o2, w2), "3.8 控制字符");
  check_eq_s(w2.str(), "{\"s\":\"\\u0001\\u001f\"}", "3.9 控制字符的转义形态");
}

/// C 字符串字段：nullptr 写 null，非空按字符串写（`const char*` 与 `char*` 都收）。
void group_c_strings() {
  static char buf[] = "plain";
  cstr c;
  c.note = nullptr;
  c.plain = buf;
  uvcpp_json_writer w;
  must_ok(uvcpp_to_json(c, w), "3.10 C 字符串");
  check_eq_s(w.str(), "{\"note\":null,\"plain\":\"plain\"}", "3.11 nullptr → null");

  c.note = "note";
  c.plain = nullptr;
  uvcpp_json_writer w2;
  must_ok(uvcpp_to_json(c, w2), "3.12 换一下");
  check_eq_s(w2.str(), "{\"note\":\"note\",\"plain\":null}", "3.13 char* 的 null");
}

// =========================================================================
// 4. 容器
// =========================================================================
void group_containers() {
  containers c;
  uvcpp_json_writer w;
  must_ok(uvcpp_to_json(c, w), "4.1 全空容器");
  check_eq_s(w.str(), "{\"empty_ints\":[],\"names\":[],\"matrix\":[],\"points\":[]}",
             "4.2 空 vector 写成 []（不是 null、不是缺字段）");

  c.names.push_back("a");
  c.names.push_back("b\"c");
  c.matrix.push_back(std::vector<int>(2, 5));
  point p;
  p.x = 1;
  p.y = 2;
  c.points.push_back(p);
  p.x = 3;
  p.y = 4;
  c.points.push_back(p);
  c.empty_ints.push_back(-1);

  uvcpp_json_writer w2;
  must_ok(uvcpp_to_json(c, w2), "4.3 装满");
  check_eq_s(w2.str(),
             "{\"empty_ints\":[-1],\"names\":[\"a\",\"b\\\"c\"],\"matrix\":[[5,5]],"
             "\"points\":[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4}]}",
             "4.4 标量/字符串/嵌套 vector/结构体 vector 的产出");
}

// =========================================================================
// 5. 接在更大的结构里
// =========================================================================
void group_compose() {
  point p;
  p.x = 1;
  p.y = 2;

  uvcpp_json_writer w;
  must_ok(w.object_begin(), "5.1 {");
  must_ok(w.key("ok"), "5.2 key");
  must_ok(w.value(true), "5.3 value");
  must_ok(w.key("point"), "5.4 key");
  must_ok(uvcpp_to_json(p, w), "5.5 嵌套 to_json");
  must_ok(w.key("list"), "5.6 key");
  must_ok(w.array_begin(), "5.7 [");
  must_ok(uvcpp_to_json(p, w), "5.8 数组里的 to_json");
  must_ok(w.array_end(), "5.9 ]");
  must_ok(w.object_end(), "5.10 }");
  must_ok(w.finish(), "5.11 finish");
  check_eq_s(w.str(), "{\"ok\":true,\"point\":{\"x\":1,\"y\":2},\"list\":[{\"x\":1,\"y\":2}]}",
             "5.12 嵌进外层结构后的产出");

  // 直写外部缓冲：`written()` 只数自己写的那一段。
  std::string out = "PRE:";
  uvcpp_json_writer w2(out);
  must_ok(uvcpp_to_json(p, w2), "5.13 外部缓冲");
  must_ok(w2.finish(), "5.13 finish");
  check_eq_s(out, "PRE:{\"x\":1,\"y\":2}", "5.14 追加语义（前缀不动）");
  check_eq_u(w2.written(), 13, "5.15 written() 不含前缀（`{\"x\":1,\"y\":2}` 是 13 字节）");
}

// =========================================================================
// 6. 失败：返回码传得对不对（短路本身不可观测，见文件头）
// =========================================================================
void group_failures() {
  three_ints t;
  t.a = 111111;
  t.b = 222222;
  t.c = 333333;

  // 16 字节的上限：第一个字段写完就超了。
  uvcpp_json_writer w(json_write_options(64, 16));
  const json_write_status st = uvcpp_to_json(t, w);
  check_st(st, json_write_status::TOO_LARGE, "6.1 超上限时 to_json 报 TOO_LARGE");
  check_st(w.status(), json_write_status::TOO_LARGE, "6.2 writer 的粘性状态一致");
  check_st(w.finish(), json_write_status::TOO_LARGE, "6.3 finish 也是那个错（不是 OK）");

  // 已经失败的 writer 上再调一次：拿到的必须是**同一个**错，而不是重新开始写。
  const size_t before = w.written();
  check_st(uvcpp_to_json(t, w), json_write_status::TOO_LARGE, "6.4 失败的 writer 上报同一个错");
  check_eq_u(w.written(), before, "6.5 失败之后一个字节都没再写");

  // 上限够大：正常成功，且 finish 是 OK（证明 6.1 那个错来自上限，不是来自形状）。
  uvcpp_json_writer w2(json_write_options(64, 4096));
  must_ok(uvcpp_to_json(t, w2), "6.6 上限够大时成功");
  must_ok(w2.finish(), "6.6 finish");
  check_eq_s(w2.str(), "{\"a\":111111,\"b\":222222,\"c\":333333}", "6.7 产出正确");

  // 深度：嵌套结构体每层占一个对象帧。这里判的是"错误能传出来"，
  // 不是"深度算得对"（那是 writer 那条用例的活）。
  uvcpp_json_writer w3(json_write_options(0, 4096));
  check_st(uvcpp_to_json(t, w3), json_write_status::TOO_DEEP, "6.8 depth=0 时对象都开不了");
}

}  // namespace

int main() {
  group_index_seq();
  group_field_table();
  group_kinds();
  group_string_bytes();
  group_c_strings();
  group_containers();
  group_compose();
  group_failures();

  if (g_failures == 0) {
    std::cout << "[json_reflect] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[json_reflect] FAIL (" << g_failures << ")" << std::endl;
  return 2;
}
