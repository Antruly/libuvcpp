#include "uvcpp_stream.h"
#include <uvcpp/uvcpp_alloc.h>
#include <vector>
namespace uvcpp {
uvcpp_stream::uvcpp_stream() {
  uv_stream_t* stream = uvcpp::uvcpp_alloc<uv_stream_t>();
  this->set_handle(stream, true);
  this->init();
}
uvcpp_stream::~uvcpp_stream() { }

int uvcpp_stream::init() {
  // 已接进循环的句柄不能清零，理由见 uvcpp_handle::reset_handle_state。
  this->reset_handle_state(this->get_handle(), sizeof(uv_stream_t));
  return 0;
}

int uvcpp_stream::listen(std::function<void(uvcpp_stream *, int)> connection_cb,
                    int backlog) {
  stream_connection_cb = connection_cb;
  return uv_listen(UVCPP_STREAM_HANDLE, backlog, callback_connection);
}

int uvcpp_stream::accept(uvcpp_stream *client) {
  return uv_accept(UVCPP_STREAM_HANDLE, OBJ_UVCPP_STREAM_HANDLE(*client));
}

int uvcpp_stream::read_start(
    std::function<void(uvcpp_handle *, size_t, uv_buf_t*)> alloc_cb,
    std::function<void(uvcpp_stream *, ssize_t, const uv_buf_t*)> read_cb) {
  handle_alloc_cb = alloc_cb;
  stream_read_cb = read_cb;
  return uv_read_start(UVCPP_STREAM_HANDLE, uvcpp_handle::callback_alloc, callback_read);
}

int uvcpp_stream::write(uvcpp_write *req, const uv_buf_t bufs[], unsigned int nbufs,
                   std::function<void(uvcpp_write *, int)> write_cb) {
  // **`std::move` 是承重的**（同 `uvcpp_tcp_client::write` 里那条注释）：
  // 按值收下的 `write_cb` 到这一句已经没人要了，而拷贝一个 `std::function`
  // 会把它捕获的东西整份复制一遍 —— 那是一次实打实的堆分配（GCC 的判据是
  // `__is_location_invariant`，即 `is_trivially_copyable`，**与 sizeof 无关**：
  // 捕获 `shared_ptr` 的 lambda 只占 16 字节也照样走堆）。移动只是把内部
  // 指针接过来。每响应一次写，所以这是每请求一笔。
  req->m_write_cb = std::move(write_cb);
 
  return uv_write(OBJ_UVCPP_WRITE_REQ(*req), UVCPP_STREAM_HANDLE,
                  reinterpret_cast<const uv_buf_t *>(bufs),
                  nbufs,
                  uvcpp_write::callback_write);
}

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 41
int uvcpp_stream::write(uvcpp_write *req, const uv_buf_t bufs[], unsigned int nbufs,
                   uvcpp_stream *send_handle,
                   std::function<void(uvcpp_write *, int)> write_cb) {
  // **`std::move` 是承重的**（同 `uvcpp_tcp_client::write` 里那条注释）：
  // 按值收下的 `write_cb` 到这一句已经没人要了，而拷贝一个 `std::function`
  // 会把它捕获的东西整份复制一遍 —— 那是一次实打实的堆分配（GCC 的判据是
  // `__is_location_invariant`，即 `is_trivially_copyable`，**与 sizeof 无关**：
  // 捕获 `shared_ptr` 的 lambda 只占 16 字节也照样走堆）。移动只是把内部
  // 指针接过来。每响应一次写，所以这是每请求一笔。
  req->m_write_cb = std::move(write_cb);

  return uv_write2(OBJ_UVCPP_WRITE_REQ(*req), UVCPP_STREAM_HANDLE,
                   reinterpret_cast<const uv_buf_t *>(bufs),
                   nbufs, (uv_stream_t *)send_handle->get_handle(),
                   uvcpp_write::callback_write);
}
#endif
#endif

#if UV_VERSION_MAJOR >= 1
int uvcpp_stream::try_write(const uv_buf_t bufs[], unsigned int nbufs) {
  // `uv_try_write` 收的就是 `const uv_buf_t bufs[]`，直接透传即可：试发不持有
  // 这些 buffer（要么当场写进内核、要么一个字节都不动），调用方的数组本来就得
  // 活过这次调用，所以那份"拷一份自己的"既保不住什么，也白付一次
  // malloc + memset + free（`uvcpp_alloc_arry` 失败还会抛 `std::bad_alloc`）。
  return uv_try_write(UVCPP_STREAM_HANDLE, bufs, nbufs);
}
#else
int uvcpp_stream::try_write(const uv_buf_t bufs[], unsigned int nbufs) {
  (void)bufs; (void)nbufs;
  return UV_ENOSYS;
}
#endif
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 42
int uvcpp_stream::try_write(const uv_buf_t bufs[], unsigned int nbufs,
                      uvcpp_stream *send_handle) {
  // 同上：`uv_try_write2` 的 `bufs` 也是 `const uv_buf_t[]`。
  return uv_try_write2(UVCPP_STREAM_HANDLE, bufs, nbufs,
                       OBJ_UVCPP_STREAM_HANDLE(*send_handle));
}
#endif
#endif

int uvcpp_stream::is_readable() { return uv_is_readable(UVCPP_STREAM_HANDLE); }

int uvcpp_stream::is_writable() { return uv_is_writable(UVCPP_STREAM_HANDLE); }

int uvcpp_stream::stream_set_blocking(int blocking) {
  return uv_stream_set_blocking(UVCPP_STREAM_HANDLE, blocking);
}

// uv_connection_cb
void uvcpp_stream::callback_connection(uv_stream_t *handle, int status) {
  uvcpp_stream *self = reinterpret_cast<uvcpp_stream *>(handle->data);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 句柄回调会重复触发，所以是拷不是搬。
  auto cb = self->stream_connection_cb;
  if (cb) {
    cb(self, status);
  }
}

// uv_read_cb
void uvcpp_stream::callback_read(uv_stream_t *handle, ssize_t nread,
                            const uv_buf_t *buf) {
  uvcpp_stream *self = reinterpret_cast<uvcpp_stream *>(handle->data);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 读回调会重复触发，所以是拷不是搬。
  // Pass the libuv buffer directly as uv_buf_t (no copy).
  auto cb = self->stream_read_cb;
  if (cb) {
    cb(self, nread, (const uv_buf_t*)buf);
  }
}


} // namespace uvcpp