/**
 * @file src/ssl/uvcpp_ssl_context.h
 * @brief SSL/TLS context — wraps OpenSSL SSL_CTX.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Manages SSL_CTX lifecycle, certificate/key loading, protocol
 * configuration, and verification settings.  One context can be shared
 * across multiple connections.
 */

#pragma once
#ifndef SRC_SSL_UVCPP_SSL_CONTEXT_H
#define SRC_SSL_UVCPP_SSL_CONTEXT_H

#if UVCPP_OPENSSL_ENABLE

#include <functional>
#include <string>
#include <uvcpp/uvcpp_define.h>
#include <ssl/uvcpp_ssl_common.h>

// Forward declarations for OpenSSL types (defined in .cpp via <openssl/ssl.h>)
struct ssl_ctx_st;

namespace uvcpp {

class UVCPP_API uvcpp_ssl_context {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_ssl_context)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_ssl_context)

  explicit uvcpp_ssl_context(tls_mode mode,
                              tls_version ver = tls_version::TLS_1_2);

  // -------------------------------------------------------------------
  // Certificate configuration
  // -------------------------------------------------------------------

  /** @brief Load certificate chain from PEM file. */
  bool load_certificate_file(const std::string& path);

  /** @brief Load private key from PEM file. */
  bool load_private_key_file(const std::string& path);

  /** @brief Load certificate chain from PEM string. */
  bool load_certificate_data(const std::string& pem);

  /** @brief Load private key from PEM string. */
  bool load_private_key_data(const std::string& pem);

  /**
   * @brief Generate a self-signed certificate (for testing).
   * @param common_name  CN field (e.g. "localhost").
   * @param bits         RSA key size (default 2048).
   */
  bool generate_self_signed(const std::string& common_name, int bits = 2048);

  // -------------------------------------------------------------------
  // CA / verification
  // -------------------------------------------------------------------

  /** @brief Load trusted CA certificates from PEM file. */
  bool load_ca_file(const std::string& path);

  /** @brief Set peer certificate verification mode. */
  void set_verify_mode(tls_verify_mode mode);

  // -------------------------------------------------------------------
  // Protocol
  // -------------------------------------------------------------------

  /** @brief Set allowed TLS protocol version range. */
  void set_min_version(tls_version ver);
  void set_max_version(tls_version ver);

  /** @brief Set cipher list (OpenSSL format, e.g. "HIGH:!aNULL"). */
  bool set_cipher_list(const std::string& ciphers);

  // -------------------------------------------------------------------
  // Status
  // -------------------------------------------------------------------

  bool is_ready() const;
  int get_status() const;
  std::string get_last_error() const;

  // -------------------------------------------------------------------
  // Internal access (for uvcpp_ssl)
  // -------------------------------------------------------------------
  ssl_ctx_st* raw_ctx() const { return ctx_; }
  tls_mode get_mode() const { return mode_; }

 private:
  void init_client();
  void init_server();
  void set_default_verify();
  void clear_error();

  ssl_ctx_st* ctx_ = nullptr;
  tls_mode    mode_;
  tls_version min_version_;
  int status_ = TLS_CTX_NONE;
  std::string last_error_;
};

}  // namespace uvcpp

#endif  // UVCPP_OPENSSL_ENABLE
#endif  // SRC_SSL_UVCPP_SSL_CONTEXT_H
