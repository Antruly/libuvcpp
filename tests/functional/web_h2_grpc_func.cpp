/**
 * @file tests/functional/web_h2_grpc_func.cpp
 * @brief gRPC 在**明文 h2** 上的整条往返：`src/grpc/uvcpp_grpc_server.*` 那三段。
 *
 * 文件名里的 `web_` 与 `h2_` 都是**必须的**，两个前缀各自把住一个开关：
 *   - `web_` —— 服务端那一半的参数全是 web 层的类型，web 关掉时它根本不在库里；
 *   - `h2_`  —— gRPC 的三段结构里，第三段（尾随头）只有 h2 有。h1 上
 *     `send_trailers()` 返 `UV_ENOTSUP`，这条路**不存在**，用例该表现为
 *     "不存在"而不是"跳过"（`tests/functional/CMakeLists.txt` 开头那段注释里的
 *     老坑：没测过 ≠ 测过）。
 *
 * 编解码那一半（帧、varint、字段）在 `grpc_codec_func.cpp`，那条**不**受这两个
 * 开关管，任何树上都跑 —— 两半分开挂，是因为它们的依赖面本来就不同。
 *
 * 判据的骨架：一条 unary 应答在线上是 HEADERS → DATA → HEADERS 三段，所以
 * 判据也必须**分三段各钉一次**，只钉"客户端收到了 END_STREAM"是**假绿** ——
 * 把处理函数里的 `send_trailers()` 换成 `end_stream()` 那一条照样绿
 * （两者都带 END_STREAM），而 gRPC 要的正是"END_STREAM 捎带一段字段"。
 * 所以：
 *   1. 首部块：`content-type: application/grpc`，且 **`grpc-status` 不在里面**；
 *   2. DATA：响应体**逐字节**等于手算出来的那一帧（不是"往返相等"）；
 *   3. 尾随块：`on_response_end` 那一刻的头列表里有 `grpc-status: 0`。
 * 第 3 条是尾随头真的到了的**唯一**证据：它既不是首部、也不是 body。
 *
 * 装置：服务端跑在后台线程、绑端口 0；客户端用**阻塞** `uvcpp_tcp_client` 配
 * 一个客户端 h2 会话（`uvcpp_h2_session(false)`）真字节往返。走真 socket 是必要
 * 的 —— 这条路要穿过前言判定（h2c）、llhttp 的让位、会话的重新武装、DATA 的
 * 拼装、以及尾随头的第二段头块，任何一处没接上都表现为超时或丢字段。
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_NGHTTP2_ENABLE

#include <grpc/uvcpp_grpc.h>
#include <grpc/uvcpp_grpc_server.h>
#include <net/uvcpp_tcp_client.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

#include <http2/uvcpp_h2_session.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

/// 判据与诊断都从**两条线程**上写：处理函数跑在服务端线程上，它的诊断与主线程
/// 的判据会交错。不加锁的话输出会互相插进对方的行里 —— 而这份输出的全部用处
/// 就是"红的时候看清楚发生了什么"，插花了正好毁掉它。
std::mutex g_out_mtx;

void check(bool cond, const std::string& what) {
  std::lock_guard<std::mutex> g(g_out_mtx);
  if (cond) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void info(const std::string& what) {
  std::lock_guard<std::mutex> g(g_out_mtx);
  std::cout << "  [info] " << what << std::endl;
}

std::string hex(const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  std::string        out;
  out.reserve(s.size() * 2);
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    out += kHex[c >> 4];
    out += kHex[c & 0x0F];
  }
  return out;
}

/// 头列表里**有没有**这个名字。
///
/// 与 `http_get_header(...) != ""` 的差别就是空值字段：那个对"没发"与"发了个
/// 空值"给同一个答案，而 `grpc-message: ""` 是个正常形状。
bool has_named(const http_headers& hs, const std::string& name) {
  for (size_t i = 0; i < hs.size(); ++i) {
    if (hs[i].name == name) return true;
  }
  return false;
}

/// 一段字节的十六进制（带空格、最多 24 字节），排错时看"到底发出去的是什么"。
std::string hexsp(const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  std::string        out;
  const size_t       n = s.size() < 24 ? s.size() : 24;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (i != 0) out += ' ';
    out += kHex[c >> 4];
    out += kHex[c & 0x0F];
  }
  if (s.size() > n) out += " ...";
  return out;
}

std::string bytes_of(const char* p, size_t n) { return std::string(p, n); }

// =========================================================================
// 服务端：后台线程 + 端口 0 + 明文 h2c
// =========================================================================

/**
 * @brief 处理函数：把一条 unary 的 grpc-sum 请求解出来、算和、回一帧。
 *
 * 这就是"用框架的文档化高层 API"长什么样：解析一个调用、发一个应答，
 * 帧的 5 字节前缀与 varint 一个字都不用自己写。
 */
void handle_sum(uvcpp_http_server& server, uvcpp_http_request& req,
                uvcpp_http_response& resp, uvcpp_tcp_client* client) {
  std::string body_bytes, err;
  grpc_status st = grpc_status::UNKNOWN;
  if (grpc_parse_unary_body(req.body, body_bytes, st, err) != 0) {
    // 不是一条合法的 unary 请求体：回一条只有尾随头的失败应答（Trailers-Only）。
    const int rc = grpc_send_status(server, client, resp, st, err);
    info("失败路径 grpc_send_status() = " + std::to_string(rc) + "：" + err);
    return;
  }

  int32_t a = 0, b = 0;
  // 字段缺省就是 0（protobuf 的语义），所以 `UV_ENOENT` 在这里不区分处理 ——
  // 但**必须**把返回值收下，否则"字段不在"和"字段是 0"在这行代码上长得一样。
  const int ra = grpc_field_get_int32(body_bytes.data(), body_bytes.size(), 1, a);
  const int rb = grpc_field_get_int32(body_bytes.data(), body_bytes.size(), 2, b);
  if (ra == UV_EINVAL || rb == UV_EINVAL) {
    grpc_send_status(server, client, resp, grpc_status::INVALID_ARGUMENT,
                     "请求体里的字段解不出来");
    return;
  }

  std::string out;
  grpc_field_put_int32(out, 1, a + b);

  const int rc = grpc_send_unary(server, client, resp, out);
  if (rc != 0) info("grpc_send_unary() = " + std::to_string(rc));
}

struct TestServer {
  std::thread       th;
  std::atomic<bool> stop{false};
  std::promise<int> port_promise;

  int start() {
    th = std::thread([this] {
      uvcpp_http_server server;

      // h2 与 h2c 两块都要开：`h2c_enabled()` 的判据是「h2 开着**且** h2c 开着」
      // —— h2c 是 h2 的一条腿，不是独立协议。
      server.set_http2_enabled(true);
      server.set_h2c_enabled(true);

      server.post("/grpc.test.Sum/Sum",
                  [&server](uvcpp_http_request& req, uvcpp_http_response& resp,
                            uvcpp_tcp_client* client) {
                    handle_sum(server, req, resp, client);
                  });

      // h1 那一侧：同一句 `grpc_send_unary()` 必须**明着失败**。判据只能是返回值
      // —— 处理函数跑在服务端线程上，它自己断言失败时客户端看到的是超时，什么
      // 都说明不了。所以把 rc 编进 body，让客户端那一侧能读到（第 4 轮）。
      server.post("/grpc-h1", [&server](uvcpp_http_request& req,
                                        uvcpp_http_response& resp,
                                        uvcpp_tcp_client* client) {
        std::string out;
        grpc_field_put_int32(out, 1, 3);
        const int   rc = grpc_send_unary(server, client, resp, out);
        const std::string s = std::to_string(rc);
        resp = uvcpp_http_response::ok(s.c_str(), s.size(), "text/plain");
      });

      server.bind("127.0.0.1", 0);
      sockaddr_in name;
      int         namelen = sizeof(name);
      server.get_tcp_server()->get_tcp()->getsockname(
          reinterpret_cast<sockaddr*>(&name), &namelen);
      const int port = ntohs(name.sin_port);

      // **先 listen，再发布端口**：反过来的话调用方立刻 connect 会拿到
      // ECONNREFUSED（本用例的客户端没有重试）。
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
    if (th.joinable()) th.join();
  }
};

// =========================================================================
// 客户端（阻塞）
// =========================================================================

long long ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

/// 依次写出 @p pieces（段之间停 @p gap_ms 墙钟），然后**累积读**到 @p pred 为真
/// 或超时。用于 h1 那一侧（那里的判据是明文报文里的一个子串）。
bool write_then_read(int port, const std::vector<std::string>& pieces, int gap_ms,
                     std::string& got,
                     const std::function<bool(const std::string&)>& pred,
                     int deadline_ms) {
  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;

  for (size_t i = 0; i < pieces.size(); ++i) {
    if (!pieces[i].empty() &&
        c.write_wait(pieces[i].data(), pieces[i].size(), 3000) != 0) {
      return false;
    }
    if (gap_ms > 0 && i + 1 < pieces.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
    }
  }

  const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (pred(got)) return true;
    if (ms_since(t0) >= deadline_ms) return false;

    uvcpp_buf b;
    const int rv = c.read_wait(b, 30);
    if (rv == 0) {
      if (b.size() > 0 && b.get_const_data() != nullptr) {
        got.append(b.get_const_data(), b.size());
      }
    } else if (rv != UV_ETIMEDOUT) {
      return false;
    }
  }
}

/// 一次 unary 调用的全部观测点。
struct CallResult {
  bool         done  = false;  ///< 收尾（on_response_end）到了
  bool         fatal = false;  ///< 会话报致命错
  int          status = 0;     ///< 首部块里的 `:status`
  std::string  body;           ///< 所有 DATA 拼起来
  http_headers first_headers;  ///< `on_response` 那一刻
  http_headers end_headers;    ///< `on_response_end` 那一刻（尾随头折在这里）
};

/**
 * @brief 打一次 unary：真 socket + 客户端 h2 会话。
 *
 * 请求头按 gRPC 客户端真会发的形状给：`content-type: application/grpc` 与
 * `te: trailers`（后者是规范要求的"我收尾随头"声明）。
 */
bool h2_grpc_call(int port, const std::string& path, const std::string& payload,
                  CallResult& out) {
  uvcpp_h2_session            cli(false);
  uvcpp_h2_session::callbacks cbs;
  // 明文 h2c：会话的 `:scheme` 按它填（缺省是 TLS），不声明的话服务端在明文
  // 这条腿上会（正确地）按 PROTOCOL_ERROR 关掉这条流。
  cli.set_tls(false);

  cbs.on_response = [&](uvcpp_h2_session&, uvcpp_h2_stream& stream, bool) {
    out.status        = static_cast<int>(stream.response.status_code);
    out.first_headers = stream.response.headers;
  };
  cbs.on_body = [&](uvcpp_h2_session&, uvcpp_h2_stream&, const char* d, size_t n) {
    out.body.append(d, n);
  };
  cbs.on_response_end = [&](uvcpp_h2_session&, uvcpp_h2_stream& stream) {
    out.done        = true;
    out.end_headers = stream.response.headers;
  };
  cbs.on_fatal = [&](uvcpp_h2_session&, int) { out.fatal = true; };

  if (cli.init(cbs) != 0) return false;

  uvcpp_http_request req;
  req.method = http_method::HTTP_POST;
  req.url    = path;
  req.set_header("host", "localhost");
  req.set_header("content-type", "application/grpc");
  req.set_header("te", "trailers");
  if (cli.submit_request(req, payload) <= 0) return false;

  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;

  const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  for (;;) {
    std::string wire;
    if (cli.drain(wire) != 0) break;
    if (!wire.empty() && c.write_wait(wire.data(), wire.size(), 3000) != 0) break;
    if (out.done || out.fatal) break;
    if (ms_since(t0) >= 4000) break;

    uvcpp_buf b;
    const int rv = c.read_wait(b, 30);
    if (rv == 0) {
      if (b.size() > 0 && b.get_const_data() != nullptr) {
        if (cli.recv(b.get_const_data(), b.size()) != 0) break;
      }
    } else if (rv != UV_ETIMEDOUT) {
      break;
    }
  }
  return out.done;
}

/// 造一条 grpc-sum 的请求载荷：帧（前缀 + `字段1=a` + `字段2=b`）。
std::string sum_payload(int32_t a, int32_t b) {
  std::string msg;
  grpc_field_put_int32(msg, 1, a);
  grpc_field_put_int32(msg, 2, b);
  std::string frame;
  grpc_frame_encode(msg, frame);
  return frame;
}

/**
 * @brief 装置自检：这段字节**必须**是一条恰好一帧的请求体，载荷就是 @p want_hex。
 *
 * 为什么非要有这一条：本用例的输入是**手写的十六进制字面量**，而手写前缀最容易
 * 错的正是那 5 个字节里的 4 个长度字节 —— 少写一个 `\x00`，长度就从 4 变成 1032，
 * 于是"载荷还没收全"、处理函数照样回 `INVALID_ARGUMENT(3)`，**下面那些判据一条
 * 都不会红**，用例看上去全绿而实际上一个字节都没钉住（这条用例的第一版就是这样：
 * 三处 fixture 全少一个 `\x00`，[3] 那一轮的四条判据全"通过"）。
 *
 * 所以每个 fixture 都要先过这道自检：**它是不是我以为的那份字节**。判据落在
 * 解码器上，而那正是"前缀对不对"的唯一权威。
 */
bool fixture_is_sum_frame(const std::string& payload, const std::string& want_hex) {
  std::string msg, err;
  if (grpc_frame_decode_exact(payload.data(), payload.size(), msg, &err) != 0) {
    info("装置自检失败：" + err);
    return false;
  }
  if (hex(msg) != want_hex) {
    info("装置自检失败：载荷是 " + hex(msg) + "，期望 " + want_hex);
    return false;
  }
  return true;
}

// =========================================================================
// 第 1 轮：尾随头那几个小件（不起服务器）
// =========================================================================

void test_trailer_helpers() {
  std::cout << "[1] 尾随头小件" << std::endl;

  check(std::string(grpc_content_type()) == "application/grpc",
        "content-type 就是 application/grpc（不带参数）");

  const http_headers ok = grpc_ok_trailers();
  check(ok.size() == 1, "成功路径只发一个字段");
  check(http_get_header(ok, "grpc-status") == "0", "成功路径：grpc-status: 0");
  check(!has_named(ok, "grpc-message"),
        "成功路径**不发** grpc-message（那个字段按规范是「对失败的说明」）");

  const http_headers bad =
      grpc_status_trailers(grpc_status::INVALID_ARGUMENT, "bad input");
  check(http_get_header(bad, "grpc-status") == "3",
        "INVALID_ARGUMENT 编成 grpc-status: 3");
  check(http_get_header(bad, "grpc-message") == "bad input",
        "可打印 ASCII 的说明原样进去");

  // 百分号编码：只允许 0x20..0x7E 里除 `%` 外的字符原样出现。
  check(grpc_percent_encode("") == "", "空串还是空串");
  check(grpc_percent_encode("a b") == "a b", "空格是合法的（在 0x20..0x7E 里）");
  check(grpc_percent_encode("%") == "%25", "`%` 自己必须转义（它是引导符）");
  check(grpc_percent_encode("\xe4\xb8\xad") == "%E4%B8%AD",
        "一个汉字的三个字节 → 三个大写 %XX");
  check(grpc_percent_encode("a\nb") == "a%0Ab", "控制字符要转义");
}

// =========================================================================
// 第 2 轮：整条 unary 往返（三段各钉一次）
// =========================================================================

void test_unary_roundtrip() {
  std::cout << "[2] 明文 h2 上的 unary 往返" << std::endl;

  TestServer srv;
  const int  port = srv.start();
  check(port > 0, "服务端起来了");
  if (port <= 0) return;

  // 载荷与期望都**手算**，不用本库的编码器去造期望值 —— 判据是线上那串字节，
  // 而 9 字节的 `00 00 00 00 04 08 01 10 02` 正是 HttpArena 那条 grpc-sum 请求
  // 的原文（`requests/grpc-sum.bin`）。1 + 2 = 3 → 载荷 `08 03`。
  //
  // 前缀是 `00`（压缩标志）+ `00 00 00 04`（**四字节**大端长度）—— 数一下是 5 个
  // 字节，不是 4：这一处写起来少一个 `\x00` 太自然了，而后果是整个用例静默地不
  // 钉任何东西（见下面那条自检的注释）。
  const std::string kSum9 = bytes_of("\x00\x00\x00\x00\x04\x08\x01\x10\x02", 9);
  check(fixture_is_sum_frame(kSum9, "08011002"),
        "装置自检：这一份 9 字节请求体真的是一帧，载荷是 08 01 10 02");

  CallResult r1;
  const bool ok1 = h2_grpc_call(port, "/grpc.test.Sum/Sum", kSum9, r1);
  check(ok1, "整条响应收完（含尾随块）");
  check(r1.status == 200, "HTTP 状态码是 200（gRPC 的成功在 grpc-status 上，不在这一格）");
  check(http_get_header(r1.first_headers, "content-type") == "application/grpc",
        "首部块里有 content-type: application/grpc");
  check(!has_named(r1.first_headers, "grpc-status"),
        "**首部块里没有** grpc-status —— 两个头块必须真是两块");
  check(hex(r1.body) == "00000000020803",
        "DATA 段逐字节是 00 00 00 00 02 08 03（1 + 2 = 3，帧前缀 + varint 都对）");
  info("第一组响应体 " + hexsp(r1.body));
  {
    // 手算的字面量与解码器互为独立的两道口径：字面量数错了（这条用例的第一版
    // 就把 5 字节前缀写成了 4 字节），下面这一条会红。
    std::string m, e;
    const bool  one = grpc_frame_decode_exact(r1.body.data(), r1.body.size(), m, &e) == 0;
    check(one && hex(m) == "0803",
          "而且那条响应体解得开：**正好一帧**，载荷就是 08 03");
  }
  check(http_get_header(r1.end_headers, "grpc-status") == "0",
        "尾随块里 grpc-status: 0 —— 这是尾随头真的到了的**唯一**证据");

  // 换一组数：证明确实在算那两个字段，而不是这条路由恒回 3。
  // 100000 与 234567 的 varint 都是 3 字节，和 334567，所以这一条同时钉住了
  // 多字节 varint 的编与解。
  CallResult r2;
  const std::string kSum13 = bytes_of("\x00\x00\x00\x00\x08\x08\xa0\x8d\x06\x10\xc7"
                                      "\xa8\x0e",
                                      13);
  {
    // 自检**连值一起钉**。只看"解得开一帧"是不够的：varint 里一个字节写错
    // （`8c` 与 `8d` 差 128）仍然是一条合法的帧，而自检照样绿 —— 这条用例的
    // 第一版就是 `a0 8c 06`，和变成 334439，判据在下一行才红，红得莫名其妙。
    std::string m, e;
    int32_t     fa = 0, fb = 0;
    const bool  framed =
        grpc_frame_decode_exact(kSum13.data(), kSum13.size(), m, &e) == 0;
    const bool vals =
        framed &&
        grpc_field_get_int32(m.data(), m.size(), 1, fa, &e) == 0 && fa == 100000 &&
        grpc_field_get_int32(m.data(), m.size(), 2, fb, &e) == 0 && fb == 234567;
    check(vals, "装置自检：第二份解出来就是 100000 与 234567（不只是「一帧」）");
  }
  const bool ok2 = h2_grpc_call(port, "/grpc.test.Sum/Sum", kSum13, r2);
  check(ok2, "第二组也收完");
  check(hex(r2.body) == "000000000408e7b514",
        "100000 + 234567 = 334567 → 载荷 08 e7 b5 14（三字节 varint）");
  {
    std::string m, e;
    const bool  one = grpc_frame_decode_exact(r2.body.data(), r2.body.size(), m, &e) == 0;
    check(one && hex(m) == "08e7b514", "第二组也解得开：一帧，载荷是 08 e7 b5 14");
  }
  check(http_get_header(r2.end_headers, "grpc-status") == "0", "第二组也是成功");

  srv.shutdown();
}

// =========================================================================
// 第 3 轮：不合法请求体 → 只有尾随头的失败应答
// =========================================================================

void test_unary_failures() {
  std::cout << "[3] 非法请求体：Trailers-Only 的失败应答" << std::endl;

  TestServer srv;
  const int  port = srv.start();
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  // 形态一：长度前缀说载荷有 4 字节，实际只跟了 2 个。
  //
  // 自检在这里的方向**反过来**：这一份**必须**解失败，而且是**因为截断**
  // （`UV_ENODATA`）而不是因为前缀本身是垃圾（那会是 `UV_EINVAL`）。少了这一条，
  // 一个前缀写错的 fixture 会照样让下面 `grpc-status: 3` 那行变绿。
  const std::string kTruncated = bytes_of("\x00\x00\x00\x00\x04\x08\x01", 7);
  {
    std::string m, e;
    const int   rc = grpc_frame_decode_exact(kTruncated.data(), kTruncated.size(), m, &e);
    check(rc == UV_ENODATA,
          "装置自检：截断那份的失败原因是「载荷还没收全」（前缀说的确实是 4 字节）");
  }

  CallResult r1;
  const bool ok1 = h2_grpc_call(port, "/grpc.test.Sum/Sum", kTruncated, r1);
  check(ok1, "截断的帧也收到了应答（不是超时）");
  check(r1.status == 200, "HTTP 层照样是 200（失败在 grpc-status 里）");
  check(r1.body.empty(), "**没有 DATA 段** —— 这正是 Trailers-Only 的形状");
  check(http_get_header(r1.end_headers, "grpc-status") == "3",
        "截断的帧 → grpc-status: 3（INVALID_ARGUMENT）");
  check(has_named(r1.end_headers, "grpc-message"), "带了一句说明（grpc-message 在）");
  info("grpc-message = " + http_get_header(r1.end_headers, "grpc-message"));

  // 形态二：一帧之后还有多余的字节 ⇒ "恰好一帧"那条判据。
  const std::string kExtra = bytes_of("\x00\x00\x00\x00\x04\x08\x01\x10\x02"
                                      "\x10\x05",
                                      11);
  {
    // 自检两件事：**前 9 字节**是那条好帧（否则"多余字节"这个说法不成立），
    // 而**整段 11 字节**必须因为尾巴而失败。两条一起才把这一份 fixture 钉死。
    std::string m, e;
    const bool  head_ok =
        grpc_frame_decode_exact(kExtra.data(), 9, m, &e) == 0 && hex(m) == "08011002";
    const int tail_rc =
        grpc_frame_decode_exact(kExtra.data(), kExtra.size(), m, &e);
    check(head_ok, "装置自检：多出字节那份的前 9 字节仍然是一条完整的好帧");
    check(tail_rc == UV_EINVAL,
          "装置自检：多出的两个字节确实被判成「帧后还有多余字节」");
  }

  CallResult r2;
  const bool ok2 = h2_grpc_call(port, "/grpc.test.Sum/Sum", kExtra, r2);
  check(ok2, "多出字节的请求也收到了应答");
  check(http_get_header(r2.end_headers, "grpc-status") == "3",
        "帧后还有多余字节 → grpc-status: 3");
  check(r2.body.empty(), "同样没有 DATA 段");

  // 形态三：空请求体。**这条最容易被写成"两个字段都没给，和为 0"** —— 那就把
  // 一条非法请求答成了一次成功。
  CallResult r3;
  const bool ok3 = h2_grpc_call(port, "/grpc.test.Sum/Sum", std::string(), r3);
  check(ok3, "空请求体也收到了应答");
  check(http_get_header(r3.end_headers, "grpc-status") == "3",
        "空请求体 → grpc-status: 3（不是「和为 0」的成功）");

  srv.shutdown();
}

// =========================================================================
// 第 4 轮：h1 上这条路**不存在**
// =========================================================================

void test_grpc_rejected_on_h1() {
  std::cout << "[4] h1 上 grpc_send_unary() 明着失败" << std::endl;

  TestServer srv;
  const int  port = srv.start();
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  // `UV_EINVAL`（-22）：h1 上 `resp.stream_id` 是 0，而 gRPC 没有那条腿
  // （尾随头在 h1 上要 chunked 编码，本库没做）。判据是**返回值**，所以服务端
  // 把它编进了 body。
  const std::string want = std::to_string(UV_EINVAL);
  std::string       req  = "POST /grpc-h1 HTTP/1.1\r\nHost: h\r\nContent-Length: 9\r\n\r\n";
  req += bytes_of("\x00\x00\x00\x00\x04\x08\x01\x10\x02", 9);  // 与第 2 轮同一份载荷

  std::string got;
  const bool  ok = write_then_read(
      port, {req}, 0, got,
      [&want](const std::string& s) { return s.find(want) != std::string::npos; },
      3000);
  srv.shutdown();

  info("h1 上 grpc_send_unary() 返回 " + want + "（UV_EINVAL）");
  check(ok, "h1 上 grpc_send_unary() 返回 UV_EINVAL（body 里读到的）");
  check(got.compare(0, 9, "HTTP/1.1 ") == 0, "而且回的是一条 h1 报文，没有半个 h2 帧");
  check(got.find("grpc-status") == std::string::npos,
        "尾随头没有被当成普通响应头混进 h1 的首部");
}

}  // namespace

int main() {
  std::cout << "gRPC over 明文 h2（src/grpc/uvcpp_grpc_server.*）" << std::endl;

  test_trailer_helpers();
  test_unary_roundtrip();
  test_unary_failures();
  test_grpc_rejected_on_h1();

  if (g_failures == 0) {
    std::cout << "全部通过" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " 条失败" << std::endl;
  return 1;
}

#else

int main() {
  std::cout << "SKIP: UVCPP_WEB_ENABLE / UVCPP_NGHTTP2_ENABLE 未开" << std::endl;
  return 0;
}

#endif
