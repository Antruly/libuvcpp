/**
 * @file src/uvcpp/uvcpp_alloc.h
 * @brief Allocation helpers used across the project.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_ALLOC_H
#define SRC_UVCPP_UVCPP_ALLOC_H

#include <cstdlib>
#include <cstring>
#include <new>

// 内存池宏：启用时使用企业级内存分配器
#if UVCPP_ENABLE_MEMORY_POOL
#include <expand/uvcpp_page_heap.h>
#endif

namespace uvcpp {

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

} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_ALLOC_H
