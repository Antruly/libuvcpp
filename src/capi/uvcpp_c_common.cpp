/**
 * @file src/capi/uvcpp_c_common.cpp
 * @brief C ABI 的公共地基：ABI 版本、版本串、错误码文案、线程局部的错误记录。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这个文件里的四个函数**不需要** `UVCPP_C_TRY` 包起来，理由不是"它们简单"，
 * 而是它们**一个会抛的调用都没有**：返回值是编译期的宏，或者一个常量串，
 * 或者一次 `strncpy`。给它们套上收口宏只会让人以为"这里的 try 有内容"。
 * 除此之外，本目录里**每一处** `extern "C"` 都在 `uvcpp_c_internal.h` 的收口
 * 宏里 —— 谁新加一个函数忘了包，就是让 C++ 异常穿过 C 边界。
 */

#include "capi/uvcpp_c_common.h"

#include <cstring>
#include <mutex>
#include <unordered_set>

#include <uv.h>
#include <uvcpp/uvcpp_version.h>

#include "capi/uvcpp_c_internal.h"

namespace uvcpp_c_detail {

namespace {

/// 最近一次异常的话。**线程局部**：同一个进程里两条循环线程各记各的，互不覆盖。
thread_local char g_last_error[512] = {0};

/// `uvcpp_c_strerror()` 转述 libuv 错误码时的缓冲。同上，线程局部。
thread_local char g_strerror[512] = {0};

/// 活句柄登记表。指针的值就是键 —— 不用 `unordered_set<const void*>` 的默认
/// 哈希而是拿地址本身当哈希，是因为这些键本来就是地址（默认哈希在 libstdc++
/// 上**就是**恒等，但那是实现细节，写死了将来换实现也不会变贵）。
std::mutex& registry_mutex() {
  static std::mutex m;
  return m;
}

std::unordered_set<const void*>& registry() {
  static std::unordered_set<const void*> s;
  return s;
}

}  // namespace

void registry_add(const void* handle) {
  if (handle == nullptr) return;
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry().insert(handle);
}

void registry_remove(const void* handle) {
  if (handle == nullptr) return;
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry().erase(handle);
}

bool registry_has(const void* handle) {
  if (handle == nullptr) return false;
  std::lock_guard<std::mutex> lock(registry_mutex());
  return registry().find(handle) != registry().end();
}

size_t registry_size() {
  std::lock_guard<std::mutex> lock(registry_mutex());
  return registry().size();
}

int copy_out(const std::string& s, char* buf, size_t cap) {
  const size_t n = s.size();
  // `int` 装不下就报"缓冲区太小"而不是截断：静默截断会让调用方拿到一个
  // 长度对不上的字符串，而它没有任何办法发现。
  if (n > 0x7fffffffu) return UVCPP_C_E_BUFFER_TOO_SMALL;
  if (buf != nullptr && cap >= n + 1) {
    if (n > 0) std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
  }
  return static_cast<int>(n);
}

void set_last_error(const char* what) {
  if (what == nullptr) {
    g_last_error[0] = '\0';
    return;
  }
  std::strncpy(g_last_error, what, sizeof(g_last_error) - 1);
  g_last_error[sizeof(g_last_error) - 1] = '\0';
}

const char* get_last_error() {
  return g_last_error[0] != '\0' ? g_last_error : nullptr;
}

const char* uv_error_text(int err) {
  // libuv 那一段：`-errno` 加上它自定义的那几个，最负的一个是 `UV_UNKNOWN`
  // （-4096）。**用 `UV_UNKNOWN` 当边界而不是写死一个数** —— 这个区间的两端
  // 是 libuv 自己的事，写死了将来它变（或者我们的码挪位置）就是一条静默错译。
  //
  // 区间之外一律不认：认错了就是把本层自己的码（-20001 起）翻译成一句毫无关系
  // 的 libuv 文案 —— 那种"错误信息与病因无关"正是最难查的一类。
  //
  // 注意 `uv_strerror_r()` 的返回值是**它写进去的那个 buf**，不是状态码（签名
  // 是 `char* uv_strerror_r(int, char*, size_t)`），而且它对认不出的码也会写
  // 一句 "Unknown system error %d"。所以这里不判返回值，只判"这个码归不归它管"。
  if (err < 0 && err >= static_cast<int>(UV_UNKNOWN)) {
    uv_strerror_r(err, g_strerror, sizeof(g_strerror));
    g_strerror[sizeof(g_strerror) - 1] = '\0';  // 防御：库若只写 buflen-1 个
    return g_strerror;
  }
  return nullptr;
}

}  // namespace uvcpp_c_detail

extern "C" UVCPP_C_API unsigned int uvcpp_c_abi_version(void) {
  return static_cast<unsigned int>(UVCPP_C_ABI_VERSION);
}

extern "C" UVCPP_C_API const char* uvcpp_c_version_string(void) {
  // 唯一来源是 `uvcpp_version.h`（`CMakeLists.txt` 在 configure 期读的也是它）。
  // 这里**不另抄一份字符串**：抄一份就等于多一处会漂的真相。
  return uvcpp::version_string();
}

extern "C" UVCPP_C_API size_t uvcpp_c_live_handle_count(void) {
  // 加锁取一张快照的大小。理由与"为什么这个口子存在"写在头的注释里：它是
  // **登记表收支平衡**唯一能被外部量到的形式，而那条平衡是 `alive()` 前半段
  // 的前提（表只增不减 = 一次无上限泄漏，但"释放后再用给 E_STALE"量不出来，
  // 因为 glibc 的 tcache 恰好会把空闲块的第 0 字节改写成非魔数）。
  return uvcpp_c_detail::registry_size();
}

extern "C" UVCPP_C_API const char* uvcpp_c_strerror(int err) {
  switch (err) {
    case UVCPP_C_OK:                    return "ok";
    case UVCPP_C_E_INVALID_ARG:         return "invalid argument";
    case UVCPP_C_E_STALE:               return "handle is stale (freed or foreign)";
    case UVCPP_C_E_EXCEPTION:           return "C++ exception caught at the C ABI boundary";
    case UVCPP_C_E_STATE:               return "wrong object state for this call";
    case UVCPP_C_E_NO_MEMORY:           return "out of memory";
    case UVCPP_C_E_UNSUPPORTED:         return "not provided by the C API (by design)";
    case UVCPP_C_E_NOT_BUILT:           return "module not built into this library";
    case UVCPP_C_E_WRONG_THREAD:        return "called from a thread other than the event loop's";
    case UVCPP_C_E_BUFFER_TOO_SMALL:    return "caller-provided buffer is too small";
    case UVCPP_C_E_NOT_FOUND:           return "the thing asked for is not present";
    default: break;
  }

  // 不是本层那一段就交给 libuv 那一段。缓冲与"哪一段算 libuv"的判断都在
  // `uv_error_text()` 里 —— 放在同一个文件、同一个线程局部缓冲上，好过在这里
  // 隔着命名空间去够它。
  {
    const char* text = uvcpp_c_detail::uv_error_text(err);
    if (text != nullptr) return text;
  }
  return "unknown error code";
}

extern "C" UVCPP_C_API const char* uvcpp_c_last_error_string(void) {
  return uvcpp_c_detail::get_last_error();
}
