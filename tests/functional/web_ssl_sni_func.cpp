/**
 * @file tests/functional/web_ssl_sni_func.cpp
 * @brief 服务端按 SNI 选证书（1.9.0 / 1.8.14-dev）。
 *
 * 判据是**客户端握手之后拿到的那张叶子证书**（DER 逐字节比较），不是"配置
 * 函数返回 true"：
 *
 * | # | 客户端发的 SNI | 期望的证书 |
 * |---|---|---|
 * | 1 | 不发（连 IP） | 默认证书 |
 * | 2 | `alpha.test` | `alpha.test` 那张（精确命中） |
 * | 3 | `a.wild.test` | `*.wild.test` 那张（一层通配） |
 * | 4 | `exact.wild.test` | 精确条目（精确优先于通配） |
 * | 5 | `unknown.test` | 默认证书（回落，不是握手失败） |
 *
 * 装置：真 `uvcpp_tcp_server` + TLS，跑在独立线程；客户端用
 * `uvcpp_tcp_client` 连 `127.0.0.1`，靠 `set_tls_server_name()` 发 SNI（连 IP
 * 而 SNI 是域名 —— 这正是这个覆盖口的用途）。证书由
 * `generate_self_signed()` 现生成、`certificate_pem()` 导出，不碰文件系统。
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_OPENSSL_ENABLE && UVCPP_WEB_ENABLE

#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>

#include <openssl/pem.h>
#include <openssl/x509.h>

#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "ok: " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << what << std::endl;
  }
}

std::vector<unsigned char> pem_to_der(const std::string& pem) {
  std::vector<unsigned char> out;
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr) return out;
  X509* x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  if (x != nullptr) {
    unsigned char* p = nullptr;
    const int n = i2d_X509(x, &p);
    if (n > 0 && p != nullptr) {
      out.assign(p, p + n);
      OPENSSL_free(p);
    }
    X509_free(x);
  }
  BIO_free(bio);
  return out;
}

/// 生成一张自签证书并把 PEM 导出来。
bool make_cert(const char* cn, std::string& cert_pem, std::string& key_pem) {
  uvcpp_ssl_context ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!ctx.is_ready()) return false;
  if (!ctx.generate_self_signed(cn, 2048)) return false;
  return ctx.certificate_pem(cert_pem) && ctx.private_key_pem(key_pem);
}

// =========================================================================
// 服务端
// =========================================================================

struct server_state {
  std::atomic<int> listen_rc{-1};
  std::atomic<int> accepted{0};
};

void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                server_state& st, uvcpp_ssl_context* ctx) {
  uvcpp_tcp_server server;
  server.set_ssl_context(ctx);
  server.set_read_callback([](uvcpp_tcp_client&, const net_read_result&) {});

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }

  sockaddr_in name;
  int         namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int bound_port = ntohs(name.sin_port);

  rc = server.listen([&st](uvcpp_tcp_client*) { st.accepted.fetch_add(1); }, 64);
  st.listen_rc.store(rc);
  port_promise.set_value(rc != 0 ? -1 : bound_port);
  if (rc != 0) return;

  uvcpp_loop* loop = server.get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  uvcpp_test::pump_for(loop, 300);
}

struct test_server {
  server_state      st;
  std::atomic<bool> stop{false};
  std::promise<int> port_promise;
  std::future<int>  port_future;
  std::thread       thread;
  int               port = -1;

  explicit test_server(uvcpp_ssl_context* ctx)
      : port_future(port_promise.get_future()) {
    thread = std::thread(run_server, std::ref(port_promise), std::ref(stop),
                         std::ref(st), ctx);
    port   = port_future.get();
  }

  void shutdown() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

/// 连一次、取叶子证书。`sni == nullptr` = 不发 SNI（覆盖口留空，连的是 IP）。
bool handshake_and_leaf(int port, const char* sni,
                        std::vector<unsigned char>& leaf) {
  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  cctx.set_verify_mode(tls_verify_mode::NONE);

  uvcpp_tcp_client c;
  if (c.enable_tls(&cctx) != 0) return false;
  if (sni != nullptr) c.set_tls_server_name(sni);
  if (c.connect_wait("127.0.0.1", port, 4000) != 0) return false;

  const std::vector<std::vector<unsigned char> > chain = c.peer_cert_chain_der();
  if (chain.empty()) return false;
  leaf = chain[0];
  return true;
}

void test_sni() {
  std::string def_cert, def_key;
  std::string alpha_cert, alpha_key;
  std::string wild_cert, wild_key;
  std::string exact_cert, exact_key;
  if (!make_cert("default.test", def_cert, def_key) ||
      !make_cert("alpha.test", alpha_cert, alpha_key) ||
      !make_cert("wild.test", wild_cert, wild_key) ||
      !make_cert("exact.wild.test", exact_cert, exact_key)) {
    check(false, "四张自签证书生成成功");
    return;
  }
  check(true, "四张自签证书生成成功");

  uvcpp_ssl_context ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!ctx.load_certificate_data(def_cert) ||
      !ctx.load_private_key_data(def_key) || !ctx.check_private_key()) {
    check(false, "默认证书装入服务端上下文");
    return;
  }
  check(ctx.add_sni_certificate_data("alpha.test", alpha_cert, alpha_key),
        "SNI 装入 alpha.test");
  check(ctx.add_sni_certificate_data("*.wild.test", wild_cert, wild_key),
        "SNI 装入 *.wild.test（通配）");
  check(ctx.add_sni_certificate_data("exact.wild.test", exact_cert, exact_key),
        "SNI 装入 exact.wild.test（精确，与通配并存）");
  check(ctx.sni_certificate_count() == 3,
        "SNI 表里有 3 条（实际 " +
            std::to_string(ctx.sni_certificate_count()) + "）");

  const std::vector<unsigned char> def_der   = pem_to_der(def_cert);
  const std::vector<unsigned char> alpha_der = pem_to_der(alpha_cert);
  const std::vector<unsigned char> wild_der  = pem_to_der(wild_cert);
  const std::vector<unsigned char> exact_der = pem_to_der(exact_cert);
  if (def_der.empty() || alpha_der.empty() || wild_der.empty() ||
      exact_der.empty()) {
    check(false, "PEM → DER 转换成功");
    return;
  }

  test_server srv(&ctx);
  check(srv.port > 0, "TLS 服务端起来了（端口 " + std::to_string(srv.port) + "）");
  if (srv.port <= 0) {
    srv.shutdown();
    return;
  }

  const char* cases[][2] = {
      {nullptr, "默认证书"},
      {"alpha.test", "alpha.test（精确）"},
      {"a.wild.test", "*.wild.test（通配）"},
      {"exact.wild.test", "exact.wild.test（精确优先于通配）"},
      {"unknown.test", "默认证书（未知名字回落）"},
  };
  const std::vector<unsigned char>* want[] = {
      &def_der, &alpha_der, &wild_der, &exact_der, &def_der};

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    std::vector<unsigned char> leaf;
    const bool ok = handshake_and_leaf(srv.port, cases[i][0], leaf);
    const std::string label = cases[i][0] == nullptr
                                  ? std::string("不发 SNI")
                                  : std::string("SNI=") + cases[i][0];
    check(ok, label + " 握手成功");
    if (!ok) continue;
    check(leaf == *want[i], label + " → " + cases[i][1]);
  }

  srv.shutdown();
}

}  // namespace

int main() {
  std::cout << "服务端 SNI 选证书" << std::endl;
  test_sni();
  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "all checks passed" << std::endl;
  return 0;
}

#else  // !(OpenSSL && web)

int main() {
  std::cout << "SKIP: OpenSSL / web disabled" << std::endl;
  return 0;
}

#endif
