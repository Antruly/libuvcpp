/**
 * @file tests/functional/http3_multiloop_func.cpp
 * @brief `uvcpp_http_server::set_loops(n > 1)` 之下的 **h3 扇出**。
 *
 * 本用例钉的是 `listen_quic()` 的多循环那一支：n 个 QUIC 端点各带
 * `UV_UDP_REUSEPORT` 抢同一个端口，内核按四元组把数据报分给其中一个，于是
 * 每条连接落在某一条循环上，**它此后全部 h3 回调都在那条循环的线程上跑**。
 *
 * 判据逐条（以及"坏实现会让它怎么红"）：
 *
 * 1. **结构**：`loop_count() == n`；`sum_i quic_connection_count_at(i)` 恰好等于
 *    `quic_connection_count()`，越界的那一格返回 0。坏实现（表按 1 张定尺寸、
 *    端点只建一个）红。**扇出是否真的发生按 `quic_fanout()` 分叉** —— 它是
 *    **绑定时探出来的**，不是编译期常量。
 * 2. **分布**：扇出那一档上，非零的连接计数槽**至少两个** —— 这条是"内核分流
 *    真的发生了"的可红判据，也是扇出唯一能证明自己的地方。**不拍均匀度阈值**
 *    （同 `tcp_multiloop_func.cpp` 的立场：内核哈希不保证均匀，拿它当判据就是
 *    给 flaky 留门）。
 * 3. **连接归属与计数对得上**：每条连接由它**自己的**标记（URL 里的 `?c=`）
 *    认出来，处理函数记下"我在几号循环上"（`uvcpp_loop_index_of_this_thread()`）
 *    —— 于是"每个槽里到底是谁"是可问的：按标记归并出来的每槽条数必须与
 *    `quic_connection_count_at(i)` 逐格相等。这条比"合计相等"强得多：一个把
 *    所有连接都记到 0 号的坏实现也满足"合计相等"。
 * 4. **线程身份**：同一条连接的两次请求必须落在**同一个线程 id + 同一个循环号**
 *    上（端点按循环建 ⇒ 连接不会漂移）。
 * 5. **数据正确**：每次响应都是 200、交付给客户端的 `resp.version == HVER_30`、
 *    body 是处理函数写回的版本串 `"HTTP/3"`。这条同时是"扇出没有把响应送回别人
 *    那条连接上"的证据 —— 八条连接打同一个路径，谁的回话串了号就会在这里显形。
 * 6. **对照组 n == 1**：`set_loops(1)` 时 `quic_fanout()` 为假、连接全在 0 号、
 *    所有回调都在 acceptor 线程上。没有这个对照组，"换了线程"证明不了是
 *    `set_loops` 干的。
 * 7. **收尾不挂**：客户端全关之后服务端析构在墙钟上界内返回（工作循环全部
 *    stop + join 完，含"析构前把 1..n−1 号上的 h3 层与端点投回它们自己那条循环
 *    去拆"那一段）。每条等待都带墙钟上限，超时就**自报家门**（哪一条判据等不到）
 *    然后就地结束进程 —— 不让它变成一条只有 ctest `Timeout` 的红。
 *
 * 8. **共用一个客户端 TLS 上下文**：全部客户端线程**共用** `rig.client_ctx()`
 *    这**一个** `uvcpp_ssl_context` —— 真实用法就是这样（进程里一个上下文，
 *    多条连接并发建起来）。它顺带钉住一处分库缺陷：客户端 ALPN 曾经用
 *    `SSL_CTX_set_alpn_protos` 装到**共享**上下文上，而那个接口内部是"free 掉
 *    ctx 上旧的名单、再 malloc 一份新的" ⇒ 两条线程各自读到同一个旧指针、各
 *    free 一次。ASAN 下是"三字节区域上的 `attempting double-free`"（25 次里 6
 *    次），不开 ASAN 时是 glibc 堆损坏中止（30 次里 7 次）。改成本连接装到
 *    `SSL*`（`apply_alpn_to_ssl()`）之后两条腿都干净（30/30、25/25）。
 *    **这条的红是"进程被信号打死"，不是一行 FAIL** —— 所以它必须连着多跑几十遍
 *    才算数，跑一遍绿说明不了什么。
 *
 * 9. **"回落"要是真的**：前八条的判据**全部分叉在库自己报的数上**
 *    （`quic_fanout()` / `is_reuseport()`），而"扇出没建起来（端点循环写死 1）"
 *    或"标志被静默丢掉"报出来的形状与"平台拒绝标志"**一模一样** —— 于是在那两
 *    种坏实现下它们全绿。这一条拿 `reuseport_probe.h` 的库外探针问一次内核：
 *    内核给得出标志、而 `set_loops(n>1)` 也答应了的时候，`quic_fanout()` 必须
 *    为真。给不出的平台上这条自动跳过（回落本来就是对的）。
 *
 * 另有一小节 **API 边界**：取值校验、`listen_quic()` 之后不许再改循环数、
 * 重复 `listen_quic()`、以及"没装 TLS 上下文时 `UV_EINVAL`"。
 *
 * **平台差异**：`UV_UDP_REUSEPORT` 要 libuv 1.50+，且 Windows 上 libuv 无条件
 * 拒绝、macOS 没实现 ⇒ 那些平台上 `listen_quic()` 自动回落成单端点。所以分布
 * 那几条判据**按 `quic_fanout()` 分叉**，不拿 `#ifdef` 猜平台；回落那一档上
 * 它们退化成第 6 条（对照组）那套形状 —— 而"这一档到底是平台给不出，还是实现
 * 把标志丢了"由判据 9 的库外探针回答。
 *
 * **为什么服务端在别的线程、每条客户端各占一条线程**：扇出之后服务端的回调跑在
 * 它自己的工作线程上，客户端**不可能**在同一条循环上泵 —— 而 ngtcp2 的时间戳
 * 取自 `uv_now()`、只在循环迭代时刷新，跨线程泵同一条循环会撞上它的单调性断言。
 * 所以：服务端线程泵接受者那条循环（工作循环由库自己起线程），每条客户端在自己
 * 的线程上泵自己那条循环（`uvcpp_http_client` 默认构造出来的 `uvcpp_tcp_client`
 * 自带一条循环）。分工与 `tcp_multiloop_func.cpp` 一致。
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

#include <uvcpp/uvcpp_define.h>

#include "reuseport_probe.h"

#if UVCPP_HTTP3_ENABLE

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <net/uvcpp_loop_worker.h>
#include <quic/uvcpp_quic_common.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

using namespace uvcpp;

namespace {

int g_checks   = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
  ++g_checks;
  if (!cond) {
    std::cout << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    std::cout << "  [FAIL] " << what << "：期望 " << want << "，实得 " << got
              << std::endl;
    ++g_failures;
  }
}

/**
 * @brief 等到 `pred` 成立；到点还是不成立就打印**是哪一条**等不到，然后退出。
 *
 * 与 `tcp_multiloop_func.cpp` 里那条同名函数同一个理由：等不到的那几条一旦真
 * 挂住，调用方**没有机会**去打印 —— 整条用例会一直挂到 ctest 的超时上限，红的
 * 形状变成一条 `Timeout`，看不出卡在哪一条判据上。
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
  std::cout << "  [FAIL] 等不到「" << what << "」（" << ms
            << " ms 墙钟上限）" << std::endl;  // endl 会 flush，下面 _Exit 不丢字
  std::_Exit(2);
}

// =========================================================================
// 处理函数看到的事实（服务端线程写、主线程读，用锁护）
// =========================================================================

struct Obs {
  mutable std::mutex mu;
  /// 标记（URL 里的 `?c=N`）-> 这条连接的回调所在线程。
  std::map<int, std::thread::id> conn_tid;
  /// 标记 -> 这条连接的回调所在循环号（`uvcpp_loop_index_of_this_thread()`）。
  std::map<int, int> conn_loop;
  /// 标记 -> 这条连接上被处理过的请求数。
  std::map<int, int> req_seen;
  /// 同一条连接上"两次请求落在不同地方"的次数（恒 0 才是对的）。
  int drifting = 0;

  void note(int marker, int loop_index, std::thread::id tid) {
    std::lock_guard<std::mutex> lk(mu);
    std::map<int, int>::iterator l = conn_loop.find(marker);
    if (l == conn_loop.end()) {
      conn_loop[marker] = loop_index;
      conn_tid[marker]  = tid;
    } else if (l->second != loop_index || conn_tid[marker] != tid) {
      // 同一条连接的回调换了循环 / 换了线程 —— 这正是判据 4 要抓的形状。
      ++drifting;
    }
    ++req_seen[marker];
  }

  /// 某条连接上被处理过的请求数（0 = 这条连接的标记一次都没出现过）。
  int seen(int marker) const {
    std::lock_guard<std::mutex> lk(mu);
    std::map<int, int>::const_iterator it = req_seen.find(marker);
    return it == req_seen.end() ? 0 : it->second;
  }
};

/// URL 的 `?c=N`（没有就返回 -1）。
int marker_of(const std::string& url) {
  const size_t q = url.find("?c=");
  if (q == std::string::npos) return -1;
  return std::atoi(url.c_str() + q + 3);
}

// =========================================================================
// 服务端装置：后台线程 + 接受者的那条循环由它自己泵
// =========================================================================

class H3Rig {
 public:
  H3Rig() { client_ctx_.set_verify_mode(tls_verify_mode::NONE); }

  ~H3Rig() {
    request_stop();
    if (thread_.joinable()) thread_.join();
  }

  /**
   * @brief 起 h3 服务端（后台线程）。返回 false 表示端口没发布出来。
   *
   * @param loops     传给 `set_loops()` 的值；1 = 对照组。
   * @param bind_port 绑这个端口；**0 = 让内核挑**（本用例走的就是这条：扇出要求
   *                  所有端点绑同一个端口，而那个"真号由 0 号解析出来再传给
   *                  1..n−1 号"的次序只有写 0 才测得到）。
   */
  bool start(int loops, int bind_port = 0) {
    std::promise<int> port_promise;
    std::future<int>  port_fut = port_promise.get_future();

    thread_ = std::thread([this, loops, bind_port, &port_promise]() {
      acceptor_tid_ = std::this_thread::get_id();

      // 内层作用域是**故意的**：`exited_` 要在服务端析构返回**之后**才置位，
      // 判据 7 量的就是"析构里那些 join 有没有卡住"。
      {
        uvcpp_http_server server;
        live_.store(&server);

        if (!server_ctx_.generate_self_signed("localhost", 2048)) {
          port_promise.set_value(-3);
          return;
        }

        loops_rc_.store(server.set_loops(loops));
        loop_count_.store(server.loop_count());

        server.get("/transport", [this](uvcpp_http_request& req,
                                        uvcpp_http_response& resp,
                                        uvcpp_tcp_client*) {
          // **这一句就是本用例的核心量具**：h3 那条腿上处理函数拿到的 `client`
          // 是 nullptr（没有 `uvcpp_tcp_client` 这个东西），所以"这条连接在几号
          // 循环上"只能问当前线程 —— 而当前线程正是那条端点的线程。
          obs_.note(marker_of(req.url), uvcpp_loop_index_of_this_thread(),
                    std::this_thread::get_id());
          const std::string body = uvcpp_http_version_str(req.version);
          resp = uvcpp_http_response::ok(body.c_str(), body.size());
        });

        // 顺序按契约：装凭证 -> listen_quic（QUIC 没有明文模式）。
        server.set_quic_ssl_context(&server_ctx_);
        const int qrc = server.listen_quic(bind_port, "127.0.0.1");
        quic_rc_.store(qrc);
        if (qrc != 0) {
          port_promise.set_value(-2);
          return;
        }
        // 分流还是回落是**绑定时探出来的**，所以判据要问这次的结果。
        fanout_.store(server.quic_fanout());
        port = server.quic_listen_port();
        port_promise.set_value(port);

        while (!stop_.load()) {
          server.run(UV_RUN_NOWAIT);
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      }

      exited_.store(true);
    });

    const int p = port_fut.get();
    if (p <= 0) return false;
    return true;
  }

  void request_stop() { stop_.store(true); }

  bool               exited() const { return exited_.load(); }
  std::thread::id    acceptor_tid() const { return acceptor_tid_; }
  bool               fanout() const { return fanout_.load(); }
  int                loops_rc() const { return loops_rc_.load(); }
  int                quic_rc() const { return quic_rc_.load(); }
  int                loop_count() const { return loop_count_.load(); }
  const Obs&         obs() const { return obs_; }
  uvcpp_http_server* server() const { return live_.load(); }
  uvcpp_ssl_context* client_ctx() { return &client_ctx_; }

  int port = 0;

 private:
  std::thread                    thread_;
  std::atomic<bool>              stop_{false};
  std::atomic<bool>              exited_{false};
  std::atomic<uvcpp_http_server*> live_{nullptr};
  std::thread::id                acceptor_tid_;

  std::atomic<int>  loops_rc_{-999};
  std::atomic<int>  quic_rc_{-999};
  std::atomic<int>  loop_count_{-1};
  std::atomic<bool> fanout_{false};

  Obs obs_;
  /// QUIC 那条腿的凭证。服务端线程在**建对象之前**就要它，所以它由 `start()`
  /// 的线程体自己生成（`generate_self_signed` 要有 OpenSSL 的全局初始化，
  /// `quic_crypto_backend_init()` 已在 `main()` 里做过）。
  uvcpp_ssl_context server_ctx_{tls_mode::SERVER, tls_version::TLS_1_3};
  /// 客户端那份：自签证书 ⇒ 不验。**八条客户端线程共用一份** —— 多个线程从同
  /// 一个 `SSL_CTX` 建 `SSL*` 是 OpenSSL 1.1+ 明确支持的用法。
  uvcpp_ssl_context client_ctx_{tls_mode::CLIENT, tls_version::TLS_1_3};
};

// =========================================================================
// 客户端：一条连接、两次 `/transport`，然后**挂着不动**
// =========================================================================

/// 一"趟"往返的去处。**堆上、由 `shared_ptr` 共享给回调**。
///
/// 不拿栈上的局部量给回调捕引用是**承重**的：客户的回调不受我们控制 —— 交给
/// `send()` 之后它就被存进客户端的流表里，什么时候被叫、乃至**还叫不叫**都不由
/// 这一段决定。回调晚于本函数返回时，捕引用的那套写法写的是已经消失的栈帧
/// （可观测后果是堆元数据被踩坏 —— "double free or corruption (out)"，与"哪一条
/// 判据红了"毫无关系）。
struct Round {
  std::atomic<int> calls{0};
  std::atomic<int> err{0};
  int              status  = 0;
  int              version = -1;
  std::string      body;
};

struct ClientResult {
  /// 走到哪一步就退出了（0 = 没开始，1 = 握手，2..3 = 第 k 趟，4 = 保持阶段）。
  int         stage         = 0;
  int         ok_roundtrips = 0;
  std::string err;
  /// 每一趟的回调次数与 body（打印失败原因时给出全貌）。
  int         per_calls[2]   = {0, 0};
  int         per_status[2]  = {0, 0};
  int         per_version[2] = {0, 0};
  int         per_err[2]     = {0, 0};
  std::string per_body[2];
};

/**
 * @brief 建一条 h3 连接、连打两次 `/transport?c=marker`，然后保持连接打开。
 *
 * 保持打开是判据 1/3/6 需要的：连接计数读的是**当前登记着**的连接，一关就掉。
 * "保持到什么时候"由 `release` 这个原子量与调用方协调。
 */
void client_run(uvcpp_ssl_context* cctx, int port, int marker, ClientResult* out,
                std::atomic<bool>* release) {
  // **在本线程里建** —— `uvcpp_http_client` 默认构造出来的 `uvcpp_tcp_client`
  // 自带一条循环，它今天就归这条线程泵。
  uvcpp_http_client cli;
  uvcpp_loop*       loop = cli.get_tcp_client()->get_loop();

  cli.set_http3_enabled(true);
  cli.set_ssl_context(cctx);

  std::shared_ptr<std::atomic<int> > st(new std::atomic<int>(12345));
  if (cli.connect("127.0.0.1", port, [st](int s) { st->store(s); }) != 0) {
    out->err = "connect() 没接上";
    return;
  }
  out->stage = 1;
  {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (st->load() == 12345 && std::chrono::steady_clock::now() < deadline) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (st->load() != 0) {
    out->err = "握手没完成（cb 收到 " + std::to_string(st->load()) + "）";
    return;
  }

  const std::string url = "/transport?c=" + std::to_string(marker);
  for (int k = 0; k < 2; ++k) {
    out->stage = 2 + k;
    std::shared_ptr<Round> rd(new Round());
    const int src = cli.send(uvcpp_http_request::make_get(url),
                             [rd](const uvcpp_http_response& r, int e) {
                               rd->calls.fetch_add(1);
                               if (e != 0) {
                                 rd->err.store(e);
                                 return;
                               }
                               rd->status  = static_cast<int>(r.status_code);
                               rd->version = static_cast<int>(r.version);
                               rd->body    = r.body.to_string();
                             });
    if (src != 0) {
      out->err = "send() 没发出去（rc=" + std::to_string(src) + "）";
      return;
    }
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (rd->calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    out->per_calls[k]   = rd->calls.load();
    out->per_status[k]  = rd->status;
    out->per_version[k] = rd->version;
    out->per_body[k]    = rd->body;
    out->per_err[k]     = rd->err.load();
    if (out->per_calls[k] == 0) {
      out->err = "第 " + std::to_string(k + 1) + " 条响应没到（20 s）";
      return;
    }
    if (out->per_err[k] == 0 && rd->body == "HTTP/3") ++out->ok_roundtrips;
  }

  // 保持连接打开，直到调用方放行（上限只是兜底：主线程真出事时别把用例挂死）。
  out->stage = 4;
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(90);
  while (!release->load() && std::chrono::steady_clock::now() < deadline) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

// =========================================================================
// 一条完整相位：n 条循环、nconn 条连接
// =========================================================================

bool run_phase(int loops, int nconn, const char* tag) {
  bool ok = true;

  H3Rig rig;
  if (!rig.start(loops)) {
    std::cout << "  [" << tag << "] 服务端起不来（quic_rc=" << rig.quic_rc()
              << "）" << std::endl;
    return false;
  }
  if (rig.loops_rc() != 0 || rig.quic_rc() != 0) {
    std::cout << "  [" << tag << "] set_loops=" << rig.loops_rc()
              << " listen_quic=" << rig.quic_rc() << "（都不是 0）" << std::endl;
    rig.request_stop();
    return false;
  }

  std::cout << "  [" << tag << "] 腿 = "
            << (rig.fanout() ? "REUSEPORT 内核分流" : "单端点（回落/未请求）")
            << "，端口 " << rig.port << std::endl;

  std::vector<ClientResult> res(static_cast<size_t>(nconn));
  std::atomic<bool>         release{false};
  std::vector<std::thread>  clients;
  clients.reserve(static_cast<size_t>(nconn));
  // 全部线程**共用** `rig.client_ctx()` 这一个 TLS 上下文，别改成一人一个：
  // 一来真实用法就是"进程里一个上下文"，二来判据 8 钉的那处客户端 ALPN 缺陷
  // 只有在共享上下文时才复现（每条线程各自建连接，ALPN 若装在 ctx 上就会并发
  // free 同一块）。
  for (int i = 0; i < nconn; ++i) {
    clients.push_back(std::thread(client_run, rig.client_ctx(), rig.port, i,
                                  &res[static_cast<size_t>(i)], &release));
  }

  uvcpp_http_server* srv = rig.server();

  // --- 判据 1 / 3：结构 + 计数守恒 ------------------------------------
  //
  // 先等"每条连接都打完了两次"（处理函数侧的事实），再等"登记表里的条数"。
  // 两个数都到了，下面读它们才不会读到半路。
  for (int i = 0; i < nconn; ++i) {
    require_within(
        [&rig, i]() { return rig.obs().seen(i) >= 2; }, 30000,
        "全部连接都打完了两次 /transport");
  }
  require_within(
      [srv, nconn]() {
        return srv->quic_connection_count() == static_cast<size_t>(nconn);
      },
      30000, "h3 登记表条数 == 连接数");

  const int nloops = rig.loop_count();
  check_eq_i(nloops, loops, "loop_count()");
  if (nloops != loops) ok = false;
  check(srv->quic_connection_count_at(9999) == 0,
        "quic_connection_count_at(越界) == 0");

  std::vector<size_t> per_loop(static_cast<size_t>(nloops), 0);
  size_t              sum = 0;
  std::string         shown;
  for (int i = 0; i < nloops; ++i) {
    per_loop[static_cast<size_t>(i)] = srv->quic_connection_count_at(i);
    sum += per_loop[static_cast<size_t>(i)];
    shown += " " + std::to_string(per_loop[static_cast<size_t>(i)]);
  }
  std::cout << "  [" << tag << "] quic_connection_count_at 逐格:" << shown
            << "，合计 " << sum
            << " / quic_connection_count()=" << srv->quic_connection_count()
            << std::endl;
  check(sum == srv->quic_connection_count(),
        "逐格之和 == quic_connection_count()");

  // --- 判据 3：每条连接归在哪一格（按标记归并，与计数逐格比） --------
  std::map<int, int> by_loop;  // 循环号 -> 连接条数（从处理函数的观察来）
  std::map<int, int> conn_of;  // 标记 -> 循环号
  {
    std::lock_guard<std::mutex> lk(rig.obs().mu);
    for (std::map<int, int>::const_iterator it = rig.obs().conn_loop.begin();
         it != rig.obs().conn_loop.end(); ++it) {
      if (it->first < 0 || it->first >= nconn) continue;  // 不是本用例的标记
      conn_of[it->first] = it->second;
      ++by_loop[it->second];
    }
  }
  std::string obs_shown;
  for (int i = 0; i < nloops; ++i) {
    const int v = by_loop.count(i) != 0 ? by_loop[i] : 0;
    obs_shown += " " + std::to_string(v);
    if (static_cast<size_t>(v) != per_loop[static_cast<size_t>(i)]) {
      std::cout << "  [" << tag << "] 第 " << i << " 格对不上：处理函数看到 " << v
                << " 条，计数表报 " << per_loop[static_cast<size_t>(i)] << " 条"
                << std::endl;
      ok = false;
    }
  }
  std::cout << "  [" << tag << "] 按标记归并到各循环:" << obs_shown << std::endl;
  check_eq_i(static_cast<long long>(conn_of.size()), nconn,
             "每条连接都被处理函数认出过");

  // --- 判据 2 / 6：分布与对照组 --------------------------------------
  size_t nonzero = 0;
  for (int i = 0; i < nloops; ++i) {
    if (per_loop[static_cast<size_t>(i)] != 0) ++nonzero;
  }
  if (loops == 1) {
    // --- 判据 6：对照组 ---
    check(!rig.fanout(), "n==1 时 quic_fanout() 必须为假");
    check_eq_i(static_cast<long long>(per_loop[0]),
               static_cast<long long>(nconn), "n==1 时全部连接在 0 号");
    std::set<std::thread::id> tids;
    {
      std::lock_guard<std::mutex> lk(rig.obs().mu);
      for (std::map<int, std::thread::id>::const_iterator it =
               rig.obs().conn_tid.begin();
           it != rig.obs().conn_tid.end(); ++it) {
        if (it->first >= 0 && it->first < nconn) tids.insert(it->second);
      }
    }
    check(tids.size() == 1 && tids.count(rig.acceptor_tid()) == 1,
          "n==1 时全部 h3 回调都在 acceptor 线程上");
  } else if (rig.fanout()) {
    check(nonzero >= 2, "扇出时至少两条循环分到过连接（这次 " +
                            std::to_string(nonzero) + " 条）");
    // 0 号自己也在收（它不是"只负责 accept"的那条），所以这里**不**断言
    // "0 号恒 0"，也不拍均匀度 —— 只要求"每条落地的连接都算在某一格"。
  } else {
    // ---- 判据 9：这条"回落"得是真的 ----
    //
    // 上面那三个数（`quic_fanout()` 假、非零槽只有 0 号、`is_reuseport()` 假）
    // 全都是**被测实现自己报的**：把 `listen_quic()` 的端点循环写死成 1 条，
    // 或者把 `bind_udp()` 里的标志恒填 0，报出来的形状与"平台拒绝标志"**一模
    // 一样** —— 判据 1..8 在这种坏实现下**全绿**（变异表 M1/M3 就是这么活下来
    // 的）。分得开两者的只有库外那一件事：**这个平台到底给不给得出分流**。
    const bool platform_can = uvcpp_test::platform_honors_reuseport();
    std::cout << "  [" << tag << "] 裸 libuv 探针（同端口带标志绑两次）："
              << (platform_can ? "都成功 ⇒ 这个平台上分流成立"
                               : "不成立 ⇒ 平台给不出")
              << std::endl;
    if (platform_can) {
      check(rig.fanout(),
            "内核给得出 REUSEPORT、`set_loops(n>1)` 也答应了，"
            "`quic_fanout()` 就必须为真（报假 = 扇出没建起来，或者标志被"
            "静默丢掉）");
    }
    check_eq_i(static_cast<long long>(per_loop[0]),
               static_cast<long long>(nconn), "回落时全部连接在 0 号");
  }

  // --- 判据 4：同一条连接不漂移 --------------------------------------
  {
    std::lock_guard<std::mutex> lk(rig.obs().mu);
    if (rig.obs().drifting != 0) {
      std::cout << "  [" << tag << "] 有 " << rig.obs().drifting
                << " 次「同一条连接的请求落在不同循环/线程上」" << std::endl;
      ok = false;
    }
  }

  // --- 判据 7a：收掉客户端 --------------------------------------------
  //
  // **必须在读 `res` 之前**：上面那几个判据读的都是服务端侧那张表（自带锁），
  // 而 `res` 是每条客户端线程**独占写**的。它们此刻还压在各自的等待里，主线程
  // 这时候去读就是与它们在赛跑 —— 读到的是一半的现场（"第 1 趟对了、第 2 趟的
  // 计数还是 0、err 还是空的"），而 `std::string` 的并发读写还会顺手踩坏堆。
  // 顺序是：服务端侧判据 → 放行 + join → 客户端侧判据。
  release.store(true);
  for (size_t i = 0; i < clients.size(); ++i) {
    if (clients[i].joinable()) clients[i].join();
  }

  // --- 判据 5：数据正确 ----------------------------------------------
  for (int i = 0; i < nconn; ++i) {
    const ClientResult& r = res[static_cast<size_t>(i)];
    if (!r.err.empty()) {
      std::cout << "  [" << tag << "] 连接 " << i << "：" << r.err << std::endl;
      ok = false;
      continue;
    }
    if (r.per_status[0] != 200 || r.per_status[1] != 200 ||
        r.per_version[0] != static_cast<int>(uvcpp_http_version::HVER_30) ||
        r.per_version[1] != static_cast<int>(uvcpp_http_version::HVER_30) ||
        r.ok_roundtrips != 2) {
      std::cout << "  [" << tag << "] 连接 " << i << "：stage=" << r.stage
                << " 正确的往返 " << r.ok_roundtrips << "/2"
                << "；逐趟 [calls=" << r.per_calls[0] << " err=" << r.per_err[0]
                << " body=\"" << r.per_body[0] << "\", calls=" << r.per_calls[1]
                << " err=" << r.per_err[1] << " body=\"" << r.per_body[1]
                << "\"]" << std::endl;
      ok = false;
    }
  }

  // --- 判据 7b：服务端收尾不挂 ----------------------------------------
  rig.request_stop();
  require_within([&rig]() { return rig.exited(); }, 30000,
                 "服务端析构返回（工作循环全部 stop + join 完）");

  std::cout << "  [" << tag << "] " << (ok ? "PASS" : "FAIL") << std::endl;
  return ok;
}

// =========================================================================
// API 边界
// =========================================================================

bool test_api_guards() {
  bool ok = true;

  uvcpp_ssl_context ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!ctx.generate_self_signed("localhost", 2048)) {
    std::cout << "  [api] 生成自签证书失败" << std::endl;
    return false;
  }

  uvcpp_http_server s;
  check_eq_i(s.loop_count(), 1, "默认 loop_count() == 1");
  check(!s.quic_fanout(), "没 listen_quic 之前 quic_fanout() 为假");
  check_eq_i(s.quic_listen_port(), 0,
             "没 listen_quic 之前 quic_listen_port() == 0");
  check_eq_i(static_cast<long long>(s.quic_connection_count()), 0,
             "没 listen_quic 之前连接数为 0");
  check_eq_i(static_cast<long long>(s.quic_connection_count_at(0)), 0,
             "空表上 count_at(0) == 0");

  // ---- 没装凭证：listen_quic 必须当场拒（QUIC 没有明文模式）---------
  {
    uvcpp_http_server n;
    check_eq_i(n.listen_quic(0, "127.0.0.1"), UV_EINVAL,
               "没装 TLS 上下文时 listen_quic() 返 UV_EINVAL");
  }

  // ---- 取值校验 ------------------------------------------------------
  check_eq_i(s.set_loops(0), UV_EINVAL, "set_loops(0)");
  check_eq_i(s.set_loops(65), UV_EINVAL, "set_loops(65)");
  check_eq_i(s.set_loops(1), 0, "set_loops(1) 是幂等空操作");
  check_eq_i(s.set_loops(1), 0, "set_loops(1) 第二次还是 0");
  check_eq_i(s.loop_count(), 1, "set_loops(1) 之后 loop_count() 没变");

  // ---- 真起 4 条循环，再 listen_quic ---------------------------------
  check_eq_i(s.set_loops(4), 0, "set_loops(4)");
  check_eq_i(s.loop_count(), 4, "set_loops(4) 之后 loop_count() == 4");
  check_eq_i(s.set_loops(4), UV_EBUSY, "装好之后同一个 n 也不许再装");

  s.set_quic_ssl_context(&ctx);
  check_eq_i(s.listen_quic(0, "127.0.0.1"), 0, "listen_quic(0) 返回 0");
  check(s.quic_listen_port() > 0, "listen_quic(0) 之后端口是内核给的真号");
  check_eq_i(static_cast<long long>(s.quic_connection_count()), 0,
             "一条连接都没来时计数为 0");
  for (int i = 0; i < 4; ++i) {
    // 扇出与回落都成立：没有任何连接时**每一格都是 0**。这条同时钉住"逐格读取
    // 不会越界、也不会把别的循环的账算到这一格"。
    check_eq_i(static_cast<long long>(s.quic_connection_count_at(i)), 0,
               "空表上 count_at(" + std::to_string(i) + ") == 0");
  }

  // **这道门是本层独有的**（见头文件）：QUIC 端点按 `loop_count()` 一次建齐，
  // 建完再改 n 就是"表按 4 张定尺寸、而回调报别的号"。
  check_eq_i(s.set_loops(8), UV_EBUSY,
             "listen_quic() 之后 set_loops(n>1) 返 UV_EBUSY");
  // `set_loops(1)` 什么都不建 ⇒ 它必须仍然成功（幂等），不是 EBUSY。
  check_eq_i(s.set_loops(1), 0, "listen_quic() 之后 set_loops(1) 仍然幂等成功");
  check_eq_i(s.listen_quic(1, "127.0.0.1"), UV_EALREADY, "重复 listen_quic()");

  if (g_failures != 0) ok = false;
  std::cout << "  [api] " << (ok ? "PASS" : "FAIL") << std::endl;
  return ok;
}

}  // namespace

int main() {
  std::cout << "[functional http3_multiloop] start" << std::endl;

  quic_crypto_backend_init();

  bool ok = true;
  std::cout << "[http3_multiloop] api_guards" << std::endl;
  ok = test_api_guards() && ok;

  std::cout << "[http3_multiloop] control_n1" << std::endl;
  ok = run_phase(1, 4, "n=1") && ok;

  std::cout << "[http3_multiloop] multi_n4" << std::endl;
  ok = run_phase(4, 8, "n=4") && ok;

  quic_crypto_backend_free();

  std::cout << "[functional http3_multiloop] checks=" << g_checks
            << " failures=" << g_failures << std::endl;
  if (ok && g_failures == 0) {
    std::cout << "[functional http3_multiloop] done success=true" << std::endl;
    return 0;
  }
  std::cout << "[functional http3_multiloop] done success=false" << std::endl;
  return 2;
}

#else  // UVCPP_HTTP3_ENABLE

// 关掉 http3 模块时这个文件不该被编译（`tests/functional/CMakeLists.txt` 的
// 过滤器按文件名摘掉它 —— 规则是 `http3`）。真编到了就是配置错，返回非零而不是
// 打一句 SKIP 再返回 0：后者会把"这个模块一次都没被跑过"伪装成"全绿"。
int main() {
  std::cerr << "[http3_multiloop] UVCPP_HTTP3_ENABLE=0 —— 这个测试文件不该被编译进来"
            << std::endl;
  return 2;
}

#endif  // UVCPP_HTTP3_ENABLE
