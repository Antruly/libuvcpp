/**
 * @file tests/functional/web_http_client_stream_func.cpp
 * @brief HTTP/1.1 客户端流式响应（#51）：分块交付、背压、取消、与小响应一致。
 *
 * `send()` 只在完整响应收完后回调，大响应只能整包攒内存。`send_stream()` 把
 * 头与 body 分开交付，body 按到达的块回调，且：
 *
 * 1. 小响应：流式拼起来的 body 与 `send()` 的完整 body 逐字节一致；
 * 2. Content-Length 4 MiB：body 按块到达，总长与 FNV 哈希与源一致；
 * 3. `on_body` 返回 false 会**停读 socket**（背压），`resume()` 之后把暂停
 *    期间攒下的那一块补投、再继续读，最终仍是全部字节；
 * 4. chunked 2 MiB：分块框架被剥掉，交付的是解码后的 body；
 * 5. `cancel_stream()`：`on_end(UV_ECANCELED)` 恰好一次，连接关掉，之后
 *    `send()` 拿到 `UV_ENOTCONN`。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <uv.h>

#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

#if UVCPP_WEB_ENABLE

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++g_fail;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  } else {
    std::fprintf(stderr, "ok: %s\n", what.c_str());
  }
}

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string make_body(size_t n) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>('a' + (i % 26));
  return s;
}

uint64_t fnv1a(const char* p, size_t n, uint64_t h = 1469598103934665603ULL) {
  for (size_t i = 0; i < n; ++i) {
    h ^= static_cast<unsigned char>(p[i]);
    h *= 1099511628211ULL;
  }
  return h;
}

// ---------------------------------------------------------------------------
// 服务端：一条连接一个请求；三种响应形状
// ---------------------------------------------------------------------------
struct stream_server {
  enum mode { FIXED, CHUNKED, HOLD };

  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> peer_closed{false};
  std::thread       thread;
  int               port = 0;
  mode              md = FIXED;
  size_t            body_size = 0;
  std::string       body;

  std::mutex                mu;
  std::set<uvcpp_tcp_client*> responded;

  void start(mode m, size_t n) {
    md = m;
    body_size = n;
    body = make_body(n);
    std::thread t([this]() { run(); });
    thread = std::move(t);
    while (!ready.load() && port >= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void run() {
    uvcpp_tcp_server srv;
    if (srv.bindIpv4("127.0.0.1", 0) != 0) {
      port = -1;
      ready.store(true);
      return;
    }
    struct sockaddr_in name;
    int len = static_cast<int>(sizeof(name));
    if (srv.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &len) !=
        0) {
      port = -1;
      ready.store(true);
      return;
    }
    port = ntohs(name.sin_port);
    srv.set_read_callback([this](uvcpp_tcp_client& c, const net_read_result& r) {
      if (r.event == net_read_event::PEER_CLOSED ||
          r.event == net_read_event::READ_ERROR) {
        peer_closed.store(true);
        return;
      }
      if (!r.is_data()) return;
      {
        std::lock_guard<std::mutex> lk(mu);
        if (!responded.insert(&c).second) return;
      }
      respond(c);
    });
    if (srv.listen([](uvcpp_tcp_client*) {}, 16) != 0) {
      port = -1;
      ready.store(true);
      return;
    }
    ready.store(true);
    while (!stop.load()) {
      srv.get_loop()->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int i = 0; i < 200; ++i) {
      srv.get_loop()->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void respond(uvcpp_tcp_client& c) {
    if (md == FIXED) {
      std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " +
                         std::to_string(body.size()) +
                         "\r\nConnection: close\r\n\r\n";
      resp += body;
      c.write(resp.data(), resp.size(), nullptr);
      return;
    }
    if (md == CHUNKED) {
      std::string resp =
          "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
          "Connection: close\r\n\r\n";
      const size_t kChunk = 16 * 1024;
      size_t off = 0;
      while (off < body.size()) {
        const size_t n = std::min(kChunk, body.size() - off);
        char head[32];
        std::snprintf(head, sizeof(head), "%zx\r\n", n);
        resp += head;
        resp.append(body, off, n);
        resp += "\r\n";
        off += n;
      }
      resp += "0\r\n\r\n";
      c.write(resp.data(), resp.size(), nullptr);
      return;
    }
    // HOLD：声明一个很大的 body，只发第一块，然后一直连着（等客户端取消）。
    std::string resp =
        "HTTP/1.1 200 OK\r\nContent-Length: 100000000\r\n"
        "Connection: close\r\n\r\n";
    resp.append(body.data(), 8 * 1024);
    c.write(resp.data(), resp.size(), nullptr);
  }

  ~stream_server() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// ---------------------------------------------------------------------------
// 客户端驱动
// ---------------------------------------------------------------------------
bool pump_until(uvcpp_http_client& cli, const std::function<bool()>& done,
                int timeout_ms = 10000) {
  const long long deadline = now_ms() + timeout_ms;
  while (!done()) {
    cli.run(UV_RUN_NOWAIT);
    if (now_ms() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

int connect_client(uvcpp_http_client& cli, int port) {
  int rc = cli.connect("127.0.0.1", port,
                       [](int status) { (void)status; });
  if (rc != 0) return rc;
  if (!pump_until(cli, [&] { return cli.has_status(HTTP_CLIENT_CONNECTED); })) {
    return UV_ETIMEDOUT;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// 1. 小响应：与 send() 一致
// ---------------------------------------------------------------------------
void test_small_parity() {
  stream_server srv;
  srv.start(stream_server::FIXED, 11);

  // (a) 完整响应
  uvcpp_http_client cli;
  check(connect_client(cli, srv.port) == 0, "[1] 完整响应：connect");
  std::string full_body;
  int full_status = -1;
  bool full_done = false;
  int rc = cli.send(uvcpp_http_request::make_get("/"),
                    [&](const uvcpp_http_response& r, int err) {
                      if (err == 0) {
                        full_body = r.body.to_string();
                        full_status = static_cast<int>(r.status_code);
                      }
                      full_done = true;
                    });
  check(rc == 0, "[1] send 受理");
  check(pump_until(cli, [&] { return full_done; }), "[1] send 回调落地");
  check(full_status == 200 && full_body == make_body(11),
        "[1] 完整响应内容");
  // 服务端回了 Connection: close，连接会自己收尾；析构再兜一次。

  // (b) 流式响应
  uvcpp_http_client scli;
  check(connect_client(scli, srv.port) == 0, "[1] 流式：connect");
  std::string stream_body;
  int stream_status = -1;
  int end_status = -999;
  int end_count = 0;
  bool headers_seen = false;
  rc = scli.send_stream(
      uvcpp_http_request::make_get("/"),
      [&](const uvcpp_http_response& head) {
        headers_seen = true;
        stream_status = static_cast<int>(head.status_code);
        check(head.body.size() == 0, "[1] 头回调时 body 为空");
      },
      [&](const char* d, size_t n) {
        stream_body.append(d, n);
        return true;
      },
      [&](int st) { end_status = st; ++end_count; });
  check(rc == 0, "[1] send_stream 受理");
  check(pump_until(scli, [&] { return end_count > 0; }),
        "[1] 流式 on_end 落地");
  check(headers_seen && stream_status == 200, "[1] 流式头");
  check(end_status == 0 && end_count == 1, "[1] on_end 恰好一次且为 0");
  check(stream_body == full_body, "[1] 流式 body 与 send() 逐字节一致");
  check(!scli.is_streaming(), "[1] 结束后不再是 streaming");
}

// ---------------------------------------------------------------------------
// 2. Content-Length 4 MiB + 背压（暂停一次再恢复）
// ---------------------------------------------------------------------------
void test_backpressure_large() {
  const size_t kN = 4u * 1024u * 1024u;
  stream_server srv;
  srv.start(stream_server::FIXED, kN);

  uvcpp_http_client cli;
  check(connect_client(cli, srv.port) == 0, "[2] connect");
  size_t got = 0;
  uint64_t hash = 1469598103934665603ULL;
  int end_status = -999;
  int end_count = 0;
  bool paused_once = false;
  const auto t0 = now_ms();
  long long paused_at = 0;
  long long resumed_at = 0;

  const int rc = cli.send_stream(
      uvcpp_http_request::make_get("/"), nullptr,
      [&](const char* d, size_t n) {
        got += n;
        hash = fnv1a(d, n, hash);
        if (!paused_once && got >= 64 * 1024) {
          // 第一块之后暂停：本层应停读 socket，第二块进 pending。
          paused_once = true;
          paused_at = now_ms();
          return false;
        }
        return true;
      },
      [&](int st) { end_status = st; ++end_count; });
  check(rc == 0, "[2] send_stream 受理");
  check(pump_until(cli, [&] { return paused_once; }), "[2] 第一块到达并暂停");
  check(cli.is_streaming(), "[2] 暂停期间仍是 streaming");

  // 暂停 120 ms 再恢复：如果背压没生效（继续读），这段窗口里 got 会继续涨。
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  const size_t at_pause = got;
  for (int i = 0; i < 30; ++i) {
    cli.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(got == at_pause, "[2] 暂停期间不再交付 body（真的停了 socket 读）");
  resumed_at = now_ms();
  check(cli.resume() == 0, "[2] resume 受理");

  check(pump_until(cli, [&] { return end_count > 0; }, 20000),
        "[2] on_end 落地");
  check(end_status == 0 && end_count == 1, "[2] on_end 恰好一次且为 0");
  check(got == kN, "[2] 收到全部 " + std::to_string(kN) + " 字节（实际 " +
                       std::to_string(got) + "）");
  const std::string expect = make_body(kN);
  check(hash == fnv1a(expect.data(), expect.size()),
        "[2] FNV 哈希与源一致");
  check(resumed_at - paused_at >= 100, "[2] 暂停窗口确实存在");
  (void)t0;
}

// ---------------------------------------------------------------------------
// 3. chunked 2 MiB
// ---------------------------------------------------------------------------
void test_chunked() {
  const size_t kN = 2u * 1024u * 1024u;
  stream_server srv;
  srv.start(stream_server::CHUNKED, kN);

  uvcpp_http_client cli;
  check(connect_client(cli, srv.port) == 0, "[3] connect");
  size_t got = 0;
  uint64_t hash = 1469598103934665603ULL;
  int end_status = -999;
  int blocks = 0;
  const int rc = cli.send_stream(
      uvcpp_http_request::make_get("/"), nullptr,
      [&](const char* d, size_t n) {
        ++blocks;
        got += n;
        hash = fnv1a(d, n, hash);
        return true;
      },
      [&](int st) { end_status = st; });
  check(rc == 0, "[3] send_stream 受理");
  check(pump_until(cli, [&] { return end_status != -999; }, 20000),
        "[3] on_end 落地");
  check(end_status == 0, "[3] on_end 为 0");
  check(got == kN, "[3] chunked body 总长（实际 " + std::to_string(got) + "）");
  const std::string expect = make_body(kN);
  check(hash == fnv1a(expect.data(), expect.size()), "[3] 哈希一致");
  check(blocks > 1, "[3] body 是按块交付的（不是一整包）");
}

// ---------------------------------------------------------------------------
// 4. cancel_stream：及时关连接、on_end 恰好一次
// ---------------------------------------------------------------------------
void test_cancel() {
  stream_server srv;
  srv.start(stream_server::HOLD, 8 * 1024);

  uvcpp_http_client cli;
  check(connect_client(cli, srv.port) == 0, "[4] connect");
  std::atomic<int> end_count{0};
  std::atomic<int> end_status{-999};
  std::atomic<size_t> got{0};
  bool first_chunk = false;
  const int rc = cli.send_stream(
      uvcpp_http_request::make_get("/"), nullptr,
      [&](const char* d, size_t n) {
        got.fetch_add(n);
        first_chunk = true;
        (void)d;
        return true;
      },
      [&](int st) {
        end_status.store(st);
        end_count.fetch_add(1);
      });
  check(rc == 0, "[4] send_stream 受理");
  check(pump_until(cli, [&] { return first_chunk; }), "[4] 收到第一块 body");
  check(cli.is_streaming(), "[4] 取消前是 streaming");

  cli.cancel_stream();
  check(!cli.is_streaming(), "[4] cancel 之后不再是 streaming");
  check(pump_until(cli, [&] { return end_count.load() > 0; }), "[4] on_end 落地");
  check(end_count.load() == 1, "[4] on_end 恰好一次");
  check(end_status.load() == UV_ECANCELED,
        "[4] on_end 状态是 UV_ECANCELED（实际 " +
            std::to_string(end_status.load()) + "）");
  check(!cli.has_status(HTTP_CLIENT_CONNECTED), "[4] 连接已关");
  const int src = cli.send(uvcpp_http_request::make_get("/"), nullptr);
  check(src == UV_ENOTCONN, "[4] 取消之后 send() 拿到 UV_ENOTCONN（实际 " +
                                std::to_string(src) + "）");
  (void)srv.peer_closed;
}

}  // namespace

int main() {
  std::fprintf(stderr, "HTTP client streaming response (#51)\n");
  test_small_parity();
  test_backpressure_large();
  test_chunked();
  test_cancel();
  if (g_fail != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fail);
    return 1;
  }
  std::fprintf(stderr, "all checks passed\n");
  return 0;
}

#else  // !UVCPP_WEB_ENABLE

int main() {
  std::printf("SKIP: web disabled\n");
  return 0;
}

#endif  // UVCPP_WEB_ENABLE
