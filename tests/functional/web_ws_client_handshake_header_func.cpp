/**
 * @file tests/functional/web_ws_client_handshake_header_func.cpp
 * @brief `uvcpp_ws_client::set_handshake_header` —— 自定义 Upgrade 头。
 *
 * 判据落在**裸服务端真正收到的 Upgrade 请求字节**上（不经过本库的服务端
 * 解析），这样"客户端发出去了"与"服务端愿意收"两件事分开：
 *
 *   [1] 普通头按添加顺序出现在请求里；
 *   [2] 同名头（大小写不敏感）只保留最后一次的值，且只出现一次；
 *   [3] 保留头（Host / Upgrade / Sec-WebSocket-Key / Version / Extensions）
 *       无论大小写都被忽略，标准行为不被改写；
 *   [4] CR/LF 注入被整条拒绝（既不出现该头，也不出现被注入的头）；
 *   [5] 空值允许（`X-Empty:` 只有一行），空名字忽略；
 *   [6] `Sec-WebSocket-Protocol` 可以透传（uvscope 的后端 WS 调试依赖它）。
 */

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <uv.h>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_WEB_WS_ENABLE

#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_ws_client.h>
#include <web/uvcpp_ws_server.h>

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
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] + 32);
  }
  return out;
}

/// 取一个头（名字大小写不敏感，必须在一行的开头）的值；没有时返回空串。
std::string header_value(const std::string& raw, const std::string& name) {
  const std::string low = lower_copy(raw);
  const std::string key = lower_copy(name) + ":";
  std::size_t pos = 0;
  while ((pos = low.find(key, pos)) != std::string::npos) {
    if (pos != 0 && low[pos - 1] != '\n') {
      pos += key.size();
      continue;
    }
    std::size_t v = pos + key.size();
    while (v < raw.size() && (raw[v] == ' ' || raw[v] == '\t')) ++v;
    std::size_t e = raw.find("\r\n", v);
    if (e == std::string::npos) e = raw.size();
    while (e > v && (raw[e - 1] == ' ' || raw[e - 1] == '\t')) --e;
    return raw.substr(v, e - v);
  }
  return std::string();
}

/// 一个头在请求里出现了几次（只数行首，名字大小写不敏感）。
int header_count(const std::string& raw, const std::string& name) {
  const std::string low = lower_copy(raw);
  const std::string key = lower_copy(name) + ":";
  int count = 0;
  std::size_t pos = 0;
  while ((pos = low.find(key, pos)) != std::string::npos) {
    if (pos == 0 || low[pos - 1] == '\n') ++count;
    pos += key.size();
  }
  return count;
}

void test_handshake_headers() {
  std::string captured;
  bool replied = false;

  uvcpp_tcp_server raw;
  raw.set_read_callback([&](uvcpp_tcp_client& c, const net_read_result& r) {
    if (!r.is_data()) return;
    captured.append(r.data, r.size);
    if (replied) return;
    if (captured.find("\r\n\r\n") == std::string::npos) return;
    replied = true;
    const std::string key = header_value(captured, "Sec-WebSocket-Key");
    const std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        uvcpp_ws_server::compute_accept_key(key) + "\r\n\r\n";
    c.write(resp.c_str(), resp.size(), nullptr);
  });
  if (raw.bindIpv4("127.0.0.1", 0) != 0) {
    check(false, "裸服务端绑定 127.0.0.1");
    return;
  }
  sockaddr_in addr;
  int addr_len = sizeof(addr);
  raw.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&addr), &addr_len);
  const int port = ntohs(addr.sin_port);
  raw.listen([](uvcpp_tcp_client*) {}, 128);

  auto* cli = new uvcpp_ws_client();
  cli->set_handshake_header("Authorization", "Bearer first");
  cli->set_handshake_header("authorization", "Bearer final");   // 覆盖
  cli->set_handshake_header("Cookie", "sid=1");
  cli->set_handshake_header("X-Smoke", "ok");
  cli->set_handshake_header("Sec-WebSocket-Protocol", "uvscope.test");
  cli->set_handshake_header("Host", "evil.example");              // 保留
  cli->set_handshake_header("Upgrade", "h2c");                    // 保留
  cli->set_handshake_header("Sec-WebSocket-Key", "ZmFrZQ==");     // 保留
  cli->set_handshake_header("Sec-WebSocket-Version", "8");        // 保留
  cli->set_handshake_header("Sec-WebSocket-Extensions", "nope");  // 保留
  cli->set_handshake_header("X-Inject", "a\r\nInjected: 1");      // CR/LF 拒绝
  cli->set_handshake_header("", "ignored");                       // 空名字
  cli->set_handshake_header("X-Empty", "");                       // 空值允许

  bool open = false;
  int err = 0;
  const int rc =
      cli->connect("ws://127.0.0.1:" + std::to_string(port) + "/chat",
                   [&](uvcpp_ws_connection*, int e) {
                     err = e;
                     open = (e == 0);
                   });
  check(rc == 0, "connect() 接受连接请求");

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!open && std::chrono::steady_clock::now() < deadline) {
    raw.run(UV_RUN_NOWAIT);
    cli->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  check(replied, "裸服务端收到了 Upgrade 请求");
  check(open, "握手完成（err=" + std::to_string(err) + "）");
  check(captured.rfind("GET /chat HTTP/1.1\r\n", 0) == 0,
        "请求行是 GET /chat HTTP/1.1");

  check(header_value(captured, "Authorization") == "Bearer final",
        "[1][2] 同名头保留最后一次的值");
  check(header_count(captured, "Authorization") == 1,
        "[2] 同名头只出现一次");
  check(header_value(captured, "Cookie") == "sid=1", "[1] Cookie 透传");
  check(header_value(captured, "X-Smoke") == "ok", "[1] 自定义头透传");
  check(header_value(captured, "Sec-WebSocket-Protocol") == "uvscope.test",
        "[6] Sec-WebSocket-Protocol 透传");

  check(header_value(captured, "Host").find("evil.example") == std::string::npos,
        "[3] 保留头 Host 被忽略");
  check(header_value(captured, "Host").find("127.0.0.1") != std::string::npos,
        "[3] Host 仍是真实目标");
  check(header_value(captured, "Upgrade") == "websocket",
        "[3] 保留头 Upgrade 被忽略");
  check(header_value(captured, "Sec-WebSocket-Version") == "13",
        "[3] 保留头 Sec-WebSocket-Version 被忽略");
  check(!header_value(captured, "Sec-WebSocket-Key").empty() &&
            header_value(captured, "Sec-WebSocket-Key") != "ZmFrZQ==",
        "[3] 保留头 Sec-WebSocket-Key 被忽略");
  check(header_value(captured, "Sec-WebSocket-Extensions") != "nope",
        "[3] 保留头 Sec-WebSocket-Extensions 被忽略");

  check(header_count(captured, "X-Inject") == 0, "[4] CR/LF 头被整条拒绝");
  check(lower_copy(captured).find("injected:") == std::string::npos,
        "[4] 没有头注入");
  check(header_count(captured, "X-Empty") == 1,
        "[5] 空值头允许出现一次");
  check(header_value(captured, "X-Empty").empty(), "[5] 空值头值为空");

  cli->close();
  const auto close_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!cli->has_status(WS_CLIENT_CLOSED) &&
         std::chrono::steady_clock::now() < close_deadline) {
    raw.run(UV_RUN_NOWAIT);
    cli->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  delete cli;
}

}  // namespace

int main() {
  std::cout << "WS client handshake headers" << std::endl;
  test_handshake_headers();
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
