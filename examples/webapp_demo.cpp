/**
 * @file examples/webapp_demo.cpp
 * @brief webapp 应用框架的可运行示例 —— 默认自校验，`--serve` 才是常驻服务。
 *
 * @code
 *   webapp_demo                    自校验：起一个 App（端口 0，由内核挑），
 *                                  用框架自带的 HTTP / WS 客户端连它自己，
 *                                  跑完一组断言后退出。全通过 exit 0。
 *   webapp_demo --serve [port]     常驻服务（默认 8080），Ctrl-C 退出。
 *   webapp_demo --static <dir>     常驻模式的静态根（默认 ./public）。
 * @endcode
 *
 * 为什么默认是自校验而不是"起个服务等人连"：README 和指南里贴的用法要是哪天
 * 跟代码对不上了，跑一遍这个文件就知道 —— 它同时是文档和冒烟测试。
 *
 * 覆盖的东西：路由（静态 / 参数）、中间件、静态文件服务、异步 handler（工作
 * 线程 + `next()`）、WebSocket 回显、以及框架带的两个客户端。
 *
 * 完整说明见 doc/webapp-guide.md。
 */
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <direct.h>  // _mkdir / _rmdir
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <uvcpp/uvcpp_define.h>

#if !UVCPP_WEBAPP_ENABLE
#error "webapp_demo 需要 UVCPP_BUILD_WEBAPP=ON（见 README 的构建章节）"
#endif

#include <req/uvcpp_work.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_ws_client.h>

using namespace uvcpp;

/** @brief Ctrl-C 用的。信号处理器要 C 链接，所以放在匿名命名空间外面。 */
volatile std::sig_atomic_t g_stop = 0;

extern "C" void on_sigint(int) { g_stop = 1; }

namespace {

int g_failed = 0;

void check(bool ok, const std::string& what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_failed;
}

std::string body_of(const uvcpp_http_response& r) {
  if (r.body.size() == 0) return std::string();
  return std::string(r.body.get_const_data(), r.body.size());
}

// =========================================================================
// 自校验用的临时静态根
//
// 框架对**不存在的**文档根不是回 404 而是回 500（日志里会写明），所以自校验
// 必须自己造一个真目录出来 —— 顺带也让"越过根的文件送不出去"这条断言不是空跑。
// =========================================================================

const char* const kDemoStaticRoot = "webapp_demo_public";

bool make_dir(const std::string& path) {
#if defined(_WIN32)
  return ::_mkdir(path.c_str()) == 0;
#else
  return ::mkdir(path.c_str(), 0755) == 0;
#endif
}

void remove_dir(const std::string& path) {
#if defined(_WIN32)
  ::_rmdir(path.c_str());
#else
  ::rmdir(path.c_str());
#endif
}

bool write_file(const std::string& path, const std::string& bytes) {
  std::ofstream f(path.c_str(), std::ios::binary);
  if (!f) return false;
  f << bytes;
  return f.good();
}

bool provision_static_root(std::string* err) {
  const std::string root(kDemoStaticRoot);
  // 目录已存在时 mkdir 会失败，这不算错 —— 后面写文件失败才是真的不行。
  make_dir(root);
  if (!write_file(root + "/index.html", "<h1>webapp_demo</h1>\n")) {
    *err = "写不了 index.html";
    return false;
  }
  // 点文件：默认策略是 HIDE（404），自校验拿它验"根里的隐藏文件不会被送出"。
  if (!write_file(root + "/.secret", "top-secret\n")) {
    *err = "写不了 .secret";
    return false;
  }
  return true;
}

void remove_static_root() {
  const std::string root(kDemoStaticRoot);
  std::remove((root + "/index.html").c_str());
  std::remove((root + "/.secret").c_str());
  remove_dir(root);
}

// =========================================================================
// 应用本体 —— `--serve` 和自校验用的是同一个
// =========================================================================

/** @brief 中间件就是"会调 `next()` 的 handler"，类型完全相同。 */
uvcpp_web_middleware demo_tag_middleware() {
  return [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next next) {
    resp.set_header("X-Demo", "webapp-demo");
    next();  // 不调 next() 就是这一环直接结束，链不再往下走
  };
}

void build_app(uvcpp_web_app& app, int port, const std::string& static_dir) {
  app.set_host("127.0.0.1")
      .set_port(port)  // 0 = 由内核挑端口，示例才不会和别的进程抢
      .use(web_middleware_access_log())
      .use(demo_tag_middleware());

  // 静态文件服务。`/assets` 前缀自己也要有路由，否则 `/assets` 是 404 ——
  // 通配 `*path` 至少要吃一段（这条由框架注册，不需要你写）。
  app.serve_static("/assets", static_dir);

  app.get("/hello", [](uvcpp_web_request&, uvcpp_web_response& resp,
                       uvcpp_web_next) {
    resp.json_str("{\"hello\":\"world\"}");
    resp.end();  // 注意是"我写完了"的标记，不是发送；框架收尾时才真发出去
  });

  app.get("/user/:id", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                          uvcpp_web_next) {
    const std::string* id = req.param("id");
    resp.text(id != nullptr ? *id : std::string());
    resp.end();
  });

  // 异步 handler：重活丢给工作线程池，完成回调（跑在 loop 线程上）里接着写。
  // `next` 按值捕获 —— 框架看到它被留下来了，就知道这一环会异步恢复。
  app.get("/slow", [&app](uvcpp_web_request&, uvcpp_web_response& resp,
                          uvcpp_web_next next) {
    uvcpp_web_response* rp = &resp;  // 响应对象的生存期等于上下文的，而 next
                                     // 里钉着上下文，所以异步用它是安全的
    uvcpp_work* w = new uvcpp_work();
    w->init();
    w->queue_work(
        app.loop(),
        [](uvcpp_work*) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));  // 假装很忙
        },
        [next, rp](uvcpp_work* done, int) {
          rp->json_str("{\"slow\":true}");
          rp->end();
          next();
          delete done;
        });
  });

  app.websocket("/echo", [](uvcpp_web_ws_request& ws) {
    uvcpp_ws_connection* c = ws.connection();
    if (c == nullptr) return;
    // 协议层的 `uvcpp_ws_connection::send_text` 是 (const char*, size_t) ——
    // `std::string` 那个重载在框架层的客户端上，不在连接上。
    c->on_text([c](const std::string& m) { c->send_text(m.c_str(), m.size()); });
  });
}

// =========================================================================
// 客户端侧小工具
// =========================================================================

/**
 * @brief 同步往返一次 HTTP。
 *
 * 每次新建 client：`connect_wait`/`send_wait` 是阻塞式同步接口（给脚本和测试
 * 用的），内部自己泵循环，所以连不上时它不会挂死，只会超时。
 * 外面套一层重试是因为 Windows 上 connect 偶尔撞上内核的负缓存 —— 那是系统
 * 行为，不该让示例间歇性变红。
 */
bool http_get(int port, const std::string& path, uvcpp_http_response& resp) {
  for (int i = 0; i < 40; ++i) {
    uvcpp_http_client client;
    if (client.connect_wait("127.0.0.1", port, 2000) == 0 &&
        client.send_wait(uvcpp_http_request::make_get(path), resp, 3000) == 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

/**
 * @brief 用框架自带的 WS 客户端连自己，跑一轮回显。
 *
 * 驱动方式是**一轮一轮拨**（`run(UV_RUN_NOWAIT)` + 1ms 睡眠 + 墙钟上限），
 * 和 `connect_wait()` 内部、以及仓库里的功能用例是同一套。生产路径应该用
 * `connect()` + `run()`；这里不用 `run()` 是因为它的出口只有"使用者主动
 * `stop()`"和"循环里没有活句柄"两种 —— 对端连上了却不说话时会一直不返回
 * （见 doc/webapp-guide.md「已知限制」），而示例挂死比失败难查得多。
 */
bool ws_echo(int port, const std::string& payload, int budget_ms) {
  uvcpp_web_ws_client cli;
  const std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/echo";
  std::atomic<int> opened(0), echoed(0);
  std::string got;

  cli.on_open([&](uvcpp_ws_connection*) {
       ++opened;
       cli.send_text(payload);
     })
      .on_text([&](const std::string& m) {
        got = m;
        ++echoed;
      })
      .on_error([&](int ec, const std::string& why) {
        std::fprintf(stderr, "  ws on_error: %d (%s)\n", ec, why.c_str());
      });

  if (cli.connect(url) != 0) {
    check(false, "ws 发起连接 " + url);
    return false;
  }

  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  while (echoed.load() == 0) {
    cli.run(UV_RUN_NOWAIT);
    if (echoed.load() != 0) break;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count() >= budget_ms) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 收场：发一条 Close 帧，再拨一会儿让它真的出去（不然对端看到的是掉线）。
  cli.close();
  const std::chrono::steady_clock::time_point t1 =
      std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t1).count() < 200) {
    cli.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  check(opened.load() == 1,
        "ws 握手成功（on_open " + std::to_string(opened.load()) + " 次）");
  check(echoed.load() == 1 && got == payload,
        "ws 回显一致（发 \"" + payload + "\" 收 \"" + got + "\"）");
  return opened.load() == 1 && got == payload;
}

// =========================================================================
// 自校验
// =========================================================================

/** @brief 一次 HTTP 往返 + 状态码断言，省得每条都写一遍 if。 */
void expect_status(int port, const std::string& path, int want,
                   uvcpp_http_response* out = nullptr) {
  uvcpp_http_response resp;
  if (!http_get(port, path, resp)) {
    check(false, "GET " + path + " 没拿到响应");
    return;
  }
  check(static_cast<int>(resp.status_code) == want,
        "GET " + path + " -> " + std::to_string(want) + "（实际 " +
            std::to_string(static_cast<int>(resp.status_code)) + "）");
  if (out != nullptr) *out = resp;
}

int self_check() {
  std::string why;
  if (!provision_static_root(&why)) {
    std::fprintf(stderr, "准备临时静态根失败：%s\n", why.c_str());
    return 1;
  }

  std::printf("webapp_demo —— 自校验（静态根：%s/，跑完删掉）\n\n",
              kDemoStaticRoot);

  uvcpp_web_app app;
  build_app(app, 0, kDemoStaticRoot);
  const int rc = app.start_background();
  if (rc != 0) {
    std::fprintf(stderr, "起服务失败：rc=%d\n", rc);
    remove_static_root();
    return 1;
  }
  const int port = app.bound_port();
  std::printf("服务已在 127.0.0.1:%d 起来，开始连它自己\n\n", port);

  uvcpp_http_response resp;

  std::printf("[路由 / 中间件]\n");
  expect_status(port, "/hello", 200, &resp);
  if (static_cast<int>(resp.status_code) == 200) {
    check(body_of(resp).find("hello") != std::string::npos,
          "响应体里有 hello（" + body_of(resp) + "）");
    check(http_get_header(resp.headers, "X-Demo") == "webapp-demo",
          "中间件加的 X-Demo 头在响应里");
  }
  expect_status(port, "/user/42", 200, &resp);
  check(body_of(resp) == "42", "路径参数 /user/42 -> \"42\"（实际 \"" +
                                   body_of(resp) + "\"）");

  std::printf("\n[异步 handler（工作线程 + next）]\n");
  expect_status(port, "/slow", 200, &resp);
  check(body_of(resp).find("slow") != std::string::npos,
        "/slow 的响应体是 " + body_of(resp));

  std::printf("\n[静态文件服务]\n");
  expect_status(port, "/assets/index.html", 200, &resp);
  check(body_of(resp).find("webapp_demo") != std::string::npos,
        "静态文件内容送出来了（" + body_of(resp) + "）");
  check(!http_get_header(resp.headers, "ETag").empty(),
        "响应带 ETag（" + http_get_header(resp.headers, "ETag") + "）");

  std::printf("\n[路径安全]\n");
  expect_status(port, "/assets/no-such-file.txt", 404);
  // 下面两条指向的文件都**确实存在**（上面刚写的），所以 404 不可能是"文件没
  // 找到"凑巧凑出来的 —— 只可能来自策略本身。
  expect_status(port, "/assets/.secret", 404);  // 点文件策略（默认 HIDE）
  expect_status(port, "/assets/../" + std::string(kDemoStaticRoot) +
                          "/index.html",
                404);  // 越过静态根

  std::printf("\n[WebSocket 回显]\n");
  ws_echo(port, "ping-1", 5000);

  app.stop();
  app.join();
  remove_static_root();

  std::printf("\n%s（%d 项失败）\n", g_failed == 0 ? "全部通过" : "有失败", g_failed);
  return g_failed == 0 ? 0 : 1;
}

// =========================================================================
// 常驻服务
// =========================================================================

int serve(int port, const std::string& static_dir) {
  std::signal(SIGINT, on_sigint);

  uvcpp_web_app app;
  build_app(app, port, static_dir);
  const int rc = app.start_background();
  if (rc != 0) {
    std::fprintf(stderr, "起服务失败：rc=%d（端口 %d 是不是被占了？）\n", rc, port);
    return 1;
  }

  std::printf("webapp_demo 已在 http://127.0.0.1:%d 上服务（Ctrl-C 退出）\n",
              app.bound_port());
  std::printf("  GET  /hello           JSON\n");
  std::printf("  GET  /user/:id        路径参数\n");
  std::printf("  GET  /slow            工作线程池里的异步 handler\n");
  std::printf("  GET  /assets/*        静态根 %s\n", static_dir.c_str());
  std::printf("  WS   /echo            回显\n");

  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::printf("\n收到 SIGINT，停机\n");
  app.stop();
  app.join();
  return 0;
}

void usage() {
  std::printf(
      "用法：\n"
      "  webapp_demo                    自校验（exit 0 = 全通过）；静态根用自己\n"
      "                                 造的 %s/，跑完删掉\n"
      "  webapp_demo --serve [port]     常驻服务，默认 8080\n"
      "  webapp_demo --static <dir>     常驻模式的静态根，默认 ./public\n",
      kDemoStaticRoot);
}

}  // namespace

int main(int argc, char** argv) {
  bool serve_mode = false;
  int port = 8080;
  std::string static_dir = "./public";

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--serve") {
      serve_mode = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') port = std::atoi(argv[++i]);
    } else if (a == "--static" && i + 1 < argc) {
      static_dir = argv[++i];
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "不认识的参数：%s\n\n", a.c_str());
      usage();
      return 2;
    }
  }

  return serve_mode ? serve(port, static_dir) : self_check();
}
