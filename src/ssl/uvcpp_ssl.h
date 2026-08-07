/**
 * @file src/ssl/uvcpp_ssl.h
 * @brief Per-connection SSL/TLS wrapper (OpenSSL SSL*).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Wraps an SSL* from a uvcpp_ssl_context.  Provides handshake and
 * encrypt/decrypt I/O for the TCP stream.
 */

#pragma once
#ifndef SRC_SSL_UVCPP_SSL_H
#define SRC_SSL_UVCPP_SSL_H

#if UVCPP_OPENSSL_ENABLE

#include <functional>
#include <string>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <ssl/uvcpp_ssl_common.h>

struct ssl_st;
struct ssl_ctx_st;

namespace uvcpp {

class uvcpp_ssl_context;

class UVCPP_API uvcpp_ssl {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_ssl)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_ssl)

  explicit uvcpp_ssl(uvcpp_ssl_context* ctx, int fd = 0);

  // -------------------------------------------------------------------
  // I/O
  // -------------------------------------------------------------------

  /** @brief Set the underlying socket file descriptor. */
  void set_fd(int fd);

  /**
   * @brief Perform TLS handshake.
   * @return 1 on success, 0 on need-more-data, <0 on error.
   */
  int handshake();

  /**
   * @brief Encrypt and send data (internal buffer → SSL → socket).
   * @param data  Plaintext data to send.
   * @param len   Data length.
   * @return Number of bytes consumed from input, <0 on error.
   */
  int write(const char* data, size_t len);

  /**
   * @brief Receive and decrypt data (socket → SSL → internal buffer).
   * @param buf   Output buffer for decrypted plaintext.
   * @param len   Buffer size.
   * @return Number of bytes read into buf, 0 on need-more-data, <0 on error.
   */
  int read(char* buf, size_t len);

  /** @brief Perform a clean TLS shutdown. */
  void shutdown();

  // -------------------------------------------------------------------
  // Certificate info
  // -------------------------------------------------------------------

  /** @brief Get peer certificate information. */
  tls_cert_info get_peer_cert_info() const;

  /** @brief Verify peer certificate (returns true if valid). */
  bool verify_peer() const;

  // -------------------------------------------------------------------
  // Status
  // -------------------------------------------------------------------

  bool is_handshake_done() const;
  std::string get_last_error() const;
  ssl_st* raw_ssl() const { return ssl_; }

 private:
  void clear_error();

  ssl_st*            ssl_ = nullptr;
  uvcpp_ssl_context* ctx_ = nullptr;
  bool handshake_done_ = false;
  std::string last_error_;
};

}  // namespace uvcpp

#endif  // UVCPP_OPENSSL_ENABLE
#endif  // SRC_SSL_UVCPP_SSL_H
