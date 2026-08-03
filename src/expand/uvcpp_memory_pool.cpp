/**
 * @file src/expand/uvcpp_memory_pool.cpp
 * @brief 高性能无锁内存池实现
 * @note uvcpp - 基于内嵌 Header 的无锁内存池
 */

#include "uvcpp_memory_pool.h"
#include <iostream>
#include <algorithm>

// Platform-specific includes
#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
#endif
#include <uvcpp/uvcpp_alloc.h>

namespace uvcpp {

// ============================================================
// 平台特定的内存分配函数
// ============================================================

namespace detail {

/// @brief 分配对齐内存（平台无关封装）
inline void* aligned_alloc_wrapper(size_t align, size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, align);
#elif defined(__APPLE__)
    void* ptr = nullptr;
    if (posix_memalign(&ptr, align, size) == 0) {
        return ptr;
    }
    return nullptr;
#else
    return aligned_alloc(align, size);
#endif
}

/// @brief 释放对齐内存
inline void aligned_free_wrapper(void* ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

/// @brief 获取页面大小
inline size_t get_page_size() {
    static size_t page_size = []() {
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return si.dwPageSize;
#else
        return sysconf(_SC_PAGESIZE);
#endif
    }();
    return page_size;
}

} // namespace detail

// ============================================================
// 构造函数和析构函数
// ============================================================

uvcpp_memory_pool::uvcpp_memory_pool()
    : global_tiny_pool_(nullptr)
    , global_small_pool_(nullptr)
    , global_medium_small_pool_(nullptr)
    , global_medium_pool_(nullptr)
    , global_medium_large_pool_(nullptr)
    , global_large_pool_(nullptr),
      global_extra_large_pool_(nullptr) {
  // warmup() is deferred to init_internal() after global pools are created
}

uvcpp_memory_pool::uvcpp_memory_pool(const memory_pool_config& config)
    : uvcpp_memory_pool() {
    init(config);
}

uvcpp_memory_pool::~uvcpp_memory_pool() {
    shutdown();
}

// ============================================================
// 移动语义
// ============================================================

uvcpp_memory_pool::uvcpp_memory_pool(uvcpp_memory_pool&& other) noexcept
    : config_(other.config_)
    , global_tiny_pool_(other.global_tiny_pool_)
    , global_small_pool_(other.global_small_pool_)
    , global_medium_small_pool_(other.global_medium_small_pool_)
    , global_medium_pool_(other.global_medium_pool_)
    , global_medium_large_pool_(other.global_medium_large_pool_)
    , global_large_pool_(other.global_large_pool_)
    , global_extra_large_pool_(other.global_extra_large_pool_) {
    // 移动统计信息
    stats_.active_allocations.store(other.stats_.active_allocations.load(std::memory_order_relaxed));
    stats_.total_allocations.store(other.stats_.total_allocations.load(std::memory_order_relaxed));
    stats_.total_deallocations.store(other.stats_.total_deallocations.load(std::memory_order_relaxed));
    stats_.failed_allocations.store(other.stats_.failed_allocations.load(std::memory_order_relaxed));
    stats_.allocated_bytes.store(other.stats_.allocated_bytes.load(std::memory_order_relaxed));
    stats_.freed_bytes.store(other.stats_.freed_bytes.load(std::memory_order_relaxed));
    stats_.cache_hits.store(other.stats_.cache_hits.load(std::memory_order_relaxed));
    stats_.cache_misses.store(other.stats_.cache_misses.load(std::memory_order_relaxed));

    global_tiny_count_.store(other.global_tiny_count_.load(std::memory_order_relaxed));
    global_small_count_.store(other.global_small_count_.load(std::memory_order_relaxed));
    global_medium_small_count_.store(other.global_medium_small_count_.load(std::memory_order_relaxed));
    global_medium_count_.store(other.global_medium_count_.load(std::memory_order_relaxed));
    global_medium_large_count_.store(other.global_medium_large_count_.load(std::memory_order_relaxed));
    global_large_count_.store(other.global_large_count_.load(std::memory_order_relaxed));
    global_extra_large_count_.store(other.global_extra_large_count_.load(std::memory_order_relaxed));

    initialized_.store(other.initialized_.load(std::memory_order_relaxed));
    shutdown_.store(other.shutdown_.load(std::memory_order_relaxed));

    other.global_tiny_pool_ = nullptr;
    other.global_small_pool_ = nullptr;
    other.global_medium_small_pool_ = nullptr;
    other.global_medium_pool_ = nullptr;
    other.global_medium_large_pool_ = nullptr;
    other.global_large_pool_ = nullptr;
    other.global_extra_large_pool_ = nullptr;
    other.initialized_ = false;
    other.shutdown_ = true;
}

uvcpp_memory_pool& uvcpp_memory_pool::operator=(uvcpp_memory_pool&& other) noexcept {
    if (this != &other) {
        shutdown();

        config_ = other.config_;
        global_tiny_pool_ = other.global_tiny_pool_;
        global_small_pool_ = other.global_small_pool_;
        global_medium_small_pool_ = other.global_medium_small_pool_;
        global_medium_pool_ = other.global_medium_pool_;
        global_medium_large_pool_ = other.global_medium_large_pool_;
        global_large_pool_ = other.global_large_pool_;
        global_extra_large_pool_ = other.global_extra_large_pool_;

        // 移动统计信息
        stats_.active_allocations.store(other.stats_.active_allocations.load(std::memory_order_relaxed));
        stats_.total_allocations.store(other.stats_.total_allocations.load(std::memory_order_relaxed));
        stats_.total_deallocations.store(other.stats_.total_deallocations.load(std::memory_order_relaxed));
        stats_.failed_allocations.store(other.stats_.failed_allocations.load(std::memory_order_relaxed));
        stats_.allocated_bytes.store(other.stats_.allocated_bytes.load(std::memory_order_relaxed));
        stats_.freed_bytes.store(other.stats_.freed_bytes.load(std::memory_order_relaxed));
        stats_.cache_hits.store(other.stats_.cache_hits.load(std::memory_order_relaxed));
        stats_.cache_misses.store(other.stats_.cache_misses.load(std::memory_order_relaxed));

        global_tiny_count_.store(other.global_tiny_count_.load(std::memory_order_relaxed));
        global_small_count_.store(other.global_small_count_.load(std::memory_order_relaxed));
        global_medium_small_count_.store(other.global_medium_small_count_.load(std::memory_order_relaxed));
        global_medium_count_.store(other.global_medium_count_.load(std::memory_order_relaxed));
        global_medium_large_count_.store(other.global_medium_large_count_.load(std::memory_order_relaxed));
        global_large_count_.store(other.global_large_count_.load(std::memory_order_relaxed));
        global_extra_large_count_.store(other.global_extra_large_count_.load(std::memory_order_relaxed));

        initialized_.store(other.initialized_.load(std::memory_order_relaxed));
        shutdown_.store(other.shutdown_.load(std::memory_order_relaxed));

        other.global_tiny_pool_ = nullptr;
        other.global_small_pool_ = nullptr;
        other.global_medium_small_pool_ = nullptr;
        other.global_medium_pool_ = nullptr;
        other.global_medium_large_pool_ = nullptr;
        other.global_large_pool_ = nullptr;
        other.global_extra_large_pool_ = nullptr;
        other.initialized_ = false;
        other.shutdown_ = true;
    }
    return *this;
}

// ============================================================
// 初始化和关闭
// ============================================================

bool uvcpp_memory_pool::init() {
    return init(memory_pool_config{});
}

bool uvcpp_memory_pool::init(const memory_pool_config& config) {
    return init_internal(config);
}

void uvcpp_memory_pool::shutdown() {
    if (!initialized_.load(std::memory_order_acquire)) {
        return;
    }

    // 先标记为销毁中，防止 thread_local 缓存析构时访问已销毁的队列
    destroying_.store(true, std::memory_order_release);
    shutdown_.store(true, std::memory_order_release);

    // 清理全局队列（手动管理）
    if (global_tiny_pool_) {
        global_tiny_pool_->shutdown();
        delete global_tiny_pool_;
        global_tiny_pool_ = nullptr;
    }
    if (global_small_pool_) {
        global_small_pool_->shutdown();
        delete global_small_pool_;
        global_small_pool_ = nullptr;
    }
    if (global_medium_small_pool_) {
        global_medium_small_pool_->shutdown();
        delete global_medium_small_pool_;
        global_medium_small_pool_ = nullptr;
    }
    if (global_medium_pool_) {
        global_medium_pool_->shutdown();
        delete global_medium_pool_;
        global_medium_pool_ = nullptr;
    }
    if (global_medium_large_pool_) {
        global_medium_large_pool_->shutdown();
        delete global_medium_large_pool_;
        global_medium_large_pool_ = nullptr;
    }
    if (global_large_pool_) {
        global_large_pool_->shutdown();
        delete global_large_pool_;
        global_large_pool_ = nullptr;
    }
    if (global_extra_large_pool_) {
        global_extra_large_pool_->shutdown();
        delete global_extra_large_pool_;
        global_extra_large_pool_ = nullptr;
    }

    initialized_.store(false, std::memory_order_release);
}

void uvcpp_memory_pool::reset() {
    stats_.active_allocations.store(0, std::memory_order_relaxed);
    stats_.allocated_bytes.store(0, std::memory_order_relaxed);
    stats_.freed_bytes.store(0, std::memory_order_relaxed);
    stats_.total_allocations.store(0, std::memory_order_relaxed);
    stats_.total_deallocations.store(0, std::memory_order_relaxed);
    stats_.cache_hits.store(0, std::memory_order_relaxed);
    stats_.cache_misses.store(0, std::memory_order_relaxed);
    stats_.failed_allocations.store(0, std::memory_order_relaxed);

    global_tiny_count_.store(0, std::memory_order_relaxed);
    global_small_count_.store(0, std::memory_order_relaxed);
    global_medium_small_count_.store(0, std::memory_order_relaxed);
    global_medium_count_.store(0, std::memory_order_relaxed);
    global_medium_large_count_.store(0, std::memory_order_relaxed);
    global_large_count_.store(0, std::memory_order_relaxed);
    global_extra_large_count_.store(0, std::memory_order_relaxed);
}

void uvcpp_memory_pool::reset_stats() {
    stats_.active_allocations.store(0, std::memory_order_relaxed);
    stats_.allocated_bytes.store(0, std::memory_order_relaxed);
    stats_.freed_bytes.store(0, std::memory_order_relaxed);
    stats_.total_allocations.store(0, std::memory_order_relaxed);
    stats_.total_deallocations.store(0, std::memory_order_relaxed);
    stats_.cache_hits.store(0, std::memory_order_relaxed);
    stats_.cache_misses.store(0, std::memory_order_relaxed);
    stats_.failed_allocations.store(0, std::memory_order_relaxed);

    global_tiny_count_.store(0, std::memory_order_relaxed);
    global_small_count_.store(0, std::memory_order_relaxed);
    global_medium_small_count_.store(0, std::memory_order_relaxed);
    global_medium_count_.store(0, std::memory_order_relaxed);
    global_medium_large_count_.store(0, std::memory_order_relaxed);
    global_large_count_.store(0, std::memory_order_relaxed);
    global_extra_large_count_.store(0, std::memory_order_relaxed);
}

// ============================================================
// 内存分配（核心实现）
// ============================================================

pool_block_header* uvcpp_memory_pool::allocate_block(memory_block_type type, size_t request_size) {
    // SUPER类型直接malloc（带header），不缓存
    if (type == memory_block_type::MEMORY_TYPE_SUPER) {
        // 计算总大小（header + 用户请求大小）
        size_t total_size = BLOCK_HEADER_SIZE + request_size;
        void *raw_ptr = uvcpp::uvcpp_alloc_bytes(total_size);
        if (!raw_ptr) {
            stats_.failed_allocations.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        pool_block_header* header = static_cast<pool_block_header*>(raw_ptr);
        header->flags = 0;
        header->size = total_size;
        header->user_data = 0;
        header->next = nullptr;
        header->set_type(type);

        stats_.allocated_bytes.fetch_add(total_size, std::memory_order_relaxed);
        return header;
    }

    // 根据类型从配置获取固定分配大小（用户数据大小）
    size_t block_size = config_.get_block_size(type);
    if (block_size == 0) {
        stats_.failed_allocations.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    // 计算总大小（header + 数据）
    size_t total_size = BLOCK_HEADER_SIZE + block_size;
    size_t align = config_.align;

    // 分配内存（包含 header）
    void* raw_ptr = detail::aligned_alloc_wrapper(align, total_size);
    if (!raw_ptr) {
        stats_.failed_allocations.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    // 初始化 header
    pool_block_header* header = static_cast<pool_block_header*>(raw_ptr);
    header->flags = 0;
    header->size = total_size;
    header->user_data = 0;
    header->next = nullptr;

    // 设置类型
    header->set_type(type);

    // 更新统计
    stats_.allocated_bytes.fetch_add(total_size, std::memory_order_relaxed);

    return header;
}

pool_block_header* uvcpp_memory_pool::pop_from_global_pool(memory_block_type type) {
    if (shutdown_.load(std::memory_order_acquire) || !initialized_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    mpsc_queue* queue = nullptr;
    std::atomic<uint64_t>* count = nullptr;

    // 查表法获取队列和计数器
    switch (type) {
        case memory_block_type::MEMORY_TYPE_TINY:
            queue = global_tiny_pool_;
            count = &global_tiny_count_;
            break;
        case memory_block_type::MEMORY_TYPE_SMALL:
            queue = global_small_pool_;
            count = &global_small_count_;
            break;
        case memory_block_type::MEDIUM_SMALL:
            queue = global_medium_small_pool_;
            count = &global_medium_small_count_;
            break;
        case memory_block_type::MEMORY_TYPE_MEDIUM:
            queue = global_medium_pool_;
            count = &global_medium_count_;
            break;
        case memory_block_type::MEDIUM_LARGE:
            queue = global_medium_large_pool_;
            count = &global_medium_large_count_;
            break;
        case memory_block_type::MEMORY_TYPE_LARGE:
            queue = global_large_pool_;
            count = &global_large_count_;
            break;
        case memory_block_type::EXTRA_LARGE:
            queue = global_extra_large_pool_;
            count = &global_extra_large_count_;
            break;
        default:
            return nullptr;
    }

    if (!queue) {
        return nullptr;
    }

    // 从全局池获取一个块（使用 acquire 语义保证可见性）
    pool_block_header* header = queue->pop();
    if (header && count) {
        count->fetch_sub(1, std::memory_order_relaxed);
    }

    return header;
}

void uvcpp_memory_pool::push_to_global_pool(memory_block_type type, pool_block_header* block) {
    if (!block) return;

    if (shutdown_.load(std::memory_order_acquire)) {
      // 池已关闭，直接释放
      if (type == memory_block_type::MEMORY_TYPE_SUPER) {
        uvcpp_free_bytes(block);
        return;
      }

      detail::aligned_free_wrapper(block);
      return;
    }

    mpsc_queue* queue = nullptr;
    std::atomic<uint64_t>* count = nullptr;

    // 查表法获取队列和计数器
    switch (type) {
        case memory_block_type::MEMORY_TYPE_TINY:
            queue = global_tiny_pool_;
            count = &global_tiny_count_;
            break;
        case memory_block_type::MEMORY_TYPE_SMALL:
            queue = global_small_pool_;
            count = &global_small_count_;
            break;
        case memory_block_type::MEDIUM_SMALL:
            queue = global_medium_small_pool_;
            count = &global_medium_small_count_;
            break;
        case memory_block_type::MEMORY_TYPE_MEDIUM:
            queue = global_medium_pool_;
            count = &global_medium_count_;
            break;
        case memory_block_type::MEDIUM_LARGE:
            queue = global_medium_large_pool_;
            count = &global_medium_large_count_;
            break;
        case memory_block_type::MEMORY_TYPE_LARGE:
            queue = global_large_pool_;
            count = &global_large_count_;
            break;
        case memory_block_type::EXTRA_LARGE:
            queue = global_extra_large_pool_;
            count = &global_extra_large_count_;
            break;
        default:
            // SUPER 类型直接释放
            uvcpp::uvcpp_free_bytes(block);
            return;
    }

    if (queue) {
        // 使用 release 语义确保内存顺序
        queue->push(block);
        if (count) {
            count->fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ============================================================
// 批量操作
// ============================================================

std::vector<void*> uvcpp_memory_pool::allocate_batch(size_t size, size_t count) {
    std::vector<void*> result;
    result.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        void* ptr = allocate(size);
        if (ptr) {
            result.push_back(ptr);
        }
    }

    return result;
}

void uvcpp_memory_pool::deallocate_batch(std::vector<void*>& pointers) {
    for (void* ptr : pointers) {
        deallocate(ptr);
    }
    pointers.clear();
}

// ============================================================
// 统计信息
// ============================================================

memory_pool_stats uvcpp_memory_pool::get_stats() const {
    memory_pool_stats stats;

    stats.active_allocations = stats_.active_allocations.load(std::memory_order_relaxed);
    stats.total_allocations = stats_.total_allocations.load(std::memory_order_relaxed);
    stats.total_deallocations = stats_.total_deallocations.load(std::memory_order_relaxed);
    stats.failed_allocations = stats_.failed_allocations.load(std::memory_order_relaxed);
    stats.allocated_bytes = stats_.allocated_bytes.load(std::memory_order_relaxed);
    stats.freed_bytes = stats_.freed_bytes.load(std::memory_order_relaxed);
    stats.cache_hits = stats_.cache_hits.load(std::memory_order_relaxed);
    stats.cache_misses = stats_.cache_misses.load(std::memory_order_relaxed);

    // 全局池计数（7种可缓存类型）
    stats.global_pool_blocks = global_tiny_count_.load(std::memory_order_relaxed)
                             + global_small_count_.load(std::memory_order_relaxed)
                             + global_medium_small_count_.load(std::memory_order_relaxed)
                             + global_medium_count_.load(std::memory_order_relaxed)
                             + global_medium_large_count_.load(std::memory_order_relaxed)
                             + global_large_count_.load(std::memory_order_relaxed)
                             + global_extra_large_count_.load(std::memory_order_relaxed);

    // 计算缓存命中率
    uint64_t total = stats.cache_hits + stats.cache_misses;
    if (total > 0) {
        stats.cache_hit_rate = static_cast<double>(stats.cache_hits) / static_cast<double>(total);
    }

    return stats;
}

void uvcpp_memory_pool::print_stats() const {
    memory_pool_stats stats = get_stats();

    std::cout << "[uvcpp_memory_pool] Statistics:" << std::endl;
    std::cout << "  Active allocations:   " << stats.active_allocations << std::endl;
    std::cout << "  Total allocations:    " << stats.total_allocations << std::endl;
    std::cout << "  Total deallocations:  " << stats.total_deallocations << std::endl;
    std::cout << "  Failed allocations:   " << stats.failed_allocations << std::endl;
    std::cout << "  Allocated bytes:      " << stats.allocated_bytes << std::endl;
    std::cout << "  Freed bytes:          " << stats.freed_bytes << std::endl;
    std::cout << "  Memory usage:         " << (stats.memory_usage_ratio() * 100.0) << "%" << std::endl;
    std::cout << "  Cache hits:          " << stats.cache_hits << std::endl;
    std::cout << "  Cache misses:        " << stats.cache_misses << std::endl;
    std::cout << "  Cache hit rate:      " << (stats.cache_hit_rate * 100.0) << "%" << std::endl;
    std::cout << "  Global pool blocks:  " << stats.global_pool_blocks << std::endl;
}

} // namespace uvcpp

// ============================================================
// 辅助函数实现
// ============================================================

namespace uvcpp {

bool uvcpp_memory_pool::init_internal(const memory_pool_config& config) {
    if (!config.validate()) {
        return false;
    }

    if (initialized_.load(std::memory_order_acquire)) {
        return true;
    }

    config_ = config;

    // 初始化全局队列（传入释放回调，由内存池控制释放方式）
    global_tiny_pool_ = new mpsc_queue([](pool_block_header *p) {
      if (p)
        detail::aligned_free_wrapper(p);
    });
    global_small_pool_ = new mpsc_queue([](pool_block_header *p) {
      if (p)
        detail::aligned_free_wrapper(p);
    });
    global_medium_small_pool_ = new mpsc_queue([](pool_block_header *p) {
      if (p)
        detail::aligned_free_wrapper(p);
    });
    global_medium_pool_ = new mpsc_queue([](pool_block_header *p) {
      if (p)
        detail::aligned_free_wrapper(p);
    });
    global_medium_large_pool_ = new mpsc_queue([](pool_block_header *p) {
      if (p)
        detail::aligned_free_wrapper(p);
    });
    global_large_pool_ = new mpsc_queue([](pool_block_header *p) {
      if (p)
        detail::aligned_free_wrapper(p);
    });
    global_extra_large_pool_ = new mpsc_queue([](pool_block_header *p) {
      if (p)
        detail::aligned_free_wrapper(p);
    });

    // 重置计数
    global_tiny_count_.store(0, std::memory_order_relaxed);
    global_small_count_.store(0, std::memory_order_relaxed);
    global_medium_small_count_.store(0, std::memory_order_relaxed);
    global_medium_count_.store(0, std::memory_order_relaxed);
    global_medium_large_count_.store(0, std::memory_order_relaxed);
    global_large_count_.store(0, std::memory_order_relaxed);
    global_extra_large_count_.store(0, std::memory_order_relaxed);

    // 重置统计
    reset_stats();

    // 预热：在全局池创建后预分配块，减少冷启动开销
    warmup();

    initialized_.store(true, std::memory_order_release);
    return true;
}

void uvcpp_memory_pool::warmup() {
    // 预热：提前分配一些块到全局池，减少冷启动开销
    // 7种类型全部预热，小块预热数量多，大块预热数量少
    constexpr size_t WARMUP_TINY = 64;
    constexpr size_t WARMUP_SMALL = 32;
    constexpr size_t WARMUP_MEDIUM_SMALL = 16;
    constexpr size_t WARMUP_MEDIUM = 8;
    constexpr size_t WARMUP_MEDIUM_LARGE = 4;
    constexpr size_t WARMUP_LARGE = 2;
    constexpr size_t WARMUP_EXTRA_LARGE = 1;

    // 预热 tiny 块 (64B)
    for (size_t i = 0; i < WARMUP_TINY; ++i) {
        pool_block_header* block = allocate_block(memory_block_type::MEMORY_TYPE_TINY, TINY_BLOCK_SIZE);
        if (block) {
            push_to_global_pool(memory_block_type::MEMORY_TYPE_TINY, block);
        }
    }

    // 预热 small 块 (256B)
    for (size_t i = 0; i < WARMUP_SMALL; ++i) {
        pool_block_header* block = allocate_block(memory_block_type::MEMORY_TYPE_SMALL, SMALL_BLOCK_SIZE);
        if (block) {
            push_to_global_pool(memory_block_type::MEMORY_TYPE_SMALL, block);
        }
    }

    // 预热 medium_small 块 (1KB)
    for (size_t i = 0; i < WARMUP_MEDIUM_SMALL; ++i) {
        pool_block_header* block = allocate_block(memory_block_type::MEDIUM_SMALL, MEDIUM_SMALL_BLOCK_SIZE);
        if (block) {
            push_to_global_pool(memory_block_type::MEDIUM_SMALL, block);
        }
    }

    // 预热 medium 块 (4KB)
    for (size_t i = 0; i < WARMUP_MEDIUM; ++i) {
        pool_block_header* block = allocate_block(memory_block_type::MEMORY_TYPE_MEDIUM, MEDIUM_BLOCK_SIZE);
        if (block) {
            push_to_global_pool(memory_block_type::MEMORY_TYPE_MEDIUM, block);
        }
    }

    // 预热 medium_large 块 (16KB)
    for (size_t i = 0; i < WARMUP_MEDIUM_LARGE; ++i) {
        pool_block_header* block = allocate_block(memory_block_type::MEDIUM_LARGE, MEDIUM_LARGE_BLOCK_SIZE);
        if (block) {
            push_to_global_pool(memory_block_type::MEDIUM_LARGE, block);
        }
    }

    // 预热 large 块 (64KB)
    for (size_t i = 0; i < WARMUP_LARGE; ++i) {
        pool_block_header* block = allocate_block(memory_block_type::MEMORY_TYPE_LARGE, LARGE_BLOCK_SIZE);
        if (block) {
            push_to_global_pool(memory_block_type::MEMORY_TYPE_LARGE, block);
        }
    }

    // 预热 extra_large 块 (256KB)
    for (size_t i = 0; i < WARMUP_EXTRA_LARGE; ++i) {
        pool_block_header* block = allocate_block(memory_block_type::EXTRA_LARGE, EXTRA_LARGE_BLOCK_SIZE);
        if (block) {
            push_to_global_pool(memory_block_type::EXTRA_LARGE, block);
        }
    }
}

} // namespace uvcpp
