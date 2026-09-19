/**
 * @file tests/functional/web_ssl_app_func.cpp
 * @brief 框架层 HTTPS：`uvcpp_web_app::enable_self_signed()` 跑通一个真请求。
 *
 * 这个文件补的是**框架层**的 TLS —— `web_ssl_server_func.cpp` 考的是传输层
 * （`uvcpp_tcp_server::set_ssl_context()`）的交付时机，`web_ssl_client_func.cpp`
 * 考的是 `uvcpp_tcp_client` 的过滤器。到这里为止 TLS 与**框架**的路由/解析/
 * 响应这条链从来没一起跑过。两者都必要：传输层对了，框架层仍然可能把 SSL
 * 上下文装晚了一步（`listen()` 之后才装，accept 时已经读不到），而那种错误
 * 在前面两个文件里看不见。
 *
 * 判据是**端到端**的：真的握手、真的发一个 HTTP 请求、真的拿回可解析的响应。
 * 只断言 `ssl_enabled()` 是没用的 —— 它读的是"我配置过了"，而不是"它在跑"。
 *
 * 第二个场景是反向对照：明文客户端打同一个端口，必须拿不到响应体。场景 1 的
 * 客户端本身就开着 TLS，所以"过滤器没接上"在场景 1 里表现为握手失败、也能
 * 拦住；这条补的是另一件事 —— 服务端确实是"只认 TLS"，而不是"两条路都收"。
 *
 * 第三个场景考**失败模式**：`enable_ssl()` 指到不存在的文件时，
 * `start_background()` 必须失败，而不是静默地以明文提供服务。这是这里最坏的
 * 一种错：调用方以为自己开着 HTTPS。
 *
 * 客户端为什么是裸 `uvcpp_tcp_client` 而不是 `uvcpp_http_client`：这里要的
 * 恰恰是"线上字节"这个层次。`send_wait` 会把响应解析掉，而本文件要断言的
 * 正是"解出来的明文是一个 HTTP 响应"——用对手的解析器去证明自己，等于什么
 * 都没证明。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_OPENSSL_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <ssl/uvcpp_ssl_context.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

#include <openssl/ssl.h>

#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// 客户端最多等多少**毫秒**（墙钟）。到点就收，不阻塞。
const int kClientWaitMs = 4000;

const char kRequestBody[] =
    "GET /hello HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Connection: close\r\n"
    "\r\n";

const char kReplyBody[] = "hello-over-tls";

// =========================================================================
// 一个最小的 HTTPS 客户端
// =========================================================================

struct client_probe {
  std::atomic<int>  connect_status{-99};
  std::atomic<bool> connect_fired{false};
  std::atomic<int>  read_arm_rc{-999};   ///< read_start_events() 的返回值
  std::atomic<int>  write_rc{-999};      ///< write() 的**返回值**
  std::atomic<int>  write_status{-99};   ///< 写完成回调收到的状态
  std::atomic<bool> write_fired{false};
  std::atomic<int>  io_events{0};        ///< 非数据事件（对端关闭 / 读错误）次数

  std::mutex  mu;
  std::string received;
};

/// 连上去、发一个 HTTP 请求、把收到的字节攒起来。
///
/// `cctx` 为 nullptr 时**不装 TLS** —— 那就是"明文客户端打 HTTPS 端口"的
/// 反向场景。返回时连接已关闭。
void run_client(int port, uvcpp_ssl_context* cctx, const std::string& msg,
                client_probe& p, const char* label) {
  uvcpp_tcp_client client;

  if (cctx != nullptr) {
    const int trc = client.enable_tls(cctx);
    check(trc == 0,
          std::string(label) + ": enable_tls returned " + std::to_string(trc));
  }

  const int crc = client.connect("127.0.0.1", port, [&client, &p, msg](int st) {
    p.connect_status.store(st);
    p.connect_fired.store(true);
    if (st != 0) return;  // 连都没连上，既没什么可写，也 arm 不了读

    // **读必须在连接回调里装。** 在 `connect()` 之前装的话 socket 还没建立，
    // `read_start_events` 会返回 `UV_ENOTCONN`（-4053）：回调**登记是成功了**，
    // 却没有任何人 arm 它 —— 而明文客户端的连接完成路径**不会**补这一次 arm
    // （只有 TLS 那一支会，见 `uvcpp_tcp_client.cpp` 的 connect 完成回调）。
    // 也就是说"先 connect 再装读"在明文下是一条**静默没有数据**的路。
    // 这里当场断言返回值，就是为了让那种静默变成一次失败。
    p.read_arm_rc.store(client.read_start_events(
        [&p](uvcpp_tcp_client&, const net_read_result& r) {
          if (r.is_data()) {
            std::lock_guard<std::mutex> lk(p.mu);
            p.received.append(r.data, r.size);
            return;
          }
          p.io_events.fetch_add(1);  // 对端关闭 / 读错误
        }));

    // **请求就在连接回调里当场发。**
    //
    // 这是最自然的用法，也是唯一能打到"与握手完成同一段到达"那个窗口的形状：
    // TLS 1.3 的客户端可以在自己那条 Finished 之后立刻发应用数据，两笔写背靠背
    // 出网；对端循环若没来得及跑，它们就被合并进同一次 read —— 而那一次 read
    // 既完成握手、又带着这条请求。把写推迟几十圈能让用例变稳，但那是**把窗口
    // 藏起来**，而窗口里的行为正是本文件要考的东西。
    const int wrc = client.write(msg.data(), msg.size(), [&p](int ws) {
      p.write_status.store(ws);
      p.write_fired.store(true);
    });
    p.write_rc.store(wrc);
    if (wrc != 0) p.write_fired.store(true);
  });
  check(crc == 0, std::string(label) + ": connect() start failed");

  uvcpp_loop* loop = client.get_loop();
  uvcpp_test::wait_until(
      loop,
      [&p] {
        std::lock_guard<std::mutex> lk(p.mu);
        if (p.received.find(kReplyBody) != std::string::npos) return true;
        if (p.connect_fired.load() && p.connect_status.load() != 0) return true;
        // 反向场景靠这条收工：明文打 TLS 端口，服务端拒了之后会关连接。
        return p.connect_fired.load() && p.io_events.load() > 0;
      },
      kClientWaitMs);

  client.close();
  uvcpp_test::pump_for(loop, 60);
}

std::string probe_received(client_probe& p) {
  std::lock_guard<std::mutex> lk(p.mu);
  return p.received;
}

void configure_for_test(uvcpp_web_app& app) {
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);
}

/// 三个场景共用的那条路由。
void add_hello_route(uvcpp_web_app& app) {
  app.get("/hello", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    (void)req;
    (void)next;
    resp.text(kReplyBody);
    resp.end();
  });
}

/// 连上去、发一段**永远凑不完整**的 ClientHello，然后看服务端会不会主动关掉它。
///
/// 返回 true = 在观察窗口内被服务端关闭。
///
/// 为什么这条必须存在：服务端在握手成功之前**不会**把连接交给框架层，于是这
/// 条连接既不在框架层的连接登记表里，也就碰不到 `idle_timeout_ms` —— 一个
/// 连上之后只发半个 ClientHello 的客户端，原本可以让连接连同它的 SSL 对象与
/// 读写缓冲区无限期挂着。这个用例钉的就是"握手阶段也有闸门"。
///
/// 故意**不开 TLS** 地用 `uvcpp_tcp_client`：本用例要的恰恰是"线上字节凑不成
/// 一次完整握手"这个状态，用明文 socket 发 TLS 记录头的前几个字节，对服务端
/// 来说与一个真正的 TLS 客户端发到一半卡住完全一样。
bool probe_stalled_handshake(int port, int observe_ms, const char* label) {
  // 一条真实 ClientHello 的开头：记录头 + 握手类型 + 长度 + 版本 + 随机数前几位。
  // 到这里就断掉 —— 长度字段声明的 0x0200 字节永远收不满。
  static const unsigned char kPartialHello[] = {
      0x16, 0x03, 0x01, 0x02, 0x00, 0x01, 0x00, 0x01, 0xfc, 0x03, 0x03};

  std::atomic<bool> closed{false};
  std::atomic<bool> connect_fired{false};
  std::atomic<int>  connect_status{-99};
  std::atomic<int>  read_arm_rc{-999};
  std::atomic<int>  write_rc{-999};

  uvcpp_tcp_client client;
  const int crc = client.connect("127.0.0.1", port, [&](int st) {
    connect_fired.store(true);
    connect_status.store(st);
    if (st != 0) return;
    read_arm_rc.store(client.read_start_events(
        [&closed](uvcpp_tcp_client&, const net_read_result& r) {
          if (!r.is_data()) closed.store(true);  // 对端关闭 / 读错误
        }));
    write_rc.store(client.write(
        reinterpret_cast<const char*>(kPartialHello), sizeof(kPartialHello),
        [](int) {}));
  });
  check(crc == 0, std::string(label) + ": connect() start failed");

  uvcpp_loop* loop = client.get_loop();
  uvcpp_test::wait_until(
      loop,
      [&connect_fired, &connect_status, &closed] {
        if (!connect_fired.load()) return false;
        if (connect_status.load() != 0) return true;
        return closed.load();
      },
      observe_ms);

  check(connect_fired.load(), std::string(label) + ": connect cb never fired");
  check(connect_status.load() == 0,
        std::string(label) + ": connect status = " +
            std::to_string(connect_status.load()));
  check(read_arm_rc.load() == 0,
        std::string(label) + ": read_start_events returned " +
            std::to_string(read_arm_rc.load()));
  check(write_rc.load() == 0,
        std::string(label) + ": write() returned " +
            std::to_string(write_rc.load()));

  const bool was_closed = closed.load();
  client.close();
  uvcpp_test::pump_for(loop, 60);
  return was_closed;
}

}  // namespace

int main() {
  std::cout << "[web_ssl_app] OpenSSL " << OpenSSL_version(OPENSSL_VERSION_STRING)
            << std::endl;

  // 客户端上下文：自签证书的回环用例必须不校验，否则握手就过不去。
  // **生产客户端不能这样** —— 显式写出来正是为了让这件事显眼。
  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  if (!cctx.is_ready()) {
    std::cerr << "  [FAIL] client ctx not ready: " << cctx.get_last_error()
              << std::endl;
    return 2;
  }
  cctx.set_verify_mode(tls_verify_mode::NONE);

  const std::string request(kRequestBody, sizeof(kRequestBody) - 1);

  // =====================================================================
  // 场景 1：enable_self_signed() + 真 TLS 客户端 → 完整 HTTP 往返
  // =====================================================================
  {
    uvcpp_web_app app;
    configure_for_test(app);
    app.enable_self_signed("localhost", 2048);
    add_hello_route(app);

    check(app.ssl_enabled(),
          "scenario 1: ssl_enabled() false after enable_self_signed()");
    check(app.ssl_context() != nullptr,
          "scenario 1: ssl_context() is null: " + app.ssl_error());

    const int rc = app.start_background();
    check(rc == 0, "scenario 1: start_background failed with " +
                       std::to_string(rc) + " (" + app.ssl_error() + ")");
    if (rc != 0) {
      std::cout << "[web_ssl_app] FAIL (" << g_failures << " checks)"
                << std::endl;
      return 2;
    }

    client_probe p;
    run_client(app.bound_port(), &cctx, request, p, "https_roundtrip");

    check(p.connect_fired.load(), "scenario 1: connect cb never fired");
    check(p.connect_status.load() == 0,
          "scenario 1: connect status = " +
              std::to_string(p.connect_status.load()));
    check(p.read_arm_rc.load() == 0,
          "scenario 1: read_start_events returned " +
              std::to_string(p.read_arm_rc.load()));
    check(p.write_rc.load() == 0,
          "scenario 1: write() returned " + std::to_string(p.write_rc.load()));
    check(p.write_fired.load(), "scenario 1: write cb never fired");
    check(p.write_status.load() == 0,
          "scenario 1: write status = " +
              std::to_string(p.write_status.load()));

    const std::string got = probe_received(p);
    check(got.compare(0, 7, "HTTP/1.") == 0,
          "scenario 1: response is not an HTTP status line (got " +
              std::to_string(got.size()) + " bytes, io_events=" +
              std::to_string(p.io_events.load()) + ")");
    check(got.find(" 200 ") != std::string::npos,
          "scenario 1: no 200 in the status line (got " +
              std::to_string(got.size()) + " bytes)");
    check(got.find(kReplyBody) != std::string::npos,
          "scenario 1: body `" + std::string(kReplyBody) +
              "` missing from the response (got " + std::to_string(got.size()) +
              " bytes)");

    app.stop();
    app.join();
  }

  // =====================================================================
  // 场景 2：反向对照 —— 明文客户端打同一个 HTTPS 端口
  //
  // 服务端只认 TLS，所以这个客户端一个 HTTP 字节都不该拿到。它和
  // `web_ssl_server_func.cpp` 里那条不是一回事：那条考传输层"握手失败会不会
  // 被交付给上层"，这条考的是**框架层有没有真的把上下文装到监听器上** ——
  // 一个只把 `ssl_enabled_` 记下来、忘了 `set_ssl_context()` 的实现，在这里
  // 会给出一个完整的明文响应。
  // =====================================================================
  {
    uvcpp_web_app app;
    configure_for_test(app);
    app.enable_self_signed("localhost", 2048);
    add_hello_route(app);

    const int rc = app.start_background();
    check(rc == 0,
          "scenario 2: start_background failed with " + std::to_string(rc));
    if (rc != 0) {
      std::cout << "[web_ssl_app] FAIL (" << g_failures << " checks)"
                << std::endl;
      return 2;
    }

    client_probe p;
    run_client(app.bound_port(), nullptr, request, p, "plaintext_vs_https");

    // TCP 层当然连得上（TLS 是它上面的一层），所以这里**不**要求 connect
    // 失败；要钉的是"明文在这个会话里换不出 HTTP 响应"。
    check(p.connect_status.load() == 0,
          "scenario 2: plaintext connect status = " +
              std::to_string(p.connect_status.load()));
    check(p.read_arm_rc.load() == 0,
          "scenario 2: read_start_events returned " +
              std::to_string(p.read_arm_rc.load()));
    const std::string got = probe_received(p);
    check(got.find(kReplyBody) == std::string::npos,
          "scenario 2: a PLAINTEXT client got the HTTPS response body (" +
              std::to_string(got.size()) +
              " bytes) — the TLS context is not on this listener");
    check(got.find("HTTP/1.") == std::string::npos,
          "scenario 2: a PLAINTEXT client got an HTTP status line (" +
              std::to_string(got.size()) +
              " bytes) — the TLS context is not on this listener");

    app.stop();
    app.join();
  }

  // =====================================================================
  // 场景 3：证书加载失败必须让 start() 失败，绝不能静默降级成明文
  //
  // 这条是"失败模式"用例：一个把 `enable_ssl()` 的失败吞掉、照常以明文提供
  // 服务的实现，在场景 1/2 里都能通过（它们走的是自签那条路）。而它比启动
  // 失败危险得多 —— 调用方以为在跑 HTTPS。
  // =====================================================================
  {
    uvcpp_web_app app;
    configure_for_test(app);
    add_hello_route(app);

    const std::string missing_cert = "Z:/definitely-missing/no-such-cert.pem";
    app.enable_ssl(missing_cert, "Z:/definitely-missing/no-such-key.pem");

    check(!app.ssl_error().empty(),
          "scenario 3: enable_ssl() with missing files recorded no error");
    check(!app.ssl_enabled(),
          "scenario 3: ssl_enabled() is true even though the certificate "
          "could not be loaded");

    const int rc = app.start_background();
    check(rc != 0,
          "scenario 3: start_background() SUCCEEDED with an unusable TLS "
          "configuration — the app would serve plaintext while the caller "
          "believes it is HTTPS");

    // 失败路径上线程已经自己收过尾（`start_background()` 里 join 掉了），
    // 所以这两句是安全空操作 —— 顺手钉一下"失败之后对象仍可用"。
    app.stop();
    app.join();

    // 修好配置之后还能重试：失败不该把对象弄成一次性用品。
    app.enable_self_signed("localhost", 2048);
    check(app.ssl_enabled(),
          "scenario 3: recoverable — enable_self_signed() after a failure did "
          "not take effect");
    const int rc2 = app.start_background();
    check(rc2 == 0,
          "scenario 3: could not start after fixing the TLS configuration: " +
              std::to_string(rc2));
    if (rc2 == 0) {
      client_probe p;
      run_client(app.bound_port(), &cctx, request, p, "recovered_after_failure");
      check(probe_received(p).find(kReplyBody) != std::string::npos,
            "scenario 3: the recovered app did not serve the request over TLS");
    }
    app.stop();
    app.join();
  }

  // =====================================================================
  // 场景 4：TLS 握手阶段的超时（`set_tls_handshake_timeout_ms`）
  //
  // 服务端在握手成功之前不把连接交给框架层，所以这期间它在框架层的连接表里
  // **不存在** —— `idle_timeout_ms` 遍历的是那张表，因此管不到它。一个只发
  // 半个 ClientHello 的客户端原本可以让连接无限期挂着。本场景钉两件事：
  //
  //   1. 设了握手超时 → 卡住的握手会被关掉；
  //   2. 关掉这道闸门（0）→ 同样的连接**不会**在同一个窗口内被关掉。
  //
  // 第 2 条是对照。没有它的话，"被关掉"可能来自别的路径（比如 idle 超时），
  // 那样这条用例证明不了是握手超时干的 —— `idle_timeout_ms` 故意留得远大于
  // 观察窗口，把这条路排除掉。
  // =====================================================================
  {
    uvcpp_web_app app;
    configure_for_test(app);
    app.enable_self_signed("localhost", 2048);
    app.set_idle_timeout_ms(30000);          // 远大于下面的观察窗口
    app.set_tls_handshake_timeout_ms(300);
    add_hello_route(app);

    check(app.tls_handshake_timeout_ms() == 300,
          "scenario 4: set_tls_handshake_timeout_ms(300) did not take effect "
          "(got " + std::to_string(app.tls_handshake_timeout_ms()) + ")");

    const int rc = app.start_background();
    check(rc == 0, "scenario 4: start_background failed with " +
                       std::to_string(rc) + " (" + app.ssl_error() + ")");
    if (rc == 0) {
      check(probe_stalled_handshake(app.bound_port(), 3000,
                                    "handshake_timeout"),
            "scenario 4: a connection stalled in the TLS handshake was NOT "
            "closed within 3 s despite a 300 ms handshake timeout "
            "(idle timeout was 30 s, so nothing else could have closed it)");
    }
    app.stop();
    app.join();
  }

  {
    uvcpp_web_app app;
    configure_for_test(app);
    app.enable_self_signed("localhost", 2048);
    app.set_idle_timeout_ms(30000);
    app.set_tls_handshake_timeout_ms(0);     // 关掉这道闸门
    add_hello_route(app);

    const int rc = app.start_background();
    check(rc == 0, "scenario 4-control: start_background failed with " +
                       std::to_string(rc));
    if (rc == 0) {
      check(!probe_stalled_handshake(app.bound_port(), 2000,
                                     "handshake_timeout_disabled"),
            "scenario 4-control: with the handshake timeout DISABLED the "
            "connection was still closed — this case no longer isolates the "
            "handshake timeout, so scenario 4 proves nothing");
    }
    app.stop();
    app.join();
  }

  if (g_failures == 0) {
    std::cout << "[web_ssl_app] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[web_ssl_app] FAIL (" << g_failures << " checks)" << std::endl;
  return 2;
}

#else

int main() {
  std::cout << "[web_ssl_app] SKIP (webapp or OpenSSL disabled)" << std::endl;
  return 0;
}

#endif
