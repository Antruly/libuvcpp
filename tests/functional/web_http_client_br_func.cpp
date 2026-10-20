/**
 * @file tests/functional/web_http_client_br_func.cpp
 * @brief HTTP 客户端解 brotli，且解压门槛可配置（#47）。
 *
 * 1.8.1 之前客户端只发 `gzip, deflate`、只解 gzip/deflate，门槛写死 1024。
 * 这里覆盖：
 *
 * 1. 编了 brotli 的构建：`Accept-Encoding` 里带 `br`，`Content-Encoding: br`
 *    的响应被正确解开，两个头被摘掉；
 * 2. 门槛可配：默认 1024 下小于门槛的 body 原样保留；设成 0 后同样会被解；
 * 3. gzip 不回归；
 * 4. 调用方自己设过 `Accept-Encoding` 时不覆盖（由服务端回看请求头验证）。
 */

#include <web/uvcpp_http_client.h>

#if UVCPP_ZLIB_ENABLE && UVCPP_BROTLI_ENABLE

#include <web/uvcpp_http_compress.h>
#include <net/uvcpp_tcp_server.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
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

struct one_shot_server {
  std::atomic<bool>  ready{false};
  std::atomic<bool>  stop{false};
  std::atomic<bool>  answered{false};
  std::mutex         mu;
  std::string        last_request;
  std::string        response;
  std::thread        thread;
  int                port = 0;

  void start(std::string resp) {
    response = std::move(resp);
    std::promise<int> pp;
    auto fut = pp.get_future();
    thread = std::thread([this, &pp]() {
      uvcpp_tcp_server srv;
      srv.set_read_callback(
          [this](uvcpp_tcp_client& c, const net_read_result& r) {
            if (!r.is_data()) return;
            {
              std::lock_guard<std::mutex> lk(mu);
              last_request.append(r.data, r.size);
              if (last_request.find("\r\n\r\n") == std::string::npos) return;
            }
            c.write(response.data(), response.size(),
                    [this](int) { answered.store(true); });
          });
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
      const long long deadline = now_ms() + 15000;
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
  ~one_shot_server() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// 门槛比的是**压缩后**的长度：重复字符串压完常常只有几十字节，测不出
// "大于门槛"这条路，所以用伪随机字节（压缩不掉）。
std::string random_payload(size_t n) {
  std::string out;
  out.resize(n);
  unsigned x = 0x12345678u;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    out[i] = static_cast<char>(x & 0xff);
  }
  return out;
}

std::string make_response(http_compress_method method, const std::string& body,
                          bool include_content_encoding = true) {
  http_compress_result c = http_compress::compress(
      body.data(), body.size(), method, /*level=*/-1);
  if (!c.success) return std::string();
  std::string resp;
  resp += "HTTP/1.1 200 OK\r\n";
  resp += "Content-Length: " + std::to_string(c.data.size()) + "\r\n";
  if (include_content_encoding) {
    resp += std::string("Content-Encoding: ") +
            (method == http_compress_method::BROTLI ? "br" : "gzip") + "\r\n";
  }
  resp += "Connection: close\r\n\r\n";
  resp += c.data.to_string();
  return resp;
}

struct client_result {
  std::atomic<int>  connect_status{-999};
  std::atomic<int>  send_status{-999};
  std::atomic<bool> done{false};
  int               status_code = 0;
  std::string       body;
  std::string       headers_ce;
};

void run_client(int port, int min_body, bool set_accept_encoding,
                client_result& out) {
  uvcpp_http_client client;
  client.set_compression_enabled(true);
  client.set_compression_min_body_size(static_cast<size_t>(min_body));
  int rc = client.connect("127.0.0.1", port, [&](int st) {
    out.connect_status.store(st);
    if (st != 0) {
      out.done.store(true);
      return;
    }
    uvcpp_http_request req = uvcpp_http_request::make_get("/");
    if (set_accept_encoding) req.set_header("accept-encoding", "identity");
    int src = client.send(req, [&](const uvcpp_http_response& resp, int err) {
      out.send_status.store(err);
      out.status_code = static_cast<int>(resp.status_code);
      out.body = resp.body.to_string();
      out.headers_ce = resp.get_header("content-encoding");
      out.done.store(true);
      client.stop();
    });
    if (src != 0) {
      out.send_status.store(src);
      out.done.store(true);
      client.stop();
    }
  });
  if (rc != 0) out.done.store(true);
  const long long deadline = now_ms() + 10000;
  while (!out.done.load() && now_ms() < deadline) {
    client.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  client.stop();
}

void test_brotli_above_threshold() {
  const std::string payload = random_payload(8192);
  one_shot_server srv;
  srv.start(make_response(http_compress_method::BROTLI, payload));
  if (srv.port <= 0) {
    check(false, "br/large: server listening");
    return;
  }
  client_result r;
  run_client(srv.port, 1024, false, r);
  check(r.connect_status.load() == 0 && r.send_status.load() == 0,
        "br/large: 请求完成");
  check(r.body == payload, "br/large: 正文已解压且逐字节一致");
  check(r.headers_ce.empty(), "br/large: content-encoding 已摘掉");
  check(srv.last_request.find("gzip, deflate, br") != std::string::npos,
        "br/large: Accept-Encoding 里带 br");
}

void test_threshold() {
  const std::string small = "tiny-br";
  one_shot_server srv;
  srv.start(make_response(http_compress_method::BROTLI, small));
  if (srv.port <= 0) {
    check(false, "br/small: server listening");
    return;
  }

  client_result r1;
  run_client(srv.port, 1024, false, r1);
  check(r1.send_status.load() == 0 && !r1.headers_ce.empty(),
        "threshold: 默认 1024 时小 body 不解、头保留");

  client_result r2;
  run_client(srv.port, 0, false, r2);
  check(r2.send_status.load() == 0 && r2.body == small,
        "threshold: 设为 0 后同一 body 被解开");
  check(r2.headers_ce.empty(), "threshold: 解开后 content-encoding 被摘掉");
}

void test_gzip_regression() {
  const std::string payload = random_payload(8192);
  one_shot_server srv;
  srv.start(make_response(http_compress_method::GZIP, payload));
  if (srv.port <= 0) {
    check(false, "gzip: server listening");
    return;
  }
  client_result r;
  run_client(srv.port, 1024, false, r);
  check(r.send_status.load() == 0 && r.body == payload,
        "gzip: 正文仍能解开（不回归）");
}

void test_caller_accept_encoding_wins() {
  const std::string payload = random_payload(2048);
  one_shot_server srv;
  srv.start(make_response(http_compress_method::BROTLI, payload));
  if (srv.port <= 0) {
    check(false, "accept: server listening");
    return;
  }
  client_result r;
  run_client(srv.port, 1024, true, r);
  check(srv.last_request.find("accept-encoding: identity") != std::string::npos,
        "accept: 调用方自己设的 Accept-Encoding 不被覆盖");
}

}  // namespace

int main() {
  test_brotli_above_threshold();
  test_threshold();
  test_gzip_regression();
  test_caller_accept_encoding_wins();
  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}

#else  // !(ZLIB && BROTLI)

int main() {
  std::printf("SKIP: compression requires zlib + brotli\n");
  return 3;
}

#endif
