/**
 * @file src/quic/uvcpp_quic_server.h
 * @brief QUIC 服务端端点：一条（或多条）UDP 口 + 它上面所有连接。
 * @author zhuweiye
 * @version 1.4.1
 *
 * 本头**不包含** `<ngtcp2/ngtcp2.h>`，也不包含 OpenSSL 的任何头（同
 * `uvcpp_quic_client.h`）。CID 路由用的那张表在这层，但**从包里认 CID** 那件事
 * 在内核里（`quic_detail::quic_session::peek_dcid`）—— 本层只拿字节串当键。
 */

#ifndef SRC_QUIC_UVCPP_QUIC_SERVER_H
#define SRC_QUIC_UVCPP_QUIC_SERVER_H

#include <uvcpp/uvcpp_config.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <uv.h>
#include <handle/uvcpp_loop.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>

#include "quic/uvcpp_quic_common.h"
#include "quic/uvcpp_quic_connection.h"

#if UVCPP_QUIC_ENABLE

namespace uvcpp {

/** TLS 上下文。只前置声明 —— 理由见 `uvcpp_quic_client.h` 同名那条。 */
class uvcpp_ssl_context;

/**
 * @brief QUIC 服务端端点。
 *
 * **一条 UDP 口上有很多条连接。** QUIC 的连接是由 CID（Connection ID）标识的，
 * 不是由四元组 —— 所以 `listen()` 的回调每收到一条**新**连接就调一次，而它们
 * 全都跑在同一个 socket 上。这是 QUIC 与 TCP 在形状上最大的一处不同，也是
 * 本类与 `uvcpp_quic_connection` 必须分开的原因。
 *
 * **每个入包怎么找到它的连接**：从包头里取 DCID（`peek_dcid`），拿它查一张
 * 「CID → 连接」的表。一条连接在表里有**好几个键**（RFC 9000 §5.1 允许一个连接
 * 持有多个 CID）—— 本端自己发的每一个 SCID，加上对端首包里的那个原始 DCID。
 * 表由内核的 `on_new_cid` / `on_remove_cid` 事件驱动着长与缩，本层只管维护。
 *
 * **多循环**：本版**没有** `uvcpp_tcp_server` 那套多循环扇出（`set_loops()`）。
 * QUIC 的多循环切分是按 CID 而不是按连接亲和度做的，形状不同，不是"暂时没写"，
 * 是**不打算照搬**。这行注释就是给后来人的路标。
 *
 * **线程**：所有成员都只能在 `get_loop()` 那条线程上调用。
 */
class UVCPP_API uvcpp_quic_server {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_quic_server)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_quic_server)

  /**
   * @brief 共用一个外部循环。
   *
   * @param external_loop 生命周期必须覆盖本对象。**本对象不会关掉它。**
   */
  explicit uvcpp_quic_server(uvcpp_loop* external_loop);

  /** @brief 内部循环（自建那条路）或外部循环（共享那条路）。 */
  uvcpp_loop* get_loop();

  // -----------------------------------------------------------------
  // Bind
  // -----------------------------------------------------------------

  /**
   * @brief 登记一个 UDP 地址，IPv4/IPv6 自动判断，并**校验它是个合法的地址字面量**。
   *
   * @param ip   IPv4 或 IPv6 的**字面量**（`nullptr` = 通配 IPv4，即 `"0.0.0.0"`）。
   *            域名**不收**：UDP 的绑定需要一个 `sockaddr`，而"解析成一个本机
   *            接口地址"是另一件事（解析出多个地址时选哪个？），不是这里该猜的。
   * @param port 端口，`0..65535`。**`0` 是"由内核挑一个"**，不是"不绑端口" ——
   *             挑中的那个在 `listen()` 之后由 `configured_port()` 报出来。
   * @return 0 成功；`UV_EINVAL` 地址或端口不合法。
   *
   * **校验是真的**：地址经 `uv_inet_pton()` 走一遍，所以 `"999.1.1.1"` 或
   * `"not-an-address"` 会当场被拒，而不是等到 `listen()` 才炸。
   *
   * 失败时**不改变**已经登记好的地址（先校验、后赋值），与
   * `uvcpp_tcp_server::bind()` 那条约定一致。
   *
   * @note 本函数**不创建、不绑定任何 socket** —— 它只登记并校验。端口被占用
   *       要到 `listen()` 才暴露。所以 `bind()` 返回 0 **不等于**"这个端口归我
   *       了"；为此下面两个访问器叫 `configured_*` 而不是 `bound_*`。
   */
  int bind(const char* ip, int port);

  /** @brief `bind()` 的 IPv4 版本：\p ip 必须解析成一个 IPv4 地址。 */
  int bindIpv4(const char* ip, int port);

  /** @brief `bind()` 的 IPv6 版本：\p ip 必须解析成一个 IPv6 地址。 */
  int bindIpv6(const char* ip, int port);

  /**
   * @brief 登记的地址，字面量原样；**还没登记过时返回空串**。
   *
   * 登记的是通配地址（或没登记）时，报的是**登记值**，不是"内核实际绑上的那个
   * 具体地址" —— 后者在通配的情形下是"哪个都行"，报出来反而误导。
   */
  std::string configured_ip() const;

  /**
   * @brief 登记的端口；**还没登记过时为 0**。
   *
   * @note **`listen()` 之后报的是内核实际给的那个端口**：`bind(ip, 0)` 是测试里
   *       发现端口的常规写法（`tcp_server_func.cpp` 就是这么做的），那时登记的
   *       0 不再是有效信息，"内核给了我哪个"才是。
   */
  int configured_port() const;

  // -----------------------------------------------------------------
  // 配置
  // -----------------------------------------------------------------

  /**
   * @brief 设 TLS 上下文。
   *
   * @param ctx 生命周期必须覆盖整条连接（**不属于本对象**）。清空用
   *            `set_ssl_context(nullptr)`。
   *
   * **不设就是没配**：QUIC 没有明文模式（ALPN 是 TLS 扩展），所以 `listen()` 时
   * 上下文为空会返回 `UV_EINVAL`，而不是退化成一个不加密的服务端。
   */
  void set_ssl_context(uvcpp_ssl_context* ctx);

  /**
   * @brief 设服务端愿意接受并回给对端的 ALPN 候选，按**优先顺序**排。
   *
   * 不调就用 `quic_default_alpn()`（`"h3"`）。传空列表等于"一个都不接受" ——
   * 对端但凡发了候选就会协商失败；这不是"回到默认"，别指望它。
   *
   * 服务端是**选择方**：对端的候选里没有一个落在这张表里时，按 RFC 7301 应当以
   * `no_application_protocol` 告警结束握手，而**不是**挑一个双方都没承诺的协议名。
   *
   * 只在 `listen()` **之前**调有效：SSL_CTX 上的选择回调那时候装上去。
   */
  void set_alpn_select_protos(const std::vector<std::string>& protos);

  /**
   * @brief 空闲超时（毫秒）。**默认 30000**（30 秒）；`0` = 不设超时。
   *
   * 与 `uvcpp_quic_client::set_idle_timeout()` 同一条：这个值进 QUIC 的
   * `max_idle_timeout` 传输参数发给对端，实际生效的是双方声明里小的那个。它同时
   * 是服务端收掉"对端已经走了但没告别"的连接的**唯一**手段。
   *
   * 只对 `listen()` **之后**建的连接有效 —— 每条连接的传输参数在它建出来的那一刻
   * 就定死了。
   */
  void set_idle_timeout(uint64_t ms);

  // -----------------------------------------------------------------
  // Listen / Run
  // -----------------------------------------------------------------

  /**
   * @brief 开始接收连接：绑 `configured_ip():configured_port()`，起收包。
   *
   * @param connection_cb 每收到一条**新**连接时调一次，给的指针属于本对象
   *        （**不转让所有权**），在它的 `on_close` 之后即失效。与
   *        `uvcpp_tcp_server::listen()` 那条所有权约定刻意保持一致 —— 上层那套
   *        "连接表 + 断开时自动摘除"的写法两边要能共用。
   *
   * @return 0 = 已经在收；负值 = 没起来（没设 TLS 上下文、端口被占用、地址
   *         不合法……）。**返回负值时 `connection_cb` 一次都不会被调。**
   *
   * @warning **`connection_cb` 在连接刚建出来（首包到达）时跑，不在握手完成时
   *          跑。** 这与 `uvcpp_quic_client::connect()` 的 `cb` 刻意不同，理由
   *          是这一条：`on_alpn` / `on_read` 这些回调要在**它之后**由使用者装
   *          上去，装晚了（等握手完成再装）握手期间的事件就全丢了。要从"能开始
   *          收发"这个点出发，看 `on_alpn` 或 `state()`。
   *
   * @warning 重复调返回 `UV_EALREADY`（不是"又绑了一次"）。
   */
  int listen(std::function<void(uvcpp_quic_connection*)> connection_cb);

  /**
   * @brief 泵循环。语义与 `uvcpp_tcp_server::run()` 一致。
   *
   * 每个新的到期时刻（重传、空闲超时、握手超时）都靠**一个**定时器驱动，装在
   * 所有连接里最早的那个时刻上 —— QUIC 的一条 UDP 口上跑着很多条连接，一条一个
   * 定时器会让定时器数随连接数长。
   */
  int run(uv_run_mode md = UV_RUN_DEFAULT);

  /** @brief 停循环，导致 `run()` 返回。 */
  void stop();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE

#endif  // SRC_QUIC_UVCPP_QUIC_SERVER_H
