/**
 * @file src/net/uvcpp_socks5_types.h
 * @brief SOCKS5 wire-format enumerations and the address/endpoint type.
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这些常量来自 RFC 1928 / RFC 1929，公开出来是为了让调用方不必自己抄
 * 数字；编解码本身在私有的 `uvcpp_socks5_common.h` 里。
 */

#pragma once
#ifndef SRC_NET_UVCPP_SOCKS5_TYPES_H
#define SRC_NET_UVCPP_SOCKS5_TYPES_H

#include <uvcpp/uvcpp_config.h>

#include <cstdint>
#include <string>

namespace uvcpp {

/** @brief SOCKS5 命令字节（RFC 1928 §4）。 */
enum class uvcpp_socks5_command : uint8_t {
  CONNECT = 0x01,
  BIND = 0x02,
  UDP_ASSOCIATE = 0x03
};

/** @brief 地址类型字节（RFC 1928 §5）。 */
enum class uvcpp_socks5_atyp : uint8_t {
  IPV4 = 0x01,
  DOMAIN = 0x03,
  IPV6 = 0x04
};

/**
 * @brief 应答码（RFC 1928 §6）。
 *
 * 只列到 `ATYP_UNSUPPORTED`：9..0xFF 是保留值，收到时按 `GENERAL_FAILURE`
 * 处理，不另设可枚举项。
 */
enum class uvcpp_socks5_reply : uint8_t {
  SUCCEEDED = 0x00,
  GENERAL_FAILURE = 0x01,
  NOT_ALLOWED = 0x02,
  NETWORK_UNREACHABLE = 0x03,
  HOST_UNREACHABLE = 0x04,
  CONNECTION_REFUSED = 0x05,
  TTL_EXPIRED = 0x06,
  COMMAND_UNSUPPORTED = 0x07,
  ATYP_UNSUPPORTED = 0x08
};

/** @brief 认证方法（RFC 1928 §3；0x02 的细节在 RFC 1929）。 */
enum class uvcpp_socks5_method : uint8_t {
  NO_AUTH = 0x00,
  USERNAME_PASSWORD = 0x02,
  NO_ACCEPTABLE = 0xFF
};

/**
 * @brief 一个 SOCKS5 目标 / 绑定地址。
 *
 * `host` 是文本形式：点分 IPv4、IPv6 文本（不带方括号）或域名。`port` 是
 * **主机字节序** —— 网络序只在线格式编解码里出现，出了那一层就不再漏出来。
 */
struct uvcpp_socks5_endpoint {
  uvcpp_socks5_atyp atyp;
  std::string host;
  int port;

  uvcpp_socks5_endpoint()
      : atyp(uvcpp_socks5_atyp::IPV4), host(), port(0) {}

  uvcpp_socks5_endpoint(uvcpp_socks5_atyp t, const std::string& h, int p)
      : atyp(t), host(h), port(p) {}

  bool is_ipv4() const { return atyp == uvcpp_socks5_atyp::IPV4; }
  bool is_ipv6() const { return atyp == uvcpp_socks5_atyp::IPV6; }
  bool is_domain() const { return atyp == uvcpp_socks5_atyp::DOMAIN; }
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_SOCKS5_TYPES_H
