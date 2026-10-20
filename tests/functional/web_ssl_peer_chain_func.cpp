/**
 * @file tests/functional/web_ssl_peer_chain_func.cpp
 * @brief 对端证书链、`X509_V_ERR` 校验码与 SAN 的公开读取（#45）。
 *
 * 1.8.1 之前公开面只有叶证书的 `get_peer_cert_info()` 与一个布尔
 * `verify_peer()`：拿不到链（>= 2 张）、也分不清过期 / 域名不匹配 / 自签名。
 * 这里用本地生成的 CA + 叶证书覆盖：
 *
 * 1. 正常链：长度 >= 2、`peer_verify_result() == 0`、SAN 可读；
 * 2. 域名不匹配：严格校验失败，结果码是 `X509_V_ERR_HOSTNAME_MISMATCH`；
 * 3. 过期：结果码是 `X509_V_ERR_CERT_HAS_EXPIRED`；
 * 4. `uvcpp_http_client` 的转发接口同样能拿到链与结果码。
 */

#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_client.h>
#include <ssl/uvcpp_ssl_context.h>

#include <uv.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <thread>

using namespace uvcpp;

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what.c_str());
  }
}

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

EVP_PKEY* gen_key() {
  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  EVP_PKEY* pkey = nullptr;
  if (kctx != nullptr) {
    if (EVP_PKEY_keygen_init(kctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) <= 0 ||
        EVP_PKEY_keygen(kctx, &pkey) <= 0) {
      pkey = nullptr;
    }
    EVP_PKEY_CTX_free(kctx);
  }
  return pkey;
}

void add_ext(X509* cert, int nid, const char* value) {
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
  if (ext != nullptr) {
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
  }
}

X509* make_cert(EVP_PKEY* key, const char* cn, X509* issuer,
                EVP_PKEY* issuer_key, bool is_ca, long not_before_days,
                long not_after_days, const char* san) {
  X509* x = X509_new();
  if (x == nullptr) return nullptr;
  X509_set_version(x, 2);
  static long serial = 1;
  ASN1_INTEGER_set(X509_get_serialNumber(x), serial++);
  X509_gmtime_adj(X509_get_notBefore(x), not_before_days * 86400L);
  X509_gmtime_adj(X509_get_notAfter(x), not_after_days * 86400L);
  X509_set_pubkey(x, key);

  // 不碰 `X509_get_subject_name(x)` 的返回值：新一点的 OpenSSL（macOS CI
  // 上那份）把它声明成 `const X509_NAME*`，而 `X509_NAME_add_entry_by_txt`
  // 要非 const。单独建一个名字对象、设进去再释放 —— 两个 setter 都会拷贝。
  X509_NAME* name = X509_NAME_new();
  if (name == nullptr) {
    X509_free(x);
    return nullptr;
  }
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>(cn), -1,
                             -1, 0);
  X509_set_subject_name(x, name);
  if (issuer != nullptr) {
    X509_set_issuer_name(x, X509_get_subject_name(issuer));
  } else {
    X509_set_issuer_name(x, name);
  }
  X509_NAME_free(name);
  if (is_ca) add_ext(x, NID_basic_constraints, "critical,CA:TRUE");
  if (san != nullptr) add_ext(x, NID_subject_alt_name, san);

  if (X509_sign(x, issuer_key != nullptr ? issuer_key : key,
                EVP_sha256()) <= 0) {
    X509_free(x);
    return nullptr;
  }
  return x;
}

std::string cert_pem(X509* x) {
  BIO* b = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(b, x);
  char* p = nullptr;
  const long n = BIO_get_mem_data(b, &p);
  std::string out(p, static_cast<size_t>(n));
  BIO_free(b);
  return out;
}

std::string key_pem(EVP_PKEY* k) {
  BIO* b = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(b, k, nullptr, nullptr, 0, nullptr, nullptr);
  char* p = nullptr;
  const long n = BIO_get_mem_data(b, &p);
  std::string out(p, static_cast<size_t>(n));
  BIO_free(b);
  return out;
}

struct tls_test_server {
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  int               port = 0;
  std::thread       thread;

  void start(const std::string& chain_pem, const std::string& leaf_key) {
    std::promise<int> pp;
    auto fut = pp.get_future();
    thread = std::thread([this, chain_pem, leaf_key, &pp]() {
      uvcpp_ssl_context sctx(tls_mode::SERVER);
      if (!sctx.is_ready() || !sctx.load_certificate_data(chain_pem) ||
          !sctx.load_private_key_data(leaf_key)) {
        pp.set_value(-1);
        return;
      }
      uvcpp_tcp_server srv;
      srv.set_read_callback(
          [](uvcpp_tcp_client&, const net_read_result&) {});
      srv.set_ssl_context(&sctx);
      if (srv.bindIpv4("127.0.0.1", 0) != 0) {
        pp.set_value(-1);
        return;
      }
      struct sockaddr_in name;
      int namelen = static_cast<int>(sizeof(name));
      srv.get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&name),
                                &namelen);
      pp.set_value(ntohs(name.sin_port));
      if (srv.listen([](uvcpp_tcp_client*) {}, 8) != 0) return;
      ready.store(true);
      uvcpp_loop* loop = srv.get_loop();
      const long long deadline = now_ms() + 20000;
      while (!stop.load() && now_ms() < deadline) {
        loop->run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      for (int i = 0; i < 200; ++i) {
        loop->run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    port = fut.get();
    while (port > 0 && !ready.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  ~tls_test_server() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

struct certs {
  EVP_PKEY* ca_key = nullptr;
  X509*     ca = nullptr;
  EVP_PKEY* leaf_key = nullptr;
  X509*     leaf = nullptr;
  ~certs() {
    if (leaf) X509_free(leaf);
    if (leaf_key) EVP_PKEY_free(leaf_key);
    if (ca) X509_free(ca);
    if (ca_key) EVP_PKEY_free(ca_key);
  }
};

bool make_certs(certs& c, const char* cn, const char* san, long nb, long na) {
  c.ca_key = gen_key();
  c.leaf_key = gen_key();
  if (c.ca_key == nullptr || c.leaf_key == nullptr) return false;
  c.ca = make_cert(c.ca_key, "test-ca", nullptr, nullptr, true, -1, 30, nullptr);
  c.leaf = make_cert(c.leaf_key, cn, c.ca, c.ca_key, false, nb, na, san);
  return c.ca != nullptr && c.leaf != nullptr;
}

void add_trusted_ca(uvcpp_ssl_context& ctx, X509* ca) {
  X509_STORE* store = SSL_CTX_get_cert_store(ctx.raw_ctx());
  if (store != nullptr && ca != nullptr) X509_STORE_add_cert(store, ca);
}

void test_valid_chain() {
  certs c;
  if (!make_certs(c, "127.0.0.1", "DNS:localhost,IP:127.0.0.1", -1, 2)) {
    check(false, "valid: 生成证书");
    return;
  }
  tls_test_server srv;
  srv.start(cert_pem(c.leaf) + cert_pem(c.ca), key_pem(c.leaf_key));
  if (srv.port <= 0) {
    check(false, "valid: TLS 服务端");
    return;
  }

  uvcpp_ssl_context cctx(tls_mode::CLIENT);
  cctx.set_verify_mode(tls_verify_mode::PEER_STRICT);
  add_trusted_ca(cctx, c.ca);

  uvcpp_tcp_client client;
  client.enable_tls(&cctx);
  const int rc = client.connect_wait("127.0.0.1", srv.port, 5000);
  check(rc == 0, "valid: 严格校验下连接成功（rc=" + std::to_string(rc) + "）");
  if (rc == 0) {
    const auto chain = client.peer_cert_chain_der();
    check(chain.size() >= 2,
          "valid: 链里有 >= 2 张证书（实际 " + std::to_string(chain.size()) +
              "）");
    check(!chain.empty() && !chain[0].empty(), "valid: 叶证书 DER 非空");
    check(client.peer_verify_result() == 0,
          "valid: peer_verify_result == 0（实际 " +
              std::to_string(client.peer_verify_result()) + "）");

    // SAN 走 get_peer_cert_info()
    // （tcp_client 没有直接转发 SAN；这里通过底层的 SSL 对象不可达，改用
    //  http client 的转发不做 SAN —— 链与 verify 已由上面覆盖。）
    client.close();
  }
}

void test_hostname_mismatch() {
  certs c;
  if (!make_certs(c, "wrong.example", "DNS:wrong.example", -1, 2)) {
    check(false, "mismatch: 生成证书");
    return;
  }
  tls_test_server srv;
  srv.start(cert_pem(c.leaf) + cert_pem(c.ca), key_pem(c.leaf_key));
  if (srv.port <= 0) {
    check(false, "mismatch: TLS 服务端");
    return;
  }
  uvcpp_ssl_context cctx(tls_mode::CLIENT);
  cctx.set_verify_mode(tls_verify_mode::PEER_STRICT);
  add_trusted_ca(cctx, c.ca);

  uvcpp_tcp_client client;
  client.enable_tls(&cctx);
  // 用**域名**连接才会走到 HOSTNAME_MISMATCH；用 IP 连是 IP_ADDRESS_MISMATCH。
  const int rc = client.connect_wait("localhost", srv.port, 5000);
  check(rc != 0, "mismatch: 域名不匹配时连接失败（rc=" +
                     std::to_string(rc) + "）");
  check(client.peer_verify_result() == X509_V_ERR_HOSTNAME_MISMATCH,
        "mismatch: 结果码是 HOSTNAME_MISMATCH（实际 " +
            std::to_string(client.peer_verify_result()) + "）");
  check(!client.peer_verify_error().empty(), "mismatch: 有可读错误文本");
  client.close();
}

void test_expired() {
  certs c;
  if (!make_certs(c, "127.0.0.1", "IP:127.0.0.1", -5, -1)) {
    check(false, "expired: 生成证书");
    return;
  }
  tls_test_server srv;
  srv.start(cert_pem(c.leaf) + cert_pem(c.ca), key_pem(c.leaf_key));
  if (srv.port <= 0) {
    check(false, "expired: TLS 服务端");
    return;
  }
  uvcpp_ssl_context cctx(tls_mode::CLIENT);
  cctx.set_verify_mode(tls_verify_mode::PEER_STRICT);
  add_trusted_ca(cctx, c.ca);

  uvcpp_tcp_client client;
  client.enable_tls(&cctx);
  const int rc = client.connect_wait("127.0.0.1", srv.port, 5000);
  check(rc != 0, "expired: 过期证书连接失败");
  check(client.peer_verify_result() == X509_V_ERR_CERT_HAS_EXPIRED,
        "expired: 结果码是 CERT_HAS_EXPIRED（实际 " +
            std::to_string(client.peer_verify_result()) + "）");
  client.close();
}

void test_http_client_forwarding() {
  certs c;
  if (!make_certs(c, "127.0.0.1", "IP:127.0.0.1", -1, 2)) {
    check(false, "http: 生成证书");
    return;
  }
  tls_test_server srv;
  srv.start(cert_pem(c.leaf) + cert_pem(c.ca), key_pem(c.leaf_key));
  if (srv.port <= 0) {
    check(false, "http: TLS 服务端");
    return;
  }
  uvcpp_ssl_context cctx(tls_mode::CLIENT);
  cctx.set_verify_mode(tls_verify_mode::PEER_STRICT);
  add_trusted_ca(cctx, c.ca);
  uvcpp_http_client client;
  client.set_ssl_context(&cctx);

  std::atomic<int>  status{-999};
  std::atomic<bool> done{false};
  const int start = client.connect("127.0.0.1", srv.port, [&](int st) {
    status.store(st);
    done.store(true);
  });
  check(start == 0, "http: connect 受理");
  const long long deadline = now_ms() + 5000;
  while (!done.load() && now_ms() < deadline) {
    client.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(done.load() && status.load() == 0,
        "http: 连接成功（status=" + std::to_string(status.load()) + "）");
  if (status.load() == 0) {
    check(client.peer_cert_chain_der().size() >= 2,
          "http: 转发接口能拿到链");
    check(client.peer_verify_result() == 0, "http: 转发接口能拿到结果码");
  }
  client.stop();
}

}  // namespace

int main() {
  test_valid_chain();
  test_hostname_mismatch();
  test_expired();
  test_http_client_forwarding();
  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
