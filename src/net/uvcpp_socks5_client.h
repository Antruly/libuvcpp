/**
 * @file src/net/uvcpp_socks5_client.h
 * @brief 独立的 SOCKS5 客户端：CONNECT / BIND / UDP ASSOCIATE 三种命令全支持。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 与 `uvcpp_tcp_client::set_socks5_proxy()` 的分工：
 * - 那条路只做 CONNECT（HTTP/WS 客户端要的就是它），走的是"连上代理之后
 *   悄悄把目标换掉"的透明形状；
 * - 本类把三种命令都暴露出来，尤其是 BIND（两次应答）与 UDP ASSOCIATE
 *   （控制连接 + UDP 数据面），这两件事塞不进"一次 connect 一次回调"的形状。
 *
 * 用法（异步，CONNECT）：
 * @code
 *   uvcpp_socks5_client c(loop);
 *   c.set_credentials("user", "pass");           // 需要 RFC 1929 认证时
 *   c.connect("127.0.0.1", 1080, [](int st) {
 *     if (st != 0) return;
 *     c.connect_target("example.com", 80, [](int st2) {
 *       if (st2 != 0) return;
 *       c.read_start([](uvcpp_buf* b) { ... });
 *       c.write("GET / HTTP/1.1\r\n\r\n", 18);
 *     });
 *   });
 * @endcode
 *
 * 同步版本是 `*_wait`；**不要**在事件循环的回调里调同步版本（它会泵循环）。
 *
 * 读的两点约定（与 `uvcpp_tcp_client` 不同）：
 * - 控制连接上的读**始终由本对象持有** —— 握手完毕之后到达的应用字节由
 *   `read_start()` 交付；`stream()->read_start()` 会拿到 `UV_EALREADY`。
 *   这是有意为之：握手与应用数据共用同一条字节流，谁抢到谁就丢。
 * - `read_start()` 注册之前到达的字节会先攒着（上限 2 MiB），注册时补投，
 *   所以"服务端在应答包里夹带应用数据"不会丢。
 *
 * UDP 关联的生命周期绑在控制 TCP 上（RFC 1928 §7）：控制连接一关，本对象
 * 自动关掉它持有的 UDP socket。使用者不需要（也不应该）自己删 `udp()`。
 */

#pragma once
#ifndef SRC_NET_UVCPP_SOCKS5_CLIENT_H
#define SRC_NET_UVCPP_SOCKS5_CLIENT_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <net/uvcpp_socks5_types.h>

namespace uvcpp {

class uvcpp_buf;
class uvcpp_loop;
class uvcpp_socks5_udp;
class uvcpp_tcp_client;
class uvcpp_timer;

/**
 * @brief 独立 SOCKS5 客户端（CONNECT / BIND / UDP ASSOCIATE）。
 *
 * 生命周期与线程：一个对象对应一条到代理的控制 TCP 连接。所有回调都在
 * `get_loop()` 那条循环上触发。
 */
class UVCPP_API uvcpp_socks5_client {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_socks5_client)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_socks5_client)

  /**
   * @brief 用一条外部事件循环；本对象**不拥有**它。
   *
   * 与 `uvcpp_tcp_client(uvcpp_loop*)` 同一个形状。同步 API 会泵这条循环。
   */
  explicit uvcpp_socks5_client(uvcpp_loop* loop);

  /** @brief 本对象跑在哪条循环上。 */
  uvcpp_loop* get_loop();

  // -----------------------------------------------------------------
  // 配置（握手开始前设置）
  // -----------------------------------------------------------------

  /**
   * @brief RFC 1929 用户名密码。两者都空 = 只提供 no-auth 一种方法。
   *
   * 与 `uvcpp_tcp_client` 同一条规则：**只提供一种方法**。用户名非空而
   * 密码为空是合法的（RFC 1929 允许空密码）。
   */
  void set_credentials(const std::string& username,
                       const std::string& password);

  /**
   * @brief 单个操作的总超时（毫秒，默认 30000；0 = 不超时）。
   *
   * 覆盖"连代理 + 问候 + 认证 + 请求 + 第一次应答"整段。BIND 的第二次应答
   * 由 `bind_accept_timeout` 那个入口各自计时（见 `bind()`）。
   */
  void set_connect_timeout_ms(int ms);

  // -----------------------------------------------------------------
  // CONNECT
  // -----------------------------------------------------------------

  /**
   * @brief 连代理并完成问候/认证；**不发**任何命令请求。
   *
   * 完成之后 `connect_target()`（CONNECT）、`bind()`、`udp_associate()`
   * 三选一。`cb == nullptr` 等价于 `connect_wait(host, port, 默认超时)`。
   *
   * 失败（连不上/方法不可接受/认证失败/协议错/超时）时回调非 0，且控制
   * 连接会被关掉；`last_error()` 给出人类可读原因。
   */
  int connect(const std::string& proxy_host, int proxy_port,
              std::function<void(int)> cb = nullptr);

  int connect_wait(const std::string& proxy_host, int proxy_port,
                   int timeout_ms = 30000);

  /**
   * @brief 在已协商好的控制连接上发 CONNECT 请求并把隧道接通。
   *
   * **必须**先 `connect()` / `connect_wait()` 成功（否则 `UV_EBUSY`）。
   * 目标主机**不在本地解析**：域名以 `ATYP=DOMAIN` 交给代理。
   *
   * 成功后控制连接就是隧道：用 `read_start()` 读、`write()` 写。
   */
  int connect_target(const std::string& host, int port,
                     std::function<void(int)> cb);

  int connect_target_wait(const std::string& host, int port,
                          int timeout_ms = 30000);

  // -----------------------------------------------------------------
  // BIND（RFC 1928 §6）
  // -----------------------------------------------------------------

  /**
   * @brief 请求代理在 \p host : \p port 上等待一个入站连接。
   *
   * 两次应答：
   * 1. 代理完成绑定后回 `BND.ADDR/BND.PORT` —— 交给 \p on_reply1；
   * 2. 对端连进来之后，代理再回一次，内容是**对端地址** —— 交给 \p on_peer。
   *
   * 第二次应答到达之后控制连接就变成数据通道（与 CONNECT 一样用
   * `read_start()` / `write()`）。\p on_peer 可以为空（那时第二次应答只把
   * 状态从 `BIND_WAIT` 推回就绪，不通知任何东西 —— 同步路径用
   * `wait_for_peer()`）。
   *
   * 前提：`connect()` 已成功；本对象上没有任何命令在飞（`UV_EBUSY`）。
   */
  int bind(const std::string& host, int port,
           std::function<void(int, const uvcpp_socks5_endpoint&)> on_reply1,
           std::function<void(int, const uvcpp_socks5_endpoint&)> on_peer =
               nullptr);

  /**
   * @brief 同步等到**第一次**应答（代理绑定成功）；只交付绑定地址。
   *
   * 第二次应答（对端连入）请用 `wait_for_peer()` 接。
   */
  int bind_wait(const std::string& host, int port,
                uvcpp_socks5_endpoint& out_reply1, int timeout_ms = 120000);

  /**
   * @brief 同步等 BIND 的**第二次**应答（对端地址）。
   *
   * 只对 `bind_wait()` 启动的 BIND 有意义：异步 `bind()` 用 `on_peer` 回调。
   * 已经收到过第二次应答时立即返回它（结果留在 \p out_peer）。
   */
  int wait_for_peer(uvcpp_socks5_endpoint& out_peer, int timeout_ms = 120000);

  // -----------------------------------------------------------------
  // UDP ASSOCIATE（RFC 1928 §7）
  // -----------------------------------------------------------------

  /**
   * @brief 建立 UDP 关联。
   *
   * 步骤：先在本地绑一个 UDP socket（`bind_host:bind_port`，端口 0 = 系统选），
   * 把它的地址放进请求；代理回 `BND.ADDR/BND.PORT` 之后：
   * - 若回的是通配地址（`0.0.0.0` / `::`，有的代理就这么回），替换成**代理
   *   自己的地址**（控制连接的远端地址）—— 不替换的话数据报会发到本机；
   * - 把最终中继地址交给 `udp()`，之后用 `udp()->send_to()` /
   *   `udp()->recv_start()` 收发。
   *
   * `cb(status, relay)` 里的 `relay` 就是最终的中继地址。
   */
  int udp_associate(std::function<void(int, const uvcpp_socks5_endpoint&)> cb,
                    const std::string& bind_host = "0.0.0.0",
                    int bind_port = 0);

  int udp_associate_wait(uvcpp_socks5_endpoint& relay,
                         int timeout_ms = 30000,
                         const std::string& bind_host = "0.0.0.0",
                         int bind_port = 0);

  // -----------------------------------------------------------------
  // 隧道读写（CONNECT / BIND 成功之后）
  // -----------------------------------------------------------------

  /**
   * @brief 注册应用数据消费者。
   *
   * 注册之前到达的字节会补投（见文件头）。同一个对象只能注册一次；要换
   * 消费者先 `clear_read_callback()`。
   */
  int read_start(std::function<void(uvcpp_buf*)> cb);

  /** @brief 摘掉应用数据消费者；之后到达的字节被丢弃（不攒）。 */
  void clear_read_callback();

  /** @brief 写隧道（异步）。透传给控制连接。 */
  int write(const char* data, std::size_t len,
            std::function<void(int)> cb = nullptr);

  /** @brief 写隧道（同步，默认 30 s）。 */
  int write_wait(const char* data, std::size_t len, int timeout_ms = 30000);

  // -----------------------------------------------------------------
  // 访问器 / 收尾
  // -----------------------------------------------------------------

  /** @brief 控制连接的裸客户端（写、地址查询、状态）。读请走 `read_start`。 */
  uvcpp_tcp_client* stream();

  /** @brief UDP 数据面；只在 `udp_associate()` 成功之后非空。归本对象所有。 */
  uvcpp_socks5_udp* udp();

  /** @brief 当前是否有一条已协商好的控制连接（含已进入隧道）。 */
  bool is_connected() const;

  /** @brief 主动关闭：先关 UDP（若有），再关控制连接。 */
  void close();

  /** @brief 最近一次失败的人类可读原因（成功后不清空，便于排查）。 */
  const std::string& last_error() const;

  /** @brief 最近一次失败的 libuv 错误码；成功时为 0。 */
  int last_error_code() const;

 private:
  /** @brief 控制连接的阶段。 */
  enum class phase {
    IDLE,        ///< 没有连接 / 已关闭
    CONNECTING,  ///< TCP 正在连代理
    GREETING,    ///< 已发问候，等方法选择
    AUTH,        ///< 已发认证，等认证结果
    REQUEST,     ///< 已发命令请求，等第一次应答
    BIND_WAIT,   ///< BIND 第一次应答已到，等第二次（对端）
    READY,       ///< 已协商好，可以发命令或收发隧道数据
    FAILED
  };

  /** @brief 正在进行的命令。 */
  enum class op { NONE, PROXY, TARGET, BIND, UDP };

  void reset_for_new_connect();
  int  start_connect(const std::string& host, int port);
  void on_tcp_connected(int status);
  void on_control_closed();
  void on_read(uvcpp_buf* buf);
  void drive();
  void deliver_app(const char* data, std::size_t len);
  void recreate_tcp();

  int  send_handshake(const std::string& bytes);
  int  begin_request(uvcpp_socks5_command cmd,
                     const uvcpp_socks5_endpoint& target, op kind);

  void complete_op(int status, const uvcpp_socks5_endpoint& ep);
  void fail(int code, const std::string& message, bool close_control);
  void fail(int code) { fail(code, std::string(), true); }

  bool pump_until(std::function<bool()> done, int timeout_ms);
  int  run_sync_op(int timeout_ms);

  void arm_timeout(int ms);
  void disarm_timeout();
  void on_timeout();
  void destroy_timer();

  /** @brief 存活令牌（与 tcp/udp 客户端同一套）：析构后回调不许再碰 `this`。 */
  std::shared_ptr<char> alive_token();
  static bool token_alive(const std::shared_ptr<char>& token);
  std::shared_ptr<char> alive_token_;

  uvcpp_tcp_client* tcp_;
  uvcpp_socks5_udp* udp_;
  uvcpp_loop*       loop_;
  bool              owns_loop_;
  bool              tcp_closed_;
  uvcpp_timer*      timeout_timer_;
  bool              timeout_in_cb_;
  int               close_observer_id_;

  std::string username_;
  std::string password_;
  int         connect_timeout_ms_;

  phase phase_;
  op    op_;

  /** 当前这次操作是不是同步调用（同步时**不**触发用户回调，由泵循环取结果）。 */
  bool sync_;

  bool                   op_done_;
  int                    op_status_;
  uvcpp_socks5_endpoint  op_ep_;

  std::function<void(int)>                               op_cb_;
  std::function<void(int, const uvcpp_socks5_endpoint&)> op_ep_cb_;

  /** BIND 第二次应答的异步回调。 */
  std::function<void(int, const uvcpp_socks5_endpoint&)> peer_cb_;
  bool                  peer_done_;
  int                   peer_status_;
  uvcpp_socks5_endpoint peer_ep_;
  bool                  peer_sync_;

  /** 握手期间的入站累积。 */
  std::string in_;
  /** 握手之后、消费者出现之前的应用字节。 */
  std::string stash_;
  std::function<void(uvcpp_buf*)> read_cb_;

  std::string last_error_;
  int         last_error_code_;
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_SOCKS5_CLIENT_H
