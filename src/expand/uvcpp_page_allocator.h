/**
 * @file src/expand/uvcpp_page_allocator.h
 * @brief Cross-platform page memory allocator.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides cross-platform virtual memory allocation using mmap (Linux/macOS)
 * or VirtualAlloc (Windows). All memory is page-aligned.
 */

#pragma once
#ifndef SRC_EXPAND_UVCPP_PAGE_ALLOCATOR_H
#define SRC_EXPAND_UVCPP_PAGE_ALLOCATOR_H

#include <cstddef>
#include <cstdint>
#include <atomic>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

/**
 * @brief Cross-platform page allocator.
 *
 * Uses mmap on Linux/macOS and VirtualAlloc on Windows.
 * All allocations are page-aligned.
 */
class UVCPP_API uvcpp_page_allocator {
public:
    /**
     * @brief Allocate pages from the system.
     * @param size Number of bytes to allocate (will be page-aligned).
     * @return Pointer to allocated memory, or nullptr on failure.
     */
    static void* alloc_pages(size_t size);

    /**
     * @brief Free previously allocated pages.
     * @param ptr Pointer returned by alloc_pages.
     * @param size Size of the allocation (used for munmap/VirtualFree).
     */
    static void free_pages(void* ptr, size_t size);

    /**
     * @brief Get the system page size.
     * @return Page size in bytes.
     */
    static size_t page_size();
};

} // namespace uvcpp

#endif // SRC_EXPAND_UVCPP_PAGE_ALLOCATOR_H
