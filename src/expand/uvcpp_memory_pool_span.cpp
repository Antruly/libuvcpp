
#include "uvcpp_memory_pool_span.h"
#include <cstring>

namespace uvcpp {

static size_t align_up(size_t v, size_t align)
{
    return (v + align - 1) & ~(align - 1);
}

uvcpp_memory_pool_span& uvcpp_memory_pool_span::instance()
{
    static uvcpp_memory_pool_span inst;
    return inst;
}

uvcpp_memory_pool_span::uvcpp_memory_pool_span()
{
    freelist_.store(nullptr, std::memory_order_relaxed);
    freelist_count_.store(0, std::memory_order_relaxed);
}

uvcpp_memory_pool_span::~uvcpp_memory_pool_span()
{
    // Release all blocks in the freelist
    uvcpp_page_block* curr = freelist_.exchange(nullptr, std::memory_order_acquire);
    while (curr) {
        uvcpp_page_block* next = curr->next;
        uvcpp_page_allocator::free_pages(curr->page_addr, curr->size);
        curr = next;
    }
    freelist_count_.store(0, std::memory_order_relaxed);
}

void* uvcpp_memory_pool_span::alloc(size_t size)
{
    uvcpp_page_block* blk = alloc_block(size);
    if (!blk) return nullptr;
    return (void*)((char*)blk + sizeof(uvcpp_page_block));
}

void uvcpp_memory_pool_span::free_mem(void* ptr)
{
    if (!ptr) return;
    uvcpp_page_block* blk =
        (uvcpp_page_block*)((char*)ptr - sizeof(uvcpp_page_block));
    free_block(blk);
}

uvcpp_page_block* uvcpp_memory_pool_span::pop_freelist(size_t size)
{
    uvcpp_page_block* prev = nullptr;
    uvcpp_page_block* curr = freelist_.load(std::memory_order_acquire);

    while (curr) {
        // Check if block is large enough (user size + header)
        size_t needed = align_up(size + sizeof(uvcpp_page_block), 64);
        if (curr->size >= needed) {
            uvcpp_page_block* next = curr->next;
            if (prev) {
                prev->next = next;
            } else {
                // Update head node
                if (!freelist_.compare_exchange_strong(curr, next,
                        std::memory_order_release, std::memory_order_relaxed)) {
                    // CAS failed, restart
                    return pop_freelist(size);
                }
            }

            curr->next = nullptr;
            curr->flag = 1; // Mark as in use
            freelist_count_.fetch_sub(1, std::memory_order_relaxed);
            return curr;
        }
        prev = curr;
        curr = curr->next;
    }

    return nullptr; // No suitable block in freelist
}

void uvcpp_memory_pool_span::push_freelist(uvcpp_page_block* block)
{
    if (!block) return;

    // Release to system if freelist is full
    size_t count = freelist_count_.load(std::memory_order_relaxed);
    if (count >= max_freelist_size) {
        uvcpp_page_allocator::free_pages(block->page_addr, block->size);
        return;
    }

    block->flag = 0; // Mark as free
    block->next = nullptr;

    // CAS loop to insert into freelist head
    uvcpp_page_block* old_head = freelist_.load(std::memory_order_relaxed);
    do {
        block->next = old_head;
    } while (!freelist_.compare_exchange_weak(old_head, block,
            std::memory_order_release, std::memory_order_relaxed));

    freelist_count_.fetch_add(1, std::memory_order_relaxed);
}

uvcpp_page_block* uvcpp_memory_pool_span::alloc_block(size_t size)
{
    // Fast path: try to reuse a freed block from freelist
    uvcpp_page_block* blk = pop_freelist(size);
    if (blk) {
        return blk;
    }

    // Slow path: allocate new pages from system
    size_t needed = align_up(size + sizeof(uvcpp_page_block), 64);
    size_t span_size = default_span_size;

    if (needed > span_size) {
        size_t pages = (needed + default_span_size - 1) / default_span_size;
        span_size = pages * default_span_size;
    }

    void* mem = uvcpp_page_allocator::alloc_pages(span_size);
    if (!mem) return nullptr;

    blk = (uvcpp_page_block*)mem;
    blk->flag = 1; // In use
    blk->size = span_size;
    blk->page_addr = mem;
    blk->next = nullptr;

    return blk;
}

void uvcpp_memory_pool_span::free_block(uvcpp_page_block* block)
{
    if (!block) return;

    // Try to push to freelist for reuse
    push_freelist(block);
}

void uvcpp_memory_pool_span::try_merge()
{
    // Reserved for future: merge adjacent free spans
}

} // namespace uvcpp
