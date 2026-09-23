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

namespace {

/**
 * @brief `tls_version` 映射成 OpenSSL 的协议版本号（`TLS1_2_VERSION` 等）。
 *
 * **为什么不能再用 `SSL_OP_NO_TLSv1*` 那一组。** 两个原因叠在一起：
 *
 * 1. `SSL_CTX_set_options(ctx, op)` 是「**加上** op 里的位」，它**不会清**没给
 *    的位 —— 想清必须另外调 `SSL_CTX_clear_options()`。原来那几处
 *    `opts &= ~SSL_OP_NO_TLSv1_x` 之后再 `set_options(ctx, opts)`，那个清位
 *    是**白清的**：位从来没被去掉过。
 * 2. OpenSSL 3.0 起 `SSL_OP_NO_TLSv1*` 已被废弃，官方指定用
 *    `SSL_CTX_set_min_proto_version()` / `set_max_proto_version()`。
 *
 * 用版本号 API 之后，「设下限/设上限」是一次有明确语义的赋值，不再需要在
 * 位运算上辩方向。
 */
int openssl_version_of(tls_version ver) {
  switch (ver) {
    case tls_version::TLS_1_0: return TLS1_VERSION;
    case tls_version::TLS_1_1: return TLS1_1_VERSION;
    case tls_version::TLS_1_2: return TLS1_2_VERSION;
    case tls_version::TLS_1_3: return TLS1_3_VERSION;
  }
  return TLS1_2_VERSION;
}

}  // namespace

void uvcpp_ssl_context::set_default_verify() {
  // CLIENT 默认**校验**对端证书：一个不校验的 HTTPS 客户端会把任何中间人
  // 当成正常服务端。回环自签用例必须显式调 set_verify_mode(NONE) 关掉它 ——
  // 让"关掉校验"在调用点看得见，而不是靠一个全局的宽松默认值。
  // SERVER 默认不要求客户端证书（相互认证是 opt-in，与所有主流实现一致）。
  if (mode_ == tls_mode::CLIENT) {
    verify_mode_ = tls_verify_mode::PEER;
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(ctx_);
  } else {
    verify_mode_ = tls_verify_mode::NONE;
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
  }
  SSL_CTX_set_options(ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
  // 构造时给的下限也走版本号 API —— 与 set_min_version() 用同一条路，
  // 免得"构造时设的"和"运行中改的"是两套机制（其中一套还是无效的）。
  SSL_CTX_set_min_proto_version(ctx_, openssl_version_of(min_version_));
  SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
}

// =========================================================================
// Certificate / Key
// =========================================================================

bool uvcpp_ssl_context::load_certificate_file(const std::string& path) {
  if (!ctx_) return false;
  // 必须用 `_chain_file`，不能用 `_file`。
  //
  // `SSL_CTX_use_certificate_file()` 只读 PEM 里的**第一张**证书 —— 对它来说
  // "证书链"就是一条多余的尾巴。结果是中间证书永远不会发给对端，而只信任根 CA
  // 的客户端（浏览器与 curl 的默认状态）没法把链补全，握手直接失败：
  //
  //   verify error:num=20: unable to get local issuer certificate
  //   Verify return code: 21 (unable to verify the first certificate)
  //
  // 而本 API 的文档承诺的正是"cert_file 是证书**链**"（见 uvcpp_web_app.h
  // 的 enable_ssl 注释）。现实里几乎每张证书都挂在中间 CA 下面
  // （Let's Encrypt、DigiCert、企业 PKI），所以这个差别直接决定 HTTPS 能不能用。
  //
  // 注：只有"叶子由根直接签"的两级链是例外 —— 那种情况服务端本来就不该发根
  // （客户端自己有），只发叶子是正确的，改动对它没有影响。
  if (SSL_CTX_use_certificate_chain_file(ctx_, path.c_str()) != 1) {
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
  if (bio == nullptr) { clear_error(); return false; }

  // 与 load_certificate_file 是同一个问题：PEM 里可能不止一张证书。
  // 第一张是叶子，其余是中间证书 —— 后面那些必须显式挂到上下文上，否则
  // 客户端同样补不全链。`enable_ssl_pem()` 走的正是这条路径。
  //
  // 读法照搬 OpenSSL 自己 process_chain() 的做法：先读叶子，循环读剩下的，
  // 逐个 `SSL_CTX_add_extra_chain_cert()`（成功时所有权归 SSL_CTX）。
  // 读干净的 EOF 会在错误栈上留下 PEM_R_NO_START_LINE，那是正常结束不是错误，
  // 所以最后统一清一次。
  X509* leaf = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  if (leaf == nullptr) {
    BIO_free(bio);
    clear_error();
    return false;
  }
  const int rc = SSL_CTX_use_certificate(ctx_, leaf);
  X509_free(leaf);
  if (rc != 1) {
    BIO_free(bio);
    clear_error();
    return false;
  }

  for (;;) {
    X509* extra = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    if (extra == nullptr) break;
    if (SSL_CTX_add_extra_chain_cert(ctx_, extra) != 1) {
      BIO_free(bio);
      clear_error();
      return false;
    }
  }

  BIO_free(bio);
  ERR_clear_error();  // 清掉"读到文件尾"留下的 PEM_R_NO_START_LINE
  return true;
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

bool uvcpp_ssl_context::check_private_key() {
  if (!ctx_) return false;
  if (SSL_CTX_check_private_key(ctx_) != 1) { clear_error(); return false; }
  return true;
}

bool uvcpp_ssl_context::load_ca_file(const std::string& path) {
  if (!ctx_) return false;
  if (SSL_CTX_load_verify_locations(ctx_, path.c_str(), nullptr) != 1) {
    clear_error(); return false;
  }
  return true;
}

void uvcpp_ssl_context::set_verify_mode(tls_verify_mode mode) {
  // 先记档位，再谈 `ctx_`：`verify_mode()` 是"这条连接要不要钉主机名"的唯一依据，
  // 而建连接那一层读的是**记下来的档位**，不是 `SSL_CTX` 上的 verify 位（后者表达
  // 不出 `PEER_STRICT` 与 `PEER` 的区别）。`ctx_` 为空时早退会让档位悄悄留在旧值，
  // 所以这句必须在早退之前。
  verify_mode_ = mode;
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
  // 赋值语义：设成什么就是什么，**双向都生效**。
  SSL_CTX_set_min_proto_version(ctx_, openssl_version_of(ver));
}

void uvcpp_ssl_context::set_max_version(tls_version ver) {
  if (!ctx_) return;
  SSL_CTX_set_max_proto_version(ctx_, openssl_version_of(ver));
}

bool uvcpp_ssl_context::set_cipher_list(const std::string& ciphers) {
  if (!ctx_) return false;
  if (SSL_CTX_set_cipher_list(ctx_, ciphers.c_str()) != 1) {
    clear_error(); return false;
  }
  return true;
}

// =========================================================================
// ALPN
// =========================================================================

namespace {

// `SSL_select_next_proto` 在**无交集**时也会写 out/outlen（让它指向服务端列表的
// 第一项），所以返回值的语义不能想当然。只认 OPENSSL_NPN_NEGOTIATED，
// 否则等于把第一个协议名硬塞给对端 —— 对端要的是 h2、我们答 h2、但它本不支持，
// 后果比"没协商出 ALPN"严重得多。
int alpn_select_cb(SSL* /*ssl*/, const unsigned char** out, unsigned char* outlen,
                   const unsigned char* in, unsigned int inlen, void* arg) {
  const std::string* server = static_cast<const std::string*>(arg);
  if (!server || server->empty()) return SSL_TLSEXT_ERR_NOACK;
  unsigned char* sel = nullptr;
  unsigned char  sel_len = 0;
  int rv = SSL_select_next_proto(
      &sel, &sel_len, reinterpret_cast<const unsigned char*>(server->data()),
      static_cast<unsigned int>(server->size()), in, inlen);
  if (rv != OPENSSL_NPN_NEGOTIATED) return SSL_TLSEXT_ERR_NOACK;
  *out = sel;
  *outlen = sel_len;
  return SSL_TLSEXT_ERR_OK;
}

}  // namespace

bool uvcpp_ssl_context::set_alpn_protos(const std::vector<std::string>& protos) {
  if (!ctx_) return false;
  const std::string wire = ssl_detail::alpn_wire_format(protos);
  if (wire.empty()) return false;
  // 注意：SSL_CTX_set_alpn_protos **成功返回 0**，与 OpenSSL 大多数接口相反。
  if (SSL_CTX_set_alpn_protos(ctx_,
                              reinterpret_cast<const unsigned char*>(wire.data()),
                              static_cast<unsigned int>(wire.size())) != 0) {
    clear_error();
    return false;
  }
  return true;
}

void uvcpp_ssl_context::set_alpn_select_protos(const std::vector<std::string>& protos) {
  if (!ctx_) return;
  alpn_select_wire_ = ssl_detail::alpn_wire_format(protos);
  if (alpn_select_wire_.empty()) {
    SSL_CTX_set_alpn_select_cb(ctx_, nullptr, nullptr);
    return;
  }
  SSL_CTX_set_alpn_select_cb(ctx_, alpn_select_cb, &alpn_select_wire_);
}

// =========================================================================
// Status
// =========================================================================

bool uvcpp_ssl_context::is_ready() const { return status_ == TLS_CTX_READY; }
int uvcpp_ssl_context::get_status() const { return status_; }
std::string uvcpp_ssl_context::get_last_error() const { return last_error_; }

}  // namespace uvcpp

#endif  // UVCPP_OPENSSL_ENABLE
