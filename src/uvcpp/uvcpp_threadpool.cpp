/**
 * @file src/uvcpp/uvcpp_threadpool.cpp
 * @brief 见 uvcpp_threadpool.h（为什么需要它、什么时候调才来得及、返回码各是什么意思）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这里只放两件头文件里塞不下的东西：Windows 那套"两套环境块"的**实测证据**，
 * 与"读回来的数"为什么必须逐字照抄 libuv 的算法。
 *
 * ## 实测：Windows 上只写一侧会静默失败
 *
 * 探针 `tp_probe.c`（libuv 1.51.0 / MSVC 14.44 / Windows 10，五臂同一条 exe）。
 * 每一臂的最后一步都**不看环境变量**：真投 32 个 `uv_queue_work`（每个在任务里
 * 记下线程 id 并睡 120 ms），数出池子里实际跑过几条不同线程 —— 这个终局见证不
 * 依赖我对 CRT 语义的理解，我读错了它也照样作数。这台机器 `HKCU\Environment`
 * 里本来就设着 `UV_THREADPOOL_SIZE=16`，所以基线臂是 16 而不是 4。
 *
 *     臂  做了什么                CRT getenv   Win32 块   池子线程数
 *     N   都不设                    (null)      (null)     16
 *     A   只 uv_os_setenv("..","7") 16          7          16   ← 设了、读回来也是 7、池子没变
 *     B   只 _putenv_s("..","7")     7          16          7
 *     C   两套都写                  7           7          7
 *     D   先投一批，再两套都写       7           7         16   ← 第一批 16，改完第二批**还是** 16
 *
 * A 臂就是那个静默失败：`uv_os_setenv` 在 Windows 上是 `SetEnvironmentVariableW`
 * （`src/win/util.c:1377`），写的是 Win32 进程环境块；libuv 读的是 CRT 的
 * `getenv`（`src/threadpool.c:201`）。两处存储不同源，于是"设成功但没生效"。
 * D 臂是另一半：投过第一次之后，环境变量两边都变了，池子照旧（`uv_once` 读过即缓存）。
 *
 * 结论落成代码就是下面 `write_crt_env()` + `uv_os_setenv()` 这两句必须成对出现。
 */
#include "uvcpp_threadpool.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <stdlib.h>  // _putenv_s / _putenv
#endif

namespace uvcpp {

namespace {

const char kEnvName[] = "UV_THREADPOOL_SIZE";

// libuv `src/threadpool.c:30` 的 MAX_THREADPOOL_SIZE
const unsigned int kLibuvMaxThreadpoolSize = 1024;
// libuv `src/threadpool.c:39` 的 default_threads[] 的长度
const unsigned int kLibuvDefaultThreadpoolSize = 4;

// libuv **读走那一刻**的池子大小；0 = 还没投过活儿（0 不是合法线程数，拿它当哨兵）。
//
// 为什么要把它记下来："libuv 真正会用几条线程"这个问题在投过之后**只看环境变量
// 答不出来**了 —— 那时环境变量改了也不生效（下面那张表里的 D 臂）。把这个数停在
// 第一次投递的那一瞬间，等于把它钉在"libuv 同一条指令后读到的值"上：本库投递点
// 就在 `uv__work_submit` 前面，读的是同一份环境变量。
//
// 竞态是良性的（最坏情况是"已经晚了"被说成"设上了"），但它**必须是原子**：
// 写它发生在循环线程上（投递点），读它的地方可能在主线程上。
std::atomic<unsigned int> g_pool_size_pinned(0);

/**
 * 把值写进**CRT**那份环境 —— libuv 真正读的就是它。
 *
 * MSVC 用 `_putenv_s`；MinGW 没有它（mingw-w64 的 `<stdlib.h>` 里只有 `_putenv`），
 * 用 `_putenv`，而它**不复制**传进来的那块内存 ⇒ 得让那串活到进程末尾，
 * 所以用函数级 static 而不是局部数组。
 *
 * Unix 下这里是**空操作**：`uv_os_setenv` 就是 `setenv`，写的与 libuv 读的
 * 是同一份 `environ`，没有"两套存储"这回事，所以不按平台分叉。
 *
 * @return 0 成功；非 0 是 `_putenv_s`/`_putenv` 的失败码（MSVC 给 errno_t）。
 */
int write_crt_env(const char* value) {
#if defined(_MSC_VER)
  return _putenv_s(kEnvName, value);
#elif defined(_WIN32)
  static std::string line;  // `_putenv` 不复制，必须让它活到进程末尾
  line.assign(kEnvName);
  line.push_back('=');
  line.append(value);
  return _putenv(line.c_str());
#else
  (void)value;
  return 0;
#endif
}

/**
 * 按当前环境变量算 libuv 会起几条（**逐字重演** `init_threads()`，
 * `src/threadpool.c:200-207`，含它的夹法）。
 *
 * 与 `uvcpp_threadpool_size()` 的分工：这个是"环境变量现在说什么"，那个是
 * "libuv 真正会用几条" —— 投过活儿之后两者会分叉（读了就缓存），见文件头。
 *
 * 刻意**不**改成更合理的读数：那边 `nthreads` 是 `unsigned int`，`atoi` 拿到
 * 负数会绕成天文数字，于是 `-1` 不是"非法值"而是被夹到 1024（见头文件那张表）。
 * 唯一没照抄的是超范围那一步 —— 那边是 `atoi` 的 UB，这里用 strtol，按"夹到
 * 1024"给（对一个 >=1024 的输入，两边一样）。
 */
unsigned int env_threadpool_size() {
  unsigned int nthreads = kLibuvDefaultThreadpoolSize;
  const char* val = std::getenv(kEnvName);
  if (val != nullptr) {
    const long parsed = std::strtol(val, nullptr, 10);
    if (parsed < 0) {
      // 负数在 libuv 那边绕成 unsigned 后被上限夹住，结果就是上限
      nthreads = kLibuvMaxThreadpoolSize;
    } else if (parsed > static_cast<long>(kLibuvMaxThreadpoolSize)) {
      nthreads = kLibuvMaxThreadpoolSize;
    } else {
      nthreads = static_cast<unsigned int>(parsed);
    }
  }
  if (nthreads == 0) nthreads = 1;
  return nthreads;
}

}  // namespace

unsigned int uvcpp_default_threadpool_size() {
  unsigned int n = uv_available_parallelism();
  if (n == 0) n = 1;  // 它自己保证 >=1；这里只是不让 0 漏过去
  if (n > kLibuvMaxThreadpoolSize) n = kLibuvMaxThreadpoolSize;  // libuv 那边也会夹
  return n;
}

unsigned int uvcpp_threadpool_size() {
  // 投过活儿之后，环境变量说什么都不算数了（libuv 那会儿已经读走并缓存）：
  // 那时"libuv 真正会用几条"的答案就是钉住的那个数。
  const unsigned int pinned = g_pool_size_pinned.load(std::memory_order_relaxed);
  if (pinned != 0) return pinned;
  return env_threadpool_size();
}

bool uvcpp_threadpool_size_is_set() {
  const char* val = std::getenv(kEnvName);
  if (val == nullptr) return false;
  // ⟺ "libuv 会原样采用你写的这个数"：空串/`abc`/`0` 在那边会变成 1，负数与
  // 超上限会变成 1024 —— 都不是你写的数，所以都不算"设上了"（见头文件）。
  const long v = std::strtol(val, nullptr, 10);
  return v >= 1 && v <= static_cast<long>(kLibuvMaxThreadpoolSize);
}

bool uvcpp_threadpool_used() {
  return g_pool_size_pinned.load(std::memory_order_relaxed) != 0;
}

void uvcpp_threadpool_note_use() {
  // 钉的是**这一刻**环境变量说的数 —— 本库的投递点就在 `uv__work_submit`
  // 前面一两行，libuv 读的是同一份环境变量，所以这就是它缓存的那个值。
  // 已经钉住了就别动（`compare_exchange` 的 `expected` 非 0 时它会失败）。
  unsigned int expected = 0;
  g_pool_size_pinned.compare_exchange_strong(
      expected, env_threadpool_size(),
      std::memory_order_relaxed, std::memory_order_relaxed);
}

int uvcpp_set_threadpool_size(int n) {
  int want = n;
  if (want <= 0) {
    // "默认 = 本进程能跑到的 CPU 数"：与 uvcpp_default_threadpool_size() 同一来源
    want = static_cast<int>(uvcpp_default_threadpool_size());
  }
  if (want > static_cast<int>(kLibuvMaxThreadpoolSize)) {
    want = static_cast<int>(kLibuvMaxThreadpoolSize);
  }

  char buf[16];  // 10 位十进制 + NUL
  std::snprintf(buf, sizeof(buf), "%d", want);

  // 承重的是第一句：CRT 那份才是 libuv 读的（实测表里的 A 臂）。
  // 第二句管 Win32 块 —— 本进程的池子不看它，但**子进程**会继承到，
  // 且 Unix 下这两件事是同一件事（写一次就够，write_crt_env 是空操作）。
  const int crt_rc = write_crt_env(buf);
  const int rc = uv_os_setenv(kEnvName, buf);
  if (crt_rc != 0) return UV_EINVAL;
  if (rc != 0) return rc;

  // libuv 没有"池子起没起"的查询接口，只能拿本库自己的账回答（= 有没有钉住过
  // 池子大小）。来不及时**照样**写环境变量（上面已经写了）：那对子进程仍然有意义。
  if (g_pool_size_pinned.load(std::memory_order_relaxed) != 0) return UV_EALREADY;
  return 0;
}

}  // namespace uvcpp
