/**
 * @file tests/unit/http_name_alloc_unit.cpp
 * @brief 头部查找**不能为头名构造临时 `std::string`**；值位置同理。
 *
 * ## 这个用例在钉什么
 *
 * `http_has_header(h, "transfer-encoding")` 这类调用，如果形参是
 * `const std::string&`，那么字面量在**调用点**就要构造一个 `std::string`；
 * 一旦超过该标准库的 SSO 上限，每一次调用就是一次堆分配 + 一次释放，而那个
 * 串马上就被扔掉。
 *
 * 它在热路径上是**每请求 6~8 次**（`uvcpp_http_server.cpp` 每响应三次、
 * `uvcpp_http_response.cpp` 每个响应头一次、压缩判定之前一次），h2 上每个
 * 响应头一次（`src/http2/uvcpp_h2_session.cpp` 的 `name_is`，那里原先明写着
 * `http_name_equal(n, std::string(lit))`）。
 *
 * ## SSO 上限三个标准库**不一样** —— 用例的字面量长度是照这个挑的
 *
 * | 标准库 | `std::string` 的 SSO 上限 |
 * |---|---|
 * | MSVC（`xstring`: `_BUF_SIZE - 1`） | 15 |
 * | libstdc++（`_S_local_capacity`） | 15 |
 * | libc++（`__min_cap`，64 位） | **22** |
 *
 * 所以判据用的长头名取 **`"access-control-allow-origin"`（27）** —— 它在三个
 * 标准库上**都**超过上限，用例在三条 CI 腿上都真的有牙。
 *
 * 反过来，`"transfer-encoding"`（17）在 macOS 上**不**超上限 ⇒ 拿它当判据在
 * 那条腿上恒真。它仍然被断言（0 次分配在哪儿都对），但要知道**它在 libc++ 上
 * 是空过的**，所以只当补充，不当承重的那条。
 *
 * ## 为什么不用链库
 *
 * 判据要抢过全局 `operator new`。而库那份 `.cpp` 里的 `new` 绑的是它自己那个
 * CRT 的 `operator new` —— exe 里怎么覆盖都接不到，于是"库没改"也能跑绿，判据
 * 就成了空的（同一个坑记在 `tests/expand/pool_self_sufficiency_test.cpp:9`）。
 *
 * 所以这个 exe **不链 `${UVCPP_LIB_TARGET}`**。它能这么做，是因为要证的四个
 * 函数**全是头内 `inline`**（`src/web/uvcpp_http_common.h`），本 exe 里自带一份，
 * 抢到的 `new` 一定看得见它们。
 *
 * **覆盖面要说准**：类成员那些重载（`uvcpp_http_{request,response}`、
 * `uvcpp_web_{request,response}`）是**一行转发**到这四个自由函数，而转发发生在
 * 库里 ⇒ 本用例**证不到成员那一层**。承重的是"自由函数不再构造临时串"；
 * 成员那层靠这层转发 + 行为用例（`web_http_server_func` 等）覆盖。别把这里
 * 读成"整条链都验过了"。
 *
 * ## 值那一侧（名字侧的门禁挡不住它）
 *
 * 长字面量**也**落在值位置，只是不在 `set_header` 的调用点上，而在
 * `uvcpp_web_response::body(...)` 的 `ct` 实参上 —— `text()` 传 25 字符、
 * `html()` 24、`json()` / `json_str()` 31，全由 `body(const std::string& s,
 * const std::string& ct)` 收下。仓库自己的 bench 走的就是这两条路
 * （`bench/bench_server.cpp` 的 `/json` 与 `/text`）。
 *
 * 所以下半段给值侧也立了门禁。它的**判据形状与名字侧不同**：值的字节必须被
 * 存下来，所以"插入"的判据是**恰好 1**（只付值自己那一份存储），名字侧才是 0。
 * 改前的形状是 2 —— 一个调用点上的临时串，加一次 `push_back` 的拷贝。
 * **要比的是这条 2 → 1，不是绝对数。**
 *
 * 它自带的对照：改前形状逐字重演必须恰好 2；覆写那条的初值铺了远超 31 的
 * 容量，所以覆写对照是 1（只剩那个临时串）—— 这一条同时钉住了"初值确实够宽"
 * 这个前提，前提若不成立它会自己变红。
 *
 * ## 对照组（没有它们，"0 次分配"什么也证明不了）
 *
 * 1. **27 字符字面量走 `std::string`** ⇒ 必须**恰好 1** 次。它同时证明三件事：
 *    计数器装对了、这个长度确实超过本平台的上限、以及"改前的形状"确实要付钱。
 *    这一条要是松了，下面的 0 就可能是"计数器根本没数"。
 * 2. **短字面量**（`"host"`）⇒ 必须 0 次。它证明计数器不是"什么都数"。
 */

#include <web/uvcpp_http_common.h>

#if !UVCPP_WEB_ENABLE
#error "这个用例只在 UVCPP_BUILD_WEB=ON 时有意义；CMake 里没有把它关在 if(UVCPP_BUILD_WEB) 内。"
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

// =========================================================================
// 抢全局 new/delete —— 只计数，窗口外一律不计
// =========================================================================

namespace {

bool g_counting = false;
long g_allocs   = 0;

}  // namespace

void* operator new(std::size_t n) {
  if (g_counting) ++g_allocs;
  void* p = std::malloc(n ? n : 1);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  if (g_counting) ++g_allocs;
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
  return ::operator new(n, t);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// =========================================================================
// 用例
// =========================================================================

namespace {

using namespace uvcpp;

/// 27 字符，三个标准库的 SSO 上限（15 / 15 / 22）都超过。
const char* const kLongName = "access-control-allow-origin";

/// 17 字符：MSVC/libstdc++ 超，**libc++ 不超**（见文件头那张表）。
const char* const kMidName = "transfer-encoding";

// --- 值侧的两个前提，用 `static_assert` 钉在编译期 ---
//
// 写成数组（而不是 `const char* const`）就是为了拿得到 `sizeof`。这两条是
// "值侧判据在什么情况下会是错的"的答案：值要是没超过上限，整段就是空过的。
// 数字对不上时**编译**就红，不用等哪条 CI 腿跑出个说不清的结果。

/// 31 字符，仓库自己那条 json 路径用的字面量（`uvcpp_web_response::json()`）。
const char kLongValue[] = "application/json; charset=utf-8";

/// 69 字符，只用来给"覆写"那条判据铺一个容量足够宽的初值。
const char kSlackValue[] =
    "application/json; charset=utf-8; padding=0123456789012345678901234567";

static_assert(sizeof(kLongValue) - 1 > 22u,
              "值侧判据要求判据值超过**三个标准库中最大**的 SSO 上限 22");
static_assert(sizeof(kSlackValue) - 1 > sizeof(kLongValue) - 1,
              "覆写对照要求初值比判据值宽，否则那条会因重分配而红，"
              "与被测代码无关");

// --- 注释里报的那几个长度，也钉在这里 ---
//
// 「印出来的数与算出来的数对不上」是这个仓库里**已经出现过两次**的错法。上面
// 那两条判的是**判据值**的长度，对注释里报的数一个都管不着 —— 我自己上一版就把
// `text()` 报成 24、`html()` 报成 23，实测是 **25 / 24**。所以这三条只管一件事：
// 下面几句注释里报的数不许写错。
//
// 它们**不**保证库里的 `ct` 字面量没被换成别的（换了两边一起变，这里不会红）——
// 那是行为用例的事，不是本文件的事。
static_assert(sizeof("text/plain; charset=utf-8") - 1 == 25u,
              "`text()` 的 ct 是 25 字符，注释里别写成 24");
static_assert(sizeof("text/html; charset=utf-8") - 1 == 24u,
              "`html()` 的 ct 是 24 字符，注释里别写成 23");
static_assert(sizeof("application/json; charset=utf-8") - 1 == 31u,
              "`json()` 的 ct 是 31 字符");

/// 跑一段东西并返回它分配了几次。窗口内**不能**有本用例自己的分配。
template <typename Fn>
long allocs_of(Fn fn) {
  g_allocs   = 0;
  g_counting = true;
  fn();
  g_counting = false;
  return g_allocs;
}

int g_failures = 0;

void expect(const char* what, long got, long want) {
  const bool ok = (got == want);
  if (!ok) ++g_failures;
  std::printf("  %s  %-56s got=%ld want=%ld\n", ok ? "PASS" : "FAIL", what,
              got, want);
}

}  // namespace

int main() {
  std::printf("[unit][http_name_alloc] start\n");

  // 头表在**窗口之外**建：建表本身要分配（向量 + 值串），那是正当的。
  // 值一律取短的，好让 `http_get_header` 的**返回值**走 SSO —— 于是窗口里
  // 唯一可能分配的东西就是"为头名构造的那个临时串"，判据才干净。
  http_headers h;
  h.push_back(http_header{"host", "a"});
  h.push_back(http_header{kLongName, "*"});
  h.push_back(http_header{kMidName, "chunked"});

  const std::string long_name_str(kLongName);

  // 热身：把任何惰性初始化挡在窗口外。
  (void)http_has_header(h, "host");
  (void)http_get_header(h, kLongName);

  // ---------------------------------------------------------------------
  // 对照组 1 —— 承重的那条。27 字符走 std::string 必须**恰好**分配一次：
  // 计数器装对了 + 这个长度确实超上限 + "改前的形状"确实要付钱。
  // ---------------------------------------------------------------------
  std::printf("-- 对照组（证明计数器有牙）\n");
  expect("std::string(kLongName) 构造  [27 字符，三库都超上限]",
         allocs_of([] { std::string s(kLongName); (void)s; }), 1);

  // 这一条就是**改前的形状逐字重演**：先显式构造 std::string 再比。
  // 它必须分配 —— 于是「判据那几条本该是 0」在同进程内就有了对照，
  // 不必回退源码重编一遍。
  expect("http_name_equal(h[1].name, std::string(kLongName))  [改前形状]",
         allocs_of([&] {
           (void)http_name_equal(h[1].name, std::string(kLongName));
         }),
         1);

  expect("std::string(\"host\") 构造  [SSO，不该分配]",
         allocs_of([] { std::string s("host"); (void)s; }), 0);

  // 这一条不判分，只报告：它**随平台变**（libc++ 上 0），写死期望值会让
  // macOS 那条腿红的与被测代码毫无关系。
  std::printf("  INFO  std::string(kMidName) 构造 = %ld 次"
              "（MSVC/libstdc++ 为 1；libc++ 上限 22 ⇒ 0）\n",
              allocs_of([] { std::string s(kMidName); (void)s; }));

  // ---------------------------------------------------------------------
  // 判据 —— 字面量做头名，一次分配都不该有。三条腿上都真的有牙。
  // ---------------------------------------------------------------------
  std::printf("-- 判据（字面量做头名，want=0）\n");
  expect("http_has_header(h, kLongName)",
         allocs_of([&] { (void)http_has_header(h, kLongName); }), 0);
  expect("http_get_header(h, kLongName)",
         allocs_of([&] { (void)http_get_header(h, kLongName); }), 0);
  expect("http_name_equal(h[1].name, kLongName)",
         allocs_of([&] { (void)http_name_equal(h[1].name, kLongName); }), 0);
  expect("http_set_header(h, kLongName, \"*\")",
         allocs_of([&] { http_set_header(h, kLongName, "*"); }), 0);

  // 补充：热路径上真正用到的那个名字。libc++ 上这条空过（17 < 22），
  // 但在 MSVC/libstdc++ 上是实打实的。
  expect("http_has_header(h, kMidName)  [libc++ 上为空过]",
         allocs_of([&] { (void)http_has_header(h, kMidName); }), 0);

  // 短字面量：改前改后都该是 0。证明计数器不是"什么都数"。
  expect("http_has_header(h, \"host\")  [短字面量]",
         allocs_of([&] { (void)http_has_header(h, "host"); }), 0);

  // 传 `std::string` 的既有路径：串已经存在，本就不该再分配。
  // 它同时保证新重载没有把老调用点引到一条更差的路上去。
  expect("http_has_header(h, long_name_str)  [既有 std::string 路径]",
         allocs_of([&] { (void)http_has_header(h, long_name_str); }), 0);

  // ---------------------------------------------------------------------
  // 值侧 —— 判据形状与名字侧不同（见文件头）：**插入 want=1**（值那几字节
  // 必须被存下来），覆写 want=0。每条都用一个**窗口外新建**的头表，免得
  // 上一条的插入把下一条悄悄变成覆写。
  //
  // 两个表都 `reserve(4)` —— 不是随手写的：向量自己的扩容是噪声。库那边现在是
  // **第一次插入时**才预留（`http_reserve_headers()`），这两个表是裸的
  // `http_headers`、不过那道口子，所以自己预留，窗口里才只剩"值那几字节"。
  // ---------------------------------------------------------------------
  std::printf("-- 值侧（长字面量落在值位置）\n");

  const auto fresh_table = [] {
    http_headers t;
    t.reserve(4);
    t.push_back(http_header{"host", "a"});
    return t;
  };
  const auto with_slack = [] {
    http_headers t;
    t.reserve(4);
    t.push_back(http_header{"host", "a"});
    t.push_back(http_header{"content-type", kSlackValue});
    return t;
  };

  // 对照组 3 —— 值侧承重的那条。改前的形状逐字重演：调用点上一个**值位置**的
  // 临时串（1 次），加 `push_back` 把值拷进向量（1 次）。它同时钉住两个前提：
  // 计数器看得见值位置、`kLongValue` 确实超过本平台上限。
  {
    http_headers hv = fresh_table();
    expect("http_set_header(hv, \"content-type\", std::string(kLongValue))  [改前·插入]",
           allocs_of([&] {
             http_set_header(hv, "content-type", std::string(kLongValue));
           }),
           2);
  }
  {
    http_headers hv = fresh_table();
    expect("http_set_header(hv, \"content-type\", kLongValue)  [新形状·插入]",
           allocs_of([&] { http_set_header(hv, "content-type", kLongValue); }), 1);
  }

  // 覆写才是热路径上真正发生的事（响应对象设过 content-type 之后又改一次）。
  // 初值 69 字符 ⇒ 容量远超 31 ⇒ 改前形状只剩那个临时串，新形状一次都不该有。
  // 对照那条同时钉住"初值确实够宽"这个前提：不成立它会自己变红。
  {
    http_headers hv = with_slack();
    expect("http_set_header(hv, \"content-type\", std::string(kLongValue))  [改前·覆写]",
           allocs_of([&] {
             http_set_header(hv, "content-type", std::string(kLongValue));
           }),
           1);
  }
  {
    http_headers hv = with_slack();
    expect("http_set_header(hv, \"content-type\", kLongValue)  [新形状·覆写]",
           allocs_of([&] { http_set_header(hv, "content-type", kLongValue); }), 0);
  }

  // ---------------------------------------------------------------------
  // 表本身 —— `http_lower_ascii()` 是一张 256 项的表，判据是**逐字节**对上
  // `std::tolower`（C locale），不是抽几个字母看看。为什么值得单列一条：
  // 那是张手写不了、只能生成的表，抄错一格的表现是"某个特定头名在某些大小写
  // 下认不出来"，热路径上多半表现为偶发怪异而不是红。
  //
  // 顺带钉住"只动 ASCII"：0x80-0xFF 这 128 个字节**原样**通过（C locale 下
  // `std::tolower` 也不动它们）。这一条不是多余的 —— 它把"表被写成'把高端字节
  // 也映射一遍'"这种改法挡住。
  // ---------------------------------------------------------------------
  std::printf("-- 表（http_lower_ascii）\n");
  {
    long diff = 0;
    long high_moved = 0;
    for (int i = 0; i < 256; ++i) {
      const unsigned char c = static_cast<unsigned char>(i);
      if (http_lower_ascii(c) !=
          static_cast<unsigned char>(std::tolower(c)))
        ++diff;
      if (i >= 0x80 && http_lower_ascii(c) != c) ++high_moved;
    }
    expect("http_lower_ascii 对 std::tolower（256 字节全覆盖）之差",
           diff, 0);
    expect("0x80-0xFF 原样通过（表没有去动高端字节）", high_moved, 0);
  }

  if (g_failures != 0) {
    std::printf("FAIL: %d 条判据没过 —— 字面量仍在构造临时 std::string\n",
                g_failures);
    std::fflush(stdout);
    return 1;
  }
  std::printf("PASS: 头名零分配；字面量值只付它自己那一份存储\n");
  std::fflush(stdout);
  return 0;
}
