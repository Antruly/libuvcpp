/**
 * @file tests/functional/web_http_client_selfdestroy_func.cpp
 * @brief 在 `uvcpp_http_client` **自己的回调里**把它 `delete` 掉，不许崩。
 *
 * 这是本类最自然的用法（"请求完就把客户端扔了"），而它此前是**必然崩**的：
 * 析构里那段关闭 dance 会 `loop_->run(UV_RUN_NOWAIT)` 泵若干轮，而那几轮正好
 * 把栈上还没返回的那条读路径**再叫一遍**。实测（cdb，2026-09-19）：
 *
 *   llhttp
 *   uvcpp!uvcpp_http_parser::execute
 *   uvcpp!uvcpp_http_client::on_tcp_data
 *   uvcpp!uvcpp_http_client::~uvcpp_http_client+0x45e   ← 析构自己泵出来的
 *   uvcpp!uvcpp_stream::callback_read
 *   uv!uv_run → uvcpp!uvcpp_loop::run → main
 *
 * 修法见 `~uvcpp_http_client`：与 `~uvcpp_tcp_client` / `~uvcpp_ws_client` 同一条
 * 策略 —— 在回调里被析构就**一个都不拆**（有意的泄漏），先把读停掉，其余交给
 * 循环；外加一枚 `alive_token_`，让所有指回本对象的闭包自己知道别碰。
 *
 * 判据是**进程活不活**加回调里的取值：修之前这三条全部崩在 `delete` 那一句
 * 里面（`0xC0000005`），所以任何"半截实现"都过不去。
 */
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_client.h>

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

/// 服务端固定回这一个响应，`Connection` 头不写 `close` ⇒ 连接留着。
/// `Content-Length` 与正文必须一致：客户端按它判断响应结束。
const char kResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "pong";

// =========================================================================
// 服务端 —— 每个完整请求回一次，**不关连接**（keep-alive 上才能发第二次）
// =========================================================================

struct server_state {
  std::atomic<int> listen_rc{-1};
  std::atomic<int> accepted{0};
  std::atomic<int> requests{0};
  std::atomic<int> replies{0};
};

void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                server_state& st) {
  uvcpp_tcp_server server;
  std::string      pending;

  server.set_read_callback([&st, &pending](uvcpp_tcp_client& c,
                                           const net_read_result& r) {
    if (!r.is_data()) return;
    pending.append(r.data, r.size);
    if (pending.find("\r\n\r\n") == std::string::npos) return;
    pending.clear();
    st.requests.fetch_add(1);
    c.write(kResponse, sizeof(kResponse) - 1,
            [&st](int status) {
              if (status == 0) st.replies.fetch_add(1);
            });
  });

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }
  sockaddr_in name;
  int         namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int bound_port = ntohs(name.sin_port);

  rc = server.listen([&st](uvcpp_tcp_client*) { st.accepted.fetch_add(1); }, 128);
  st.listen_rc.store(rc);
  // 端口必须在 `listen()` **之后**才放行：已 bind、尚未 listen 的 socket 是
  // 拒连的，客户端会拿到 ECONNREFUSED 而不是"服务端起来了"。
  port_promise.set_value(rc != 0 ? -1 : bound_port);
  if (rc != 0) return;

  uvcpp_loop* loop = server.get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  uvcpp_test::pump_for(loop, 400);
}

struct test_server {
  server_state      st;
  std::atomic<bool> stop{false};
  std::promise<int> port_promise;
  std::future<int>  port_future;
  std::thread       thread;
  int               port = -1;

  test_server() : port_future(port_promise.get_future()) {
    thread = std::thread(run_server, std::ref(port_promise), std::ref(stop),
                         std::ref(st));
    port = port_future.get();
  }

  void shutdown() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }

  ~test_server() { shutdown(); }

  bool ok() const { return port > 0 && st.listen_rc.load() == 0; }
};

// =========================================================================
// 客户端
// =========================================================================

/// 服务端侧的量全都在**另一个线程**上，所以一律"等到"，不许"采一下"。
///
/// `accepted` 长在 accept 回调里、`replies` 长在**写完成**回调里，两个都是异步的：
/// 客户端这边 connect 回调一返回就 `load()`，服务端线程完全可能还没跑到那一步。
/// 实测（2026-09-19，同一份代码重复 30 次）这条竞态让用例 **6/30 变红**，
/// 而红的那一条是"前置"断言 —— 也就是它与被测的那件事无关。`wait_flag` 不泵循环，
/// 正合适：这里等的是对端线程，不是本端的事件。
template <typename Pred>
bool wait_srv(Pred pred) {
  return uvcpp_test::wait_flag(pred, uvcpp_test::kWaitMs);
}

uvcpp_loop* loop_of(uvcpp_http_client* c) {
  return c->get_tcp_client()->get_loop();
}

/// 一条连接一个请求的观测值。删除发生在回调**里面**，所以结果只能落在这些
/// 独立于客户端的原子上。
struct resp_probe {
  std::atomic<bool> fired{false};
  std::atomic<int>  count{0};
  std::atomic<int>  err{-99};
  std::atomic<int>  code{0};
  std::atomic<bool> deleted{false};
};

// =========================================================================
// S1 —— 响应回调里 `delete`（本轮复现的那条）
// =========================================================================

void scenario_delete_in_response_cb() {
  std::cout << "[scenario 1] 响应回调里 delete 客户端" << std::endl;

  test_server srv;
  check(srv.ok(), "S1: 服务端 listen 成功");
  if (!srv.ok()) return;

  uvcpp_http_client* pc = new uvcpp_http_client();
  uvcpp_loop*        loop = loop_of(pc);
  resp_probe         p;

  std::atomic<bool> connected{false};
  const int crc = pc->connect("127.0.0.1", srv.port,
                              [&connected](int err) {
                                if (err == 0) connected.store(true);
                              });
  check(crc == 0, "S1: connect() 受理了这次连接");
  check(uvcpp_test::wait_until(loop, [&] { return connected.load(); },
                              uvcpp_test::kWaitMs),
        "S1: connect 在墙钟上限内成功（前置）");
  if (!connected.load()) {
    delete pc;
    return;
  }

  const int src = pc->send(
      uvcpp_http_request::make_get("/pong"),
      [pc, &p](const uvcpp_http_response& r, int err) {
        p.err.store(err);
        p.code.store(static_cast<int>(r.status_code));
        p.count.fetch_add(1);
        // **这一句就是被测的那件事。** 修之前它返回不回来：析构里泵的每一轮
        // 都会把栈上这条读路径再叫一遍，撞在已经释放的对象上。
        delete pc;
        p.deleted.store(true);
        p.fired.store(true);
      });
  check(src == 0, "S1: send() 受理了这次请求（前置）");

  check(uvcpp_test::wait_until(loop, [&] { return p.fired.load(); },
                               uvcpp_test::kWaitMs),
        "S1: 响应回调在墙钟上限内落地");
  check(p.deleted.load(), "S1: 回调确实执行到了 delete 之后的那一句（前置）");
  check(p.count.load() == 1, "S1: 回调恰好一次");
  check(p.err.load() == 0, "S1: 回调不带错误");
  check(p.code.load() == 200, "S1: 状态码 200");
  check(wait_srv([&] { return srv.st.requests.load() >= 1; }),
        "S1: 服务端确实收到了请求（前置）");
  check(wait_srv([&] { return srv.st.replies.load() >= 1; }),
        "S1: 服务端确实把响应写出去了（前置）");

  // 再从已经删掉的循环上泵几轮：上面那次 `delete` 的**栈还没退干净**是发生在
  // `loop->run()` 里面的，这里多跑几轮把"退干净之后还回来"这一段也走一遍。
  uvcpp_test::pump_for(loop, 50);

  srv.shutdown();
}

// =========================================================================
// S2 —— connect 回调里 `delete`
// =========================================================================

void scenario_delete_in_connect_cb() {
  std::cout << "[scenario 2] connect 回调里 delete 客户端" << std::endl;

  test_server srv;
  check(srv.ok(), "S2: 服务端 listen 成功");
  if (!srv.ok()) return;

  uvcpp_http_client* pc = new uvcpp_http_client();
  uvcpp_loop*        loop = loop_of(pc);
  std::atomic<bool>  fired{false};
  std::atomic<int>   cb_err{-99};

  // connect 的回调里 `this` 之后没有别的动作，所以崩点只可能在析构那一段。
  const int crc = pc->connect("127.0.0.1", srv.port, [pc, &fired, &cb_err](int err) {
    cb_err.store(err);
    delete pc;
    fired.store(true);
  });
  check(crc == 0, "S2: connect() 受理了这次连接");

  check(uvcpp_test::wait_until(loop, [&] { return fired.load(); },
                               uvcpp_test::kWaitMs),
        "S2: connect 回调在墙钟上限内落地");
  check(cb_err.load() == 0, "S2: connect 成功");
  check(wait_srv([&] { return srv.st.accepted.load() == 1; }),
        "S2: 服务端接受了 1 条连接（前置）");

  uvcpp_test::pump_for(loop, 50);
  srv.shutdown();
}

// =========================================================================
// S3 —— keep-alive 上的**第二次**请求，在它的回调里 `delete`
// =========================================================================

void scenario_delete_on_second_request() {
  std::cout << "[scenario 3] 复用连接发第二次，在那次回调里 delete" << std::endl;

  test_server srv;
  check(srv.ok(), "S3: 服务端 listen 成功");
  if (!srv.ok()) return;

  uvcpp_http_client* pc = new uvcpp_http_client();
  uvcpp_loop*        loop = loop_of(pc);
  resp_probe         p;

  std::atomic<bool> connected{false};
  pc->connect("127.0.0.1", srv.port,
              [&connected](int err) {
                if (err == 0) connected.store(true);
              });
  check(uvcpp_test::wait_until(loop, [&] { return connected.load(); },
                               uvcpp_test::kWaitMs),
        "S3: connect 在墙钟上限内成功（前置）");
  if (!connected.load()) {
    delete pc;
    return;
  }

  // 第一次：普通回调，只用来把连接热身成"已经收过一条响应"。
  std::atomic<bool> first_done{false};
  pc->send(uvcpp_http_request::make_get("/one"),
           [&first_done](const uvcpp_http_response&, int err) {
             if (err == 0) first_done.store(true);
           });
  check(uvcpp_test::wait_until(loop, [&] { return first_done.load(); },
                               uvcpp_test::kWaitMs),
        "S3: 第一次响应在墙钟上限内落地（前置）");
  check(wait_srv([&] { return srv.st.replies.load() == 1; }),
        "S3: 服务端确实回了第一次（前置）");
  if (!first_done.load()) {
    delete pc;
    return;
  }

  // 第二次：这次在回调里删。读闭包会在这一步**重新装**（第一次响应之后
  // `RECEIVING` 已经清掉），所以这条覆盖的是"重装过的那一份闭包"。
  const int src = pc->send(
      uvcpp_http_request::make_post("/two", "x", 1, "text/plain"),
      [pc, &p](const uvcpp_http_response& r, int err) {
        p.err.store(err);
        p.code.store(static_cast<int>(r.status_code));
        p.count.fetch_add(1);
        delete pc;
        p.deleted.store(true);
        p.fired.store(true);
      });
  check(src == 0, "S3: 第二次 send() 受理了（前置）");

  check(uvcpp_test::wait_until(loop, [&] { return p.fired.load(); },
                               uvcpp_test::kWaitMs),
        "S3: 第二次响应回调在墙钟上限内落地");
  check(p.deleted.load(), "S3: 回调确实执行到了 delete 之后的那一句（前置）");
  check(p.count.load() == 1, "S3: 第二次回调恰好一次");
  check(p.err.load() == 0, "S3: 第二次回调不带错误");
  check(p.code.load() == 200, "S3: 第二次状态码 200");
  check(wait_srv([&] { return srv.st.replies.load() == 2; }),
        "S3: 服务端确实把两次响应都写出去了");

  uvcpp_test::pump_for(loop, 50);
  srv.shutdown();
}

}  // namespace

int main() {
  std::cout << "=== web_http_client_selfdestroy_func ===" << std::endl;

  scenario_delete_in_response_cb();
  scenario_delete_in_connect_cb();
  scenario_delete_on_second_request();

  if (g_failures == 0) {
    std::cout << "ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}

#else  // UVCPP_WEB_ENABLE

int main() {
  std::cout << "[SKIP] web_http_client_selfdestroy_func: UVCPP_WEB_ENABLE off"
            << std::endl;
  return 0;
}

#endif
