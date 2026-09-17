/**
 * @file tests/functional/web_app_stream_resp_func.cpp
 * @brief Phase 3c 步骤 4：`uvcpp_web_response` 的 chunked 流式响应
 *        （`begin_chunked` / `write_chunk` / `end`）+ 水位背压 + `on_sent`
 *        推迟 + `streamed` 字段。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么单独一个文件：步骤 3 的 `web_stream_response_func.cpp` 打的是 **http
 * 层**（`begin_stream` / `write_stream` / `end_stream` 三个入口的字节契约），
 * 本文件打的是**框架层** —— 组帧、HEAD 的逐字节一致性、压缩闸门、`on_drain`
 * 的边沿触发、`on_sent` 的推迟与 `streamed` 字段、闲置超时对流式响应的豁免、
 * 以及对端断开时这条流被结算**恰好一次**。这些机制只在真端口上存在（没有
 * 服务器时根本没有 `uvcpp_web_response` 的流式分支）。
 *
 * 文件命名同时命中 `web_.*\.cpp$` 与 `web_app_.*\.cpp$` 两条过滤 —— 只命中
 * 一条会在关掉某模块时留下来然后链接失败（3b 步骤 2 记下的坑）。
 *
 * 客户端：裸 `uvcpp_tcp_client` 分阶段读写（沿 `web_app_stream_func.cpp` 的
 * `staged_conn`）。`uvcpp_http_client` 只能整包读，而这里要判断的恰恰是
 * **线上字节的逐字节形状**与**到达的先后**。
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include "net/uvcpp_net_read.h"
#include "net/uvcpp_tcp_client.h"
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_router.h>

#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/** @brief 轮询等待一个条件成立，最长 timeout_ms 毫秒。 */
template <typename Pred>
bool wait_for(Pred pred, int timeout_ms) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

/** @brief 测试用的 App 骨架：回环 + 端口 0 + 日志压到 WARN（免得刷屏）。 */
void configure_for_test(uvcpp_web_app& app) {
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);
}

// =========================================================================
// 期望值的独立算法
// =========================================================================
//
// **刻意不复用实现里的 `chunk_hex`**：两边共用一份就等于"拿实现验实现"，
// 而组帧的 hex 是最容易错的一步（大小写、零填充、按字符数而不是字节数算
// 长度）。这里用 `std::ostringstream << std::hex` 自己算一遍。

std::string chunk_hex_of(size_t n) {
  std::ostringstream os;
  os << std::hex << n;  // 小写、不补零 —— 与线上形状一致
  return os.str();
}

std::string chunk_frame(const std::string& data) {
  return chunk_hex_of(data.size()) + "\r\n" + data + "\r\n";
}

/** @brief 把一条报文的头部（含结尾空行）与 body 切开。 */
bool split_wire(const std::string& wire, std::string& head, std::string& body) {
  const size_t p = wire.find("\r\n\r\n");
  if (p == std::string::npos) return false;
  head = wire.substr(0, p + 4);
  body = wire.substr(p + 4);
  return true;
}

bool head_has(const std::string& head, const std::string& needle) {
  return head.find(needle) != std::string::npos;
}

/**
 * @brief 解开 chunked 的 body，返回是否**完整且自洽**。
 *
 * 判据比"能解出字节"严：要求终止块在场、且终止块之后**恰好**是空 trailer
 * 与报文结尾。少了这一条，一条**没有终止块**的流照样能解出全部负载，而
 * keep-alive 上对端会一直等下一个块 —— 那正是收尾项 2 要修的缺陷。
 */
bool decode_chunked(const std::string& body, std::string& out) {
  out.clear();
  size_t p = 0;
  for (;;) {
    const size_t eol = body.find("\r\n", p);
    if (eol == std::string::npos) return false;
    const std::string hex = body.substr(p, eol - p);
    if (hex.empty()) return false;
    size_t n = 0;
    for (size_t i = 0; i < hex.size(); ++i) {
      const char ch = hex[i];
      int d = -1;
      if (ch >= '0' && ch <= '9') {
        d = ch - '0';
      } else if (ch >= 'a' && ch <= 'f') {
        d = ch - 'a' + 10;
      } else if (ch >= 'A' && ch <= 'F') {
        d = ch - 'A' + 10;
      } else {
        return false;
      }
      n = n * 16 + static_cast<size_t>(d);
    }
    p = eol + 2;
    if (n == 0) {
      // 终止块之后只允许空的 trailer，且必须正好是报文结尾。
      return p + 2 == body.size() && body.compare(p, 2, "\r\n") == 0;
    }
    if (p + n + 2 > body.size()) return false;
    out.append(body, p, n);
    p += n + 2;
  }
}

std::string get_head(const std::string& path,
                     const std::string& extra = std::string()) {
  return "GET " + path + " HTTP/1.1\r\nHost: t\r\n" + extra + "\r\n";
}

/** @brief 收到的字节里 `HTTP/1.1 ` 出现了几次（用来数响应个数）。 */
int count_responses(const std::string& wire) {
  int n = 0;
  size_t p = 0;
  while ((p = wire.find("HTTP/1.1 ", p)) != std::string::npos) {
    ++n;
    p += 9;
  }
  return n;
}

/**
 * @brief 分阶段裸客户端（沿 `web_app_stream_func.cpp` 的形状）。
 *
 * 线程：连接与其 loop 都在**本线程**上（App 的循环在它自己的后台线程），
 * 所以下面的标志不需要原子 —— 回调都是 `loop_->run()` 在我们自己的栈上
 * 跑出来的。`rx()` 同理：只有本线程碰它。
 *
 * 比原版多一个 `data_events_`：**读边界**在流式响应里是有意义的判据（一块
 * 负载有没有跨 read 边界到达），所以它必须可观测，否则"跨边界"这件事只是
 * 一句愿望而不是一个前提。
 */
class staged_conn {
 public:
  staged_conn()
      : loop_(nullptr),
        connected_(false),
        written_(false),
        closed_(false),
        data_events_(0) {}

  bool open(int port, int timeout_ms = 3000) {
    int rc = c_.connect("127.0.0.1", port, [this](int st) {
      if (st != 0) return;
      connected_ = true;
      // 起读必须在连上之后（socket 还没建立时装回调会静默收不到任何字节）。
      c_.read_start_events([this](uvcpp_tcp_client&, const net_read_result& r) {
        if (r.event == net_read_event::DATA && r.size > 0 && r.data != nullptr) {
          ++data_events_;
          rx_.append(r.data, r.size);
        } else if (r.event == net_read_event::PEER_CLOSED ||
                   r.event == net_read_event::READ_ERROR) {
          closed_ = true;
        }
      });
    });
    if (rc != 0) return false;
    loop_ = c_.get_loop();
    return pump_until([this]() { return connected_; }, timeout_ms);
  }

  /**
   * @brief 写一段字节，泵到写完成。
   *
   * **必须等写完成，且失败要当回事**：`uvcpp_tcp_client` 同一时刻只允许一个
   * 异步写，撞上在途写会返回 `UV_EALREADY` 并**把这一块丢掉**。
   */
  bool write(const std::string& s, int timeout_ms = 3000) {
    if (s.empty()) return true;
    written_ = false;
    int rc = c_.write(s.data(), s.size(), [this](int) { written_ = true; });
    if (rc != 0) return false;
    return pump_until([this]() { return written_; }, timeout_ms);
  }

  /// 上限是**墙钟**毫秒，不是圈数：一圈的代价就是系统定时器粒度（Windows 无
  /// 请求者时默认 15.625 ms），按圈数计时在粗粒度机器上会整体放大约 8 倍。
  void pump(int ms) { uvcpp_test::pump_for(loop_, ms); }

  template <typename Pred>
  bool pump_until(Pred pred, int timeout_ms) {
    return uvcpp_test::wait_until(loop_, pred, timeout_ms);
  }

  const std::string& rx() const { return rx_; }
  bool peer_closed() const { return closed_; }
  int data_events() const { return data_events_; }

  /**
   * @brief 关闭连接。
   *
   * **这个析构函数不能用来确定"关连接时接收缓冲里还有没有没读走的字节"。**
   * 它原来那段注释写的是「不把 socket 读干净 ⇒ 一定发 RST」，实测不成立：
   * 关之前用例已经用 `pump_until` 等过数据，而 `pump_until` 每轮都会跑循环
   * 把 socket 读一遍，于是缓冲里还剩没剩字节取决于服务端那批数据发到哪儿了，
   * 是纯粹的时序运气。
   *
   * 更根本的是，**那条路本身就不该被依赖**：第 8 组原来靠"写还在途时断开"
   * 去构造 `ok=false`，而那要求内核缓冲被填满 —— 实测在这台机器的回环上，
   * 服务端一次 8 MiB 的写在 200ms 内就"成功"完成了，哪怕对端一个字节都没
   * 读走（`uv__process_tcp_write_req` 在 Windows 上也从不检查
   * `overlapped.InternalHigh`）。所以那一组现在走的是**与缓冲填充程度无关**
   * 的另一条路（收尾发生在连接确证已从登记表摘除之后），既不依赖 RST，
   * 也不依赖 FIN，`stop_reading()` 那个访问器因此已经不需要了。
   */
  ~staged_conn() {
    if (loop_ == nullptr) return;
    if (c_.get_tcp() == nullptr) return;
    bool done = false;
    c_.get_tcp()->close([&done](uvcpp_handle*) { done = true; });
    for (int i = 0; i < 200 && !done; ++i) loop_->run(UV_RUN_NOWAIT);
  }

 private:
  uvcpp_tcp_client c_;
  uvcpp_loop*      loop_;
  std::string      rx_;
  bool             connected_;
  bool             written_;
  bool             closed_;
  int              data_events_;
};

/** @brief 一段二进制可打印不出来的负载：含 NUL、0xFF、CRLF。 */
std::string binary_body(size_t n) {
  std::string b;
  b.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    // 刻意让 NUL 与 0xFF 周期性出现 —— 任何按 C 字符串处理的地方都会在这里断。
    switch (i % 8) {
      case 0: b.push_back('\0'); break;
      case 1: b.push_back(static_cast<char>(0xFF)); break;
      case 2: b.push_back('\r'); break;
      case 3: b.push_back('\n'); break;
      default: b.push_back(static_cast<char>('a' + (i % 26))); break;
    }
  }
  return b;
}

/** @brief `on_sent` 的采样点。全部具名初始化（C++11 的 atomic 默认构造不置零）。 */
struct sent_probe {
  std::atomic<int>    count;
  std::atomic<size_t> bytes;
  std::atomic<int>    streamed;
  std::atomic<int>    ok;
  std::atomic<size_t> pending_at_cb;
  std::atomic<size_t> written_at_cb;

  sent_probe()
      : count(0),
        bytes(0),
        streamed(-1),
        ok(-1),
        pending_at_cb(static_cast<size_t>(-1)),
        written_at_cb(static_cast<size_t>(-1)) {}
};

typedef std::shared_ptr<sent_probe> sent_probe_ptr;

// =========================================================================
// 1. begin_chunked / write_chunk / end —— 线上字节逐字节等于期望的组帧
// =========================================================================
void test_begin_write_end() {
  uvcpp_web_app app;
  configure_for_test(app);
  app.get("/sse", [](uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next) {
    resp.begin_chunked("text/event-stream");
    resp.write_chunk("data: 1\n\n");
    resp.write_chunk("data: 2\n\n");
    resp.write_chunk("data: 3\n\n");
    resp.end();
  });

  check(app.start_background() == 0, "sse: 服务启动");
  const int port = app.bound_port();

  const std::string payload = "data: 1\n\ndata: 2\n\ndata: 3\n\n";
  const std::string expect_body = chunk_frame("data: 1\n\n") +
                                  chunk_frame("data: 2\n\n") +
                                  chunk_frame("data: 3\n\n") + "0\r\n\r\n";

  staged_conn c;
  // **只能调一次 open()。** `uvcpp_tcp_client::connect` 在**成功**路径上
  // 从不复位 `has_async_connect_cb_`，所以对同一个客户端第二次 `connect`
  // 拿到的是 `UV_EALREADY` —— `open()` 返回 false。而 `check(c.open(port))`
  // 查的是**第一次**那个 true，于是"检查通过、分支体一次都不跑"：整组
  // 断言全部静默跳过，报 `-> PASS`。这个形状本组只暴露成 3 条断言失败，
  // 其余几组的断言全在块内，一条都不报 —— 是同一族里最难看见的一面。
  const bool ok = c.open(port);
  check(ok, "sse: 连上服务端");
  if (ok) {
    check(c.write(get_head("/sse")), "sse: 发出请求");
    const bool arrived =
        c.pump_until([&]() { return c.rx().find(expect_body) != std::string::npos; },
                     3000);
    check(arrived, "sse: 整条 chunked body 逐字节到达（含终止块）");

    std::string head, body;
    check(split_wire(c.rx(), head, body), "sse: 报文头体可分");
    check(head_has(head, "transfer-encoding: chunked"),
          "sse: 头部声明 transfer-encoding: chunked");
    check(!head_has(head, "content-length"),
          "sse: 头部**不得**同时带 content-length（RFC 7230 3.3.2）");
    check(head_has(head, "content-type: text/event-stream"),
          "sse: content-type 是 begin_chunked 给的那个");
    check(body == expect_body, "sse: body 与期望组帧逐字节相同");

    std::string decoded;
    check(decode_chunked(body, decoded), "sse: body 可完整解开（含终止块）");
    check(decoded == payload, "sse: 解出来的负载是原文");
    check(count_responses(c.rx()) == 1, "sse: 恰好一个响应");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 2. 二进制安全 + 跨 read 边界
// =========================================================================
void test_binary_chunk() {
  const size_t kSlice = 128 * 1024;
  std::shared_ptr<std::string> body(
      new std::string(binary_body(3 * kSlice)));

  // 三块的切点**刻意不是 8 的倍数**：`binary_body` 的周期是 8，切在整数倍
  // 上会让三块的"字节种类分布"完全一样，某一种字节出错时可能只在某一块的
  // 某个位置暴露 —— 错开之后每块都从不同的相位开始。
  const size_t off1 = kSlice;
  const size_t off2 = kSlice + 3;
  const size_t len1 = off1;
  const size_t len2 = 3;
  const size_t len3 = body->size() - off2;

  uvcpp_web_app app;
  configure_for_test(app);
  app.get("/bin", [body, off1, off2, len1, len2, len3](
                      uvcpp_web_request&, uvcpp_web_response& resp,
                      uvcpp_web_next) {
    resp.begin_chunked("application/octet-stream");
    resp.write_chunk(body->data() + 0, len1);
    resp.write_chunk(body->data() + off1, len2);
    resp.write_chunk(body->data() + off2, len3);
    resp.end();
  });

  check(app.start_background() == 0, "bin: 服务启动");
  const int port = app.bound_port();

  const std::string expect_body =
      chunk_frame(body->substr(0, len1)) +
      chunk_frame(body->substr(off1, len2)) +
      chunk_frame(body->substr(off2, len3)) + "0\r\n\r\n";

  staged_conn c;
  const bool ok = c.open(port);  // 只调一次：见上面 test_begin_write_end 的说明
  check(ok, "bin: 连上服务端");
  if (ok) {
    check(c.write(get_head("/bin")), "bin: 发出请求");

    const bool arrived = c.pump_until(
        [&]() {
          const size_t sep = c.rx().find("\r\n\r\n");
          return sep != std::string::npos &&
                 c.rx().size() >= sep + 4 + expect_body.size();
        },
        5000);
    check(arrived, "bin: 整条 body 到达");

    // **前置断言**：负载有 384 KiB，不可能一个 read 就收完。没有这一条，
    // "跨 read 边界"只是注释里的一句愿望 —— 而这一组存在的理由正是它。
    check(c.data_events() >= 2,
          "bin: 负载确实跨了多个 read（前置；实测 " +
              std::to_string(c.data_events()) + " 次）");

    std::string head, got;
    check(split_wire(c.rx(), head, got), "bin: 报文头体可分");
    check(got == expect_body, "bin: body 与期望组帧逐字节相同");

    std::string decoded;
    check(decode_chunked(got, decoded), "bin: body 可完整解开");
    check(decoded.size() == body->size(),
          "bin: 解出来是 " + std::to_string(body->size()) + " 字节（实测 " +
              std::to_string(decoded.size()) + "）");
    check(decoded.compare(*body) == 0,
          "bin: 逐字节等于原文（含 NUL / 0xFF / CRLF）");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 3. HEAD：头部与 GET 逐字节相同，body 一个字节都没有
// =========================================================================
void test_head_no_chunks() {
  uvcpp_web_app app;
  configure_for_test(app);
  app.set_head_as_get(true);
  app.get("/h", [](uvcpp_web_request&, uvcpp_web_response& resp,
                   uvcpp_web_next) {
    resp.begin_chunked("text/plain");
    resp.write_chunk("alpha");
    resp.write_chunk("beta");
    resp.end();
  });

  check(app.start_background() == 0, "head: 服务启动");
  const int port = app.bound_port();

  std::string get_wire, head_wire;
  {
    staged_conn c;
    const bool ok = c.open(port);  // 只调一次
    check(ok, "head: GET 连上");
    if (ok) {
      check(c.write(get_head("/h")), "head: 发出 GET");
      const bool done =
          c.pump_until([&]() { return c.rx().find("0\r\n\r\n") != std::string::npos; },
                       3000);
      check(done, "head: GET 收到终止块");
      c.pump(50);
      get_wire = c.rx();
    }
  }
  {
    staged_conn c;
    const bool ok = c.open(port);  // 只调一次
    check(ok, "head: HEAD 连上");
    if (ok) {
      check(c.write("HEAD /h HTTP/1.1\r\nHost: t\r\n\r\n"), "head: 发出 HEAD");
      const bool done =
          c.pump_until([&]() { return c.rx().find("\r\n\r\n") != std::string::npos; },
                       3000);
      check(done, "head: HEAD 收到头部");
      c.pump(100);  // 多泵一会儿：真有 body 的话这段时间足够到达
      head_wire = c.rx();
    }
  }

  std::string gh, gb, hh, hb;
  check(split_wire(get_wire, gh, gb), "head: GET 头体可分");
  check(split_wire(head_wire, hh, hb), "head: HEAD 头体可分");
  check(gh == hh, "head: HEAD 的头部与 GET 逐字节相同（含 transfer-encoding）");
  check(hb.empty(), "head: HEAD 的 body 一个字节都没有（实测 " +
                        std::to_string(hb.size()) + "）");

  // 对照组的关键：GET 那条**确实**有 body，否则上面那句"相同"可能只是
  // "两边都没发任何东西"。
  check(!gb.empty(), "head: GET 的 body 非空（对照，证明上面比的不是两个空报文）");

  app.stop();
  app.join();
}

// =========================================================================
// 4. 流式响应不得被压缩（http 层的 apply_compression 必须绕过它）
// =========================================================================
#if UVCPP_ZLIB_ENABLE
void test_compress_skipped() {
  uvcpp_web_app app;
  configure_for_test(app);
  // 阈值调到 0 让"小响应不压"这条规则失活 —— 于是唯一还能挡住压缩的
  // 就只剩"这是一条流"本身。两条路都用 text/plain（不在默认排除表里），
  // 所以**唯一的差别就是流式与非流式**。
  app.set_compression(true);
  app.set_compress_min_body_size(0);

  const std::string payload(64, 'a');

  app.get("/z_stream", [payload](uvcpp_web_request&, uvcpp_web_response& resp,
                                 uvcpp_web_next) {
    resp.begin_chunked("text/plain");
    resp.write_chunk(payload);
    resp.end();
  });
  app.get("/z_plain", [payload](uvcpp_web_request&, uvcpp_web_response& resp,
                                uvcpp_web_next) {
    resp.text(payload);
    resp.end();
  });

  check(app.start_background() == 0, "zip: 服务启动");
  const int port = app.bound_port();

  const std::string ae = "accept-encoding: gzip\r\n";

  // --- 流式：不得带 content-encoding ---
  {
    staged_conn c;
    const bool ok = c.open(port);  // 只调一次
    check(ok, "zip: 流式连上");
    if (ok) {
      check(c.write(get_head("/z_stream", ae)), "zip: 发出流式请求");
      const bool done =
          c.pump_until([&]() { return c.rx().find("0\r\n\r\n") != std::string::npos; },
                       3000);
      check(done, "zip: 流式响应到达终止块");
      std::string head, body;
      check(split_wire(c.rx(), head, body), "zip: 流式头体可分");
      check(head_has(head, "transfer-encoding: chunked"),
            "zip: 流式响应确实是 chunked");
      check(!head_has(head, "content-encoding"),
            "zip: 流式响应**不得**带 content-encoding（body 还没产生）");
    }
  }

  // --- 对照：同 app 上的普通响应必须被压 ---
  {
    staged_conn c;
    const bool ok = c.open(port);  // 只调一次
    check(ok, "zip: 普通连上");
    if (ok) {
      check(c.write(get_head("/z_plain", ae)), "zip: 发出普通请求");
      const bool done = c.pump_until(
          [&]() { return c.rx().find("\r\n\r\n") != std::string::npos; }, 3000);
      check(done, "zip: 普通响应到达头部");
      std::string head, body;
      check(split_wire(c.rx(), head, body), "zip: 普通头体可分");
      check(head_has(head, "content-encoding: gzip"),
            "zip: **对照** —— 同样的 accept-encoding 与阈值下，"
            "普通 text/plain 响应必须被压缩（否则上面那条是空断言）");
    }
  }

  app.stop();
  app.join();
}
#endif  // UVCPP_ZLIB_ENABLE

// =========================================================================
// 5. on_sent 推迟到流结束之后；对照：普通响应仍是老时序
// =========================================================================
void test_on_sent_deferred() {
  sent_probe_ptr sp(new sent_probe());
  sent_probe_ptr pp(new sent_probe());
  const std::string chunk_a(10, 'x');
  const std::string chunk_b(20, 'y');
  const std::string plain(40, 'z');

  uvcpp_web_app app;
  configure_for_test(app);

  app.get("/sent_stream", [sp, chunk_a, chunk_b](
                              uvcpp_web_request&, uvcpp_web_response& resp,
                              uvcpp_web_next) {
    uvcpp_web_response* rp = &resp;
    // **不能用 `set_stream_finished_cb`**：那是单槽，而 send_response 的
    // 流式分支已经占着它，用户再装一次会被静默顶掉。`on_sent` 是叠加的。
    resp.on_sent([sp, rp](const uvcpp_web_sent_info& si) {
      sp->count.fetch_add(1);
      sp->bytes.store(si.body_bytes);
      sp->streamed.store(si.streamed ? 1 : 0);
      sp->ok.store(si.ok ? 1 : 0);
      // 判别式：回调触发的那一刻，这条流的字节**应该已经全部落地**。
      // 捕获 `rp` 是安全的 —— 框架是先 `notify_sent()` 再 `release()`
      // 上下文的，即回调期间上下文（连同它的响应对象）一定还活着。
      sp->pending_at_cb.store(rp->stream_pending_bytes());
      sp->written_at_cb.store(rp->stream_bytes_written());
    });
    resp.begin_chunked("text/plain");
    resp.write_chunk(chunk_a);
    resp.write_chunk(chunk_b);
    resp.end();
  });

  app.get("/sent_plain", [pp, plain](uvcpp_web_request&, uvcpp_web_response& resp,
                                     uvcpp_web_next) {
    resp.on_sent([pp](const uvcpp_web_sent_info& si) {
      pp->count.fetch_add(1);
      pp->bytes.store(si.body_bytes);
      pp->streamed.store(si.streamed ? 1 : 0);
      pp->ok.store(si.ok ? 1 : 0);
    });
    resp.text(plain);
    resp.end();
  });

  check(app.start_background() == 0, "sent: 服务启动");
  const int port = app.bound_port();

  const std::string expect_body = chunk_frame(chunk_a) + chunk_frame(chunk_b) +
                                  "0\r\n\r\n";

  // --- 流式 ---
  {
    staged_conn c;
    const bool ok = c.open(port);  // 只调一次
    check(ok, "sent: 流式连上");
    if (ok) {
      check(c.write(get_head("/sent_stream")), "sent: 发出流式请求");
      const bool arrived =
          c.pump_until([&]() { return c.rx().find(expect_body) != std::string::npos; },
                       3000);
      check(arrived, "sent: 线上已经收到整条流（终止块在场）");
      // 客户端**先**观察到整条流，**再**等 on_sent —— 于是"回调晚于流结束"
      // 这件事是由客户端收到的字节确立的，不是靠内部时钟猜的。
      const bool fired = c.pump_until([&]() { return sp->count.load() == 1; }, 3000);
      check(fired, "sent: on_sent 在流结束之后触发了");
      c.pump(100);
      check(sp->count.load() == 1, "sent: on_sent **恰好一次**（实测 " +
                                       std::to_string(sp->count.load()) + "）");
      check(sp->streamed.load() == 1, "sent: streamed 为真");
      check(sp->ok.load() == 1, "sent: ok 为真");
      check(sp->bytes.load() == chunk_a.size() + chunk_b.size(),
            "sent: body_bytes 是实际写出去的总长（期望 " +
                std::to_string(chunk_a.size() + chunk_b.size()) + "，实测 " +
                std::to_string(sp->bytes.load()) + "）");
      check(sp->pending_at_cb.load() == 0,
            "sent: 回调触发时缓冲已空（实测 " +
                std::to_string(sp->pending_at_cb.load()) + "）");
      check(sp->written_at_cb.load() == chunk_a.size() + chunk_b.size(),
            "sent: 回调触发时 stream_bytes_written 已是总量");
    }
  }

  // --- 对照：普通响应 ---
  //
  // **这里不假装能验证"回调早于 body"那一点。** 头部入队是服务端线程上的
  // 动作，要"证明回调发生在 body 之前"就得在服务端读到客户端的接收进度 ——
  // 那是跨线程读 `c.rx()`，是一次数据竞争，不是证据。可诚实断言的是这几条：
  // 同一条路径上 `streamed` 为假、`ok` 为真、`body_bytes` 是真实长度。
  {
    staged_conn c;
    const bool ok = c.open(port);  // 只调一次
    check(ok, "sent: 普通连上");
    if (ok) {
      check(c.write(get_head("/sent_plain")), "sent: 发出普通请求");
      const bool fired = c.pump_until([&]() { return pp->count.load() == 1; }, 3000);
      check(fired, "sent: 普通响应的 on_sent 触发了");
      check(pp->streamed.load() == 0,
            "sent: **对照** —— 普通响应的 streamed 为假");
      check(pp->ok.load() == 1, "sent: 普通响应 ok 为真");
      check(pp->bytes.load() == plain.size(),
            "sent: 普通响应 body_bytes 是真实长度（实测 " +
                std::to_string(pp->bytes.load()) + "）");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 6. 水位背压：越过高水位返回 false，降到低水位 on_drain 恰好一次，不丢字节
// =========================================================================
void test_drain_backpressure() {
  // 高水位 64、块长 32 ⇒ 帧长 38（hex 两位 + CRLF + 32 + CRLF）。
  const size_t kWater = 64;
  const size_t kChunk = 32;

  std::shared_ptr<std::atomic<int> > first_ok(new std::atomic<int>(-1));
  std::shared_ptr<std::atomic<int> > second_ok(new std::atomic<int>(-1));
  std::shared_ptr<std::atomic<int> > drain_count(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<size_t> > at_drain(new std::atomic<size_t>(0));
  sent_probe_ptr sp(new sent_probe());

  const std::string a(kChunk, 'a');
  const std::string b(kChunk, 'b');
  const std::string cch(kChunk, 'c');

  uvcpp_web_app app;
  configure_for_test(app);
  app.get("/drain", [a, b, cch, kWater, first_ok, second_ok, drain_count,
                     at_drain, sp](uvcpp_web_request&, uvcpp_web_response& resp,
                                   uvcpp_web_next) {
    uvcpp_web_response* rp = &resp;
    resp.set_max_stream_buffer_bytes(kWater);
    resp.on_sent([sp](const uvcpp_web_sent_info& si) {
      sp->count.fetch_add(1);
      sp->bytes.store(si.body_bytes);
      sp->ok.store(si.ok ? 1 : 0);
    });
    resp.begin_chunked("text/plain");

    // 第 1 块：38 ≤ 64 ⇒ 有富余，返回 true。
    first_ok->store(resp.write_chunk(a) ? 1 : 0);
    // 第 2 块：累计 76 > 64 ⇒ 越过水位，返回 false —— 但**数据仍然收下了**。
    second_ok->store(resp.write_chunk(b) ? 1 : 0);

    // 注意：此刻 sink 还没装上（它在 send_response 的流式分支里才建），
    // 所以前两块都还攒着、一次都没 flush。on_drain 必须在这之前注册好。
    resp.on_drain([rp, drain_count, at_drain, cch]() {
      drain_count->fetch_add(1);
      at_drain->store(rp->stream_bytes_written());
      // 降到低水位之后继续写 —— 这一块**不该**再把水位顶回去
      // （38 ≤ 64），所以 on_drain 不会第二次触发。
      rp->write_chunk(cch);
      rp->end();
    });
  });

  check(app.start_background() == 0, "drain: 服务启动");
  const int port = app.bound_port();

  const std::string expect_body = chunk_frame(a) + chunk_frame(b) +
                                  chunk_frame(cch) + "0\r\n\r\n";

  staged_conn c;
  const bool ok = c.open(port);  // 只调一次
  check(ok, "drain: 连上服务端");
  if (ok) {
    check(c.write(get_head("/drain")), "drain: 发出请求");
    const bool arrived =
        c.pump_until([&]() { return c.rx().find(expect_body) != std::string::npos; },
                     3000);
    check(arrived, "drain: 三块 + 终止块全部到达（背压期间一个字节都没丢）");

    check(first_ok->load() == 1, "drain: 未越水位时 write_chunk 返回 true");
    check(second_ok->load() == 0, "drain: 越过水位时 write_chunk 返回 false");
    check(drain_count->load() == 1, "drain: on_drain **恰好一次**（实测 " +
                                        std::to_string(drain_count->load()) + "）");
    check(at_drain->load() == 2 * kChunk,
          "drain: 触发时已写出去的是前两块（期望 " +
              std::to_string(2 * kChunk) + "，实测 " +
              std::to_string(at_drain->load()) + "）");

    std::string head, body;
    check(split_wire(c.rx(), head, body), "drain: 头体可分");
    std::string decoded;
    check(decode_chunked(body, decoded), "drain: body 可完整解开");
    check(decoded == a + b + cch, "drain: 解出来的负载是 abc 三段，顺序不变");

    check(sp->count.load() == 1, "drain: 整条流结算恰好一次");
    check(sp->bytes.load() == 3 * kChunk,
          "drain: body_bytes 是 3 块共 " + std::to_string(3 * kChunk) +
              "（实测 " + std::to_string(sp->bytes.load()) + "）");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 7. 安静的出站流不被闲置超时关掉；对照：普通连接必须被关
// =========================================================================
void test_quiet_stream_not_killed() {
  uvcpp_web_app app;
  configure_for_test(app);
  app.set_idle_timeout_ms(300);
  // 写两块之后**永不 end()** —— SSE 的常态（心跳之间可以安静很久）。
  app.get("/q_stream", [](uvcpp_web_request&, uvcpp_web_response& resp,
                          uvcpp_web_next) {
    resp.begin_chunked("text/event-stream");
    resp.write_chunk("data: 1\n\n");
    resp.write_chunk("data: 2\n\n");
  });
  app.get("/q_plain", [](uvcpp_web_request&, uvcpp_web_response& resp,
                         uvcpp_web_next) {
    resp.text("hello");
    resp.end();
  });

  check(app.start_background() == 0, "quiet: 服务启动");
  const int port = app.bound_port();

  staged_conn a;
  staged_conn b;
  const bool oka = a.open(port);  // 各自只调一次
  const bool okb = b.open(port);
  check(oka, "quiet: 流式连接建立");
  check(okb, "quiet: 普通连接建立");
  if (oka && okb) {
    check(a.write(get_head("/q_stream")), "quiet: 发出流式请求");
    check(b.write(get_head("/q_plain")), "quiet: 发出普通请求");

    const std::string stream_part = chunk_frame("data: 1\n\n");
    check(a.pump_until([&]() { return a.rx().find(stream_part) != std::string::npos; },
                       3000),
          "quiet: 流式响应已经发出前两块");
    check(b.pump_until([&]() { return b.rx().find("\r\n\r\n") != std::string::npos; },
                       3000),
          "quiet: 普通响应已经发出");

    // **前置断言**：两条连接此刻都还活着。没有它，"两边都被关了"与
    // "两边都还活着"在后面的断言里长得一样。
    check(!a.peer_closed() && !b.peer_closed(),
          "quiet: 两个连接在等待开始前都还活着（前置）");

    // 两个 `staged_conn` 各有**自己的 loop**（`uvcpp_tcp_client` 的默认构造
    // 就会 new 一个），所以要交替泵 —— 只泵一个的话另一个的 FIN 永远读不到。
    // 服务端的 idle_sweep 在它自己的线程上跑，不需要我们泵。
    a.pump(400); b.pump(400);
    a.pump(400); b.pump(400);

    check(b.peer_closed(),
          "quiet: **对照** —— 同样安静 1.6s 的普通 keep-alive 连接必须被关掉");
    check(!a.peer_closed(),
          "quiet: 写了几块之后长时间不写的流**不被**关（钉住 idle_sweep 现有的豁免）");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 8. 对端断开之后这条流才收尾 ⇒ 结算恰好一次、ok 为假、上下文被释放
// =========================================================================
//
// **这一组的判据必须由构造保证，不能靠运气。**
//
// 走到 `on_sent(ok=false)` 只有一条路：`end()` 被调过（`stream_finished_`
// 只由它置位），**并且**之后有一次写失败。而写失败有两个来源：
//
//   (a) `stream_write` 在登记表里查不到这条连接 ⇒ 返回 UV_ECANCELED，
//       且**不会调 done**，由 `flush_stream` 自己把这一笔结算掉
//       （`uvcpp_web_app.cpp:1771-1780`）；
//   (b) 连接还在，但在途写被对端的断开弄失败（`close_connection` 的队列唤醒）。
//
// (b) **取决于内核缓冲被填到什么程度**，而那不可控：实测在这台机器的回环上，
// 服务端一次 8 MiB 的写在 200ms 内就「成功」完成了，哪怕对端一个字节都没读走
// （探针原话：`[PROBE-CLOSE] after sleep: count=1 rx=126 alive=1`）。最初那一版
// 正是靠「写还在途时断开」去构造 (b)，于是它是个 vacuous 用例 —— 这条流在断开
// **之前**就正常收尾了，`ok` 恒为真（100/100 次 `ok=1`）。第 3 步的
// `queued_done_cancelled_on_close` 已经在 http 层用 FIN + RST 两轮把 (b) 钉死了
// （64 块 × 256 KiB、故意不 `end_stream`），本组不必再覆盖它。
//
// 所以这里走 (a)：**让收尾发生在连接确证已经不在登记表里之后**。判据与缓冲
// 填充程度、与 FIN/RST 全都无关。时序（每一步都是确定的）：
//
//   1. handler 写一块真实负载之后**不 end()**，把自己那个 `resp` 交给测试；
//   2. 客户端读到那一块（**前置断言**：这条流真的开始产 body 了），然后关闭；
//   3. 测试等连接数归零 —— 到这一刻，对端的断开已经被框架处理完了；
//   4. 测试 `app.post()` 到 loop 线程，在那个闭包里**再读一次**连接数并存下来，
//      然后调 `end()`。
//
// 第 4 步存下来的那个读数是本组最关键的前置断言：没有它，这一组随时可能悄悄
// 退回成「连接还活着的时候收尾」—— 也就是 vacuous 那一版。本组只建立一条
// 连接，所以那个读数必然是 0。
void test_client_disconnect_frees_transfer() {
  const size_t kChunk = 64 * 1024;

  std::shared_ptr<std::string> payload(new std::string(kChunk, 'B'));
  sent_probe_ptr sp(new sent_probe());
  // handler 把自己那个响应对象留在这里 —— 它由 `ctx.hold()` 钉着，活得比
  // handler 久。只有 loop 线程读它；测试线程只用它区分「handler 没跑过」。
  std::shared_ptr<std::atomic<uvcpp_web_response*> > live(
      new std::atomic<uvcpp_web_response*>(nullptr));
  // `end()` 那一刻的活连接数。**-1 = 那个闭包压根没跑成。**
  std::shared_ptr<std::atomic<int> > conns_at_end(new std::atomic<int>(-1));

  uvcpp_web_app app;
  configure_for_test(app);
  app.get("/tick", [payload, sp, live](uvcpp_web_request&,
                                       uvcpp_web_response& resp,
                                       uvcpp_web_next) {
    resp.on_sent([sp](const uvcpp_web_sent_info& si) {
      sp->count.fetch_add(1);
      sp->bytes.store(si.body_bytes);
      sp->ok.store(si.ok ? 1 : 0);
      sp->streamed.store(si.streamed ? 1 : 0);
    });
    resp.begin_chunked("application/octet-stream");
    (void)resp.write_chunk(payload->data(), payload->size());
    // **刻意不 end()** —— 收尾时机交给第 4 步（见上面那段时序）。
    live->store(&resp);
  });

  check(app.start_background() == 0, "drop: 服务启动");
  const int port = app.bound_port();

  {
    staged_conn c;
    const bool ok = c.open(port);  // 只调一次
    check(ok, "drop: 连上服务端");
    if (ok) {
      check(c.write(get_head("/tick")), "drop: 发出请求");
      const std::string first = chunk_frame(std::string(kChunk, 'B'));
      check(c.pump_until(
                [&]() { return c.rx().find(first) != std::string::npos; }, 3000),
            "drop: 第一块负载已经到达（这条流真的开始产 body 了）");
    }
    // 出作用域即关连接。**优雅关闭就够** —— 本组不依赖 RST：判据是「登记表里
    // 查不到这条连接」，不是「在途写失败」。
  }

  check(wait_for([&]() { return app.connection_count() == 0; }, 5000),
        "drop: 服务端已经回收这条连接（登记表里没有它了）");
  // **对端断开本身不结算响应流。** 这是有意钉住的现状：`on_close` 只对
  // **请求**流对象调 `stream_abort()`（判据是 `ctx.streaming()`，也就是
  // `stream_ != nullptr`），而响应流不在那条路上 —— 所以这条流此刻仍被
  // `hold()` 钉着，等它自己 `end()`。
  check(app.inflight_count() == 1,
        "drop: 对端断开**不**结算响应流（上下文仍被 hold，实测 " +
            std::to_string(app.inflight_count()) + "）");

  app.post([live, conns_at_end, &app]() {
    uvcpp_web_response* rp = live->load();
    if (rp == nullptr) return;  // handler 压根没跑（前面已经报过错了）
    conns_at_end->store(static_cast<int>(app.connection_count()));
    rp->end();
  });

  const bool settled = wait_for([&]() { return sp->count.load() == 1; }, 8000);
  check(settled, "drop: 对端断开之后 end() ⇒ 这条流被结算了（on_sent 到达）");
  // **下面这条前置断言的判据为什么成立（实测过，不是推理）**：`end()` 是那个
  // post 闭包里的最后一句，而 `store` 在它前面，所以 `sp->count` 变成 1 ⇒
  // 闭包已经跑过 `store` ⇒ 这里那次 `load()` 一定看得到它。
  //
  // 这个**顺序**是判据的一部分，不许挪：把这几条检查挪到 `settled` 之前就会
  // 读到初始值 -1。实测过 —— 把本组改回 vacuous 那一版（handler 自己 `end()`）
  // 之后 `settled` 立刻返回，检查跑到 post 闭包前面，报的正是
  // `前置 …… 实测 -1`（外加 vacuous 的招牌签名 `ok=1`），3/3 全红。
  check(conns_at_end->load() == 0,
        "drop: **前置** —— end() 那一刻连接已经不在登记表里（实测 " +
            std::to_string(conns_at_end->load()) + "）");
  check(sp->count.load() == 1,
        "drop: 结算**恰好一次**（实测 " + std::to_string(sp->count.load()) + "）");
  check(sp->ok.load() == 0,
        "drop: ok 为假（写失败；实测 ok=" + std::to_string(sp->ok.load()) + "）");
  check(sp->streamed.load() == 1, "drop: streamed 为真");
  check(sp->bytes.load() == kChunk,
        "drop: body_bytes 只算那一块真实负载，终止块不计入 —— 它压根没发出去"
        "（期望 " + std::to_string(kChunk) + "，实测 " +
            std::to_string(sp->bytes.load()) + "）");

  check(wait_for([&]() { return app.inflight_count() == 0; }, 5000),
        "drop: 上下文被 release —— 这才是「释放」那一半（在途请求数归零）");

  app.stop();
  app.join();
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  const std::string filter = (argc > 1) ? argv[1] : std::string();

  struct { const char* name; void (*fn)(); } tests[] = {
    {"begin_write_end", test_begin_write_end},
    {"binary_chunk", test_binary_chunk},
    {"head_no_chunks", test_head_no_chunks},
#if UVCPP_ZLIB_ENABLE
    {"compress_skipped", test_compress_skipped},
#endif
    {"on_sent_deferred", test_on_sent_deferred},
    {"drain_backpressure", test_drain_backpressure},
    {"quiet_stream_not_killed", test_quiet_stream_not_killed},
    {"client_disconnect_frees_transfer", test_client_disconnect_frees_transfer},
  };

  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    if (!filter.empty() &&
        std::string(tests[i].name).find(filter) == std::string::npos) {
      continue;
    }
    // 先打名字再跑：进程中途挂掉也能从最后一行看出死在哪一条。
    std::cout << "[web_app_stream_resp] " << tests[i].name << std::endl;
    const int before = g_failures;
    tests[i].fn();
    std::cout << "  -> " << (g_failures == before ? "PASS" : "FAIL") << std::endl;
  }

  std::cout << "[web_app_stream_resp] " << (g_failures == 0 ? "ALL PASS" : "FAIL")
            << std::endl;
  return g_failures == 0 ? 0 : 2;
}

#else
int main() { return 0; }
#endif
