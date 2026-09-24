/**
 * @file tests/functional/web_app_worklimit_func.cpp
 * @brief `uvcpp_web_work_limit` 的功能测试 —— 真端口、真静态文件、真客户端。
 *
 * 这个模块要证明的是三件事，用例就围着它们：
 *
 *   1. **名额满了真的会回绝**（503 + `Retry-After`），而不是照投不误。
 *   2. **名额会还**。这一条比第一条更容易被漏掉：一个"只 acquire 不 release"
 *      的实现能让第一次请求完全正常，只在**第二个**请求上才开始回 503 ——
 *      而任何人都只跑一次的话，看上去一切正常。所以对照组是**连发 N 次**
 *      且上限设成 1：只要漏还一次，第二次就红。
 *   3. **记账本身**（`acquire`/`release`/`in_flight`/`limit`）在各种边界上
 *      不越界、不下溢、不因为并发而放过头。
 *
 * ## 怎么造"池子已满"才是确定性的
 *
 * 不靠"投一个慢任务占住名额" —— 那要靠 worker 线程的调度时机来撞，是竞态
 * 而不是判据。这里用的是**直接占位**：上限设成 1，测试自己调
 * `app.work_limit()->acquire()` 把那个唯一的名额拿走。服务端的静态请求于是
 * 必然拿不到名额，503 是**必然**发生的，与线程调度无关。
 *
 * 代价是：这样测出来的 503 是"名额被占住时**要读盘的**静态路径会回绝"，而
 * "静态路径确实会 acquire"这一点由**同一条断言**保证（如果它根本不做准入，
 * 这一条会返回 200 而不是 503）。而"确实会 release"由第 6 组（上限 1 连发
 * 5 个不同的冷文件全 200）保证。三条合起来才是完整的：**会拿、会还、拿不到
 * 会拒**。
 *
 * ## 还有第四条：**命中缓存的那一支不占名额**
 *
 * 闸门卡的是"并发的磁盘读"，而它过去在 `serve()` 里、投递之前就取名额 ——
 * 于是"命中 LRU 缓存、一次磁盘读都不需要"的请求也一起被 503 掉，而那是绝
 * 大多数。`static_integration` 里用**同一个被占住的名额**同时钉住两侧：冷文件
 * 必须 503、热文件必须 200。两条互为对照 —— 只测一边分不出"命中绕开闸门"和
 * "闸门整个失效"这两种截然不同的实现。
 *
 * 与它配套的还有第 7 组 `stream_pauses_not_rejects`：名额**只在"确实要读盘"
 * 的两条支路上取**（整读进内存 / 超过阈值走分片流式），覆盖集合里唯一被移出
 * 去的是缓存命中那一支 —— 这一点与改动前一致。**但流式那一支的"拿不到名额
 * 怎么办"在 2026-09-24 反了过来**：不是回绝，是**停下等**（详见那一组的说明）。
 *
 * ## 第五条：**上限这个数字是从哪来的**
 *
 * 闸门的上限是"线程池线程数 × 4"，所以"线程池线程数"从哪来是它的地基。这里
 * 有两条判据，分别在两组里：
 *
 *   - **第 3 组 `default_limit`**：还没投过池子活儿时，那个数就是"环境变量现在
 *     说什么"（libuv 是**惰性**读的，第一次 `uv__work_submit` 才读）。所以这
 *     一组自带一条**前提断言**："本用例必须在任何池子活儿之前跑" —— 投过之后
 *     读数是"钉住"的那个，这一组每条"改了立刻可见"都会红，而红的是前提没了。
 *   - **第 9 组 `limit_source`**：本类那两条静态函数是 `uvcpp_threadpool.h` 的
 *     **转发**（自己不再重读环境变量），以及用户显式给过的上限**不许被
 *     `start()` 的重算覆盖**（`set_work_limit(0)` = 明确要不限，最承重的一格）。
 *
 * 池子大小被"钉住"之后的行为（环境变量改不动它、`uvcpp_set_threadpool_size()`
 * 回 `UV_EALREADY`）不在本文件里 —— 那是核心模块自己的契约，装置放在
 * `tests/functional/threadpool_func.cpp`，那边能自己控制进程里的先后次序。
 *
 * ## 为什么不用真慢任务
 *
 * 静态读盘是 `stat` + 读一个小文件，耗时可忽略。要让它"慢"，只能往根里放一
 * 个大文件或者干脆卡住文件系统 —— 前者不可靠（页缓存一热就快了），后者没法
 * 在测试里做。所以确定性的路只有占位这一条。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include "net/uvcpp_net_read.h"
#include "net/uvcpp_tcp_client.h"
#include <uvcpp/uvcpp_threadpool.h>  // 上限那个数从哪来（第 3、9 组）
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_static.h>
#include <webapp/uvcpp_web_work_limit.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
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

int status_of(const uvcpp_http_response& r) {
  return static_cast<int>(r.status_code);
}

std::string body_of(const uvcpp_http_response& r) {
  if (r.body.size() == 0) return std::string();
  return std::string(r.body.get_const_data(), r.body.size());
}

std::string header_of(const uvcpp_http_response& r, const char* name) {
  return http_get_header(r.headers, name);
}

// =========================================================================
// 测试用文档根（沿用 web_app_static_func.cpp 的写法）
// =========================================================================

const char* k_root = "uvcpp_worklimit_test_root";
const char* k_file = "uvcpp_worklimit_test_root/hello.txt";
const char* k_text = "WORK-LIMIT-OK";

/// 一个**从没被请求过**的文件 —— 用它来保证"这次一定要读盘"。
///
/// 闸门从 `serve()` 搬到 worker 之后，"要不要占名额"分成两条路：命中 LRU
/// 缓存的一次磁盘读都不做（不该占），未命中要整读进内存（该占）。要辨出
/// 这两条，用例就必须能**指定**是哪一条 —— 冷文件与热文件各一个，最直接。
const char* k_cold_file = "uvcpp_worklimit_test_root/cold.txt";
/// 还名额那一组专用的一批冷文件（每发一个都必须是未命中）。
char k_seq_files[5][64] = {
    "uvcpp_worklimit_test_root/seq0.txt", "uvcpp_worklimit_test_root/seq1.txt",
    "uvcpp_worklimit_test_root/seq2.txt", "uvcpp_worklimit_test_root/seq3.txt",
    "uvcpp_worklimit_test_root/seq4.txt"};

#ifdef _WIN32
#include <direct.h>
#define TEST_MKDIR(p) _mkdir(p)
#define TEST_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>   // rmdir（`sys/stat.h` 只给 mkdir）
#define TEST_MKDIR(p) mkdir(p, 0755)
#define TEST_RMDIR(p) rmdir(p)
#endif

void write_file(const std::string& path, const std::string& text) {
  std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void make_root() {
  TEST_MKDIR(k_root);
  write_file(k_file, k_text);
  write_file(k_cold_file, k_text);
  for (int i = 0; i < 5; ++i) write_file(k_seq_files[i], k_text);
}

void cleanup_root() {
  std::remove(k_file);
  TEST_RMDIR(k_root);
}

// =========================================================================
// 客户端侧小工具（沿用 web_app_app_func.cpp 的写法）
// =========================================================================

bool roundtrip(int port, const uvcpp_http_request& req,
               uvcpp_http_response& resp) {
  for (int i = 0; i < 40; ++i) {
    uvcpp_http_client client;
    if (client.connect_wait("127.0.0.1", port, 2000) == 0 &&
        client.send_wait(req, resp, 3000) == 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

bool get(int port, const std::string& path, uvcpp_http_response& resp) {
  return roundtrip(port, uvcpp_http_request::make_get(path), resp);
}

void configure_for_test(uvcpp_web_app& app) {
  // 用 `ERR` 而不是 `WARN`：本文件**故意**会把池子占满，那条"工作池已满"的
  // WARN 是**预期行为**，不该混在输出里看起来像问题。等级名是 `ERR` 不是
  // `ERROR` —— Windows 的 `<wingdi.h>` 里有 `#define ERROR 0`，见
  // `uvcpp_log.h` 里那段说明。
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_log_level(log_level::ERR)
      .set_access_log(false);
}

// =========================================================================
// 1. 记账：acquire / release / in_flight / 上限
// =========================================================================
void test_accounting() {
  uvcpp_web_work_limit lim(2);
  check_eq_i(static_cast<long long>(lim.limit()), 2, "记账：上限 = 2");
  check_eq_i(static_cast<long long>(lim.in_flight()), 0, "记账：初值 0");
  check(!lim.full(), "记账：空池子不该是满的");

  check(lim.acquire(), "记账：第 1 个名额应当拿到");
  check(lim.acquire(), "记账：第 2 个名额应当拿到");
  check_eq_i(static_cast<long long>(lim.in_flight()), 2, "记账：在途 = 2");

  // 关键的一条：满了之后**必须拒绝**，而且拒绝时**不能**把计数也加进去。
  check(lim.full(), "记账：2/2 应当是满的");
  check(!lim.acquire(), "记账：满员时第 3 个名额必须被拒");
  check_eq_i(static_cast<long long>(lim.in_flight()), 2,
             "记账：被拒的那次不该改变在途数");

  lim.release();
  check_eq_i(static_cast<long long>(lim.in_flight()), 1, "记账：还回后 = 1");
  check(!lim.full(), "记账：还回后不该还是满的");
  check(lim.acquire(), "记账：腾出名额后应当还能拿到");
  check_eq_i(static_cast<long long>(lim.in_flight()), 2, "记账：又回到 2");

  lim.release();
  lim.release();
  check_eq_i(static_cast<long long>(lim.in_flight()), 0, "记账：全部还回 = 0");

  // **多还不会下溢。** 下溢之后 `in_flight()` 会变成一个接近 SIZE_MAX 的
  // 巨数，闸门就此永久打开，而唯一的症状是"限流突然不管用了" —— 极难定位。
  // 钳位之后的正确语义是"多还一次是空操作"。
  lim.release();
  lim.release();
  check_eq_i(static_cast<long long>(lim.in_flight()), 0, "记账：多还不会下溢");
  check(!lim.full(), "记账：多还之后不该变成满的");
  check(lim.acquire(), "记账：多还之后仍然能正常拿到名额");

  lim.reset();
  check_eq_i(static_cast<long long>(lim.in_flight()), 0, "记账：reset 清零");
}

// =========================================================================
// 2. 0 = 不限
// =========================================================================
void test_unlimited() {
  uvcpp_web_work_limit lim(0);
  check_eq_i(static_cast<long long>(lim.limit()), 0, "不限：limit() = 0");
  check(!lim.full(), "不限：永远不满");

  bool all = true;
  for (int i = 0; i < 1000; ++i) {
    if (!lim.acquire()) all = false;
  }
  check(all, "不限：1000 次 acquire 应当全部成功");
  // 计数照常走 —— 不限的是闸门，不是账本。运维要靠 `in_flight()` 看出
  // "现在到底堆了多少"，一个恒为 0 的计数器等于把观测能力也一起关掉了。
  check_eq_i(static_cast<long long>(lim.in_flight()), 1000,
             "不限：在途数照常累计");
}

// =========================================================================
// 3. 默认值由 UV_THREADPOOL_SIZE 推导
// =========================================================================
void test_default_limit() {
  // **前提**：本用例断言的是"**还没投过**池子活儿时，池子大小跟着环境变量走"。
  // 一旦本进程已经投过（`uvcpp_threadpool_used()`），libuv 那边早把线程数读进
  // 缓存、本库也把它钉住了（见 `src/uvcpp/uvcpp_threadpool.h`）—— 那时下面每条
  // "改了立刻可见"的断言都会红，而红的**不是代码坏了，是这个前提没了**。
  // 所以先把它说出来：万一以后有人把本用例挪到某个会投池子的用例后面，这里报
  // 的是"前提不成立"，而不是让人对着几个数字发呆。钉住之后的行为由最后一组
  // `pool_size_pins` 正面覆盖。
  check(!uvcpp_threadpool_used(),
        "默认值：前提 —— 本用例必须在**任何**池子活儿之前跑");

  // 本进程默认**没有**设这个变量（ctest 不会替我们设），所以先按未设计算。
  // 但如果外部环境恰好设了，就不能拿"未设"的期望值去断言 —— 那样在别人
  // 机器上会红，而且看起来像代码坏了。先查再断言。
  const char* orig_env = std::getenv("UV_THREADPOOL_SIZE");
  const bool was_set = (orig_env != nullptr && *orig_env != '\0');
  // 原值要留下来 —— 还原时必须把它**原样放回去**，而不是一律删掉：万一
  // 外部环境（CI、开发者的 shell）设过，删掉就是把这个进程的环境改坏了，
  // 而后面还有别的用例在读它。
  const std::string original_value = was_set ? std::string(orig_env)
                                             : std::string();
  const size_t pool = uvcpp_web_work_limit::threadpool_size();
  const size_t expect = pool * 4 < 16 ? 16 : pool * 4;

  check_eq_i(static_cast<long long>(uvcpp_web_work_limit::default_limit()),
             static_cast<long long>(expect),
             "默认值：应当 = 池子大小 × 4（下限 16）");
  check(pool >= 1, "默认值：池子大小应当 >= 1");
  check_eq_i(static_cast<long long>(uvcpp_web_work_limit().limit()),
             static_cast<long long>(expect), "默认值：默认构造应当用它");

  // 未设时池子就是 libuv 的默认值 4，默认上限因此是 16（下限正好生效）。
  if (!was_set) {
    check_eq_i(static_cast<long long>(pool), 4,
               "默认值：未设时池子应当按 libuv 默认的 4 算");
    check_eq_i(static_cast<long long>(expect), 16,
               "默认值：未设时上限应当是 16（下限生效）");
  } else {
    std::cout << "  (注：本进程已设置 UV_THREADPOOL_SIZE=" << pool
              << "，跳过「未设」分支)" << std::endl;
  }

  // 显式设成别的值 → 上限跟着走。之所以能"立刻可见"，是因为**本进程还没投过
  // 池子活儿**（上面那条前提断言）：没投过时池子大小就是"环境变量现在说什么"。
  // 投过之后就改成"libuv 真正在用几条"了 —— 那是 `pool_size_pins` 那一组。
  const size_t kProbe = 7;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(kProbe));
#ifdef _WIN32
  _putenv_s("UV_THREADPOOL_SIZE", buf);
#else
  setenv("UV_THREADPOOL_SIZE", buf, 1);
#endif
  check(uvcpp_web_work_limit::threadpool_size_is_set(),
        "默认值：设置后 threadpool_size_is_set() 应当为真");
  check_eq_i(static_cast<long long>(uvcpp_web_work_limit::threadpool_size()),
             static_cast<long long>(kProbe), "默认值：应当读到 " + std::string(buf));
  check_eq_i(static_cast<long long>(uvcpp_web_work_limit::default_limit()),
             static_cast<long long>(kProbe * 4), "默认值：上限应当是 28");

  // 还原，别把污染留给后面的用例（同一个进程里还有别人读这个变量）。
  // Windows 上 `_putenv_s(name, "")` 的语义就是**删除**该变量。
#ifdef _WIN32
  _putenv_s("UV_THREADPOOL_SIZE", was_set ? original_value.c_str() : "");
#else
  if (was_set) {
    setenv("UV_THREADPOOL_SIZE", original_value.c_str(), 1);
  } else {
    unsetenv("UV_THREADPOOL_SIZE");
  }
#endif
  check_eq_i(static_cast<long long>(uvcpp_web_work_limit::threadpool_size()),
             static_cast<long long>(was_set ? pool : 4),
             "默认值：还原之后应当回到原值");
}

// =========================================================================
// 4/5/6. 与静态服务的接线
// =========================================================================
void test_static_integration() {
  make_root();

  uvcpp_web_app app;
  configure_for_test(app);

  // 上限设成 1 —— 这样"有没有名额"只有一个来源，断言最好读。
  app.set_work_limit(1);

  std::shared_ptr<uvcpp_web_static> st = app.serve_static("/s", k_root);
  check(st->work_limit().get() == app.work_limit().get(),
        "接线：静态服务装上的应当是 App 那个闸门（同一个对象）");

  check(app.start_background() == 0, "接线：服务启动");
  const int port = app.bound_port();

  uvcpp_http_response r;

  // ---- 对照组：名额空着 → 正常服务 ----
  //
  // 没有这一组的话，"做不做准入"是分不出来的：一个**从不 acquire** 的实现
  // 会让下面那条 503 断言红，但如果断言方向写反了（或者只看"有没有响应"），
  // 它就蒙混过去了。有了这一组，"名额空着必须 200、被占住必须 503"这一对
  // 才构成判据。
  check(get(port, "/s/hello.txt", r), "对照：名额空着时请求应当成功");
  check_eq_i(status_of(r), 200, "对照：名额空着时应当是 200");
  check_eq_i(static_cast<long long>(body_of(r).size()),
             static_cast<long long>(std::strlen(k_text)),
             "对照：body 长度应当对得上");
  check(body_of(r) == k_text, "对照：body 内容应当是文件内容");

  // ---- 占住唯一的名额 → 静态请求必须被回绝 ----
  //
  // 这里是**直接占位**（不是投一个慢任务）：上限是 1，测试把这个名额拿走，
  // 服务端就必然拿不到。503 是确定的，不依赖任何调度时机。
  //
  // 请求的是 `cold.txt` —— 一个从没被请求过的文件，所以这次**必然要读盘**。
  // 这一点不能省：闸门现在只卡"确实要读盘"那一支，换成已缓存的 URL，这条
  // 断言测的就不再是"闸门会不会拒"，而是"缓存会不会命中"。
  check(app.work_limit()->acquire(), "饱和：测试应当能占住唯一的名额");
  check_eq_i(static_cast<long long>(app.work_limit()->in_flight()), 1,
             "饱和：在途数应当是 1");

  uvcpp_http_response r503;
  check(get(port, "/s/cold.txt", r503), "饱和：请求应当拿到响应");
  check_eq_i(status_of(r503), 503, "饱和：名额满且要读盘时应当是 503");

  // `Retry-After` 是 503 的配套语义（RFC 9110 §10.2.3）。**它必须存在** ——
  // 没有它，客户端只能靠猜，最常见的反应是立刻重试，正好把已经饱和的池子
  // 压得更死。所以这条断言和状态码同等重要。
  check(!header_of(r503, "retry-after").empty(),
        "饱和：503 必须带 Retry-After");
  check_eq_i(static_cast<long long>(st->rejected_count()), 1,
             "饱和：被回绝的计数应当是 1");

  // 503 的 body 里**不该有文件内容** —— 状态码对了但内容照发的实现，
  // 会让这条红。
  check(body_of(r503).find(k_text) == std::string::npos,
        "饱和：503 的 body 里不该出现文件内容");

  // ---- 同一时刻、同一个饱和的闸门：**缓存命中必须照常 200** ----
  //
  // 这一条就是本次改动的判据，而且它与上面那条 503 **互为对照** —— 同一个
  // 服务、同一个上限、同一个被占住的名额，差别只在"这个 URL 有没有进过
  // 缓存"。改之前两条都是 503（闸门在 `serve()` 里投递前就抢名额，而缓存
  // 查找在 worker 里、根本轮不到）。只测其中一条分不出"命中绕开闸门"和
  // "闸门整个失效"这两种截然不同的实现。
  uvcpp_http_response rhit;
  check(get(port, "/s/hello.txt", rhit), "饱和-命中：请求应当拿到响应");
  check_eq_i(status_of(rhit), 200,
             "饱和-命中：命中缓存不读盘、不占名额，必须是 200");
  check(body_of(rhit) == k_text, "饱和-命中：内容应当正常");
  check_eq_i(static_cast<long long>(st->rejected_count()), 1,
             "饱和-命中：命中不该被回绝（回绝计数仍为 1）");

  // ---- 还回名额 → 立刻恢复 ----
  app.work_limit()->release();
  check_eq_i(static_cast<long long>(app.work_limit()->in_flight()), 0,
             "恢复：还回之后在途数应当是 0");

  uvcpp_http_response r2;
  check(get(port, "/s/cold.txt", r2), "恢复：还回名额后请求应当成功");
  check_eq_i(status_of(r2), 200, "恢复：还回名额之后应当回到 200");
  check(body_of(r2) == k_text, "恢复：内容应当正常");

  // ---- 名额必须**每次都还**：上限 1 连发 5 个不同的冷文件 ----
  //
  // 五个文件必须**互不相同、且都还没进过缓存**：连发同一个的话，第 1 次
  // 读过之后后面全走缓存命中那一支，`release` 一次都不需要 —— 于是这条
  // 用例会在"漏还名额"的实现上照样全绿，判据就没了。
  //
  // **这是"会 release"唯一的判据。** 只 acquire 不 release 的实现，第一次
  // 请求完全正常，从第二次起全部 503 —— 只跑一次的话根本看不出来。上限压到
  // 1（连发之间**没有任何余量**）让这个泄漏在第 2 次就暴露，而不是等到第 17 次。
  int ok200 = 0;
  for (int i = 0; i < 5; ++i) {
    uvcpp_http_response rr;
    if (get(port, std::string("/s/") + (k_seq_files[i] +
                                       std::strlen(k_root) + 1),
            rr) &&
        status_of(rr) == 200 &&
        body_of(rr) == k_text) {
      ++ok200;
    }
  }
  check_eq_i(ok200, 5,
             "还名额：上限 1 连发 5 次必须全部 200（漏还一次就会从第 2 次起红）");
  check_eq_i(static_cast<long long>(app.work_limit()->in_flight()), 0,
             "还名额：5 次之后在途数应当回到 0");
  check_eq_i(static_cast<long long>(st->rejected_count()), 1,
             "还名额：连发 5 次不该再产生回绝（仍然只有饱和那一次）");

  app.stop();
  app.join();
  cleanup_root();
}

// =========================================================================
// 7. 上限是"准入"而不是"队列"：拒绝之后池子不会被撑大
// =========================================================================
void test_does_not_queue() {
  // 这条测的是**语义**：被拒的请求不该在暗处留下任何痕迹（没有排队、
  // 没有延迟的任务）。判据是"在途数在拒绝前后不变"以及"释放之后立刻可用"。
  uvcpp_web_work_limit lim(1);
  check(lim.acquire(), "不排队：占住名额");

  int accepted = 0;
  for (int i = 0; i < 100; ++i) {
    if (lim.acquire()) ++accepted;
  }
  check_eq_i(accepted, 0, "不排队：占满之后 100 次 acquire 必须全部被拒");
  // 拒绝**不留痕迹**：如果实现把被拒的请求塞进了某个暗处的队列（或者
  // acquire 在失败路径上也把计数加了），账本就会涨 —— 那样"上限"就成了
  // 装饰品：压力照样堆积，只是换了个地方堆。
  check_eq_i(static_cast<long long>(lim.in_flight()), 1,
             "不排队：100 次被拒之后在途数仍然只能是 1（拒绝不留痕迹）");

  lim.release();
  check(lim.acquire(), "不排队：释放之后应当立刻可用");
}

// =========================================================================
// 8. 唤醒 API：名额变松时真的会叫醒等着的人
// =========================================================================
//
// 这一组是上传路径（步骤 5）的**生命线**：拿不到名额时上传会
// `stream->pause()` 等唤醒，而没人叫它就是**永久卡死**。前面的 7 组全部建立在
// "名额"这一个概念上，没有任何一组碰过这条等待通道。
//
// 判据的形状照旧是"对的叫、错的别叫"两条同时断言：只测"会叫醒"的话，一个
// 每次 `release()` 都无脑叫一遍的实现照样能过（见第 9 组）。
void test_wakeup_fires_on_release() {
  uvcpp_web_work_limit lim(1);

  int called = 0;
  bool got_slot = false;
  check(lim.acquire(), "唤醒：先占住唯一的名额");
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 0,
             "唤醒：注册之前表是空的");

  const size_t id = lim.add_wakeup([&]() {
    ++called;
    got_slot = lim.acquire();
  });
  check(id != uvcpp_web_work_limit::INVALID_WAKEUP, "唤醒：注册应当成功");
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 1,
             "唤醒：注册之后表里有 1 条");

  // 名额满了的时候 release 之外没有别的触发点，所以这里叫醒只可能来自
  // `release()` —— 而"只 acquire 不 release 就永远等不到"正是要钉的那件事。
  lim.release();

  check_eq_i(called, 1, "唤醒：release 之后回调必须被调用**恰好一次**");
  check(got_slot, "唤醒：回调里重试 acquire 应当拿到刚还回来的名额");
  check_eq_i(static_cast<long long>(lim.in_flight()), 1,
             "唤醒：回调拿到的名额应当记在账上");

  lim.release();  // 收尾，把回调里拿的那个还回去
}

// =========================================================================
// 9. 唤醒是**一次性**的：叫醒一次就摘掉，不会重复投递
// =========================================================================
void test_wakeup_one_shot() {
  uvcpp_web_work_limit lim(1);

  int called = 0;
  check(lim.acquire(), "一次性：占住名额");
  lim.add_wakeup([&]() { ++called; });

  lim.release();
  check_eq_i(called, 1, "一次性：第一次 release 应当叫醒");
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 0,
             "一次性：被叫醒的条目应当已经从表里摘掉");

  check(lim.acquire(), "一次性：再占一次名额");
  lim.release();
  check_eq_i(called, 1, "一次性：第二次 release **不该**再叫一遍");
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 0,
             "一次性：表里始终是空的");
}

// =========================================================================
// 10. 名额**没变松**时不该叫醒（否则就是无效唤醒风暴）
// =========================================================================
//
// 上限调小之后，在途数可能暂时大于上限 —— `release()` 一次仍然不满足
// `!full()`。此时叫醒等待者只会让它们白跑一次 `acquire()` 再重新注册，而
// 真正的唤醒会在名额**真的**变松那一次到来。这一组钉的就是"别乱叫"。
void test_wakeup_only_when_slot_frees() {
  uvcpp_web_work_limit lim(0);  // 先不限，方便占够

  int called = 0;
  check(lim.acquire(), "不该乱叫：占 1");
  check(lim.acquire(), "不该乱叫：占 2");
  check(lim.acquire(), "不该乱叫：占 3");

  lim.add_wakeup([&]() { ++called; });

  // 上限压到 1：在途 3 → 需要还 3 次才真正不满了。
  lim.set_limit(1);

  lim.release();
  check_eq_i(called, 0, "不该乱叫：在途 2 >= 上限 1 时**不该**叫醒");
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 1,
             "不该乱叫：此时条目应当还在表里等着");

  lim.release();
  check_eq_i(called, 0, "不该乱叫：在途 1 >= 上限 1 时仍然**不该**叫醒");

  lim.release();
  check_eq_i(called, 1, "不该乱叫：在途 0 < 上限 1 时才应当叫醒 —— 恰好一次");
  check_eq_i(static_cast<long long>(lim.in_flight()), 0,
             "不该乱叫：账本应当回到 0");
}

// =========================================================================
// 11. 注销
// =========================================================================
void test_wakeup_remove() {
  uvcpp_web_work_limit lim(1);

  int called = 0;
  check(lim.acquire(), "注销：占住名额");
  const size_t id = lim.add_wakeup([&]() { ++called; });
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 1, "注销：先注册上");

  lim.remove_wakeup(id);
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 0, "注销：条目消失");

  lim.release();
  check_eq_i(called, 0, "注销：注销之后不该再被叫醒");

  // 幂等：重复注销、注销不存在/保留的 id 都是安全空操作 ——
  // 而"安全"必须包含"不会顺手把别人从表里删掉"。
  int keep = 0;
  const size_t keep_id = lim.add_wakeup([&]() { ++keep; });
  lim.remove_wakeup(id);
  lim.remove_wakeup(keep_id + 1000);
  lim.remove_wakeup(uvcpp_web_work_limit::INVALID_WAKEUP);
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 1,
             "注销：无关的注销不该动到别人");
  check(lim.acquire(), "注销：再占一次名额");
  lim.release();
  check_eq_i(keep, 1, "注销：剩下的那条仍然会被叫醒");
}

// =========================================================================
// 12. 空回调必须被拒绝
// =========================================================================
//
// 注册一个"永远什么都不做"的条目，唯一的后果就是唤醒表无声地长胖：每次
// `release()` 都要多查一次表、多拷一个空 `std::function`，而它永远不可能做出
// 任何反应。所以空回调在入口就挡掉，并且**不占用 id**。
void test_wakeup_null_rejected() {
  uvcpp_web_work_limit lim(1);

  check_eq_i(static_cast<long long>(
                 lim.add_wakeup(uvcpp_web_work_wakeup())),
             static_cast<long long>(uvcpp_web_work_limit::INVALID_WAKEUP),
             "空回调：必须返回 INVALID_WAKEUP");
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 0,
             "空回调：不该在表里留下任何条目");

  // 紧接着的一次正常注册必须仍然能用（空回调那一次不该消耗掉任何状态）。
  int called = 0;
  check(lim.acquire(), "空回调：占住名额");
  const size_t id = lim.add_wakeup([&]() { ++called; });
  check(id != uvcpp_web_work_limit::INVALID_WAKEUP, "空回调：正常注册应当成功");
  lim.release();
  check_eq_i(called, 1, "空回调：正常注册的那条照常工作");
}

// =========================================================================
// 13. 从回调里注销**别的**条目：陈旧的快照必须重新查表
// =========================================================================
//
// 派发用的是开始时取的 id 快照。回调里注销别人之后，快照里那个 id 就不该再被
// 调用 —— 少了"调用前重新查一次表"这一步，被注销的回调照样会被跑一遍。
// 触发顺序按 id 升序，所以先注册的那条一定先跑，时序是确定的。
void test_wakeup_cross_remove() {
  uvcpp_web_work_limit lim(1);

  int first = 0;
  int second = 0;
  check(lim.acquire(), "交叉注销：占住名额");

  // 派发按 id 升序，所以**先注册的先跑** —— 要让注销者先跑，它就得先注册。
  // 顺序反过来就什么都测不到（被注销的那条已经跑完了）。
  size_t victim_id = 0;
  const size_t killer_id = lim.add_wakeup([&]() {
    ++first;
    lim.remove_wakeup(victim_id);
  });
  victim_id = lim.add_wakeup([&]() { ++second; });
  check(killer_id != uvcpp_web_work_limit::INVALID_WAKEUP,
        "交叉注销：注销者注册成功");
  check(victim_id != uvcpp_web_work_limit::INVALID_WAKEUP,
        "交叉注销：被注销者注册成功");
  check(killer_id < victim_id, "交叉注销：注销者的 id 必须更小（先跑）");
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 2, "交叉注销：表里 2 条");

  lim.release();
  check_eq_i(first, 1, "交叉注销：注销者应当被叫醒");
  check_eq_i(second, 0,
             "交叉注销：被注销者**不该**再被叫醒（陈旧的快照必须重新查表）");
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 0, "交叉注销：表清空");
}

// =========================================================================
// 14. 回调里**再注册**一条：嵌套 release 不能把这条新条目弄丢
// =========================================================================
//
// 这是唤醒机制里唯一一处"看着不会发生、实际是真漏洞"的地方，也是本轮给
// `notify_wakeups()` 加 `notify_again_` 补跑那一趟的全部理由。
//
// 场景：A 被叫醒 → A 拿到名额 → **A 注册了 B** → A 立刻把名额还回去。
// 那一刻池子重新变空，`release()` 会再喊一次 `notify_wakeups()`。可是：
//
//   - 外层那一趟的 id 快照是**开始**时取的，里面**没有 B**；
//   - 嵌套那一趟会被 `notifying_` 挡掉。
//
// 于是 B 既不在快照里、又叫不到人 —— 而池子里已经**没有任何在途任务**了，
// 那句"反正还有活儿没结算完，它们完成时还会再 release 一次"在这里**不成立**。
// B 就永远等不到人叫它。修法就是让嵌套那一次**记一笔**，外层跑完补一趟。
void test_wakeup_reentrant_registration_not_lost() {
  uvcpp_web_work_limit lim(1);

  int a_calls = 0;
  int b_calls = 0;
  bool b_got_slot = false;
  size_t b_id = uvcpp_web_work_limit::INVALID_WAKEUP;

  check(lim.acquire(), "重入注册：先占住唯一的名额");

  const size_t a_id = lim.add_wakeup([&]() {
    ++a_calls;
    lim.acquire();  // A 拿到名额（此刻满）
    b_id = lim.add_wakeup([&]() {
      ++b_calls;
      b_got_slot = lim.acquire();
    });
    lim.release();  // ← 嵌套 release：池子在这一刻重新变空
  });
  check(a_id != uvcpp_web_work_limit::INVALID_WAKEUP, "重入注册：A 注册成功");

  lim.release();  // ← 触发派发

  check_eq_i(a_calls, 1, "重入注册：A 必须被叫醒");
  check(b_id != uvcpp_web_work_limit::INVALID_WAKEUP,
        "重入注册：A 里注册的 B 必须注册成功");
  check_eq_i(b_calls, 1,
             "重入注册：A 里注册的 B **必须**在同一趟派发里被补上（否则永久饿死）");
  check(b_got_slot, "重入注册：B 醒来后应当能拿到名额");
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 0,
             "重入注册：两条都消费掉了，表应当是空的");

  lim.release();  // 收尾
}

}  // namespace

// =========================================================================
// 7. 流式（大文件）那一支：名额满时**停**，不**拒**
// =========================================================================
//
// 这一组 2026-09-24 反向重写过。原来钉的是"流式也占名额、也 503"，现在钉的
// 是"流式**不** 503"。
//
// **为什么反**：名额的语义是"并发的磁盘读"，可原来分片那条路把一个名额从
// worker 一直攥到 `after_work` —— 而 `after_work` 跑在**循环线程**上。1000 条
// 并发大文件时循环线程搬运不过来、`after_work` 排不上队，名额就被"一个字节都
// 还没读"的请求占着。实测 `ab -c 1000` 打 8 MiB 文件：4000 个请求里 **267 个
// 503（约 6.7%）**，服务端日志是
//
//     工作池已满（在途 0 / 16），拒绝静态请求 /big.bin
//
// **在途 0 却拒绝** —— 那一行就是病灶的现场（日志是事后在循环线程上读的，
// 那时名额早还光了）。同一个装置打 4 KiB 小文件（整读路径，一个都没拒）是
// **0 个 503**，所以"503 主要来自整读路径"这个原本的猜测不成立。
//
// ★ **这个 267 是服务端日志里"拒绝静态请求"的行数，不是 `ab` 的
// `Failed requests`。** 早先记成 3735 是把 `ab` 读错了：它把期望的
// `Document Length` 钉在**第一条**响应上，而那一跑的第一条恰好就是 503
// （body 23 字节）—— 于是后面那些**正常**的 200（8 MiB）反被它算成
// `Failed requests: Length`。三个数要一起看：
//
//     Document Length:    23 bytes
//     Failed requests:    3733   (Length: 3733)
//     Non-2xx responses:   267
//
// 真被拒的是 `Non-2xx responses` / 服务端那 267 行日志；"失败 3733"里装着的
// 是成功。**`ab` 的 `Failed requests` 在这个形状下不是拒绝计数。**
//
// 现在分片路改成**按块借还**：读一块之前拿、读完立刻还；拿不到就停在块边界上
// 等唤醒（走的是与背压同一条 `resume()` 续做路径）。于是名额封的是"真正并行
// 的读盘数"，而 1000 条传输可以**全都在跑** —— 对外的表现是"慢一点"，不再是
// "失败"。
//
// 怎么让一个小文件走流式：`max_cached_file_size = 0` 就是"一律走流式"
// （见该选项的说明），所以这里不需要真造一个大文件。
void test_stream_path_pauses_not_rejects() {
  make_root();

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_work_limit(1);

  uvcpp_web_static_options o;
  o.max_cached_file_size = 0;  // 0 = 一律走分片流式
  std::shared_ptr<uvcpp_web_static> st2 = app.serve_static("/raw", k_root, o);
  check(st2->work_limit().get() == app.work_limit().get(),
        "流式：接线，静态服务装上的应当是 App 那个闸门");

  check(app.start_background() == 0, "流式：服务启动");
  const int port = app.bound_port();

  uvcpp_http_response r;
  check(get(port, "/raw/hello.txt", r), "流式-对照：请求应当成功");
  check_eq_i(status_of(r), 200, "流式-对照：名额空着应当是 200");
  check(body_of(r) == k_text, "流式-对照：内容应当正常");

  // 占住唯一的名额，然后在**同一条连接**上把全程看完：
  //
  //   t=0      请求发出去 —— 它要读盘，拿不到名额
  //   t=0..600 必须**一声不响**（既不是 200 也不是 503）
  //   t=600    在**循环线程**上还名额（`app.post()`；与生产上两处归还点
  //            `after_work` / `on_read_done` 是同一个线程）
  //   t>600    那条挂着的请求被叫醒，自己读完、发完
  //
  // 这样"停"与"续做"由同一条连接钉住，"停"的代价只是**等**，不是失败。
  //
  // ★ 别用 `get()`：它内部要重试 40 次（每次 3 s 超时）⇒ 想看"没响应"得等
  //   两分钟。这里直接用客户端的一次 `send_wait`，超时自己定。
  check(app.work_limit()->acquire(), "流式：占住唯一的名额");
  check_eq_i(static_cast<long long>(app.work_limit()->in_flight()), 1,
             "流式：在途数应当是 1");

  std::atomic<bool> replied(false);              // 响应回来了吗
  std::atomic<bool> replied_at_release(false);   // 还名额的那一刻回来了吗
  std::atomic<bool> released(false);

  uvcpp_http_client probe;
  check(probe.connect_wait("127.0.0.1", port, 2000) == 0, "流式：连得上");

  std::thread releaser([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    // 非循环线程调 `post()` 落到 0 号那条循环 —— 这是它写明的契约。
    app.post([&]() {
      replied_at_release.store(replied.load());
      app.work_limit()->release();
      released.store(true);
    });
  });

  uvcpp_http_response rs;
  const int rc = probe.send_wait(uvcpp_http_request::make_get("/raw/hello.txt"),
                                 rs, 6000);
  replied.store(true);
  releaser.join();

  for (int i = 0; i < 200 && !released.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(released.load(),
        "流式-挂起：那次投递必须真的跑到（否则下面几条全无意义 —— 这是前提断言）");
  check(!replied_at_release.load(),
        "流式-挂起：还名额之前的那 600 ms 里**一个响应都没有** —— 是停，不是拒"
        "（老实现会立刻回 503，这一条会红）");

  check_eq_i(rc, 0, "流式-续做：还回名额之后，挂住的那条必须被叫醒并收尾");
  check_eq_i(status_of(rs), 200, "流式-续做：状态应当是 200（**不是** 503）");
  check(body_of(rs) == k_text, "流式-续做：内容应当正常");
  check(header_of(rs, "retry-after").empty(),
        "流式-续做：这条路**不**该有 Retry-After（它没有被回绝）");

  // 传输读完那一块就把名额还了，所以收尾时在途必须是 0。**这一条钉的是
  // "块级借还成对"**：漏还一次不会让本组红，但会让闸门越用越紧（最后全 503）。
  check_eq_i(static_cast<long long>(app.work_limit()->in_flight()), 0,
             "流式-收尾：块级名额必须成对还掉");

  app.stop();
  app.join();
  cleanup_root();
}

// =========================================================================
// 9. 上限的**来源**：直通核心模块 + 用户显式给过的数不会被 `start()` 改掉
// =========================================================================
//
// 两件互相独立的事，放在一组里因为它们都是"上限这个数字从哪来"：
//
// **① 直通。** 本类的 `threadpool_size()` / `threadpool_size_is_set()` 现在
// 是 `src/uvcpp/uvcpp_threadpool.h` 那两个同名函数的转发，**不再自己重读一遍
// 环境变量**。那一份多记了两件事：libuv 是惰性读的（先决条件比"进程启动前"
// 松），以及投过之后环境变量说什么都不算了（读数是**钉住**的那个）。这里自己
// 再读一遍只会得到"两个答案里更旧的那个"。断言取等，是因为**它就是转发** ——
// 将来谁再在本地实现一遍，这条会红。
//
// **② 显式给过的数不许被 `start()` 覆盖。** `uvcpp_web_app::start()` 在用户
// **没调过** `set_work_limit()` 时会按当时的线程池大小重算一次上限（构造与
// 启动之间池子可能变了）。那个"调过没有"的标记就是为这条设的 —— 判据里最
// 承重的一格是 `set_work_limit(0)`（**明确要不限**）：重算若不分情形地做，
// 结果是把用户**关掉**的闸门又装回去，而且悄无声息。
//
// 这一组**不断言**那条重算本身（"构造时快照"与"启动时重算"在这里给出的数
// 往往相同，分辨不了）；它断言的是重算**不能越过用户**。分辨重算是否真的发生
// 要在**构造与启动之间换一个池子大小**，而池子大小在第一次投递之后就钉住了、
// 换不动了 —— 所以那条只能靠代码读，不能靠用例。
void test_limit_source() {
  // ① 直通
  check_eq_i(static_cast<long long>(uvcpp_web_work_limit::threadpool_size()),
             static_cast<long long>(uvcpp_threadpool_size()),
             "来源：threadpool_size() 应当就是核心模块那个数");
  check(uvcpp_web_work_limit::threadpool_size_is_set() ==
            uvcpp_threadpool_size_is_set(),
        "来源：threadpool_size_is_set() 应当就是核心模块那个判断");
  const long long pool =
      static_cast<long long>(uvcpp_web_work_limit::threadpool_size());
  check_eq_i(static_cast<long long>(uvcpp_web_work_limit::default_limit()),
             pool * 4 < 16 ? 16 : pool * 4, "来源：默认上限 = 池子 × 4（下限 16）");

  // ② 显式给过的数活过 `start()`
  make_root();

  {
    uvcpp_web_app app;
    configure_for_test(app);
    app.set_work_limit(0);  // 明确"不限"
    check(app.start_background() == 0, "来源：显式 0 —— 服务应当启动");
    check_eq_i(static_cast<long long>(app.work_limit()->limit()), 0,
               "来源：显式 0 必须活过 start()（重算不许把它装回去）");
    app.stop();
    app.join();
  }

  {
    uvcpp_web_app app;
    configure_for_test(app);
    const size_t default_now = uvcpp_web_work_limit::default_limit();
    check(app.start_background() == 0, "来源：没设过 —— 服务应当启动");
    check_eq_i(static_cast<long long>(app.work_limit()->limit()),
               static_cast<long long>(default_now),
               "来源：没设过时上限应当就是 default_limit()");
    app.stop();
    app.join();
  }

  cleanup_root();
}

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  const std::string only = (argc > 1) ? argv[1] : std::string();

  struct case_entry {
    const char* name;
    void (*fn)();
  };
  const case_entry cases[] = {
      {"accounting", test_accounting},
      {"unlimited", test_unlimited},
      {"default_limit", test_default_limit},
      {"static_integration", test_static_integration},
      {"stream_pauses_not_rejects", test_stream_path_pauses_not_rejects},
      {"does_not_queue", test_does_not_queue},
      {"wakeup_fires_on_release", test_wakeup_fires_on_release},
      {"wakeup_one_shot", test_wakeup_one_shot},
      {"wakeup_only_when_slot_frees", test_wakeup_only_when_slot_frees},
      {"wakeup_remove", test_wakeup_remove},
      {"wakeup_null_rejected", test_wakeup_null_rejected},
      {"wakeup_cross_remove", test_wakeup_cross_remove},
      {"wakeup_reentrant_registration_not_lost",
       test_wakeup_reentrant_registration_not_lost},
      {"limit_source", test_limit_source},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (!only.empty() && std::string(cases[i].name).find(only) == std::string::npos) {
      continue;
    }
    std::cout << "[worklimit] " << cases[i].name << std::endl;
    cases[i].fn();
    std::cout << (g_failures == 0 ? "  -> PASS" : "  -> FAIL") << std::endl;
    if (g_failures != 0) {
      std::cout << "[worklimit] FAIL" << std::endl;
      return 2;
    }
  }

  std::cout << "[worklimit] ALL PASS" << std::endl;
  return 0;
}

#else
int main() { return 0; }
#endif  // UVCPP_WEBAPP_ENABLE
