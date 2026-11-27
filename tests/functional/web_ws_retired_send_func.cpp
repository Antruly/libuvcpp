/**
 * @file tests/functional/web_ws_retired_send_func.cpp
 * @brief `send_data()`/`send_frame_impl()` 的 retired 守卫（issue #87）。
 *
 * 判据：会话 `terminate()` 之后，在**同一个回调里**立刻发一个大帧：
 *
 * 1. `send_binary()` 返回 -1；
 * 2. 完成回调恰好被调一次，且值是 -1。
 *
 * 旧实现先跑 `parser_.compress()` 再判活，这里会白跑一次 64 KiB 级 deflate；
 * 对象若已被属主回收（生产现场），这一步就是 zlib 里的 SIGSEGV。
 */

#include <uvcpp/uvcpp_define.h>

#include <atomic>
#include <chrono>
#include <cstdio>
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

}  // namespace

int main() {
  std::printf("[ws_retired_send] issue #87\n");

  std::atomic<int> guard_rc(-999);
  std::atomic<int> guard_cb(-999);
  std::atomic<int> guard_calls(0);

  uvcpp_web_app app;
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);

  app.websocket("/retire-send", [&](uvcpp_web_ws_request& ws) {
    uvcpp_ws_connection* c = ws.connection();
    c->terminate();
    std::vector<char> big(700 * 1024, 'x');
    guard_rc.store(c->send_binary(big.data(), big.size(), [&](int status) {
      ++guard_calls;
      guard_cb.store(status);
    }));
  });

  check(app.start_background() == 0, "app.start_background");
  const int port = app.bound_port();
  check(port > 0, "拿到端口");

  uvcpp_ws_client client;
  check(client.connect("ws://127.0.0.1:" + std::to_string(port) + "/retire-send",
                       [](uvcpp_ws_connection*, int) {}) == 0,
        "客户端 connect 调用成功");

  for (int i = 0; i < 500; ++i) {
    if (guard_rc.load() != -999) break;
    client.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  check(guard_rc.load() == -1, "terminate 后同一回调里发送返回 -1");
  check(guard_calls.load() == 1, "发送回调恰好一次");
  check(guard_cb.load() == -1, "发送回调收到 -1");

  client.close();
  app.stop();
  app.join();

  if (g_fail == 0) {
    std::printf("[ws_retired_send] ALL PASS\n");
    return 0;
  }
  std::printf("[ws_retired_send] %d check(s) failed\n", g_fail);
  return 1;
}
