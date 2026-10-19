/**
 * @file tests/functional/web_ssl_client_timeout_func.cpp
 * @brief 阻塞 `*_wait` 路径的 `timeout_ms` 必须在 POSIX 上也生效（uvp#39）。
 *
 * 1.8.0 之前 `send_wait_*` 的 `SO_RCVTIMEO` / `SO_SNDTIMEO` 整段包在
 * `#ifdef _WIN32` 里：Linux/macOS 上 `timeout_ms` 被静默忽略，对端收下请求却
 * 不响应时，调用方无限等下去（对端关闭时还会拿到一个"成功的"空响应）。
 * TLS 握手同样没有上限：TCP 连上了但对端不发 ServerHello，`connect_wait`
 * 永远不返回。
 *
 * 三个场景（服务端是手写 POSIX socket，不与本库同源）：
 *   1. 纯 HTTP：对端收下请求后静默 3 s → `send_wait_plain(…, 800)` 必须在
 *      约 0.8 s 返回 `UV_ETIMEDOUT`，且失败响应不能挂着默认的 `200 OK`；
 *   2. TLS 握手：对端 accept 后一个字节都不发 → `connect_wait(…, 1200)`
 *      必须在约 1.2 s 返回 `UV_ETIMEDOUT`（旧实现会一直阻塞）；
 *   3. 正常响应仍是成功（防止把超时修成"什么都超时"）。
 *
 * Windows 走的是另一套 socket 调用（本用例用裸 POSIX socket），返回 3 由
 * ctest 记为 Skipped —— 大小写分支在同一个实现里，Linux/macOS 的绿足以覆盖
 * 这段逻辑。
 */

#include <web/uvcpp_http_client.h>
#include <ssl/uvcpp_ssl_context.h>
#include <uv.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32

int main() {
  std::printf("SKIP: this case drives raw POSIX sockets\n");
  return 3;
}

#else  // !_WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

enum server_mode { kSilent, kSilentAfterRequest, kRespondNormally };

struct server_ctx {
  int listen_fd = -1;
  server_mode mode = kSilent;
  int accepted = 0;
};

void* server_main(void* arg) {
  server_ctx* c = static_cast<server_ctx*>(arg);
  int fd = ::accept(c->listen_fd, nullptr, nullptr);
  if (fd < 0) return nullptr;
  ++c->accepted;

  if (c->mode == kSilentAfterRequest) {
    std::string req;
    while (req.find("\r\n\r\n") == std::string::npos) {
      char buf[2048];
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n <= 0) break;
      req.append(buf, static_cast<size_t>(n));
    }
  }

  if (c->mode == kRespondNormally) {
    std::string req;
    while (req.find("\r\n\r\n") == std::string::npos) {
      char buf[2048];
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n <= 0) break;
      req.append(buf, static_cast<size_t>(n));
    }
    const char resp[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
    ::send(fd, resp, sizeof(resp) - 1, 0);
  } else {
    // 静默：不握手、不回包，时间要长过客户端的超时，但别长到拖住 join。
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  }

  ::shutdown(fd, SHUT_RDWR);
  ::close(fd);
  return nullptr;
}

int listen_on_loopback(int& port_out) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct timeval tv;
  tv.tv_sec = 15;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 4) != 0) {
    ::close(fd);
    return -1;
  }
  socklen_t alen = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen);
  port_out = ntohs(addr.sin_port);
  return fd;
}

long long elapsed_ms(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

int failures = 0;
void check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  } else {
    std::printf("ok: %s\n", what.c_str());
  }
}

void test_plain_read_timeout() {
  server_ctx sc;
  sc.mode = kSilentAfterRequest;
  int port = 0;
  sc.listen_fd = listen_on_loopback(port);
  check(sc.listen_fd >= 0, "plain: server listening");
  if (sc.listen_fd < 0) return;

  pthread_t tid;
  pthread_create(&tid, nullptr, server_main, &sc);

  uvcpp::uvcpp_http_client client;
  int rc = client.connect_wait("127.0.0.1", port, 3000);
  check(rc == 0, "plain: connect_wait ok");

  uvcpp::uvcpp_http_response resp;
  const auto t0 = std::chrono::steady_clock::now();
  rc = client.send_wait_plain(uvcpp::uvcpp_http_request::make_get("/"), resp, 800);
  const long long ms = elapsed_ms(t0);

  check(rc == UV_ETIMEDOUT,
        "plain: send_wait_plain returns UV_ETIMEDOUT (got " +
            std::to_string(rc) + ")");
  check(ms >= 500 && ms <= 2500,
        "plain: timeout honoured in wall clock (" + std::to_string(ms) + " ms)");
  check(resp.status_code == uvcpp::HTTP_STATUS_NONE,
        "plain: failed response is not 200 OK");

  pthread_join(tid, nullptr);
  ::close(sc.listen_fd);
}

void test_tls_handshake_timeout() {
  server_ctx sc;
  sc.mode = kSilent;
  int port = 0;
  sc.listen_fd = listen_on_loopback(port);
  check(sc.listen_fd >= 0, "tls: server listening");
  if (sc.listen_fd < 0) return;

  pthread_t tid;
  pthread_create(&tid, nullptr, server_main, &sc);

  uvcpp::uvcpp_ssl_context ctx(uvcpp::tls_mode::CLIENT);
  ctx.set_verify_mode(uvcpp::tls_verify_mode::NONE);
  uvcpp::uvcpp_http_client client;
  client.set_ssl_context(&ctx);

  const auto t0 = std::chrono::steady_clock::now();
  const int rc = client.connect_wait("127.0.0.1", port, 1200);
  const long long ms = elapsed_ms(t0);

  check(rc == UV_ETIMEDOUT,
        "tls: connect_wait returns UV_ETIMEDOUT (got " +
            std::to_string(rc) + ")");
  check(ms >= 800 && ms <= 3000,
        "tls: handshake timeout honoured (" + std::to_string(ms) + " ms)");

  pthread_join(tid, nullptr);
  ::close(sc.listen_fd);
}

void test_normal_exchange() {
  server_ctx sc;
  sc.mode = kRespondNormally;
  int port = 0;
  sc.listen_fd = listen_on_loopback(port);
  check(sc.listen_fd >= 0, "normal: server listening");
  if (sc.listen_fd < 0) return;

  pthread_t tid;
  pthread_create(&tid, nullptr, server_main, &sc);

  uvcpp::uvcpp_http_client client;
  int rc = client.connect_wait("127.0.0.1", port, 3000);
  check(rc == 0, "normal: connect_wait ok");

  uvcpp::uvcpp_http_response resp;
  rc = client.send_wait_plain(uvcpp::uvcpp_http_request::make_get("/"), resp,
                              3000);
  check(rc == 0, "normal: send_wait_plain ok");
  check(resp.status_code == uvcpp::http_status::OK, "normal: status 200");
  check(resp.body.to_string() == "hello", "normal: body delivered");

  pthread_join(tid, nullptr);
  ::close(sc.listen_fd);
}

}  // namespace

int main() {
  test_plain_read_timeout();
  test_tls_handshake_timeout();
  test_normal_exchange();
  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}

#endif  // _WIN32
