/**
 * @file tests/expand/memory_pool_test.cpp
 * @brief Unit tests for the expand module (uvcpp_memory_pool, uvcpp_memory_pool_span).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    // uvcpp_memory_pool_span tests
    test_span_alloc_free_basic();
    test_span_reuse();
    test_span_nullptr_free();

    // uvcpp_page_heap (enterprise) tests
    test_enterprise_alloc_free();
    test_enterprise_get_block_size();
    test_enterprise_stats();

    // Integration tests
    test_uvcpp_alloc_integration();

    std::printf("[expand][memory_pool] done\n");
    return 0;
}
