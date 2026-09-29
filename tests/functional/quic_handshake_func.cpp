/**
 * @file tests/functional/quic_handshake_func.cpp
 * @brief QUIC 传输层 1.4.1 的**第一件真事**：两个端点在一条循环上真握上手。
 * @author zhuweiye
 * @version 1.4.1
 *
 * 这个用例是 `quic_api_func.cpp` 那句"哪天真实现了，用例会红"兑现之后用来接住
 * 那条线索的。它测的是一条链路**端到端**走通，不是任何单个函数的返回码：
 *
 * 1. 服务端绑 `127.0.0.1:0`（端口由内核挑），`listen()` 之后 `configured_port()`
 *    报出真端口 —— 这条同时验了"`bind(ip, 0)` + `getsockname` 那一跳"。
 * 2. 客户端 `connect()` 返回 0，`cb(0)` 在**握手完成**时跑（不是"受理了"就跑）。
 * 3. 两边都拿到 ALPN = `"h3"`：客户端那条是它发出去、对端选定的；服务端那条是
 *    它选的。**两条断言都要** —— 只查一边的话，"ALPN 根本没协商、两边各自用了
 *    默认值"这种错会漏过去。
 * 4. `state()` 到 `ESTABLISHED`，`alpn_selected()` 是 `"h3"`。
 * 5. **服务端的 `on_alpn` 也跑到了** —— 这一条顺带钉住了"`connection_cb` 在连接
 *    刚建出来（首包到达）时就跑，不是等握手完成"这条契约：如果它等握手完成才跑，
 *    使用者在 `connection_cb` 里装的 `on_alpn` 就永远不会被调。
 *
 * **为什么服务端与客户端共用一条循环**：QUIC 的两端各要一个 socket，但一个
 * `uv_loop_t` 上放几个句柄都行，而共用一条循环让"两边的事件交替推进"变成一次
 * `wait_until` 里的事 —— 两个循环就要 `wait_until_pair`，那种写法在两边的等待
 * 条件互相依赖时（本用例就是：客户端等对端的回复）容易写成忙转。
 *
 * **5 秒的 deadline 不是"大约"。** 回环上的握手是毫秒级的，5 秒是给 CI 上最慢
 * 那条腿（Windows runner）留的余量；本机实测在 10ms 量级。
 */
#include <iostream>
#include <string>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_QUIC_ENABLE

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_common.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_context.h>
#include <ssl/uvcpp_ssl_common.h>

#include "loop_drain.h"
#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_checks   = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
  ++g_checks;
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "：期望 " << want << "，实得 " << got
              << std::endl;
    ++g_failures;
  }
}

void check_eq_s(const std::string& got, const std::string& want,
                const std::string& what) {
  ++g_checks;
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "：期望 \"" << want << "\"，实得 \""
              << got << "\"" << std::endl;
    ++g_failures;
  }
}

/// 回环上一条 QUIC 连接从 `connect()` 到 `ESTABLISHED` 的 deadline（毫秒）。
const int kHandshakeDeadlineMs = 5000;

void test_handshake_over_one_loop() {
  uvcpp_loop loop;
  uvcpp_test::loop_drain drain_loop(&loop);

  // -------------------------------------------------------------------
  // 两套 TLS 上下文。服务端自签一张，客户端**显式**关掉校验 ——
  // `uvcpp_ssl_context` 的客户端默认是 `PEER`（校验对端证书），而这里对端拿的
  // 是当场生成的自签证书，没有任何 CA 认得它。
  // -------------------------------------------------------------------
  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    std::cerr << "  [FAIL] 生成自签证书失败" << std::endl;
    ++g_failures;
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  // -------------------------------------------------------------------
  // 服务端
  // -------------------------------------------------------------------
  uvcpp_quic_server server(&loop);
  server.set_ssl_context(&server_ctx);
  check_eq_i(server.bind("127.0.0.1", 0), 0, "bind(127.0.0.1, 0)");

  bool server_got_conn  = false;
  bool server_got_alpn  = false;
  std::string server_alpn;
  int  server_close_code = 0;
  bool server_closed     = false;

  const int listen_rc = server.listen(
      [&](uvcpp_quic_connection* c) {
        server_got_conn = true;
        // **就在这儿装回调**，不等握手 —— 见文件头第 5 条。
        uvcpp_quic_connection::callbacks cbs;
        cbs.on_alpn = [&](uvcpp_quic_connection&, const std::string& a) {
          server_got_alpn = true;
          server_alpn     = a;
        };
        cbs.on_close = [&](uvcpp_quic_connection&, int code) {
          server_closed     = true;
          server_close_code = code;
        };
        c->set_callbacks(cbs);
      });
  check_eq_i(listen_rc, 0, "listen() 返回 0");

  const int port = server.configured_port();
  check(port > 0, "listen() 之后 configured_port() 报内核分配的端口");

  // -------------------------------------------------------------------
  // 客户端
  // -------------------------------------------------------------------
  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  bool        connect_called  = false;
  int         connect_status  = 12345;  // 一个绝不可能"碰巧对上"的哨兵
  bool        client_got_alpn = false;
  std::string client_alpn;
  int         client_close_code = 0;
  bool        client_closed     = false;

  const int rc = client.connect("127.0.0.1", port, [&](int status) {
    connect_called = true;
    connect_status = status;
  });
  check_eq_i(rc, 0, "connect() 返回 0");
  if (rc != 0) return;

  // `connect()` 返回之后、下一次循环迭代之前装回调 —— 头里那条 `@warning` 说的
  // 就是这个窗口。
  {
    uvcpp_quic_connection::callbacks cbs;
    cbs.on_alpn = [&](uvcpp_quic_connection&, const std::string& a) {
      client_got_alpn = true;
      client_alpn     = a;
    };
    cbs.on_close = [&](uvcpp_quic_connection&, int code) {
      client_closed     = true;
      client_close_code = code;
    };
    client.connection()->set_callbacks(cbs);
  }

  // -------------------------------------------------------------------
  // 推进：两个端点共用这一条循环
  // -------------------------------------------------------------------
  const bool done = uvcpp_test::wait_until(&loop, [&] { return connect_called; },
                               kHandshakeDeadlineMs);
  check(done, "握手在 deadline 内完成（connect 的 cb 被调了）");
  if (!done) return;

  check_eq_i(connect_status, 0, "connect 的 cb 收到 0");
  check(client_got_alpn, "客户端 on_alpn 跑到了");
  check_eq_s(client_alpn, std::string(quic_default_alpn()), "客户端选定的 ALPN");
  check(server_got_conn, "服务端 listen 的回调收到了一条连接");

  // **服务端那一侧的握手完成要单独等。** 客户端的 `connect` 回调在它处理完服务端
  // 的 Finished 时就跑，而服务端的握手完成要等它**收到客户端的 Finished** ——
  // 后者在时间上严格更晚，但两者之间只隔着一次循环迭代。直接断言是一个真竞态：
  // 本机实测会以任意一侧先到（第一次跑的时候服务端先到，第二次就反了）。
  check(uvcpp_test::wait_until(&loop, [&] { return server_got_alpn; },
                               kHandshakeDeadlineMs),
        "服务端 on_alpn 跑到了");
  check_eq_s(server_alpn, std::string(quic_default_alpn()), "服务端选定的 ALPN");

  check(client.connection() != nullptr, "connect() 之后 connection() 非空");
  if (client.connection() != nullptr) {
    check(client.connection()->state() == quic_connection_state::ESTABLISHED,
          "客户端 state() == ESTABLISHED");
    check_eq_s(client.connection()->alpn_selected(),
               std::string(quic_default_alpn()), "客户端 alpn_selected()");
  }

  // -------------------------------------------------------------------
  // 收尾：两端各关一次，两边的 on_close 都要响，且**恰好一次**。
  // -------------------------------------------------------------------
  // 这一条必须在**已经建立**之后调。`quic_api_func.cpp` 里那条"无内核时返回
  // `UV_ENOTCONN`"测的是另一个状态，两条互补，不是重复。
  check_eq_i(client.close(), 0, "close() 返回 0");

  // `close()` 是**优雅的**：它只把 CONNECTION_CLOSE 发出去、状态推到 CLOSING，
  // `on_close` 要等关闭期走完（见 `uvcpp_quic_client.h` 里那条 `@warning`）。
  check(client.connection() != nullptr &&
            client.connection()->state() == quic_connection_state::CLOSING,
        "close() 之后客户端 state() == CLOSING");

  const bool closed = uvcpp_test::wait_until(
      &loop, [&] { return client_closed && client.connection() == nullptr; },
      kHandshakeDeadlineMs);
  check(closed, "客户端的 on_close 跑到了");
  check_eq_i(client_close_code, 0, "客户端 on_close 的错误码是 0（正常关闭）");
  // 端点收尾之后 `connection()` 就该是空的 —— 头里那句"`on_close` 之后它就失效
  // 了"不是建议，是端点真的把它删了。这条断言钉的是那个删除**发生过**：只查
  // "on_close 跑了"的话，一个忘了收尾的端点也能过。
  check(client.connection() == nullptr, "on_close 之后 connection() 变空");

  // 服务端那条连接会因为收到 CONNECTION_CLOSE 而结束（DRAINING 那一格），
  // 也可能因为空闲超时。两种都算"对端走了"，但只有前一种说明 CONNECTION_CLOSE
  // **真的到了对端** —— 所以这里等它，并且不宽松地断言"总得有个收场"。
  const bool sclosed = uvcpp_test::wait_until(&loop, [&] { return server_closed; },
                                  kHandshakeDeadlineMs);
  check(sclosed, "服务端的 on_close 跑到了（对端关连接传到了对端）");
  if (sclosed) {
    check(server_close_code <= 0,
          "服务端 on_close 的错误码不是应用错误码（我们没发错误码）");
  }
}

}  // namespace

int main() {
  std::cout << "[functional quic_handshake] start" << std::endl;

  // ngtcp2 的 crypto 后端是**进程级**的，在任何端点和任何 TLS 上下文之前。
  // 与 `quic_api_func.cpp` 里那条"探针"测的是同一件事的两端：那边测它能不能
  // 调进去，这边测调了之后真的能握手。
  check_eq_i(quic_crypto_backend_init(), 0, "quic_crypto_backend_init()");

  test_handshake_over_one_loop();

  quic_crypto_backend_free();

  std::cout << "[functional quic_handshake] checks=" << g_checks
            << " failures=" << g_failures << std::endl;
  if (g_failures == 0) {
    std::cout << "[functional quic_handshake] done success=true" << std::endl;
    return 0;
  }
  std::cout << "[functional quic_handshake] done success=false" << std::endl;
  return 2;
}

#else  // UVCPP_QUIC_ENABLE

// 关掉 quic 模块时这个文件不该被编译（`tests/functional/CMakeLists.txt` 的
// 过滤器按文件名摘掉它 —— 规则是 `quic`）。真编到了就是配置错，返回非零而不是
// 打一句 SKIP 再返回 0：后者会把"这个模块一次都没被跑过"伪装成"全绿"。
int main() {
  std::cerr << "[quic_handshake] UVCPP_QUIC_ENABLE=0 —— 这个测试文件不该被编译进来"
            << std::endl;
  return 2;
}

#endif  // UVCPP_QUIC_ENABLE
