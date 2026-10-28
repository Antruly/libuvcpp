/**
 * @file tests/functional/web_http_client_redirect_func.cpp
 * @brief 客户端跟随重定向（`set_follow_redirects()`）：方法改写、相对
 *        Location 解析、跳数上限、跨源不跟随。
 *
 * 这一条测试的是**替调用方改请求**的那层策略，所以判据全落在"服务端实际收到了
 * 什么"上，而不是"客户端回调说跟了几跳"：
 *
 *   1. 302 到绝对路径：跟过去，最终 200；
 *   2. 相对 Location（`ok` 从 `/dir/r302rel` 出发）解析成 `/dir/ok`；
 *   3. `../ok` 从 `/a/b/rdot` 出发解析成 `/a/ok`；
 *   4. 两跳链（302 → 301 → 200）`redirect_count() == 2`；
 *   5. 303 + POST：第二跳必须是 **GET**、body 为空、没有 `content-length`；
 *   6. 307 + POST：方法与 body **原样保留**；
 *   7. 自指环 + 跳数上限：`UV_ELOOP`，`resp` 是最后一跳的 3xx；
 *   8. 跨源 `Location`：**不跟**，把 3xx 原样交回，服务端只看到一条请求
 *      （证明没有对 `example.invalid` 发起任何连接）；
 *   9. 没开跟随（默认）：302 原样返回；
 *  10. 3xx 没有 `Location`：原样返回，不算错误。
 *
 * 装置：一个**阻塞在独立线程**的真 TCP 服务端，按请求路径回不同的报文；它把
 * 每个收到的请求（方法、目标、body）逐条记下来 —— 判据 5/6 的证据只可能来自
 * 那里。
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
  } else {
    std::cout << "  [ ok ] " << what << std::endl;
  }
}

void info(const std::string& what) {
  std::cout << "  [info] " << what << std::endl;
}

// =========================================================================
// 服务端
// =========================================================================

struct seen_request {
  std::string method;
  std::string target;
  std::string body;
  std::string headers;  ///< 头部原文（判"没有 content-length"用）
};

struct server_state {
  std::atomic<int>  listen_rc{-1};
  std::atomic<int>  requests{0};
  std::mutex        mu;
  std::vector<seen_request> seen;
};

struct parsed_request {
  std::string method;
  std::string target;
  std::string headers;  ///< 头块原文（含结尾 CRLF，不含请求行）
  std::string body;
};

std::string lower_copy(const std::string& s) {
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] + 32);
  }
  return out;
}

std::string header_of(const std::string& headers, const std::string& name) {
  const std::string low = lower_copy(headers);
  const std::string key = lower_copy(name) + ":";
  size_t p = low.find(key);
  while (p != std::string::npos) {
    // 必须在行首
    if (p == 0 || low[p - 1] == '\n') {
      size_t v = p + key.size();
      while (v < headers.size() && (headers[v] == ' ' || headers[v] == '\t')) ++v;
      size_t e = headers.find("\r\n", v);
      if (e == std::string::npos) e = headers.size();
      return headers.substr(v, e - v);
    }
    p = low.find(key, p + 1);
  }
  return std::string();
}

/// 按请求目标给出应答。`Location` 的形状覆盖绝对路径 / 相对路径 / `..` /
/// 绝对 URL（跨源）/ 缺失。
std::string reply_for(const parsed_request& r) {
  const std::string& t = r.target;
  if (t == "/r302") {
    return "HTTP/1.1 302 Found\r\nLocation: /ok\r\nContent-Length: 0\r\n\r\n";
  }
  if (t == "/dir/r302rel") {
    return "HTTP/1.1 302 Found\r\nLocation: ok\r\nContent-Length: 0\r\n\r\n";
  }
  if (t == "/a/b/rdot") {
    return "HTTP/1.1 302 Found\r\nLocation: ../ok\r\nContent-Length: 0\r\n\r\n";
  }
  if (t == "/chain1") {
    return "HTTP/1.1 302 Found\r\nLocation: /chain2\r\nContent-Length: 0\r\n\r\n";
  }
  if (t == "/chain2") {
    return "HTTP/1.1 301 Moved Permanently\r\nLocation: /ok\r\n"
           "Content-Length: 0\r\n\r\n";
  }
  if (t == "/r303") {
    return "HTTP/1.1 303 See Other\r\nLocation: /echo\r\nContent-Length: 0\r\n\r\n";
  }
  if (t == "/r307") {
    return "HTTP/1.1 307 Temporary Redirect\r\nLocation: /echo307\r\n"
           "Content-Length: 0\r\n\r\n";
  }
  if (t == "/loop") {
    return "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n";
  }
  if (t == "/cross") {
    return "HTTP/1.1 302 Found\r\nLocation: http://example.invalid/ok\r\n"
           "Content-Length: 0\r\n\r\n";
  }
  if (t == "/no-location") {
    return "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n";
  }
  // /ok, /dir/ok, /a/ok, /echo, /echo307 … 都从这里回 200，body 是请求目标。
  std::string body = "ok:" + t;
  return "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// 从 @p pending 里切出**一条完整请求**（头 + 按 CL 数出来的 body）。
bool cut_one_request(std::string& pending, parsed_request& out) {
  const size_t hdr_end = pending.find("\r\n\r\n");
  if (hdr_end == std::string::npos) return false;

  const size_t line_end = pending.find("\r\n");
  if (line_end == std::string::npos || line_end > hdr_end) return false;

  const std::string line = pending.substr(0, line_end);
  const size_t sp1 = line.find(' ');
  if (sp1 == std::string::npos) return false;
  const size_t sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) return false;

  out.method  = line.substr(0, sp1);
  out.target  = line.substr(sp1 + 1, sp2 - sp1 - 1);
  out.headers = pending.substr(line_end + 2, hdr_end - line_end - 2);

  size_t body_len = 0;
  const std::string cl = header_of(out.headers, "content-length");
  if (!cl.empty()) body_len = static_cast<size_t>(std::strtoul(cl.c_str(), nullptr, 10));

  const size_t total = hdr_end + 4 + body_len;
  if (pending.size() < total) return false;  // body 还没收全

  out.body = pending.substr(hdr_end + 4, body_len);
  pending.erase(0, total);
  return true;
}

void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                server_state& st) {
  uvcpp_tcp_server server;
  std::string      pending;

  server.set_read_callback(
      [&st, &pending](uvcpp_tcp_client& c, const net_read_result& r) {
        if (!r.is_data()) return;
        pending.append(r.data, r.size);

        for (;;) {
          parsed_request req;
          if (!cut_one_request(pending, req)) break;
          {
            std::lock_guard<std::mutex> lk(st.mu);
            seen_request s;
            s.method  = req.method;
            s.target  = req.target;
            s.body    = req.body;
            s.headers = req.headers;
            st.seen.push_back(s);
            st.requests.fetch_add(1);
          }
          const std::string resp = reply_for(req);
          if (c.write(resp.data(), resp.size(), [](int) {}) != 0) {
            c.close();
            return;
          }
        }
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

  rc = server.listen([](uvcpp_tcp_client*) {}, 128);
  st.listen_rc.store(rc);
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
    port   = port_future.get();
  }

  void shutdown() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }

  /// 服务端见过的请求快照。
  std::vector<seen_request> snapshot() {
    std::lock_guard<std::mutex> lk(st.mu);
    return st.seen;
  }
  void clear() {
    std::lock_guard<std::mutex> lk(st.mu);
    st.seen.clear();
    st.requests.store(0);
  }
};

// =========================================================================
// 客户端装置
// =========================================================================

int status_of(const uvcpp_http_response& r) {
  return static_cast<int>(r.status_code);
}

std::string body_text(const uvcpp_http_response& r) {
  if (r.body.size() == 0) return std::string();
  return std::string(r.body.get_const_data(), r.body.size());
}

// =========================================================================
// 场景
// =========================================================================

void test_follow_absolute_path(int port) {
  std::cout << "[1] 302 → 绝对路径" << std::endl;
  uvcpp_http_client c;
  check(c.connect_wait("127.0.0.1", port, 3000) == 0, "连上服务端");
  c.set_follow_redirects(true);

  uvcpp_http_response resp;
  const int rc = c.send_wait(uvcpp_http_request::make_get("/r302"), resp, 3000);
  check(rc == 0, "send_wait 返回 0");
  check(status_of(resp) == 200, "最终状态码 200");
  check(body_text(resp) == "ok:/ok", "跟到了 /ok（body 回显目标）");
  check(c.redirect_count() == 1, "跟了 1 跳");
  check(c.last_redirect_location() == "/ok", "last_redirect_location = /ok");
}

void test_relative_and_dot(int port) {
  std::cout << "[2] 相对 Location 与 `..`" << std::endl;
  {
    uvcpp_http_client c;
    check(c.connect_wait("127.0.0.1", port, 3000) == 0, "连上服务端");
    c.set_follow_redirects(true);
    uvcpp_http_response resp;
    const int rc = c.send_wait(uvcpp_http_request::make_get("/dir/r302rel"),
                               resp, 3000);
    check(rc == 0 && status_of(resp) == 200, "相对 Location 被跟随");
    check(body_text(resp) == "ok:/dir/ok", "`ok` 从 /dir/ 起算 → /dir/ok");
  }
  {
    uvcpp_http_client c;
    check(c.connect_wait("127.0.0.1", port, 3000) == 0, "连上服务端");
    c.set_follow_redirects(true);
    uvcpp_http_response resp;
    const int rc = c.send_wait(uvcpp_http_request::make_get("/a/b/rdot"),
                               resp, 3000);
    check(rc == 0 && status_of(resp) == 200, "`../ok` 被跟随");
    check(body_text(resp) == "ok:/a/ok", "`../ok` 从 /a/b/ 起算 → /a/ok");
  }
}

void test_chain(int port) {
  std::cout << "[3] 两跳链（302 → 301 → 200）" << std::endl;
  uvcpp_http_client c;
  check(c.connect_wait("127.0.0.1", port, 3000) == 0, "连上服务端");
  c.set_follow_redirects(true);

  uvcpp_http_response resp;
  const int rc = c.send_wait(uvcpp_http_request::make_get("/chain1"), resp, 5000);
  check(rc == 0 && status_of(resp) == 200, "两跳之后 200");
  check(c.redirect_count() == 2, "redirect_count = 2");
}

void test_303_rewrites_to_get(int port, test_server& srv) {
  std::cout << "[4] 303 + POST → GET（body 丢掉）" << std::endl;
  srv.clear();
  uvcpp_http_client c;
  check(c.connect_wait("127.0.0.1", port, 3000) == 0, "连上服务端");
  c.set_follow_redirects(true);

  uvcpp_http_request req =
      uvcpp_http_request::make_post("/r303", "payload=1", 9, "text/plain");
  uvcpp_http_response resp;
  const int rc = c.send_wait(req, resp, 3000);
  check(rc == 0 && status_of(resp) == 200, "303 之后拿到 200");

  const std::vector<seen_request> seen = srv.snapshot();
  check(seen.size() == 2, "服务端收到两条请求（实际 " +
                              std::to_string(seen.size()) + "）");
  if (seen.size() == 2) {
    check(seen[0].method == "POST" && seen[0].target == "/r303",
          "第一条是 POST /r303");
    check(seen[1].method == "GET", "第二条变成 GET");
    check(seen[1].target == "/echo", "第二条去 /echo");
    check(seen[1].body.empty(), "第二条没有 body");
    check(lower_copy(seen[1].headers).find("content-length") ==
              std::string::npos,
          "第二条没有 content-length（否则服务端会等一个不来的 body）");
  }
}

void test_307_keeps_method_and_body(int port, test_server& srv) {
  std::cout << "[5] 307 + POST → 方法与 body 原样保留" << std::endl;
  srv.clear();
  uvcpp_http_client c;
  check(c.connect_wait("127.0.0.1", port, 3000) == 0, "连上服务端");
  c.set_follow_redirects(true);

  uvcpp_http_request req =
      uvcpp_http_request::make_post("/r307", "payload=2", 9, "text/plain");
  uvcpp_http_response resp;
  const int rc = c.send_wait(req, resp, 3000);
  check(rc == 0 && status_of(resp) == 200, "307 之后拿到 200");

  const std::vector<seen_request> seen = srv.snapshot();
  check(seen.size() == 2, "服务端收到两条请求（实际 " +
                              std::to_string(seen.size()) + "）");
  if (seen.size() == 2) {
    check(seen[1].method == "POST", "第二条仍是 POST");
    check(seen[1].target == "/echo307", "第二条去 /echo307");
    check(seen[1].body == "payload=2", "body 逐字节保留");
  }
}

void test_hop_limit(int port) {
  std::cout << "[6] 自指环：跳数上限 → UV_ELOOP" << std::endl;
  uvcpp_http_client c;
  check(c.connect_wait("127.0.0.1", port, 3000) == 0, "连上服务端");
  c.set_follow_redirects(true, 3);

  uvcpp_http_response resp;
  const int rc = c.send_wait(uvcpp_http_request::make_get("/loop"), resp, 5000);
  check(rc == UV_ELOOP, "返回 UV_ELOOP（实际 " + std::to_string(rc) + "）");
  check(status_of(resp) == 302, "resp 里是最后一跳的 302");
  check(c.redirect_count() == 3, "跟满 3 跳后停下");
}

void test_cross_origin_not_followed(int port, test_server& srv) {
  std::cout << "[7] 跨源 Location：不跟随" << std::endl;
  srv.clear();
  uvcpp_http_client c;
  check(c.connect_wait("127.0.0.1", port, 3000) == 0, "连上服务端");
  c.set_follow_redirects(true);

  uvcpp_http_response resp;
  const int rc = c.send_wait(uvcpp_http_request::make_get("/cross"), resp, 3000);
  check(rc == 0, "返回 0（3xx 原样交付，不是错误）");
  check(status_of(resp) == 302, "状态码仍是 302");
  check(c.redirect_count() == 0, "没有跟跳");
  check(srv.snapshot().size() == 1,
        "服务端只看到一条请求（没有向 example.invalid 发起任何连接）");
}

void test_disabled_by_default(int port) {
  std::cout << "[8] 默认关：302 原样返回" << std::endl;
  uvcpp_http_client c;
  check(c.connect_wait("127.0.0.1", port, 3000) == 0, "连上服务端");
  check(!c.follow_redirects(), "默认 follow_redirects() 为假");

  uvcpp_http_response resp;
  const int rc = c.send_wait(uvcpp_http_request::make_get("/r302"), resp, 3000);
  check(rc == 0 && status_of(resp) == 302, "拿到的是 302，不是跟过去的 200");
  check(c.redirect_count() == 0, "没有跟跳");
}

void test_missing_location(int port) {
  std::cout << "[9] 3xx 没有 Location：原样返回" << std::endl;
  uvcpp_http_client c;
  check(c.connect_wait("127.0.0.1", port, 3000) == 0, "连上服务端");
  c.set_follow_redirects(true);

  uvcpp_http_response resp;
  const int rc =
      c.send_wait(uvcpp_http_request::make_get("/no-location"), resp, 3000);
  check(rc == 0 && status_of(resp) == 302, "302 原样交付");
  check(c.redirect_count() == 0, "没有跟跳");
}

}  // namespace

int main() {
  std::cout << "HTTP 客户端重定向跟随" << std::endl;
  test_server srv;
  if (srv.port <= 0) {
    std::cerr << "服务端没起来" << std::endl;
    srv.shutdown();
    return 1;
  }

  test_follow_absolute_path(srv.port);
  test_relative_and_dot(srv.port);
  test_chain(srv.port);
  test_303_rewrites_to_get(srv.port, srv);
  test_307_keeps_method_and_body(srv.port, srv);
  test_hop_limit(srv.port);
  test_cross_origin_not_followed(srv.port, srv);
  test_disabled_by_default(srv.port);
  test_missing_location(srv.port);

  srv.shutdown();
  if (g_failures == 0) {
    std::cout << "全部通过" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " 条失败" << std::endl;
  return 1;
}

#else  // !UVCPP_WEB_ENABLE

int main() {
  std::cout << "SKIP: web 未启用" << std::endl;
  return 0;
}

#endif
