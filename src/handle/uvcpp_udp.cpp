#include "uvcpp_udp.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_udp::uvcpp_udp() : uvcpp_handle(), udp_recv_cb(){
  uv_udp_t *udp = uvcpp::uvcpp_alloc<uv_udp_t>();
  this->set_handle(udp, true);
  this->init();
}
uvcpp_udp::~uvcpp_udp() {}

uvcpp_udp::uvcpp_udp(uvcpp_loop *loop) : uvcpp_handle(), udp_recv_cb() {
  uv_udp_t *udp = uvcpp::uvcpp_alloc<uv_udp_t>();
  this->set_handle(udp, true);
  this->init(loop);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 6
uvcpp_udp::uvcpp_udp(uvcpp_loop *loop, unsigned int flags)
    : uvcpp_handle(), udp_recv_cb() {
  uv_udp_t *udp = uvcpp::uvcpp_alloc<uv_udp_t>();
  this->set_handle(udp, true);
  this->init(loop, flags);
}
#endif
#endif

int uvcpp_udp::init() {
  memset(UVCPP_UDP_HANDLE, 0, sizeof(uv_udp_t));
  this->set_handle_data();
  return 0;
}

int uvcpp_udp::init(uvcpp_loop *loop) {
  int ret = uv_udp_init(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_UDP_HANDLE);
  this->set_handle_data();
  return ret;
}

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 6
int uvcpp_udp::init(uvcpp_loop *loop, unsigned int flags) {
  int ret =
  uv_udp_init_ex(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_UDP_HANDLE, flags);
  this->set_handle_data();
  return ret;
}
#endif
#endif

int uvcpp_udp::open(uv_os_sock_t sock) { return uv_udp_open(UVCPP_UDP_HANDLE, sock); }

int uvcpp_udp::bind(const sockaddr *addr, unsigned int flags) {
  return uv_udp_bind(UVCPP_UDP_HANDLE, addr, flags);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 27
int uvcpp_udp::connect(const sockaddr *addr) {
  return uv_udp_connect(UVCPP_UDP_HANDLE, addr);
}

int uvcpp_udp::getpeername(sockaddr *name, int *namelen) {
  return uv_udp_getpeername(UVCPP_UDP_HANDLE, name, namelen);
}
#endif
#endif

int uvcpp_udp::getsockname(sockaddr *name, int *namelen) {
  return uv_udp_getsockname(UVCPP_UDP_HANDLE, name, namelen);
}

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 32
int uvcpp_udp::set_membership(const char *multicast_addr, const char *interface_addr,
                        uv_membership membership) {
  return uv_udp_set_membership(UVCPP_UDP_HANDLE, multicast_addr, interface_addr,
                               membership);
}
#endif
#endif
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 32
int uvcpp_udp::set_source_membership(const char *multicast_addr,
                              const char *interface_addr,
                              const char *source_addr,
                              uv_membership membership) {
  return uv_udp_set_source_membership(UVCPP_UDP_HANDLE, multicast_addr,
                                      interface_addr, source_addr, membership);
}
#endif
#endif

int uvcpp_udp::set_multicast_loop(int on) {
  return uv_udp_set_multicast_loop(UVCPP_UDP_HANDLE, on);
}

int uvcpp_udp::set_multicast_ttl(int ttl) {
  return uv_udp_set_multicast_ttl(UVCPP_UDP_HANDLE, ttl);
}

int uvcpp_udp::set_multicast_interface(const char *interface_addr) {
  return uv_udp_set_multicast_interface(UVCPP_UDP_HANDLE, interface_addr);
}

int uvcpp_udp::set_broadcast(int on) { return uv_udp_set_broadcast(UVCPP_UDP_HANDLE, on); }

int uvcpp_udp::set_ttl(int ttl) { return uv_udp_set_ttl(UVCPP_UDP_HANDLE, ttl); }

int uvcpp_udp::send(uvcpp_udp_send *req, const uv_buf_t bufs[], unsigned int nbufs,
               const sockaddr *addr,
               std::function<void(uvcpp_udp_send *, int)> udp_send_cb) {
  req->udp_send_cb = udp_send_cb;

  return uv_udp_send(OBJ_UVCPP_UDP_SEND_REQ(*req), UVCPP_UDP_HANDLE,
                     bufs,
                     nbufs, addr,
                     uvcpp_udp_send::callback_udp_send);
}

int uvcpp_udp::recv_start(std::function<void(uvcpp_handle *, size_t, uv_buf_t*)> alloc_cb,
                    std::function<void(uvcpp_udp *, ssize_t, const uv_buf_t*,
                                       const struct sockaddr *, unsigned int)>
                        recv_cb) {
  this->handle_alloc_cb = alloc_cb;
  this->udp_recv_cb = recv_cb;
 
  return uv_udp_recv_start(UVCPP_UDP_HANDLE, uvcpp_handle::callback_alloc,
                           callback_udp_recv);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 39
int uvcpp_udp::using_recvmmsg() { return uv_udp_using_recvmmsg(UVCPP_UDP_HANDLE); }

#endif
#endif

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 19
size_t uvcpp_udp::get_send_queue_size() {
  return uv_udp_get_send_queue_size(UVCPP_UDP_HANDLE);
}
size_t uvcpp_udp::get_send_queue_count() {
  return uv_udp_get_send_queue_count(UVCPP_UDP_HANDLE);
}
#endif
#endif
// uv_udp_recv_cb
void uvcpp_udp::callback_udp_recv(uv_udp_t *handle, ssize_t nread,
                             const uv_buf_t *buf, const sockaddr *addr,
                             unsigned flags) {
  uvcpp_udp *wrapper = reinterpret_cast<uvcpp_udp *>(handle->data);
  if (!wrapper || !wrapper->udp_recv_cb)
    return;
  wrapper->udp_recv_cb(wrapper, nread, buf,
                       addr, flags);

}
} // namespace uvcpp
