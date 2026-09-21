/**
 * @file tests/unit/http_name_alloc_unit.cpp
 * @brief 头部查找**不能为头名构造临时 `std::string`**。
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

  if (g_failures != 0) {
    std::printf("FAIL: %d 条判据没过 —— 头名仍在构造临时 std::string\n",
                g_failures);
    std::fflush(stdout);
    return 1;
  }
  std::printf("PASS: 头名字面量没有产生任何堆分配\n");
  std::fflush(stdout);
  return 0;
}
