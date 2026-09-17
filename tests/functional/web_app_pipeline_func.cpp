/**
 * @file tests/functional/web_app_pipeline_func.cpp
 * @brief HTTP 流水线：一次读出多条请求时，**响应按请求顺序发出**、头部互不串味、
 *        以及超出同连接上限时的 503 + 关连接。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么单独一个文件：本文件打的是**一条连接上多条在途请求**这件事，而其余
 * `web_app_*_func.cpp` 全部是"一问一答"（哪怕 keep-alive 也是等第一条响应回来
 * 才发第二条）。流水线把两条请求挤进**同一个读缓冲**，于是两条路径同时被压到：
 *
 *   1. 解析器 —— `execute()` 一次吃下整个缓冲区，第二条消息的累积缓冲会叠在
 *      第一条上，产出 `/slow/fast` 这样的 URL 和"读到上一条 `Host`/`X-Probe`"
 *      的头部；
 *   2. 框架 —— 响应必须按到达顺序发出（RFC 7230 §6.3.2）。慢的第一条不能被
 *      快的第二条超车。
 *
 * 覆盖：
 *   1. `order_async_then_fast`      第一条走异步中间件（150 ms）——它必须**先**发
 *   2. `headers_do_not_cross_talk`  两条各带不同 `X-Probe`，各自只看得见自己那个
 *   3. `two_sync_routes`            两条同步路由 —— 最纯粹的顺序回归（改前 200+404）
 *   4. `chunked_stream_not_split`   队首是 chunked 流式响应时，排队的响应必须等
 *                                   终止块 `0\r\n\r\n` 落线之后才能发
 *   5. `queued_chunked_response`    chunked 流式响应**排在队里**（头部还没发，
 *                                  帧先攒着）—— 与 4 是两条不同的路径
 *   6. `overflow_rejected`          `set_max_pipelined_requests(2)` + 3 条 → 第三条
 *                                   503 且带 `Connection: close`，前面的照常发完
 *   7. `disconnect_drains_inflight` 流水线两条后立刻断开，在途计数必须归零
 *
 * 刻意**不覆盖**：HTTP/2、请求侧的读背压（`pause()` 在流水线下的语义）、
 * `Connection: close` 之后同缓冲里剩余请求的处理（会被协议层直接丢弃，见 5）。
 *
 * 崩溃时的定位手段：每个用例**先打名字再跑**，进程中途挂掉也能从最后一行
 * 看出死在哪一条。
 */
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

// =========================================================================
// 客户端侧小工具
// =========================================================================

/** @brief 轮询等待一个条件成立，最长 timeout_ms 毫秒（不泵循环，App 有自己的）。 */
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

void configure_for_test(uvcpp_web_app& app) {
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);
}

/** @brief 一条 HTTP/1.1 请求（无 body）。**流水线必须把它们拼成一次 write()**。 */
std::string get_req(const std::string& path, const std::string& extra = "") {
  return "GET " + path + " HTTP/1.1\r\nHost: t\r\n" + extra + "\r\n";
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

std::string to_lower(std::string s) {
  for (size_t i = 0; i < s.size(); ++i) {
    s[i] = static_cast<char>(
        std::tolower(static_cast<unsigned char>(s[i])));
  }
  return s;
}

struct resp_info {
  int status;
  std::string head;  // 状态行 + 头部 + 结尾空行，原样
  std::string body;
};

/**
 * @brief 把线上字节切成逐条响应。
 *
 * 按 `Content-Length` 定位 body，所以**只认整包响应**；chunked 的那条 body 会是
 * 空的（本文件对 chunked 用位置断言，不用这个）。
 */
std::vector<resp_info> parse_responses(const std::string& wire) {
  std::vector<resp_info> out;
  size_t p = 0;
  for (;;) {
    const size_t s = wire.find("HTTP/1.1 ", p);
    if (s == std::string::npos) break;
    const size_t he = wire.find("\r\n\r\n", s);
    if (he == std::string::npos) break;

    resp_info r;
    r.status = std::atoi(wire.c_str() + s + 9);
    r.head = wire.substr(s, he + 4 - s);
    const std::string low = to_lower(r.head);
    size_t cl = low.find("content-length:");
    size_t len = 0;
    if (cl != std::string::npos) {
      len = static_cast<size_t>(
          std::strtoul(low.c_str() + cl + 15, nullptr, 10));
    }
    if (he + 4 + len <= wire.size()) r.body = wire.substr(he + 4, len);
    out.push_back(r);
    p = he + 4 + len;
  }
  return out;
}

/**
 * @brief 分阶段裸客户端：想什么时候写就什么时候写，随时读回已到达的字节。
 */
class staged_conn {
 public:
  staged_conn()
      : loop_(nullptr), connected_(false), written_(false), closed_(false) {}

  bool open(int port, int timeout_ms = 3000) {
    int rc = c_.connect("127.0.0.1", port, [this](int st) {
      if (st != 0) return;
      connected_ = true;
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
    return uvcpp_test::wait_until(loop_, [this]() { return connected_; },
                                  timeout_ms);
  }

  /**
   * @brief 写一段字节，泵到写完成。
   *
   * **必须等写完成，且失败要当回事**：`uvcpp_tcp_client` 同一时刻只允许一个
   * 异步写，撞上在途写会返回 `UV_EALREADY` 并**把这一块丢掉**。流水线的形状
   * 恰恰是"两条请求挤在一次写里"，所以这里失败就意味着整个用例的前提没了。
   */
  bool write(const std::string& s, int timeout_ms = 3000) {
    if (s.empty()) return true;
    written_ = false;
    int rc = c_.write(s.data(), s.size(), [this](int) { written_ = true; });
    if (rc != 0) return false;
    return uvcpp_test::wait_until(loop_, [this]() { return written_; },
                                  timeout_ms);
  }

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

/**
 * @brief 中间件里起的后台线程：析构时 join。
 *
 * **必须在 `uvcpp_web_app` 之前声明**（析构逆序 → 它比 App 活得久）：线程体里
 * 调的是 `next()`，而 `next()` 会 `post()` 回 App 的循环。App 已经析构而线程还在
 * 跑，就是一次 use-after-free。
 */
class worker_pool {
 public:
  void spawn(std::function<void()> fn) {
    th_.push_back(std::shared_ptr<std::thread>(new std::thread(fn)));
  }

  ~worker_pool() {
    for (size_t i = 0; i < th_.size(); ++i) {
      if (th_[i]->joinable()) th_[i]->join();
    }
  }

 private:
  std::vector<std::shared_ptr<std::thread> > th_;
};

/**
 * @brief 给 `/slow`、`/defer`、`/sse` 挂一个"在别的线程上睡 ms 毫秒再 next()"的
 *        中间件 —— 这就是"慢的第一条"。
 */
void install_delayer(uvcpp_web_app& app, worker_pool& pool, int ms) {
  app.use([&pool, ms](uvcpp_web_request& req, uvcpp_web_response& resp,
                      uvcpp_web_next next) {
    (void)resp;
    const std::string p = req.path();
    if (p != "/slow" && p != "/defer" && p != "/sse") {
      next();
      return;
    }
    pool.spawn([next, ms]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      next();
    });
  });
}

void add_fast_route(uvcpp_web_app& app, const char* path, const char* body) {
  const std::string b = body;
  app.get(path, [b](uvcpp_web_request&, uvcpp_web_response& resp,
                    uvcpp_web_next) {
    resp.status(200).text(b);
    resp.end();
  });
}

// =========================================================================
// 1. 顺序：第一条走异步，第二条是同步快路由
// =========================================================================
void test_order_async_then_fast() {
  worker_pool pool;
  uvcpp_web_app app;
  configure_for_test(app);
  install_delayer(app, pool, 150);
  add_fast_route(app, "/slow", "SLOW");
  add_fast_route(app, "/fast", "FAST");

  check(app.start_background() == 0, "order: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    check(c.write(get_req("/slow") + get_req("/fast")),
          "order: 一次 write 写出两条请求（流水线的前提）");
    c.pump_until([&]() { return count_responses(c.rx()) >= 2; }, 4000);
    c.pump(150);

    const std::string wire = c.rx();
    check(count_responses(wire) == 2,
          "order: 两条请求各回一个响应（实测 " +
              std::to_string(count_responses(wire)) + "）");

    // 先按整包切开逐条对 —— 位置断言说得清"顺序"，逐条对说得清"哪条是哪条"。
    const std::vector<resp_info> rs = parse_responses(wire);
    check(rs.size() == 2, "order: 两条响应都完整（实测 " +
                              std::to_string(rs.size()) + "）");
    if (rs.size() == 2) {
      check(rs[0].body == "SLOW",
            "order: 第一条响应的正文是 SLOW（实测 '" + rs[0].body + "'）");
      check(rs[1].body == "FAST",
            "order: 第二条响应的正文是 FAST（实测 '" + rs[1].body + "'）");
    }

    // 这条是本用例存在的理由：修之前 /fast 会超车 /slow，FAST 的字节先落线。
    const size_t i_slow = wire.find("SLOW");
    const size_t i_fast = wire.find("FAST");
    check(i_slow != std::string::npos && i_fast != std::string::npos &&
              i_slow < i_fast,
          "order: 慢的第一条**先**发（SLOW 的下标 "
          "必须小于 FAST 的）");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 2. 头部不串味（同时是解析器累积缺陷的回归）
// =========================================================================
void test_headers_do_not_cross_talk() {
  uvcpp_web_app app;
  configure_for_test(app);
  app.get("/echo", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                      uvcpp_web_next) {
    // 正文里带上"看到的 X-Probe"和"一共几个头"：前者抓串味，后者抓累积
    // （第二条读到上一条的头时，个头数会变成 4）。
    resp.status(200).text("E[" + req.header("x-probe", "<none>") + "]n=" +
                          std::to_string(req.headers().size()));
    resp.end();
  });

  check(app.start_background() == 0, "headers: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    check(c.write(get_req("/echo", "X-Probe: FIRST\r\n") +
                  get_req("/echo", "X-Probe: SECOND\r\n")),
          "headers: 一次 write 写出两条带不同 X-Probe 的请求");
    c.pump_until([&]() { return count_responses(c.rx()) >= 2; }, 4000);
    c.pump(150);

    const std::vector<resp_info> rs = parse_responses(c.rx());
    check(rs.size() == 2, "headers: 两个响应（实测 " +
                              std::to_string(rs.size()) + "）");
    if (rs.size() == 2) {
      check(rs[0].body == "E[FIRST]n=2",
            "headers: 第一条只看得见自己那个头（实测 '" + rs[0].body + "'）");
      check(rs[1].body == "E[SECOND]n=2",
            "headers: 第二条只看得见自己那个头，且头部没有叠加"
            "（实测 '" + rs[1].body + "'）");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 3. 两条同步路由 —— 最纯粹的顺序回归
// =========================================================================
void test_two_sync_routes() {
  uvcpp_web_app app;
  configure_for_test(app);
  add_fast_route(app, "/a", "AAA");
  add_fast_route(app, "/b", "BBB");

  check(app.start_background() == 0, "sync: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    check(c.write(get_req("/a") + get_req("/b")), "sync: 一次 write 两条");
    c.pump_until([&]() { return count_responses(c.rx()) >= 2; }, 4000);
    c.pump(150);

    const std::vector<resp_info> rs = parse_responses(c.rx());
    check(rs.size() == 2, "sync: 两条响应（实测 " + std::to_string(rs.size()) +
                              "）—— 改前这里是 1 条 200 + 1 条 404");
    if (rs.size() == 2) {
      check(rs[0].status == 200 && rs[0].body == "AAA", "sync: 第一条 /a");
      check(rs[1].status == 200 && rs[1].body == "BBB", "sync: 第二条 /b");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 4. 队首是 chunked 流式响应时，排队的响应必须等终止块落线
// =========================================================================
void test_chunked_stream_not_split() {
  worker_pool pool;
  uvcpp_web_app app;
  configure_for_test(app);
  install_delayer(app, pool, 120);
  app.get("/sse", [](uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next) {
    resp.begin_chunked("text/event-stream");
    resp.write_chunk("one");
    resp.write_chunk("two");
    resp.end();
  });
  add_fast_route(app, "/fast", "FAST");

  check(app.start_background() == 0, "chunked: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    check(c.write(get_req("/sse") + get_req("/fast")), "chunked: 一次 write 两条");
    c.pump_until([&]() { return count_responses(c.rx()) >= 2; }, 4000);
    c.pump(150);

    const std::string wire = c.rx();
    check(count_responses(wire) == 2,
          "chunked: 两条响应（实测 " + std::to_string(count_responses(wire)) +
              "）");

    const std::string frames = "3\r\none\r\n3\r\ntwo\r\n0\r\n\r\n";
    const size_t i_frames = wire.find(frames);
    const size_t i_fast = wire.rfind("HTTP/1.1 ");
    check(i_frames != std::string::npos,
          "chunked: 流式响应的三段帧逐字节完整（改前会被插进别的东西）");

    // 判据：终止块在**第二条响应**的状态行之前。反过来说，排队的 /fast 没有
    // 插进流中间 —— 那样它就会落在 i_frames 里面。
    check(i_frames != std::string::npos && i_fast != std::string::npos &&
              i_frames < i_fast,
          "chunked: 排队的响应没有插进流中间（终止块先落线）");

    const size_t i_body = wire.find("FAST");
    check(i_frames != std::string::npos && i_body != std::string::npos &&
              i_frames < i_body,
          "chunked: FAST 的正文在终止块之后");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 5. 流式响应**排在队里**（不是队首）—— 头部还没发，chunk 先攒着
// =========================================================================
//
// 与用例 4 是**两条不同的路径**，不能互相代替：用例 4 里流式那条是队首，
// `begin_chunked()` 时头部当场就发出去了；这里它是第二条，`begin_chunked()`
// 与几次 `write_chunk()` 都发生在它**还排在别人后面**的时候 —— 那一刻 sink
// 还没装上，帧只能攒在 `pending_buf_` 里，等轮到自己才由 `pump_stream()` 一次性
// 冲出去。头部、帧、终止块三者的**相对顺序**正是这条路径上的判据。
void test_queued_chunked_response() {
  worker_pool pool;
  uvcpp_web_app app;
  configure_for_test(app);
  install_delayer(app, pool, 150);
  add_fast_route(app, "/slow", "SLOW");
  // `/qchunk` 不在 install_delayer 的名单里 —— 它是流水线里**第二条**，
  // 不能也被推迟（推迟的话它就成了队首，用例要的形状就没了）。
  app.get("/qchunk", [](uvcpp_web_request&, uvcpp_web_response& resp,
                        uvcpp_web_next) {
    resp.begin_chunked("text/event-stream");
    resp.write_chunk("one");
    resp.write_chunk("two");
    resp.end();
  });

  check(app.start_background() == 0, "queued-chunked: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    check(c.write(get_req("/slow") + get_req("/qchunk")),
          "queued-chunked: 一次 write 写出两条");
    c.pump_until([&]() { return count_responses(c.rx()) >= 2; }, 4000);
    c.pump(150);

    const std::string wire = c.rx();
    check(count_responses(wire) == 2,
          "queued-chunked: 两条响应（实测 " +
              std::to_string(count_responses(wire)) + "）");

    const size_t i_slow = wire.find("SLOW");
    const size_t i_head = to_lower(wire).find("transfer-encoding: chunked");
    const std::string frames = "3\r\none\r\n3\r\ntwo\r\n0\r\n\r\n";
    const size_t i_frames = wire.find(frames);

    // 慢的那条必须**整体**先落线（连流式那边的头部都还没开始发）。
    check(i_slow != std::string::npos && i_head != std::string::npos &&
              i_slow < i_head,
          "queued-chunked: 排队期间流式的**头部**没有提前发出");
    // 完整性先判：帧缺了的话，下面那条顺序断言会以"头部在帧之前"报红，
    // 那句话会把"帧根本没发"说成"顺序不对"，是误导性的失败信息。
    check(i_frames != std::string::npos,
          "queued-chunked: 攒着的两块 + 终止块逐字节完整"
          "（sink 是轮到自己才装上的）");
    check(i_head != std::string::npos && i_frames != std::string::npos &&
              i_head < i_frames,
          "queued-chunked: 头部在帧之前");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 6. 超出同连接上限 → 503 + Connection: close，前面的照常发完
// =========================================================================
void test_overflow_rejected() {
  worker_pool pool;
  uvcpp_web_app app;
  configure_for_test(app);
  app.set_max_pipelined_requests(2);
  install_delayer(app, pool, 120);
  add_fast_route(app, "/defer", "OK");

  check(app.start_background() == 0, "overflow: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    check(c.write(get_req("/defer") + get_req("/defer") + get_req("/defer")),
          "overflow: 一次 write 三条（上限 2）");
    c.pump_until([&]() { return count_responses(c.rx()) >= 3; }, 4000);
    c.pump(200);

    const std::vector<resp_info> rs = parse_responses(c.rx());
    check(rs.size() == 3, "overflow: 三个响应都发了（含被拒的那条，实测 " +
                              std::to_string(rs.size()) + "）");
    if (rs.size() == 3) {
      check(rs[0].status == 200 && rs[1].status == 200,
            "overflow: 上限内的两条照常 200");
      check(rs[2].status == 503, "overflow: 第三条 503（实测 " +
                                     std::to_string(rs[2].status) + "）");
      check(to_lower(rs[2].head).find("connection: close") != std::string::npos,
            "overflow: 503 带 Connection: close");
    }

    check(c.pump_until([&]() { return c.peer_closed(); }, 3000),
          "overflow: 拒绝之后连接被关掉");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 6. 流水线两条后立刻断开 → 在途计数归零
// =========================================================================
void test_disconnect_drains_inflight() {
  worker_pool pool;
  uvcpp_web_app app;
  configure_for_test(app);
  install_delayer(app, pool, 100);
  add_fast_route(app, "/defer", "OK");

  check(app.start_background() == 0, "drain: 服务启动");
  const int port = app.bound_port();

  {
    staged_conn c;
    if (c.open(port)) {
      check(c.write(get_req("/defer") + get_req("/defer")),
            "drain: 一次 write 两条");
      // 等到两条都进了在途表（响应还没发），再断开。
      check(wait_for([&]() { return app.inflight_count() >= 2; }, 2000),
            "drain: 两条都在途");
    }
    // c 在这里析构 → 服务端看到对端断开。
  }

  check(wait_for([&]() { return app.inflight_count() == 0; }, 4000),
        "drain: 断开后在途计数归零（实测 " +
            std::to_string(app.inflight_count()) + "）");

  app.stop();
  app.join();
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  const std::string filter = (argc > 1) ? argv[1] : std::string();

  struct { const char* name; void (*fn)(); } tests[] = {
    {"order_async_then_fast", test_order_async_then_fast},
    {"headers_do_not_cross_talk", test_headers_do_not_cross_talk},
    {"two_sync_routes", test_two_sync_routes},
    {"chunked_stream_not_split", test_chunked_stream_not_split},
    {"queued_chunked_response", test_queued_chunked_response},
    {"overflow_rejected", test_overflow_rejected},
    {"disconnect_drains_inflight", test_disconnect_drains_inflight},
  };

  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    if (!filter.empty() &&
        std::string(tests[i].name).find(filter) == std::string::npos) {
      continue;
    }
    // 先打名字再跑：进程中途挂掉也能从最后一行看出死在哪一条。
    std::cout << "[web_app_pipeline] " << tests[i].name << std::endl;
    const int before = g_failures;
    tests[i].fn();
    std::cout << "  -> " << (g_failures == before ? "PASS" : "FAIL") << std::endl;
  }

  std::cout << "[web_app_pipeline] " << (g_failures == 0 ? "ALL PASS" : "FAIL")
            << std::endl;
  return g_failures == 0 ? 0 : 2;
}

#else
int main() { return 0; }
#endif
