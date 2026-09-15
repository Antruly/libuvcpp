#include "uvcpp_handle.h"
namespace uvcpp {

namespace {

// ---------------------------------------------------------------------------
// 「宿主已经析构」哨兵
// ---------------------------------------------------------------------------
// `uv_handle_t::data` 平时指向包装它的 uvcpp_handle。但在析构路径上，
// wrapper 会比 libuv 的关闭回调先消失 —— 如果这时还让 data 指着它，
// callback_close 就会读到已释放的内存。
//
// 于是析构时把 data 换成下面这两个哨兵之一：它们保证不等同于任何真实
// wrapper 指针（取的是静态对象的地址），同时把「底层句柄内存要不要还」
// 这件 callback_close 本来要从 wrapper 上问的事情编码进去。
char g_detached_handle_owned_marker = 0;   ///< 宿主已走，但底层内存仍需释放
char g_detached_handle_borrowed_marker = 0;///< 宿主已走，底层内存不归我们

inline void *detached_owned_marker() { return &g_detached_handle_owned_marker; }
inline void *detached_borrowed_marker() {
  return &g_detached_handle_borrowed_marker;
}

}  // namespace

uvcpp_handle::uvcpp_handle()
    : handle_close_cb(), handle_alloc_cb(), _handle(nullptr), _handle_union() {
}

uvcpp_handle::~uvcpp_handle() { this->free_handle(); }

int uvcpp_handle::set_data(void *pdata) {
  _vdata = pdata;
  return 0;
}

void *uvcpp_handle::get_data() { return _vdata; }

// 实例方法与静态重载同一套判空规则，理由见 is_active() 处那段说明。
void uvcpp_handle::ref() {
  if (_handle == nullptr) return;
  uv_ref(_handle);
}

void uvcpp_handle::unref() {
  if (_handle == nullptr) return;
  uv_unref(_handle);
}

int uvcpp_handle::has_ref() {
  if (_handle == nullptr) return 0;
  return uv_has_ref(_handle);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 18
uv_handle_type uvcpp_handle::handle_get_type() {
  if (_handle == nullptr) return UV_UNKNOWN_HANDLE;
  return uv_handle_get_type(_handle);
}
const char *uvcpp_handle::handle_type_name() {
  if (_handle == nullptr) return nullptr;
  return uv_handle_type_name(_handle->type);
}
void *uvcpp_handle::handle_get_data() {
  if (_handle == nullptr) return nullptr;
  return uv_handle_get_data(_handle);
}

void *uvcpp_handle::handle_get_loop() {
  if (_handle == nullptr) return nullptr;
  return uv_handle_get_loop(_handle);
}

void uvcpp_handle::handle_set_data(void *data) {
  if (_handle == nullptr) return;
  uv_handle_set_data(_handle, data);
}
#endif
#endif

size_t uvcpp_handle::handle_size() {
  if (_handle == nullptr) return 0;
  return uv_handle_size(_handle->type);
}

// **句柄已经释放完了就没有"活跃"可言。** `_handle` 为空的含义是"底层
// `uv_handle_t` 已被 `callback_close` 释放"，此时把它交给 libuv 是空指针解引用
// （`uv_is_active` 内部先取 `handle->flags`，实测崩在 `[0x58]`）。`close()`
// 一直是这么判的（见下），这两个查询漏了，于是"在关完的连接上问一句状态"
// 变成了崩溃 —— `~uvcpp_ws_client` 正是这么用的。
int uvcpp_handle::is_active() {
  if (_handle == nullptr) return 0;
  return uv_is_active(_handle);
}

void uvcpp_handle::set_handle_data() { _handle->data = this; }

void uvcpp_handle::callback_alloc(uv_handle_t *handle, size_t suggested_size,
                             uv_buf_t *buf) {
  uvcpp_handle *wrapper = reinterpret_cast<uvcpp_handle *>(handle->data);
  if (!wrapper || !wrapper->handle_alloc_cb)
    return;
  // Call user alloc callback with a uvcpp_buf view directly (no copy).
  wrapper->handle_alloc_cb(wrapper, suggested_size, buf);
}

void uvcpp_handle::callback_close(uv_handle_t *handle) {
  if (handle == nullptr) return;

  void *data = handle->data;

  // --- 情况一：宿主已经析构，只差把底层句柄内存还回去 -------------------
  if (data == detached_owned_marker() || data == detached_borrowed_marker()) {
    handle->data = nullptr;
    if (data == detached_owned_marker()) {
      UVCPP_VFREE(handle)
    }
    return;
  }

  uvcpp_handle *wrapper = reinterpret_cast<uvcpp_handle *>(data);

  // --- 情况二：宿主体还在 -------------------------------------------------
  // 顺序非常关键：凡是需要碰 wrapper 的事情，都必须在调用使用者的关闭回调
  // **之前**做完。使用者的关闭回调里 delete 掉 wrapper（或 wrapper 的宿主
  // 对象）是合法用法 —— 仓库里 poll_func / tcp_func / pipe_func /
  // shutdown_func / tcp_client_func 都是这么写的。回调返回之后这里既不能
  // 再碰 wrapper，也不能再碰 handle（handle 的内存可能已经被还回去了）。
  ::std::function<void(uvcpp_handle *)> close_cb;
  bool owns_handle = false;
  if (wrapper != nullptr) {
    close_cb = wrapper->handle_close_cb;
    // 用掉就清掉。否则之后任何一次无参 close()（或析构里的 free_handle）
    // 都会把这个陈旧回调再跑一遍 —— 那时它捕获的东西可能早就没了。
    wrapper->handle_close_cb = ::std::function<void(uvcpp_handle *)>();
    owns_handle = (wrapper->_owns_handle && wrapper->_handle == handle);
    wrapper->_handle = nullptr;
  }

  // 在回调之前释放底层句柄内存：这样即使回调把 wrapper 一起 delete 了，
  // 也不会留下一块没人负责的 uv_handle_t。
  if (owns_handle) {
    handle->data = nullptr;
    UVCPP_VFREE(handle)
  } else {
    handle->data = nullptr;
  }

  if (close_cb) {
    close_cb(wrapper);
  }
}

void uvcpp_handle::close() {
  if (_handle == nullptr)
    return;
  uv_close(_handle, callback_close);
}

// 同上：句柄已释放时"正在关闭"为假 —— 它早就关完了。**答"假"而不是答"崩溃"**
// 是这个查询唯一有意义的语义，`close()` 的守卫也是这么写的。
int uvcpp_handle::is_closing() {
  if (_handle == nullptr) return 0;
  return uv_is_closing(_handle);
}

uvcpp_handle::uvcpp_handle(const uvcpp_handle &obj)
    : handle_close_cb(), handle_alloc_cb(), _handle(nullptr), _handle_union() {
  if (obj._handle != nullptr) {
    size_t sz = uv_handle_size(obj._handle->type);
    _handle = (uv_handle_t *)uvcpp::uvcpp_alloc_bytes(sz);
    if (_handle == nullptr)
      throw std::bad_alloc();
    memcpy(this->_handle, obj._handle, sz);
    this->set_handle_data();
    _vdata = obj._vdata;
    _handle_union.uvcpp_handle = _handle;
  } else {
    _handle = nullptr;
  }
}

uvcpp_handle &uvcpp_handle::operator=(const uvcpp_handle &obj) {
  this->free_handle();

  if (obj._handle != nullptr) {
    size_t sz = uv_handle_size(obj._handle->type);
    _handle = (uv_handle_t *)uvcpp::uvcpp_alloc_bytes(sz);
    if (_handle == nullptr)
      throw std::bad_alloc();
    memcpy(this->_handle, obj._handle, sz);
    this->set_handle_data();
    _vdata = obj._vdata;
  } else {
    _handle = nullptr;
  }

  return *this;
}

uvcpp_handle *uvcpp_handle::clone(uvcpp_handle *obj, int memSize) {
  uvcpp_handle *newObj = (uvcpp_handle *)new char[memSize];
  newObj->set_handle_data();
  memcpy(newObj, obj, memSize);
  return newObj;
}

// static overloads removed during rename
//
// 判空规则与实例方法一致：`_handle == nullptr` 表示底层 uv_handle_t 已被
// callback_close 释放（见 is_active 处的说明）。把空指针交给 libuv 会读
// `handle->flags`（偏移 0x58）而崩溃，所以每个直接接触 _handle 的入口都要
// 在**第一句**挡住它。
void uvcpp_handle::ref(uvcpp_handle *vhd) {
  if (vhd == nullptr || vhd->_handle == nullptr) return;
  uv_ref(vhd->_handle);
}
void uvcpp_handle::unref(uvcpp_handle *vhd) {
  if (vhd == nullptr || vhd->_handle == nullptr) return;
  uv_unref(vhd->_handle);
}
int uvcpp_handle::has_ref(const uvcpp_handle *vhd) {
  if (vhd == nullptr || vhd->_handle == nullptr) return 0;
  return uv_has_ref(vhd->_handle);
}
int uvcpp_handle::is_active(const uvcpp_handle *vhd) {
  if (vhd == nullptr || vhd->_handle == nullptr) return 0;
  return uv_is_active(vhd->_handle);
}
void uvcpp_handle::close(uvcpp_handle *vhd,
                         ::std::function<void(uvcpp_handle *)> closeCallback) {
  if (vhd == nullptr || vhd->_handle == nullptr) return;
  vhd->handle_close_cb = closeCallback;
  uv_close(vhd->_handle, callback_close);
}
int uvcpp_handle::is_closing(const uvcpp_handle *vhd) {
  if (vhd == nullptr || vhd->_handle == nullptr) return 0;
  return uv_is_closing(vhd->_handle);
}
int uvcpp_handle::fileno(const uvcpp_handle *vhd, uv_os_sock_t &sock) {
  if (vhd == nullptr || vhd->_handle == nullptr) return UV_EBADF;
  return uv_fileno(vhd->_handle, (uv_os_fd_t *)&sock);
}
size_t uvcpp_handle::handle_size(const uvcpp_handle *vhd) {
  if (vhd == nullptr || vhd->_handle == nullptr) return 0;
  return uv_handle_size(vhd->_handle->type);
}


#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 18
uv_handle_type uvcpp_handle::handle_get_type(const uvcpp_handle *vhd) {
  if (vhd == nullptr || vhd->_handle == nullptr) return UV_UNKNOWN_HANDLE;
  return uv_handle_get_type(vhd->_handle);
}
const char *uvcpp_handle::handle_type_name(uvcpp_handle *vhd) {
  if (vhd == nullptr || vhd->_handle == nullptr) return nullptr;
  return uv_handle_type_name(vhd->_handle->type);
}

void *uvcpp_handle::handle_get_data(const uvcpp_handle *vhd) {
  if (vhd == nullptr || vhd->_handle == nullptr) return nullptr;
  return uv_handle_get_data(vhd->_handle);
}

void *uvcpp_handle::handle_get_loop(const uvcpp_handle *vhd) {
  if (vhd == nullptr || vhd->_handle == nullptr) return nullptr;
  return uv_handle_get_loop(vhd->_handle);
}

void uvcpp_handle::handle_set_data(uvcpp_handle *vhd, void *data) {
  if (vhd == nullptr || vhd->_handle == nullptr) return;
  uv_handle_set_data(vhd->_handle, data);
}
#endif
#endif


// 与无参 close() 同一判据：句柄已释放就什么都不做（也无从"关闭"）。
// 原先这两处不一致 —— 无参版判空、带回调版不判 —— 于是同一个类里
// `close()` 安全而 `close(cb)` 崩溃。
void uvcpp_handle::close(::std::function<void(uvcpp_handle *)> closeCallback) {
  if (_handle == nullptr) return;
  handle_close_cb = closeCallback;
  uv_close(_handle, callback_close);
  return;
}

int uvcpp_handle::fileno(uv_os_sock_t& sock) {
  if (_handle == nullptr) return UV_EBADF;
  return uv_fileno(_handle, (uv_os_fd_t *)&sock);
}

uv_handle_t *uvcpp_handle::get_handle() const { return _handle; }

void uvcpp_handle::set_handle(void *hd) {
  // Default: assume ownership when using single-arg set_handle.
  this->set_handle(hd, true);
  return;
}

void uvcpp_handle::set_handle(void *hd, bool owns) {
  this->free_handle();
  _handle = (uv_handle_t *)hd;
  _handle_union = *(uvcpp_handle_union *)&_handle;
  _owns_handle = owns;
  if (_handle)
    this->set_handle_data();
  return;
}

void uvcpp_handle::free_handle() {
  if (_handle == nullptr)
    return;

  /* If the handle is already closing, do not free it here; wait for the
     close callback to run. If it's active, request close and let the
     close callback handle final cleanup. If it's inactive and not closing,
     it's safe to free the underlying memory. */
  if (uv_is_closing(_handle)) {
    // 关闭回调迟早会跑，但跑到的时候本对象（wrapper）已经析构了。把 data
    // 换成哨兵，回调就不必去找一个不存在的 wrapper，也能正确决定要不要
    // 把底层句柄内存还回去。
    _handle->data = _owns_handle ? detached_owned_marker()
                                 : detached_borrowed_marker();
    _handle = nullptr;
    return;
  }

  if (uv_is_active(_handle)) {
    /* If we own the handle, request close and let callback free memory.
       If we do not own it, do not issue close (owner is responsible). */
    if (_owns_handle) {
      // 同样先断开指向本对象的引用：uv_close 是异步的，回调回来时这里
      // 已经析构完了。留着 data 指向自己就是一枚悬垂指针。
      _handle->data = detached_owned_marker();
      uv_close(_handle, callback_close);
    }
    _handle = nullptr;
    return;
  }

  /* Not active and not closing: free only if we own the memory */
  if (_owns_handle) {
    UVCPP_VFREE(_handle)
  }
  _handle = nullptr;
}
} // namespace uvcpp