/**
 * @file src/quic/uvcpp_quic_server.h
 * @brief QUIC 服务端端点：一条（或多条）UDP 口 + 它上面所有连接。
 * @author zhuweiye
 * @version 1.4.1
 *
 * **1.4.1 交付的是形状，不是功能。** `bind()` 系列与几个 setter 是**真实现**
 * （存值 + 参数校验），因为它们现在就能被验证、也是配置期就要用的东西；
 * `listen()` 返回 `UV_ENOSYS` —— 一条连接都收不上来。
 *
 * 本头**不包含** `<ngtcp2/ngtcp2.h>`，也不包含 OpenSSL 的任何头（同
 * `uvcpp_quic_client.h`）。
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
  // Bind —— **真实现**
  // -----------------------------------------------------------------

  /**
   * @brief 登记一个 UDP 地址，IPv4/IPv6 自动判断，并**校验它是个合法的地址字面量**。
   *
   * @param ip   IPv4 或 IPv6 的**字面量**（`nullptr` = 通配 IPv4，即 `"0.0.0.0"`）。
   *            域名**不收**：UDP 的绑定需要一个 `sockaddr`，而"解析成一个本机
   *            接口地址"是另一件事（解析出多个地址时选哪个？），不是这里该猜的。
   * @param port 端口，`0..65535`。
   * @return 0 成功；`UV_EINVAL` 地址或端口不合法。
   *
   * **校验是真的**：地址经 `uv_inet_pton()` 走一遍，所以 `"999.1.1.1"` 或
   * `"not-an-address"` 会当场被拒，而不是等到 `listen()` 才炸。
   *
   * 失败时**不改变**已经登记好的地址（先校验、后赋值），与
   * `uvcpp_tcp_server::bind()` 那条约定一致。
   *
   * @warning **本版不创建、不绑定任何 socket** —— 它只登记并校验。端口被占用
   *          这件事要等到 `listen()` 才会暴露，而 `listen()` 这一版返回
   *          `UV_ENOSYS`。所以 `bind()` 返回 0 **不等于**"这个端口归我了"；
   *          为此下面两个访问器叫 `configured_*` 而不是 `bound_*`，名字里
   *          就该看得出这一点。
   */
  int bind(const char* ip, int port);

  /** @brief `bind()` 的 IPv4 版本：\p ip 必须解析成一个 IPv4 地址。 */
  int bindIpv4(const char* ip, int port);

  /** @brief `bind()` 的 IPv6 版本：\p ip 必须解析成一个 IPv6 地址。 */
  int bindIpv6(const char* ip, int port);

  /**
   * @brief 登记的地址，字面量原样；**还没登记过时返回空串**。
   *
   * @note 不是"绑定成功了"的判据 —— 见 `bind()` 那条 `@warning`。
   */
  std::string configured_ip() const;

  /** @brief 登记的端口；**还没登记过时为 0**（端口 0 也表示"由内核挑"）。 */
  int configured_port() const;

  // -----------------------------------------------------------------
  // 配置 —— **真实现**（只存值）
  // -----------------------------------------------------------------

  /**
   * @brief 设 TLS 上下文。
   *
   * @param ctx 生命周期必须覆盖整条连接（**不属于本对象**）。清空用
   *            `set_ssl_context(nullptr)`。
   *
   * **不设就是没配**：QUIC 没有明文模式。
   */
  void set_ssl_context(uvcpp_ssl_context* ctx);

  /**
   * @brief 设服务端愿意接受并回给对端的 ALPN 候选，按**优先顺序**排。
   *
   * 不调就用 `quic_default_alpn()`（`"h3"`）。服务端是**选择方**：对端的候选里
   * 没有一个落在本列表里时，按 RFC 7301 应当以 `no_application_protocol` 告警
   * 结束握手，而**不是**挑一个双方都没承诺的协议名。
   */
  void set_alpn_select_protos(const std::vector<std::string>& protos);

  // -----------------------------------------------------------------
  // Listen / Run —— **1.4.1 不实现**
  // -----------------------------------------------------------------

  /**
   * @brief 开始接收连接。
   *
   * @param connection_cb 每收到一条**新**连接的握手**完成**时调一次，给的指针
   *        属于本对象（**不转让所有权**），在它的 `on_close` 之后即失效。
   *        与 `uvcpp_tcp_server::listen()` 那条所有权约定刻意保持一致 ——
   *        上层那套"连接表 + 断开时自动摘除"的写法两边要能共用。
   *
   * @warning **1.4.1 恒返回 `UV_ENOSYS`，且 `connection_cb` 一次都不会被调。**
   *          回调不跑是一条**成文契约**（测试钉了它）："受理了但永远不回调"
   *          是最难查的一类挂死，宁可不收。
   */
  int listen(std::function<void(uvcpp_quic_connection*)> connection_cb);

  /**
   * @brief 泵循环。语义与 `uvcpp_tcp_server::run()` 一致。
   *
   * @warning 本版**没有** `uvcpp_tcp_server` 那套多循环扇出（`set_loops()`）——
   *          QUIC 的多循环切分是按 CID 而不是按连接亲和度做的，形状不同，
   *          不是"暂时没写"，是**不打算照搬**。这行注释就是给后来人的路标。
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
