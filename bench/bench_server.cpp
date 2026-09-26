/**
 * @file bench/bench_server.cpp
 * @brief 压测靶场 —— 用本库搭一个**可配 N 条事件循环**的 HTTP 服务，用来量 RPS。
 *
 * 为什么不拿 `examples/webapp_demo` 当靶场：它**每个请求**都写一行访问日志，而那一行
 * 要过全局日志锁（一次请求五次锁）、两次 `vsnprintf`，再 `fwrite` 到 stdout。本机实测
 * 它一个核就吃满了 —— 拿它当靶场，量到的主要是日志系统的价钱，不是 HTTP 路径的价钱。
 * 这个靶场**默认一个中间件都不装**，把日志变成一根能单独扳动的旋钮，于是
 * 「日志值多少 RPS」本身也成了一个可以量出来的量。
 *
 * 口径（要和 `doc/benchmark.md` 里那条读数对得上）：
 *  - **默认单事件循环** —— `start_background()` 起一个线程、一个 loop，服务端不吃第二个核；
 *  - `--loops N`（N>1）改成「**1 条接受者 + N−1 条工作循环**，N 条线程」，用来量横向扩展。
 *    `N == 1` 与不传这个开关**逐字节相同**（契约见 `src/webapp/uvcpp_web_app.h:968-990`）。
 *    注意 N>1 之后每条被接受的连接都要**转手**给工作循环（Windows 上是
 *    `WSADuplicateSocketW` + `WSASocketW(FROM_PROTOCOL_INFO)` + `uv_tcp_open`），而 N==1 是
 *    就地 `finish_accept`、**不付这笔税** —— 所以 `N=1` 与 `N=4` 的差是「扩展收益 − 转手税」
 *    一个数两件事，要拆开量就得再插一档 `N=2`（见 `doc/benchmark.md` 多循环那一节）；
 *  - `/json` 返回一份**内存里预先拼好**的 JSON，不序列化、不碰磁盘、不进工作线程池；
 *  - 默认不装访问日志、不装静态路由、不用 TLS。
 *
 * 读数必须自带配置，所以启动时把版本、开关、实际端口全部打出来（端口传 0 时由内核分配，
 * 不打出来就复现不了）。
 *
 * 用法：
 *   uvcpp_bench_server [--port N] [--loops N] [--access-log] [--log-level LEVEL]
 *                      [--static <dir>]
 *
 *   --port N         监听端口，0（默认）= 由内核分配，实际端口看启动横幅
 *   --loops N        事件循环数 1..64，默认 1（= 单事件循环）。越界报 UV_EINVAL
 *   --access-log     装上访问日志中间件（用来量它的价钱），默认**不装**
 *   --log-level L    TRACE/DEBUG/INFO/WARN/ERR/FATAL，默认只放行 WARN 及以上
 *   --static <dir>   额外挂一个静态根到 /assets（默认不挂）
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_version.h>

#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_middleware.h>

using namespace uvcpp;

namespace {

std::atomic<bool> g_stop(false);

void on_sigint(int) { g_stop = true; }

const char* pool_state() {
#if defined(UVCPP_ENABLE_MEMORY_POOL) && (UVCPP_ENABLE_MEMORY_POOL == 1)
  return "on";
#else
  return "off";
#endif
}

bool parse_level(const std::string& s, log_level* out) {
  if (s == "TRACE") { *out = log_level::TRACE; return true; }
  if (s == "DEBUG") { *out = log_level::DEBUG; return true; }
  if (s == "INFO")  { *out = log_level::INFO;  return true; }
  if (s == "WARN")  { *out = log_level::WARN;  return true; }
  if (s == "ERR" || s == "ERROR") { *out = log_level::ERR; return true; }
  if (s == "FATAL") { *out = log_level::FATAL; return true; }
  return false;
}

void usage() {
  std::printf(
      "用法：uvcpp_bench_server [--port N] [--loops N] [--access-log]"
      " [--log-level LEVEL] [--static <dir>]\n");
}

}  // namespace

int main(int argc, char** argv) {
  int port = 0;
  int loops = 1;
  bool access_log = false;
  std::string static_dir;
  log_level level = log_level::WARN;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (a == "--loops" && i + 1 < argc) {
      loops = std::atoi(argv[++i]);
    } else if (a == "--access-log") {
      access_log = true;
    } else if (a == "--log-level" && i + 1 < argc) {
      if (!parse_level(argv[++i], &level)) {
        std::fprintf(stderr, "不认识的等级：%s\n", argv[i]);
        return 2;
      }
    } else if (a == "--static" && i + 1 < argc) {
      static_dir = argv[++i];
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "不认识的参数：%s\n", a.c_str());
      usage();
      return 2;
    }
  }

  // 先把等级定死再起服务：默认 WARN，于是即便某个模块在热路径上留了 DEBUG 日志，
  // 它也只花掉一次 `is_enabled` 判断。要量日志的价钱就 --log-level INFO。
  //
  // ★ 这一句**必须排在 `app.start_background()` 之前**，而且**只有**在靶场不调
  //   `app.set_log_level()` 时才管用 —— 后者会把配置里的 `min_log_level` 当成
  //   显式意图覆盖掉全局等级（见 `uvcpp_web_app.cpp` 的 `init_process_once`）。
  //   在 1.4.0 之前不是这样：那时 `init_process_once()` 无条件覆盖，于是
  //   `--log-level` 两头都失灵（WARN/ERR 被抬成 INFO、TRACE/DEBUG 被压成 INFO），
  //   而下面那行自报还印着**请求值**，读数整个是假的。修好之后自报值与生效值
  //   才是一回事 —— 这也是"量访问日志的价钱"这件事能成立的前提。
  uvcpp_logger::instance().set_level(level);

  std::signal(SIGINT, on_sigint);

  // 预先拼好的响应体。每个请求都重新拼一个字符串就会把测量的对象从
  // 「HTTP 路径」变成「字符串拼接」，所以这里在循环外拼一次。
  const std::string json_body = "{\"ok\":true,\"message\":\"hello\",\"n\":42}";

  uvcpp_web_app app;
  app.set_host("127.0.0.1").set_port(port).set_access_log(false);

  // 多循环是**结构性**开关（开 N−1 条线程、每格建自己的句柄），必须在
  // `start_background()` 之前定下来。返回值刻意不是 `*this`（见
  // `src/webapp/uvcpp_web_app.h:974-985`），所以链不进上面那一串 `set_*` 里。
  //
  // 这里只走得到 `UV_EINVAL`（--loops 0 / 65 / 非数字都归 0）；`UV_EBUSY`（装晚了）
  // 在这条路上不可达 —— 本行在 start 之前，且没人先动过 `tcp_server()`。两个码都判、
  // 都印：印错的那一半比不印好，且日后若有人把这行挪到 start 之后，会立刻看见原因。
  const int lrc = app.set_loops(loops);
  if (lrc != 0) {
    std::fprintf(stderr, "set_loops(%d) 失败：rc=%d（%s）\n", loops, lrc,
                 lrc == UV_EINVAL ? "UV_EINVAL 参数越界（合法区间 1..64）"
                                  : "UV_EBUSY 装晚了");
    return 2;
  }

  if (access_log) {
    app.use(web_middleware_access_log());
  }

  app.get("/json", [&json_body](uvcpp_web_request&, uvcpp_web_response& resp,
                                uvcpp_web_next) {
    resp.json_str(json_body);
    resp.end();
  });

  app.get("/text", [](uvcpp_web_request&, uvcpp_web_response& resp,
                      uvcpp_web_next) {
    resp.text("ok");
    resp.end();
  });

  // 逐循环的分布：**分布不是判据，能看见分布才是判据**。多循环最坏的那种失败是
  // 「只有一个循环真干活」，而它与成功**逐字相同** —— 只有把逐格计数读出来才分得开。
  // 只在 curl 时被访问，不进 `/json` 的热路径；**不要**改成负载期用定时器打印，
  // 那会把量具本身变成噪声源。
  app.get("/stats", [&app](uvcpp_web_request&, uvcpp_web_response& resp,
                           uvcpp_web_next) {
    const int n = app.loop_count();
    std::string body = "{\"loops\":" + std::to_string(n) + ",\"per_loop\":[";
    for (int i = 0; i < n; ++i) {
      if (i != 0) body += ",";
      body += std::to_string(app.connection_count_at(i));
    }
    body += "],\"total\":" + std::to_string(app.connection_count()) + "}";
    resp.json_str(body);
    resp.end();
  });

  if (!static_dir.empty()) {
    app.serve_static("/assets", static_dir);
  }

  const int rc = app.start_background();
  if (rc != 0) {
    std::fprintf(stderr, "起服务失败：rc=%d（端口 %d 是不是被占了？）\n", rc, port);
    return 1;
  }

  // 这一块就是「读数自带配置」。少打一行，这个数就从读数降级成传闻。
  std::printf("uvcpp_bench_server %s\n", UVCPP_VERSION_STRING);
  std::printf("  listen        http://127.0.0.1:%d\n", app.bound_port());
  // N==1 那一行**逐字节**与加 `--loops` 之前相同 —— 于是改动前后的 `--loops 1` 日志
  // 可以直接 diff，这是「n=1 与不调 set_loops 逐字节相同」在靶场侧的一次端到端证据。
  if (app.loop_count() == 1) {
    std::printf("  loop          单事件循环（一个线程）\n");
  } else {
    std::printf("  loop          %d 条循环（1 条接受者 + %d 条工作循环）\n",
                app.loop_count(), app.loop_count() - 1);
  }
  std::printf("  routes        GET /json（内存 JSON）、GET /text、GET /stats\n");
  std::printf("  access_log    %s\n", access_log ? "ON" : "off");
  // 印**生效值**而不是请求值：这两者曾经不一样（见上面那段说明），而一行
  // 自报假掉的读数比没有读数更糟 —— 它会让人相信一个不存在的配置。
  // `uvcpp_logger::instance().global_level()` 读的就是真正在过滤的那个数。
  // （不是 `level(category)` —— 那个返回的是叠加了模块覆盖之后的结果，
  //  `--log-level` 设的是全局那一档。）
  std::printf("  log_level     %s\n",
              uvcpp_log_level_name(uvcpp_logger::instance().global_level()));
  std::printf("  memory_pool   %s\n", pool_state());
  std::printf("  try_write     %s (min %d B)\n",
              UVCPP_TRY_WRITE_ENABLE ? "on" : "off", UVCPP_TRY_WRITE_MIN_BYTES);
  if (!static_dir.empty()) {
    std::printf("  static        /assets -> %s\n", static_dir.c_str());
  }
  std::fflush(stdout);

  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::printf("\n收到 SIGINT，停机\n");
  app.stop();
  app.join();
  return 0;
}
