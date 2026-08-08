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
  if (rc == 1) { handshake_done_ = true; return 1; }
  int err = SSL_get_error(ssl_, rc);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;
  clear_error(); return -1;
}

int uvcpp_ssl::write(const char* data, size_t len) {
  if (!ssl_ || len == 0) return -1;
  int rc = SSL_write(ssl_, data, static_cast<int>(len));
  if (rc > 0) return rc;
  int err = SSL_get_error(ssl_, rc);
  if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) return 0;
  clear_error(); return -1;
}

int uvcpp_ssl::read(char* buf, size_t len) {
  if (!ssl_) return -1;
  int rc = SSL_read(ssl_, buf, static_cast<int>(len));
  if (rc > 0) return rc;
  int err = SSL_get_error(ssl_, rc);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;
  if (err == SSL_ERROR_ZERO_RETURN) return 0; // clean shutdown
  clear_error(); return -1;
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
