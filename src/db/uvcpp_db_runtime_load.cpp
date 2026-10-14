/**
 * @file src/db/uvcpp_db_runtime_load.cpp
 * @brief 运行时加载原语的两条平台实现。理由与契约见同名头。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "db/uvcpp_db_runtime_load.h"

#if UVCPP_DB_ENABLE && UVCPP_DB_RUNTIME_LOAD

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace uvcpp {
namespace db_detail {

namespace {

#if defined(_WIN32)

void* open_one(const char* name) {
  // `LoadLibraryA` 与 `dlopen` 的语义差别只有一处值得记：**同名 DLL 已经被
  // 加载过时它返回同一个模块**（引用计数 +1），不会加载第二份。这正是我们要的 ——
  // 调用方自己先 LoadLibrary 过一份的话，我们拿到的是那一份。
  return reinterpret_cast<void*>(::LoadLibraryA(name));
}

/// 失败原因。`FormatMessage` 能把那个数字翻成人话，但它要分配缓冲区、还要选
/// 语言，收益不值这份代码；错误号本身就足够定位（`126` = 找不到模块是最常见的
/// 那一个，也正是"没装客户端库"）。
std::string last_open_error() {
  return "GetLastError=" +
         std::to_string(static_cast<unsigned long>(::GetLastError()));
}

void* lookup(void* handle, const char* symbol) {
  return reinterpret_cast<void*>(
      ::GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol));
}

#else  // POSIX：dlfcn

void* open_one(const char* name) {
  // `RTLD_NOW` 而不是 `RTLD_LAZY`：**加载期就把 undefined 解析完**。懒绑定的
  // 话"这份库缺一个符号"要等到第一次调用才炸，而那可能是用户把服务跑起来
  // 三小时之后的第一条 SQL。现在它变成一次加载失败，与"没装库"同一个形状、
  // 同一句可读的错误。
  //
  // `RTLD_LOCAL` 而不是 `RTLD_GLOBAL`：**它导出的符号不进全局作用域**。
  // libpq 会拉进它自己的 `libcrypto.so.3`，而进程里可能已经有另一份 OpenSSL
  // （宿主程序自己链的，或者我们这份库静态链进来的 —— 后者已由
  // `-Wl,--exclude-libs,ALL` 藏掉）。用 `RTLD_GLOBAL` 等于让 libpq 的
  // `SSL_*` 有机会顶替全局那份，两边各建各的 `X509`。不导出就没有这个问题。
  return ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
}

std::string last_open_error() {
  // `dlerror()` 是**一次性的**：读一次就清空。所以调用方必须先清、再开、再读。
  const char* e = ::dlerror();
  return e ? std::string(e) : std::string("未知原因");
}

void* lookup(void* handle, const char* symbol) {
  // `dlsym` 在找不到时**返回 nullptr**，而 nullptr 也可能是某个符号的合法值
  // （弱符号），所以标准要求"再读一次 dlerror()"来区分。这里用不上这个区分
  // （我们要的都是函数，函数地址不可能是 0），但清一下免得污染下一个调用者
  // 的错误信息。
  ::dlerror();
  return ::dlsym(handle, symbol);
}

#endif

}  // namespace

void* uvcpp_db_open_client_library(const char* const* names,
                                   const char* env_var, std::string* tried,
                                   std::string* loaded) {
  if (tried) tried->clear();
  if (loaded) loaded->clear();

  const char* override_name = nullptr;
  if (env_var != nullptr) {
    const char* v = std::getenv(env_var);
    // 空串当"没设"：`UVCPP_DB_PGSQL_LIBRARY=` 这种写法（比如在 shell 里
    // `unset` 之后又 export 了一次）本意几乎不可能是"加载一个叫空串的库"。
    if (v != nullptr && v[0] != '\0') override_name = v;
  }

  if (override_name != nullptr) {
    void* h = open_one(override_name);
    if (h != nullptr) {
      if (loaded) *loaded = override_name;
      return h;
    }
    if (tried) {
      *tried = std::string(env_var) + "=" + override_name + "（" +
               last_open_error() + "）";
    }
    return nullptr;
  }

  for (const char* const* p = names; p != nullptr && *p != nullptr; ++p) {
    void* h = open_one(*p);
    if (h != nullptr) {
      if (loaded) *loaded = *p;
      return h;
    }
    if (tried) {
      if (!tried->empty()) tried->append("、");
      tried->append(*p);
      tried->append("（");
      tried->append(last_open_error());
      tried->append("）");
    }
  }
  return nullptr;
}

void* uvcpp_db_lookup_symbol(void* handle, const char* symbol) {
  if (handle == nullptr || symbol == nullptr) return nullptr;
  return lookup(handle, symbol);
}

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE && UVCPP_DB_RUNTIME_LOAD
