/**
 * @file src/net/uvcpp_udp_client.h
 * @brief Higher-level UDP client with async/sync dual-mode API built on uvcpp_udp.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides both callback-based (async) and blocking-wait (sync) operations
 * for UDP send and receive, with status tracking and address helpers.
 */

#pragma once
#ifndef SRC_NET_UVCPP_UDP_CLIENT_H
#define SRC_NET_UVCPP_UDP_CLIENT_H

#include <functional>
#include <memory>
#include <string>
#include <uv.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_udp.h>
#include <uvcpp/uvcpp_buf.h>

#include <vector>

namespace uvcpp {

/**
 * @brief Bitmask status flags for UDP client lifecycle.
 */
enum uvcpp_udp_client_status : int {
  UDP_CLIENT_NONE      = 0x00,  ///< Initial state
  UDP_CLIENT_BOUND     = 0x01,  ///< Bound to local address
  UDP_CLIENT_CONNECTED = 0x02,  ///< Connected to remote peer
  UDP_CLIENT_READABLE  = 0x04,  ///< Data available in recv cache
  UDP_CLIENT_CLOSING   = 0x10,  ///< Close in progress
  UDP_CLIENT_CLOSED    = 0x20,  ///< Handle closed
  UDP_CLIENT_ERROR     = 0x40   ///< Error occurred (see last_error_code)
};

/**
 * @brief Application-layer UDP client wrapping uvcpp_udp.
 *
 * Dual-mode API:
 * - async:  provide a callback; the method returns immediately and the
 *           callback fires on the internal event loop when complete.
 * - sync:   omit the callback (or use the *_wait variants); the method
 *           blocks up to timeout_ms (default 30000) pumping the loop.
 *
 * Send supports two forms:
 * - With (ip, port): send to a specific address each time.
 * - Without address: send to a previously connected peer (requires
 *   prior connect() call; libuv >= 1.27).
 *
 * Usage (sync):
 * @code
 *   uvcpp_udp_client client;
 *   client.bind("0.0.0.0", 0);
 *   client.send_wait("127.0.0.1", 9999, "hello", 5);
 *   uvcpp_buf buf;
 *   client.recv_wait(buf);
 * @endcode
 *
 * Usage (async):
 * @code
 *   uvcpp_udp_client client;
 *   client.bind("0.0.0.0", 0);
 *   client.recv_start([](uvcpp_buf* b, const char* ip, int port) {
 *     // handle received data
 *   });
 *   client.send("127.0.0.1", 9999, "ping", 4, [](int s) { ... });
 *   client.run(UV_RUN_DEFAULT);
 * @endcode
 */
class UVCPP_API uvcpp_udp_client {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_udp_client)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_udp_client)

  /**
   * @brief 共用一个**外部**事件循环：本对象不拥有它，析构时不关不删。
   *
   * 与 `uvcpp_tcp_client(uvcpp_loop*)` 同一个形状，给"一个循环上挂多个
   * 客户端"的场景用（SOCKS5 的 UDP 中继、服务端每条连接自己的收尾）。
   * 句柄仍然归本对象所有：析构时该关的照关，只是不去动循环。
   */
  explicit uvcpp_udp_client(uvcpp_loop* external_loop);

  // -----------------------------------------------------------------
  // Accessors
  // -----------------------------------------------------------------

  /** @brief Return the underlying uvcpp_udp handle. */
  uvcpp_udp* get_udp();

  /** @brief Return the internal event loop. */
  uvcpp_loop* get_loop();

  /** @brief Return the current status bitmask. */
  int get_status() const;

  /** @brief Check whether all given status flags are set. */
  bool has_status(int flags) const;

  /** @brief Return the last recorded error code (libuv errno). */
  int get_last_error() const;

  // -----------------------------------------------------------------
  // Address helpers
  // -----------------------------------------------------------------

  /**
   * @brief Retrieve the local socket address.
   * @param[out] ip   Receives the IP string.
   * @param[out] port Receives the port number in host byte order.
   * @return The raw sockaddr_storage.
   */
  sockaddr_storage getLocalAddrs(std::string& ip, int& port);

  /**
   * @brief Retrieve the remote (peer) socket address.
   * @return The raw sockaddr_storage.
   */
  sockaddr_storage getPeerAddrs(std::string& ip, int& port);

  // -----------------------------------------------------------------
  // Bind / Connect
  // -----------------------------------------------------------------

  /**
   * @brief Bind to local address (auto-detect IPv4/IPv6).
   *
   * `ip` 只接受**数字地址**（`0.0.0.0`、`::` 等）；域名返回 `UV_EINVAL`。
   * 与 `send` / `connect` 不同，本地绑定不引入 DNS 依赖。
   */
  int bind(const char* ip, int port);

  /** @brief Bind to IPv4 address. */
  int bindIpv4(const char* ip, int port);

  /** @brief Bind to IPv6 address. */
  int bindIpv6(const char* ip, int port);

  /**
   * @brief "Connect" to a remote peer (libuv >= 1.27).
   *
   * After connecting, send() can be called without specifying an
   * address.  This does NOT establish a real connection — it just
   * sets the default destination for packets.
   *
   * `ip` 可以是数字地址，也可以是**主机名**：域名走 `AF_UNSPEC` 解析
   * （A + AAAA），取第一个可用结果 —— 这条是同步接口，DNS 也同步等待。
   */
  int connect(const char* ip, int port);

  // -----------------------------------------------------------------
  // Send (to specific address)
  // -----------------------------------------------------------------

  /**
   * @brief 发到 `host:port`。`host` 可以是数字地址，也可以是主机名。
   *
   * 域名用 `AF_UNSPEC` 解析（A + AAAA），取第一个可用结果。异步重载在
   * **事件循环上**解析、绝不阻塞调用线程；`send_wait` 同步等待解析。
   * DNS 失败返回可区分的 `UV_EAI_*`。
   */
  int send(const char* ip, int port, const char* data, size_t len,
           std::function<void(int)> cb = nullptr);
  int send_wait(const char* ip, int port, const char* data, size_t len,
                int timeout_ms = 30000);
  int send(const char* ip, int port, const uvcpp_buf& buf,
           std::function<void(int)> cb = nullptr);
  int send(const char* ip, int port, uvcpp_buf* buf,
           std::function<void(int)> cb = nullptr);

  /**
   * @brief 发到**已经解析好的** `sockaddr`（`AF_INET` / `AF_INET6` 都接受）。
   *
   * 给 SOCKS5 UDP 中继这类"手里已经是 `sockaddr_storage`"的调用方用：
   * 转回字符串再解析一遍既多一次失败点，也表达不了"解析结果跨地址族"这件事。
   * 地址在调用期间被 libuv 拷走，栈上的对象可以随用随失。
   *
   * 在途契约与其它 `send` 重载相同：同一时刻只允许一笔异步发。
   */
  int send_addr(const struct sockaddr* addr, const char* data, size_t len,
                std::function<void(int)> cb = nullptr);
  int send_addr_wait(const struct sockaddr* addr, const char* data, size_t len,
                     int timeout_ms = 30000);

  // -----------------------------------------------------------------
  // Send (to connected peer — requires prior connect())
  // -----------------------------------------------------------------

  int send(const char* data, size_t len,
           std::function<void(int)> cb = nullptr);
  int send_wait(const char* data, size_t len, int timeout_ms = 30000);
  int send(const uvcpp_buf& buf, std::function<void(int)> cb = nullptr);
  int send(uvcpp_buf* buf, std::function<void(int)> cb = nullptr);

  // -----------------------------------------------------------------
  // Receive
  // -----------------------------------------------------------------

  /**
   * @brief Start receiving datagrams.
   *
   * Async mode (cb != nullptr): cb(buf, sender_ip, sender_port) is
   * called for every received datagram.
   *
   * Sync mode (cb == nullptr): enables the internal recv cache for
   * subsequent recv_wait() calls.
   */
  int recv_start(std::function<void(uvcpp_buf*, const char*, int)> cb = nullptr);

  /** @brief Synchronously wait for and return received data. */
  int recv_wait(uvcpp_buf& out_buf, int timeout_ms = 30000);

  /** @brief Stop receiving datagrams. */
  int recv_stop();

  // -----------------------------------------------------------------
  // Read cache
  // -----------------------------------------------------------------

  void set_max_read_cache_size(size_t max_size);
  size_t get_max_read_cache_size() const;

  // -----------------------------------------------------------------
  // Loop control
  // -----------------------------------------------------------------

  int run(uv_run_mode md = UV_RUN_DEFAULT);
  void stop();

 private:
  /**
   * @brief 把 host 解析成候选发送地址（阻塞；数字字面量直接成功）。
   *
   * 主机名用 AF_UNSPEC：A 与 AAAA 都收，顺序即 getaddrinfo 的顺序。数字
   * 字面量返回单元素列表、不触网。失败返回可区分的错误（DNS 是 `UV_EAI_*`）。
   *
   * 返回**全部**候选而不是第一个：socket 的地址族在 `bind()` 时就定了，
   * `localhost` 这类双栈名字的第一个结果可能属于另一个地址族，直接发会
   * `EAFNOSUPPORT` —— 调用方按顺序试到 `uv_udp_send` / `uv_udp_connect`
   * 接受为止。
   */
  int resolve_send_addrs(const char* host, int port,
                         std::vector<sockaddr_storage>& out, int timeout_ms);

  /**
   * @brief 从候选里挑一个与**当前 socket 地址族**一致的目标。
   *
   * `bind()` / 首次发送会把 socket 的地址族定下来，之后不能换：双栈名字
   * （`localhost` → `::1` + `127.0.0.1`）的第一个候选未必是这一个族。
   * 未绑定的句柄（`getsockname` 失败）取第一个，由 libuv 的延迟绑定决定。
   * 一个都匹配不上返回 `UV_EAFNOSUPPORT`（DNS 本身是成功的）。
   */
  int pick_send_addr(const std::vector<sockaddr_storage>& addrs,
                     sockaddr_storage& out) const;

  /**
   * @brief 异步发送的公共入口：数字地址直接发，域名先异步解析再发。
   *
   * `cb` 是请求完成回调（由调用方拼好）；DNS 失败时它以错误码落地，
   * 并负责释放 `w`（不经过 libuv 的请求完成路径）。
   */
  int start_send_to(const char* host, int port, uv_buf_t* raw,
                    uvcpp_udp_send* w,
                    std::function<void(uvcpp_udp_send*, int)> cb);
  void set_status(int flags);
  void clear_status(int flags);

  bool wait_for_condition(std::function<bool()> condition, int timeout_ms);
  void ensure_read_cache();

  /** @brief Resolve ip:port to sockaddr_in (IPv4). */
  static bool resolve_addr(const char* ip, int port, struct sockaddr_in& addr);

  // Internal alloc/recv callbacks
  static void internal_alloc_cb(uvcpp_handle* h, size_t sz, uv_buf_t* buf);
  void on_internal_recv(uvcpp_udp* u, ssize_t nread, const uv_buf_t* buf,
                        const struct sockaddr* addr, unsigned int flags);

  // -----------------------------------------------------------------
  // 存活令牌（与 `uvcpp_tcp_client` 同一套，见那里的长篇说明）
  // -----------------------------------------------------------------
  //
  // 完成回调原先捕获裸 `this`：libuv 在对象析构之后仍可能把在飞的完成回调送
  // 进来，那时回调去读 `send_fn_` / `send_arg_` / `last_error_code_` 就是读已
  // 释放内存。令牌按值捕进每个异步完成回调：对象活着时 `*token == 0`，析构时
  // 置 1。用 `shared_ptr` 是为了让回调手里那份副本在对象消失后仍然有效可读。
  //
  // 只挡「析构之后」这一种情况：对象还活着时行为与原来逐字节相同。

  /** @brief 取存活令牌，必要时建立（每个客户端只建一次）。 */
  ::std::shared_ptr<char> alive_token();

  /** @brief 令牌是否表示「对象还活着」。空令牌视为不活着。 */
  static bool token_alive(const ::std::shared_ptr<char>& token);

  ::std::shared_ptr<char> alive_token_;

  // Trampolines (free functions in .cpp, not class members)
  using send_callback_t = void(*)(int status, void* arg);
  using recv_callback_t  = void(*)(uvcpp_buf* buf, const char* ip,
                                   int port, void* arg);

  // -----------------------------------------------------------------
  // Member variables
  // -----------------------------------------------------------------

  uvcpp_loop* loop_ = nullptr;
  uvcpp_udp*  udp_  = nullptr;
  /// 循环是不是自己建的：外部循环那条路上析构**不能**关它、更不能删它。
  bool owns_loop_ = true;
  int status_       = UDP_CLIENT_NONE;
  int last_error_code_ = 0;

  // Read cache
  uvcpp_buf* read_cache_          = nullptr;
  size_t     max_read_cache_size_ = 2 * 1024 * 1024;
  bool       recv_started_        = false;

  // Mode tracking
  bool has_async_send_cb_  = false;
  bool has_async_recv_cb_  = false;

  // Async callbacks (trampoline pattern)
  send_callback_t send_fn_  = nullptr;
  void*           send_arg_ = nullptr;
  recv_callback_t recv_fn_  = nullptr;
  void*           recv_arg_ = nullptr;

  // Sync coordination
  bool sync_send_done_   = false;
  int  sync_send_result_ = 0;
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_UDP_CLIENT_H
