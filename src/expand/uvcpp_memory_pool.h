/**
 * @file src/expand/uvcpp_memory_pool.h
 * @brief 高性能无锁内存池
 * @note uvcpp - 基于内嵌 Header 的无锁内存池
 *
 * **定位：** 独立可选的无锁内存池，不依赖页分配器，适合需要独立池实例的场景。
 * 与 uvcpp_memory_pool_enterprise 无依赖关系，可单独使用。
 *
 * 设计原则：
 * 1. 真正的无锁设计（原子操作 + lock-free 算法）
 * 2. 每个内存块内嵌 32 字节 Header（元数据自包含）
 * 3. 分类管理（SMALL/MEDIUM/LARGE/HUGE 四类）
 * 4. 线程本地缓存（单线程）+ 全局 MPSC 回收队列（多生产单消费）
 * 5. 直接使用 pool_block_header* 作为链表节点
 */

#pragma once

#ifndef SRC_EXPAND_UVCPP_MEMORY_POOL_H
#define SRC_EXPAND_UVCPP_MEMORY_POOL_H

#include <uvcpp/uvcpp_export.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>
#include <memory>
#include <new>

namespace uvcpp {

// ============================================================
// 编译时配置 - 内存块大小分类阈值（用户请求大小，不含header）
// ============================================================

/// 最小块阈值（64B）
constexpr size_t TINY_BLOCK_THRESHOLD = 64;

/// 小块阈值（256B）
constexpr size_t SMALL_BLOCK_THRESHOLD = 256;

/// 中小块阈值（1024B）
constexpr size_t MEDIUM_SMALL_BLOCK_THRESHOLD = 1024;

/// 中块阈值（4096B）
constexpr size_t MEDIUM_BLOCK_THRESHOLD = 4096;

/// 中大块阈值（16KB）
constexpr size_t MEDIUM_LARGE_BLOCK_THRESHOLD = 16384;

/// 大块阈值（64KB）
constexpr size_t LARGE_BLOCK_THRESHOLD = 65536;

/// 特大块阈值（256KB）
constexpr size_t EXTRA_LARGE_BLOCK_THRESHOLD = 262144;

/// 默认内存对齐字节数
constexpr size_t DEFAULT_MEMORY_ALIGNMENT = 16;

/// 每种类型的固定分配大小（用户数据大小，不含header）
constexpr size_t TINY_BLOCK_SIZE = 64;                   // 64B
constexpr size_t SMALL_BLOCK_SIZE = 256;                 // 256B
constexpr size_t MEDIUM_SMALL_BLOCK_SIZE = 1024;         // 1KB
constexpr size_t MEDIUM_BLOCK_SIZE = 4096;               // 4KB
constexpr size_t MEDIUM_LARGE_BLOCK_SIZE = 16384;        // 16KB
constexpr size_t LARGE_BLOCK_SIZE = 65536;               // 64KB
constexpr size_t EXTRA_LARGE_BLOCK_SIZE = 262144;        // 256KB

// ============================================================
// 常量定义
// ============================================================

/// Header 大小（32字节）
constexpr size_t BLOCK_HEADER_SIZE = 32;

/// 标志位定义
constexpr uint64_t FLAG_IN_USE = 1ULL << 0;        // 使用中标志
constexpr uint64_t FLAG_TYPE_MASK = 0x7FULL << 1;   // 类型掩码 (bits 1-6)
constexpr uint64_t FLAG_USER_MASK = 0xFFULL << 8;  // 用户标志掩码 (bits 8-15)

// 类型定义（bits 1-6）
enum class memory_block_type : uint8_t {
    MEMORY_TYPE_TINY = 0,        ///< 最小块（<= 64B）
    MEMORY_TYPE_SMALL = 1,      ///< 小块（64B - 256B）
    MEDIUM_SMALL = 2,           ///< 中小块（256B - 1024B）
    MEMORY_TYPE_MEDIUM = 3,     ///< 中块（1024B - 4096B）
    MEDIUM_LARGE = 4,           ///< 中大块（4096B - 16KB）
    MEMORY_TYPE_LARGE = 5,      ///< 大块（16KB - 64KB）
    EXTRA_LARGE = 6,            ///< 特大块（64KB - 256KB）
    MEMORY_TYPE_SUPER = 7,      ///< 超级大块（> 256KB，直接 malloc）
};

// ============================================================
// Header 结构（32字节，每个内存块头部）
// ============================================================

/// @brief 内存块 Header（32字节，内嵌在每个内存块）
struct pool_block_header {
    uint64_t flags;                    // bit0: in_use, bit1-4: type, bit8-15: user_flags
    uint64_t size;                     // 内存块总大小（含header）
    uint64_t user_data;                // 用户自定义数据
    pool_block_header* next;           // 指向下一个空闲块（用于链表，普通指针）

    /// 获取使用状态
    inline bool is_in_use() const {
        return (flags & FLAG_IN_USE) != 0;
    }

    /// 设置使用状态
    inline void set_in_use(bool in_use) {
        if (in_use) {
            flags |= FLAG_IN_USE;
        } else {
            flags &= ~FLAG_IN_USE;
        }
    }

    /// 获取类型
    inline memory_block_type get_type() const {
        return static_cast<memory_block_type>((flags & FLAG_TYPE_MASK) >> 1);
    }

    /// 设置类型
    inline void set_type(memory_block_type type) {
        flags = (flags & ~FLAG_TYPE_MASK) | (static_cast<uint64_t>(type) << 1);
    }

    /// 获取用户数据
    inline uint64_t get_user_data() const {
        return user_data;
    }

    /// 设置用户数据
    inline void set_user_data(uint64_t data) {
        user_data = data;
    }

    /// 获取指向用户数据的指针（返回 header 后的地址）
    /// @note 强制内联，确保零开销
    inline void* to_user_ptr() noexcept {
        return reinterpret_cast<uint8_t*>(this) + BLOCK_HEADER_SIZE;
    }

    /// 从用户指针获取 header
    /// @note 强制内联，确保零开销
    inline static pool_block_header* from_user_ptr(void* ptr) noexcept {
        return reinterpret_cast<pool_block_header*>(reinterpret_cast<uint8_t*>(ptr) - BLOCK_HEADER_SIZE);
    }
};

// ============================================================
// 内存池配置
// ============================================================

/// @brief 内存池详细配置
struct memory_pool_config {
    /// 内存对齐方式
    size_t align = DEFAULT_MEMORY_ALIGNMENT;

    /// 最小块大小（<= 64B），默认64B
    size_t tiny_block_size = TINY_BLOCK_SIZE;

    /// 小块大小（64B - 256B），默认256B
    size_t small_block_size = SMALL_BLOCK_SIZE;

    /// 中小块大小（256B - 1024B），默认1024B
    size_t medium_small_block_size = MEDIUM_SMALL_BLOCK_SIZE;

    /// 中块大小（1024B - 4096B），默认4096B
    size_t medium_block_size = MEDIUM_BLOCK_SIZE;

    /// 中大块大小（4096B - 16KB），默认16384B
    size_t medium_large_block_size = MEDIUM_LARGE_BLOCK_SIZE;

    /// 大块大小（16KB - 64KB），默认65536B
    size_t large_block_size = LARGE_BLOCK_SIZE;

    /// 特大块大小（64KB - 256KB），默认262144B
    size_t extra_large_block_size = EXTRA_LARGE_BLOCK_SIZE;

    /// 最大总内存限制（0 = 无限制），单位：字节。
    ///
    /// 口径是**池当前持有的实占字节**（含每块的 `BLOCK_HEADER_SIZE` 头部），不是
    /// 「历史上累计分配过多少」：从线程缓存 / 全局池**命中**的复用不占额度，归还
    /// 之后额度**会还回来**。所以它挡的是池的驻留内存，不是分配次数。
    ///
    /// 触顶时 `allocate()` 返回 `nullptr` 并计一次 `failed_allocations` —— 与
    /// `malloc` 失败同一个表现，调用方本来就要判空。
    ///
    /// `MEMORY_TYPE_SUPER`（>256 KiB）也在口径内：它不进任何缓存，释放时由
    /// `push_to_global_pool()` 直接释放并把额度还回来，所以并发持有一个大块同样受
    /// 这个上限约束（只是它不占缓存的驻留内存，因为压根不缓存）。
    ///
    /// 一处不在这个口径里：`shutdown()` 与本池重新 `init()` 都会把额度清零。
    size_t max_total_memory = 0;

    /// @brief 获取指定类型对应的块大小
    /// @param type 内存块类型
    /// @return 该类型的块大小（用户数据大小），SUPER类型返回0
    inline size_t get_block_size(memory_block_type type) const {
        switch (type) {
            case memory_block_type::MEMORY_TYPE_TINY:
                return tiny_block_size;
            case memory_block_type::MEMORY_TYPE_SMALL:
                return small_block_size;
            case memory_block_type::MEDIUM_SMALL:
                return medium_small_block_size;
            case memory_block_type::MEMORY_TYPE_MEDIUM:
                return medium_block_size;
            case memory_block_type::MEDIUM_LARGE:
                return medium_large_block_size;
            case memory_block_type::MEMORY_TYPE_LARGE:
                return large_block_size;
            case memory_block_type::EXTRA_LARGE:
                return extra_large_block_size;
            default:
                return 0; // SUPER类型不缓存
        }
    }

    /// @brief 获取指定大小的内存块类型
    /// @param size 请求的内存大小（用户数据大小）
    /// @return 对应的内存块类型
    inline memory_block_type get_block_type(size_t size) const {
        // 使用二分查找或查表法优化（针对常见小尺寸优化）
        if (size <= tiny_block_size) {
            return memory_block_type::MEMORY_TYPE_TINY;
        }
        if (size <= small_block_size) {
            return memory_block_type::MEMORY_TYPE_SMALL;
        }
        if (size <= medium_small_block_size) {
            return memory_block_type::MEDIUM_SMALL;
        }
        if (size <= medium_block_size) {
            return memory_block_type::MEMORY_TYPE_MEDIUM;
        }
        if (size <= medium_large_block_size) {
            return memory_block_type::MEDIUM_LARGE;
        }
        if (size <= large_block_size) {
            return memory_block_type::MEMORY_TYPE_LARGE;
        }
        if (size <= extra_large_block_size) {
            return memory_block_type::EXTRA_LARGE;
        }
        return memory_block_type::MEMORY_TYPE_SUPER;
    }

    /// @brief 类型转数组索引（用于查表法优化）
    /// @note 枚举值 0-7 可直接作为索引，SUPER 类型返回特殊值
    static constexpr size_t type_to_index(memory_block_type type) noexcept {
        return static_cast<size_t>(type);
    }

    /// @brief 块大小数组（用户数据大小，不含header）
    static constexpr size_t block_sizes[8] = {
        TINY_BLOCK_SIZE,                    // 0: TINY
        SMALL_BLOCK_SIZE,                   // 1: SMALL
        MEDIUM_SMALL_BLOCK_SIZE,            // 2: MEDIUM_SMALL
        MEDIUM_BLOCK_SIZE,                  // 3: MEDIUM
        MEDIUM_LARGE_BLOCK_SIZE,            // 4: MEDIUM_LARGE
        LARGE_BLOCK_SIZE,                   // 5: LARGE
        EXTRA_LARGE_BLOCK_SIZE,              // 6: EXTRA_LARGE
        0                                   // 7: SUPER (不缓存)
    };

    /// 检查配置是否有效（必须递增）
    bool validate() const {
        return tiny_block_size > 0
            && small_block_size >= tiny_block_size
            && medium_small_block_size >= small_block_size
            && medium_block_size >= medium_small_block_size
            && medium_large_block_size >= medium_block_size
            && large_block_size >= medium_large_block_size
            && extra_large_block_size >= large_block_size;
    }
};

// ============================================================
// 统计信息结构体
// ============================================================

/// @brief 内存池统计信息
struct memory_pool_stats {
    uint64_t active_allocations = 0;
    uint64_t total_allocations = 0;
    uint64_t total_deallocations = 0;
    uint64_t failed_allocations = 0;
    uint64_t allocated_bytes = 0;
    uint64_t freed_bytes = 0;
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t global_pool_blocks = 0;
    double cache_hit_rate = 0.0;
    uint64_t growth_count = 0;

    /// @brief `(allocated_bytes - freed_bytes) / allocated_bytes`。
    /// @warning **两个计数器口径不同，这个读数不是使用率。** `allocated_bytes` 只在
    ///          **新建块**时累加，`freed_bytes` 却在**每次** `deallocate()` 时累加 ——
    ///          缓存命中的分配不进前者、它的释放照样进后者。自由块一多就出现
    ///          `freed_bytes > allocated_bytes`，而两个操作数都是 `uint64_t`，
    ///          减法**回绕成天文数字**，比值于是是垃圾（或 `inf`）。
    ///          要判"这个池现在占了多少"，用 `active_allocations` 配自己的块大小算，
    ///          或者拿 `failed_allocations` 判有没有触到 `max_total_memory` ——
    ///          池把额度记在一个**私有**的 `held_bytes_` 上，没有公开读法。
    double memory_usage_ratio() const {
        if (allocated_bytes == 0) return 0.0;
        return static_cast<double>(allocated_bytes - freed_bytes)
             / static_cast<double>(allocated_bytes);
    }
};

// ============================================================
// MPSC（多生产者单消费者）无锁队列 - 用于全局回收池
// ============================================================

/// @brief MPSC（多生产者单消费者）无锁队列
/// @note 多生产者从头部 push，单消费者从头部 pop（LIFO 顺序）
///
/// **线程安全约束：**
/// - push() 可从多个线程并发调用
/// - pop() 仅限单消费者线程调用
/// - **禁止在 shutdown() 期间并发 push/pop**（否则有 ABA 风险）
class mpsc_queue {
private:
    std::atomic<pool_block_header*> head_;   // 头部（单消费者访问）
    std::atomic<bool> shutting_down_{false};
    using dealloc_func = std::function<void(pool_block_header*)>;
    dealloc_func dealloc_func_;

  public:
    /// @brief 带自定义释放函数的构造函数
    explicit mpsc_queue(dealloc_func dealloc)
        : head_(nullptr), dealloc_func_(std::move(dealloc)) {
        if (!dealloc_func_) {
            dealloc_func_ = [](pool_block_header* p) {
                if (p) ::operator delete(p);
            };
        }
    }

    ~mpsc_queue() {
        shutdown();
    }

    // 禁止拷贝
    mpsc_queue(const mpsc_queue&) = delete;
    mpsc_queue& operator=(const mpsc_queue&) = delete;

    /// @brief 多生产者放入元素（可多线程并发调用）
    /// @note 从头部插入，使用改良的 CAS 循环减少竞争
    inline bool push(pool_block_header *item) {
      if (!item || shutting_down_.load(std::memory_order_acquire)) {
        return false;
      }

      // 标记为未使用
      item->set_in_use(false);

      // 快速路径：尝试一次 CAS 成功（常见情况）
      pool_block_header *old_head = head_.load(std::memory_order_relaxed);
      item->next = old_head;

      // CAS 循环
      while (!head_.compare_exchange_weak(old_head, item,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
        // CAS 失败，重试
        item->next = old_head;
      }

      return true;
    }

    /// @brief 消费者取出一个元素
    /// @note 从头部取出，返回给用户
    inline pool_block_header *pop() {
      // 循环直到成功或队列为空
      while (true) {
        // 1. 获取当前头节点
        pool_block_header *curr_head = head_.load(std::memory_order_acquire);
        if (!curr_head) {
          return nullptr; // 队列为空
        }

        // 2. 获取下一个节点
        // 注意：这里读取next可能不安全，但CAS会验证
        pool_block_header *curr_next = curr_head->next;

        // 3. 尝试原子更新
        if (head_.compare_exchange_weak(
                curr_head,                    // 期望：当前头节点
                curr_next,                    // 新值：下一个节点
                std::memory_order_release,    // 成功：需要release
                std::memory_order_acquire)) { // 失败：需要acquire重新加载

          // 4. CAS成功，安全地返回节点
          curr_head->next = nullptr; // 清除next
          return curr_head;
        }
        // 5. CAS失败，重试
      }
    }

    /// @brief 检查队列是否为空
    inline bool empty() const {
        return head_.load(std::memory_order_acquire) == nullptr;
    }

    /// @brief 清空队列（仅在 shutdown 时调用）
    /// @warning 调用方必须保证 shutdown 期间无并发 push/pop
    void shutdown() {
      // 1. 设置关闭标志
      shutting_down_.store(true, std::memory_order_seq_cst);

      // 2. 内存屏障，确保所有线程看到这个标志
      std::atomic_thread_fence(std::memory_order_seq_cst);

      // 3. 等待短时间让正在进行的操作完成（缓解 ABA）
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

      // 4. 原子地获取并清空链表
      pool_block_header *old_head =
          head_.exchange(nullptr, std::memory_order_acquire);

      // 5. 安全删除所有节点（此时已无并发操作）
      pool_block_header *curr = old_head;
      while (curr) {
        pool_block_header *next = curr->next;
        if (dealloc_func_) {
          dealloc_func_(curr);
        }
        curr = next;
      }
    }
};

// ============================================================
// 线程本地缓存（单线程，直接用指针链表）
// ============================================================

/// @brief 线程本地缓存结构
/// @note 单线程使用，不需要任何锁。析构时**会**把缓存归还全局池 —— 但前提是这个缓存
///       被 `uvcpp_memory_pool::init_thread_cache()` 配过释放回调。默认构造的缓存
///       `pool_ptr_`/`release_func_` 都是空，`release_all()` 第一句就返回，挂在链上的
///       块随之**泄漏**（不报错、不计数、`detect_leaks()` 也看不出来）。
struct thread_local_cache {
    // 最小块缓存（<= 64B）
    pool_block_header* tiny_head = nullptr;
    pool_block_header* tiny_tail = nullptr;
    size_t tiny_count = 0;

    // 小块缓存（64B - 256B）
    pool_block_header* small_head = nullptr;
    pool_block_header* small_tail = nullptr;
    size_t small_count = 0;

    // 中小块缓存（256B - 1024B）
    pool_block_header* medium_small_head = nullptr;
    pool_block_header* medium_small_tail = nullptr;
    size_t medium_small_count = 0;

    // 中块缓存（1024B - 4096B）
    pool_block_header* medium_head = nullptr;
    pool_block_header* medium_tail = nullptr;
    size_t medium_count = 0;

    // 中大块缓存（4096B - 16KB）
    pool_block_header* medium_large_head = nullptr;
    pool_block_header* medium_large_tail = nullptr;
    size_t medium_large_count = 0;

    // 大块缓存（16KB - 64KB）
    pool_block_header* large_head = nullptr;
    pool_block_header* large_tail = nullptr;
    size_t large_count = 0;

    // 特大块缓存（64KB - 256KB）
    pool_block_header* extra_large_head = nullptr;
    pool_block_header* extra_large_tail = nullptr;
    size_t extra_large_count = 0;

    // 容量限制（优化后的值，减少全局池访问）
    static constexpr size_t TINY_CAPACITY = 1024;      // 64B 块
    static constexpr size_t SMALL_CAPACITY = 512;       // 256B 块
    static constexpr size_t MEDIUM_SMALL_CAPACITY = 256; // 1KB 块
    static constexpr size_t MEDIUM_CAPACITY = 128;     // 4KB 块
    static constexpr size_t MEDIUM_LARGE_CAPACITY = 64; // 16KB 块
    static constexpr size_t LARGE_CAPACITY = 32;       // 64KB 块
    static constexpr size_t EXTRA_LARGE_CAPACITY = 16; // 256KB 块

    // 友元声明：允许 uvcpp_memory_pool 访问私有成员
    friend class uvcpp_memory_pool;

private:
    /// 指向所属内存池的指针
    void* pool_ptr_ = nullptr;
    /// 释放回调函数类型
    using release_func_type = void(*)(void*, pool_block_header*);
    /// 释放回调函数
    release_func_type release_func_ = nullptr;

public:
    /// @brief 默认构造函数
    thread_local_cache() = default;

    /// @brief 带回调函数的构造函数
    thread_local_cache(void* pool, release_func_type func)
        : pool_ptr_(pool), release_func_(func) {}

    /// @brief 析构函数 - 归还缓存的块到全局池
    /// @warning 「归还」只在**配过释放回调**时发生（见 @ref set_release_func）。
    ///          默认构造的缓存回调为空，`release_all()` 直接返回 —— 挂着的块**泄漏**。
    ~thread_local_cache() {
        release_all();
    }

    /// @brief 禁止拷贝
    thread_local_cache(const thread_local_cache&) = delete;
    thread_local_cache& operator=(const thread_local_cache&) = delete;

    /// @brief 允许移动
    thread_local_cache(thread_local_cache&& other) noexcept
        : tiny_head(other.tiny_head), tiny_tail(other.tiny_tail), tiny_count(other.tiny_count),
          small_head(other.small_head), small_tail(other.small_tail), small_count(other.small_count),
          medium_small_head(other.medium_small_head), medium_small_tail(other.medium_small_tail), medium_small_count(other.medium_small_count),
          medium_head(other.medium_head), medium_tail(other.medium_tail), medium_count(other.medium_count),
          medium_large_head(other.medium_large_head), medium_large_tail(other.medium_large_tail), medium_large_count(other.medium_large_count),
          large_head(other.large_head), large_tail(other.large_tail), large_count(other.large_count),
          extra_large_head(other.extra_large_head), extra_large_tail(other.extra_large_tail), extra_large_count(other.extra_large_count),
          pool_ptr_(other.pool_ptr_), release_func_(other.release_func_) {
        other.reset();
    }

    thread_local_cache& operator=(thread_local_cache&& other) noexcept {
        if (this != &other) {
            release_all();
            tiny_head = other.tiny_head; tiny_tail = other.tiny_tail; tiny_count = other.tiny_count;
            small_head = other.small_head; small_tail = other.small_tail; small_count = other.small_count;
            medium_small_head = other.medium_small_head; medium_small_tail = other.medium_small_tail; medium_small_count = other.medium_small_count;
            medium_head = other.medium_head; medium_tail = other.medium_tail; medium_count = other.medium_count;
            medium_large_head = other.medium_large_head; medium_large_tail = other.medium_large_tail; medium_large_count = other.medium_large_count;
            large_head = other.large_head; large_tail = other.large_tail; large_count = other.large_count;
            extra_large_head = other.extra_large_head; extra_large_tail = other.extra_large_tail; extra_large_count = other.extra_large_count;
            pool_ptr_ = other.pool_ptr_;
            release_func_ = other.release_func_;
            other.reset();
        }
        return *this;
    }

    /// @brief 设置释放回调
    inline void set_release_func(void* pool, release_func_type func) {
        pool_ptr_ = pool;
        release_func_ = func;
    }

    /// @brief 释放所有缓存的块到全局池
    inline void release_all() {
        if (!pool_ptr_ || !release_func_) return;

        // 依次释放所有类型的缓存
        release_chain(tiny_head, tiny_tail, tiny_count);
        release_chain(small_head, small_tail, small_count);
        release_chain(medium_small_head, medium_small_tail, medium_small_count);
        release_chain(medium_head, medium_tail, medium_count);
        release_chain(medium_large_head, medium_large_tail, medium_large_count);
        release_chain(large_head, large_tail, large_count);
        release_chain(extra_large_head, extra_large_tail, extra_large_count);
    }

    /// @brief 手动释放缓存（用于测试）
    inline void release() {
        release_all();
    }

    /// @brief 获取当前缓存的块数量
    inline size_t size() const {
        return tiny_count + small_count + medium_small_count + medium_count
             + medium_large_count + large_count + extra_large_count;
    }

    /// @brief 重置缓存（不调用回调）
    inline void reset() {
        tiny_head = tiny_tail = nullptr; tiny_count = 0;
        small_head = small_tail = nullptr; small_count = 0;
        medium_small_head = medium_small_tail = nullptr; medium_small_count = 0;
        medium_head = medium_tail = nullptr; medium_count = 0;
        medium_large_head = medium_large_tail = nullptr; medium_large_count = 0;
        large_head = large_tail = nullptr; large_count = 0;
        extra_large_head = extra_large_tail = nullptr; extra_large_count = 0;
    }

private:
    /// @brief 释放一条链表
    inline void release_chain(pool_block_header*& head, pool_block_header*& tail, size_t& count) {
        if (!head || !pool_ptr_ || !release_func_) return;

        pool_block_header* curr = head;
        while (curr) {
            pool_block_header* next = curr->next;
            curr->next = nullptr;
            release_func_(pool_ptr_, curr);
            curr = next;
        }
        head = tail = nullptr;
        count = 0;
    }

    /// @brief 从指定类型缓存获取块（查表法优化版本）
    /// @return 获取的块指针，失败返回 nullptr
    inline pool_block_header* pop(memory_block_type type) {
        size_t idx = static_cast<size_t>(type);
        if (idx >= 7) return nullptr;

        pool_block_header* head = nullptr;
        size_t* count_ptr = nullptr;

        // 查表法：直接数组索引替代 switch-case
        switch (idx) {
            case 0: // TINY
                if (tiny_head) {
                    head = tiny_head;
                    tiny_head = head->next;
                    if (!tiny_head) tiny_tail = nullptr;
                    count_ptr = &tiny_count;
                }
                break;
            case 1: // SMALL
                if (small_head) {
                    head = small_head;
                    small_head = head->next;
                    if (!small_head) small_tail = nullptr;
                    count_ptr = &small_count;
                }
                break;
            case 2: // MEDIUM_SMALL
                if (medium_small_head) {
                    head = medium_small_head;
                    medium_small_head = head->next;
                    if (!medium_small_head) medium_small_tail = nullptr;
                    count_ptr = &medium_small_count;
                }
                break;
            case 3: // MEDIUM
                if (medium_head) {
                    head = medium_head;
                    medium_head = head->next;
                    if (!medium_head) medium_tail = nullptr;
                    count_ptr = &medium_count;
                }
                break;
            case 4: // MEDIUM_LARGE
                if (medium_large_head) {
                    head = medium_large_head;
                    medium_large_head = head->next;
                    if (!medium_large_head) medium_large_tail = nullptr;
                    count_ptr = &medium_large_count;
                }
                break;
            case 5: // LARGE
                if (large_head) {
                    head = large_head;
                    large_head = head->next;
                    if (!large_head) large_tail = nullptr;
                    count_ptr = &large_count;
                }
                break;
            case 6: // EXTRA_LARGE
                if (extra_large_head) {
                    head = extra_large_head;
                    extra_large_head = head->next;
                    if (!extra_large_head) extra_large_tail = nullptr;
                    count_ptr = &extra_large_count;
                }
                break;
        }

        if (head) {
            --(*count_ptr);
            head->next = nullptr;
        }
        return head;
    }

    /// @brief 将块归还到缓存
    /// @return true=本缓存已接手（放进了本地缓存）；false=本缓存不管，请调用方放全局池
    /// @note `SUPER`（>256 KiB）**不归本地缓存管**，这里返回 `false` —— 唯一调用方
    ///       `deallocate()` 随即走 `push_to_global_pool()`，那条路的 `default:` 分支
    ///       把它 `uvcpp_free_bytes()` 掉、并经 `release_bytes()` 把额度还回去。
    /// @warning 这个分支**必须**返回 `false`。返回 `true`（历史实现如此）时调用方见
    ///          `true` 就不去放全局池，`push_to_global_pool()` 里那两条写着「SUPER
    ///          直接释放」的分支**一条都走不到**：每块 >256 KiB 的内存在释放时既不
    ///          还给系统、额度也不退，而 `update_dealloc_stats()` 照常记一次「已释放」，
    ///          `stats()` 与 `detect_leaks()` 都看不出来。回归用例：
    ///          `tests/expand/memory_pool_test.cpp` 的 `test_pool_super_block_quota_returned`。
    inline bool push(pool_block_header* block) {
        if (!block) return true;

        memory_block_type type = block->get_type();
        block->set_in_use(false);
        block->next = nullptr;

        size_t idx = static_cast<size_t>(type);
        if (idx >= 7) return false;   // SUPER：交给 push_to_global_pool() 真正释放

        size_t* count_ptr = nullptr;
        size_t capacity;
        pool_block_header** head_ptr = nullptr;
        pool_block_header** tail_ptr = nullptr;

        // 查表法获取缓存信息
        switch (idx) {
            case 0: // TINY
                count_ptr = &tiny_count;
                capacity = TINY_CAPACITY;
                head_ptr = &tiny_head;
                tail_ptr = &tiny_tail;
                break;
            case 1: // SMALL
                count_ptr = &small_count;
                capacity = SMALL_CAPACITY;
                head_ptr = &small_head;
                tail_ptr = &small_tail;
                break;
            case 2: // MEDIUM_SMALL
                count_ptr = &medium_small_count;
                capacity = MEDIUM_SMALL_CAPACITY;
                head_ptr = &medium_small_head;
                tail_ptr = &medium_small_tail;
                break;
            case 3: // MEDIUM
                count_ptr = &medium_count;
                capacity = MEDIUM_CAPACITY;
                head_ptr = &medium_head;
                tail_ptr = &medium_tail;
                break;
            case 4: // MEDIUM_LARGE
                count_ptr = &medium_large_count;
                capacity = MEDIUM_LARGE_CAPACITY;
                head_ptr = &medium_large_head;
                tail_ptr = &medium_large_tail;
                break;
            case 5: // LARGE
                count_ptr = &large_count;
                capacity = LARGE_CAPACITY;
                head_ptr = &large_head;
                tail_ptr = &large_tail;
                break;
            case 6: // EXTRA_LARGE
                count_ptr = &extra_large_count;
                capacity = EXTRA_LARGE_CAPACITY;
                head_ptr = &extra_large_head;
                tail_ptr = &extra_large_tail;
                break;
            default:
                return true;
        }

        if (*count_ptr >= capacity) {
            return false;
        }

        if (*tail_ptr) {
            (*tail_ptr)->next = block;
            *tail_ptr = block;
        } else {
            *head_ptr = *tail_ptr = block;
        }
        ++(*count_ptr);
        return true;
    }

    /// @brief 获取缓存中指定类型的数量
    inline size_t count(memory_block_type type) const {
        switch (type) {
            case memory_block_type::MEMORY_TYPE_TINY:
                return tiny_count;
            case memory_block_type::MEMORY_TYPE_SMALL:
                return small_count;
            case memory_block_type::MEDIUM_SMALL:
                return medium_small_count;
            case memory_block_type::MEMORY_TYPE_MEDIUM:
                return medium_count;
            case memory_block_type::MEDIUM_LARGE:
                return medium_large_count;
            case memory_block_type::MEMORY_TYPE_LARGE:
                return large_count;
            case memory_block_type::EXTRA_LARGE:
                return extra_large_count;
            default:
                return 0;
        }
    }

    /// @brief 获取总缓存数量
    inline size_t total() const {
        return tiny_count + small_count + medium_small_count + medium_count
             + medium_large_count + large_count + extra_large_count;
    }
};

// ============================================================
// 核心类：高性能内存池
// ============================================================

class UVCPP_API uvcpp_memory_pool {
public:
    uvcpp_memory_pool();
    explicit uvcpp_memory_pool(const memory_pool_config& config);
    ~uvcpp_memory_pool();

    // 禁止拷贝
    uvcpp_memory_pool(const uvcpp_memory_pool&) = delete;
    uvcpp_memory_pool& operator=(const uvcpp_memory_pool&) = delete;

    // 允许移动
    uvcpp_memory_pool(uvcpp_memory_pool&&) noexcept;
    uvcpp_memory_pool& operator=(uvcpp_memory_pool&&) noexcept;

    // ============================================================
    // 初始化和关闭
    // ============================================================

    bool init();
    bool init(const memory_pool_config& config);
    void shutdown();
    /// @brief 把**统计计数**清零。
    /// @warning 名字与行为不符：当前实现与 `reset_stats()` 逐行相同（只差函数名），
    ///          既不释放也不重排池里的块 —— 调用它不会归还一个字节。要真回收用
    ///          `shutdown()`（或 `release_thread_cache()` 推回本线程缓存）。
    void reset();
    void warmup();
    bool is_initialized() const {
        return initialized_.load(std::memory_order_acquire);
    }

    // ============================================================
    // 内存分配
    // ============================================================

    void* allocate(size_t size);
    /// @brief 申请 `size` 字节，并把 `size` 向上凑整到 `align` 的倍数。
    /// @warning **返回的指针不保证按 `align` 对齐** —— `align` 只参与凑整，真正的
    ///          对齐由 `config_.align` 决定（默认 `DEFAULT_MEMORY_ALIGNMENT = 16`）。
    ///          传 `align = 64` 拿到的仍是 16 对齐的指针（`tests/expand/memory_pool_test.cpp`
    ///          里有断言钉着这个行为）。要真按 `align` 对齐，得自己配 `config_.align`。
    void* allocate_aligned(size_t size, size_t align);
    void deallocate(void* ptr);

    // ============================================================
    // 批量操作
    // ============================================================

    std::vector<void*> allocate_batch(size_t size, size_t count);
    void deallocate_batch(std::vector<void*>& pointers);

    // ============================================================
    // 内存统计
    // ============================================================

    memory_pool_stats get_stats() const;
    uint64_t active_allocations() const {
        return stats_.active_allocations.load(std::memory_order_relaxed);
    }
    uint64_t total_allocations() const {
        return stats_.total_allocations.load(std::memory_order_relaxed);
    }
    uint64_t total_deallocations() const {
        return stats_.total_deallocations.load(std::memory_order_relaxed);
    }
    uint64_t cached_blocks() const;
    double usage_ratio() const {
        return stats_.memory_usage_ratio();
    }

    // ============================================================
    // 调试和诊断
    // ============================================================

    bool detect_leaks() const {
        return active_allocations() > 0;
    }
    void print_stats() const;
    void reset_stats();

    // ============================================================
    // 线程本地缓存
    // ============================================================

    inline static thread_local_cache &get_thread_cache() {
        static thread_local thread_local_cache cache;
        return cache;
    }

    /// @brief 初始化线程本地缓存（设置回调，线程首次使用内存池时调用）
    /// @note 线程退出时析构函数会自动将缓存归还到全局池
    inline void init_thread_cache() {
        thread_local_cache& cache = get_thread_cache();
        cache.set_release_func(this, &uvcpp_memory_pool::static_release_callback);
    }

    /// @brief 手动释放当前线程的本地缓存（用于测试）
    inline void release_thread_cache() {
        get_thread_cache().release();
    }

    /// @brief 获取当前线程本地缓存的块数量（用于测试）
    inline size_t thread_cache_size() const {
        return get_thread_cache().size();
    }

    /// @brief 静态释放回调函数（供 thread_local_cache 析构时调用）
    /// @note **一律转交 `push_to_global_pool()`，不要在这里自己判状态再释放。**
    ///       那个函数第一件事就是查 `shutdown_`：池已关闭时按类型用**各自的分配器**
    ///       释放（`SUPER` → `uvcpp_free_bytes()`，其余 → `aligned_free_wrapper()`），
    ///       未关闭时正常入队。所以这里既不需要额外的状态分支，也不能自己动手。
    /// @warning 自己写 `::operator delete(block)` 是**堆损坏**：块来自
    ///          `aligned_alloc_wrapper()`（Windows 上是 `_aligned_malloc`）或
    ///          `uvcpp_alloc_bytes()`，**都不是 `operator new` 分配的**。实测在
    ///          「`shutdown()` 之后线程缓存里还挂着块」时直接
    ///          `0xC0000374 STATUS_HEAP_CORRUPTION`。
    static void static_release_callback(void* pool, pool_block_header* block) {
        if (!pool || !block) return;
        auto* self = static_cast<uvcpp_memory_pool*>(pool);
        self->push_to_cache(block);
    }

    /// @brief 将内存块放入全局公共池
    inline void push_to_cache(pool_block_header* block) {
        if (!block) return;

        memory_block_type type = block->get_type();
        block->set_in_use(false);
        block->next = nullptr;

        push_to_global_pool(type, block);
    }

private:
    /// @brief 快速路径：从本地缓存获取块
    /// @note 缓存命中时，仅更新最少必要统计，减少原子操作开销
    inline void* allocate_from_cache(pool_block_header* header) {
        if (header) {
            header->set_in_use(true);
            // 合并统计更新：一次原子操作完成多个计数
            uint64_t prev = stats_.cache_hits.fetch_add(1, std::memory_order_relaxed);
            (void)prev; // 避免未使用警告
            stats_.active_allocations.fetch_add(1, std::memory_order_relaxed);
            stats_.total_allocations.fetch_add(1, std::memory_order_relaxed);
        }
        return header ? header->to_user_ptr() : nullptr;
    }

    /// @brief 快速路径：分配新块
    inline void* allocate_new_block(pool_block_header* header) {
        if (header) {
            header->set_in_use(true);
            stats_.cache_misses.fetch_add(1, std::memory_order_relaxed);
            stats_.active_allocations.fetch_add(1, std::memory_order_relaxed);
            stats_.total_allocations.fetch_add(1, std::memory_order_relaxed);
        }
        return header ? header->to_user_ptr() : nullptr;
    }

    /// @brief 快速路径：释放统计更新
    inline void update_dealloc_stats(size_t alloc_size) {
        stats_.active_allocations.fetch_sub(1, std::memory_order_relaxed);
        stats_.freed_bytes.fetch_add(alloc_size, std::memory_order_relaxed);
        stats_.total_deallocations.fetch_add(1, std::memory_order_relaxed);
    }
    /// @brief 为一块 `total_size` 字节的内存申请额度（`max_total_memory`）。
    /// @return 准许返回 true；触顶返回 false。
    /// @note 上限为 0 时**恒真且不碰计数器** —— 「不设上限」的路径只多一次分支。
    ///       用 CAS 而不是「先读后加」：并发分配下后者会**超发**，而这个字段就是上限本身。
    bool try_reserve_bytes(size_t total_size);

    /// @brief 归还额度。上限为 0 时是空操作。
    void release_bytes(size_t total_size);

    pool_block_header* allocate_block(memory_block_type type, size_t size);
    pool_block_header* pop_from_global_pool(memory_block_type type);
    void push_to_global_pool(memory_block_type type, pool_block_header* block);
    bool init_internal(const memory_pool_config& config);

    // ============================================================
    // 成员变量
    // ============================================================

    memory_pool_config config_;

    // 全局分类池（MPSC 队列）- 7种可缓存类型（手动管理）
    mpsc_queue* global_tiny_pool_ = nullptr;            // <= 64B
    mpsc_queue* global_small_pool_ = nullptr;           // 64B - 256B
    mpsc_queue* global_medium_small_pool_ = nullptr;    // 256B - 1024B
    mpsc_queue* global_medium_pool_ = nullptr;          // 1024B - 4096B
    mpsc_queue* global_medium_large_pool_ = nullptr;    // 4096B - 16KB
    mpsc_queue* global_large_pool_ = nullptr;           // 16KB - 64KB
    mpsc_queue* global_extra_large_pool_ = nullptr;     // 64KB - 256KB

    // 全局池计数
    std::atomic<uint64_t> global_tiny_count_{0};
    std::atomic<uint64_t> global_small_count_{0};
    std::atomic<uint64_t> global_medium_small_count_{0};
    std::atomic<uint64_t> global_medium_count_{0};
    std::atomic<uint64_t> global_medium_large_count_{0};
    std::atomic<uint64_t> global_large_count_{0};
    std::atomic<uint64_t> global_extra_large_count_{0};

    // 统计数据
    struct stats_t {
        std::atomic<uint64_t> active_allocations{0};
        std::atomic<uint64_t> total_allocations{0};
        std::atomic<uint64_t> total_deallocations{0};
        std::atomic<uint64_t> failed_allocations{0};
        std::atomic<uint64_t> allocated_bytes{0};
        std::atomic<uint64_t> freed_bytes{0};
        std::atomic<uint64_t> cache_hits{0};
        std::atomic<uint64_t> cache_misses{0};

        double memory_usage_ratio() const {
            uint64_t allocated = allocated_bytes.load(std::memory_order_relaxed);
            uint64_t freed = freed_bytes.load(std::memory_order_relaxed);
            if (allocated == 0) return 0.0;
            return static_cast<double>(allocated - freed) / static_cast<double>(allocated);
        }
    } stats_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutdown_{false};

    // 池**当前持有**的实占字节，`max_total_memory` 就是拿它比的。
    // 只在 `allocate_block()` 增长、只在真正把块还给系统时缩减（见 `release_bytes`）。
    // **上限为 0 时全程不碰它**。
    std::atomic<uint64_t> held_bytes_{0};
};

// ============================================================
// 内联实现（高性能路径）
// ============================================================

inline void* uvcpp_memory_pool::allocate(size_t size) {
    if (size == 0) return nullptr;

    memory_block_type type = config_.get_block_type(size);

    // 快速路径 1：从本地缓存获取
    thread_local_cache& cache = get_thread_cache();
    pool_block_header* header = cache.pop(type);
    if (header) {
        return allocate_from_cache(header);
    }

    // 快速路径 2：从全局池获取
    header = pop_from_global_pool(type);
    if (header) {
        return allocate_from_cache(header);
    }

    // 慢速路径：分配新块
    header = allocate_block(type, size);
    return allocate_new_block(header);
}

inline void* uvcpp_memory_pool::allocate_aligned(size_t size, size_t align) {
    if (size == 0) return nullptr;
    size_t aligned_size = (size + align - 1) & ~(align - 1);
    return allocate(aligned_size);
}

inline void uvcpp_memory_pool::deallocate(void* ptr) {
    if (!ptr) return;

    pool_block_header* header = pool_block_header::from_user_ptr(ptr);
    memory_block_type type = header->get_type();

    // 获取实际分配的大小（包括header）
    size_t alloc_size = header->size;

    // 快速路径：尝试放入本地缓存
    thread_local_cache& cache = get_thread_cache();
    if (!cache.push(header)) {
        // 本地缓存满了，放到全局池
        push_to_global_pool(type, header);
    }

    update_dealloc_stats(alloc_size);
}

inline uint64_t uvcpp_memory_pool::cached_blocks() const {
    const thread_local_cache& cache = const_cast<uvcpp_memory_pool*>(this)->get_thread_cache();
    return cache.total();
}

// ============================================================
// 智能指针支持
// ============================================================

template<typename T, typename... Args>
T* make_from_pool(uvcpp_memory_pool& pool, Args&&... args) {
    void* ptr = pool.allocate(sizeof(T));
    if (!ptr) throw std::bad_alloc();
    return new (ptr) T(std::forward<Args>(args)...);
}

template<typename T>
class pool_deleter {
public:
    explicit pool_deleter(uvcpp_memory_pool& p)
        : pool_(&p) {}

    void operator()(T* ptr) const {
        if (ptr) {
            ptr->~T();
            pool_->deallocate(ptr);
        }
    }

private:
    uvcpp_memory_pool* pool_;
};

template<typename T, typename... Args>
std::unique_ptr<T, pool_deleter<T>> make_unique_from_pool(uvcpp_memory_pool& pool, Args&&... args) {
    T* ptr = make_from_pool<T>(pool, std::forward<Args>(args)...);
    return std::unique_ptr<T, pool_deleter<T>>(ptr, pool_deleter<T>(pool));
}

template<typename T, typename... Args>
std::shared_ptr<T> make_shared_from_pool(uvcpp_memory_pool& pool, Args&&... args) {
    T* ptr = make_from_pool<T>(pool, std::forward<Args>(args)...);
    // 删除器复用 pool_deleter<T>：它的 operator() 与这里原来那个 lambda 逐句相同
    // （`ptr->~T()` 后 `deallocate(ptr)`），顺带多一个空指针检查。原来写的是
    // C++14 的 init-capture（`[pool = &pool]`），而构建声明的是 C++11 —— 消费者
    // 用 `-std=c++11 -pedantic-errors` 编到这里就失败。
    return std::shared_ptr<T>(ptr, pool_deleter<T>(pool));
}

} // namespace uvcpp

#endif // SRC_EXPAND_UVCPP_MEMORY_POOL_H
