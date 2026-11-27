/**
 * @file tests/functional/web_app_ws_alive_func.cpp
 * @brief WS 连接的安全存活查询与按 id 发送（issue #87）。
 *
 * 判据：
 *
 * 1. `uvcpp_ws_connection::id()` 非 0；`app.ws_is_alive(id)` 在连接存活时为真；
 * 2. `app.ws_send_text(id, ...)` 能把帧投到会话自己的循环上，真客户端收到；
 * 3. 对端关闭后 `ws_is_alive(id)` 变假，再 `ws_send_*` 立刻返回 -1 且回调收到
 *    -1（不再依赖裸指针，也不会卡在发送路径）；
 * 4. `send_data()` 的 retired 守卫：会话 `terminate()` 之后同一回调里发大帧，
 *    立刻返回 -1、回调恰好一次（旧实现会先跑一次 deflate）。
 */

#include <uvcpp/uvcpp_define.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <web/uvcpp_ws_client.h>
#include <webapp/uvcpp_web_app.h>

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::printf("  [ ok ] %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("  [FAIL] %s\n", what.c_str());
  }
  std::fflush(stdout);
}

/// 拨真客户端的循环，直到谓词成立或超时。
bool pump_client(uvcpp_ws_client& c, const std::function<bool()>& done,
                 int ms) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    c.run(UV_RUN_NOWAIT);
    if (!done()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 3; ++i) c.run(UV_RUN_NOWAIT);
  return done();
}

}  // namespace

int main() {
  std::printf("[ws_alive] issue #87\n");

  std::atomic<unsigned long long> first_id(0);
  std::atomic<int> guard_rc(-999);
  std::atomic<int> guard_cb(-999);
  std::atomic<int> guard_calls(0);

  uvcpp_web_app app;
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);

  app.websocket("/ws", [&](uvcpp_web_ws_request& ws) {
    uvcpp_ws_connection* c = ws.connection();
    first_id.store(static_cast<unsigned long long>(c->id()));
  });

  // 同一个回调里先 terminate 再发大帧：守卫必须让它立刻返回。
  app.websocket("/retire-send", [&](uvcpp_web_ws_request& ws) {
    uvcpp_ws_connection* c = ws.connection();
    c->terminate();
    std::vector<char> big(700 * 1024, 'x');
    guard_rc.store(c->send_binary(big.data(), big.size(),
                                  [&](int st) {
                                    ++guard_calls;
                                    guard_cb.store(st);
                                  }));
  });

  check(app.start_background() == 0, "app.start_background");
  const int port = app.bound_port();
  check(port > 0, "拿到端口");

  // ---- 1/2：存活 + 按 id 发送 ------------------------------------------
  uvcpp_ws_client client;
  bool             connected = false;
  int              got = 0;
  std::string      last;
  const int rc = client.connect(
      "ws://127.0.0.1:" + std::to_string(port) + "/ws",
      [&](uvcpp_ws_connection* c, int err) {
        if (err != 0 || c == nullptr) return;
        connected = true;
        c->on_text([&](const std::string& m) {
          last = m;
          ++got;
        });
      });
  check(rc == 0, "真客户端 connect 调用成功");
  check(pump_client(client, [&] { return connected && first_id.load() != 0; },
                    5000),
        "连接建立且服务端记录了 id");

  const uvcpp_ws_id id = static_cast<uvcpp_ws_id>(first_id.load());
  check(id != 0, "id 非 0");
  check(app.ws_is_alive(id), "存活时 ws_is_alive(id) 为真");

  std::atomic<int> send_cb(-1);
  check(app.ws_send_text(id, "via-id", [&](int st) { send_cb.store(st); }) == 0,
        "ws_send_text 已投递");
  check(pump_client(client, [&] { return got >= 1; }, 5000),
        "客户端收到按 id 发出的文本帧");
  check(last == "via-id", "内容一致");
  check(pump_client(client, [&] { return send_cb.load() == 0; }, 3000),
        "发送完成回调 0");

  // ---- 3：关闭之后 id 失效，发送立刻 -1 --------------------------------
  client.close();
  bool alive_after = true;
  for (int i = 0; i < 500; ++i) {
    if (!app.ws_is_alive(id)) {
      alive_after = false;
      break;
    }
    client.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(!alive_after, "关闭之后 ws_is_alive(id) 变假");

  std::atomic<int> dead_cb(-1);
  const int        dead_rc =
      app.ws_send_text(id, "after-close", [&](int st) { dead_cb.store(st); });
  check(dead_rc == -1, "对已关闭的 id 发送返回 -1");
  check(dead_cb.load() == -1, "回调同步收到 -1");

  // ---- 4：retired 守卫 -------------------------------------------------
  {
    uvcpp_ws_client guard_client;
    bool           guard_connected = false;
    check(guard_client.connect(
              "ws://127.0.0.1:" + std::to_string(port) + "/retire-send",
              [&](uvcpp_ws_connection*, int err) {
                guard_connected = (err == 0);
              }) == 0,
          "retire-send 客户端 connect 调用成功");
    // 服务端在升级回调里 terminate + send；客户端是否握手成功不重要，只等
    // 服务端那两个原子量落地。
    for (int i = 0; i < 500; ++i) {
      if (guard_rc.load() != -999) break;
      guard_client.run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(guard_rc.load() == -1, "terminate 后同一回调里发送返回 -1");
    check(guard_calls.load() == 1, "发送回调恰好一次");
    check(guard_cb.load() == -1, "发送回调收到 -1");
    guard_client.close();
    (void)guard_connected;
  }

  app.stop();
  app.join();

  if (g_fail == 0) {
    std::printf("[ws_alive] ALL PASS\n");
    return 0;
  }
  std::printf("[ws_alive] %d check(s) failed\n", g_fail);
  return 1;
}
