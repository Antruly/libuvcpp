/**
 * @file tests/functional/web_ws_reassembly_func.cpp
 * @brief WebSocket 消息重组 + 协议错误 + 聚合上限 + 发送队列。
 *
 * 为什么单独一个文件、且**不依赖压缩**：重组的 bug 会被压缩的用例盖住
 * （压缩要求"先重组再解压"，所以压缩用例跑通时重组必然是对的，反过来不成立）。
 * 这里全部用手工构造的线上字节驱动一个真实的 `uvcpp_ws_connection`，
 * 走真 TCP、真端口、真事件循环，不经过 HTTP 升级（升级握手与重组无关）。
 *
 * 客户端方向发出的帧按 RFC 6455 §5.3 带掩码（掩码位 + 固定 mask key），
 * 服务端方向（本端回复）不带掩码 —— 与真实客户端行为一致。
 */
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include "net/uvcpp_tcp_server.h"
#include "net/uvcpp_tcp_client.h"
#include "web/uvcpp_ws_connection.h"
#include "web/uvcpp_ws_parser.h"
#include "web/uvcpp_ws_sessions.h"

using namespace uvcpp;

// =========================================================================
// 线上字节构造
// =========================================================================

static const char k_mask[4] = {0x37, (char)0xfa, 0x21, 0x3d};

static void put16(std::string& s, uint16_t v) {
  s.push_back(static_cast<char>((v >> 8) & 0xFF));
  s.push_back(static_cast<char>(v & 0xFF));
}

/**
 * @brief 构造一个完整的线上帧（客户端方向 = 带掩码）。
 * @param opcode  0x1/TEXT、0x2/BINARY、0x0/CONTINUATION、0x8/CLOSE、0x9/PING、0xA/PONG
 * @param fin     是否消息末帧
 * @param rsv     置位 RSV1(0x40) / RSV2(0x20) / RSV3(0x10)
 */
static std::string raw_frame(int opcode, bool fin, const std::string& payload,
                             bool mask = true, unsigned char rsv = 0,
                             const char* mask_key = k_mask) {
  std::string f;
  unsigned char b0 = static_cast<unsigned char>(opcode & 0x0F);
  if (fin) b0 |= 0x80;
  b0 |= rsv;
  f.push_back(static_cast<char>(b0));

  const unsigned char mbit = mask ? 0x80 : 0x00;
  const size_t n = payload.size();
  if (n < 126) {
    f.push_back(static_cast<char>(mbit | n));
  } else if (n <= 0xFFFF) {
    f.push_back(static_cast<char>(mbit | 126));
    put16(f, static_cast<uint16_t>(n));
  } else {
    f.push_back(static_cast<char>(mbit | 127));
    for (int i = 0; i < 8; i++) f.push_back(static_cast<char>((n >> (56 - i * 8)) & 0xFF));
  }

  if (mask) {
    for (int i = 0; i < 4; i++) f.push_back(mask_key[i]);
    for (size_t i = 0; i < n; i++) {
      f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^
                                    static_cast<unsigned char>(mask_key[i % 4])));
    }
  } else {
    f += payload;
  }
  return f;
}

/**
 * @brief 只发一个"长度头"，后面不带任何数据。
 *
 * 用来验证单帧上限是在**长度字段解析完成的那一刻**就被拦住的 —— 如果实现
 * 等到数据到齐才判，那么"声明 8 EB"就能让对端一直占着内存，而这个用例会
 * 因为永远等不到数据而超时失败。
 */
static std::string raw_frame_header_only(uint64_t declared_len, int opcode = 0x1) {
  std::string f;
  f.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));   // FIN + opcode
  if (declared_len < 126) {
    f.push_back(static_cast<char>(declared_len));
  } else if (declared_len <= 0xFFFF) {
    f.push_back(static_cast<char>(126));
    put16(f, static_cast<uint16_t>(declared_len));
  } else {
    f.push_back(static_cast<char>(127));
    for (int i = 0; i < 8; i++) {
      f.push_back(static_cast<char>((declared_len >> (56 - i * 8)) & 0xFF));
    }
  }
  return f;   // 不带掩码位、不带数据
}

// =========================================================================
// 场景：一个 server + 一个 raw client，服务端那侧套上 uvcpp_ws_connection
// =========================================================================

struct scenario {
  uvcpp_tcp_server server;
  uvcpp_tcp_client client;
  /** @brief 会话属主。建会话的一方 `adopt()` 之后不再自己 `delete`。 */
  uvcpp_ws_sessions sessions_;
  uvcpp_ws_connection* conn = nullptr;
  int  port = 0;
  bool connected = false;

  /**
   * @brief 会话必须死在循环之前。
   *
   * `server` / `client` 各持一个循环，而会话注册的接收回调捕获了本场景的
   * `this`。属主正常路径上就是在关循环之前 `shutdown()`（`~uvcpp_ws_server`
   * 同一形状）；`recycle_all()` 是**当场**终结 + 删除，所以析构体里这一句
   * 之后会话已经没了，后面成员析构里的有界泵不可能再回调到已死的场景。
   */
  ~scenario() { sessions_.shutdown(); }

  // 服务端观测到的事件
  std::vector<std::string>              texts;
  std::vector<std::string>              bins;
  std::vector<size_t>                   pings;
  std::vector<int>                      errors;
  std::vector<int>                      closes;
  std::vector<std::string>              error_reasons;

  // 客户端侧解析本端发出的帧（服务端→客户端不带掩码）
  uvcpp_ws_parser            cparser;
  std::vector<uvcpp_ws_frame> cframes;

  bool start(size_t max_msg = 0) {
    if (server.bindIpv4("127.0.0.1", 0) != 0) return false;
    struct sockaddr_in name;
    int nl = static_cast<int>(sizeof(name));
    server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &nl);
    port = ntohs(name.sin_port);

    int rc = server.listen([this, max_msg](uvcpp_tcp_client* c) {
      // 走框架的所有权约定（`~uvcpp_ws_server::handle_upgrade` 同一形状）：
      // 建会话的一方 `adopt()`，回收交给属主，自己不再 `delete`。
      // `set_loop` 必须在循环线程、循环正跑着的时候调（`uv_async_init` 不是
      // 线程安全的），而这里正是接入回调，两个条件都满足。
      sessions_.set_loop(server.get_loop());
      auto* wc = new uvcpp_ws_connection(c, ws_role::SERVER);
      sessions_.adopt(wc);
      if (max_msg > 0) wc->set_max_message_size(max_msg);
      wc->on_text([this](const std::string& m) { texts.push_back(m); });
      wc->on_binary([this](const uint8_t* d, size_t n) {
        bins.push_back(std::string(reinterpret_cast<const char*>(d), n));
      });
      wc->on_ping([this](const uint8_t*, size_t n) { pings.push_back(n); });
      wc->on_error([this](int code, const std::string& r) {
        errors.push_back(code);
        error_reasons.push_back(r);
      });
      wc->on_close([this](ws_close_code cd, const std::string&) {
        closes.push_back(static_cast<int>(cd));
      });
      conn = wc;
      wc->start();
    }, 128);
    if (rc != 0) return false;

    rc = client.connect("127.0.0.1", port, [this](int st) { connected = (st == 0); });
    if (rc != 0) return false;
    return pump_until([this] { return connected && conn != nullptr; }, 3000);
  }

  bool pump_until(const std::function<bool()>& done, int timeout_ms) {
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      server.get_loop()->run(UV_RUN_NOWAIT);
      client.get_loop()->run(UV_RUN_NOWAIT);
      if (done()) return true;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= timeout_ms) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  /** @brief 跑固定时长，让事件充分投递（不做断言，只推进）。 */
  void settle(int ms) {
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      server.get_loop()->run(UV_RUN_NOWAIT);
      client.get_loop()->run(UV_RUN_NOWAIT);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= ms) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  /** @brief 客户端发一段字节并等写完成（一次一个异步写，不能并发）。 */
  bool send(const std::string& bytes) {
    bool done = false;
    int rc = client.write(bytes.data(), bytes.size(), [&done](int) { done = true; });
    if (rc != 0) return false;
    return pump_until([&done] { return done; }, 3000);
  }

  /** @brief 取出客户端侧已解析到的帧（服务端→客户端方向）。 */
  void drain_client_frames() {
    client.read_start([this](uvcpp_buf* b) {
      if (!b || b->size() == 0) return;
      const char* d = b->get_const_data();
      size_t n = b->size();
      // 与连接层同样的"喂 → 看状态 → reset → 继续喂"循环（解析器一次只吃一帧）
      while (n > 0) {
        size_t used = cparser.execute(d, n);
        if (cparser.get_state() == ws_parser_state::COMPLETE) {
          cframes.push_back(cparser.get_current_frame());
          cparser.reset();
          if (used == 0) break;
          d += used;
          n -= used;
          continue;
        }
        break;
      }
    });
  }
};

// =========================================================================
// 用例
// =========================================================================

// 1. 单帧往返：重组改造之后最基本的路径不能坏
static bool test_single_frame() {
  scenario s;
  if (!s.start()) return false;
  if (!s.send(raw_frame(0x1, true, "hello"))) return false;
  s.pump_until([&] { return !s.texts.empty(); }, 2000);
  return s.texts.size() == 1 && s.texts[0] == "hello";
}

// 2. 一个写里塞两帧 —— 解析器一次只吃一帧，连接层必须自己 reset 并接着喂
static bool test_two_frames_one_write() {
  scenario s;
  if (!s.start()) return false;
  std::string both = raw_frame(0x1, true, "first") + raw_frame(0x1, true, "second");
  if (!s.send(both)) return false;
  s.pump_until([&] { return s.texts.size() >= 2; }, 2000);
  return s.texts.size() == 2 && s.texts[0] == "first" && s.texts[1] == "second";
}

// 3. 分成多个写、按顺序到达（跨 TCP 读回调边界）
static bool test_sequential_frames() {
  scenario s;
  if (!s.start()) return false;
  const char* words[] = {"alpha", "beta", "gamma", "delta"};
  for (int i = 0; i < 4; i++) {
    if (!s.send(raw_frame(0x1, true, words[i]))) return false;
  }
  s.pump_until([&] { return s.texts.size() >= 4; }, 2000);
  if (s.texts.size() != 4) return false;
  for (int i = 0; i < 4; i++) {
    if (s.texts[i] != words[i]) return false;
  }
  return true;
}

// 4. 分片文本：三段拼成一条消息，**只回调一次**
static bool test_fragmented_text() {
  scenario s;
  if (!s.start()) return false;
  std::string all = raw_frame(0x1, false, "Hel") +
                    raw_frame(0x0, false, "lo, ") +
                    raw_frame(0x0, true,  "world");
  if (!s.send(all)) return false;
  s.pump_until([&] { return !s.texts.empty(); }, 2000);
  // 关键：不是 3 次回调、也不是只有第一段
  return s.texts.size() == 1 && s.texts[0] == "Hello, world";
}

// 5. 分片二进制
static bool test_fragmented_binary() {
  scenario s;
  if (!s.start()) return false;
  std::string p1("\x00\x01\x02", 3), p2("\x03\x04", 2);
  std::string all = raw_frame(0x2, false, p1) + raw_frame(0x0, true, p2);
  if (!s.send(all)) return false;
  s.pump_until([&] { return !s.bins.empty(); }, 2000);
  return s.bins.size() == 1 &&
         s.bins[0] == std::string("\x00\x01\x02\x03\x04", 5);
}

// 6. 控制帧穿插在分片中间：可以穿插、不参与重组、不打断消息
static bool test_control_interleaved() {
  scenario s;
  if (!s.start()) return false;
  std::string all = raw_frame(0x1, false, "frag") +
                    raw_frame(0x9, true,  "ping!") +     // PING 插在中间
                    raw_frame(0x0, true,  "mented");
  if (!s.send(all)) return false;
  s.pump_until([&] { return !s.texts.empty() && !s.pings.empty(); }, 2000);
  return s.texts.size() == 1 && s.texts[0] == "fragmented" &&
         s.pings.size() == 1 && s.pings[0] == 5;
}

// 7. 没有消息在途却收到 CONTINUATION → 1002
static bool test_continuation_without_start() {
  scenario s;
  if (!s.start()) return false;
  if (!s.send(raw_frame(0x0, true, "orphan"))) return false;
  s.pump_until([&] { return !s.errors.empty(); }, 2000);
  return s.errors.size() == 1 &&
         s.errors[0] == static_cast<int>(ws_close_code::PROTOCOL_ERROR) &&
         s.texts.empty();
}

// 8. 分片未结束就插入新的数据帧 → 1002
static bool test_new_data_frame_mid_message() {
  scenario s;
  if (!s.start()) return false;
  std::string all = raw_frame(0x1, false, "part1") +
                    raw_frame(0x1, true,  "sneaky");   // 应该出现在 CONTINUATION 位置
  if (!s.send(all)) return false;
  s.pump_until([&] { return !s.errors.empty(); }, 2000);
  return s.errors.size() == 1 &&
         s.errors[0] == static_cast<int>(ws_close_code::PROTOCOL_ERROR) &&
         s.texts.empty();
}

// 9. RSV2/RSV3 未协商却置位 → 1002
static bool test_rsv2_rejected() {
  scenario s;
  if (!s.start()) return false;
  if (!s.send(raw_frame(0x1, true, "x", true, 0x20))) return false;   // RSV2
  s.pump_until([&] { return !s.errors.empty(); }, 2000);
  return s.errors.size() == 1 &&
         s.errors[0] == static_cast<int>(ws_close_code::PROTOCOL_ERROR) &&
         s.texts.empty();
}

// 10. RSV1（压缩标志）但没有协商过 permessage-deflate → 1002
static bool test_rsv1_without_negotiation() {
  scenario s;
  if (!s.start()) return false;
  if (!s.send(raw_frame(0x1, true, "x", true, 0x40))) return false;   // RSV1
  s.pump_until([&] { return !s.errors.empty(); }, 2000);
  return s.errors.size() == 1 &&
         s.errors[0] == static_cast<int>(ws_close_code::PROTOCOL_ERROR) &&
         s.texts.empty();
}

// 11. 聚合上限：每一片都合规，合起来超限 → 1009
//     （只卡单帧的实现会在这里放过去 —— 这正是分片绕过单帧限制的手法）
static bool test_aggregate_cap() {
  scenario s;
  if (!s.start(100)) return false;                      // 消息上限 100 字节
  std::string chunk(40, 'a');
  std::string all = raw_frame(0x1, false, chunk) +      // 40
                    raw_frame(0x0, false, chunk) +      // 80
                    raw_frame(0x0, true,  chunk);       // 120 > 100
  if (!s.send(all)) return false;
  s.pump_until([&] { return !s.errors.empty(); }, 2000);
  return s.errors.size() == 1 &&
         s.errors[0] == static_cast<int>(ws_close_code::MESSAGE_TOO_BIG) &&
         s.texts.empty();
}

// 12. 单帧上限：**只发长度头**。长度一确定就该被拦，不用等数据。
static bool test_frame_cap() {
  scenario s;
  if (!s.start(1000)) return false;
  if (!s.send(raw_frame_header_only(0x00000000000FFFFFULL))) return false;   // 声明 1 MiB
  s.pump_until([&] { return !s.errors.empty(); }, 2000);
  return s.errors.size() == 1 &&
         s.errors[0] == static_cast<int>(ws_close_code::PROTOCOL_ERROR) &&
         s.texts.empty();
}

// 13. 发送队列：一个回调里连发多条，必须**一条不少、顺序不变**
//     （原先在同一时刻只允许一个异步写，第二条会被 UV_EALREADY 静默丢掉）
static bool test_send_queue_order() {
  scenario s;
  if (!s.start()) return false;
  s.drain_client_frames();

  // 服务端收到一条消息就回 5 条 —— 全部在同一个回调里背靠背发出
  if (s.conn) {
    // 用 on_text 已经在 start() 里注册过了，这里改成"收到就回 5 条"
    s.conn->on_text([&s](const std::string&) {
      for (int i = 0; i < 5; i++) {
        std::string m = "msg" + std::to_string(i);
        s.conn->send_text(m.c_str(), m.size());
      }
    });
  }
  if (!s.send(raw_frame(0x1, true, "go"))) return false;
  s.pump_until([&] { return s.cframes.size() >= 5; }, 3000);

  if (s.cframes.size() != 5) return false;
  for (int i = 0; i < 5; i++) {
    std::string want = "msg" + std::to_string(i);
    if (s.cframes[i].opcode != ws_opcode::TEXT) return false;
    if (s.cframes[i].payload.to_string() != want) return false;
  }
  return true;
}

// 14. Close 原因超长时截断：控制帧负载上限 125 字节，
//     超了会拼出一个对端**必须拒绝**的帧
static bool test_close_reason_truncated() {
  scenario s;
  if (!s.start()) return false;
  s.drain_client_frames();
  std::string long_reason(400, 'r');
  if (s.conn) s.conn->send_close(ws_close_code::NORMAL, long_reason);
  s.pump_until([&] { return !s.cframes.empty(); }, 2000);
  if (s.cframes.empty()) return false;
  const uvcpp_ws_frame& f = s.cframes[0];
  if (f.opcode != ws_opcode::CLOSE) return false;
  // 状态码 2 字节 + 原因 ≤ 123 字节 = 控制帧负载 ≤ 125
  if (f.payload.size() > 125) return false;
  if (f.get_close_code() != ws_close_code::NORMAL) return false;
  return f.get_close_reason().size() == 123;
}

// =========================================================================
// 驱动
// =========================================================================

// =========================================================================
// 掩码的方向性与文本帧的 UTF-8 —— 两条 RFC 6455 的 MUST
//
// 这两条以前都没实现：服务端把**未掩码**的客户端帧当正常消息收下（§5.1 要求
// 以 1002 关连接）；文本帧里的**非法 UTF-8** 被原样交给 on_text（§8.1 要求
// 关连接）。两条都不会崩、不会报错，只是"照单全收"，所以能一直躺着。
//
// 补上之后必须有用例盯着：否则哪天动了 on_ws_frame 里的判断顺序，它们会静悄悄
// 回到老样子，而 CI 一片绿。
// =========================================================================

/** @brief 服务端收到未掩码的客户端帧：§5.1 要求以 1002 关闭。 */
static bool test_unmasked_frame_rejected() {
  scenario s;
  if (!s.start()) return false;
  // mask=false：客户端**必须**掩码（§5.1），这里故意违反
  if (!s.send(raw_frame(0x1, true, "unmasked", false))) return false;
  s.pump_until([&] { return !s.errors.empty(); }, 2000);
  return s.errors.size() == 1 &&
         s.errors[0] == static_cast<int>(ws_close_code::PROTOCOL_ERROR) &&
         s.texts.empty();               // 不该被当成正常消息交付
}

/** @brief 文本帧的负载不是合法 UTF-8：§8.1 要求关连接（1007）。 */
static bool test_invalid_utf8_rejected() {
  scenario s;
  if (!s.start()) return false;
  std::string bad;                      // 0xFF 0xFE 0xFD 不是合法 UTF-8
  bad.push_back(static_cast<char>(0xFF));
  bad.push_back(static_cast<char>(0xFE));
  bad.push_back(static_cast<char>(0xFD));
  if (!s.send(raw_frame(0x1, true, bad))) return false;
  s.pump_until([&] { return !s.errors.empty(); }, 2000);
  return s.errors.size() == 1 &&
         s.errors[0] == static_cast<int>(ws_close_code::INVALID_PAYLOAD) &&
         s.texts.empty();
}

/**
 * @brief 反向对照：**合法**的多字节 UTF-8 必须照常交付。
 *
 * 补 UTF-8 校验最容易的错法是规则写严了 —— 于是中文、emoji 全被拒掉，而一般
 * 用例只发 ASCII，根本发现不了。这条同时覆盖三字节与四字节两种长度。
 */
static bool test_valid_utf8_still_delivered() {
  scenario s;
  if (!s.start()) return false;
  std::string ok;
  ok.push_back(static_cast<char>(0xE4));   // 「中」= U+4E2D，三字节
  ok.push_back(static_cast<char>(0xB8));
  ok.push_back(static_cast<char>(0xAD));
  ok.push_back(static_cast<char>(0xF0));   // 😀 = U+1F600，四字节
  ok.push_back(static_cast<char>(0x9F));
  ok.push_back(static_cast<char>(0x98));
  ok.push_back(static_cast<char>(0x80));
  if (!s.send(raw_frame(0x1, true, ok))) return false;
  s.pump_until([&] { return !s.texts.empty() || !s.errors.empty(); }, 2000);
  return s.errors.empty() && s.texts.size() == 1 && s.texts[0] == ok;
}

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"single_frame",              test_single_frame},
    {"two_frames_one_write",      test_two_frames_one_write},
    {"sequential_frames",         test_sequential_frames},
    {"fragmented_text",           test_fragmented_text},
    {"fragmented_binary",         test_fragmented_binary},
    {"control_interleaved",       test_control_interleaved},
    {"continuation_without_start",test_continuation_without_start},
    {"new_data_frame_mid_message",test_new_data_frame_mid_message},
    {"rsv2_rejected",             test_rsv2_rejected},
    {"rsv1_without_negotiation",  test_rsv1_without_negotiation},
    {"aggregate_cap",             test_aggregate_cap},
    {"frame_cap",                 test_frame_cap},
    {"send_queue_order",          test_send_queue_order},
    {"close_reason_truncated",    test_close_reason_truncated},
    // 两条 RFC 6455 MUST + 一条反向对照（见上面的说明）
    {"unmasked_frame_rejected",   test_unmasked_frame_rejected},
    {"invalid_utf8_rejected",     test_invalid_utf8_rejected},
    {"valid_utf8_still_delivered",test_valid_utf8_still_delivered},
  };
  for (const auto& t : tests) {
    std::cout << "[web_ws_reasm] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_ws_reasm] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  // 这里曾经是 `std::_Exit(...)`：那时"每个用例都建了一对 loop、正常析构会卡住"
  // 是仓库的已知问题，所以断言出完就直接退出、跳过收尾。现在每个 `scenario`
  // 都是栈上对象、各自析构里做有界收尾泵（第十四批），没有要跳的东西了 ——
  // `_Exit` 留着只会让 main 的正常返回路径永远不被走到。
  return ok ? 0 : 2;
}
#else
int main() {
  std::cout << "[web_ws_reasm] SKIP (web disabled)\n";
  return 0;
}
#endif
