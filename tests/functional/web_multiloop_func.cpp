/**
 * @file tests/functional/web_multiloop_func.cpp
 * @brief `uvcpp_http_server` 在 `set_loops(n > 1)` 下的**按循环切容器**。
 *
 * 本用例测的是 `uvcpp_http_server::contexts_` 这张**每请求都要碰**的表有没有
 * 真的按循环切开，以及由此带出来的三条：连接落在哪条循环上、一条连接的上下文
 * 是不是只在它自己那条循环上被碰、以及连接代次号（全服务一个序列）还唯不唯一。
 *
 * 起点是 net 层那条已有的路：`get_tcp_server()->set_loops(n)` 把"一条循环"变成
 * 「一条接受者 + n−1 条工作循环」，转手之后每条连接的全部回调都在它自己那条
 * 工作线程上跑（`tcp_multiloop_func.cpp` 已经把这条钉住了）。**那不算完** ——
 * 上层如果还共用一张 `contexts_`，几条工作线程就会并发 insert/find/erase 同一
 * 张 `std::map`，那是 UB，不是"慢一点"。
 *
 * 判据逐条（以及"坏实现会让它怎么红"）：
 *
 * 1. **对照组 n == 1 逐字不变** —— 请求全部答对、连接全挂在 0 号循环上、
 *    `on_connection` 就在接受者线程上、`loop_count() == 1` 且一条工作线程都没有。
 *    没有这个对照组，"n>1 也答对了"证明不了是切表切对了。
 * 2. **分布是确定的** —— 转手是显式轮转，所以 16 条连接分给 3 条工作循环必然
 *    得到 {6,5,5}，而 0 号（接受者）**一条都不留**。坏实现（全投 0 号）红。
 *    这是 `doc/multiloop-design.md` §6 定的可红判据，**不拍均匀度阈值**。
 * 3. **一条连接的请求在它自己那条循环上被处理** —— 路由处理函数里记的线程 id
 *    必须等于该连接 `on_connection` 里记的那个，且 n>1 时一条都不落在接受者
 *    线程上。坏实现（上下文跟着别的循环走 / 转手没真发生）红。
 * 4. **代次号跨循环唯一，且请求里回读得到** —— `connection_generation()` 是
 *    全服务一个序列，16 条连接必须拿到 16 个互不相同的号；而且每条连接的
 *    处理函数里回读到的那个号，必须就是它 `on_connection` 时发出的那个。
 *    **实测这条才是切表切错时真正会响的那一条**（见下面判据 5 的变异结论）：
 *    共用一张表时几条线程并发 insert/erase 会把它搅坏，
 *    `connection_generation()` 于是查不到、如实返回 0，16 条连接读出来的是
 *    同一个值 ⇒ "16 次发出、1 个不同的号" + "有连接的代次号是 0"。
 *    （**不是**"计数器丢更新"——那个计数器是原子的，一开始就是为了别处不撞号。）
 * 5. **压力形状：并发建/拆** —— n>1 时 16 条连接**同时**建起来、跑完请求、
 *    再**同时**拆掉，重复若干轮。上面那些判据都要靠这个形状才够得着：一条
 *    一条地跑，共用一张表也能全绿（`seq_n4` 那一档就是专门摆出来对照这个的）。
 *
 *    **这条必须说清它是什么**：它是一条**压力形状**的判据，不是确定性的 ——
 *    数据竞争本来就没有确定性的观测方式。变异结论（把 `ctxs_at()` 改成恒返回
 *    `contexts_[0]`）实测：连跑 **10 次里 9 次红**，形状两种 —— 多数是
 *    **进程崩溃**（跑得比 ctest 的判据还早，一个字都不打），少数走上面判据 4
 *    打印出来；**剩下 1 次侥幸全绿**。所以这条判据的正确用法是"红了一定有
 *    问题"，**不是**"绿了就没问题" —— 想确认没串台要连跑几遍，或者直接上
 *    页面堆门禁（`run_pageheap_gate.py`；变异版的裸跑基线在那上面也是当场崩）。
 * 6. **收尾不挂** —— 客户端全关之后 `client_count()` 回到 0，析构在墙钟上界内
 *    返回。每条等待都带墙钟上限，超时就**自报家门**（哪一条判据等不到）然后
 *    就地结束进程 —— 不让它变成一条只有 ctest `Timeout` 的红。
 *
 * **不在本用例里的**：TLS 的多循环语义（§4.4 要求"每条循环的 `SSL` 对象是它
 * 自己的"，那要等 webapp 那半边）；webapp 层（`uvcpp_web_app::set_loops` 还
 * 不存在）；POSIX 那条 `dup()` 路 —— 本机是 Windows，那条路**编得到、跑不到**，
 * 证据在 CI 的 ubuntu/macOS 腿上。不要在记录里写成"两端都验过"。
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

#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

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
// 服务端装置：接受者循环 + n−1 条工作线程，回一个带序号的 body
// =========================================================================

class HttpRig {
 public:
  ~HttpRig() {
    request_stop();
    if (thread_.joinable()) thread_.join();
  }

  /**
   * @brief 起服务端（后台线程）。返回 false 表示端口没能发布出来。
   *
   * @param loops 传给 `set_loops()` 的值；1 = 对照组。
   */
  bool start(int loops) {
    std::promise<int> port_promise;
    std::future<int> port_fut = port_promise.get_future();

    thread_ = std::thread([this, loops, &port_promise]() {
      acceptor_tid_ = std::this_thread::get_id();

      // 内层作用域是**故意的**：`exited_` 要在服务端析构返回**之后**才置位，
      // 第 6 条判据量的就是"析构里那些 join 有没有卡住"。
      {
        uvcpp_http_server server;
        live_.store(&server);

        // **顺序是被测的一部分**：`set_loops()` 必须在 `listen()` 之前 ——
        // `set_loops` 之后返回 `UV_EBUSY`，而 `uvcpp_http_server::listen()` 里
        // 表就是按 `loop_count()` 定尺寸的。装晚了这张表只有一份。
        loops_rc_.store(server.get_tcp_server()->set_loops(loops));
        loop_count_.store(server.get_tcp_server()->loop_count());

        // `/e<n>` → body `c<n>`。**每个请求都在连接自己那条循环的线程上跑**，
        // 所以处理函数里记的线程 id 就是判据 3 的被测物。
        //
        // 一条路由一个下标（而不是一条 `/echo` 读查询串）是**故意的**：这层的
        // 路由是"方法与路径全等"匹配，没有前缀匹配，也没有查询串解析 —— 那两样
        // 都是 webapp 层的（`web_parse_query`）。用查询串会顺带测到一条本层
        // 根本没有的路径，红了也说不清是谁的问题。
        for (int i = 0; i < 32; ++i) {
          const std::string path = "/e" + std::to_string(i);
          const std::string body = "c" + std::to_string(i);
          server.get(path, [this, body](uvcpp_http_request&,
                                        uvcpp_http_response& resp,
                                        uvcpp_tcp_client* client) {
            {
              std::lock_guard<std::mutex> lk(mu_);
              req_tid_[client] = std::this_thread::get_id();
              gen_seen_.insert(server_generation(client));
            }
            resp = uvcpp_http_response::ok(body.c_str(), body.size());
          });
        }

        if (server.bind("127.0.0.1", 0) != 0) {
          port_promise.set_value(-1);
          return;
        }

        sockaddr_in name;
        int namelen = static_cast<int>(sizeof(name));
        server.get_tcp_server()->get_tcp()->getsockname(
            reinterpret_cast<sockaddr*>(&name), &namelen);

        // 连接钩子：记线程身份与代次号，**早于这条连接上的任何请求**。
        server.on_connection([this](uvcpp_tcp_client* client) {
          std::lock_guard<std::mutex> lk(mu_);
          conn_tid_[client] = std::this_thread::get_id();
          gen_issued_.push_back(server_generation(client));
          accepted_.fetch_add(1);
        });

        const int lrc = server.listen();
        listen_rc_.store(lrc);

        // 端口**在 listen 成功之后**才发布：回环上没人监听的端口不给 RST，
        // 连接会停在 SYN_SENT 挂住 —— 先发布端口的话客户端可能撞进那个窗口。
        port_promise.set_value(lrc == 0 ? ntohs(name.sin_port) : -2);

        while (!stop_.load()) {
          server.run(UV_RUN_NOWAIT);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }

      exited_.store(true);
    });

    const int p = port_fut.get();
    if (p <= 0) return false;
    port = p;
    return true;
  }

  /** @brief `connection_generation()`：**必须在服务端线程那边问**，见实现。 */
  uint64_t server_generation(uvcpp_tcp_client* client) {
    uvcpp_http_server* s = live_.load();
    return (s != nullptr) ? s->connection_generation(client) : 0;
  }

  void request_stop() { stop_.store(true); }
  bool exited() const { return exited_.load(); }
  std::thread::id acceptor_tid() const { return acceptor_tid_; }
  int loops_rc() const { return loops_rc_.load(); }
  int listen_rc() const { return listen_rc_.load(); }
  int loop_count() const { return loop_count_.load(); }
  int accepted() const { return accepted_.load(); }
  uvcpp_tcp_server* tcp() const {
    uvcpp_http_server* s = live_.load();
    return (s != nullptr) ? s->get_tcp_server() : nullptr;
  }

  /** @brief 每条连接 `on_connection` / 路由处理函数里看到的线程 id。 */
  void thread_ids(std::map<uvcpp_tcp_client*, std::thread::id>* conn,
                  std::map<uvcpp_tcp_client*, std::thread::id>* req) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (conn != nullptr) *conn = conn_tid_;
    if (req != nullptr) *req = req_tid_;
  }

  /** @brief `on_connection` 那一刻发出的代次号（按发出顺序）。 */
  std::vector<uint64_t> issued_generations() const {
    std::lock_guard<std::mutex> lk(mu_);
    return gen_issued_;
  }

  /** @brief 路由处理函数里回读到的代次号（应当与发出的那些是同一个集合）。 */
  std::set<uint64_t> seen_generations() const {
    std::lock_guard<std::mutex> lk(mu_);
    return gen_seen_;
  }

  int port = 0;

 private:
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> exited_{false};
  std::atomic<uvcpp_http_server*> live_{nullptr};
  std::thread::id acceptor_tid_;

  std::atomic<int> loops_rc_{-999};
  std::atomic<int> listen_rc_{-999};
  std::atomic<int> loop_count_{-1};
  std::atomic<int> accepted_{0};

  mutable std::mutex mu_;
  std::map<uvcpp_tcp_client*, std::thread::id> conn_tid_;
  std::map<uvcpp_tcp_client*, std::thread::id> req_tid_;
  std::vector<uint64_t> gen_issued_;
  std::set<uint64_t> gen_seen_;
};

// =========================================================================
// 客户端：连上、请求、比对 body，然后**挂着不动**（好让分布看得见）
// =========================================================================

/**
 * @brief 一条连接跑完一次请求；返回后**连接还开着**（调用方负责保持/关闭）。
 *
 * 连接保持打开是判据 2 需要的：`client_count_at()` 读的是**当前登记的**连接数，
 * 一关就掉。所以"保持"这件事放在调用方（它才能决定什么时候放），这里只负责
 * 把请求做完 —— 顺序很重要：先报成功、再挂着等，否则"成功的计数"要等到整段
 * 等完才加上，主线程的等待判据就成了永远不成立。
 */
static bool client_echo(uvcpp_http_client& client, int port, int idx,
                        std::atomic<int>* stage) {
  const int crc = client.connect_wait("127.0.0.1", port, 5000);
  if (crc != 0) {
    stage[idx].store(100 + (crc < 0 ? -crc % 100 : crc % 100));
    return false;
  }

  const std::string path = "/e" + std::to_string(idx);
  uvcpp_http_response resp;
  const int src = client.send_wait(uvcpp_http_request::make_get(path), resp, 5000);
  if (src != 0) {
    stage[idx].store(200 + (src < 0 ? -src % 100 : src % 100));
    return false;
  }
  if (resp.status_code != http_status::OK) {
    stage[idx].store(300 + static_cast<int>(resp.status_code) % 100);
    return false;
  }
  const std::string want = "c" + std::to_string(idx);
  if (resp.body.to_string() != want) {
    stage[idx].store(400);
    return false;
  }
  stage[idx].store(1);
  return true;
}

// =========================================================================
// 一条完整相位：n 条循环、nconn 条连接、rounds 轮并发建/拆
// =========================================================================

static bool run_phase(int loops, int nconn, int rounds, const char* tag) {
  bool ok = true;
  HttpRig rig;
  if (!rig.start(loops)) {
    std::cout << "  [" << tag << "] 服务端起不来\n";
    return false;
  }

  if (rig.loops_rc() != 0 || rig.listen_rc() != 0) {
    std::cout << "  [" << tag << "] set_loops=" << rig.loops_rc()
              << " listen=" << rig.listen_rc() << "（都不是 0）\n";
    rig.request_stop();
    return false;
  }

  for (int round = 0; round < rounds; ++round) {
    std::atomic<bool> release{false};
    std::atomic<int> good{0};
    std::vector<std::atomic<int> > stage(static_cast<size_t>(nconn));
    for (int i = 0; i < nconn; ++i) stage[static_cast<size_t>(i)].store(0);

    std::vector<std::thread> clients;
    clients.reserve(static_cast<size_t>(nconn));
    for (int i = 0; i < nconn; ++i) {
      clients.push_back(std::thread([&rig, &release, &good, &stage, i]() {
        // **客户端对象活在本 lambda 的作用域里**：它必须活过下面那段等待，
        // 否则连接一析构就关了，"保持打开"这件事白做。
        uvcpp_http_client client;
        if (!client_echo(client, rig.port, i, stage.data())) return;

        // 先报成功、再挂着等 —— 反过来的话主线程那条"全部答完"的判据
        // 要等到整段等完才可能成立。
        good.fetch_add(1);

        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!release.load() && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // 出作用域 = 析构 = 关连接：服务端应当观察到断开并把登记表清干净。
      }));
    }

    // --- 判据 1/3 的前置：全部请求答对 ---
    //
    // 这条**不**用 `require_within`：客户端每一段都有 5 s 上限，所以这里不会
    // 无限挂；而"没回来"到底是**哪一段**没回来（连不上 / 发不出 / 状态码不对 /
    // body 不符）是必须自报家门的诊断 —— 四种成因的修法完全不同。
    {
      const std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (good.load() != nconn &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (good.load() != nconn) {
        std::cout << "  [" << tag << "] 第 " << round << " 轮应答 " << good.load()
                  << "/" << nconn << "，服务端 accepted=" << rig.accepted()
                  << "（每档:";
        for (int i = 0; i < nconn; ++i) {
          std::cout << " " << stage[static_cast<size_t>(i)].load();
        }
        std::cout << "；100+=connect 失败, 200+=send 失败, 300+=状态码不对, "
                     "400=body 不符, 1=成功）\n";
        release.store(true);
        for (size_t i = 0; i < clients.size(); ++i) {
          if (clients[i].joinable()) clients[i].join();
        }
        rig.request_stop();
        require_within([&rig]() { return rig.exited(); }, 15000, "服务端析构返回");
        return false;
      }
    }

    // --- 判据 2：分布（只在第一轮查，后面几轮连接是在同一批循环上重建的） ---
    uvcpp_tcp_server* srv = rig.tcp();
    if (round == 0) {
      require_within([srv, nconn]() {
        return srv->client_count() == static_cast<size_t>(nconn);
      }, 10000, "登记表条数 == 连接数");

      size_t per_loop[64];
      for (int i = 0; i < 64; ++i) per_loop[i] = 0;
      const int nloops = rig.loop_count();
      for (int i = 0; i < nloops && i < 64; ++i) {
        per_loop[i] = srv->client_count_at(i);
      }
      if (nloops != loops) {
        std::cout << "  [" << tag << "] loop_count=" << nloops << "，期望 "
                  << loops << "\n";
        ok = false;
      }
      if (srv->client_count_at(9999) != 0) {
        std::cout << "  [" << tag << "] client_count_at(越界) 不是 0\n";
        ok = false;
      }

      if (loops == 1) {
        if (per_loop[0] != static_cast<size_t>(nconn)) {
          std::cout << "  [" << tag << "] n==1 时连接没挂在唯一那条循环上（"
                    << per_loop[0] << "/" << nconn << "）\n";
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
        for (int i = 1; i < nloops; ++i) {
          if (per_loop[i] == 0) {
            std::cout << "  [" << tag << "] 第 " << i << " 条工作循环一条都没拿到\n";
            ok = false;
          }
        }
        // 显式轮转下 16 条分给 3 条必然得到 {6,5,5}；这条与**接受顺序**无关
        // （只看条数），所以是确定性的。
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

      // --- 判据 3：线程身份 ---
      std::map<uvcpp_tcp_client*, std::thread::id> conn_tid;
      std::map<uvcpp_tcp_client*, std::thread::id> req_tid;
      rig.thread_ids(&conn_tid, &req_tid);

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
        if (used.size() != 1 || used.count(acceptor) == 0) {
          std::cout << "  [" << tag << "] n==1 时 on_connection 必须就在接受者线程上\n";
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

      // 一条连接的请求必须跑在它自己那条循环的线程上。
      size_t compared = 0;
      for (std::map<uvcpp_tcp_client*, std::thread::id>::const_iterator it =
               req_tid.begin();
           it != req_tid.end(); ++it) {
        std::map<uvcpp_tcp_client*, std::thread::id>::const_iterator c =
            conn_tid.find(it->first);
        if (c == conn_tid.end()) {
          std::cout << "  [" << tag
                    << "] 有请求跑到了一条没登记 on_connection 的连接上\n";
          ok = false;
          continue;
        }
        ++compared;
        if (c->second != it->second) {
          std::cout << "  [" << tag << "] 同一条连接的请求跨了线程（请求 ≠ 连接）\n";
          ok = false;
        }
      }
      if (compared != static_cast<size_t>(nconn)) {
        std::cout << "  [" << tag << "] 只比对了 " << compared << "/" << nconn
                  << " 条连接的线程身份\n";
        ok = false;
      }

      // --- 判据 4：代次号跨循环唯一 ---
      const std::vector<uint64_t> issued = rig.issued_generations();
      const std::set<uint64_t> seen = rig.seen_generations();
      std::set<uint64_t> uniq(issued.begin(), issued.end());
      if (issued.size() != static_cast<size_t>(nconn)) {
        std::cout << "  [" << tag << "] on_connection 只记到 " << issued.size()
                  << " 个代次号（该 " << nconn << "）\n";
        ok = false;
      }
      if (uniq.size() != issued.size()) {
        std::cout << "  [" << tag << "] 代次号有重复（并发自增丢了更新）："
                  << issued.size() << " 次发出、" << uniq.size() << " 个不同的号\n";
        ok = false;
      }
      if (uniq.count(0) != 0) {
        std::cout << "  [" << tag << "] 有连接的代次号是 0（0 留给「不在表里」）\n";
        ok = false;
      }
      // 请求里回读到的那批必须就是发出的那批 —— 这条正是"上下文没串台"：
      // 串了的话连接 A 的解析上下文会被连接 B 看到，代次号也就对不上了。
      for (std::set<uint64_t>::const_iterator it = seen.begin();
           it != seen.end(); ++it) {
        if (uniq.count(*it) == 0) {
          std::cout << "  [" << tag << "] 请求里读到一个没发出过的代次号 " << *it
                    << "\n";
          ok = false;
        }
      }
      if (seen.size() != uniq.size()) {
        std::cout << "  [" << tag << "] 请求只见到 " << seen.size()
                  << " 个代次号，发出的是 " << uniq.size() << " 个\n";
        ok = false;
      }
    }

    // --- 判据 5/6：这一轮拆干净，下一轮重建 ---
    release.store(true);
    for (size_t i = 0; i < clients.size(); ++i) {
      if (clients[i].joinable()) clients[i].join();
    }
    require_within([srv]() { return srv->client_count() == 0; }, 15000,
                   "客户端全关之后 client_count() 回到 0");
  }

  std::cout << "  [" << tag << "] " << rounds << " 轮并发建/拆，"
            << (ok ? "PASS" : "FAIL") << "\n";

  rig.request_stop();
  require_within([&rig]() { return rig.exited(); }, 15000,
                 "服务端析构返回（工作线程全部 stop + join 完）");
  return ok;
}

// =========================================================================
// 顺序建、顺序拆：判据 3/4 在**没有并发**的形状下也必须成立
//
// 与上面那条相位是互补的：上面靠并发把"共用一张表"撞出来，这条把每条连接单独
// 摆开、逐条核身份。一条连接单独跑时线程身份错了，那一定是切表切错了，不是运气。
// =========================================================================

static bool test_sequential_identity(int loops, const char* tag) {
  bool ok = true;
  HttpRig rig;
  if (!rig.start(loops)) {
    std::cout << "  [" << tag << "] 服务端起不来\n";
    return false;
  }

  std::vector<uvcpp_http_client*> clients;
  std::vector<std::atomic<int> > stage(8);
  for (size_t i = 0; i < stage.size(); ++i) stage[i].store(0);
  for (int i = 0; i < 6; ++i) {
    uvcpp_http_client* c = new uvcpp_http_client();
    if (!client_echo(*c, rig.port, i, stage.data())) {
      std::cout << "  [" << tag << "] 第 " << i << " 条请求失败（stage="
                << stage[static_cast<size_t>(i)].load() << "）\n";
      delete c;
      ok = false;
      break;
    }
    clients.push_back(c);
  }

  if (ok) {
    require_within([&rig]() { return rig.accepted() == 6; }, 10000,
                   "6 条连接全部被接受");

    std::map<uvcpp_tcp_client*, std::thread::id> conn_tid;
    std::map<uvcpp_tcp_client*, std::thread::id> req_tid;
    rig.thread_ids(&conn_tid, &req_tid);

    std::set<std::thread::id> conns, reqs;
    for (std::map<uvcpp_tcp_client*, std::thread::id>::const_iterator it =
             conn_tid.begin();
         it != conn_tid.end(); ++it) {
      conns.insert(it->second);
    }
    for (std::map<uvcpp_tcp_client*, std::thread::id>::const_iterator it =
             req_tid.begin();
         it != req_tid.end(); ++it) {
      reqs.insert(it->second);
    }
    // 逐条比对：请求的线程必须等于同一条连接 on_connection 的线程。上面那条
    // 相位是**在并发里**比，这条是**单独一条一条**比 —— 后者错了没有借口。
    if (conn_tid.size() != req_tid.size()) {
      std::cout << "  [" << tag << "] on_connection " << conn_tid.size()
                << " 条、请求 " << req_tid.size() << " 条（该一样多）\n";
      ok = false;
    }
    if (loops > 1) {
      // 6 条连接、3 条工作循环 ⇒ 轮转必然让**每条工作循环都被用到**。
      if (conns.size() != static_cast<size_t>(loops - 1)) {
        std::cout << "  [" << tag << "] 6 条连接只落在 " << conns.size()
                  << " 条工作循环上（该 " << (loops - 1) << "）\n";
        ok = false;
      }
      if (conns.count(rig.acceptor_tid()) != 0) {
        std::cout << "  [" << tag << "] 有连接落在接受者线程上\n";
        ok = false;
      }
    }
  }

  // `uvcpp_http_client` 没有 `close()`：析构就是关连接。
  for (size_t i = 0; i < clients.size(); ++i) delete clients[i];

  rig.request_stop();
  require_within([&rig]() { return rig.exited(); }, 15000, "服务端析构返回");
  std::cout << "  [" << tag << "] " << (ok ? "PASS" : "FAIL") << "\n";
  return ok;
}

// =========================================================================
int main() {
  bool ok = true;

  // 对照组排在前面：没有它，"n>1 也对"证明不了是切表切对了。
  std::cout << "[web_multiloop] control_n1\n";
  ok = run_phase(1, 4, 2, "n=1") && ok;

  std::cout << "[web_multiloop] seq_n4\n";
  ok = test_sequential_identity(4, "seq n=4") && ok;

  std::cout << "[web_multiloop] multi_n4\n";
  ok = run_phase(4, 16, 3, "n=4") && ok;

  // 2 条循环那一档：只有一条工作循环，分布判据退化成"全部 6 条都在它上面"。
  // **它抓不到"全都投 0 号"那种坏实现**（只有一条工作循环，投给谁都一样）——
  // 那一档由 n=4 那条负责，这里只补"n 很小时也走同一套收尾"。
  std::cout << "[web_multiloop] multi_n2\n";
  ok = run_phase(2, 6, 2, "n=2") && ok;

  std::cout << "[web_multiloop] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}

#else  // !UVCPP_WEB_ENABLE
int main() {
  std::cout << "[web_multiloop] UVCPP_WEB_ENABLE=0，跳过\n";
  return 0;
}
#endif
