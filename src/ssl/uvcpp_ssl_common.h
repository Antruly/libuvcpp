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

/// @warning `PEER_STRICT` **目前与 `PEER` 完全等价**：`set_verify_mode()` 把两者都映射
///          到同一个 `SSL_VERIFY_PEER` 位，主机名那一半**从未实现**（全仓没有一处
///          `SSL_set1_host` / `X509_VERIFY_PARAM_set1_host`，也没有把 SNI 主机名递给
///          校验的管道）。所以靠它**挡不住**「证书链可信、但签发给别的域名」的对端 ——
///          需要这个保证的调用方得自己在握手后校验对端证书。
enum class tls_verify_mode : uint8_t {
  NONE     = 0,  // Don't verify peer certificate
  PEER     = 1,  // Verify peer certificate
  PEER_STRICT = 2,  // Verify peer + hostname（**主机名这一半尚未实现**，见上）
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
