/**
 * @file src/quic/uvcpp_quic_connection.h
 * @brief 一条 QUIC 连接：流的生命周期与连接元数据的公开面。
 * @author zhuweiye
 * @version 1.4.1
 *
 * 本类是**公开面**：谁看得见、回调叫什么、动作成功失败怎么报。协议怎么跑全在
 * `src/quic/uvcpp_quic_session.h` 那个私有内核里，两者的界线是"改公开 API"与
 * "改协议实现"互不牵扯。
 *
 * 本头**不包含** `<ngtcp2/ngtcp2.h>` —— ngtcp2 的类型全在 `impl` 里，公开面
 * 只留一个内核的**前置声明**（使用者永远不需要那个类型，它出现在这里只是为了让
 * 端点能把这个壳与内核接起来）。
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

namespace quic_detail {
/** 连接的内核，定义在 `src/quic/uvcpp_quic_session.h`（私有头）。
 *  在这里出现只是为了让 `attach()` / `session()` 有个类型名 —— **使用者不需要
 *  它，也不需要配 ngtcp2 的头搜索路径**：本头只用到它的指针。 */
class quic_session;
}  // namespace quic_detail

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
 * **所有权**：连接对象属于**端点**（`uvcpp_quic_client` / `uvcpp_quic_server`），
 * 不转让给使用者。`on_close` 之后它就失效了，别留着指针。
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
   * @warning **重入**：这些回调会在某个内部调用**还没返回**的时候就同步跑用户
   *          代码，而用户代码可以 `write_stream()` / `close()` 掉这条连接。
   *          这两件事都是**允许的**（内核会把它们推到回调退栈之后再兑现，
   *          理由见 `uvcpp_quic_session.h` 里那条 `@warning`），但**回调返回后
   *         不要再碰本对象**，尤其是 `on_close` 之后 —— 那时端点已经准备把它
   *          销毁了。
   */
  struct callbacks {
    /**
     * @brief 某条流上有字节可读（或对端收了 / 读出错）。
     *
     * 事件语义与 net 层**完全一致**（`net_read_event::DATA / PEER_CLOSED /
     * READ_ERROR`）—— 见 `uvcpp_quic_common.h` 里 `uvcpp_quic_read_cb` 的说明。
     *
     * @note `PEER_CLOSED` 是**按流**报的：某条流收到 FIN 只会让那条流报一次
     *       `PEER_CLOSED`，连接照常、别的流照常。要等整条连接结束，看
     *       `on_close`。
     */
    uvcpp_quic_read_cb on_read;

    /**
     * @brief 对端放开了流数上限，本端现在可以多开 `max_streams` 条**本地发起**
     *        的流。
     *
     * @param bidi        `true` = 双向流额度（`open_stream(true)` 那一档），
     *                    `false` = 单向流额度（`open_stream(false)`）。
     * @param max_streams **累计**上限（"本端到此刻为止最多能开多少条"），
     *                    不是这一回的增量 —— 与 ngtcp2 的
     *                    `extend_max_local_streams_*` 同义。
     *
     * **为什么要有它**：`open_stream()` 在对端还没放开额度时返回
     * `NGTCP2_ERR_STREAM_ID_BLOCKED`，而"什么时候再试"这件事**只有这里**说得出。
     * 没有这个回调，调用方只能轮询 —— 而轮询在一条静下来的连接上永远不会变成
     * "可以了"（额度是随包来的，不是随时间来的）。
     *
     * @note 握手刚完时对端的额度通常**已经**到了（`initial_max_streams_*` 是
     *       传输参数，随第一个包来），所以这条回调在多数连接上一次都不响。它是
     *       为"额度用完了、对端后来才放开"那一路准备的 —— 而 HTTP/3 的控制流与
     *       两条 QPACK 流正好是这种"一上来就要三条单向流"的用法。
     */
    std::function<void(uvcpp_quic_connection&, bool bidi, uint64_t max_streams)>
        on_streams_available;

    /**
     * @brief 对端下发了 STOP_SENDING —— "这条流你别再发了"。
     *
     * @param stream_id      被要求停下的那条流。
     * @param app_error_code 对端给的应用错误码（它在 STOP_SENDING 里带的）。
     *
     * **与 `on_read` 的收尾是两件事**，方向相反：`on_read` 报"对端不发了"
     * （我这一侧读完了），这一格报"对端不要我发了"（我这一侧发不动了）。一次
     * 对端取消在线上是**两个**帧 —— RESET_STREAM（我读不动了）与 STOP_SENDING
     * （你别发了）—— 所以两条通知各来一次是正常的，不是重复。
     *
     * @note **线上回应由协议栈自己发掉**：RFC 9000 §3.5 要求收到 STOP_SENDING
     *       的一侧回一个 RESET_STREAM，ngtcp2 在它自己的收帧路径里就直接排了那个
     *       帧（除非那条流的数据早已 FIN 且被确认）。所以这一格是**纯通知**，
     *       不是"等你决定回不回"。你要做的是：别再往这条流上排新东西，并且把
     *       应用自己那份还没交给 `write_stream()` 的待发送内容取消掉 —— 那些
     *       东西协议栈不知道。
     *
     *       那条流上已经受理但还没确认的 `write_stream()` 会以 `on_write` 报
     *       `NGTCP2_ERR_STREAM_SHUT_WR` 收场（走 `stream_close2` 那条路）。
     */
    std::function<void(uvcpp_quic_connection&, int64_t stream_id,
                       uint64_t app_error_code)>
        on_stop_sending;

    /**
     * @brief 对端开了一条新流（服务端侧才收得到；客户端侧是自己 `open_stream()`
     *        开的，不走这个回调）。
     */
    std::function<void(uvcpp_quic_connection&, int64_t stream_id)> on_stream_open;

    /**
     * @brief 一次 `write_stream()` 完成了（或确定完不成了）。
     *
     * @param stream_id 当初传给 `write_stream()` 的那个号。
     * @param status    0 = 对端确认了这次调用写入的**全部**字节；非 0
     *                  （`NGTCP2_ERR_STREAM_SHUT_WR` 的负值）= 那条流在那之前
     *                  就没了，这些字节**永远不会**到对端。
     *
     * **一次 `write_stream()` 对应一次本回调**，顺序与调用顺序一致。这条契约
     * 存在的理由与 `uvcpp_tcp_client::write()` 的完成回调一样：`write_stream()`
     * 是"受理"不是"发出去了"，没有完成通知，调用方就只能靠猜来决定什么时候能
     * 重用缓冲区。报 `status` 而不是只报成功，是为了**不让调用方在流被重置之后
     * 永远等一个不会来的回调**。
     */
    std::function<void(uvcpp_quic_connection&, int64_t stream_id, int status)>
        on_write;

    /**
     * @brief ALPN 协商完成，\p alpn 是对端选定的协议名。
     *
     * 在此之前应用数据一个字节都不能发 —— 发出去也没人认。
     */
    std::function<void(uvcpp_quic_connection&, const std::string& alpn)> on_alpn;

    /**
     * @brief 连接结束了（对端关、超时、或我们关完了）。
     *
     * @param error_code **按符号分三种意思**：
     *   - `> 0`：对端的**应用**错误码（它在 CONNECTION_CLOSE 里给的那个数）；
     *   - `= 0`：干净关闭；
     *   - `< 0`：传输层级的原因，值就是 ngtcp2 的错误码（`NGTCP2_ERR_DRAINING`
     *     表示对端关了我们、`NGTCP2_ERR_IDLE_CLOSE` 表示空闲超时、其余是本端
     *     自己报的错），拿 `quic_error_string()` 问它是什么意思。
     *
     * **恰好跑一次**：这条连接上无论有多少条路径通向终结（对端关、我们关完、
     * 超时、协议错），本回调都只跑一次。
     *
     * 回调返回后**不要**再碰本对象 —— 端点会在随后把它销毁。
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

  /** @brief 当前状态。见 `quic_connection_state`。 */
  quic_connection_state state() const;

  /**
   * @brief 握手协商出来的 ALPN。
   *
   * 握手完成前是**空串**（不是猜测值，也不是默认 ALPN）—— 应用不该在这之前拿它
   * 做分支。要判断"能不能开始用"，看 `state()` 或 `on_alpn`，不要拿这里的空串
   * 当"还没好"的判据（那是巧合，不是契约）。
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
   *         libuv/ngtcp2 错误码。`NGTCP2_ERR_STREAM_ID_BLOCKED` 是其中一种
   *         **正常**结果：对端还没放开流数上限，等 `on_stream_open` 之后
   *         对端再放开时会重新可开（本层不另发通知，因为那个信号总是搭着
   *         一个包来，而包到了就意味着该重试了）。
   *
   * 返回 `int64_t` 而不是 `int`：QUIC 流号用完了 `int` 的 31 位（RFC 9000 §2.1
   * 允许到 2^62-1），用一个会截断的返回类型，就是把一个"永远到不了"的假设
   * 焊进签名里。
   */
  int64_t open_stream(bool bidi = true);

  /**
   * @brief 往一条流上写。
   * @param stream_id `open_stream()` 给的号，或 `on_stream_open` 给的号。
   * @param data 要写的字节。**返回后即可释放** —— 本函数会拷一份进发送队列。
   * @param end_stream true = 这块之后本端不再发了（发 FIN）。
   * @return 0 = 已受理；负值 = 没收下（流已关写方向 / 连接已在关闭），调用方
   *         自己收尾。
   *
   * **这是"受理"不是"发出去了"** —— 真正的完成由 `on_write` 报，一次调用对
   * 一次回调。
   *
   * @note `data` 会被拷贝。这看起来比 `uvcpp_tcp_client::write()` 多一次
   *       memcpy，但那边的"调用方保证 buffer 活到回调"是一条**使用者的义务**，
   *       而它的签名里没有任何东西提示这件事（那条约束经常被违反）。这里是拿
   *       一次拷贝换掉一类难查的错误。
   */
  int write_stream(int64_t stream_id, const char* data, size_t len,
                   bool end_stream = false);

  /**
   * @brief 只关发送方向（发 FIN，收方向照常）—— QUIC 的 `shutdown`。
   *
   * QUIC 的流是**两个方向各关一次**的，所以"关一条流"不是一个动作。想要
   * TCP 那种"两边一起关"，得 `shutdown_stream()` 之后再等对端也关。
   *
   * @note 与 `write_stream(..., end_stream=true)` 的区别：那条是"把队列里排的
   *       都写完，再关"；这条是"队列里没写完的**也**不发了"（底层
   *       `ngtcp2_conn_shutdown_stream_write` 的语义就是丢掉它们）。所以这条
   *       之后，那些还没被确认的 `write_stream()` 调用会以 `on_write` 报
   *       `NGTCP2_ERR_STREAM_SHUT_WR` 收场 —— 不会有人永远等着。
   *
   * @param app_error_code 随 RESET_STREAM 发给对端的应用错误码。0 是缺省值，
   *       也是"没有错误"的约定值。非 0 时对端的读侧会以 `READ_ERROR` 收场
   *       （而不是 `PEER_CLOSED`）—— 见 `net_read_result::fin`。
   */
  int shutdown_stream(int64_t stream_id, uint64_t app_error_code = 0);

  /**
   * @brief 只关**读**方向（发 STOP_SENDING，写方向照常）。
   *
   * 这是上面那条的镜像：`shutdown_stream()` 说"我不发了"（RESET_STREAM），
   * 这条说"你别发了"（STOP_SENDING）。对端收到之后会在它的 `on_stop_sending`
   * 里知道。
   *
   * @param app_error_code 随 STOP_SENDING 发给对端的应用错误码；0 是缺省值。
   * @return 0 = 已受理；`UV_ENOTCONN` = 还没挂到端点上。
   *
   * @note 调过之后，这条流的**读侧就到此为止了**：不会再有任何 `on_read`
   *       事件（连接不因此结束，别的流也不受影响）。**也不会有一次"收尾"
   *       通知** —— 那是你自己刚下的决定，`on_stream_*` 里不会再报一遍。
   *       这与对端 reset 我们时不同（那个会报 `PEER_CLOSED`/`READ_ERROR`），
   *       因为那件事除了这里没人知道。
   *
   * @note 与 `shutdown_stream()` 是对称的：那条关写、这条关读。两个都调了，
   *       这条流才算真正收场（对端也关完之后 `on_stream_close` 那边才算完账）。
   */
  int shutdown_stream_read(int64_t stream_id, uint64_t app_error_code = 0);

  /**
   * @brief 本端还能**新开**多少条本地发起的流。
   *
   * @param bidi `true` 问双向流额度，`false` 问单向流额度。
   * @return 剩余条数；还没挂到端点上时返回 0。
   *
   * 这是 `open_stream()` 那个 `NGTCP2_ERR_STREAM_ID_BLOCKED` 的**主动**版本：
   * 想在开流之前先看一眼能不能开（比如服务端要按额度决定给对端多少
   * `SETTINGS_MAX_CONCURRENT_STREAMS`），就用它。**不要拿它当"能不能开"的唯一
   * 判据** —— 它报的是一个快照，`open_stream()` 仍可能因为别的原因失败。
   */
  uint64_t streams_left(bool bidi) const;

  /**
   * @brief 关掉整条连接。
   * @param error_code 0 = 正常关闭；非 0 = 应用层错误码，会发给对端。
   * @return 0 = 已受理（CONNECTION_CLOSE 已发出）；`UV_ENOTCONN` = 这条连接
   *         还没有内核（没挂到端点上），或已经在关闭了。
   *
   * 之后 `state()` 到 `CLOSING`，等对端确认（或到期）后到 `CLOSED` 并跑一次
   * `on_close`。这段时间里 RFC 9000 §10.2.1 允许重发 CONNECTION_CLOSE，本层
   * 会重发有限次（丢了终端包就永远关不掉是真实存在的 —— 那条 UDP 口上谁都不
   * 替我们重传）。
   */
  int close(int error_code = 0);

  /**
   * @brief 端点侧接口 —— `uvcpp_quic_client` / `uvcpp_quic_server` 专用。
   *
   * **使用者不需要它，也不该调它**（参数里那个 `quic_detail::quic_session`
   * 是私有头里的类型，使用者手上根本没有那种东西）。
   *
   * 为什么单独做成一个嵌套类型，而不是把 `attach` / `session` 直接摊在公开面上：
   * **嵌套类自带外围类的访问权**（C++11 起，嵌套类与外围绕的成员访问权相同），
   * 于是这两个函数能碰 `impl_`，而 `uvcpp_quic_connection` 的公开面里不必多出
   * 两个写着"别调我"的方法。做成 `endpoint::attach(...)` 这个形状，还让
   * "哪些接口是给端点的"在调用点上就一眼可查 ——
   * `uvcpp_quic_connection::endpoint::session(*c)` 自报家门。
   *
   * **不能用 `friend class uvcpp_quic_client;`**：友元不传递。端点的接线代码
   * 写在 `uvcpp_quic_client::impl` 那几个方法里，而它们是 `impl`（一个嵌套类）
   * 的成员，不是 `uvcpp_quic_client` 的成员 —— 也就是说那个友元声明对它们
   * **无效**，编译期直接"is private within this context"。
   */
  struct endpoint {
    /**
     * @brief 端点侧关心的 CID 变化 —— 只有服务端会填。
     *
     * 服务端的一条 UDP 口上跑着很多条连接，它靠一张「CID → 连接」的路由表把入包
     * 分派出去，而那张表的键**只能从这里来**：一条连接的 CID 什么时候多一个、
     * 什么时候少一个，是协议知道的事。
     *
     * @note 客户端留空是对的，不是漏了：它只有一条连接、一个固定对端，没有表可
     *       维护（见 `uvcpp_quic_client.cpp` 里 `attach` 那一处）。
     */
    struct hooks {
      /// 连接多了一个可用于路由的 CID（含**建连接时**那两个，见内核
      /// `on_new_cid` 的 `@note`）。`cid` 是裸字节，因为路由表要拿它当键。
      std::function<void(uvcpp_quic_connection&, const std::string& cid)>
          on_new_cid;
      /// 一个 CID 退休了，表里该摘掉它。
      std::function<void(uvcpp_quic_connection&, const std::string& cid)>
          on_remove_cid;
    };

    /**
     * @brief 把内核挂上本壳（**接管所有权**）。
     *
     * @param session 内核；`nullptr` = 摘掉（先析构旧的）。
     * @param on_closed 连接终结时的第二跳：本壳先跑使用者的 `on_close`，再跑
     *        它。端点在这儿标记"这条可以销毁了"，**不能在这里直接 delete** ——
     *        这一跳是从内核的回调栈里出来的，而栈上还压着 `read_pkt` /
     *        `on_expiry`，它们回来以后还要用那个内核。
     * @param hooks 端点侧的路由钩子（见上）。客户端不填。
     */
    static void attach(
        uvcpp_quic_connection& c, quic_detail::quic_session* session,
        std::function<void(uvcpp_quic_connection&, int)> on_closed,
        const hooks& h = hooks());

    /** @brief 拿到内核（收发数据报、问下次到期都靠它）。可能为 `nullptr`。 */
    static quic_detail::quic_session* session(uvcpp_quic_connection& c);
  };

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE

#endif  // SRC_QUIC_UVCPP_QUIC_CONNECTION_H
