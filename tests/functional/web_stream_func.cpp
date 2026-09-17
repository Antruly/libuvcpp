/**
 * @file tests/functional/web_stream_func.cpp
 * @brief `src/web` 层的流式体交付：认领钩子、100-continue、提前 413、417。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一层测的是**协议时机**，不是框架。所以全部用裸 TCP 客户端手写线上字节
 * 分阶段发送 —— `uvcpp_http_client` 只会"整包发完再收"，而这里每一条断言
 * 考的都是"某个响应在 body 发完**之前**就出现了"。
 *
 * 覆盖：
 *   1. `claim_matched`              认领后 body 逐块送达、END 送达、
 *                                   `on_request` 一次都不调，且**不受**
 *                                   `max_body_size` 约束（body 没被缓冲）
 *   2. `claim_missed_falls_through` 钩子返回空 → 走正常路由，body 完整
 *                                   **对照组**：没有它，"见请求就认领"的实现
 *                                   也能过第 1 组
 *   3. `expect_continue`            线上**先**出现 100 Continue，**再**是最终响应
 *   4. `expect_unknown_417`         `Expect: foo` → 417 + 连接关闭
 *   5. `early_413_by_content_length` 声明超过上限、body 只发一小半 →
 *                                   **不等发完**就回 413（用"我们根本没发完"
 *                                   作为判据）
 *   6. `chunked_413_at_complete`    chunked（无 content-length）仍走原有的
 *                                   message-complete 413
 *                                   **对照组**：证明提前 413 没把老路径改坏
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_server.h>

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
// 分阶段裸客户端
// =========================================================================

/**
 * @brief 连上去、想什么时候写就什么时候写、随时读回已到达的字节。
 *
 * 整个文件的判据都依赖"**发了一半就去看对端说了什么**"，所以不能用
 * `raw_exchange`（它发完整包才读）。这里把三件事拆开：`write()` 发一段、
 * `pump()` 泵一段时间收字节、`rx()` 看收到的东西。
 *
 * 线程：连接与其 loop 都在**本线程**上（服务端在后台线程），所以下面这些
 * 标志不需要原子 —— 回调都是 `loop_->run()` 在我们自己栈上跑出来的。
 */
class raw_conn {
 public:
  raw_conn() : loop_(nullptr), connected_(false), written_(false), closed_(false) {}

  bool open(int port) {
    int rc = c_.connect("127.0.0.1", port, [this](int st) {
      if (st != 0) return;
      connected_ = true;
      // 起读必须在连上之后（socket 还没有时装回调会静默收不到任何字节）。
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
    return pump_until([this]() { return connected_; }, 2000);
  }

  /** @brief 写一段字节，泵到写完成。 */
  bool write(const std::string& s) {
    if (s.empty()) return true;
    written_ = false;
    int rc = c_.write(s.data(), s.size(), [this](int) { written_ = true; });
    if (rc != 0) return false;
    return pump_until([this]() { return written_; }, 2000);
  }

  /** @brief 泵 ms 毫秒（每毫秒一轮 NOWAIT）。 */
  /// 上限是**墙钟**毫秒，不是圈数：一圈的代价就是系统定时器粒度（Windows 无
  /// 请求者时默认 15.625 ms），按圈数计时在粗粒度机器上会整体放大约 8 倍。
  void pump(int ms) { uvcpp_test::pump_for(loop_, ms); }

  /** @brief 泵到条件成立或超时。 */
  template <typename Pred>
  bool pump_until(Pred pred, int timeout_ms) {
    return uvcpp_test::wait_until(loop_, pred, timeout_ms);
  }

  const std::string& rx() const { return rx_; }
  bool peer_closed() const { return closed_; }

  ~raw_conn() {
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

/** @brief 收到的字节里有没有某个子串。 */
bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

/**
 * @brief 把一个响应写到连接上。
 *
 * 认领了请求之后 http 层的 `send_response` 不再适用（它按自己的 `conn_ctx`
 * 记账），所以这些用例自己写。**必须先把 `to_string()` 存进具名变量** ——
 * 在同一个表达式里调两次会得到两个不同的临时对象，`.c_str()` 那个在分号前
 * 就析构了。（`const char*` 重载内部会把字节拷进 `uvcpp_buf`，所以局部变量
 * 本身可以在 `write()` 返回后消亡。）
 */
void write_response(uvcpp_tcp_client* client, const uvcpp_http_response& resp) {
  if (client == nullptr) return;
  const std::string wire = resp.to_string();
  client->write(wire.c_str(), wire.size(), [](int) {});
}

/** @brief 响应状态行（"HTTP/1.1 413 ..."）里的码，找不到返回 0。 */
int status_in(const std::string& wire) {
  const size_t p = wire.find("HTTP/1.1 ");
  if (p == std::string::npos) return 0;
  return std::atoi(wire.c_str() + p + 9);
}

// =========================================================================
// 测试用服务器（沿 web_http_server_func.cpp 的后台线程模式）
// =========================================================================

using SetupFn = std::function<void(uvcpp_http_server&)>;

struct test_server {
  std::promise<int> port_promise;
  std::atomic<bool> stop;
  std::thread       thread;

  test_server() : stop(false) {}

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

      // **先 listen，再发布端口**：反过来的话调用方立刻 connect 会拿到
      // ECONNREFUSED（"已 bind、尚未 listen"在内核里是拒连的）。
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
};

/** @brief 一次 POST 的请求头。 */
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

// =========================================================================
// 1. 认领命中
// =========================================================================
void test_claim_matched() {
  // 认领**绕过** `max_body_size` —— 因为 body 从来不进服务器的缓冲。上限设得
  // 极小，好让"claim 的 body 没被服务器缓冲"这件事变成可断言的事实。
  const size_t kCap    = 16;
  const size_t kBody   = 300;

  std::shared_ptr<std::atomic<int> > headers_ev(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > body_ev(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > end_ev(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > request_calls(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > headers_had_body(new std::atomic<int>(-1));
  std::shared_ptr<std::string>       got(new std::string());

  test_server srv;
  int port = srv.start([&](uvcpp_http_server& s) {
    s.set_max_body_size(kCap);
    s.on_request([request_calls](uvcpp_http_request&, uvcpp_http_response& resp,
                                 uvcpp_tcp_client* client) {
      // 认领了就不该走到这儿。真走到了至少得回点什么，否则客户端挂死。
      request_calls->fetch_add(1);
      write_response(client, uvcpp_http_response::ok("unclaimed", 9));
    });
    s.set_stream_claim([headers_ev, body_ev, end_ev, headers_had_body,
                        got](uvcpp_http_request& req, uvcpp_tcp_client* client)
                           -> http_stream_handler {
      if (req.url != "/upload") return http_stream_handler();

      // HEADERS 事件与认领钩子在**同一个同步栈**上：这里拿到的 req 必须是
      // 只有头、没有 body 的那一份。
      headers_ev->fetch_add(1);
      headers_had_body->store(req.body.size() == 0 ? 1 : 0);

      return [client, body_ev, end_ev, got](
                 http_stream_event ev, const char* data, size_t len,
                 uvcpp_http_request&, uvcpp_tcp_client*) {
        switch (ev) {
          case http_stream_event::BODY:
            body_ev->fetch_add(1);
            got->append(data, len);
            break;
          case http_stream_event::END: {
            end_ev->fetch_add(1);
            write_response(client,
                           uvcpp_http_response::ok(got->c_str(), got->size()));
            break;
          }
          default:
            break;
        }
      };
    });
  });

  check(port > 0, "claim_matched: 服务器起来了");
  if (port > 0) {
    raw_conn c;
    if (c.open(port)) {
      const std::string body(kBody, 'z');
      c.write(post_head("/upload", kBody));
      // 分三块写，每块之间泵一会儿 —— 让 body 回调至少有机会分多次到达。
      const size_t chunk = kBody / 3;
      c.write(body.substr(0, chunk));
      c.pump(60);
      c.write(body.substr(chunk, chunk));
      c.pump(60);
      c.write(body.substr(2 * chunk));
      c.pump_until([&]() { return end_ev->load() > 0; }, 2000);
      c.pump(200);

      check(headers_ev->load() == 1, "claim_matched: HEADERS 恰好一次");
      check(headers_had_body->load() == 1,
            "claim_matched: HEADERS 时请求 body 为空（body 还没到）");
      check(body_ev->load() >= 1, "claim_matched: 至少收到一块 body");
      check(end_ev->load() == 1, "claim_matched: END 恰好一次");
      check(*got == body,
            "claim_matched: 收到的 body 与发出的逐字节相同（"
            "含超过 max_body_size 的部分 —— 认领的 body 不进服务器缓冲）");
      check(request_calls->load() == 0,
            "claim_matched: 认领之后 on_request **一次都不调**");
      check(status_in(c.rx()) == 200, "claim_matched: 收到了 200");
    } else {
      check(false, "claim_matched: 连上服务器");
    }
  }
  srv.shutdown();
}

// =========================================================================
// 2. 认领未命中 → 回落（对照组）
// =========================================================================
void test_claim_missed_falls_through() {
  std::shared_ptr<std::atomic<int> > claim_calls(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<int> > route_calls(new std::atomic<int>(0));
  std::shared_ptr<std::atomic<size_t> > route_body(new std::atomic<size_t>(0));

  test_server srv;
  int port = srv.start([&](uvcpp_http_server& s) {
    s.set_max_body_size(0);  // 不限
    s.set_stream_claim([claim_calls](uvcpp_http_request& req,
                                     uvcpp_tcp_client*) -> http_stream_handler {
      claim_calls->fetch_add(1);
      if (req.url != "/upload") return http_stream_handler();  // 不认领
      return [](http_stream_event, const char*, size_t, uvcpp_http_request&,
                uvcpp_tcp_client*) {};
    });
    s.post("/plain", [route_calls, route_body](uvcpp_http_request& req,
                                               uvcpp_http_response& resp,
                                               uvcpp_tcp_client*) {
      route_calls->fetch_add(1);
      route_body->store(req.body.size());
      resp = uvcpp_http_response::ok("plain", 5);
    });
  });

  check(port > 0, "fallthrough: 服务器起来了");
  if (port > 0) {
    raw_conn c;
    if (c.open(port)) {
      const std::string body(200, 'q');
      c.write(post_head("/plain", body.size()) + body);
      c.pump_until([&]() { return status_in(c.rx()) != 0; }, 2000);
      c.pump(100);

      check(claim_calls->load() == 1, "fallthrough: 钩子被问到过");
      check(route_calls->load() == 1,
            "fallthrough: 钩子返回空 → 请求走了正常路由（**对照组**）");
      check(route_body->load() == body.size(),
            "fallthrough: 正常路由拿到完整 body");
      check(status_in(c.rx()) == 200, "fallthrough: 收到 200");
    } else {
      check(false, "fallthrough: 连上服务器");
    }
  }
  srv.shutdown();
}

// =========================================================================
// 3. Expect: 100-continue
// =========================================================================
void test_expect_continue() {
  std::shared_ptr<std::atomic<size_t> > route_body(new std::atomic<size_t>(0));

  test_server srv;
  int port = srv.start([&](uvcpp_http_server& s) {
    s.set_max_body_size(0);
    s.post("/e", [route_body](uvcpp_http_request& req, uvcpp_http_response& resp,
                              uvcpp_tcp_client*) {
      route_body->store(req.body.size());
      resp = uvcpp_http_response::ok("ok", 2);
    });
  });

  check(port > 0, "expect_continue: 服务器起来了");
  if (port > 0) {
    raw_conn c;
    if (c.open(port)) {
      const std::string body(500, 'e');

      // 只发头，然后**什么 body 都不发**地等一会儿。
      c.write(post_head("/e", body.size(), "expect: 100-continue\r\n"));
      c.pump_until([&]() { return has(c.rx(), "100 Continue"); }, 1500);

      const std::string after_head = c.rx();
      check(has(after_head, "HTTP/1.1 100 Continue"),
            "expect_continue: body 发出去**之前**就收到了 100 Continue");
      check(status_in(after_head) == 100,
            "expect_continue: 此刻线上的第一行确实是 100，不是最终响应");
      check(after_head.find("200") == std::string::npos,
            "expect_continue: 此刻还没有最终响应");

      // 现在才发 body —— 正是 100 的语义（客户端等到了许可再发）。
      c.write(body);
      c.pump_until([&]() { return route_body->load() != 0; }, 2000);
      c.pump(200);

      const std::string all = c.rx();
      const size_t p100 = all.find("100 Continue");
      const size_t p200 = all.find("HTTP/1.1 200");
      check(p100 != std::string::npos && p200 != std::string::npos,
            "expect_continue: 100 和最终响应都出现在线上");
      check(p100 < p200, "expect_continue: 100 排在最终响应**之前**");
      check(route_body->load() == body.size(),
            "expect_continue: 路由拿到了完整 body");
    } else {
      check(false, "expect_continue: 连上服务器");
    }
  }
  srv.shutdown();
}

// =========================================================================
// 4. 未知 Expect → 417
// =========================================================================
void test_expect_unknown_417() {
  std::shared_ptr<std::atomic<int> > route_calls(new std::atomic<int>(0));

  test_server srv;
  int port = srv.start([&](uvcpp_http_server& s) {
    s.set_max_body_size(0);
    s.post("/e", [route_calls](uvcpp_http_request&, uvcpp_http_response& resp,
                               uvcpp_tcp_client*) {
      route_calls->fetch_add(1);
      resp = uvcpp_http_response::ok("ok", 2);
    });
  });

  check(port > 0, "expect_417: 服务器起来了");
  if (port > 0) {
    raw_conn c;
    if (c.open(port)) {
      // 声明的 body 一个字节都不发：417 是在**头**上判出来的，因此照样要回。
      c.write(post_head("/e", 100, "expect: something-else\r\n"));
      c.pump_until([&]() { return status_in(c.rx()) != 0; }, 1500);
      c.pump(200);

      check(status_in(c.rx()) == 417,
            "expect_417: 未知 Expect 得到 417");
      check(has(c.rx(), "connection: close"),
            "expect_417: 带 connection: close");
      check(route_calls->load() == 0, "expect_417: 不进路由");
      // 关闭现在被推迟到消息结束（见 conn_ctx::defer_close_to_message_end），
      // 所以这里只能断言"响应已经发出"，不断言"此刻已收到 FIN"。
    } else {
      check(false, "expect_417: 连上服务器");
    }
  }
  srv.shutdown();
}

// =========================================================================
// 5. 声明超限 → 提前 413
// =========================================================================
void test_early_413_by_content_length() {
  const size_t kCap = 64;

  std::shared_ptr<std::atomic<int> > route_calls(new std::atomic<int>(0));

  test_server srv;
  int port = srv.start([&](uvcpp_http_server& s) {
    s.set_max_body_size(kCap);
    s.post("/big", [route_calls](uvcpp_http_request&, uvcpp_http_response& resp,
                                 uvcpp_tcp_client*) {
      route_calls->fetch_add(1);
      resp = uvcpp_http_response::ok("ok", 2);
    });
  });

  check(port > 0, "early_413: 服务器起来了");
  if (port > 0) {
    raw_conn c;
    if (c.open(port)) {
      // 声明 100000，实际只发 8 字节 —— **剩下的永远不发**。
      //
      // 这就是判据本身：老路径（等整包读完再 413）在这里只会干等到超时，
      // 而提前 413 必须在头解析完的那一刻就回。所以"我们没发完却收到了
      // 413"这件事，只有提前判定才可能成立。
      c.write(post_head("/big", 100000));
      c.write(std::string(8, 'x'));
      c.pump_until([&]() { return status_in(c.rx()) != 0; }, 1500);

      check(status_in(c.rx()) == 413,
            "early_413: body 只发了 8/100000 就收到 413（**不等发完**）");
      check(has(c.rx(), "connection: close"),
            "early_413: 带 connection: close");
      check(route_calls->load() == 0, "early_413: 不进路由");
    } else {
      check(false, "early_413: 连上服务器");
    }
  }
  srv.shutdown();
}

// =========================================================================
// 6. chunked 超限 → 仍是 message-complete 的 413（对照组）
// =========================================================================
void test_chunked_413_at_complete() {
  const size_t kCap = 64;

  test_server srv;
  int port = srv.start([&](uvcpp_http_server& s) {
    s.set_max_body_size(kCap);
    s.post("/chunky", [](uvcpp_http_request&, uvcpp_http_response& resp,
                         uvcpp_tcp_client*) {
      resp = uvcpp_http_response::ok("ok", 2);
    });
  });

  check(port > 0, "chunked_413: 服务器起来了");
  if (port > 0) {
    raw_conn c;
    if (c.open(port)) {
      // 没有 content-length，所以**没有声明值可判** —— 只能等 message
      // complete。这一组是"提前 413 没把老路径改坏"的对照。
      // 注意 `extra` 必须从 post_head 传进去：它自己会补那个结束的空行，
      // 在外面 append 的话头就提前结束了，这一行会变成 body 的第一个字节。
      const std::string head =
          post_head("/chunky", static_cast<size_t>(-1),
                    "transfer-encoding: chunked\r\n");

      const size_t n = 200;
      std::string body;
      char hex[32];
      std::snprintf(hex, sizeof(hex), "%zx", n);
      body += std::string(hex) + "\r\n" + std::string(n, 'y') + "\r\n";
      body += "0\r\n\r\n";

      c.write(head + body);
      c.pump_until([&]() { return status_in(c.rx()) != 0; }, 2000);
      c.pump(200);

      check(status_in(c.rx()) == 413,
            "chunked_413: 无 content-length 的超限 chunked body 仍是 413");
    } else {
      check(false, "chunked_413: 连上服务器");
    }
  }
  srv.shutdown();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string filter = (argc > 1) ? argv[1] : std::string();

  struct { const char* name; void (*fn)(); } tests[] = {
    {"claim_matched", test_claim_matched},
    {"claim_missed_falls_through", test_claim_missed_falls_through},
    {"expect_continue", test_expect_continue},
    {"expect_unknown_417", test_expect_unknown_417},
    {"early_413_by_content_length", test_early_413_by_content_length},
    {"chunked_413_at_complete", test_chunked_413_at_complete},
  };

  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    if (!filter.empty() &&
        std::string(tests[i].name).find(filter) == std::string::npos) {
      continue;
    }
    // 显式 flush：这些用例会起真实网络连接，一旦挂住，被缓冲的进度输出
    // 会让人无从判断卡在哪一条。
    std::cout << "[web_stream] " << tests[i].name << std::endl;
    const int before = g_failures;
    tests[i].fn();
    std::cout << "  -> " << (g_failures == before ? "PASS" : "FAIL") << std::endl;
    std::cout.flush();
  }

  std::cout << "[web_stream] " << (g_failures == 0 ? "ALL PASS" : "FAIL")
            << std::endl;
  return g_failures == 0 ? 0 : 2;
}

#else
int main() { return 0; }
#endif
