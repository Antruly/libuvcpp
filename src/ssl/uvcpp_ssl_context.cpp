/**
 * @file src/ssl/uvcpp_ssl_context.cpp
 * @brief SSL/TLS context implementation wrapping OpenSSL SSL_CTX.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <ssl/uvcpp_ssl_context.h>

#if UVCPP_OPENSSL_ENABLE

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <cstring>
#include <mutex>

namespace uvcpp {

// =========================================================================
// One-time OpenSSL initialisation
// =========================================================================

static bool ssl_initialised = false;
static std::mutex ssl_init_mutex;

static void ensure_ssl_init() {
  std::lock_guard<std::mutex> lock(ssl_init_mutex);
  if (!ssl_initialised) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    ssl_initialised = true;
  }
}

void uvcpp_ssl_context::clear_error() {
  last_error_.clear();
  char buf[256];
  unsigned long err;
  while ((err = ERR_get_error()) != 0) {
    ERR_error_string_n(err, buf, sizeof(buf));
    if (!last_error_.empty()) last_error_ += "; ";
    last_error_ += buf;
  }
}

// =========================================================================
// Construction / Destruction
// =========================================================================

uvcpp_ssl_context::uvcpp_ssl_context(tls_mode mode, tls_version ver)
    : mode_(mode), min_version_(ver) {
  ensure_ssl_init();

  if (mode == tls_mode::CLIENT) init_client();
  else init_server();
}

uvcpp_ssl_context::~uvcpp_ssl_context() {
  if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
}

void uvcpp_ssl_context::init_client() {
  ctx_ = SSL_CTX_new(TLS_client_method());
  if (!ctx_) { clear_error(); status_ = TLS_CTX_ERROR; return; }
  set_default_verify();
  status_ = TLS_CTX_READY;
}

void uvcpp_ssl_context::init_server() {
  ctx_ = SSL_CTX_new(TLS_server_method());
  if (!ctx_) { clear_error(); status_ = TLS_CTX_ERROR; return; }
  set_default_verify();
  status_ = TLS_CTX_READY;
}

void uvcpp_ssl_context::set_default_verify() {
  SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
  long opts = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;
  // Disable TLS < min_version
  if (static_cast<int>(min_version_) > static_cast<int>(tls_version::TLS_1_0))
    opts |= SSL_OP_NO_TLSv1;
  if (static_cast<int>(min_version_) > static_cast<int>(tls_version::TLS_1_1))
    opts |= SSL_OP_NO_TLSv1_1;
  if (static_cast<int>(min_version_) > static_cast<int>(tls_version::TLS_1_2))
    opts |= SSL_OP_NO_TLSv1_2;
  SSL_CTX_set_options(ctx_, opts);
  SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
}

// =========================================================================
// Certificate / Key
// =========================================================================

bool uvcpp_ssl_context::load_certificate_file(const std::string& path) {
  if (!ctx_) return false;
  if (SSL_CTX_use_certificate_file(ctx_, path.c_str(), SSL_FILETYPE_PEM) != 1) {
    clear_error(); return false;
  }
  return true;
}

bool uvcpp_ssl_context::load_private_key_file(const std::string& path) {
  if (!ctx_) return false;
  if (SSL_CTX_use_PrivateKey_file(ctx_, path.c_str(), SSL_FILETYPE_PEM) != 1) {
    clear_error(); return false;
  }
  return SSL_CTX_check_private_key(ctx_) == 1;
}

bool uvcpp_ssl_context::load_certificate_data(const std::string& pem) {
  if (!ctx_ || pem.empty()) return false;
  BIO* bio = BIO_new_mem_buf(pem.c_str(), static_cast<int>(pem.size()));
  X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!cert) { clear_error(); return false; }
  int rc = SSL_CTX_use_certificate(ctx_, cert);
  X509_free(cert);
  return rc == 1;
}

bool uvcpp_ssl_context::load_private_key_data(const std::string& pem) {
  if (!ctx_ || pem.empty()) return false;
  BIO* bio = BIO_new_mem_buf(pem.c_str(), static_cast<int>(pem.size()));
  EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!key) { clear_error(); return false; }
  int rc = SSL_CTX_use_PrivateKey(ctx_, key);
  EVP_PKEY_free(key);
  return rc == 1 && SSL_CTX_check_private_key(ctx_) == 1;
}

bool uvcpp_ssl_context::generate_self_signed(const std::string& cn, int bits) {
  if (!ctx_) return false;

  // Generate RSA key via EVP_PKEY_keygen (OpenSSL 1.1.1+ / 3.x compatible,
  // avoids deprecated RSA_generate_key_ex / RSA_free / BN_*)
  EVP_PKEY* pkey = nullptr;
  EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  if (!pctx) { clear_error(); return false; }
  if (EVP_PKEY_keygen_init(pctx) <= 0 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, bits) <= 0 ||
      EVP_PKEY_keygen(pctx, &pkey) <= 0) {
    clear_error(); EVP_PKEY_CTX_free(pctx); return false;
  }
  EVP_PKEY_CTX_free(pctx);
  if (!pkey) { clear_error(); return false; }

  X509* x509 = X509_new();
  if (!x509) { clear_error(); EVP_PKEY_free(pkey); return false; }
  ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
  X509_gmtime_adj(X509_get_notBefore(x509), 0);
  X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);
  X509_set_pubkey(x509, pkey);

  X509_NAME* name = const_cast<X509_NAME*>(X509_get_subject_name(x509));
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                              reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
  X509_set_issuer_name(x509, name);
  if (X509_sign(x509, pkey, EVP_sha256()) == 0) {
    clear_error(); X509_free(x509); EVP_PKEY_free(pkey); return false;
  }

  int rc = SSL_CTX_use_certificate(ctx_, x509);
  if (rc != 1) { clear_error(); X509_free(x509); EVP_PKEY_free(pkey); return false; }
  rc = SSL_CTX_use_PrivateKey(ctx_, pkey);
  if (rc != 1) { clear_error(); X509_free(x509); EVP_PKEY_free(pkey); return false; }
  rc = SSL_CTX_check_private_key(ctx_);
  X509_free(x509);
  EVP_PKEY_free(pkey);
  if (rc != 1) { clear_error(); return false; }
  return true;
}

// =========================================================================
// CA / verification
// =========================================================================

bool uvcpp_ssl_context::load_ca_file(const std::string& path) {
  if (!ctx_) return false;
  if (SSL_CTX_load_verify_locations(ctx_, path.c_str(), nullptr) != 1) {
    clear_error(); return false;
  }
  return true;
}

void uvcpp_ssl_context::set_verify_mode(tls_verify_mode mode) {
  if (!ctx_) return;
  int mode_flags = SSL_VERIFY_NONE;
  if (mode == tls_verify_mode::PEER || mode == tls_verify_mode::PEER_STRICT)
    mode_flags = SSL_VERIFY_PEER;
  SSL_CTX_set_verify(ctx_, mode_flags, nullptr);
}

// =========================================================================
// Protocol
// =========================================================================

void uvcpp_ssl_context::set_min_version(tls_version ver) {
  min_version_ = ver;
  if (!ctx_) return;
  long opts = SSL_CTX_get_options(ctx_);
  opts |= SSL_OP_NO_TLSv1;
  if (static_cast<int>(ver) <= static_cast<int>(tls_version::TLS_1_0))
    opts &= ~SSL_OP_NO_TLSv1;
  if (static_cast<int>(ver) <= static_cast<int>(tls_version::TLS_1_1))
    opts &= ~SSL_OP_NO_TLSv1_1;
  if (static_cast<int>(ver) <= static_cast<int>(tls_version::TLS_1_2))
    opts &= ~SSL_OP_NO_TLSv1_2;
  SSL_CTX_set_options(ctx_, opts);
}

void uvcpp_ssl_context::set_max_version(tls_version ver) {
  if (!ctx_) return;
  if (static_cast<int>(ver) < static_cast<int>(tls_version::TLS_1_3))
    SSL_CTX_set_options(ctx_, SSL_CTX_get_options(ctx_) | SSL_OP_NO_TLSv1_3);
  if (static_cast<int>(ver) < static_cast<int>(tls_version::TLS_1_2))
    SSL_CTX_set_options(ctx_, SSL_CTX_get_options(ctx_) | SSL_OP_NO_TLSv1_2);
}

bool uvcpp_ssl_context::set_cipher_list(const std::string& ciphers) {
  if (!ctx_) return false;
  if (SSL_CTX_set_cipher_list(ctx_, ciphers.c_str()) != 1) {
    clear_error(); return false;
  }
  return true;
}

// =========================================================================
// Status
// =========================================================================

bool uvcpp_ssl_context::is_ready() const { return status_ == TLS_CTX_READY; }
int uvcpp_ssl_context::get_status() const { return status_; }
std::string uvcpp_ssl_context::get_last_error() const { return last_error_; }

}  // namespace uvcpp

#endif  // UVCPP_OPENSSL_ENABLE
