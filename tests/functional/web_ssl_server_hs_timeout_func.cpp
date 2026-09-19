/**
 * @file tests/functional/web_ssl_server_hs_timeout_func.cpp
 * @brief 握手超时**收尾之后循环要关得掉**：`set_tls_handshake_timeout_ms` 那
 *        条路上不留孤儿句柄。
 *
 * `web_ssl_app_func.cpp` 的场景 4（+ 4-control）钉的是"闸门确实会关掉卡住的
 * 握手"，那是对**连接**的断言。本文件钉的是**循环**：那条路上客户端是在
 * **超时回调里**被析构的（`tls_fail()` 一路走到框架自己的管理槽就 `delete`），
 * 而那个回调正是定时器句柄自己的回调 —— 包装对象不能删（正在执行的闭包就存在
 * `uvcpp_timer::timer_start_cb` 里），但**底层句柄仍然必须 `uv_close`**：
 * 只 `stop()` 不关的句柄既不是活跃也没在关，却照样挂在 `handle_queue` 上，而
 * `uv_loop_close()` 遍历的正是那个队列（`_local_deps/libuv/src/uv-common.c`），
 * 于是它**永远**返回 `UV_EBUSY`，`~uvcpp_loop` 只能按既有策略把整块
 * `uv_loop_t` 泄漏掉 —— 每超时一次赔一个循环。
 *
 * 这条断言是实测逼出来的，不是推出来的（`web_ssl_orphanprobe`，2026-09-20）：
 * 同一套服务端 + 同一个卡住的 ClientHello，只把"销毁发生在哪个回调里"换掉 ——
 *   超时回调（闸门响）：`uv_loop_close()` = -4082(EBUSY)，队列上剩下的孤儿是
 *                        `timer active=0 closing=0`；
 *   读回调（对端断开走同一条 `tls_fail`）：0，干净。
 * "连接被关掉了"那条断言对这两种情况**都是绿的**，所以它挡不住这个。
 *
 * 判据不是"跑一遍 `uv_loop_close()`"（那会把循环真的关掉，而本文件的
 * `uvcpp_tcp_server` / `uvcpp_tcp_client` 还都是栈对象，之后析构会碰一个已经
 * 销毁的循环），而是**同一件事的非破坏性形式**：`uv_loop_close()` 只查两样东西
 * —— `uv__has_active_reqs()` 与"`handle_queue` 上还有没有非 internal 句柄"，
 * 而 `uv_walk` 恰好只回调非 internal 的那些。两样分开断言，失败时还能把残留
 * 句柄的类型与 active/closing 标志打出来（孤儿自己报出身份）。
 */
#include <atomic>
#include <cstdio>
#include <string>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_OPENSSL_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>

#include <uv.h>

#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "  [FAIL] %s\n", what.c_str());
    ++g_failures;
  }
}

/// 一条真实 ClientHello 的开头：记录头 + 握手类型 + 长度 + 版本 + 随机数前
/// 几位。到这里就断掉 —— 长度字段声明的 0x0200 字节永远收不满。
const unsigned char kPartialHello[] = {
    0x16, 0x03, 0x01, 0x02, 0x00, 0x01, 0x00, 0x01, 0xfc, 0x03, 0x03};

struct handle_census {
  int non_internal = 0;
  std::string detail;
};

void count_handles(uv_handle_t* h, void* arg) {
  handle_census* c = reinterpret_cast<handle_census*>(arg);
  ++c->non_internal;
  if (!c->detail.empty()) c->detail += ", ";
  c->detail += "type=" + std::to_string(static_cast<int>(uv_handle_get_type(h))) +
                " active=" + std::to_string(uv_is_active(h)) +
                " closing=" + std::to_string(uv_is_closing(h));
}

}  // namespace

int main() {
  uvcpp_ssl_context ctx(tls_mode::SERVER, tls_version::TLS_1_2);
  check(ctx.is_ready(), "server SSL context not ready");
  check(ctx.generate_self_signed("localhost", 2048),
        "generate_self_signed failed");
  if (!ctx.is_ready()) return 2;

  // 注意：`uvcpp_tcp_server` 用的是 `default_loop()`，所以本文件**一个进程
  // 只跑这一个场景** —— 判据是"这个循环上还剩什么"，跑第二个场景就等于把前
  // 一个的残留算到它头上。
  uvcpp_tcp_server server;
  server.set_ssl_context(&ctx);
  server.set_tls_handshake_timeout_ms(300);

  std::atomic<int> delivered{0};
  check(server.bind("127.0.0.1", 0) == 0, "bind failed");
  check(server.listen([&delivered](uvcpp_tcp_client*) { ++delivered; }) == 0,
        "listen failed");

  struct sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int port = ntohs(name.sin_port);

  uvcpp_loop* loop = server.get_loop();

  // 对端故意**不开 TLS**：本用例要的就是"线上字节凑不成一次完整握手"这个状态，
  // 对服务端来说与一个真 TLS 客户端发到一半卡住完全一样。
  uvcpp_tcp_client peer(loop);
  std::atomic<bool> connected{false};
  std::atomic<bool> peer_saw_close{false};
  const int crc = peer.connect("127.0.0.1", port, [&](int st) {
    if (st != 0) return;
    connected.store(true);
    peer.read_start_events([&](uvcpp_tcp_client&, const net_read_result& r) {
      if (!r.is_data()) peer_saw_close.store(true);
    });
    peer.write(reinterpret_cast<const char*>(kPartialHello),
               sizeof(kPartialHello), [](int) {});
  });
  check(crc == 0, "connect() start failed: " + std::to_string(crc));

  check(uvcpp_test::wait_until(loop, [&] { return connected.load(); }, 3000),
        "peer never connected");

  // 让那半个 ClientHello 真的到达服务端并开始握手。
  uvcpp_test::pump_for(loop, 300);

  // 前置（`assert-test-preconditions`）：这条连接必须**还在握手里** ——
  // 只要它被交付给了上层，本用例后面的东西就都不是它在测的了。
  check(delivered.load() == 0,
        "the stalled connection was delivered to on_connection (" +
            std::to_string(delivered.load()) +
            ") — the handshake was not actually in progress, so this case "
            "proves nothing");

  // 闸门确实响了：服务端自己把这条连接关掉。裸服务端没有 idle 超时，对端也
  // 没有自己走，所以这个窗口内没有别的路径能解释它。
  check(uvcpp_test::wait_until(loop, [&] { return peer_saw_close.load(); }, 3000),
        "the peer never saw the server close it — the handshake timeout did "
        "not fire, so the cleanup path under test was never taken");

  // 对端句柄要关干净（它自己也是一个非 internal 句柄，留着会污染下面的判据）。
  // 已经有了就别再 close —— 对已关句柄调 uv_read_stop 是空指针解引用。
  uvcpp_tcp* ptcp = peer.get_tcp();
  if (ptcp != nullptr && ptcp->get_handle() != nullptr && !ptcp->is_closing()) {
    peer.close([] {});
  }
  uvcpp_test::wait_until(loop, [&] { return ptcp->get_handle() == nullptr; },
                         2000);
  check(ptcp->get_handle() == nullptr,
        "the peer's own handle never finished closing — the census below would "
        "be measuring the wrong thing");

  server.close_all_clients();
  server.stop([] {});
  uvcpp_test::pump_for(loop, 300);

  // 判据一：`handle_queue` 上没有非 internal 句柄。
  uv_loop_t* raw = reinterpret_cast<uv_loop_t*>(loop->get_handle());
  handle_census census;
  uv_walk(raw, count_handles, &census);
  check(census.non_internal == 0,
        std::to_string(census.non_internal) +
            " handle(s) left on the loop's handle queue after the handshake "
            "timeout fired — uv_loop_close() would return UV_EBUSY forever and "
            "the whole uv_loop_t leaks (" +
            census.detail + ")");

  // 判据二：`uv__has_active_reqs()` 那一半。两半合起来才是
  // `uv_loop_close() == 0`。
  check(uv_loop_alive(raw) == 0,
        "the loop is still alive (active handles or pending requests) after "
        "the handshake timeout fired — uv_loop_close() would return UV_EBUSY");

  if (g_failures == 0) {
    std::printf("[web_ssl_server_hs_timeout] ALL PASS\n");
    return 0;
  }
  std::printf("[web_ssl_server_hs_timeout] FAIL (%d checks)\n", g_failures);
  return 2;
}

#else

int main() {
  std::printf("[web_ssl_server_hs_timeout] SKIP (web or OpenSSL disabled)\n");
  return 0;
}

#endif
