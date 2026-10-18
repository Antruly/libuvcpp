/**
 * @file tests/functional/redis_real_func.cpp
 * @brief 对真实 `redis-server` 的用例：SET/GET/INCR/LPUSH/LRANGE/发布订阅。
 *
 * `redis_client_func.cpp` 用假服务端钉时序；这一条钉的是"真服务器说的方言
 * 我们到底读没读懂" —— 假服务端里的应答字节是我们自己写的，只能证明解析器
 * 与写它的人想法一致。
 *
 * 地址来自 `UVCPP_REDIS_TEST_URL`，默认 `redis://127.0.0.1:6379/0`。连不上
 * 返回 **3**（ctest 的 `SKIP_RETURN_CODE`，报成 `***Skipped`）—— 这台机器没
 * 起服务器，不能长得像"测过了"。CI 那一格的 legs 起了 redis-server 并设
 * `UVCPP_REDIS_TEST_REQUIRE=1`：那时连不上是**失败**，不是跳过。
 *
 * 写进服务器的键都带 `uvcpp:test:<时间戳>:` 前缀，末尾统一 DEL；不碰别的键。
 */

#include <redis/uvcpp_redis.h>

#include <handle/uvcpp_loop.h>

#include <uv.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++g_fail;
    std::cout << "[redis_real] FAIL: " << what << "\n";
  }
}

bool pump_until(uvcpp::uvcpp_loop* loop, const std::function<bool()>& done,
                int timeout_ms) {
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  while (!done()) {
    loop->run(UV_RUN_NOWAIT);
    if (done()) break;
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count() > timeout_ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

}  // namespace

int main() {
  const char* env_url = std::getenv("UVCPP_REDIS_TEST_URL");
  const char* env_require = std::getenv("UVCPP_REDIS_TEST_REQUIRE");
  const std::string url = env_url != nullptr && env_url[0] != '\0'
                              ? std::string(env_url)
                              : std::string("redis://127.0.0.1:6379/0");
  const bool require = env_require != nullptr && std::string(env_require) == "1";

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_redis_client c(&loop);
  c.set_connect_timeout_ms(3000);

  const int rc = c.connect_wait(url, 3000);
  if (rc != 0) {
    if (require) {
      std::cout << "[redis_real] FAIL: UVCPP_REDIS_TEST_REQUIRE=1 but connect to "
                << url << " failed: " << c.last_error() << "\n";
      return 1;
    }
    std::cout << "[redis_real] SKIPPED: no redis-server at " << url << " ("
              << c.last_error() << ")\n";
    return 3;
  }

  char prefix_buf[64];
  std::snprintf(prefix_buf, sizeof(prefix_buf), "uvcpp:test:%lld:",
                static_cast<long long>(std::time(nullptr)));
  const std::string prefix(prefix_buf);

  const std::string k_str = prefix + "str";
  const std::string k_ctr = prefix + "ctr";
  const std::string k_list = prefix + "list";
  const std::string k_bin = prefix + "bin";
  const std::string k_ch = prefix + "chan";

  uvcpp::uvcpp_redis_value v;

  // ---- SET / GET / 空应答 ----
  check(c.command_wait({"SET", k_str, "hello"}, v, 3000) == 0 &&
            v.type() == uvcpp::uvcpp_redis_type::STATUS &&
            v.as_string() == "OK",
        "SET returns +OK");
  check(c.command_wait({"GET", k_str}, v, 3000) == 0 &&
            v.as_string() == "hello",
        "GET returns what SET wrote");
  check(c.command_wait({"GET", prefix + "nope"}, v, 3000) == 0 && v.is_null(),
        "GET of a missing key is null");

  // ---- INCR ----
  check(c.command_wait({"INCR", k_ctr}, v, 3000) == 0 &&
            v.as_integer() == 1,
        "INCR starts at 1");
  check(c.command_wait({"INCR", k_ctr}, v, 3000) == 0 &&
            v.as_integer() == 2,
        "INCR goes to 2");

  // ---- LPUSH / LRANGE ----
  check(c.command_wait({"DEL", k_list}, v, 3000) == 0, "DEL list");
  check(c.command_wait({"LPUSH", k_list, "a", "b", "c"}, v, 3000) == 0 &&
            v.as_integer() == 3,
        "LPUSH returns the list length");
  check(c.command_wait({"LRANGE", k_list, "0", "-1"}, v, 3000) == 0 &&
            v.type() == uvcpp::uvcpp_redis_type::ARRAY && v.size() == 3 &&
            v.at(0).as_string() == "c" && v.at(1).as_string() == "b" &&
            v.at(2).as_string() == "a",
        "LRANGE returns the list in push order (newest first)");

  // ---- 错误应答：status 仍是 0 ----
  //
  // 判据要求 `INCR` 打在**列表**上：打在非数字**字符串**上 Redis 给的是
  // `ERR value is not an integer or out of range`，不是 WRONGTYPE —— 这条
  // 一开始写错了，是这台机器上真跑出来的。
  check(c.command_wait({"INCR", k_list}, v, 3000) == 0 && v.is_error() &&
            v.as_string().find("WRONGTYPE") == 0,
        "INCR on a list is a WRONGTYPE error reply");

  // ---- 二进制安全 ----
  const std::string bin("a\0b\r\nc", 7);
  check(c.command_wait({"SET", k_bin, bin}, v, 3000) == 0, "SET binary");
  check(c.command_wait({"GET", k_bin}, v, 3000) == 0 && v.as_string() == bin,
        "GET binary round-trips");

  // ---- pipeline ----
  std::vector<std::vector<std::string>> pipe;
  pipe.push_back(std::vector<std::string>{"INCR", k_ctr});
  pipe.push_back(std::vector<std::string>{"INCR", k_ctr});
  std::vector<uvcpp::uvcpp_redis_value> out;
  check(c.pipeline_wait(pipe, out, 3000) == 0 && out.size() == 2 &&
            out[0].as_integer() == 3 && out[1].as_integer() == 4,
        "pipeline keeps order");

  // ---- PUBLISH / SUBSCRIBE ----
  uvcpp::uvcpp_redis_client sub(&loop);
  uvcpp::uvcpp_redis_client pub(&loop);
  check(sub.connect_wait(url, 3000) == 0, "subscribe client connects");
  check(pub.connect_wait(url, 3000) == 0, "publish client connects");

  int sub_ack = -1;
  int published = -1;
  std::string got_channel;
  std::string got_message;

  check(sub.subscribe({k_ch},
                      [&](const std::string& channel,
                          const uvcpp::uvcpp_redis_value& m) {
                        got_channel = channel;
                        got_message = m.as_string();
                      },
                      [&](int st) { sub_ack = st; }) == 0,
        "SUBSCRIBE accepted");

  // 等订阅确认之后才发布：订阅是异步的，抢在确认前发布会让 PUBLISH 返回 0。
  const bool acked = pump_until(&loop, [&]() { return sub_ack >= 0; }, 3000);
  check(acked && sub_ack == 0, "subscribe acknowledged");

  check(pub.command({"PUBLISH", k_ch, "hi"},
                    [&](int st, const uvcpp::uvcpp_redis_value& r) {
                      published = st == 0 ? static_cast<int>(r.as_integer())
                                          : -st;
                    }) == 0,
        "PUBLISH accepted");

  check(pump_until(&loop,
                   [&]() { return published >= 0 && !got_message.empty(); },
                   3000),
        "publish and message both arrive");
  check(published == 1, "PUBLISH reaches the one subscriber");
  check(got_channel == k_ch && got_message == "hi",
        "message carries the right channel and payload");

  // ---- 清理 ----
  c.command_wait({"DEL", k_str}, v, 3000);
  c.command_wait({"DEL", k_ctr}, v, 3000);
  c.command_wait({"DEL", k_list}, v, 3000);
  c.command_wait({"DEL", k_bin}, v, 3000);

  sub.close();
  pub.close();
  c.close();
  pump_until(&loop, []() { return false; }, 20);

  if (g_fail != 0) {
    std::cout << "[redis_real] " << g_fail << " check(s) FAILED\n";
    return 1;
  }
  std::cout << "[redis_real] ALL PASSED\n";
  return 0;
}
