/**
 * @file src/net/uvcpp_socks5_udp.h
 * @brief SOCKS5 UDP 关联的数据面：封装/解封装与收发。
 * @author zhuweiye
 * @version 1.0.0
 *
 * RFC 1928 §7：UDP ASSOCIATE 之后，客户端与代理之间的**每一个**数据报都带
 * 一个 SOCKS5 头（RSV(2) + FRAG(1) + ATYP + ADDR + PORT），载荷跟在后面。
 * 本类就是那个头 —— 它把 `uvcpp_udp_client` 的裸收发包成"发到某个目标 /
 * 从某个来源收到"。
 *
 * 本类**不做分片重组**（FRAG ≠ 0 的数据报直接丢弃并计数）。RFC 1928 明确
 * 允许实现不支持分片（"A SOCKS implementation MUST NOT support fragmentation
 * if it does not implement the FRAG field"），常见实现都不做。
 *
 * 典型用法（一般不必直接用：`uvcpp_socks5_client::udp()` 返回的就是它，
 * 且中继地址已经由客户端填好）：
 * @code
 *   uvcpp_socks5_udp udp(loop);
 *   udp.bind("0.0.0.0", 0);
 *   udp.set_relay(uvcpp_socks5_endpoint(uvcpp_socks5_atyp::IPV4, "127.0.0.1", 1080));
 *   udp.recv_start([](int st, const uvcpp_socks5_endpoint& from,
 *                     const void* data, size_t len) { ... });
 *   udp.send_to("example.com", 53, query, n);
 * @endcode
 */

#pragma once
#ifndef SRC_NET_UVCPP_SOCKS5_UDP_H
#define SRC_NET_UVCPP_SOCKS5_UDP_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <uv.h>

#include <net/uvcpp_socks5_types.h>

namespace uvcpp {

class uvcpp_loop;
class uvcpp_udp_client;

/**
 * @brief SOCKS5 UDP 数据面（封装/解封装 + 收发）。
 *
 * 地址两件事要分清：
 * - **中继地址**（relay）：所有数据报的**实际目的地**，是代理给的那个
 *   `BND.ADDR/BND.PORT`。必须由调用方（或 `uvcpp_socks5_client`）先设好。
 * - **目标地址**（target）：写在 SOCKS5 头里的真正目的地，域名也行 ——
 *   解析是代理的事。
 *
 * 中继地址**必须是数字地址**（IPv4/IPv6）。域名要么由代理回了域名（少见），
 * 要么由调用方先解析；本类不在热路径上做 DNS，`set_relay` 遇到域名直接
 * `UV_EINVAL`。
 */
class UVCPP_API uvcpp_socks5_udp {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_socks5_udp)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_socks5_udp)

  /**
   * @brief 用一条**外部**事件循环（与 `uvcpp_udp_client` 同一条规则）。
   *
   * 本对象**不拥有**它：析构时句柄照关，循环不动。
   */
  explicit uvcpp_socks5_udp(uvcpp_loop* loop);

  /** @brief 本对象跑在哪条循环上。 */
  uvcpp_loop* get_loop();

  /** @brief 底层的裸 UDP 客户端（收发与地址查询用）。 */
  uvcpp_udp_client* get_udp();

  // -----------------------------------------------------------------
  // 绑定 / 中继
  // -----------------------------------------------------------------

  /**
   * @brief 绑本地 UDP socket。`host` 含 `:` 按 IPv6 处理（与 udp_client 同规则）。
   *
   * 必须在 `recv_start()` / `send_to()` 之前调用。
   */
  int bind(const std::string& host = "0.0.0.0", int port = 0);

  /** @brief 本地地址（`bind()` 之后有效）；未绑定时返回空端点（端口 0）。 */
  uvcpp_socks5_endpoint local_endpoint();

  /**
   * @brief 设中继地址（代理回的 `BND.ADDR/BND.PORT`）。
   *
   * `relay.host` 必须是数字 IPv4/IPv6；域名返回 `UV_EINVAL` —— 这是热路径上
   * 唯一一次地址解析，放在发送里做会让每个数据报都可能失败一次。
   */
  int set_relay(const uvcpp_socks5_endpoint& relay);

  /** @brief 当前中继地址；未设时 `port == 0`。 */
  const uvcpp_socks5_endpoint& relay() const;

  // -----------------------------------------------------------------
  // 发送
  // -----------------------------------------------------------------

  /**
   * @brief 发一个封装好的数据报给 \p target（经中继）。
   *
   * 未绑 / 未设中继返回 `UV_EINVAL`；`len + 头` 超过 65535 返回
   * `UV_EMSGSIZE`（**不截断**）。
   *
   * 异步：`cb` 在发出完成时触发（0 为成功）。同步：`cb` 为空时阻塞等待
   * （见 `send_to_wait`），默认 30 s。
   */
  int send_to(const uvcpp_socks5_endpoint& target,
              const void* data, std::size_t len,
              std::function<void(int)> cb = nullptr);

  /** @brief 同上，目标用 host/port 文本表示（域名可行，代理负责解析）。 */
  int send_to(const std::string& host, int port,
              const void* data, std::size_t len,
              std::function<void(int)> cb = nullptr);

  int send_to_wait(const uvcpp_socks5_endpoint& target,
                   const void* data, std::size_t len,
                   int timeout_ms = 30000);

  int send_to_wait(const std::string& host, int port,
                   const void* data, std::size_t len,
                   int timeout_ms = 30000);

  // -----------------------------------------------------------------
  // 接收
  // -----------------------------------------------------------------

  /**
   * @brief 开始接收并解封装。
   *
   * 回调参数：
   * - `status`：0 正常；负值为解封装失败（**坏包不会回调**，只计数，
   *   所以这里实际上恒为 0 —— 保留参数是为了将来"中继不可达"之类的
   *   socket 级错误）。
   * - `from`：SOCKS5 头里的来源地址（代理写的是**目标**的地址）。
   * - `data`/`len`：载荷；**只在本次回调期间有效**。
   *
   * RSV ≠ 0、FRAG ≠ 0、头部截断、ATYP 非法：丢弃并计入
   * `dropped_datagrams()`。
   */
  int recv_start(std::function<void(int status,
                                    const uvcpp_socks5_endpoint& from,
                                    const void* data, std::size_t len)> cb);

  /** @brief 停止接收（保留中继与绑定，可以再 `recv_start`）。 */
  int recv_stop();

  /** @brief 关掉底层 socket（之后要重新 `bind`）。可以在别的回调里调。 */
  void close();

  /** @brief 被丢弃的数据报数（坏头 / RSV / FRAG / 超长）。 */
  std::uint64_t dropped_datagrams() const;

  /** @brief 最近一次本地错误的可读说明。 */
  const std::string& last_error() const;

 private:
  /** @brief 文本端点 → sockaddr；域名/非法端口返回 false。 */
  static bool endpoint_to_sockaddr(const uvcpp_socks5_endpoint& ep,
                                   struct sockaddr_storage& out);

  /** @brief 从 `sockaddr` 解出文本端点（IPv4/IPv6）。 */
  static bool sockaddr_to_endpoint(const struct sockaddr* addr,
                                   uvcpp_socks5_endpoint& out);

  void on_datagram(const char* data, std::size_t len);

  uvcpp_loop*       loop_;
  uvcpp_udp_client* udp_;
  bool              owns_loop_;

  uvcpp_socks5_endpoint relay_;
  bool                  has_relay_;
  struct sockaddr_storage relay_addr_;

  bool bound_;
  bool recv_installed_;
  std::uint64_t dropped_;
  std::string last_error_;

  std::function<void(int, const uvcpp_socks5_endpoint&, const void*,
                     std::size_t)> recv_cb_;
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_SOCKS5_UDP_H
