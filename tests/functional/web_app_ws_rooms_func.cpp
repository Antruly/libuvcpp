/**
 * @file tests/functional/web_app_ws_rooms_func.cpp
 * @brief WebSocket 房间与广播（`uvcpp_ws_rooms`）。
 *
 * 判据（全部落在"客户端实际收到了什么"上）：
 *
 *   1. **按房间隔离**：两个客户端进 `roomA`、一个进 `roomB`，向 `roomA`
 *      广播 → 只有 A 的两个人收到；
 *   2. **断线自动退订**：客户端断开后 `room_size()` 回到 0（房间被摘掉）；
 *   3. **慢客户端 · DROP**：单条连接的积压超过 `max_queue_bytes` 时跳过这一条，
 *      其他消息照发；`stats().dropped` 涨、连接还活着；
 *   4. **慢客户端 · CLOSE**：同一情形按策略关连接（1001），`stats().closed_slow`
 *      涨；
 *   5. **成员/房间计数**：`stats()` 与 `room_count()/member_count()` 对得上。
 *
 * 装置：`uvcpp_web_app` 后台线程；HTTP 侧用阻塞 `uvcpp_http_client` 触发广播，
 * WS 侧用**裸 TCP** 做握手与帧解析（需要区分"收到/没收到"，现成的 ws 客户端
 * 没有这个观测点）。慢客户端故意**不读 socket**，让服务端发送积压涨起来。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_WEB_WS_ENABLE

#include <net/uvcpp_tcp_client.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_ws_rooms.h>

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "  [FAIL] " << what << std::endl;
  }
}

long long ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

// ---------------------------------------------------------------------------
// 裸 WebSocket 客户端（只认服务端→客户端方向）
// ---------------------------------------------------------------------------
struct ws_raw {
  uvcpp_tcp_client        c;
  std::string             buf;
  std::vector<std::string> texts;
  bool                    closed = false;

  bool connect(int port) {
    return c.connect_wait("127.0.0.1", port, 3000) == 0;
  }

  bool handshake(const std::string& path) {
    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: localhost\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    req += "Sec-WebSocket-Version: 13\r\n\r\n";
    if (c.write_wait(req.data(), req.size(), 3000) != 0) return false;

    const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    while (buf.find("\r\n\r\n") == std::string::npos) {
      if (ms_since(t0) > 3000) return false;
      uvcpp_buf b;
      if (c.read_wait(b, 50) == 0 && b.size() > 0 &&
          b.get_const_data() != nullptr) {
        buf.append(b.get_const_data(), b.size());
      }
    }
    const size_t p = buf.find("\r\n\r\n") + 4;
    const bool   ok = buf.find("101") != std::string::npos;
    buf.erase(0, p);  // 多读到的字节属于后续帧
    parse();
    return ok;
  }

  /// 解析缓冲里的完整服务端帧（不掩码）。
  void parse() {
    for (;;) {
      if (buf.size() < 2) return;
      const unsigned char b0 = static_cast<unsigned char>(buf[0]);
      const unsigned char b1 = static_cast<unsigned char>(buf[1]);
      const int           op = b0 & 0x0f;
      if ((b1 & 0x80) != 0) {  // 服务端帧不该带掩码
        closed = true;
        buf.clear();
        return;
      }
      uint64_t len = b1 & 0x7f;
      size_t   off = 2;
      if (len == 126) {
        if (buf.size() < 4) return;
        len = (static_cast<unsigned char>(buf[2]) << 8) |
              static_cast<unsigned char>(buf[3]);
        off = 4;
      } else if (len == 127) {
        if (buf.size() < 10) return;
        len = 0;
        for (int i = 0; i < 8; ++i) {
          len = (len << 8) | static_cast<unsigned char>(buf[2 + i]);
        }
        off = 10;
      }
      if (buf.size() < off + len) return;
      const std::string payload = buf.substr(off, static_cast<size_t>(len));
      buf.erase(0, off + static_cast<size_t>(len));
      if (op == 0x1) {
        texts.push_back(payload);
      } else if (op == 0x8) {
        closed = true;
      }
    }
  }

  /// 读一会儿（最多 @p ms），返回是否读到了任何字节。
  bool pump(int ms) {
    const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    bool got = false;
    while (ms_since(t0) < ms) {
      uvcpp_buf b;
      const int rv = c.read_wait(b, 20);
      if (rv == 0 && b.size() > 0 && b.get_const_data() != nullptr) {
        buf.append(b.get_const_data(), b.size());
        got = true;
        parse();
      } else if (rv != 0 && rv != UV_ETIMEDOUT) {
        closed = true;
        break;
      }
    }
    return got;
  }

  void close_socket() { c.close(); }
};

// ---------------------------------------------------------------------------
// 服务端
// ---------------------------------------------------------------------------
struct TestApp {
  uvcpp_ws_rooms rooms;  // 先声明：后于 app 析构（app 停掉后会话不再回调）
  uvcpp_web_app  app;

  void build() {
    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN);

    app.websocket("/chat/:room",
                  [this](uvcpp_web_ws_request& ws_req) {
                    const std::string* room = ws_req.param("room");
                    if (room == nullptr || ws_req.connection() == nullptr) {
                      return;
                    }
                    rooms.join(ws_req.connection(), *room);
                  });

    // GET /bcast/:room?text=... → 按房间广播，body 里回 "delivered=N dropped=M"
    app.get("/bcast/:room",
            [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                   uvcpp_web_next) {
              const std::string* room = req.param("room");
              const std::string* text = req.query("text");
              size_t dropped = 0;
              const size_t delivered =
                  room == nullptr
                      ? 0
                      : rooms.broadcast_text(*room, text ? text->data() : "",
                                             text ? text->size() : 0,
                                             uvcpp_ws_broadcast_options(),
                                             &dropped);
              resp.text(std::string("delivered=") + std::to_string(delivered) +
                        " dropped=" + std::to_string(dropped));
              resp.end();
            });

    // GET /bcast_twice/:room?policy=drop|close&cap=N
    // 一条 200 KiB 的大消息连发两次：第一次把积压顶上去，第二次按策略处置。
    app.get("/bcast_twice/:room",
            [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                   uvcpp_web_next) {
              const std::string* room = req.param("room");
              const std::string* pol  = req.query("policy");
              const std::string* cap  = req.query("cap");
              uvcpp_ws_broadcast_options o;
              o.max_queue_bytes = cap != nullptr
                                      ? static_cast<size_t>(std::strtoul(
                                            cap->c_str(), nullptr, 10))
                                      : 1024;
              o.policy = (pol != nullptr && *pol == "close")
                             ? uvcpp_ws_slow_client::CLOSE
                             : uvcpp_ws_slow_client::DROP;
              const std::string payload(200 * 1024, 'x');
              size_t d1 = 0, d2 = 0;
              const size_t n1 =
                  room ? rooms.broadcast_text(*room, payload.data(),
                                              payload.size(), o, &d1)
                       : 0;
              const size_t n2 =
                  room ? rooms.broadcast_text(*room, payload.data(),
                                              payload.size(), o, &d2)
                       : 0;
              (void)n2;
              resp.text(std::string("delivered=") + std::to_string(n1) +
                        " dropped=" + std::to_string(d1 + d2));
              resp.end();
            });

    app.get("/room_size/:room",
            [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                   uvcpp_web_next) {
              const std::string* room = req.param("room");
              resp.text(std::to_string(room ? rooms.room_size(*room) : 0));
              resp.end();
            });

    app.get("/stats",
            [this](uvcpp_web_request&, uvcpp_web_response& resp,
                   uvcpp_web_next) {
              const uvcpp_ws_room_stats s = rooms.stats();
              resp.text("rooms=" + std::to_string(s.rooms) + " members=" +
                        std::to_string(s.members) + " dropped=" +
                        std::to_string(s.dropped) + " closed_slow=" +
                        std::to_string(s.closed_slow));
              resp.end();
            });
  }

  int start() {
    if (app.start_background() != 0) return -1;
    return app.bound_port();
  }
  void shutdown() {
    app.stop();
    app.join();
  }
};

std::string http_get(int port, const std::string& path) {
  uvcpp_http_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return std::string();
  uvcpp_http_response r;
  if (c.send_wait(uvcpp_http_request::make_get(path), r, 3000) != 0) {
    return std::string();
  }
  return r.body.to_string();
}

bool wait_room_size(int port, const std::string& room, size_t want,
                    int deadline_ms = 3000) {
  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  for (;;) {
    const std::string s = http_get(port, "/room_size/" + room);
    if (s == std::to_string(want)) return true;
    if (ms_since(t0) > deadline_ms) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------
void test_room_isolation_and_cleanup() {
  std::cout << "-- 房间隔离 / 断线退订 --" << std::endl;
  TestApp   t;
  t.build();
  const int port = t.start();
  check(port > 0, "app 起来了");
  if (port <= 0) {
    t.shutdown();
    return;
  }

  ws_raw a1, a2, b;
  check(a1.connect(port) && a1.handshake("/chat/roomA"), "A1 升级成功");
  check(a2.connect(port) && a2.handshake("/chat/roomA"), "A2 升级成功");
  check(b.connect(port) && b.handshake("/chat/roomB"), "B 升级成功");
  check(wait_room_size(port, "roomA", 2), "roomA 有 2 个成员");
  check(wait_room_size(port, "roomB", 1), "roomB 有 1 个成员");

  const std::string r = http_get(port, "/bcast/roomA?text=hi");
  check(r == "delivered=2 dropped=0", "广播回报 delivered=2（实际 " + r + "）");

  a1.pump(500);
  a2.pump(500);
  b.pump(200);
  check(a1.texts.size() == 1 && a1.texts[0] == "hi", "A1 收到 hi");
  check(a2.texts.size() == 1 && a2.texts[0] == "hi", "A2 收到 hi");
  check(b.texts.empty(), "roomB 的 B 没收到（按房间隔离）");

  // 断线 → 自动退订
  a1.close_socket();
  check(wait_room_size(port, "roomA", 1), "A1 断开后 roomA 回到 1 个成员");

  const std::string st = http_get(port, "/stats");
  check(st.find("rooms=2") != std::string::npos, "stats：还剩 2 个房间（" + st + "）");

  a2.close_socket();
  b.close_socket();
  t.shutdown();
}

void test_slow_client_drop() {
  std::cout << "-- 慢客户端：DROP --" << std::endl;
  TestApp   t;
  t.build();
  const int port = t.start();
  if (port <= 0) {
    check(false, "app 起来了");
    t.shutdown();
    return;
  }

  ws_raw s;
  check(s.connect(port) && s.handshake("/chat/slow"), "慢客户端升级成功");

  // 连发两次 200 KiB：第一次入队/在途，第二次应因积压 > cap 被丢弃。
  const std::string r =
      http_get(port, "/bcast_twice/slow?policy=drop&cap=1024");
  check(r.find("dropped=1") != std::string::npos,
        "DROP：第二次被丢弃（实际 " + r + "）");
  const std::string st = http_get(port, "/stats");
  check(st.find("dropped=1") != std::string::npos ||
            st.find("dropped=2") != std::string::npos,
        "stats().dropped 涨了（" + st + "）");
  check(!s.closed, "DROP 不关连接");

  // 把 socket 读空：应当只看到第一条大消息。
  s.pump(1000);
  check(s.texts.size() == 1, "慢客户端只收到第一条（实际 " +
                                 std::to_string(s.texts.size()) + "）");

  s.close_socket();
  t.shutdown();
}

void test_slow_client_close() {
  std::cout << "-- 慢客户端：CLOSE --" << std::endl;
  TestApp   t;
  t.build();
  const int port = t.start();
  if (port <= 0) {
    check(false, "app 起来了");
    t.shutdown();
    return;
  }

  ws_raw s;
  check(s.connect(port) && s.handshake("/chat/slow2"), "慢客户端升级成功");
  const std::string r =
      http_get(port, "/bcast_twice/slow2?policy=close&cap=1024");
  check(r.find("dropped=1") != std::string::npos,
        "CLOSE：第二次被丢弃（实际 " + r + "）");

  s.pump(1500);
  check(s.closed, "连接被服务端关掉（收到 Close 或 EOF）");
  const std::string st = http_get(port, "/stats");
  check(st.find("closed_slow=1") != std::string::npos,
        "stats().closed_slow=1（" + st + "）");

  s.close_socket();
  t.shutdown();
}

}  // namespace

int main() {
  std::cout << "[web_app_ws_rooms] start" << std::endl;
  test_room_isolation_and_cleanup();
  test_slow_client_drop();
  test_slow_client_close();
  if (g_fail == 0) {
    std::cout << "[web_app_ws_rooms] ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << "[web_app_ws_rooms] " << g_fail << " check(s) failed"
            << std::endl;
  return 1;
}

#else  // !(webapp && ws)

int main() {
  std::cout << "SKIP: webapp/ws disabled" << std::endl;
  return 0;
}

#endif
