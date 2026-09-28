/**
 * @file src/quic/uvcpp_quic_connection.h
 * @brief 一条 QUIC 连接：流的生命周期与连接元数据的公开面。
 * @author zhuweiye
 * @version 1.4.1
 *
 * **1.4.1 交付的是形状，不是功能。** 下面每一个会改变状态的方法都返回
 * `UV_ENOSYS`（"这个功能还没实现"），一条真连接都建不出来。这个约定由
 * `tests/functional/quic_api_func.cpp` 钉死 —— 一旦哪天真实现了，那条用例会红，
 * 逼着实现者**显式**回来改契约，而不是让"框架写好了"这句话悄悄变成假的。
 *
 * 本头**不包含** `<ngtcp2/ngtcp2.h>` —— ngtcp2 的类型全在 `impl` 里。
 */

#ifndef SRC_QUIC_UVCPP_QUIC_CONNECTION_H
#define SRC_QUIC_UVCPP_QUIC_CONNECTION_H

#include <uvcpp/uvcpp_config.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>

#include "quic/uvcpp_quic_common.h"

#if UVCPP_QUIC_ENABLE

namespace uvcpp {

/**
 * @brief 一条 QUIC 连接。
 *
 * 为什么它和 `uvcpp_quic_client` / `uvcpp_quic_server` 是**三个**类：QUIC 是
 * **面向连接**的，但连接不是"一条 socket"。客户端起步时连对端的地址都还没有
 * （首包之前那条 socket 只是个 UDP 口），服务端的**一条** UDP 口背后可能同时有
 * 成千上万条连接。所以"端点"（client/server，管 socket 与监听）与"连接"
 * （管状态机、流、CID）必须分开 —— 这与 `uvcpp_tcp_client` 那种"一个对象既是
 * socket 又是连接"的形状不同，不是命名习惯问题。
 *
 * **线程**：所有成员都只能在它所属的那条 loop 线程上调用。
 */
class UVCPP_API uvcpp_quic_connection {
 public:
  /**
   * @brief 回调集合。
   *
   * 每个回调都在 **loop 线程**上被调用。
   *
   * @warning **重入**：`on_read` / `on_stream_open` 会在某个内部调用**还没返回**
   *          的时候就同步跑用户代码，而用户代码可以 `close()` 掉这条连接。于是
   *          回调返回后**不要再碰本对象** —— 与 `uvcpp_h2_connection::callbacks`
   *          那条警告逐字同理（那里踩过）。
   */
  struct callbacks {
    /**
     * @brief 某条流上有字节可读（或对端收了 / 读出错）。
     *
     * 事件语义与 net 层**完全一致**（`net_read_event::DATA / PEER_CLOSED /
     * READ_ERROR`）—— 见 `uvcpp_quic_common.h` 里 `uvcpp_quic_read_cb` 的说明。
     */
    uvcpp_quic_read_cb on_read;

    /**
     * @brief 对端开了一条新流（服务端侧才收得到；客户端侧是自己 `open_stream()`
     *        开的，不走这个回调）。
     */
    std::function<void(uvcpp_quic_connection&, int64_t stream_id)> on_stream_open;

    /**
     * @brief ALPN 协商完成，\p alpn 是对端选定的协议名。
     *
     * 在此之前应用数据一个字节都不能发 —— 发出去也没人认。
     */
    std::function<void(uvcpp_quic_connection&, const std::string& alpn)> on_alpn;

    /**
     * @brief 连接结束了（对端关、超时、或我们关完了）。
     *
     * @param error_code 0 = 正常收尾；非 0 = ngtcp2 的错误码（负值），
     *                   拿 `quic_error_string()` 问它是什么意思。
     *
     * 回调返回后**不要**再碰本对象 —— 持有者应当在这里把它销毁。
     */
    std::function<void(uvcpp_quic_connection&, int error_code)> on_close;
  };

  UVCPP_DEFINE_FUNC(uvcpp_quic_connection)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_quic_connection)

  /** @brief 装回调集合。随时可换，换掉的那份立刻失效。 */
  void set_callbacks(const callbacks& cbs);

  // -----------------------------------------------------------------
  // 元数据
  // -----------------------------------------------------------------

  /**
   * @brief 当前状态。
   *
   * @warning **1.4.1 恒返回 `quic_connection_state::IDLE`** —— 状态机还没写，
   *          没有任何代码路径给这个值赋过别的。见 `uvcpp_quic_common.h` 里那个
   *          枚举的说明。
   */
  quic_connection_state state() const;

  /**
   * @brief 握手协商出来的 ALPN。
   *
   * 握手完成前是**空串**（不是猜测值，也不是默认 ALPN）—— 应用不该在这之前拿它
   * 做分支。要判断"能不能开始用"，看 `state()` 或 `on_alpn`，不要拿这里的空串
   * 当"还没好"的判据（那是巧合，不是契约）。
   *
   * @warning **1.4.1 恒返回空串**（握手还没实现）。
   */
  std::string alpn_selected() const;

  // -----------------------------------------------------------------
  // 流
  // -----------------------------------------------------------------

  /**
   * @brief 开一条流。
   * @param bidi true = 双向流（`open_stream` 那条），false = 单向流。
   * @return 成功返回**流号**（`int64_t`，QUIC 的流号是 62 位无符号，不是
   *         `int`）—— 它是**低位 bit 编码方向**的，别自己拼；失败返回负的
   *         libuv/ngtcp2 错误码。
   *
   * @warning **1.4.1 恒返回 `UV_ENOSYS`。**
   *
   * 返回 `int64_t` 而不是 `int`：QUIC 流号用完了 `int` 的 31 位（RFC 9000 §2.1
   * 允许到 2^62-1），用一个会截断的返回类型，就是把一个"永远到不了"的假设
   * 焊进签名里。
   */
  int64_t open_stream(bool bidi = true);

  /**
   * @brief 往一条流上写。
   * @param stream_id `open_stream()` 给的号，或 `on_stream_open` 给的号。
   * @param end_stream true = 这块之后本端不再发了（发 FIN）。
   * @return 0 = 已受理；负值 = 没收下，调用方自己收尾。
   *
   * **注意这是"受理"，不是"发出去了"** —— 与 `uvcpp_tcp_client::write()` 一样，
   * 真正的完成要靠回调。但这一版连回调都还没有。
   *
   * @warning **1.4.1 恒返回 `UV_ENOSYS`。**
   */
  int write_stream(int64_t stream_id, const char* data, size_t len,
                   bool end_stream = false);

  /**
   * @brief 只关发送方向（发 FIN，收方向照常）—— QUIC 的 `shutdown`。
   *
   * QUIC 的流是**两个方向各关一次**的，所以"关一条流"不是一个动作。想要
   * TCP 那种"两边一起关"，得 `shutdown_stream()` 之后再等对端也关。
   *
   * @warning **1.4.1 恒返回 `UV_ENOSYS`。**
   */
  int shutdown_stream(int64_t stream_id);

  /**
   * @brief 关掉整条连接。
   * @param error_code 0 = 正常关闭；非 0 = 应用层错误码，会发给对端。
   *
   * @warning **1.4.1 恒返回 `UV_ENOSYS`。**
   */
  int close(int error_code = 0);

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE

#endif  // SRC_QUIC_UVCPP_QUIC_CONNECTION_H
