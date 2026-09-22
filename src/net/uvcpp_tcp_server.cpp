/**
 * @file src/net/uvcpp_tcp_server.cpp
 * @brief Implementation of uvcpp_tcp_server.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <net/uvcpp_tcp_server.h>
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_define.h>

#include <net/uvcpp_loop_worker.h>
#include <net/uvcpp_socket_handoff.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_context.h>
#endif

namespace uvcpp {

// =========================================================================
// Trampoline — bridge C-style callback to std::function stored on heap
// =========================================================================

static void trampoline_connection(uvcpp_tcp_client* client, void* arg) {
  auto* cb =
      static_cast<std::function<void(uvcpp_tcp_client*)>*>(arg);
  (*cb)(client);
  // cb is NOT deleted — persists for multiple connections
}

namespace {

/**
 * @brief 转手中途那个 socket 的 RAII 持有者：**恰好关一次**。
 *
 * 为什么需要它：socket 从 acceptor 线程上取出来之后，要经过一次跨线程投递
 * 才到得了目标循环。这条路上有三处"到不了"：投递被拒（worker 正在停机）、
 * 任务被丢掉（闸关了之后 `drain()` 剩下的整批一起析构）、以及任务跑了但
 * `uv_tcp_open` 不收货。前两处我们**没有机会写清理代码** —— 唯一的落点就是
 * 析构函数，所以持有者本身得是个对象，而不是一个裸整数。
 *
 * 投递失败/任务被丢时它随 lambda 一起析构，把 fd 关掉；成功交出去时用
 * `release()` 摘掉所有权（此后归那个连接，不归我们）。
 */
class handed_socket {
 public:
  explicit handed_socket(uv_os_sock_t s) : sock_(s) {}

  ~handed_socket() { uvcpp_handoff_close_raw(sock_); }

  handed_socket(const handed_socket&) = delete;
  handed_socket& operator=(const handed_socket&) = delete;

  uv_os_sock_t get() const { return sock_; }

  /** @brief 交出所有权：返回那个 socket，并且此后本对象不再关它。 */
  uv_os_sock_t release() {
    const uv_os_sock_t s = sock_;
    sock_ = uvcpp_handoff_invalid_socket();
    return s;
  }

 private:
  uv_os_sock_t sock_;
};

}  // namespace

// =========================================================================
// Construction / Destruction
// =========================================================================

uvcpp_tcp_server::uvcpp_tcp_server() {
  loop_ = new uvcpp_loop();  // constructor already calls init()
  tcp_  = new uvcpp_tcp(loop_);
  status_ = TCP_SERVER_NONE;
}

uvcpp_tcp_server::~uvcpp_tcp_server() {
  // -------------------------------------------------------------------
  // 第零步：**先**把工作线程停掉、join 完，再走下面那四步。
  //
  // 为什么必须在最前面：工作线程名下的连接，它们的句柄是挂在**工作循环**
  // 上的，而工作循环的关与释放由工作线程自己负责（退出路径里做）。顺序反过来
  // ——先 `delete loop_`（acceptor 的循环）不影响它们，但先删连接就会让那些
  // 句柄在一条已经被还回去的循环上 `uv_close`。
  //
  // 每个 worker 的退出路径里有一步 `on_exit` 钩子（装的是
  // `close_clients_of_loop`），所以 join 返回时它名下的连接已经关完、登记表
  // 里也不再有人 —— 下面第二步、第三步于是只管 acceptor 自己那一份。
  // -------------------------------------------------------------------
  stop_workers();

  // -------------------------------------------------------------------
  // 收尾分四步，**顺序不能换**。原先那个版本在这四处各有一个缺陷，
  // 合起来的后果是"析构完把整个循环泄漏掉"——实测 177 处（第 8 条探针
  // `tests/tools/run_loop_leak_probe.py`，占全部泄漏循环的 57%）。
  //
  // 第一步必须**无条件**关监听句柄。原先的条件是
  // `!stopped_ && !is_closing() && is_active()`，两边都能漏：
  //
  //   - `stop()` 已经把这个句柄 close 了（`stopped_ == true`）时整块被跳过，
  //     而 `stop()` 只**发起**关闭、终结还没放完 —— 于是循环关不掉；
  //   - 句柄 init 过但没 `uv_listen`（或已经停掉）时 `is_active()` 为假，
  //     同样被跳过。这一种更隐蔽：`uv__loop_alive()` 在 Windows 上算的是
  //     「活跃句柄 || 活跃请求 || pending_reqs_tail || endgame_handles」，
  //     既没 close 又不活跃的句柄**哪一个都不占**，所以 `uv_run` 连 while
  //     体都不进 —— 靠"多泵几轮"永远救不回来，只能先 `uv_close` 把它推进
  //     `endgame_handles`（`_local_deps/libuv/src/win/core.c:401`）。
  // -------------------------------------------------------------------
  if (tcp_ != nullptr && tcp_->get_handle() != nullptr &&
      !tcp_->is_closing()) {
    tcp_->close([](uvcpp_handle*) {});
  }

  // 第二步：客户端也要**在循环还活着的时候**收掉 —— 它们各自的析构会关自己
  // 的句柄、跑 token 与用户回调。原先这一步排在 `delete loop_` **之后**，
  // 那些 `uv_close()` 是在一块已经"关闭过"的内存上写的（之所以没炸，是因为
  // 关不掉时 `~uvcpp_loop` 有意泄漏了那块内存；那是巧合，不是设计）。
  for (auto* client : clients_) {
    delete client;
  }
  clients_.clear();

  // 第三步：有界泵，把前两步排上去的终结放完，然后才关循环。**没有 sleep、
  // 不看墙钟**（原先是 5000 轮 + 每次 1ms 睡眠，再加一段固定的 20 × 1ms
  // "补漏"——也就是说**每一次**正常析构都至少睡 20 毫秒，而且它正是第 10 条
  // 从 `~uvcpp_tcp_client` 里拿掉的那个形状）。
  //
  // 泵的判据是 `uv_loop_alive()`，**不能把 `loop_close()` 写进条件里**：
  // `loop_close()` 内部先调 `stop()`，而 `stop()` 在循环还活着时会
  // `uv_stop()` 把 `stop_flag` 立起来；`uv_run` 的
  // `while (r != 0 && loop->stop_flag == 0)` 是在**进 body 之前**判的，
  // 于是那一轮整段空转、一个终结都不放（`stop_flag` 要到 `uv_run` 返回前才
  // 清掉）。交替调用就成了一个不推进的死循环，256 轮全烧完还是关不掉。
  //
  // `is_running()` 那一问是兜底：本对象若是在**自己的某个回调里**被析构的
  // （栈上还压着一层 `uv_run`），泵循环就是重入 `uv_run`，而 `delete loop_`
  // 是把外层那一帧脚下的 `uv_loop_t` 还回去。这时整块交给循环 —— 与
  // `~uvcpp_tcp_client` / `~uvcpp_ws_client` 同一条策略：泄漏一块仍然有效的
  // 内存，换掉一个必然发生的 use-after-free。（目前没有用例走这一路：测试里
  // 的 server 都是栈对象，析构发生在泵返回之后。）
  if (loop_ != nullptr && !loop_->is_running()) {
    for (int i = 0; i < 256 && loop_->loop_alive() != 0; ++i) {
      loop_->run(UV_RUN_NOWAIT);
    }
    loop_->loop_close();
    delete loop_;
  }
  loop_ = nullptr;

  // 第四步：现在才轮到 TCP wrapper —— 它的句柄在第三步里已经终结、循环也
  // 已经还回去了，`free_handle()` 会走"内部句柄已置空"那条路。
  if (tcp_ != nullptr) {
    delete tcp_;
    tcp_ = nullptr;
  }

  if (on_connection_arg_ != nullptr) {
    delete static_cast<std::function<void(uvcpp_tcp_client*)>*>(
        on_connection_arg_);
    on_connection_arg_ = nullptr;
  }
}

// =========================================================================
// Accessors
// =========================================================================

uvcpp_tcp* uvcpp_tcp_server::get_tcp() {
  return tcp_;
}

uvcpp_loop* uvcpp_tcp_server::get_loop() {
  return loop_;
}

int uvcpp_tcp_server::get_status() const {
  return status_.load();
}

bool uvcpp_tcp_server::has_status(int flags) const {
  return (status_.load() & flags) == flags;
}

int uvcpp_tcp_server::get_last_error() const {
  return last_error_code_.load();
}

// =========================================================================
// Bind
// =========================================================================

int uvcpp_tcp_server::bind(const char* ip, int port) {
  // Auto-detect IPv4 vs IPv6: if the IP string contains ':', it's IPv6
  if (std::strchr(ip, ':') != nullptr) {
    return bindIpv6(ip, port);
  }
  return bindIpv4(ip, port);
}

int uvcpp_tcp_server::bindIpv4(const char* ip, int port) {
  int rc = tcp_->bindIpv4(ip, port);
  if (rc != 0) {
    last_error_code_.store(rc);
    set_status(TCP_SERVER_ERROR);
    return rc;
  }
  set_status(TCP_SERVER_LISTENING);
  return 0;
}

int uvcpp_tcp_server::bindIpv6(const char* ip, int port) {
  int rc = tcp_->bindIpv6(ip, port);
  if (rc != 0) {
    last_error_code_.store(rc);
    set_status(TCP_SERVER_ERROR);
    return rc;
  }
  set_status(TCP_SERVER_LISTENING);
  return 0;
}

// =========================================================================
// Listen
// =========================================================================

int uvcpp_tcp_server::listen(
    std::function<void(uvcpp_tcp_client*)> connection_cb, int backlog) {
  if (connection_cb == nullptr) {
    last_error_code_.store(UV_EINVAL);
    return UV_EINVAL;
  }

  // Store the user callback via trampoline to avoid std::function
  // copy-chain corruption through libuv's callback layers.
  on_connection_fn_  = trampoline_connection;
  on_connection_arg_ =
      new std::function<void(uvcpp_tcp_client*)>(connection_cb);

  int rc = tcp_->listen(
      [this](uvcpp_stream* s, int status) {
        if (status < 0) {
          last_error_code_.store(status);
          set_status(TCP_SERVER_ERROR);
          return;
        }

        // ---- 多循环：转手给工作线程，本循环一条都不留。 ----
        //
        // `workers_` 非空 ⟺ 装过 `set_loops(n>1)`（且线程已经在跑）。
        if (!workers_.empty()) {
          accept_and_handoff(s);
          return;
        }

        // ---- 以下 n == 1 那条路（今天的行为）----

        // Create a new client sharing the server's loop
        uvcpp_tcp_client* client = new uvcpp_tcp_client(loop_);

        // Accept the pending connection into the client's TCP handle
        int accept_rc = s->accept(client->get_tcp());
        if (accept_rc != 0) {
          last_error_code_.store(accept_rc);
          delete client;
          return;
        }

        finish_accept(client);
      },
      backlog);

  if (rc != 0) {
    last_error_code_.store(rc);
    set_status(TCP_SERVER_ERROR);
    // Clean up the heap-allocated callback
    delete static_cast<std::function<void(uvcpp_tcp_client*)>*>(
        on_connection_arg_);
    on_connection_fn_  = nullptr;
    on_connection_arg_ = nullptr;
    return rc;
  }

  return 0;
}

// =========================================================================
// 多循环：接受者 + 工作线程
// =========================================================================

int uvcpp_tcp_server::set_loops(int n) {
  if (n < 1 || n > 64) return UV_EINVAL;
  if (has_status(TCP_SERVER_LISTENING)) return UV_EBUSY;  // 已经跑起来了

  // `n == 1` = 今天的行为，**什么都不建**：不建 worker、不建线程、不建邮箱。
  // 幂等，因为"什么都不做"重复多少次都一样。
  if (n == 1) return 0;

  if (!workers_.empty()) return UV_EBUSY;  // 已经装过一遍了（n>1 不能再改）

  // 先全建出来再 start：任何一条起不来就把已经起了的收干净、返回错误码，
  // 不留下"workers_ 非空但线程没起"这种半成品状态 —— accept 回调是拿
  // `workers_.empty()` 当分叉判据的。
  for (int i = 0; i < n - 1; ++i) {
    uvcpp_loop_worker* w = new uvcpp_loop_worker();

    // 循环号**必须在 `start()` 之前装**：线程体开跑第一件事就是把它写进线程
    // 本地量，晚一步那条线程就会一直自称 0 号（接受者的号）。0 号是接受者，
    // 工作循环从 1 起 —— 与 `loop_at(i)` 的下标是同一个编号。
    w->set_loop_index(i + 1);

    // **必须在 `start()` 之前装**：这个钩子在 worker 线程上跑，写它的人和读
    // 它的人之间靠 `start()` 的交握手建立先后关系。它干的是"退出前把挂在我
    // 这条循环上的连接关掉" —— 那些连接的关闭收尾必须在它们自己的循环线程上。
    w->set_on_exit([this, w]() { close_clients_of_loop(w->loop()); });

    // 属主的就绪钩子（见 `set_loop_start_hook()`）。循环号是 `i + 1` —— 0 号是
    // 接受者，不走这个钩子。同样**必须在 `start()` 之前装**。
    if (loop_start_hook_) {
      // 捕获 `w` 而**不是** `workers_[i]`：钩子跑在 `start()` 里面，而
      // `push_back(w)` 在它之后 —— 那时候下标 i 还越界。
      w->set_on_start([this, w, i]() { loop_start_hook_(i + 1, w->loop()); });
    }

    const int src = w->start();
    if (src != 0) {
      delete w;
      stop_workers();
      last_error_code_.store(src);
      set_status(TCP_SERVER_ERROR);
      return src;
    }
    workers_.push_back(w);
  }

  return 0;
}

void uvcpp_tcp_server::set_loop_start_hook(
    std::function<void(int, uvcpp_loop*)> fn) {
  // 已经起过 worker 了就没法再装 —— 线程早跑过了，装进去也不会被调用。这里
  // **不报错也不静默**：写一条 ERROR 说清楚，因为"配了却没生效"正是最难查的
  // 那种形状（同 `set_ssl_context()` 装晚了的表现）。
  // 这里用 `fprintf` 而不是 `UVCPP_LOG_*`：net 层**不依赖 webapp 层**（`uvcpp_log`
  // 在 `src/webapp/`），本层的诊断一律走 stderr + 模块前缀，与
  // `uvcpp_loop_worker.cpp` / `uvcpp_tcp_client.cpp` 同一套写法。
  if (!workers_.empty()) {
    std::fprintf(stderr,
                 "[uvcpp_tcp_server] set_loop_start_hook() 在工作线程已启动"
                 "之后才调用，本次不生效（它必须在 set_loops() 之前设）\n");
    return;
  }
  loop_start_hook_ = std::move(fn);
}

int uvcpp_tcp_server::loop_count() const {
  return 1 + static_cast<int>(workers_.size());
}

uvcpp_loop* uvcpp_tcp_server::loop_at(int index) const {
  if (index == 0) return loop_;
  const size_t i = static_cast<size_t>(index - 1);
  if (i >= workers_.size()) return nullptr;
  return workers_[i]->loop();
}

size_t uvcpp_tcp_server::client_count_at(int loop_index) const {
  uvcpp_loop* want = loop_at(loop_index);
  if (want == nullptr) return 0;

  std::vector<uvcpp_tcp_client*> snap = snapshot_clients();
  size_t n = 0;
  for (size_t i = 0; i < snap.size(); ++i) {
    if (snap[i] != nullptr && snap[i]->get_loop() == want) ++n;
  }
  return n;
}

void uvcpp_tcp_server::accept_and_handoff(uvcpp_stream* s) {
  // 临时句柄**必须建在接受者这条循环上**：POSIX 的 `uv_accept` 硬断言要求
  // server 与 client 同循环（`libuv:unix/stream.c`）。它只用来到手 fd，
  // 之后立刻关掉。
  uvcpp_tcp* tmp = new uvcpp_tcp(loop_);

  const int accept_rc = s->accept(tmp);
  if (accept_rc != 0) {
    last_error_code_.store(accept_rc);
    delete tmp;  // 走 free_handle 的 uv_close 路，句柄自己收干净
    return;
  }

  // **取 socket 必须在关掉 tmp 之前**：`uv_close` 会**同步**关掉它那份 fd，
  // 关完再取就是 EBADF / WSAENOTSOCK。
  uv_os_sock_t raw = uvcpp_handoff_invalid_socket();
  const int ex_rc = uvcpp_handoff_extract(tmp, raw);
  if (ex_rc != 0) {
    last_error_code_.store(ex_rc);
    set_status(TCP_SERVER_ERROR);
    delete tmp;
    return;
  }

  // 取出去了就**立刻**关掉自己这份。这不是优化：Windows 上重复句柄的 FIN 只在
  // **最后一个**句柄关闭时才发出去，拖着不关会让对端一直等不到关闭
  // （`doc/worker-process-design.md` §9.7 实测把吞吐打到 63 连接/s）。
  delete tmp;

  // 显式轮转：第 i 条连接必然落到 1 + (i % (n-1)) 号。确定性的，不偶发。
  const size_t nw = workers_.size();
  const size_t k = static_cast<size_t>(rr_.fetch_add(1) % nw);

  // 持有者随 lambda 一起走：投递被拒、或者任务在停机时被丢掉，socket 都会
  // 随它析构而被关掉 —— 那两条路上我们没有别的地方写清理代码。
  std::shared_ptr<handed_socket> hs(new handed_socket(raw));
  const int wi = static_cast<int>(k);
  if (!workers_[k]->post([this, wi, hs]() {
        on_handoff_task(wi, hs->get());
        // 交出去了（无论 `uv_tcp_open` 收没收下，收尾都已经在那边做过），
        // 所以这里必须摘掉所有权，否则析构时又关一次。
        hs->release();
      })) {
    // 受理失败 = 任务已经析构 = socket 已经关掉了。什么都不用做。
    return;
  }
}

void uvcpp_tcp_server::on_handoff_task(int worker_index, uv_os_sock_t sock) {
  // **这一段跑在 worker 线程上。** 下面碰的全是这条循环的东西。
  uvcpp_loop* wl = loop_at(worker_index + 1);
  if (wl == nullptr) {
    // 到不了这儿（线程退干净之后不会再受理投递），真到了就放掉这条连接。
    uvcpp_handoff_close_raw(sock);
    return;
  }

  uvcpp_tcp_client* client = new uvcpp_tcp_client(wl, sock);
  // **循环号就是 `worker_index + 1`**：0 号留给接受者那条循环（转手路径下它
  // 自己一条连接都不留），工作循环从 1 号起。上层按循环切容器时拿它当数组下标，
  // 所以这里必须是**全局**号而不是 worker 自己的序号。
  client->set_loop_index(worker_index + 1);
  if ((client->get_status() & TCP_CLIENT_ERROR) != 0) {
    // `uv_tcp_open` 没收下 ⇒ 构造里压根没接管这个 socket，**它还归我们**。
    // 这条失败路径只有这里能收尾：libuv 不会替我们关它。
    last_error_code_.store(client->get_last_error());
    set_status(TCP_SERVER_ERROR);
    uvcpp_handoff_close_raw(sock);
    delete client;
    return;
  }

  finish_accept(client);
}

void uvcpp_tcp_server::finish_accept(uvcpp_tcp_client* client) {
  // Mark the client as connected — sets CONNECTED | READABLE |
  // WRITABLE so that write() and read_start() work correctly.
  client->mark_accepted();

  // **登记 + 装管理回调要排在用户回调之前。**
  //
  // 原来是反过来的，于是用户在 on_connection 里立刻关掉这个连接
  // （比如鉴权不过要拒绝它）时：关事件先于 push_back 发生，客户端
  // 要么没被登记、要么登记了一个已经关闭的；用户回调抛异常则两步
  // 都不执行，直接泄漏。先登记就先受管，后面怎么走都不会漏。
  register_client(client);
  setup_client_callbacks(client);

#if UVCPP_OPENSSL_ENABLE
  // --- TLS：装过滤器，并且**不把握手没完的连接交给上层**。 ---
  //
  // **必须排在 `setup_client_callbacks` 之后。** `enable_tls()` 在连接
  // 已建立时会顺手 `arm_async_read()` 来推进握手，而那个函数装的是
  // **原始**读回调并置上 `read_started_`；先调它的话，后面
  // `setup_client_callbacks` 想给框架层读（`read_start_events`）就会被
  // 两条读路径互斥的检查挡住 —— 上层的读回调永远装不上，HTTPS 请求
  // 到了也没人解析。反过来先装读则没有这个问题：握手靠读事件推进，
  // 而两种读路径都会把入站字节喂给过滤器（见 `arm_async_read`）。
  //
  // 失败时走 `close_client_on_callback_error`（关闭 + 回收）而不是
  // `delete`：此刻句柄是活的，且已经进了登记表。
  bool tls_handshake_pending = false;
  if (ssl_ctx_ != nullptr) {
    // 握手超时**必须在 enable_tls 之前设**：`enable_tls()` 里就会推进
    // 一次握手，而超时是从"第一次推进"起算的 —— 设晚了对这一步不生效。
    client->set_tls_handshake_timeout_ms(tls_hs_timeout_ms_);

    const int tls_rc = client->enable_tls(ssl_ctx_);
    if (tls_rc != 0) {
      last_error_code_.store(tls_rc);
      close_client_on_callback_error(client);
      return;
    }

    tls_handshake_pending = !client->is_tls_handshake_done();
    if (tls_handshake_pending) {
      // 通知只是把 `on_connection` 挂到"握手成功"那一刻。
      client->set_tls_ready_callback([this](uvcpp_tcp_client* c, int st) {
        on_tls_handshake_done(c, st);
      });
    }
  }
  if (tls_handshake_pending) return;  // 握手完成时再交付
#endif

  // `n > 1` 时这一步跑在 worker 线程上 ⇒ 用户的 `on_connection` **会并发**。
  deliver_connection(client);
}

void uvcpp_tcp_server::register_client(uvcpp_tcp_client* client) {
  if (client == nullptr) return;
  std::lock_guard<std::mutex> lk(clients_mu_);
  clients_.push_back(client);
}

void uvcpp_tcp_server::unregister_client(uvcpp_tcp_client* client) {
  std::lock_guard<std::mutex> lk(clients_mu_);
  for (std::vector<uvcpp_tcp_client*>::iterator it = clients_.begin();
       it != clients_.end(); ++it) {
    if (*it == client) {
      clients_.erase(it);
      return;
    }
  }
}

std::vector<uvcpp_tcp_client*> uvcpp_tcp_server::snapshot_clients() const {
  std::lock_guard<std::mutex> lk(clients_mu_);
  return clients_;
}

size_t uvcpp_tcp_server::close_clients_of_loop(uvcpp_loop* l) {
  // 先取快照：`client->close()` 的收尾是**异步**的（等循环转到关闭完成回调），
  // 所以这一轮里登记表不会被改；但用户的 on_close 回调可能重入到这里，
  // 快照 + 逐个重判能挡住那种情况。
  std::vector<uvcpp_tcp_client*> snapshot = snapshot_clients();

  size_t closed = 0;
  for (size_t i = 0; i < snapshot.size(); ++i) {
    uvcpp_tcp_client* c = snapshot[i];
    if (c == nullptr || c->get_loop() != l) continue;  // 别的循环的，不归我们
    if (!owns_client(c)) continue;                     // 途中被 take 走的不动

    uvcpp_tcp* t = c->get_tcp();
    if (t == nullptr || t->get_handle() == nullptr) {
      // 句柄已经关完了，人却还在登记表里 —— 说明那次关闭没有走框架的收尾
      // （比如有人直接 `get_tcp()->close(...)`）。连接已经是死的，这里顺手
      // 归还：摘除 + 释放，不用再发起关闭。
      //
      // 这个判断不是洁癖：`uvcpp_handle::is_closing()` 会把句柄指针直接交给
      // libuv，而句柄关完之后那个指针是 nullptr —— 对一个已经关掉的连接调
      // `close()` 就是空指针解引用。先看 get_handle() 才是安全的顺序。
      release_client(c);
      continue;
    }

    c->close();
    ++closed;
  }
  return closed;
}

void uvcpp_tcp_server::stop_workers() {
  // `~uvcpp_loop_worker` 就是 `stop_and_join()`：线程退出路径里会排空邮箱、
  // 跑 `on_exit`（关掉它名下的连接）、泵完挂起的关闭回调、关掉并释放循环。
  // 所以 delete 一返回，那条循环上就什么都不剩了。
  for (size_t i = 0; i < workers_.size(); ++i) {
    delete workers_[i];
    workers_[i] = nullptr;
  }
  workers_.clear();
}

// =========================================================================
// Stop
// =========================================================================

size_t uvcpp_tcp_server::close_all_clients() {
  // 本循环那一份：就地关，**计入返回值**。
  size_t closed = close_clients_of_loop(loop_);

  // 别的循环那一份：各投一条任务过去，由那条循环的线程关它自己名下的连接。
  // 投递是异步的，所以**不计入返回值** —— 头文件里写明了 n>1 时这个返回值的
  // 语义是"本循环发起关闭的条数"，不是"已经关掉多少"。
  //
  // 循环指针在**任务里**取（`loop_at`），不在投递线程上取：`loop_` 是 worker
  // 线程自己的东西，停机时会被它置空。
  for (size_t i = 0; i < workers_.size(); ++i) {
    const int idx = static_cast<int>(i);
    workers_[i]->post([this, idx]() { close_clients_of_loop(loop_at(idx + 1)); });
  }
  return closed;
}

void uvcpp_tcp_server::release_client(uvcpp_tcp_client* client) {
  if (client == nullptr) return;
  unregister_client(client);
  // 在锁外 delete：`~uvcpp_tcp_client` 会跑用户回调，而用户回调里再进
  // `client_count()` / `owns_client()` 是合法用法 —— 持着锁就死锁了。
  delete client;
}

void uvcpp_tcp_server::stop(std::function<void()> on_stopped) {
  if (!has_status(TCP_SERVER_LISTENING)) {
    if (on_stopped) on_stopped();
    return;
  }

  set_status(TCP_SERVER_STOPPING);
  clear_status(TCP_SERVER_LISTENING);

  // Close the server TCP handle — this stops accepting new connections
  stopped_ = true;
  if (tcp_ != nullptr && !tcp_->is_closing()) {
    tcp_->close([this, on_stopped](uvcpp_handle*) {
      set_status(TCP_SERVER_STOPPED);
      clear_status(TCP_SERVER_STOPPING);
      if (on_stopped) on_stopped();
    });
  } else {
    set_status(TCP_SERVER_STOPPED);
    clear_status(TCP_SERVER_STOPPING);
    if (on_stopped) on_stopped();
  }
}

// =========================================================================
// Loop control
// =========================================================================

int uvcpp_tcp_server::run(uv_run_mode md) {
  // **接受者那条循环的线程是调用者的**，服务端没法在别的时刻认识它：`run()`
  // 是那条线程踏进本服务端的入口，所以"我是 0 号循环"在这里认领。
  //
  // 只在没人认领过时写（`< 0`）：同一线程上跑两条服务端时，后一条的 `run()`
  // 不该把前一条的号改掉 —— 两条都是 0 号，值一样，但这个判断更贴原意。
  if (uvcpp_loop_index_of_this_thread() < 0) {
    uvcpp_set_loop_index_of_this_thread(0);
  }
  return loop_->run(md);
}

void uvcpp_tcp_server::stop_loop() {
  loop_->stop();
}

// =========================================================================
// Internal helpers
// =========================================================================

void uvcpp_tcp_server::set_status(int flags) {
  // 读-改-写必须是原子的那一步：`n > 1` 时 worker 线程也会置位，两边各写回
  // 自己那份旧快照就会互相抹掉。
  status_.fetch_or(flags);
}

void uvcpp_tcp_server::clear_status(int flags) {
  status_.fetch_and(~flags);
}

void uvcpp_tcp_server::install_client_manager(uvcpp_tcp_client* client) {
  if (client == nullptr) return;

  // 走 close_manager 槽，**不是**用户的 set_on_close。
  // 用户设不设自己的 on_close 都影响不到这里 —— 这正是原来那个缺陷的修法：
  // 以前两者共用一个槽，用户一设自己的回调，框架的释放就被整条跳过，
  // 于是每连接泄漏一个 client，而且用户顺手 delete 时还会 double free。
  client->set_close_manager([this, client]() { release_client(client); });
}

void uvcpp_tcp_server::setup_client_callbacks(uvcpp_tcp_client* client) {
  if (client == nullptr) return;

  // **无条件**装框架的释放逻辑。客户端的生命周期由服务端负责，这一点不由
  // 用户有没有注册回调决定。用户的观察回调是另一个槽，两者都会被触发。
  install_client_manager(client);

  // 起读。
  //
  // 这一段的顺序值得说明：它跑在用户的 on_connection **之前**，所以下面装的
  // 只是兜底，用户在 on_connection 里注册自己的读回调时会把兜底顶掉（见
  // uvcpp_tcp_client::read_start 里的 release_auto_read）。
  //
  // 为什么一定要有人读：断开只在读回调的 `nread < 0` 分支里被发现。没有读，
  // 连接断了服务端也不会知道 —— 客户端对象留在登记表里，关闭回调永远不触发，
  // 于是每个连接泄漏一个对象。以前这一点完全靠用户自觉，现在默认就装好。
  if (read_cb_) {
    // 上层框架（webapp）注册的共用回调优先。
    client->read_start_events(read_cb_);
  } else if (auto_read_) {
    // 兜底自动读：只为发现断开；数据没有消费者，收到时会打一次警告。
    client->install_auto_read();
  } else if (!client->has_read_callback()) {
    // 用户明确关掉了自动读，自己也还没设 —— 把后果说清楚，别让它静默。
    std::fprintf(stderr,
                 "[uvcpp_tcp_server] Warning: auto_read is off and no read "
                 "callback is registered for new client %p. Disconnects will "
                 "NOT be detected and the client will not be released.\n",
                 static_cast<void*>(client));
  }
}

void uvcpp_tcp_server::deliver_connection(uvcpp_tcp_client* client) {
  // Call the user's connection callback.
  // 它可能已经把这个客户端 take_client() 走了 —— 那样下面就不能再
  // 碰 client。所以这里除了 catch 分支之外不再引用它。
  if (!on_connection_fn_) return;
  try {
    on_connection_fn_(client, on_connection_arg_);
  } catch (const std::exception& e) {
    std::fprintf(stderr,
                 "[uvcpp_tcp_server] on_connection callback threw: %s\n",
                 e.what());
    close_client_on_callback_error(client);
  } catch (...) {
    std::fprintf(stderr,
                 "[uvcpp_tcp_server] on_connection callback threw (unknown)\n");
    close_client_on_callback_error(client);
  }
}

#if UVCPP_OPENSSL_ENABLE
void uvcpp_tcp_server::set_ssl_context(uvcpp_ssl_context* ctx) {
  ssl_ctx_ = ctx;
}

void uvcpp_tcp_server::set_tls_handshake_timeout_ms(int ms) {
  tls_hs_timeout_ms_ = ms > 0 ? ms : 0;
}

void uvcpp_tcp_server::on_tls_handshake_done(uvcpp_tcp_client* client,
                                             int status) {
  if (client == nullptr) return;

  if (status != 0) {
    // 握手失败：**这条连接从此不再交给上层**。
    //
    // 只记账，不在这里动手收尾 —— 通知是从 `tls_fail()` 中间发出来的，
    // 它后面还要走"通知读侧 → fire_close_callbacks"，服务端的 close
    // manager（`release_client`）会在那里把客户端摘除并释放。在这里再关
    // 一次只会把收尾做两遍。
    //
    // 常见的两种成因：对端根本不是 TLS（明文客户端打 TLS 端口，比如把
    // HTTP 请求直接写进来），以及证书协商/校验失败。两者的可诊断信息都
    // 在 `tls_last_ssl_error()` 里 —— 但那个得在客户端还活着时读，这里
    // 已经晚了（通知之后 tls_fail 还会继续，但不保证原样返回）。所以只
    // 记错误码，需要细节请在客户端的读回调里看。
    last_error_code_.store(status);
    return;
  }

  deliver_connection(client);
}
#endif  // UVCPP_OPENSSL_ENABLE

// =========================================================================
// 读
// =========================================================================

void uvcpp_tcp_server::set_read_callback(const uvcpp_net_read_cb& cb) {
  read_cb_ = cb;
}

void uvcpp_tcp_server::clear_read_callback() { read_cb_ = nullptr; }

bool uvcpp_tcp_server::has_read_callback() const {
  return static_cast<bool>(read_cb_);
}

void uvcpp_tcp_server::set_auto_read(bool enable) { auto_read_ = enable; }

bool uvcpp_tcp_server::auto_read() const { return auto_read_; }

int uvcpp_tcp_server::pause_read(uvcpp_tcp_client* client) {
  // 只对自己的连接动手：对别人的（或已经 take 走的）连接调 read_stop 会
  // 停掉一个我们不了解的读路径，那种错很难查，不如直接拒掉。
  if (client == nullptr || !owns_client(client)) return UV_EINVAL;
  return client->read_pause();
}

int uvcpp_tcp_server::resume_read(uvcpp_tcp_client* client) {
  if (client == nullptr || !owns_client(client)) return UV_EINVAL;
  return client->read_resume();
}

void uvcpp_tcp_server::close_client_on_callback_error(
    uvcpp_tcp_client* client) {
  if (client == nullptr || !owns_client(client)) {
    // 所有权已经被 take_client() 取走了 —— 那是调用方的东西，不归我们回收。
    return;
  }

  // 先清掉管理槽，保证"摘除 + 释放"只发生一次（完成回调里那次）。
  client->clear_close_manager();

  uvcpp_tcp* tcp = client->get_tcp();
  if (tcp == nullptr) {
    unregister_client(client);
    delete client;
    return;
  }

  // 不在这里同步 delete：此刻句柄还是活的，而我们在 libuv 的 accept 回调
  // 里。交给关闭流程收尾。
  tcp->close([this, client](uvcpp_handle*) {
    unregister_client(client);
    delete client;
  });
}

// =========================================================================
// 客户端所有权
// =========================================================================

size_t uvcpp_tcp_server::client_count() const {
  std::lock_guard<std::mutex> lk(clients_mu_);
  return clients_.size();
}

bool uvcpp_tcp_server::owns_client(const uvcpp_tcp_client* client) const {
  if (client == nullptr) return false;
  std::lock_guard<std::mutex> lk(clients_mu_);
  for (std::vector<uvcpp_tcp_client*>::const_iterator it = clients_.begin();
       it != clients_.end(); ++it) {
    if (*it == client) return true;
  }
  return false;
}

uvcpp_tcp_client* uvcpp_tcp_server::take_client(uvcpp_tcp_client* client) {
  if (client == nullptr) return nullptr;

  // 摘出登记表（加锁，且**只在这一段里**持锁）——
  // 不归我们管时直接返回，所有权不变。
  {
    std::lock_guard<std::mutex> lk(clients_mu_);
    bool found = false;
    for (std::vector<uvcpp_tcp_client*>::iterator it = clients_.begin();
         it != clients_.end(); ++it) {
      if (*it == client) {
        clients_.erase(it);
        found = true;
        break;
      }
    }
    if (!found) return nullptr;
  }

  // 再清掉管理槽。两步都做完之后，服务端就真的不再碰它了。
  client->clear_close_manager();
  return client;
}

bool uvcpp_tcp_server::return_client(uvcpp_tcp_client* client) {
  if (client == nullptr) return false;
  if (owns_client(client)) return false;  // 已经在管了，重复交还

  uvcpp_tcp* tcp = client->get_tcp();
  const int status = client->get_status();
  const bool dead = (tcp == nullptr) || (tcp->get_handle() == nullptr) ||
                    ((status & TCP_CLIENT_CLOSED) != 0) ||
                    ((status & TCP_CLIENT_CLOSING) != 0) ||
                    ((status & TCP_CLIENT_ERROR) != 0);

  if (dead) {
    // 已经关掉的连接不会再产生任何事件，重新登记只会留个空壳。
    // **这里就是"由框架决定删除还是复用"的落点** —— 将来要加客户端
    // 对象池，复用的分支接在这里。
    delete client;
    return true;
  }

  // 登记回去用 `register_client`（加锁）：`n > 1` 时它回到的是**它自己那条
  // 循环**名下 —— 登记表里没有"哪条循环"这一列，`client_count_at()` 是靠
  // `client->get_loop()` 现算的，所以这里不需要额外记。
  register_client(client);
  install_client_manager(client);
  return true;
}

}  // namespace uvcpp
