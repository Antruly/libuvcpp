#include "uvcpp_handle.h"
namespace uvcpp {

uvcpp_handle::uvcpp_handle()
    : handle_close_cb(), handle_alloc_cb(), _handle(nullptr), _handle_union() {
}

uvcpp_handle::~uvcpp_handle() { this->free_handle(); }

int uvcpp_handle::set_data(void *pdata) {
  _vdata = pdata;
  return 0;
}

void *uvcpp_handle::get_data() { return _vdata; }

void uvcpp_handle::ref() { uv_ref(_handle); }

void uvcpp_handle::unref() { uv_unref(_handle); }

int uvcpp_handle::has_ref() { return uv_has_ref(_handle); }
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 18
uv_handle_type uvcpp_handle::handle_get_type() {
  return uv_handle_get_type(_handle);
}
const char *uvcpp_handle::handle_type_name() {
  return uv_handle_type_name(_handle->type);
}
void *uvcpp_handle::handle_get_data() { return uv_handle_get_data(_handle); }

void *uvcpp_handle::handle_get_loop() { return uv_handle_get_loop(_handle); }

void uvcpp_handle::handle_set_data(void *data) {
  uv_handle_set_data(_handle, data);
}
#endif
#endif

size_t uvcpp_handle::handle_size() { return uv_handle_size(_handle->type); }

int uvcpp_handle::is_active() { return uv_is_active(_handle); }

void uvcpp_handle::set_handle_data() { _handle->data = this; }

void uvcpp_handle::callback_alloc(uv_handle_t *handle, size_t suggested_size,
                             uv_buf_t *buf) {
  uvcpp_handle *wrapper = reinterpret_cast<uvcpp_handle *>(handle->data);
  if (!wrapper || !wrapper->handle_alloc_cb)
    return;

  // Call user alloc callback with a uv_buf view directly (no copy).
  uv_buf *view = reinterpret_cast<uv_buf *>(buf);
  wrapper->handle_alloc_cb(wrapper, suggested_size, view);
}

void uvcpp_handle::callback_close(uv_handle_t *handle) {
  uvcpp_handle *wrapper = nullptr;
  if (handle && handle->data)
    wrapper = reinterpret_cast<uvcpp_handle *>(handle->data);

  if (wrapper && wrapper->handle_close_cb) {
    wrapper->handle_close_cb(wrapper);
  }

  // If the wrapper still owns the underlying handle memory, free it.
  if (wrapper) {
    if (wrapper->_owns_handle && wrapper->_handle == handle) {
      UVCPP_VFREE(handle)
      wrapper->_handle = nullptr;
    }
    // if wrapper does not own it, do not free
  } else {
    // No wrapper associated: do NOT free here. Ownership is unclear and
    // freeing an unowned handle risks double-free. Leave memory management to caller.
  }
}

void uvcpp_handle::close() {
  if (_handle == nullptr)
    return;
  uv_close(_handle, callback_close);
}

int uvcpp_handle::is_closing() { return uv_is_closing(_handle); }

uvcpp_handle::uvcpp_handle(const uvcpp_handle &obj)
    : handle_close_cb(), handle_alloc_cb(), _handle(nullptr), _handle_union() {
  if (obj._handle != nullptr) {
    size_t sz = uv_handle_size(obj._handle->type);
    _handle = (uv_handle_t *)uvcpp::uv_alloc_bytes(sz);
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
    _handle = (uv_handle_t *)uvcpp::uv_alloc_bytes(sz);
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
void uvcpp_handle::ref(uvcpp_handle *vhd) { uv_ref(vhd->_handle); }
void uvcpp_handle::unref(uvcpp_handle *vhd) { uv_unref(vhd->_handle); }
int uvcpp_handle::has_ref(const uvcpp_handle *vhd) {
  return uv_has_ref(vhd->_handle);
}
int uvcpp_handle::is_active(const uvcpp_handle *vhd) {
  return uv_is_active(vhd->_handle);
}
void uvcpp_handle::close(uvcpp_handle *vhd,
                         ::std::function<void(uvcpp_handle *)> closeCallback) {
  vhd->handle_close_cb = closeCallback;
  uv_close(vhd->_handle, callback_close);
}
int uvcpp_handle::is_closing(const uvcpp_handle *vhd) {
  return uv_is_closing(vhd->_handle);
}
int uvcpp_handle::fileno(const uvcpp_handle *vhd, uv_os_sock_t &sock) {
  return uv_fileno(vhd->_handle, (uv_os_fd_t *)&sock);
}
size_t uvcpp_handle::handle_size(const uvcpp_handle *vhd) {
  return uv_handle_size(vhd->_handle->type);
}


#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 18
uv_handle_type uvcpp_handle::handle_get_type(const uvcpp_handle *vhd) {
  return uv_handle_get_type(vhd->_handle);
}
const char *uvcpp_handle::handle_type_name(uvcpp_handle *vhd) {
  return uv_handle_type_name(vhd->_handle->type);
}

void *uvcpp_handle::handle_get_data(const uvcpp_handle *vhd) {
  return uv_handle_get_data(vhd->_handle);
}

void *uvcpp_handle::handle_get_loop(const uvcpp_handle *vhd) {
  return uv_handle_get_loop(vhd->_handle);
}

void uvcpp_handle::handle_set_data(uvcpp_handle *vhd, void *data) {
  uv_handle_set_data(vhd->_handle, data);
}
#endif
#endif


void uvcpp_handle::close(::std::function<void(uvcpp_handle *)> closeCallback) {
  handle_close_cb = closeCallback;
  uv_close(_handle, callback_close);
  return;
}

int uvcpp_handle::fileno(uv_os_sock_t& sock) {
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
    return;
  }

  if (uv_is_active(_handle)) {
    /* If we own the handle, request close and let callback free memory.
       If we do not own it, do not issue close (owner is responsible). */
    if (_owns_handle) {
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