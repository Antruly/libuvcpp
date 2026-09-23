/**
 * @file tests/functional/threadpool_func.cpp
 * @brief `uvcpp_threadpool.h` 的功能测试 —— 进程内设 libuv 线程池大小。
 *
 * ## 这里测的是一条**时间轴**，不是一组函数
 *
 * libuv 的线程池大小只有 `UV_THREADPOOL_SIZE` 一个来源，而且是**惰性**读的：
 * `init_threads()` 由第一次 `uv__work_submit()` 经 `uv_once` 触发
 * （`_local_deps/libuv/src/threadpool.c:200` 那句 `getenv` 就在这条路上）。于是"现在
 * 池子有几条线程"这个问题的答案**随时间变化**：
 *
 * ```
 *   投过第一笔活儿之前:  环境变量说什么就是什么（改了就变）
 *   ──────────────────── 投第一笔（libuv 读走并缓存）
 *   投过之后:            libuv 那边定死了，改环境变量**毫无效果**
 * ```
 *
 * 所以本用例的顺序**不是**可以随便排的，它整条就是在走这条时间轴：
 *
 *   1. 纯函数（`uvcpp_default_threadpool_size()`）：与时间无关，随时可断言；
 *   2. **还没投过** —— 环境变量 → 读数的每一档（含 libuv 那三条反直觉的夹法）；
 *   3. `uvcpp_set_threadpool_size(0)` / `(3)` 都应当回 0（此刻还来得及）；
 *   4. **投第一笔**（`uvcpp_work::queue_work`）—— 这一笔把池子定死，同时是本库
 *      池账的记账点；**见证**是数出池子里真有 3 条不同线程在跑；
 *   5. **投过之后** —— `set(5)` 必须回 `UV_EALREADY`，读数仍是 3，而环境变量
 *      确实被写成了 5（那对子进程有意义）；再直接改环境变量也不动读数。
 *
 * 写法上有一条硬约束：**第 2、3 步必须排在第 4 步之前**，而且本进程里不能有
 * 任何别的投递插到中间。这是为什么它是**独立的一个用例文件**（自己的进程），
 * 而不是挂在别的用例里 —— 顺序一旦被别的用例的投递打乱，第 2 步那句断言就会
 * 红在一个与它无关的地方。第 2 步开头有一条**前提断言**把这件事说出来。
 *
 * ## 见证为什么是"数几条不同线程"
 *
 * 环境变量读回来是几、`uvcpp_threadpool_size()` 报几，都是**本库自己的说法**：
 * 我要是把 libuv 的语义理解错了，这些数是**自洽地一起错**的（头文件里那张实测
 * 表就是这么来的）。所以每一档都要有一个不依赖那个理解的终局见证 —— 这里就是
 * 真投一批任务、每个任务在自己的线程上记下 `std::this_thread::get_id()`，最后
 * 数一共有几个不同的 id。池子真的是 3 条，这个数就只能是 3。
 * （`src/uvcpp/uvcpp_threadpool.cpp` 文件头那张表用的是同一套办法：投 32 个
 * 任务、每个睡 120 ms，数池子里跑过几条不同线程。）
 *
 * ## 命名
 *
 * 文件名**不**带 `web_` / `web_app_` / `h2_` 前缀，也不带 `web_ssl_` —— 本用例
 * 只用核心模块（`uvcpp_work` + `uvcpp_threadpool`），与 web / webapp / OpenSSL /
 * nghttp2 四个开关都无关，所以哪一条过滤器都不该命中它。反过来说，一旦有人给
 * 它改名成 `web_threadpool_func.cpp`，"关掉 web 时它被摘掉"就会表现为"这条
 * 用例从此不存在"——那是 `tests/functional/CMakeLists.txt` 里记着的那个老坑
 * （"没测表现为通过"）。
 */
#include <uv.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <req/uvcpp_work.h>
#include <uvcpp/uvcpp_threadpool.h>

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

// =========================================================================
// 环境变量小工具
// =========================================================================

const char* kEnvName = "UV_THREADPOOL_SIZE";

/// @brief 读环境变量；返回"存在且非空"（空串与不存在在这里是同一回事）。
bool env_read(std::string* out) {
  const char* s = std::getenv(kEnvName);
  if (s == nullptr || *s == '\0') return false;
  if (out != nullptr) *out = s;
  return true;
}

/**
 * @brief 写环境变量；**空串表示删除**。
 *
 * 这不是我挑的语义，是两平台的 API 各自的意思，而且**不一样**：
 *
 *   - Windows：`_putenv_s(name, "")` 就是**删除**该变量（MSVC 文档如此，本仓
 *     `web_app_worklimit_func.cpp` 里也写着这一条）；
 *   - POSIX：`setenv(name, "", 1)` 是"设成空串"，变量**存在**，`getenv` 返回
 *     `""`。
 *
 * 于是"设成空串"这件事在 Windows 上根本做不出来 —— 而 libuv 对空串是另一条
 * 分支（`atoi("")` 给 0，`0 → 1`），所以这一档在 Windows 上只能走"未设"那条
 * 路（→ 4）。用例里那一档按平台分开断言，就是因为它。
 */
void env_write(const char* value) {
#ifdef _WIN32
  _putenv_s(kEnvName, value);
#else
  setenv(kEnvName, value, 1);
#endif
}

void env_restore(const std::string& original, bool was_set) {
#ifdef _WIN32
  env_write(was_set ? original.c_str() : "");
#else
  if (was_set) {
    env_write(original.c_str());
  } else {
    unsetenv(kEnvName);
  }
#endif
}

// =========================================================================
// 见证装置：真投一批活儿，数池子里跑过几条不同线程
// =========================================================================

/// @brief 每个见证任务在池线程上停留多久（毫秒）。
///
/// 放在**命名空间作用域**而不是函数里：函数作用域的 `const int` 在 lambda 里
/// 需要显式捕获（它是**变量**，不是常量表达式那种可以免捕获的情形），而命名
/// 空间作用域的 `const int` 有内部链接、能在 lambda 里直接用。
const int kSleepMs = 20;

/**
 * @brief 投 `n` 笔活儿，返回池子里跑过几条**不同**的线程。
 *
 * @return 不同线程数；投递失败或没跑完时返回 0（调用方会因此红）。
 *
 * 每个任务先记线程 id、再睡 `kSleepMs`：不睡的话一个任务可能在别人还没被唤醒
 * 之前就做完了，线程数会被调度时机左右；睡了之后 `n >> 线程数` 个任务必然把
 * 每一条线程都占住过一轮，数出来的就是**池子的规模**，不是"这一刻有几条在忙"。
 *
 * 全部 `n` 笔在拨循环**之前**投完，让它们**同时**在队列里 —— 一次投一笔、
 * 等它做完再投下一笔的话，池子规模是几都只会有 1 条线程跑过。
 */
size_t count_pool_threads(size_t n) {
  uvcpp_loop loop;
  std::vector<std::unique_ptr<uvcpp_work> > works;
  works.reserve(n);

  std::mutex mu;
  std::set<std::thread::id> seen;
  size_t finished = 0;

  for (size_t i = 0; i < n; ++i) {
    std::unique_ptr<uvcpp_work> w(new uvcpp_work());
    w->init();
    const int rc = w->queue_work(
        &loop,
        [&mu, &seen](uvcpp_work*) {
          const std::thread::id me = std::this_thread::get_id();
          {
            std::lock_guard<std::mutex> lk(mu);
            seen.insert(me);
          }
          // 睡在锁**外面**：抱着锁睡会让 n 个任务串成一条，池子规模是几都
          // 只会有 1 条线程出现在 seen 里 —— 那就把见证变成了它自己的伪影。
          std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
        },
        [&mu, &finished, &loop, n](uvcpp_work*, int) {
          bool all = false;
          {
            std::lock_guard<std::mutex> lk(mu);
            all = (++finished == n);
          }
          // 最后一笔的 after 回调里停循环。`loop.stop()` 自己幂等，所以不
          // 需要在锁里调（更不该：它会把同一个锁再抱一次）。
          if (all) loop.stop();
        });
    if (rc != 0) return 0;
    works.push_back(std::move(w));
  }

  loop.run(UV_RUN_DEFAULT);
  if (finished != n) return 0;
  return seen.size();
}

}  // namespace

int main() {
  std::cout << std::unitbuf;

  // 环境变量的原值留下来，收尾时**原样放回去**（不是一律删掉）：CI 或开发者
  // 的 shell 里若设过它，删掉就是把本进程的环境改坏了。
  std::string original;
  const bool was_set = env_read(&original);

  // -----------------------------------------------------------------
  // 1. 默认值：与时间无关的纯函数
  // -----------------------------------------------------------------
  const unsigned int def = uvcpp_default_threadpool_size();
  const unsigned int parallel = uv_available_parallelism();
  check(def >= 1 && def <= 1024, "默认：应当落在 1..1024");
  check_eq_i(static_cast<long long>(def),
             static_cast<long long>(parallel == 0 ? 1 : parallel),
             "默认：应当就是 uv_available_parallelism()（0 兜成 1）");

  // -----------------------------------------------------------------
  // 2. **还没投过**：环境变量 → 读数
  // -----------------------------------------------------------------
  // 前提：这一档断言的是"环境变量改了读数就跟着变"，而那只在**本进程还没往
  // 线程池投过任何一笔**时成立。本文件是一个独立的进程，所以这条前提靠"本
  // 文件里没有任何别的东西先投递"保证 —— 谁要是往前面插一笔投递（或者本用例
  // 被并进别的文件），这一句就会先红，红的是前提，不是下面的读数。
  check(!uvcpp_threadpool_used(),
        "前提：本用例必须在**本进程第一笔池子投递之前**跑");

  if (!was_set) {
    check_eq_i(static_cast<long long>(uvcpp_threadpool_size()), 4,
               "读数：未设 → libuv 的默认 4");
    check(!uvcpp_threadpool_size_is_set(), "读数：未设 → is_set() 为假");
  } else {
    std::cout << "  (注：外部环境已设 " << kEnvName << "=" << original
              << "，跳过「未设」那一档)" << std::endl;
  }

  {
    // 三条**反直觉**的夹法，都是 libuv 的原样行为（见 uvcpp_threadpool.h）：
    //   "0"/"abc" → atoi 给 0 → 1；"-1" → 绕成 unsigned 后被上限夹住 → 1024。
    // 写成逐档列表，是因为"看起来不对劲"正是这几档的要点：它们是**实测**过的
    // libuv 行为，够格当契约，所以这里不许"更合理地"纠正。
    // `expect` 是把 `uvcpp_threadpool.h` 那张表**逐字抄下来**（那张表又是从
    // libuv 源码 + 实测来的）。抄进用例是对的：它在这儿是**规格**，被测实现
    // 只负责照它说。写成表而不是现算，是为了让"哪一档对不上"一目了然。
    struct env_case {
      const char* value;
      long long expect;
      bool is_set;
      const char* what;
    };
    const env_case cases[] = {
        {"7", 7, true, "设成 7 → 7，且 is_set 为真"},
        {"0", 1, false, "设成 0 → 1（atoi 给 0，0→1），且 is_set 为假"},
        {"abc", 1, false, "设成 abc → 1（atoi 给 0），且 is_set 为假"},
        {"-1", 1024, false,
         "设成 -1 → 1024（绕成无符号后被上限夹住），且 is_set 为假"},
        {"5000", 1024, false, "超过 1024 → 夹到 1024，且 is_set 为假"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      env_write(cases[i].value);
      check_eq_i(static_cast<long long>(uvcpp_threadpool_size()), cases[i].expect,
                 std::string("读数：") + cases[i].what);
      check(uvcpp_threadpool_size_is_set() == cases[i].is_set,
            std::string("读数：") + cases[i].what);
    }

    // 空串这一档**按平台分开**，不是因为 libuv 分平台，而是因为"设成空串"这件
    // 事在两平台的 API 里是两回事（见 env_write 的注释）：Windows 上它等于
    // **删除**，于是走的是"未设 → 4"；POSIX 上它真的存在、libuv 读到空串，
    // 走的是"atoi 给 0 → 1"。两条都对，错的是把它当成同一档。
    env_write("");
#ifdef _WIN32
    check_eq_i(static_cast<long long>(uvcpp_threadpool_size()), 4,
               "读数：空串在 Windows 上 = 删除 ⇒ 未设 → 4");
#else
    check_eq_i(static_cast<long long>(uvcpp_threadpool_size()), 1,
               "读数：空串在 POSIX 上真的存在 ⇒ libuv 读到它 ⇒ 1");
#endif
  }

  // -----------------------------------------------------------------
  // 3. 此刻**还来得及**：set() 应当回 0
  // -----------------------------------------------------------------
  check_eq_i(static_cast<long long>(uvcpp_set_threadpool_size(0)), 0,
             "设置：还没投过时 set(0) 应当回 0");
  check_eq_i(static_cast<long long>(uvcpp_threadpool_size()),
             static_cast<long long>(def),
             "设置：set(0) = 按 CPU 核数，读数应当就是默认值");
  check_eq_i(static_cast<long long>(uvcpp_set_threadpool_size(3)), 0,
             "设置：还没投过时 set(3) 应当回 0");
  check_eq_i(static_cast<long long>(uvcpp_threadpool_size()), 3,
             "设置：set(3) 之后读数应当是 3");
  check(uvcpp_threadpool_size_is_set(), "设置：set(3) 之后 is_set() 应当为真");

  // -----------------------------------------------------------------
  // 4. 投第一笔 —— 池子在这里被定死；见证是数出真有 3 条线程
  // -----------------------------------------------------------------
  const size_t kJobs = 32;
  const size_t threads = count_pool_threads(kJobs);
  check_eq_i(static_cast<long long>(threads), 3,
             "见证：投 32 笔之后池子里应当恰好跑过 3 条不同线程");
  check(uvcpp_threadpool_used(), "见证：投过之后池账应当记上");

  // -----------------------------------------------------------------
  // 5. **投过之后**：环境变量说什么都不算数
  // -----------------------------------------------------------------
  check_eq_i(static_cast<long long>(uvcpp_set_threadpool_size(5)),
             static_cast<long long>(UV_EALREADY),
             "晚了：set(5) 应当回 UV_EALREADY（回 0 会让调用方以为生效了）");
  // 环境变量**确实被写上了 5** —— 这一条与下一条必须同时在：一条说"对子进程
  // 和下次启动仍然有意义"，另一条说"对本进程的池子不再有意义"。少了前一条，
  // "写环境变量"这件事可能被悄悄砍掉（`set()` 退化成纯粹的拒绝）。
  check(uvcpp_threadpool_size_is_set(), "晚了：环境变量仍然被写上了（= 5）");
  check_eq_i(static_cast<long long>(uvcpp_threadpool_size()), 3,
             "晚了：读数应当仍是钉住的 3，而不是环境变量现在的 5");

  // 再直接改环境变量也不动 —— 钉住之后它与环境变量无关了。
  env_write("9");
  check_eq_i(static_cast<long long>(uvcpp_threadpool_size()), 3,
             "晚了：直接改环境变量也不该动读数");

  // 而 set() 里那个"按 CPU 核数"的默认分支走的也是同一条路：写环境变量、
  // 然后照旧报 UV_EALREADY。
  check_eq_i(static_cast<long long>(uvcpp_set_threadpool_size(0)),
             static_cast<long long>(UV_EALREADY),
             "晚了：set(0) 同样应当回 UV_EALREADY");
  check_eq_i(static_cast<long long>(uvcpp_threadpool_size()), 3,
             "晚了：set(0) 被拒之后读数不该被那次调用带跑");

  env_restore(original, was_set);

  if (g_failures == 0) {
    std::cout << "[threadpool] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[threadpool] FAIL (" << g_failures << ")" << std::endl;
  return 2;
}
