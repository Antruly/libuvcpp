/**
 * @file src/ssl/uvcpp_ssl_common.h
 * @brief SSL/TLS common types and enumerations.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_SSL_UVCPP_SSL_COMMON_H
#define SRC_SSL_UVCPP_SSL_COMMON_H

#if UVCPP_OPENSSL_ENABLE

#include <cstdint>
#include <string>

namespace uvcpp {

// =========================================================================
// TLS protocol versions
// =========================================================================

enum class tls_version : uint8_t {
  TLS_1_0 = 0,   // Deprecated — insecure
  TLS_1_1 = 1,   // Deprecated — insecure
  TLS_1_2 = 2,   // Minimum recommended
  TLS_1_3 = 3,   // Latest (default)
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

enum class tls_verify_mode : uint8_t {
  NONE     = 0,  // Don't verify peer certificate
  PEER     = 1,  // Verify peer certificate
  PEER_STRICT = 2,  // Verify peer + hostname
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

}  // namespace uvcpp

#endif  // UVCPP_OPENSSL_ENABLE
#endif  // SRC_SSL_UVCPP_SSL_COMMON_H
