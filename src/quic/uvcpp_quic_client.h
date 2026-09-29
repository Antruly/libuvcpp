/**
 * @file src/quic/uvcpp_quic_client.h
 * @brief QUIC 客户端端点：一条 UDP 口 + 它上面那条连接。
 * @author zhuweiye
 * @version 1.4.1
 *
 * 本头**不包含** `<ngtcp2/ngtcp2.h>`，也**不包含** OpenSSL 的任何头：
 * `uvcpp_ssl_context` 只以**前置声明的指针**出现（同 `uvcpp_h2_connection` 对
 * `uvcpp_tcp_client` 的做法）。使用者不需要为了 include 这个头去配 OpenSSL 的
 * 搜索路径。
 */

#ifndef SRC_QUIC_UVCPP_QUIC_CLIENT_H
#define SRC_QUIC_UVCPP_QUIC_CLIENT_H

#include <uvcpp/uvcpp_config.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <handle/uvcpp_loop.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>

#include "quic/uvcpp_quic_common.h"
#include "quic/uvcpp_quic_connection.h"

#if UVCPP_QUIC_ENABLE

namespace uvcpp {

/**
 * @brief TLS 上下文。**只前置声明** —— 本头不认识它的形状，只拿着指针。
 *
 * 生命周期由调用方负责，且**必须覆盖整条连接**：与
 * `uvcpp_tcp_client::enable_tls()` 那条约束逐字相同。
 *
 * 这里不写 `#if UVCPP_OPENSSL_ENABLE` 包裹：QUIC 在配置期就要求 OpenSSL 打开
 * （`CMakeLists.txt` 里那条守卫），所以 `UVCPP_QUIC_ENABLE` 为真时
 * `UVCPP_OPENSSL_ENABLE` 必然为真，生成头里这两个宏不会漂。
 */
class uvcpp_ssl_context;

/**
 * @brief QUIC 客户端端点。
 *
 * 与 `uvcpp_tcp_client` 的形状差别，根子在 QUIC 自己身上：TCP 的"客户端"从头到尾
 * 就是**一条** socket，于是那一个类既是端点又是连接；QUIC 的首包之前那条 UDP
 * socket 连对端地址都还不知道，握手完成之后"连接"是被 CID 标识的、可以随着路径
 * 变化换 socket。所以端点（本类）与连接（`uvcpp_quic_connection`）是两件事。
 *
 * **线程**：所有成员都只能在 `get_loop()` 那条线程上调用。
 */
class UVCPP_API uvcpp_quic_client {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_quic_client)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_quic_client)

  /**
   * @brief 共用一个外部循环。
   *
   * @param external_loop 生命周期必须覆盖本对象。**本对象不会关掉它** ——
   *        同 `uvcpp_tcp_client(uvcpp_loop*)` 那条。
   */
  explicit uvcpp_quic_client(uvcpp_loop* external_loop);

  /** @brief 内部循环（自建那条路）或外部循环（共享那条路）。 */
  uvcpp_loop* get_loop();

  // -----------------------------------------------------------------
  // 配置
  // -----------------------------------------------------------------

  /**
   * @brief 设 TLS 上下文。
   *
   * @param ctx 生命周期必须覆盖整条连接（**不属于本对象**）。清空用
   *            `set_ssl_context(nullptr)`。
   *
   * **不设就是没配**：QUIC 没有明文模式（ALPN 是 TLS 扩展），所以
   * `connect()` 时上下文为空会返回 `UV_EINVAL`，而不是退化成一个不加密的连接。
   */
  void set_ssl_context(uvcpp_ssl_context* ctx);

  /**
   * @brief 设要交给 TLS 的 ALPN 候选，按**优先顺序**排。
   *
   * 不调就用 `quic_default_alpn()`（`"h3"`）。传空列表等于"一个都不发" ——
   * 那会让对端无从选择，通常直接握手失败；这不是"回到默认"，别指望它。
   *
   * 存的是拷贝，调用方那两份 `std::string` 可以立刻销毁。
   */
  void set_alpn_protos(const std::vector<std::string>& protos);

  /**
   * @brief 空闲超时（毫秒）。**默认 30000**（30 秒）；`0` = 不设超时。
   *
   * 这个值会进 QUIC 的 `max_idle_timeout` 传输参数**发给对端**，所以它是双方
   * 各自的声明里取小的那个 —— 设成 100ms 时，链路上 100ms 没有包就会被关掉，
   * 而 `on_close` 收到的错误码是 `NGTCP2_ERR_IDLE_CLOSE`。
   *
   * 只在 `connect()` **之前**调有效：握手一开始，传输参数就发出去定死了。
   */
  void set_idle_timeout(uint64_t ms);

  // -----------------------------------------------------------------
  // 传输
  // -----------------------------------------------------------------

  /**
   * @brief 连到对端：解析地址、建 UDP 口、建内核、发出第一个 Initial 包。
   *
   * @param host 主机名或 IP 字面量。**用主机名就会阻塞解析**（这是有意的：
   *             QUIC 的握手包总得先有个地址，而"异步解析"会把一个地址问题
   *             变成一串异步状态机）。字面量走 `uv_ip4_addr` / `uv_ip6_addr`
   *             的快速路径，不碰 DNS。
   * @param port 端口。
   * @param cb   完成回调：`0` = **握手完成**（不是"受理了"）；负值 = 失败。
   *
   * @return 0 = 已开始（此后 `cb` **一定**会被调一次）；负值 = 连开始都没开始
   *         （没设 TLS 上下文、地址不合法、解析失败、UDP 口建不出来……），
   *         **此时 `cb` 一次都不会被调**。
   *
   * @warning **`cb` 只在握手完成时跑，不在连接建立时跑** —— 那是 `on_alpn` /
   *          `state()` 到 `ESTABLISHED` 的同一时刻。所以"能发数据了"这个判据
   *          在 `cb(0)` 里成立，在 `connect()` 返回时**不**成立。
   *
   * @warning **回调要在这里装：`connect()` 返回之后、下一次循环迭代之前。**
   *          形状是
   *          ```cpp
   *          int rc = client.connect(host, port, cb);
   *          if (rc == 0) client.connection()->set_callbacks(cbs);
   *          ```
   *          `connect()` 内部不发包（第一个 Initial 要等循环转起来），而 libuv
   *          的回调只能从循环里出来 —— 所以"返回之后、下一次迭代之前"这段窗口
   *          里装回调是安全的，而且这是**唯一**安全的地方：等 `cb(0)` 到了再装，
   *          握手期间那些事件（`on_stream_open` 之类）已经过去了。
   */
  int connect(const char* host, int port, std::function<void(int)> cb);

  /**
   * @brief 本对象名下那条连接；还没 `connect()` 过（或已 `close()`）时是
   *        `nullptr`。
   *
   * 指针**不转让所有权**（属于本对象），在 `on_close` 之后即失效。
   */
  uvcpp_quic_connection* connection();

  /**
   * @brief 关掉端点与它名下的连接：先发 CONNECTION_CLOSE，再收 socket 与定时器。
   *
   * @return 0 = 关掉了（或本来就没东西可关）；`UV_ENOTCONN` = 没连过。
   *
   * 关闭是**优雅的**：CONNECTION_CLOSE 要发出去，`on_close` 要跑一次。所以
   * 调完本函数之后循环还得再转几轮，DRAINING/CLOSING 那段才算走完 ——
   * 如果循环已经不会再转（比如你就是在这个循环的最后一个回调里调的），
   * 析构里那 256 轮 `UV_RUN_NOWAIT` 是最后的机会。
   */
  int close();

 private:
  // 全部成员（含 `loop_` 与 `owns_loop_`）都在 impl 里 —— 本类是 pimpl 形状，
  // 一个裸成员就够把 `uvcpp.dll` 的内存布局钉死在这里，而那正是 pimpl 要免掉
  // 的事。只为省掉一次指针跳转而留一个裸成员，是拿 ABI 稳定性换的，不划算。
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE

#endif  // SRC_QUIC_UVCPP_QUIC_CLIENT_H
