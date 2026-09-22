/**
 * @file src/net/uvcpp_tcp_client.h
 * @brief Higher-level TCP client with async/sync dual-mode API built on uvcpp_tcp.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides both callback-based (async) and blocking-wait (sync) operations
 * with internal read cache, status tracking, and address helpers.
 */

#pragma once
#ifndef SRC_NET_UVCPP_TCP_CLIENT_H
#define SRC_NET_UVCPP_TCP_CLIENT_H

#include <uvcpp/uvcpp_config.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <uv.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_tcp.h>
#include <net/uvcpp_net_read.h>
#include <uvcpp/uvcpp_buf.h>

/**
 * @brief 试发快速路径的**最小长度**（字节，构建值，默认 32 KiB）。
 *
 * 低于它的写不试发、直接走原来的路径。为什么是 32 KiB 见
 * `src/net/uvcpp_tcp_client.cpp` 里那段账 —— 一句话：快路径每笔写固定多一次
 * 系统调用（整条吃下时那次 `uv_write` 走 0 长度，libuv 两个平台上都不对它
 * 短路），换来的是省掉一份 `len` 字节的拷贝，小报文上这笔是亏的。
 *
 * 值**不能**在这里由使用者覆盖：它决定的是客户端对象的大小/行为，而这个类会
 * 被链接进 uvcpp.dll，两边不一致就是 ABI 不一致。要改就重编库。
 * 这个宏由 <uvcpp/uvcpp_config.h> 给出，本文件开头已包含它。
 */

namespace uvcpp {

#if UVCPP_OPENSSL_ENABLE
class uvcpp_ssl;          // 见 src/ssl/uvcpp_ssl.h
class uvcpp_ssl_context;
#endif
class uvcpp_timer;        // 见 src/handle/uvcpp_timer.h（握手超时用，只存指针）

/**
 * @brief Bitmask status flags for TCP client lifecycle.
 *
 * Multiple flags can be combined: a connected, readable, writable client
 * has status TCP_CLIENT_CONNECTED | TCP_CLIENT_READABLE | TCP_CLIENT_WRITABLE.
 */
enum uvcpp_tcp_client_status : int {
  TCP_CLIENT_NONE       = 0x00,  ///< Initial state
  TCP_CLIENT_CONNECTING = 0x01,  ///< Connect in progress
  TCP_CLIENT_CONNECTED  = 0x02,  ///< Connection established
  TCP_CLIENT_READABLE   = 0x04,  ///< Data available to read
  TCP_CLIENT_WRITABLE   = 0x08,  ///< Ready to write
  TCP_CLIENT_CLOSING    = 0x10,  ///< Close in progress
  TCP_CLIENT_CLOSED     = 0x20,  ///< Connection closed
  TCP_CLIENT_ERROR      = 0x40   ///< An error occurred (see last_error_code)
};

/**
 * @brief Application-layer TCP client wrapping uvcpp_tcp.
 *
 * Dual-mode API:
 * - async:  provide a callback; the method returns immediately and the
 *           callback fires on the internal event loop when complete.
 * - sync:   omit the callback (or use the *_wait variants); the method
 *           blocks up to timeout_ms (default 30000) pumping the loop.
 *
 * Mixing async and sync for the same operation (connect/write/read) is
 * not allowed and will throw std::runtime_error.
 *
 * Usage (sync):
 * @code
 *   uvcpp_tcp_client client;
 *   client.connect_wait("127.0.0.1", 8080);
 *   client.write_wait("hello", 5);
 *   uvcpp_buf buf;
 *   client.read_wait(buf);
 * @endcode
 *
 * Usage (async):
 * @code
 *   uvcpp_tcp_client client;
 *   client.connect("127.0.0.1", 8080, [](int status) {
 *     // connected
 *     client.write("data", 4, [](int ws) { ... });
 *     client.read_start([](uvcpp_buf* b) { ... });
 *   });
 *   client.run(UV_RUN_DEFAULT);
 * @endcode
 */
class UVCPP_API uvcpp_tcp_client {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_tcp_client)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_tcp_client)

  /**
   * @brief Construct a client that shares an external event loop.
   *
   * Used by uvcpp_tcp_server to create accepted clients that run on the
   * server's loop. The client does NOT own the loop and will not close it
   * on destruction.
   */
  explicit uvcpp_tcp_client(uvcpp_loop* external_loop);

  /**
   * @brief 认领一个**已经属于** \p external_loop 的 socket，在它上面建客户端。
   *
   * 给多循环的转手路径用：socket 是在别的循环上接受下来的，取出来之后到目标
   * 循环的线程上装成句柄。**调用者必须保证这个 socket 与目标循环的关联还没
   * 发生过**（Windows 的完成端口关联是一次性的，见 `uvcpp_socket_handoff.h`）。
   *
   * 失败（`uv_tcp_open` 没收下）时对象仍可构造，但状态是 `TCP_CLIENT_ERROR`、
   * `get_last_error()` 给出原因 —— 而且**socket 仍归调用方**，要自己关掉。
   */
  uvcpp_tcp_client(uvcpp_loop* external_loop, uv_os_sock_t adopted_sock);

  // -----------------------------------------------------------------
  // Accessors
  // -----------------------------------------------------------------

  /** @brief Return the underlying uvcpp_tcp handle. */
  uvcpp_tcp* get_tcp();

  /** @brief Return the internal event loop. */
  uvcpp_loop* get_loop();

  /**
   * @brief Return the current status bitmask.
   * @see uvcpp_tcp_client_status
   */
  int get_status() const;

  /** @brief Return the last recorded error code (libuv errno). */
  int get_last_error() const;

  /** @brief Check whether all given status flags are set. */
  bool has_status(int flags) const;

  /**
   * @brief Check whether a **使用者**注册的读回调存在。
   *
   * 框架的自动读（`install_auto_read`，只为发现断开而存在）**不算** ——
   * 否则"用户没设读回调"这个判断会被框架自己装的东西掩盖掉。
   */
  bool has_read_callback() const;

  /** @brief 当前跑的是不是框架的自动读（只为发现断开，数据会被丢弃）。 */
  bool is_auto_read() const;

  /** @brief Check whether an async write callback has been registered. */
  bool has_write_callback() const;

  /** @brief Check whether a close callback has been registered. */
  bool has_close_callback() const;

  /**
   * @brief Mark the client as accepted (connected without a connect() call).
   *
   * Used by uvcpp_tcp_server to set the initial CONNECTED | READABLE |
   * WRITABLE state on accepted clients so that write() and read_start()
   * work correctly.
   */
  void mark_accepted();

  // -----------------------------------------------------------------
  // TLS 过滤层（UVCPP_OPENSSL_ENABLE=1 时才有实现）
  // -----------------------------------------------------------------
  //
  // 装上之后，**本对象对外说的仍然是明文** —— `write()` 收明文、读回调交
  // 明文；密文只在这层内部与 socket 之间流动。所以 http / ws 层一行都不用改。
  //
  // 为什么是「这一层」而不是 `SSL_set_fd`：`SSL_set_fd` 把 fd 交给 OpenSSL
  // 持有，于是 `WANT_WRITE` 时本端无从得知"socket 何时可写"，只能 `uv_poll`
  // （同一个 fd 上不能与 `uv_read_start` 共存）或者转圈（违反本项目的
  // 「不在主循环做耗时操作」）。换成 memory BIO 之后 socket 始终归 libuv，
  // `WANT_WRITE` 的含义变得精确 —— wbio 里有字节待发，而它们何时发完，
  // libuv 的写完成回调会告知。详细论证见 `src/ssl/uvcpp_ssl.h`。
#if UVCPP_OPENSSL_ENABLE
  /**
   * @brief 给这条连接装上 TLS 过滤层。
   *
   * 调用时机：
   * - **客户端**：`connect()` 之前。连接建立后不立刻回调使用者，而是先把
   *   握手跑完；握手成功才回调 —— 使用者拿到 CONNECTED 时就可以直接发
   *   应用数据了。
   * - **服务端**：accept 之后（`mark_accepted()` 之后）。此时连接已建立，
   *   本函数会立即发起握手并在读事件里推进它。
   *
   * @param ctx 生命周期必须覆盖整条连接（`uvcpp_ssl_context` 不属于本对象）。
   * @return 0 成功；`UV_EALREADY` 已经装过；`UV_EINVAL` ctx 不可用；
   *         `UV_ENOMEM` 内部 SSL 对象创建失败。
   */
  int enable_tls(uvcpp_ssl_context* ctx);

  /** @brief 本连接是否装了 TLS 过滤层。 */
  bool is_tls() const { return tls_ssl_ != nullptr; }

  /**
   * @brief 本条连接宣告支持的 ALPN 协议名（顺序即优先级）。
   *
   * `enable_tls()` 前后调都可以：之前调就先记下、建好 SSL 对象时再装上。
   * 必须在**握手开始之前**生效，而客户端握手是在 connect 完成回调里才起的，
   * 所以这两个时机都够早。
   *
   * 与 context 上那个 `set_alpn_protos` 的关系是"覆盖"：同一条连接只有一次
   * 协商机会，而"要不要 h2"是每条连接各自的决定。
   */
  bool set_tls_alpn_protos(const std::vector<std::string>& protos);

  /** @brief TLS 握手是否已完成（非 TLS 连接恒为 true）。 */
  bool is_tls_handshake_done() const {
    return tls_ssl_ == nullptr ? true : tls_handshake_done_;
  }

  /**
   * @brief TLS 层最近一次 `SSL_ERROR_*`。
   *
   * 握手/读写失败时用它区分「对端干净关闭」（`SSL_ERROR_ZERO_RETURN`）、
   * 「证书校验失败」等。0 表示没有记录。
   */
  int tls_last_ssl_error() const;

  /**
   * @brief 握手协商出的 ALPN 协议名；没协商出来（含非 TLS 连接）返回空串。
   *
   * 值在握手完成那一刻就缓存进本对象，所以**在 ready 回调里、以及回调之后
   * 任何时刻读都是安全的** —— ready 回调的契约允许对象在回调内被析构，从
   * 那里反查 `SSL*` 是不安全的。
   */
  const std::string& tls_alpn_selected() const { return tls_alpn_; }

  /**
   * @brief 握手结束（成功或失败）时通知一次。
   *
   * **服务端用它把"把连接交给上层"推迟到握手之后。** accept 之后先
   * `enable_tls()`，如果握手还没完就先不调 `on_connection` —— 否则上层会在
   * TLS 尚未建立时就认为连接可用：此时 `write()` 必然失败（SSL_write 要求
   * 握手已完成），而"已连接"这个语义本身就是错的。
   *
   * 客户端不需要它：客户端那条路径由 `connect` 回调推迟来处理。
   *
   * @param cb     回调；`status == 0` 为握手成功，否则为失败的错误码。
   * @param 注意   只触发一次，触发后自动清空。失败时它在关闭回调**之前**
   *               触发，所以回调里看到的是一个还活着的对象。
   */
  void set_tls_ready_callback(std::function<void(uvcpp_tcp_client*, int)> cb);

  /**
   * @brief TLS **握手阶段**的超时（毫秒）。**0 = 不设超时**。默认 10000。
   *
   * 为什么必须有它
   * --------------
   * 服务端 accept 之后要等握手成功才把连接交给上层（见 `set_tls_ready_callback`
   * 的说明），而**在这之前这条连接不在上层任何登记表里** —— webapp 那一层的
   * 闲置扫描遍历的是自己的连接表，握手期的连接根本不在里面，`idle_timeout_ms`
   * 因此管不到它。
   *
   * 于是有一条明文的 slowloris 覆盖不到的路：客户端连上之后，每 500 ms 发一个
   * 字节去喂 ClientHello，握手永远差一点不完成。这条连接会连同它的 SSL 对象、
   * 读写缓冲区**无限期挂着**，而占用的带宽近乎为零。明文那条路有
   * `idle_timeout_ms` 兜着（判据是"从请求的第一个字节起算"），但那条规则的前提
   * 是"连接已经在登记表里" —— 握手期的连接不满足这个前提。
   *
   * 所以这不是 `idle_timeout_ms` 的替代品，而是它**覆盖不到的那一段**的补位：
   * 上限从握手开始（第一次 `tls_drive_handshake()`）起算，握手一结束（无论成功
   * 还是失败）就撤销。
   *
   * @note 客户端一侧同样生效 —— "连上了但对端永远不把握手走完"也是同一种占用。
   */
  void set_tls_handshake_timeout_ms(int ms);

  /** @brief 当前的握手超时；0 表示不设。 */
  int tls_handshake_timeout_ms() const { return tls_hs_timeout_ms_; }
#endif  // UVCPP_OPENSSL_ENABLE

  // -----------------------------------------------------------------
  // Address helpers
  // -----------------------------------------------------------------

  /**
   * @brief Retrieve the local socket address.
   * @param[out] ip  Receives the IP string (IPv4 or IPv6).
   * @param[out] port Receives the port number in host byte order.
   * @return The raw sockaddr_storage for advanced use.
   */
  sockaddr_storage getLocalAddrs(std::string& ip, int& port);

  /**
   * @brief Retrieve the remote (peer) socket address.
   * @param[out] ip  Receives the IP string (IPv4 or IPv6).
   * @param[out] port Receives the port number in host byte order.
   * @return The raw sockaddr_storage for advanced use.
   */
  sockaddr_storage getPeerAddrs(std::string& ip, int& port);

  // -----------------------------------------------------------------
  // Read cache
  // -----------------------------------------------------------------

  /** @brief Set the maximum read cache size in bytes (default 2 MB). */
  void set_max_read_cache_size(size_t max_size);

  /** @brief Get the current maximum read cache size. */
  size_t get_max_read_cache_size() const;

  // -----------------------------------------------------------------
  // Connect
  // -----------------------------------------------------------------

  /**
   * @brief Connect to a remote host.
   *
   * @param ip   IPv4 or IPv6 address string.
   * @param port Port number.
   * @param cb   If non-null, async connect: returns immediately, cb(status)
   *             fires on the internal loop when complete.
   *             If null, behaves like connect_wait(ip, port, 30000).
   * @return 0 on async start, or the connect result for sync mode.
   */
  int connect(const char* ip, int port,
              std::function<void(int)> cb = nullptr);

  /**
   * @brief Synchronous connect with explicit timeout.
   * @throws std::runtime_error if an async connect callback was already
   *         registered.
   */
  int connect_wait(const char* ip, int port, int timeout_ms = 30000);

  // -----------------------------------------------------------------
  // Write
  // -----------------------------------------------------------------

  /**
   * @brief Write data to the connection (raw pointer + length).
   *
   * @param data Pointer to data to send.
   * @param len  Number of bytes to send.
   * @param cb   If non-null, async write: returns immediately, cb(status)
   *             fires when the write completes.
   *             If null, behaves like write_wait(data, len, 30000).
   * @return 0 on async start, or the write result for sync mode.
   *
   * @note 一条连接上**同时只允许一笔异步写在途**（这条对下面每个带 `cb` 的重载
   *       都成立，标志是每连接一个）。上一笔的完成回调还没跑时再发起，返回
   *       `UV_EALREADY`，而且**这一笔的字节不会被发出、也不会有回调** ——
   *       它不是排队，是拒收。要连着写就在回调里续投（`cb` 是在标志清掉
   *       **之后**才调的，所以回调里直接再写是合法的）。这一层不做写队列，
   *       见 `doc/net-guide.md` §9、§14。
   */
  int write(const char* data, size_t len,
            std::function<void(int)> cb = nullptr);

  /**
   * @brief Write data from a uvcpp_buf (auto-copy).
   *
   * The buffer's data is copied internally; the caller retains ownership
   * and the original uvcpp_buf is unchanged after the call.
   */
  int write(const uvcpp_buf& buf,
            std::function<void(int)> cb = nullptr);

  /**
   * @brief Write data from a uvcpp_buf pointer (zero-copy, transfers ownership).
   *
   * The buffer's internal data is moved out via out_uv_buf() and passed
   * directly to the write layer.  After the call the uvcpp_buf is empty.
   * The caller still owns the uvcpp_buf object itself (e.g. must delete
   * it if heap-allocated) — only the data payload is transferred.
   *
   * **TLS 连接上是例外**：加密要求明文经过 `SSL_write`，而它会把明文**拷**进
   * 自己的缓冲，所以没有"零拷贝"可做，`out_uv_buf()` 也不调用 —— 调用之后
   * `uvcpp_buf` **仍然是满的**，数据仍归它（它析构时释放）。两种连接下
   * "回调触发时这次写已完成、可以安全释放缓冲"这一点是一样的。
   *
   * NOTE: do NOT pass the address of a temporary; the pointer must remain
   * valid until the write callback fires (for async) or the call returns
   * (for sync).
   *
   * 在途契约同 `write(const char*, size_t, cb)`：已经有异步写在飞时返回
   * `UV_EALREADY`。这一支上拒收是**安全**的 —— 返回时缓冲一个字节没动、
   * 仍归调用方（`out_uv_buf()` 还没被调用），可以直接重投。
   */
  int write(uvcpp_buf* buf,
            std::function<void(int)> cb = nullptr);

  /**
   * @brief 头 + 体两块分开写（`uv_write` 的 `nbufs = 2`），体那块不拷。
   *
   * 与 `write(const char*, size_t, cb)` 的差别只有一个：那一支要求调用方先把
   * "头部 + 体"合并成一条，而合并那一步会把体整份拷一遍。这里体是**第二块**，
   * 一个字节都不拷；头仍然照旧拷进自有块（几百字节量级，是这条路上剩下的唯一
   * 一份拷贝）。
   *
   * **不走 `uv_try_write` 快路径**，这是有意的：快路径存在的唯一理由是那一支
   * 已经拷了一份、所以"已经出去的前缀不必再拷"能省下一次 memcpy。这条路上体
   * 根本没有拷贝，试发省不下任何东西，只会平白多一次系统调用。
   *
   * `body` 的内容**被消费**，与 `write(uvcpp_buf*)` 同一条契约：
   *   - 共享视图（`is_shared()`）—— 把引用计数接走，那块字节原地不动，`*body`
   *     自己仍是同一份视图；
   *   - 自有块 —— 把块的所有权接走（`out_uv_buf()`），`*body` 之后是空的。
   * 两种情况下**都不许**在回调之前改写 `*body`。
   *
   * **TLS 连接上是例外**（与 `write(uvcpp_buf*)` 同一个理由）：`SSL_write` 要求
   * 明文经它加密，那是**拷**进去的，所以那一条支路退回"合并成一条再写"。
   *
   * @param head     头部字节（状态行 + 各头 + 空行）。
   * @param head_len 头部长度。
   * @param body     响应体。**不能**是临时对象的地址：异步写要等到完成回调
   *                 才用完它（数据本身由请求对象持有，见上面的消费语义）。
   * @param cb       完成回调；传 nullptr 会退化成"合并成一条的同步写"。
   */
  int write(const char* head, size_t head_len, uvcpp_buf* body,
            std::function<void(int)> cb);

  /**
   * @brief Synchronous write with explicit timeout (raw pointer).
   * @throws std::runtime_error if an async write callback was already
   *         registered.
   */
  int write_wait(const char* data, size_t len, int timeout_ms = 30000);

  /**
   * @brief Synchronous write from uvcpp_buf (auto-copy).
   */
  int write_wait(const uvcpp_buf& buf, int timeout_ms = 30000);

  /**
   * @brief Synchronous write from uvcpp_buf pointer (zero-copy).
   */
  int write_wait(uvcpp_buf* buf, int timeout_ms = 30000);

  // -----------------------------------------------------------------
  // Read
  // -----------------------------------------------------------------

  /**
   * @brief Start reading from the connection.
   *
   * Async mode (cb != nullptr): cb is called for every chunk of data
   * received. Use read_stop() to stop.
   *
   * Sync mode (cb == nullptr): enables the internal read cache so that
   * subsequent read_wait() calls can retrieve data.
   */
  int read_start(std::function<void(uvcpp_buf*)> cb = nullptr);

  /**
   * @brief 框架层读：回调带明确的语义（数据 / 对端关闭 / 读错误）。
   *
   * 与 `read_start(cb)` 的区别见 `uvcpp_net_read.h` 的说明 —— 简言之，
   * 原始那版在 `nread < 0` 时**什么都不回调**，用户既分不清"对端关了"和
   * "读挂了"，也拿不到错误码。
   *
   * 两者互斥：已经用过其中一个就不能再用另一个（返回 `UV_EALREADY`）。
   *
   * 收到 `PEER_CLOSED` / `READ_ERROR` 之后本回调不会再被调用；框架随后的
   * 收尾（关闭回调）照常发生，所以调用方不需要在这个回调里做释放。
   */
  int read_start_events(const uvcpp_net_read_cb& cb);

  /**
   * @brief Synchronously wait for and return received data.
   *
   * Blocks until data is available in the internal read cache or timeout.
   * On success the data is moved into out_buf.
   *
   * @throws std::runtime_error if an async read callback was already
   *         registered.
   */
  int read_wait(uvcpp_buf& out_buf, int timeout_ms = 30000);

  /** @brief Stop reading from the connection. */
  int read_stop();

  /**
   * @brief Pause the underlying stream read WITHOUT clearing the async read
   *        callback. Use read_resume() to resume. Intended for backpressure
   *        (e.g. a streaming upload whose bounded buffer is full).
   */
  int read_pause();

  /** @brief Resume a paused read by re-arming the async read callback. */
  int read_resume();

  // -----------------------------------------------------------------
  // 关闭：主动关闭的唯一正确入口
  // -----------------------------------------------------------------

  /**
   * @brief **主动关闭连接** —— 会走完整的关闭流程（含框架的释放）。
   *
   * 为什么必须有这个函数，而不是直接 `get_tcp()->close(...)`：
   *
   * 断开的发现原本只有**读回调的 `nread < 0` 分支**这一条路。对端断开时它
   * 会跑，于是框架的释放（`uvcpp_tcp_server` 的 close manager）能收尾；但
   * **自己主动关**的时候，`uv_close()` 只调用你传进去的那一个完成回调，读
   * 分支永远不会来 —— 于是 close manager 不被通知，客户端对象留在服务端的
   * 登记表里，永远不被摘除、永远不被 `delete`（句柄关了，对象和簿记还在）。
   * 每一个 `Connection: close` 的 HTTP 响应都会漏一个。
   *
   * 本函数把这个通知补上：关闭完成时依次跑「调用方的收尾」和框架的关闭
   * 回调（用户的 `set_on_close` + close manager），让**主动关闭**和**对端
   * 断开**在下游完全一致。
   *
   * @param after_close 关闭完成后的调用方收尾（比如 http 层要摘掉自己的
   *                    连接上下文）。**它排在框架的关闭回调之前**，所以那时
   *                    客户端对象还在，可以安全地读它的状态。可以为空。
   *
   * @return 0 成功发起关闭（含"已经在关闭中"）；`UV_EINVAL` 句柄已不存在。
   *
   * @warning 已经有人在关这条连接时（`is_closing()`），本函数只跑
   *          `after_close`，**不会**再触发关闭回调 —— 那个关闭事件的完成
   *          回调会负责收尾。硬要在这里再触发一次就会踩到正在关闭的句柄。
   *
   * @warning 关掉之后**不要**再持有这个指针：框架可能在完成回调里把它
   *          `delete` 掉（`uvcpp_tcp_server` 管理的连接就是这样）。
   */
  int close(const std::function<void()>& after_close = nullptr);

  /**
   * @brief Register a callback to be invoked when the connection closes.
   *
   * Fires on UV_EOF (graceful peer shutdown) or read error.
   *
   * @note 「对端断开」这件事**只有读得见**（`nread < 0` 分支）。所以这条
   *       回调生效的前提是有人在读：`read_start` / `read_start_events` /
   *       自动读。**一个不读的连接即使设了它也不会被触发** —— 对端关掉的
   *       那一刻，libuv 没有任何事件可投递。只想知道连接死活而不要数据的
   *       场合，请用 `read_start_events()` 注册一个忽略数据的回调。
   *
   * 这是**用户的通知回调**，纯粹只读观察：它不会、也不能影响框架对这个
   * 客户端的释放。以前它和框架的清理用的是同一个槽，于是"用户设了自己的
   * 回调"就会让框架放弃释放 —— 那是每连接泄漏一个 client、并且用户顺手
   * `delete` 时演成 double free 的成因。现在两者是**两个独立的槽**：
   * 用户的先触发（此时客户端还活着，可以读状态/对端信息），框架的后触发。
   *
   * @warning **不要在这里 `delete` 客户端** —— 除非你已经用
   *          `uvcpp_tcp_server::take_client()` 把所有权取走了。删除一个
   *          仍归框架管的客户端会让框架随后的清理二次释放它。
   */
  void set_on_close(std::function<void()> cb);

  /** @brief Remove any registered close callback. */
  void clear_on_close();

  // -----------------------------------------------------------------
  // 框架内部：关闭时的管理槽
  // -----------------------------------------------------------------

  /**
   * @brief 安装框架的关闭管理回调（**框架内部使用，使用者不要调**）。
   *
   * 和 `set_on_close()` 分开是有意的：用户的观察回调必须永远无法取消
   * 框架的释放，否则"设了回调就泄漏"这个缺陷会以另一种形式回来。
   */
  void set_close_manager(std::function<void()> cb);

  /** @brief 清掉框架的管理回调（`take_client()` 取走所有权时调用）。 */
  void clear_close_manager();

  /** @brief 是否装了框架的管理回调。 */
  bool has_close_manager() const;

  // -----------------------------------------------------------------
  // 关闭观察者：**加法式**，可以有任意多个
  // -----------------------------------------------------------------

  /**
   * @brief 追加一个"连接关闭了"的观察者，返回它的句柄（0 = 失败）。
   *
   * 与 `set_on_close()` 的区别是**加法式**：`set_on_close()` 是单槽，后装的
   * 顶掉先装的；本函数可以叠任意多个，互不影响。
   *
   * 为什么需要第三个槽：连接关闭有**三条**互相独立的路 —— 对端断开（读回调
   * 的 `nread < 0` 分支）、自己主动关（`close()` 的完成回调）、以及别人替你
   * 关（http 层的闲置超时、`uvcpp_tcp_server::stop()`）。前两条 `set_on_close`
   * 都能收到，第三条同样能收到，但**单槽意味着只能有一个收件人**：http 层已经
   * 占了用户槽、tcp_server 已经占了管理槽，于是上层（WebSocket 会话、静态服务
   * 的在途请求……）想知道"这条连接还在不在"就无处可挂 —— 除非去抢槽，那就变成
   * "谁后装谁赢"的静默失效。这跟 `install_client_manager` 当初把用户槽和管理槽
   * 拆开是同一个道理，这里是它的推广形式。
   *
   * 触发时机：在**用户槽之后、管理槽之前**。所以观察者跑的时候客户端对象还活着
   * （可以读状态、对端信息），但连接确实已经关了 —— **不要在这里写**。
   *
   * @warning 管理槽会 `delete` 本客户端，所以**不要在观察者里 `delete` 客户端**；
   *          也不要在观察者里 `remove_close_observer()` 别的观察者（那一批已经
   *          取出来了，删不掉；本次仍会跑到）。
   */
  int add_close_observer(std::function<void()> cb);

  /**
   * @brief 摘掉一个观察者。\p id 是 `add_close_observer()` 的返回值。
   *
   * 必须在**观察者自己失效之前**调：观察者通常捕获上层对象的 `this`，而上层
   * 对象（比如一个 WebSocket 会话）可能比这条连接先结束 —— 尤其是上层**主动**
   * 结束时（服务器停机逐个关会话就是这样），不摘掉就会在连接稍后关闭时回调到
   * 一个已释放的对象。
   */
  void remove_close_observer(int id);

  /** @brief 当前挂着几个关闭观察者。 */
  size_t close_observer_count() const;

  /**
   * @brief 析构时**包装对象最终归宿**的三条支路各走了多少次（诊断用）。
   *
   * 析构发生在事件循环的回调里时，"包装对象"（`uvcpp_tcp` 那个 wrapper）
   * 什么时候能释放取决于句柄处在哪一步，三条支路**对外行为完全一样** ——
   * 从外面看只是连接断了，看不出走的是哪一条，也看不出有没有走。这个计数
   * 把它变成可观测量，回归用例才有判据：
   *
   *   - `released`：句柄已摘（`get_handle() == nullptr`），当场释放；
   *   - `deferred`：句柄还活着，交给它自己的关闭完成回调去释放；
   *   - `skipped` ：句柄正在关闭、关闭回调槽被占，只能留下（唯一的真泄漏）。
   *
   * 统计对象是**本客户端在析构时要放掉的全部包装对象**：`tcp_`，以及
   * （`UVCPP_OPENSSL_ENABLE` 下）握手超时用的那个定时器 —— 后者只在 TLS
   * 握手超时那一条路上活到析构，非 TLS 路径上恒为 0。
   *
   * 计的是**释放真的做成了**，不是"打算这么做"：`released` 记在 `delete`
   * 之后、`deferred` 记在那个延迟的 `delete` 里面。所以把释放动作删掉（或退
   * 回不释放的老写法）会让计数**停在原地**，用例当场红 —— 不必等内存涨到能
   * 测出来。
   *
   * 判据是 `released + deferred` 随断开次数一起涨（`deferred` 要等收尾拨完
   * 才记上），且 `skipped` 恒为 0。
   *
   * 进程级累计，不随单个客户端归零。
   */
  struct reclaim_stat {
    uint64_t released;  ///< 句柄已摘：当场释放
    uint64_t deferred;  ///< 句柄还在：交给关闭完成回调，在那里释放
    uint64_t skipped;   ///< 关闭回调槽被占：只能留下（应当恒为 0）
  };

  /** @brief 读一次回收支路计数。见 `reclaim_stat`。 */
  static reclaim_stat reclaim_stats();

  /**
   * @brief `uv_try_write` 快速路径的诊断计数（诊断用）。
   *
   * 快速路径是**纯粹的性能改动**：打开它与关掉它，对外可观察行为**按设计**
   * 完全一样，所以黑盒用例本来就分不出二者 —— "快路径到底有没有被走到"因此
   * 没法用行为断言锁住，只能把它变成可观测量。这个计数就是那个抓手：
   *
   *   - `attempted`：长度过了门槛、真的试发了多少次；
   *   - `consumed` ：其中套接字**直接吃下 > 0 字节**的次数（真省下了拷贝）；
   *   - `skipped`  ：长度没过门槛、按设计没试发的次数；
   *   - `bytes`    ：直接吃下的字节总数（= 省掉的用户态拷贝字节数）。
   *
   * 判据是"小报文走 `skipped`、大报文走 `attempted`"（见
   * `tests/functional/tcp_try_write_fastpath_func.cpp` 判据 1c），开关关掉时
   * 四个计数**恒为 0**。只增不减。
   *
   * 进程级累计，不随单个客户端归零 —— 与 `reclaim_stat` 同一形状。
   */
  struct try_write_stat {
    uint64_t attempted;  ///< 过了门槛、真的试发过几次
    uint64_t consumed;   ///< 其中套接字直接吃下 > 0 字节的次数
    uint64_t skipped;    ///< 没过门槛、按设计没试发的次数
    uint64_t bytes;      ///< 直接吃下的字节总数（= 省掉的拷贝字节数）
  };

  /** @brief 读一次快速路径计数。见 `try_write_stat`。 */
  static try_write_stat try_write_stats();

  /**
   * @brief 试发快速路径的最小长度，见文件开头 `UVCPP_TRY_WRITE_MIN_BYTES`。
   *
   * 用例拿它来选"该落 `skipped` 的报文"和"该落 `attempted` 的报文"，这样
   * 改门槛时用例不用跟着改。
   */
  static const size_t kTryWriteMinBytes = UVCPP_TRY_WRITE_MIN_BYTES;

  // -----------------------------------------------------------------
  // 框架内部：自动读
  // -----------------------------------------------------------------

  /**
   * @brief 装上框架的自动读（**框架内部使用，使用者不要调**）。
   *
   * 存在的唯一理由是**发现断开**：断开只有在读回调里才看得见（`nread < 0`
   * 分支），不读的连接永远不会被回收，也不会走关闭回调。以前这件事要用户
   * 自己记得 `read_start()`，忘了就是每连接静默泄漏一个 client —— 现在是
   * 默认就装上。
   *
   * 数据**没有消费者**，所以自动读收到的数据会被丢弃；第一次丢的时候会往
   * stderr 打一条警告（只打一次，且带客户端指针），免得"我明明发了数据对端
   * 没反应"这种问题无声无息。
   *
   * 使用者随后调用 `read_start()` / `read_start_events()` 会自动把自动读顶
   * 掉（见 `release_auto_read`），所以在 `on_connection` 里照常注册自己的读
   * 回调即可，不需要先手动停。
   */
  void install_auto_read();

  /** @brief 卸掉自动读（用户注册自己的读回调时自动调用）。 */
  void release_auto_read();

  // -----------------------------------------------------------------
  // Loop control (async mode)
  // -----------------------------------------------------------------

  /** @brief Run the internal event loop (for async mode users). */
  int run(uv_run_mode md = UV_RUN_DEFAULT);

  /** @brief Stop the internal event loop. */
  void stop();

 private:
  // -----------------------------------------------------------------
  // Internal helpers
  // -----------------------------------------------------------------

  void set_status(int flags);
  void clear_status(int flags);
  void reset_status();

  /**
   * @brief Spin-wait with event-loop pumping until condition is true or
   *        timeout expires.
   * @return true if condition became true, false on timeout.
   */
  bool wait_for_condition(std::function<bool()> condition, int timeout_ms);

  /** @brief Ensure the internal read cache buffer is allocated. */
  void ensure_read_cache();

  /** @brief Arm the async read callback on the underlying stream. */
  int arm_async_read();

  /** @brief Static alloc callback forwarded to the client instance. */
  static void internal_alloc_cb(uvcpp_handle* h, size_t sz, uv_buf_t* buf);

  /** @brief Internal read callback registered when using sync reads. */
  void on_internal_read(uvcpp_stream* s, ssize_t nread, const uv_buf_t* buf);

  /**
   * @brief 用已解析的 sockaddr 发起异步连接（内部辅助）。
   *
   * 复用 trampoline 回调机制，将用户 cb 存入 connect_fn_/connect_arg_，
   * 随后调用 tcp_->connect。失败时清理状态并返回错误码（不触发 cb）。
   */
  int connect_async(const struct sockaddr* addr, std::function<void(int)> cb);

  /**
   * @brief 解析主机名（数字 IP 快速路径 + DNS 回退），阻塞等待结果。
   *
   * @param host 主机名或 IPv4 字符串。
   * @param port 端口。
   * @param[out] out 解析出的 sockaddr。
   * @return 0 成功；非 0 为 libuv 错误码（EINVAL/ETIMEDOUT/EAI_* 等）。
   */
  int resolve_host_sync(const char* host, int port, sockaddr_storage& out,
                        int timeout_ms);

  // -----------------------------------------------------------------
  // Member variables
  // -----------------------------------------------------------------

  uvcpp_loop* loop_ = nullptr;
  uvcpp_tcp*  tcp_  = nullptr;
  bool owns_loop_   = true;  ///< false for server-managed clients
  int status_       = TCP_CLIENT_NONE;
  int last_error_code_ = 0;

  // Read cache
  uvcpp_buf* read_cache_          = nullptr;
  size_t     max_read_cache_size_ = 2 * 1024 * 1024;  // 2 MB default
  bool       read_cache_paused_   = false;
  bool       read_started_        = false;  ///< true if tcp_->read_start was called
  /// 有调用方**真的**在走同步读（`read_start(nullptr)` / `read_wait`）。
  ///
  /// 不能用 `read_started_ && !has_async_read_cb_` 代替：TLS 握手阶段
  /// `enable_tls()` 会自己 arm 一次读，于是这个组合为真而**根本没人要同步读**
  /// —— 见 `tls_deliver_plain()` 里为什么要分清这两件事。
  bool       sync_read_wanted_    = false;

  // Mode tracking — prevent mixing sync/async
  bool has_async_connect_cb_ = false;
  bool has_async_read_cb_    = false;
  bool has_async_write_cb_   = false;

  // User async callbacks — stored as C-style fn+arg to completely avoid
  // std::function copy-chain issues through libuv's callback layers.
  using connect_callback_t = void(*)(int status, void* arg);
  using write_callback_t   = void(*)(int status, void* arg);
  using read_callback_t    = void(*)(uvcpp_buf* buf, void* arg);
  using close_callback_t   = void(*)(void* arg);

  connect_callback_t connect_fn_  = nullptr;
  void*              connect_arg_ = nullptr;
  write_callback_t   write_fn_    = nullptr;
  void*              write_arg_   = nullptr;

  // -------------------------------------------------------------------
  // 存活令牌 —— 异步完成回调不许踩已析构的对象
  // -------------------------------------------------------------------
  //
  // 完成回调是 libuv **稍后**送进来的，而本对象完全可能在那一刻之前就没了：
  // 对端断开（读回调的 `nread < 0` 分支会触发关闭回调，框架据此 delete 掉
  // 客户端）、上层收尾、连接被淘汰，任何一条路都会。回调里读 `write_fn_` /
  // `write_arg_` / `last_error_code_` 就是读已释放内存 —— 表现是 `call rax`
  // 到 0xFEEEFEEE 直接段错误，而且**只在堆恰好复用了那块内存时才崩**，其余
  // 时候静默地把别人的数据当成自己的，比崩溃更难查。
  //
  // 令牌按值捕进每个异步完成回调：对象活着时 `*token == 0`，析构时置 1。
  // 用 `shared_ptr` 是为了让回调手里那份副本在对象消失后仍然有效可读 ——
  // 回调唯一还能做的事就是把请求对象（连同它的缓冲区）还回去。
  //
  // 只挡「析构之后」这一种情况：对象还活着时行为与原来逐字节相同。
  ::std::shared_ptr<char> alive_token_;

  /** @brief 取存活令牌，必要时建立（每个客户端只建一次）。 */
  ::std::shared_ptr<char> alive_token();

  /** @brief 令牌是否表示「对象还活着」。空令牌视为不活着。 */
  static bool token_alive(const ::std::shared_ptr<char>& token);

  read_callback_t    read_fn_     = nullptr;
  void*              read_arg_    = nullptr;
  close_callback_t   close_fn_    = nullptr;
  void*              close_arg_   = nullptr;

  /// 框架层读回调（read_start_events 注册）。与 read_fn_ 互斥。
  uvcpp_net_read_cb  net_read_cb_;
  /// 读是否被 read_pause() 暂停（恢复时据此决定要不要重新 arm）。
  bool               net_read_paused_ = false;
  /// 当前 net_read_cb_ 是不是框架的自动读（见 install_auto_read）。
  bool               auto_read_installed_ = false;
  /// 自动读丢弃数据的警告是否已经打过（一个连接只打一次，别刷屏）。
  bool               auto_read_warned_ = false;

  /// 框架的管理槽（见 set_close_manager）。与 close_fn_ 分开存放。
  close_callback_t   close_mgr_fn_  = nullptr;
  void*              close_mgr_arg_ = nullptr;

  /// 关闭观察者表（见 add_close_observer）。{句柄, 回调}，句柄单调递增不复用。
  std::vector<std::pair<int, std::function<void()> > > close_observers_;
  /// 下一个观察者句柄。从 1 开始，0 专门表示"没有/失败"。
  int                next_close_observer_id_ = 1;

  /**
   * @brief 依次触发用户的、观察者的、框架的关闭回调。
   *
   * 抽成一个函数是因为有多处触发点（两个 read 回调里的 nread < 0 分支、
   * `close()` 的完成回调），各处必须走**完全一样**的槽位处理顺序，否则其中
   * 一个会漏掉新的置空逻辑。
   */
  void fire_close_callbacks();

  // Sync operation coordination
  bool sync_connect_done_   = false;
  int  sync_connect_result_ = 0;
  bool sync_write_done_     = false;
  int  sync_write_result_   = 0;

#if UVCPP_OPENSSL_ENABLE
  // -----------------------------------------------------------------
  // TLS 过滤层的内部状态
  // -----------------------------------------------------------------

  /** 推进一次握手；返回 0 正常，< 0 真出错（0 返回不是错误，见 handler 说明）。 */
  int  tls_drive_handshake();

  /** 把 `tls_out_` 里的密文投出去（同一时刻只允许一个在途写）。 */
  void tls_flush_out();

  /** 把 `tls_plain_` 交给当前注册的消费者；没有消费者就先留着。 */
  void tls_deliver_plain();

  /** 喂入 socket 收到的密文，推进握手并解出明文到 `tls_plain_`。 */
  int  tls_feed(const char* data, size_t len);

  /** 把整段明文交给 SSL_write 并抽出密文；返回 0 成功，非 0 为 libuv 错误码。 */
  int  tls_write_plain(const char* data, size_t len);

  /** 握手完成后的收尾：补调被推迟的 connect 回调。 */
  void tls_complete_connect();

  /**
   * 触发一次并清空 `tls_ready_cb_`。
   *
   * **调用点必须是所在回调的最后一句** —— 用户拿到这个通知后完全可能把连接
   * 关掉/释放（服务端拒绝一条握手完成的连接就是常规用法），之后再碰任何成员
   * 都是 use-after-free。
   */
  void tls_notify_ready(int status);

  /** 密文全部出网之后，收尾一个在等的应用层写（async 或 sync）。 */
  void tls_finish_write(int status);

  /** 握手/读写不可恢复地失败：通知在读的一方并走关闭流程。 */
  void tls_fail(int err, bool peer_closed);

  uvcpp_ssl*  tls_ssl_            = nullptr;
  bool        tls_handshake_done_ = false;
  int         tls_ssl_error_      = 0;   ///< 最近一次 SSL_get_error
  std::string tls_alpn_;                 ///< 握手协商出的协议名（空 = 没协商出）
  std::vector<std::string> tls_alpn_want_;  ///< 待宣告的协议名（enable_tls 时装上）
  std::string tls_out_;                  ///< 待发密文（wbio 抽出来的）
  bool        tls_out_busy_       = false;  ///< 有一个密文写在途
  std::string tls_plain_;                ///< 已解密、尚未交付的明文
  bool        tls_connect_pending_ = false;  ///< connect 回调推迟到握手之后
  bool        tls_write_pending_   = false;  ///< 有应用层写在等密文出网
  bool        tls_write_sync_      = false;  ///< 上面那个写走的是同步接口
  int         tls_write_status_    = 0;      ///< 上面那个写的最终状态
  std::function<void(uvcpp_tcp_client*, int)> tls_ready_cb_;  ///< 握手结束通知一次

  /** 起算握手超时。幂等：已经起过就不重起（每次读事件都会调一次）。 */
  void tls_arm_handshake_timer();
  /**
   * @brief 停掉握手超时**并释放句柄**。
   *
   * @warning **绝不能在定时器自己的回调里调它** —— 那等于在回调压栈期间把
   *          正在执行的那个闭包析构掉（与本文件析构注释里记的"三层悬垂"是
   *          同一类）。要停就从回调里直接 `tls_hs_timer_->stop()`，或者走
   *          `tls_fail()`（它只 stop、不释放）。
   */
  void tls_disarm_handshake_timer();

  /**
   * @brief 握手超时用的定时器。
   *
   * 惰性创建（第一次推进握手时），握手成功即释放 —— 正常路径上它只活几十
   * 微秒。只有"真的超时了"这一条路上它会活到连接被拆掉为止，那时析构会收掉。
   */
  uvcpp_timer* tls_hs_timer_      = nullptr;
  /** 握手超时毫秒数；0 = 不设。 */
  int          tls_hs_timeout_ms_ = 10000;
  /**
   * @brief 此刻是否正跑在握手超时的回调里。
   *
   * 只给析构看：超时回调会一路走到 `tls_fail()` → 拆连接 → 释放本对象，而
   * 那一切都在**这个回调的栈帧还在**的时候发生。析构据此决定 `tls_hs_timer_`
   * 是能安全 delete，还是只能 stop 之后留着（否则就是析构一个正在执行的回调）。
   */
  bool         tls_hs_in_cb_      = false;
#endif  // UVCPP_OPENSSL_ENABLE
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_TCP_CLIENT_H
