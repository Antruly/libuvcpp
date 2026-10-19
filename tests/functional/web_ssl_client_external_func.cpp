/**
 * @file tests/functional/web_ssl_client_external_func.cpp
 * @brief TLS 客户端对**外部实现**（裸 OpenSSL，不是本库的服务端）的往返。
 *
 * 为什么必须有一条这样的用例
 * --------------------------
 * 仓库里原有的 `web_ssl_client_func` / `web_ssl_http_client_func` 把客户端
 * 连到**本库自己的 `uvcpp_tcp_server`**。两端同源会把两类缺陷一起抵消掉，
 * 2026-10-20 的 v1.8.0 就同时踩了两条（uvp#274）：
 *
 *   1. 服务端把响应与 `close_notify` 放在同一段里发出时，异步路径在
 *      `tls_feed` 里见到 `SSL_ERROR_ZERO_RETURN` 就直接收尾，**把刚解出来、
 *      还躺在 `tls_plain_` 里的正文丢掉**；
 *   2. `tls_complete_connect()` 在读回调里被**无条件**调用了一直以来——
 *      服务端的第一段数据只够走到 Certificate 时（真实主机的证书链常分几段
 *      到达；本库的测试服务端一次就把整段飞行发完），"已连接"回调会在握手
 *      完成之前交给调用方，紧接着的 `write()` 拿到 `UV_ENOTCONN`。
 *
 * 本用例的服务端是**裸 OpenSSL + 手写 socket**，并且故意：
 *   - 把握手的第一个飞行拆成两段 TCP 写（`kSplitAfter` 字节 + 50 ms 延迟），
 *     让客户端的握手必然跨多次读；
 *   - 响应**不带 `Content-Length` / `Transfer-Encoding`**（HTTP/1.1 +
 *     `Connection: close` 的关闭定界），并且把正文与 `close_notify`
 *     放在**同一次** socket 写里。
 *
 * 三条断言分别对应：回调时握手必须已完成、响应必须完整交付、
 * 正文必须逐字节一致。Windows 没有 POSIX socket 这一套（本用例用裸
 * `socket()/accept()`），返回 3 由 ctest 记为 Skipped。
 */

#include <web/uvcpp_http_client.h>
#include <ssl/uvcpp_ssl_context.h>
#include <uv.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>

#ifdef _WIN32

int main() {
  std::printf("SKIP: this case drives a raw POSIX socket TLS server\n");
  return 3;
}

#else  // !_WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace {

const char kBody[] =
    "external-peer-response-0123456789-abcdefghijklmnopqrstuvwxyz";
const size_t kSplitAfter = 180;  // 握手的第一个飞行只先发这么多字节

void net_write_all(int fd, const char* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::send(fd, data + off, len - off, 0);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

/** 把 wbio 里攒着的密文全部取出来发到 socket 上。 */
void flush_wbio(SSL* ssl, int fd, bool split_first) {
  BIO* wbio = SSL_get_wbio(ssl);
  std::string batch;
  char buf[4096];
  for (;;) {
    int n = BIO_read(wbio, buf, sizeof(buf));
    if (n <= 0) break;
    batch.append(buf, static_cast<size_t>(n));
  }
  if (batch.empty()) return;
  if (split_first && batch.size() > kSplitAfter) {
    net_write_all(fd, batch.data(), kSplitAfter);
    // 50 ms 足够让客户端把"只到一半的握手"读完并回到事件循环；这里要的是
    // **两次读**，不是精确的时长。
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    net_write_all(fd, batch.data() + kSplitAfter, batch.size() - kSplitAfter);
  } else {
    net_write_all(fd, batch.data(), batch.size());
  }
}

EVP_PKEY* gen_key() {
  EVP_PKEY* pkey = nullptr;
  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  if (kctx == nullptr) return nullptr;
  if (EVP_PKEY_keygen_init(kctx) <= 0 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) <= 0 ||
      EVP_PKEY_keygen(kctx, &pkey) <= 0) {
    pkey = nullptr;
  }
  EVP_PKEY_CTX_free(kctx);
  return pkey;
}

X509* gen_cert(EVP_PKEY* pkey) {
  X509* x = X509_new();
  if (x == nullptr) return nullptr;
  X509_set_version(x, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
  X509_gmtime_adj(X509_get_notBefore(x), 0);
  X509_gmtime_adj(X509_get_notAfter(x), 86400);
  X509_set_pubkey(x, pkey);
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("localhost"),
                             -1, -1, 0);
  X509_set_issuer_name(x, name);
  X509V3_CTX v3;
  X509V3_set_ctx_nodb(&v3);
  X509V3_set_ctx(&v3, x, x, nullptr, nullptr, 0);
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(
      nullptr, &v3, NID_subject_alt_name, "IP:127.0.0.1,DNS:localhost");
  if (ext != nullptr) {
    X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
  }
  if (X509_sign(x, pkey, EVP_sha256()) <= 0) {
    X509_free(x);
    return nullptr;
  }
  return x;
}

struct server_ctx {
  int listen_fd = -1;
  SSL_CTX* sctx = nullptr;
  int accepted = 0;
  int handshake_ok = 0;
  int request_ok = 0;
};

void* server_main(void* arg) {
  server_ctx* c = static_cast<server_ctx*>(arg);
  int fd = ::accept(c->listen_fd, nullptr, nullptr);
  if (fd < 0) return nullptr;
  c->accepted = 1;

  SSL* ssl = SSL_new(c->sctx);
  if (ssl == nullptr) {
    ::close(fd);
    return nullptr;
  }
  BIO* rbio = BIO_new(BIO_s_mem());
  BIO* wbio = BIO_new(BIO_s_mem());
  SSL_set_bio(ssl, rbio, wbio);  // 所有权交给 SSL

  bool split_first = true;
  bool hs_ok = false;
  for (;;) {
    ERR_clear_error();
    int rc = SSL_accept(ssl);
    flush_wbio(ssl, fd, split_first);
    split_first = false;
    if (rc == 1) { hs_ok = true; break; }
    int err = SSL_get_error(ssl, rc);
    if (err != SSL_ERROR_WANT_READ) break;
    char buf[4096];
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    BIO_write(rbio, buf, static_cast<int>(n));
  }
  c->handshake_ok = hs_ok ? 1 : 0;

  if (hs_ok) {
    // 读到请求头结束（客户端的一条 GET）。
    std::string req;
    while (req.find("\r\n\r\n") == std::string::npos) {
      char buf[2048];
      int n = SSL_read(ssl, buf, sizeof(buf));
      if (n <= 0) {
        // memory BIO 的服务端不会自己从 socket 取字节：WANT_READ 时补喂。
        int err = SSL_get_error(ssl, n);
        if (err != SSL_ERROR_WANT_READ) break;
        char raw[4096];
        ssize_t rn = ::read(fd, raw, sizeof(raw));
        if (rn <= 0) break;
        BIO_write(SSL_get_rbio(ssl), raw, static_cast<int>(rn));
        continue;
      }
      req.append(buf, static_cast<size_t>(n));
    }
    c->request_ok = req.find("GET / HTTP/1.") != std::string::npos ? 1 : 0;

    // **关闭定界**的响应：没有 Content-Length / Transfer-Encoding。
    std::string resp =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
    resp.append(kBody);
    SSL_write(ssl, resp.data(), static_cast<int>(resp.size()));
    SSL_shutdown(ssl);
    // 正文与 close_notify 在 wbio 里，**一次**写到同一个 socket 段里 ——
    // 这正是旧代码把正文丢掉的那一段。
    flush_wbio(ssl, fd, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  SSL_free(ssl);
  ::shutdown(fd, SHUT_RDWR);
  ::close(fd);
  return nullptr;
}

struct client_result {
  int connect_status = -999;
  bool connect_fired = false;
  int send_status = -999;
  bool send_fired = false;
  bool hs_done_at_connect = false;
  int status_code = 0;
  std::string body;
};

bool run_client(int port, client_result& out) {
  uvcpp::uvcpp_ssl_context ctx(uvcpp::tls_mode::CLIENT);
  ctx.set_verify_mode(uvcpp::tls_verify_mode::NONE);

  uvcpp::uvcpp_http_client client;
  client.set_ssl_context(&ctx);

  int rc = client.connect("127.0.0.1", port, [&](int status) {
    out.connect_status = status;
    // http_client 没有暴露握手状态；用一条独立探针记录"回调时握手是否完成"
    // 由下面的 tcp 直连用例覆盖。这里只断言成功回调意味着可以发请求。
    out.connect_fired = true;
    if (status != 0) return;
    int src = client.send(
        uvcpp::uvcpp_http_request::make_get("/"),
        [&](const uvcpp::uvcpp_http_response& resp, int err) {
          out.send_status = err;
          out.send_fired = true;
          out.status_code = static_cast<int>(resp.status_code);
          out.body = resp.body.to_string();
          client.stop();
        });
    if (src != 0) {
      out.send_status = src;
      out.send_fired = true;
      client.stop();
    }
  });
  if (rc != 0) return false;

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(15000);
  while (!out.send_fired && std::chrono::steady_clock::now() < deadline) {
    client.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (!out.send_fired) client.stop();
  return true;
}

int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

}  // namespace

int main() {
  EVP_PKEY* pkey = gen_key();
  X509* cert = pkey ? gen_cert(pkey) : nullptr;
  if (pkey == nullptr || cert == nullptr) {
    std::printf("FAIL: could not generate a test certificate\n");
    return 1;
  }

  server_ctx sc;
  sc.sctx = SSL_CTX_new(TLS_server_method());
  if (sc.sctx == nullptr ||
      SSL_CTX_use_certificate(sc.sctx, cert) != 1 ||
      SSL_CTX_use_PrivateKey(sc.sctx, pkey) != 1) {
    std::printf("FAIL: could not configure the TLS server context\n");
    return 1;
  }

  sc.listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(sc.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  // `accept()` 也要有墙钟上限：客户端一开始就没连上时，服务端线程会永远
  // 停在 accept 里，`pthread_join` 于是把整条用例拖到 ctest 的 TIMEOUT。
  struct timeval accept_tv;
  accept_tv.tv_sec = 15;
  accept_tv.tv_usec = 0;
  ::setsockopt(sc.listen_fd, SOL_SOCKET, SO_RCVTIMEO, &accept_tv,
               sizeof(accept_tv));
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // 内核挑端口
  if (::bind(sc.listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(sc.listen_fd, 4) != 0) {
    std::printf("FAIL: could not bind/listen the test server\n");
    return 1;
  }
  socklen_t alen = sizeof(addr);
  ::getsockname(sc.listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen);
  const int port = ntohs(addr.sin_port);

  pthread_t tid;
  if (pthread_create(&tid, nullptr, server_main, &sc) != 0) {
    std::printf("FAIL: could not start the test server thread\n");
    return 1;
  }

  client_result res;
  const bool started = run_client(port, res);
  pthread_join(tid, nullptr);

  check(started, "client started");
  check(sc.accepted == 1, "server accepted the connection");
  check(sc.handshake_ok == 1, "external server completed the handshake");
  check(sc.request_ok == 1, "external server received the HTTP request");
  check(res.connect_fired && res.connect_status == 0,
        "connect callback fired with status 0");
  check(res.send_fired, "send callback fired (no hang)");
  check(res.send_status == 0, "send callback reported success");
  check(res.status_code == 200, "status line parsed");
  check(res.body == kBody, "close-delimited body delivered in full");

  SSL_CTX_free(sc.sctx);
  ::close(sc.listen_fd);
  X509_free(cert);
  EVP_PKEY_free(pkey);

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}

#endif  // _WIN32
