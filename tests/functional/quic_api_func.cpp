/**
 * @file tests/functional/quic_api_func.cpp
 * @brief QUIC 传输层 1.4.1 的**契约**用例：形状在不在、后端链上没有、谎有没有说。
 * @author zhuweiye
 * @version 1.4.1
 *
 * **这个用例在 1.4.1 之前测的是"QUIC 还跑不了这件事是被写下来的"**：那时动作
 * 一律返回 `UV_ENOSYS`、回调一条都不许响。文件头写着"哪天真实现了，用例会红，
 * 逼着实现者显式回来改契约"。传输落地之后它红了，这个文件就是那次改契约的结果。
 *
 * 现在它管三类事：
 *
 * 1. **后端真链上了**（`test_backend_is_really_linked`）。三条证据分别压在
 *    ngtcp2 的头路径、`ngtcp2_static` 的符号、`ngtcp2_crypto_ossl_static` 的
 *    符号上。第三条尤其重要 —— 那一半取决于"那份 OpenSSL 是不是带 QUIC API 的
 *    mainline ≥ 3.2"，是整条接线里最容易坏的地方。**光有
 *    `quic_ngtcp2_version_string()` 不算证据**：它读的是一个编译期宏，一个
 *    "头骗到了、库没链上"的树照样能跑绿它。
 * 2. **没挂到连接上的那个壳，说的是实话**（`test_connection_shell`）。一个默认
 *    构造的 `uvcpp_quic_connection` 背后**没有内核**，于是
 *    `open_stream()` / `write_stream()` / `close()` 一律返回 `UV_ENOTCONN` ——
 *    不是 `UV_ENOSYS`。"功能没实现"这句在 1.4.1 已经不成立了，"这个对象没挂到
 *    连接上"才是。这条区别是**故意**用同一个词测出来的：把 `UV_ENOTCONN` 换成
 *    `UV_ENOSYS`（或反过来）都会红。
 * 3. **`connect()` / `listen()` 返回负值就是没收下，回调一次都不许排**
 *    （`test_endpoint_contract`）。一个接口收下回调、返回 0、然后永远不回调，
 *    在外观上与"正在工作"完全一样，而调用方会把整条超时路径压在那条依赖上 ——
 *    本仓库里最难查的一类挂死。所以"没返 0 就不许回调"这条要**在返负值的那条
 *    路上也成立**（没设 TLS 上下文 → `UV_EINVAL`）。
 * 4. **读侧收尾说的必须是真话**（`test_read_side_termination`，1.4.1 Q5 加的）。
 *    `net_read_result::fin` 这一位把"对端说完了"与"对端不要了"分开 —— 而这两件
 *    事在事件名上是**一样**的（都是 `PEER_CLOSED`）。HTTP/3 那一层要靠它选路
 *    （喂 FIN 还是关流），所以它红的时候不是"少了一条断言"，是上层会写错。
 *    同一节还钉住 `streams_left()` 真问到了内核、以及 `on_stop_sending` 是
 *    **另一个方向**的通知（收它不产生读侧收尾）。
 *
 * 第 3 条怎么才能**不是空断言**：光"我没拨循环所以回调没跑"是没意义的。所以
 * 用例在这期间**真拨**循环，并且用一个 10ms 定时器**证明这段时间里循环确实在
 * 被拨**（`pumped`）。定时器触发了而那些回调一条都没响，那句话才成立。
 *
 * **这个文件里没有看门狗。** 同目录的 `*_func.cpp` 有一多半带一只，用来兜住
 * "回调永远不来"那类挂死。这里不需要：`loop_drain.h` 的 `drain()` 是**有界**的
 * （最多 256 轮 `UV_RUN_NOWAIT`），`wait_util.h` 的 `wait_until` 每个都带
 * deadline，`loop_drain` 还是 RAII —— 本文件里没有任何一处可能无限等待。
 * 真会挂死的那些路径（握手、收流）在 `quic_handshake_func.cpp` 与
 * `quic_stream_func.cpp` 里测，那两个文件里的 `wait_until` 也全都带 5 秒
 * deadline —— 第 5、6 节那几段真连接也是（复用同一个 `kCloseDeadlineMs`）。
 */
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_QUIC_ENABLE

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
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

int g_checks = 0;
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
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_eq_s(const std::string& got, const std::string& want,
                const std::string& what) {
  ++g_checks;
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: [" << want
              << "]\n         实际: [" << got << "]" << std::endl;
    ++g_failures;
  }
}

/**
 * @brief 版本串是不是 `a.b.c` 且三段全数字。
 *
 * 只钉**形状**，不钉具体的号：`NGTCP2_VERSION` 来自 ngtcp2 自己的
 * `project(ngtcp2 VERSION x.y.z)`，将来在本库 `CMakeLists.txt` 里把
 * `NGTCP2_VERSION` 那个 cache 变量往上抬一档时，这个文件**不该跟着改**。
 * 会红的是形状 —— 空串、只剩一个分量、或者哪来的占位符（`@PACKAGE_VERSION@`
 * 之类没被 configure 掉的东西），那才是真出错。
 *
 * @param[out] major 解析成功时写入主版本号。
 */
bool version_shaped(const std::string& v, int* major) {
  if (v.empty()) return false;
  const size_t first = v.find('.');
  if (first == std::string::npos) return false;
  const size_t second = v.find('.', first + 1);
  if (second == std::string::npos) return false;
  if (v.find('.', second + 1) != std::string::npos) return false;

  const size_t bounds[3][2] = {
      {0, first}, {first + 1, second}, {second + 1, v.size()}};
  for (size_t p = 0; p < 3; ++p) {
    const size_t from = bounds[p][0];
    const size_t to   = bounds[p][1];
    if (to <= from) return false;  // 空分量（`1..0` 这种）
    for (size_t i = from; i < to; ++i) {
      if (std::isdigit(static_cast<unsigned char>(v[i])) == 0) return false;
    }
  }
  *major = std::atoi(v.substr(0, first).c_str());
  return true;
}

// =========================================================================
// 1. 后端自述：三条链接证据
// =========================================================================

void test_backend_is_really_linked() {
  std::cout << "  -- 后端自述" << std::endl;

  const std::string ver = quic_ngtcp2_version_string();
  int major = -1;
  check(version_shaped(ver, &major),
        "ngtcp2 版本串形如 a.b.c 且全数字，实际 [" + ver + "]");
  // 下界只为挡住"链到了一份古老的 ngtcp2"。取 1 是因为 0.x 从没有过
  // QUIC-API 时代的那套 crypto 回调，链上它上面两条断言应当已经先红了。
  check(major >= 1, "ngtcp2 主版本 >= 1，实际 " + std::to_string(major));

  // **第二条证据**：`ngtcp2_strerror` 是 libngtcp2 导出的符号。它真调进去了，
  // 少链 `ngtcp2_static` 这个 TU 就编不出可执行文件（未定义符号），用例在
  // 链接期红，而不是运行期跑出一句"没链上"。
  check_eq_s(quic_error_string(0), "NO_ERROR",
             "ngtcp2_strerror(0) 的原文（上游的字面量，改了就说明换库了）");
  check(!quic_error_string(0).empty(), "错误码文本非空");

  // **第三条证据**：`ngtcp2_crypto_ossl_static` 那一半。它不是"再验一遍前一条"
  // —— 那是另一个目标，而且是取决于 OpenSSL 版本的那一个。
  //
  // init/free 成对调两次：一次往返只能说明"这两个符号在"，两次能顺带说明
  // `free()` 真把状态清干净了（第二次 fetch 不会撞上半释放的静态指针）。
  // **不要**连着调两次 init —— 上游不查重，那样会丢引用计数，头文件里写了。
  for (int round = 0; round < 2; ++round) {
    const int rv = quic_crypto_backend_init();
    check_eq_i(rv, 0, "quic_crypto_backend_init() 第 " + std::to_string(round + 1) +
                          " 轮返回 0");
    quic_crypto_backend_free();
  }
  // 可重复：上游每个指针释放后都置 NULL，所以多调一次是无害的空操作。
  quic_crypto_backend_free();
  check(true, "quic_crypto_backend_free() 可重复调用");
}

// =========================================================================
// 2. 默认 ALPN
// =========================================================================

void test_default_alpn() {
  std::cout << "  -- 默认 ALPN" << std::endl;
  check_eq_s(std::string(quic_default_alpn()), "h3",
             "默认 ALPN 是 h3（RFC 9114 §3.1）");
}

// =========================================================================
// 3. 没挂到连接上的那个壳
// =========================================================================

void test_connection_shell() {
  std::cout << "  -- 连接壳（没有内核）" << std::endl;

  uvcpp_quic_connection conn;

  // 没有内核 → `IDLE`。这里钉的不是"状态机恒为 IDLE"（从前那条已经作废），
  // 而是"**没挂上去**的连接报 IDLE" —— 挂上去之后它就会跟着内核走，
  // 那一半在握手/流两个用例里测。
  check(conn.state() == quic_connection_state::IDLE,
        "没挂到端点的壳，state() == IDLE");
  check_eq_s(conn.alpn_selected(), "",
             "没挂到端点的壳，alpn_selected() 是空串（不是猜的默认值）");

  // 回调装上去不崩，且**一条都不会被调**（这个壳没有循环、没有内核）。
  uvcpp_quic_connection::callbacks cbs;
  cbs.on_read = [](uvcpp_quic_connection&, int64_t, const net_read_result&) {};
  cbs.on_stream_open = [](uvcpp_quic_connection&, int64_t) {};
  cbs.on_write = [](uvcpp_quic_connection&, int64_t, int) {};
  cbs.on_alpn = [](uvcpp_quic_connection&, const std::string&) {};
  cbs.on_close = [](uvcpp_quic_connection&, int) {};
  conn.set_callbacks(cbs);
  check(true, "set_callbacks() 收下五个回调");

  // **`UV_ENOTCONN` 而不是 `UV_ENOSYS`** —— 见文件头第 2 条。下面这几条断言
  // 与 1.4.0 那版逐字相同、期望值不同，那个差别就是"传输实现了"这件事在
  // 契约上的化身。
  check_eq_i(conn.open_stream(true), UV_ENOTCONN,
             "open_stream(双向) == UV_ENOTCONN");
  check_eq_i(conn.open_stream(false), UV_ENOTCONN,
             "open_stream(单向) == UV_ENOTCONN");
  check_eq_i(conn.write_stream(0, "x", 1, false), UV_ENOTCONN,
             "write_stream() == UV_ENOTCONN");
  check_eq_i(conn.write_stream(0, nullptr, 0, true), UV_ENOTCONN,
             "write_stream(空块收尾) == UV_ENOTCONN");
  check_eq_i(conn.shutdown_stream(0), UV_ENOTCONN,
             "shutdown_stream() == UV_ENOTCONN");
  // 1.4.1 Q5 加进来的两条：带错误码的 shutdown_stream（默认实参不改变上面那条
  // 断言的意思）与关读方向的 shutdown_stream_read。两条都是"没有内核"，
  // 与 `UV_ENOSYS` 是两句话。
  check_eq_i(conn.shutdown_stream(0, 5), UV_ENOTCONN,
             "shutdown_stream(id, 错误码) == UV_ENOTCONN");
  check_eq_i(conn.shutdown_stream_read(0), UV_ENOTCONN,
             "shutdown_stream_read() == UV_ENOTCONN");
  check_eq_i(conn.shutdown_stream_read(0, 7), UV_ENOTCONN,
             "shutdown_stream_read(id, 错误码) == UV_ENOTCONN");
  // 流额度同理：没有内核就没有额度可报，报 **0** 而不是"随便一个大数"或 -1
  // —— 返回类型是无符号的，负数表达不了"不知道"。
  check_eq_i(conn.streams_left(true), 0, "没有内核时 streams_left(双向) == 0");
  check_eq_i(conn.streams_left(false), 0, "没有内核时 streams_left(单向) == 0");
  check_eq_i(conn.close(), UV_ENOTCONN, "close() == UV_ENOTCONN");
  check_eq_i(conn.close(42), UV_ENOTCONN, "close(error_code) == UV_ENOTCONN");
}

// =========================================================================
// 4. 客户端 / 服务端契约（共用一条外部循环）
// =========================================================================

void test_endpoint_contract(uvcpp_loop& loop) {
  std::cout << "  -- 端点契约（共享循环）" << std::endl;

  bool any_cb_fired = false;
  const auto mark = [&any_cb_fired]() { any_cb_fired = true; };

  // 块作用域：客户端的析构发生在这里，而下面的 `loop_alive()` 断言在那之后 ——
  // "共享循环没被我关掉"这句话必须**在析构之后**才有意义。
  {
    uvcpp_quic_client client(&loop);
    uvcpp_quic_server server(&loop);

    check(client.get_loop() == &loop, "客户端回的是传进去的那条循环");
    check(server.get_loop() == &loop, "服务端回的是传进去的那条循环");

    // 配置项是真实现：设了不崩，且不影响下面任何一条判据。
    client.set_ssl_context(nullptr);
    client.set_alpn_protos({std::string(quic_default_alpn())});
    client.set_alpn_protos({});  // "一个都不发"，与"没设过"是两件事
    server.set_ssl_context(nullptr);
    server.set_alpn_select_protos({std::string(quic_default_alpn())});
    server.set_idle_timeout(30000);
    client.set_idle_timeout(30000);

    // **没设 TLS 上下文就是没配。** QUIC 没有明文模式（ALPN 是 TLS 扩展），
    // 退化成一个不加密的端点不是"宽容"，是假装成功 —— 所以这条路返回
    // `UV_EINVAL`，而不是建一条谁都认的裸连接。
    check_eq_i(client.connect("127.0.0.1", 1, [&mark](int) { mark(); }),
               UV_EINVAL, "没设 TLS 上下文时 client.connect() == UV_EINVAL");
    // 主机名这条也要试：解析那一步在 TLS 校验**之后**，不能因为"名字看起来对"
    // 就绕过了上面那条判据。
    check_eq_i(client.connect("localhost", 1, [&mark](int) { mark(); }),
               UV_EINVAL, "没设 TLS 上下文时 connect(主机名) == UV_EINVAL");
    // 端口范围与空主机名同样在"一切失败都在返回负值的那条路上解决"之内。
    check_eq_i(client.connect("127.0.0.1", -1, [&mark](int) { mark(); }),
               UV_EINVAL, "connect(端口 -1) == UV_EINVAL");
    check_eq_i(client.connect("127.0.0.1", 65536, [&mark](int) { mark(); }),
               UV_EINVAL, "connect(端口 65536) == UV_EINVAL");
    check_eq_i(client.connect(nullptr, 1, [&mark](int) { mark(); }), UV_EINVAL,
               "connect(host == nullptr) == UV_EINVAL");
    check(client.connection() == nullptr,
          "connect() 没返 0 时 connection() 是空的（没建出半条连接）");
    // 连不上就没得关 —— 这一条与上面那句 `UV_ENOTCONN` 是同一个道理。
    check_eq_i(client.close(), UV_ENOTCONN,
               "没连接时 client.close() == UV_ENOTCONN");

    check_eq_i(server.listen([&mark](uvcpp_quic_connection*) { mark(); }),
               UV_EINVAL, "没设 TLS 上下文时 server.listen() == UV_EINVAL");

    // ---- bind：文件里唯一的"纯登记"动作，所以判据要真的分得开好坏 ----
    check_eq_i(server.bind("127.0.0.1", 5555), 0, "bind(合法 IPv4) == 0");
    check_eq_s(server.configured_ip(), "127.0.0.1", "configured_ip() 回登记值");
    check_eq_i(server.configured_port(), 5555, "configured_port() 回登记值");

    // 非法输入必须**当场**被拒。这四条是 `bind()` 值得被当成真实现的理由：
    // 它们经 `uv_inet_pton()` 走了一遍，而不是"存下来就算数"。
    check_eq_i(server.bind("999.1.1.1", 5555), UV_EINVAL,
               "bind(非法 IPv4 字面量) == UV_EINVAL");
    check_eq_i(server.bind("not-an-address", 5555), UV_EINVAL,
               "bind(非地址字符串) == UV_EINVAL");
    check_eq_i(server.bind("127.0.0.1", -1), UV_EINVAL,
               "bind(端口 -1) == UV_EINVAL");
    check_eq_i(server.bind("127.0.0.1", 65536), UV_EINVAL,
               "bind(端口 65536) == UV_EINVAL");

    // 失败**不改变**已经登记好的东西：一半新一半旧是最难查的状态。
    check_eq_s(server.configured_ip(), "127.0.0.1",
               "失败之后 configured_ip() 没被改脏");
    check_eq_i(server.configured_port(), 5555,
               "失败之后 configured_port() 没被改脏");

    // 端口 0 是合法的（"由内核挑"），别把它当成"没设过"给拒了。
    check_eq_i(server.bindIpv4("0.0.0.0", 0), 0, "bindIpv4(端口 0) == 0");
    // 但 `listen()` 之前它**报的就是 0** —— "内核挑了哪个"要 listen 之后才知道。
    // 这一条钉的是"`configured_port()` 不做无中生有的猜测"：它报的要么是登记值，
    // 要么是内核真给的那个，唯独不是"顺手挑一个看起来合理的"。
    check_eq_i(server.configured_port(), 0,
               "登记为 0 且还没 listen() 时，configured_port() 也是 0");
    check_eq_i(server.bindIpv4(nullptr, 0), 0, "bindIpv4(nullptr) == 0（通配）");
    check_eq_s(server.configured_ip(), "0.0.0.0",
               "bindIpv4(nullptr) 回落到通配地址");
    check_eq_i(server.bindIpv6("::1", 5555), 0, "bindIpv6(合法 IPv6) == 0");
    check_eq_i(server.bindIpv6("127.0.0.1", 5555), UV_EINVAL,
               "bindIpv6(给的是 IPv4) == UV_EINVAL —— 显式的那两个要认族");
    check_eq_i(server.bindIpv4("::1", 5555), UV_EINVAL,
               "bindIpv4(给的是 IPv6) == UV_EINVAL");

    // ---- 这一串失败之后，回调一次都不许响 ----
    //
    // `pump_proof` 是这条断言**不空转**的证据：10ms 后它会把 `pumped` 置真，
    // 而 `wait_until` 会在那一刻返回。于是"循环在这段时间里确实被拨了"
    // 与"那些回调一条都没响"是同一段墙钟里发生的两件事。
    bool pumped = false;
    uvcpp_timer pump_proof(&loop);
    pump_proof.start(
        [&pumped](uvcpp_timer* self) {
          pumped = true;
          self->stop();
        },
        10, 0);

    const bool saw =
        uvcpp_test::wait_until(&loop, [&] { return pumped || any_cb_fired; },
                               500);
    check(saw, "500ms 内循环被拨到了（定时器或回调）");
    check(pumped,
          "是**定时器**先响的 —— 它证明这段时间里循环真在被拨，"
          "否则下面那条断言只是没拨循环而已");
    check(!any_cb_fired,
          "connect/listen 的完成回调一次都没被调 —— "
          "返回负值时必须一条回调都不排，"
          "否则调用方会挂在一条永远不会来的依赖上");
  }

  // **这里不断言 `loop_alive()`**：`pump_proof` 刚刚被析构，它的关闭回调还排在
  // 队列上，而 libuv 的 `uv_loop_alive()` 把 `closing_handles` 也算作"活着" ——
  // 现在断言它等于 0 只会红，红的还不是被测代码。那个判据放在全部作用域结束
  // 之后（`test_self_owned_loop` 尾部），那里该收的尾都已经收完了。
  //
  // 这条注释要留着：它是"为什么这里少一条看起来该有的断言"的答案，
  // 免得后来人顺手补上一条必然失败的判据。
  (void)loop;
}

// =========================================================================
// 5. 关闭与空闲超时
// =========================================================================

/// `ngtcp2.h:688`：`NGTCP2_ERR_IDLE_CLOSE`。理由同 `quic_stream_func.cpp` 里
/// 那个 `kErrStreamShutWr` —— 公开头不许露 ngtcp2 的符号，而 `on_close` 的
/// `error_code < 0` 那一支要的正是这个数。
const int kErrIdleClose = -238;

/// 空闲超时用例里两边声明的 `max_idle_timeout`（毫秒）。
///
/// **300 不是"随便一个小数"。** 实际生效的是双方声明里小的那个（RFC 9000
/// §10.1），而 ngtcp2 会按 3 倍 PTO 之类的规则决定真正的到期时刻 —— 取 300
/// 是为了让整个用例在 1 秒量级收场，同时离"刚握完手就超时"那种假红（握手本身
/// 就要花几十毫秒）留出足够余量。设成 50 会开始飘。
const uint64_t kIdleTimeoutMs = 300;

/// 一条连接从 `connect()` 到空闲超时收场的 deadline（毫秒）。
const int kCloseDeadlineMs = 5000;

/// 第 6 节里"先让连接静下来"的那段时长（毫秒）。
///
/// **取值有讲究**：太短则连接上还有包在飞（那些包会把排队中的帧顺带带出去，
/// 于是"少了 flush"就观察不到）；太长则整条用例白等。250ms 是实测下来
/// "ACK 延迟计时器、PTO 这些都过去了、而空闲超时（默认几十秒）还远没到"的
/// 一档 —— 改小之前先重跑一次变异（去掉 `shutdown_stream_read()` 里那次
/// `flush()`，这一条必须红）。
const int kQuietMs = 250;

/// 应用错误码用例里服务端给的那个数。**随便取的，只要非 0 且不是常见的
/// libuv/ngtcp2 码** —— 断言要的是"它原样传到了对端"，取 42 就没有
/// "碰巧对上某个内部码"的余地。
const int kAppErrorCode = 42;

/**
 * @brief 两端各起一条真连接，测两种**收场**：空闲超时 与 带应用错误码的关闭。
 *
 * 两条都是"连接终结"这条路，而这条路在 1.4.0 那版里根本没有 —— 那时
 * `close()` 返回 `UV_ENOSYS`、`on_close` 一次都不会响。
 *
 * @param which true = 跑空闲超时那条，false = 跑应用错误码那条。
 */
void test_close_path(uvcpp_loop& loop, bool idle_timeout_case) {
  if (idle_timeout_case) {
    std::cout << "  -- 空闲超时" << std::endl;
  } else {
    std::cout << "  -- 带应用错误码的关闭" << std::endl;
  }

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    check(false, "生成自签证书失败");
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  // 这两条都跑在**共享循环**上，所以整段包在一个块里 —— 退出块时两个端点都
  // 析构、句柄都进 `closing` 队列，外层最后那条 `loop_alive() == 0` 才谈得上
  // "收干净了"。
  {
    uvcpp_quic_server server(&loop);
    server.set_ssl_context(&server_ctx);
    server.set_idle_timeout(idle_timeout_case ? kIdleTimeoutMs : 30000);
    check_eq_i(server.bind("127.0.0.1", 0), 0, "bind(127.0.0.1, 0)");

    int  server_close_code = 12345;  // 哨兵：绝不可能"碰巧对上"
    int  server_close_hits = 0;

    server.listen([&](uvcpp_quic_connection* c) {
      uvcpp_quic_connection::callbacks cbs;
      cbs.on_close = [&](uvcpp_quic_connection&, int code) {
        ++server_close_hits;
        server_close_code = code;
      };
      if (!idle_timeout_case) {
        // 握手一完成就带错误码 42 关掉。这条路**只有**
        // `uvcpp_quic_connection::close(code)` 走得通 —— 两个端点自己的
        // `close()` 都不带码（客户端的干脆没有参数）。所以本用例是那个
        // 公开 API 的唯一实测点。
        //
        // 而且它顺带测了"在回调里关连接"：`on_alpn` 是从 ngtcp2 的回调栈里
        // 出来的，ngtcp2 明令不许在那里面调 `write_connection_close` ——
        // `quic_session` 必须把这次关闭**记下来**、等 `read_pkt` 退栈再兑现。
        cbs.on_alpn = [](uvcpp_quic_connection& conn, const std::string&) {
          check_eq_i(conn.close(kAppErrorCode), 0,
                     "握手完成后 connection::close(42) 返回 0");
        };
      }
      c->set_callbacks(cbs);
    });
    const int port = server.configured_port();
    check(port > 0, "listen() 之后 configured_port() 报内核分配的端口");

    uvcpp_quic_client client(&loop);
    client.set_ssl_context(&client_ctx);
    client.set_idle_timeout(idle_timeout_case ? kIdleTimeoutMs : 30000);

    bool connected = false;
    int  client_close_code = 12345;
    int  client_close_hits = 0;
    bool client_closed = false;

    const int rc = client.connect("127.0.0.1", port, [&](int status) {
      connected = (status == 0);
    });
    check_eq_i(rc, 0, "connect() 返回 0");
    if (rc != 0) return;

    {
      uvcpp_quic_connection::callbacks cbs;
      cbs.on_close = [&](uvcpp_quic_connection&, int code) {
        ++client_close_hits;
        client_close_code = code;
        client_closed      = true;
      };
      client.connection()->set_callbacks(cbs);
    }

    check(uvcpp_test::wait_until(&loop, [&] { return connected; },
                                 kCloseDeadlineMs),
          "握手在 deadline 内完成");
    if (!connected) return;

    // 从这一刻起**谁也不说话**（空闲超时那条），或者等对端那张 CONNECTION_CLOSE。
    check(uvcpp_test::wait_until(&loop, [&] { return client_closed; },
                                 kCloseDeadlineMs),
          idle_timeout_case ? "客户端因空闲超时收场"
                            : "客户端收到了对端的 CONNECTION_CLOSE");

    if (!idle_timeout_case) {
      // **错误码如实**：42 是服务端在 CONNECTION_CLOSE 里给的应用错误码，
      // 客户端那边必须是**正数 42**（`> 0` = 对端的应用错误码）。报成
      // `NGTCP2_ERR_DRAINING`（-224）也"能过"，所以这条断言要精确到数。
      check_eq_i(client_close_code, kAppErrorCode,
                 "客户端的 on_close 拿到对端的应用错误码 42");
      // 本端自己关的那一侧看不到对端的回话 —— `close()` 把连接推进 CLOSING
      // 之后来的包一律被 ngtcp2 丢掉（`NGTCP2_ERR_CLOSING` 那条）。所以这里
      // 只能等它自己走完关闭期，拿到的是**干净关闭**那个 0。
      check(uvcpp_test::wait_until(&loop, [&] { return server_close_hits != 0; },
                                   kCloseDeadlineMs),
            "服务端的 on_close 跑到了");
      check_eq_i(server_close_code, 0,
                 "本端先关的那一侧拿不到对端的错误码（CLOSING 期丢包），"
                 "报的是 0");
    } else {
      // **空闲超时是传输层原因，不是应用错误码** —— `NGTCP2_ERR_IDLE_CLOSE`
      // 走的是 `on_close` 约定里 `error_code < 0` 那一支，而且这一格
      // **一个包都不发**（RFC 9000 §10.1：空闲关闭不算一次连接错误）。
      check_eq_i(client_close_code, kErrIdleClose,
                 "客户端的 on_close 报 NGTCP2_ERR_IDLE_CLOSE");
      check(uvcpp_test::wait_until(&loop, [&] { return server_close_hits != 0; },
                                   kCloseDeadlineMs),
            "服务端也因为空闲超时收场了（一条静下来的连接两侧都会超时）");
      check_eq_i(server_close_code, kErrIdleClose,
                 "服务端的 on_close 也报 NGTCP2_ERR_IDLE_CLOSE");
    }

    // **恰好一次。** 这条路有好几个入口（对端关、超时、协议错），一个把
    // `finalize()` 的"只报一次"守卫漏掉的实现会在两边都报两遍 —— 而上层拿到
    // 两次终结通知，就会把"摘表 + 销毁"做两遍。
    check_eq_i(client_close_hits, 1, "客户端 on_close 恰好跑了 1 次");
    check_eq_i(server_close_hits, 1, "服务端 on_close 恰好跑了 1 次");

    // 端点收尾之后指针就该是空的 —— 这不是"跑到了"的重复，是钉住收尾**发生过**。
    check(client.connection() == nullptr,
          "on_close 之后客户端的 connection() 变空");
  }
}

// =========================================================================
// 6. 读侧那三样：FIN 与 RESET 分得开、流额度、STOP_SENDING
// =========================================================================

/// 一条连接上跑三段，全部复用同一次握手 —— 三样东西各有一段。
///
/// **这三样是 1.4.1 Q5（HTTP/3）的**前置**：分不出 FIN 与 RESET，h3 就没法把
/// "请求体发完了"与"请求被取消了"分开；没有流额度事件，h3 那三条关键单向流
/// 在额度没到时只能轮询（而额度是随包来的，轮询在静下来的连接上永远等不到）；
/// 没有 STOP_SENDING，一条被对端取消的流会一直往外填字节。
///
/// 三段共用的判据只有一条主线：**`net_read_result::fin` 说的必须是真话**。
/// 所以每段都在同一个 `on_read` 里把 `(event, fin)` 一起记下来，而不是各段自己
/// 猜一个事件名 —— 事件名在 FIN 与 RESET-0 两条路上是**一样**的（都是
/// `PEER_CLOSED`），这一位是唯一的分界。
void test_read_side_termination(uvcpp_loop& loop) {
  std::cout << "  -- 读侧收尾 / 流额度 / STOP_SENDING" << std::endl;

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    check(false, "生成自签证书失败");
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  // 服务端给 STOP_SENDING 带的应用错误码。取 7 而不是 0：0 在两边看起来与
  // "没有错误"一样，断言就分不出"码传对了"与"码被丢掉了"。
  const uint64_t kStopCode = 7;

  {
    uvcpp_quic_server server(&loop);
    server.set_ssl_context(&server_ctx);
    check_eq_i(server.bind("127.0.0.1", 0), 0, "bind(127.0.0.1, 0)");

    std::map<int64_t, std::string>   srv_bytes;
    std::map<int64_t, bool>          srv_data_fin;   ///< DATA 事件带过的 fin
    std::map<int64_t, net_read_event> srv_term_event; ///< 非 DATA 事件
    std::map<int64_t, bool>          srv_term_fin;
    std::map<int64_t, uint64_t>      srv_stop;       ///< 对端叫我们停的码
    uvcpp_quic_connection*           server_conn = nullptr;

    check_eq_i(server.listen([&](uvcpp_quic_connection* c) {
                 server_conn = c;
                 uvcpp_quic_connection::callbacks cbs;
                 cbs.on_read = [&](uvcpp_quic_connection&, int64_t id,
                                   const net_read_result& r) {
                   if (r.event == net_read_event::DATA) {
                     srv_bytes[id].append(r.data, r.size);
                     srv_data_fin[id] = r.fin;
                   } else {
                     srv_term_event[id] = r.event;
                     srv_term_fin[id]   = r.fin;
                   }
                 };
                 cbs.on_stop_sending = [&](uvcpp_quic_connection&, int64_t id,
                                           uint64_t code) {
                   srv_stop[id] = code;
                 };
                 c->set_callbacks(cbs);
               }),
               0, "listen() 返回 0");
    const int port = server.configured_port();
    check(port > 0, "listen() 之后 configured_port() 报内核分配的端口");

    uvcpp_quic_client client(&loop);
    client.set_ssl_context(&client_ctx);

    std::map<int64_t, bool>           cli_data_fin;
    std::map<int64_t, net_read_event> cli_term_event;
    std::map<int64_t, bool>           cli_term_fin;
    std::map<int64_t, uint64_t>       cli_stop;
    bool  connected = false;
    int64_t sid_grace = -1;  ///< FIN 收场：干净
    int64_t sid_reset = -1;  ///< RESET-0 收场：不干净，但事件名一样
    int64_t sid_stop  = -1;  ///< 服务端关读方向，客户端在 on_stop_sending 上知道

    check_eq_i(client.connect("127.0.0.1", port, [&](int status) {
                 connected = (status == 0);
                 if (!connected) return;
                 uvcpp_quic_connection* c = client.connection();
                 if (c == nullptr) return;
                 sid_grace = c->open_stream(true);
                 if (sid_grace >= 0) {
                   c->write_stream(sid_grace, "abc", 3, /*end_stream=*/true);
                 }
                 sid_reset = c->open_stream(true);
                 if (sid_reset >= 0) {
                   c->write_stream(sid_reset, "half", 4, false);
                 }
                 sid_stop = c->open_stream(true);
                 if (sid_stop >= 0) c->write_stream(sid_stop, "x", 1, false);
               }),
               0, "connect() 返回 0");

    {
      uvcpp_quic_connection::callbacks cbs;
      cbs.on_read = [&](uvcpp_quic_connection&, int64_t id,
                        const net_read_result& r) {
        if (r.event == net_read_event::DATA) {
          cli_data_fin[id] = r.fin;
        } else {
          cli_term_event[id] = r.event;
          cli_term_fin[id]   = r.fin;
        }
      };
      cbs.on_stop_sending = [&](uvcpp_quic_connection&, int64_t id,
                                uint64_t code) { cli_stop[id] = code; };
      client.connection()->set_callbacks(cbs);
    }

    check(uvcpp_test::wait_until(&loop, [&] { return connected; },
                                 kCloseDeadlineMs),
          "握手在 deadline 内完成");
    if (!connected) return;

    // ---- 流额度 ------------------------------------------------------
    //
    // 握手之后客户端手上一定有多条双向流的额度（服务端的
    // `initial_max_streams_bidi` 默认是 100）。这一条钉的是"这个函数真的问到了
    // 内核"：一个恒返 0 的实现会让上面那句"没有内核时 == 0"照样绿。
    uvcpp_quic_connection* cc = client.connection();
    check(cc != nullptr, "握手之后 connection() 还在");
    if (cc == nullptr) return;
    check(cc->streams_left(true) > 0,
          "握手之后 streams_left(双向) > 0 —— 它真问到了内核");

    check(sid_grace >= 0 && sid_reset >= 0 && sid_stop >= 0,
          "客户端开出了三条双向流");
    if (sid_grace < 0 || sid_reset < 0 || sid_stop < 0) return;

    // ---- 第 1 段：FIN -------------------------------------------------
    check(uvcpp_test::wait_until(
              &loop, [&] { return srv_term_event.count(sid_grace) != 0; },
              kCloseDeadlineMs),
          "服务端在 grace 那条流上收到了收尾事件");
    check_eq_s(srv_bytes[sid_grace], "abc", "服务端收到的字节");
    // **DATA 事件自己带着 fin。** QUIC 的 STREAM 帧可以同时带数据与 FIN 位，
    // 所以"这块就是最后一块"是这一格的信息 —— 而 TCP 那条路上它恒为 false。
    // 丢了这一位，h3 在"头块 + 数据 + FIN"这种形状上就结束不了流。
    check(srv_data_fin.count(sid_grace) != 0 && srv_data_fin[sid_grace],
          "服务端 DATA 事件的 fin == true（这块数据后面就是 FIN）");
    check(srv_term_event[sid_grace] == net_read_event::PEER_CLOSED,
          "FIN 的收尾事件是 PEER_CLOSED");
    check(srv_term_fin[sid_grace],
          "FIN 的 PEER_CLOSED 带 fin == true（干净收尾）");

    // ---- 第 2 段：RESET-0 ---------------------------------------------
    //
    // `shutdown_stream()` 不带码（默认实参 0），线上是 RESET_STREAM(0)。
    // 服务端看到的**事件名与上面那段一样**，唯一的区别就在 `fin` 上 ——
    // 这正是这一位存在的全部理由。
    check(uvcpp_test::wait_until(
              &loop, [&] { return srv_bytes[sid_reset] == "half"; },
              kCloseDeadlineMs),
          "服务端收到了 half（不带 FIN 的那块）");
    check_eq_i(cc->shutdown_stream(sid_reset), 0, "shutdown_stream() 返回 0");
    check(uvcpp_test::wait_until(
              &loop, [&] { return srv_term_event.count(sid_reset) != 0; },
              kCloseDeadlineMs),
          "服务端在 reset 那条流上收到了收尾事件");
    check(srv_term_event[sid_reset] == net_read_event::PEER_CLOSED,
          "RESET-0 的收尾事件**也是** PEER_CLOSED（错误码 0 不是错误）");
    check(!srv_term_fin[sid_reset],
          "RESET-0 的 PEER_CLOSED 带 fin == false —— 与上面那段正好相反");
    // 两块字节是分两次到的（"abc" 带 FIN、"half" 不带），所以这一段顺带钉住了
    // 上面那条 `srv_data_fin` 的判据不是"恒为真"。
    check(srv_data_fin.count(sid_reset) != 0 && !srv_data_fin[sid_reset],
          "不带 FIN 的那块数据，DATA 事件的 fin == false");

    // ---- 第 3 段：STOP_SENDING ----------------------------------------
    check(uvcpp_test::wait_until(&loop, [&] { return srv_bytes[sid_stop] == "x"; },
                                 kCloseDeadlineMs),
          "服务端收到了 x");
    check(server_conn != nullptr, "服务端拿到过连接对象");
    if (server_conn == nullptr) return;

    // **先让这条连接静下来。** 这一步是判据的一部分，不是"等一等更保险"：
    // `shutdown_stream_read()` 只**排**一个 STOP_SENDING 帧，把它变成数据报的
    // 是同一函数尾巴上那次 flush。连接上还有别的包在飞时，那个帧会搭着下一个
    // 包出去 —— 于是"少了那次 flush"这件事在功能上**看不出来**（实测：不静默
    // 的话，去掉 flush 照样在几毫秒内到达）。只有在一条真的静下来的连接上，
    // "没有 flush 就出不去"才是可观察的，而这正是 `write_stream()` 那条注释
    // 说过的同一个道理。
    uvcpp_test::pump_for(&loop, kQuietMs);

    // 服务端"不读了"。线上是一个 STOP_SENDING 帧，客户端必须在
    // `on_stop_sending` 上知道，并且拿到服务端给的那个码。
    check_eq_i(server_conn->shutdown_stream_read(sid_stop, kStopCode), 0,
               "shutdown_stream_read() 返回 0");
    check(uvcpp_test::wait_until(&loop, [&] { return cli_stop.count(sid_stop) != 0; },
                                 kCloseDeadlineMs),
          "客户端收到了对端的 STOP_SENDING");
    check_eq_i(cli_stop.count(sid_stop) ? static_cast<long long>(cli_stop[sid_stop])
                                        : -1,
               static_cast<long long>(kStopCode),
               "STOP_SENDING 上的应用错误码原样传到");
    // 上面那条"客户端收到了 STOP_SENDING"就是"帧真发出去了"的证据：
    // `shutdown_stream_read()` 只**排**帧，把它变成数据报的是同一函数尾巴上
    // 那次 flush。删掉那次 flush，那条断言会一直等到 deadline 而红 ——
    // 这正是 `write_stream()` 那里注释过的同一件事。
    //
    // 顺带一条**不许报错**的：对一条已经收尾的流再关一次读方向不该崩。ngtcp2
    // 在流不存在时返回 0（"This function returns 0 if a stream denoted by
    // stream_id is not found."），所以这里断言的是 0 而不是"随便什么都行"。
    check_eq_i(cc->shutdown_stream_read(sid_grace, 0), 0,
               "对一条已经收尾的流再调 shutdown_stream_read() 返回 0");

    // ---- 收尾 ---------------------------------------------------------
    // **收 STOP_SENDING 是发送方向的事，不是读方向的。** 客户端在第 3 段里
    // 收到的是"你别发了"，它这条流的**读**方向一个字节都没动过 —— 所以这边
    // 不该冒出任何收尾事件。这一条防的是"两个方向的通知合成一个"那种实现：
    // 那种实现下，客户端会在 `on_stop_sending` 之外**再**收到一次读侧收尾。
    check(cli_term_event.count(sid_stop) == 0,
          "收到 STOP_SENDING 的那条流上，读侧没有多余的收尾事件");
    check(server_conn->streams_left(true) > 0,
          "服务端的 streams_left(双向) > 0（额度对两端都成立）");
    check_eq_i(client.close(), 0, "close() 返回 0");
  }
}

// =========================================================================
// 7. 自建循环那条路 + run/stop
// =========================================================================

/**
 * @brief 共享循环在这一切之后**还能用**的正面证据。
 *
 * "没崩"不等于"没坏"：一条被 `uv_loop_close()` 过的循环在上面再跑一个定时器
 * 是**不会崩**的（libuv 读的是已经清零的内存）。所以判据取"定时器真响了"。
 */
void check_loop_still_usable(uvcpp_loop& loop) {
  bool ticked = false;
  uvcpp_timer t(&loop);
  t.start(
      [&ticked](uvcpp_timer* self) {
        ticked = true;
        self->stop();
      },
      10, 0);
  const bool ok = uvcpp_test::wait_until(&loop, [&] { return ticked; }, 500);
  check(ok && ticked, "共享循环在全部端点析构之后仍然能拨动定时器");
}

void test_self_owned_loop(uvcpp_loop& shared) {
  std::cout << "  -- 自建循环 / run / stop" << std::endl;

  {
    uvcpp_quic_client client;
    uvcpp_quic_server server;

    check(client.get_loop() != nullptr, "默认构造的客户端有循环");
    check(client.get_loop() != &shared, "默认构造的客户端**不**用别人的循环");
    check(server.get_loop() != nullptr, "默认构造的服务端有循环");
    check(server.get_loop() != &shared, "默认构造的服务端**不**用别人的循环");
    check(client.get_loop() != server.get_loop(),
          "两个默认构造的对象各有各的循环");

    // `run(UV_RUN_NOWAIT)` 是真实现：循环上什么都没有，所以立刻回 0。
    // 这一条**会**在自建循环上测 —— 别在共享那条上测，`run` 是拨循环，
    // 而拨谁的是能看出区别的。
    check_eq_i(server.run(UV_RUN_NOWAIT), 0,
               "server.run(UV_RUN_NOWAIT) 在空循环上 == 0");
    server.stop();
    server.stop();  // 幂等，不该崩
    check(true, "server.stop() 调两次是安全的");
    check(client.connection() == nullptr, "默认构造的客户端也建不出连接");
  }

  // 顺序是 load-bearing 的：**先**证明循环还能拨（`wait_until` 自己会拨循环，
  // 上面那个定时器的关闭回调顺带跑完），**再**把队列彻底收干净，最后才断言
  // "什么都没留下"。
  check_loop_still_usable(shared);

  uvcpp_test::drain(&shared);
  check_eq_i(shared.loop_alive(), 0,
             "共享循环在全部端点与定时器都析构、队列也收干净之后，"
             "没有留下任何句柄 —— 端点那两边的析构不碰自己没建的循环");
}

}  // namespace

int main() {
  std::cout << "[functional quic_api] start" << std::endl;

  test_backend_is_really_linked();
  test_default_alpn();
  test_connection_shell();

  // **不要再调 `loop.init()`。** `uvcpp_loop` 的构造函数里已经调过一次了，
  // 在一条已经初始化过的循环上再 `uv_loop_init` 会把它的内部句柄（async、
  // 定时器堆）重置一遍 —— libuv 不查这个，于是它不报错，只是把上一次分配
  // 的东西漏掉。这里从前有一句，是骨架期留下的。
  uvcpp_loop loop;
  uvcpp_test::loop_drain drain_loop(&loop);

  test_endpoint_contract(loop);

  // ngtcp2 的 crypto 后端是**进程级**的，在任何端点和任何 TLS 上下文之前。
  // `test_backend_is_really_linked()` 结束时把它 free 掉了，所以这里重新起一次
  // （那一节测的是 init/free 这对函数本身，与这里的用法不冲突）。
  check_eq_i(quic_crypto_backend_init(), 0, "为关闭用例重起 crypto 后端");
  test_close_path(loop, /*idle_timeout_case=*/true);
  test_close_path(loop, /*idle_timeout_case=*/false);
  quic_crypto_backend_free();

  // 读侧那三样（FIN/RESET 的分界、流额度、STOP_SENDING）要一条**真握手过的**
  // 连接才谈得上，所以放在这里 —— 它们不是"没挂内核的壳"能测的。
  check_eq_i(quic_crypto_backend_init(), 0, "为读侧用例重起 crypto 后端");
  test_read_side_termination(loop);
  quic_crypto_backend_free();

  test_self_owned_loop(loop);

  std::cout << "[functional quic_api] checks=" << g_checks
            << " failures=" << g_failures << std::endl;
  if (g_failures == 0) {
    std::cout << "[functional quic_api] done success=true" << std::endl;
    return 0;
  }
  std::cout << "[functional quic_api] done success=false" << std::endl;
  return 2;
}

#else  // UVCPP_QUIC_ENABLE

// 关掉 quic 模块时这个文件不该被编译（`tests/functional/CMakeLists.txt` 的
// 过滤器按文件名摘掉它 —— 规则是 `quic`）。真编到了就是一个必须修的配置错，
// 所以返回非零，而不是打一句 SKIP 再返回 0：后者会把"这个模块一次都没被测过"
// 伪装成"全绿"，那个教训在 `tests/functional/CMakeLists.txt` 里写着。
int main() {
  std::cerr << "[quic_api] UVCPP_QUIC_ENABLE=0 —— 这个测试文件不该被编译进来"
            << std::endl;
  return 2;
}

#endif  // UVCPP_QUIC_ENABLE
