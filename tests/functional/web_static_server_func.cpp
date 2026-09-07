/**
 * @file tests/functional/web_static_server_func.cpp
 * @brief uvcpp_static_server 静态文件服务功能测试。
 *
 * 覆盖：
 *  - 构造与配置（无网络，验证构造器/setter 不崩溃）；
 *  - 静态文件返回（真实 HTTP 往返）；
 *  - SPA 回退（不存在的路由 → index.html）；
 *  - mtime 实时更新（文件改写后下一请求返回新内容，即「动态加载」）；
 *  - 禁用 SPA 回退时的 404；
 *  - 目录穿越防护（URL 含 ".." → 404）。
 *
 * 通过真实的 uvcpp_http_server（后台线程）+ uvcpp_http_client（阻塞同步）
 * 端到端验证，参考 tcp_server_func.cpp 的 server 线程模式。
 */
#include <iostream>
#include <string>
#include <fstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <future>
#include <cstdio>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_static_server.h>
#include <web/uvcpp_http_server.h>
#include <web/uvcpp_http_client.h>

#ifdef _WIN32
#include <direct.h>   // _mkdir / _rmdir
#endif

using namespace uvcpp;

// =========================================================================
// 临时目录与文件写入工具
// =========================================================================
static const char* kTmpDir = "uvcpp_static_test_tmp";

static std::string tmp_path(const std::string& rel) {
  return std::string(kTmpDir) + "/" + rel;
}

static bool write_text(const std::string& rel, const std::string& content) {
  std::ofstream f(tmp_path(rel).c_str(), std::ios::binary | std::ios::trunc);
  if (!f.good()) return false;
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
  f.close();
  return true;
}

static bool prepare_fs() {
#ifdef _WIN32
  _mkdir(kTmpDir);
#else
  mkdir(kTmpDir, 0755);
#endif
  return write_text("index.html", "<html>INDEX-V1</html>") &&
         write_text("app.js", "console.log('JS-V1');");
}

static void cleanup_fs() {
  std::remove(tmp_path("index.html").c_str());
  std::remove(tmp_path("app.js").c_str());
#ifdef _WIN32
  _rmdir(kTmpDir);
#else
  rmdir(kTmpDir);
#endif
}

// =========================================================================
// 后台静态服务：在独立线程跑 HTTP server，端口 0 自动分配
// （server + static_svr 均在后台线程内创建/析构，loop 只在同线程跑，安全）
// =========================================================================
struct StaticTestServer {
  std::string root;
  bool spa_fallback;
  std::promise<int> port_promise;
  std::atomic<bool> stop{false};
  std::thread thread;

  StaticTestServer(const std::string& r, bool spa) : root(r), spa_fallback(spa) {}

  int start() {
    thread = std::thread([this]() {
      uvcpp_static_server static_svr(root);
      static_svr.set_spa_fallback(spa_fallback);
      uvcpp_http_server server;
      server.on_request(static_svr.handler(&server));

      server.bind("127.0.0.1", 0);
      // 读取系统分配的实际端口
      sockaddr_in name;
      int namelen = sizeof(name);
      server.get_tcp_server()->get_tcp()->getsockname(
          reinterpret_cast<sockaddr*>(&name), &namelen);
      port_promise.set_value(ntohs(name.sin_port));

      server.listen();
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

// 阻塞式 GET：每次新建 client + connect_wait + send_wait（带连接重试，容忍 server 启动延迟）
static bool http_get(int port, const std::string& path, int& status, std::string& body) {
  uvcpp_http_response resp;
  int rc = -1;
  for (int i = 0; i < 40; ++i) {
    uvcpp_http_client client;
    rc = client.connect_wait("127.0.0.1", port, 2000);
    if (rc != 0) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); continue; }
    rc = client.send_wait(uvcpp_http_request::make_get(path), resp, 3000);
    if (rc == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (rc != 0) return false;
  status = static_cast<int>(resp.status_code);
  body.assign(resp.body.get_const_data(), resp.body.size());
  return true;
}

// =========================================================================
// Test 1: 构造与配置（无网络）
// =========================================================================
static bool test_construct_and_config() {
  uvcpp_static_server s1("frontend/dist");
  uvcpp_static_server s2("frontend/dist", "/static", "home.html");
  uvcpp_static_server s3("", "", "index.html");
  if (s1.cache_size() != 0) return false;
  s1.set_spa_fallback(false);
  s1.set_cache_enabled(false);
  s1.clear_cache();
  return true;
}

// =========================================================================
// Test 2: 静态文件返回 + SPA 回退 + mtime 实时更新
// =========================================================================
static bool test_serve_and_update() {
  if (!prepare_fs()) return false;

  StaticTestServer runner(kTmpDir, /*spa_fallback=*/true);
  int port = runner.start();

  bool ok = true;
  int status = 0;
  std::string body;

  // 2.1 根路径 -> index.html
  if (!http_get(port, "/", status, body)) ok = false;
  else if (status != 200 || body != "<html>INDEX-V1</html>") {
    std::cout << "  [err] GET / -> " << status << " '" << body << "'\n";
    ok = false;
  }

  // 2.2 具体静态文件 -> app.js
  if (!http_get(port, "/app.js", status, body)) ok = false;
  else if (status != 200 || body != "console.log('JS-V1');") {
    std::cout << "  [err] GET /app.js -> " << status << " '" << body << "'\n";
    ok = false;
  }

  // 2.3 SPA 回退：不存在的路由 -> index.html
  if (!http_get(port, "/some/client/route", status, body)) ok = false;
  else if (status != 200 || body != "<html>INDEX-V1</html>") {
    std::cout << "  [err] GET /some/client/route -> " << status << " '" << body << "'\n";
    ok = false;
  }

  // 2.4 实时更新：改写 app.js（大小与内容均变）后，下一请求应返回新内容
  if (!write_text("app.js", "console.log('JS-V2-UPDATED');")) ok = false;
  else {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    if (!http_get(port, "/app.js", status, body)) ok = false;
    else if (status != 200 || body != "console.log('JS-V2-UPDATED');") {
      std::cout << "  [err] GET /app.js(after update) -> " << status << " '" << body << "'\n";
      ok = false;
    }
  }

  runner.shutdown();
  cleanup_fs();
  return ok;
}

// =========================================================================
// Test 3: 禁用 SPA 回退时：404 + 目录穿越防护
// =========================================================================
static bool test_not_found_and_traversal() {
  if (!prepare_fs()) return false;

  StaticTestServer runner(kTmpDir, /*spa_fallback=*/false);
  int port = runner.start();

  bool ok = true;
  int status = 0;
  std::string body;

  // 3.1 存在的文件 -> 200
  if (!http_get(port, "/index.html", status, body)) ok = false;
  else if (status != 200 || body != "<html>INDEX-V1</html>") {
    std::cout << "  [err] GET /index.html -> " << status << "\n";
    ok = false;
  }

  // 3.2 不存在的文件（回退已禁用）-> 404
  if (!http_get(port, "/missing.css", status, body)) ok = false;
  else if (status != 404) {
    std::cout << "  [err] GET /missing.css -> " << status << " (expect 404)\n";
    ok = false;
  }

  // 3.3 目录穿越 -> 404
  if (!http_get(port, "/../secret.txt", status, body)) ok = false;
  else if (status != 404) {
    std::cout << "  [err] GET /../secret.txt -> " << status << " (expect 404)\n";
    ok = false;
  }

  runner.shutdown();
  cleanup_fs();
  return ok;
}

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"construct_and_config", test_construct_and_config},
    {"serve_and_update", test_serve_and_update},
    {"not_found_and_traversal", test_not_found_and_traversal},
  };
  for (const auto& t : tests) {
    std::cout << "[web_static_server] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_static_server] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}

#else
int main() {
  std::cout << "[web_static_server] SKIP (web module disabled)\n";
  return 0;
}
#endif
