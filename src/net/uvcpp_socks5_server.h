/**
 * @file src/net/uvcpp_socks5_server.h
 * @brief SOCKS5 服务端：CONNECT / BIND / UDP ASSOCIATE 三种命令。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 建立在 `uvcpp_tcp_server` 之上的协议层：每条控制连接一个状态机
 * （GREETING → AUTH? → REQUEST → 转发 / 等待对端 / UDP 中继），上游连接、
 * BIND 监听器、UDP socket 全部建在**连接自己那条循环**上，所以
 * `set_loops(n > 1)` 时整条链路天然分片，不需要跨线程碰句柄。
 *
 * @warning **默认配置就是开放代理**：`allow_no_auth = true`、不限制目标。
 *          绑 `0.0.0.0` 之前先想清楚 —— 生产上至少装 `authenticator` 或
 *          `allow_target`，示例都绑 `127.0.0.1`。
 *
 * 用法：
 * @code
 *   uvcpp_socks5_server srv;
 *   srv.bind("127.0.0.1", 1080);
 *   srv.listen([](const uvcpp_socks5_session_info& info) {
 *     // info.status == 0 表示这条会话已按 info.command 建立
 *   });
 *   srv.run(UV_RUN_DEFAULT);
 * @endcode
 *
 * 线程模型与 `uvcpp_tcp_server` 完全一致：`bind()` / `listen()` / `close()`
 * 在服务端自己的循环线程上调用；会话回调在**会话自己那条循环**上触发。
 */

#pragma once
#ifndef SRC_NET_UVCPP_SOCKS5_SERVER_H
#define SRC_NET_UVCPP_SOCKS5_SERVER_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

#include <uv.h>

#include <net/uvcpp_socks5_types.h>

namespace uvcpp {

class uvcpp_loop;
class uvcpp_tcp_server;

/**
 * @brief 服务端策略（`listen()` 时快照，之后改不影响已接受的连接）。
 */
struct uvcpp_socks5_server_options {
  uvcpp_socks5_server_options()
      : allow_no_auth(true),
        allow_username_password(false),
        allow_connect(true),
        allow_bind(true),
        allow_udp_associate(true),
        handshake_timeout_ms(30000),
        upstream_connect_timeout_ms(30000),
        bind_accept_timeout_ms(120000),
        udp_idle_timeout_ms(0),
        max_connections(0) {}

  /** @brief 接受 no-auth。关掉之后客户端只能走用户名密码。 */
  bool allow_no_auth;

  /**
   * @brief 接受 RFC 1929 用户名密码。
   *
   * 必须同时给 `authenticator`：没有校验函数时这个开关**不会**把方法选给
   * 客户端（否则等于"任何密码都对"）。
   */
  bool allow_username_password;

  /** @brief 用户名密码校验。返回 true = 通过。 */
  std::function<bool(const std::string&, const std::string&)> authenticator;

  bool allow_connect;
  bool allow_bind;
  bool allow_udp_associate;

  /**
   * @brief 目标白名单；空 = 不限制。返回 false 时回 `REP=NOT_ALLOWED`。
   *
   * 域名目标**原样**交给它（不在服务端替调用方解析）；`port` 是主机序。
   */
  std::function<bool(const uvcpp_socks5_endpoint&)> allow_target;

  /** @brief 从接受到命令建立的总时限（毫秒；0 = 不限）。 */
  int handshake_timeout_ms;

  /** @brief CONNECT 连上游的时限（毫秒；0 = 用 libuv 默认）。 */
  int upstream_connect_timeout_ms;

  /** @brief BIND 等对端连入的时限（毫秒；0 = 不限）。 */
  int bind_accept_timeout_ms;

  /**
   * @brief UDP 关联的空闲超时（毫秒；0 = 不超时，RFC 1928 §7 的推荐做法）。
   *
   * 有流量时重新计时；超时即关闭控制连接（关联随之结束）。
   */
  int udp_idle_timeout_ms;

  /** @brief 同时活跃的会话上限（0 = 不限）。超了在 accept 之后立刻关。 */
  std::size_t max_connections;
};

/** @brief 一条会话的最终结果。 */
struct uvcpp_socks5_session_info {
  uvcpp_socks5_session_info()
      : command(uvcpp_socks5_command::CONNECT),
        target(),
        peer(),
        status(0) {}

  /** @brief 客户端请求的命令。 */
  uvcpp_socks5_command command;

  /** @brief 请求里的目标地址（CONNECT 的是远端，BIND/UDP 是提示地址）。 */
  uvcpp_socks5_endpoint target;

  /** @brief 客户端那条控制连接的远端地址。 */
  uvcpp_socks5_endpoint peer;

  /**
   * @brief 0 = 会话已建立；非 0 = libuv 错误码（请求被拒时是 `UV_EACCES`
   *        或 `UV_EPROTO`，目标不可达时是连上游的错误）。
   */
  int status;
};

/**
 * @brief 一条会话结束时的累计统计。
 *
 * 字节数只统计**转发载荷**（CONNECT/BIND 两个方向分开）；UDP 关联把两个
 * 方向的数据报合在一起统计。握手阶段的协议字节不计入。
 */
struct uvcpp_socks5_session_stats {
  uvcpp_socks5_session_stats()
      : bytes_client_to_target(0),
        bytes_target_to_client(0),
        udp_datagrams(0),
        udp_bytes(0),
        started_ms(0),
        ended_ms(0),
        duration_ms(0),
        end_status(0) {}

  /** @brief 客户端 → 目标（CONNECT / BIND）的字节数。 */
  std::uint64_t bytes_client_to_target;
  /** @brief 目标 → 客户端（CONNECT / BIND）的字节数。 */
  std::uint64_t bytes_target_to_client;
  /** @brief UDP 关联处理过的数据报数（两个方向合计）。 */
  std::uint64_t udp_datagrams;
  /** @brief UDP 关联处理过的载荷字节数（两个方向合计）。 */
  std::uint64_t udp_bytes;
  /** @brief 会话开始时间（Unix epoch 毫秒）。 */
  std::int64_t started_ms;
  /** @brief 会话结束时间（Unix epoch 毫秒）。 */
  std::int64_t ended_ms;
  /** @brief 持续时长（毫秒）。 */
  std::int64_t duration_ms;
  /**
   * @brief 结束原因：0 = 正常关闭；非 0 是 libuv 错误码（握手被拒、读/写错误、
   *        空闲超时等）。
   */
  int end_status;
};

/**
 * @brief 一条会话结束时的通知（恰好一次）。
 *
 * 触发时机：对端关闭、读/写错误、服务端 `close()`、握手失败都覆盖。
 * 在**会话自己那条循环**上调用；不持有服务端任何锁（服务端没有锁）。
 * 回调里可以安全地调用 `uvcpp_socks5_server` 的访问器。
 */
using uvcpp_socks5_session_end_cb =
    std::function<void(const uvcpp_socks5_session_info&,
                       const uvcpp_socks5_session_stats&)>;

/**
 * @brief SOCKS5 服务端。
 *
 * 生命周期：本对象**不拥有**会话对象 —— 每条连接的状态机由框架在连接关闭时
 * 自动收掉；`close()` 之后不会再接受新连接，已建立的会话按各自的节奏结束。
 */
class UVCPP_API uvcpp_socks5_server {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_socks5_server)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_socks5_server)

  /**
   * @brief 自建事件循环（与 `uvcpp_tcp_server` 同一条约定：本对象拥有它）。
   *
   * 要把它跑起来，用 `run()` 或 `get_loop()->run()`；本对象析构时收掉循环。
   * 构造/析构由 `UVCPP_DEFINE_FUNC` 展开，不在类体里重复声明。
   */

  /** @brief 本对象（接受者）跑在哪条循环上。 */
  uvcpp_loop* get_loop();

  /** @brief 泵本对象自己的循环（转发给内部 `uvcpp_tcp_server::run`）。 */
  int run(uv_run_mode md = UV_RUN_DEFAULT);

  /** @brief 设置策略。必须在 `listen()` 之前调用；`listen()` 时快照。 */
  int set_options(const uvcpp_socks5_server_options& opts);

  /** @brief 绑本地监听地址。`host` 含 `:` 按 IPv6 处理。 */
  int bind(const std::string& host, int port);

  /**
   * @brief 多条循环（转发给内部 `uvcpp_tcp_server::set_loops`）。
   *
   * 必须在 `listen()` 之前调用；会话的状态机、上游、BIND 监听器与 UDP
   * socket 都建在会话自己那条循环上。
   */
  int set_loops(int n);

  /**
   * @brief 开始接受连接。
   *
   * \p on_session 在每条会话**命令结果确定时**调一次：建立成功是
   * `status == 0`，被拒/失败是错误码。它跑在会话自己那条循环上。
   */
  int listen(
      std::function<void(const uvcpp_socks5_session_info&)> on_session = nullptr,
      int backlog = 128);

  /**
   * @brief 会话结束回调（可选；必须在 `listen()` 之前设置）。
   *
   * 每条会话**恰好一次**：对端关闭、读/写错误、服务端 `close()`、握手失败
   * 都覆盖。在会话自己的循环上调用，不持锁；回调里可以读服务端状态。
   */
  void set_session_end_callback(uvcpp_socks5_session_end_cb cb);

  /** @brief 实际监听端口（`bind()` 用 0 之后有效）；未绑定时 -1。 */
  int port() const;

  /** @brief 当前活跃会话数。 */
  std::size_t connection_count() const;

  /** @brief 停止接受新连接并关闭所有会话（包括工作循环上的）。 */
  void close();

  /** @brief 最近一次本地错误的人类可读说明。 */
  const std::string& last_error() const;

 private:
  friend struct socks5_server_conn;

  void on_accepted(class uvcpp_tcp_client* client);

  uvcpp_loop*       loop_;
  uvcpp_tcp_server* tcp_;
  bool              listening_;
  bool              closed_;
  int               bound_port_;
  std::atomic<std::size_t> conn_count_;
  uvcpp_socks5_server_options opts_;
  std::function<void(const uvcpp_socks5_session_info&)> on_session_;
  uvcpp_socks5_session_end_cb on_session_end_;
  std::string last_error_;
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_SOCKS5_SERVER_H
