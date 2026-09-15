/**
 * @file tests/functional/web_app_app_func.cpp
 * @brief `uvcpp_web_app` 的端到端功能测试 —— 真端口、真连接、真客户端。
 *
 * 前面那些 `web_app_*_func.cpp` 各自验证一个零件（路由、请求、响应、中间件、
 * 上下文控制流），这个文件验证它们**装在一起**之后确实能跑：真实的
 * `start_background()`、真实的端口（配 0 让系统分配）、真实的
 * `uvcpp_http_client` 往返。
 *
 * 为什么非要用真端口而不是假的 host
 * --------------------------------
 * 这套东西的风险几乎全在"真连接"上：跨线程投递、请求还在跑的时候连接断了、
 * 优雅关闭的排空窗口、钩子的调用时机。假 host 一个都测不到 —— 所以这里
 * 一条都不省。
 *
 * 覆盖：
 *   1. 生命周期：start / stop / join、端口 0、重复启动被拒、未启动时幂等
 *   2. 路由：静态、参数、通配
 *   3. 404 / 405（带 Allow）/ 自动 OPTIONS
 *   4. 中间件：注册序、短路（不调 next 就不往下走）
 *   5. 异步 handler：**从工作线程**调 next() 恢复（框架的核心用法）
 *   6. 钩子：on_connection / on_connection_close / on_raw_tcp_data / claim
 *   7. 断连：请求发一半就断开，服务不受影响、连接登记表回零
 *   8. 压缩：配置真的传到了 HTTP 层（按 gzip 魔数验证，不看声明）
 *   9. 优雅关闭：请求挂着不响应时 stop() 仍在宽限期内收场
 *  10. 启动失败可恢复：绑定到本机没有的地址 → 失败；改回配置还能起来
 *  11. 闲置超时（slowloris 防御）：半截请求、连上不说话、慢速滴字节都被
 *      关掉；在途请求不被误杀；关掉的连接从登记表里摘干净
 *
 * 刻意**不覆盖**的三件事（都是已知限制，不是漏测）：
 *   - HEAD：`uvcpp_http_client` 不认 HEAD（它按 content-length 等 body，
 *     而 HEAD 的 body 是被丢掉的），走这条路径只会等超时；HEAD 的语义
 *     由 `web_app_router_func.cpp` 的 head_as_get 与响应层各自覆盖。
 *   - 流水线：框架明确不支持，见 `uvcpp_web_app.h` 的已知限制。
 *   - HTTPS/WSS：Phase 4。
 *
 * 崩溃时的定位手段：每个用例**先打名字再跑**（std::unitbuf），所以进程中途
 * 挂掉也能从最后一行看出死在哪一条。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
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

/**
 * 同步往返一次。
 *
 * 沿用 `web_static_server_func.cpp` 的写法：每次新建 client + connect_wait。
 * 外面套一层重试是因为 Windows 上 connect 偶尔会撞上内核的负缓存 —— 那是
 * 系统行为，不是被测代码的问题，不该让用例间歇性变红。
 */
bool roundtrip(int port, const uvcpp_http_request& req,
               uvcpp_http_response& resp) {
  for (int i = 0; i < 40; ++i) {
    uvcpp_http_client client;
    if (client.connect_wait("127.0.0.1", port, 2000) == 0 &&
        client.send_wait(req, resp, 3000) == 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

bool get(int port, const std::string& path, uvcpp_http_response& resp) {
  return roundtrip(port, uvcpp_http_request::make_get(path), resp);
}

bool call(int port, http_method m, const std::string& path,
          uvcpp_http_response& resp) {
  uvcpp_http_request req;
  req.method = m;
  req.url    = path;
  return roundtrip(port, req, resp);
}

std::string body_of(const uvcpp_http_response& r) {
  if (r.body.size() == 0) return std::string();
  return std::string(r.body.get_const_data(), r.body.size());
}

int status_of(const uvcpp_http_response& r) {
  return static_cast<int>(r.status_code);
}

/** 路径参数取不到时返回一个显眼的哨兵，避免"空串也算过"的假绿。 */
std::string param_of(uvcpp_web_request& req, const char* name) {
  const std::string* p = req.param(name);
  return p != nullptr ? *p : std::string("<missing>");
}

/**
 * 裸客户端：连上去、把 bytes **一次**写完、停 settle_ms 毫秒、然后 FIN 关闭。
 *
 * 用来造 `uvcpp_http_client` 表达不出来的场景 —— 它只会发完整请求：
 *   - 请求发一半就断开（没有结尾的 CRLF）；
 *   - 写一段解析器根本不认识的字节。
 */
bool raw_send(int port, const std::string& bytes, int settle_ms) {
  uvcpp_tcp_client client;
  std::atomic<bool> connected(false);
  std::atomic<bool> written(false);
  std::atomic<bool> closed(false);

  int rc = client.connect("127.0.0.1", port, [&](int st) {
    if (st != 0) return;
    connected.store(true);
    if (bytes.empty()) {
      written.store(true);
      return;
    }
    client.write(bytes.data(), bytes.size(),
                 [&](int) { written.store(true); });
  });
  if (rc != 0) return false;

  uvcpp_loop* loop = client.get_loop();
  for (int i = 0; i < 500 && !connected.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!connected.load()) return false;

  for (int i = 0; i < 500 && !written.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 留时间让对端把数据读走并处理完。
  for (int i = 0; i < settle_ms; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 走裸句柄的 close：`client.close()` 在这条路径上没有对应语义，
  // tcp_read_func.cpp 用的也是这一句。
  if (client.get_tcp() != nullptr) {
    client.get_tcp()->close([&](uvcpp_handle*) { closed.store(true); });
  }
  for (int i = 0; i < 300 && !closed.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 30; ++i) loop->run(UV_RUN_NOWAIT);
  return true;
}

/** `raw_hold` 的结果。 */
struct raw_hold_result {
  bool      peer_closed;  ///< 服务端在我们等的这段时间里关了连接（收到 FIN）
  long long elapsed_ms;   ///< 等到关闭用了多久；没等到就是喂进去的 timeout_ms
};

/**
 * 裸客户端「占着不放」：连上去、把 bytes 发出去，然后**自己不关**，一直泵
 * 循环直到服务端关掉它（收到 FIN）或者 wait_ms 用光。
 *
 * 和 `raw_send` 的关键区别就在"自己不关"：`raw_send` 最后主动 FIN，所以
 * "连接被关了"这件事分不清是谁干的。这个函数里客户端从不主动关，**收到
 * FIN 只可能来自服务端** —— 这才能用来验闲置超时。
 *
 * 起读是必须的（不是顺手）：`uvcpp_tcp_client` 只在"有人在读"的时候才会把
 * 对端关闭报到读回调上，不起读的话对端关了我们也发现不了。这也正是
 * `uvcpp_net_read.h` 开头讲的那个坑。
 */
raw_hold_result raw_hold(int port, const std::string& bytes, int wait_ms) {
  raw_hold_result res;
  res.peer_closed = false;
  res.elapsed_ms  = wait_ms;

  uvcpp_tcp_client client;
  std::atomic<bool> connected(false);
  std::atomic<bool> peer_closed(false);

  int rc = client.connect("127.0.0.1", port, [&](int st) {
    if (st != 0) return;
    connected.store(true);
    client.read_start_events([&](uvcpp_tcp_client&, const net_read_result& r) {
      if (r.is_end()) peer_closed.store(true);
    });
    if (!bytes.empty()) {
      client.write(bytes.data(), bytes.size(), [](int) {});
    }
  });
  if (rc != 0) return res;

  uvcpp_loop* loop = client.get_loop();
  for (int i = 0; i < 1000 && !connected.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!connected.load()) {
    if (client.get_tcp() != nullptr) {
      client.get_tcp()->close([](uvcpp_handle*) {});
    }
    return res;
  }

  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  while (true) {
    loop->run(UV_RUN_NOWAIT);
    if (peer_closed.load()) {
      res.peer_closed = true;
      res.elapsed_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
      break;
    }
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count() >= wait_ms) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 收尾：到这儿连接可能已经被服务端关了，也可能还开着。走裸句柄关闭
  // （`raw_send` 用的是同一句）。
  std::atomic<bool> done(false);
  if (client.get_tcp() != nullptr) {
    client.get_tcp()->close([&](uvcpp_handle*) { done.store(true); });
  }
  for (int i = 0; i < 300 && !done.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 30; ++i) loop->run(UV_RUN_NOWAIT);
  return res;
}

/** 轮询等待一个条件成立，最长 timeout_ms 毫秒。 */
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

/** 测试用的 App 骨架：回环 + 端口 0 + 日志压到 WARN（免得刷屏）。 */
void configure_for_test(uvcpp_web_app& app) {
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);
}

// =========================================================================
// 1. 生命周期
// =========================================================================
void test_lifecycle() {
  uvcpp_web_app app;
  configure_for_test(app);
  app.set_server_header("uvcpp-app-test");

  app.get("/hello", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    (void)req;
    (void)next;
    resp.text("hello world");
    resp.end();
  });

  check(!app.running(), "启动前 running() 为假");
  check(app.bound_port() == 0, "启动前 bound_port() 为 0");

  // 没启动时 stop/join 必须是安全空操作（很多人会无脑调）。
  app.stop();
  app.join();
  check(!app.running(), "未启动时 stop()/join() 不炸、状态不变");

  check(app.start_background() == 0, "start_background() 返回 0");
  const int port = app.bound_port();
  check(port > 0, "端口填 0 之后 bound_port() 是真端口");
  check(app.running(), "启动后 running() 为真");
  check(app.loop_started(), "启动后 loop_started() 为真");

  uvcpp_http_response r;
  check(get(port, "/hello", r), "GET /hello 拿到了响应");
  check(status_of(r) == 200, "GET /hello 是 200");
  check(body_of(r) == "hello world", "正文与 handler 写的一致");
  check(http_get_header(r.headers, "server") == "uvcpp-app-test",
        "Server 头来自配置（不是硬编码）");
  check(http_get_header(r.headers, "content-length") == "11",
        "空正文也要有 content-length 的同一套逻辑给出了 11");

  app.stop();
  app.join();
  check(!app.running(), "stop()+join() 之后 running() 为假");
  check(!app.loop_started(), "stop()+join() 之后 loop_started() 为假");

  // 一个实例只允许起一次：复用同一个 http_server 会撞上已关闭的 loop。
  check(app.start_background() != 0, "同一个 App 重复启动被拒");
}

// =========================================================================
// 2. 路由：参数 + 通配
// =========================================================================
void test_params_and_wildcard() {
  uvcpp_web_app app;
  configure_for_test(app);

  app.get("/user/:id", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                          uvcpp_web_next next) {
    (void)next;
    resp.text("id=" + param_of(req, "id"));
    resp.end();
  });
  app.get("/files/*fp", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                           uvcpp_web_next next) {
    (void)next;
    resp.text("fp=" + param_of(req, "fp"));
    resp.end();
  });

  check(app.start_background() == 0, "参数路由服务启动");
  const int port = app.bound_port();

  uvcpp_http_response r;
  check(get(port, "/user/42", r), "GET /user/42 有响应");
  check(status_of(r) == 200 && body_of(r) == "id=42",
        "单段参数被填进了 request::param()");

  // 参数值里带 URL 编码：框架负责解出原文，业务不该自己再去解一遍。
  check(get(port, "/user/a%20b", r), "GET /user/a%20b 有响应");
  check(body_of(r) == "id=a b", "路径参数做了百分号解码");

  check(get(port, "/files/a/b/c.txt", r), "GET /files/a/b/c.txt 有响应");
  check(body_of(r) == "fp=a/b/c.txt", "通配吃下剩余整段（含 /）");

  app.stop();
  app.join();
}

// =========================================================================
// 3. 404 / 405 / 自动 OPTIONS
// =========================================================================
void test_404_405_options() {
  uvcpp_web_app app;
  configure_for_test(app);
  app.get("/only", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                      uvcpp_web_next next) {
    (void)req;
    (void)next;
    resp.text("only-get");
    resp.end();
  });

  check(app.start_background() == 0, "404/405 服务启动");
  const int port = app.bound_port();

  uvcpp_http_response r;

  check(get(port, "/nope", r), "不存在的路径有响应（不是被吞掉）");
  check(status_of(r) == 404, "不存在的路径回 404");

  // 路径存在但方法不对：405 而不是 404 —— 两者对外是不同语义。
  check(call(port, http_method::HTTP_POST, "/only", r), "POST /only 有响应");
  check(status_of(r) == 405, "方法不对回 405（不是 404）");
  check(http_get_header(r.headers, "allow").find("GET") != std::string::npos,
        "405 带 Allow，且里面确实有 GET");

  // 自动 OPTIONS：不用注册，框架按路由表算 Allow。
  check(call(port, http_method::HTTP_OPTIONS, "/only", r),
        "OPTIONS /only 有响应");
  check(status_of(r) == 204, "自动 OPTIONS 回 204");
  check(http_get_header(r.headers, "allow").find("GET") != std::string::npos,
        "自动 OPTIONS 的 Allow 里有 GET");
  check(body_of(r).empty(), "204 没有正文");

  // 但 OPTIONS 一条不存在的路径仍然该是 404 —— 自动应答只对已注册路径生效。
  check(call(port, http_method::HTTP_OPTIONS, "/nope", r),
        "OPTIONS /nope 有响应");
  check(status_of(r) == 404, "未注册路径的 OPTIONS 仍是 404");

  app.stop();
  app.join();
}

// =========================================================================
// 4. 中间件：注册序 + 短路
// =========================================================================
void test_middleware_order() {
  uvcpp_web_app app;
  configure_for_test(app);

  // 用响应头当"顺序账本"：每个中间件往后追加自己。
  //
  // 顺序刻意排成 a → 拦截器 → b：这样"拦截器之后的中间件到底跑没跑"就
  // 变成了一个可断言的事实（账本里有没有 'b'），而不是靠推测。
  app.use([](uvcpp_web_request& req, uvcpp_web_response& resp,
             uvcpp_web_next next) {
    (void)req;
    resp.set_header("x-order", resp.get_header("x-order") + "a");
    next();
  });

  // 短路：不调 next()，后面的中间件与 handler 都不该跑。
  app.use([](uvcpp_web_request& req, uvcpp_web_response& resp,
             uvcpp_web_next next) {
    if (req.path() == "/blocked") {
      resp.status(401);
      resp.text("blocked");
      resp.end();
      return;  // 不放行
    }
    next();
  });

  app.use([](uvcpp_web_request& req, uvcpp_web_response& resp,
             uvcpp_web_next next) {
    (void)req;
    resp.set_header("x-order", resp.get_header("x-order") + "b");
    next();
  });

  app.get("/chain", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    (void)req;
    (void)next;
    resp.text("done");
    resp.end();
  });

  check(app.start_background() == 0, "中间件服务启动");
  const int port = app.bound_port();

  uvcpp_http_response r;
  check(get(port, "/chain", r), "GET /chain 有响应");
  check(status_of(r) == 200, "/chain 是 200");
  check(http_get_header(r.headers, "x-order") == "ab",
        "中间件按注册序执行（先 a 后 b）");

  check(get(port, "/blocked", r), "GET /blocked 有响应");
  check(status_of(r) == 401, "短路中间件拦下了请求");
  check(body_of(r) == "blocked", "短路时用的是中间件自己写的正文");
  // /blocked 没有注册路由。如果链没被拦住，它会继续跑到兜底的 404 并从
  // 框架手里回一个 404 —— 所以 401 本身就是"链确实停在这里"的证据。
  // 账本再补一刀：拦截器**之前**的 a 在（请求进得来），**之后**的 b 不在。
  check(http_get_header(r.headers, "x-order") == "a",
        "短路之后链上后面的中间件一个都没跑");

  app.stop();
  app.join();
}

// =========================================================================
// 5. 异步 handler：从工作线程恢复
// =========================================================================
void test_async_from_worker_thread() {
  uvcpp_web_app app;
  configure_for_test(app);

  // 工作线程用 shared_ptr 持有，测试线程在拿到响应之后 join 掉它 ——
  // 比 detach 干净，也避免"线程还活着但对象已经析构"这种测试自身的竞态。
  std::shared_ptr<std::thread> worker(new std::thread());
  std::atomic<int> ran(0);

  // 「续跑到底跑在哪个线程上」是这个用例真正的判据。
  //
  // 只看"响应回来了没有"是不够的 —— 直接在别的线程上跑 `advance()` 也
  // **经常**能把响应发出去（libuv 的写队列是自旋保护的，恰好能容忍这种
  // 误用），于是坏实现照样能过。唯一稳的判据是比线程身份：loop 线程的身份
  // 从 `on_connection` 那里取（accept 回调必然在 loop 线程上），再和续跑
  // handler 里看到的比。
  std::mutex tid_mu;
  std::thread::id loop_tid;
  std::thread::id handler_tid;
  bool have_loop_tid = false;
  bool have_handler_tid = false;

  app.on_connection([&](uvcpp_web_conn_id id, uvcpp_tcp_client* c) {
    (void)id;
    (void)c;
    std::lock_guard<std::mutex> lk(tid_mu);
    loop_tid      = std::this_thread::get_id();
    have_loop_tid = true;
  });

  app.use([worker, &ran](uvcpp_web_request& req, uvcpp_web_response& resp,
                         uvcpp_web_next next) {
    (void)req;
    (void)resp;
    // 把 next **按值**带进工作线程 —— 框架就是靠"handler 返回时 next 还
    // 被人握着"这一点判断这一环会异步恢复的。
    *worker = std::thread([next, &ran]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      ran.fetch_add(1);
      next();  // ← 在**别的线程**上放行：框架必须自己投回 loop 线程
    });
  });

  app.get("/async", [&](uvcpp_web_request& req, uvcpp_web_response& resp,
                        uvcpp_web_next next) {
    (void)req;
    (void)next;
    {
      std::lock_guard<std::mutex> lk(tid_mu);
      handler_tid      = std::this_thread::get_id();
      have_handler_tid = true;
    }
    resp.text("after-async");
    resp.end();
  });

  check(app.start_background() == 0, "异步服务启动");
  const int port = app.bound_port();

  uvcpp_http_response r;
  check(get(port, "/async", r), "异步 handler 恢复之后响应确实回来了");
  check(status_of(r) == 200, "异步链路仍然是 200");
  check(body_of(r) == "after-async",
        "续跑的 handler 写出的正文正确（没在错误的线程上写响应）");
  check(ran.load() == 1, "工作线程里的续跑只发生了一次");

  std::thread::id seen_loop_tid;
  std::thread::id seen_handler_tid;
  bool ok_loop = false;
  bool ok_handler = false;
  {
    std::lock_guard<std::mutex> lk(tid_mu);
    seen_loop_tid    = loop_tid;
    seen_handler_tid = handler_tid;
    ok_loop          = have_loop_tid;
    ok_handler       = have_handler_tid;
  }
  check(ok_loop, "从 on_connection 拿到了 loop 线程身份");
  check(ok_handler, "续跑的 handler 里记下了线程身份");
  check(ok_loop && ok_handler && seen_loop_tid == seen_handler_tid,
        "续跑**发生在 loop 线程上**（工作线程里的 next() 被投回来了）");

  if (worker->joinable()) worker->join();

  app.stop();
  app.join();
}

// =========================================================================
// 6. 连接钩子
// =========================================================================
void test_connection_hooks() {
  uvcpp_web_app app;
  configure_for_test(app);

  std::atomic<int> conn_calls(0);
  std::atomic<int> close_calls(0);
  std::atomic<bool> raw_seen(false);
  std::atomic<unsigned long long> first_id(0);

  app.on_connection([&](uvcpp_web_conn_id id, uvcpp_tcp_client* c) {
    (void)c;
    first_id.store(static_cast<unsigned long long>(id));
    conn_calls.fetch_add(1);
  });
  app.on_connection_close([&](uvcpp_web_conn_id id, uvcpp_tcp_client* c) {
    (void)id;
    (void)c;
    close_calls.fetch_add(1);
  });
  app.on_raw_tcp_data([&](uvcpp_web_conn_id id, uvcpp_tcp_client* c,
                          const char* data, size_t len) {
    (void)id;
    (void)c;
    if (len >= 4 && std::memcmp(data, "GET ", 4) == 0) raw_seen.store(true);
  });

  app.get("/hello", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    (void)req;
    (void)next;
    resp.text("hi");
    resp.end();
  });

  check(app.start_background() == 0, "钩子服务启动");
  const int port = app.bound_port();

  uvcpp_http_response r;
  check(get(port, "/hello", r), "GET /hello 有响应");
  check(status_of(r) == 200, "钩子挂满时请求照常完成");

  check(conn_calls.load() >= 1, "on_connection 被调用了");
  check(first_id.load() != 0, "连接 id 从 1 开始发（0 是无效 id）");
  check(raw_seen.load(),
        "on_raw_tcp_data 在解析前看到了原始字节（首块以 \"GET \" 开头）");

  // 客户端在上面的 roundtrip 里已经析构 → 对端 FIN → 服务端应当收到闭合通知。
  check(wait_for([&]() { return close_calls.load() >= 1; }, 3000),
        "客户端断开之后 on_connection_close 触发");
  check(wait_for([&]() { return app.connection_count() == 0; }, 3000),
        "断开之后连接登记表回到 0（没有泄漏的登记项）");

  app.stop();
  app.join();
}

// =========================================================================
// 7. 原始数据：观察 + 接管
// =========================================================================
void test_raw_data_claim() {
  uvcpp_web_app app;
  configure_for_test(app);

  std::atomic<int> hits(0);
  std::atomic<int> claim_calls(0);
  std::atomic<int> observers(0);

  app.get("/hit", [&](uvcpp_web_request& req, uvcpp_web_response& resp,
                      uvcpp_web_next next) {
    (void)req;
    (void)next;
    hits.fetch_add(1);
    resp.text("hit");
    resp.end();
  });

  // 观察者：它应当**也**看得到被接管的那一块（文档里的承诺：接管影响的是
  // 解析器，不是观察者）。
  app.on_raw_tcp_data([&](uvcpp_web_conn_id id, uvcpp_tcp_client* c,
                          const char* data, size_t len) {
    (void)id;
    (void)c;
    (void)data;
    (void)len;
    observers.fetch_add(1);
  });

  // 同一份字节、相反的处置 —— 这就是本用例的判据：**输入相同，结果必须
  // 由返回值决定**。如果极性和实现反了，两半必然有一半是错的。
  app.set_raw_data_claim([&](uvcpp_web_conn_id id, uvcpp_tcp_client* c,
                             const char* data,
                             size_t len) -> uvcpp_web_raw_action {
    (void)id;
    (void)c;
    (void)data;
    (void)len;
    return claim_calls.fetch_add(1) == 0
               ? uvcpp_web_raw_action::PASS     // 第一次：放行
               : uvcpp_web_raw_action::CONSUME;  // 第二次：接管
  });

  check(app.start_background() == 0, "接管服务启动");
  const int port = app.bound_port();

  const std::string req = "GET /hit HTTP/1.1\r\nHost: x\r\n\r\n";

  // 第一段：PASS → 请求正常进解析器 → 路由命中。
  check(raw_send(port, req, 150), "裸客户端把第一份请求写完了");
  check(wait_for([&]() { return hits.load() >= 1; }, 3000),
        "claim 返回 PASS 时请求正常进了解析器并命中了路由");
  check(claim_calls.load() >= 1, "claim 被咨询过");

  const int hits_after_first = hits.load();

  // 第二段：**一模一样的字节**，但 claim 说 CONSUME → 解析器不该看见它，
  // 所以路由不会被触发。判据是路由计数不变 —— 不需要自己去读 socket。
  check(raw_send(port, req, 200), "裸客户端把第二份请求写完了");
  check(wait_for([&]() { return claim_calls.load() >= 2; }, 3000),
        "第二份数据也走过了 claim");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  check(hits.load() == hits_after_first,
        "claim 返回 CONSUME 时同一份字节没有进解析器（路由计数没变）");
  check(observers.load() >= 2,
        "观察者看到了被接管的那一块（接管只影响解析器）");

  app.stop();
  app.join();
}

// =========================================================================
// 8. 请求发一半就断开
// =========================================================================
void test_disconnect_mid_request() {
  uvcpp_web_app app;
  configure_for_test(app);
  app.get("/hello", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    (void)req;
    (void)next;
    resp.text("hi");
    resp.end();
  });

  check(app.start_background() == 0, "断连服务启动");
  const int port = app.bound_port();

  // 没有结尾的 CRLF：请求永远不完整。
  check(raw_send(port, "GET /hello HTTP/1.1\r\nHost: x\r\n", 100),
        "半截请求写出去并断开");
  check(wait_for([&]() { return app.connection_count() == 0; }, 3000),
        "半截请求断开后登记表回零（上下文没泄漏）");

  // 关键：服务必须**毫发无伤**。
  uvcpp_http_response r;
  check(get(port, "/hello", r), "断连之后服务照常响应");
  check(status_of(r) == 200 && body_of(r) == "hi",
        "断连之后的响应内容正确");

  app.stop();
  app.join();
}

// =========================================================================
// 9. 压缩：配置真的传到了 HTTP 层
// =========================================================================
void test_compression_wiring() {
  uvcpp_web_app app;
  configure_for_test(app);
  app.set_compression(true).set_compress_min_body_size(64);

  const std::string big(4000, 'a');
  app.get("/big", [big](uvcpp_web_request& req, uvcpp_web_response& resp,
                        uvcpp_web_next next) {
    (void)req;
    (void)next;
    resp.text(big);
    resp.end();
  });

  check(app.start_background() == 0, "压缩服务启动");
  const int port = app.bound_port();

  // (a) 不带 accept-encoding：不该压缩，正文原样到达。
  uvcpp_http_response plain;
  check(get(port, "/big", plain), "不带 accept-encoding 的请求有响应");
  check(body_of(plain).size() == big.size(),
        "没要求压缩时正文长度不变");
  check(http_get_header(plain.headers, "content-encoding").empty(),
        "没要求压缩时没有 content-encoding");

  // (b) 显式要 gzip，且客户端**关掉**自动解压 —— 于是我们看到的
  //     就是线上的真实字节，可以按 gzip 魔数断言，而不是信一个声明。
  uvcpp_http_request req = uvcpp_http_request::make_get("/big");
  req.set_header("accept-encoding", "gzip");
  uvcpp_http_response gz;
  check(roundtrip(port, req, gz), "带 accept-encoding: gzip 的请求有响应");
  check(status_of(gz) == 200, "压缩响应仍是 200");
  check(http_get_header(gz.headers, "content-encoding").find("gzip") !=
            std::string::npos,
        "响应带了 content-encoding: gzip");
  check(gz.body.size() > 2 &&
            static_cast<unsigned char>(gz.body.get_const_data()[0]) == 0x1f &&
            static_cast<unsigned char>(gz.body.get_const_data()[1]) == 0x8b,
        "正文真的是 gzip 流（1f 8b 魔数），不是只贴了个头");
  check(gz.body.size() < big.size(),
        "4000 字节的重复内容压完之后确实变小了");
  check(!http_get_header(gz.headers, "vary").empty(),
        "做过编码决策就带 vary（缓存/CDN 正确性）");

  app.stop();
  app.join();
}

// =========================================================================
// 9b. on_sent 的 body_bytes 必须描述**真正写上线的字节数**
// =========================================================================

/**
 * 把一次 `on_sent` 的结果记下来。
 *
 * 全部走原子量：回调跑在 app 的 loop 线程上，断言跑在测试线程上。
 */
struct sent_probe {
  std::atomic<int>       calls;
  std::atomic<int>       status;
  std::atomic<long long> bytes;
  std::atomic<int>       ok;

  sent_probe() : calls(0), status(0), bytes(-1), ok(-1) {}

  void attach(uvcpp_web_response& r) {
    r.on_sent([this](const uvcpp_web_sent_info& i) {
      calls.fetch_add(1);
      status.store(i.status_code);
      bytes.store(static_cast<long long>(i.body_bytes));
      ok.store(i.ok ? 1 : 0);
    });
  }

  bool fired() const { return calls.load() > 0; }
};

/**
 * HEAD 响应的 `body_bytes` 必须是 0。
 *
 * `uvcpp_web_sent_info::body_bytes` 的文档写的是"**实际写入连接的** body 字节
 * 数（HEAD 时为 0）"，而 `send_response` 原先在构造 `info` **之前没调
 * `sync_meta()`** —— 于是 HEAD 读到的是 GET 本该发的长度，一个从来没上过线的
 * 数字。而 `web_middleware_access_log()` 正是念这个字段，所以每个 HEAD 请求
 * 都会在访问日志里记成一个不存在的字节数。
 *
 * **对照组是必需的一半**：没有同一路由上的 GET，一个"body_bytes 恒为 0"的
 * 实现能让主断言全绿。
 *
 * HEAD 走裸客户端：`uvcpp_http_client` 按 content-length 等 body，而 HEAD 的
 * body 被丢掉了，走它只会等超时（见本文件开头"刻意不覆盖"那一节）。
 */
void test_sent_bytes_head() {
  uvcpp_web_app app;
  configure_for_test(app);

  sent_probe head_probe;
  sent_probe get_probe;
  const std::string payload = "hello world";  // 11 字节

  app.get("/hello", [&](uvcpp_web_request& req, uvcpp_web_response& resp,
                        uvcpp_web_next next) {
    (void)next;
    if (req.method() == http_method::HTTP_HEAD) {
      head_probe.attach(resp);
    } else {
      get_probe.attach(resp);
    }
    resp.text(payload);
    resp.end();
  });

  check(app.start_background() == 0, "HEAD 探测服务启动");
  const int port = app.bound_port();

  // (a) GET：body_bytes 就是正文长度。
  check(raw_send(port, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n", 200),
        "裸客户端把 GET 写完了");
  check(wait_for([&] { return get_probe.fired(); }, 3000), "GET 的 on_sent 被触发");
  check(get_probe.status.load() == 200, "GET 的 status 是 200");
  check(get_probe.ok.load() == 1, "GET 的 ok 为真");
  check(get_probe.bytes.load() == static_cast<long long>(payload.size()),
        "GET 的 body_bytes 等于正文长度");

  // (b) HEAD：头部与 GET 相同，但 body 一个字节都没上线 ⇒ body_bytes == 0。
  check(raw_send(port, "HEAD /hello HTTP/1.1\r\nHost: x\r\n\r\n", 200),
        "裸客户端把 HEAD 写完了");
  check(wait_for([&] { return head_probe.fired(); }, 3000),
        "HEAD 的 on_sent 被触发");
  check(head_probe.status.load() == 200, "HEAD 的 status 是 200");
  check(head_probe.ok.load() == 1, "HEAD 的 ok 为真（响应确实写出去了）");
  check(head_probe.bytes.load() == 0,
        "HEAD 的 body_bytes 是 0 —— 没有字节写上过线");

  app.stop();
  app.join();
}

/**
 * `body_bytes` 必须是**压缩之后**的长度。
 *
 * `apply_compression` 跑在 `http_->send_response` 内部，它把 `resp.body` 换成
 * 压缩后的字节；而 `raw()` 返回的是**引用**。所以采集点必须在那个调用
 * **之后** —— 在之前采集的话，`body_bytes` 记的是压缩前的长度，而报文里的
 * `content-encoding: gzip` 又明说了发的是压缩体，两边自相矛盾。
 *
 * 同样要**对照组**：不带 accept-encoding 的同一路由，`body_bytes` 必须还是原长
 * —— 没有它，"body_bytes 恒等于压缩后长度"（即恒为一个小数）也能蒙混过关。
 */
void test_sent_bytes_compressed() {
  uvcpp_web_app app;
  configure_for_test(app);
  app.set_compression(true).set_compress_min_body_size(64);

  const std::string big(4000, 'a');
  sent_probe plain_probe;
  sent_probe gz_probe;

  app.get("/big", [&](uvcpp_web_request& req, uvcpp_web_response& resp,
                      uvcpp_web_next next) {
    (void)next;
    if (req.header("accept-encoding").find("gzip") != std::string::npos) {
      gz_probe.attach(resp);
    } else {
      plain_probe.attach(resp);
    }
    resp.text(big);
    resp.end();
  });

  check(app.start_background() == 0, "压缩探测服务启动");
  const int port = app.bound_port();

  // (a) 对照组：不要求压缩，body_bytes == 4000。
  check(raw_send(port, "GET /big HTTP/1.1\r\nHost: x\r\n\r\n", 300),
        "裸客户端把不压缩的请求写完了");
  check(wait_for([&] { return plain_probe.fired(); }, 3000),
        "不压缩的 on_sent 被触发");
  check(plain_probe.bytes.load() == static_cast<long long>(big.size()),
        "没要求压缩时 body_bytes 是原长");

  // (b) 要 gzip：body_bytes 必须是**压缩后**的长度，而且确实变小了。
  check(raw_send(port,
                 "GET /big HTTP/1.1\r\nHost: x\r\n"
                 "Accept-Encoding: gzip\r\n\r\n",
                 500),
        "裸客户端把压缩的请求写完了");
  check(wait_for([&] { return gz_probe.fired(); }, 3000),
        "压缩的 on_sent 被触发");
  check(gz_probe.status.load() == 200, "压缩响应仍是 200");
  check(gz_probe.ok.load() == 1, "压缩响应的 ok 为真");
  check(gz_probe.bytes.load() > 0 &&
            gz_probe.bytes.load() < static_cast<long long>(big.size()),
        "压缩响应的 body_bytes 是**压缩后**的长度（严格小于 4000）");

  app.stop();
  app.join();
}

// =========================================================================
// 10. 优雅关闭：请求挂着不响应
// =========================================================================
void test_graceful_shutdown_with_inflight() {
  uvcpp_web_app app;
  configure_for_test(app);
  app.set_shutdown_grace_ms(300);

  // 把 next 扣押下来：链永远恢复不了，这个请求也就永远不会有响应 ——
  // 正是优雅关闭最难处理的那种情况。
  std::shared_ptr<uvcpp_web_next> trapped(new uvcpp_web_next());
  std::atomic<bool> arrived(false);

  app.get("/hang", [trapped, &arrived](uvcpp_web_request& req,
                                       uvcpp_web_response& resp,
                                       uvcpp_web_next next) {
    (void)req;
    (void)resp;
    *trapped = next;  // 留着，永不调用
    arrived.store(true);
  });

  check(app.start_background() == 0, "停机服务启动");
  const int port = app.bound_port();

  // 请求必须从**另一个线程**发：它永远等不到响应，会一直阻塞到超时。
  std::thread requester([port]() {
    uvcpp_http_response r;
    roundtrip(port, uvcpp_http_request::make_get("/hang"), r);
  });

  check(wait_for([&]() { return arrived.load(); }, 3000),
        "挂起的请求到达了 handler");
  check(app.inflight_count() == 1, "在途计数记下了这个请求");

  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  app.stop();
  app.join();
  const long long elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0).count();

  check(elapsed_ms < 3000,
        "有请求挂着时 stop() 仍在宽限期附近收场（实测 " +
            std::to_string(elapsed_ms) + " ms）");
  check(!app.running(), "停机之后 running() 为假");

  requester.join();
  *trapped = uvcpp_web_next();  // 松开扣押的 next
}

// =========================================================================
// 11. 启动失败可恢复
// =========================================================================
void test_start_failure_is_recoverable() {
  uvcpp_web_app app;
  configure_for_test(app);

  // 203.0.113.0/24 是 TEST-NET-3，按 RFC 5737 专供文档使用 —— 本机一定
  // 没有这个地址，bind 必然失败。用它而不是"占一个端口"是为了不依赖
  // SO_REUSEADDR 在不同平台上的差异。
  app.set_host("203.0.113.1");
  const int rc = app.start_background();
  check(rc != 0, "绑定到本机不存在的地址必须失败");
  check(rc != UV_EBUSY, "失败原因是 bind 而不是重入（错误码 " +
                            std::to_string(rc) + "）");
  check(!app.running(), "启动失败后 running() 为假");
  check(!app.loop_started(), "启动失败后 loop_started() 为假");
  check(app.bound_port() == 0, "启动失败后 bound_port() 仍是 0");

  // 失败路径必须把这个对象还回"没启动过"的状态，否则改完配置就再也起不来。
  app.set_host("127.0.0.1");
  app.get("/x", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                   uvcpp_web_next next) {
    (void)req;
    (void)next;
    resp.text("recovered");
    resp.end();
  });

  const int rc2 = app.start_background();
  check(rc2 == 0, "失败之后改配置仍能启动（错误码 " + std::to_string(rc2) + "）");
  if (rc2 == 0) {
    const int port = app.bound_port();
    check(port > 0, "恢复后端口有效");

    uvcpp_http_response r;
    check(get(port, "/x", r), "恢复之后服务真的可用");
    check(body_of(r) == "recovered", "恢复之后的响应内容正确");

    app.stop();
    app.join();
  }
}

// =========================================================================
// 12. 闲置超时（slowloris 防御）
// =========================================================================
void test_idle_timeout() {
  uvcpp_web_app app;
  configure_for_test(app);
  // 600 毫秒：短到用例跑得快，又长到不会被"连接建立/首字节"的正常延迟误伤。
  app.set_idle_timeout_ms(600);

  app.get("/hello", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    (void)req;
    (void)next;
    resp.text("hi");
    resp.end();
  });

  // 把 next 扣下来永不调用 —— 用来验证"服务端自己在忙"时连接**不会**被
  // 超时杀掉。和优雅关闭那条用例是同一个手法。
  std::shared_ptr<uvcpp_web_next> trapped(new uvcpp_web_next());
  std::atomic<bool> hang_arrived(false);
  app.get("/hang", [trapped, &hang_arrived](uvcpp_web_request& req,
                                            uvcpp_web_response& resp,
                                            uvcpp_web_next next) {
    (void)req;
    (void)resp;
    *trapped = next;
    hang_arrived.store(true);
  });

  check(app.start_background() == 0, "闲置超时服务启动");
  const int port = app.bound_port();

  // --- (a) 先说清楚这条服务本身是好的 ---
  //
  // 没有这一条，下面三个"连接被关了"的断言全都可以被一个"服务压根不工作"
  // 的坏版本满足。
  uvcpp_http_response r;
  check(get(port, "/hello", r), "正常请求照常拿到响应");
  check(status_of(r) == 200 && body_of(r) == "hi", "正常请求内容正确");

  // --- (b) 慢速攻击的经典形态：半行请求头之后再也不发字节 ---
  //
  // 判据是**服务端真的关了连接**（客户端等到 FIN），而不是"我们等够了时间"。
  const raw_hold_result slow = raw_hold(
      port, "GET /hello HTTP/1.1\r\nHost: x\r\n", 6000);
  check(slow.peer_closed, "半截请求的连接被服务端关掉了");
  check(slow.elapsed_ms >= 300 && slow.elapsed_ms <= 4000,
        "关闭发生在超时附近（实测 " + std::to_string(slow.elapsed_ms) +
            " ms，上限 600 ms —— 太早说明杀错了，太晚说明没在扫）");

  // --- (c) 连上就一句话不说 ---
  //
  // 挡的是另一种耗尽：不设超时的话，光"连上不动"就能把连接槽占满。
  const raw_hold_result silent = raw_hold(port, "", 6000);
  check(silent.peer_closed, "连上不说话的连接也被关掉了");

  // --- (d) 慢速滴字节刷新不了计时 ---
  //
  // 这条是"为什么不能只用 last_read"的现场证据：每 200 毫秒发一个字节，
  // **空闲永远是新鲜的**（最后一个字节才过去不到 200 ms），但整个请求已经
  // 拖过了 600 ms 的预算 —— 只有按"整个请求的预算"算才会在滴的过程中被关。
  //
  // 判据必须是**滴到第几个字节时被关**，不能是"最后关没关"：
  // 一条滴完了才被闲置超时关掉的连接，和一条在滴的过程中就被按请求预算
  // 关掉的连接，收盘状态是一样的（都关了）。变异测试当场抓到过这个假绿 ——
  // 把活动时刻换回 `last_read_ms`，用例照样全绿。
  //
  // 客户端在这里得分多次写、中间还夹着等待，所以不能用 raw_hold（它一次
  // 写完就不动了）。用 rawsend 那套手动泵循环。
  {
    uvcpp_tcp_client drip;
    std::atomic<bool> connected(false);
    std::atomic<bool> closed(false);

    int rc = drip.connect("127.0.0.1", port, [&](int st) {
      if (st == 0) connected.store(true);
    });
    check(rc == 0, "慢速滴字节的客户端连上了");
    uvcpp_loop* loop = drip.get_loop();
    for (int i = 0; i < 1000 && !connected.load(); ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    drip.read_start_events([&](uvcpp_tcp_client&, const net_read_result& ev) {
      if (ev.is_end()) closed.store(true);
    });

    const char* chunks[] = {"G", "E", "T", " ", "/", "h", "e", "l", "l", "o"};
    const int   kChunks = 10;
    int  written   = 0;
    int  write_err = 0;
    for (int i = 0; i < kChunks; ++i) {
      // 服务端已经关了 —— 这正是本用例要的结果，停下来数清楚滴了几个。
      if (closed.load()) break;

      // **每一字节都必须确认出去了**，不能发完就走。
      //
      // `uvcpp_tcp_client` 同一时刻只允许一个异步写，再发一个会返回
      // `UV_EALREADY` 并**把这一字节丢掉** —— 于是"每 200 毫秒滴一个字节"
      // 实际变成了"第一字节之后再也没人说话"，连接自然会被超时关掉。那样
      // 这条用例即使在被测保证被删掉之后也照样"通过"（变异测试当场抓到过
      // 这个假绿）。所以：等写回调，并且把返回码当回事。
      std::atomic<bool> wrote(false);
      const int wrc = drip.write(chunks[i], 1, [&](int) { wrote.store(true); });
      if (wrc != 0) {
        write_err = wrc;
        break;
      }

      const std::chrono::steady_clock::time_point t =
          std::chrono::steady_clock::now();
      while (std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - t)
                 .count() < 200) {
        loop->run(UV_RUN_NOWAIT);
        if (closed.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!wrote.load() && !closed.load()) {
        write_err = -1;  // 写回调一直没来（既没成功也没报错）
        break;
      }
      ++written;
    }

    check(write_err == 0, "慢速滴字节的每一次写都真的出去了（返回码 " +
                              std::to_string(write_err) + "）");
    // **非空真性检查**：至少滴出去两个字节，才谈得上"计时被刷新过"。
    // 否则下面那条断言只是在验"一个从头到尾没说过话的连接被超时关掉"，
    // 而那件事 (c) 已经验过了 —— 这条用例就成了重复劳动。
    check(written >= 2,
          "慢速攻击至少滴出了 2 个字节（实际 " + std::to_string(written) +
              " 个），后面那条断言才不是空跑");
    // **就是这一条在分辨两种计时法。**
    //
    // 十个字节 × 200 毫秒 = 2 秒，预算只有 600 毫秒，所以按请求预算算的话，
    // 连接会在第 3~4 个字节附近就被关掉，`written` 停在 3 或 4。改成按
    // "最后一次收字节"算的话，每一滴都会把计时刷新，连接**滴完十个字节都
    // 不会被关**（之后闲下来才关），`written` 就是 10。
    check(written < kChunks,
          "连接是在**滴的过程中**被关的：只滴出去 " + std::to_string(written) +
              "/" + std::to_string(kChunks) +
              " 个字节。滴满 10 个说明计时被最后一个字节刷新了 —— 那正是"
              "慢速攻击想要的");
    // 滴完再等一拍：确认它最后确实是被（闲置超时）关的，而不是我们放弃滴了。
    for (int i = 0; i < 800 && !closed.load(); ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(closed.load(), "这条连接最终确实被服务端关掉了");

    std::atomic<bool> drip_done(false);
    if (drip.get_tcp() != nullptr) {
      drip.get_tcp()->close([&](uvcpp_handle*) { drip_done.store(true); });
    }
    for (int i = 0; i < 300 && !drip_done.load(); ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // --- (e) 服务端自己在忙 ≠ 客户端在攻击 ---
  //
  // 请求已经完整送达、处理器还挂着没应答。这时连接是**安静**的，但安静的
  // 原因是服务端，不是客户端。杀掉它只会让一个正确发起的请求失败 —— 比慢速
  // 攻击更难查，因为它只在服务慢的时候出现。
  {
    uvcpp_tcp_client busy;
    std::atomic<bool> busy_closed(false);
    std::atomic<bool> busy_connected(false);
    int rc = busy.connect("127.0.0.1", port, [&](int st) {
      if (st == 0) busy_connected.store(true);
    });
    check(rc == 0, "在途请求的客户端连上了");
    uvcpp_loop* loop = busy.get_loop();
    for (int i = 0; i < 1000 && !busy_connected.load(); ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    busy.read_start_events([&](uvcpp_tcp_client&, const net_read_result& ev) {
      if (ev.is_end()) busy_closed.store(true);
    });

    const std::string full = "GET /hang HTTP/1.1\r\nHost: x\r\n\r\n";
    busy.write(full.data(), full.size(), [](int) {});

    check(wait_for([&]() { return hang_arrived.load(); }, 3000),
          "挂起的请求到达了 handler");

    // 整整 1.8 秒（三倍于 600 ms 的超时）都不该被关。
    const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
               .count() < 1800) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(!busy_closed.load(),
          "有请求在途时连接不会被闲置超时杀掉（服务端自己慢，怪不到客户端）");

    std::atomic<bool> busy_done(false);
    if (busy.get_tcp() != nullptr) {
      busy.get_tcp()->close([&](uvcpp_handle*) { busy_done.store(true); });
    }
    for (int i = 0; i < 300 && !busy_done.load(); ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    *trapped = uvcpp_web_next();  // 松开扣押的 next
  }

  // --- (f) 关掉的连接要从登记表里摘干净 ---
  //
  // 超时关连接如果只关 socket 不摘表，那"防御"本身就成了泄漏 —— 表会一直
  // 涨，接着 (b)(c)(d) 挡住的资源耗尽就从另一条路回来了。
  check(wait_for([&]() { return app.connection_count() == 0; }, 3000),
        "被超时关掉的连接都从登记表里摘掉了（没有泄漏）");

  // --- (g) 超时之后服务照常工作 ---
  uvcpp_http_response r2;
  check(get(port, "/hello", r2), "超时关了一批连接之后服务照常响应");
  check(status_of(r2) == 200, "后续请求仍是 200");

  app.stop();
  app.join();
}

// =========================================================================
// 用例表
// =========================================================================
struct test_case {
  const char* name;
  void (*fn)();
};

}  // namespace

int main(int argc, char** argv) {
  // 崩了也不要丢缓冲输出 —— 每行都实时落盘，才能从最后一行看出死在哪。
  std::cout << std::unitbuf;

  // `--only <名字>` 只跑一条用例。做**变异验证**（把某条保证改坏，确认对应用
  // 例真的会红）时要一条一条跑 —— 否则一条用例把进程带崩，后面那些根本
  // 没机会执行，也就分不清"没红"和"没跑"。
  std::string only;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
      only = argv[++i];
    }
  }

  const test_case tests[] = {
      {"lifecycle", test_lifecycle},
      {"params_wildcard", test_params_and_wildcard},
      {"404_405_options", test_404_405_options},
      {"middleware_order", test_middleware_order},
      {"async_from_worker", test_async_from_worker_thread},
      {"connection_hooks", test_connection_hooks},
      {"raw_data_claim", test_raw_data_claim},
      {"disconnect_mid_request", test_disconnect_mid_request},
      {"compression_wiring", test_compression_wiring},
      {"sent_bytes_head", test_sent_bytes_head},
      {"sent_bytes_compressed", test_sent_bytes_compressed},
      {"graceful_shutdown", test_graceful_shutdown_with_inflight},
      {"start_failure_recoverable", test_start_failure_is_recoverable},
      {"idle_timeout", test_idle_timeout},
  };
  const int count = static_cast<int>(sizeof(tests) / sizeof(tests[0]));

  int ran = 0;
  for (int i = 0; i < count; ++i) {
    if (!only.empty() && only != tests[i].name) continue;
    ++ran;
    std::cout << "[" << tests[i].name << "] " << std::flush;
    const int before = g_failures;
    tests[i].fn();
    std::cout << (g_failures == before ? "PASS" : "FAIL") << std::endl;
  }

  if (ran == 0) {
    std::cerr << "[web_app_app] --only " << only << " 没匹配到任何用例"
              << std::endl;
    return 2;
  }

  if (g_failures == 0) {
    std::cout << "[web_app_app] ALL PASS (" << ran << " cases)" << std::endl;
    return 0;
  }
  std::cout << "[web_app_app] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}

#else

#include <iostream>

int main() {
  // 这个文件只在 UVCPP_BUILD_WEBAPP=ON 时才进构建；真跑到这儿说明构建
  // 配置和测试筛选对不上（例如加了文件却忘了重跑 cmake）。宁可红，也不要
  // 静默地"零个用例全绿"。
  std::cerr << "[web_app_app] UVCPP_WEBAPP_ENABLE=0 —— 构建配置有问题，"
               "这个测试文件不该被编译进来" << std::endl;
  return 2;
}

#endif  // UVCPP_WEBAPP_ENABLE
