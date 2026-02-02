#include "uvcpp_tcp.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_tcp::uvcpp_tcp() : uvcpp_stream() {
  uv_tcp_t* tcp = uvcpp::uvcpp_alloc<uv_tcp_t>();
  this->set_handle(tcp, true);
  this->init();
}

uvcpp_tcp::~uvcpp_tcp() {
  for (auto item = addrs.begin(); item != addrs.end(); item++) {
    UVCPP_VDELETE(*item);
    *item = nullptr;
  }
  addrs.clear();
}

uvcpp_tcp::uvcpp_tcp(uvcpp_loop *loop) : uvcpp_stream() {
  uv_tcp_t* tcp = uvcpp::uvcpp_alloc<uv_tcp_t>();
  this->set_handle(tcp, true);
  this->init(loop);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 7
uvcpp_tcp::uvcpp_tcp(uvcpp_loop *loop, unsigned int flags) : uvcpp_stream() {
  uv_tcp_t* tcp = uvcpp::uvcpp_alloc<uv_tcp_t>();
  this->set_handle(tcp, true);
  init(loop, flags);
}
#endif
#endif

int uvcpp_tcp::init() {
  memset(UVCPP_TCP_HANDLE, 0, sizeof(uv_tcp_t));
  this->set_handle_data();
  return 0;
}

int uvcpp_tcp::init(uvcpp_loop *loop) {
  return uv_tcp_init((uv_loop_t *)loop->get_handle(),
                     (uv_tcp_t *)this->get_handle());
}

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 7
int uvcpp_tcp::init(uvcpp_loop *loop, unsigned int flags) {
  return uv_tcp_init_ex((uv_loop_t *)loop->get_handle(),
                        (uv_tcp_t *)this->get_handle(), flags);
}
#endif
#endif

int uvcpp_tcp::open(uv_os_sock_t sock) { return uv_tcp_open(UVCPP_TCP_HANDLE, sock); }

int uvcpp_tcp::nodelay(int enable) { return uv_tcp_nodelay(UVCPP_TCP_HANDLE, enable); }

int uvcpp_tcp::keepalive(int enable, unsigned int delay) {
  return uv_tcp_keepalive(UVCPP_TCP_HANDLE, enable, delay);
}

int uvcpp_tcp::simultaneousAccepts(int enable) {
  return uv_tcp_simultaneous_accepts(UVCPP_TCP_HANDLE, enable);
}

int uvcpp_tcp::bind(const sockaddr *addr, unsigned int flags) {
  return uv_tcp_bind(UVCPP_TCP_HANDLE, addr, flags);
}

int uvcpp_tcp::getsockname(sockaddr *name, int *namelen) {
  return uv_tcp_getsockname(UVCPP_TCP_HANDLE, name, namelen);
}

int uvcpp_tcp::getpeername(sockaddr *name, int *namelen) {
  return uv_tcp_getpeername(UVCPP_TCP_HANDLE, name, namelen);
}

int uvcpp_tcp::bindIpv4(const char *addripv4, int port, int flags) {
  int ret;
  sockaddr_in *addr_in = new sockaddr_in();
  ret = uv_ip4_addr(addripv4, port, addr_in);
  if (ret) {
    return ret;
  }
  ret = this->bind((sockaddr *)addr_in, flags);
  if (ret) {
    return ret;
  }
  addrs.push_back((sockaddr *)addr_in);
  return ret;
}

int uvcpp_tcp::bindIpv6(const char *addripv6, int port, int flags) {
  int ret;
  sockaddr_in6 *addr_in6 = new sockaddr_in6();
  ret = uv_ip6_addr(addripv6, port, addr_in6);
  if (ret) {
    return ret;
  }
  ret = this->bind((sockaddr *)addr_in6, flags);
  if (ret) {
    return ret;
  }
  addrs.push_back((sockaddr *)addr_in6);
  return ret;
}

int uvcpp_tcp::connect(uvcpp_connect *req, const sockaddr *addr,
                       std::function<void(uvcpp_connect *, int)> connect_cb) {
  req->m_connect_cb = connect_cb;
  return uv_tcp_connect(OBJ_UVCPP_CONNECT_REQ(*req), UVCPP_TCP_HANDLE, addr,
                        uvcpp_connect::callback_connect);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 32
int uvcpp_tcp::closeReset(std::function<void(uvcpp_handle *)> close_cb) {
  this->handle_close_cb = close_cb;
  return uv_tcp_close_reset(UVCPP_TCP_HANDLE, uvcpp_handle::callback_close);
}
#endif
#endif



} // namespace uvcpp