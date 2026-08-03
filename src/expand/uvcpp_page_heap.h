/**
 * @file src/expand/uvcpp_page_heap.h
 * @brief TCMalloc-like enterprise memory allocator — 主分配器
 * @author zhuweiye
 * @version 1.0.0
 *
 * **定位：** 这是 expand 模块的**主分配器**，通过 uvcpp_enterprise_alloc/free
 * 接入全局分配接口（uvcpp_alloc.h）。当 UVCPP_ENABLE_MEMORY_POOL=1 时，
 * 所有 uvcpp::uv_alloc<T>() 等调用最终路由到此分配器。
 *
 * A high-performance, enterprise-grade memory allocator inspired by TCMalloc.
 * Features:
 * - Three-level cache: Thread Cache -> Central Cache -> Span Allocator
 * - Lock-free fast path for small allocations
 * - Page-aligned memory from system (mmap/VirtualAlloc)
 * - Support for huge pages and NUMA (reserved interfaces)
 * - Memory pressure detection and response
 */

#pragma once
#ifndef SRC_EXPAND_UVCPP_PAGE_HEAP_H
#define SRC_EXPAND_UVCPP_PAGE_HEAP_H

#include <cstddef>
#include <cstdint>
#include <atomic>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {
namespace detail {
/**
 * @brief Cross-platform virtual memory allocation.
 * @param size Number of bytes to allocate (will be page-aligned).
 * @return Pointer to allocated memory, or nullptr on failure.
 */
void *uvcpp_virtual_alloc(size_t size);

/**
 * @brief Free virtual memory.
 * @param ptr Pointer returned by uvcpp_virtual_alloc.
 * @param size Size of the allocation.
 */
void uvcpp_virtual_free(void *ptr, size_t size);

/**
 * @brief Get the system page size.
 * @return Page size in bytes.
 */
size_t uvcpp_get_page_size();

/**
 * @brief Enable huge page support (reserved interface).
 * @param enable Whether to enable huge pages.
 * @return True if successful.
 */
bool uvcpp_enable_huge_page(bool enable);

/**
 * @brief Check if huge page is enabled.
 * @return True if huge page is enabled.
 */
bool uvcpp_is_huge_page_enabled();

/**
 * @brief Set NUMA node (reserved interface).
 * @param node NUMA node index.
 */
void uvcpp_set_numa_node(int node);

/**
 * @brief Get current NUMA node.
 * @return NUMA node index, or -1 if not set.
 */
int uvcpp_get_numa_node();
} // namespace detail
} // namespace uvcpp

// ========================
// Page Heap - 企业级页内存管理
// ========================
namespace uvcpp {

// ========================
// 块头部 (32B) - 每个分配块的前置元数据
// ========================
/**
 * @brief Block header (32 bytes) - metadata for each allocated block.
 */
struct page_block_header {
    uint32_t size_class;         // Size class index
    uint32_t flags;              // Flags (bit 0: large object)
    uint64_t requested_size;     // User-requested size
    struct span_header* span;    // Associated span
    struct page_block_header* next;    // Freelist link
};

// ========================
// span 结构 - 核心内存管理单元
// ========================
/**
 * @brief Span header - core memory management unit.
 *
 * A span is a contiguous memory region that contains multiple blocks
 * of the same size class.
 */
struct span_header {
    uint64_t page_count;              // Number of pages in this span
    uint64_t block_size;              // Size of each block
    std::atomic<void*> free_list;      // Lock-free freelist
    std::atomic<uint32_t> in_use;     // Number of blocks in use
    struct span_header* next;         // Next span in list
    struct span_header* prev;         // Previous span in list
    void* base_addr;                  // Starting address of span

    span_header() : page_count(0), block_size(0),
                   free_list(nullptr), in_use(0),
                   next(nullptr), prev(nullptr), base_addr(nullptr) {}
};

// ========================
// Size Class 信息
// ========================
/**
 * @brief Size class information for allocation.
 */
struct size_class_info {
    uint32_t block_size;    // Block size in bytes
    uint32_t pages;         // Number of pages for this size class
    uint32_t batch_size;    // Batch size for cache refill
};

/**
 * @brief Find the appropriate size class index for a given size.
 * @param size Requested allocation size.
 * @return Size class index.
 */
size_t uvcpp_size_class_index(size_t size);

/**
 * @brief Get page size at runtime.
 * @return Page size in bytes.
 */
size_t uvcpp_get_page_size_runtime();

// Constants
constexpr size_t k_page_block_header_size = sizeof(page_block_header);
constexpr size_t k_span_header_size = sizeof(span_header);
constexpr size_t k_large_threshold = 256 * 1024;  // 256KB threshold for large allocations
constexpr size_t k_num_size_classes = 50;

/**
 * @brief TCMalloc-like enterprise memory pool.
 *
 * A high-performance memory allocator with three-level cache architecture:
 * - Thread Cache (L1): Per-thread cache for fast allocation
 * - Central Cache (L2): Shared cache across threads
 * - Span Allocator (L3): Page-based allocation from system
 *
 * All memory ultimately comes from page-aligned virtual memory (mmap/VirtualAlloc).
 */
class UVCPP_API uvcpp_memory_pool_enterprise {
public:
    /**
     * @brief Get the singleton instance.
     * @return Reference to the memory pool instance.
     */
    static uvcpp_memory_pool_enterprise& instance();

    /**
     * @brief Allocate memory from the pool.
     * @param size Number of bytes to allocate.
     * @return Pointer to allocated memory, or nullptr on failure.
     */
    void* alloc(size_t size);

    /**
     * @brief Free memory back to the pool.
     * @param ptr Pointer to memory to free.
     */
    void free_mem(void* ptr);

    /**
     * @brief Batch allocate multiple blocks (for thread cache refill).
     * @param size Size of each block.
     * @param count Number of blocks to allocate.
     * @return Pointer to first block, or nullptr on failure.
     */
    void* alloc_batch(size_t size, uint32_t count);

    /**
     * @brief Batch free multiple     * @param blocks.
 ptr Pointer to memory to free.
     * @param size Size of each block.
     */
    void free_batch(void* ptr, size_t size);

    /**
     * @brief Get the actual block size for a requested size.
     * @param requested_size User-requested size.
     * @return Actual block size (rounded up to size class).
     */
    size_t get_block_size(size_t requested_size);

    /**
     * @brief Get memory pool statistics.
     * @param total_allocated Total bytes allocated from system.
     * @param in_use Number of blocks currently in use.
     * @param free_spans Number of free spans.
     */
    void get_stats(size_t& total_allocated, size_t& in_use, size_t& free_spans);

    /**
     * @brief Memory pressure level enum.
     */
    enum class pressure_level {
        none,       // Normal state
        low,        // Low pressure
        medium,     // Medium pressure
        high,       // High pressure
        critical    // Critical pressure
    };

    /**
     * @brief Get current memory pressure level.
     * @return Current pressure level.
     */
    pressure_level get_pressure_level() const;

    /**
     * @brief Trigger garbage collection / span merging.
     */
    void trigger_gc();

    /**
     * @brief Memory pressure callback type.
     */
    using memory_pressure_callback = void(*)();

    /**
     * @brief Set memory pressure callback.
     * @param cb Callback function to invoke on memory pressure.
     */
    void set_pressure_callback(memory_pressure_callback cb);

    /**
     * @brief Enable huge page support (reserved interface).
     * @param enable Whether to enable huge pages.
     */
    void enable_huge_page(bool enable);

    /**
     * @brief Check if huge page is enabled.
     * @return True if huge page is enabled.
     */
    bool is_huge_page_enabled() const;

    /**
     * @brief Set NUMA node (reserved interface).
     * @param node NUMA node index.
     */
    void set_numa_node(int node);

    /**
     * @brief Get current NUMA node.
     * @return NUMA node index, or -1 if not set.
     */
    int get_numa_node() const;

    /**
     * @brief Dump pool statistics (for debugging).
     */
    void dump_stats() const;

private:
    uvcpp_memory_pool_enterprise();
    ~uvcpp_memory_pool_enterprise();

    // Internal allocation paths
    void* allocate_from_thread_cache(size_t size);
    void* allocate_from_central(size_t size);
    void* allocate_from_span(size_t size);
    void* allocate_large_object(size_t size);

    // Internal free paths
    void return_to_thread_cache(void* ptr, size_t size);
    void return_to_central(void* ptr, size_t size);
    void return_to_span(void* ptr, size_t size);
    void return_large_object(void* ptr, size_t size);

    // Span management
    span_header* allocate_span(uint64_t pages);
    void release_span(span_header* span);

    // Central Cache operations
    void* central_cache_pop(size_t size_class);
    void central_cache_push(void* ptr, size_t size_class);

    // Thread Cache operations
    void* thread_cache_pop(size_t size_class);
    void thread_cache_push(void* ptr, size_t size_class);
    void thread_cache_refill(size_t size_class);
    void thread_cache_return(size_t size_class);

    // Disable copying
    uvcpp_memory_pool_enterprise(const uvcpp_memory_pool_enterprise&) = delete;
    uvcpp_memory_pool_enterprise& operator=(const uvcpp_memory_pool_enterprise&) = delete;

    // Private implementation
    class impl;
    impl* impl_;
};

/**
 * @brief Global enterprise allocator allocation function.
 * @param size Number of bytes to allocate.
 * @return Pointer to allocated memory, or nullptr on failure.
 */
UVCPP_API void* uvcpp_enterprise_alloc(size_t size);

/**
 * @brief Global enterprise allocator free function.
 * @param ptr Pointer to memory to free.
 */
UVCPP_API void uvcpp_enterprise_free(void* ptr);

} // namespace uvcpp

#endif // SRC_EXPAND_UVCPP_PAGE_HEAP_H
