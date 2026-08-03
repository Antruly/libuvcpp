/**
 * @file src/expand/uvcpp_memory_pool_span.h
 * @brief Span-based memory pool allocator - WIP/Experimental
 * @author zhuweiye
 * @version 1.0.0
 *
 * Status: Basic freelist-based pooling is implemented.
 * Advanced features (span merge, etc.) are reserved for future iterations.
 *
 * Role: Experimental span-based page allocator.
 * Each span is a contiguous memory region divided into fixed-size blocks.
 */

#pragma once
#ifndef SRC_EXPAND_UVCPP_MEMORY_POOL_SPAN_H
#define SRC_EXPAND_UVCPP_MEMORY_POOL_SPAN_H

#include <cstddef>
#include <cstdint>
#include <atomic>

#include <expand/uvcpp_page_allocator.h>
#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

// ========================
// Page block (32 bytes)
// ========================
/**
 * @brief Page block header (32 bytes).
 */
struct uvcpp_page_block {
    uint32_t flag;           // 1 = in use, 0 = free
    uint32_t reserved;
    size_t size;             // Total allocation size (including header)
    void* page_addr;         // Base address of the page allocation
    uvcpp_page_block* next;  // Freelist link
};

// ========================
// span (page management unit)
// ========================
/**
 * @brief Span structure for managing contiguous memory regions.
 */
struct uvcpp_span {
    void* base_addr;
    size_t span_size;
    uvcpp_span* next;  // Linked list of spans
};

/**
 * @brief Span-based memory pool allocator.
 *
 * Provides memory allocation using a span-based approach with freelist reuse.
 * Freed blocks are cached in a freelist for fast subsequent allocations.
 */
class UVCPP_API uvcpp_memory_pool_span {
public:
    /**
     * @brief Get the singleton instance.
     * @return Reference to the memory pool instance.
     */
    static uvcpp_memory_pool_span& instance();

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
     * @brief Try to merge adjacent free spans (reserved for future).
     */
    void try_merge();

private:
    uvcpp_memory_pool_span();
    ~uvcpp_memory_pool_span();

    uvcpp_page_block* alloc_block(size_t size);
    void free_block(uvcpp_page_block* block);

    // Pop a block from the freelist that is large enough for the requested size
    uvcpp_page_block* pop_freelist(size_t size);
    // Push a block back to the freelist (release to system if over threshold)
    void push_freelist(uvcpp_page_block* block);

private:
    static const size_t default_span_size = 256 * 1024;  // 256KB
    static const size_t max_freelist_size = 64;           // Max freelist cache count

    std::atomic<uvcpp_page_block*> freelist_{nullptr};    // Free block list
    std::atomic<size_t> freelist_count_{0};                // Free block count
};

} // namespace uvcpp

#endif // SRC_EXPAND_UVCPP_MEMORY_POOL_SPAN_H
