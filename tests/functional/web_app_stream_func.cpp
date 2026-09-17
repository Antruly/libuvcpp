/**
 * @file tests/functional/web_app_stream_func.cpp
 * @brief `uvcpp_web_app` 的流式请求体：认领时机、逐块交付、背压、停顿、中止。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一层测的是**框架把流式接线装对了没有** —— 真 `start_background()`、真端口、
 * 真连接、裸 `uvcpp_tcp_client` 分块写。`uvcpp_http_client` 在这里完全用不上：
 * 它只会"整包发完再收"，而本文件每一条判据都要求"**body 还没发完**，服务端
 * 那边就已经发生了某件事"。
 *
 * 覆盖：
 *   1. `handler_runs_before_body`  handler 被调用时**一个 body 字节都还没发出去**，
 *                                  但 `total()` 已经是声明值 —— 这是"headers 时机
 *                                  派发"与"收完才派发"的分水岭
 *   2. `chunks_in_order`            分 5 次写入（含 NUL 与高位字节），拼起来
 *                                  与原文**逐字节相同**
 *   3. `path_params_and_query`      `/up/:room` 的参数与查询串在 handler 里都拿得到
 *   4. `on_end_then_response`       `on_end` 里填的响应真的发出去，且**只有一个**
 *   5. `pause_is_real`              `pause()` 之后对端再发多少都不交付；
 *                                  `resume()` 之后接着交付
 *   6. `stall_killed`               发一半停住 → 在 `idle_timeout_ms` 后被关
 *   7. `slow_but_progressing_survives`  慢速但**持续推进**的上传不被**掐断**
 *                                  （`on_abort` 零次；**对照组**：与 6 互为判据，
 *                                  缺任一条，"永远不关"或"一律关掉"的实现都能
 *                                  蒙混过关）
 *   8. `abort_mid_stream`           对端断开 → `on_abort` 恰好一次、`on_end` 零次
 *   9. `progress_reported`          进度回调单调不减、终值 == 声明总长
 *  10. `keepalive_after_stream`     流式请求之后**同一条连接**上的普通请求照常
 *  11. `normal_route_unaffected`    非流式路由走原路、拿到完整 body（回归网）
 *
 * 刻意**不覆盖**：multipart（3b）、chunked 流式**响应**（3b）、HTTP 流水线
 * （`web_app_pipeline_func.cpp`）。
 *
 * 崩溃时的定位手段：每个用例**先打名字再跑**，进程中途挂掉也能从最后一行
 * 看出死在哪一条。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include "net/uvcpp_net_read.h"
#include "net/uvcpp_tcp_client.h"
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_router.h>
#include <webapp/uvcpp_web_stream.h>

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

// =========================================================================
// 客户端侧小工具
// =========================================================================

/** @brief 轮询等待一个条件成立，最长 timeout_ms 毫秒。 */
template <typename Pred>
bool wait_for(Pred pred, int timeout_ms) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeout_ms);
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

/** @brief 一次 POST 的请求头（`extra` 里**不要**再补空行，这里会补）。 */
std::string post_head(const std::string& path, size_t content_length,
                      const std::string& extra = std::string()) {
  std::string h = "POST " + path + " HTTP/1.1\r\nHost: t\r\n";
  if (content_length != static_cast<size_t>(-1)) {
    h += "content-length: " + std::to_string(content_length) + "\r\n";
  }
  h += extra;
  h += "\r\n";
  return h;
}

/** @brief 收到的字节里 "HTTP/1.1 " 出现了几次（用来数响应个数）。 */
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
 * @brief 分阶段裸客户端：想什么时候写就什么时候写，随时读回已到达的字节。
 *
 * `raw_send` / `raw_hold`（`web_app_app_func.cpp`）都是"一次写完就不动了"，
 * 表达不出本文件需要的形状 —— 大半个 body 要**分多次、中间夹着等待**地写。
 *
 * 线程：连接与其 loop 都在**本线程**上（App 的循环在它自己的后台线程），
 * 所以下面的标志不需要原子 —— 回调都是 `loop_->run()` 在我们自己的栈上
 * 跑出来的。
 */
class staged_conn {
 public:
  staged_conn()
      : loop_(nullptr), connected_(false), written_(false), closed_(false) {}

  bool open(int port, int timeout_ms = 3000) {
    int rc = c_.connect("127.0.0.1", port, [this](int st) {
      if (st != 0) return;
      connected_ = true;
      // 起读必须在连上之后（socket 还没建立时装回调会静默收不到任何字节）。
      c_.read_start_events([this](uvcpp_tcp_client&, const net_read_result& r) {
        if (r.event == net_read_event::DATA && r.size > 0 && r.data != nullptr) {
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
   * 异步写，撞上在途写会返回 `UV_EALREADY` 并**把这一块丢掉**。不检查的话
   * "分 5 次写入"实际会变成"只写了第一次"，而用例照样可能绿。
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

// =========================================================================
// 1. handler 在 body 之前跑
// =========================================================================
void test_handler_runs_before_body() {
  const size_t kBody = 500;

  std::shared_ptr<std::atomic<int> > called(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<uint64_t> > received_at_call(
      new std::atomic<uint64_t>(999));
  std::shared_ptr<std::atomic<uint64_t> > total_at_call(
      new std::atomic<uint64_t>(999));
  std::shared_ptr<std::atomic<int> > has_total_at_call(
      new std::atomic<int>(-1));
  std::shared_ptr<std::atomic<uint64_t> > got(new std::atomic<uint64_t>(0));

  uvcpp_web_app app;
  configure_for_test(app);
  app.post_stream("/up", [called, received_at_call, total_at_call,
                          has_total_at_call, got](
                             uvcpp_web_request& req, uvcpp_web_response& resp,
                             uvcpp_web_next) {
    uvcpp_web_stream* s = req.stream();
    if (s == nullptr) return;
    // 这个 handler 与"headers 解析完"在**同一个同步栈**上，所以此刻：
    called->fetch_add(1);
    received_at_call->store(s->received());
    total_at_call->store(s->total());
    has_total_at_call->store(s->has_total() ? 1 : 0);

    s->on_data([got](const char*, size_t n) -> bool {
      got->fetch_add(n);
      return true;
    });
    s->on_end([&resp]() {
      resp.status(200).text("ok");
      resp.end();
    });
  });

  check(app.start_background() == 0, "before_body: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  check(c.open(port), "before_body: 连上服务端");
  if (c.open(port)) {
    // 只发头。**一个 body 字节都不发**，看 handler 会不会跑。
    check(c.write(post_head("/up", kBody)), "before_body: 发出请求头");

    const bool ran = wait_for([&]() { return called->load() > 0; }, 2000);
    check(ran, "before_body: handler 在**只发完头**的时候就被调用了");
    check(received_at_call->load() == 0,
          "before_body: handler 被调用时收到的 body 是 0 字节"
          "（实测 " + std::to_string(received_at_call->load()) + "）");
    check(has_total_at_call->load() == 1,
          "before_body: 此刻 has_total() 已经为真");
    check(total_at_call->load() == kBody,
          "before_body: total() 就是声明值 " + std::to_string(kBody) +
              "（实测 " + std::to_string(total_at_call->load()) + "）");

    // 现在才发 body，收尾。
    const std::string body(kBody, 'x');
    check(c.write(body), "before_body: 发出 body");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 3000);
    check(count_responses(c.rx()) == 1, "before_body: 收到一个响应");
    check(got->load() == kBody, "before_body: 全部 " + std::to_string(kBody) +
                                    " 字节都交付了");
  }
  app.stop();
  app.join();
}

// =========================================================================
// 2. 分块按序、二进制安全
// =========================================================================
void test_chunks_in_order() {
  const std::string body = binary_body(1000);

  std::shared_ptr<std::string> got(new std::string());
  std::shared_ptr<std::atomic<int> > chunks(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > ended(new std::atomic<int>(0));

  uvcpp_web_app app;
  configure_for_test(app);
  app.post_stream("/up", [got, chunks, ended](uvcpp_web_request& req,
                                              uvcpp_web_response& resp,
                                              uvcpp_web_next) {
    uvcpp_web_stream* s = req.stream();
    if (s == nullptr) return;
    s->on_data([got, chunks](const char* d, size_t n) -> bool {
      chunks->fetch_add(1);
      got->append(d, n);
      return true;
    });
    s->on_end([&resp, ended]() {
      ended->fetch_add(1);
      resp.status(200).text("ok");
      resp.end();
    });
  });

  check(app.start_background() == 0, "chunks: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    check(c.write(post_head("/up", body.size())), "chunks: 发出请求头");

    // 分 5 次写，每次之间泵一会儿 —— 让服务端有机会把它们当**不同的块**处理。
    const size_t step = body.size() / 5;
    for (int i = 0; i < 5; ++i) {
      const size_t off = step * i;
      const size_t len = (i == 4) ? (body.size() - off) : step;
      check(c.write(body.substr(off, len)),
            "chunks: 第 " + std::to_string(i + 1) + " 块写成功");
      c.pump(40);
    }
    c.pump_until([&]() { return ended->load() > 0; }, 3000);
    c.pump(100);

    check(ended->load() == 1, "chunks: on_end 恰好一次");
    check(chunks->load() >= 1, "chunks: 至少交付了一次");
    check(*got == body,
          "chunks: 收到的 body 与发出的**逐字节相同**（含 NUL / 0xFF / CRLF）"
          "，收到 " + std::to_string(got->size()) + " 应有 " +
              std::to_string(body.size()));
  }
  app.stop();
  app.join();
}

// =========================================================================
// 3. 路径参数与查询串
// =========================================================================
void test_path_params_and_query() {
  std::shared_ptr<std::string> room(new std::string("<unset>"));
  std::shared_ptr<std::string> token(new std::string("<unset>"));
  std::shared_ptr<std::string> path(new std::string("<unset>"));
  std::shared_ptr<std::atomic<int> > done(new std::atomic<int>(0));

  uvcpp_web_app app;
  configure_for_test(app);
  app.post_stream("/up/:room", [room, token, path, done](
                                   uvcpp_web_request& req,
                                   uvcpp_web_response& resp, uvcpp_web_next) {
    uvcpp_web_stream* s = req.stream();
    if (s == nullptr) return;
    path->assign(req.path());
    const std::string* p = req.param("room");
    if (p != nullptr) room->assign(*p);
    const std::string* q = req.query("token");
    if (q != nullptr) token->assign(*q);

    s->on_end([&resp, done]() {
      done->fetch_add(1);
      resp.status(200).text("ok");
      resp.end();
    });
  });

  check(app.start_background() == 0, "params: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    const std::string body = "hello";
    check(c.write(post_head("/up/kitchen?token=abc123&x=1", body.size())),
          "params: 发出请求");
    check(c.write(body), "params: 发出 body");
    c.pump_until([&]() { return done->load() > 0; }, 3000);
    c.pump(100);

    check(*path == "/up/kitchen", "params: path() 把查询串剥掉了（实测 '" +
                                      *path + "'）");
    check(*room == "kitchen", "params: 路径参数 room 取到了（实测 '" + *room +
                                  "'）");
    check(*token == "abc123", "params: 查询参数 token 取到了（实测 '" +
                                  *token + "'）");
  }
  app.stop();
  app.join();
}

// =========================================================================
// 4. on_end 里填的响应真的发出去
// =========================================================================
void test_on_end_then_response() {
  std::shared_ptr<std::atomic<int> > ended(new std::atomic<int>(0));

  uvcpp_web_app app;
  configure_for_test(app);
  app.post_stream("/save", [ended](uvcpp_web_request& req,
                                   uvcpp_web_response& resp, uvcpp_web_next) {
    uvcpp_web_stream* s = req.stream();
    if (s == nullptr) return;
    s->on_end([&resp, ended]() {
      ended->fetch_add(1);
      // 状态码与 body 都只在 on_end 里定 —— handler 返回时它还是空的。
      resp.status(201).set_header("x-saved", "yes").text("saved");
      resp.end();
    });
  });

  check(app.start_background() == 0, "on_end: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    const std::string body = "payload";
    check(c.write(post_head("/save", body.size()) + body), "on_end: 发出请求");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 3000);
    c.pump(150);

    check(ended->load() == 1, "on_end: on_end 恰好一次");
    check(count_responses(c.rx()) == 1,
          "on_end: 线上**只有一个**响应（实测 " +
              std::to_string(count_responses(c.rx())) + " 个）");
    const std::string& wire = c.rx();
    check(wire.find("HTTP/1.1 201") != std::string::npos,
          "on_end: 状态码是 on_end 里设的 201");
    check(wire.find("x-saved: yes") != std::string::npos,
          "on_end: on_end 里设的头真的发出去了");
    check(wire.find("saved") != std::string::npos, "on_end: body 是 'saved'");
  }
  app.stop();
  app.join();
}

// =========================================================================
// 5. pause() 真的停读，resume() 真的续上
// =========================================================================
void test_pause_is_real() {
  // 第一块 100 字节 → handler 在这一块里 pause()。
  // 之后无论对端发多少，都不该再交付；resume() 之后才继续。
  const size_t kFirst  = 100;
  const size_t kSecond = 200;
  const size_t kThird  = 100;

  std::shared_ptr<std::atomic<uint64_t> > received(
      new std::atomic<uint64_t>(0));
  std::shared_ptr<std::atomic<int> > phase(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<uvcpp_web_stream*> > stream_handle(
      new std::atomic<uvcpp_web_stream*>(nullptr));
  std::shared_ptr<std::atomic<int> > ended(new std::atomic<int>(0));

  uvcpp_web_app app;
  configure_for_test(app);
  app.post_stream("/slow", [received, phase, stream_handle, ended](
                               uvcpp_web_request& req, uvcpp_web_response& resp,
                               uvcpp_web_next) {
    uvcpp_web_stream* s = req.stream();
    if (s == nullptr) return;
    s->on_data([received, phase, stream_handle, s](const char*, size_t n)
                   -> bool {
      received->fetch_add(n);
      if (phase->load() == 0) {
        phase->store(1);
        stream_handle->store(s);
        // 在这一块**之内**暂停：这一块仍会交付完（解析器缓冲里已经在解析
        // 的数据插不进去），但从下一次读开始就不再有了。
        s->pause();
      }
      return true;
    });
    s->on_end([&resp, ended]() {
      ended->fetch_add(1);
      resp.status(200).text("ok");
      resp.end();
    });
  });

  check(app.start_background() == 0, "pause: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    check(c.write(post_head("/slow", kFirst + kSecond + kThird)),
          "pause: 发出请求头");

    // --- 第一块：让 handler 拿到它并暂停 ---
    check(c.write(std::string(kFirst, 'a')), "pause: 发出第一块");
    check(wait_for([&]() { return phase->load() == 1; }, 2000),
          "pause: handler 收到第一块并调用了 pause()");
    check(received->load() == kFirst,
          "pause: 此刻收到的正是第一块（实测 " +
              std::to_string(received->load()) + "）");

    // --- 暂停之后对端继续发：**一个字节都不该交付** ---
    //
    // 这里是确定性的（不靠缓冲区大小赌）：第一块已经交付完毕、pause 已经
    // 下发之后，我们才发第二块。所以第二块只可能停在服务端的内核接收缓冲里。
    check(c.write(std::string(kSecond, 'b')), "pause: 发出第二块");
    c.pump(500);
    check(received->load() == kFirst,
          "pause: 暂停之后第二块**没有被交付**（实测 " +
              std::to_string(received->load()) + "，应仍是 " +
              std::to_string(kFirst) + "）");

    // --- resume：从 App 的循环线程上恢复 ---
    //
    // 用 `app.post()`（框架的跨线程入口），因为这个用例的测试线程拨不动
    // 服务端的循环。
    app.post([stream_handle]() {
      uvcpp_web_stream* s = stream_handle->load();
      if (s != nullptr) s->resume();
    });
    check(wait_for([&]() { return received->load() == kFirst + kSecond; }, 3000),
          "pause: resume() 之后第二块补上了（实测 " +
              std::to_string(received->load()) + "，应为 " +
              std::to_string(kFirst + kSecond) + "）");

    // --- 恢复之后继续正常交付 ---
    check(c.write(std::string(kThird, 'c')), "pause: 发出第三块");
    c.pump_until([&]() { return ended->load() > 0; }, 3000);
    c.pump(100);

    check(ended->load() == 1, "pause: on_end 恰好一次");
    check(received->load() == kFirst + kSecond + kThird,
          "pause: 三块全部到齐（实测 " + std::to_string(received->load()) +
              "）");
    check(count_responses(c.rx()) == 1, "pause: 收到一个响应");
  }
  app.stop();
  app.join();
}

// =========================================================================
// 6. 停顿的上传被关（停顿保护）
// =========================================================================
void test_stall_killed() {
  const int kIdleMs = 400;

  std::shared_ptr<std::atomic<uint64_t> > received(
      new std::atomic<uint64_t>(0));

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_idle_timeout_ms(kIdleMs);
  app.post_stream("/stall", [received](uvcpp_web_request& req,
                                       uvcpp_web_response& resp,
                                       uvcpp_web_next) {
    uvcpp_web_stream* s = req.stream();
    if (s == nullptr) return;
    s->on_data([received](const char*, size_t n) -> bool {
      received->fetch_add(n);
      return true;
    });
    s->on_end([&resp]() {
      resp.status(200).text("ok");
      resp.end();
    });
  });

  check(app.start_background() == 0, "stall: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    // 声明 4000，只发 100，然后**再也不发**。
    check(c.write(post_head("/stall", 4000)), "stall: 发出请求头");
    check(c.write(std::string(100, 's')), "stall: 发出开头的一小块");

    // 判据是**服务端真的关了连接**（我们等到 FIN），而不是"我们等够了时间"。
    const bool closed = c.pump_until([&]() { return c.peer_closed(); }, 4000);
    check(closed, "stall: 停止推进的上传被服务端关掉了"
                  "（闲置超时 " + std::to_string(kIdleMs) + " ms）");
    check(received->load() == 100,
          "stall: 被关之前只交付了那 100 字节（实测 " +
              std::to_string(received->load()) + "）");
  }
  app.stop();
  app.join();
}

// =========================================================================
// 7. 慢速但持续推进的上传**不被**关（对照组）
// =========================================================================
void test_slow_but_progressing_survives() {
  const int kIdleMs = 400;
  const int kChunks = 10;
  const int kPaceMs = 150;

  std::shared_ptr<std::atomic<uint64_t> > received(
      new std::atomic<uint64_t>(0));
  std::shared_ptr<std::atomic<int> > ended(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > aborted(new std::atomic<int>(0));

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_idle_timeout_ms(kIdleMs);
  app.post_stream("/drip", [received, ended, aborted](uvcpp_web_request& req,
                                                      uvcpp_web_response& resp,
                                                      uvcpp_web_next) {
    uvcpp_web_stream* s = req.stream();
    if (s == nullptr) return;
    s->on_data([received](const char*, size_t n) -> bool {
      received->fetch_add(n);
      return true;
    });
    // **本组真正的判据在 `aborted` 上**，见用例末尾的说明。
    s->on_abort([aborted]() { aborted->fetch_add(1); });
    s->on_end([&resp, ended]() {
      ended->fetch_add(1);
      resp.status(200).text("ok");
      resp.end();
    });
  });

  check(app.start_background() == 0, "survives: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    const size_t kChunk = 8;
    const size_t kTotal = kChunk * kChunks;
    check(c.write(post_head("/drip", kTotal)), "survives: 发出请求头");

    // 每 150 ms 发 8 字节，一共拖 1500 ms —— 远超 400 ms 的闲置预算。
    //
    // **每块都必须确认写出去**（见 staged_conn::write 的注释）：`UV_EALREADY`
    // 会让这一块被静默丢掉，于是"持续推进"变成"第一块之后再没动静"，
    // 连接被关，而一个"把流式按整段预算计时"的坏实现反而会通过。
    int sent = 0;
    for (int i = 0; i < kChunks; ++i) {
      if (!c.write(std::string(kChunk, 'd'))) break;
      ++sent;
      c.pump(kPaceMs);
      if (c.peer_closed()) break;
    }

    c.pump_until([&]() { return ended->load() > 0; }, 3000);
    c.pump(100);

    // **这条用例的判据是"上传进行期间没有被掐断"，不是"上传结束后连接还开着"。**
    //
    // 早先这里写的是 `check(!c.peer_closed(), ...)`，它约 53% 概率失败 —— 而
    // 探针证明被测实现是对的：整个上传期间 `streaming=1`、闲置计数从未超过
    // ~252 ms（预算 400 ms），请求结束后 `request_start_ms` 归零、`since` 冻在
    // 最后一个 body 字节上，**连接这时才变成一条普通的闲置 keep-alive 连接**，
    // 于是下一次扫描（闲置 550 ms > 400 ms）把它关掉。
    //
    // 那正是 `idle_timeout_ms` 该干的事，所以坏的是断言不是实现：它要求一条
    // 已经超出闲置预算的连接必须还开着，成不成全看"最后一次扫描"与"事后多泵
    // 的这 100 ms"谁先到 —— 这正是那 ~53% 的来源。
    //
    // 正确的判据是 `on_abort` 零次：流对象只在**请求结束之前**被掐断时才报
    // abort（`stream_abort()` 在 `delivered_end_` 为真时直接早返回），所以
    // "上传途中断了"与"传完之后闲置被收掉"在它上面分得开，且没有任何竞态。
    // 配合 `sent == kChunks`（每块都真的写出去并被确认）与 `received == kTotal`
    // （每个字节都真的交付到 handler），一个"按整段预算计时的坏实现"会把上传
    // 在 400 ms 处掐掉，三条断言同时红。
    check(sent == kChunks, "survives: " + std::to_string(kChunks) +
                               " 块全部写成功（实测 " + std::to_string(sent) +
                               "）");
    check(aborted->load() == 0,
          "survives: 持续推进的慢上传**没有**被掐断"
          "（总耗时约 " + std::to_string(kChunks * kPaceMs) +
              " ms，远超 " + std::to_string(kIdleMs) + " ms 的闲置预算）");
    check(ended->load() == 1, "survives: on_end 恰好一次");
    check(received->load() == kTotal,
          "survives: 全部 " + std::to_string(kTotal) + " 字节都交付了（实测 " +
              std::to_string(received->load()) + "）");
    check(count_responses(c.rx()) == 1, "survives: 收到一个响应");
  }
  app.stop();
  app.join();
}

// =========================================================================
// 8. 对端中途断开 → on_abort
// =========================================================================
void test_abort_mid_stream() {
  std::shared_ptr<std::atomic<int> > aborted(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > ended(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > got_data(new std::atomic<int>(0));

  uvcpp_web_app app;
  configure_for_test(app);
  app.post_stream("/cut", [aborted, ended, got_data](
                              uvcpp_web_request& req, uvcpp_web_response& resp,
                              uvcpp_web_next) {
    uvcpp_web_stream* s = req.stream();
    if (s == nullptr) return;
    s->on_data([got_data](const char*, size_t) -> bool {
      got_data->fetch_add(1);
      return true;
    });
    s->on_abort([aborted]() { aborted->fetch_add(1); });
    s->on_end([&resp, ended]() {
      ended->fetch_add(1);
      resp.status(200).text("ok");
      resp.end();
    });
  });

  check(app.start_background() == 0, "abort: 服务启动");
  const int port = app.bound_port();

  {
    staged_conn c;
    if (c.open(port)) {
      // 声明 10000，发 100，然后**直接析构**（staged_conn 的析构会 FIN 关闭）。
      check(c.write(post_head("/cut", 10000)), "abort: 发出请求头");
      check(c.write(std::string(100, 'z')), "abort: 发出开头的一小块");
      c.pump(200);
      check(got_data->load() >= 1, "abort: 断开之前确实收到过数据");
    }
    // c 在这里析构 → 服务端看到对端断开。
  }

  check(wait_for([&]() { return aborted->load() > 0; }, 3000),
        "abort: 对端断开触发了 on_abort");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  check(aborted->load() == 1,
        "abort: on_abort **恰好一次**（实测 " + std::to_string(aborted->load()) +
            "）");
  check(ended->load() == 0, "abort: on_abort 与 on_end 互斥，on_end 零次");

  app.stop();
  app.join();
}

// =========================================================================
// 9. 进度回调
// =========================================================================
void test_progress_reported() {
  const size_t kBody = 600;

  std::shared_ptr<std::atomic<int> > calls(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > monotonic(new std::atomic<int>(1));
  std::shared_ptr<std::atomic<uint64_t> > last(new std::atomic<uint64_t>(0));
  std::shared_ptr<std::atomic<uint64_t> > final_got(
      new std::atomic<uint64_t>(0));
  std::shared_ptr<std::atomic<uint64_t> > final_total(
      new std::atomic<uint64_t>(0));
  std::shared_ptr<std::atomic<int> > bad_total(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > ended(new std::atomic<int>(0));

  uvcpp_web_app app;
  configure_for_test(app);
  app.post_stream("/prog", [calls, monotonic, last, final_got, final_total,
                            bad_total, ended](uvcpp_web_request& req,
                                              uvcpp_web_response& resp,
                                              uvcpp_web_next) {
    uvcpp_web_stream* s = req.stream();
    if (s == nullptr) return;
    s->on_progress([calls, monotonic, last, final_got, final_total,
                    bad_total](uint64_t got, uint64_t total) {
      calls->fetch_add(1);
      if (got < last->load()) monotonic->store(0);
      last->store(got);
      final_got->store(got);
      final_total->store(total);
      if (total == 0) bad_total->fetch_add(1);
    });
    s->on_end([&resp, ended]() {
      ended->fetch_add(1);
      resp.status(200).text("ok");
      resp.end();
    });
  });

  check(app.start_background() == 0, "progress: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    check(c.write(post_head("/prog", kBody)), "progress: 发出请求头");
    // 分 6 次写，逼出多次进度回调。
    const size_t step = kBody / 6;
    for (int i = 0; i < 6; ++i) {
      const size_t off = step * i;
      const size_t len = (i == 5) ? (kBody - off) : step;
      check(c.write(std::string(len, 'p')), "progress: 写第 " +
                                                std::to_string(i + 1) + " 块");
      c.pump(40);
    }
    c.pump_until([&]() { return ended->load() > 0; }, 3000);
    c.pump(100);

    check(calls->load() >= 1, "progress: 进度回调至少被调用一次");
    check(monotonic->load() == 1, "progress: 已收字节单调不减");
    check(bad_total->load() == 0,
          "progress: 有 Content-Length 时 total 从不为 0");
    check(final_got->load() == kBody,
          "progress: 终值 == 总长（实测 " + std::to_string(final_got->load()) +
              "，应为 " + std::to_string(kBody) + "）");
    check(final_total->load() == kBody,
          "progress: 报出的 total == 声明值（实测 " +
              std::to_string(final_total->load()) + "）");
  }
  app.stop();
  app.join();
}

// =========================================================================
// 10. 流式请求之后同一条连接照常
// =========================================================================
void test_keepalive_after_stream() {
  std::shared_ptr<std::atomic<int> > stream_done(new std::atomic<int>(0));

  uvcpp_web_app app;
  configure_for_test(app);
  app.post_stream("/up", [stream_done](uvcpp_web_request& req,
                                       uvcpp_web_response& resp,
                                       uvcpp_web_next) {
    uvcpp_web_stream* s = req.stream();
    if (s == nullptr) return;
    s->on_end([&resp, stream_done]() {
      stream_done->fetch_add(1);
      resp.status(200).text("streamed");
      resp.end();
    });
  });
  app.get("/ping", [](uvcpp_web_request&, uvcpp_web_response& resp,
                      uvcpp_web_next) {
    resp.status(200).text("pong");
    resp.end();
  });

  check(app.start_background() == 0, "keepalive: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    const std::string body = "first";
    check(c.write(post_head("/up", body.size()) + body), "keepalive: 流式请求");
    c.pump_until([&]() { return stream_done->load() > 0; }, 3000);
    c.pump_until([&]() { return count_responses(c.rx()) >= 1; }, 2000);

    // **同一条连接**上接着发一个普通 GET。
    check(c.write("GET /ping HTTP/1.1\r\nHost: t\r\n\r\n"),
          "keepalive: 在同一条连接上发出第二个请求");
    c.pump_until([&]() { return count_responses(c.rx()) >= 2; }, 3000);
    c.pump(150);

    const std::string& wire = c.rx();
    check(count_responses(wire) == 2,
          "keepalive: 两个响应都到了（实测 " +
              std::to_string(count_responses(wire)) + " 个）");
    check(!c.peer_closed(),
          "keepalive: 流式响应没有把连接弄断（keep-alive 保住了）");
    const size_t p1 = wire.find("streamed");
    const size_t p2 = wire.find("pong");
    check(p1 != std::string::npos && p2 != std::string::npos,
          "keepalive: 两个响应体都在");
    check(p1 < p2, "keepalive: 响应**按请求顺序**返回");
  }
  app.stop();
  app.join();
}

// =========================================================================
// 11. 非流式路由不受影响（回归网）
// =========================================================================
void test_normal_route_unaffected() {
  std::shared_ptr<std::string> plain_body(new std::string());
  std::shared_ptr<std::atomic<int> > plain_calls(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > stream_calls(new std::atomic<int>(0));

  uvcpp_web_app app;
  configure_for_test(app);
  // 有流式路由存在，所以认领钩子是装着的 —— 每一条普通请求都要先过它。
  app.post_stream("/streamed", [stream_calls](uvcpp_web_request& req,
                                              uvcpp_web_response& resp,
                                              uvcpp_web_next) {
    stream_calls->fetch_add(1);
    uvcpp_web_stream* s = req.stream();
    if (s == nullptr) return;
    s->on_end([&resp]() {
      resp.status(200).text("s");
      resp.end();
    });
  });
  app.post("/plain", [plain_body, plain_calls](uvcpp_web_request& req,
                                               uvcpp_web_response& resp,
                                               uvcpp_web_next) {
    plain_calls->fetch_add(1);
    // 普通路由拿到的必须是**完整**的 body —— 认领钩子没命中时，HTTP 层
    // 应当照旧把它攒完。
    plain_body->assign(req.body_str());
    resp.status(200).text("plain");
    resp.end();
  });

  check(app.start_background() == 0, "normal: 服务启动");
  const int port = app.bound_port();

  // --- (a) 普通 POST 路由拿到完整 body ---
  uvcpp_http_response r;
  {
    uvcpp_http_request req;
    req.method = http_method::HTTP_POST;
    req.url    = "/plain";
    const std::string body = "the whole thing";
    req.body.append_data(body.data(), body.size());
    bool ok = false;
    for (int i = 0; i < 40 && !ok; ++i) {
      uvcpp_http_client client;
      if (client.connect_wait("127.0.0.1", port, 2000) == 0 &&
          client.send_wait(req, r, 3000) == 0) {
        ok = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    check(ok, "normal: 普通 POST 往返成功");
  }
  check(plain_calls->load() == 1, "normal: 普通路由被调用了");
  check(*plain_body == "the whole thing",
        "normal: 普通路由拿到**完整** body（实测 '" + *plain_body + "'）");
  check(stream_calls->load() == 0, "normal: 流式路由没被误触发");

  // --- (b) 流式路由照常工作（证明 (a) 不是因为整个机制坏了才通过的）---
  staged_conn c;
  if (c.open(port)) {
    const std::string body = "abc";
    check(c.write(post_head("/streamed", body.size()) + body),
          "normal: 流式请求");
    c.pump_until([&]() { return count_responses(c.rx()) >= 1; }, 3000);
    c.pump(100);
    check(stream_calls->load() == 1, "normal: 流式路由被调用了恰好一次");
    check(count_responses(c.rx()) == 1, "normal: 流式路由也回了一个响应");
  }

  app.stop();
  app.join();
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  const std::string filter = (argc > 1) ? argv[1] : std::string();

  struct { const char* name; void (*fn)(); } tests[] = {
    {"handler_runs_before_body", test_handler_runs_before_body},
    {"chunks_in_order", test_chunks_in_order},
    {"path_params_and_query", test_path_params_and_query},
    {"on_end_then_response", test_on_end_then_response},
    {"pause_is_real", test_pause_is_real},
    {"stall_killed", test_stall_killed},
    {"slow_but_progressing_survives", test_slow_but_progressing_survives},
    {"abort_mid_stream", test_abort_mid_stream},
    {"progress_reported", test_progress_reported},
    {"keepalive_after_stream", test_keepalive_after_stream},
    {"normal_route_unaffected", test_normal_route_unaffected},
  };

  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    if (!filter.empty() &&
        std::string(tests[i].name).find(filter) == std::string::npos) {
      continue;
    }
    // 先打名字再跑：进程中途挂掉也能从最后一行看出死在哪一条。
    std::cout << "[web_app_stream] " << tests[i].name << std::endl;
    const int before = g_failures;
    tests[i].fn();
    std::cout << "  -> " << (g_failures == before ? "PASS" : "FAIL") << std::endl;
  }

  std::cout << "[web_app_stream] " << (g_failures == 0 ? "ALL PASS" : "FAIL")
            << std::endl;
  return g_failures == 0 ? 0 : 2;
}

#else
int main() { return 0; }
#endif
