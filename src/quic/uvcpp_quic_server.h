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
 * **多循环**：本类**不提供** `uvcpp_tcp_server::set_loops()` 那样的旋钮，而那是
 * **设计**不是缺口：本类的一个实例就是**一条循环上的一个 UDP socket**，多循环那
 * 件事是**在上面一层**由多个本类实例拼出来的 —— 每个实例一个 socket、各带
 * `UV_UDP_REUSEPORT` 抢同一个端口，内核按四元组哈希把数据报分给其中一个。
 * `uvcpp_http_server::set_loops(n)` 干的就是这件事（见 `listen_quic()`）。
 *
 * 两个端点各持一张 CID 表**够用**，因为：SCID 是每连接现取的随机值，连接归属由
 * 「谁收到首包谁建」定，而本库**不做连接迁移** ⇒ 同一个四元组被内核哈希恒定映射
 * 到同一个 socket，后续包必落回同一端点。落到陌生端点的包**丢掉，不转发**（在
 * QUIC 里这与链路丢包等价，重传兜底）。边界：一旦将来加连接迁移、或者做 socket
 * 热重启，转发或共享 CID 表就从"可选"变成"必需"。
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
   * **销毁顺序**：上下文必须比本对象活得久 —— 同一作用域里先声明上下文、再声明本对象；
   * 反过来（先本对象后上下文）时，本对象析构收尾仍会拿这个裸指针去用，是 UB（issue #56）。
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
   * @brief 让下一次 `listen()` **带 `UV_UDP_REUSEPORT` 去绑**，好与别的端点
   *        （各自一条循环、各自一个本类实例）**抢同一个端口**。
   *
   * 这是拼多循环扇出的那一半：n 个本类实例各绑各的 socket、带同一个标志、
   * 落在同一个 `(地址, 端口)` 上，内核按四元组哈希把数据报分给其中一个。
   * **不调它（默认）就是今天的行为**：一个端点独占那个端口。
   *
   * 回落是**运行时**的，且不改变返回值 —— 与 `uvcpp_tcp_server::set_loops()`
   * 那条同一套：平台不支持（Windows 直接返 `ERROR_NOT_SUPPORTED`；macOS 等
   * `uv__sock_reuseport()` 返 `UV_ENOTSUP`）、或者这份 libuv 太老
   * （`UV_UDP_REUSEPORT` 是 **1.50** 才有的，见 `handle/uvcpp_udp.h` 末尾），
   * 那次带标志的 `bind()` 会失败 ⇒ 自动**去掉标志重绑一次**（`bind(2)` 还没发
   * 生过，同一个句柄可以重来），并在 stderr 上打一行说明。真伪一律问
   * @ref is_reuseport：**调用方要靠它决定"能不能再建第 2..n 个端点"**。
   *
   * @warning 必须在 @ref listen **之前**调：标志是 `bind()` 那一刻用的，绑完
   *          再改它不会改变已经绑好的形状。
   *
   * @note 这是**给多循环扇出用的口子**，不是"更快的开关"：单独一个端点带不带
   *       这个标志，功能与吞吐完全一样 —— 差别只在"同一个端口能不能被第二个人
   *       绑上"。
   */
  void set_reuseport(bool on);

  /**
   * @brief 这次 `listen()` 到底带上 `SO_REUSEPORT` 了没有。
   *
   * **运行时探测的结果，不是编译期常量**：同一个二进制在不同平台上不一样
   * （Linux 上给得出来，Windows 上 libuv 无条件拒绝，macOS 上
   * `uv__sock_reuseport()` 没有实现）。判据要看这个值决定断言哪一套，别拿
   * `#ifdef` 猜平台。
   *
   * `false` 有三种来路，对调用方是**同一件事**（这个端点独占端口）：没调过
   * @ref set_reuseport、平台不支持、标志被 libuv 忽略。`listen()` 成功之前
   * 恒为 `false`。
   */
  bool is_reuseport() const;

  /**
   * @brief 本端点**自己发的 SCID** 用几个字节。默认 18
   *        （`quic_session::scid_len()`），不调它 = 今天逐字节相同。
   *
   * 为什么这是个可调的值：短包头**不把 DCID 长度写在包上**（RFC 9000
   * §17.3.1），收方只能按**自己**选的长度反推；而两端各自选自己的长度
   * （RFC 9000 §5.1）。本库两端默认都选 18 ⇒ 同源，于是「拿**对端** SCID 的
   * 长度去解**本端**的 DCID」这类错误**恒等抵消**：既有用例一条都不会红
   * （`745608c` 那条缺陷就是这么在平台上才现形的 —— 上游 `h2load` 选的是
   * 8）。这个口子让用例把两端拉开，让 CI 自己复现那一条。
   *
   * @param n 取 `8..20`。上限是 `NGTCP2_MAX_CIDLEN`(20)；下限取 8 而不是
   *          `NGTCP2_MIN_CIDLEN`(1)，是因为一条 UDP 口上跑着很多条连接，1 字节
   *          的 CID 只有 256 种取值 ⇒ 撞键是常态，那个档不是"另一种配置"而是
   *          "路由坏掉"。越界与"装晚了"一样只打一行 stderr 并忽略。
   *
   * @warning 必须在 @ref listen **之前**调：这个数在**建连接**时定下（本端
   *          SCID 的长度就写进首包了），而每一次入包查表都用它 —— 监听之后再
   *          改，已经在跑的那些连接会立刻全部解错包头。
   *
   * @note 这是**给用例用的口子**，不是"更省字节的开关"：CID 长短只影响每包
   *       18 − n 个字节的开销，而它换来的是"两端长度可以不一样"这条被测的
   *       自由度。
   */
  void set_scid_len(size_t n);

  /**
   * @brief 本端点自己发的 SCID 有几个字节 —— @ref set_scid_len 记下的值，
   *        没调过就是 `quic_session::scid_len()`（18）。
   *
   * @note 它与 `quic_detail::quic_session::scid_len()` **不一定相等**：那一个
   *       是本库的默认值，这一个是**这个端点**实际用的值。端点在查表时用的是
   *       后者。
   */
  size_t scid_len() const;

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

  /**
   * @brief 停**本端点所在的那条循环**，导致 `run()` 返回。
   *
   * @warning **只能在 `get_loop()` 自己的线程上调。** `uvcpp_http_server` 的多
   *          循环扇出会建 n 个本类实例（1..n−1 号跑在各自的工作循环上），那些
   *          **一次都不能调它**：在工作循环上调这条会让那条 `uv_run()` 提前返回、
   *          绕过 `uvcpp_loop_worker` 自己的停机闸，于是排空邮箱、关本循环的
   *          连接、泵掉挂起的关闭回调这一串收尾就再也没人跑。工作循环的停止权
   *          **独占** `uvcpp_tcp_server::stop_workers()`。
   */
  void stop();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE

#endif  // SRC_QUIC_UVCPP_QUIC_SERVER_H
