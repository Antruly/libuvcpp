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
  // SNI 表里的上下文是**本对象拥有**的（`new` 出来、挂在 `sni_ctxs_` 上）。
  // 先释放它们再释放默认的 `ctx_`：回调注册在 `ctx_` 上，而 `ctx_` 一 free，
  // 回调连同它的 arg 都不再被调用 —— 次序反过来也不会有问题，但先把"由本对象
  // 派生出来的东西"收掉更符合所有权链。
  for (std::map<std::string, uvcpp_ssl_context*>::iterator it =
           sni_ctxs_.begin();
       it != sni_ctxs_.end(); ++it) {
    delete it->second;
  }
  sni_ctxs_.clear();
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

std::string sni_lower(const std::string& s) {
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') {
      out[i] = static_cast<char>(out[i] + ('a' - 'A'));
    }
  }
  return out;
}

/// 一层标签的通配：`*.example.com` 命中 `a.example.com`，不命中
/// `a.b.example.com`（RFC 6125 §6.4.3 的"最左标签可以是通配"）。
bool sni_host_matches(const std::string& pattern, const std::string& host) {
  if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
    const std::string suffix = pattern.substr(1);  // ".example.com"
    if (host.size() <= suffix.size()) return false;
    if (host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0) {
      return false;
    }
    const std::string label = host.substr(0, host.size() - suffix.size());
    return !label.empty() && label.find('.') == std::string::npos;
  }
  return pattern == host;
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

bool uvcpp_ssl_context::load_ca_data(const std::string& pem) {
  if (!ctx_ || pem.empty()) return false;
  X509_STORE* store = SSL_CTX_get_cert_store(ctx_);
  if (store == nullptr) { clear_error(); return false; }

  BIO* bio = BIO_new_mem_buf(pem.c_str(), static_cast<int>(pem.size()));
  if (bio == nullptr) { clear_error(); return false; }

  bool any = false;
  for (;;) {
    // 与 `load_certificate_data()` 同一种读法：先读一张，再循环读剩下的；
    // 读到干净的文件尾会在错误栈上留 PEM_R_NO_START_LINE，最后统一清掉。
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    if (cert == nullptr) break;
    any = true;
    if (X509_STORE_add_cert(store, cert) != 1) {
      // 同一张证书装两次是**幂等成功**（`load_ca_file()` 对重复项也是静默
      // 去重）；其余错误（OOM、畸形 DER）如实失败。判据只能看 reason ——
      // `X509_STORE_add_cert` 失败时都返回 0，不区分。
      const unsigned long e = ERR_peek_last_error();
      if (ERR_GET_REASON(e) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
        X509_free(cert);
        BIO_free(bio);
        clear_error();
        return false;
      }
    }
    X509_free(cert);
  }
  BIO_free(bio);
  ERR_clear_error();  // 清掉"读到文件尾"留下的 PEM_R_NO_START_LINE
  return any;
}

// =========================================================================
// 导出（测试 / 证书固定）
// =========================================================================

bool uvcpp_ssl_context::certificate_pem(std::string& out) const {
  out.clear();
  if (ctx_ == nullptr) return false;
  X509* leaf = SSL_CTX_get0_certificate(ctx_);
  if (leaf == nullptr) return false;
  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr) return false;
  const bool ok = PEM_write_bio_X509(bio, leaf) == 1;
  if (ok) {
    char* data = nullptr;
    const long n = BIO_get_mem_data(bio, &data);
    if (data != nullptr && n > 0) out.assign(data, static_cast<size_t>(n));
  }
  BIO_free(bio);
  return ok && !out.empty();
}

bool uvcpp_ssl_context::private_key_pem(std::string& out) const {
  out.clear();
  if (ctx_ == nullptr) return false;
  EVP_PKEY* key = SSL_CTX_get0_privatekey(ctx_);
  if (key == nullptr) return false;
  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr) return false;
  const bool ok = PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0,
                                           nullptr, nullptr) == 1;
  if (ok) {
    char* data = nullptr;
    const long n = BIO_get_mem_data(bio, &data);
    if (data != nullptr && n > 0) out.assign(data, static_cast<size_t>(n));
  }
  BIO_free(bio);
  return ok && !out.empty();
}

// =========================================================================
// SNI（服务端虚拟主机 / 多域名证书）
// =========================================================================

int uvcpp_ssl_context::sni_select_cb(ssl_st* ssl, int* /*al*/, void* arg) {
  uvcpp_ssl_context* self = static_cast<uvcpp_ssl_context*>(arg);
  if (self == nullptr || ssl == nullptr) return SSL_TLSEXT_ERR_NOACK;

  const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (name == nullptr || *name == '\0') {
    // 没带 SNI（老客户端、按 IP 连）：用默认证书，不是错误。
    return SSL_TLSEXT_ERR_NOACK;
  }

  SSL_CTX* alt = self->find_sni_ctx(name);
  if (alt == nullptr) return SSL_TLSEXT_ERR_NOACK;

  // 把这条连接切到那张上下文上。证书、私钥、协议版本都随之生效；返回值是
  // 旧上下文，不需要（也不应该）释放它 —— 那由本对象拥有。
  if (SSL_set_SSL_CTX(ssl, alt) == nullptr) {
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  return SSL_TLSEXT_ERR_OK;
}

ssl_ctx_st* uvcpp_ssl_context::find_sni_ctx(const std::string& host) const {
  const std::string h = sni_lower(host);

  std::map<std::string, uvcpp_ssl_context*>::const_iterator it =
      sni_ctxs_.find(h);
  if (it != sni_ctxs_.end()) return it->second->raw_ctx();

  // 精确没命中再试通配。表很小（通常个位数），线性扫一遍就够。
  for (it = sni_ctxs_.begin(); it != sni_ctxs_.end(); ++it) {
    if (sni_host_matches(it->first, h)) return it->second->raw_ctx();
  }
  return nullptr;
}

bool uvcpp_ssl_context::add_sni_certificate_data(const std::string& host,
                                                 const std::string& cert_pem,
                                                 const std::string& key_pem) {
  if (mode_ != tls_mode::SERVER || ctx_ == nullptr) return false;
  if (host.empty()) return false;

  // **先建好新的、再换上去**：替换现有条目时，半路失败不能把旧的那一套弄坏
  // （旧上下文已经被正在跑的握手握着，改它等于给线上连接换证书）。
  uvcpp_ssl_context* alt = new uvcpp_ssl_context(tls_mode::SERVER, min_version_);
  if (!alt->is_ready() || !alt->load_certificate_data(cert_pem) ||
      !alt->load_private_key_data(key_pem) || !alt->check_private_key()) {
    delete alt;
    return false;
  }

  const std::string key = sni_lower(host);
  std::map<std::string, uvcpp_ssl_context*>::iterator it = sni_ctxs_.find(key);
  if (it != sni_ctxs_.end()) {
    delete it->second;
    it->second = alt;
  } else {
    sni_ctxs_.insert(std::make_pair(key, alt));
  }

  if (!sni_cb_installed_) {
    SSL_CTX_set_tlsext_servername_callback(ctx_, &uvcpp_ssl_context::sni_select_cb);
    SSL_CTX_set_tlsext_servername_arg(ctx_, this);
    sni_cb_installed_ = true;
  }
  return true;
}

bool uvcpp_ssl_context::add_sni_certificate_file(const std::string& host,
                                                 const std::string& cert_path,
                                                 const std::string& key_path) {
  if (mode_ != tls_mode::SERVER || ctx_ == nullptr) return false;
  if (host.empty()) return false;

  uvcpp_ssl_context* alt = new uvcpp_ssl_context(tls_mode::SERVER, min_version_);
  if (!alt->is_ready() || !alt->load_certificate_file(cert_path) ||
      !alt->load_private_key_file(key_path) || !alt->check_private_key()) {
    delete alt;
    return false;
  }

  const std::string key = sni_lower(host);
  std::map<std::string, uvcpp_ssl_context*>::iterator it = sni_ctxs_.find(key);
  if (it != sni_ctxs_.end()) {
    delete it->second;
    it->second = alt;
  } else {
    sni_ctxs_.insert(std::make_pair(key, alt));
  }

  if (!sni_cb_installed_) {
    SSL_CTX_set_tlsext_servername_callback(ctx_, &uvcpp_ssl_context::sni_select_cb);
    SSL_CTX_set_tlsext_servername_arg(ctx_, this);
    sni_cb_installed_ = true;
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

// 护住"改 ctx 上那份 ALPN 名单"的几行。两个理由都要它：
//
//   1. **客户端侧**：`SSL_CTX_set_alpn_protos` 内部是"free 掉 ctx 上旧的
//      `ext.alpn` 再 malloc 一份新的"。一个上下文被多个线程共用时，两次调用会
//      各自读到同一个旧指针、各 free 一次 —— `attempting double-free`。
//   2. **服务端侧**：`alpn_select_wire_` 是 `std::string`，写入侧（安装）与
//      读取侧（`has_alpn_select()` 那道闸门）都是并发发生的，而"谁先装"这件事
//      恰好就发生在多条连接同时建起来的那一刻。
//
// 不做成成员：`uvcpp_ssl_context` 是公开类，加一个 `std::mutex` 成员就改了它的
// 布局。这把锁只覆盖"改名单"这一小段，不进任何热路径，进程内一把足够。
std::mutex g_alpn_ctx_mutex;

std::mutex& alpn_ctx_mutex() {
  return g_alpn_ctx_mutex;
}

}  // namespace

bool uvcpp_ssl_context::set_alpn_protos(const std::vector<std::string>& protos) {
  if (!ctx_) return false;
  const std::string wire = ssl_detail::alpn_wire_format(protos);
  if (wire.empty()) return false;
  // 注意：SSL_CTX_set_alpn_protos **成功返回 0**，与 OpenSSL 大多数接口相反。
  //
  // 逐连接的名单请用 `apply_alpn_to_ssl()`：这条留的是 ctx 级语义，改的是共享
  // 状态。锁只保证它不再撕裂 OpenSSL 内部那一对 free/malloc，**不**表示"边握手
  // 边改名单"是安全的 —— 已经建出来的 `SSL` 继承的是建它那一刻的名单。
  std::lock_guard<std::mutex> guard(alpn_ctx_mutex());
  if (SSL_CTX_set_alpn_protos(ctx_,
                              reinterpret_cast<const unsigned char*>(wire.data()),
                              static_cast<unsigned int>(wire.size())) != 0) {
    clear_error();
    return false;
  }
  return true;
}

bool uvcpp_ssl_context::apply_alpn_to_ssl(ssl_st* ssl,
                                          const std::vector<std::string>& protos) {
  if (ssl == nullptr) return false;
  const std::string wire = ssl_detail::alpn_wire_format(protos);
  if (wire.empty()) return false;
  // 与 SSL_CTX_set_alpn_protos 同样：**成功返回 0**。不需要锁：写的是这条连接
  // 自己的 `SSL`，别的线程碰不到它。
  if (SSL_set_alpn_protos(ssl, reinterpret_cast<const unsigned char*>(wire.data()),
                          static_cast<unsigned int>(wire.size())) != 0) {
    clear_error();
    return false;
  }
  return true;
}

void uvcpp_ssl_context::set_alpn_select_protos(const std::vector<std::string>& protos) {
  if (!ctx_) return;
  const std::string wire = ssl_detail::alpn_wire_format(protos);
  std::lock_guard<std::mutex> guard(alpn_ctx_mutex());
  // 幂等：同一份名单直接返回。既省掉一次无谓的注册，也让"多条连接并发建起来"
  // 时只有一个线程真正动 ctx。空名单也要走完，因为它是"把回调摘掉"的语义。
  if (wire == alpn_select_wire_) return;
  alpn_select_wire_ = wire;
  if (alpn_select_wire_.empty()) {
    SSL_CTX_set_alpn_select_cb(ctx_, nullptr, nullptr);
    return;
  }
  SSL_CTX_set_alpn_select_cb(ctx_, alpn_select_cb, &alpn_select_wire_);
}

bool uvcpp_ssl_context::has_alpn_select() const {
  // 与写入侧同一把锁，理由见 alpn_ctx_mutex()。
  std::lock_guard<std::mutex> guard(alpn_ctx_mutex());
  return !alpn_select_wire_.empty();
}

// =========================================================================
// Status
// =========================================================================

bool uvcpp_ssl_context::is_ready() const { return status_ == TLS_CTX_READY; }
int uvcpp_ssl_context::get_status() const { return status_; }
std::string uvcpp_ssl_context::get_last_error() const { return last_error_; }

void uvcpp_ssl_context::set_max_early_data(uint32_t n) {
  if (ctx_ != nullptr) SSL_CTX_set_max_early_data(ctx_, n);
}

uint32_t uvcpp_ssl_context::max_early_data() const {
  return ctx_ != nullptr ? SSL_CTX_get_max_early_data(ctx_) : 0;
}

}  // namespace uvcpp

#endif  // UVCPP_OPENSSL_ENABLE
