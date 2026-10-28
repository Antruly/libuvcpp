/**
 * @file tests/functional/web_ws_subprotocol_func.cpp
 * @brief 服务端 WS 子协议选择（`Sec-WebSocket-Protocol`，RFC 6455 §4.1）。
 *
 * 1.9.0 之前 h1 的 WS 握手**完全不看**这个头：不管客户端提了什么，101 里都
 * 没有这一行。h2/h3 那条腿更糟 —— 它把客户端提的**整串**原样回过去，而
 * "服务端必须**挑一个**客户端提过的名字"，回合的是一串就等于两个都合规的
 * 客户端才会现形的协议违例。
 *
 * 判据落在**101 响应的字节**上：
 *
 * | # | 客户端提议 | 期望 |
 * |---|---|---|
 * | 1 | `chat, superchat` | 回 `superchat`（服务端偏好顺序在前） |
 * | 2 | `chat` | 回 `chat` |
 * | 3 | `bogus` | **不回**这个头（合法降级，握手仍成功） |
 * | 4 | 不提 | 不回这个头 |
 *
 * 另外每条都断言 `uvcpp_ws_connection::subprotocol()` 与线上那个名字一致 ——
 * 业务侧分派读的是它。
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_WEB_WS_ENABLE

#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_ws_connection.h>
#include <web/uvcpp_ws_server.h>

#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "ok: " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << what << std::endl;
  }
}

std::string lower_copy(const std::string& s) {
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] + 32);
  }
  return out;
}

struct server_state {
  std::mutex        mu;
  std::vector<std::string> seen;  ///< 每条连接上 `subprotocol()` 的值
  std::atomic<int>  accepted{0};
};

int bound_port(uvcpp_ws_server& s) {
  sockaddr_in n;
  int         l = sizeof(n);
  s.get_http_server()->get_tcp_server()->get_tcp()->getsockname(
      reinterpret_cast<sockaddr*>(&n), &l);
  return ntohs(n.sin_port);
}

/// 发一次 WS 握手，读回响应头。返回响应原文（含头）。
bool handshake(int port, const char* offered, std::string& got) {
  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;

  std::string req = "GET /ws HTTP/1.1\r\n";
  req += "Host: localhost\r\n";
  req += "Upgrade: websocket\r\n";
  req += "Connection: Upgrade\r\n";
  req += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
  req += "Sec-WebSocket-Version: 13\r\n";
  if (offered != nullptr) {
    req += "Sec-WebSocket-Protocol: ";
    req += offered;
    req += "\r\n";
  }
  req += "\r\n";
  if (c.write_wait(req.data(), req.size(), 3000) != 0) return false;

  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  for (;;) {
    if (got.find("\r\n\r\n") != std::string::npos) return true;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count() >= 3000) {
      return false;
    }
    uvcpp_buf b;
    const int rv = c.read_wait(b, 30);
    if (rv == 0) {
      if (b.size() > 0 && b.get_const_data() != nullptr) {
        got.append(b.get_const_data(), b.size());
      }
    } else if (rv != UV_ETIMEDOUT) {
      return false;
    }
  }
}

/// 从响应头里取 `sec-websocket-protocol` 的值；没有时为空。
std::string response_protocol(const std::string& resp) {
  const std::string low = lower_copy(resp);
  const std::string key = "sec-websocket-protocol:";
  size_t p = low.find(key);
  if (p == std::string::npos || (p != 0 && low[p - 1] != '\n')) return std::string();
  size_t v = p + key.size();
  while (v < resp.size() && (resp[v] == ' ' || resp[v] == '\t')) ++v;
  size_t e = resp.find("\r\n", v);
  return resp.substr(v, e == std::string::npos ? std::string::npos : e - v);
}

void run_loop(uvcpp_ws_server& srv, std::atomic<bool>& stop) {
  while (!stop.load()) {
    srv.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void test_subprotocols() {
  server_state st;
  uvcpp_ws_server srv;
  srv.set_subprotocols({"superchat", "chat"});
  check(srv.get_subprotocols().size() == 2, "服务端配置了 2 个子协议");
  srv.bind("127.0.0.1", 0);
  const int port = bound_port(srv);
  check(port > 0, "WS 服务端绑上了端口（" + std::to_string(port) + "）");

  srv.on_connection([&st](uvcpp_ws_connection* c) {
    std::lock_guard<std::mutex> lk(st.mu);
    st.seen.push_back(c->subprotocol());
    st.accepted.fetch_add(1);
  });
  srv.listen();

  std::atomic<bool> stop{false};
  std::thread th(run_loop, std::ref(srv), std::ref(stop));

  struct Case {
    const char* offer;
    const char* want_online;  ///< 期望在 101 里出现的值（"" = 不应该有这个头）
    const char* label;
  } cases[] = {
      {"chat, superchat", "superchat", "服务端偏好顺序在前"},
      {"chat", "chat", "只提一个"},
      {"bogus", "", "没有交集（合法降级）"},
      {nullptr, "", "客户端不提"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    std::string resp;
    const bool ok = handshake(port, cases[i].offer, resp);
    check(ok, std::string(cases[i].label) + "：握手完成");
    if (!ok) continue;
    check(resp.compare(0, 12, "HTTP/1.1 101") == 0, "响应是 101");
    const std::string got = response_protocol(resp);
    check(got == cases[i].want_online,
          std::string(cases[i].label) + "：101 里的子协议是 \"" +
              cases[i].want_online + "\"（实际 \"" + got + "\"）");
  }

  // 服务端会话上读到的值必须与线上一致（每条连接一次）。
  //
  // 这里**不能**用 `uvcpp_test::wait_until(loop, …)`：那个会自己泵这条循环，
  // 而循环此刻正被上面那个线程泵着 —— 同一条循环不许两个线程跑。所以只睡等。
  {
    const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    while (st.accepted.load() < 4 &&
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
                   .count() < 3000) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  check(st.accepted.load() >= 4, "服务端建立了 4 条 WS 会话（实际 " +
                                     std::to_string(st.accepted.load()) + "）");
  {
    std::lock_guard<std::mutex> lk(st.mu);
    check(st.seen.size() == 4, "记录了 4 条会话（实际 " +
                                   std::to_string(st.seen.size()) + "）");
    if (st.seen.size() == 4) {
      check(st.seen[0] == "superchat", "会话 1 的 subprotocol() = superchat");
      check(st.seen[1] == "chat", "会话 2 的 subprotocol() = chat");
      check(st.seen[2].empty(), "会话 3 的 subprotocol() 为空");
      check(st.seen[3].empty(), "会话 4 的 subprotocol() 为空");
    }
  }

  stop.store(true);
  if (th.joinable()) th.join();
}

}  // namespace

int main() {
  std::cout << "WS 子协议选择" << std::endl;
  test_subprotocols();
  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "all checks passed" << std::endl;
  return 0;
}

#else  // !(web && ws)

int main() {
  std::cout << "SKIP: web / ws disabled" << std::endl;
  return 0;
}

#endif
