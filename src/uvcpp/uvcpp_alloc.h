/**
 * @file src/uvcpp/uvcpp_alloc.h
 * @brief Allocation helpers used across the project.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_ALLOC_H
#define SRC_UVCPP_UVCPP_ALLOC_H

#include <uvcpp/uvcpp_config.h>

#include <cstdlib>
#include <cstring>
#include <new>

// 内存池宏：启用时使用企业级内存分配器
#if UVCPP_ENABLE_MEMORY_POOL
#include <expand/uvcpp_page_heap.h>
#endif

namespace uvcpp {

// **本命名空间里所有 `uvcpp_alloc_*` 都是 calloc 语义**（分配后整块 memset 0）。
// 唯一不清零的那一个（读缓冲要用的）**故意**留在 `uvcpp_buf.cpp` 的匿名命名空间里，
// 没有放进这个头 —— 同前缀、同形状、唯独不保证清零的兄弟放在一起是个陷阱。

#if UVCPP_ENABLE_MEMORY_POOL

// 使用企业级内存池的分配/释放

template<typename T>
inline T* uvcpp_alloc() {
    T* p = static_cast<T*>(uvcpp_enterprise_alloc(sizeof(T)));
    if (p == nullptr) throw std::bad_alloc();
    std::memset(p, 0, sizeof(T));
    return p;
}

template<typename T>
inline T* uvcpp_alloc_arry(size_t len) {
    T* p = static_cast<T*>(uvcpp_enterprise_alloc(sizeof(T) * len));
    if (p == nullptr) throw std::bad_alloc();
    std::memset(p, 0, sizeof(T) * len);
    return p;
}

inline void* uvcpp_alloc_bytes(size_t sz) {
    void* p = uvcpp_enterprise_alloc(sz);
    if (p == nullptr) throw std::bad_alloc();
    std::memset(p, 0, sz);
    return p;
}

/**
 * @brief 重新分配并搬迁已有内容。
 *
 * @param p       旧指针（可为 nullptr，等价于新分配）。
 * @param old_sz  旧块的**有效字节数**。内存池路径下旧块的真实大小无法由指针
 *                反查（只有调用方知道它要过多少字节），所以必须由调用方传入。
 *                传大了会读越界 —— 这正是 128KB 以上的 HTTP 响应会崩溃的原因
 *                （旧实现直接 memcpy(np, p, 新大小)，从一个小块后面读出几十 KB）。
 * @param sz      新的字节数。
 * @return 新块指针；失败抛 std::bad_alloc。
 */
inline void* uvcpp_realloc_bytes(void* p, size_t old_sz, size_t sz) {
    // realloc 需要特殊处理：先分配新的，再复制数据，然后释放旧的
    if (p == nullptr) {
        return uvcpp_enterprise_alloc(sz);
    }
    void* np = uvcpp_enterprise_alloc(sz);
    if (np == nullptr) throw std::bad_alloc();
    // 只复制两边都存在的部分
    size_t copy_sz = (old_sz < sz) ? old_sz : sz;
    if (copy_sz > 0) {
        std::memcpy(np, p, copy_sz);
    }
    uvcpp_enterprise_free(p);
    return np;
}

inline void uvcpp_free_bytes(void* p) {
    if (p == nullptr) return;
    uvcpp_enterprise_free(p);
}

template<typename T>
inline void uvcpp_free(T* p) {
    uvcpp_free_bytes(reinterpret_cast<void*>(p));
}

#else // !UVCPP_ENABLE_MEMORY_POOL

// 使用标准库的 malloc/free

template<typename T>
inline T* uvcpp_alloc() {
    T* p = static_cast<T*>(std::malloc(sizeof(T)));
    if (p == nullptr) throw std::bad_alloc();
    std::memset(p, 0, sizeof(T));
    return p;
}

template<typename T>
inline T* uvcpp_alloc_arry(size_t len) {
    T* p = static_cast<T*>(std::malloc(sizeof(T) * len));
    if (p == nullptr) throw std::bad_alloc();
    std::memset(p, 0, sizeof(T) * len);
    return p;
}

inline void* uvcpp_alloc_bytes(size_t sz) {
    void* p = std::malloc(sz);
    if (p == nullptr) throw std::bad_alloc();
    std::memset(p, 0, sz);
    return p;
}

// old_sz 只有内存池路径需要（它自己搬数据）；std::realloc 自己知道旧大小，
// 这里保留同名同签名的重载，让上层代码两种配置下写法一致。
inline void* uvcpp_realloc_bytes(void* p, size_t /*old_sz*/, size_t sz) {
    void* np = std::realloc(p, sz);
    if (np == nullptr)
        throw std::bad_alloc();
    return np;
}

inline void uvcpp_free_bytes(void* p) {
    if (p == nullptr) return;
    std::free(p);
}

template<typename T>
inline void uvcpp_free(T* p) {
    uvcpp_free_bytes(reinterpret_cast<void*>(p));
}

#endif // UVCPP_ENABLE_MEMORY_POOL


// ---------------------------------------------------------------------------
// 定长块的**线程局部**自由表（`M6` 用；与上面那族 `uvcpp_alloc_*` 不同：
// 这里**不清零**，因为它服务的不是"要一块干净内存"的场景，而是
// "这块内存马上要被构造函数整块覆盖"的 `std::allocate_shared`）
// ---------------------------------------------------------------------------

/**
 * @brief 按**精确尺寸**回收定长块的自由表。
 *
 * 尺寸类按精确尺寸匹配（同一个类型的 `allocate_shared` 块尺寸是常数），不做
 * 向上取整 —— 那会让「取一块存货来复用」变成「取一块尺寸不对的」。
 */
class uvcpp_block_cache {
 public:
  enum { kSlotCount = 4, kSlotMax = 256 };

  /** @brief 取一块装得下 `size` 字节的存货；没有就返回 nullptr（调用方退回 `operator new`）。 */
  void* try_take(std::size_t size) noexcept {
    if (size == 0) return nullptr;
    for (unsigned i = 0; i < kSlotCount; ++i) {
      slot& s = slots_[i];
      if (s.size != size || s.head == nullptr) continue;
      void* p = s.head;
      s.head = *static_cast<void**>(p);  // 自由块的头 8 字节存下一块的地址
      --s.count;
      return p;
    }
    return nullptr;
  }

  /** @brief 还一块回来。表满、或没有空闲的尺寸槽，就直接 `operator delete`。 */
  void put(void* p, std::size_t size) noexcept {
    if (p == nullptr) return;
    if (size == 0) {
      ::operator delete(p);
      return;
    }
    slot* spare = nullptr;
    for (unsigned i = 0; i < kSlotCount; ++i) {
      slot& s = slots_[i];
      if (s.size == size) {
        if (s.count >= kSlotMax) {
          ::operator delete(p);
          return;
        }
        *static_cast<void**>(p) = s.head;
        s.head = p;
        ++s.count;
        return;
      }
      if (spare == nullptr && s.size == 0) spare = &s;
    }
    if (spare == nullptr) {
      ::operator delete(p);
      return;
    }
    spare->size = size;
    *static_cast<void**>(p) = nullptr;
    spare->head = p;
    spare->count = 1;
  }

 private:
  struct slot {
    std::size_t size;  ///< 这个槽服务的块尺寸；0 = 槽还空着
    void* head;        ///< 自由块单链表
    unsigned count;    ///< 链上有几块（上限 kSlotMax，防长跑把内存留住）
  };
  slot slots_[kSlotCount];
};

/**
 * @brief 本线程的自由表。
 *
 * `= {}` 让它是**常量初始化**的（没有 `__cxa_guard_*`），结构体平凡析构 ⇒
 * **不登记** `__cxa_thread_atexit`。跨线程释放只是把块留在释放那个线程的表里，
 * 正确性不受影响（块是 `::operator new` 来的，谁 `delete` 都行）。
 */
inline uvcpp_block_cache& uvcpp_block_cache_here() noexcept {
  static thread_local uvcpp_block_cache cache = {};
  return cache;
}

/**
 * @brief 给 `std::allocate_shared` 用的分配器：只加一层块回收，别的都不管。
 *
 * 它**必须**是无状态的（`operator==` 恒真）：`allocate_shared` 会把分配器存进
 * 控制块，有状态的话那份拷贝与自由表就不再是一回事了。
 */
template <typename T>
class uvcpp_block_allocator {
 public:
  typedef T value_type;

  uvcpp_block_allocator() noexcept {}
  template <typename U>
  uvcpp_block_allocator(const uvcpp_block_allocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
    const std::size_t bytes = n * sizeof(T);
    if (n == 1) {
      void* p = uvcpp_block_cache_here().try_take(bytes);
      if (p != nullptr) return static_cast<T*>(p);
    }
    return static_cast<T*>(::operator new(bytes));
  }

  void deallocate(T* p, std::size_t n) noexcept {
    if (n == 1) {
      uvcpp_block_cache_here().put(p, sizeof(T));
      return;
    }
    ::operator delete(p);
  }

  template <typename U>
  struct rebind {
    typedef uvcpp_block_allocator<U> other;
  };
};

template <typename T, typename U>
inline bool operator==(const uvcpp_block_allocator<T>&,
                       const uvcpp_block_allocator<U>&) noexcept {
  return true;
}

template <typename T, typename U>
inline bool operator!=(const uvcpp_block_allocator<T>&,
                       const uvcpp_block_allocator<U>&) noexcept {
  return false;
}

} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_ALLOC_H
