/**
 * @file tests/expand/memory_pool_test.cpp
 * @brief Unit tests for the expand module (uvcpp_memory_pool, uvcpp_memory_pool_span).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

#include <uvcpp/uvcpp_alloc.h>
#include <expand/uvcpp_memory_pool.h>
#include <expand/uvcpp_memory_pool_span.h>
#include <expand/uvcpp_page_heap.h>

// Simple test helper: exit with error if condition is false
#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d): %s\n", __FUNCTION__, __LINE__, msg); \
        std::exit(2); \
    } \
} while(0)

#define TEST_PASS() std::printf("  PASS: %s\n", __FUNCTION__)

// ==================== uvcpp_memory_pool tests ====================

static void test_pool_init_shutdown()
{
    uvcpp::uvcpp_memory_pool pool;
    TEST_ASSERT(!pool.is_initialized(), "pool should not be initialized by default ctor");

    bool ok = pool.init();
    TEST_ASSERT(ok, "init() should succeed");
    TEST_ASSERT(pool.is_initialized(), "pool should be initialized after init()");

    // Double init should be harmless
    ok = pool.init();
    TEST_ASSERT(ok, "double init() should succeed");

    pool.shutdown();
    TEST_ASSERT(!pool.is_initialized(), "pool should not be initialized after shutdown()");

    TEST_PASS();
}

static void test_pool_alloc_free_basic()
{
    uvcpp::uvcpp_memory_pool pool;
    pool.init();

    // Tiny allocation (<= 64B)
    void* p1 = pool.allocate(32);
    TEST_ASSERT(p1 != nullptr, "allocate(32) should succeed");
    std::memset(p1, 0xAB, 32);
    pool.deallocate(p1);

    // Small allocation (64B - 256B)
    void* p2 = pool.allocate(128);
    TEST_ASSERT(p2 != nullptr, "allocate(128) should succeed");
    std::memset(p2, 0xCD, 128);
    pool.deallocate(p2);

    // Medium allocation (1KB - 4KB)
    void* p3 = pool.allocate(2048);
    TEST_ASSERT(p3 != nullptr, "allocate(2048) should succeed");
    std::memset(p3, 0xEF, 2048);
    pool.deallocate(p3);

    // Large allocation (> 256KB, SUPER type)
    void* p4 = pool.allocate(300 * 1024); // 300KB
    TEST_ASSERT(p4 != nullptr, "allocate(300KB) should succeed");
    std::memset(p4, 0x11, 300 * 1024);
    pool.deallocate(p4);

    pool.shutdown();
    TEST_PASS();
}

static void test_pool_zero_size()
{
    uvcpp::uvcpp_memory_pool pool;
    pool.init();

    void* p = pool.allocate(0);
    TEST_ASSERT(p == nullptr, "allocate(0) should return nullptr");

    pool.deallocate(nullptr); // should be safe

    pool.shutdown();
    TEST_PASS();
}

static void test_pool_aligned_alloc()
{
    uvcpp::uvcpp_memory_pool pool;
    pool.init();

    // allocate_aligned rounds up size, returns pointer at default pool alignment (16-byte)
    void* p1 = pool.allocate_aligned(100, 16);
    TEST_ASSERT(p1 != nullptr, "allocate_aligned(100, 16) should succeed");
    TEST_ASSERT((reinterpret_cast<uintptr_t>(p1) % 16) == 0, "pointer should be 16-byte aligned");
    std::memset(p1, 0xAA, 100);
    pool.deallocate(p1);

    void* p2 = pool.allocate_aligned(7, 16);
    TEST_ASSERT(p2 != nullptr, "allocate_aligned(7, 16) should succeed");
    TEST_ASSERT((reinterpret_cast<uintptr_t>(p2) % 16) == 0, "pointer should be 16-byte aligned");
    pool.deallocate(p2);

    pool.shutdown();
    TEST_PASS();
}

static void test_pool_batch_alloc()
{
    uvcpp::uvcpp_memory_pool pool;
    pool.init();

    std::vector<void*> ptrs = pool.allocate_batch(64, 10);
    TEST_ASSERT(ptrs.size() == 10, "batch alloc should return 10 pointers");
    for (auto* p : ptrs) {
        TEST_ASSERT(p != nullptr, "batch alloc pointer should not be null");
    }

    pool.deallocate_batch(ptrs);
    TEST_ASSERT(ptrs.empty(), "batch dealloc should clear the vector");

    pool.shutdown();
    TEST_PASS();
}

static void test_pool_stats()
{
    uvcpp::uvcpp_memory_pool pool;
    pool.init();

    // Initial stats
    TEST_ASSERT(pool.active_allocations() == 0, "initial active_allocations should be 0");

    void* p = pool.allocate(128);
    TEST_ASSERT(pool.active_allocations() == 1, "active_allocations should be 1 after alloc");

    pool.deallocate(p);
    // Note: after dealloc, the block goes to thread cache, so active_allocations may still be 1
    // Just check that get_stats() doesn't crash
    uvcpp::memory_pool_stats stats = pool.get_stats();
    TEST_ASSERT(stats.total_allocations > 0, "total_allocations should be > 0");

    pool.shutdown();
    TEST_PASS();
}

static void test_pool_nullptr_free()
{
    uvcpp::uvcpp_memory_pool pool;
    pool.init();

    // Freeing nullptr should be safe
    pool.deallocate(nullptr);
    pool.deallocate(nullptr);

    pool.shutdown();
    TEST_PASS();
}

static void test_pool_reuse()
{
    uvcpp::uvcpp_memory_pool pool;
    pool.init();

    // Allocate and free the same size multiple times to trigger cache reuse
    const int N = 100;
    void* ptrs[100];
    for (int i = 0; i < N; i++) {
        ptrs[i] = pool.allocate(64);
        TEST_ASSERT(ptrs[i] != nullptr, "repeated alloc should succeed");
        std::memset(ptrs[i], 0, 64); // touch memory
    }
    for (int i = 0; i < N; i++) {
        pool.deallocate(ptrs[i]);
    }

    // After freeing all, we should be able to allocate them again from cache
    for (int i = 0; i < N; i++) {
        ptrs[i] = pool.allocate(64);
        TEST_ASSERT(ptrs[i] != nullptr, "re-alloc from cache should succeed");
    }
    for (int i = 0; i < N; i++) {
        pool.deallocate(ptrs[i]);
    }

    pool.shutdown();
    TEST_PASS();
}

static void test_pool_thread_cache()
{
    uvcpp::uvcpp_memory_pool pool;
    pool.init();

    // Initialize thread cache for current thread
    pool.init_thread_cache();

    // Check that thread cache size is reportable
    size_t cache_size = pool.thread_cache_size();
    (void)cache_size; // may be 0 if nothing cached yet

    // Release thread cache
    pool.release_thread_cache();

    pool.shutdown();
    TEST_PASS();
}

static void test_pool_multithread()
{
    uvcpp::uvcpp_memory_pool pool;
    pool.init();

    std::atomic<bool> start{false};
    std::atomic<int> errors{0};
    const int num_threads = 4;
    const int allocs_per_thread = 500;

    auto worker = [&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < allocs_per_thread; i++) {
            void* p = pool.allocate(64 + (i % 10) * 16);
            if (!p) {
                errors.fetch_add(1);
                continue;
            }
            std::memset(p, 0x42, 64);
            pool.deallocate(p);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(worker);
    }

    start.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }

    TEST_ASSERT(errors.load() == 0, "multithreaded alloc should have no errors");

    pool.shutdown();
    TEST_PASS();
}

// ==================== uvcpp_memory_pool_span tests ====================

static void test_span_alloc_free_basic()
{
    auto& span_pool = uvcpp::uvcpp_memory_pool_span::instance();

    void* p1 = span_pool.alloc(128);
    TEST_ASSERT(p1 != nullptr, "span alloc(128) should succeed");
    std::memset(p1, 0xAB, 128);
    span_pool.free_mem(p1);

    void* p2 = span_pool.alloc(4096);
    TEST_ASSERT(p2 != nullptr, "span alloc(4096) should succeed");
    std::memset(p2, 0xCD, 4096);
    span_pool.free_mem(p2);

    TEST_PASS();
}

static void test_span_reuse()
{
    auto& span_pool = uvcpp::uvcpp_memory_pool_span::instance();

    // Allocate and free same size, should trigger freelist reuse
    void* p1 = span_pool.alloc(128);
    TEST_ASSERT(p1 != nullptr, "first alloc should succeed");
    std::memset(p1, 0xAA, 128);
    span_pool.free_mem(p1);

    void* p2 = span_pool.alloc(128);
    TEST_ASSERT(p2 != nullptr, "second alloc should succeed (from freelist)");
    std::memset(p2, 0xBB, 128);
    span_pool.free_mem(p2);

    TEST_PASS();
}

static void test_span_nullptr_free()
{
    auto& span_pool = uvcpp::uvcpp_memory_pool_span::instance();
    span_pool.free_mem(nullptr); // should not crash
    span_pool.free_mem(nullptr);
    TEST_PASS();
}

// ==================== uvcpp_page_heap (enterprise) tests ====================

static void test_enterprise_alloc_free()
{
    // Small allocation
    void* p1 = uvcpp::uvcpp_enterprise_alloc(32);
    TEST_ASSERT(p1 != nullptr, "enterprise alloc(32) should succeed");
    std::memset(p1, 0x12, 32);
    uvcpp::uvcpp_enterprise_free(p1);

    // Medium allocation
    void* p2 = uvcpp::uvcpp_enterprise_alloc(4096);
    TEST_ASSERT(p2 != nullptr, "enterprise alloc(4096) should succeed");
    std::memset(p2, 0x34, 4096);
    uvcpp::uvcpp_enterprise_free(p2);

    // Large allocation (> 256KB)
    void* p3 = uvcpp::uvcpp_enterprise_alloc(300 * 1024);
    TEST_ASSERT(p3 != nullptr, "enterprise alloc(300KB) should succeed");
    std::memset(p3, 0x56, 300 * 1024);
    uvcpp::uvcpp_enterprise_free(p3);

    TEST_PASS();
}

static void test_enterprise_get_block_size()
{
    auto& instance = uvcpp::uvcpp_memory_pool_enterprise::instance();
    size_t bs = instance.get_block_size(128);
    TEST_ASSERT(bs >= 128, "block size should be >= requested size");

    size_t bs2 = instance.get_block_size(512);
    TEST_ASSERT(bs2 >= 512, "block size for 512 should be >= 512");

    TEST_PASS();
}

static void test_enterprise_stats()
{
    auto& instance = uvcpp::uvcpp_memory_pool_enterprise::instance();
    size_t total = 0, in_use = 0, free_spans = 0;
    instance.get_stats(total, in_use, free_spans);
    // Just verify the call doesn't crash
    (void)total; (void)in_use; (void)free_spans;

    // Check pressure level
    auto level = instance.get_pressure_level();
    (void)level; // just verify it returns without crashing

    TEST_PASS();
}

// ==================== 池语义：统计量必须真的跟着分配走 ====================
//
// 下面两条是池**语义**的用例（量的是 `get_stats()`），原来住在
// tests/functional/expand_memory_pool_func.cpp 里、包在 `#if UVCPP_ENABLE_MEMORY_POOL`
// 里 —— 池关掉时它们从 stdout 里**静默少掉两行**，而 ctest 照旧报那个文件 PASS。
// 按 tests/functional/CMakeLists.txt:38-40 的口径（"关掉某模块就把它的用例从目标
// 列表里摘掉，让「没测」表现为用例不存在，而不是表现为通过"），它们该待在这个由
// UVCPP_BUILD_EXPAND 门控的目录里：池关 ⇒ 整个目录不建，"没测"表现为**目标不存在**。

// 回归的是**只漏不崩**的缺陷，所以它比死循环/崩溃的活得更久：`allocate_large_object()`
// 建好 span 之后没给 `span->in_use` 记账，而判断"这是不是最后一个使用者"用的正是
// `span->in_use.fetch_sub(1) == 1` —— 从 0 开始减，返回值永远不是 1，
// `release_span_to_system()` 一次都不被调用，于是**每一次大块分配都整段泄漏**。
// 既有用例抓不到它，是因为它们的尺寸数组最大是 262144，而大块路径的门槛是
// `size > k_large_size_threshold`(= 262144) —— 差一个字节。
static void test_enterprise_large_object_returned()
{
    auto& pool = uvcpp::uvcpp_memory_pool_enterprise::instance();

    size_t before_total = 0, before_in_use = 0, before_free = 0;
    pool.get_stats(before_total, before_in_use, before_free);

    const size_t kBig = 300 * 1024;   // > 256 KiB ⇒ 走大块路径
    const int kRounds = 8;
    for (int i = 0; i < kRounds; ++i) {
        void* p = uvcpp::uvcpp_alloc_bytes(kBig);
        TEST_ASSERT(p != nullptr, "300 KiB alloc should succeed");
        std::memset(p, 0xA5, kBig);
        uvcpp::uvcpp_free_bytes(p);
    }

    size_t after_total = 0, after_in_use = 0, after_free = 0;
    pool.get_stats(after_total, after_in_use, after_free);

    // 留 1 MiB 余量给 span 管理与线程缓存；有缺陷时这里会是 kRounds * 300+ KiB。
    const size_t growth = after_total > before_total ? after_total - before_total : 0;
    if (growth > 1024 * 1024) {
        std::string msg = "大块释放后没有归还：" + std::to_string(growth) +
                          " 字节仍挂在本进程上（" + std::to_string(kRounds) + " 轮 × " +
                          std::to_string(kBig) + " 字节）";
        TEST_ASSERT(false, msg.c_str());
    }

    TEST_PASS();
}

// 「在用块数」必须跟着分配/释放走。
//
// 缺陷形状：`g_in_use` 只在两条**冷路径**上 +1（`allocate_from_span`、
// `allocate_large_object`），而从 thread cache / central cache **命中**的分配一个计数
// 都不动；`free_mem()` 却**每次释放都 -1**。稳态下块都从缓存里出，于是这个计数只减不增、
// 往负数漂 —— 未修的库上实测 `-75`（读数 `18446744073709551541`），对 64 字节的分配
// 「根本不响应」：持有 32 块只涨 0。
//
// 判据量的是**增量**：`g_in_use` 是文件作用域 atomic、进程级累计，别的用例早就动过它，
// 绝对值没有意义。
//
// 两次读数之间**不能插入任何分配** —— 这个量具量的就是本进程的分配数，自己构造一次
// `std::string` 就等于把读数顶偏。所以下面的诊断串只在失败分支里造。
static void test_enterprise_stats_in_use_follows_alloc()
{
    auto& pool = uvcpp::uvcpp_memory_pool_enterprise::instance();

    const size_t kSmall = 64;          // 小块：稳态下由 thread cache 供块
    const int    kCount = 32;
    const size_t kBig   = 300 * 1024;  // > 256 KiB ⇒ 走大对象路径

    // 先热身，让 thread cache 备好块 —— 缺陷正在"缓存命中"这条路上，不热身量不到它。
    for (int i = 0; i < kCount; ++i) {
        void* p = uvcpp::uvcpp_alloc_bytes(kSmall);
        TEST_ASSERT(p != nullptr, "warm-up alloc should succeed");
        uvcpp::uvcpp_free_bytes(p);
    }

    size_t t0 = 0, before = 0, f0 = 0;
    pool.get_stats(t0, before, f0);

    void* blocks[kCount];
    int held = 0;
    for (; held < kCount; ++held) {
        blocks[held] = uvcpp::uvcpp_alloc_bytes(kSmall);
        if (blocks[held] == nullptr) break;
    }
    size_t t1 = 0, during = 0, f1 = 0;
    pool.get_stats(t1, during, f1);
    const long long grew = static_cast<long long>(during) - static_cast<long long>(before);

    // 大对象路径是另一条路：同一条判据，换一条路再量一次。
    void* big = uvcpp::uvcpp_alloc_bytes(kBig);
    size_t t2 = 0, with_big = 0, f2 = 0;
    pool.get_stats(t2, with_big, f2);
    const long long big_grew = static_cast<long long>(with_big) - static_cast<long long>(during);

    // 先把块全还回去，再判 —— 这几条都是量增量的用例，留着残块会把后面的量偏。
    if (big) uvcpp::uvcpp_free_bytes(big);
    for (int i = 0; i < held; ++i) uvcpp::uvcpp_free_bytes(blocks[i]);

    size_t t3 = 0, after = 0, f3 = 0;
    pool.get_stats(t3, after, f3);

    if (held != kCount || grew != kCount) {
        std::string msg = "持有 " + std::to_string(held) + " 个 " + std::to_string(kSmall) +
                          " 字节块，在用块数只涨了 " + std::to_string(grew) + "（期望 " +
                          std::to_string(kCount) + "）；读数 " + std::to_string(before) +
                          " -> " + std::to_string(during);
        TEST_ASSERT(false, msg.c_str());
    }
    TEST_ASSERT(big != nullptr, "300 KiB alloc should succeed");
    if (big_grew != 1) {
        std::string msg = "一个 " + std::to_string(kBig) + " 字节的大对象只让在用块数涨了 " +
                          std::to_string(big_grew) + "（期望 1）；读数 " +
                          std::to_string(during) + " -> " + std::to_string(with_big);
        TEST_ASSERT(false, msg.c_str());
    }
    if (after != before) {
        std::string msg = "全部释放之后在用块数没回到原读数：" + std::to_string(before) +
                          " -> " + std::to_string(after) + "（差 " +
                          std::to_string(static_cast<long long>(after) -
                                         static_cast<long long>(before)) + "）";
        TEST_ASSERT(false, msg.c_str());
    }

    TEST_PASS();
}

// ==================== uvcpp_alloc integration test ====================

static void test_uvcpp_alloc_integration()
{
    // Test that uvcpp_alloc uses enterprise allocator when UVCPP_ENABLE_MEMORY_POOL=1
    int* p = uvcpp::uvcpp_alloc<int>();
    TEST_ASSERT(p != nullptr, "uvcpp_alloc<int> should succeed");
    *p = 42;
    uvcpp::uvcpp_free(p);

    // Array allocation
    char* arr = uvcpp::uvcpp_alloc_arry<char>(100);
    TEST_ASSERT(arr != nullptr, "uvcpp_alloc_arry<char> should succeed");
    std::memset(arr, 0, 100);
    uvcpp::uvcpp_free(arr);

    // Bytes allocation
    void* bytes = uvcpp::uvcpp_alloc_bytes(256);
    TEST_ASSERT(bytes != nullptr, "uvcpp_alloc_bytes should succeed");
    std::memset(bytes, 0, 256);
    uvcpp::uvcpp_free_bytes(bytes);

    TEST_PASS();
}

// `make_shared_from_pool()` 在全仓**零实例化** —— 模板没被实例化过，它的删除器
// （`~T()` 后 `deallocate(ptr)`）一次都没跑过，改坏了也没人知道。这条用例把它钉住：
// 构造走池、持有期计数为 1、离开作用域后 `~T()` 被调到且内存回池。
namespace {
std::atomic<int> g_msfp_ctor{0};
std::atomic<int> g_msfp_dtor{0};

struct msfp_probe {
    int v;
    explicit msfp_probe(int x) : v(x) { g_msfp_ctor.fetch_add(1, std::memory_order_relaxed); }
    ~msfp_probe() { g_msfp_dtor.fetch_add(1, std::memory_order_relaxed); }
    msfp_probe(const msfp_probe&) = delete;
    msfp_probe& operator=(const msfp_probe&) = delete;
};
}  // namespace

static void test_pool_make_shared_from_pool()
{
    uvcpp::uvcpp_memory_pool pool;
    pool.init();

    // 线程缓存是 `thread_local` 的**单例**，它只记一个 `pool_ptr_`：最后一次调
    // `init_thread_cache()` 的那个池。本文件前面的 `test_pool_thread_cache()` 绑的
    // 是它自己的局部池，函数一返回那个栈对象就没了。
    //
    // 所以这里**必须先重绑到本池**：`deallocate()` 会把块压进这条 TLS 链，
    // 而链要活到线程退出；那时 `release_all()` 拿着已经销毁的 `pool_ptr_` 去调
    // `release_func_` —— 实测直接 `0xC0000374`（堆损坏），而且是在**进程退出阶段**
    // 才报，现场离真凶很远。先 `init_thread_cache()` 绑本池、销毁前
    // `release_thread_cache()` 把块还回来，链就是空的，退出时不会碰那个指针。
    pool.init_thread_cache();

    g_msfp_ctor.store(0);
    g_msfp_dtor.store(0);
    // 前置断言：起点必须是干净的，否则下面的 1 / 0 都说明不了问题。
    TEST_ASSERT(pool.active_allocations() == 0, "前置：起点没有在用块");

    {
        std::shared_ptr<msfp_probe> sp = uvcpp::make_shared_from_pool<msfp_probe>(pool, 7);
        TEST_ASSERT(sp != nullptr, "make_shared_from_pool 应返回非空");
        TEST_ASSERT(sp->v == 7, "构造参数应转发给 T");
        TEST_ASSERT(g_msfp_ctor.load() == 1, "前置：T 的构造应恰好跑一次");
        TEST_ASSERT(g_msfp_dtor.load() == 0, "前置：持有期间不该析构");
        TEST_ASSERT(pool.active_allocations() == 1, "在用块数应跟着走到 1");
    }

    TEST_ASSERT(g_msfp_dtor.load() == 1, "删除器应调用 ~T()");
    TEST_ASSERT(pool.active_allocations() == 0,
                "归还后在用块数应回到 0（删除器必须 deallocate 回池）");

    // 趁本池还活着，把 TLS 链上那个块还回本池（见上面的说明）。
    pool.release_thread_cache();
    pool.shutdown();
    TEST_PASS();
}

// ==================== Main ====================

int main()
{
    std::printf("[expand][memory_pool] start\n");

    // uvcpp_memory_pool tests
    test_pool_init_shutdown();
    test_pool_alloc_free_basic();
    test_pool_zero_size();
    test_pool_aligned_alloc();
    test_pool_batch_alloc();
    test_pool_stats();
    test_pool_nullptr_free();
    test_pool_reuse();
    test_pool_thread_cache();
    test_pool_multithread();
    test_pool_make_shared_from_pool();

    // uvcpp_memory_pool_span tests
    test_span_alloc_free_basic();
    test_span_reuse();
    test_span_nullptr_free();

    // uvcpp_page_heap (enterprise) tests
    test_enterprise_alloc_free();
    test_enterprise_get_block_size();
    test_enterprise_stats();
    test_enterprise_large_object_returned();
    test_enterprise_stats_in_use_follows_alloc();

    // Integration tests
    test_uvcpp_alloc_integration();

    std::printf("[expand][memory_pool] done\n");
    return 0;
}
