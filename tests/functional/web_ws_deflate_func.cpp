/**
 * @file tests/functional/web_ws_deflate_func.cpp
 * @brief permessage-deflate 的**接线**测试：101 里的协商 → 连接层真的压缩。
 *
 * 分工（三层各管一段，别在这里重复别人已经钉死的东西）：
 *
 * | 层 | 在哪测 | 管什么 |
 * |---|---|---|
 * | 帧的线格式（剥尾、补尾、BFINAL、窗口位数方向） | `web_compress_func.cpp` | 与真 zlib 的互操作 |
 * | 协商策略表（哪些参数该回、回多少） | `web_ws_ext_func.cpp` | RFC 7692 §7.1.2 的纯函数 |
 * | **接线**（协商结果有没有真的到达压缩器） | 本文件 | 升级握手 + 连接层收发 |
 *
 * 所以本文件不重写一个按 RFC 手写的参照 zlib 实现 —— 那是上面两层的活。
 * 本文件要回答的是：`uvcpp_ws_server` 到底有没有把 101 里谈定的参数交给
 * `uvcpp_ws_connection`，以及连接层有没有真的按它压缩/解压。
 *
 * ## 一个刻意选出来的判据
 *
 * 手工构造线上字节、再用本库自己的解析器解开，只能证明"自洽"，证明不了
 * "协商结果真的生效了"：zlib 的解码侧对窗口位数不做校验（详见
 * `web_compress_func.cpp` 里那段实测记录），所以把窗口位数配错**解压照样成功**。
 *
 * `server_no_context_takeover` 是少数**确定性可观测**的协商结果：
 * - 协商成 no_context_takeover → 服务端每压一条就 `deflateReset`，于是两条
 *   **完全相同**的消息压出来**逐字节相同**；
 * - 协商成 context takeover → 第二条能整段回溯引用第一条，压出来**必然不同**。
 *
 * 同一份消息、同一个服务端，只改这一个开关，产物的相等/不等关系就翻转 ——
 * 这就把"协商结果有没有真的传到压缩器"变成了一个确定的断言，而不是靠
 * "能解开就行"的模糊信心。`server_context_takeover_reaches_compressor` 用的
 * 就是这个判据（两个方向都断言，缺一半的话"永远不压缩"的实现也能过）。
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

#if UVCPP_WEB_ENABLE && UVCPP_ZLIB_ENABLE

#include "net/uvcpp_tcp_client.h"
#include "net/uvcpp_tcp_server.h"
#include "web/uvcpp_ws_client.h"
#include "web/uvcpp_ws_connection.h"
#include "web/uvcpp_ws_ext.h"
#include "web/uvcpp_ws_frame.h"
#include "web/uvcpp_ws_parser.h"
#include "web/uvcpp_ws_server.h"

using namespace uvcpp;

// =========================================================================
// 线上字节
// =========================================================================

static const char k_mask[4] = {0x37, (char)0xfa, 0x21, 0x3d};

/** @brief 构造一个完整的线上帧（客户端方向默认带掩码，RFC 6455 §5.3）。 */
static std::string raw_frame(int opcode, bool fin, const std::string& payload,
                             bool mask, unsigned char rsv) {
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
    f.push_back(static_cast<char>((n >> 8) & 0xFF));
    f.push_back(static_cast<char>(n & 0xFF));
  } else {
    f.push_back(static_cast<char>(mbit | 127));
    for (int i = 0; i < 8; i++) {
      f.push_back(static_cast<char>((n >> (56 - i * 8)) & 0xFF));
    }
  }
  if (mask) {
    for (int i = 0; i < 4; i++) f.push_back(k_mask[i]);
    for (size_t i = 0; i < n; i++) {
      f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^
                                    static_cast<unsigned char>(k_mask[i % 4])));
    }
  } else {
    f += payload;
  }
  return f;
}

/** @brief 从原始报文里取一个头（名字大小写不敏感）。 */
static std::string grab_header(const std::string& raw, const char* name) {
  const size_t nlen = std::strlen(name);
  size_t pos = raw.find("\r\n");           // 跳过状态行
  if (pos == std::string::npos) return std::string();
  pos += 2;
  while (pos < raw.size()) {
    size_t eol = raw.find("\r\n", pos);
    if (eol == std::string::npos) eol = raw.size();
    const size_t colon = raw.find(':', pos);
    if (colon != std::string::npos && colon < eol && colon - pos == nlen) {
      bool eq = true;
      for (size_t i = 0; i < nlen; ++i) {
        char a = raw[pos + i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) { eq = false; break; }
      }
      if (eq) {
        size_t v = colon + 1;
        size_t ve = eol;
        while (v < ve && (raw[v] == ' ' || raw[v] == '\t')) ++v;
        while (ve > v && (raw[ve - 1] == ' ' || raw[ve - 1] == '\t')) --ve;
        return raw.substr(v, ve - v);
      }
    }
    pos = eol + 2;
  }
  return std::string();
}

/**
 * @brief 某个头**在不在**。
 *
 * 与 `grab_header` 的区别是这个用例的判别力所在：`grab_header` 会 trim，
 * 所以 `Sec-WebSocket-Extensions: \r\n`（有头、值为空）和"压根没这个头"
 * 都返回 ""，两者分不开。而 RFC 6455 §9.1 的 `extension-list = 1#extension`
 * 要求**至少一个**扩展，空值本身就不合法 —— 严格客户端会因此握手失败。
 * 变异 D2（服务端无条件写扩展头）就藏在这个缝里：谈崩时
 * `ws_deflate_response_header` 返回空串，产出的正是上面那个空值头。
 */
static bool has_header(const std::string& raw, const char* name) {
  const size_t nlen = std::strlen(name);
  size_t pos = raw.find("\r\n");           // 跳过状态行
  if (pos == std::string::npos) return false;
  pos += 2;
  while (pos < raw.size()) {
    size_t eol = raw.find("\r\n", pos);
    if (eol == std::string::npos) eol = raw.size();
    const size_t colon = raw.find(':', pos);
    if (colon != std::string::npos && colon < eol && colon - pos == nlen) {
      bool eq = true;
      for (size_t i = 0; i < nlen; ++i) {
        char a = raw[pos + i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) { eq = false; break; }
      }
      if (eq) return true;
    }
    pos = eol + 2;
  }
  return false;
}

// =========================================================================
// 场景：真 `uvcpp_ws_server` + 手工拼字节的裸 TCP 客户端
//
// 用裸客户端而不是 `uvcpp_ws_client`，是为了能精确控制 offer 的内容、并直接
// 检查 101 的原始报文和线上帧的 RSV1。真客户端那条路径另有一组用例。
// =========================================================================

struct scenario {
  uvcpp_ws_server* server = nullptr;
  uvcpp_tcp_client client;
  uvcpp_ws_connection* conn = nullptr;

  int  port = 0;
  bool connected = false;
  bool handshake_done = false;
  std::string handshake_resp;    // 101 的原始报文

  std::vector<std::string>    server_texts;
  std::vector<uvcpp_ws_frame> cframes;   // 服务端 → 客户端方向，测试侧解析
  bool parse_error = false;

  uvcpp_ws_parser      tparser;          // 测试侧"客户端"的压缩器/解压器
  uvcpp_ws_deflate_params negotiated;

  /** @brief 最近一个失败点（用例失败时打出来，省得靠猜）。 */
  const char* why = "";

  bool start(const uvcpp_ws_deflate_config& scfg) {
    server = new uvcpp_ws_server();      // 故意泄漏，见文件末尾说明
    if (server->bind("127.0.0.1", 0) != 0) { why = "bind"; return false; }
    server->set_compression(scfg);

    struct sockaddr_in name;
    int nl = static_cast<int>(sizeof(name));
    server->get_http_server()->get_tcp_server()->get_tcp()->getsockname(
        reinterpret_cast<sockaddr*>(&name), &nl);
    port = ntohs(name.sin_port);

    server->on_connection([this](uvcpp_ws_connection* c) {
      conn = c;
      // 回显：把收到的消息原样发回去。发送侧是否压缩由协商结果决定。
      c->on_text([this](const std::string& m) {
        server_texts.push_back(m);
        if (conn) conn->send_text(m.c_str(), m.size());
      });
    });

    if (server->listen() != 0) { why = "listen"; return false; }

    if (client.connect("127.0.0.1", port,
                       [this](int st) { connected = (st == 0); }) != 0) {
      why = "client connect call";
      return false;
    }
    if (!pump([this] { return connected; }, 3000)) { why = "tcp connect timeout"; return false; }

    // 读回调必须等**连上之后**再装：socket 还没建立时 read_start 起不来，
    // 装早了会静默拿不到任何字节（真客户端的 read_start 也是写在写完成回调里的）。
    const int rrc = client.read_start([this](uvcpp_buf* b) {
      if (!b || b->size() == 0) return;
      std::string chunk(b->get_const_data(), b->size());
      if (!handshake_done) {
        handshake_resp += chunk;
        const size_t e = handshake_resp.find("\r\n\r\n");
        if (e == std::string::npos) return;
        handshake_done = true;
        chunk = handshake_resp.substr(e + 4);   // 101 之后可能已经跟了帧
        if (chunk.empty()) return;
      }
      feed_frames(chunk);
    });
    if (rrc != 0) { why = "read_start"; return false; }
    return true;
  }

  // 只喂**这一次新到的**字节。解析器自己就留着半帧状态（`state_` /
  // `payload_received_` / `ext_pos_`，见 `parse_payload` 的 append_data），
  // 所以外面**绝不能再存一份"半帧缓冲"**：上一版这里累积 `pending` 并把
  // 整段重喂，而 `off` 只在 COMPLETE 时推进 —— 解析器在半帧那次已经吃掉的
  // 那几个头字节既留在了 `pending` 里、又已经进了解析器状态，于是被当成新
  // 字节**又喂了一遍**。
  //
  // 另两个调用点（生产侧 `uvcpp_ws_connection::on_tcp_data`、
  // `web_ws_reassembly_func.cpp` 的 `drain_client_frames`）都是下面这个形状，
  // 这里是全仓库唯一一处写错的。
  void feed_frames(const std::string& chunk) {
    const char* d = chunk.data();
    size_t n = chunk.size();
    while (n > 0) {
      const size_t used = tparser.execute(d, n);
      const ws_parser_state st = tparser.get_state();
      if (st == ws_parser_state::PARSE_ERROR) { parse_error = true; return; }
      if (st != ws_parser_state::COMPLETE) return;  // 余下字节解析器自己留着
      cframes.push_back(tparser.get_current_frame());
      tparser.reset();
      if (used == 0) return;      // 防御：不前进就退出，绝不空转
      d += used;
      n -= used;
    }
  }

  bool pump(const std::function<bool()>& done, int timeout_ms) {
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      server->run(UV_RUN_NOWAIT);
      client.get_loop()->run(UV_RUN_NOWAIT);
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
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      server->run(UV_RUN_NOWAIT);
      client.get_loop()->run(UV_RUN_NOWAIT);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= ms) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  bool send_bytes(const std::string& b) {
    bool done = false;
    if (client.write(b.data(), b.size(), [&done](int) { done = true; }) != 0) {
      return false;
    }
    return pump([&done] { return done; }, 3000);
  }

  std::string ext_header() const {
    return grab_header(handshake_resp, "sec-websocket-extensions");
  }

  /** @brief 101 里到底有没有扩展头 —— 值是空串也算"有"（见 `has_header`）。 */
  bool ext_header_present() const {
    return has_header(handshake_resp, "sec-websocket-extensions");
  }

  /** @brief 发升级请求并等 101 到齐。offer 为空则不提议任何扩展。 */
  bool connect_and_upgrade(const std::string& offer) {
    std::string req =
        "GET /chat HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n";
    if (!offer.empty()) req += "Sec-WebSocket-Extensions: " + offer + "\r\n";
    req += "\r\n";
    if (!send_bytes(req)) { why = "send upgrade request"; return false; }
    if (!pump([this] { return handshake_done; }, 3000)) {
      why = "no 101 response";
      return false;
    }
    return true;
  }

  /**
   * @brief 按**101 的实际应答**配置测试侧的压缩器/解压器（像个真客户端）。
   *
   * 这本身就是一条断言：真客户端的 `ws_deflate_accept_client` 必须接受服务端
   * 的应答。它不接受，这里就拿不到参数。
   */
  bool setup_compression(const uvcpp_ws_deflate_config& ccfg) {
    negotiated = ws_deflate_accept_client(ws_parse_extensions(ext_header()), ccfg);
    if (!negotiated.accepted || negotiated.invalid) return false;
    tparser.enable_compression(false /* is_server */,
                               negotiated.client_no_context_takeover,
                               negotiated.server_no_context_takeover,
                               negotiated.client_max_window_bits,
                               negotiated.server_max_window_bits);
    return true;
  }

  /** @brief 按测试侧的协商参数压一条消息，产出线上负载。 */
  bool compress(const std::string& msg, std::string& wire) {
    uvcpp_buf out;
    if (tparser.compress(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(),
                         out) != 0) {
      return false;
    }
    wire.assign(reinterpret_cast<const char*>(out.get_const_udata()), out.size());
    return true;
  }
};

/** @brief 起场景 + 升级握手，失败时把失败点打出来（省得靠猜）。 */
static bool started(scenario& s, const uvcpp_ws_deflate_config& cfg) {
  if (!s.start(cfg)) {
    std::cout << "    start failed at: " << s.why << "\n";
    return false;
  }
  return true;
}
static bool upgraded(scenario& s, const char* offer) {
  if (!s.connect_and_upgrade(offer)) {
    std::cout << "    upgrade failed at: " << s.why << "\n";
    return false;
  }
  return true;
}

/** @brief 2000 字节高重复度文本：压得动，且两条相同消息必有一次回溯可引用。 */
static std::string repetitive(size_t n = 2000) {
  std::string s;
  s.reserve(n);
  while (s.size() < n) s += "abcdefghij";
  s.resize(n);
  return s;
}

/**
 * @brief 压不动的伪随机负载（固定的 LCG，可复现）。
 *
 * 与 `repetitive` 相反：deflate 对它几乎无计可施，所以"压缩后"的帧仍然和
 * 原负载一样大。要的就是这个 —— 只有帧大到装不进一次 read，服务端的回显
 * 才会**跨多次读**到达测试侧，从而真正走到 `feed_frames` 的跨读路径。
 */
static std::string incompressible(size_t n) {
  std::string s;
  s.reserve(n);
  uint32_t x = 0x12345678u;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;          // Numerical Recipes 的 LCG
    s.push_back(static_cast<char>((x >> 24) & 0xFF));
  }
  return s;
}

/** @brief 把一条测试侧发出的**未压缩**消息经过服务端回显，返回服务端的产物。 */
static bool echo_plain(scenario& s, const std::string& msg, std::string& out) {
  const size_t before = s.cframes.size();
  if (!s.send_bytes(raw_frame(0x1, true, msg, true, 0x00))) return false;
  if (!s.pump([&] { return s.cframes.size() > before; }, 3000)) return false;
  if (s.parse_error) return false;   // 帧解析坏了就别当成"帧没来"往下走
  const uvcpp_ws_frame& f = s.cframes[before];
  out.assign(reinterpret_cast<const char*>(f.payload.get_const_udata()),
             f.payload.size());
  return true;
}

// =========================================================================
// 用例
// =========================================================================

/**
 * @brief 一个帧被 TCP 拆成两次读到达 —— **客户端 → 服务端**方向。
 *
 * 这是**生产侧**的路径：`uvcpp_ws_connection::on_tcp_data` 的"喂 → 看状态 →
 * reset → 继续喂"循环。套件里此前没有任何一条用例让一帧跨两次读到达，是真实
 * 的覆盖缺口。
 *
 * 切口取 3 字节：无论长度用 7 位、16 位还是 64 位编码，第 4 个字节都还在
 * **帧头内部**（<126 时是 4 字节 masking key，否则是扩展长度字段），所以
 * 这个切法不依赖压缩后的长度，是确定的。
 *
 * 注意方向：本用例**测不到**测试侧 `feed_frames` 的跨读行为 —— 那函数解析的
 * 是反方向（服务端 → 客户端）。要打它得让**回显**跨多次读到达，那是下面
 * `large_echo_across_reads` 的活。
 */
static bool test_split_frame_across_reads() {
  scenario& s = *new scenario();
  if (!started(s, uvcpp_ws_deflate_config())) return false;
  if (!upgraded(s, "permessage-deflate; client_max_window_bits")) return false;
  if (!s.setup_compression(uvcpp_ws_deflate_config())) return false;

  const std::string msg = repetitive();
  std::string cwire;
  if (!s.compress(msg, cwire)) return false;
  const std::string frame = raw_frame(0x1, true, cwire, true, 0x40);
  const size_t cut = 3;
  if (frame.size() <= cut + 4) return false;      // 得真的把帧切开

  // 前半截（帧头的一半）先发。此时语义上还不该有任何消息到场。
  if (!s.send_bytes(frame.substr(0, cut))) return false;
  s.settle(50);
  if (s.parse_error) return false;
  if (!s.server_texts.empty()) return false;      // 半帧不算消息

  // 再发后半截。两次写之间 `send_bytes` 已经等过写完成回调，
  // 所以不会撞上 `uvcpp_tcp_client` 的 UV_EALREADY。
  if (!s.send_bytes(frame.substr(cut))) return false;
  if (!s.pump([&] { return !s.server_texts.empty(); }, 3000)) return false;
  if (s.parse_error) return false;
  if (s.server_texts.size() != 1) return false;   // 拆开的帧只能算**一条**消息
  return s.server_texts[0] == msg;
}

/**
 * @brief 带掩码的帧在**负载中间**被切开 —— 三个相位各来一次。
 *
 * 这条用例是给 `parse_payload` 的掩码路径上的一个具体风险准备的：掩码是
 * "负载第 i 个字节异或 `mask_key[i % 4]`"（§5.3），所以一段续接的负载必须
 * 接着上一段的**相位**继续异或（`payload_received_ % 4`），不能从 0 重来。
 *
 * 什么时候会踩到：帧跨多次 read 到达、且切口落在负载里。此时第二段的相位
 * 是 1/2/3，正好各自出错一种写法。而 `split_frame_across_reads` 的切口在
 * **帧头**里（第 3 字节），负载整段到达、相位恒为 0 —— 它抓不到这一类。
 *
 * 负载用 `incompressible` 且够大，一帧必然被 TCP 切开（发的是明文帧，
 * rsv=0；回显才压缩，那是另一条已经覆盖过的路径）。
 */
static bool masked_split_phase(size_t payload_offset_in_segment) {
  scenario& s = *new scenario();
  if (!started(s, uvcpp_ws_deflate_config())) return false;
  if (!upgraded(s, "permessage-deflate; client_max_window_bits")) return false;

  const std::string msg = incompressible(256 * 1024);
  const std::string frame = raw_frame(0x1, true, msg, true, 0x00);
  // 负载 > 65535 → 帧头是 2 + 8(长度) + 4(掩码) = 14 字节。
  const size_t header = 2 + 8 + 4;
  if (frame.size() <= header + 4) return false;
  const size_t cut = header + payload_offset_in_segment;

  if (!s.send_bytes(frame.substr(0, cut))) return false;
  s.settle(50);
  if (s.parse_error) return false;
  if (!s.server_texts.empty()) return false;      // 半帧不算消息

  if (!s.send_bytes(frame.substr(cut))) return false;
  if (!s.pump([&] { return !s.server_texts.empty(); }, 10000)) return false;
  if (s.parse_error) return false;
  if (s.server_texts.size() != 1) return false;
  // 相位错一位，解出来的就是一堆乱码 —— 这个比较就是判据
  return s.server_texts[0] == msg;
}

/** @brief 把上面那条用例绑成 `bool(*)()`（用例表用的是裸函数指针，C++11 没有
 *  能捕获常量的写法，所以用模板）。 */
template <size_t N>
struct masked_split_phase_thunk {
  static bool run() { return masked_split_phase(N); }
};

/**
 * @brief 大到装不进一次 read 的回显 —— **服务端 → 客户端**方向。
 *
 * 这条才是打测试侧 `feed_frames` 跨读路径的：负载用 `incompressible`，压完
 * 还是一样大，于是服务端发回来的那一帧会被 TCP 切成多段，`read_start` 分多次
 * 回调，`feed_frames` 就被调用多次、每次都只拿到半个帧。
 *
 * 这正是 harness 之前那个"半帧缓冲"bug 藏身的地方：它把解析器已经吃掉的半截
 * 又喂了一遍。用 `repetitive` 的负载打不到 —— 高度可压的东西压完只有几十字节，
 * 一次 read 就收全了。
 */
static bool test_large_echo_across_reads() {
  scenario& s = *new scenario();
  if (!started(s, uvcpp_ws_deflate_config())) return false;
  if (!upgraded(s, "permessage-deflate; client_max_window_bits")) return false;
  if (!s.setup_compression(uvcpp_ws_deflate_config())) return false;

  const std::string msg = incompressible(512 * 1024);
  if (!s.send_bytes(raw_frame(0x1, true, msg, true, 0x00))) return false;
  if (!s.pump([&] { return !s.server_texts.empty(); }, 10000)) return false;
  if (s.server_texts.size() != 1 || s.server_texts[0] != msg) return false;

  // 等回显整帧解析完。载荷压不动，所以这一帧必然跨多次读。
  if (!s.pump([&] { return !s.cframes.empty(); }, 10000)) return false;
  if (s.parse_error) return false;
  if (s.cframes.size() != 1) return false;     // 跨读的帧只能算**一条**

  const uvcpp_ws_frame& f = s.cframes[0];
  if (f.opcode != ws_opcode::TEXT) return false;
  if (!f.rsv1) return false;
  // 别拿压缩后的长度和明文比：`incompressible` 的数据 deflate 之后**更大**
  // （每 16KB 一个存储块，加 5 字节块头：512KiB 的负载量出来是 524449）。
  // 只断言"解出来一样"。
  uvcpp_buf plain;
  if (s.tparser.decompress(f.payload.get_const_udata(), f.payload.size(), plain) != 0) {
    return false;
  }
  if (plain.size() != msg.size()) return false;
  return std::memcmp(plain.get_const_udata(), msg.data(), msg.size()) == 0;
}

// 1. 默认配置：协商成 permessage-deflate，且两个方向都真的压了
static bool test_negotiate_default() {
  scenario& s = *new scenario();
  if (!started(s, uvcpp_ws_deflate_config())) return false;
  if (!upgraded(s, "permessage-deflate; client_max_window_bits")) return false;

  // 服务端默认配置不提任何参数（15 位 = 协议默认，不必显式声明）
  if (s.ext_header() != "permessage-deflate") {
    std::cout << "    101 header: [" << s.ext_header() << "]\n";
    return false;
  }
  if (!s.conn || !s.conn->is_compression_enabled()) return false;
  if (!s.setup_compression(uvcpp_ws_deflate_config())) return false;

  const std::string msg = repetitive();

  // ---- 客户端 → 服务端：发一条**真的压过**的帧 ----
  std::string cwire;
  if (!s.compress(msg, cwire)) return false;
  if (cwire.size() >= msg.size() / 2) return false;   // 2000 字节重复文本压不动？
  if (!s.send_bytes(raw_frame(0x1, true, cwire, true, 0x40))) return false;
  if (!s.pump([&] { return !s.server_texts.empty(); }, 3000)) return false;
  if (s.server_texts.size() != 1 || s.server_texts[0] != msg) return false;

  // ---- 服务端 → 客户端：回显也必须是压过的 ----
  if (!s.pump([&] { return !s.cframes.empty(); }, 3000)) return false;
  const uvcpp_ws_frame& f = s.cframes[0];
  if (f.opcode != ws_opcode::TEXT) return false;
  if (!f.rsv1) return false;                          // RSV1 没置位 = 根本没压
  if (f.payload.size() >= msg.size() / 2) return false;
  uvcpp_buf plain;
  if (s.tparser.decompress(f.payload.get_const_udata(), f.payload.size(), plain) != 0) {
    return false;
  }
  if (plain.size() != msg.size()) return false;
  if (std::memcmp(plain.get_const_udata(), msg.data(), msg.size()) != 0) return false;
  return true;
}

// 2. 服务端配置的参数要原样出现在 101 里
static bool test_negotiate_with_params() {
  scenario& s = *new scenario();
  uvcpp_ws_deflate_config scfg;
  scfg.client_max_window_bits = 12;
  scfg.server_max_window_bits = 10;
  scfg.server_no_context_takeover = true;
  if (!started(s, scfg)) return false;

  // 对端只提了**无值**的 client_max_window_bits —— 正是"选择权交给你"的形态，
  // 服务端这时才该定下 12。
  if (!upgraded(s, "permessage-deflate; client_max_window_bits")) return false;
  const std::string want =
      "permessage-deflate; server_no_context_takeover; "
      "server_max_window_bits=10; client_max_window_bits=12";
  if (s.ext_header() != want) {
    std::cout << "    want: [" << want << "]\n    got:  [" << s.ext_header() << "]\n";
    return false;
  }
  if (!s.conn || !s.conn->is_compression_enabled()) return false;
  // 真客户端必须接受这条应答（方向和取值都对得上）
  return s.setup_compression(uvcpp_ws_deflate_config());
}

// 3. 协商结果真的到达了压缩器 —— 用 context takeover 的**可观测后果**判定
static bool context_takeover_probe(bool no_ctxt) {
  scenario& s = *new scenario();
  uvcpp_ws_deflate_config scfg;
  scfg.server_no_context_takeover = no_ctxt;
  if (!started(s, scfg)) return false;
  if (!upgraded(s, "permessage-deflate; client_max_window_bits")) return false;

  const std::string want = no_ctxt ? "permessage-deflate; server_no_context_takeover"
                                   : "permessage-deflate";
  if (s.ext_header() != want) {
    std::cout << "    101 header: [" << s.ext_header() << "]\n";
    return false;
  }
  if (!s.conn->is_compression_enabled()) return false;

  const std::string msg = repetitive();
  std::string a, b;
  if (!echo_plain(s, msg, a)) return false;
  if (!echo_plain(s, msg, b)) return false;

  // 服务端回显的必须是压缩形态（否则"不压缩"的实现也能让下面成立）
  if (a.size() >= msg.size() / 2) return false;
  if (a.empty()) return false;

  // 协商成 no_context_takeover → 每条都从干净上下文压起 → 逐字节相同；
  // 协商成 context takeover  → 第二条整段回溯引用第一条 → 必然不同。
  return no_ctxt ? (a == b) : (a != b);
}

static bool test_server_context_takeover_reaches_compressor() {
  // 两个方向都要断言：只测 no_ctxt 那一半的话，"服务端压根不压缩"的实现也能过；
  // 只测 takeover 那一半的话，"服务端每次都 reset"的实现也能过。
  if (!context_takeover_probe(true)) {
    std::cout << "    no_context_takeover: two identical echoes should be identical\n";
    return false;
  }
  if (!context_takeover_probe(false)) {
    std::cout << "    context_takeover: two identical echoes should differ\n";
    return false;
  }
  return true;
}

// 4. 服务端关掉压缩：101 里不能有这个头，帧必须是明文的
static bool test_disabled_no_negotiation() {
  scenario& s = *new scenario();
  uvcpp_ws_deflate_config scfg;
  scfg.enabled = false;
  if (!started(s, scfg)) return false;
  if (!upgraded(s, "permessage-deflate; client_max_window_bits")) return false;

  if (s.ext_header_present()) {          // 关掉了就一个头都不该有（空值头也不行）
    std::cout << "    101 header present: [" << s.ext_header() << "]\n";
    return false;
  }
  if (!s.ext_header().empty()) {
    std::cout << "    101 header: [" << s.ext_header() << "]\n";
    return false;
  }
  if (!s.conn || s.conn->is_compression_enabled()) return false;

  // 明文帧照常工作，回显也必须是明文
  std::string out;
  if (!echo_plain(s, "plain text", out)) return false;
  if (!s.server_texts.empty() && s.server_texts[0] != "plain text") return false;
  const uvcpp_ws_frame& f = s.cframes[0];
  if (f.rsv1) return false;            // 没协商成却置了 RSV1
  return out == "plain text";
}

// 5. 谈不拢的 offer：不应答，退化成普通 WS，连接照常可用
static bool test_declined_offer_still_works() {
  const char* bad[] = {
    "permessage-deflate; server_max_window_bits=99",   // 越界
    "permessage-deflate; x-bogus=1",                   // 未知参数
    "permessage-deflate; client_max_window_bits=09",   // 前导零
    "permessage-deflate; server_max_window_bits",      // 该带值却没带
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    scenario& s = *new scenario();
    if (!started(s, uvcpp_ws_deflate_config())) return false;
    if (!upgraded(s, bad[i])) return false;
    // 谈崩时**连头都不能有**。只查 `ext_header().empty()` 是抓不住"回了一个
    // 空值头"的 —— 两者都返回 ""，而空值头违反 §9.1 的 `1#extension`。
    if (s.ext_header_present()) {
      std::cout << "    declined expected: " << bad[i] << " -> header present ["
                << s.ext_header() << "]\n";
      return false;
    }
    if (!s.ext_header().empty()) {
      std::cout << "    declined expected: " << bad[i] << " -> [" << s.ext_header()
                << "]\n";
      return false;
    }
    if (!s.conn || s.conn->is_compression_enabled()) return false;
    std::string out;
    if (!echo_plain(s, "still fine", out)) return false;
    if (out != "still fine") return false;
    if (s.cframes[0].rsv1) return false;
  }
  return true;
}

// 6. 真客户端（uvcpp_ws_client）：走完协商，大消息往返
static bool test_real_client_roundtrip() {
  auto* server = new uvcpp_ws_server();          // 故意泄漏（双 loop 析构问题）
  if (server->bind("127.0.0.1", 0) != 0) return false;
  struct sockaddr_in name;
  int nl = static_cast<int>(sizeof(name));
  server->get_http_server()->get_tcp_server()->get_tcp()->getsockname(
      reinterpret_cast<sockaddr*>(&name), &nl);
  const int port = ntohs(name.sin_port);

  bool server_compression = false;
  server->on_connection([&](uvcpp_ws_connection* c) {
    server_compression = c->is_compression_enabled();
    c->on_text([c](const std::string& m) { c->send_text(m.c_str(), m.size()); });
  });
  if (server->listen() != 0) return false;

  auto* client = new uvcpp_ws_client();
  const std::string big = repetitive(20000);
  bool got = false, client_compression = false;
  int rc = client->connect("ws://127.0.0.1:" + std::to_string(port) + "/chat",
                           [&](uvcpp_ws_connection* c, int err) {
    if (err != 0 || c == nullptr) return;
    client_compression = c->is_compression_enabled();
    c->on_text([&](const std::string& m) {
      if (m == big) got = true;
    });
    c->send_text(big.c_str(), big.size());
  });
  if (rc != 0) return false;

  auto t0 = std::chrono::steady_clock::now();
  while (!got) {
    server->run(UV_RUN_NOWAIT);
    client->run(UV_RUN_NOWAIT);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count() >= 10000) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!got) std::cout << "    echo never arrived\n";
  // 双侧都必须报告压缩已启用：只开一侧的话上面那个往返会解出乱码
  if (!server_compression) std::cout << "    server did not enable compression\n";
  if (!client_compression) std::cout << "    client did not enable compression\n";
  return got && server_compression && client_compression;
}

// 6b. 真客户端遇到**不合规的应答**必须让握手失败（而不是悄悄降级）
//
// 一个只会按剧本回 101 的假服务端：真客户端要的"应答合法性判定"这条路径，
// 用真服务端造不出来 —— 真服务端不会答一个自己都认为非法的头。
struct fake_server {
  uvcpp_tcp_server server;
  int  port = 0;
  std::string reply;
  bool req_seen = false;

  bool start(const std::string& ext_header) {
    if (server.bindIpv4("127.0.0.1", 0) != 0) return false;
    struct sockaddr_in name;
    int nl = static_cast<int>(sizeof(name));
    server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &nl);
    port = ntohs(name.sin_port);

    reply = "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n";
    if (!ext_header.empty()) {
      reply += "Sec-WebSocket-Extensions: " + ext_header + "\r\n";
    }
    reply += "\r\n";

    return server.listen([this](uvcpp_tcp_client* c) {
      c->read_start([this, c](uvcpp_buf* b) {
        if (!b || b->size() == 0) return;
        req_seen = true;
        // 异步写（绝不传 nullptr：那样会掉进 tcp_client 的**阻塞**写分支，
        // 在事件循环线程上自旋泵循环 —— Phase 0 修掉的那个坑）。
        c->write(reply.data(), reply.size(), [](int) {});
      });
    }, 8) == 0;
  }

  bool pump(const std::function<bool()>& done, int timeout_ms) {
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      server.get_loop()->run(UV_RUN_NOWAIT);
      if (done()) return true;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= timeout_ms) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
};

/** @brief 真客户端连一个只会背台词的假服务端，返回握手结果错误码。 */
static bool client_handshake_against(const std::string& ext_header, int& out_err) {
  fake_server& fs = *new fake_server();          // 故意泄漏
  if (!fs.start(ext_header)) return false;
  uvcpp_ws_client* cl = new uvcpp_ws_client();   // 故意泄漏

  bool done = false;
  out_err = 0;
  if (cl->connect("ws://127.0.0.1:" + std::to_string(fs.port) + "/x",
                  [&](uvcpp_ws_connection*, int e) { out_err = e; done = true; }) != 0) {
    return false;
  }
  auto t0 = std::chrono::steady_clock::now();
  while (!done) {
    cl->run(UV_RUN_NOWAIT);
    fs.server.get_loop()->run(UV_RUN_NOWAIT);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count() >= 5000) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

static bool test_client_fails_on_invalid_response() {
  // 对照：同一条路径上**合规**的应答必须让握手成功。没有这一半，一个"永远
  // 拒绝"的客户端也能让下面的断言全绿。
  int err = 0;
  if (!client_handshake_against("permessage-deflate", err)) return false;
  if (err != 0) {
    std::cout << "    valid response was rejected: " << err << "\n";
    return false;
  }
  // 对端压根没提这个扩展也是正常降级，不是失败
  if (!client_handshake_against("", err)) return false;
  if (err != 0) {
    std::cout << "    absent extension was treated as an error: " << err << "\n";
    return false;
  }

  // 应答了 permessage-deflate 却答得不合规 → 必须**握手失败**。
  // 服务端写下这个头就已经认定压缩生效，本端装作没看见等于双方对帧格式的
  // 理解不一致，连上也是个必错的会话（RFC 7692 §7.1.2）。
  const char* bad[] = {
    "permessage-deflate; server_max_window_bits=7",  // 越界
    "permessage-deflate; server_max_window_bits",    // 应答里该带值却没带
    "permessage-deflate; client_max_window_bits",    // 应答里必须带值
    "permessage-deflate; x-bogus=1",                 // 本端没提过的参数
    "permessage-deflate, permessage-deflate",        // 重复应答
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    int e = 0;
    if (!client_handshake_against(bad[i], e)) return false;
    if (e == 0) {
      std::cout << "    invalid response accepted: " << bad[i] << "\n";
      return false;
    }
  }
  return true;
}

// 6c. 压缩阈值：小于阈值的消息必须**原样**发出去
//
// 阈值不是优化开关而是**协议要求**：`compress()` 会推进 deflate 上下文，
// 一旦压了就必须发出去（对端得吃到这条流来保持上下文同步）。所以"压完看看
// 哪个小再决定发哪个"是**做不到**的 —— 只能发之前按大小预判。
static bool test_compress_min_size_threshold() {
  scenario& s = *new scenario();
  if (!started(s, uvcpp_ws_deflate_config())) return false;
  if (!upgraded(s, "permessage-deflate; client_max_window_bits")) return false;
  if (!s.conn || !s.conn->is_compression_enabled()) return false;

  // 注意：变量名不能叫 `small` —— Windows 的 <rpcndr.h> 里有 `#define small char`
  // 这样的遗留宏，写出来会被替换成 `const std::string char = ...`。
  const std::string below = repetitive(2000);

  // 先确认默认（阈值 0）下这条消息确实会被压 —— 否则下面"没压"的断言
  // 可能只是因为这条消息本来就压不动，而不是阈值起了作用。
  std::string a;
  if (!echo_plain(s, below, a)) return false;
  if (a.size() >= below.size() / 2) return false;
  if (!s.cframes[0].rsv1) return false;

  // 阈值抬到这条消息之上 → 必须明文原样发（RSV1 清掉、负载逐字节相同）
  s.conn->set_compress_min_size(64 * 1024);
  if (s.conn->get_compress_min_size() != 64 * 1024) return false;
  std::string b;
  if (!echo_plain(s, below, b)) return false;
  if (s.cframes[1].rsv1) {
    std::cout << "    frame above threshold was still compressed\n";
    return false;
  }
  if (b != below) {
    std::cout << "    below-threshold frame payload is not the plain text\n";
    return false;
  }

  // 反过来：超过阈值的消息仍旧要压（阈值是"下限"不是"开关"）
  const std::string big = repetitive(80 * 1024);
  std::string c;
  if (!echo_plain(s, big, c)) return false;
  if (!s.cframes[2].rsv1) {
    std::cout << "    frame above threshold was not compressed\n";
    return false;
  }
  return true;
}

// 7. 发送通道坏掉：必须**报错**，不能静默丢帧（第二步 R10 的覆盖缺口）
//
// 触发方式用"一条从未连上的连接"：`uvcpp_tcp_client::write()` 在
// `TCP_CLIENT_CONNECTED` 之外一律返回 `UV_ENOTCONN`，而且**返回前不保存
// 回调** —— 这正是发送队列里"写没提交成功"那条分支要处理的形状。
//
// 为什么不用"真的把对端关掉"来造这个失败：那条路要等内核缓冲填满或等到
// 对端回 RST，**时机不可控**；而且关掉服务端那侧的 tcp_client 会让
// `uvcpp_ws_connection::tcp_` 指向随时可能被回收的对象（连接对象归谁所有
// 是 Phase 4 才定的事）。本用例要钉死的是"失败必须被报告"，不该顺带依赖
// 一个还没定的所有权约定。
static bool test_send_failure_is_reported() {
  auto* dead = new uvcpp_tcp_client();          // 故意泄漏（从不 run，无副作用）
  auto* conn = new uvcpp_ws_connection(dead);   // 故意泄漏

  // 首帧：入队后立刻在 pump 里失败。错误走**回调**（`send_frame` 的返回值
  // 是"有没有排上队"，不是"有没有发出去"），所以这里断言回调。
  int  first_cb = 0;
  bool first_called = false;
  const int rc1 = conn->send_text("a", 1, [&](int s) { first_cb = s; first_called = true; });
  if (rc1 != 0) { std::cout << "    first send returned " << rc1 << "\n"; return false; }
  if (!first_called) { std::cout << "    first send never reported\n"; return false; }
  if (first_cb == 0) { std::cout << "    first send reported success\n"; return false; }

  // 粘性：通道坏了之后的发送必须**立刻**结算，而不是排队等一个永远不来的
  // 完成回调（那样调用方既拿不到错误，也永远等不到回调）。
  int  second_cb = 0;
  bool second_called = false;
  const int rc2 = conn->send_text("b", 1, [&](int s) { second_cb = s; second_called = true; });
  if (rc2 == 0) { std::cout << "    send_error_ is not sticky\n"; return false; }
  if (!second_called || second_cb != rc2) {
    std::cout << "    sticky error not reported to callback\n";
    return false;
  }

  // 每一条帧都要有确定结局 —— 队列里不该留下任何被遗忘的项。
  for (int i = 0; i < 3; ++i) {
    bool called = false;
    int  st = 0;
    if (conn->send_text("c", 1, [&](int s) { st = s; called = true; }) == 0) return false;
    if (!called || st == 0) return false;
  }
  return true;
}

int main(int argc, char** argv) {
  // 无缓冲：崩在某条用例里时，缓冲里的输出不会跟着一起丢，才看得出崩在哪。
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"negotiate_default", test_negotiate_default},
    {"negotiate_with_params", test_negotiate_with_params},
    {"server_context_takeover_reaches_compressor",
     test_server_context_takeover_reaches_compressor},
    {"disabled_no_negotiation", test_disabled_no_negotiation},
    {"declined_offer_still_works", test_declined_offer_still_works},
    {"real_client_roundtrip", test_real_client_roundtrip},
    {"client_fails_on_invalid_response", test_client_fails_on_invalid_response},
    {"split_frame_across_reads", test_split_frame_across_reads},
    {"masked_split_phase_1", masked_split_phase_thunk<1>::run},
    {"masked_split_phase_2", masked_split_phase_thunk<2>::run},
    {"masked_split_phase_3", masked_split_phase_thunk<3>::run},
    {"large_echo_across_reads", test_large_echo_across_reads},
    {"compress_min_size_threshold", test_compress_min_size_threshold},
    {"send_failure_is_reported", test_send_failure_is_reported},
  };
  // 带名字参数时只跑那一条：出事时能一条条单独隔离。
  for (const auto& t : tests) {
    if (argc > 1 && std::strcmp(argv[1], t.name) != 0) continue;
    std::cout << "[web_ws_deflate] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_ws_deflate] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}

#else
int main() {
  std::cout << "[web_ws_deflate] SKIP (web or zlib disabled)\n";
  return 0;
}
#endif
