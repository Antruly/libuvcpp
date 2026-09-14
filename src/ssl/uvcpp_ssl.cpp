/**
 * @file src/ssl/uvcpp_ssl.cpp
 * @brief Per-connection SSL/TLS wrapper implementation.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <ssl/uvcpp_ssl.h>
#include <ssl/uvcpp_ssl_context.h>

#if UVCPP_OPENSSL_ENABLE

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/bio.h>   // BIO_s_mem / BIO_ctrl（内存 BIO 模式）
#include <cstring>

namespace uvcpp {

void uvcpp_ssl::clear_error() {
  last_error_.clear();
  char buf[256];
  unsigned long e;
  while ((e = ERR_get_error()) != 0) {
    ERR_error_string_n(e, buf, sizeof(buf));
    if (!last_error_.empty()) last_error_ += "; ";
    last_error_ += buf;
  }
}

uvcpp_ssl::uvcpp_ssl(uvcpp_ssl_context* ctx, int fd) : ctx_(ctx) {
  if (ctx && ctx->raw_ctx()) {
    ssl_ = SSL_new(ctx->raw_ctx());
    if (ssl_ && fd > 0) SSL_set_fd(ssl_, fd);
  }
}

uvcpp_ssl::~uvcpp_ssl() {
  if (ssl_) { SSL_free(ssl_); ssl_ = nullptr; }
}

void uvcpp_ssl::set_fd(int fd) {
  if (ssl_) SSL_set_fd(ssl_, fd);
}

int uvcpp_ssl::handshake() {
  if (!ssl_) return -1;
  int rc = (ctx_ && ctx_->get_mode() == tls_mode::CLIENT)
               ? SSL_connect(ssl_)
               : SSL_accept(ssl_);
  if (rc == 1) { handshake_done_ = true; last_ssl_error_ = SSL_ERROR_NONE; return 1; }
  int err = SSL_get_error(ssl_, rc);
  last_ssl_error_ = err;
  // WANT_READ / WANT_WRITE 都只是「还没完」。**不要**看 rc 的符号下判断：
  // SSL_do_handshake 在这种情形下同样返回 -1。
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;
  clear_error(); return -1;
}

int uvcpp_ssl::write(const char* data, size_t len) {
  if (!ssl_ || len == 0) return -1;
  int rc = SSL_write(ssl_, data, static_cast<int>(len));
  if (rc > 0) { last_ssl_error_ = SSL_ERROR_NONE; return rc; }
  int err = SSL_get_error(ssl_, rc);
  last_ssl_error_ = err;
  if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) return 0;
  clear_error(); return -1;
}

int uvcpp_ssl::read(char* buf, size_t len) {
  if (!ssl_) return -1;
  int rc = SSL_read(ssl_, buf, static_cast<int>(len));
  if (rc > 0) { last_ssl_error_ = SSL_ERROR_NONE; return rc; }
  int err = SSL_get_error(ssl_, rc);
  last_ssl_error_ = err;
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;
  if (err == SSL_ERROR_ZERO_RETURN) return 0; // clean shutdown
  clear_error(); return -1;
}

// =========================================================================
// Memory-BIO mode
// =========================================================================

bool uvcpp_ssl::use_memory_bio() {
  if (!ssl_) return false;
  if (memory_bio_) return true;

  BIO* rbio = BIO_new(BIO_s_mem());
  BIO* wbio = BIO_new(BIO_s_mem());
  if (rbio == nullptr || wbio == nullptr) {
    if (rbio) BIO_free(rbio);
    if (wbio) BIO_free(wbio);
    clear_error();
    return false;
  }
  // SSL_set_bio 接管这两个 BIO 的所有权，并释放原先挂着的 socket BIO
  // （构造函数里 SSL_set_fd 装上的那个）。此后 SSL 不再碰任何 fd。
  SSL_set_bio(ssl_, rbio, wbio);
  memory_bio_ = true;
  return true;
}

size_t uvcpp_ssl::feed_ciphertext(const char* data, size_t len) {
  if (!ssl_ || data == nullptr || len == 0) return 0;
  BIO* rbio = SSL_get_rbio(ssl_);
  if (rbio == nullptr) return 0;
  int n = BIO_write(rbio, data, static_cast<int>(len));
  return n > 0 ? static_cast<size_t>(n) : 0;
}

size_t uvcpp_ssl::take_ciphertext(char* out, size_t len) {
  if (!ssl_ || out == nullptr || len == 0) return 0;
  BIO* wbio = SSL_get_wbio(ssl_);
  if (wbio == nullptr) return 0;
  int n = BIO_read(wbio, out, static_cast<int>(len));
  return n > 0 ? static_cast<size_t>(n) : 0;
}

size_t uvcpp_ssl::pending_ciphertext() const {
  if (!ssl_) return 0;
  BIO* wbio = SSL_get_wbio(ssl_);
  if (wbio == nullptr) return 0;
  size_t n = static_cast<size_t>(BIO_ctrl(wbio, BIO_CTRL_PENDING, 0, nullptr));
  return n;
}

size_t uvcpp_ssl::pending_plaintext() const {
  if (!ssl_) return 0;
  int n = SSL_pending(ssl_);
  return n > 0 ? static_cast<size_t>(n) : 0;
}

void uvcpp_ssl::shutdown() {
  if (ssl_) { SSL_shutdown(ssl_); handshake_done_ = false; }
}

tls_cert_info uvcpp_ssl::get_peer_cert_info() const {
  tls_cert_info info;
  if (!ssl_) return info;
  X509* cert = SSL_get_peer_certificate(ssl_);
  if (!cert) return info;

  char buf[512] = {0};
  const X509_NAME* subj = X509_get_subject_name(cert);
  if (subj) { X509_NAME_oneline(subj, buf, sizeof(buf)); info.subject = buf; }

  const X509_NAME* iss = X509_get_issuer_name(cert);
  if (iss) { memset(buf,0,sizeof(buf)); X509_NAME_oneline(iss, buf, sizeof(buf)); info.issuer = buf; }

  const ASN1_TIME* nb = X509_get0_notBefore(cert);
  if (nb) { BIO* b = BIO_new(BIO_s_mem()); ASN1_TIME_print(b, nb); int n = BIO_read(b,buf,sizeof(buf)-1); if(n>0) info.not_before=std::string(buf,n); BIO_free(b); }
  const ASN1_TIME* na = X509_get0_notAfter(cert);
  if (na) { BIO* b = BIO_new(BIO_s_mem()); ASN1_TIME_print(b, na); int n = BIO_read(b,buf,sizeof(buf)-1); if(n>0) info.not_after=std::string(buf,n); BIO_free(b); }

  unsigned char md[EVP_MAX_MD_SIZE]; unsigned int mdlen=0;
  if (X509_digest(cert, EVP_sha256(), md, &mdlen) == 1) {
    for (unsigned int i=0;i<mdlen;i++) { char h[3]; snprintf(h,sizeof(h),"%02x",md[i]); info.fingerprint+=h; }
  }
  X509_free(cert);
  return info;
}

bool uvcpp_ssl::verify_peer() const {
  if (!ssl_) return false;
  long rc = SSL_get_verify_result(ssl_);
  return rc == X509_V_OK;
}

bool uvcpp_ssl::is_handshake_done() const { return handshake_done_; }
std::string uvcpp_ssl::get_last_error() const { return last_error_; }

}  // namespace uvcpp

#endif  // UVCPP_OPENSSL_ENABLE
