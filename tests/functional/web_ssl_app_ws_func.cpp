/**
 * @file tests/functional/web_ssl_app_ws_func.cpp
 * @brief `enable_wss()` 的 TLS 那一半：WS 跑在 TLS 之上，就是 WSS。
 *
 * 这个文件回答的问题
 * ------------------
 * `web_app_ws_func.cpp` 已经把"框架有没有把 WS 接起来"考完了，但它整篇跑在
 * **明文**上。而 `enable_wss()` 这个名字承诺的是 WSS —— 一句在实现里根本
 * 不存在、只写在注释里的承诺。注释里那句推论是：
 *
 *   > TLS 过滤器在 `uvcpp_tcp_client` 里，所以挂同一个 `uvcpp_http_server`
 *   > 上的 `uvcpp_ws_server` 会自动继承 WSS。
 *
 * 这**应该**是对的（`uvcpp_ws_server` 只拿到 `uvcpp_tcp_client*`，而对
 * `tcp_` 说的一切都是明文），但"应该"不是证据。这个文件就是那句注释的证据。
 *
 * 为什么必须真起 TLS，而不是断言一个标志位
 * -----------------------------------------
 * WS 这条路上有**两处**自己直接写 socket 的地方，它们都绕开了 HTTP 层的
 * `send_response()`：
 *
 *   1. 101 响应是 `uvcpp_ws_server::handle_upgrade()` 里 `client->write(...)`；
 *   2. 之后每一帧都是 `uvcpp_ws_connection::send_frame()` → `tcp_->write(...)`。
 *
 * 一个"HTTP 走 TLS、WS 走明文"的实现（比如把 TLS 装在 HTTP 层的出口而不是
 * 传输层）能让所有 HTTPS 用例照绿，而 WSS 一个字节都通不了。所以判据只能是
 * **真握手 + 真往返**。
 *
 * 反向对照是必需的
 * ----------------
 * 场景 2 拿**明文**客户端打同一个 WSS 端口，断言它拿不到 101。没有场景 3
 * 的话，"客户端本身就是坏的 / 根本没连上"也能让场景 2 全绿 —— 所以场景 3
 * 用**同一个客户端代码**打一个**没开 TLS** 的同构 App，必须拿到 101。
 * 有了它，场景 2 的"没有 101"才只能归因于 TLS。
 *
 * 命名：`web_ssl_` 前缀让 `tests/functional/CMakeLists.txt` 给它链
 * `${UVCPP_SSL_LIBS}`（`:47`），`web_ssl_app_` 前缀让 webapp 关掉时被 `:20`
 * 那条过滤摘掉 —— 否则它会以 SKIP 恒绿通过，变成"没测表现为全绿"。
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

#if UVCPP_WEBAPP_ENABLE && UVCPP_OPENSSL_ENABLE

#include <net/uvcpp_tcp_client.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_ws_frame.h>
#include <web/uvcpp_ws_parser.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_ws.h>
#include <webapp/uvcpp_web_ws_client.h>

#include <openssl/ssl.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

const char kPattern[] = "/echo";
const char kEchoText[] = "wss-hello";

// =========================================================================
// 线上字节：客户端 → 服务端方向必须带掩码（RFC 6455 §5.3）
// =========================================================================

const char kMask[4] = {0x37, static_cast<char>(0xfa), 0x21, 0x3d};

std::string masked_text(const std::string& s) {
  std::string f;
  f.push_back(static_cast<char>(0x80 | 0x1));           // FIN | TEXT
  const size_t n = s.size();
  f.push_back(static_cast<char>(0x80 | n));             // MASK | len（用例里都很短）
  for (int i = 0; i < 4; i++) f.push_back(kMask[i]);
  for (size_t i = 0; i < n; i++) {
    f.push_back(static_cast<char>(
        static_cast<unsigned char>(s[i]) ^
        static_cast<unsigned char>(kMask[i % 4])));
  }
  return f;
}

// =========================================================================
// 服务端观察点：handler 跑在 App 的循环线程上，测试读它 —— 全部加锁
// =========================================================================

struct ws_sink {
  std::mutex mu;
  std::string peer_ip;
  std::vector<std::string> texts;
  std::atomic<int> upgrades{0};
  std::atomic<int> closes{0};

  void note_peer(const std::string& ip) {
    std::lock_guard<std::mutex> lk(mu);
    peer_ip = ip;
  }
  void note_text(const std::string& t) {
    std::lock_guard<std::mutex> lk(mu);
    texts.push_back(t);
  }
  size_t text_count() {
    std::lock_guard<std::mutex> lk(mu);
    return texts.size();
  }
  std::string text_at(size_t i) {
    std::lock_guard<std::mutex> lk(mu);
    return i < texts.size() ? texts[i] : std::string("<missing>");
  }
};

/** @brief 装一条回显 WS 路由，并记录命中 / 收到的文本。 */
void install_echo(uvcpp_web_app& app, ws_sink* sink) {
  app.websocket(kPattern, [sink](uvcpp_web_ws_request& ws) {
    sink->upgrades.fetch_add(1);
    sink->note_peer(ws.peer_ip());
    uvcpp_ws_connection* conn = ws.connection();
    if (conn == nullptr) return;   // 不该发生；真发生了上面的断言会红
    conn->on_text([sink, conn](const std::string& m) {
      sink->note_text(m);
      conn->send_text(m.c_str(), m.size());
    });
    conn->on_close([sink](ws_close_code, const std::string&) {
      sink->closes.fetch_add(1);
    });
  });
}

void configure_for_test(uvcpp_web_app& app) {
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);
}

// =========================================================================
// 客户端探针：裸 TCP（可选 TLS）+ 手工字节，事件循环由测试线程拨
// =========================================================================

struct probe {
  uvcpp_tcp_client tcp;
  uvcpp_ws_parser  parser;

  bool connected = false;
  bool handshake_done = false;   ///< 收到了一个完整的 HTTP 报头段
  bool upgraded = false;         ///< 那个报头段是 101
  bool peer_closed = false;
  std::string resp;              ///< 非 101 时：完整响应
  std::vector<uvcpp_ws_frame> frames;
  int  connect_status = -99;
  int  read_arm_rc = -999;
  std::string where;

  bool connect(int port, uvcpp_ssl_context* cctx) {
    if (cctx != nullptr) {
      const int trc = tcp.enable_tls(cctx);
      if (trc != 0) {
        where = "enable_tls returned " + std::to_string(trc);
        return false;
      }
    }
    tcp.set_on_close([this]() { peer_closed = true; });
    if (tcp.connect("127.0.0.1", port, [this](int st) {
          connected = (st == 0);
          connect_status = st;
        }) != 0) {
      where = "connect call";
      return false;
    }
    // **读必须在连上之后装。** TLS 连接的 connect 回调是**握手完成后**才来的
    // （过滤器在 `uvcpp_tcp_client` 里，`connect` 的完成被推迟到握手完成），
    // 所以这里装读是安全的 —— 而提前装的话 socket 还没建立，会拿到
    // `UV_ENOTCONN`（-4053）：回调登记成功却没人 arm 它，表现为"数据永不到"。
    if (!pump([this] { return connected || connect_status != -99; }, 5000)) {
      where = "connect timeout";
      return false;
    }
    if (!connected) {
      where = "connect status " + std::to_string(connect_status);
      return false;
    }
    read_arm_rc = tcp.read_start([this](uvcpp_buf* b) {
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
        where = "ws parse error";
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
    if (!pump([done] { return *done; }, 5000)) {
      where = "write timeout";
      return false;
    }
    return true;
  }

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

  bool upgrade(const std::string& path) {
    const std::string req =
        "GET " + path +
        " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    return write_bytes(req);
  }

  bool wait_handshake(int timeout_ms = 5000) {
    return pump([this] { return handshake_done; }, timeout_ms);
  }

  bool wait_texts(size_t n, int timeout_ms = 5000) {
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

  int status_code() const {
    if (resp.size() < 12) return -1;
    return std::atoi(resp.c_str() + 9);
  }

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

  void close_transport() {
    tcp.get_tcp()->close();
    settle(30);
  }
};

/// 自签证书 + TLS 1.2 的客户端上下文。
/// `set_verify_mode(NONE)` 是**回环自签用例**的前提，生产客户端绝不能这样 ——
/// 显式写出来正是为了让这件事显眼（与 `web_ssl_app_func.cpp` 同一个理由）。
bool make_client_ctx(uvcpp_ssl_context& cctx) {
  if (!cctx.is_ready()) return false;
  cctx.set_verify_mode(tls_verify_mode::NONE);
  return true;
}

}  // namespace

int main() {
  std::cout << "[web_ssl_app_ws] OpenSSL "
            << OpenSSL_version(OPENSSL_VERSION_STRING) << std::endl;

  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  if (!make_client_ctx(cctx)) {
    std::cerr << "  [FAIL] client ctx not ready: " << cctx.get_last_error()
              << std::endl;
    return 2;
  }

  // =====================================================================
  // 场景 1：`websocket()` + `enable_self_signed()` → 真 WSS 往返
  // =====================================================================
  {
    uvcpp_web_app app;
    configure_for_test(app);
    app.enable_self_signed("localhost", 2048);
    ws_sink sink;
    install_echo(app, &sink);

    check(app.ssl_enabled(), "1: ssl_enabled() false after enable_self_signed()");
    const int rc = app.start_background();
    check(rc == 0, "1: start_background failed " + std::to_string(rc) + " (" +
                       app.ssl_error() + ")");
    if (rc != 0) {
      std::cout << "[web_ssl_app_ws] FAIL (" << g_failures << " checks)"
                << std::endl;
      return 2;
    }

    probe p;
    const bool up = p.connect(app.bound_port(), &cctx);
    check(up, "1: TLS connect failed: " + p.where);
    if (up) {
      check(p.read_arm_rc == 0,
            "1: read_start returned " + std::to_string(p.read_arm_rc));

      const bool sent = p.upgrade(kPattern);
      check(sent, "1: upgrade write failed: " + p.where);
      if (sent) {
        // **握手完成是 TLS 之后的第一个证据**：`connect` 的完成回调被推迟到
        // TLS 握手完成，所以能走到这里就已经说明握手过了；101 能出来则说明
        // WS 的写确实走上了 TLS 通道。
        check(p.wait_handshake(), "1: no HTTP response over TLS (peer_closed=" +
                                      std::to_string(p.peer_closed) + ")");
        check(p.upgraded,
              "1: response is not 101 (status " +
                  std::to_string(p.status_code()) + ")");
        check(p.header("upgrade") == "websocket",
              "1: 101 lacks `Upgrade: websocket` (got `" + p.header("upgrade") +
                  "`)");
        // RFC 6455 §4.2.2 的示例值：这个头不对，说明 101 是伪造的而不是
        // 真算出来的。
        check(p.header("sec-websocket-accept") ==
                  "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
              "1: Sec-WebSocket-Accept mismatch (got `" +
                  p.header("sec-websocket-accept") + "`)");

        if (p.upgraded) {
          // ---- 帧往返：这一笔走的不是 HTTP 层，是 `uvcpp_ws_connection`
          //      自己的 `tcp_->write` —— 也就是"WS 有没有继承 TLS"的判据 ----
          const bool wrote = p.write_bytes(masked_text(kEchoText));
          check(wrote, "1: frame write failed: " + p.where);
          if (wrote) {
            check(p.wait_texts(1),
                  "1: no echo frame came back over TLS (peer_closed=" +
                      std::to_string(p.peer_closed) + ", tc=" +
                      std::to_string(p.text_count()) + ")");
            check(p.text_at(0) == kEchoText,
                  "1: echo payload `" + p.text_at(0) + "` != `" + kEchoText +
                      "`");
            check(sink.text_count() == 1,
                  "1: server saw " + std::to_string(sink.text_count()) +
                      " messages, want 1");
            check(sink.text_at(0) == kEchoText,
                  "1: server-side payload is `" + sink.text_at(0) + "`");
          }
        }
      }
    }
    check(sink.upgrades.load() == 1,
          "1: WS handler ran " + std::to_string(sink.upgrades.load()) +
              " times, want 1");
    check(!sink.peer_ip.empty(), "1: peer_ip() was empty");

    p.close_transport();
    app.stop();
    app.join();
  }

  // =====================================================================
  // 场景 2：反向对照 —— **明文**客户端打同一个 WSS 端口
  //
  // 要钉的是"WS 也在 TLS 之后"：一个只在 HTTP 出口做 TLS 的实现，会让明文
  // 客户端照样拿到 101，然后 WS 帧以明文收发。
  // =====================================================================
  {
    uvcpp_web_app app;
    configure_for_test(app);
    app.enable_self_signed("localhost", 2048);
    ws_sink sink;
    install_echo(app, &sink);

    const int rc = app.start_background();
    check(rc == 0, "2: start_background failed " + std::to_string(rc));
    if (rc != 0) {
      std::cout << "[web_ssl_app_ws] FAIL (" << g_failures << " checks)"
                << std::endl;
      return 2;
    }

    probe p;
    const bool up = p.connect(app.bound_port(), nullptr);   // 不装 TLS
    check(up, "2: plaintext connect failed: " + p.where);
    if (up) {
      p.upgrade(kPattern);
      // 给足时间让它**有机会**回 101 —— 立刻查等于没查。
      p.settle(600);
      check(!p.upgraded,
            "2: a PLAINTEXT client got 101 from the WSS port —— WS 没有跑在 "
            "TLS 后面");
      check(p.status_code() != 101,
            "2: plaintext client saw status " + std::to_string(p.status_code()));
    }
    // 服务端**一次都不该**把这条连接当成 WS 交付给用户。
    check(sink.upgrades.load() == 0,
          "2: WS handler ran " + std::to_string(sink.upgrades.load()) +
              " times for a plaintext client, want 0");

    p.close_transport();
    app.stop();
    app.join();
  }

  // =====================================================================
  // 场景 3：场景 2 的**对照** —— 同一个客户端代码打一个**没开 TLS** 的
  //          同构 App，必须拿到 101。
  //
  // 没有这一条，场景 2 的全绿可以来自"这个探针根本就不会升级"（写错了、
  // 连不上、读没装上……），而不是来自 TLS。
  // =====================================================================
  {
    uvcpp_web_app app;
    configure_for_test(app);
    ws_sink sink;
    install_echo(app, &sink);   // **不调 enable_self_signed()**

    check(!app.ssl_enabled(), "3: ssl_enabled() true without enable_ssl*()");

    const int rc = app.start_background();
    check(rc == 0, "3: start_background failed " + std::to_string(rc));
    if (rc != 0) {
      std::cout << "[web_ssl_app_ws] FAIL (" << g_failures << " checks)"
                << std::endl;
      return 2;
    }

    probe p;
    const bool up = p.connect(app.bound_port(), nullptr);
    check(up, "3: plaintext connect failed: " + p.where);
    if (up) {
      p.upgrade(kPattern);
      check(p.wait_handshake(), "3: no response at all (peer_closed=" +
                                    std::to_string(p.peer_closed) + ")");
      check(p.upgraded,
            "3: plaintext WS upgrade did not yield 101 (status " +
                std::to_string(p.status_code()) +
                ") —— 对照失效，场景 2 的结论不成立");
      if (p.upgraded) {
        p.write_bytes(masked_text(kEchoText));
        check(p.wait_texts(1), "3: no echo in the plaintext control");
        check(p.text_at(0) == kEchoText,
              "3: control echo `" + p.text_at(0) + "` != `" + kEchoText + "`");
      }
    }
    check(sink.upgrades.load() == 1,
          "3: WS handler ran " + std::to_string(sink.upgrades.load()) +
              " times, want 1");

    p.close_transport();
    app.stop();
    app.join();
  }

  // =====================================================================
  // 场景 4：**框架层** WS 客户端（`uvcpp_web_ws_client`）用 `wss://` 打同一个
  //         端口 —— 钉的是协议层 `uvcpp_ws_client::do_handshake()` 的 wss 分支。
  //
  // 为什么必须单列：上面三个场景的探针都是**自己**调 `tcp.enable_tls()` 手搓
  // TLS（见 `probe::connect`），也就是说 `uvcpp_ws_client` 里那段 fd-based 握手
  // **一次都没被走过** —— 这正是它那句 `if (rc <= 0) 失败` 能一直错着的原因：
  // `uvcpp_ssl::handshake()` 的契约是「0 = 还需要更多 I/O」（`uvcpp_ssl.h:87-93`），
  // 而非阻塞 socket 上第一次 `SSL_connect` **必然**返回 0，于是每一次 `wss://`
  // 都在第一句被判成失败。这条走真路径。
  //
  // 判据是**真往返**：连上只说明握手过了，回显帧回来才说明 WS 那一层也在 TLS 上
  // —— 与场景 1 同一个理由（101 与每一帧都是绕开 HTTP 层自己写 socket 的）。
  // =====================================================================
  {
    uvcpp_web_app app;
    configure_for_test(app);
    app.enable_self_signed("localhost", 2048);
    ws_sink sink;
    install_echo(app, &sink);

    const int rc = app.start_background();
    check(rc == 0, "4: start_background failed " + std::to_string(rc));
    if (rc != 0) {
      std::cout << "[web_ssl_app_ws] FAIL (" << g_failures << " checks)"
                << std::endl;
      return 2;
    }

    uvcpp_web_ws_client wsc;
    wsc.set_ssl_context(&cctx);   // 生命周期要覆盖整条连接（本层不持有）
    std::vector<std::string> got;
    wsc.on_text([&got](const std::string& m) { got.push_back(m); });

    const std::string url = std::string("wss://127.0.0.1:") +
                            std::to_string(app.bound_port()) + kPattern;
    const int crc = wsc.connect_wait(url, 5000);
    check(crc == 0, "4: wss:// connect_wait rc=" + std::to_string(crc) +
                        " last_error=" + std::to_string(wsc.get_last_error()));
    check(wsc.is_open(), "4: connect 成功但 is_open() 为假");
    // 前提断言：不确认"确实升级成了一条 WS 会话"，下面收到的帧可能来自别处。
    //
    // **这一句必须先等，不能一返回就采样。** 服务端是**先把 101 写出去**、
    // App 的 `websocket()` handler 在**写完成回调里**才跑的
    // （`src/web/uvcpp_ws_server.cpp:214-259`：`client->write(101, cb)` 里才
    // `new uvcpp_ws_connection` 并调 `on_ready`）。所以"客户端已经收到 101"
    // 与"handler 已经跑过"之间隔着服务端循环的一拍 —— `connect_wait` 返回只
    // 证明前者。这里不等待就是拿一个还没被写到的变量当判据：场景 1 之所以没
    // 这个问题，是因为它在同一个断言之前先跑了 `p.wait_texts(1)`。
    //
    // 采样点挪到"状态必然成立"的地方，而不是把断言删掉 / 放宽：真没跑的
    // 话这里一样会超时并红，只是红得准确。
    const std::chrono::steady_clock::time_point tu =
        std::chrono::steady_clock::now();
    while (sink.upgrades.load() != 1 &&
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - tu)
                   .count() < 5000) {
      wsc.run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(sink.upgrades.load() == 1,
          "4: 服务端 WS handler 跑了 " + std::to_string(sink.upgrades.load()) +
              " 次，want 1（已等 5 秒）");

    if (crc == 0) {
      check(wsc.send_text(std::string(kEchoText)) == 0, "4: send_text 失败");

      const std::chrono::steady_clock::time_point t0 =
          std::chrono::steady_clock::now();
      while (got.empty() &&
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - t0)
                     .count() < 5000) {
        wsc.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      check(!got.empty(), "4: 5 秒内没有回显帧（TLS 上的收发没通）");
      if (!got.empty()) {
        check(got[0] == kEchoText,
              "4: 回显 `" + got[0] + "` != `" + kEchoText + "`");
      }
      check(sink.text_count() == 1,
            "4: 服务端收到 " + std::to_string(sink.text_count()) +
                " 条，want 1");
    }

    wsc.close();
    app.stop();
    app.join();
  }

  if (g_failures == 0) {
    std::cout << "[web_ssl_app_ws] ALL PASS (4 cases)" << std::endl;
    return 0;
  }
  std::cout << "[web_ssl_app_ws] FAIL (" << g_failures << " checks)" << std::endl;
  return 2;
}

#else   // !UVCPP_WEBAPP_ENABLE || !UVCPP_OPENSSL_ENABLE

int main() {
  // **不许静默通过。** 这个文件在两种配置下会被编译进来而能力其实不存在：
  // webapp 关、或 OpenSSL 关。前者靠 CMake 的 `web_ssl_app_` 过滤摘掉，后者
  // 靠 `web_ssl_` 那条 —— 两条都没命中就说明过滤器漏了，此时**报错**才是对的
  // （恒绿的空用例比没有用例更坏：它会让"全绿"这件事失去意义）。
  std::cerr << "[web_ssl_app_ws] SKIP-CONFIG: needs UVCPP_WEBAPP_ENABLE and "
               "UVCPP_OPENSSL_ENABLE"
            << std::endl;
  return 2;
}

#endif
