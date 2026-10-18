/**
 * @file src/net/uvcpp_socks5_udp.cpp
 * @brief SOCKS5 UDP 关联数据面的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "net/uvcpp_socks5_udp.h"

#include <net/uvcpp_socks5_common.h>
#include <net/uvcpp_udp_client.h>
#include <handle/uvcpp_loop.h>
#include <uvcpp/uvcpp_buf.h>

#include <cstring>

namespace uvcpp {

namespace {

/** @brief 端口 0..65535 才算合法（端口 0 在请求里是"由代理决定"）。 */
bool port_ok(int port) { return port >= 0 && port <= 65535; }

}  // namespace

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_socks5_udp::uvcpp_socks5_udp()
    : loop_(new uvcpp_loop()),
      udp_(nullptr),
      owns_loop_(true),
      has_relay_(false),
      bound_(false),
      recv_installed_(false),
      dropped_(0) {
  std::memset(&relay_addr_, 0, sizeof(relay_addr_));
}

uvcpp_socks5_udp::uvcpp_socks5_udp(uvcpp_loop* loop)
    : loop_(loop),
      udp_(nullptr),
      owns_loop_(false),
      has_relay_(false),
      bound_(false),
      recv_installed_(false),
      dropped_(0) {
  std::memset(&relay_addr_, 0, sizeof(relay_addr_));
}

uvcpp_socks5_udp::~uvcpp_socks5_udp() {
  // 先放 UDP 客户端：它只关自己的句柄，不碰循环（外部循环那条路）。
  if (udp_ != nullptr) {
    delete udp_;
    udp_ = nullptr;
  }
  // 自建循环这条路与 `uvcpp_udp_client` 的析构同一套次序：句柄关完之后循环
  // 才可能退干净，而 `loop_close()` 只看队列、不跑回调，所以先拨几轮把关闭
  // 回调放掉。
  if (owns_loop_ && loop_ != nullptr) {
    for (int i = 0; i < 256 && loop_->loop_alive() != 0; ++i) {
      loop_->run(UV_RUN_NOWAIT);
    }
    loop_->loop_close();
    delete loop_;
    loop_ = nullptr;
  }
}

uvcpp_loop* uvcpp_socks5_udp::get_loop() { return loop_; }

uvcpp_udp_client* uvcpp_socks5_udp::get_udp() { return udp_; }

// =========================================================================
// 绑定 / 中继
// =========================================================================

int uvcpp_socks5_udp::bind(const std::string& host, int port) {
  if (loop_ == nullptr) {
    last_error_ = "no event loop";
    return UV_EINVAL;
  }
  if (!port_ok(port)) {
    last_error_ = "invalid port";
    return UV_EINVAL;
  }

  // 重新绑：先把旧的放掉（close() 之后 udp_ 为空）。
  if (udp_ != nullptr) {
    if (recv_installed_) {
      udp_->recv_stop();
      recv_installed_ = false;
    }
    delete udp_;
    udp_ = nullptr;
  }

  udp_ = new uvcpp_udp_client(loop_);
  const int rc = udp_->bind(host.c_str(), port);
  if (rc != 0) {
    last_error_ = uv_strerror(rc);
    delete udp_;
    udp_ = nullptr;
    return rc;
  }
  bound_ = true;
  return 0;
}

uvcpp_socks5_endpoint uvcpp_socks5_udp::local_endpoint() {
  if (udp_ == nullptr || !bound_) return uvcpp_socks5_endpoint();
  std::string ip;
  int port = 0;
  const sockaddr_storage ss =
      udp_->getLocalAddrs(ip, port);
  (void)ss;
  uvcpp_socks5_atyp atyp = uvcpp_socks5_atyp::IPV4;
  if (socks5_detail::classify_host(ip, atyp)) {
    return uvcpp_socks5_endpoint(atyp, ip, port);
  }
  return uvcpp_socks5_endpoint(uvcpp_socks5_atyp::IPV4, ip, port);
}

bool uvcpp_socks5_udp::endpoint_to_sockaddr(const uvcpp_socks5_endpoint& ep,
                                            struct sockaddr_storage& out) {
  std::memset(&out, 0, sizeof(out));
  if (!port_ok(ep.port)) return false;
  if (ep.atyp == uvcpp_socks5_atyp::IPV4) {
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    if (uv_ip4_addr(ep.host.c_str(), ep.port, &sa) != 0) return false;
    std::memcpy(&out, &sa, sizeof(sa));
    return true;
  }
  if (ep.atyp == uvcpp_socks5_atyp::IPV6) {
    struct sockaddr_in6 sa;
    std::memset(&sa, 0, sizeof(sa));
    if (uv_ip6_addr(ep.host.c_str(), ep.port, &sa) != 0) return false;
    std::memcpy(&out, &sa, sizeof(sa));
    return true;
  }
  // 域名：热路径上不做 DNS（见头文件）。
  return false;
}

bool uvcpp_socks5_udp::sockaddr_to_endpoint(const struct sockaddr* addr,
                                            uvcpp_socks5_endpoint& out) {
  if (addr == nullptr) return false;
  char ip[64];
  if (addr->sa_family == AF_INET) {
    const struct sockaddr_in* sa = reinterpret_cast<const struct sockaddr_in*>(addr);
    if (uv_ip4_name(sa, ip, sizeof(ip)) != 0) return false;
    out = uvcpp_socks5_endpoint(uvcpp_socks5_atyp::IPV4, ip,
                                ntohs(sa->sin_port));
    return true;
  }
  if (addr->sa_family == AF_INET6) {
    const struct sockaddr_in6* sa =
        reinterpret_cast<const struct sockaddr_in6*>(addr);
    if (uv_ip6_name(sa, ip, sizeof(ip)) != 0) return false;
    out = uvcpp_socks5_endpoint(uvcpp_socks5_atyp::IPV6, ip,
                                ntohs(sa->sin6_port));
    return true;
  }
  return false;
}

int uvcpp_socks5_udp::set_relay(const uvcpp_socks5_endpoint& relay) {
  struct sockaddr_storage ss;
  if (!endpoint_to_sockaddr(relay, ss)) {
    last_error_ =
        "SOCKS5 UDP relay must be a numeric IPv4/IPv6 address with a valid port";
    return UV_EINVAL;
  }
  relay_     = relay;
  relay_addr_ = ss;
  has_relay_ = true;
  return 0;
}

const uvcpp_socks5_endpoint& uvcpp_socks5_udp::relay() const { return relay_; }

// =========================================================================
// 发送
// =========================================================================

int uvcpp_socks5_udp::send_to(const uvcpp_socks5_endpoint& target,
                              const void* data, std::size_t len,
                              std::function<void(int)> cb) {
  if (udp_ == nullptr || !bound_) {
    last_error_ = "UDP socket is not bound";
    return UV_EINVAL;
  }
  if (!has_relay_) {
    last_error_ = "SOCKS5 UDP relay address is not set";
    return UV_EINVAL;
  }

  std::string frame;
  if (!socks5_detail::encode_udp_datagram(frame, target, data, len)) {
    last_error_ = "invalid SOCKS5 UDP target";
    return UV_EINVAL;
  }
  // UDP 载荷上限是 65535 - IP 头 - UDP 头，而 SOCKS5 头也算在报文里。
  // 超过就明确报错，**不截断**：截断的 DNS 查询/应答比失败更难查。
  if (frame.size() > 65535) {
    last_error_ = "SOCKS5 UDP datagram exceeds 65535 bytes";
    return UV_EMSGSIZE;
  }

  return udp_->send_addr(
      reinterpret_cast<const struct sockaddr*>(&relay_addr_), frame.data(),
      frame.size(), std::move(cb));
}

int uvcpp_socks5_udp::send_to(const std::string& host, int port,
                              const void* data, std::size_t len,
                              std::function<void(int)> cb) {
  uvcpp_socks5_atyp atyp = uvcpp_socks5_atyp::DOMAIN_NAME;
  if (!socks5_detail::classify_host(host, atyp)) {
    last_error_ = "invalid SOCKS5 UDP target host";
    return UV_EINVAL;
  }
  if (!port_ok(port) || port == 0) {
    last_error_ = "invalid SOCKS5 UDP target port";
    return UV_EINVAL;
  }
  return send_to(uvcpp_socks5_endpoint(atyp, host, port), data, len,
                 std::move(cb));
}

int uvcpp_socks5_udp::send_to_wait(const uvcpp_socks5_endpoint& target,
                                   const void* data, std::size_t len,
                                   int timeout_ms) {
  if (udp_ == nullptr || !bound_) {
    last_error_ = "UDP socket is not bound";
    return UV_EINVAL;
  }
  if (!has_relay_) {
    last_error_ = "SOCKS5 UDP relay address is not set";
    return UV_EINVAL;
  }

  std::string frame;
  if (!socks5_detail::encode_udp_datagram(frame, target, data, len)) {
    last_error_ = "invalid SOCKS5 UDP target";
    return UV_EINVAL;
  }
  if (frame.size() > 65535) {
    last_error_ = "SOCKS5 UDP datagram exceeds 65535 bytes";
    return UV_EMSGSIZE;
  }

  return udp_->send_addr_wait(
      reinterpret_cast<const struct sockaddr*>(&relay_addr_), frame.data(),
      frame.size(), timeout_ms);
}

int uvcpp_socks5_udp::send_to_wait(const std::string& host, int port,
                                   const void* data, std::size_t len,
                                   int timeout_ms) {
  uvcpp_socks5_atyp atyp = uvcpp_socks5_atyp::DOMAIN_NAME;
  if (!socks5_detail::classify_host(host, atyp)) {
    last_error_ = "invalid SOCKS5 UDP target host";
    return UV_EINVAL;
  }
  return send_to_wait(uvcpp_socks5_endpoint(atyp, host, port), data, len,
                      timeout_ms);
}

// =========================================================================
// 接收
// =========================================================================

int uvcpp_socks5_udp::recv_start(
    std::function<void(int, const uvcpp_socks5_endpoint&, const void*,
                       std::size_t)> cb) {
  if (udp_ == nullptr || !bound_) {
    last_error_ = "UDP socket is not bound";
    return UV_EINVAL;
  }
  if (cb == nullptr) {
    last_error_ = "recv_start requires a callback";
    return UV_EINVAL;
  }
  recv_cb_ = std::move(cb);
  const int rc = udp_->recv_start(
      [this](uvcpp_buf* b, const char* ip, int port) {
        (void)ip;
        (void)port;
        if (b == nullptr) return;
        on_datagram(b->get_const_data(), b->size());
      });
  if (rc != 0) {
    last_error_ = uv_strerror(rc);
    return rc;
  }
  recv_installed_ = true;
  return 0;
}

int uvcpp_socks5_udp::recv_stop() {
  if (udp_ == nullptr) return 0;
  recv_installed_ = false;
  return udp_->recv_stop();
}

void uvcpp_socks5_udp::on_datagram(const char* data, std::size_t len) {
  uvcpp_socks5_endpoint from;
  std::size_t payload_off = 0;
  const socks5_detail::parse_status st =
      socks5_detail::decode_udp_datagram(data, len, from, payload_off);
  if (st != socks5_detail::parse_status::OK || payload_off > len) {
    // RSV ≠ 0 / FRAG ≠ 0 / 头截断 / ATYP 非法：丢弃并计数。**不重组分片**，
    // 见头文件的说明。
    ++dropped_;
    return;
  }
  if (!recv_cb_) return;
  recv_cb_(0, from, len > payload_off ? data + payload_off : nullptr,
           len - payload_off);
}

std::uint64_t uvcpp_socks5_udp::dropped_datagrams() const { return dropped_; }

const std::string& uvcpp_socks5_udp::last_error() const { return last_error_; }

void uvcpp_socks5_udp::close() {
  if (udp_ == nullptr) return;
  if (recv_installed_) {
    udp_->recv_stop();
    recv_installed_ = false;
  }
  delete udp_;
  udp_ = nullptr;
  bound_ = false;
  has_relay_ = false;
  std::memset(&relay_addr_, 0, sizeof(relay_addr_));
}

}  // namespace uvcpp
