/**
 * @file src/ssl/uvcpp_ssl_common.h
 * @brief SSL/TLS common types and enumerations.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_SSL_UVCPP_SSL_COMMON_H
#define SRC_SSL_UVCPP_SSL_COMMON_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_OPENSSL_ENABLE

#include <cstdint>
#include <string>
#include <vector>

namespace uvcpp {

// =========================================================================
// TLS protocol versions
// =========================================================================

/// 这些值是**协议下界**，不是「默认版本」—— 用哪个由 `uvcpp_ssl_context` 的构造参数
/// 决定，那边的默认是 `TLS_1_2`（`src/ssl/uvcpp_ssl_context.h`）。选定后只往
/// `SSL_CTX_set_min_proto_version()` 送（`src/ssl/uvcpp_ssl_context.cpp`），也就是
/// 「不低于它」，实际协商出来的版本由对端与握手决定。
enum class tls_version : uint8_t {
  TLS_1_0 = 0,   // Deprecated — insecure
  TLS_1_1 = 1,   // Deprecated — insecure
  TLS_1_2 = 2,   // Minimum recommended
  TLS_1_3 = 3,   // Latest
};

// =========================================================================
// SSL/TLS method (client or server)
// =========================================================================

enum class tls_mode : uint8_t {
  CLIENT = 0,
  SERVER = 1,
};

// =========================================================================
// Certificate verification mode
// =========================================================================

/// **客户端**上 `PEER_STRICT` = 校验对端证书 **+ 主机名**：`uvcpp_tcp_client` 与
/// `uvcpp_http_client` 在建立连接时，会把 `connect()` 收到的那个主机名钉给这张证书的
/// 校验（数字 IP 字面量走 iPAddress 匹配，其余走 DNS 名匹配）。于是「证书链可信、
/// 但签发给别的域名」的对端**建立不起来** —— 这正是 `PEER` 挡不住的那一类。
///
/// @warning **服务端上 `PEER_STRICT` 仍与 `PEER` 等价。** 本库的服务端不发 SNI 扩展、
///          也不要求客户端证书，没有可校验的名字，所以那一半只对客户端有意义。
/// @warning 绕开 `uvcpp_tcp_client` / `uvcpp_http_client` 直接用 `uvcpp_ssl` 的调用方
///          **拿不到这层校验** —— 自动通路只覆盖这两个入口，得自己调
///          `uvcpp_ssl::set_verify_hostname()`。
enum class tls_verify_mode : uint8_t {
  NONE     = 0,  // Don't verify peer certificate
  PEER     = 1,  // Verify peer certificate（**不含主机名**）
  PEER_STRICT = 2,  // Verify peer + hostname（服务端上等价于 PEER，见上）
};

// =========================================================================
// SSL context status
// =========================================================================

enum tls_ctx_status : int {
  TLS_CTX_NONE   = 0x00,
  TLS_CTX_READY  = 0x01,
  TLS_CTX_ERROR  = 0x10,
};

// =========================================================================
// Certificate information
// =========================================================================

struct tls_cert_info {
  std::string subject;       // e.g. "/CN=example.com"
  std::string issuer;
  std::string not_before;    // Validity start
  std::string not_after;     // Validity end
  std::string fingerprint;   // SHA-256 fingerprint (hex)
};

// =========================================================================
// ALPN
// =========================================================================

// OpenSSL 的 ALPN 接口吃的是**线格式**（每个协议名前面一个长度字节），不是字符串
// 数组。抽出来是因为它有两个使用点（SSL_CTX_set_alpn_protos 与
// SSL_set_alpn_protos），而长度上限 255 忘了判就会静默产生一个对端解析不了的
// 列表。超长或空的项直接跳过，不报错 —— 调用方给的多半是常量名单。
namespace ssl_detail {

inline std::string alpn_wire_format(const std::vector<std::string>& protos) {
  std::string out;
  for (size_t i = 0; i < protos.size(); ++i) {
    const std::string& p = protos[i];
    if (p.empty() || p.size() > 255) continue;
    out.push_back(static_cast<char>(p.size()));
    out += p;
  }
  return out;
}

}  // namespace ssl_detail

}  // namespace uvcpp

#endif  // UVCPP_OPENSSL_ENABLE
#endif  // SRC_SSL_UVCPP_SSL_COMMON_H
