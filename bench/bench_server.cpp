/**
 * @file bench/bench_server.cpp
 * @brief 压测靶场 —— 用本库搭一个**单事件循环**的 HTTP 服务，用来量 RPS。
 *
 * 为什么不拿 `examples/webapp_demo` 当靶场：它**每个请求**都写一行访问日志，而那一行
 * 要过全局日志锁（一次请求五次锁）、两次 `vsnprintf`，再 `fwrite` 到 stdout。本机实测
 * 它一个核就吃满了 —— 拿它当靶场，量到的主要是日志系统的价钱，不是 HTTP 路径的价钱。
 * 这个靶场**默认一个中间件都不装**，把日志变成一根能单独扳动的旋钮，于是
 * 「日志值多少 RPS」本身也成了一个可以量出来的量。
 *
 * 口径（要和 `doc/benchmark.md` 里那条读数对得上）：
 *  - **单事件循环** —— `start_background()` 起一个线程、一个 loop，服务端不吃第二个核；
 *  - `/json` 返回一份**内存里预先拼好**的 JSON，不序列化、不碰磁盘、不进工作线程池；
 *  - 默认不装访问日志、不装静态路由、不用 TLS。
 *
 * 读数必须自带配置，所以启动时把版本、开关、实际端口全部打出来（端口传 0 时由内核分配，
 * 不打出来就复现不了）。
 *
 * 用法：
 *   uvcpp_bench_server [--port N] [--access-log] [--log-level LEVEL] [--static <dir>]
 *
 *   --port N         监听端口，0（默认）= 由内核分配，实际端口看启动横幅
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
      "用法：uvcpp_bench_server [--port N] [--access-log] [--log-level LEVEL]"
      " [--static <dir>]\n");
}

}  // namespace

int main(int argc, char** argv) {
  int port = 0;
  bool access_log = false;
  std::string static_dir;
  log_level level = log_level::WARN;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) {
      port = std::atoi(argv[++i]);
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
  uvcpp_logger::instance().set_level(level);

  std::signal(SIGINT, on_sigint);

  // 预先拼好的响应体。每个请求都重新拼一个字符串就会把测量的对象从
  // 「HTTP 路径」变成「字符串拼接」，所以这里在循环外拼一次。
  const std::string json_body = "{\"ok\":true,\"message\":\"hello\",\"n\":42}";

  uvcpp_web_app app;
  app.set_host("127.0.0.1").set_port(port).set_access_log(false);

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
  std::printf("  loop          单事件循环（一个线程）\n");
  std::printf("  routes        GET /json（内存 JSON）、GET /text\n");
  std::printf("  access_log    %s\n", access_log ? "ON" : "off");
  std::printf("  log_level     %s\n", uvcpp_log_level_name(level));
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
