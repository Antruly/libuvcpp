
#include "uvcpp_page_heap.h"
#include <uvcpp/uvcpp_define.h>

// ========================
// 跨平台虚拟内存分配
// ========================
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <cstring>
#include <cstdlib>
#include <new>

namespace uvcpp {
namespace detail {
    // 缓存页大小
static size_t g_page_size = 0;
static bool g_huge_page_enabled = false;
static int g_numa_node = -1;

size_t uvcpp_get_page_size()
{
    if (g_page_size == 0) {
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        g_page_size = si.dwPageSize;
#else
        g_page_size = (size_t)sysconf(_SC_PAGESIZE);
#endif
    }
    return g_page_size;
}

void* uvcpp_virtual_alloc(size_t size)
{
    if (size == 0) size = 1;
    
    // 大页对齐
    size_t page_size = uvcpp_get_page_size();
    size = (size + page_size - 1) & ~(page_size - 1);

#if defined(_WIN32)
    void* ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return ptr;
#else
    // Linux/macOS 使用 mmap
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    
    // 尝试大页（预留接口）
    if (g_huge_page_enabled) {
#if defined(MAP_HUGETLB)
        flags |= MAP_HUGETLB;
#endif
    }
    
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#endif
}

void uvcpp_virtual_free(void* ptr, size_t size)
{
    if (!ptr) return;
    
    if (size == 0) {
        size = uvcpp_get_page_size();
    }
    
#if defined(_WIN32)
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

bool uvcpp_enable_huge_page(bool enable)
{
    g_huge_page_enabled = enable;
    return true;
}

bool uvcpp_is_huge_page_enabled()
{
    return g_huge_page_enabled;
}

void uvcpp_set_numa_node(int node)
{
    g_numa_node = node;
}

int uvcpp_get_numa_node()
{
    return g_numa_node;
}
} // namespace detail
} // namespace uvcpp

// ========================
// Size Class 表 (工业级)
// ========================
namespace uvcpp {

// 核心常量 - 使用运行时值（不在头文件中定义的）
static const size_t k_large_size_threshold = 256 * 1024;  // 256KB

// 运行时获取 page size
size_t uvcpp_get_page_size_runtime() {
    return uvcpp::detail::uvcpp_get_page_size();
}

// 使用运行时初始化的常量（在 C++11 中可以用函数返回值初始化）
static size_t k_page_size() { return uvcpp::detail::uvcpp_get_page_size(); }
static const size_t k_span_header_size_actual = sizeof(span_header);
static const size_t k_page_block_header_size_actual = 32;

// Size Class 表 - power-of-two + jemalloc 风格递增
static constexpr size_class_info k_size_classes[k_num_size_classes] = {
    // 小块：8B 递增 (8-256B)
    {8,       1,   64},
    {16,      1,   64},
    {24,      1,   64},
    {32,      1,   64},
    {40,      1,   32},
    {48,      1,   32},
    {56,      1,   32},
    {64,      1,   32},
    {72,      1,   16},
    {80,      1,   16},
    {88,      1,   16},
    {96,      1,   16},
    {104,     1,   16},
    {112,     1,   16},
    {120,     1,   16},
    {128,     1,   16},
    {136,     1,   8},
    {144,     1,   8},
    {152,     1,   8},
    {160,     1,   8},
    {168,     1,   8},
    {176,     1,   8},
    {184,     1,   8},
    {192,     1,   8},
    {200,     1,   8},
    {208,     1,   8},
    {216,     1,   8},
    {224,     1,   8},
    {232,     1,   8},
    {240,     1,   8},
    {248,     1,   8},
    {256,     1,   8},
    // 中块：512B 递增 (512-8KB)
    {512,     1,   8},
    {768,     1,   4},
    {1024,    1,   4},
    {1536,    2,   4},
    {2048,    2,   4},
    {3072,    4,   2},
    {4096,    4,   2},
    // 大块：8KB 递增 (8KB-256KB)
    // pages = ceil((header_span(40) + header_block(32) + block_size) / page_size(4096))
    {8192,    3,   2},
    {12288,   4,   2},
    {16384,   5,   1},
    {24576,   7,   1},
    {32768,   9,   1},
    {49152,   13,  1},
    {65536,   17,  1},
    {98304,   25,  1},
    {131072,  33,  1},
    {196608,  49,  1},
    {262144,  65,  1},
};

// 查找 size class 索引
size_t uvcpp_size_class_index(size_t size)
{
    if (size <= 256) {
        // 8B increments: indices 0..31 map to 8,16,...,248,256
        return (size + 7) / 8 - 1;
    }
    // 512B increments: indices 32..38 map to 512,768,1024,1536,2048,3072,4096
    if (size <= 512)  return 32;
    if (size <= 768)  return 33;
    if (size <= 1024) return 34;
    if (size <= 1536) return 35;
    if (size <= 2048) return 36;
    if (size <= 3072) return 37;
    if (size <= 4096) return 38;
    // Large block range: use explicit thresholds to avoid off-by-one overflow
    // Indices 39..49 map to block sizes 8K,12K,16K,24K,32K,48K,64K,96K,128K,192K,256K
    if (size <= 8192)   return 39;
    if (size <= 12288)  return 40;
    if (size <= 16384)  return 41;
    if (size <= 24576)  return 42;
    if (size <= 32768)  return 43;
    if (size <= 49152)  return 44;
    if (size <= 65536)  return 45;
    if (size <= 98304)  return 46;
    if (size <= 131072) return 47;
    if (size <= 196608) return 48;
    if (size <= 262144) return 49;
    // Above 256KB: treated as large object (k_large_threshold), not reached here
    return k_num_size_classes - 1;
}

// ========================
// Central Cache - 每个 size class 一个
// ========================
class central_cache {
public:
    static constexpr size_t k_num_classes = k_num_size_classes;

    central_cache() {
        for (auto& c : cache_) {
            c.span_list.store(nullptr, std::memory_order_relaxed);
            c.lock.store(false, std::memory_order_relaxed);
        }
    }
    
    void* pop(size_t size_class) {
        if (size_class >= k_num_classes) return nullptr;
        
        auto& c = cache_[size_class];
        
        // 尝试从 span 获取
        span_header* span = c.span_list.load(std::memory_order_acquire);
        while (span) {
            void* ptr = span->free_list.load(std::memory_order_relaxed);
            while (ptr) {
                void* next = *((void**)ptr);
                if (span->free_list.compare_exchange_weak(ptr, next, 
                    std::memory_order_release, std::memory_order_relaxed)) {
                    span->in_use.fetch_add(1, std::memory_order_relaxed);
                    return ptr;
                }
                // ptr 被更新为新的 free_list 头
            }
            
            // 当前 span 空了，尝试下一个
            span_header* next_span = span->next;
            if (span != c.span_list.load(std::memory_order_acquire)) {
                span = c.span_list.load(std::memory_order_acquire);
                continue;
            }
            
            if (next_span == span) {
                // 没有更多 span 了，需要分配新的
                break;
            }
            span = next_span;
        }
        
        // 需要分配新 span
        return nullptr;
    }
    
    void push(void* ptr, size_t size_class, span_header* span) {
        if (!ptr || size_class >= k_num_classes) return;
        
        auto& c = cache_[size_class];
        
        // 加入 freelist
        void* old_head = span->free_list.load(std::memory_order_relaxed);
        do {
            *((void**)ptr) = old_head;
        } while (!span->free_list.compare_exchange_weak(old_head, ptr,
            std::memory_order_release, std::memory_order_relaxed));
        
        span->in_use.fetch_sub(1, std::memory_order_relaxed);
    }
    
    void add_span(span_header* span, size_t size_class) {
        if (!span || size_class >= k_num_classes) return;
        
        auto& c = cache_[size_class];
        
        // 简单的加锁保护
        while (c.lock.exchange(true, std::memory_order_acquire)) {
            // spin
        }
        
        span->next = c.span_list.load(std::memory_order_relaxed);
        span->prev = nullptr;
        
        span_header* old_head = c.span_list.load(std::memory_order_relaxed);
        if (old_head) {
            old_head->prev = span;
        }
        
        c.span_list.store(span, std::memory_order_release);
        c.lock.store(false, std::memory_order_release);
    }
    
    span_header* remove_span(size_t size_class) {
        if (size_class >= k_num_classes) return nullptr;

        auto& c = cache_[size_class];

        while (c.lock.exchange(true, std::memory_order_acquire)) {
            // spin
        }

        span_header* span = c.span_list.load(std::memory_order_acquire);
        if (span) {
            span_header* next = span->next;
            if (next) {
                next->prev = nullptr;
            }
            c.span_list.store(next, std::memory_order_release);
        }

        c.lock.store(false, std::memory_order_release);
        return span;
    }

    // 批量 push - 归还多个块到 span
    void push_batch(void* first, void* last, size_t count, size_t size_class, span_header* span) {
        if (!first || !span || size_class >= k_num_classes) return;

        // 将链表连接到 span 的 freelist
        void* old_head = span->free_list.load(std::memory_order_relaxed);
        *((void**)last) = old_head;

        while (!span->free_list.compare_exchange_weak(old_head, first,
            std::memory_order_release, std::memory_order_relaxed)) {
            *((void**)last) = old_head;
        }

        span->in_use.fetch_sub(count, std::memory_order_relaxed);
    }

    // 获取指定 size class 的 span
    span_header* get_span(size_t size_class) {
        if (size_class >= k_num_classes) return nullptr;
        return cache_[size_class].span_list.load(std::memory_order_acquire);
    }

private:
    struct cache_entry {
        std::atomic<span_header*> span_list;
        std::atomic<bool> lock;
    };
    cache_entry cache_[k_num_classes];
};

// ========================
// Thread Cache - 每个线程本地缓存
// ========================

// 前向声明 central_cache
class central_cache;
// 全局 Central Cache（在使用前必须先初始化，这里声明，定义在后面）
extern central_cache g_central_cache;

class thread_cache {
public:
    static constexpr size_t k_num_classes = k_num_size_classes;
    static constexpr size_t k_max_per_class = 256;
    static constexpr size_t k_refill_batch_size = 8;   // 批量获取数量
    static constexpr size_t k_return_threshold = 128;  // 归还阈值
    
    thread_cache() {
        for (auto& c : cache_) {
            c.count = 0;
            c.head = nullptr;
        }
    }
    
    ~thread_cache() {
        // 清理所有缓存
        for (auto& c : cache_) {
            void* p = c.head;
            while (p) {
                void* next = *((void**)p);
                // 归还到 central cache
                page_block_header* header = (page_block_header*)((char*)p - k_page_block_header_size_actual);
                span_header* span = header->span;
                if (span) {
                    g_central_cache.push(p, header->size_class, span);
                }
                p = next;
            }
        }
    }
    
    void* pop(size_t size_class) {
        if (size_class >= k_num_classes) return nullptr;
        
        auto& c = cache_[size_class];
        if (!c.head) {
            // cache miss，批量从 central 获取
            refill(size_class);
        }
        
        if (!c.head) return nullptr;
        
        void* ptr = c.head;
        c.head = *((void**)ptr);
        c.count--;
        
        return ptr;
    }
    
    void push(void* ptr, size_t size_class) {
        if (!ptr || size_class >= k_num_classes) return;
        
        auto& c = cache_[size_class];
        
        // 超过阈值，批量归还
        if (c.count >= k_max_per_class) {
            return_to_central_batch(size_class);
        }
        
        *((void**)ptr) = c.head;
        c.head = ptr;
        c.count++;
    }

    // 批量 refill - 从 central cache 获取多个块
    void refill(size_t size_class) {
        if (size_class >= k_num_classes) return;
        
        auto& c = cache_[size_class];
        
        // 获取对应的 span
        span_header* span = g_central_cache.get_span(size_class);
        if (!span) return;
        
        // 批量获取
        uint32_t batch_size = k_refill_batch_size;
        void* first = nullptr;
        void* last = nullptr;
        uint32_t actual_count = 0;
        
        for (uint32_t i = 0; i < batch_size; i++) {
            void* ptr = span->free_list.load(std::memory_order_relaxed);
            while (ptr) {
                void* next = *((void**)ptr);
                if (span->free_list.compare_exchange_weak(ptr, next,
                    std::memory_order_release, std::memory_order_relaxed)) {
                    // 成功获取一个块
                    if (!first) {
                        first = ptr;
                        last = ptr;
                    } else {
                        *((void**)last) = ptr;
                        last = ptr;
                    }
                    actual_count++;
                    span->in_use.fetch_sub(1, std::memory_order_relaxed);
                    break;
                }
            }
            if (!ptr) break;
        }
        
        if (first) {
            *((void**)last) = nullptr;
            // 将获取的块添加到 thread cache
            *((void**)last) = c.head;
            c.head = first;
            c.count += actual_count;
        }
    }
    
    // 批量归还到 central cache
    void return_to_central_batch(size_t size_class) {
        if (size_class >= k_num_classes) return;
        
        auto& c = cache_[size_class];
        if (!c.head) return;
        
        // 批量归还一半
        uint32_t return_count = c.count / 2;
        if (return_count < 1) return_count = 1;
        
        // 取出 return_count 个块
        void* batch = c.head;
        void* prev = nullptr;
        void* p = c.head;
        
        for (uint32_t i = 0; i < return_count && p; i++) {
            prev = p;
            p = *((void**)p);
        }
        
        if (prev && p) {
            // 断开链表
            *((void**)prev) = nullptr;
            c.head = p;
            c.count -= return_count;
            
            // 归还到 central cache
            span_header* span = g_central_cache.get_span(size_class);
            if (span) {
                g_central_cache.push_batch(batch, prev, return_count, size_class, span);
            }
        }
    }

    // 清空所有缓存到 central
    void flush_all() {
        for (size_t i = 0; i < k_num_classes; i++) {
            auto& c = cache_[i];
            if (c.head && c.count > 0) {
                span_header* span = g_central_cache.get_span(i);
                if (span) {
                    // 找到链表尾部
                    void* last = c.head;
                    while (*((void**)last)) {
                        last = *((void**)last);
                    }
                    g_central_cache.push_batch(c.head, last, c.count, i, span);
                }
                c.head = nullptr;
                c.count = 0;
            }
        }
    }
    
    size_t get_count(size_t size_class) const {
        if (size_class >= k_num_classes) return 0;
        return cache_[size_class].count;
    }

    // 动态调整 - 预留接口
    size_t get_optimal_size(size_t size_class) const {
        // 热 size class 可以有更大的缓存
        // 冷 size class 减少缓存
        return k_max_per_class;
    }

private:
    struct cache_entry {
        void* head;
        uint32_t count;
    };
    cache_entry cache_[k_num_classes];
};

// 线程本地存储
static thread_local thread_cache g_thread_cache;

// 全局 Central Cache
static central_cache g_central_cache;

// ========================
// Span 管理
// ========================
static std::atomic<span_header*> g_span_free_list{nullptr};
static std::atomic<size_t> g_total_spans{0};
static std::atomic<size_t> g_free_spans{0};
static std::atomic<size_t> g_total_allocated{0};
static std::atomic<size_t> g_in_use{0};

// 空闲 span 链表锁
static std::atomic_flag g_span_list_lock = ATOMIC_FLAG_INIT;

// 尝试合并相邻的空闲 span
bool try_merge_spans(span_header* span1, span_header* span2) {
    if (!span1 || !span2) return false;
    
    // 检查是否相邻
    char* end1 = (char*)span1->base_addr + span1->page_count * k_page_size();
    if (end1 != span2->base_addr) return false;
    
    // 合并：扩展 span1
    span1->page_count += span2->page_count;
    span1->next = span2->next;
    
    // 释放 span2
    delete span2;
    g_total_spans.fetch_sub(1, std::memory_order_relaxed);
    
    return true;
}

// 扫描并合并空闲 span
void try_coalesce_spans() {
    span_header* current = g_span_free_list.load(std::memory_order_acquire);
    
    while (current && current->next) {
        span_header* next = current->next;
        
        // 尝试合并
        if (try_merge_spans(current, next)) {
            // 继续检查是否还能合并
            continue;
        }
        current = current->next;
    }
}

// 检查是否应该回收 span
bool should_release_span(span_header* span) {
    if (!span) return false;
    
    // 如果 in_use 为 0 且超过一定阈值，可以考虑回收
    // 这里简化：总是允许回收空闲 span
    return span->in_use.load(std::memory_order_relaxed) == 0;
}

// 内存压力检测
enum class memory_pressure_level {
    none,       // 正常
    low,        // 低压力
    medium,     // 中压力
    high,       // 高压力
    critical    // 临界
};

static std::atomic<memory_pressure_level> g_pressure_level{memory_pressure_level::none};

memory_pressure_level get_memory_pressure() {
    size_t total = g_total_allocated.load(std::memory_order_relaxed);
    size_t in_use_count = g_in_use.load(std::memory_order_relaxed);
    size_t free = g_free_spans.load(std::memory_order_relaxed);
    
    if (free == 0 && in_use_count > 0) {
        return memory_pressure_level::critical;
    }
    
    double usage_ratio = (double)in_use_count / (double)(total + 1);
    
    if (usage_ratio > 0.95) return memory_pressure_level::high;
    if (usage_ratio > 0.8) return memory_pressure_level::medium;
    if (usage_ratio > 0.6) return memory_pressure_level::low;
    
    return memory_pressure_level::none;
}

// 响应内存压力
void respond_to_memory_pressure() {
    memory_pressure_level level = get_memory_pressure();
    g_pressure_level.store(level, std::memory_order_relaxed);
    
    if (level >= memory_pressure_level::medium) {
        // 尝试合并空闲 span
        try_coalesce_spans();
        
        // 释放完全空闲的 span（预留实现）
        // 实际可以添加更复杂的策略
    }
}

span_header* allocate_span_from_system(uint64_t pages, size_t block_size = 0)
{
    using namespace uvcpp::detail;

    size_t page_size = uvcpp_get_page_size();
    size_t total_size = pages * page_size;

    // 分配原始内存
    void* mem = uvcpp_virtual_alloc(total_size);
    if (!mem) return nullptr;

    g_total_allocated.fetch_add(total_size, std::memory_order_relaxed);

    // 分配 span header
    span_header* span = new (std::nothrow) span_header();
    if (!span) {
        uvcpp_virtual_free(mem, total_size);
        return nullptr;
    }

    // 初始化 span
    span->page_count = pages;
    span->block_size = block_size;
    span->base_addr = mem;
    span->in_use.store(0, std::memory_order_relaxed);
    span->next = nullptr;
    span->prev = nullptr;

    // 初始化 freelist：每个块 = page_block_header + 用户数据区
    // 如果 block_size 为 0（未指定），则回退到仅 header 大小的步长
    size_t stride = k_page_block_header_size_actual + block_size;
    if (stride < sizeof(void*)) {
        stride = k_page_block_header_size_actual; // 最小对齐保证
    }

    char* ptr = (char*)mem + k_span_header_size_actual;
    char* end = (char*)mem + total_size;

    void* first = nullptr;
    void* prev = nullptr;
    while (ptr + stride <= end) {
        void* cur = (void*)ptr;
        *((void**)cur) = nullptr;

        if (prev) {
            *((void**)prev) = cur;
        } else {
            first = cur;
        }
        prev = cur;
        ptr += stride;
    }

    span->free_list.store(first, std::memory_order_relaxed);

    g_total_spans.fetch_add(1, std::memory_order_relaxed);
    g_free_spans.fetch_add(1, std::memory_order_relaxed);

    return span;
}

void release_span_to_system(span_header* span)
{
    if (!span) return;
    
    using namespace uvcpp::detail;
    
    size_t page_size = uvcpp_get_page_size();
    size_t total_size = span->page_count * page_size;
    
    uvcpp_virtual_free(span->base_addr, total_size);
    g_total_allocated.fetch_sub(total_size, std::memory_order_relaxed);
    
    g_free_spans.fetch_sub(1, std::memory_order_relaxed);
    g_total_spans.fetch_sub(1, std::memory_order_relaxed);
    
    delete span;
}

// ========================
// 内存池实现
// ========================
class uvcpp_memory_pool_enterprise::impl {
public:
    memory_pressure_callback pressure_cb = nullptr;
    bool huge_page_enabled = false;
    int numa_node = -1;
};

uvcpp_memory_pool_enterprise::uvcpp_memory_pool_enterprise()
    : impl_(new impl())
{
}

uvcpp_memory_pool_enterprise::~uvcpp_memory_pool_enterprise()
{
    delete static_cast<impl*>(impl_);
}

void* uvcpp_memory_pool_enterprise::alloc(size_t size)
{
    if (size == 0) return nullptr;
    
    // 大块直接分配
    if (size > k_large_size_threshold) {
        return allocate_large_object(size);
    }
    
    // 查找 size class
    size_t size_class = uvcpp_size_class_index(size);
    size_t block_size = k_size_classes[size_class].block_size;
    
    // 先尝试 thread cache
    void* ptr = thread_cache_pop(size_class);
    if (ptr) {
        // 设置块头
        page_block_header* header = (page_block_header*)((char*)ptr - k_page_block_header_size_actual);
        header->size_class = (uint32_t)size_class;
        header->requested_size = size;
        header->flags = 0;
        return ptr;
    }
    
    // thread cache miss，尝试 central cache
    ptr = central_cache_pop(size_class);
    if (ptr) {
        // 设置块头
        page_block_header* header = (page_block_header*)((char*)ptr - k_page_block_header_size_actual);
        header->size_class = (uint32_t)size_class;
        header->requested_size = size;
        header->flags = 0;
        return ptr;
    }
    
    // central cache miss，从 span 分配
    void* result = allocate_from_span(block_size);
    if (result) {
        // 设置块头
        page_block_header* header = (page_block_header*)((char*)result - k_page_block_header_size_actual);
        header->size_class = (uint32_t)size_class;
        header->requested_size = size;
        header->flags = 0;
    }
    return result;
}

void uvcpp_memory_pool_enterprise::free_mem(void* ptr)
{
    if (!ptr) return;
    
    // 获取块头
    page_block_header* header = (page_block_header*)((char*)ptr - k_page_block_header_size_actual);
    uint32_t size_class = header->size_class;
    uint64_t requested_size = header->requested_size;
    span_header* span = header->span;
    
    // 判断是否为大块
    if (header->flags & 1) {
        // 大块释放
        if (span && span->in_use.fetch_sub(1, std::memory_order_relaxed) == 1) {
            release_span_to_system(span);
        }
        g_in_use.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    
    // 小块返回 thread cache
    if (size_class < k_num_size_classes) {
        thread_cache_push(ptr, size_class);
    }
    
    g_in_use.fetch_sub(1, std::memory_order_relaxed);
}

void* uvcpp_memory_pool_enterprise::allocate_from_thread_cache(size_t size)
{
    size_t size_class = uvcpp_size_class_index(size);
    return g_thread_cache.pop(size_class);
}

void* uvcpp_memory_pool_enterprise::allocate_from_central(size_t size)
{
    size_t size_class = uvcpp_size_class_index(size);
    return g_central_cache.pop(size_class);
}

void* uvcpp_memory_pool_enterprise::allocate_from_span(size_t size)
{
    size_t size_class = uvcpp_size_class_index(size);
    const auto& info = k_size_classes[size_class];
    
    // 尝试从 central cache 获取
    span_header* span = g_central_cache.remove_span(size_class);
    
    if (!span) {
        // 需要分配新 span
        span = allocate_span_from_system(info.pages, info.block_size);
        if (!span) return nullptr;

        // 初始化 freelist，每个块前面有 page_block_header
        char* user_ptr = (char*)span->base_addr + k_span_header_size_actual;
        size_t total_size = info.pages * k_page_size();
        char* end = (char*)span->base_addr + total_size;
        
        void* first = nullptr;
        void* prev = nullptr;
        
        while (user_ptr + info.block_size + k_page_block_header_size_actual <= end) {
            // 设置 page_block_header
            page_block_header* header = (page_block_header*)user_ptr;
            header->size_class = (uint32_t)size_class;
            header->requested_size = size;
            header->flags = 0;
            header->span = span;
            header->next = nullptr;
            
            // 用户指针从 page_block_header 之后开始
            void* cur = (void*)(user_ptr + k_page_block_header_size_actual);
            *((void**)cur) = nullptr;
            
            if (prev) {
                *((void**)prev) = cur;
            } else {
                first = cur;
            }
            prev = cur;
            user_ptr += k_page_block_header_size_actual + info.block_size;
        }
        
        span->free_list.store(first, std::memory_order_relaxed);
        
        // 添加到 central cache
        g_central_cache.add_span(span, size_class);
    }
    
    // 从 span 获取一个块
    void* ptr = span->free_list.load(std::memory_order_relaxed);
    while (ptr) {
        void* next = *((void**)ptr);
        if (span->free_list.compare_exchange_weak(ptr, next,
            std::memory_order_release, std::memory_order_relaxed)) {
            span->in_use.fetch_add(1, std::memory_order_relaxed);
            g_in_use.fetch_add(1, std::memory_order_relaxed);
            return ptr;
        }
    }
    
    return nullptr;
}

void* uvcpp_memory_pool_enterprise::allocate_large_object(size_t size)
{
    // 计算需要的页数
    size_t total_size = size + k_span_header_size_actual + k_page_block_header_size_actual;
    size_t page_size = uvcpp::detail::uvcpp_get_page_size();
    uint64_t pages = (total_size + page_size - 1) / page_size;
    
    span_header* span = allocate_span_from_system(pages, size);
    if (!span) return nullptr;

    // 设置 page_block_header
    char* user_ptr = (char*)span->base_addr + k_span_header_size_actual;
    page_block_header* header = (page_block_header*)user_ptr;
    header->size_class = 0xFFFFFFFF; // 标记为大块
    header->requested_size = size;
    header->flags = 1; // 大块标志
    header->span = span;
    header->next = nullptr;
    
    // 大块直接返回
    void* ptr = (char*)span->base_addr + k_span_header_size_actual + k_page_block_header_size_actual;
    span->free_list.store(ptr, std::memory_order_relaxed);
    
    g_in_use.fetch_add(1, std::memory_order_relaxed);
    
    return ptr;
}

void* uvcpp_memory_pool_enterprise::thread_cache_pop(size_t size_class)
{
    return g_thread_cache.pop(size_class);
}

void uvcpp_memory_pool_enterprise::thread_cache_push(void* ptr, size_t size_class)
{
    g_thread_cache.push(ptr, size_class);
}

// 批量 refill - 从 central cache 获取多个块
void uvcpp_memory_pool_enterprise::thread_cache_refill(size_t size_class)
{
    g_thread_cache.refill(size_class);
}

// 批量归还 - 将超过阈值的块归还到 central cache
void uvcpp_memory_pool_enterprise::thread_cache_return(size_t size_class)
{
    g_thread_cache.return_to_central_batch(size_class);
}

void* uvcpp_memory_pool_enterprise::central_cache_pop(size_t size_class)
{
    return g_central_cache.pop(size_class);
}

void uvcpp_memory_pool_enterprise::central_cache_push(void* ptr, size_t size_class)
{
    if (!ptr) return;
    
    // 从块头获取 span
    page_block_header* header = (page_block_header*)((char*)ptr - k_page_block_header_size_actual);
    span_header* span = header->span;
    
    if (!span) return;
    
    // 归还到 central cache
    g_central_cache.push(ptr, size_class, span);
}

void uvcpp_memory_pool_enterprise::return_to_thread_cache(void* ptr, size_t size)
{
    size_t size_class = uvcpp_size_class_index(size);
    thread_cache_push(ptr, size_class);
}

void uvcpp_memory_pool_enterprise::return_to_central(void* ptr, size_t size)
{
    if (!ptr) return;
    
    // 从块头获取信息
    page_block_header* header = (page_block_header*)((char*)ptr - k_page_block_header_size_actual);
    uint32_t size_class = header->size_class;
    span_header* span = header->span;
    
    if (!span || size_class >= k_num_size_classes) return;
    
    // 归还到 central cache
    g_central_cache.push(ptr, size_class, span);
}

void uvcpp_memory_pool_enterprise::return_to_span(void* ptr, size_t size)
{
    if (!ptr) return;
    
    // 从块头获取 span
    page_block_header* header = (page_block_header*)((char*)ptr - k_page_block_header_size_actual);
    span_header* span = header->span;
    
    if (!span) return;
    
    // 直接归还到 span 的 freelist
    void* old_head = span->free_list.load(std::memory_order_relaxed);
    do {
        *((void**)ptr) = old_head;
    } while (!span->free_list.compare_exchange_weak(old_head, ptr,
        std::memory_order_release, std::memory_order_relaxed));
    
    span->in_use.fetch_sub(1, std::memory_order_relaxed);
    g_in_use.fetch_sub(1, std::memory_order_relaxed);
}

void uvcpp_memory_pool_enterprise::return_large_object(void* ptr, size_t size)
{
    // 找到对应的 span 并释放
    span_header* span = (span_header*)((char*)ptr - k_span_header_size_actual);
    if (span->in_use.fetch_sub(1, std::memory_order_relaxed) == 1) {
        release_span_to_system(span);
    }
    g_in_use.fetch_sub(1, std::memory_order_relaxed);
}

span_header* uvcpp_memory_pool_enterprise::allocate_span(uint64_t pages)
{
    return allocate_span_from_system(pages);
}

void uvcpp_memory_pool_enterprise::release_span(span_header* span)
{
    release_span_to_system(span);
}

void* uvcpp_memory_pool_enterprise::alloc_batch(size_t size, uint32_t count)
{
    if (size == 0 || count == 0) return nullptr;
    
    size_t size_class = uvcpp_size_class_index(size);
    size_t block_size = k_size_classes[size_class].block_size;
    
    // 分配一个 span
    uint64_t pages = (block_size * count + k_page_size() - 1) / k_page_size();
    if (pages < 1) pages = 1;
    
    span_header* span = allocate_span_from_system(pages, block_size);
    if (!span) return nullptr;

    // 初始化 freelist，每个块前面有 page_block_header
    char* user_ptr = (char*)span->base_addr + k_span_header_size_actual;
    size_t total_size = pages * k_page_size() - k_span_header_size_actual;
    
    void* first = nullptr;
    void* prev = nullptr;
    
    for (uint32_t i = 0; i < count && (char*)user_ptr - (char*)span->base_addr < total_size; i++) {
        // 设置 page_block_header
        page_block_header* header = (page_block_header*)user_ptr;
        header->size_class = (uint32_t)size_class;
        header->requested_size = size;
        header->flags = 0;
        header->span = span;
        header->next = nullptr;
        
        // 用户指针从 page_block_header 之后开始
        void* cur = (void*)(user_ptr + k_page_block_header_size_actual);
        *((void**)cur) = nullptr;
        
        if (prev) {
            *((void**)prev) = cur;
        } else {
            first = cur;
        }
        prev = cur;
        user_ptr += k_page_block_header_size_actual + block_size;
    }
    
    span->free_list.store(first, std::memory_order_relaxed);
    
    // 添加到 central cache
    g_central_cache.add_span(span, size_class);
    
    // 返回第一个块
    return first;
}

void uvcpp_memory_pool_enterprise::free_batch(void* ptr, size_t size)
{
    // 简化实现
    free_mem(ptr);
}

size_t uvcpp_memory_pool_enterprise::get_block_size(size_t requested_size)
{
    if (requested_size > k_large_size_threshold) {
        return requested_size;
    }
    size_t idx = uvcpp_size_class_index(requested_size);
    return k_size_classes[idx].block_size;
}

void uvcpp_memory_pool_enterprise::get_stats(size_t& total_allocated, size_t& in_use_count, size_t& free_span_count)
{
    total_allocated = g_total_allocated.load(std::memory_order_relaxed);
    in_use_count = g_in_use.load(std::memory_order_relaxed);
    free_span_count = g_free_spans.load(std::memory_order_relaxed);
}

uvcpp_memory_pool_enterprise::pressure_level uvcpp_memory_pool_enterprise::get_pressure_level() const
{
    memory_pressure_level level = get_memory_pressure();
    switch (level) {
        case memory_pressure_level::none: return pressure_level::none;
        case memory_pressure_level::low: return pressure_level::low;
        case memory_pressure_level::medium: return pressure_level::medium;
        case memory_pressure_level::high: return pressure_level::high;
        case memory_pressure_level::critical: return pressure_level::critical;
    }
    return pressure_level::none;
}

void uvcpp_memory_pool_enterprise::trigger_gc()
{
    respond_to_memory_pressure();
}

void uvcpp_memory_pool_enterprise::set_pressure_callback(memory_pressure_callback cb)
{
    static_cast<impl*>(impl_)->pressure_cb = cb;
}

void uvcpp_memory_pool_enterprise::enable_huge_page(bool enable)
{
    static_cast<impl*>(impl_)->huge_page_enabled = enable;
    uvcpp::detail::uvcpp_enable_huge_page(enable);
}

bool uvcpp_memory_pool_enterprise::is_huge_page_enabled() const
{
    return static_cast<const impl*>(impl_)->huge_page_enabled;
}

void uvcpp_memory_pool_enterprise::set_numa_node(int node)
{
    static_cast<impl*>(impl_)->numa_node = node;
    uvcpp::detail::uvcpp_set_numa_node(node);
}

int uvcpp_memory_pool_enterprise::get_numa_node() const
{
    return static_cast<const impl*>(impl_)->numa_node;
}

uvcpp_memory_pool_enterprise& uvcpp_memory_pool_enterprise::instance()
{
    static uvcpp_memory_pool_enterprise inst;
    return inst;
}

void uvcpp_memory_pool_enterprise::dump_stats() const
{
    size_t total, in_use, free;
    const_cast<uvcpp_memory_pool_enterprise*>(this)->get_stats(total, in_use, free);
    
    // 这里可以用 printf 或其他日志输出
    // 简化实现：
    pressure_level level = get_pressure_level();
    
    // 打印信息
    // printf("UVCPP Memory Pool Stats:\n");
    // printf("  Total allocated: %zu bytes\n", total);
    // printf("  In use: %zu blocks\n", in_use);
    // printf("  Free spans: %zu\n", free);
    // printf("  Pressure level: %d\n", (int)level);
}

// ========================
// 对外接口
// ========================
void* uvcpp_enterprise_alloc(size_t size)
{
    return uvcpp_memory_pool_enterprise::instance().alloc(size);
}

void uvcpp_enterprise_free(void* ptr)
{
    uvcpp_memory_pool_enterprise::instance().free_mem(ptr);
}

} // namespace uvcpp
