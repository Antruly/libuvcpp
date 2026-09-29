/**
 * @file tests/functional/quic_stream_func.cpp
 * @brief QUIC 传输层 1.4.1 的**第二件真事**：一条连接上的多条流真能收发。
 * @author zhuweiye
 * @version 1.4.1
 *
 * `quic_handshake_func.cpp` 测的是"两个端点握上手"。这个用例测的是握上手之后
 * **应用数据真的过得去**，而且过得去的东西带得回正确的流号 —— 这是 QUIC 与
 * TCP 在形状上最大的差别，也是本层读回调改成 `(连接, 流号, 结果)` 的唯一理由。
 *
 * 一条连接上跑三段，全部用同一条握手：
 *
 * 1. **双向流回声。** 客户端开流、写 `"ping"`、带 FIN；服务端在 `on_stream_open`
 *    里记下流号、在 `on_read` 里收全，收到 FIN 之后回写 `"pong"` 也带 FIN。
 *    客户端在 `on_read` 里收到回声与收尾，在 `on_write` 里看到自己那次
 *    `write_stream()` 被确认。
 * 2. **单向流。** 客户端开一条单向流写 `"uni"` —— 它只能发，服务端只能收。
 *    这条钉的是 `open_stream(false)` 没有退化成双向流（流号的低位不同）。
 * 3. **`shutdown_stream()` 只关发送方向。** 客户端写 `"half"`（**不带** FIN），
 *    等服务端收到之后调 `shutdown_stream()`。此后：
 *    - 本端再 `write_stream()` 同一流 → `NGTCP2_ERR_STREAM_SHUT_WR`；
 *    - **读方向照常** —— 服务端已经回写的 `"half-echo"` 仍然到得了。
 *
 * 第 3 条的两个断言必须成对看：只查前一条的话，一个把整条流拆掉的实现也能过，
 * 而那正是这条 API 要避免的事。
 *
 * **流号是断言的**，不是"拿到了就用"。客户端发起的双向流低位是 `0b00`
 * （0、4、8…），单向流是 `0b10`（2、6、10…）—— 见 RFC 9000 §2.1。这两条一起
 * 钉住了 `open_stream(bidi)` 那个布尔真的传到了 `ngtcp2_conn_open_*_stream`。
 *
 * **为什么两端共用一条循环**：同 `quic_handshake_func.cpp` 文件头那条。
 */
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

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

/// `ngtcp2_ngtcp2.h` 里 `NGTCP2_ERR_STREAM_SHUT_WR` 的值（`ngtcp2.h:657`）。
///
/// **为什么在这儿抄一个数而不是引那个宏**：公开头一个 ngtcp2 的符号都不许露
/// （见 `uvcpp_quic_common.h` 文件头），而 `write_stream()` 的返回值就是那条
/// 契约里说的"`NGTCP2_ERR_STREAM_SHUT_WR` 的负值"。抄下来，是为了让这条断言
/// 精确 —— 写 `!= 0` 的话，"返回的是随便哪个错误码"也能过。
///
/// ngtcp2 的错误码是它的**公开 ABI**（`ngtcp2_error_string()` 按它们查表），
/// 升级时不会静默改号。
const int kErrStreamShutWr = -219;

const int kDeadlineMs = 5000;

void test_streams_over_one_loop() {
  uvcpp_loop loop;
  uvcpp_test::loop_drain drain_loop(&loop);

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    std::cerr << "  [FAIL] 生成自签证书失败" << std::endl;
    ++g_failures;
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  // -------------------------------------------------------------------
  // 服务端：一条 UDP 口，回声机
  // -------------------------------------------------------------------
  uvcpp_quic_server server(&loop);
  server.set_ssl_context(&server_ctx);
  check_eq_i(server.bind("127.0.0.1", 0), 0, "bind(127.0.0.1, 0)");

  std::map<int64_t, std::string> server_got;    // 流号 -> 收到的字节
  std::map<int64_t, int> server_peer_closed;    // 流号 -> 收过几次 PEER_CLOSED
  std::map<int64_t, int> server_read_errors;    // 流号 -> 错误码
  std::map<int64_t, int> server_writes;         // 流号 -> on_write 跑了几次
  std::vector<int64_t>   server_opened;         // on_stream_open 见过的流号
  uvcpp_quic_connection* server_conn = nullptr;

  const int listen_rc = server.listen([&](uvcpp_quic_connection* c) {
    server_conn = c;
    // 与握手用例同一条：**就在这儿装回调**，不等握手完成。
    uvcpp_quic_connection::callbacks cbs;
    cbs.on_stream_open = [&](uvcpp_quic_connection&, int64_t id) {
      server_opened.push_back(id);
    };
    cbs.on_read = [&](uvcpp_quic_connection& conn, int64_t id,
                      const net_read_result& r) {
      switch (r.event) {
        case net_read_event::DATA: {
          std::string& acc = server_got[id];
          acc.append(r.data, r.size);
          // 回声的判据是"**收全了**"，不是"收到过一块"：一次 `write_stream()`
          // 里的 4 个字节会被 ngtcp2 一次交上来，但按块判会在将来被分片时
          // 悄悄变成一次错误的半截回声。按内容判没有这个问题。
          if (acc == "ping") {
            conn.write_stream(id, "pong", 4, true);
          } else if (acc == "half") {
            // 客户端这条流**没带 FIN**，所以服务端不能等 FIN 才回 ——
            // 它收到这 4 个字节就回。回写本身带 FIN：服务端的发送方向到此为止。
            conn.write_stream(id, "half-echo", 9, true);
          }
          break;
        }
        case net_read_event::PEER_CLOSED:
          ++server_peer_closed[id];
          break;
        case net_read_event::READ_ERROR:
          server_read_errors[id] = r.error;
          break;
      }
    };
    cbs.on_write = [&](uvcpp_quic_connection&, int64_t id, int status) {
      // 回声那一笔自己也要有完成通知。0 = 对端确认了。
      server_writes[id] = status;
    };
    c->set_callbacks(cbs);
  });
  check_eq_i(listen_rc, 0, "listen() 返回 0");

  const int port = server.configured_port();
  check(port > 0, "listen() 之后 configured_port() 报内核分配的端口");

  // -------------------------------------------------------------------
  // 客户端：开两条流，第三段留着后面做
  // -------------------------------------------------------------------
  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  std::map<int64_t, std::string> client_got;
  std::map<int64_t, int> client_peer_closed;
  std::map<int64_t, int> client_writes;
  int64_t bidi_id  = -1;
  int64_t uni_id   = -1;
  int64_t half_id  = -1;
  bool    connected      = false;
  int     connect_status = 12345;

  const int rc = client.connect("127.0.0.1", port, [&](int status) {
    connected      = true;
    connect_status = status;
    if (status != 0) return;
    uvcpp_quic_connection* c = client.connection();
    if (c == nullptr) {
      std::cerr << "  [FAIL] connect 回调里 connection() 是空的" << std::endl;
      ++g_failures;
      return;
    }
    // 1) 双向流：写一块、带 FIN。
    bidi_id = c->open_stream(true);
    if (bidi_id >= 0) c->write_stream(bidi_id, "ping", 4, true);
    // 2) 单向流：只能发。对端那边收到的东西与我们写进去的一模一样 ——
    //    "客户端单向流"在线上就是一条只带发送方向的流。
    uni_id = c->open_stream(false);
    if (uni_id >= 0) c->write_stream(uni_id, "uni", 3, true);
  });
  check_eq_i(rc, 0, "connect() 返回 0");
  if (rc != 0) return;

  // `connect()` 返回之后、下一次循环迭代之前装回调 —— 见
  // `uvcpp_quic_client.h` 里那条 `@warning`。
  {
    uvcpp_quic_connection::callbacks cbs;
    cbs.on_read = [&](uvcpp_quic_connection&, int64_t id,
                      const net_read_result& r) {
      switch (r.event) {
        case net_read_event::DATA:
          client_got[id].append(r.data, r.size);
          break;
        case net_read_event::PEER_CLOSED:
          ++client_peer_closed[id];
          break;
        case net_read_event::READ_ERROR:
          std::cerr << "  [FAIL] 客户端流 " << id << " 读到错误：" << r.error
                    << std::endl;
          ++g_failures;
          break;
      }
    };
    cbs.on_write = [&](uvcpp_quic_connection&, int64_t id, int status) {
      client_writes[id] = status;
    };
    client.connection()->set_callbacks(cbs);
  }

  // -------------------------------------------------------------------
  // 推进：两个端点共用这一条循环
  // -------------------------------------------------------------------
  const bool up = uvcpp_test::wait_until(&loop, [&] { return connected; },
                                         kDeadlineMs);
  check(up, "握手在 deadline 内完成");
  if (!up) return;
  check_eq_i(connect_status, 0, "connect 的 cb 收到 0");

  check(bidi_id >= 0, "客户端开出了双向流");
  check(uni_id >= 0, "客户端开出了单向流");
  // RFC 9000 §2.1：客户端发起的流，最低两位 0b00 = 双向、0b10 = 单向。
  check_eq_i(bidi_id & 0x3, 0, "双向流的流号低位是 0b00");
  check_eq_i(uni_id & 0x3, 2, "单向流的流号低位是 0b10");

  // --- 第 1 段：双向流回声 -------------------------------------------
  const bool echoed = uvcpp_test::wait_until(
      &loop, [&] { return client_got[bidi_id] == "pong"; }, kDeadlineMs);
  check(echoed, "客户端收到了回声");
  check_eq_s(server_got[bidi_id], "ping", "服务端收到的字节");
  check(!server_opened.empty(), "服务端 on_stream_open 跑过");
  check(server_opened.size() >= 1 &&
            server_opened[0] == bidi_id,
        "服务端 on_stream_open 报的流号就是客户端开的那个");

  // 服务端那边"收到 FIN"这件事单独等 —— 它在时间上严格晚于客户端发完。
  check(uvcpp_test::wait_until(
            &loop, [&] { return server_peer_closed.count(bidi_id) != 0; },
            kDeadlineMs),
        "服务端在双向流上收到 PEER_CLOSED");
  check(uvcpp_test::wait_until(
            &loop, [&] { return client_peer_closed.count(bidi_id) != 0; },
            kDeadlineMs),
        "客户端在双向流上收到 PEER_CLOSED");

  // `on_write`：客户端那笔 "ping" 被确认了，服务端那笔 "pong" 也被确认了。
  check(uvcpp_test::wait_until(
            &loop, [&] { return client_writes.count(bidi_id) != 0; },
            kDeadlineMs),
        "客户端 on_write 跑到了");
  check_eq_i(client_writes.count(bidi_id) ? client_writes[bidi_id] : -1, 0,
             "客户端那次 write_stream 的完成状态");
  check(uvcpp_test::wait_until(
            &loop, [&] { return server_writes.count(bidi_id) != 0; },
            kDeadlineMs),
        "服务端 on_write 跑到了（回声那一笔）");
  check_eq_i(server_writes.count(bidi_id) ? server_writes[bidi_id] : -1, 0,
             "服务端那次 write_stream 的完成状态");

  // --- 第 2 段：单向流 -----------------------------------------------
  check(uvcpp_test::wait_until(
            &loop, [&] { return server_got[uni_id] == "uni"; }, kDeadlineMs),
        "服务端收到了单向流上的字节");
  // 单向流上服务端只能收：它连"回"这个动作都做不了，所以 `on_write` 一次都不会响。
  check_eq_i(server_writes.count(uni_id), 0, "服务端没有在单向流上写过东西");
  check(uvcpp_test::wait_until(
            &loop, [&] { return client_writes.count(uni_id) != 0; },
            kDeadlineMs),
        "客户端单向流的 on_write 跑到了");

  // --- 第 3 段：shutdown_stream() 只关发送方向 ------------------------
  uvcpp_quic_connection* c = client.connection();
  if (c == nullptr) {
    check(false, "收尾前 connection() 还在");
    return;
  }
  half_id = c->open_stream(true);
  check(half_id >= 0, "客户端开出了第三条流（双向）");
  if (half_id >= 0) {
    check_eq_i(c->write_stream(half_id, "half", 4, false), 0,
               "write_stream(half) 不带 FIN");

    // **这一条同时是"从回调外面写"那条路的实测。** `write_stream()` 只是把字节
    // 排进队列，真正把它变成数据报的是同一函数尾巴上那次 flush —— 少了它，
    // 下面这个 wait_until 会一直等到空闲超时。
    check(uvcpp_test::wait_until(
              &loop, [&] { return server_got[half_id] == "half"; }, kDeadlineMs),
          "服务端收到了 half（从回调外面调 write_stream 也发得出去）");

    check_eq_i(c->shutdown_stream(half_id), 0, "shutdown_stream() 返回 0");
    check_eq_i(c->write_stream(half_id, "x", 1, false), kErrStreamShutWr,
               "shutdown 之后再 write_stream 同一流报 NGTCP2_ERR_STREAM_SHUT_WR");

    // **读方向没被一起关掉。** 服务端早在收到 "half" 时就回写了 "half-echo"
    // —— 那一笔必须还能到达。把这一条删掉的话，"`shutdown_stream` 把整条流
    // 拆了"这种实现会全绿。
    check(uvcpp_test::wait_until(
              &loop,
              [&] { return client_got[half_id] == "half-echo"; }, kDeadlineMs),
          "shutdown 只关发送方向：读方向的回声照常到达");
  }

  // -------------------------------------------------------------------
  // 收尾
  // -------------------------------------------------------------------
  if (server_conn != nullptr) {
    check(server_read_errors.empty(), "服务端没读到过错误事件");
  }
  check_eq_i(client.close(), 0, "close() 返回 0");
}

}  // namespace

int main() {
  std::cout << "[functional quic_stream] start" << std::endl;

  check_eq_i(quic_crypto_backend_init(), 0, "quic_crypto_backend_init()");

  test_streams_over_one_loop();

  quic_crypto_backend_free();

  std::cout << "[functional quic_stream] checks=" << g_checks
            << " failures=" << g_failures << std::endl;
  if (g_failures == 0) {
    std::cout << "[functional quic_stream] done success=true" << std::endl;
    return 0;
  }
  std::cout << "[functional quic_stream] done success=false" << std::endl;
  return 2;
}

#else  // UVCPP_QUIC_ENABLE

// 关掉 quic 模块时这个文件不该被编译（`tests/functional/CMakeLists.txt` 的
// 过滤器按文件名摘掉它 —— 规则是 `quic`）。真编到了就是配置错，返回非零而不是
// 打一句 SKIP 再返回 0：后者会把"这个模块一次都没被跑过"伪装成"全绿"。
int main() {
  std::cerr << "[quic_stream] UVCPP_QUIC_ENABLE=0 —— 这个测试文件不该被编译进来"
            << std::endl;
  return 2;
}

#endif  // UVCPP_QUIC_ENABLE
