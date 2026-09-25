#include <iostream>
#include <cstring>
#include <string>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE
#include <web/uvcpp_http_server.h>
#include <web/uvcpp_http_parser.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <thread>
#include <web/uvcpp_http_client.h>
#include <net/uvcpp_tcp_client.h>
using namespace uvcpp;

// Test 1: Server construction, bind, listen, status
static bool test_server_lifecycle() {
  uvcpp_http_server server;
  if (server.get_status() != HTTP_SERVER_NONE) return false;
  server.bind("127.0.0.1", 20001);
  server.listen();
  if (!server.has_status(HTTP_SERVER_LISTENING)) return false;
  return true;
}

// Test 2: Route registration (no network)
static bool test_route_registration() {
  uvcpp_http_server server;
  server.bind("127.0.0.1", 20002);

  bool get_called = false;
  server.get("/hello", [&](uvcpp_http_request&, uvcpp_http_response& resp, uvcpp_tcp_client*) {
    get_called = true;
    resp = uvcpp_http_response::ok("hi", 2);
  });

  server.post("/data", [&](uvcpp_http_request&, uvcpp_http_response& resp, uvcpp_tcp_client*) {
    resp = uvcpp_http_response::ok("posted", 6);
  });

  // Routes are stored — can't easily test without real connection,
  // but at least verify no crash during registration
  (void)get_called;
  return true;
}

// Test 3: HTTP response serialization
static bool test_response_format() {
  auto resp = uvcpp_http_response::ok("body", 4);
  resp.set_header("x-test", "value");

  std::string wire = resp.to_string();
  if (wire.find("HTTP/1.1 200") == std::string::npos) return false;
  if (wire.find("content-length: 4") == std::string::npos) return false;
  if (wire.find("x-test: value") == std::string::npos) return false;
  if (wire.find("\r\n\r\nbody") == std::string::npos) return false;
  return true;
}

// Test 4: Status code responses
static bool test_status_codes() {
  auto ok = uvcpp_http_response::ok("ok", 2);
  if (ok.status_code != http_status::OK) return false;

  auto nf = uvcpp_http_response::not_found();
  if (nf.status_code != http_status::NOT_FOUND) return false;

  auto se = uvcpp_http_response::server_error();
  if (se.status_code != http_status::INTERNAL_SERVER_ERROR) return false;

  return true;
}

/** `sub` 在 `s` 里出现的次数。数"恰好一次"比数"至少一次"强，见下面几条。 */
static size_t count_substr(const std::string& s, const std::string& sub) {
  if (sub.empty()) return 0;
  size_t n = 0, pos = 0;
  while ((pos = s.find(sub, pos)) != std::string::npos) {
    ++n;
    pos += sub.size();
  }
  return n;
}

// =========================================================================
// Test 5: Chunked transfer encoding generation (server send path)
// =========================================================================
static bool test_chunked_response() {
  auto resp = uvcpp_http_response::ok("Hello World!", 12, "text/plain");
  resp.set_header("transfer-encoding", "chunked");

  std::string wire = resp.to_string();
  // Should contain Transfer-Encoding: chunked
  if (wire.find("transfer-encoding: chunked") == std::string::npos) return false;
  // Should NOT contain content-length (chunked replaces it)
  if (wire.find("content-length:") != std::string::npos) return false;
  // Should contain chunked body: hex-size, data, final chunk
  if (wire.find("\r\nc\r\nHello World!\r\n0\r\n\r\n") == std::string::npos) return false;

  // Verify it's still parseable
  uvcpp_http_parser parser(http_parser_mode::PARSE_RESPONSE);
  parser.execute(wire.c_str(), wire.size());
  parser.finish();
  if (!parser.is_complete() || parser.has_error()) return false;
  if (parser.get_status_code() != http_status::OK) return false;

  return true;
}

// =========================================================================
// Test 5b: 空 body 的 chunked 响应必须发终止块
//
// 终止块 `0\r\n\r\n` 是 chunked 的**帧**，不是 body 的一部分，所以它跟 body
// 是否为空无关 —— 这正是这一组存在的理由。原先的实现把终止块写在了
// `if (chunked && body.size() > 0)` 的块里面，于是空 body 时既没有
// content-length 也没有终止块，产出一条**未终止**的报文：keep-alive 上
// 对端只能一直等下一个块，直到自己超时。
//
// 判据用 `parser.is_complete()` 而不是字符串匹配：llhttp 只在收到终止块时
// 才报 message-complete，所以"报文完整"这件事本身就是终止块的可观测定义。
// 字符串那几条钉的是另一半 —— 终止块**恰好一次**、且必须在报文末尾。
// =========================================================================
static bool test_chunked_empty_terminated() {
  auto resp = uvcpp_http_response::ok(nullptr, 0, "text/plain");
  resp.set_header("transfer-encoding", "chunked");

  const std::string wire = resp.to_string();

  if (wire.find("transfer-encoding: chunked") == std::string::npos) return false;
  // chunked 与 content-length 互斥（RFC 7230 §3.3.2）。
  if (wire.find("content-length:") != std::string::npos) return false;

  if (count_substr(wire, "0\r\n\r\n") != 1) return false;
  // 必须是**末尾**那几个字节，后面什么都没有。空 body 时终止块前面紧挨着
  // 头部块末尾的空行，所以整条报文以 "\r\n0\r\n\r\n" 收尾。
  //
  // 长度取 `expected.size()` 而不是手写的数字：`compare(pos, len, s)` 比的是
  // `[pos, pos+len)` 与**整条** C 串 `s`，两者必须相等才可能命中，而手数一个
  // 转义串有几个字节错一次就是"永远不等"的恒假断言（这一版连错两次：先写 5
  // 比 7 个字节，再写 6 比 7 个字节，两条用例一起红）。
  const std::string tail = "\r\n0\r\n\r\n";
  if (wire.size() < tail.size()) return false;
  if (wire.compare(wire.size() - tail.size(), tail.size(), tail) != 0) return false;

  uvcpp_http_parser parser(http_parser_mode::PARSE_RESPONSE);
  parser.execute(wire.c_str(), wire.size());
  parser.finish();
  if (!parser.is_complete()) return false;  // ← 少了终止块这里就是 false
  if (parser.has_error()) return false;
  if (parser.get_status_code() != http_status::OK) return false;

  return true;
}

// =========================================================================
// Test 5c: 非空 body 的 chunked（对照组）
//
// 上面那条改了终止块的发送位置（从 `body.size() > 0` 的块里挪到块外），所以
// 必须有一条钉住"非空那条路没被改坏"，而且钉的是**恰好一次** —— 一个把终止块
// 发两次的实现会让上面的空 body 那条照样绿（字符串计数是 1，但报文里多一个
// 空块），只有这里看得出来。
// =========================================================================
static bool test_chunked_body_terminated() {
  auto resp = uvcpp_http_response::ok("Hello World!", 12, "text/plain");
  resp.set_header("transfer-encoding", "chunked");

  const std::string wire = resp.to_string();

  // 12 字节 → 十六进制 "c"。
  if (wire.find("\r\nc\r\nHello World!\r\n") == std::string::npos) return false;
  if (count_substr(wire, "0\r\n\r\n") != 1) return false;
  const std::string tail = "\r\n0\r\n\r\n";
  if (wire.size() < tail.size()) return false;
  if (wire.compare(wire.size() - tail.size(), tail.size(), tail) != 0) return false;

  uvcpp_http_parser parser(http_parser_mode::PARSE_RESPONSE);
  parser.execute(wire.c_str(), wire.size());
  parser.finish();
  if (!parser.is_complete() || parser.has_error()) return false;
  if (parser.get_status_code() != http_status::OK) return false;

  return true;
}

// =========================================================================
// Test 6: Server compression enabled by default (smoke test)
// =========================================================================
#if UVCPP_ZLIB_ENABLE
#include <web/uvcpp_http_compress.h>

static bool test_server_compress_enabled() {
  uvcpp_http_server server;
  // Default: compression enabled
  if (!server.is_compression_enabled()) return false;
  server.set_compression_enabled(false);
  if (server.is_compression_enabled()) return false;
  server.set_compression_enabled(true);
  if (!server.is_compression_enabled()) return false;
  return true;
}

// =========================================================================
// Test 6: Server MIME exclusion list
// =========================================================================
static bool test_server_mime_exclusion() {
  uvcpp_http_server server;
  server.add_compress_excluded_type("application/octet-stream");
  server.set_compress_min_body_size(512);

  // Compress should still work for text types
  auto& defaults = http_compress::default_excluded_mime_types();
  if (http_compress::should_compress("image/png", defaults)) return false;
  if (!http_compress::should_compress("text/html", defaults)) return false;

  return true;
}

// =========================================================================
// Test 7: Response header removal after compression check
// =========================================================================
static bool test_response_header_ops() {
  uvcpp_http_response resp = uvcpp_http_response::ok("test", 4, "text/plain");
  resp.set_header("content-encoding", "gzip");
  if (!resp.has_header("content-encoding")) return false;
  resp.remove_header("content-encoding");
  if (resp.has_header("content-encoding")) return false;

  // Verify response still serializes correctly after header removal
  std::string wire = resp.to_string();
  if (wire.find("HTTP/1.1 200") == std::string::npos) return false;
  if (wire.find("content-encoding") != std::string::npos) return false;
  // Content-Length should still be present (auto-inserted)
  if (wire.find("content-length") == std::string::npos) return false;

  return true;
}
#endif  // UVCPP_ZLIB_ENABLE

// =========================================================================
// 网络测试基础设施
//
// 后台线程跑 http server（端口 0 由系统分配），主线程用阻塞客户端往返。
// server 与其 loop 同在后台线程内创建/析构，避免跨线程跑 loop 的语义问题。
// 参考 web_static_server_func.cpp 的同名模式。
// =========================================================================

using SetupFn = std::function<void(uvcpp_http_server&)>;

struct TestServer {
  std::promise<int> port_promise;
  std::atomic<bool> stop{false};
  std::atomic<int> closed_conns{0};
  // 服务端登记的活连接数，每圈循环刷新一次 —— 服务器对象在后台线程里，
  // 线程退出后外面就拿不到它了，所以用这个快照来断言"没有连接被漏在表里"。
  std::atomic<int> live_clients{-1};
  std::thread thread;

  // setup 在后台线程内、listen 之前执行，可注册路由与配置。
  int start(SetupFn setup) {
    thread = std::thread([this, setup]() {
      uvcpp_http_server server;
      if (setup) setup(server);
      server.bind("127.0.0.1", 0);

      sockaddr_in name;
      int namelen = sizeof(name);
      server.get_tcp_server()->get_tcp()->getsockname(
          reinterpret_cast<sockaddr*>(&name), &namelen);
      const int port = ntohs(name.sin_port);

      // **先 listen，再发布端口。**
      //
      // 反过来的话，start() 返回时套接字还没进入 LISTEN：调用方立刻 connect
      // 会拿到 ECONNREFUSED。uv_listen 本身是同步的（就是 bind+listen 两个
      // 系统调用），不需要 loop 在跑，所以挪到这里是安全的 —— 内核的
      // backlog 会替我们把连接排住，直到下面 run() 开始 accept。
      //
      // 这个顺序问题是三条测试间歇性失败（约 4/6）的**唯一**根因：
      // http_roundtrip 自带 40 次重连重试所以掩盖了它，而直接用
      // uvcpp_tcp_client / raw_exchange 的那几条没有重试，一次拒绝就直接判失败。
      server.listen();

      port_promise.set_value(port);
      while (!stop.load()) {
        server.run(UV_RUN_NOWAIT);
        live_clients.store(
            static_cast<int>(server.get_tcp_server()->client_count()));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    return port_promise.get_future().get();
  }

  void shutdown() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// 阻塞式请求：连接重试以容忍 server 启动延迟。
// 传 accept_encoding 时手动加头（客户端自动压缩默认关闭，因此不会解压响应体，
// 便于断言线上确实是压缩过的）。
static bool http_roundtrip(int port, const uvcpp_http_request& req,
                           uvcpp_http_response& resp,
                           const std::string& accept_encoding = "") {
  for (int i = 0; i < 40; ++i) {
    uvcpp_http_client client;
    if (client.connect_wait("127.0.0.1", port, 2000) != 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      continue;
    }
    uvcpp_http_request r = req;
    if (!accept_encoding.empty()) r.set_header("accept-encoding", accept_encoding);
    if (client.send_wait(r, resp, 5000) == 0) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

static bool http_get(int port, const std::string& path, uvcpp_http_response& resp,
                     const std::string& accept_encoding = "") {
  return http_roundtrip(port, uvcpp_http_request::make_get(path), resp,
                        accept_encoding);
}

// 向 server 发原始字节并读回响应；返回 false 表示读失败/超时。
static bool raw_exchange(int port, const std::string& send,
                         std::string& received, int read_timeout_ms = 3000) {
  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;
  if (c.write_wait(send.c_str(), send.size(), 3000) != 0) return false;
  uvcpp_buf out;
  if (c.read_wait(out, read_timeout_ms) != 0) return false;
  received.assign(out.get_const_data() ? out.get_const_data() : "", out.size());
  return true;
}

// -------------------------------------------------------------------------
// Test: 声明了 chunked 的 HEAD 响应不得多出终止块
//
// 与 `test_chunked_empty_terminated` 是**同一个改动的两面**：那边加了"空 body
// 也要发终止块"，这边就必须保证 HEAD 不会因此凭空多出一段 body。清空 body 并
// 不足以让 HEAD 正确 —— `to_string()` 里那个终止块与 body 是否为空无关，所以
// HEAD 必须走"只序列化头部"那一支（`to_string(false)`），而不是靠 body 为空。
//
// 这条必须走真服务器：`to_string(false)` 是 `send_response` 的选择，不是
// 响应对象的属性。所以 HEAD 路由要**显式按 HEAD 方法注册** —— 路由匹配是
// 按 `http_method` 精确比的（`find_handler`），HEAD 请求**不会**落到 GET 路由上。
// -------------------------------------------------------------------------
static bool test_head_no_terminator() {
  TestServer srv;
  const int port = srv.start([](uvcpp_http_server& s) {
    s.head("/chunked", [](uvcpp_http_request&, uvcpp_http_response& resp,
                          uvcpp_tcp_client*) {
      resp = uvcpp_http_response::ok("Hello World!", 12, "text/plain");
      resp.set_header("transfer-encoding", "chunked");
    });
  });
  if (port <= 0) return false;

  std::string raw;
  if (!raw_exchange(port, "HEAD /chunked HTTP/1.1\r\nHost: x\r\n\r\n", raw)) {
    srv.shutdown();
    return false;
  }
  srv.shutdown();

  // 头部必须与 GET 的**逐字节相同** —— 含 `transfer-encoding: chunked`。
  if (raw.find("200") == std::string::npos) return false;
  if (raw.find("transfer-encoding: chunked") == std::string::npos) return false;
  // 而 body 一个字节都不许有：终止块是 chunked 的帧，它一出现就是一段 body。
  if (count_substr(raw, "0\r\n\r\n") != 0) return false;
  if (raw.find("Hello World!") != std::string::npos) return false;
  // 头部块必须完整收尾（不然上面两条"找不到"就是假绿）。
  if (raw.find("\r\n\r\n") == std::string::npos) return false;
  // 整条报文正好是头部块：空行之后不该有任何字节。
  if (raw.size() != raw.find("\r\n\r\n") + 4) return false;

  return true;
}

// -------------------------------------------------------------------------
// Test: 大响应体走异步写，完整送达
//
// 这条与下一条一起覆盖「响应写不再阻塞事件循环」：send_response 的写是异步的，
// 因此 4MB 响应不会卡住循环，也不会被 30s 同步写超时截断。
// -------------------------------------------------------------------------
// 大响应体尺寸可用环境变量覆盖，便于对「多大开始出问题」做二分定位。
static size_t big_body_size(size_t fallback) {
  const char* env = std::getenv("UVCPP_TEST_BIG_SIZE");
  if (env == nullptr) return fallback;
  long v = std::atol(env);
  return (v > 0) ? static_cast<size_t>(v) : fallback;
}

static bool test_large_response_async() {
  const size_t kSize = big_body_size(4u * 1024 * 1024);
  TestServer srv;
  int port = srv.start([kSize](uvcpp_http_server& s) {
    s.get("/big", [kSize](uvcpp_http_request&, uvcpp_http_response& resp,
                          uvcpp_tcp_client*) {
      std::string body(kSize, 'x');
      // 首尾做标记，确认没有被截断或错位
      body[0] = 'A';
      body[kSize - 1] = 'Z';
      resp = uvcpp_http_response::ok(body.data(), body.size(), "text/plain");
    });
  });

  bool ok = true;
  uvcpp_http_response resp;
  if (!http_get(port, "/big", resp)) {
    std::cout << "  [err] /big 请求失败\n";
    ok = false;
  } else if (resp.body.size() != kSize) {
    std::cout << "  [err] /big 长度 " << resp.body.size() << " != " << kSize << "\n";
    ok = false;
  } else if (resp.body.get_const_data()[0] != 'A' ||
             resp.body.get_const_data()[kSize - 1] != 'Z') {
    std::cout << "  [err] /big 首尾标记不符（数据被截断/错位）\n";
    ok = false;
  }

  srv.shutdown();
  return ok;
}

// -------------------------------------------------------------------------
// Test: 慢客户端不阻塞事件循环
//
// 连接 A 请求大响应后完全不读，把它的发送窗口占满；此期间连接 B 的请求必须
// 依然被及时处理。旧的同步写实现会在 A 上自旋泵循环最多 30s，B 只能干等。
// -------------------------------------------------------------------------
static bool test_slow_client_does_not_block_loop() {
  const size_t kSize = big_body_size(8u * 1024 * 1024);
  TestServer srv;
  int port = srv.start([kSize](uvcpp_http_server& s) {
    s.get("/big", [kSize](uvcpp_http_request&, uvcpp_http_response& resp,
                          uvcpp_tcp_client*) {
      std::string body(kSize, 'x');
      resp = uvcpp_http_response::ok(body.data(), body.size(), "text/plain");
    });
    s.get("/ping", [](uvcpp_http_request&, uvcpp_http_response& resp,
                      uvcpp_tcp_client*) {
      resp = uvcpp_http_response::ok("pong", 4, "text/plain");
    });
  });

  bool ok = true;

  // A：请求 8MB 后一个字节都不读。用内层作用域包住，保证在 server 关闭
  // 之前先断开 —— 否则 server 侧会带着一个永远写不完的 8MB 异步写进入
  // 析构（这是另一种情形，见文件末尾的说明）。
  {
    uvcpp_tcp_client slow;
    if (slow.connect_wait("127.0.0.1", port, 3000) != 0) {
      srv.shutdown();
      return false;
    }
    const std::string req = "GET /big HTTP/1.1\r\nHost: x\r\n\r\n";
    if (slow.write_wait(req.c_str(), req.size(), 3000) != 0) {
      srv.shutdown();
      return false;
    }
    // 给 server 一点时间把响应推满 A 的发送缓冲区
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // B：此时必须仍能被服务
    auto t0 = std::chrono::steady_clock::now();
    uvcpp_http_response resp;
    bool got = http_get(port, "/ping", resp);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();

    if (!got || resp.body.size() != 4) {
      std::cout << "  [err] 慢客户端占线时 /ping 失败\n";
      ok = false;
    } else if (elapsed_ms > 3000) {
      // 旧实现会阻塞到 30s 同步写超时
      std::cout << "  [err] 慢客户端占线时 /ping 耗时 " << elapsed_ms
                << "ms（事件循环被阻塞）\n";
      ok = false;
    }
  }  // slow 析构：server 侧收到对端关闭，未完成的写随之结束

  // 给 server 处理对端关闭一点时间
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  srv.shutdown();
  return ok;
}

// -------------------------------------------------------------------------
// Test: deferred 响应同样会被压缩
//
// 框架的异步 handler 全部走 deferred 路径。压缩原先在 on_request_complete
// 里、deferred 提前返回之前执行，导致所有 deferred 响应静默不压缩。
// -------------------------------------------------------------------------
#if UVCPP_ZLIB_ENABLE
static bool test_deferred_response_is_compressed() {
  const std::string kBody(4096, 'c');
  TestServer srv;
  int port = srv.start([kBody](uvcpp_http_server& s) {
    // handler 在 on_request_complete 返回后才发送响应 —— 这正是框架里所有
    // 异步 handler 的形态。send_response 需要 server，故取地址捕获：
    // server 的生存期覆盖整个后台线程。
    uvcpp_http_server* sp = &s;
    s.get("/deferred", [sp, kBody](uvcpp_http_request&, uvcpp_http_response& resp,
                                   uvcpp_tcp_client* client) {
      resp = uvcpp_http_response::ok(kBody.data(), kBody.size(), "text/plain");
      resp.deferred = true;  // 让 on_request_complete 跳过自动发送
      sp->send_response(client, resp);
    });
  });

  bool ok = true;
  uvcpp_http_response resp;
  if (!http_get(port, "/deferred", resp, "gzip")) {
    std::cout << "  [err] /deferred 请求失败\n";
    ok = false;
  } else {
    std::string ce = resp.get_header("content-encoding");
    if (ce != "gzip") {
      std::cout << "  [err] deferred 响应未被压缩 (content-encoding='" << ce
                << "')\n";
      ok = false;
    } else if (resp.body.size() >= kBody.size()) {
      std::cout << "  [err] 压缩后体积未减小: " << resp.body.size()
                << " >= " << kBody.size() << "\n";
      ok = false;
    } else if (resp.get_header("vary").find("accept-encoding") == std::string::npos) {
      std::cout << "  [err] 压缩响应缺少 Vary: accept-encoding\n";
      ok = false;
    }
  }

  srv.shutdown();
  return ok;
}
#endif

// -------------------------------------------------------------------------
// Test: 畸形请求 -> 规范 400 + 连接关闭
// -------------------------------------------------------------------------
static bool test_malformed_request_gets_400() {
  TestServer srv;
  int port = srv.start(nullptr);

  bool ok = true;
  std::string got;
  // 非法请求行：llhttp 会报错
  if (!raw_exchange(port, "GARBAGE !!!\r\n\r\n", got)) {
    std::cout << "  [err] 畸形请求没有任何响应\n";
    ok = false;
  } else if (got.find("400") == std::string::npos) {
    std::cout << "  [err] 畸形请求响应不是 400: '"
              << got.substr(0, 40) << "'\n";
    ok = false;
  } else if (got.find("connection: close") == std::string::npos) {
    std::cout << "  [err] 400 响应缺少 connection: close\n";
    ok = false;
  }

  srv.shutdown();
  return ok;
}

// -------------------------------------------------------------------------
// Test: 超出 body 上限 -> 413
// -------------------------------------------------------------------------
static bool test_body_limit_returns_413() {
  TestServer srv;
  int port = srv.start([](uvcpp_http_server& s) {
    s.set_max_body_size(128);
    s.post("/upload", [](uvcpp_http_request&, uvcpp_http_response& resp,
                         uvcpp_tcp_client*) {
      resp = uvcpp_http_response::ok("should not be reached", 21, "text/plain");
    });
  });

  bool ok = true;
  uvcpp_http_response resp;
  uvcpp_http_request req = uvcpp_http_request::make_post(
      "/upload", std::string(4096, 'u').data(), 4096, "text/plain");
  if (!http_roundtrip(port, req, resp)) {
    std::cout << "  [err] 超限请求无响应\n";
    ok = false;
  } else if (resp.status_code != http_status::PAYLOAD_TOO_LARGE) {
    std::cout << "  [err] 超限请求状态码 "
              << static_cast<int>(resp.status_code) << " != 413\n";
    ok = false;
  }

  // 上限内的小请求仍应正常路由
  // 注意：不要用 `small` 作变量名 —— Windows 的 rpcndr.h 里 `#define small char`。
  uvcpp_http_response within_limit;
  uvcpp_http_request req2 = uvcpp_http_request::make_post(
      "/upload", std::string(64, 's').data(), 64, "text/plain");
  if (ok && !http_roundtrip(port, req2, within_limit)) {
    std::cout << "  [err] 上限内的请求无响应\n";
    ok = false;
  } else if (ok && within_limit.status_code != http_status::OK) {
    std::cout << "  [err] 上限内的请求状态码 "
              << static_cast<int>(within_limit.status_code) << " != 200\n";
    ok = false;
  }

  srv.shutdown();
  return ok;
}

// -------------------------------------------------------------------------
// Test: 超出请求头上限 -> 431（而且是在**收到完整头块之前**就回）
// -------------------------------------------------------------------------
static bool test_header_limit_returns_431() {
  TestServer srv;
  int port = srv.start([](uvcpp_http_server& s) {
    s.set_max_header_bytes(1024);
    s.get("/x", [](uvcpp_http_request&, uvcpp_http_response& resp,
                   uvcpp_tcp_client*) { resp = uvcpp_http_response::ok("x", 1); });
  });

  bool ok = true;

  // 关键的一条：这一段**没有**结尾的空行，头块还没收完。能收到 431 就说明
  // 服务端是边收边判、超了当场停 —— 若改成等 `headers_complete` 再看，
  // 这里会一直等一个永远不来的 CRLF，`raw_exchange` 直接读超时。
  const std::string unterminated =
      "GET /x HTTP/1.1\r\nHost: a\r\nX-Big: " + std::string(2000, 'b');
  std::string got;
  if (!raw_exchange(port, unterminated, got)) {
    std::cout << "  [err] 超长头部没有在头块收完之前得到响应（说明是等收完才判的）\n";
    ok = false;
  } else if (got.find("431") == std::string::npos) {
    std::cout << "  [err] 超长头部的响应不是 431，收到："
              << got.substr(0, 40) << "\n";
    ok = false;
  } else if (got.find("400") != std::string::npos) {
    // 撞上限也让解析器进 PARSE_ERROR，所以 `has_error()` 那条路是抢得到的 ——
    // 判据的顺序错了就会回 400（"请求畸形"），把两件事混成一件。
    std::cout << "  [err] 回成了 400：撞上限被当成解析错误了\n";
    ok = false;
  }

  // 上限内的正常请求照常路由 —— 否则上面那条可能只是"什么请求都拒"。
  uvcpp_http_response within;
  if (ok && !http_get(port, "/x", within)) {
    std::cout << "  [err] 上限内的请求无响应\n";
    ok = false;
  } else if (ok && within.status_code != http_status::OK) {
    std::cout << "  [err] 上限内的请求状态码 "
              << static_cast<int>(within.status_code) << " != 200\n";
    ok = false;
  }

  srv.shutdown();
  return ok;
}

// -------------------------------------------------------------------------
// Test: 超出 URL 上限 -> 414（同样在请求行收完之前就回）
// -------------------------------------------------------------------------
static bool test_url_limit_returns_414() {
  TestServer srv;
  int port = srv.start([](uvcpp_http_server& s) {
    s.set_max_url_bytes(64);
    s.get("/x", [](uvcpp_http_request&, uvcpp_http_response& resp,
                   uvcpp_tcp_client*) { resp = uvcpp_http_response::ok("x", 1); });
  });

  bool ok = true;

  // 只有 "GET /" + 一长串，连 " HTTP/1.1" 都还没发出去。
  const std::string unterminated = "GET /" + std::string(200, 'c');
  std::string got;
  if (!raw_exchange(port, unterminated, got)) {
    std::cout << "  [err] 超长 URL 没有在请求行收完之前得到响应\n";
    ok = false;
  } else if (got.find("414") == std::string::npos) {
    std::cout << "  [err] 超长 URL 的响应不是 414，收到："
              << got.substr(0, 40) << "\n";
    ok = false;
  }

  uvcpp_http_response within;
  if (ok && !http_get(port, "/x", within)) {
    std::cout << "  [err] 上限内的请求无响应\n";
    ok = false;
  } else if (ok && within.status_code != http_status::OK) {
    std::cout << "  [err] 上限内的请求状态码 "
              << static_cast<int>(within.status_code) << " != 200\n";
    ok = false;
  }

  srv.shutdown();
  return ok;
}

// -------------------------------------------------------------------------
// Test: 服务端主动关闭连接时 on_connection_close 被触发
//
// 旧实现只在删除 conn_ctx 的路径上回调，而服务端主动关闭走的是
// get_tcp()->close()，其完成回调是空函数 —— 连接关闭回调永远不会触发。
// -------------------------------------------------------------------------
static bool test_server_close_notifies() {
  TestServer srv;
  int port = srv.start([&srv](uvcpp_http_server& s) {
    s.on_request([](uvcpp_http_request&, uvcpp_http_response& resp,
                    uvcpp_tcp_client*) {
      resp = uvcpp_http_response::ok("bye", 3, "text/plain");
      resp.set_header("connection", "close");  // 触发服务端主动关闭
    });
    s.on_connection_close([&srv](uvcpp_tcp_client*) { srv.closed_conns++; });
  });

  bool ok = true;
  uvcpp_http_response resp;
  if (!http_get(port, "/anything", resp)) {
    std::cout << "  [err] 请求失败\n";
    ok = false;
  }

  // 关闭回调在 server 线程上执行，等它落地
  for (int i = 0; i < 100 && srv.closed_conns.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (srv.closed_conns.load() == 0) {
    std::cout << "  [err] 服务端主动关闭连接未触发 on_connection_close\n";
    ok = false;
  }

  // 关掉之后**客户端对象本身**也必须被摘除并释放。
  //
  // `on_connection_close` 只证明 http 层自己的连接上下文收掉了；客户端对象
  // 归 uvcpp_tcp_server 管，服务端主动关闭时如果不通知框架的关闭回调，它就
  // 永远留在 tcp_server 的登记表里 —— 每个 Connection: close 的响应漏一个。
  //
  // 这里必须**等**它归零，不能查一次就断言。两个原因：`live_clients` 是 server
  // 线程每 ~1ms 采样一次的 `client_count()` 快照（见 `TestServer::start`）；
  // 而 http 层的 `on_connection_close` 和 tcp_server 摘除客户端是两条独立回调，
  // 先后没有约定。所以"等 closed_conns 落地就立刻查 live_clients"会读到摘除前
  // 的旧快照 —— 实测约 1/20 概率误报成"漏在表里"。真漏了的实现永远归不了零，
  // 这个用例的判别力不受影响。
  for (int i = 0; i < 100 && srv.live_clients.load() != 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (srv.live_clients.load() != 0) {
    std::cout << "  [err] 服务端主动关闭之后仍有 " << srv.live_clients.load()
              << " 个客户端留在 tcp_server 登记表里（应为 0）\n";
    ok = false;
  }

  srv.shutdown();
  return ok;
}

// -------------------------------------------------------------------------
// Test: keep-alive 上连续两个响应不被写串行化打乱
//
// uvcpp_tcp_client 同时只允许一个异步写，连续响应必须在 write_queue 里排队。
// 在一条连接上连做两次请求：两个响应体必须各自完整、按序、不交错 —— 一旦
// 交错，客户端的解析会直接失败。
// -------------------------------------------------------------------------
static bool test_keepalive_sequential_responses() {
  TestServer srv;
  int port = srv.start([](uvcpp_http_server& s) {
    s.get("/one", [](uvcpp_http_request&, uvcpp_http_response& resp,
                     uvcpp_tcp_client*) {
      std::string b(2000, 'a');
      resp = uvcpp_http_response::ok(b.data(), b.size(), "text/plain");
    });
    s.get("/two", [](uvcpp_http_request&, uvcpp_http_response& resp,
                     uvcpp_tcp_client*) {
      std::string b(3000, 'b');
      resp = uvcpp_http_response::ok(b.data(), b.size(), "text/plain");
    });
  });

  bool ok = true;

  uvcpp_http_client client;
  if (client.connect_wait("127.0.0.1", port, 3000) != 0) {
    srv.shutdown();
    return false;
  }

  uvcpp_http_response r1, r2;
  int rc1 = client.send_wait(uvcpp_http_request::make_get("/one"), r1, 5000);
  int rc2 = client.send_wait(uvcpp_http_request::make_get("/two"), r2, 5000);

  if (rc1 != 0 || rc2 != 0) {
    std::cout << "  [err] keep-alive 复用失败 rc1=" << rc1 << " rc2=" << rc2 << "\n";
    ok = false;
  } else if (std::string(r1.body.get_const_data(), r1.body.size()) !=
             std::string(2000, 'a')) {
    std::cout << "  [err] 响应 1 的 body 不完整或内容错误 (size="
              << r1.body.size() << ")\n";
    ok = false;
  } else if (std::string(r2.body.get_const_data(), r2.body.size()) !=
             std::string(3000, 'b')) {
    std::cout << "  [err] 响应 2 的 body 不完整或内容错误 (size="
              << r2.body.size() << ")\n";
    ok = false;
  }

  srv.shutdown();
  return ok;
}

// -------------------------------------------------------------------------
// Test: 客户端侧 HEAD —— 有 `Content-Length` 却没有 body
//
// 服务端的 HEAD 语义是对的（上一条已钉：头部与 GET 逐字节相同——**包括
// `Content-Length`**——body 一个字节不发）。坏的一直是**客户端**：它按分帧头
// 决定还要读多少，于是照着头里那个 `Content-Length` 去等一个按 RFC 7231
// §4.3.2 根本不会来的 body，一路等到超时。异步路径（llhttp）也一样——llhttp
// 只认 `flags & F_SKIPBODY`，从不自己看 `method`。
//
// 判据分两种形态，**别只留 rc**：异步那条是硬判据（改前回调根本不落地，
// 3 秒内必然不出来）；阻塞那条在本机（Windows）**改前也是 rc == 0** —— 阻塞
// 路径只在 Windows 上给 socket 设 `SO_RCVTIMEO`，于是改前的形态是"卡满超时
// 再回一个空 body"，所以阻塞那条还得加**墙钟**（毫秒级 vs ≥3 秒）。变异实测过：
// 只断言 rc / body 的话，变异态照样 ALL PASS。其余几条防止"收是收了，收错了"：
//   - 状态码 200、body 恰好 0 字节；
//   - `content-length` 仍然在头里（那是 HEAD 的正当语义，不是该被抹掉的东西）；
//   - 紧接着在**同一条连接**上发一个 GET，body 必须完整——HEAD 少读的那次
//     不能把下一个响应的字节吃掉。
//
// 两条路径都在这里过一遍：`send_wait` 在 build-webapp 落异步解析器、在
// 开 OpenSSL 的树里落 `send_wait_plain`（同为阻塞式原始 socket）；`send(cb)`
// 两种树上都走解析器。
// -------------------------------------------------------------------------
static bool test_client_head_request() {
  TestServer srv;
  const int port = srv.start([](uvcpp_http_server& s) {
    s.head("/h", [](uvcpp_http_request&, uvcpp_http_response& resp,
                    uvcpp_tcp_client*) {
      resp = uvcpp_http_response::ok("0123456789", 10, "text/plain");
    });
    s.get("/g", [](uvcpp_http_request&, uvcpp_http_response& resp,
                   uvcpp_tcp_client*) {
      resp = uvcpp_http_response::ok("get-body", 8, "text/plain");
    });
  });
  if (port <= 0) return false;

  bool ok = true;
  uvcpp_http_client client;
  if (client.connect_wait("127.0.0.1", port, 3000) != 0) {
    srv.shutdown();
    return false;
  }

  // ---- 1) 阻塞式：3 秒超时。本机改前是"卡满 3 秒 + 空 body"，判据见下面的墙钟
  {
    uvcpp_http_response resp;
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = client.send_wait(uvcpp_http_request::make_head("/h"), resp, 3000);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    // **墙钟判据不能省。** 两条阻塞路径（`send_wait_ssl` / `send_wait_plain`）
    // 只在 Windows 上给 socket 设了 `SO_RCVTIMEO`，所以改前的形态是"卡满那个
    // 超时再回一个空 body"，而 `rc == 0` / `body 为空` **两态都成立** ——
    // 实测过：把修好的那几行变异掉、重编，光靠这两条断言照样 ALL PASS。
    // 真正在两种形态之间分得开的是"到底等了多久"（毫秒级 vs ≥3 秒）。
    if (ms >= 1500) {
      std::cout << "  [err] HEAD 的 send_wait 用了 " << ms
                << " ms（毫秒级才正常；≥3s 就是在等那个不会来的 body）\n";
      ok = false;
    }
    if (rc != 0) {
      std::cout << "  [err] HEAD send_wait rc=" << rc << "（超时就是那个缺陷）\n";
      ok = false;
    } else {
      if (resp.status_code != http_status::OK) {
        std::cout << "  [err] HEAD 状态码 = " << static_cast<int>(resp.status_code) << "\n";
        ok = false;
      }
      if (resp.body.size() != 0) {
        std::cout << "  [err] HEAD 不该有 body，实到 " << resp.body.size() << " 字节\n";
        ok = false;
      }
      // 头部必须与 GET 相同 —— 抹掉 content-length 是"绕过"不是"修好"。
      if (resp.get_header("content-length") != "10") {
        std::cout << "  [err] HEAD 的 content-length = '"
                  << resp.get_header("content-length") << "'（应为 10）\n";
        ok = false;
      }
    }
  }

  // ---- 2) 同一条连接上紧接着 GET：HEAD 不能吃掉后续响应的字节 ----
  {
    uvcpp_http_response resp;
    const int rc = client.send_wait(uvcpp_http_request::make_get("/g"), resp, 3000);
    if (rc != 0) {
      std::cout << "  [err] HEAD 之后的 GET rc=" << rc << "\n";
      ok = false;
    } else if (std::string(resp.body.get_const_data() ? resp.body.get_const_data() : "",
                           resp.body.size()) != "get-body") {
      std::cout << "  [err] HEAD 之后的 GET body 不对 (size=" << resp.body.size() << ")\n";
      ok = false;
    }
  }

  // ---- 3) 异步 send(cb) —— 走 llhttp 的那条路（两种构建都是） ----
  {
    std::atomic<bool> done{false};
    int err_seen = -1;
    int status_seen = 0;
    size_t body_seen = 1;
    const int rc = client.send(
        uvcpp_http_request::make_head("/h"),
        [&](const uvcpp_http_response& r, int err) {
          err_seen = err;
          status_seen = static_cast<int>(r.status_code);
          body_seen = r.body.size();
          done.store(true);
        });
    if (rc != 0) {
      std::cout << "  [err] HEAD async send rc=" << rc << "\n";
      ok = false;
    } else {
      const auto t0 = std::chrono::steady_clock::now();
      while (!done.load() &&
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - t0).count() < 3000) {
        client.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!done.load()) {
        std::cout << "  [err] HEAD async 3 秒内没有回调（llhttp 在等那个不来的 body）\n";
        ok = false;
      } else if (err_seen != 0 || status_seen != 200 || body_seen != 0) {
        std::cout << "  [err] HEAD async err=" << err_seen << " status=" << status_seen
                  << " body=" << body_seen << "\n";
        ok = false;
      }
    }
  }

  srv.shutdown();
  return ok;
}

// -------------------------------------------------------------------------
// Test: 原始数据钩子 —— 观察模式与接管模式
// -------------------------------------------------------------------------
static bool test_raw_data_hook() {
  bool ok = true;

  // 观察模式：返回 true，字节照常进入解析器，同时钩子看到了原始字节。
  {
    std::atomic<size_t> seen{0};
    std::atomic<int> calls{0};
    TestServer srv;
    int port = srv.start([&seen, &calls](uvcpp_http_server& s) {
      s.set_raw_data_hook([&seen, &calls](uvcpp_tcp_client*, const char*, size_t len) {
        seen += len;
        calls++;
        return true;  // 只观察，不接管
      });
      s.get("/ok", [](uvcpp_http_request&, uvcpp_http_response& resp,
                      uvcpp_tcp_client*) {
        resp = uvcpp_http_response::ok("yes", 3, "text/plain");
      });
    });

    uvcpp_http_response resp;
    if (!http_get(port, "/ok", resp)) {
      std::cout << "  [err] 观察模式下请求失败（不应影响解析）\n";
      ok = false;
    } else if (resp.body.size() != 3) {
      std::cout << "  [err] 观察模式下响应体错误\n";
      ok = false;
    } else if (calls.load() == 0 || seen.load() == 0) {
      std::cout << "  [err] 观察模式下钩子未收到任何原始字节\n";
      ok = false;
    }
    srv.shutdown();
  }

  // 接管模式：返回 false，字节不进解析器 -> 不会产生 HTTP 响应。
  {
    std::atomic<size_t> seen{0};
    TestServer srv;
    int port = srv.start([&seen](uvcpp_http_server& s) {
      s.set_raw_data_hook([&seen](uvcpp_tcp_client*, const char*, size_t len) {
        seen += len;
        return false;  // 接管：这些字节不被解析为 HTTP
      });
      s.get("/ok", [](uvcpp_http_request&, uvcpp_http_response& resp,
                      uvcpp_tcp_client*) {
        resp = uvcpp_http_response::ok("yes", 3, "text/plain");
      });
    });

    std::string got;
    // 读会超时 —— 这正是期望：没有任何 HTTP 响应产生
    bool read_ok = raw_exchange(port, "GET /ok HTTP/1.1\r\nHost: x\r\n\r\n",
                                got, 600);
    if (read_ok && got.find("HTTP/") != std::string::npos) {
      std::cout << "  [err] 接管模式下解析器仍产生了 HTTP 响应\n";
      ok = false;
    } else if (seen.load() == 0) {
      std::cout << "  [err] 接管模式下钩子未收到任何原始字节\n";
      ok = false;
    }
    srv.shutdown();
  }

  return ok;
}

// `deferred` 是处理器与服务器之间"先别发我、我稍后自己发"的约定（服务器在
// 处理器返回后 `if (resp.deferred) return;`）。拷贝构造/赋值是手写的，漏字段
// 正是这类写法的通病 —— 漏掉它，一份拷贝看起来就是"没设过 deferred"，于是
// 本该延迟发送的响应当场发出去。判据做成一对：true 要跟过去，false 不能被
// 变成 true（只判前一半的话，"两个拷贝都恒为 true"也能过）。
static bool test_response_copy_keeps_deferred() {
  uvcpp_http_response src;
  src.status_code = http_status::OK;
  src.body.clone_data("body", 4);
  src.deferred = true;

  uvcpp_http_response by_copy(src);
  if (!by_copy.deferred) {
    std::cout << "  [err] 拷贝构造丢了 deferred\n";
    return false;
  }
  uvcpp_http_response by_assign;
  by_assign = src;
  if (!by_assign.deferred) {
    std::cout << "  [err] 拷贝赋值丢了 deferred\n";
    return false;
  }
  // 同一对手写的拷贝/赋值，顺带核对别的字段没被漏掉
  if (by_copy.status_code != http_status::OK || by_copy.body.size() != 4 ||
      by_assign.status_code != http_status::OK || by_assign.body.size() != 4) {
    std::cout << "  [err] 拷贝/赋值丢了别的字段\n";
    return false;
  }

  uvcpp_http_response plain;  // deferred 默认 false
  plain.body.clone_data("p", 1);
  uvcpp_http_response plain_copy(plain);
  uvcpp_http_response plain_assign;
  plain_assign = plain;
  if (plain_copy.deferred || plain_assign.deferred) {
    std::cout << "  [err] 拷贝把 deferred 置成了 true\n";
    return false;
  }
  return true;
}

// -------------------------------------------------------------------------
// Test: 请求头表不跨请求累积（搬移的目的地是**连接上**那块表）
//
// `take_headers_into()` 是**搬**（`make_move_iterator`），不是拷：目的地是
// 连接级的 `ctx.request.headers`，容量因此跨请求留下 —— 省掉的那一笔就是
// 站点普查里 `take_headers()` 的 1.00 次/请求。
//
// 搬的代价是"清目的地"成了**唯一**守卫：这条路上 `set_on_message_begin`
// 只复位 `stream_request`（流式视图那份），**不碰** `request`。删掉
// `dst.clear()`，第二条请求就会看到 `2 x n` 条 —— 前一条被搬空的壳留在表里。
//
// 所以这里两条判据都要（照 `web_app_app_func.cpp` 那条的教训）：
//   · **表的大小**：搬走会留空名字的壳，按名字查是查不到的 ⇒ 只看查表的
//     判据在变异态下**恒真**；
//   · **按名字查**：补上"拷而非搬"那一类（名字会留下）。
// -------------------------------------------------------------------------
static bool test_request_headers_do_not_accumulate() {
  TestServer srv;
  std::atomic<std::size_t> n_one(0), n_two(0);
  std::atomic<bool> one_own(false), two_own(false);
  std::atomic<bool> two_saw_stale(false);

  int port = srv.start([&](uvcpp_http_server& s) {
    s.get("/one", [&](uvcpp_http_request& req, uvcpp_http_response& resp,
                      uvcpp_tcp_client*) {
      if (req.get_header("x-req-one") == "1") one_own.store(true);
      n_one.store(req.headers.size());
      resp = uvcpp_http_response::ok("1", 1);
    });
    s.get("/two", [&](uvcpp_http_request& req, uvcpp_http_response& resp,
                      uvcpp_tcp_client*) {
      if (!req.get_header("x-req-one").empty()) two_saw_stale.store(true);
      if (req.get_header("x-req-two") == "2") two_own.store(true);
      n_two.store(req.headers.size());
      resp = uvcpp_http_response::ok("2", 1);
    });
  });

  bool ok = true;
  uvcpp_http_client client;
  if (client.connect_wait("127.0.0.1", port, 3000) != 0) {
    srv.shutdown();
    return false;
  }

  uvcpp_http_request r1 = uvcpp_http_request::make_get("/one");
  r1.set_header("x-req-one", "1");
  uvcpp_http_request r2 = uvcpp_http_request::make_get("/two");
  r2.set_header("x-req-two", "2");

  uvcpp_http_response resp1, resp2;
  const int rc1 = client.send_wait(r1, resp1, 5000);
  const int rc2 = client.send_wait(r2, resp2, 5000);

  // 前提：两条请求都得走完，而且各自**自己的**头必须到 —— 否则"第二条看不见
  // 第一条的头"这件事根本没法归因（可能是头压根没解析）。
  if (rc1 != 0 || rc2 != 0) {
    std::cout << "  [err] keep-alive 复用失败 rc1=" << rc1
              << " rc2=" << rc2 << "\n";
    srv.shutdown();
    return false;
  }
  if (!one_own.load() || !two_own.load()) {
    std::cout << "  [err] 两条请求自己的头没到（one=" << one_own.load()
              << " two=" << two_own.load() << "）—— 判据的前提不成立\n";
    ok = false;
  }
  if (n_one.load() < 2) {
    std::cout << "  [err] 第一条请求只有 " << n_one.load()
              << " 条头（至少 2 条：host + x-req-one）—— 判据的前提不成立\n";
    ok = false;
  }
  if (two_saw_stale.load()) {
    std::cout << "  [err] 第二条请求里 `x-req-one` **还在**（留下的表没被清过）\n";
    ok = false;
  }
  if (n_two.load() != n_one.load()) {
    std::cout << "  [err] 头表跨请求**累积**了：第一条 " << n_one.load()
              << " 条、第二条 " << n_two.load() << " 条（必须相等）\n";
    ok = false;
  }
  std::cout << "  [info] 头表条数：第一条 " << n_one.load()
            << "、第二条 " << n_two.load() << std::endl;

  srv.shutdown();
  return ok;
}

// =========================================================================
// Test: to_string_into —— 复用缓冲形态与造串形态**逐字节相同**，且缓冲真复用
//
// 这一笔省掉的是"每响应造一个串"那一次分配（站点普查里
// `uvcpp_http_response::to_string` 的 1.00 次/请求）：那个串的字节**立刻**被拷进
// 写请求自己的头部缓冲（`uvcpp_tcp_client::write_owned` 里那次 memcpy），然后当场
// 析构。改法 = 目的地由调用方给 —— 服务端把它挂在**连接**上，容量因此跨请求留下。
//
// 三条判据，各自都要能红（变异见 `n1_mutations.py`）：
//   · **逐字节相同**：`to_string_into(out, v)` 与 `to_string(v)` 一字不差
//     （v ∈ {true, false}；`false` 那支是 HEAD / 流式头部走的路）。
//     ★ 这条现在**结构上是"按构造成立"的**（`to_string` 就是把活交给
//     `to_string_into`），所以它挡的不是"实现写错"而是"**以后两者漂开**"——
//     能红的变异是"壳里忘了转发 flag"那一种（M3）。
//   · **脏缓冲**：目的地**已经有内容**时，结果不许带上旧内容 ⇒ 删掉
//     `to_string_into` 里那句 `result.clear()` 必红（M1）。
//   · **缓冲没被换掉**：先给一块**远大于**本次需要量的缓冲，调完之后容量不许
//     掉下来 ⇒ `std::string().swap(x)` / `clear()+shrink_to_fit()` 这类真换缓冲的
//     写法必红（M2）。
//     ★ **不许**只判 `data()` 不变：真换缓冲那几种写法会把旧块还给分配器，下一次
//     `reserve` 往往拿到**同一个地址**（本机实测，见 `n1_streambuf_probe.cpp` 的
//     C/D 两臂：`data same` 而 `allocs reserve 1`）⇒ 那条判据是瞎子。
static bool test_to_string_into_reuses_buffer() {
  bool ok = true;

  // ---- 一、两种形态逐字节相同（chunked 空体那条边界也要盖到）
  struct Case { const char* name; bool chunked; bool empty; };
  const Case cases[] = {
    {"plain",         false, false},
    {"chunked",       true,  false},
    {"chunked-empty", true,  true },
  };
  for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    uvcpp_http_response resp = cases[i].empty
        ? uvcpp_http_response::ok(nullptr, 0, "text/plain")
        : uvcpp_http_response::ok("body-bytes", 10, "text/plain");
    resp.set_header("x-a", "1");
    if (cases[i].chunked) resp.set_header("transfer-encoding", "chunked");

    for (int v = 0; v < 2; ++v) {
      const bool include_body = (v == 0);
      const std::string want = resp.to_string(include_body);
      std::string got;
      resp.to_string_into(got, include_body);
      if (got != want) {
        std::cout << "  [err] " << cases[i].name << " include_body=" << include_body
                  << "：两种形态字节不同（" << got.size() << " vs " << want.size()
                  << " 字节）\n";
        ok = false;
      }
    }
  }

  // ---- 二、脏缓冲：目的地里先塞一段别的，结果不许带上它
  {
    uvcpp_http_response resp = uvcpp_http_response::ok("payload", 7, "text/plain");
    std::string out = "STALE-CONTENT-MUST-VANISH-0123456789";
    resp.to_string_into(out, true);
    if (out != resp.to_string(true)) {
      std::cout << "  [err] 目的地里原来那段内容没被清掉（缺 `clear()`？）\n";
      ok = false;
    }
  }

  // ---- 三、缓冲没被换掉：手工撑到 4096（整条响应只要 ~96），调完之后容量不许
  //      掉下来。`clear()` 只改长度 ⇒ 仍是 4096；若实现里改成真换缓冲的写法，
  //      容量会先掉到 SSO 尺寸、再按本次需要量涨回 ~96 ⇒ 掉下来 ⇒ 红。
  {
    uvcpp_http_response resp = uvcpp_http_response::ok("keep", 4, "text/plain");
    std::string out;
    out.reserve(4096);
    const std::string::size_type cap_before = out.capacity();
    resp.to_string_into(out, true);
    if (out.capacity() < cap_before) {
      std::cout << "  [err] 目的地被换掉了（容量 " << cap_before << " → "
                << out.capacity() << "）⇒ 复用没成立\n";
      ok = false;
    }
    std::cout << "  [info] 复用缓冲容量 " << cap_before << " → " << out.capacity()
              << "，本次响应 " << out.size() << " 字节" << std::endl;
  }

  return ok;
}

int main(int argc, char** argv) {
  // 可选参数：测试名子串过滤，便于单条定位。
  const std::string filter = (argc > 1) ? argv[1] : std::string();
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"server_lifecycle", test_server_lifecycle},
    {"route_registration", test_route_registration},
    {"response_format", test_response_format},
    {"status_codes", test_status_codes},
    {"chunked_response", test_chunked_response},
    {"chunked_empty_terminated", test_chunked_empty_terminated},
    {"chunked_body_terminated", test_chunked_body_terminated},
    {"head_no_terminator", test_head_no_terminator},
#if UVCPP_ZLIB_ENABLE
    {"compress_enabled", test_server_compress_enabled},
    {"mime_exclusion", test_server_mime_exclusion},
    {"header_ops", test_response_header_ops},
    {"deferred_response_is_compressed", test_deferred_response_is_compressed},
#endif
    {"large_response_async", test_large_response_async},
    {"slow_client_does_not_block_loop", test_slow_client_does_not_block_loop},
    {"malformed_request_gets_400", test_malformed_request_gets_400},
    {"body_limit_returns_413", test_body_limit_returns_413},
    {"header_limit_returns_431", test_header_limit_returns_431},
    {"url_limit_returns_414", test_url_limit_returns_414},
    {"server_close_notifies", test_server_close_notifies},
    {"keepalive_sequential_responses", test_keepalive_sequential_responses},
    {"client_head_request", test_client_head_request},
    {"raw_data_hook", test_raw_data_hook},
    {"response_copy_keeps_deferred", test_response_copy_keeps_deferred},
    {"request_headers_do_not_accumulate", test_request_headers_do_not_accumulate},
    {"to_string_into_reuses_buffer", test_to_string_into_reuses_buffer},
  };
  for (const auto& t : tests) {
    if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) {
      continue;
    }
    // 显式 flush：这些测试会起真实网络连接，一旦挂住，缓冲住的进度输出会让人
    // 无从判断卡在哪一条。
    std::cout << "[web_http_server] " << t.name << std::endl;
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << std::endl;
    ok = r && ok;
  }
  std::cout << "[web_http_server] " << (ok ? "ALL PASS" : "FAIL") << std::endl;
  return ok ? 0 : 2;
}
#else
int main() { return 0; }
#endif
