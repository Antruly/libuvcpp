/**
 * @file src/uvcpp/uvcpp_threadpool.h
 * @brief 进程内设置 libuv 线程池大小（`UV_THREADPOOL_SIZE`）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么需要它
 * ------------
 * libuv 线程池的大小**只有**一个来源：环境变量 `UV_THREADPOOL_SIZE`
 * （`src/threadpool.c:201` 的 `getenv`），没有运行时扩容 API。过去"在我的进程
 * 里把池子调大一点"只能靠在进程外设环境变量。
 *
 * 那进程内 `setenv()` 呢？**在 Windows 上是个静默陷阱**：libuv 读它用的是
 * **CRT 的 `getenv`**，而 `uv_os_setenv()` 在 Windows 上走
 * `SetEnvironmentVariableW`（`src/win/util.c:1377`），写的是 **Win32 进程环境
 * 块** —— 与 CRT 的 `_environ[]` **不是同一份存储**。只调它，会得到一个
 * "设了、读回来也是新值、但池子一点没变"的状态。
 *
 * 这不是照文档推的，是**实测**出来的（libuv 1.51.0 / MSVC / Windows 10，
 * 每一臂都用"真投 32 个任务、数池子里跑过几个不同线程"作终局见证）：
 *
 * | 只写哪一侧 | CRT `getenv`（libuv 读的） | 池子实际线程数 |
 * |---|---|---|
 * | `uv_os_setenv`（Win32 块） | 不变（还是旧的 16） | **16**（没生效） |
 * | `_putenv_s`（CRT 块） | 变成 7 | **7**（生效） |
 *
 * 所以本模块在 Windows 上**两套都写**：`uv_os_setenv` 管 Win32 块（也让**子
 * 进程**继承到），`_putenv_s`/`_putenv` 管 libuv 真正读的那个 CRT 块。
 *
 * 什么时候调才来得及
 * ------------------
 * **不是**"进程启动之前" —— libuv 是**惰性**读的：`init_threads()` 由 `uv_once`
 * 经**第一次 `uv__work_submit()`** 触发（`src/threadpool.c:194-207` 与
 * `:266-271`）。实测同样确认了两头：进程启动时环境变量是 16，在**第一次投递
 * 之前**进程内改成 7，池子就是 7；而投递过一次之后再改，两套环境块都变了、
 * 池子**仍是原来的数**（读过即缓存）。
 *
 * 所以真正的前提是"**在本进程第一次往线程池投递任务之前**"。约定俗成的位置是
 * `main()` 开头 —— 因为任何一次**带回调的** `uv_fs_*`、`uv_queue_work`、
 * `uv_getaddrinfo` 都会把池子启起来，而这三样在本库内部到处都是。
 *
 * 记账与返回值
 * ------------
 * libuv **没有**"池子起没起"的查询接口，本库只能记自己的账。`note_use()` 打在
 * 本库**所有**会把活儿送进线程池的入口上（覆盖面是照 libuv 源码数的，不是估的
 * —— 那边只有 `uv__work_submit()` 一处会读线程数）：
 *
 * | 本库入口 | 底下那条 libuv 路 |
 * |---|---|
 * | `uvcpp_work::queue_work` | `uv_queue_work` |
 * | `uvcpp_fs` 的 36 个异步重载 | `uv_fs_*`（带回调的） |
 * | `uvcpp_fs_poll::start`（两个重载） | `uv_fs_poll_start` 内部那次 `uv_fs_stat` |
 * | `uvcpp_getaddrinfo::getaddrinfo` | `uv_getaddrinfo` |
 * | `uvcpp_getnameinfo::getnameinfo` | `uv_getnameinfo` |
 * | `uvcpp_random::random`（带回调的） | `uv_random` |
 *
 * 这笔账有两个用处：① `uvcpp_set_threadpool_size()` 答得出"现在改还来得及吗"
 * （来不及时 `UV_EALREADY`，**环境变量照样会被写上**，那对子进程有意义）；
 * ② 第一次投递的那一刻把池子大小**钉住**，于是 `uvcpp_threadpool_size()` 在
 * 投过之后报的仍是"libuv 真正会用几条"，而不是环境变量现在说什么。
 *
 * @warning 这笔账**只覆盖本库自己经手的投递**。你如果在调用之前自己用过
 *          `uv_fs_*` / `uv_queue_work` / `uv_getaddrinfo` / `uv_getnameinfo` /
 *          `uv_random`，本库无从得知 —— 那种情况下它会返回 0（"设上了"）而实际
 *          不生效。这也是为什么"在 `main()` 开头、任何 libuv 调用之前调"仍然是
 *          最优解：那种用法不依赖这笔账准确。
 */
#pragma once
#ifndef SRC_UVCPP_UVCPP_THREADPOOL_H
#define SRC_UVCPP_UVCPP_THREADPOOL_H

#include <uvcpp/uvcpp_define.h>

namespace uvcpp {

/**
 * @brief 默认线程池大小：`uv_available_parallelism()`。
 *
 * 用它而不是 `std::thread::hardware_concurrency()`/`uv_cpu_info`：它数的是
 * **本进程能跑到的 CPU**（Linux 上是 `sched_getaffinity()`、Windows 上是进程
 * 亲和性掩码），所以被 cpuset / 亲和性限制住的容器与服务里得到的是**它真正
 * 能用的核数**，不是宿主的总核数。（`std::thread::hardware_concurrency()` 数
 * 的是宿主；`uv_cpu_info` 也是宿主的拓扑。）
 *
 * @note 两条**它自己也会错**的边界，用之前值得知道：① **cgroup 的配额**
 *       （`cpu.max`，不是 cpuset）不反映在亲和性里 ⇒ 那种限制下这个值偏高；
 *       ② Windows 的实测用的是 `GetProcessAffinityMask()`，它只有 64 位掩码
 *       ⇒ 逻辑核超过 64 的机器上会**偏低**（libuv 源码里那句 TODO 就是说这个）。
 *       libuv 只有这一条依据，本库不另造一套。
 * @return 至少 1；上限按 libuv 的 `MAX_THREADPOOL_SIZE`（1024）。
 */
UVCPP_API unsigned int uvcpp_default_threadpool_size();

/**
 * @brief 当前 `UV_THREADPOOL_SIZE` 会起多少条池线程 —— **逐字重演 libuv 的算法**。
 *
 * libuv 在 `init_threads()`（`src/threadpool.c:200-207`）里就是这么算的：
 *
 *     nthreads = 4;                      // ARRAY_SIZE(default_threads)
 *     val = getenv("UV_THREADPOOL_SIZE");
 *     if (val != NULL) nthreads = atoi(val);   // 注意 nthreads 是 **unsigned int**
 *     if (nthreads == 0) nthreads = 1;
 *     if (nthreads > 1024) nthreads = 1024;    // MAX_THREADPOOL_SIZE
 *
 * 于是"读回来的数"有两条反直觉的路，都照上面三行，**不"更合理地"纠正**：
 *
 * - 没设 → **4**；设成空串 / `abc` / `0` → **1**（`atoi` 给 0，`0 → 1`）；
 * - 设成**负数** → **1024**：`atoi("-1")` 是 `-1`，赋给 `unsigned int` 绕成
 *   4294967295，"小于 1024"那步拦不住，于是被 `> 1024` 夹到上限。
 *   `UV_THREADPOOL_SIZE=-1` 起的是 **1024 条线程**，这不是笔误。
 *
 * 这个函数存在的意义就是"说出 libuv 会起几条"，所以它故意不去平滑这些。
 *
 * @note 超出 `int` 范围的值（如 `99999999999`）在 libuv 那边落在 `atoi` 的**未定义
 *       行为**上，不在契约内；本函数不复制 UB，一律按"夹到 1024"给（对一个 ≥1024
 *       的数，夹本来就是它的归宿）。
 *
 * @note **投过活儿之后，环境变量说什么都不算了**（libuv 那会儿已经读走并缓存，
 *       见文件头 D 臂）。所以本函数在第一次投递的那一瞬间把数**钉住**，之后一律
 *       返回钉住的那个 —— 于是它一直是"libuv 真正会用几条"，而不是"环境变量现在
 *       说什么"。想知道是哪一种情形，看 `uvcpp_threadpool_used()`。
 * @return libuv 真正会用（或已经用上）的线程数，1..1024。
 */
UVCPP_API unsigned int uvcpp_threadpool_size();

/**
 * @brief `UV_THREADPOOL_SIZE` 是不是被显式设成了**一个 libuv 会原样采用的数**。
 *
 * 判据就一条：`真实起起来的线程数 == 你写的那个数` 时为真。于是它比
 * `uvcpp_threadpool_size() != 4` 严格：空串 / `abc` / `0`（libuv 会给 1）、
 * 负数与超上限（会给 1024）都算**没设好** → false，而"确实写了 4" → true。
 *
 * 用途是启动时那条 WARN 的分辨：那个函数没设也给 4，分辨不出"没设"和"设成了 4"。
 */
UVCPP_API bool uvcpp_threadpool_size_is_set();

/**
 * @brief 本库有没有往线程池投过活儿（= libuv 那边**多半**已经把大小读进缓存了）。
 *
 * 为真即意味着 `uvcpp_threadpool_size()` 返回的是**钉住**的那个数、且
 * `uvcpp_set_threadpool_size()` 会回 `UV_EALREADY`。
 *
 * 只是本库的账，见文件头那条 @warning：它覆盖 `uvcpp_work::queue_work`、
 * `uvcpp_fs` 的 36 个异步重载、`uvcpp_fs_poll::start` 两个重载、
 * `uvcpp_getaddrinfo`、`uvcpp_getnameinfo`、`uvcpp_random`（带回调的那个）；
 * **别人**直接用 libuv 投的它看不见。
 */
UVCPP_API bool uvcpp_threadpool_used();

/**
 * @brief 在任何一次线程池投递**之前**打一笔（内部用，见文件头"记账"一节）。
 *
 * @note 调用点在**投递之前**，所以"投递当场失败"也会被记上 —— 这个方向是保守
 *       的：宁可让 `uvcpp_set_threadpool_size()` 多说一次 `UV_EALREADY`，也不能
 *       说"设上了"而其实没生效。
 */
UVCPP_API void uvcpp_threadpool_note_use();

/**
 * @brief 把 libuv 线程池大小设成 `n`（进程内，通常摆在 `main()` 开头）。
 *
 * @param n 线程数；**`n <= 0` → 按 `uvcpp_default_threadpool_size()`**（本进程
 *          能用到的 CPU 数）。大于 1024 按 1024 算（libuv 自己的上限）。
 *
 * @return `0` 设上了（此刻本库还没投过活儿 ⇒ libuv 下次投递会读到它）；
 *         `UV_EALREADY` **已经晚了**：本库账上已经投递过，libuv 读过即缓存，
 *                      再改不会生效（**环境变量仍然被写入** —— 那对子进程有意义，
 *                      且不影响返回值语义）；
 *         `UV_EINVAL` **CRT 那一份没有写上**（`_putenv_s`/`_putenv` 失败）——
 *                      那一份才是 libuv 读的，它没写上 ⇒ 本进程**一定**不生效；
 *         其他负值 `uv_os_setenv()` 的错误码（Win32 块没写上：本进程仍可能生效，
 *                      但子进程继承不到）。
 *
 * 三种失败里**先判 CRT 那一份**：它是承重的，`uv_os_setenv` 失败只影响子进程。
 *
 * @note 反复调用是安全的：来不及时重设也只是重复写一次环境变量。
 * @note 内部要改的是**进程环境**（`_putenv_s`/`setenv` 不是线程安全的），所以
 *       只该在启动那段单线程窗口里调 —— 与"在第一次投递之前"是同一个窗口。
 */
UVCPP_API int uvcpp_set_threadpool_size(int n);

}  // namespace uvcpp

#endif  // SRC_UVCPP_UVCPP_THREADPOOL_H
