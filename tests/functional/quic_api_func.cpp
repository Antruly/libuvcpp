/**
 * @file tests/functional/quic_api_func.cpp
 * @brief QUIC 传输层 1.4.1 的**契约**用例：形状在不在、后端链上没有、谎有没有说。
 * @author zhuweiye
 * @version 1.4.1
 *
 * **这个用例测的不是"QUIC 能跑"，而是"QUIC 还跑不了这件事是被写下来的"。**
 * 1.4.1 交付的只有构建契约、模块骨架与 API 形状（见 `doc/quic-guide.md`），
 * 传输一个字节都不通。于是一个诚实的问题就变成：怎么让"还没实现"这件事
 * **在 CI 上表现为可判定的**，而不是靠文档里一句"暂不支持"？
 *
 * 答案是三条，各管一件事：
 *
 * 1. **后端真链上了**（1.x）。三条证据分别压在 ngtcp2 的头路径、`ngtcp2_static`
 *    的符号、`ngtcp2_crypto_ossl_static` 的符号上。第三条尤其重要 —— 那一半
 *    取决于"那份 OpenSSL 是不是带 QUIC API 的 mainline ≥ 3.2"，是整条接线里
 *    最容易坏的地方。**光有 `quic_ngtcp2_version_string()` 不算证据**：它读的是
 *    一个编译期宏，一个"头骗到了、库没链上"的树照样能跑绿它。
 * 2. **动作全部返回 `UV_ENOSYS`**（3.x/4.x/5.x）。这是"没实现"的**正面**表达：
 *    调用方拿得到一个明确的"这条路没通"，而不是一个沉默的空操作。
 * 3. **回调一次都不被调**（4.x/5.x）。这一条是三条里唯一不能靠"我看了一眼
 *    返回码"得到的：一个接口收下回调、返回 0、然后永远不回调，在外观上与
 *    "正在工作"完全一样，而调用方会把整条超时路径压在那条依赖上 ——
 *    本仓库里最难查的一类挂死。
 *
 * 第 3 条怎么才能**不是空断言**：光"我没拨循环所以回调没跑"是没意义的。所以
 * 用例在这期间**真拨**循环，并且用一个 10ms 定时器**证明这段时间里循环确实在
 * 被拨**（`pumped`）。定时器触发了而四条回调一条都没响，那句话才成立。
 *
 * 反过来，一旦哪天真把传输实现了，这个文件会**红**（`UV_ENOSYS` 断言不再成立），
 * 逼着实现者回来显式改契约 —— 这正是想要的：不能让"框架写好了"这句话在
 * 没人注意的时候悄悄变成假的。
 */
#include <cctype>
#include <cstdlib>
#include <iostream>
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
// 3. 连接壳
// =========================================================================

void test_connection_shell() {
  std::cout << "  -- 连接壳" << std::endl;

  uvcpp_quic_connection conn;

  // 1.4.1 的状态机还不存在，所以这两个是**恒定值**。断言它们不是"测实现"，
  // 而是把这个恒定事实写下来：将来状态机落地时这里会红，提醒改契约。
  check(conn.state() == quic_connection_state::IDLE,
        "state() 在 1.4.1 恒为 IDLE（状态机还没写）");
  check_eq_s(conn.alpn_selected(), "",
             "alpn_selected() 在 1.4.1 恒为空串（握手还没实现）");

  // 回调装上去不崩，且**一条都不会被调** —— 见本文件开头第 3 条。
  // 这里没有循环可拨，所以这轮只是"装得上"；真正的"不回调"断言在下面
  // 客户端/服务端那一段里（那里有循环）。
  uvcpp_quic_connection::callbacks cbs;
  cbs.on_read = [](uvcpp_quic_connection&, const net_read_result&) {};
  cbs.on_stream_open = [](uvcpp_quic_connection&, int64_t) {};
  cbs.on_alpn = [](uvcpp_quic_connection&, const std::string&) {};
  cbs.on_close = [](uvcpp_quic_connection&, int) {};
  conn.set_callbacks(cbs);
  check(true, "set_callbacks() 收下四个回调");

  check_eq_i(conn.open_stream(), UV_ENOSYS, "open_stream() == UV_ENOSYS");
  check_eq_i(conn.open_stream(false), UV_ENOSYS,
             "open_stream(单向) == UV_ENOSYS");
  check_eq_i(conn.write_stream(0, "x", 1, false), UV_ENOSYS,
             "write_stream() == UV_ENOSYS");
  check_eq_i(conn.write_stream(0, nullptr, 0, true), UV_ENOSYS,
             "write_stream(空块收尾) == UV_ENOSYS");
  check_eq_i(conn.shutdown_stream(0), UV_ENOSYS,
             "shutdown_stream() == UV_ENOSYS");
  check_eq_i(conn.close(), UV_ENOSYS, "close() == UV_ENOSYS");
  check_eq_i(conn.close(42), UV_ENOSYS, "close(error_code) == UV_ENOSYS");
}

// =========================================================================
// 4. 客户端 / 服务端契约（共用一条外部循环）
// =========================================================================

void test_endpoint_contract(uvcpp_loop& loop) {
  std::cout << "  -- 端点契约（共享循环）" << std::endl;

  // 这四条回调如果在 1.4.1 里的任何时刻被调到，就是契约被破坏了。
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

    check_eq_i(client.connect("127.0.0.1", 1, [&mark](int) { mark(); }),
               UV_ENOSYS, "client.connect() == UV_ENOSYS");
    // 主机名这条也要试：解析那一步同样没实现，不能因为"名字看起来对"就走到
    // 别的分支上去（那会让 `host` 参数在校验里被悄悄用上）。
    check_eq_i(client.connect("localhost", 1, [&mark](int) { mark(); }),
               UV_ENOSYS, "client.connect(主机名) == UV_ENOSYS");
    check(client.connection() == nullptr,
          "1.4.1 的 connection() 恒为 nullptr（建不出连接）");
    check_eq_i(client.close(), UV_ENOSYS, "client.close() == UV_ENOSYS");

    check_eq_i(server.listen([&mark](uvcpp_quic_connection*) { mark(); }),
               UV_ENOSYS, "server.listen() == UV_ENOSYS");

    // ---- bind：1.4.1 里唯一的"真行为"，所以判据要真的分得开好坏 ----
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
    check_eq_i(server.bindIpv4(nullptr, 0), 0, "bindIpv4(nullptr) == 0（通配）");
    check_eq_s(server.configured_ip(), "0.0.0.0",
               "bindIpv4(nullptr) 回落到通配地址");
    check_eq_i(server.bindIpv6("::1", 5555), 0, "bindIpv6(合法 IPv6) == 0");
    check_eq_i(server.bindIpv6("127.0.0.1", 5555), UV_EINVAL,
               "bindIpv6(给的是 IPv4) == UV_EINVAL —— 显式的那两个要认族");
    check_eq_i(server.bindIpv4("::1", 5555), UV_EINVAL,
               "bindIpv4(给的是 IPv6) == UV_EINVAL");

    // ---- 三次调用之后，回调一次都不许响 ----
    //
    // `pump_proof` 是这条断言**不空转**的证据：10ms 后它会把 `pumped` 置真，
    // 而 `wait_until` 会在那一刻返回。于是"循环在这段时间里确实被拨了"
    // 与"四条回调一条都没响"是同一段墙钟里发生的两件事。
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
          "返回 UV_ENOSYS 时必须一条回调都不排，"
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
// 5. 自建循环那条路 + run/stop
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

  // 顺序是 load-bearing 的：**先**证明循环还能拨（这一条会被上面那个定时器的
  // 关闭回调干扰吗？不会 —— `wait_until` 自己会拨循环，关闭回调顺带跑完），
  // **再**把队列彻底收干净，最后才断言"什么都没留下"。
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

  uvcpp_loop loop;
  uvcpp_test::loop_drain drain_loop(&loop);
  loop.init();

  test_endpoint_contract(loop);
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
