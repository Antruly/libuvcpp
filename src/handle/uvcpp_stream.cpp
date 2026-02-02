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
  memset(this->get_handle(), 0, sizeof(uv_stream_t));
  this->set_handle_data();
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
  req->m_write_cb = write_cb;
 
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
  req->m_write_cb = write_cb;

  return uv_write2(OBJ_UVCPP_WRITE_REQ(*req), UVCPP_STREAM_HANDLE,
                   reinterpret_cast<const uv_buf_t *>(bufs),
                   nbufs, (uv_stream_t *)send_handle->get_handle(),
                   uvcpp_write::callback_write);
}
#endif
#endif

#if UV_VERSION_MAJOR >= 1
int uvcpp_stream::try_write(const uv_buf_t bufs[], unsigned int nbufs) {
  uv_buf_t *uvbufs = uvcpp::uvcpp_alloc_arry<uv_buf_t>(nbufs);
  for (int i = 0; i < nbufs; i++){
    uvbufs[i].base = bufs[i].base;
    uvbufs[i].len = bufs[i].len;
  }
  int ret = uv_try_write(UVCPP_STREAM_HANDLE, uvbufs, nbufs);
  UVCPP_VFREE(uvbufs);
  return ret;
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
  uv_buf_t *uvbufs = uvcpp::uvcpp_alloc_arry<uv_buf_t>(nbufs);
  for (int i = 0; i < nbufs; i++){
    uvbufs[i].base = bufs[i].base;
    uvbufs[i].len = bufs[i].len;
  }
  int ret = uv_try_write2(UVCPP_STREAM_HANDLE, uvbufs, nbufs,
                       OBJ_UVCPP_STREAM_HANDLE(*send_handle));
  UVCPP_VFREE(uvbufs);
  return ret;
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
  if (reinterpret_cast<uvcpp_stream *>(handle->data)->stream_connection_cb)
    reinterpret_cast<uvcpp_stream *>(handle->data)
        ->stream_connection_cb(reinterpret_cast<uvcpp_stream *>(handle->data),
                               status);
}

// uv_read_cb
void uvcpp_stream::callback_read(uv_stream_t *handle, ssize_t nread,
                            const uv_buf_t *buf) {
  uvcpp_stream *wrapper = reinterpret_cast<uvcpp_stream *>(handle->data);
  if (!wrapper || !wrapper->stream_read_cb)
    return;
  // Pass the libuv buffer directly as uv_buf_t (no copy).
  uv_buf_t view;
  wrapper->stream_read_cb(wrapper, nread, (const uv_buf_t*)buf);
  
}


} // namespace uvcpp