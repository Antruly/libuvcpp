/**
 * @file tests/functional/web_ssl_func.cpp
 * @brief SSL/TLS module tests (UVCPP_OPENSSL_ENABLE=1 only)
 */
#include <iostream>
#include <cstring>
#include <string>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>
#include <ssl/uvcpp_ssl.h>
#include <openssl/ssl.h>   // SSL_ERROR_* 常量：光看返回值分不出「还没完」和「出错」
using namespace uvcpp;

// =========================================================================
// Test 1: Client SSL context creation
// =========================================================================
static bool test_client_context() {
  uvcpp_ssl_context ctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  if (!ctx.is_ready()) return false;
  if (ctx.get_status() != TLS_CTX_READY) return false;
  if (ctx.get_mode() != tls_mode::CLIENT) return false;
  return true;
}

// =========================================================================
// Test 2: Server SSL context creation
// =========================================================================
static bool test_server_context() {
  uvcpp_ssl_context ctx(tls_mode::SERVER, tls_version::TLS_1_2);
  if (!ctx.is_ready()) return false;
  return true;
}

// =========================================================================
// Test 3: Generate self-signed certificate
// =========================================================================
static bool test_self_signed_cert() {
  uvcpp_ssl_context ctx(tls_mode::SERVER, tls_version::TLS_1_2);
  if (!ctx.generate_self_signed("test.local", 2048)) {
    std::cout << "  [err] " << ctx.get_last_error() << std::endl;
    return false;
  }
  if (!ctx.is_ready()) return false;
  return true;
}

// =========================================================================
// Test 4: Reject malformed PEM input
//
// 这一条原本是个**恒真断言**：建一个没人用的 context，再断言 gen.is_ready() ——
// 而 gen 在 test_self_signed_cert 里就已经 ready 了，这里什么都没测。
// 改成真正的断言：非法输入必须被拒绝（而不是"看起来还能用"）。
// 注：generate_self_signed 产出的证书存在 SSL_CTX 里，没有导出 PEM 的接口，
// 所以「加载自签 PEM 再校验」这条正向路径暂时无法构造 —— 如实留白，不假装。
// =========================================================================
static bool test_reject_bad_pem() {
  uvcpp_ssl_context ctx(tls_mode::SERVER, tls_version::TLS_1_2);
  if (ctx.load_certificate_data("this is not a PEM certificate")) return false;
  if (ctx.load_private_key_data("this is not a PEM key")) return false;
  // 空串也必须被拒（原实现对空串直接 return false，这里把它钉住）
  if (ctx.load_certificate_data("")) return false;
  if (ctx.load_private_key_data("")) return false;
  // 拒绝之后 context 本身必须仍然可用
  return ctx.is_ready();
}

// =========================================================================
// Test 5: SSL object creation and basic operations
// =========================================================================
static bool test_ssl_object() {
  uvcpp_ssl_context ctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  uvcpp_ssl ssl(&ctx, 0);  // fd=0, no real socket
  if (ssl.raw_ssl() == nullptr) return false;
  if (ssl.is_handshake_done()) return false;  // no handshake done yet

  // set_fd should work without crashing
  ssl.set_fd(0);
  return true;
}

// =========================================================================
// Test 6: TLS version configuration
// =========================================================================
static bool test_tls_versions() {
  uvcpp_ssl_context ctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  ctx.set_min_version(tls_version::TLS_1_2);
  ctx.set_max_version(tls_version::TLS_1_3);
  if (!ctx.is_ready()) return false;

  // Older TLS 1.0/1.1 context should still create
  uvcpp_ssl_context ctx2(tls_mode::SERVER, tls_version::TLS_1_0);
  if (!ctx2.is_ready()) return false;
  return true;
}

// =========================================================================
// Test 7: Verify mode and cipher configuration
// =========================================================================
static bool test_verify_and_cipher() {
  uvcpp_ssl_context ctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  ctx.set_verify_mode(tls_verify_mode::NONE);
  if (!ctx.set_cipher_list("HIGH:!aNULL")) return false;  // may fail if unsupported
  // Even if cipher list fails, context should still be ready
  if (!ctx.is_ready()) return false;
  return true;
}

// =========================================================================
// Test 8: Multi-context independence
// =========================================================================
static bool test_multi_context() {
  uvcpp_ssl_context srv(tls_mode::SERVER);
  uvcpp_ssl_context cli(tls_mode::CLIENT);
  if (!srv.is_ready() || !cli.is_ready()) return false;
  if (srv.get_mode() != tls_mode::SERVER) return false;
  if (cli.get_mode() != tls_mode::CLIENT) return false;
  return true;
}

// =========================================================================
// Memory-BIO 模式：真握手 + 真往返
//
// 本文件原来的 8 组全是「构造成功」级别的断言，从头到尾**没有一次握手**，
// 所以「握手成功之后发明文」这类缺陷能一直躺着（详见计划文件 Phase 4b 的
// 前置发现）。下面这几组补上真正的东西：两端各一个 uvcpp_ssl，挂内存 BIO，
// 由泵循环在两者之间搬密文 —— 真实实现里这一步由 libuv 的读写回调驱动，
// 但推进逻辑与之完全相同。
// =========================================================================

/// 在两端之间搬一次密文，返回是否搬动了字节。
static bool pump_ciphertext(uvcpp_ssl& from, uvcpp_ssl& to) {
  char buf[16384];
  size_t n;
  bool moved = false;
  while ((n = from.take_ciphertext(buf, sizeof(buf))) > 0) {
    to.feed_ciphertext(buf, n);
    moved = true;
  }
  return moved;
}

/// 驱动一次完整握手。
///
/// **判据是 `< 0`，不是 `!= 1`。** `handshake()` 返回 0 表示 WANT_READ /
/// WANT_WRITE —— 即「还没完」。把它当成失败，就会让每一个**正常**的握手都
/// 被判成失败；`uvcpp_ws_client` 的 `wss://` 路径（:165-166）正是这么写的，
/// 于是它在非阻塞 socket 上永远握不上手。
static bool drive_handshake(uvcpp_ssl& cli, uvcpp_ssl& srv, int& iters) {
  for (iters = 0; iters < 200; ++iters) {
    if (cli.is_handshake_done() && srv.is_handshake_done()) return true;
    if (!cli.is_handshake_done() && cli.handshake() < 0) return false;
    if (!srv.is_handshake_done() && srv.handshake() < 0) return false;
    const bool a = pump_ciphertext(cli, srv);
    const bool b = pump_ciphertext(srv, cli);
    if (!a && !b) return false;   // 双向都没字节可搬 = 真死锁
  }
  return false;
}

/// 一对配好的内存 BIO 端点（服务端带进程内自签证书）。
struct tls_pair {
  uvcpp_ssl_context sctx;
  uvcpp_ssl_context cctx;
  uvcpp_ssl*        srv;
  uvcpp_ssl*        cli;
  bool              ok;

  /// `disable_verify=false` 保留 CLIENT 的**默认**校验（PEER）—— 第 14 组
  /// 需要它来验证"默认就是校验的"，其余各组按回环自签的前提关掉校验。
  explicit tls_pair(bool disable_verify = true)
      : sctx(tls_mode::SERVER, tls_version::TLS_1_2),
        cctx(tls_mode::CLIENT, tls_version::TLS_1_2),
        srv(nullptr), cli(nullptr), ok(false) {
    if (!sctx.is_ready() || !cctx.is_ready()) return;
    // 自签证书 + 无信任锚 ⇒ 必须显式关掉校验（客户端默认是 PEER）
    if (disable_verify) cctx.set_verify_mode(tls_verify_mode::NONE);
    if (!sctx.generate_self_signed("loopback.test", 2048)) return;
    // fd=0：不装 socket BIO，直接走内存 BIO（socket 由调用方/libuv 独占）
    srv = new uvcpp_ssl(&sctx, 0);
    cli = new uvcpp_ssl(&cctx, 0);
    ok = srv->use_memory_bio() && cli->use_memory_bio();
  }
  ~tls_pair() { delete cli; delete srv; }
  tls_pair(const tls_pair&) = delete;
  tls_pair& operator=(const tls_pair&) = delete;
};

// 9. 内存 BIO 下握手真的能完成
static bool test_memory_bio_handshake() {
  tls_pair p;
  if (!p.ok) { std::cout << "    pair setup failed\n"; return false; }
  int iters = 0;
  if (!drive_handshake(*p.cli, *p.srv, iters)) {
    std::cout << "    handshake did not complete in " << iters << " iters\n";
    return false;
  }
  if (!p.cli->is_handshake_done() || !p.srv->is_handshake_done()) return false;
  // 握手完成后两边都不该再有待发密文（都被泵搬走了）
  if (p.cli->pending_ciphertext() != 0 || p.srv->pending_ciphertext() != 0) {
    std::cout << "    ciphertext left unsent after handshake\n";
    return false;
  }
  return true;
}

// 10. 握手之后明文能真的过去（这是原来完全没有的覆盖）
static bool test_memory_bio_roundtrip() {
  tls_pair p;
  if (!p.ok) return false;
  int iters = 0;
  if (!drive_handshake(*p.cli, *p.srv, iters)) return false;

  const std::string msg = "GET / HTTP/1.1\r\nHost: loopback.test\r\n\r\n";
  size_t sent = 0;
  while (sent < msg.size()) {
    int n = p.cli->write(msg.data() + sent, msg.size() - sent);
    if (n <= 0) { std::cout << "    SSL_write returned " << n << "\n"; return false; }
    sent += static_cast<size_t>(n);
  }
  if (!pump_ciphertext(*p.cli, *p.srv)) {
    std::cout << "    SSL_write produced no ciphertext\n";
    return false;
  }
  std::string got;
  char buf[4096];
  while (got.size() < msg.size()) {
    int rn = p.srv->read(buf, sizeof(buf));
    if (rn <= 0) break;
    got.append(buf, static_cast<size_t>(rn));
  }
  if (got != msg) {
    std::cout << "    roundtrip mismatch: got " << got.size()
              << " bytes, expected " << msg.size() << "\n";
    return false;
  }
  return true;
}

// 11. 反向对照：明文喂进 TLS 必须**报错**，不能被当成正常数据收下
//
// 这条专门用来钉死「TLS 层只是个摆设」的实现 —— 如果过滤器没接上，
// 明文会被原样收下，这条就 FAIL。
static bool test_plaintext_rejected_by_tls() {
  tls_pair p;
  if (!p.ok) return false;
  int iters = 0;
  if (!drive_handshake(*p.cli, *p.srv, iters)) return false;

  const std::string plain = "GET / HTTP/1.1\r\nHost: loopback.test\r\n\r\n";
  p.srv->feed_ciphertext(plain.data(), plain.size());
  char buf[4096];
  int rn = p.srv->read(buf, sizeof(buf));
  if (rn > 0) {
    std::cout << "    plaintext accepted as TLS data (" << rn << " bytes)\n";
    return false;
  }
  // 必须是真错误，而不是「等更多数据」
  if (p.srv->last_ssl_error() == SSL_ERROR_WANT_READ) {
    std::cout << "    plaintext neither parsed nor rejected (WANT_READ)\n";
    return false;
  }
  return true;
}

// 12. 「还没完」不等于「出错」—— 把本层最容易写错的一处钉住
static bool test_want_read_is_not_error() {
  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  if (!cctx.is_ready()) return false;
  uvcpp_ssl cli(&cctx, 0);
  if (!cli.use_memory_bio()) return false;

  // 第一次推进：应当生成 ClientHello 并停在 WANT_READ
  int rc = cli.handshake();
  if (rc != 0) {
    std::cout << "    fresh handshake returned " << rc << " (expect 0)\n";
    return false;
  }
  if (cli.last_ssl_error() != SSL_ERROR_WANT_READ) {
    std::cout << "    last_ssl_error=" << cli.last_ssl_error()
              << " (expect WANT_READ=" << SSL_ERROR_WANT_READ << ")\n";
    return false;
  }
  if (cli.is_handshake_done()) return false;
  if (cli.pending_ciphertext() == 0) {
    std::cout << "    no ClientHello produced\n";
    return false;
  }
  return true;
}

// 13. take_ciphertext 必须能搬空，且搬空后再取仍是 0
static bool test_take_ciphertext_drains() {
  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  if (!cctx.is_ready()) return false;
  uvcpp_ssl cli(&cctx, 0);
  if (!cli.use_memory_bio()) return false;
  cli.handshake();
  if (cli.pending_ciphertext() == 0) return false;

  char buf[64];
  size_t total = 0;
  size_t n;
  while ((n = cli.take_ciphertext(buf, sizeof(buf))) > 0) total += n;
  if (cli.pending_ciphertext() != 0) {
    std::cout << "    pending_ciphertext not drained\n";
    return false;
  }
  if (cli.take_ciphertext(buf, sizeof(buf)) != 0) return false;
  // ClientHello 至少几十字节；远小于此说明搬运逻辑有问题
  if (total < 40) {
    std::cout << "    suspiciously small ClientHello: " << total << " bytes\n";
    return false;
  }
  return true;
}

// 14. CLIENT 默认**校验**对端证书 —— 对进程内自签证书必须握不上手
//
// 缺陷（全清单里唯一的安全级问题）：`set_default_verify()` 对 CLIENT 模式也设
// `SSL_VERIFY_NONE`，于是 HTTPS 客户端默认接受**任何**证书 —— 中间人可以直接
// 接管。回环自签用例需要宽松的默认值，但那是**用例**的需要，不是库的默认。
//
// 判据是一对，缺一条都不成立：
//   (a) 默认（PEER）客户端对自签服务端握手**必须失败**；
//   (b) 同一个服务端 + 显式 `set_verify_mode(NONE)` 的客户端**必须成功**。
// 只有 (a) 的话，"把握手整个弄坏"也能通过；(b) 才是那个对照，它同时排除了
// "失败是因为别的原因"。
static bool test_client_default_rejects_untrusted_cert() {
  {
    tls_pair p(/*disable_verify=*/false);
    if (!p.ok) { std::cout << "    pair setup failed\n"; return false; }
    int iters = 0;
    if (drive_handshake(*p.cli, *p.srv, iters)) {
      std::cout << "    默认校验的客户端与自签证书握手成功了（iters=" << iters
                << "）—— 客户端证书校验没生效\n";
      return false;
    }
    if (p.cli->is_handshake_done()) {
      std::cout << "    校验失败但客户端握手仍是完成态\n";
      return false;
    }
  }
  {
    tls_pair p(/*disable_verify=*/true);
    if (!p.ok) { std::cout << "    control pair setup failed\n"; return false; }
    int iters = 0;
    if (!drive_handshake(*p.cli, *p.srv, iters)) {
      std::cout << "    对照组（verify=NONE）也握不上手（iters=" << iters
                << "）—— 对照不成立，本条判据无效\n";
      return false;
    }
  }
  return true;
}

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"client_context", test_client_context},
    {"server_context", test_server_context},
    {"self_signed_cert", test_self_signed_cert},
    {"reject_bad_pem", test_reject_bad_pem},
    {"ssl_object", test_ssl_object},
    {"tls_versions", test_tls_versions},
    {"verify_and_cipher", test_verify_and_cipher},
    {"multi_context", test_multi_context},
    // 真握手 / 真往返 —— 见上面那段注释：前 8 组一次握手都没有
    {"memory_bio_handshake", test_memory_bio_handshake},
    {"memory_bio_roundtrip", test_memory_bio_roundtrip},
    {"plaintext_rejected_by_tls", test_plaintext_rejected_by_tls},
    {"want_read_is_not_error", test_want_read_is_not_error},
    {"take_ciphertext_drains", test_take_ciphertext_drains},
    {"client_default_rejects_untrusted_cert",
     test_client_default_rejects_untrusted_cert},
  };
  for (const auto& t : tests) {
    std::cout << "[web_ssl] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_ssl] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}
#else
int main() {
  std::cout << "[web_ssl] SKIP (SSL disabled)\n";
  return 0;
}
#endif
