/**
 * @file tests/functional/web_app_ws_func.cpp
 * @brief 框架层的 WebSocket 接线：升级按**路径**分派到 WS 路由。
 *
 * 这个文件回答的问题
 * ------------------
 * 协议层（`uvcpp_ws_server`）早就把 101 握手、帧收发、permessage-deflate 都
 * 做完了，各自的用例也都在。缺的一直是**框架这一层**。所以本文件不重测协议
 * （那是 `web_ws_*_func.cpp` 的活），只测"框架有没有把 WS 接起来、接对了没有"：
 *
 *   - 升级请求有没有按**路径**选中正确的处理器；
 *   - 路径参数（`/chat/:room`）有没有真的传进来；
 *   - **没命中**的路径会不会哑掉（应当回落到普通 HTTP 路由，而不是既不升级
 *     也不应答，把连接晾在那儿）；
 *   - 缺 `Sec-WebSocket-Key` 的畸形升级请求会不会哑掉（应当 400）；
 *   - 会话有没有被回收（活动数归零 + 已回收数 +1，两个都要）；
 *   - 升级之后的连接还受不受 HTTP 闲置超时管（**不受**，且对照组要能证明
 *     闲置超时确实在跑）；
 *   - 停机时对端收到的是 Close 帧还是被 RST。
 *
 * 驱动方式
 * --------
 * 这里的服务端跑在 `start_background()` 自己的线程上，测试拨不动它的事件
 * 循环；能拨的只有**客户端**的循环。所以客户端一律用裸 `uvcpp_tcp_client`
 * 加手工拼的线上字节 —— 好处是能逐字节检查 101 的原始报文和线上帧格式。
 * 服务端一侧则完全交给后台线程，测试只通过 `sink`（加锁的观察点）和 App
 * 的计数器（`ws_session_count()` 等）观察。
 *
 * 最后一条用例用**真** `uvcpp_ws_client` 走一遍，证明框架接出来的东西
 * 不是"只有手工字节能连通"。
 *
 * 崩溃定位：每个用例先打名字再跑（`std::unitbuf`），进程中途挂掉也能从最后
 * 一行看出死在哪一条。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include "net/uvcpp_tcp_client.h"
#include "web/uvcpp_ws_client.h"
#include "web/uvcpp_ws_connection.h"
#include "web/uvcpp_ws_frame.h"
#include "web/uvcpp_ws_parser.h"
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_ws.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/** @brief 等后台线程上的 App 满足条件。App 有自己的循环线程，只能等不能拨。 */
bool pump_app(const std::function<bool()>& done, int timeout_ms = 3000) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!done()) {
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count() >= timeout_ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// =========================================================================
// 线上字节：客户端 → 服务端方向必须带掩码（RFC 6455 §5.3）
// =========================================================================

const char kMask[4] = {0x37, static_cast<char>(0xfa), 0x21, 0x3d};

/** @brief 构造一个完整的客户端帧（带掩码、FIN 置位）。 */
std::string masked_frame(int opcode, const std::string& payload) {
  std::string f;
  f.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
  const size_t n = payload.size();
  if (n < 126) {
    f.push_back(static_cast<char>(0x80 | n));
  } else if (n <= 0xFFFF) {
    f.push_back(static_cast<char>(0x80 | 126));
    f.push_back(static_cast<char>((n >> 8) & 0xFF));
    f.push_back(static_cast<char>(n & 0xFF));
  } else {
    f.push_back(static_cast<char>(0x80 | 127));
    for (int i = 0; i < 8; i++) {
      f.push_back(static_cast<char>((n >> (56 - i * 8)) & 0xFF));
    }
  }
  for (int i = 0; i < 4; i++) f.push_back(kMask[i]);
  for (size_t i = 0; i < n; i++) {
    f.push_back(static_cast<char>(
        static_cast<unsigned char>(payload[i]) ^
        static_cast<unsigned char>(kMask[i % 4])));
  }
  return f;
}

std::string masked_text(const std::string& s) {
  return masked_frame(0x1, s);
}

// =========================================================================
// 服务端观察点：处理器跑在 App 的循环线程上，测试读它 —— 全部加锁
// =========================================================================

struct ws_sink {
  std::mutex mu;
  std::vector<std::string> routes;    ///< 每次升级命中的模式串
  std::vector<std::string> rooms;     ///< `param("room")` 取到的值
  std::vector<std::string> texts;     ///< 服务端收到的文本消息
  std::string peer_ip;

  std::atomic<int> upgrades{0};
  std::atomic<int> closes{0};
  std::atomic<int> close_codes{0};

  void note_route(const std::string& r) {
    std::lock_guard<std::mutex> lk(mu);
    routes.push_back(r);
  }
  void note_room(const std::string& r) {
    std::lock_guard<std::mutex> lk(mu);
    rooms.push_back(r);
  }
  void note_text(const std::string& t) {
    std::lock_guard<std::mutex> lk(mu);
    texts.push_back(t);
  }
  void note_peer(const std::string& ip) {
    std::lock_guard<std::mutex> lk(mu);
    peer_ip = ip;
  }
  size_t route_count() {
    std::lock_guard<std::mutex> lk(mu);
    return routes.size();
  }
  std::string route_at(size_t i) {
    std::lock_guard<std::mutex> lk(mu);
    return i < routes.size() ? routes[i] : std::string("<missing>");
  }
  std::string room_at(size_t i) {
    std::lock_guard<std::mutex> lk(mu);
    return i < rooms.size() ? rooms[i] : std::string("<missing>");
  }
};

/**
 * @brief 把一条 WS 路由装到 App 上：记录命中、回显文本、记录关闭码。
 *
 * 这些 handler 都跑在 App 的循环线程上，所以只能碰 `sink`（加锁）和会话本身。
 */
void install_echo(uvcpp_web_app& app, const std::string& pattern, ws_sink* sink,
                  bool record_room) {
  app.websocket(pattern, [sink, record_room](uvcpp_web_ws_request& ws) {
    sink->upgrades.fetch_add(1);
    sink->note_route(ws.route());
    sink->note_peer(ws.peer_ip());
    if (record_room) {
      const std::string* r = ws.param("room");
      sink->note_room(r != nullptr ? *r : std::string("<missing>"));
    }

    uvcpp_ws_connection* conn = ws.connection();
    if (conn == nullptr) return;   // 不该发生；真发生了上面的断言会红

    conn->on_text([sink, conn](const std::string& m) {
      sink->note_text(m);
      conn->send_text(m.c_str(), m.size());
    });
    conn->on_close([sink](ws_close_code code, const std::string&) {
      sink->closes.fetch_add(1);
      sink->close_codes.store(static_cast<int>(code));
    });
  });
}

// =========================================================================
// 客户端探针：裸 TCP + 手工字节，事件循环由测试线程拨
// =========================================================================

struct probe {
  uvcpp_tcp_client tcp;
  uvcpp_ws_parser  parser;

  bool connected = false;
  bool handshake_done = false;
  bool upgraded = false;      ///< 服务端回的是 101
  bool peer_closed = false;   ///< 底层连接被关了
  bool parse_error = false;
  std::string resp;           ///< 非 101 时：完整响应（含 body）
  std::vector<uvcpp_ws_frame> frames;
  const char* where = "";

  bool connect(int port) {
    tcp.set_on_close([this]() { peer_closed = true; });
    if (tcp.connect("127.0.0.1", port,
                    [this](int st) { connected = (st == 0); }) != 0) {
      where = "connect call";
      return false;
    }
    if (!pump([this] { return connected; }, 3000)) {
      where = "connect timeout";
      return false;
    }
    return true;
  }

  /**
   * @brief 装读回调。**必须在连上之后**：socket 还没建立时装了会静默拿不到
   *        任何字节（真客户端的 read_start 也写在写完成回调里）。
   */
  bool start_reading() {
    const int rc = tcp.read_start([this](uvcpp_buf* b) {
      if (!b || b->size() == 0) return;
      const std::string chunk(b->get_const_data(), b->size());
      if (handshake_done) {
        if (upgraded) {
          feed(chunk);
        } else {
          resp += chunk;
        }
        return;
      }
      resp += chunk;
      const size_t e = resp.find("\r\n\r\n");
      if (e == std::string::npos) return;
      handshake_done = true;
      upgraded = (resp.compare(0, 12, "HTTP/1.1 101") == 0);
      if (!upgraded) return;   // 报文体留在 resp 里，不是 WS 帧
      const std::string tail = resp.substr(e + 4);
      resp.resize(e + 4);
      if (!tail.empty()) feed(tail);
    });
    if (rc != 0) {
      where = "read_start";
      return false;
    }
    return true;
  }

  /**
   * @brief 只喂**这一次新到的**字节。
   *
   * 解析器自己留着半帧状态，所以外面**绝不能再存一份半帧缓冲**并整段重喂。
   */
  void feed(const std::string& chunk) {
    const char* d = chunk.data();
    size_t n = chunk.size();
    while (n > 0) {
      const size_t used = parser.execute(d, n);
      const ws_parser_state st = parser.get_state();
      if (st == ws_parser_state::PARSE_ERROR) {
        parse_error = true;
        return;
      }
      if (st != ws_parser_state::COMPLETE) return;
      frames.push_back(parser.get_current_frame());
      parser.reset();
      if (used == 0) return;   // 防御：不前进就退出，绝不空转
      d += used;
      n -= used;
    }
  }

  bool write_bytes(const std::string& b) {
    // 用 shared_ptr 而不是栈上的 bool：写没完成时这个闭包由 TCP 客户端持有，
    // 引用局部变量就是悬垂。
    std::shared_ptr<bool> done(new bool(false));
    if (tcp.write(b.data(), b.size(), [done](int) { *done = true; }) != 0) {
      where = "write call";
      return false;
    }
    if (!pump([done] { return *done; }, 3000)) {
      where = "write timeout";
      return false;
    }
    return true;
  }

  /** @brief 拨自己的循环直到 done() 为真或超时。 */
  bool pump(const std::function<bool()>& done, int timeout_ms) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      tcp.get_loop()->run(UV_RUN_NOWAIT);
      if (done()) return true;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= timeout_ms) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  /** @brief 跑固定时长推进事件（不做断言）。 */
  void settle(int ms) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      tcp.get_loop()->run(UV_RUN_NOWAIT);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= ms) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  /** @brief 发升级请求。`with_key=false` 用来造缺 Key 的畸形请求。 */
  bool upgrade(const std::string& path, bool with_key = true) {
    std::string req = "GET " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n";
    if (with_key) {
      req += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    }
    req += "Sec-WebSocket-Version: 13\r\n\r\n";
    return write_bytes(req);
  }

  bool wait_response(int timeout_ms = 3000) {
    return pump([this] { return handshake_done; }, timeout_ms);
  }

  /** @brief 等第 n 条文本回显（n 从 1 起）。 */
  bool wait_texts(size_t n, int timeout_ms = 3000) {
    return pump([this, n] { return text_count() >= n; }, timeout_ms);
  }

  size_t text_count() const {
    size_t c = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
      if (frames[i].opcode == ws_opcode::TEXT) ++c;
    }
    return c;
  }

  std::string text_at(size_t i) const {
    size_t seen = 0;
    for (size_t k = 0; k < frames.size(); ++k) {
      if (frames[k].opcode != ws_opcode::TEXT) continue;
      if (seen == i) return frames[k].payload.to_string();
      ++seen;
    }
    return std::string("<missing>");
  }

  /** @brief 收到的 Close 帧的码；没有 Close 帧时返回 -1。 */
  int close_frame_code() const {
    for (size_t i = 0; i < frames.size(); ++i) {
      if (frames[i].opcode == ws_opcode::CLOSE) {
        return static_cast<int>(frames[i].get_close_code());
      }
    }
    return -1;
  }

  int status_code() const {
    if (resp.size() < 12) return -1;
    return std::atoi(resp.c_str() + 9);
  }

  /** @brief 报头里的一个头（名字大小写不敏感）。只在 `\r\n\r\n` 之前找。 */
  std::string header(const char* name) const {
    const size_t head_end = resp.find("\r\n\r\n");
    const size_t limit = head_end == std::string::npos ? resp.size() : head_end;
    const size_t nlen = std::strlen(name);
    size_t pos = resp.find("\r\n");
    if (pos == std::string::npos || pos >= limit) return std::string();
    pos += 2;
    while (pos < limit) {
      size_t eol = resp.find("\r\n", pos);
      if (eol == std::string::npos || eol > limit) eol = limit;
      const size_t colon = resp.find(':', pos);
      if (colon != std::string::npos && colon < eol && colon - pos == nlen) {
        bool eq = true;
        for (size_t i = 0; i < nlen; ++i) {
          char a = resp[pos + i];
          char b = name[i];
          if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
          if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
          if (a != b) { eq = false; break; }
        }
        if (eq) {
          size_t v = colon + 1;
          while (v < eol && (resp[v] == ' ' || resp[v] == '\t')) ++v;
          return resp.substr(v, eol - v);
        }
      }
      pos = eol + 2;
    }
    return std::string();
  }

  std::string body() const {
    const size_t e = resp.find("\r\n\r\n");
    return e == std::string::npos ? std::string() : resp.substr(e + 4);
  }

  /** @brief 裸 TCP 关闭：一个 WS 字节都不发（用来造 1006）。 */
  void close_transport() {
    tcp.get_tcp()->close();
    settle(30);
  }
};

/** @brief 裸 TCP，只连上、什么都不发（用来验证闲置超时）。 */
struct quiet_conn {
  uvcpp_tcp_client tcp;
  bool connected = false;
  bool peer_closed = false;
  uvcpp_loop* loop = nullptr;

  bool open(int port, int timeout_ms = 3000) {
    tcp.set_on_close([this]() { peer_closed = true; });
    if (tcp.connect("127.0.0.1", port,
                    [this](int st) { connected = (st == 0); }) != 0) {
      return false;
    }
    loop = tcp.get_loop();
    const auto t0 = std::chrono::steady_clock::now();
    while (!connected) {
      loop->run(UV_RUN_NOWAIT);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= timeout_ms) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // **必须装读回调**：对端关闭这件事就报在读回调的 `nread < 0` 分支上。
    // 不装的话这条连接收不到任何通知，`peer_closed` 永远是 false ——
    // 那正好会让本用例的对照组变成"我什么都没等到，所以通过"。
    tcp.read_start([](uvcpp_buf*) {});
    return true;
  }

  void pump(int ms) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      loop->run(UV_RUN_NOWAIT);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= ms) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
};

/** @brief 起一个 App（端口 0），失败时记账并返回 0 —— 让调用方早退。 */
int start_app(uvcpp_web_app& app) {
  if (app.start_background() != 0) {
    check(false, "start_background() 应当成功");
    return 0;
  }
  const int port = app.bound_port();
  check(port > 0, "端口 0 之后 bound_port() 是真端口");
  return port;
}

// =========================================================================
// 1. 路径分派：升级打到已注册的 WS 路由 → 101 + 消息往返
// =========================================================================
void test_route_dispatch() {
  std::shared_ptr<ws_sink> sink(new ws_sink());
  uvcpp_web_app app;
  app.set_port(0);
  install_echo(app, "/chat", sink.get(), /*record_room=*/false);

  const int port = start_app(app);
  if (port > 0) {
    probe p;
    const bool ok = p.connect(port) && p.start_reading();
    check(ok, std::string("裸客户端准备（") + (ok ? "ok" : p.where) + "）");
    if (ok) {
      check(p.upgrade("/chat"), "升级请求写得出去");
      check(p.wait_response(), "在超时前收到响应");
      check(p.upgraded, "响应是 101 Switching Protocols（实际状态 " +
                            std::to_string(p.status_code()) + "）");
      check(p.header("upgrade") == "websocket", "101 带 Upgrade: websocket");
      check(p.header("connection") == "Upgrade", "101 带 Connection: Upgrade");
      // `Sec-WebSocket-Accept` 是 RFC 6455 §4.2.2 的定值 —— key 固定时它也是
      // 固定的，所以这里能直接断言真值，而不是"非空就行"。
      check(p.header("sec-websocket-accept") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
            "Sec-WebSocket-Accept 与 RFC 6455 的示例一致（实际 '" +
                p.header("sec-websocket-accept") + "'）");

      check(p.write_bytes(masked_text("hello")), "发得出第一条消息");
      check(p.wait_texts(1), "收到回显");
      check(p.text_at(0) == "hello",
            "回显内容一致（实际 '" + p.text_at(0) + "'）");

      // 第二条：证明连接不是「只处理一帧」。
      check(p.write_bytes(masked_text("world")), "发得出第二条消息");
      check(p.wait_texts(2), "收到第二条回显");
      check(p.text_at(1) == "world", "第二条回显内容一致");

      check(pump_app([&] { return sink->upgrades.load() == 1; }, 2000),
            "处理器恰好被调用一次");
      check(sink->route_count() == 1 && sink->route_at(0) == "/chat",
            "处理器拿到的 route() 是 '/chat'（实际 '" + sink->route_at(0) +
                "'）");
      check(!sink->peer_ip.empty(), "处理器拿到了对端 IP");
    }
    p.close_transport();
  }

  app.stop();
  app.join();
}

// =========================================================================
// 2. 路径参数：/chat/:room → handler 里 param("room") 拿到真值
// =========================================================================
void test_path_params() {
  std::shared_ptr<ws_sink> sink(new ws_sink());
  uvcpp_web_app app;
  app.set_port(0);
  install_echo(app, "/chat/:room", sink.get(), /*record_room=*/true);

  const int port = start_app(app);
  if (port > 0) {
    probe p;
    const bool ok = p.connect(port) && p.start_reading();
    check(ok, std::string("裸客户端准备（") + (ok ? "ok" : p.where) + "）");
    if (ok) {
      check(p.upgrade("/chat/lobby"), "升级到 /chat/lobby");
      check(p.wait_response() && p.upgraded, "收到 101");
      check(pump_app([&] { return sink->upgrades.load() == 1; }, 2000),
            "处理器被调用");
      check(sink->room_at(0) == "lobby",
            "param(\"room\") = 'lobby'（实际 '" + sink->room_at(0) + "'）");
      check(sink->route_count() == 1 && sink->route_at(0) == "/chat/:room",
            "route() 是带参数的模式串（实际 '" + sink->route_at(0) + "'）");
      check(p.write_bytes(masked_text("hi")), "发消息");
      check(p.wait_texts(1), "收到回显");
    }
    p.close_transport();
  }

  app.stop();
  app.join();
}

// =========================================================================
// 3. 没命中的路径必须**回落**成普通 HTTP 请求，不能哑掉
// =========================================================================
void test_unknown_path_falls_through() {
  std::shared_ptr<ws_sink> sink(new ws_sink());
  std::atomic<int> hits(0);
  uvcpp_web_app app;
  app.set_port(0);
  install_echo(app, "/chat", sink.get(), false);
  // **对照组**：同一条路径上注册一条普通 GET 路由。没有它的话，一个
  // "见到升级请求就回 404"的实现也能让主断言全绿。
  app.get("/plain", [&hits](uvcpp_web_request&, uvcpp_web_response& resp,
                            uvcpp_web_next) {
    hits.fetch_add(1);
    resp.text("plain-ok");
    resp.end();
  });

  const int port = start_app(app);
  if (port > 0) {
    {
      probe p;
      const bool ok = p.connect(port) && p.start_reading();
      check(ok, std::string("裸客户端准备（") + (ok ? "ok" : p.where) + "）");
      if (ok) {
        check(p.upgrade("/nope"), "升级到没注册的 /nope");
        check(p.wait_response(), "**必须**有响应（不能哑在那里）");
        check(!p.upgraded, "没有升级成 WS");
        check(p.status_code() == 404, "回的是 404（实际 " +
                                          std::to_string(p.status_code()) +
                                          "）");
        check(sink->upgrades.load() == 0, "没命中时不该调用任何 WS 处理器");
      }
      p.close_transport();
    }

    {
      probe p;
      const bool ok = p.connect(port) && p.start_reading();
      check(ok, std::string("裸客户端准备（") + (ok ? "ok" : p.where) + "）");
      if (ok) {
        check(p.upgrade("/plain"), "升级请求打到已注册的普通 GET 路由");
        check(p.wait_response(), "**必须**有响应");
        check(!p.upgraded, "没有升级成 WS");
        check(p.status_code() == 200,
              "走的是普通 GET 路由，回 200（实际 " +
                  std::to_string(p.status_code()) + "）");
        check(p.body() == "plain-ok",
              "body 是 GET 处理器的输出（实际 '" + p.body() + "'）");
        check(hits.load() == 1, "GET 处理器被调用一次");
      }
      p.close_transport();
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 4. 缺 Sec-WebSocket-Key 的升级请求 → 400，不能哑掉
// =========================================================================
void test_bad_handshake() {
  std::shared_ptr<ws_sink> sink(new ws_sink());
  uvcpp_web_app app;
  app.set_port(0);
  install_echo(app, "/chat", sink.get(), false);

  const int port = start_app(app);
  if (port > 0) {
    probe p;
    const bool ok = p.connect(port) && p.start_reading();
    check(ok, std::string("裸客户端准备（") + (ok ? "ok" : p.where) + "）");
    if (ok) {
      check(p.upgrade("/chat", /*with_key=*/false), "发一个缺 Key 的升级请求");
      check(p.wait_response(),
            "**必须**有响应（协议层对缺 Key 是静默 return 的，框架这一层"
            "必须自己拦下来）");
      check(!p.upgraded, "没有升级成 WS");
      check(p.status_code() == 400, "回的是 400（实际 " +
                                        std::to_string(p.status_code()) + "）");
      check(sink->upgrades.load() == 0, "缺 Key 时不该调用 WS 处理器");
    }
    p.close_transport();
  }

  app.stop();
  app.join();
}

// =========================================================================
// 5. 一次 websocket() 都不调 → 升级请求走普通路由 → 404
// =========================================================================
void test_no_ws_registered() {
  uvcpp_web_app app;
  app.set_port(0);
  app.get("/x", [](uvcpp_web_request&, uvcpp_web_response& resp,
                   uvcpp_web_next) {
    resp.text("x");
    resp.end();
  });

  const int port = start_app(app);
  if (port > 0) {
    check(!app.wss_enabled(), "没注册过 WS 路由时 wss_enabled() 为假");
    check(app.ws_session_count() == 0, "没有会话");

    probe p;
    const bool ok = p.connect(port) && p.start_reading();
    check(ok, std::string("裸客户端准备（") + (ok ? "ok" : p.where) + "）");
    if (ok) {
      check(p.upgrade("/x"), "升级请求打到 /x");
      check(p.wait_response(), "有响应");
      check(!p.upgraded, "没有升级");
      check(p.status_code() == 200, "走普通路由回 200（实际 " +
                                        std::to_string(p.status_code()) + "）");
      check(p.body() == "x", "body 正确");
    }
    p.close_transport();
  }

  app.stop();
  app.join();
}

// =========================================================================
// 6. 对端关闭 → 会话归零 + 已回收数恰好 +1（两个都要）
// =========================================================================
void test_session_recycled() {
  std::shared_ptr<ws_sink> sink(new ws_sink());
  uvcpp_web_app app;
  app.set_port(0);
  install_echo(app, "/chat", sink.get(), false);

  const int port = start_app(app);
  if (port > 0) {
    const size_t recycled_before = app.ws_recycled_session_count();

    probe p;
    const bool ok = p.connect(port) && p.start_reading();
    check(ok, std::string("裸客户端准备（") + (ok ? "ok" : p.where) + "）");
    if (ok) {
      check(p.upgrade("/chat"), "升级");
      check(p.wait_response() && p.upgraded, "101");
      check(pump_app([&] { return app.ws_session_count() == 1; }, 2000),
            "升级之后恰好 1 个活动会话");
      check(p.write_bytes(masked_text("ping")), "发一条消息");
      check(p.wait_texts(1), "收到回显");

      // 对端（客户端）主动断开，且**一个 Close 字节都不发**。
      p.close_transport();

      check(pump_app([&] { return app.ws_session_count() == 0; }, 3000),
            "对端断开后活动会话归零");
      check(pump_app([&] {
              return app.ws_recycled_session_count() == recycled_before + 1;
            }, 3000),
            "已回收会话数恰好 +1（实际增量 " +
                std::to_string(app.ws_recycled_session_count() -
                               recycled_before) + "）");
      check(pump_app([&] { return sink->closes.load() == 1; }, 2000),
            "on_close 恰好回调一次");
      check(sink->close_codes.load() ==
                static_cast<int>(ws_close_code::ABNORMAL_CLOSE),
            "对端没发 Close 帧就断 → 1006（实际 " +
                std::to_string(sink->close_codes.load()) + "）");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 7. 升级之后不受 HTTP 闲置超时约束（**带对照组**）
// =========================================================================
void test_idle_timeout_exempt() {
  std::shared_ptr<ws_sink> sink(new ws_sink());
  uvcpp_web_app app;
  app.set_port(0);
  app.set_idle_timeout_ms(300);
  install_echo(app, "/chat", sink.get(), false);

  const int port = start_app(app);
  if (port > 0) {
    // 两条连接同时安静待着：一条普通 HTTP（对照组），一条已升级的 WS。
    quiet_conn plain;
    check(plain.open(port), "对照组：普通连接连上");

    bool ws_live = false;
    probe p;
    const bool ok = p.connect(port) && p.start_reading();
    check(ok, std::string("WS 客户端准备（") + (ok ? "ok" : p.where) + "）");
    if (ok) {
      check(p.upgrade("/chat"), "升级");
      check(p.wait_response() && p.upgraded, "101");
      check(pump_app([&] { return app.ws_session_count() == 1; }, 2000),
            "WS 会话已登记");
      ws_live = p.upgraded;
    }

    // 等够久：闲置超时 300ms、扫描间隔 200ms，1500ms 足够扫好几轮。
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      plain.loop->run(UV_RUN_NOWAIT);
      if (ok) p.tcp.get_loop()->run(UV_RUN_NOWAIT);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= 1500) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // **对照组必须红得起来**：它证明闲置超时确实在跑。少了这一条，一个
    // "idle_sweep 根本没在跑"的实现能让下面那条断言照样绿。
    check(plain.peer_closed,
          "对照组：安静的**普通**连接被闲置超时关掉了"
          "（否则本用例什么都没证明）");

    if (ws_live) {
      check(!p.peer_closed, "升级后的 WS 连接**没有**被闲置超时关掉");
      check(app.ws_session_count() == 1, "WS 会话仍然活着");
      // 行为证据，不只是标志位：还能收发。
      check(p.write_bytes(masked_text("still-here")), "WS 连接还能写");
      check(p.wait_texts(1),
            "WS 连接还能收到回显（说明真的活着，不是在自说自话）");
      check(p.text_at(0) == "still-here", "回显内容正确");
    }

    p.close_transport();
    plain.pump(30);
  }

  app.stop();
  app.join();
}

// =========================================================================
// 8. 停机时有活动会话 → 对端收到 Close 帧（1001），不是被 RST
// =========================================================================
void test_app_shutdown_closes_sessions() {
  std::shared_ptr<ws_sink> sink(new ws_sink());
  uvcpp_web_app app;
  app.set_port(0);
  install_echo(app, "/chat", sink.get(), false);

  const int port = start_app(app);
  if (port > 0) {
    probe p;
    const bool ok = p.connect(port) && p.start_reading();
    check(ok, std::string("裸客户端准备（") + (ok ? "ok" : p.where) + "）");
    if (ok) {
      check(p.upgrade("/chat"), "升级");
      check(p.wait_response() && p.upgraded, "101");
      check(pump_app([&] { return app.ws_session_count() == 1; }, 2000),
            "有 1 个活动会话，停机时确实有东西要关");

      app.stop();

      // 停机流程跑在 App 的线程上，本端要同时拨自己的循环才收得到帧。
      const auto t0 = std::chrono::steady_clock::now();
      for (;;) {
        p.tcp.get_loop()->run(UV_RUN_NOWAIT);
        if (p.close_frame_code() >= 0 || p.peer_closed) break;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count() >= 3000) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      check(p.close_frame_code() >= 0,
            "对端收到的是 **Close 帧**，不是连接被直接断掉"
            "（RFC 6455 §7.1.4 的优雅关闭）");
      check(p.close_frame_code() == static_cast<int>(ws_close_code::GOING_AWAY),
            "Close 码是 1001 GOING_AWAY（实际 " +
                std::to_string(p.close_frame_code()) + "）");
      check(!p.parse_error, "帧格式合法（没有解析错误）");
    }
    p.close_transport();
  }

  app.join();
}

// =========================================================================
// 9. 真 uvcpp_ws_client 往返 —— 证明接出来的东西不是"只有手工字节能连通"
// =========================================================================
void test_real_client_roundtrip() {
  std::shared_ptr<ws_sink> sink(new ws_sink());
  uvcpp_web_app app;
  app.set_port(0);
  install_echo(app, "/echo", sink.get(), false);

  const int port = start_app(app);
  if (port > 0) {
    uvcpp_ws_client client;
    uvcpp_ws_connection* conn = nullptr;
    bool connected = false;
    int got = 0;
    std::string last;

    // 客户端的事件循环由测试线程自己拨（`client.run()`），所以下面这些
    // 捕获变量全部只在**本线程**被读写，不需要加锁。
    const int rc = client.connect(
        "ws://127.0.0.1:" + std::to_string(port) + "/echo",
        [&](uvcpp_ws_connection* c, int err) {
          if (err != 0 || c == nullptr) return;
          conn = c;
          connected = true;
          c->on_text([&](const std::string& m) {
            last = m;
            ++got;
          });
          c->send_text("from-real-client", 16);
        });
    check(rc == 0, "uvcpp_ws_client::connect 调用成功");

    const auto t0 = std::chrono::steady_clock::now();
    while (got < 1) {
      client.run(UV_RUN_NOWAIT);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= 5000) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    check(connected, "真 uvcpp_ws_client 连上并完成升级");
    check(got == 1,
          "真客户端收到了回显（收到 " + std::to_string(got) + " 条）");
    check(last == "from-real-client", "回显内容一致（实际 '" + last + "'）");
    check(pump_app([&] { return sink->upgrades.load() == 1; }, 2000),
          "服务端处理器被调用");
    check(sink->route_count() == 1 && sink->route_at(0) == "/echo",
          "命中的是 /echo（实际 '" + sink->route_at(0) + "'）");

    if (conn != nullptr) conn->close();
    for (int i = 0; i < 100; ++i) {
      client.run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  app.stop();
  app.join();
}

struct test_case {
  const char* name;
  void (*fn)();
};

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;

  std::string only;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
      only = argv[++i];
    }
  }

  const test_case tests[] = {
      {"route_dispatch", test_route_dispatch},
      {"path_params", test_path_params},
      {"unknown_path_falls_through", test_unknown_path_falls_through},
      {"bad_handshake", test_bad_handshake},
      {"no_ws_registered", test_no_ws_registered},
      {"session_recycled", test_session_recycled},
      {"idle_timeout_exempt", test_idle_timeout_exempt},
      {"app_shutdown_closes_sessions", test_app_shutdown_closes_sessions},
      {"real_client_roundtrip", test_real_client_roundtrip},
  };
  const int count = static_cast<int>(sizeof(tests) / sizeof(tests[0]));

  int ran = 0;
  for (int i = 0; i < count; ++i) {
    if (!only.empty() && only != tests[i].name) continue;
    ++ran;
    std::cout << "[" << tests[i].name << "] " << std::flush;
    const int before = g_failures;
    tests[i].fn();
    std::cout << (g_failures == before ? "PASS" : "FAIL") << std::endl;
  }

  if (ran == 0) {
    std::cerr << "[web_app_ws] --only " << only << " 没匹配到任何用例"
              << std::endl;
    return 2;
  }

  if (g_failures == 0) {
    std::cout << "[web_app_ws] ALL PASS (" << ran << " cases)" << std::endl;
    return 0;
  }
  std::cout << "[web_app_ws] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}

#else

#include <iostream>

int main() {
  std::cerr << "[web_app_ws] UVCPP_WEBAPP_ENABLE=0 —— 构建配置有问题，"
               "这个测试文件不该被编译进来" << std::endl;
  return 2;
}

#endif  // UVCPP_WEBAPP_ENABLE
