/**
 * @file src/net/uvcpp_tcp_server.h
 * @brief Higher-level TCP server with bind/listen convenience and auto client management.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Wraps uvcpp_tcp to provide a simple bind+listen API with automatic
 * callback setup for accepted clients (default write/close handlers,
 * missing-read-callback warnings). By default (or with `set_loops(1)`) it is a
 * single-threaded event-loop object — IO-intensive work should be offloaded via
 * uvcpp_work. `set_loops(n > 1)` turns it into one acceptor loop plus n-1
 * dedicated worker threads (see that method's docs).
 */

#pragma once
#ifndef SRC_NET_UVCPP_TCP_SERVER_H
#define SRC_NET_UVCPP_TCP_SERVER_H

#include <uvcpp/uvcpp_config.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <uv.h>
#include <vector>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_tcp.h>
#include <net/uvcpp_tcp_client.h>

namespace uvcpp {

class uvcpp_loop_worker;

/**
 * @brief Bitmask status flags for TCP server lifecycle.
 */
enum uvcpp_tcp_server_status : int {
  TCP_SERVER_NONE      = 0x00,  ///< Initial state
  TCP_SERVER_LISTENING = 0x02,  ///< Actively accepting connections
  TCP_SERVER_STOPPING  = 0x04,  ///< stop() called, closing
  TCP_SERVER_STOPPED   = 0x08,  ///< Fully stopped
  TCP_SERVER_ERROR     = 0x10   ///< Error occurred (see last_error_code)
};

/**
 * @brief Application-layer TCP server wrapping uvcpp_tcp.
 *
 * Provides convenience bind (IPv4/IPv6) and listen with automatic
 * per-client callback management.
 *
 * Before the user's connection callback runs, each new client is:
 * - **registered and given a close manager** that removes it from the
 *   server and deletes it when the connection closes. This happens
 *   unconditionally — the user's own `set_on_close()` is a separate,
 *   purely observational slot and can never suppress it.
 * - **started reading** (`set_auto_read()`, default on). Reading is what
 *   makes a peer disconnect visible at all: the `nread < 0` branch of the
 *   read callback is the only place it is detected. Without it a closed
 *   connection would never be noticed, never released, and never reported
 *   through the close callbacks — a silent per-connection leak. Registering
 *   your own reader in the connection callback (via `read_start()` or
 *   `read_start_events()`) replaces the automatic one.
 * - Write buffers are automatically freed by uvcpp_write regardless of
 *   whether a user callback was provided.
 *
 * Prefer `set_read_callback()` over per-connection `read_start()`: it hands
 * you `net_read_result` events, which distinguish received data from a
 * graceful peer close (`PEER_CLOSED`) and from a read error (`READ_ERROR`,
 * carrying the libuv error code). The raw callback cannot — it receives
 * nothing at all in those last two cases.
 *
 * IMPORTANT: All callbacks (connection, read, write, close) run on
 * the event-loop thread. Do NOT perform IO-intensive work (file I/O,
 * heavy computation, blocking calls) inside any callback — it will block
 * the entire event loop. Use uvcpp_work to offload heavy tasks:
 *
 * @code
 *   uvcpp_work* w = new uvcpp_work();
 *   w->queue_work(server->get_loop(),
 *     [](uvcpp_work* w) {
 *       // Heavy work here (runs on worker thread)
 *     },
 *     [](uvcpp_work* w, int status) {
 *       // Process results, send response (runs on loop thread)
 *       delete w;
 *     });
 * @endcode
 *
 * Simple data checks and async writes are fine inline. Do NOT use
 * sync write_wait/read_wait inside any callback: they block the loop thread, so one
 * slow peer stalls *every* connection on it. Mind the trap — these take an optional
 * completion callback, and **omitting it is precisely what makes them synchronous**
 * (`uvcpp_tcp_client.h`: a null callback behaves like `write_wait(data, len, 30000)`).
 * Inside a callback always pass one, as the example below does.
 *
 * Usage:
 * @code
 *   uvcpp_tcp_server server;
 *   server.bind("0.0.0.0", 8080);
 *
 *   // 一份读回调，所有连接共用（框架层读：事件带语义）
 *   server.set_read_callback([](uvcpp_tcp_client& c, const net_read_result& r) {
 *     if (r.is_data()) {
 *       // 收到数据（keep this fast!）
 *       c.write(r.data, r.size(), [](int) {});   // 回显；传回调才是异步，别传 nullptr
 *     } else if (r.event == net_read_event::PEER_CLOSED) {
 *       // 对端正常收工
 *     } else {
 *       // 读错误，r.error 是 libuv 错误码
 *     }
 *   });
 *
 *   server.listen([](uvcpp_tcp_client* client) {
 *     // 连接已登记、已起读、断开时会自动释放 —— 这里通常什么都不用做
 *     (void)client;
 *   });
 *   server.run(UV_RUN_DEFAULT);
 * @endcode
 */
class UVCPP_API uvcpp_tcp_server {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_tcp_server)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_tcp_server)

  // -----------------------------------------------------------------
  // Accessors
  // -----------------------------------------------------------------

  /** @brief Return the underlying uvcpp_tcp handle. */
  uvcpp_tcp* get_tcp();

  /** @brief Return the internal event loop. */
  uvcpp_loop* get_loop();

  /** @brief Return the current status bitmask. */
  int get_status() const;

  /** @brief Check whether all given status flags are set. */
  bool has_status(int flags) const;

  /** @brief Return the last recorded error code (libuv errno). */
  int get_last_error() const;

  // -----------------------------------------------------------------
  // Bind
  // -----------------------------------------------------------------

  /**
   * @brief Bind to an address, auto-detecting IPv4 vs IPv6.
   * @param ip   IPv4 or IPv6 address string.
   * @param port Port number.
   * @return 0 on success, libuv error code on failure.
   */
  int bind(const char* ip, int port);

  /** @brief Bind to an IPv4 address. */
  int bindIpv4(const char* ip, int port);

  /** @brief Bind to an IPv6 address. */
  int bindIpv6(const char* ip, int port);

  // -----------------------------------------------------------------
  // Listen
  // -----------------------------------------------------------------

  /**
   * @brief Start listening for incoming connections.
   *
   * @param connection_cb  Called for each new connection with a
   *                       uvcpp_tcp_client* whose TCP handle has
   *                       already been accepted. The client is in
   *                       CONNECTED | READABLE | WRITABLE state.
   *
   *                       Thread safety: this callback runs on
   *                       the event-loop thread. Do not block, do
   *                       not perform IO-intensive work. Use
   *                       uvcpp_work::queue_work() for heavy tasks.
   *
   *                       Simple data inspection and async writes
   *                       are safe. Do NOT use sync write_wait or
   *                       read_wait inside this or any event-loop
   *                       callback.
   *
   * @param backlog  Listen backlog (default 128).
   * @return 0 on success, libuv error code on failure.
   */
  int listen(std::function<void(uvcpp_tcp_client*)> connection_cb,
             int backlog = 128);

  // -----------------------------------------------------------------
  // 多循环（接受者 + 工作线程）
  // -----------------------------------------------------------------

  /**
   * @brief 设定工作循环数。**不调用（或 `set_loops(1)`）= 今天的行为**。
   *
   * n 是**循环总数**，含调用线程跑的那一条：0 号是接受者（就是 `get_loop()`），
   * 1..n-1 号各有一条**专用 `std::thread`** 跑自己的循环（不是 libuv 线程池）。
   * 每条被接受的连接按**显式轮转**交给 `1..n-1` 号，一条不留在接受者上 ——
   * 接受者只接受和分发，不跑连接的业务回调。
   *
   * 轮转是显式的、确定的：第 i 条连接必然落到 `1 + (i % (n-1))` 号循环，
   * 所以连接数的分布是确定的、不会偶发（`client_count_at()` 看得见它）。
   *
   * 转手怎么做的（每个平台各自的机制）见 `net/uvcpp_socket_handoff.h`。
   *
   * ## 装上之后，谁在哪个线程上
   *
   * - `on_connection`、读/写/关闭回调都在**那条连接自己的循环线程**上跑；
   *   n>1 时它们**会并发**（不同连接在不同线程），共享状态要自己加锁。
   * - `take_client()` / `return_client()` / `owns_client()` / `pause_read()` /
   *   `resume_read()` 管的是**登记**，登记表是加锁的，所以从任何线程调都对；
   *   但**驱动一条连接**（`write` / `read_start` / `close`）仍然只能在它自己
   *   那条循环的线程上 —— 也就是在它自己的回调里，或者用工作循环的邮箱把活儿
   *   挪过去。跨线程直接驱动一条连接是未定义行为。
   * - `set_read_callback()` / `set_auto_read()` / `set_ssl_context()` /
   *   `set_tls_handshake_timeout_ms()` 写的东西是**工作线程读**的，所以必须在
   *   `listen()` **之前**设好，之后再改就是数据竞争。
   * - `close_all_clients()` 在 n>1 时对别的循环是**异步发起**的（见该函数的说明）。
   *
   * ## Windows 上这条路的代价与边界
   *
   * 转手走 `WSADuplicateSocketW` + `WSASocketW(FROM_PROTOCOL_INFO)` + `uv_tcp_open`
   * —— libuv 眼里 **imported** 的一条血统。**"imported 才是问题所在"是错的**：
   * 闸是 `UV_HANDLE_SYNC_BYPASS_IOCP`，而它只在 worker 侧那次
   * `CreateIoCompletionPort` **成功**时才置（`libuv:win/tcp.c:103-110`、`:117-124`），
   * 那取决于**源 socket 有没有被关联过**。
   *
   * 本库这条转手的源是 `uv_accept` 出来的（**已关联**，`win/tcp.c:662` 那次
   * `imported=0` 的关联）⇒ worker 侧关联必然 `ERROR_INVALID_PARAMETER`（87）
   * ⇒ `UV_HANDLE_EMULATE_IOCP` ⇒ BYPASS **恒 0** ⇒
   * `UV_SUCCEEDED_WITHOUT_IOCP`（`libuv:win/req-inl.h:69`，**只查这一个位**）恒假
   * ⇒ `libuv/libuv#5282` 那一族在**这条**路上没有入口。外部贡献者的仪器在这条腿上
   * 读到 worker 句柄 `flags=0x8e088`；`0x6f08c`（BYPASS=1）是**另一条血统**
   * ——「master 裸 `accept()` 拿到再 dup」，也就是多进程那条形状。
   *
   * **代价是真的，只是落在别处**：EMULATE 意味着这条连接上每一笔 I/O 都走事件句柄
   * 那条模拟路。实测**库侧**约 **25~27%**（单循环 / 8 连接），端到端台架上只值
   * 约 2.5% —— 两个数**量程不同，别单独引**，见 `doc/net-guide.md` §4.2/§4.3。
   *
   * 上面那句"没有入口"是**结构**判断（那一族唯一的闸恒假），**不是**"这条路已经
   * 验过没有别的问题"。机制本身仍未定，证据包与复现装置见
   * `doc/worker-process-design.md` §9。
   *
   * @param n 循环总数，取 1..64。
   * @return 0 成功；`UV_EINVAL` n 越界；`UV_EBUSY` 已经 `listen()` 过，或者
   *         已经用 `n > 1` 装过一遍（装好之后不能再改）。`n == 1` 是幂等的
   *         空操作 —— 它什么都不建，所以随时返回 0。
   *
   * @warning 必须在 `listen()` 之前调用。工作线程是在**本函数里**起的，所以
   *          调用之后（哪怕还没 `listen()`）就已经有 n−1 条线程在跑了。
   */
  int set_loops(int n);

  /**
   * @brief 给每条工作循环装一个"就绪"钩子：**在 worker 线程上、那条循环开始跑
   *        之前**各调一次，参数是循环号与那条循环。
   *
   * **只对 1..n-1 号调**。0 号是接受者，它就是 `get_loop()`，由调用方在自己的
   * 线程上初始化 —— 那里没有"就绪"可言，调用方本来就掌握时机。
   *
   * 用途：属主要在每条工作循环上建**只能建在循环线程上的**句柄（`uv_async` /
   * `uv_timer`），并且要保证在**任何连接落上来之前**建好。`set_loops()` 返回时
   * 工作线程已经在跑了，所以这是唯一插得进去的地方。
   *
   * 时序保证：钩子跑在 worker 放行 `start()` 之前 ⇒ **`set_loops()` 返回时，
   * 所有钩子都已经跑完**。所以"先 `set_loops()` 再 `listen()`"这个顺序本身就
   * 保证了"连接到达时钩子已经跑过"。
   *
   * @param fn 钩子；传空函数则等同于没装（不产生额外开销 —— `n == 1` 时本来
   *           也没有工作线程可挂钩子）。
   *
   * @warning 必须在 `set_loops()` 之前设。装晚了（工作线程已经在跑）它不会
   *          被调用，也不会报错 —— 那正是"配了却没生效"这种最难查的形状，
   *          所以这条约束与 `set_ssl_context()` 那条同级。
   */
  void set_loop_start_hook(std::function<void(int, uvcpp_loop*)> fn);

  /** @brief 循环总数（1 + 工作循环数）。没调过 `set_loops()` 就是 1。 */
  int loop_count() const;

  /**
   * @brief 第 \p loop_index 条循环名下**当前登记着**的连接数。
   *
   * 0 号是接受者（`set_loops(n>1)` 时它一条都不留，恒为 0）。越界返回 0。
   * 它就是"分布看得见"的那个读数：轮转是显式的，所以这个数在连接数固定时
   * 是确定的。
   */
  size_t client_count_at(int loop_index) const;

  // -----------------------------------------------------------------
  // Stop
  // -----------------------------------------------------------------

  /**
   * @brief 停止接受新连接：关闭服务器的监听句柄。
   *
   * **不动已建立的连接** —— 它们照常收发，直到自己走完或对端断开。这一点
   * 和"关服"是两件事，所以要分两步：先 `stop()` 不再收新的，把手上正在处理
   * 的做完（这就是优雅关闭里的"排空"），再 `close_all_clients()` 送走剩下的。
   * 想一步到位就两个都调。
   *
   * 已经停止过（或从没 listen 过）时直接调 `on_stopped` 返回，不会重复关闭。
   *
   * @param on_stopped Optional callback invoked when the server has
   *                   fully stopped.
   */
  void stop(std::function<void()> on_stopped = nullptr);

  /**
   * @brief 关掉当前所有已登记的连接（**不影响监听**）。
   *
   * 每个连接都走 `uvcpp_tcp_client::close()`：关闭完成后摘除 + 释放，和对端
   * 断开走同一个收尾。所以调用之后不需要再管这些客户端。
   *
   * 登记表里如果有**句柄已经关掉、人却还留着**的连接（有人绕过框架直接
   * `get_tcp()->close(...)` 的结果），这里顺手把它们归还掉，不再发起关闭。
   *
   * @return 本次**发起关闭**的连接数。已经死掉被顺手归还的、所有权被
   *         `take_client()` 取走的都不计入。
   *
   * @note `set_loops(n>1)` 时：本循环名下的就地关（计入返回值），**别的循环是
   *       投递过去异步发起的**（不计入返回值）。所以 n>1 时这个数**不表示
   *       "已经关掉多少"** —— 要看关没关干净，看 `client_count()` 回零。
   *
   * @warning 必须在 loop 线程调用（n>1 时是**接受者**那条循环的线程）。
   */
  size_t close_all_clients();

  /**
   * @brief 关掉挂在 \p l 这条循环上的全部已登记连接 —— `close_all_clients()`
   *        的**每循环那一份**，不投递、不跨界。
   *
   * 语义与 `close_all_clients()` 的"本循环那一份"逐字相同（同一次快照、同一
   * 个 `get_loop() != l` 筛选、同一套顺手归还死连接的规则），区别只在于**它
   * 不会去碰别的循环**：不投递任何任务，返回的就是本次真正发起关闭的条数。
   *
   * 存在的理由是**停机次序**：优雅关闭要求"先发 Close/GOAWAY 帧、等它们出网、
   * 再关连接"。`close_all_clients()` 会把这件事劈成 n 份投递（acceptor 就地、
   * worker 各自异步），于是 0 号可以在别的循环还没道完别的时候就把它们的连接
   * 关了 —— 而那些循环上一拍才排队的帧一个字节都发不出去，对端只看到 1006。
   * 按循环各关各的，这条次序就在每一格内部**各自**成立，与 `n == 1` 同形，
   * 不需要任何跨循环栅栏。
   *
   * @return 本次**发起关闭**的条数（顺手归还的死连接不计）。
   *
   * @warning 必须在 \p l 的线程上调用。它不投递、不加锁地碰连接对象，
   *          从别的线程调就是隔着线程去动别人的连接。停机时每一格在**自己**
   *          那条循环上各调一次（这正是它的用法）。
   */
  size_t close_clients_on_loop(uvcpp_loop* l);

  // -----------------------------------------------------------------
  // 读：由框架统一管理
  // -----------------------------------------------------------------

  /**
   * @brief 设置**所有连接**共用的读回调（带语义的框架层读）。
   *
   * 这是给上层框架（webapp）用的入口：注册一次，之后每个被接受的连接都从
   * 这里拿数据。回调签名见 `net_read_result` —— 数据、对端正常关闭、读错误
   * 是三个**明确的事件**，而不是让用户去猜 `uvcpp_buf` 为什么没来。
   *
   * 设了它之后，每个新连接会自动 `read_start_events()`，使用者**不需要**也
   * **不应该**再在 `on_connection` 里自己 `read_start()`（那会拿到
   * `UV_EALREADY`）。设它之前 `on_connection` 里注册的读回调优先级更高。
   *
   * @warning 必须在 loop 线程调用，且应当在 `listen()` 之前设好。
   */
  void set_read_callback(const uvcpp_net_read_cb& cb);

  /** @brief 清掉共用的读回调（回到自动读兜底）。 */
  void clear_read_callback();

  /** @brief 是否设过共用的读回调。 */
  bool has_read_callback() const;

  /**
   * @brief 是否给新连接自动起读（默认 **true**）。
   *
   * 为什么默认开：**断开只在读回调里看得见**（`nread < 0` 分支），不读的连接
   * 永远不会被发现断开，也就永远不会走关闭回调、不会被回收 —— 那是每连接
   * 泄漏一个客户端对象的静默缺陷。默认自动读之后，"忘记起读"这个失败模式
   * 从根上没有了。
   *
   * 关掉它只在一种情况下有意义：你要自己完全接管读路径，且**自己负责发现
   * 断开**。
   */
  void set_auto_read(bool enable);
  bool auto_read() const;

  /**
   * @brief 暂停某个连接的读（背压）。
   *
   * 给"消费者跟不上生产者的速度"用：比如上传落盘跟不上、或者有界队列满了。
   * 连接上已注册的读回调**不会**被清掉，`resume_read()` 之后继续往同一个
   * 回调投递。被暂停期间数据留在内核缓冲区里，对端会因为 TCP 窗口而自然
   * 减速。
   *
   * @return 0 成功；非 0 为 libuv 错误码。客户端不归本服务端管时返回
   *         `UV_EINVAL`（不能对别人的连接动手）。
   *
   * @note `set_loops(n>1)` 时这条连接只可能在**它自己那条循环的线程**上被
   *       暂停/恢复 —— 从别的线程调是未定义行为（`owns_client` 那一问是加锁
   *       的、跨线程安全，但真正动句柄那一步不是）。
   */
  int pause_read(uvcpp_tcp_client* client);

  /** @brief 恢复某个连接的读。语义见 `pause_read()`。 */
  int resume_read(uvcpp_tcp_client* client);

  // -----------------------------------------------------------------
  // 客户端所有权：三态
  // -----------------------------------------------------------------

  /**
   * @brief 本服务端当前登记的客户端数量（**所有循环的合计**）。
   *
   * 取走所有权（`take_client`）的客户端**不计入**这里 —— 它们已经不归
   * 服务端管了。可以用它来断言"没有泄漏"：连接关掉一批之后这个数应该
   * 回到基线。
   *
   * 登记表是加锁的，所以**从任何线程调都对**。n>1 时它是一份快照：读数的那一
   * 刻别的线程可能正在增删，所以"回到 0"这件事要**重试着看**，不要只看一次。
   */
  size_t client_count() const;

  /** @brief 这个客户端当前是否归本服务端管理。可跨线程调用（登记表加锁）。 */
  bool owns_client(const uvcpp_tcp_client* client) const;

  /**
   * @brief **取走所有权**：把客户端从服务端手里摘出来，交给你自己管。
   *
   * 调用之后服务端**再也不碰它**：不关闭、不释放、不做任何后续处理，
   * 也不再从它那里收到任何回调。你必须自己负责在合适的时候
   * `client->get_tcp()->close(...)` 并 `delete client`。
   *
   * 什么时候需要它：
   *   - 要把连接**长期留住**当成一条自己的信道（比如协议升级后自己接管
   *     读写、或者把连接转交给另一个对象/线程）；
   *   - 要用一个不是 `delete` 的方式来回收（放进自己的对象池）。
   *
   * **不取走就什么都不用做**：默认情况下客户端完全由服务端管理，断开时
   * 自动摘除并释放，使用者不需要写任何清理代码。
   *
   * @return 成功返回同一个指针；如果它本来就不归本服务端管（已经取走过、
   *         已经交还过、或者压根不是这里的客户端）返回 nullptr。
   *         返回 nullptr 时**所有权没有变化**，不要据此去 delete。
   *
   * @note 取走的是**管理权**，不是"这条连接从今往后归你随便驱动"。`set_loops(n>1)`
   *       时它的事件仍然只在**它自己那条循环的线程**上到达，你也只能在那个线程上
   *       驱动它（在自己的回调里，或者把活儿投到那条循环的邮箱上）。在别的线程上
   *       对它 `write`/`close` 是未定义行为。
   *
   * @warning 必须在 loop 线程上调用。
   */
  uvcpp_tcp_client* take_client(uvcpp_tcp_client* client);

  /**
   * @brief **交还所有权**：把之前 `take_client()` 取走的客户端还给服务端。
   *
   * 交还之后它重新受服务端管理 —— 断开时自动摘除并释放，你就不必再管了。
   *
   * 连接**已经关闭**的情况下不会重新登记（它不可能再产生事件了），而是
   * 就地释放并返回 true。这就是"由框架决定是删除还是复用"的落点：现在是
   * 删除；将来要加客户端对象池，复用的分支就接在这里。
   *
   * @return 交还成功返回 true。客户端本来就归服务端管（重复交还）返回
   *         false，此时什么都没发生。
   *
   * @note `set_loops(n>1)` 时它回到**它自己那条循环**名下（登记表里记的是
   *       客户端自己的 `get_loop()`，不是"你现在所在的这条"），所以交还必须
   *       在它自己那条循环的线程上做。
   *
   * @warning 必须在 loop 线程上调用。交还之后**不要**再持有那个指针。
   */
  bool return_client(uvcpp_tcp_client* client);

  // -----------------------------------------------------------------
  // TLS（HTTPS / WSS 服务端）
  // -----------------------------------------------------------------

#if UVCPP_OPENSSL_ENABLE
  /**
   * @brief 给之后接受的每条连接装上 TLS 过滤层。
   *
   * 装上之后，**上层的 `on_connection` 被推迟到握手成功之后**才调用：握手
   * 没完成时连接算不上可用（此时 `write()` 必然失败，因为 SSL_write 要求
   * 握手已完成），所以不把它交出去。客户端一上来就打明文（比如直接把
   * HTTP 请求写进 TLS 端口）时，握手失败 —— 上层收到的是**根本没被通知
   * 过这条连接**，连接由框架自行收尾，不会走 `on_connection`。
   *
   * 握手失败只记账（`last_error_code_`），不额外做清理：收尾走的是和明文
   * 读错误完全相同的一条路（读侧通知 → close manager 摘除并释放）。
   *
   * @param ctx 生命周期必须覆盖整个服务端（本服务端**不持有**它的所有权，
   *            也不负责释放）。用 `uvcpp_ssl_context` 的 SERVER 模式，
   *            并装好证书/私钥。
   *
   * @warning 必须在 `listen()` 之前设置。清空用 `set_ssl_context(nullptr)`。
   * @warning 必须在 loop 线程调用。
   */
  void set_ssl_context(uvcpp_ssl_context* ctx);

  /** @brief 当前的 TLS 上下文；没装 TLS 时为 nullptr。 */
  uvcpp_ssl_context* ssl_context() const { return ssl_ctx_; }

  /**
   * @brief TLS 握手的超时（毫秒）。0 = 不设。默认 10000。装 TLS 才有意义。
   *
   * 服务端在握手成功之前**不会**把连接交给上层（见 `uvcpp_tcp_client::
   * set_tls_ready_callback`），所以这期间这条连接不在上层的连接表里，上层的
   * 闲置超时看不到它。没有这道闸门时，一个「连上之后每 500 ms 发一个字节喂
   * ClientHello、永远不把握手做完」的客户端可以让连接无限期挂着。
   *
   * 值会传给之后 accept 的每一条连接；已经建立的连接不受影响。
   * @warning 必须在 loop 线程调用。
   */
  void set_tls_handshake_timeout_ms(int ms);

  /** @brief 当前的 TLS 握手超时；0 表示不设。 */
  int tls_handshake_timeout_ms() const { return tls_hs_timeout_ms_; }
#endif  // UVCPP_OPENSSL_ENABLE

  // -----------------------------------------------------------------
  // Loop control
  // -----------------------------------------------------------------

  /** @brief Run the internal event loop. */
  int run(uv_run_mode md = UV_RUN_DEFAULT);

  /** @brief Stop the internal event loop. */
  void stop_loop();

 private:
  // -----------------------------------------------------------------
  // Internal helpers
  // -----------------------------------------------------------------

  void set_status(int flags);
  void clear_status(int flags);

  /**
   * @brief 摘除 + 释放一个客户端（关闭收尾的唯一落点）。
   *
   * 用户的 `on_close` 观察槽和框架的管理槽都走到这里；`close_all_clients()`
   * 清理"句柄已关、人还在"的残留也用它。
   *
   * @warning 调用之后不能再持有 `client`。
   */
  void release_client(uvcpp_tcp_client* client);

  /** @brief Post-connection callback: check/set client callbacks. */
  void setup_client_callbacks(uvcpp_tcp_client* client);

  /**
   * @brief 给客户端装上框架的关闭管理回调（摘除 + 释放）。
   *
   * 走的是 `set_close_manager()` 而不是用户的 `set_on_close()` —— 两个槽
   * 分开，用户的回调就永远无法取消框架的释放。
   */
  void install_client_manager(uvcpp_tcp_client* client);

  /**
   * @brief 用户 on_connection 回调抛异常时的收尾：关掉并回收这个客户端。
   *
   * 不直接 `delete`：那时连接句柄还是活的，在 libuv 的 accept 回调里同步
   * 析构一个活动句柄不安全。改成走关闭流程，在它的完成回调里摘除 + 释放。
   */
  void close_client_on_callback_error(uvcpp_tcp_client* client);

  /**
   * @brief 把连接交给上层（`on_connection`），带异常兜底。
   *
   * 抽出来是因为它有**两个**调用点：不装 TLS 时在 accept 回调里直接就调；
   * 装了 TLS 时推迟到握手完成的通知里。两处必须走同一段逻辑。
   *
   * **刻意不在 `#if UVCPP_OPENSSL_ENABLE` 里面。** 不装 TLS 的那条路
   * （`listen()` 的 accept 回调末尾）也调它，而那句调用在 `#endif` 之外 ——
   * 把声明一起关掉会让**没开 OpenSSL 的构建直接编译不过**。这个错误曾经
   * 真的存在过：`build-ssl` 单独加了这个函数、`build-webapp` 没跟着重建，
   * 于是那边编译失败而 ctest **照样报 54/54 全绿**（跑的是上一轮的陈旧
   * 二进制）。比红更坏的是"因为没构建而看起来是绿"。
   */
  void deliver_connection(uvcpp_tcp_client* client);

#if UVCPP_OPENSSL_ENABLE
  /**
   * @brief 握手结束（成功/失败）时的服务端侧处理。
   *
   * 成功 → `deliver_connection()`；失败 → 只记 `last_error_code_`，收尾
   * 交给框架既有的那条路（见头文件里 `set_ssl_context` 的说明）。
   */
  void on_tls_handshake_done(uvcpp_tcp_client* client, int status);

  /** @brief 服务端级 TLS 上下文（非拥有）。nullptr = 明文。 */
  uvcpp_ssl_context* ssl_ctx_ = nullptr;
  /** TLS 握手超时（毫秒）；0 = 不设。accept 时传给每条新连接。 */
  int tls_hs_timeout_ms_ = 10000;
#endif  // UVCPP_OPENSSL_ENABLE

  // -----------------------------------------------------------------
  // 多循环（`set_loops(n > 1)` 时才用得上）
  // -----------------------------------------------------------------

  /** @brief 第 \p index 条循环。0 = acceptor 循环（`loop_`）；越界返回 nullptr。 */
  uvcpp_loop* loop_at(int index) const;

  /**
   * @brief acceptor 侧：把刚接受的连接取出来转手给某个 worker。
   *
   * 只在 `workers_` 非空时被调。**必须在关掉 acceptor 侧那个临时句柄之前**
   * 把 socket 取出来（`uv_close` 会同步关掉它那份 fd），所以顺序是
   * 取 fd → 转手 → 关临时句柄。
   *
   * 任何一步失败都只记账（`last_error_code_` / `TCP_SERVER_ERROR`）并放掉
   * 已经造出来的东西 —— **不**让一条坏连接把整个 accept 回调掀掉。
   */
  void accept_and_handoff(uvcpp_stream* s);

  /**
   * @brief worker 线程侧：认领转手过来的 socket，跑完整条接受尾巴。
   *
   * \p sock 一进来就归本函数；无论走哪条失败路径，它都会被恰好关一次。
   */
  void on_handoff_task(int worker_index, uv_os_sock_t sock);

  /**
   * @brief 接受尾巴：登记 → 装读 → TLS → 交付。
   *
   * **必须在目标循环的线程上跑**（`enable_tls` 会当场在这条循环上 arm 一次
   * 读）。`n == 1` 时就是 accept 回调里那段；`n > 1` 时由 worker 线程调 ——
   * 两条路共用这一份，免得以后改一处忘一处。
   */
  void finish_accept(uvcpp_tcp_client* client);

  /** @brief 登记（加锁）。任何线程可调。 */
  void register_client(uvcpp_tcp_client* client);
  /** @brief 摘除（加锁）。任何线程可调；**不**释放对象。 */
  void unregister_client(uvcpp_tcp_client* client);
  /** @brief 取登记表快照（加锁）。在锁外逐个动手，别跨用户代码持锁。 */
  std::vector<uvcpp_tcp_client*> snapshot_clients() const;

  /**
   * @brief 关掉挂在 \p l 这条循环上的全部登记连接。**在 \p l 的线程上跑。**
   *
   * `close_all_clients()` 的每循环那一份：`n > 1` 时 acceptor 自己那份直接调，
   * 其余各 worker 那份由 `post()` 投进去。worker 退出前的收尾也用它。
   *
   * @return 本次**发起关闭**的条数（顺手归还的死连接不计）。
   */
  size_t close_clients_of_loop(uvcpp_loop* l);

  /** @brief 停掉并 join 全部 worker（幂等）。**必须在既有四步收尾之前**调。 */
  void stop_workers();

  // Trampoline — C-style fn ptr + void* to avoid MSVC std::function
  // copy-chain corruption through libuv's callback layers.
  using connection_callback_t = void(*)(uvcpp_tcp_client* client,
                                        void* arg);

  // -----------------------------------------------------------------
  // Member variables
  // -----------------------------------------------------------------

  uvcpp_loop* loop_ = nullptr;
  uvcpp_tcp*  tcp_  = nullptr;
  bool stopped_     = false;  ///< true if stop() already closed the handle

  /**
   * @brief 状态位。
   *
   * `n > 1` 时 **worker 线程上也会写**（转手失败要记账），而用户随时可能从
   * 别的线程 `get_status()` —— 所以是原子。读-改-写在 `set_status` 里用
   * `fetch_or`/`fetch_and`，不是"读出来再写回去"。
   */
  std::atomic<int> status_{TCP_SERVER_NONE};
  /** @brief 最近一次错误码。同上：worker 线程可能写。 */
  std::atomic<int> last_error_code_{0};

  /**
   * @brief 全服务端的连接登记表 + 它的锁。
   *
   * **一张表，不是每循环一张。** `n > 1` 时这张表被多个线程碰：worker 线程
   * 在收尾时摘除、用户可能从任何线程 `client_count()` / `owns_client()`。
   * 锁只护"读表 / 改表"这一步 —— 绝不跨 `client->close()`、也绝不跨用户代码
   * 持有（用户回调里再进 `client_count()` 就是死锁）。
   *
   * `n == 1` 时它仍然只有 acceptor 线程碰，锁是白拿的（`std::mutex` 未竞争
   * 时就是一条原子指令）。
   */
  std::vector<uvcpp_tcp_client*> clients_;
  mutable std::mutex clients_mu_;

  /** @brief 工作循环（n−1 条；`n == 1` 时为空 = 今天那条路）。 */
  std::vector<uvcpp_loop_worker*> workers_;
  /** @brief 轮转计数：第 i 条连接给 `1 + (i % (n-1))`。 */
  std::atomic<uint64_t> rr_{0};

  /**
   * @brief `set_loop_start_hook()` 装进来的钩子。
   *
   * **必须在 `set_loops()` 之前写、之后只读** —— 它在 worker 线程上被读，写它
   * 的人和读它的人之间靠 `start()` 的交握手建立先后关系（与 `ssl_ctx_` 同一套）。
   * 装晚了不会报错也不会生效，见那条 `@warning`。
   */
  std::function<void(int, uvcpp_loop*)> loop_start_hook_;

  /** @brief User connection callback stored as trampoline pair. */
  connection_callback_t on_connection_fn_ = nullptr;
  void*                 on_connection_arg_ = nullptr;

  /** @brief 所有连接共用的读回调（set_read_callback）。空则走自动读。 */
  uvcpp_net_read_cb read_cb_;
  /** @brief 自动给新连接起读（默认 true，见 set_auto_read）。 */
  bool auto_read_ = true;
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_TCP_SERVER_H
