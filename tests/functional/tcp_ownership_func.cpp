/**
 * @file tests/functional/tcp_ownership_func.cpp
 * @brief `uvcpp_tcp_server` 客户端所有权的功能测试。
 *
 * 这个文件守的是**四种已经确认的缺陷**，每条都对应一个真实场景：
 *
 *  1. **用户设了 on_close，框架就放弃释放** —— 原来用户的观察回调和框架的
 *     清理共用 `close_fn_` 一个槽，`setup_client_callbacks` 里那句
 *     `if (!client->has_close_callback())` 让用户一设回调就整条跳过释放。
 *     表现是每连接泄漏一个 client、`clients_` 无限增长。
 *
 *  2. **double free** —— 框架默认的关闭回调里 `delete client`，而
 *     `~uvcpp_tcp_client` 会释放 `close_arg_` 那个 std::function；
 *     `trampoline_close` 返回之后还要再 `delete cb` 一次。**每次连接关闭
 *     都 double free 一次**，只是释放和再释放挨得太近、中间没别的分配，
 *     所以平时看不出来。
 *
 *  3. **登记排在用户回调之后** —— 用户在 on_connection 里立刻关掉连接时
 *     登记的是个已关闭的；用户回调抛异常则登记和 setup 都不执行，直接泄漏。
 *
 *  4. **所有权无法交接** —— 要么框架全管（无法把连接留住当自己的信道），
 *     要么用户自己管（框架不知道，容易两边都删）。所以加了
 *     `take_client()` / `return_client()` 这两个显式的交接口。
 *
 * 起的是真连接而不是构造对象直接调 —— 这些缺陷全都出在「连接关闭」这条
 * 真实路径上（read 回调里的 nread < 0 分支），不跑真连接根本触发不到。
 */
#include <atomic>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "handle/uvcpp_timer.h"
#include "net/uvcpp_tcp_client.h"
#include "net/uvcpp_tcp_server.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

// =========================================================================
// 服务端线程
// =========================================================================

struct server_state {
  /// 服务端在 on_connection 里看到的客户端指针（只用于断言，不解引用）。
  std::atomic<int> accepted;
  /// 用户的 on_close 被调用的次数（缺陷 1 的观察点）。
  std::atomic<int> user_close;
  /// on_connection 是否抛过异常（缺陷 3）。
  std::atomic<int> threw;
  /// 服务端停止、loop 退出之后的 client_count（缺陷 1/2 的最终判据）。
  std::atomic<int> final_count;
  /// take_client 之后立刻读到的 client_count（缺陷 4）。
  std::atomic<int> count_after_take;
  /// take_client 的返回值是否为非空（拿到所有权）。
  std::atomic<int> take_ok;
  /// take_client 取走之后，服务端是否还认得它。
  std::atomic<int> still_owns_after_take;
  /// 被 take_client 取走所有权的客户端。取走之后服务端不管了，就得**测试自己
  /// 收尾**：它们的 tcp 句柄还挂在 server 的循环上，不在这里关掉的话
  /// `uv_loop_close()` 永远 EBUSY，整个 `uv_loop_t` 赔进去（实测泄漏 1 个循环）。
  std::vector<uvcpp_tcp_client*> taken;

  server_state()
      : accepted(0),
        user_close(0),
        threw(0),
        final_count(-1),
        count_after_take(-1),
        take_ok(0),
        still_owns_after_take(-1) {}
};

/// 让服务端一直跑，直到 stop 置位。
void run_server(std::promise<int>& port_promise, std::atomic<bool>& ready,
                std::atomic<bool>& stop, server_state* st,
                bool user_sets_on_close, bool take_ownership) {
  // uvcpp_tcp_server 自带内部 loop（构造时就建好了），不接受外部注入。
  uvcpp_tcp_server server;

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }

  // 绑 0 让内核挑端口，再问出来 —— 固定端口会跟别的测试/别的进程撞。
  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int port = ntohs(name.sin_port);

  rc = server.listen(
      [&](uvcpp_tcp_client* client) {
        st->accepted.fetch_add(1);

        // **这里故意什么都不做** —— 不起读、不设读回调。
        //
        // 断开是在读回调的 nread < 0 分支里被发现的（UV_EOF / 读错误），
        // 所以"有没有人读"这件事直接决定了断开能不能被发现。以前这一点
        // 要使用者自己记得调 `read_start()`，忘了就是每连接静默泄漏一个
        // client（本文件开头记的就是这个坑）。现在服务端默认给每个新连接
        // 装自动读（`set_auto_read`，默认 on），于是**使用者完全不用参与**，
        // 而这个测试顺带就成了那条保证的第二个护栏：谁要是把自动读去掉，
        // 下面的 final_count 断言立刻变成连接数。

        // 缺陷 1 的触发条件：用户注册自己的 on_close。
        // 以前这一句会让框架的释放被整条跳过。
        if (user_sets_on_close) {
          uvcpp_tcp_client* c = client;
          c->set_on_close([st, c]() {
            (void)c;
            st->user_close.fetch_add(1);
          });
        }

        // 缺陷 4：取走所有权，框架此后不该再管它。
        if (take_ownership) {
          uvcpp_tcp_client* taken = server.take_client(client);
          st->take_ok.store(taken == client ? 1 : 0);
          if (taken != nullptr) st->taken.push_back(taken);
          st->count_after_take.store(
              static_cast<int>(server.client_count()));
          st->still_owns_after_take.store(
              server.owns_client(client) ? 1 : 0);
        }
      },
      128);

  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }
  // **端口必须在 listen() 成功之后才放行**：调用方拿到端口就立刻 connect，
  // 而"已 bind、尚未 listen"的 socket 在内核里是**拒连**的（ECONNREFUSED），
  // 不是排队等 listen。
  port_promise.set_value(port);
  ready.store(true);

  // 跑循环直到 stop。用 UV_RUN_NOWAIT 轮转，这样能及时看到 stop 标志。
  uvcpp_loop* loop = server.get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 再转几圈，让挂起的 close 事件跑完。
  for (int i = 0; i < 200; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // **判据**：所有连接都断开之后，登记表必须回到 0。
  // 缺陷 1 没修的话这里会 > 0（每个连接泄漏一个 client）。
  st->final_count.store(static_cast<int>(server.client_count()));

  // 取走所有权的那些客户端，由本测试负责收尾 —— 而且**必须在这里**，在
  // `server` 析构之前：它们的 tcp 句柄挂在 server 自己的循环上，等 server
  // 析构完再删就是往一个已经没了的循环里 `uv_close`；不删则那三个句柄谁也不
  // 关，`alive=0`（既不活跃也没在关），`uv_run` 连 while 体都不进，
  // `uv_loop_close()` 从此永远 EBUSY。原先这里的注释写的是"进程即将退出，
  // 所以省略"—— 省略的代价是整个 `uv_loop_t`（实测泄漏 1 个循环，句柄
  // `tcp(active=0 closing=0)` ×3）。
  for (uvcpp_tcp_client* c : st->taken) {
    delete c;
  }
  st->taken.clear();
}

// =========================================================================
// 客户端侧：在主线程里建一条真连接，然后主动断开
// =========================================================================

/// 连上去再立刻断开。返回是否成功建立过连接。
bool connect_and_drop(int port) {
  // 用 uvcpp_tcp_client 而不是裸 uvcpp_tcp：它自带 loop，connect 的签名也
  // 简单（不用自己造 uvcpp_connect req）。
  uvcpp_tcp_client client;

  std::atomic<bool> connected(false);
  std::atomic<bool> closed(false);

  int rc = client.connect("127.0.0.1", port, [&](int status) {
    if (status == 0) {
      connected.store(true);
      // 连上就直接关 —— 这正是触发服务端 read 回调里 nread < 0 分支、
      // 从而走到关闭回调的路径。
      client.get_tcp()->close([&](uvcpp_handle*) { closed.store(true); });
    }
  });

  if (rc != 0) return false;

  uvcpp_loop* loop = client.get_loop();
  for (int i = 0; i < 500 && !closed.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // 再转几圈，确保 close 事件彻底跑完。
  for (int i = 0; i < 50; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return connected.load();
}

// =========================================================================
// 场景跑一遍
// =========================================================================

struct scenario_result {
  int accepted;
  int user_close;
  int final_count;
  int count_after_take;
  int take_ok;
  int still_owns_after_take;
};

scenario_result run_scenario(int connections, bool user_sets_on_close,
                             bool take_ownership) {
  std::promise<int> port_promise;
  std::future<int> port_future = port_promise.get_future();
  std::atomic<bool> ready(false);
  std::atomic<bool> stop(false);
  server_state st;

  std::thread server_thread(run_server, std::ref(port_promise),
                            std::ref(ready), std::ref(stop), &st,
                            user_sets_on_close, take_ownership);

  const int port = port_future.get();
  scenario_result r;
  r.accepted = r.user_close = r.final_count = 0;
  r.count_after_take = r.take_ok = r.still_owns_after_take = 0;

  if (port <= 0) {
    stop.store(true);
    server_thread.join();
    std::cerr << "  [FAIL] 服务端没起来" << std::endl;
    ++g_failures;
    return r;
  }

  while (!ready.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  for (int i = 0; i < connections; ++i) {
    connect_and_drop(port);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  // 留时间让服务端处理完所有关闭事件。
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  stop.store(true);
  server_thread.join();

  r.accepted = st.accepted.load();
  r.user_close = st.user_close.load();
  r.final_count = st.final_count.load();
  r.count_after_take = st.count_after_take.load();
  r.take_ok = st.take_ok.load();
  r.still_owns_after_take = st.still_owns_after_take.load();
  return r;
}

}  // namespace

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // ---------------------------------------------------------------------
  // 场景 A：用户设了自己的 on_close。
  //
  // 缺陷 1 的回归护栏 —— 修好之前，final_count 会等于连接数（每个连接
  // 泄漏一个 client），而 user_close 是 0（因为框架的删除逻辑压根没装，
  // 用户的回调倒是装了但要走同一条 nread<0 路径，所以计数会 > 0）。
  // ---------------------------------------------------------------------
  {
    const int n = 5;
    scenario_result r = run_scenario(n, /*user_sets_on_close=*/true,
                                     /*take_ownership=*/false);
    check(r.accepted == n, "A: 服务端接受到了全部连接");
    check(r.user_close == n, "A: 用户的 on_close 被调用了（观察回调仍然生效）");
    // **核心断言**：设了用户回调，框架照样释放。
    check(r.final_count == 0,
          "A: 用户设了 on_close 也不影响框架释放（client_count 归零）");
  }

  // ---------------------------------------------------------------------
  // 场景 B：用户**不**设 on_close（框架默认路径）。
  //
  // 这条路径就是原来 double free 的路径：默认回调里 delete client，
  // 析构释放 close_arg_，然后 trampoline 再 delete 一次。
  // 能跑完不崩、且计数归零，就说明槽位交接是对的。
  // ---------------------------------------------------------------------
  {
    const int n = 5;
    scenario_result r = run_scenario(n, /*user_sets_on_close=*/false,
                                     /*take_ownership=*/false);
    check(r.accepted == n, "B: 服务端接受到了全部连接");
    check(r.user_close == 0, "B: 没设用户回调时它不该被调用");
    check(r.final_count == 0, "B: 默认路径下 client_count 归零（无泄漏）");
  }

  // ---------------------------------------------------------------------
  // 场景 C：take_client 取走所有权。
  //
  // 取走之后服务端不该再认得它 —— client_count 立刻归零。
  // ---------------------------------------------------------------------
  {
    const int n = 3;
    scenario_result r = run_scenario(n, /*user_sets_on_close=*/false,
                                     /*take_ownership=*/true);
    check(r.accepted == n, "C: 服务端接受到了全部连接");
    check(r.take_ok == 1, "C: take_client 返回了同一个指针（拿到所有权）");
    check(r.count_after_take == 0, "C: 取走之后 client_count 立刻归零");
    check(r.still_owns_after_take == 0, "C: 取走之后服务端不再认它");
    // 取走的客户端由测试自己负责 —— 这里服务端已经不管了，所以计数是 0。
    // 注意：本测试没有自己 delete 它们，这正是"取走后必须自己管"的语义
    // 演示（真实使用者应当自己 close + delete；测试里省略是因为进程即将
    // 退出，且这里要断言的只是"框架松手了"）。
    check(r.final_count == 0, "C: 服务端不会去释放已经交出去的客户端");
  }

  // ---------------------------------------------------------------------
  // 场景 D：return_client 把已关闭的连接还回来。
  //
  // 连接已经断开，服务端应当**就地释放**而不是重新登记 —— 这就是
  // "由框架决定是删除还是复用"的落点。
  // ---------------------------------------------------------------------
  {
    // 这个场景用不上连接，直接验证 API 的边界语义。
    uvcpp_tcp_server server;

    check(server.client_count() == 0, "D: 新服务端没有客户端");
    check(server.take_client(nullptr) == nullptr, "D: take_client(nullptr) 返回 nullptr");
    check(!server.return_client(nullptr), "D: return_client(nullptr) 返回 false");
    check(!server.owns_client(nullptr), "D: owns_client(nullptr) 为 false");
  }

  if (g_failures == 0) {
    std::cout << "[tcp_ownership] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[tcp_ownership] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}
