/**
 * @file tests/functional/h2_flush_reject_func.cpp
 * @brief `uvcpp_h2_connection::flush()` 里"写没进队列"那条路：**已经 drain
 *        出去的帧不能丢**。
 *
 * 为什么单开一条：`flush()` 的 `wr != 0` 分支在仓库里**够不着** —— h2 层是
 * 那个 `uvcpp_tcp_client` 上唯一的异步写使用者，`writing_`（本层）与
 * `has_async_write_cb_`（tcp 层）一对一，所以 UV_EALREADY 不会自己出现。但
 * "够不着"不等于"不成立"：那个分支里 `out_.clear()` 已经把帧清掉了，一旦真被
 * 走到，`drain()` 是**消费式**的（`nghttp2_session_mem_send2` 取走即从出站
 * 队列消失）⇒ 这些帧的**唯一副本**没了，而它们的 `done` 早已带着 0（成功）
 * 躺在会话的 `completed` 里 —— 线上一个字节都没有，发起方却收到"发出去了"。
 *
 * 装置（确定性，不靠时序也不靠 socket 缓冲大小）：libuv 的**异步写完成必然
 * 推迟到下一轮循环**，所以只要在 `start()` 之前先占住传输层一笔异步写，
 * `start()` 里那次 `flush()` 就**一定**撞上 UV_EALREADY。判据落在"服务端最终
 * 有没有收到 h2 客户端前言"上 —— 前言正是 `start()` 那次 `flush()` drain 出来
 * 的东西。
 *
 * **不上 TLS**，与 `h2_session_func` 不上 socket 是同一个理由：判据落在传输层
 * 的"写没进队列"上，掺进 OpenSSL 只会把"字节没出去"和"握手没成"混在一起。
 * `start()` 本身不校验 ALPN（`client_` 非空、`read_start_events` 成功即可），
 * 所以要的只是"传输层被占住"这一个条件。
 *
 * 三处断言缺一不可：
 *   1. **前提**：`start()` 的返回值必须**就是** UV_EALREADY。不是的话说明装置
 *      没造出"写没进队列"，这条用例会退化成"普通 h2 启动能发字节" —— 那在
 *      有缺陷的实现上**照样绿**。前提不成立按 `fs_async_reuse_func` 的规矩返 3。
 *   2. **量具自检**：占位那笔写的载荷必须到达服务端。否则"没收到前言"完全可能
 *      是"服务端一个字节都没收到"（记录没装上 / 连接压根没通）。
 *   3. **判据**：服务端收到了 h2 客户端前言。
 */
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_NGHTTP2_ENABLE

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_tcp.h>
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <req/uvcpp_write.h>
#include <uvcpp/uvcpp_buf.h>

#include <http2/uvcpp_h2_connection.h>

#include "loop_drain.h"
#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (cond) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// h2 客户端前言（RFC 9113 §3.4）。`start()` 那次 `flush()` 的产物里必有它。
const char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/// 占住传输层那笔写的载荷。取一个在前言里**不会出现**的串，免得两条判据串味。
const char kOccupy[] = "OCCUPY-MARKER-9f3a";

/// 服务端收到的全部字节。单线程（客户端与服务端同一条循环），不用锁。
std::string g_got;

}  // namespace

int main() {
  std::cout << "[functional h2_flush_reject] start" << std::endl;

  uvcpp_loop              loop;
  uvcpp_test::loop_drain  drain_guard(&loop);
  loop.init();

  // -----------------------------------------------------------------
  // 服务端：裸 uvcpp_tcp，只收不答
  // -----------------------------------------------------------------
  uvcpp_tcp server(&loop);
  if (server.bindIpv4("127.0.0.1", 0) != 0) {
    std::cerr << "[functional h2_flush_reject] bind failed" << std::endl;
    return 3;
  }
  sockaddr_in name;
  int         namelen = sizeof(name);
  server.getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int port = ntohs(name.sin_port);

  uvcpp_tcp* srv_peer = nullptr;
  const int  lrc      = server.listen(
      [&srv_peer, &loop](uvcpp_stream* s, int status) {
        if (status != 0) return;
        srv_peer = new uvcpp_tcp(&loop);
        s->accept(srv_peer);
        srv_peer->read_start(
            [](uvcpp_handle*, size_t suggested, uv_buf_t* buf) {
              uvcpp_buf::alloc_buf(buf, suggested);
            },
            [](uvcpp_stream*, ssize_t nread, const uv_buf_t* buf) {
              if (nread > 0) g_got.append(buf->base, static_cast<size_t>(nread));
              uvcpp_free_bytes(buf->base);
            });
      },
      128);
  if (lrc != 0) {
    std::cerr << "[functional h2_flush_reject] listen failed " << lrc
              << std::endl;
    return 3;
  }

  // -----------------------------------------------------------------
  // 客户端
  //
  // 声明次序是有意的（析构逆序）：`conn` 必须在 `client` **之前**声明 ——
  // 它在 `start()` 里把自己的读回调挂在 `client` 上，客户端还没拆干净就先删
  // h2 层的话，那之后的任何一次读都会进已释放的对象。
  // -----------------------------------------------------------------
  std::unique_ptr<uvcpp_h2_connection> conn;
  uvcpp_tcp_client                     client(&loop);

  bool              connected     = false;
  int               connect_rc    = 0;
  int               start_rc      = 0;
  std::atomic<bool> occupy_done{false};
  int               occupy_rc = 0;

  const int crc = client.connect("127.0.0.1", port, [&](int status) {
    connect_rc = status;
    connected  = status == 0;
    if (status != 0) return;

    // 占住传输层。`write(const char*, size_t, cb)` 那条重载在 TLS 关着时是
    // **异步**的：libuv 的完成回调必然推迟到下一轮循环，所以在本次回调返回
    // 之前 `has_async_write_cb_` 一直是立着的。
    std::string marker(kOccupy);
    const int   orc = client.write(marker.data(), marker.size(), [&](int st) {
      occupy_rc   = st;
      occupy_done = true;
    });
    if (orc != 0) {
      // 没受理就没有完成回调，别在这儿干等。
      occupy_rc   = orc;
      occupy_done = true;
      std::cerr << "  [note] occupy write rejected " << orc << std::endl;
      return;
    }

    conn.reset(new uvcpp_h2_connection(&client, /*server_side=*/false));

    uvcpp_h2_session::callbacks h2c;
    h2c.on_fatal = [](uvcpp_h2_session&, int code) {
      std::cerr << "  [note] h2 fatal " << code << std::endl;
    };
    uvcpp_h2_connection::callbacks cc;
    cc.on_disconnect = [](uvcpp_h2_connection&) {};

    // 这里就该拿到 UV_EALREADY —— 传输层被上面那笔占着。
    start_rc = conn->start(h2c, cc);
  });

  if (crc != 0) {
    std::cerr << "[functional h2_flush_reject] connect() start failed " << crc
              << std::endl;
    return 3;
  }

  const bool ok_connect = uvcpp_test::wait_until(
      &loop, [&] { return connected && occupy_done.load(); },
      uvcpp_test::kWaitMs);
  if (!ok_connect) {
    std::cerr << "[functional h2_flush_reject] connect/occupy timed out"
              << std::endl;
    return 3;
  }

  // ---- 前提：装置真的造出了"写没进队列" ----
  std::cout << "  [info] connect_rc=" << connect_rc << " start_rc=" << start_rc
            << " occupy_rc=" << occupy_rc << std::endl;
  if (start_rc != UV_EALREADY) {
    std::cerr << "[functional h2_flush_reject] 前提不成立：start() 返回 "
              << start_rc << "，期望 UV_EALREADY(" << UV_EALREADY
              << ")。装置没造出「写没进队列」，这条用例判不了。\n";
    return 3;
  }

  // 占位那笔写已经完成，传输层空出来了。此刻把 h2 层攒下的帧再冲一次 ——
  // 现实里这一步由下一次 `on_read` 触发（见用例说明）。
  conn->flush();

  const bool ok_preface = uvcpp_test::wait_until(
      &loop, [&] { return g_got.find(kPreface) != std::string::npos; },
      uvcpp_test::kWaitMs);

  // ---- 量具自检：服务端确实收到了东西 ----
  check(g_got.find(kOccupy) != std::string::npos,
        "占位写的载荷到达了服务端（量具自检）");

  std::cout << "  [info] 服务端收到 " << g_got.size() << " 字节，前言="
            << (ok_preface ? "有" : "无") << std::endl;

  // ---- 判据 ----
  check(ok_preface, "写没进队列之后，drain 出去的帧仍然发得出去（h2 前言到达）");

  // -----------------------------------------------------------------
  // 收摊：句柄先关、循环后关
  // -----------------------------------------------------------------
  conn.reset();
  client.close([] {});
  if (srv_peer != nullptr) {
    srv_peer->close([](uvcpp_handle*) {});
  }
  server.close([](uvcpp_handle*) {});
  uvcpp_test::wait_until(&loop, [] { return false; }, 100);

  std::cout << "[functional h2_flush_reject] done failures=" << g_failures
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}

#else

int main() {
  std::cout << "[functional h2_flush_reject] [skip] nghttp2 disabled"
            << std::endl;
  return 0;
}

#endif  // UVCPP_NGHTTP2_ENABLE
