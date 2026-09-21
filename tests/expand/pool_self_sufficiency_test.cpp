/**
 * @file tests/expand/pool_self_sufficiency_test.cpp
 * @brief 池的自足性：池自己的元数据不能走全局 `operator new`。
 *
 * 为什么要单独一个 exe、而且**不链库**
 * ------------------------------------
 * 判据是「池的元数据有没有绕回全局 new」，而要看这件事就必须**抢过全局
 * new/delete**。链库做不到：库那份 `uvcpp_page_heap.cpp` 里的 `new` 绑的是
 * 它自己那个 CRT 的 `operator new`，exe 里怎么覆盖都接不到。所以这个 exe 把
 * 池的实现文件**直接编进来**（见 `tests/expand/CMakeLists.txt`），也不链
 * `${UVCPP_LIB_TARGET}` —— 那个文件不引用任何 `uv_*` 符号，因此也不需要 libuv 的库。
 *
 * 判据的形状：把重入**抓在手里**，而不是等超时
 * --------------------------------------------
 * 光靠"能不能跑完"是不够的。这个缺陷的原始形态是**卡死**：函数局部静态的
 * 初始化守卫在同一线程里重入不递归，于是表现为 CPU 0、单线程、日志空、进程还在 ——
 * 读数是「什么也没发生」，只能靠超时被抓到，而超时红长得像别的东西。
 *
 * 所以这里在进入池之前置一个**线程局部**标志、池返回后清掉；只要池在分配
 * 途中又回到全局 new，那只能是它自己的元数据 —— 当场报错并 `abort()`，
 * 不必等超时，且报错信息直接点名是哪一边。
 *
 * 标志必须是 `thread_local`：它记的是**同一线程**的重入（那正是死锁的成因），
 * 而另一个线程在池里的时候，本线程正常调用 new 是完全合法的。用一个普通
 * 全局 bool 会把并发当成缺陷。
 *
 * 三处元数据都在这里被走到（且都是**运行中**，不只是启动期）
 * ----------------------------------------------------------
 * | 元数据 | 什么时候新建 | 本用例里由哪一步走到 |
 * |---|---|---|
 * | `impl` | 池第一次被用到 | 第 1 步的第一次分配 |
 * | `thread_cache` | **每个工作线程第一次分配时** | 第 1 步（主线程）、第 3 步（新线程） |
 * | `span_header` | **每次向系统扩 span 时** | 第 1 步就撞上（缓存全空 ⇒ 落到新 span），第 2 步大块另起一个 |
 *
 * 「第 1 步就撞上 `span_header`」不是笔误：新线程的缓存是空的，`thread_cache_pop`
 * 与 `central_cache_pop` 都给空之后落到 `allocate_from_span`，而它必然向系统要
 * 一个新 span。所以这个判据在**第一次分配**上就已经把三处里的三处全盖住了 ——
 * 第 2、3 步是补其余的形状（SUPER 大块的建与删、非主线程的缓存），不是它的前提。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

#include <expand/uvcpp_page_heap.h>

namespace {

/**
 * @brief 「本线程此刻正在池里面」。
 *
 * `thread_local` 不是可选项：死锁的成因是**同一线程**重入，而另一个线程正
 * 在池里时本线程分配是合法的。
 */
thread_local bool t_in_pool = false;

/** @brief 重入的报错。参数只为了说清楚是哪一边、多大。 */
void reentered(const char* pool_op, const char* global_op, size_t n)
{
    std::fprintf(stderr,
                 "FAIL: 池正在 %s 的时候又回到了全局 operator %s（%zu 字节）。\n"
                 "      池的元数据没有走 malloc，于是递归回了池自己 —— 这正是\n"
                 "      `meta_new()` / `meta_delete()` 要消掉的那条路。\n"
                 "      对照：src/expand/uvcpp_page_heap.cpp 的 meta_new/meta_delete。\n",
                 pool_op, global_op, n);
    std::fflush(stderr);
    std::abort();
}

void* pool_new(size_t n)
{
    if (t_in_pool) reentered("alloc", "new", n);
    t_in_pool = true;
    void* p = uvcpp::uvcpp_enterprise_alloc(n);
    t_in_pool = false;
    return p;
}

void pool_delete(void* p)
{
    if (p == nullptr) return;
    if (t_in_pool) reentered("free", "delete", 0);
    t_in_pool = true;
    uvcpp::uvcpp_enterprise_free(p);
    t_in_pool = false;
}

/** @brief 另一个工作线程：只为逼出这个线程自己的 `thread_cache`。 */
void worker(size_t round)
{
    for (size_t i = 0; i < round; ++i) {
        void* p = ::operator new(96);
        if (p == nullptr) {
            std::fprintf(stderr, "FAIL: 工作线程里 new(96) 给了空指针\n");
            std::fflush(stderr);
            std::abort();
        }
        std::memset(p, 0xA5, 96);
        ::operator delete(p);
    }
    std::printf("  PASS: 工作线程 %zu 轮分配/释放走通（并已建出它自己的 thread_cache）\n",
                round);
    std::fflush(stdout);
}

}  // namespace

// =========================================================================
// 全局 new/delete 全覆盖
// =========================================================================
//
// 这就是「把池当分配器用」在应用侧最自然的那个写法，也是这个缺陷的触发条件。
// 十一个变体一个都不能漏：漏一个，池里那种形状的分配就从判据底下溜过去了。

void* operator new(size_t n)
{
    void* p = pool_new(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }

void* operator new(size_t n, const std::nothrow_t&) noexcept { return pool_new(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return pool_new(n); }

void operator delete(void* p) noexcept { pool_delete(p); }
void operator delete[](void* p) noexcept { pool_delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { pool_delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { pool_delete(p); }
void operator delete(void* p, size_t) noexcept { pool_delete(p); }
void operator delete[](void* p, size_t) noexcept { pool_delete(p); }

int main()
{
    // ---------------------------------------------------------------------
    // 第 1 步：小块。走线程缓存那一档。
    //
    // 这一步会触发 `impl`（池单例在第一次 `uvcpp_enterprise_alloc` 里建）与
    // 主线程的 `thread_cache` —— 三处元数据里的两处就在这一次调用里。
    // 修之前，**卡死就发生在这一行**。
    // ---------------------------------------------------------------------
    void* small = ::operator new(64);
    if (small == nullptr) {
        std::fprintf(stderr, "FAIL: new(64) 给了空指针\n");
        return 2;
    }
    std::memset(small, 0x5A, 64);
    ::operator delete(small);
    std::printf("  PASS: new(64)/delete 走通（已建出 impl 与主线程的 thread_cache）\n");
    std::fflush(stdout);

    // ---------------------------------------------------------------------
    // 第 2 步：大块。> 256 KiB 走 SUPER 支，它自己那条路也会向系统要 span；
    // 释放时经 `release_span_to_system` 再把 `span_header` 删掉 —— 所以这一
    // 步同时压到"建"与"删"两端。
    //
    // 它不是判据的前提（第 1 步已经把三处元数据全走过了），但它压的是**运行中
    // 反复发生**的那条路：谁接上全局 new，不是"启动时死一次"，而是每次向系统
    // 扩 span 都要再赌一次。
    // ---------------------------------------------------------------------
    const size_t kBig = 512 * 1024;
    void* big = ::operator new(kBig);
    if (big == nullptr) {
        std::fprintf(stderr, "FAIL: new(512 KiB) 给了空指针\n");
        return 2;
    }
    std::memset(big, 0x3C, kBig);
    ::operator delete(big);
    std::printf("  PASS: new(512 KiB)/delete 走通（span_header 建了又删）\n");
    std::fflush(stdout);

    // ---------------------------------------------------------------------
    // 第 3 步：另一个线程。`thread_cache` 是**每个工作线程第一次分配时**才建的，
    // 所以主线程走过不等于别的线程走过。
    // ---------------------------------------------------------------------
    std::thread t(worker, 64);
    t.join();

    std::printf("PASS: 池的元数据全程没有回到全局 operator new\n");
    std::fflush(stdout);
    return 0;
}
