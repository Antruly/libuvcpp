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
  // Memory-BIO mode (event-loop integration)
  // -------------------------------------------------------------------
  //
  // 默认走 SSL_set_fd —— OpenSSL 自己持有 socket 做读写。那在事件循环里不能用：
  // SSL 报 WANT_WRITE 时，「socket 何时可写」本端无从得知，只能 uv_poll（但同一个
  // fd 上不能再叠 uv_read_start）或者转圈问（违反「不在主循环做耗时操作」）。
  //
  // 换成内存 BIO 之后 SSL 只跟内存打交道，socket 完全由 libuv 独占，
  // WANT_WRITE 的含义随之变精确 —— **wbio 里有待发字节**，而这些字节什么时候
  // 发完，uv_write 的完成回调会告知。
  //
  // 典型推进循环：
  //   ssl.use_memory_bio();
  //   ssl.handshake();                  // ClientHello 落进 wbio
  //   while (n = ssl.take_ciphertext(b, sizeof b)) uv_write(b, n);
  //   ... 收到可读 → ssl.feed_ciphertext(d, n) → ssl.handshake() / ssl.read()
  //
  // 返回值契约（非阻塞下必须按这个判）：
  //   handshake(): 1 = 握手完成，0 = 需要更多 I/O，< 0 = 真出错
  //   read/write(): > 0 = 处理的字节数，0 = 需要更多 I/O，< 0 = 真出错
  //
  // 所以**判失败要用 `< 0`，不能用 `!= 1`**：0 是「还没完」，是常态，不是错误。
  // （OpenSSL 自己的 `SSL_do_handshake` 在 WANT_READ 时返回的是 -1，本类把它
  // 归一化成 0；直接拿 OpenSSL 的原返回值判断就会把正常握手当成失败。）
  // 需要区分 WANT_READ / WANT_WRITE / 对端干净关闭（ZERO_RETURN）时看
  // `last_ssl_error()`。

  /** @brief Switch to memory BIOs. Must be called before any handshake/I/O. */
  bool use_memory_bio();

  /** @brief True if the SSL object is in memory-BIO mode. */
  bool is_memory_bio() const { return memory_bio_; }

  /** @brief Feed ciphertext received from the socket. @return bytes consumed. */
  size_t feed_ciphertext(const char* data, size_t len);

  /** @brief Take ciphertext destined for the socket. @return bytes taken. */
  size_t take_ciphertext(char* out, size_t len);

  /** @brief Bytes waiting to be written to the socket (0 = nothing to send). */
  size_t pending_ciphertext() const;

  /** @brief Decrypted plaintext bytes already buffered and readable. */
  size_t pending_plaintext() const;

  /**
   * @brief The SSL_ERROR_* code from the most recent handshake/read/write.
   *
   * 用它区分「WANT_READ（等更多数据）」与「真的出错」，以及
   * `SSL_ERROR_ZERO_RETURN`（对端干净关闭）。只看返回值是分不出来的。
   */
  int last_ssl_error() const { return last_ssl_error_; }

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
  bool memory_bio_ = false;
  int  last_ssl_error_ = 0;
  std::string last_error_;
};

}  // namespace uvcpp

#endif  // UVCPP_OPENSSL_ENABLE
#endif  // SRC_SSL_UVCPP_SSL_H
