/**
 * @file tests/functional/tcp_multiloop_func.cpp
 * @brief `uvcpp_tcp_server::set_loops(n > 1)`：接受者 + 专用工作线程。
 *
 * 本用例测的是**结构**，不是吞吐：n−1 条工作循环各有一条专用 `std::thread`，
 * 每条连接被显式轮转交给其中一条，并且**一条连接的全部回调都在同一条线程上**。
 *
 * 判据逐条（以及"坏实现会让它怎么红"）：
 *
 * 1. **每个工作循环都拿到过连接** —— 轮转是显式的、确定的，所以这条不是统计
 *    判据：16 条连接分给 3 条工作循环必然得到 {6,5,5}。坏实现（全投 0 号、
 *    或者按别的规则散）红。（`doc/multiloop-design.md` §6 定的可红判据，
 *    **不拍均匀度阈值**。）
 * 2. **转手真的换了线程** —— 比**线程身份**：`on_connection` 里看到的 id 必须
 *    ≠ acceptor 线程的 id，且不同的工作循环 id 互不相同。坏实现（没真转手）
 *    红。线程身份是唯一稳的判据，先例见 `web_app_app_func.cpp`。
 * 3. **一条连接的所有回调在同一条线程上** —— 读回调里记的 id 必须等于该连接
 *    `on_connection` 里记的那个。坏实现（句柄跨循环漂移）红。
 * 4. **数据正确** —— 每条连接发一个带序号的 payload，逐条比对回显。
 * 5. **对照组 n == 1** —— `set_loops(1)` 时 `on_connection` 的线程 **==**
 *    acceptor 线程，`loop_count() == 1`，一条工作循环都不存在。没有这个对照组，
 *    "换了线程"证明不了是 `set_loops` 干的。
 * 6. **收尾不挂** —— 客户端全关之后 `client_count()` 回到 0，并且在墙钟上界内
 *    析构返回（worker 全部 stop+join 完）。"永不返回"是一种真实的失败模式，
 *    所以每条等待都带墙钟上限，超时就**自报家门**（哪一条判据等不到）然后就地
 *    结束进程 —— 不让它变成一条只有 ctest `Timeout` 的红。
 *
 * 另外还有一小节 **API 边界**：`set_loops` 的取值校验与"装好之后不能再改"。
 *
 * **不在本用例里的**：TLS 的多循环语义（留到 webapp 那一批）；以及 POSIX 那条
 * `dup()` 路 —— 它在 Windows 上**根本编不到**（在 `uvcpp_socket_handoff.cpp:65`
 * 的 `#else` 里），本机跑的是**同一函数的 Windows 支**（`WSADuplicateSocketW`）。
 * CI 上也只有 **macOS** 腿真跑到它：Linux 侧 `UV_TCP_REUSEPORT` 绑得上 ⇒
 * `fanout_` 为真 ⇒ 永不走转手。不要在记录里写成"两端都验过"。
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "net/uvcpp_tcp_client.h"
#include "net/uvcpp_tcp_server.h"
#include "uvcpp/uvcpp_buf.h"

using namespace uvcpp;

// =========================================================================
// 有界等待：超时 = 报失败 + 就地结束进程
// =========================================================================

/**
 * @brief 等到 \p pred 成立；到点还是不成立就打印**是哪一条**等不到，然后退出。
 *
 * 为什么不是"返回 false 让调用方记账"：等不到的那两条（服务端析构返回、
 * 登记表回到 0）一旦真的挂住，调用方**没有机会**去打印 —— 整条用例会一直挂到
 * ctest 的超时上限，红的形状变成一条 Timeout，看不出卡在哪一条判据上。
 */
static void require_within(const std::function<bool()>& pred, int ms,
                           const char* what) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (pred()) return;
  std::cout << "  -> FAIL: 等不到「" << what << "」（" << ms
            << " ms 墙钟上限）" << std::endl;  // endl 会 flush，下面 _Exit 不丢字
  std::_Exit(2);
}

// =========================================================================
// 服务端装置：acceptor 循环 + n−1 条工作线程，回显
// =========================================================================

class ServerRig {
 public:
  ServerRig() {}

  ~ServerRig() {
    request_stop();
    if (thread_.joinable()) thread_.join();
  }

  /**
   * @brief 起服务端（后台线程）。返回 false 表示端口没能发布出来。
   *
   * @param loops 传给 `set_loops()` 的值；1 = 对照组。
   */
  bool start(int loops, bool force_handoff = false) {
    std::promise<int> port_promise;
    std::future<int> port_fut = port_promise.get_future();

    thread_ = std::thread([this, loops, force_handoff, &port_promise]() {
      acceptor_tid_ = std::this_thread::get_id();

      // 内层作用域是**故意的**：`exited_` 要在服务端析构返回**之后**才置位，
      // 第 6 条判据（收尾不挂）量的就是"析构里那些 join 有没有卡住"。
      {
        uvcpp_tcp_server server;
        live_.store(&server);

        // **必须在 `set_loops()` 之前**（同 `set_loop_start_hook()` 的时序规则）。
        server.set_handoff_forced(force_handoff);
        loops_rc_.store(server.set_loops(loops));
        loop_count_.store(server.loop_count());

        if (server.bind("127.0.0.1", 0) != 0) {
          port_promise.set_value(-1);
          return;
        }
        // 分流还是转手是**绑定时探出来的**（不是编译期常量），所以判据要问这次
        // 的结果，别拿 `#ifdef` 猜平台。
        fanout_.store(server.is_fanout());

        sockaddr_in name;
        int namelen = static_cast<int>(sizeof(name));
        server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name),
                                      &namelen);

        const int lrc = server.listen(
            [this](uvcpp_tcp_client* client) {
              {
                std::lock_guard<std::mutex> lk(mu_);
                conn_tid_[client] = std::this_thread::get_id();
              }
              accepted_.fetch_add(1);

              // 在这条连接自己的循环线程上装读回调 —— n>1 时这里就是某条
              // 工作线程。
              client->read_start([this, client](uvcpp_buf* buf) {
                if (buf == nullptr) return;  // 断开
                if (buf->size() == 0) return;

                {
                  std::lock_guard<std::mutex> lk(mu_);
                  read_tid_[client] = std::this_thread::get_id();
                }

                // 回显（形状照抄 `tcp_server_func.cpp` 的 EchoServer）。
                uvcpp_buf* echo = new uvcpp_buf();
                echo->clone_data(buf->get_data(), buf->size());
                uvcpp_write* w = new uvcpp_write();
                w->set_uv_buf(echo->out_uv_buf(), true);
                client->get_tcp()->write(
                    w, w->get_uv_buf(), 1, [echo](uvcpp_write* wr, int) {
                      delete echo;
                      delete wr;
                    });
              });
            },
            128);
        listen_rc_.store(lrc);

        // 端口**在 listen 成功之后**才发布：回环上没人监听的端口不给 RST，
        // 连接会停在 SYN_SENT 挂住 —— 先发布端口的话客户端可能撞进那个窗口。
        port_promise.set_value(lrc == 0 ? ntohs(name.sin_port) : -2);

        while (!stop_.load()) {
          server.run(UV_RUN_NOWAIT);
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      }

      exited_.store(true);
    });

    // 启动阶段的失败是同步可见的：端口拿不到就没必要往下走。
    const int p = port_fut.get();
    if (p <= 0) return false;
    port = p;
    return true;
  }

  void request_stop() { stop_.store(true); }

  bool exited() const { return exited_.load(); }
  std::thread::id acceptor_tid() const { return acceptor_tid_; }
  /** @brief 这次绑定走的是内核分流还是接受者+转手（`n>1` 才有意义）。 */
  bool fanout() const { return fanout_.load(); }

  int loops_rc() const { return loops_rc_.load(); }
  int listen_rc() const { return listen_rc_.load(); }

  /** @brief 登记表快照（`client_count_at` 也是这么读的）。 */
  uvcpp_tcp_server* server() const { return live_.load(); }

  /** @brief 每条连接 `on_connection` / 读回调里看到的线程 id。 */
  void thread_ids(std::map<uvcpp_tcp_client*, std::thread::id>* conn,
                  std::map<uvcpp_tcp_client*, std::thread::id>* read) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (conn != nullptr) *conn = conn_tid_;
    if (read != nullptr) *read = read_tid_;
  }

  int accepted() const { return accepted_.load(); }

  int port = 0;

 private:
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> exited_{false};
  std::atomic<uvcpp_tcp_server*> live_{nullptr};
  std::thread::id acceptor_tid_;

  std::atomic<int> loops_rc_{-999};
  std::atomic<int> listen_rc_{-999};
  std::atomic<int> loop_count_{-1};
  std::atomic<bool> fanout_{false};
  std::atomic<int> accepted_{0};

  mutable std::mutex mu_;
  std::map<uvcpp_tcp_client*, std::thread::id> conn_tid_;
  std::map<uvcpp_tcp_client*, std::thread::id> read_tid_;
};

// =========================================================================
// 客户端：连上、发带序号的 payload、比对回显，然后**挂着不动**
// =========================================================================

/**
 * @brief 一条连接跑完一次回显；返回后**连接还开着**（调用方负责保持/关闭）。
 *
 * 连接保持打开是判据 1 需要的：`client_count_at()` 读的是**当前登记的**连接数，
 * 一关就掉。所以"保持"这件事放在调用方（它才能决定什么时候放），这里只负责
 * 把回显做完 —— 顺序很重要：先报成功、再挂着等，否则"成功的计数"要等到
 * 整段等完才加上，主线程的等待判据就成了永远不成立。
 */
static bool client_echo(uvcpp_tcp_client& client, int port, int idx,
                        std::atomic<int>* stage) {
  const int crc = client.connect_wait("127.0.0.1", port, 5000);
  if (crc != 0) {
    stage[idx].store(100 + (crc < 0 ? -crc % 100 : crc % 100));
    return false;
  }

  const std::string msg = "c" + std::to_string(idx) + "_echo";
  const int wrc = client.write_wait(msg.c_str(), msg.size(), 5000);
  if (wrc != 0) {
    stage[idx].store(200 + (wrc < 0 ? -wrc % 100 : wrc % 100));
    return false;
  }

  uvcpp_buf buf;
  const int rrc = client.read_wait(buf, 5000);
  if (rrc != 0) {
    stage[idx].store(300 + (rrc < 0 ? -rrc % 100 : rrc % 100));
    return false;
  }
  if (buf.to_string() != msg) {
    stage[idx].store(400);
    return false;
  }
  stage[idx].store(1);
  return true;
}

// =========================================================================
// 一条完整相位：n 条循环、nconn 条连接
// =========================================================================

static bool run_phase(int loops, int nconn, const char* tag,
                      bool force_handoff = false) {
  bool ok = true;
  ServerRig rig;
  if (!rig.start(loops, force_handoff)) {
    std::cout << "  [" << tag << "] 服务端起不来\n";
    return false;
  }

  if (rig.loops_rc() != 0 || rig.listen_rc() != 0) {
    std::cout << "  [" << tag << "] set_loops=" << rig.loops_rc()
              << " listen=" << rig.listen_rc() << "（都不是 0）\n";
    rig.request_stop();
    return false;
  }

  // 这次跑的是**哪条腿**：`is_fanout()` 是绑定时探出来的（不是编译期常量），
  // 所以这里印的是本次的真实结果。
  std::cout << "  [" << tag << "] 腿 = "
            << (rig.fanout() ? "REUSEPORT 内核分流" : "接受者 + 转手") << "\n";
  // 强制档的判据是**运行时探测的结果**，不是"我调过那个 setter 了" —— 否则
  // 这个新开关自己就是个静默失效的装置（调了、没生效、用例照样全绿）。
  if (force_handoff && rig.fanout()) {
    std::cout << "  [" << tag << "] [FAIL] 调过 set_handoff_forced(true)"
                 "之后 is_fanout() 仍为真\n";
    rig.request_stop();
    return false;
  }

  std::atomic<bool> release{false};
  std::atomic<int> good{0};
  std::vector<std::atomic<int>> stage(static_cast<size_t>(nconn));
  for (int i = 0; i < nconn; ++i) stage[static_cast<size_t>(i)].store(0);

  std::vector<std::thread> clients;
  clients.reserve(static_cast<size_t>(nconn));
  for (int i = 0; i < nconn; ++i) {
    clients.push_back(std::thread([&rig, &release, &good, &stage, i]() {
      // **客户端对象活在本 lambda 的作用域里**：它必须活过下面那段等待，
      // 否则连接一析构就关了，"保持打开"这件事白做。
      uvcpp_tcp_client client;
      if (!client_echo(client, rig.port, i, stage.data())) return;

      // 先报成功、再挂着等放行 —— 反过来的话主线程那条"全部回显完"的判据
      // 要等到整段等完才可能成立。
      good.fetch_add(1);

      // 有界地保持连接打开（上限只是兜底：主线程真出事时别把用例挂死）。
      const std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (!release.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      // 出作用域 = 析构 = 关连接：服务端应当观察到断开并把登记表清干净。
    }));
  }

  // --- 判据 4：全部回显正确 ---
  //
  // 这条**不**用 `require_within`：客户端每一段都有 5 s 上限，所以这里不会
  // 无限挂；而"没回来"到底是**哪一段**没回来（连不上 / 写不动 / 读不到 /
  // 读到的不是发出去的那串）是必须自报家门的诊断 —— 三种成因的修法完全不同。
  {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (good.load() != nconn &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (good.load() != nconn) {
      std::cout << "  [" << tag << "] 回显 " << good.load() << "/" << nconn
                << "，服务端 accepted=" << rig.accepted() << "（每档:";
      for (int i = 0; i < nconn; ++i) {
        std::cout << " " << stage[static_cast<size_t>(i)].load();
      }
      std::cout << "；100+=connect 失败, 200+=write 失败, 300+=read 失败, "
                   "400=内容不符, 1=成功）\n";
      release.store(true);
      for (size_t i = 0; i < clients.size(); ++i) {
        if (clients[i].joinable()) clients[i].join();
      }
      rig.request_stop();
      require_within([&rig]() { return rig.exited(); }, 15000, "服务端析构返回");
      return false;
    }
  }
  std::cout << "  [" << tag << "] 回显 " << good.load() << "/" << nconn << "\n";

  uvcpp_tcp_server* srv = rig.server();

  // --- 判据 1：分布 ---
  //
  // 等到登记表装满：回显完成**晚于**登记（登记发生在 accept 尾巴里），所以
  // 这里通常已经是满的；留一个有界等待是为了不把"时序"当判据。
  require_within([srv, nconn]() {
    return srv->client_count() == static_cast<size_t>(nconn);
  }, 10000, "登记表条数 == 连接数");

  size_t per_loop[64];
  for (int i = 0; i < 64; ++i) per_loop[i] = 0;
  const int nloops = rig.server()->loop_count();
  for (int i = 0; i < nloops && i < 64; ++i) {
    per_loop[i] = srv->client_count_at(i);
  }
  if (nloops != loops) {
    std::cout << "  [" << tag << "] loop_count=" << nloops << "，期望 " << loops
              << "\n";
    ok = false;
  }
  if (srv->client_count_at(9999) != 0) {
    std::cout << "  [" << tag << "] client_count_at(越界) 不是 0\n";
    ok = false;
  }

  if (loops == 1) {
    // 对照组：acceptor 自己就是唯一的循环，连接当然全挂在它名下。
    if (per_loop[0] != static_cast<size_t>(nconn)) {
      std::cout << "  [" << tag << "] n==1 时连接没挂在唯一那条循环上（"
                << per_loop[0] << "/" << nconn << "）\n";
      ok = false;
    }
  } else if (rig.fanout()) {
    // ---- 内核分流：0 号**自己也在收**，分布由内核哈希定。 ----
    //
    // 能当判据的只有"守恒"：每条连接只落一处，一条不多、一条不少。**不在
    // Linux 上断言分布均匀**，也不断言"每条循环都分到过" —— 内核哈希两样都
    // 不保证（实测 16 条在 n=4 下拿到过 {5,3,3,5} 这类形状），拿它当判据就是
    // 给 flaky 留门。转手那条路的精确轮转判据整体降级到下面那一支。
    size_t sum = 0;
    std::string shown;
    for (int i = 0; i < nloops; ++i) {
      shown += " " + std::to_string(per_loop[static_cast<size_t>(i)]);
      sum += per_loop[static_cast<size_t>(i)];
    }
    std::cout << "  [" << tag << "] 分流分布（含 0 号）:" << shown << "，合计 "
              << sum << "\n";
    if (sum != static_cast<size_t>(nconn)) {
      std::cout << "  [" << tag << "] 合计不等于连接数（有连接被记了两处或没记）\n";
      ok = false;
    }
  } else {
    if (per_loop[0] != 0) {
      std::cout << "  [" << tag << "] 接受者循环留了 " << per_loop[0]
                << " 条连接（一条都不该留）\n";
      ok = false;
    }
    std::multiset<size_t> dist;
    size_t sum = 0;
    for (int i = 1; i < nloops; ++i) {
      dist.insert(per_loop[i]);
      sum += per_loop[i];
    }
    std::string shown;
    for (std::multiset<size_t>::const_iterator it = dist.begin();
         it != dist.end(); ++it) {
      shown += " " + std::to_string(*it);
    }
    std::cout << "  [" << tag << "] 分布（工作循环）:" << shown << "，合计 "
              << sum << "\n";

    if (sum != static_cast<size_t>(nconn)) {
      std::cout << "  [" << tag << "] 合计不等于连接数\n";
      ok = false;
    }
    // 每条工作循环都必须拿到过连接 —— 这是判据本身，不是"均匀度阈值"。
    for (int i = 1; i < nloops; ++i) {
      if (per_loop[i] == 0) {
        std::cout << "  [" << tag << "] 第 " << i << " 条工作循环一条都没拿到\n";
        ok = false;
      }
    }
    // 显式轮转下 16 条分给 3 条必然得到 {6,5,5}；拿这个当判据可以，因为它
    // 与**接受顺序**无关（只看条数）。
    if (loops == 4 && nconn == 16) {
      std::multiset<size_t> want;
      want.insert(6);
      want.insert(5);
      want.insert(5);
      if (dist != want) {
        std::cout << "  [" << tag << "] 分布不是 {6,5,5}（轮转是确定的）\n";
        ok = false;
      }
    }
  }

  // --- 判据 2/3：线程身份 ---
  std::map<uvcpp_tcp_client*, std::thread::id> conn_tid;
  std::map<uvcpp_tcp_client*, std::thread::id> read_tid;
  rig.thread_ids(&conn_tid, &read_tid);

  const std::thread::id acceptor = rig.acceptor_tid();
  std::set<std::thread::id> used;
  for (std::map<uvcpp_tcp_client*, std::thread::id>::const_iterator it =
           conn_tid.begin();
       it != conn_tid.end(); ++it) {
    used.insert(it->second);
  }
  std::cout << "  [" << tag << "] on_connection 用到的线程数: " << used.size()
            << "（接受者 "
            << (used.count(acceptor) != 0 ? "也在其中" : "不在其中") << "）\n";

  if (loops == 1) {
    // --- 判据 5：对照组 ---
    if (used.size() != 1 || used.count(acceptor) == 0) {
      std::cout << "  [" << tag << "] n==1 时 on_connection 必须就在接受者线程上\n";
      ok = false;
    }
  } else if (rig.fanout()) {
    // 分流：**0 号也会承载连接**，所以"接受者线程上一条都不该有"不成立；能断
    // 言的只有"回调没有超出这台服务端自己的循环数"。至于"每条连接的回调都在
    // 它自己那条循环的线程上"，由下面判据 3 逐连接比对。
    if (used.size() > static_cast<size_t>(loops)) {
      std::cout << "  [" << tag << "] on_connection 出现在 " << used.size()
                << " 个线程上，超过循环总数 " << loops << "\n";
      ok = false;
    }
  } else {
    if (used.count(acceptor) != 0) {
      std::cout << "  [" << tag << "] 接受者线程上跑了连接回调（该一条都不留）\n";
      ok = false;
    }
    if (used.size() != static_cast<size_t>(loops - 1)) {
      std::cout << "  [" << tag << "] 工作循环线程数 " << used.size()
                << "，期望 " << (loops - 1) << "\n";
      ok = false;
    }
  }

  // 判据 3：同一条连接的回调必须全在同一条线程上。
  size_t compared = 0;
  for (std::map<uvcpp_tcp_client*, std::thread::id>::const_iterator it =
           read_tid.begin();
       it != read_tid.end(); ++it) {
    std::map<uvcpp_tcp_client*, std::thread::id>::const_iterator c =
        conn_tid.find(it->first);
    if (c == conn_tid.end()) {
      std::cout << "  [" << tag << "] 有读回调跑到了一条没登记 on_connection 的连接上\n";
      ok = false;
      continue;
    }
    ++compared;
    if (c->second != it->second) {
      std::cout << "  [" << tag << "] 同一条连接的回调跨了线程（读 ≠ 连接）\n";
      ok = false;
    }
  }
  if (compared != static_cast<size_t>(nconn)) {
    std::cout << "  [" << tag << "] 只比对了 " << compared << "/" << nconn
              << " 条连接的线程身份\n";
    ok = false;
  }

  // --- 判据 6：收尾 ---
  release.store(true);
  for (size_t i = 0; i < clients.size(); ++i) {
    if (clients[i].joinable()) clients[i].join();
  }

  require_within([srv]() { return srv->client_count() == 0; }, 15000,
                 "客户端全关之后 client_count() 回到 0");

  rig.request_stop();
  require_within([&rig]() { return rig.exited(); }, 15000,
                 "服务端析构返回（工作线程全部 stop + join 完）");

  std::cout << "  [" << tag << "] " << (ok ? "PASS" : "FAIL") << "\n";
  return ok;
}

// =========================================================================
// API 边界：取值校验 + 装好之后不能再改
// =========================================================================

static bool test_api_guards() {
  bool ok = true;
  uvcpp_tcp_server server;

  if (server.loop_count() != 1) {
    std::cout << "  默认 loop_count != 1\n";
    ok = false;
  }
  if (server.set_loops(0) != UV_EINVAL) { std::cout << "  set_loops(0) 没红\n"; ok = false; }
  if (server.set_loops(65) != UV_EINVAL) { std::cout << "  set_loops(65) 没红\n"; ok = false; }
  if (server.set_loops(1) != 0) { std::cout << "  set_loops(1) 该是幂等空操作\n"; ok = false; }
  if (server.set_loops(1) != 0) { std::cout << "  set_loops(1) 第二次该还是 0\n"; ok = false; }
  if (server.loop_count() != 1) { std::cout << "  set_loops(1) 之后 loop_count 变了\n"; ok = false; }

  // 真起 2 条循环（1 acceptor + 1 worker）：不起线程这条路径没被测过。
  if (server.set_loops(2) != 0) { std::cout << "  set_loops(2) 失败\n"; ok = false; }
  if (server.loop_count() != 2) { std::cout << "  set_loops(2) 之后 loop_count != 2\n"; ok = false; }
  if (server.set_loops(3) != UV_EBUSY) { std::cout << "  装好之后还能改\n"; ok = false; }
  if (server.client_count_at(1) != 0) { std::cout << "  空表上 client_count_at(1) != 0\n"; ok = false; }

  // 已经 listen（bind 就置了 LISTENING 位）之后不许再改。
  if (server.bind("127.0.0.1", 0) != 0) { std::cout << "  bind 失败\n"; ok = false; }
  if (server.set_loops(4) != UV_EBUSY) { std::cout << "  跑了之后还能改循环数\n"; ok = false; }

  std::cout << "  [api_guards] " << (ok ? "PASS" : "FAIL") << "\n";
  return ok;
}

// =========================================================================
// 就绪钩子：`set_loop_start_hook()` 在每条工作循环上、开跑之前各调一次
//
// 这条 API 是给 webapp 那半边的每循环容器用的：per-loop 的 `uv_async` /
// `uv_timer` 只能建在循环线程上，而 `set_loops()` 返回时线程已经在跑了。
// 这里量的是它的**时序契约**，因为契约错了就是"配了却没生效"那种最难查的形状。
// =========================================================================

/** @brief 钩子观察到的事实（worker 线程写、主线程读，用锁护）。 */
struct HookLog {
  mutable std::mutex mu;
  std::map<int, std::thread::id> tid;  ///< 循环号 -> 钩子所在地线程
  int calls = 0;
  /** @brief 第一次钩子跑时已接受的连接数 —— 时序判据，恒 0。 */
  int accepted_at_hook = -1;
  /** @brief `set_loops()` 返回那一刻已经调过几次。 */
  int calls_at_return = -1;
  std::thread::id acceptor;
  std::set<std::thread::id> conn_tids;  ///< `on_connection` 里看到的线程
};

static bool test_loop_start_hook() {
  bool ok = true;

  HookLog log;
  std::atomic<int> accepted{0};
  std::atomic<int> stop{0};
  std::atomic<int> exited{0};
  std::atomic<int> loops_rc{-999};
  std::atomic<bool> fanout{false};
  std::promise<int> port_p;
  std::future<int> port_f = port_p.get_future();

  std::thread srv([&]() {
    log.acceptor = std::this_thread::get_id();
    {
      uvcpp_tcp_server server;

      // **装的顺序就是被测物**：钩子必须在 `set_loops()` 之前装。
      server.set_loop_start_hook([&log, &accepted](int idx, uvcpp_loop* lp) {
        if (lp == nullptr) return;  // 钩子不该拿到空循环
        std::lock_guard<std::mutex> lk(log.mu);
        log.tid[idx] = std::this_thread::get_id();
        ++log.calls;
        // 只看第一次：`listen()` 排在 `set_loops()` 之后，所以钩子跑的时候
        // **一条连接都不该被接受过**。
        if (log.accepted_at_hook < 0) log.accepted_at_hook = accepted.load();
      });

      loops_rc.store(server.set_loops(4));
      {
        std::lock_guard<std::mutex> lk(log.mu);
        log.calls_at_return = log.calls;
      }

      if (server.bind("127.0.0.1", 0) != 0) {
        port_p.set_value(-1);
        return;
      }
      fanout.store(server.is_fanout());
      sockaddr_in name;
      int namelen = static_cast<int>(sizeof(name));
      server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name),
                                    &namelen);

      // 连接回调只记账、**不装读回调**：本段要的是"连接稳住别掉"，好让
      // `client_count_at()` 读得到分布。
      const int lrc = server.listen(
          [&log, &accepted](uvcpp_tcp_client*) {
            std::lock_guard<std::mutex> lk(log.mu);
            log.conn_tids.insert(std::this_thread::get_id());
            accepted.fetch_add(1);
          },
          128);
      port_p.set_value(lrc == 0 ? ntohs(name.sin_port) : -2);

      while (!stop.load()) {
        server.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    exited.store(1);
  });

  const int port = port_f.get();
  if (port <= 0) {
    std::cout << "  [hook] 服务端起不来（port=" << port << "）\n";
    stop.store(1);
    if (srv.joinable()) srv.join();
    return false;
  }

  // --- 判据 1：调了几次、调在哪几条循环上 ---------------------------------
  {
    std::lock_guard<std::mutex> lk(log.mu);
    if (loops_rc.load() != 0) {
      std::cout << "  [hook] set_loops(4) 失败：" << loops_rc.load() << "\n";
      ok = false;
    }
    if (log.calls != 3) {
      std::cout << "  [hook] 钩子调了 " << log.calls << " 次（该 3 次）\n";
      ok = false;
    }
    if (log.tid.size() != 3 || log.tid.find(1) == log.tid.end() ||
        log.tid.find(2) == log.tid.end() || log.tid.find(3) == log.tid.end()) {
      std::cout << "  [hook] 循环号不是 {1,2,3}（0 号是接受者，不该走钩子）\n";
      ok = false;
    }
    // --- 判据 2：`set_loops()` 返回时，钩子必须都已经跑完 ------------------
    // 这条是全部时序保证的落点：它成立，才谈得上"连接到达时钩子已经跑过"。
    if (log.calls_at_return != 3) {
      std::cout << "  [hook] set_loops() 返回时只调了 " << log.calls_at_return
                << " 次（该 3 次）—— 钩子跑在放行之前这条契约破了\n";
      ok = false;
    }
    // --- 判据 3：钩子跑的时候还没有任何连接 ---------------------------------
    if (log.accepted_at_hook != 0) {
      std::cout << "  [hook] 第一次钩子跑时已经有 " << log.accepted_at_hook
                << " 条连接被接受\n";
      ok = false;
    }
    // --- 判据 4：每条工作循环一个**独立**线程，且都不是接受者 ---------------
    std::set<std::thread::id> tids;
    for (std::map<int, std::thread::id>::const_iterator it = log.tid.begin();
         it != log.tid.end(); ++it) {
      tids.insert(it->second);
    }
    if (tids.size() != 3) {
      std::cout << "  [hook] 三条工作循环只落在 " << tids.size()
                << " 个线程上\n";
      ok = false;
    }
    if (tids.count(log.acceptor) != 0) {
      std::cout << "  [hook] 有一条钩子跑在**接受者**线程上\n";
      ok = false;
    }
  }

  // --- 判据 5：连接真的落在钩子记下的那些线程上 ---------------------------
  {
    std::vector<uvcpp_tcp_client*> clients;
    for (int i = 0; i < 6; ++i) {
      uvcpp_tcp_client* c = new uvcpp_tcp_client();
      if (c->connect_wait("127.0.0.1", port, 5000) != 0) {
        std::cout << "  [hook] 第 " << i << " 条客户端连不上\n";
        delete c;
        ok = false;
        break;
      }
      clients.push_back(c);
    }

    if (ok) {
      require_within([&accepted]() { return accepted.load() == 6; }, 10000,
                     "6 条连接全部被接受");

      std::lock_guard<std::mutex> lk(log.mu);
      std::set<std::thread::id> hook_tids;
      for (std::map<int, std::thread::id>::const_iterator h = log.tid.begin();
           h != log.tid.end(); ++h) {
        hook_tids.insert(h->second);
      }
      if (log.conn_tids.empty()) {
        std::cout << "  [hook] 6 条连接一条都没回调\n";
        ok = false;
      }
      if (fanout.load()) {
        // **分流：0 号自己也在收，而 0 号本来就不走钩子**（`set_loop_start_hook`
        // 只对 1..n-1 调）⇒ "没跑过钩子的线程"不再等于"不属于本服务端的线程"，
        // 那条判据在这条路上必然为假。能断言的确定性判据是"每条连接都落在本
        // 服务端自己的循环线程上"：要么是某条钩子线程，要么就是接受者。
        for (std::set<std::thread::id>::const_iterator it =
                 log.conn_tids.begin();
             it != log.conn_tids.end(); ++it) {
          if (hook_tids.count(*it) == 0 && *it != log.acceptor) {
            std::cout << "  [hook] 有连接跑在一条**不属于本服务端**的线程上\n";
            ok = false;
            break;
          }
        }
      } else {
        // 6 条连接分给 3 条工作循环，轮转是显式的 ⇒ {2,2,2}。
        if (log.conn_tids.size() != 3) {
          std::cout << "  [hook] 6 条连接的 on_connection 只出现在 "
                    << log.conn_tids.size() << " 个线程上（该 3 个）\n";
          ok = false;
        }
        for (std::set<std::thread::id>::const_iterator it =
                 log.conn_tids.begin();
             it != log.conn_tids.end(); ++it) {
          bool found = false;
          for (std::map<int, std::thread::id>::const_iterator h =
                   log.tid.begin();
               h != log.tid.end(); ++h) {
            if (h->second == *it) { found = true; break; }
          }
          if (!found) {
            std::cout << "  [hook] 有连接跑在一个**没有跑过钩子**的线程上\n";
            ok = false;
            break;
          }
        }
      }
    }

    // 析构即关闭（自建循环那个构造函数本来就是这个契约）。这一段的目的是
    // 让 6 条连接掉干净，好让服务端那条线程收尾时不欠账。
    for (size_t i = 0; i < clients.size(); ++i) {
      clients[i]->close();
      delete clients[i];
    }
  }

  stop.store(1);
  require_within([&exited]() { return exited.load() != 0; }, 15000,
                 "服务端析构返回");
  if (srv.joinable()) srv.join();

  // --- API 边界：装晚了不生效（且不崩） -----------------------------------
  {
    uvcpp_tcp_server late;
    if (late.set_loops(2) != 0) {
      std::cout << "  [hook] 边界：set_loops(2) 失败\n";
      ok = false;
    }
    int after = 0;
    late.set_loop_start_hook([&after](int, uvcpp_loop*) { ++after; });
    if (after != 0) {
      std::cout << "  [hook] 边界：装晚了居然也被调用了\n";
      ok = false;
    }
  }

  std::cout << "  [loop_start_hook] " << (ok ? "PASS" : "FAIL") << "\n";
  return ok;
}

// =========================================================================
int main() {
  bool ok = true;

  std::cout << "[tcp_multiloop] api_guards\n";
  ok = test_api_guards() && ok;

  std::cout << "[tcp_multiloop] loop_start_hook\n";
  ok = test_loop_start_hook() && ok;

  std::cout << "[tcp_multiloop] control_n1\n";
  ok = run_phase(1, 4, "n=1") && ok;

  std::cout << "[tcp_multiloop] multi_n4\n";
  ok = run_phase(4, 16, "n=4") && ok;

  // 2 条循环那一档：只有一条工作线程，分布判据退化成"全部 6 条都在它上面"。
  // **它抓不到"全都投 0 号"那种坏实现**（只有一条工作循环，投给谁都一样）——
  // 那一档由 n=4 那条负责，这里只补"n 很小时也走同一套收尾"。
  std::cout << "[tcp_multiloop] multi_n2\n";
  ok = run_phase(2, 6, "n=2") && ok;

  // **转手腿（强制）**：`dup()` 那条 POSIX 路在 Linux 上永不被选中（REUSEPORT
  // 绑得上），在本机更编不到（在 `#else` 里）⇒ 不加这一档，它就只在 macOS 的
  // CI 腿上跑到，且"macOS 是否真的回落"无人验过。这一档把它拉到 Linux 上跑：
  // `set_handoff_forced(true)` ⇒ `is_fanout()` 必为假 ⇒ 下面那套分布 / 线程
  // 归属 / 收尾判据同时成了**转手路**的证据（连接必须真的被 dup+open 到别的
  // 循环上去，否则一条都收不到）。
  std::cout << "[tcp_multiloop] handoff_forced_n4\n";
  ok = run_phase(4, 16, "转手(强制 n=4)", true) && ok;

  std::cout << "[tcp_multiloop] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}
