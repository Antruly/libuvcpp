/**
 * @file tests/functional/web_app_h2_grpc_func.cpp
 * @brief gRPC 在 `uvcpp_web_app` 上那条往返：**三段顺序由框架排，不由路由排**。
 *
 * 与 `web_h2_grpc_func.cpp` 的分工
 * -------------------------------
 * 那一份测的是**底层**（`uvcpp_http_server` + `grpc_send_unary()`）：三段顺序由
 * `grpc_send_unary()` 自己排，测试里能直接看到 `begin_stream → write_stream →
 * send_trailers` 这条链。
 *
 * 这一份测的是**框架那一侧**：路由只**声明**应答长什么样
 * （`grpc_web_send_unary()` 设 status / content-type / body / 尾随头），发送者是
 * `uvcpp_web_app::send_response()` —— 整个框架里唯一同时看得见 `write_stream()`
 * 的 `done` 与 `send_trailers()` 的地方。两半的失败模式完全不同：底层那半写错了
 * 是"顺序反了"，这一半写错了是"框架压根没走那条路，尾随头被当成普通响应头混进
 * 首部块"或者"被静默丢掉"。
 *
 * 文件名里的 `web_app_` 与 `h2_` 都是**必须的**，两个前缀各把住一个开关
 * （`tests/functional/CMakeLists.txt` 的命名约定）：
 *   - `web_app_` —— 这个用例要 webapp 层。webapp 是可选模块且**默认关**，而
 *     "web 开、webapp 关"是一份合法且默认的构建（CI 的 `http3` 那一格就是，
 *     见 `ci-linux-ubuntu.yml`）。少了这个前缀，它会落到 webapp 不在的树上编译
 *     失败，而不是"不被测"。
 *   - `h2_` —— 尾随头只有 h2 有。h1 上这条路**不存在**（本库不做 chunked 尾随
 *     头），所以第 4 轮测的是"被丢弃"，不是"也能用"。
 *
 * `grpc_web_send_*` 与 `grpc_parse_unary_body(std::string)` 三条都在
 * `#if UVCPP_WEBAPP_ENABLE` 里（`src/grpc/uvcpp_grpc_server.h` 的第二个开关），
 * 所以本文件的编译期前置就是 `UVCPP_WEBAPP_ENABLE && UVCPP_NGHTTP2_ENABLE`。
 *
 * 判据的骨架与那一份同源，也是**分三段各钉一次**：
 *   1. 首部块：`content-type: application/grpc`，且 **`grpc-status` 不在里面**；
 *   2. DATA：响应体**逐字节**等于手算出来的那一帧；
 *   3. 尾随块：`on_response_end` 那一刻的头列表里有 `grpc-status: 0`。
 * 第 3 条是尾随头真的到了的**唯一**证据 —— 它既不是首部、也不是 body。把
 * `set_trailers()` 那一句删掉（框架照常回 200 + 帧），前两条照样绿。
 *
 * 装置：`uvcpp_web_app` 跑在后台线程（`start_background()`、端口 0），客户端用
 * **阻塞** `uvcpp_tcp_client` 配一个客户端 h2 会话（`uvcpp_h2_session(false)`）
 * 真字节往返，走 h2c（`set_h2c_enabled(true)`）。走真 socket 是必要的：这条路
 * 要穿过前言判定、llhttp 的让位、会话重新武装、DATA 拼装、以及**框架统一发送**
 * 那一跳 —— 任何一处没接上都表现为超时或丢字段。
 */

#include <chrono>
#include <functional>
#include <iostream>
#include <string>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_NGHTTP2_ENABLE

#include <grpc/uvcpp_grpc.h>
#include <grpc/uvcpp_grpc_server.h>
#include <net/uvcpp_tcp_client.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>

#include <http2/uvcpp_h2_session.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

// 判据全部落在**主线程**这一侧（观测点是客户端收到的那串字节），处理函数里
// 一个 print 都没有：所以这里不需要锁。处理函数那边的信息一律靠"线上长什么样"
// 暴露出来 —— 那也正是判据该看的唯一一面对不对。
void check(bool cond, const std::string& what) {
  if (cond) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void info(const std::string& what) {
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

std::string to_lower(const std::string& s) {
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] + 32);
  }
  return out;
}

/// @p s 里每个字节都在 `0x20..0x7E` 里（含 `%`）。尾随头的字段值必须是这个样子
/// —— h2 的头块是字节串，一个非 ASCII 字节就是一条协议违例（而 gRPC 的
/// `grpc-message` 正是为此才要百分号编码）。
bool all_printable_ascii(const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20u || c > 0x7Eu) return false;
  }
  return true;
}

long long ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

// =========================================================================
// 服务端：uvcpp_web_app + 后台线程 + 端口 0 + 明文 h2c
// =========================================================================

struct TestApp {
  uvcpp_web_app app;

  void build() {
    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN)
        .set_h2c_enabled(true);

    // 一条正常的 unary：解请求 → 求和 → 声明应答。**帧的 5 字节前缀与 varint
    // 一个字都不用自己写**，这就是"用框架的文档化高层 API"长什么样。
    app.post("/grpc.test.Sum/Sum",
             [](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next) {
               std::string message, err;
               grpc_status st = grpc_status::UNKNOWN;
               if (grpc_parse_unary_body(req.body_str(), message, st, err) != 0) {
                 grpc_web_send_status(resp, st, err);
                 resp.end();
                 return;
               }

               int32_t a = 0, b = 0;
               const int ra =
                   grpc_field_get_int32(message.data(), message.size(), 1, a);
               const int rb =
                   grpc_field_get_int32(message.data(), message.size(), 2, b);
               if (ra == UV_EINVAL || rb == UV_EINVAL) {
                 grpc_web_send_status(resp, grpc_status::INVALID_ARGUMENT,
                                      "请求体里的字段解不出来");
                 resp.end();
                 return;
               }

               std::string out;
               grpc_field_put_int32(out, 1, a + b);
               grpc_web_send_unary(resp, out);
               resp.end();
             });

    // 与上一条同一件事，只多了"处理函数先往响应里放过别的东西"。这条把两类
    // 头**分开**钉：
    //   - **描述 body 的那三个**（`content-length` / `transfer-encoding` /
    //     `content-encoding`）必须被摘掉。留一个 `content-length: 999` 就是
    //     线上错帧：这条应答只有 7 个字节，对端会一直等剩下 992 个。
    //   - **别的头留着**：它们就是 gRPC 的 initial metadata（`:status` 之后、
    //     DATA 之前那一段头块），服务端捎带应答元数据是规范内的用法。
    app.post("/grpc.override",
             [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
               resp.status(http_status::INTERNAL_SERVER_ERROR);
               resp.text("这段体不该上线");
               resp.set_header("content-length", "999");
               resp.set_header("content-encoding", "gzip");
               resp.set_header("x-meta", "kept");
               std::string out;
               grpc_field_put_int32(out, 1, 7);
               grpc_web_send_unary(resp, out);
               resp.end();
             });

    // 一句**自己给的**失败说明，用来把 `grpc-message` 的百分号编码钉死在一条与
    // 解码器措辞无关的字面量上。先 `text()` 放一段体：这条路才真的走到
    // `grpc_web_send_status()` 里那句 `clear_body()`（否则它只是清一个本来就空
    // 的体，"清没清"看不出来）。
    app.post("/grpc.msg",
             [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
               resp.text("这段体不该上线");
               resp.set_header("content-length", "999");
               grpc_web_send_status(resp, grpc_status::INVALID_ARGUMENT,
                                    "\xe4\xb8\xad\xe6\x96\x87");
               resp.end();
             });

    // 流式响应上**又**设了尾随头。两条路互斥，判据是"段没被发出去"而不是
    // "发了一段空的" —— 流的形状仍然是干净的。
    app.post("/grpc.stream",
             [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
               resp.begin_chunked("text/event-stream");
               resp.write_chunk("data: 1\n\n");
               resp.set_trailers(grpc_ok_trailers());
               resp.end();
             });
  }

  /// @return 实际端口；`<= 0` = 没起来。
  int start() {
    if (app.start_background() != 0) return -1;
    return app.bound_port();
  }

  void shutdown() {
    app.stop();
    app.join();
  }
};

// =========================================================================
// 客户端（阻塞）
// =========================================================================

/// 一次调用的全部观测点（与 `web_h2_grpc_func.cpp` 同形）。
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
bool h2_call(int port, const std::string& path, const std::string& payload,
             const std::string& content_type, CallResult& out,
             int deadline_ms = 4000) {
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
  req.set_header("content-type", content_type);
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
    if (ms_since(t0) >= deadline_ms) break;

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

/// h1 那次往返：写出整条请求、累积读到条件成立或超时。
///
/// 这一轮不能用 h2 会话 —— 要证的就是"**一条诚实的 h1 连接上发生了什么**"，
/// 而有线协议由被测端选，不由我们声明。
bool h1_call(int port, const std::string& req, std::string& got,
             const std::function<bool(const std::string&)>& pred,
             int deadline_ms) {
  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;
  if (c.write_wait(req.data(), req.size(), 3000) != 0) return false;

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

/**
 * @brief 装置自检：这段字节**必须**是一条恰好一帧的请求体，载荷就是 @p want_hex。
 *
 * 手写十六进制字面量最容易错的正是那 5 个字节里的 4 个长度字节 —— 少写一个
 * `\x00`，长度就从 4 变成 1032，于是"载荷还没收全"、处理函数照样回
 * `INVALID_ARGUMENT(3)`，**下面那些判据一条都不会红**（`web_h2_grpc_func.cpp`
 * 的第一版三处 fixture 全少一个 `\x00`，整组判据静默通过）。所以每个 fixture
 * 都要先过这道自检：**它是不是我以为的那份字节**。
 */
bool fixture_is_frame(const std::string& payload, const std::string& want_hex) {
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
// 第 1 轮：框架那一侧的 unary 往返（三段各钉一次）
// =========================================================================

void test_web_unary_roundtrip() {
  std::cout << "[1] web_app 上的 unary 往返（框架排三段）" << std::endl;

  TestApp   t;
  t.build();
  const int port = t.start();
  check(port > 0, "uvcpp_web_app 起来了（h2c 已开）");
  if (port <= 0) return;

  // 载荷与期望都**手算**，不用本库的编码器去造期望值 —— 判据是线上那串字节，
  // 而 9 字节的 `00 00 00 00 04 08 01 10 02` 正是 HttpArena 那条 grpc-sum 请求
  // 的原文（`requests/grpc-sum.bin`）。1 + 2 = 3 → 载荷 `08 03`。
  const std::string kSum9 = bytes_of("\x00\x00\x00\x00\x04\x08\x01\x10\x02", 9);
  check(fixture_is_frame(kSum9, "08011002"),
        "装置自检：这一份 9 字节请求体真的是一帧，载荷是 08 01 10 02");

  CallResult r1;
  const bool ok1 =
      h2_call(port, "/grpc.test.Sum/Sum", kSum9, "application/grpc", r1);
  check(ok1, "整条响应收完（含尾随块）");

  check(r1.status == 200, "HTTP 状态码是 200（gRPC 的成功在 grpc-status 上）");
  check(http_get_header(r1.first_headers, "content-type") == "application/grpc",
        "首部块里有 content-type: application/grpc");
  check(!has_named(r1.first_headers, "grpc-status"),
        "**首部块里没有** grpc-status —— 两个头块必须真是两块");
  check(hex(r1.body) == "00000000020803",
        "DATA 段逐字节是 00 00 00 00 02 08 03（1 + 2 = 3）");
  info("第一组响应体 " + hexsp(r1.body));
  {
    // 手算的字面量与解码器互为独立的两道口径：字面量数错了，下面这一条会红。
    std::string m, e;
    const bool  one =
        grpc_frame_decode_exact(r1.body.data(), r1.body.size(), m, &e) == 0;
    check(one && hex(m) == "0803",
          "而且那条响应体解得开：**正好一帧**，载荷就是 08 03");
  }
  check(http_get_header(r1.end_headers, "grpc-status") == "0",
        "尾随块里 grpc-status: 0 —— 这是尾随头真的到了的**唯一**证据");

  // 换一组数：证明确实在算那两个字段，而不是这条路由恒回 3。100000 与 234567
  // 的 varint 都是 3 字节，和 334567，所以这一条同时钉住多字节 varint 的编与解。
  const std::string kSum13 = bytes_of("\x00\x00\x00\x00\x08\x08\xa0\x8d\x06\x10\xc7"
                                      "\xa8\x0e",
                                      13);
  {
    // 自检**连值一起钉**。只看"解得开一帧"是不够的：varint 里一个字节写错
    // （`8c` 与 `8d` 差 128）仍然是一条合法的帧，自检照样绿。
    std::string m, e;
    int32_t     fa = 0, fb = 0;
    const bool  framed =
        grpc_frame_decode_exact(kSum13.data(), kSum13.size(), m, &e) == 0;
    const bool vals =
        framed && grpc_field_get_int32(m.data(), m.size(), 1, fa, &e) == 0 &&
        fa == 100000 &&
        grpc_field_get_int32(m.data(), m.size(), 2, fb, &e) == 0 && fb == 234567;
    check(vals, "装置自检：第二份解出来就是 100000 与 234567（不只是「一帧」）");
  }
  CallResult r2;
  const bool ok2 =
      h2_call(port, "/grpc.test.Sum/Sum", kSum13, "application/grpc", r2);
  check(ok2, "第二组也收完");
  check(hex(r2.body) == "000000000408e7b514",
        "100000 + 234567 = 334567 → 载荷 08 e7 b5 14（三字节 varint）");
  check(http_get_header(r2.end_headers, "grpc-status") == "0", "第二组也是成功");

  // 请求头里那个 `content-type` 是不是 `application/grpc` **不影响**这条路由
  // 认不认（判据是路径）。拿一个别的 ct 再打一次，应答形状必须一字不差：
  // gRPC 那条路的 `content-type` 是**服务端定的**（规范里就一个值），
  // 不是从请求里反射回来的。
  CallResult r3;
  const bool ok3 = h2_call(port, "/grpc.test.Sum/Sum", kSum9, "text/plain", r3);
  check(ok3, "换个请求 content-type 也收完");
  check(http_get_header(r3.first_headers, "content-type") == "application/grpc",
        "响应头的 content-type 仍是 application/grpc（服务端定的那个值）");

  // 处理函数先放过别的东西的那条路由：形状只有一种。
  CallResult r4;
  const bool ok4 = h2_call(port, "/grpc.override", std::string(), "text/plain", r4);
  check(ok4, "被覆盖过的那条路由也收完");
  check(r4.status == 200, "状态码被 grpc_web_send_unary() 改回 200（不是 500）");
  check(http_get_header(r4.first_headers, "content-type") == "application/grpc",
        "content-type 也被改回 application/grpc（不是 text/plain）");
  check(hex(r4.body) == "00000000020807",
        "体是这一条路由自己编的帧（字段1=7 → 08 07），不是处理函数留下的那段文字");
  // 帧 = 5 字节前缀 + `08 07`（2 字节）= 7 字节。处理函数设过 `999`，而
  // `sync_meta()` 的规则是"已存在 Content-Length 时不动它" —— 不摘掉的话
  // 这条应答会带着 999 的声明上线，对端一直等剩下 992 个字节。
  check(http_get_header(r4.first_headers, "content-length") == "7",
        "content-length 被重算成帧的 7 字节（处理函数设的 999 被摘掉了）");
  check(!has_named(r4.first_headers, "content-encoding"),
        "content-encoding 也被摘掉了（gRPC 自己的压缩是 grpc-encoding，不是它）");
  check(http_get_header(r4.first_headers, "x-meta") == "kept",
        "处理函数设的**别的**头留着 —— 那就是 gRPC 的 initial metadata");
  check(http_get_header(r4.end_headers, "grpc-status") == "0", "这条也有成功尾随头");

  t.shutdown();
}

// =========================================================================
// 第 2 轮：Trailers-Only 与 grpc-message 的百分号编码
// =========================================================================

void test_web_trailers_only() {
  std::cout << "[2] 失败应答：Trailers-Only + grpc-message" << std::endl;

  TestApp   t;
  t.build();
  const int port = t.start();
  if (port <= 0) {
    check(false, "uvcpp_web_app 起来了");
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
    const int   rc =
        grpc_frame_decode_exact(kTruncated.data(), kTruncated.size(), m, &e);
    check(rc == UV_ENODATA,
          "装置自检：截断那份的失败原因是「载荷还没收全」（前缀说的确实是 4 字节）");
  }

  CallResult r1;
  const bool ok1 =
      h2_call(port, "/grpc.test.Sum/Sum", kTruncated, "application/grpc", r1);
  check(ok1, "截断的帧也收到了应答（不是超时）");
  check(r1.status == 200, "HTTP 层照样是 200（失败在 grpc-status 里）");
  check(r1.body.empty(), "**没有 DATA 段** —— 这正是 Trailers-Only 的形状");
  check(http_get_header(r1.end_headers, "grpc-status") == "3",
        "截断的帧 → grpc-status: 3（INVALID_ARGUMENT）");
  check(has_named(r1.end_headers, "grpc-message"), "带了一句说明（grpc-message 在）");
  {
    const std::string msg = http_get_header(r1.end_headers, "grpc-message");
    check(all_printable_ascii(msg),
          "grpc-message 在线上全是可打印 ASCII（`%XX` 转义过了）");
    info("grpc-message = " + msg);
  }

  // 形态二：空请求体。**这条最容易被写成"两个字段都没给，和为 0"** —— 那就把
  // 一条非法请求答成了一次成功。
  CallResult r2;
  const bool ok2 =
      h2_call(port, "/grpc.test.Sum/Sum", std::string(), "application/grpc", r2);
  check(ok2, "空请求体也收到了应答");
  check(http_get_header(r2.end_headers, "grpc-status") == "3",
        "空请求体 → grpc-status: 3（不是「和为 0」的成功）");
  check(r2.body.empty(), "空请求体这条也没有 DATA 段");

  // 形态三：`grpc-message` 的编码**逐字节**对一条自己给的字面量。前两组的说明
  // 来自解码器，措辞会随实现变；这一组的「中文」是我们自己写的，期望值
  // （`%E4%B8%AD%E6%96%87`）也是手算的 —— 判据落在规范那条"只允许 0x20..0x7E
  // 里除 `%` 外的字符原样出现"上。
  //
  // 这条路由另外**先 `text()` 放了一段体**：它才是真的走到
  // `grpc_web_send_status()` 里那句 `clear_body()`（其余几组都是从空体起步，
  // "清没清"看不出来）。
  CallResult r3;
  const bool ok3 = h2_call(port, "/grpc.msg", std::string(), "application/grpc", r3);
  check(ok3, "自定说明那条也收到了应答");
  check(http_get_header(r3.end_headers, "grpc-message") == "%E4%B8%AD%E6%96%87",
        "中文说明逐字节编成 %E4%B8%AD%E6%96%87（大写十六进制）");
  check(http_get_header(r3.end_headers, "grpc-status") == "3",
        "而且状态码是我们给的那个（INVALID_ARGUMENT）");
  check(r3.body.empty(),
        "Trailers-Only 的响应体恒为空 —— 处理函数先放进去的那段被清掉了");
  check(http_get_header(r3.first_headers, "content-type") == "application/grpc",
        "content-type 也被改回来了（处理函数留的是 text/plain）");
  check(http_get_header(r3.first_headers, "content-length") == "0",
        "content-length 是 0（处理函数设的 999 被摘掉后由 sync_meta 按空体重算）");

  t.shutdown();
}

// =========================================================================
// 第 3 轮：尾随头与**流式**互斥 —— 被丢弃，不是被发出去
// =========================================================================

void test_web_trailers_dropped_on_stream() {
  std::cout << "[3] 流式响应上的尾随头被丢弃" << std::endl;

  TestApp   t;
  t.build();
  const int port = t.start();
  if (port <= 0) {
    check(false, "uvcpp_web_app 起来了");
    return;
  }

  CallResult  r;
  const bool  ok =
      h2_call(port, "/grpc.stream", std::string(), "text/event-stream", r);
  check(ok, "流式响应收完（end_stream 到了）");
  check(r.status == 200, "状态码 200");
  check(r.body == "data: 1\n\n",
        "体就是 write_chunk 写进去的那一块（h2 上没有 chunked 的 hex 帧）");
  check(!has_named(r.end_headers, "grpc-status"),
        "**结束那一刻也没有** grpc-status —— 尾随头被丢弃，不是「发了一段空的」");
  check(!has_named(r.first_headers, "grpc-status"),
        "首部块里同样没有 —— 丢弃不是「挪到首部去发」");

  t.shutdown();
}

// =========================================================================
// 第 4 轮：h1 上那条路**不存在** —— 尾随头被丢弃，体照发
// =========================================================================

void test_web_trailers_dropped_on_h1() {
  std::cout << "[4] h1 上尾随头被丢弃（体照发）" << std::endl;

  TestApp   t;
  t.build();
  const int port = t.start();
  if (port <= 0) {
    check(false, "uvcpp_web_app 起来了");
    return;
  }

  // 明文 h1 请求。**故意不带 h2 前言** —— 带前言的话前言判定会把连接交给 h2，
  // 这一轮就什么都没测。h2c 开着不影响它：分流按那 24 个字节走，不是猜。
  const std::string body = bytes_of("\x00\x00\x00\x00\x04\x08\x01\x10\x02", 9);
  std::string       req  = "POST /grpc.test.Sum/Sum HTTP/1.1\r\n";
  req += "Host: localhost\r\n";
  req += "Content-Type: application/grpc\r\n";
  req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  req += body;

  // 1 + 2 = 3 → 帧是 `00 00 00 00 02 08 03`（7 字节）。
  const std::string kFrame = bytes_of("\x00\x00\x00\x00\x02\x08\x03", 7);

  std::string got;
  const bool  ok = h1_call(
      port, req, got,
      [&kFrame](const std::string& s) { return s.find(kFrame) != std::string::npos; },
      3000);

  const std::string low = to_lower(got);
  info("h1 上收到的前 64 字节：" + hexsp(got));

  check(ok, "h1 上照样收到了那条帧（体没有因为尾随头发不出去而消失）");
  check(got.compare(0, 9, "HTTP/1.1 ") == 0, "回的是 h1 报文，没有半个 h2 帧");
  check(got.compare(0, 12, "HTTP/1.1 200") == 0, "HTTP 状态码仍然是 200");
  check(low.find("grpc-status") == std::string::npos,
        "尾随头**没有**被当成普通响应头混进 h1 的首部");
  check(low.find("grpc-message") == std::string::npos, "也没有第二个字段漏出去");
  // 体长就是 7 —— 这是"体真的还在、且没被拆成 chunked"的判据。少一个字节
  // （比如框架把它当成"没有体"的 204 类响应清掉）这条就红。
  check(low.find("content-length: 7") != std::string::npos,
        "Content-Length 是 7（帧的字节数），不是 0、也没有改走 chunked");
  check(low.find("transfer-encoding") == std::string::npos,
        "没有带 transfer-encoding（本库的 h1 尾随头那条路没做，就不该露出半个迹象）");

  t.shutdown();
}

}  // namespace

int main() {
  std::cout << "gRPC over web_app（src/webapp + src/grpc，框架排三段）" << std::endl;

  test_web_unary_roundtrip();
  test_web_trailers_only();
  test_web_trailers_dropped_on_stream();
  test_web_trailers_dropped_on_h1();

  if (g_failures == 0) {
    std::cout << "全部通过" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " 条失败" << std::endl;
  return 1;
}

#else

int main() {
  std::cout << "SKIP: UVCPP_WEBAPP_ENABLE / UVCPP_NGHTTP2_ENABLE 未开" << std::endl;
  return 0;
}

#endif
