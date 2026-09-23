/**
 * @file tests/functional/web_stream_response_func.cpp
 * @brief http 层的**流式响应**写路径：begin_stream / write_stream / end_stream。
 *
 * 为什么单独一个文件：本层的判据几乎全是"**没有**发生什么"——没有终止块、
 * 没有 body、没有压缩、没有多出来的字节。而 `send_response()` 那条路上这些
 * 东西**都会**发生，所以两组用例必须是**同一个响应形状在两个出口上的对照**，
 * 放在一起才看得出差别。
 *
 * 文件名同时命中 `tests/functional/CMakeLists.txt` 的 `web_.*\.cpp$` 过滤
 * （关掉 web 模块时整份文件被摘掉，而不是编译成一个恒绿的空壳）。
 *
 * 覆盖范围与边界的**如实说明**：
 *  - `head_include_body_false`（HEAD 分支必须走 `to_string(false)`）由
 *    `web_http_server_func.cpp` 的 `head_no_terminator` 覆盖（Step 1 已落地），
 *    本文件**不重复**。
 *  - `chunked_empty_terminated` / `chunked_body_terminated` 同理，也在那个文件里。
 *  - 本文件管的是**新增的流式写机制本身**：头部只出一次、块按序、done 的回调
 *    契约（恰好一次、顺序、连接没了要唤醒成 UV_ECANCELED）、end_stream 的关闭
 *    语义、以及流式响应**不被压缩**这道结构性守卫。
 */

#include <iostream>
#include <cstring>
#include <string>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE
#include <web/uvcpp_http_server.h>
#include <web/uvcpp_http_parser.h>
#include <web/uvcpp_http_common.h>
#include <net/uvcpp_tcp_client.h>
#include <req/uvcpp_shutdown.h>

#include "http_date_check.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace uvcpp;

// =========================================================================
// 测试装置
// =========================================================================

/** `sub` 在 `s` 里出现的次数。数"恰好一次"比数"至少一次"强。 */
static size_t count_substr(const std::string& s, const std::string& sub) {
  if (sub.empty()) return 0;
  size_t n = 0, pos = 0;
  while ((pos = s.find(sub, pos)) != std::string::npos) {
    ++n;
    pos += sub.size();
  }
  return n;
}

using SetupFn = std::function<void(uvcpp_http_server&)>;

struct TestServer {
  std::promise<int> port_promise;
  std::atomic<bool> stop{false};
  std::atomic<int> closed_conns{0};
  std::thread thread;

  int start(SetupFn setup) {
    thread = std::thread([this, setup]() {
      uvcpp_http_server server;
      server.on_connection_close([this](uvcpp_tcp_client*) {
        closed_conns.fetch_add(1);
      });
      if (setup) setup(server);
      server.bind("127.0.0.1", 0);

      sockaddr_in name;
      int namelen = sizeof(name);
      server.get_tcp_server()->get_tcp()->getsockname(
          reinterpret_cast<sockaddr*>(&name), &namelen);
      const int port = ntohs(name.sin_port);

      // **先 listen，再发布端口**：反过来调用方会拿到 ECONNREFUSED
      //（"已 bind、尚未 listen"的 socket 在内核里是拒连的）。
      server.listen();

      port_promise.set_value(port);
      while (!stop.load()) {
        server.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    return port_promise.get_future().get();
  }

  void shutdown() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }

  /** 等一个条件成立，最多 timeout_ms（用墙钟，不用拍数）。 */
  template <typename Pred>
  bool wait_for(Pred pred, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (pred()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
  }
};

/**
 * @brief 反复读，直到 `until` 出现（或读不出东西了），把读到的都追加进 `acc`。
 *
 * **不能只读一次**：`read_wait` 是"有数据就返回一次"，而一条流式响应会被拆成
 * 好几个 TCP 段。只读一次的话拿到什么完全看内核怎么分段 —— 用例会变成
 * 在测调度，而不是在测代码。更坏的是读到一半就返回，客户端随即析构关连接，
 * 服务端那几块还没写出去的就全变成失败，于是"服务端写坏了"和"用例读早了"
 * 长得一模一样。
 *
 * 返回 true 表示 `until` 出现了；`until` 为空时表示"读到读不出为止"。
 */
static bool pump_read(uvcpp_tcp_client& c, std::string& acc,
                      const std::string& until, int per_read_ms = 1000,
                      int max_reads = 64) {
  for (int i = 0; i < max_reads; ++i) {
    uvcpp_buf b;
    if (c.read_wait(b, per_read_ms) != 0) break;  // 超时或对端关了
    acc.append(b.get_const_data() ? b.get_const_data() : "", b.size());
    if (!until.empty() && acc.find(until) != std::string::npos) return true;
    if (until.empty() && b.size() == 0) break;
  }
  return until.empty() ? !acc.empty() : (acc.find(until) != std::string::npos);
}

/** 发一串字节、读回一串字节（同步客户端，自带 connect/write 重试）。 */
static bool raw_exchange(int port, const std::string& send, std::string& received,
                         const std::string& until = std::string()) {
  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;
  if (c.write_wait(send.c_str(), send.size(), 3000) != 0) return false;
  received.clear();
  if (!until.empty()) return pump_read(c, received, until);

  // 没有终止判据（`stream_head_only` 那条：报文就是头部块，之后什么都没有）：
  // 用短超时反复读，读到读不出为止。服务端随后会关连接，那时读就到头了。
  pump_read(c, received, std::string(), /*per_read_ms=*/250, /*max_reads=*/8);
  return !received.empty();
}

/** chunked 的一帧：`hex(len)\r\n` + data + `\r\n`。 */
static std::string chunk_frame(const std::string& data) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  size_t n = data.size();
  std::string digits;
  if (n == 0) {
    digits = "0";
  } else {
    while (n > 0) {
      digits.insert(digits.begin(), kHex[n & 0xF]);
      n >>= 4;
    }
  }
  out += digits;
  out += "\r\n";
  out += data;
  out += "\r\n";
  return out;
}

// =========================================================================
// 1. `stream_head_only` —— begin_stream 只出头部
//
// 这是本文件里**唯一能把"流式"与"普通 chunked 响应"分开**的那条判据。
//
// 响应里声明了 `transfer-encoding: chunked`，但 `begin_stream` 走的是
// `to_string(/*include_body=*/false)`，所以**一个字节的 body 都不发** ——
// 连终止块都不发（终止块是 chunked 的**帧**，不是 body 的附属品，所以它跟
// "body 为空"无关；`send_response` 那条路会照样补出来，这正是 X4 变异的形状）。
//
// handler 故意**一块都不写**就 end_stream：这样线上收到的东西**只能**是
// 头部块本身。若换成 `send_response(client, resp, false)`，同样的 handler
// 会多出 `0\r\n\r\n` 四个字节，本组当场变红。
// =========================================================================
static bool test_stream_head_only() {
  TestServer srv;
  const int port = srv.start([](uvcpp_http_server& s) {
    s.get("/head-only", [&s](uvcpp_http_request&, uvcpp_http_response& resp,
                             uvcpp_tcp_client* client) {
      resp.deferred = true;
      resp.status_code = http_status::OK;
      resp.set_content_type("text/plain");
      resp.set_header("transfer-encoding", "chunked");
      s.begin_stream(client, resp);
      s.end_stream(client, /*close_after=*/true);
    });
  });
  if (port <= 0) return false;

  std::string raw;
  const bool got =
      raw_exchange(port, "GET /head-only HTTP/1.1\r\nHost: x\r\n\r\n", raw);
  srv.shutdown();

  if (!got) {
    std::cout << "  [err] 没有收到任何响应\n";
    return false;
  }
  // 头部必须完整，否则下面几条"找不到"全是假绿。
  const size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos) {
    std::cout << "  [err] 头部块没有收尾\n";
    return false;
  }
  if (raw.find("HTTP/1.1 200") == std::string::npos) return false;
  if (raw.find("transfer-encoding: chunked") == std::string::npos) return false;

  // **整条报文正好是头部块**：空行之后不许有任何字节。
  if (raw.size() != head_end + 4) {
    std::cout << "  [err] 头部之后多出了 " << (raw.size() - head_end - 4)
              << " 个字节（流式出口不该发 body/终止块）\n";
    return false;
  }
  if (count_substr(raw, "0\r\n\r\n") != 0) return false;
  // chunked 与 content-length 互斥（RFC 7230 §3.3.2），流式出口也不该自作主张补它。
  if (raw.find("content-length:") != std::string::npos) {
    std::cout << "  [err] 流式响应里不该出现 content-length\n";
    return false;
  }

  // 流式头部走的是 `to_string(/*include_body=*/false)` 那条出口，与 `send_response`
  // 是两个不同的函数 —— 所以"整包响应有 Date"完全不能推出"这里也有"。
  // 取值判据与 h2 那两条共用（`http_date_check.h`），格式本身由新用例的 7 组
  // 硬编码向量钉住。
  {
    std::string why;
    if (!uvcpp_test::date_is_fresh_imf(uvcpp_test::raw_header_value(raw, "date"),
                                       &why)) {
      std::cout << "  [err] 流式头部里的 Date 不合格 —— " << why << "\n";
      return false;
    }
  }
  return true;
}

// =========================================================================
// 2. `stream_roundtrip` —— 三块按序发出、组帧由调用方负责、报文可被解析到完成
//
// 组帧是**调用方**的事（本层一个 hex 都不加），所以这里自己组 `hex\r\n data
// \r\n`，再断言对端解析出来的 body 与原文**逐字节相同**。这条同时钉住：
// 队列按序排空、end_stream 不追加任何东西、头部里的 chunked 声明与实际组帧
// 是自洽的（llhttp 能解析到 complete）。
// =========================================================================
static bool test_stream_roundtrip() {
  const std::string c0 = "data: 1\n\n";
  const std::string c1 = "data: 2\n\n";
  const std::string c2 = "data: 3\n\n";

  TestServer srv;
  const int port = srv.start([&](uvcpp_http_server& s) {
    s.get("/sse", [&s, c0, c1, c2](uvcpp_http_request&, uvcpp_http_response& resp,
                                   uvcpp_tcp_client* client) {
      resp.deferred = true;
      resp.status_code = http_status::OK;
      resp.set_content_type("text/event-stream");
      resp.set_header("transfer-encoding", "chunked");
      s.begin_stream(client, resp);
      s.write_stream(client, chunk_frame(c0));
      s.write_stream(client, chunk_frame(c1));
      s.write_stream(client, chunk_frame(c2));
      s.write_stream(client, "0\r\n\r\n");  // 终止块也由调用方组
      s.end_stream(client, /*close_after=*/true);
    });
  });
  if (port <= 0) return false;

  std::string raw;
  const bool got = raw_exchange(port, "GET /sse HTTP/1.1\r\nHost: x\r\n\r\n", raw,
                                "0\r\n\r\n");
  srv.shutdown();

  if (!got) {
    std::cout << "  [err] 报文里没有终止块；收到 " << raw.size() << " 字节：["
              << raw << "]\n";
    return false;
  }
  if (raw.find("transfer-encoding: chunked") == std::string::npos) return false;

  // 三块的组帧必须在，且**顺序**正确。
  size_t p0 = raw.find(chunk_frame(c0));
  size_t p1 = raw.find(chunk_frame(c1));
  size_t p2 = raw.find(chunk_frame(c2));
  if (p0 == std::string::npos || p1 == std::string::npos || p2 == std::string::npos)
    return false;
  if (!(p0 < p1 && p1 < p2)) {
    std::cout << "  [err] 三块的次序不对\n";
    return false;
  }
  // 终止块恰好一次、且在报文末尾。
  if (count_substr(raw, "0\r\n\r\n") != 1) return false;
  const std::string tail = "0\r\n\r\n";
  if (raw.size() < tail.size()) return false;
  if (raw.compare(raw.size() - tail.size(), tail.size(), tail) != 0) return false;

  // 交给 llhttp：报文必须能解析到 complete。这一条比字符串匹配强 ——
  // 它证明组帧与头部声明是自洽的。
  uvcpp_http_parser parser(http_parser_mode::PARSE_RESPONSE);
  std::string body;
  parser.set_on_body([&body](const char* d, size_t n) { body.append(d, n); });
  parser.execute(raw.c_str(), raw.size());
  parser.finish();
  if (!parser.is_complete()) {
    std::cout << "  [err] 响应报文不完整（llhttp 没到 complete）\n";
    return false;
  }
  if (parser.has_error()) return false;
  if (parser.get_status_code() != http_status::OK) return false;

  if (body != c0 + c1 + c2) {
    std::cout << "  [err] 解析出来的 body 与原文不同，长度 " << body.size()
              << " vs " << (c0 + c1 + c2).size() << "\n";
    return false;
  }
  return true;
}

// =========================================================================
// 3. `write_done_contract` —— done 的回调契约：恰好一次、按序、status 为 0
//
// 这不是"顺手加的一条"：`queued_write::done` 是 Phase 3c 新增的机制，
// 上层（`send_file` 的传输对象）**靠它**知道"这一块出去了、可以放下一个了"。
// done 少调一次 → 传输永久挂起；多调一次 → 重复提交。两种都不会让别的用例
// 变红，所以必须有这一条。
//
// 顺序也在判据里：done 按入队序触发（写队列是 FIFO），乱序会让 send_file
// 的窗口算术错位。
// =========================================================================
static bool test_write_done_contract() {
  struct probe_t {
    std::mutex m;
    std::vector<int> order;    // 依次落下的块序号
    std::vector<int> status;   // 各自的 status
    probe_t() {}
  };
  std::shared_ptr<probe_t> pr(new probe_t());

  TestServer srv;
  const int port = srv.start([&](uvcpp_http_server& s) {
    s.get("/done", [&s, pr](uvcpp_http_request&, uvcpp_http_response& resp,
                            uvcpp_tcp_client* client) {
      resp.deferred = true;
      resp.status_code = http_status::OK;
      resp.set_content_type("text/plain");
      resp.set_header("transfer-encoding", "chunked");
      s.begin_stream(client, resp);
      for (int i = 0; i < 3; ++i) {
        // 每块都够大，好让"三块同时在队列里"这件事真的发生 —— 否则
        // done 会在下一次 write_stream 之前就同步落地，测不到排队。
        const std::string payload(4096, static_cast<char>('a' + i));
        s.write_stream(client, chunk_frame(payload),
                       [pr, i](int st) {
                         std::lock_guard<std::mutex> lk(pr->m);
                         pr->order.push_back(i);
                         pr->status.push_back(st);
                       });
      }
      s.write_stream(client, "0\r\n\r\n");
      s.end_stream(client, /*close_after=*/true);
    });
  });
  if (port <= 0) return false;

  std::string raw;
  const bool got = raw_exchange(port, "GET /done HTTP/1.1\r\nHost: x\r\n\r\n", raw,
                                "0\r\n\r\n");
  const bool settled = srv.wait_for(
      [&]() {
        std::lock_guard<std::mutex> lk(pr->m);
        return pr->order.size() >= 3;
      },
      3000);
  srv.shutdown();

  if (!got) return false;
  (void)settled;

  std::lock_guard<std::mutex> lk(pr->m);
  if (pr->order.size() != 3) {
    std::cout << "  [err] done 触发了 " << pr->order.size() << " 次，期望 3\n";
    return false;
  }
  for (size_t i = 0; i < 3; ++i) {
    if (pr->order[i] != static_cast<int>(i)) {
      std::cout << "  [err] done 的次序不对：第 " << i << " 个是 " << pr->order[i]
                << "\n";
      return false;
    }
    if (pr->status[i] != 0) {
      std::cout << "  [err] 第 " << i << " 块的 status 是 " << pr->status[i]
                << "（连接好着，必须是 0）\n";
      return false;
    }
  }
  return true;
}

// =========================================================================
// 4. `end_stream_close` —— end_stream(true) 在队列排空之后关连接
//
// 与 `stream_head_only` 是**同一个入口的两个分支**：那边 close_after=true
// 且队列空，这边 close_after=false —— 后者必须**不关**。只测一个方向的话，
// 一个"永远关"或"永远不关"的实现都能蒙混过关。
//
// 判据用服务端自己的 `on_connection_close` 计数，而不是客户端读到的 EOF：
// 前者是框架层的账，后者还混着"读超时"的可能。
// =========================================================================
static bool test_end_stream_close() {
  // (a) close_after = true → 连接被关
  {
    TestServer srv;
    const int port = srv.start([](uvcpp_http_server& s) {
      s.get("/close", [&s](uvcpp_http_request&, uvcpp_http_response& resp,
                           uvcpp_tcp_client* client) {
        resp.deferred = true;
        resp.status_code = http_status::OK;
        resp.set_header("transfer-encoding", "chunked");
        s.begin_stream(client, resp);
        s.write_stream(client, chunk_frame("bye"));
        s.write_stream(client, "0\r\n\r\n");
        s.end_stream(client, /*close_after=*/true);
      });
    });
    if (port <= 0) return false;
    std::string raw;
    const bool got = raw_exchange(port, "GET /close HTTP/1.1\r\nHost: x\r\n\r\n", raw,
                                  "0\r\n\r\n");
    const bool closed = srv.wait_for([&]() { return srv.closed_conns.load() > 0; }, 3000);
    srv.shutdown();
    if (!got) return false;
    if (raw.find("bye") == std::string::npos) return false;
    if (!closed) {
      std::cout << "  [err] close_after=true 之后连接没有被关\n";
      return false;
    }
  }

  // (b) close_after = false → 连接留着，同一条连接上还能再发一个请求
  {
    TestServer srv;
    const int port = srv.start([](uvcpp_http_server& s) {
      s.get("/keep", [&s](uvcpp_http_request&, uvcpp_http_response& resp,
                          uvcpp_tcp_client* client) {
        resp.deferred = true;
        resp.status_code = http_status::OK;
        resp.set_header("transfer-encoding", "chunked");
        s.begin_stream(client, resp);
        s.write_stream(client, chunk_frame("stay"));
        s.write_stream(client, "0\r\n\r\n");
        s.end_stream(client, /*close_after=*/false);
      });
      s.get("/ping", [](uvcpp_http_request&, uvcpp_http_response& resp,
                        uvcpp_tcp_client*) {
        resp = uvcpp_http_response::ok("pong", 4, "text/plain");
      });
    });
    if (port <= 0) return false;

    uvcpp_tcp_client c;
    if (c.connect_wait("127.0.0.1", port, 3000) != 0) {
      srv.shutdown();
      return false;
    }
    const std::string req1 = "GET /keep HTTP/1.1\r\nHost: x\r\n\r\n";
    const std::string req2 = "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n";
    if (c.write_wait(req1.c_str(), req1.size(), 3000) != 0) {
      srv.shutdown();
      return false;
    }
    std::string s1;
    const bool r1 = pump_read(c, s1, "0\r\n\r\n");
    if (c.write_wait(req2.c_str(), req2.size(), 3000) != 0) {
      srv.shutdown();
      return false;
    }
    std::string s2;
    const bool r2 = pump_read(c, s2, "pong");
    srv.shutdown();

    if (!r1 || !r2) {
      std::cout << "  [err] close_after=false 之后连接不可用了\n";
      return false;
    }
    if (s1.find("stay") == std::string::npos) {
      std::cout << "  [err] 第一个流式响应的体不完整：[" << s1 << "]\n";
      return false;
    }
    if (s2.find("pong") == std::string::npos) {
      std::cout << "  [err] 第二个请求没拿到响应：[" << s2 << "]\n";
      return false;
    }
  }
  return true;
}

// =========================================================================
// 5. `queued_done_cancelled_on_close` —— 连接没了，**每一个**提交过的 done 都必须结算
//
// 一个**永远不回调**的 done 会让等它的人（`send_file` 的传输对象）永久挂起，
// 而那条路在真实网络里**必然**发生（对端中途断开）。契约是：连接关掉时，
// 每一块**已经被 `write_stream` 接受**的 done 都要恰好调一次，status 非 0。
//
// 这条契约有**两个**必须都在位的机制，而且它们的失败形状都是"静默挂起"：
//
//   ① **队列里**那几块 —— 对端断开走的是 `set_on_close` → `remove_ctx`，
//      **不是** `close_connection`。少唤醒一处，它们就永远不响（variation: X-Q）。
//   ② **在途**那一块 —— 它的 done 已经交给网络层的写完成回调了，而那条回调
//      有一条早返回 `if (!token_alive(life)) return;`；服务端连接恰恰是被
//      tcp_server 的 close manager 删掉的，于是对端在写入途中断开时，这一块的
//      done 连一次都不会响。兜底只能来自 `conn_ctx::inflight`（variation: X-I）。
//
// 判据因此是**聚合**的：**每一个提交成功的块恰好结算一次**。只看"某一块收到了
// UV_ECANCELED"的话，①漏了但②恰好响一次的实现照样能过。
//
// ---------------------------------------------------------------------------
// 第一版的判据是**错的**，改的原因值得写下来（`assert-test-preconditions`）：
// 它做的是"一个 8 MiB 块 + 一个 tail 块"，然后断言 tail 必然还在队列里、
// 8 MiB 必然在途。**这两条前提它一条都没断言**，于是 8 MiB 被内核缓冲整个吃掉
// （实测 status=0、`close_connection dropped=0`）时，用例报出来的是
// "第一块写失败了，status 不该是 0" —— 一句描述**症状**而非缺陷的话，
// 而且 20 次里只红 1 次。
//
// 现在改成 64 × 256 KiB，并且把三条前提写成断言：
//   - `submitted + refused == kBlocks`（handler 有机会提交完）
//   - `settled_at_close < kBlocks`（关闭那一刻确实还有块没写完 —— 否则
//     "被打断"根本没发生，本组测的是别的东西）
//   - `done_cancel >= 1`（确实有块是被**框架取消**的，而不是全都自己写失败）
// 少了这几条，一个"永远不唤醒"的实现能在大多数轮次里蒙混过关。
//
// 客户端那一侧也必须**真的断开**：`close()` 只是提交一次 `uv_close`，
// 不泵客户端循环的话完成回调永远不来、FIN 也永远发不出去，服务端那边压根
// 看不到断开。第一版就是栽在这里（`wait_for` 只按墙钟轮询，**不泵任何循环**）。
//
// 而断开的**方式**同样是要紧的，而且**两种方式各走一条不同的唤醒路**：
//
//   | 对端怎么走 | 服务端内核 → 框架 | 唤醒队列里那些块的是 |
//   |---|---|---|
//   | 优雅半关（FIN） | 读到 EOF → `fire_close_callbacks()` | `remove_ctx` |
//   | 直接 `close()`（RST） | 在途写**当场失败**（status 非 0） | `close_connection` |
//
// 这两条路各有**一个自己的**队列唤醒循环，而且**都必须存在**：
// `remove_ctx` 那条管对端优雅走，`close_connection` 那条管写失败。少一条，
// 那条路上的块就永久挂起。
//
// **这是实测出来的，不是推出来的**（`assert-test-preconditions`）。探针打在
// `close_connection` 的队列唤醒循环上，统计它看到的 `dropped` / `with_done`：
//
//   - 只跑优雅半关：`close_connection dropped=0 with_done=0` ×5 次 —— 它确实
//     被调用，但**队列永远是空的**（FIN 那条路队列是在 `remove_ctx` 里清的）。
//     后果：把 `close_connection` 的唤醒循环整个删掉，本组**照样全绿**。
//   - 同一组改用 `c.close()`（RST）：`close_connection dropped=62 with_done=62`
//     —— 一次就看到了 62 块。
//
// 也就是说"抓不住"**不是等价变异，是真实的覆盖缺口**：那段代码是可达的，
// 只是原来的构造只走了两条路里的一条。所以本组现在**两种方式各跑一轮**，
// 契约逐轮断言。
//
// 客户端那一侧也必须**真的断开**：`close()` 只是提交一次 `uv_close`，
// 不泵客户端循环的话完成回调永远不来、FIN 也永远发不出去，服务端那边压根
// 看不到断开。第一版就是栽在这里（`wait_for` 只按墙钟轮询，**不泵任何循环**）。
// =========================================================================

// 64 × 256 KiB = 16 MiB：远超任何默认 socket 发送缓冲，写不完是必然的。
// 用多个中等块而不是一个大块，是因为"队列非空"这件事需要**块数**来保证，
// 而"单个大块能否被内核吃掉"取决于缓冲大小，不是稳定的事实。
static const int kQdBlocks = 64;
static const size_t kQdPayload = 256u * 1024u;

struct queued_done_probe {
  std::mutex m;
  int submitted;   // write_stream 接受了、done 归框架管的块数
  int refused;     // write_stream 当场拒绝的块数（这些 done **不该**被调）
  int calls;       // done 的总调用次数
  int nonzero;     // 其中 status 非 0 的次数
  int cancel;      // 其中恰好是 UV_ECANCELED 的次数
  uint64_t submitted_mask;  // 提交成功的块下标（基准）
  uint64_t seen;            // 结算过的块下标（必须与基准**逐一相等**）
  queued_done_probe()
      : submitted(0), refused(0), calls(0), nonzero(0), cancel(0),
        submitted_mask(0), seen(0) {}
};

/** 探针的**无锁快照** —— 断言在锁外做，免得把 print 也圈进临界区。 */
struct queued_done_snapshot {
  int submitted, refused, calls, nonzero, cancel;
  uint64_t submitted_mask, seen;
  bool settled;  // 等结算的窗口内，全部提交过的块都收到了回调
  queued_done_snapshot()
      : submitted(0), refused(0), calls(0), nonzero(0), cancel(0),
        submitted_mask(0), seen(0), settled(false) {}
};

// 跑**一轮**场景。`abrupt` 决定对端**怎么走**：false = `uv_shutdown`（FIN）、
// true = `close()`（RST）。返回值只表示**装置**是否成立（连上 / 拿到头部 /
// 提交跑完）；契约断言由调用方在快照上做 —— 装置失败与契约失败是两件事，
// 混在一起会让"用例自己没跑起来"看起来像"被测代码坏了"。
static bool run_queued_done_scenario(
    bool abrupt, const std::shared_ptr<queued_done_probe>& pr,
    queued_done_snapshot* snap, int* settled_at_close) {
  const int kBlocks = kQdBlocks;
  const size_t kPayload = kQdPayload;

  TestServer srv;
  const int port = srv.start([&](uvcpp_http_server& s) {
    s.get("/big", [&s, pr, kBlocks, kPayload](
                      uvcpp_http_request&, uvcpp_http_response& resp,
                      uvcpp_tcp_client* client) {
      resp.deferred = true;
      resp.status_code = http_status::OK;
      resp.set_header("transfer-encoding", "chunked");
      s.begin_stream(client, resp);

      const std::string payload(kPayload, 'x');
      for (int i = 0; i < kBlocks; ++i) {
        int rc = s.write_stream(client, chunk_frame(payload), [pr, i](int st) {
          std::lock_guard<std::mutex> lk(pr->m);
          pr->calls++;
          if (st != 0) pr->nonzero++;
          if (st == UV_ECANCELED) pr->cancel++;
          pr->seen |= (uint64_t(1) << i);
        });
        std::lock_guard<std::mutex> lk(pr->m);
        if (rc == 0) {
          pr->submitted++;
          pr->submitted_mask |= (uint64_t(1) << i);
        } else {
          // 被拒绝的块，它的 done **不应该**被调用（契约的另一半）。
          pr->refused++;
        }
      }
      // 故意不 end_stream：连接要靠对端断开（或写失败）来收尾。
    });
  });
  if (port <= 0) return false;

  *settled_at_close = 0;
  int settled_before_disconnect = 0;
  {
    uvcpp_tcp_client c;
    if (c.connect_wait("127.0.0.1", port, 3000) != 0) {
      srv.shutdown();
      return false;
    }
    const std::string req = "GET /big HTTP/1.1\r\nHost: x\r\n\r\n";
    if (c.write_wait(req.c_str(), req.size(), 3000) != 0) {
      srv.shutdown();
      return false;
    }
    uvcpp_buf head;
    // 只读一次：拿到头部就说明 handler 跑到了 begin_stream。
    if (c.read_wait(head, 3000) != 0) {
      srv.shutdown();
      return false;
    }

    // **等 handler 把所有块提交完**再断开。不等的话，"提交到一半就被打断"
    // 会让 `submitted` 是个随机数，三条前提断言全部变成在测调度。
    if (!srv.wait_for(
            [&]() {
              std::lock_guard<std::mutex> lk(pr->m);
              return pr->submitted + pr->refused == kBlocks;
            },
            5000)) {
      srv.shutdown();
      return false;
    }
    {
      std::lock_guard<std::mutex> lk(pr->m);
      settled_before_disconnect = pr->calls;
    }

    if (abrupt) {
      // RST：接收缓冲里还压着没读走的数据，所以这个 `close()` 会让内核发
      // **RST**。服务端那条在途写**当场失败**，完成回调拿到真实 status →
      // `close_connection` → 队列里剩下的块由**它**唤醒。
      c.close();
    } else {
      // FIN：`uv_shutdown` 只关**写**方向 —— FIN 发出去，socket 不关、接收
      // 缓冲一个字节不动（客户端也不读，对端窗口一直是关的）。于是服务端读到
      // 的是 **EOF**（不是错误），走 `on_close` → `remove_ctx`；紧接着
      // tcp_server 的 close manager 把客户端对象 **delete** 掉，那条在途写
      // 此后再也拿不到一次"令牌还活着"的完成回调 —— 它只能靠
      // `conn_ctx::inflight` 兜底结算。队列里那些块则由 `remove_ctx` 唤醒。
      //
      // 实测（这就是非要这一轮不可的理由）：把 `ctx.inflight = wd;` 删掉，
      // **只跑这一轮的写法 5/5 全绿** —— RST 那条路会把在途块交给网络层的正常
      // 回调结算，`inflight` 那份兜底压根用不上。
      //
      // 泄漏是**故意**的：`uv_shutdown` 的请求对象必须活到回调跑完，而在回调里
      // 释放自己正是本仓记过四次的"删除自身回调"。两种方式各跑一次，代价是
      // 一块请求对象。
      uvcpp_shutdown* sd = new uvcpp_shutdown();
      if (sd->shutdown(c.get_tcp(), [](uvcpp_shutdown*, int) {}) != 0) {
        srv.shutdown();
        return false;
      }
    }

    // **把客户端循环真正跑起来**，让 FIN / RST 真的发出去。
    // 不泵循环的话服务端永远看不到断开，本组的全部前提都不成立。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < deadline) {
      c.get_loop()->run(UV_RUN_NOWAIT);
      {
        std::lock_guard<std::mutex> lk(pr->m);
        if (pr->calls >= pr->submitted) break;  // 目标达成，提前收工
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // 结算是在**服务端**循环上发生的（对端断开 → remove_ctx / 写失败回调），
  // 所以要等服务端那一侧也跑完，而不是靠客户端泵循环的时长。
  snap->settled = srv.wait_for(
      [&]() {
        std::lock_guard<std::mutex> lk(pr->m);
        return pr->calls >= pr->submitted;
      },
      5000);
  srv.shutdown();

  {
    std::lock_guard<std::mutex> lk(pr->m);
    snap->submitted      = pr->submitted;
    snap->refused        = pr->refused;
    snap->calls          = pr->calls;
    snap->nonzero        = pr->nonzero;
    snap->cancel         = pr->cancel;
    snap->submitted_mask = pr->submitted_mask;
    snap->seen           = pr->seen;
  }
  // 断开**之前**已经结算了几块。它进前提断言（必须 < submitted），用来证明
  // 这一轮确实考到了"断开时才结算"的那一部分，而不是断开前就全跑完了。
  *settled_at_close = settled_before_disconnect;
  return true;
}

// 契约断言，**每一轮**都跑。`label` 进错误信息 —— 两种断开方式的失败形状
// 一模一样（都是"少唤醒了几块"），不带上标签就分不清是哪一轮红的。
static bool check_queued_done(const char* label, const queued_done_snapshot& p,
                              int settled_at_close) {
  const int kBlocks = kQdBlocks;

  // --- 先看有没有**永久挂起**（这是最坏的一种：等它的人一直等下去）---
  if (!p.settled) {
    std::cout << "  [err] " << label << ": 等结算超时 —— done 触发了 " << p.calls
              << " 次，提交了 " << p.submitted
              << " 次。有块提交过却从未被唤醒，等它的人（send_file 的传输对象）"
                 "会一直等下去\n";
    return false;
  }

  // --- 前提（三条缺一不可，缺了就说明本组测的不是它想测的东西）---
  if (p.submitted + p.refused != kBlocks) {
    std::cout << "  [err] " << label << ": 提交账目对不上：submitted="
              << p.submitted << " refused=" << p.refused << "，合计该是 "
              << kBlocks << "\n";
    return false;
  }
  if (p.submitted < 2) {
    std::cout << "  [err] " << label << ": 只提交成功 " << p.submitted
              << " 块 —— 连排队的形状都构不成\n";
    return false;
  }
  if (settled_at_close >= p.submitted) {
    std::cout << "  [err] " << label << ": 断开之前 " << settled_at_close << " / "
              << p.submitted
              << " 块就已经写完了 —— 16 MiB 不该被缓冲吃干净，"
                 "本组要考的被打断这个前提不成立\n";
    return false;
  }

  // --- 契约 ---
  if (p.calls != p.submitted) {
    std::cout << "  [err] " << label << ": done 触发了 " << p.calls
              << " 次，提交了 " << p.submitted
              << " 次 —— 每一个提交过的块都该恰好结算一次（少了就是永久挂起）\n";
    return false;
  }
  if (p.seen != p.submitted_mask) {
    std::cout << "  [err] " << label << ": 结算的块与提交的块对不上 —— 提交 0x"
              << std::hex << p.submitted_mask << "，结算 0x" << p.seen
              << std::dec << "（少了是漏唤醒，多了是重复结算）\n";
    return false;
  }
  if (p.nonzero == 0) {
    std::cout << "  [err] " << label << ": 所有 done 的 status 都是 0 —— "
                 "连接都断了，不可能每块都写成功\n";
    return false;
  }
  if (p.cancel < 1) {
    std::cout << "  [err] " << label << ": 没有任何一块是被框架取消的"
                 "（UV_ECANCELED）—— 队列/在途里那些块没有走唤醒路径，"
                 "本组的机制一条都没被考到\n";
    return false;
  }
  return true;
}

static bool test_queued_done_cancelled_on_close() {
  // **两种断开方式各跑一轮**（理由与实测数字见本组开头那段注释）：
  // 第 0 轮优雅半关（FIN）走 `remove_ctx` 的唤醒循环，第 1 轮 RST 走
  // `close_connection` 的唤醒循环。只跑一轮的话，另一条路上的唤醒循环
  // 整个删掉都不会被发现。
  static const char* kLabels[2] = {"FIN", "RST"};
  for (int mode = 0; mode < 2; ++mode) {
    std::shared_ptr<queued_done_probe> pr(new queued_done_probe());
    queued_done_snapshot snap;
    int settled_at_close = 0;
    if (!run_queued_done_scenario(mode == 1, pr, &snap, &settled_at_close)) {
      std::cout << "  [err] " << kLabels[mode]
                << ": 装置没跑起来（连不上 / 没拿到头部 / handler 没提交完）\n";
      return false;
    }
    if (!check_queued_done(kLabels[mode], snap, settled_at_close)) return false;
  }
  return true;
}

// =========================================================================
// 6. `stream_not_compressed` —— 流式响应**不压缩**（结构性守卫）
//
// 这一刻 body 还不存在，而 `apply_compression` 是按 `content_type()` 判定的：
// 让它跑一遍，响应头上就会多出一个 `content-encoding: gzip` —— 那是**声明了
// 一件没发生的事**，客户端会拿着 gzip 解压器去解 chunked 帧。
//
// 守卫是**结构性**的：`begin_stream` 那条入口压根不调用 `apply_compression`。
// 配套的第二道（`apply_compression` 里见到 `transfer-encoding` 就早返回）
// 是防御性的 —— 这个用例打的是第一道，所以把 `apply_compression` 加回
// `begin_stream` 会当场变红。
//
// 对照组：同一条连接上再发一个**普通**请求，它必须**照旧被压缩** —— 少了它，
// 一个"全局关掉压缩"的实现能让主断言全绿。
// =========================================================================
#if UVCPP_ZLIB_ENABLE
static bool test_stream_not_compressed() {
  const std::string big(2000, 'z');

  TestServer srv;
  const int port = srv.start([&](uvcpp_http_server& s) {
    s.set_compression_enabled(true);
    s.set_compress_min_body_size(0);
    s.get("/stream", [&s](uvcpp_http_request&, uvcpp_http_response& resp,
                          uvcpp_tcp_client* client) {
      resp.deferred = true;
      resp.status_code = http_status::OK;
      resp.set_content_type("text/plain");
      resp.set_header("transfer-encoding", "chunked");
      s.begin_stream(client, resp);
      s.write_stream(client, chunk_frame("live"));
      s.write_stream(client, "0\r\n\r\n");
      s.end_stream(client, /*close_after=*/true);
    });
    s.get("/plain", [big](uvcpp_http_request&, uvcpp_http_response& resp,
                          uvcpp_tcp_client*) {
      resp = uvcpp_http_response::ok(big.c_str(), big.size(), "text/plain");
    });
  });
  if (port <= 0) return false;

  std::string streamed;
  const std::string gz_req =
      "GET /stream HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n";
  const bool got_stream = raw_exchange(port, gz_req, streamed);

  std::string plain;
  const bool got_plain =
      raw_exchange(port, "GET /plain HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n",
                   plain);
  srv.shutdown();

  if (!got_stream || !got_plain) return false;

  if (streamed.find("content-encoding") != std::string::npos) {
    std::cout << "  [err] 流式响应被打上了 content-encoding（body 还不存在）\n";
    return false;
  }
  if (streamed.find("live") == std::string::npos) return false;

  // 对照组：普通响应必须**照旧**被压缩，否则上面那条是"压缩整个坏了"的假绿。
  if (plain.find("content-encoding: gzip") == std::string::npos) {
    std::cout << "  [err] 对照组的普通响应没有被压缩 —— 主断言因此不成立\n";
    return false;
  }
  if (plain.find(big) != std::string::npos) {
    std::cout << "  [err] 对照组的 body 是明文（「压缩没生效」）\n";
    return false;
  }
  return true;
}
#endif  // UVCPP_ZLIB_ENABLE

int main(int argc, char** argv) {
  const std::string filter = (argc > 1) ? argv[1] : std::string();
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"stream_head_only", test_stream_head_only},
    {"stream_roundtrip", test_stream_roundtrip},
    {"write_done_contract", test_write_done_contract},
    {"end_stream_close", test_end_stream_close},
    {"queued_done_cancelled_on_close", test_queued_done_cancelled_on_close},
#if UVCPP_ZLIB_ENABLE
    {"stream_not_compressed", test_stream_not_compressed},
#endif
  };
  for (const auto& t : tests) {
    if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) {
      continue;
    }
    std::cout << "[web_stream_response] " << t.name << std::endl;
    const bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << std::endl;
    ok = r && ok;
  }
  std::cout << "[web_stream_response] " << (ok ? "ALL PASS" : "FAIL") << std::endl;
  return ok ? 0 : 2;
}
#else
int main() { return 0; }
#endif
