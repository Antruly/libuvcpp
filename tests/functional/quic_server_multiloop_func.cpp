/**
 * @file tests/functional/quic_server_multiloop_func.cpp
 * @brief QUIC 扇出的**原语**：n 个端点各带 `SO_REUSEPORT` 抢同一个端口。
 *
 * 上一层（`uvcpp_http_server::set_loops(n)`）的 h3 扇出是**用这个原语拼出来的**
 * —— 每个工作循环上一个 `uvcpp_quic_server`，各绑各的 socket、各带那个标志、
 * 落在同一个 `(地址, 端口)` 上。本用例只测原语本身，不经过 web 层，所以它红了
 * 就是"扇出的地基坏了"，而不是"h3 层写错了"。
 *
 * 判据逐条（以及"坏实现会让它怎么红"）：
 *
 * 1. **不请求标志 = 今天的行为**：`set_reuseport()` 一次都没调时
 *    `is_reuseport()` 为假，第二个端点**绑不上同一个端口**（`listen()` 非零）
 *    —— 也就是说这条旋钮"没被请求就一点不动"，端口独占性没有被削弱。这条**每个
 *    平台都跑得到**，也正是回落那条腿的形状（见判据 5）。
 * 2. **请求了就得给**：`set_reuseport(true)` 之后 `is_reuseport()` 报真时，
 *    第 2..n 个端点绑同一个端口必须成功。报假时（判据 5）这条不成立，而那是
 *    平台的答复，不是缺陷。
 * 3. **分流真的发生了**：n 条连接之后，**非零的连线计数槽至少两个**。这是扇出
 *    唯一能证明自己的地方。**不拍均匀度阈值**（同 `tcp_multiloop_func.cpp:17-19`
 *    的立场：内核哈希不保证均匀，拿它当判据就是给 flaky 留门）。
 * 4. **计数守恒**：`Σ 各端点连线数 == 客户端条数` —— 一个"把每条连接都记到 0 号"
 *    的坏实现能过判据 3 的形状，过不了这一条。
 * 5. **平台回落**：`set_reuseport(true)` 但平台给不出标志时（Windows 上 libuv 在
 *    造 socket **之前**就返 `ERROR_NOT_SUPPORTED`；macOS 等 `uv__sock_reuseport()`
 *    返 `UV_ENOTSUP`；libuv < 1.50 根本没有 `UV_UDP_REUSEPORT`），`is_reuseport()`
 *    为假，于是 1..n−1 号端点也绑不上 —— **整条用例退化成判据 1 的形状**，并且
 *    2 条连接全部落在 0 号。判据一律**按 `is_reuseport()` 分叉**，不拿 `#ifdef`
 *    猜平台：那个值是一次真绑定探出来的，比任何编译期判断都准。
 * 6. **标志是 `bind()` 那一刻用的**：`listen()` 之后再 `set_reuseport(true)` 不
 *    生效（stderr 一行 + `is_reuseport()` 不变），别让"配了却没生效"变成静默。
 * 7. **每条等待带墙钟上限**，超时就**自报家门**（哪一条等不到）然后就地结束进程
 *    —— 不给 ctest 一个只有 `Timeout` 的红。
 * 8. **"回落"要是真的**：请求了标志、绑定**也成功了**，那 `is_reuseport()` 报假
 *    就只能是"标志被静默丢掉"。前七条**全部分叉在 `is_reuseport()` 上**，于是
 *    "悄悄把标志扔掉"与"平台真的给不出"在它们眼里**形状完全一样**（都诚实报假、
 *    都只有一个端点、连接都落在 0 号）—— 这一条用**裸 libuv** 自己问内核一次
 *    （`reuseport_probe.h` 的 `uvcpp_test::platform_honors_reuseport()`），把两者
 *    分开。坏实现
 *    （`bind_udp()` 里 `flags` 恒 0）在"内核给得出"的平台上红，在给不出的平台上
 *    这条自动跳过。
 *
 * **为什么不需要一个"强制回落"开关**：回落那条腿在本用例里就是**判据 1** ——
 * 不请求标志的那次绑定与"请求了但平台拒绝"之后的那次绑定是同一条代码路径
 * （`uvcpp_quic_server::impl::bind_udp()` 的第二个 `bind()`），断言形状也一样。
 * Linux CI 上照样跑得到，不必像 `uvcpp_tcp_server::set_handoff_forced()` 那样为
 * 它单开一个只给测试用的口子。
 *
 * 它换不来的是判决力：不给库开任何口子，测试能看到的就只有"这次绑定带没带上
 * 标志"这个**库自己报的**数 —— 所以判据 8 另加一条**库外**的平台能力探针，用来
 * 把"平台真的给不出"与"实现悄悄把标志丢了"分开（两者在前七条下形状一样）。
 *
 * **线程**：每个端点一条循环、一条线程，**由它自己的线程泵** —— 句柄只能在
 * `get_loop()` 那条线程上用；而且 ngtcp2 的时间戳取自 `uv_now()`、只在循环迭代
 * 时刷新，跨线程替它泵会撞上单调性断言（`doc/quic-guide.md` 里那条）。客户端
 * 同理，各自一条线程。分工与 `http3_multiloop_func.cpp` 一致。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#include "reuseport_probe.h"

#if UVCPP_QUIC_ENABLE

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_common.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_context.h>

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

void check_ne_i(long long got, long long unwanted, const std::string& what) {
  ++g_checks;
  if (got == unwanted) {
    std::cerr << "  [FAIL] " << what << "：不该等于 " << unwanted << std::endl;
    ++g_failures;
  }
}

long long elapsed_ms(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

/// 等一条判据成立；超时就自报家门后就地结束进程（见文件头判据 7）。
void require_within(const std::function<bool()>& pred, int ms,
                    const std::string& what) {
  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  while (!pred()) {
    if (elapsed_ms(t0) >= ms) {
      std::cerr << "  [FATAL] 超时 " << ms << " ms：等不到「" << what << "」"
                << std::endl;
      std::_Exit(2);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

// =========================================================================
// 端点：一条循环 + 一个 socket + 一条自己泵的线程
// =========================================================================

/// 一条连接的 deadline（回环上是毫秒级；5 s 是给 CI 最慢那条腿的余量）。
const int kConnectDeadlineMs = 5000;
/// 端点起监听 / 等判据的墙钟上界。
const int kWaitMs = 20000;

struct Endpoint {
  // --- 入参（线程起来之前填好）---
  int  bind_port      = 0;      ///< 0 = 让内核挑
  bool want_reuseport = false;

  // --- 出参（`ready` 置位之后才读）---
  int  listen_rc = 12345;       ///< 哨兵：绝不可能碰巧对上
  int  port      = 0;
  bool reuseport = false;

  /// 这个端点上建起来的连线数（`connection_cb` 每响一次加一）。
  std::atomic<int> conns{0};
  std::atomic<bool> stop{false};
  std::promise<void> ready;
  std::thread        th;
};

/**
 * @brief 端点线程体：建循环、建端点、起监听、然后**一直泵到 `stop`**。
 *
 * 循环与端点在函数作用域里，`srv` 后声明 ⇒ 先析构：端点要先把它的 QUIC 连接与
 * 定时器放掉，循环才轮到。
 */
void endpoint_thread(Endpoint* ep, uvcpp_ssl_context* ctx) {
  uvcpp_loop        loop;
  uvcpp_quic_server srv(&loop);

  srv.set_ssl_context(ctx);
  srv.set_reuseport(ep->want_reuseport);

  int rc = srv.bind("127.0.0.1", ep->bind_port);
  if (rc == 0) {
    rc = srv.listen([ep](uvcpp_quic_connection*) { ep->conns.fetch_add(1); });
  }
  ep->listen_rc = rc;
  ep->port      = srv.configured_port();
  ep->reuseport = srv.is_reuseport();
  ep->ready.set_value();

  while (!ep->stop.load()) {
    loop.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

/// 起一个端点线程。`bind_port` / `want_reuseport` 必须已经填好。
void start_endpoint(Endpoint* ep, uvcpp_ssl_context* ctx) {
  ep->th = std::thread(endpoint_thread, ep, ctx);
}

/// 等端点报出结果（`listen()` 的返回码与端口）。
void await_endpoint(Endpoint* ep, const std::string& what) {
  if (ep->ready.get_future().wait_for(std::chrono::milliseconds(kWaitMs)) !=
      std::future_status::ready) {
    std::cerr << "  [FATAL] 超时 " << kWaitMs << " ms：等不到「" << what
              << "」的 listen 结果" << std::endl;
    std::_Exit(2);
  }
}

/// 停一个端点线程并 join（析构 `Endpoint` 之前必须做）。
void stop_endpoint(Endpoint* ep) {
  ep->stop.store(true);
  if (ep->th.joinable()) ep->th.join();
}

// =========================================================================
// 客户端：一条循环 + 一条自己泵的线程
// =========================================================================

struct ClientResult {
  int  connect_status = 12345;
  bool returned       = false;  ///< `connect()` 自己受理了没有
};

/**
 * @brief 客户端线程体：连上之后**继续泵到 `release`**。
 *
 * 连上就停泵会让连接停在"刚发完首包"那一步，而这里要的是"连接真的建起来了、
 * 且停在对端那个端点上"，所以握手之后还要活着。
 */
void client_thread(uvcpp_ssl_context* ctx, int port, ClientResult* out,
                   std::atomic<bool>* release) {
  // **默认构造**（`UVCPP_DEFINE_FUNC` 那个 `explicit uvcpp_quic_client()`）= 自建
  // 一条循环、由本线程独占。传 `nullptr` 走的是"共享外部循环"那条，而
  // `impl_->loop == nullptr` 会让 `connect()` 直接返 `UV_EINVAL` —— 这里不共享。
  uvcpp_quic_client cli;
  cli.set_ssl_context(ctx);

  // **不能捕获栈上局部量的引用**：回调可能比那一帧活得久。`shared_ptr` 到堆上
  // 是这里唯一安全的形状（`http3_multiloop_func.cpp` 里踩过这个坑）。
  std::shared_ptr<std::atomic<int> > st(new std::atomic<int>(12345));
  if (cli.connect("127.0.0.1", port, [st](int s) { st->store(s); }) != 0) {
    // **要落一个不同于哨兵的值**：主线程等的是"`connect_status` 不再是 12345"，
    // 直接 return 会让它等满 20 s 再报 FATAL —— 一个正常的失败路径被写成了挂。
    out->returned       = false;
    out->connect_status = -1;
    return;
  }
  out->returned = true;

  uvcpp_loop* loop = cli.get_loop();
  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  while (st->load() == 12345 && elapsed_ms(t0) < kConnectDeadlineMs) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  out->connect_status = st->load();

  while (!release->load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
}

// =========================================================================
// 判据 1 / 5：不请求标志 = 端口独占（回落那条腿也是这个形状）
// =========================================================================

void test_shared_port_needs_the_flag() {
  std::cout << "  [独占] 不请求 REUSEPORT 时，第二个端点抢不到同一个端口"
            << std::endl;

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    std::cerr << "  [FAIL] 生成自签证书失败" << std::endl;
    ++g_failures;
    return;
  }

  Endpoint a;
  Endpoint b;
  a.bind_port      = 0;      // 内核挑
  a.want_reuseport = false;
  start_endpoint(&a, &server_ctx);
  await_endpoint(&a, "0 号端点");

  check_eq_i(a.listen_rc, 0, "不请求标志时 0 号端点 listen()");
  check(!a.reuseport, "不请求标志时 is_reuseport() == false");
  check(a.port > 0, "listen() 之后 configured_port() 报出真端口");

  b.bind_port      = a.port;  // 同一个端口
  b.want_reuseport = false;
  start_endpoint(&b, &server_ctx);
  await_endpoint(&b, "1 号端点");

  check_ne_i(b.listen_rc, 0, "同一个端口上第二个端点 listen()（不带标志）必须失败");
  check(!b.reuseport, "失败的那个端点 is_reuseport() == false");

  stop_endpoint(&b);
  stop_endpoint(&a);
}

// =========================================================================
// 判据 2 / 3 / 4：请求了标志 ⇒ n 个端点共占一个端口，连接按四元组分散
// =========================================================================

void test_fanout(int nendpoints, int nconn) {
  std::cout << "  [扇出] " << nendpoints << " 个端点抢一个端口，" << nconn
            << " 条连接" << std::endl;

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    std::cerr << "  [FAIL] 生成自签证书失败" << std::endl;
    ++g_failures;
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  // 用固定大小的数组而不是 vector：`Endpoint` 里有 `std::atomic` 与
  // `std::promise`（前者不可移动、后者不可拷贝），装进需要搬家的容器里是自找麻烦。
  Endpoint eps[4];
  if (nendpoints > 4) nendpoints = 4;

  // 0 号先起 —— 端口是它从内核那儿问来的，1..n−1 号要等这个数。
  eps[0].bind_port      = 0;
  eps[0].want_reuseport = true;
  start_endpoint(&eps[0], &server_ctx);
  await_endpoint(&eps[0], "0 号端点");

  check_eq_i(eps[0].listen_rc, 0, "请求标志时 0 号端点 listen()");
  if (eps[0].listen_rc != 0) {
    stop_endpoint(&eps[0]);
    return;
  }

  const int port = eps[0].port;
  check(port > 0, "0 号端点报出真端口");

  for (int i = 1; i < nendpoints; ++i) {
    eps[i].bind_port      = port;
    eps[i].want_reuseport = true;
    start_endpoint(&eps[i], &server_ctx);
  }
  for (int i = 1; i < nendpoints; ++i) {
    await_endpoint(&eps[i], "后起的端点");
  }

  // ---- 判据 2 / 5：按**这一次绑定的结果**分叉，不猜平台 ----
  const bool fanout = eps[0].reuseport;
  std::cout << "  [扇出] 腿 = "
            << (fanout ? "REUSEPORT 内核分流" : "单端点（平台拒绝标志 ⇒ 回落）")
            << std::endl;

  // ---- 判据 8：回落是不是真的（"标志被静默丢掉"与"平台拒绝"必须分得开）----
  //
  // 只有"内核给得出"的平台上这条才说话：那种平台上 `set_reuseport(true)` +
  // 绑定成功 + `is_reuseport()` 报假，三件事凑在一起就只能是标志没被带上。
  // 给不出的平台上（Windows/macOS/老 libuv）这条自动跳过 —— 回落本来就是对的。
  const bool platform_can = uvcpp_test::platform_honors_reuseport();
  std::cout << "  [扇出] 裸 libuv 探针（同端口带标志绑两次）："
            << (platform_can ? "都成功 ⇒ 这个平台上分流成立" : "不成立 ⇒ 平台给不出")
            << std::endl;
  if (platform_can) {
    check(eps[0].reuseport,
          "内核给得出 REUSEPORT 时，请求了标志就必须真的带上（报假 = 标志被"
          "静默丢掉）");
  }

  if (fanout) {
    for (int i = 1; i < nendpoints; ++i) {
      check_eq_i(eps[i].listen_rc, 0, "带标志的第 i 个端点 listen() 成功");
      check(eps[i].reuseport, "每个端点的 is_reuseport() 都是 true");
    }
  } else {
    // 平台给不出标志 ⇒ 0 号独占端口，后起的必须绑不上（判据 5）。
    for (int i = 1; i < nendpoints; ++i) {
      check_ne_i(eps[i].listen_rc, 0,
                 "平台拒绝标志时第 i 个端点 listen() 必须失败（回落形状）");
      check(!eps[i].reuseport, "回落时 is_reuseport() == false");
    }
  }

  // 回落档上没有"分流"可测（就一个端点），但**连接还得照建** —— 否则这条用例在
  // Windows/macOS 上等于什么都没验。所以两个档都打连接，只是条数不同。
  const int nclients = fanout ? nconn : 2;
  std::vector<std::unique_ptr<ClientResult> > res;
  std::vector<std::thread>                    clients;
  std::atomic<bool>                           release{false};
  for (int i = 0; i < nclients; ++i) {
    res.push_back(std::unique_ptr<ClientResult>(new ClientResult()));
    clients.push_back(std::thread(client_thread, &client_ctx, port,
                                  res[static_cast<size_t>(i)].get(), &release));
  }

  for (int i = 0; i < nclients; ++i) {
    require_within(
        [&res, i]() { return res[static_cast<size_t>(i)]->connect_status != 12345; },
        kWaitMs, "第 i 条客户端拿到握手结果");
  }

  int handshaken = 0;
  for (int i = 0; i < nclients; ++i) {
    const ClientResult* r = res[static_cast<size_t>(i)].get();
    check(r->returned, "connect() 受理了第 i 条连接");
    if (r->connect_status == 0) ++handshaken;
  }
  check_eq_i(handshaken, nclients, "全部连接都握手成功");

  // ---- 判据 3 / 4：等你**读之前**两个数都到齐 ----
  require_within(
      [&eps, nendpoints, nclients]() {
        int sum = 0;
        for (int i = 0; i < nendpoints; ++i) sum += eps[i].conns.load();
        return sum >= nclients;
      },
      kWaitMs, "全部连接都登记到某个端点上");

  int sum     = 0;
  int nonzero = 0;
  std::string shown;
  for (int i = 0; i < nendpoints; ++i) {
    const int c = eps[i].conns.load();
    sum += c;
    if (c > 0) ++nonzero;
    shown += " " + std::to_string(c);
  }
  std::cout << "  [扇出] 各端点连线数:" << shown << "（合计 " << sum << "）"
            << std::endl;

  check_eq_i(sum, nclients, "Σ 各端点连线数 == 客户端条数");

  if (fanout) {
    check(nonzero >= 2, "非零的连线计数槽 ≥ 2（内核分流真的发生了）");
  } else {
    check_eq_i(nonzero, 1, "回落档上全部连接都在 0 号端点");
    check_eq_i(eps[0].conns.load(), nclients, "回落档上 0 号端点的连线数");
  }

  // 收尾：先放客户端，再停端点（端点析构时那些连接还在，正是要压的形状）。
  release.store(true);
  for (size_t i = 0; i < clients.size(); ++i) {
    if (clients[i].joinable()) clients[i].join();
  }
  for (int i = 0; i < nendpoints; ++i) stop_endpoint(&eps[i]);
}

// =========================================================================
// 判据 6：标志是 bind() 那一刻用的
// =========================================================================

void test_flag_after_listen_is_ignored() {
  std::cout << "  [闸门] listen() 之后再 set_reuseport() 不生效" << std::endl;

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    std::cerr << "  [FAIL] 生成自签证书失败" << std::endl;
    ++g_failures;
    return;
  }

  uvcpp_loop        loop;
  uvcpp_quic_server srv(&loop);
  srv.set_ssl_context(&server_ctx);
  check_eq_i(srv.bind("127.0.0.1", 0), 0, "bind()");
  check_eq_i(srv.listen([](uvcpp_quic_connection*) {}), 0, "listen()");
  check(!srv.is_reuseport(), "没请求标志时 is_reuseport() == false");

  // 这一句会在 stderr 上留一行说明（"装晚了会被忽略"），正是要它别静默。
  srv.set_reuseport(true);
  check(!srv.is_reuseport(),
        "listen() 之后 set_reuseport(true) 不改变已经绑好的形状");
}

}  // namespace

int main() {
  std::cout << "[functional quic_server_multiloop] start" << std::endl;

  // ngtcp2 的 crypto 后端是**进程级**的，在任何端点与任何 TLS 上下文之前。
  if (quic_crypto_backend_init() != 0) {
    std::cerr << "  [FAIL] quic_crypto_backend_init()" << std::endl;
    return 2;
  }

  test_shared_port_needs_the_flag();
  test_fanout(4, 8);
  test_flag_after_listen_is_ignored();

  quic_crypto_backend_free();

  std::cout << "[functional quic_server_multiloop] checks=" << g_checks
            << " failures=" << g_failures << std::endl;
  if (g_failures == 0) {
    std::cout << "[functional quic_server_multiloop] done success=true"
              << std::endl;
    return 0;
  }
  std::cout << "[functional quic_server_multiloop] done success=false"
            << std::endl;
  return 2;
}

#else  // UVCPP_QUIC_ENABLE

// 关掉 quic 模块时这个文件不该被编译（`tests/functional/CMakeLists.txt` 的
// 过滤器按文件名摘掉它 —— 规则是 `quic`）。真编到了就是配置错，返回非零而不是
// 打一句 SKIP 再返回 0：后者会把"这个模块一次都没被跑过"伪装成"全绿"。
int main() {
  std::cerr << "[quic_server_multiloop] UVCPP_QUIC_ENABLE=0 —— 这个测试文件不该"
               "被编译进来"
            << std::endl;
  return 2;
}

#endif  // UVCPP_QUIC_ENABLE
