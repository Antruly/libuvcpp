/**
 * @file tests/functional/web_app_ws_bus_redis_func.cpp
 * @brief 跨进程 WS 广播桥（Redis pub/sub，issue #74）。
 *
 * 装置：本机真 Redis（`UVCPP_REDIS_TEST_URL`，与 `redis_real_func` 同一变量；
 * 缺省时 SKIP，`UVCPP_REDIS_TEST_REQUIRE=1` 则直接失败）。多个 `uvcpp_ws_bus`
 * 实例挂在**同一条循环**上，模拟多个进程 —— 它们只通过 Redis 说话。
 *
 * 判据：
 *
 * 1. **跨节点**：A 发布 `room1` → B 收到，房间名与 payload 原样；
 * 2. **不回环**：A 自己收不到自己发的（`loopback_skipped` 涨）；打开
 *    `deliver_loopback` 的 C 能收到自己的；
 * 3. **二进制安全**：payload 里的 `\0` 与换行原样到达；
 * 4. **降级**：连不上 Redis 的实例 `publish()` 仍返回 0（进有界队列），
 *    队列满返 `UV_ENOBUFS`；本地 `broadcast()` 那一半照常；
 * 5. **重连**：`drop_connection()` 之后自动重连并**重新订阅**，再发布 B 仍能收到；
 * 6. **统计**：published/received/delivered/loopback_skipped/reconnects 对得上。
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <uv.h>

#ifndef _WIN32
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_REDIS_ENABLE

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_signal.h>
#include <handle/uvcpp_timer.h>
#include <webapp/uvcpp_ws_bus.h>
#include <webapp/uvcpp_ws_rooms.h>

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

bool pump_until(uvcpp_loop& loop, const std::function<bool()>& done, int ms) {
  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  while (!done()) {
    loop.run(UV_RUN_NOWAIT);
    if (done()) return true;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count() >= ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

struct received_msg {
  std::string room;
  std::string payload;
};

uvcpp_ws_bus_options bus_opts(const std::string& node,
                              const std::string& prefix) {
  uvcpp_ws_bus_options o;
  o.node_id        = node;
  o.channel_prefix = prefix;
  o.reconnect_ms   = 50;
  o.reconnect_max_ms = 200;
  return o;
}

bool wait_connected(uvcpp_loop& loop, uvcpp_ws_bus& bus, uvcpp_ws_bus& other,
                    int ms = 8000) {
  return pump_until(
      loop, [&] { return bus.connected() && other.connected(); }, ms);
}


#ifndef _WIN32

std::string self_exe() {
  char buf[4096];
  std::size_t size = sizeof(buf);
  if (uv_exepath(buf, &size) != 0) return std::string();
  return std::string(buf, size);
}

std::string read_file(const std::string& path) {
  std::string out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return out;
  char buf[512];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

bool file_has(const std::string& path, const std::string& needle) {
  return read_file(path).find(needle) != std::string::npos;
}

pid_t spawn_child_node(const std::string& exe, const std::string& node,
                       const std::string& prefix, const std::string& room_out,
                       const std::string& result, const std::string& ready) {
  const pid_t pid = ::fork();
  if (pid != 0) return pid;
  ::execl(exe.c_str(), exe.c_str(), "--child-node", node.c_str(),
          prefix.c_str(), room_out.c_str(), result.c_str(), ready.c_str(),
          static_cast<char*>(nullptr));
  ::_exit(127);
}

bool wait_exit(pid_t pid, int timeout_ms, int& status) {
  const int steps = timeout_ms / 50;
  for (int i = 0; i < steps; ++i) {
    if (::waitpid(pid, &status, WNOHANG) == pid) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

// ---------------------------------------------------------------------------
// 子进程：只做"另一个节点"该做的事 —— 连 Redis、收到谁的消息就记一行
// ---------------------------------------------------------------------------

int child_node_main(int argc, char** argv) {
  if (argc < 7) return 2;
  const std::string node     = argv[2];
  const std::string prefix   = argv[3];
  const std::string room_out = argv[4];
  const std::string result   = argv[5];
  const std::string ready    = argv[6];

  const char* url = std::getenv("UVCPP_REDIS_TEST_URL");
  if (url == nullptr || *url == '\0') return 3;

  uvcpp_loop loop;
  uvcpp_ws_bus bus(&loop, bus_opts(node, prefix));

  FILE* f = std::fopen(result.c_str(), "ab");
  if (f == nullptr) return 4;
  bus.set_on_message([f](const std::string& room, const std::string& payload) {
    std::fprintf(f, "%s\t%s\n", room.c_str(), payload.c_str());
    std::fflush(f);
  });
  bus.start(url);

  bool announced = false;
  uvcpp_signal sig(&loop);
  sig.start(
      [&loop](uvcpp_signal*, int) { loop.stop(); }, SIGTERM);

  // 连上（发布与订阅两条都就绪）之后才写 ready、才发第一条 —— 父进程拿
  // ready 文件当"这个节点真的在听"的判据。
  uvcpp_timer tick(&loop);
  tick.start(
      [&](uvcpp_timer*) {
        if (announced || !bus.connected()) return;
        announced = true;
        FILE* r = std::fopen(ready.c_str(), "wb");
        if (r != nullptr) {
          std::fprintf(r, "%ld\n", static_cast<long>(uv_os_getpid()));
          std::fclose(r);
        }
        bus.publish(room_out, "child-to-parent");
      },
      20, 20);

  loop.run(UV_RUN_DEFAULT);
  tick.stop();
  sig.stop();
  bus.stop();
  for (int i = 0; i < 16; ++i) loop.run(UV_RUN_NOWAIT);
  std::fclose(f);
  return 0;
}

#endif  // !_WIN32

}  // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
  if (argc > 1 && std::strcmp(argv[1], "--child-node") == 0) {
    return child_node_main(argc, argv);
  }
#endif
  const char* url = std::getenv("UVCPP_REDIS_TEST_URL");
  const char* require = std::getenv("UVCPP_REDIS_TEST_REQUIRE");
  const bool  required = require != nullptr && std::strcmp(require, "1") == 0;
  if (url == nullptr || *url == '\0') {
    if (required) {
      std::printf("[ws_bus] FAIL: REQUIRE=1 但没有 UVCPP_REDIS_TEST_URL\n");
      return 1;
    }
    std::printf("SKIP: 没有 UVCPP_REDIS_TEST_URL\n");
    return 3;  // ctest 的 SKIP_RETURN_CODE
  }

  std::printf("[ws_bus] issue #74\n");

  uvcpp_loop loop;
  const std::string prefix =
      "uvcpp:test:" + std::to_string(static_cast<long>(uv_os_getpid())) + ":";

  uvcpp_ws_bus a(&loop, bus_opts("node-a", prefix));
  uvcpp_ws_bus b(&loop, bus_opts("node-b", prefix));

  uvcpp_ws_bus loopback(&loop, [&] {
    uvcpp_ws_bus_options o = bus_opts("node-loop", prefix);
    o.deliver_loopback = true;
    return o;
  }());

  std::vector<received_msg> b_got;
  std::vector<received_msg> loop_got;
  std::vector<received_msg> a_got;
  b.set_on_message([&](const std::string& room, const std::string& payload) {
    received_msg m;
    m.room    = room;
    m.payload = payload;
    b_got.push_back(m);
  });
  a.set_on_message([&](const std::string& room, const std::string& payload) {
    received_msg m;
    m.room    = room;
    m.payload = payload;
    a_got.push_back(m);
  });
  loopback.set_on_message([&](const std::string& room,
                              const std::string& payload) {
    received_msg m;
    m.room    = room;
    m.payload = payload;
    loop_got.push_back(m);
  });

  check(a.start(url) == 0, "A.start");
  check(b.start(url) == 0, "B.start");
  check(loopback.start(url) == 0, "loopback.start");
  check(a.start(url) == 0, "A.start 幂等");
  check(wait_connected(loop, a, b), "A / B 都连上并订阅");
  check(pump_until(loop, [&] { return loopback.connected(); }, 8000),
        "loopback 实例连上");

  // ---- 1/2：跨节点 + 不回环 ------------------------------------------
  check(a.publish("room1", "hello") == 0, "A 发布 room1");
  check(pump_until(loop, [&] { return !b_got.empty(); }, 5000),
        "B 收到 A 的广播");
  if (!b_got.empty()) {
    check(b_got[0].room == "room1", "房间名 = room1");
    check(b_got[0].payload == "hello", "payload = hello");
  }
  // 给 A 的回环消息一点时间（它本来就不该回调）。
  pump_until(loop, [] { return false; }, 300);
  check(a.snapshot().loopback_skipped >= 1, "A 跳过了自己的回环");
  check(a.snapshot().delivered == 0, "A 的 on_message 一次都没被自己的消息叫醒");

  // ---- 3：二进制安全 --------------------------------------------------
  const std::string bin("a\0b\r\nc", 7);
  check(a.publish("room1", bin) == 0, "A 发布二进制 payload");
  check(pump_until(loop, [&] { return b_got.size() >= 2; }, 5000),
        "B 收到第二条");
  if (b_got.size() >= 2) {
    check(b_got[1].payload == bin, "payload 的 NUL / CRLF 原样保留");
  }

  // ---- loopback 实例收到自己的 ---------------------------------------
  check(loopback.publish("roomL", "self") == 0, "loopback 实例发布");
  // 这个实例订阅的是同一个前缀，所以 A 之前那两条它也会收到 —— 要按房间找，
  // 不能看 `loop_got[0]`。
  check(pump_until(
            loop,
            [&] {
              for (size_t i = 0; i < loop_got.size(); ++i) {
                if (loop_got[i].room == "roomL" &&
                    loop_got[i].payload == "self") {
                  return true;
                }
              }
              return false;
            },
            5000),
        "deliver_loopback=true 时能收到自己发的");
  if (!loop_got.empty()) {
    bool found = false;
    for (size_t i = 0; i < loop_got.size(); ++i) {
      if (loop_got[i].room == "roomL" && loop_got[i].payload == "self") {
        found = true;
      }
    }
    check(found, "回环消息内容正确");
  }

  // ---- 4：降级（坏 URL） ---------------------------------------------
  {
    uvcpp_ws_bus bad(&loop, bus_opts("node-bad", prefix));
    bad.start("redis://127.0.0.1:1/0");  // 必定连不上
    pump_until(loop, [] { return false; }, 200);
    check(!bad.connected(), "坏 URL 的实例不在 connected 状态");
    check(bad.publish("roomX", "queued-1") == 0, "降级时 publish 仍受理（排队）");
    check(bad.snapshot().degraded, "stats.degraded 为真");
    // 队列上限：max_pending=1，再发两条应当先排队后拒绝。
    uvcpp_ws_bus_options o = bus_opts("node-bad2", prefix);
    o.max_pending = 1;
    uvcpp_ws_bus bad2(&loop, o);
    bad2.start("redis://127.0.0.1:1/0");
    pump_until(loop, [] { return false; }, 100);
    check(bad2.publish("roomX", "q1") == 0, "队列未满时受理");
    check(bad2.publish("roomX", "q2") == UV_ENOBUFS,
          "队列满时 publish 返 UV_ENOBUFS");
    check(bad2.snapshot().dropped >= 1, "dropped 计数涨");
    bad.stop();
    bad2.stop();
  }

  // ---- 5：重连并重新订阅 ---------------------------------------------
  b_got.clear();
  a.drop_connection();
  check(!a.connected(), "drop 之后 A 不在 connected");
  check(pump_until(loop, [&] { return a.connected(); }, 8000),
        "A 自动重连成功");
  pump_until(loop, [&] { return a.snapshot().reconnects >= 1; }, 3000);
  check(a.snapshot().reconnects >= 1, "reconnects 计数涨");
  check(a.publish("room2", "after-reconnect") == 0, "重连后发布");
  check(pump_until(
            loop,
            [&] {
              for (size_t i = 0; i < b_got.size(); ++i) {
                if (b_got[i].room == "room2" &&
                    b_got[i].payload == "after-reconnect") {
                  return true;
                }
              }
              return false;
            },
            8000),
        "重新订阅生效：B 收到重连后的消息");

  // ---- 6：broadcast() 本地 + 远端 ------------------------------------
  {
    uvcpp_ws_rooms rooms;
    b_got.clear();
    const size_t local =
        a.broadcast(rooms, "room3", "via-broadcast", std::strlen("via-broadcast"));
    check(local == 0, "本地没有成员时 delivered=0");
    check(pump_until(
              loop,
              [&] {
                for (size_t i = 0; i < b_got.size(); ++i) {
                  if (b_got[i].room == "room3" &&
                      b_got[i].payload == "via-broadcast") {
                    return true;
                  }
                }
                return false;
              },
              5000),
          "broadcast() 的远端那一半也送到了 B");
  }

  // ---- 7：真两进程（fork + exec）---------------------------------------
#ifndef _WIN32
  {
    const std::string exe = self_exe();
    check(!exe.empty(), "拿到自己的可执行文件路径（两进程用例）");
    const std::string base =
        "/tmp/uvcpp_ws_bus_" +
        std::to_string(static_cast<long>(uv_os_getpid()));
    const std::string result = base + ".result";
    const std::string ready  = base + ".ready";
    ::unlink(result.c_str());
    ::unlink(ready.c_str());

    const pid_t child =
        spawn_child_node(exe, "node-child-proc", prefix, "room-child-out",
                         result, ready);
    check(child > 0, "子进程节点已拉起");
    check(pump_until(loop, [&] { return file_has(ready, "\n"); }, 10000),
          "子进程节点连上 Redis（ready 文件出现）");

    // 方向 1：子 → 父。父进程这条集合里只有 A 收了 on_message。
    check(pump_until(
              loop,
              [&] {
                for (std::size_t i = 0; i < a_got.size(); ++i) {
                  if (a_got[i].room == "room-child-out" &&
                      a_got[i].payload == "child-to-parent") {
                    return true;
                  }
                }
                return false;
              },
              8000),
          "跨进程：父进程收到子进程的广播");

    // 方向 2：父 → 子（子进程把收到的每一行写进 result 文件）。
    check(a.publish("room-parent-out", "parent-to-child") == 0,
          "父进程发布给子进程");
    check(pump_until(
              loop,
              [&] { return file_has(result, "room-parent-out\tparent-to-child"); },
              8000),
          "跨进程：子进程收到父进程的广播");

    ::kill(child, SIGTERM);
    int st = 0;
    const bool done = wait_exit(child, 8000, st);
    check(done, "子进程节点收到 SIGTERM 后退出");
    if (done) {
      check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "子进程退出码 0");
    } else {
      ::kill(child, SIGKILL);
      ::waitpid(child, &st, 0);
    }
    ::unlink(result.c_str());
    ::unlink(ready.c_str());
  }
#else
  std::printf("  [skip] 两进程用例在 Windows 上不跑（没有 fork/exec）\n");
#endif

  // 统计自洽
  check(b.snapshot().received >= b.snapshot().delivered,
        "B: received >= delivered");
  check(a.snapshot().published >= 3, "A: published 计数");

  loopback.stop();
  b.stop();
  a.stop();
  // 让 close 回调跑完
  pump_until(loop, [] { return false; }, 100);

  if (g_fail == 0) {
    std::printf("[ws_bus] ALL PASS\n");
    return 0;
  }
  std::printf("[ws_bus] %d check(s) failed\n", g_fail);
  return 1;
}

#else  // !(webapp && redis)

int main() {
  std::printf("SKIP: webapp / redis disabled\n");
  return 0;
}

#endif
