/**
 * @file src/http3/uvcpp_h3_connection.h
 * @brief 把 h3 会话接到一条**已经握手完、ALPN 协商出 h3** 的 QUIC 连接上。
 * @author zhuweiye
 * @version 1.4.1
 *
 * 分工与 `src/http2/` 那边逐字相同：`uvcpp_h3_session` 只管协议（收字节 /
 * 吐字节），本类管**字节从哪来、往哪去** —— 三条关键单向流的开启、
 * `drain()` 出来的段交给 `write_stream()`、以及"一次 `write_stream()` 对应
 * 一次 `on_write`"这条契约的落地。
 *
 * 与 h2 那一层的三处**实质**差别（都是 QUIC 带来的，不是风格差异）：
 *
 * 1. **没有 socket。** 底下是 `uvcpp_quic_connection`，一个对象背后是一条 QUIC
 *    连接（可能跨好几条路径），不是一条 TCP 连接。本类**不拥有**它。
 * 2. **有"关键单向流"。** h3 的控制流与两条 QPACK 流是**握手完就要开出来**的，
 *    而 QUIC 的流额度可能还没到（`NGTCP2_ERR_STREAM_ID_BLOCKED`）。所以有
 *    `ready()` 这一位，和一条"额度来了再补开"的路。
 * 3. **流的收场要自己合成。** QUIC 的公开面没有"某条流关了"这个事件（它按
 *    方向报：读侧归 `on_read`、写侧归 `on_write`），而 nghttp3 **不会**自己
 *    摘掉一条流 —— `nghttp3_conn_close_stream()` 必须由应用在"两个方向都收场
 *    了"的那一刻调。所以本类按方向记账，两边齐了再告诉会话。
 *
 * **本头是公开的**，而且**不含** `<nghttp3/nghttp3.h>`、也不含
 * `uvcpp_h3_session.h`（那两个是私有头）—— 使用者拿到的 h3 面里没有 nghttp3
 * 的类型，也不需要配它的头搜索路径。
 */

#ifndef SRC_HTTP3_UVCPP_H3_CONNECTION_H
#define SRC_HTTP3_UVCPP_H3_CONNECTION_H

#include <uvcpp/uvcpp_config.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>

#include "http3/uvcpp_h3_common.h"

#if UVCPP_HTTP3_ENABLE

namespace uvcpp {

class uvcpp_quic_connection;  // 见 src/quic/uvcpp_quic_connection.h
// 私有头里的会话内核。在这里出现只是为了让 `session_` 有个类型名 —— 使用者
// 不需要它（那个类根本没装出去）。**属性要与定义处逐字相同**，否则 MSVC 会在
// "先声明不带 `__declspec`、定义带"这件事上报 C4273。
class UVCPP_API uvcpp_h3_session;
struct net_read_result;

// =========================================================================
// 一次请求 / 一次响应
// =========================================================================

/**
 * @brief 一条 h3 请求。
 *
 * 伪头拆成**具名成员**（`method` / `scheme` / `authority` / `path`），普通头留在
 * `headers` 里 —— 与 `uvcpp_http_request` 对 h1/h2 的做法一致：伪头是每个使用
 * 者都要读的那四个，没人愿意在一条列表里做字符串比较。
 *
 * 反方向（发出去）也是同一个结构：填好这四栏 + `headers`，`send_request()` 会
 * 按 RFC 9114 §4.3.1 的顺序拼成伪头在前、普通头在后的头块。**调用方不要**自己
 * 往 `headers` 里塞伪头 —— 那条列表里带 `:` 开头的项会被原样发出去，而 h3 要求
 * 伪头必须自成一段。
 */
struct UVCPP_API h3_request {
  /// 这条请求在哪条流上。发出去时**不用填**（`send_request()` 自己开流并写回
  /// 去把它盖掉）；收到时由本层填好。
  int64_t stream_id = -1;
  std::string method;
  /// 缺省 `"https"` —— h3 跑在 QUIC 上，QUIC 只有加密那一档，所以这不是个
  /// "猜的值"，是这一层唯一可能的取值。
  std::string scheme;
  /// `:authority`。为空时**不发**这个伪头（RFC 9114 允许省，对端会退回看
  /// `host`）；发布出去的场景请填上。
  std::string authority;
  std::string path;
  /// 普通头，按收到/要发的顺序。
  std::vector<h3_header> headers;
  /// 请求体。**本层会在内存里攒满整个体**（上限 `H3_DEFAULT_MAX_BODY_BYTES`），
  /// 不做流式 —— 本批没有流式请求体那一档。
  std::string body;
};

/**
 * @brief 一条 h3 响应。
 *
 * 同一形状两用：服务端填它去 `send_response()`；客户端从 `take_completed()`
 * 里拿它。
 */
struct UVCPP_API h3_response {
  int64_t stream_id = -1;
  /// `:status`。0 表示**没拿到**（`error` 非 0 时就是这样）。
  int status = 0;
  std::vector<h3_header> headers;
  std::string body;
  /**
   * @brief 这次请求的收场：0 = 正常；非 0 = 没能干净收场。
   *
   * 取值是 `UV_ECANCELED`（对端 reset、写方向被 STOP_SENDING 掐掉、或连接在
   * 响应收全之前就没了）或 h3 的应用错误码。**有它才不会挂死调用方** ——
   * 一条失败的请求也要在 `take_completed()` 里出现一次，否则"取不到就是还没
   * 回来"与"取不到就是永远回不来了"这两件事分不开。
   */
  int error = 0;
};

// =========================================================================
// 连接
// =========================================================================

/**
 * @brief 一条 h3 连接的驱动层。
 *
 * **不拥有** `uvcpp_quic_connection` —— 那个属于 `uvcpp_quic_client` /
 * `uvcpp_quic_server`。本类只在它还在的时候用它；持有者必须在 QUIC 连接被端点
 * 释放之前销毁本对象（`on_disconnect` 就是那个时机）。
 *
 * **线程**：只能在 loop 线程上用（与底下那条 QUIC 连接同一条）。
 */
class UVCPP_API uvcpp_h3_connection {
 public:
  /**
   * @brief 回调集合。
   *
   * 每个回调都在 **loop 线程**上被调用，而且**很可能在某个内部调用还没返回的
   * 时候**跑起来（`recv_stream_data()` 会同步回调进用户代码）。回调里可以
   * `send_response()` / `close()` —— 那些是允许的；但**返回之后不要再碰本
   * 对象**这件事，在 `on_disconnect` 上是硬要求（见那一条的说明）。
   */
  struct callbacks {
    /**
     * @brief 收到一条**完整的**请求（服务端侧才有意义）。
     *
     * 触发点是这条流的**读方向收场**，不是"头块收全"—— 于是拿到它的时候
     * `req.body` 一定是全的，不需要再等一次。
     *
     * 客户端侧不想被调就留空。
     */
    std::function<void(uvcpp_h3_connection&, const h3_request&)> on_request;

    /// 一条流关掉了（两个方向都收场之后）。`info` 里两个方向的原因码是分开的。
    std::function<void(uvcpp_h3_connection&, const h3_stream_close_info&)>
        on_stream_close;

    /**
     * @brief 连接级致命错误：本层已经没法在这条连接上继续了。
     *
     * @param error_code 按**符号**分两种意思（与 `uvcpp_quic_connection::on_close`
     *        同一条规矩）：`> 0` 是要发给对端的 h3 应用错误码；
     *        `< 0` 是本端的失败码（nghttp3 / ngtcp2 / libuv）。
     *
     * 本层收到之后**会自己把 QUIC 连接关掉**（用那个应用错误码，那是唯一正确
     * 的动作），所以调用方通常只需要记日志。随后一定会收到一次 `on_disconnect`。
     */
    std::function<void(uvcpp_h3_connection&, int error_code)> on_error;

    /**
     * @brief 底层 QUIC 连接结束了。**恰好一次。**
     *
     * 回调返回后**不要**再碰本对象 —— 持有者应当在这里把它销毁。
     */
    std::function<void(uvcpp_h3_connection&)> on_disconnect;
  };

  /**
   * @param conn        已经（或即将）握手完的 QUIC 连接。**生命周期必须覆盖本
   *                    对象**，本对象不拥有它、也不会销毁它。
   * @param server_side true 建服务端会话（等对端提请求）。
   */
  uvcpp_h3_connection(uvcpp_quic_connection* conn, bool server_side);
  ~uvcpp_h3_connection();

  uvcpp_h3_connection(const uvcpp_h3_connection&)            = delete;
  uvcpp_h3_connection& operator=(const uvcpp_h3_connection&) = delete;

  /**
   * @brief 装回调、建会话、装 QUIC 回调，并把首轮待发字节发出去。
   *
   * 三条关键单向流**不在这里开** —— 它们要等 ALPN（`on_alpn` 回调）。所以
   * `start()` 之后 `ready()` 还是假，这是正常的，不是失败。
   *
   * @return 0 成功；`UV_EINVAL` 没给 QUIC 连接；负值是 nghttp3 的错误码。
   */
  int start(const callbacks& cbs);

  // -----------------------------------------------------------------
  // 提问 / 答复
  // -----------------------------------------------------------------

  /**
   * @brief 客户端：开一条双向流、提交请求、立刻冲出去。
   *
   * @return 成功返回**流号**（拿它去 `take_completed()` 的结果里对号）；负值是
   *         失败码 —— `UV_EAGAIN` 表示关键流还没开出来（`ready()` 还是假），
   *         `UV_ENOTCONN` 表示连接已经没了，其余是 nghttp3 / ngtcp2 的错误码
   *         （`NGTCP2_ERR_STREAM_ID_BLOCKED` 是其中一种**正常**结果：对端还没
   *         放开双向流额度）。
   *
   * `req.body` 会被**移走**（这是有意的：nghttp3 要求那些字节活到对端确认，
   * 与其在两层之间拷一份，不如把所有权直接交给它）。所以这里收的是
   * `h3_request&` 而不是 `const&`……但为了让常见写法（传一个临时对象）仍然能
   * 编译，签名收的是 `const h3_request&`，内部**拷一份**。一次请求一次拷贝，
   * 这是本批愿意付的代价。
   */
  int64_t send_request(const h3_request& req);

  /**
   * @brief 客户端：取一条**已经收全**的响应。
   *
   * **每次调用至多给一条**，取不到就返回 false。这个"一次一条"是刻意的：
   * 队列里可能有几条同时到齐，回调里一次性抽干会让调用方在处理第一条时看不见
   * 第二条的到达，于是"这次回调要不要再排一次"这个问题永远答不准。
   *
   * 一条请求**恰好**在这里出现一次：正常收全是一次（`error == 0`），中途被
   * reset / 连接断掉也是一次（`error != 0`）。不会两次，也不会零次。
   */
  bool take_completed(h3_response& out);

  /// 队列里还积着几条。`take_completed()` 取不走的东西只有它自己知道。
  size_t completed_count() const { return completed_.size(); }

  /**
   * @brief 服务端：提交响应并立刻冲出去。
   *
   * @param omit_body true = 只发头块（HEAD / 204 / 304 走这一支）。
   * @return 0 成功；负值是失败码（`UV_EMSGSIZE` 头块超了我们自己的上限，
   *         其余是 nghttp3 的错误码）。
   */
  int send_response(const h3_response& resp, bool omit_body = false);

  /// 只发 `:status` 的最小响应 + `flush()`。
  int send_status(int64_t stream_id, int status,
                  const std::string& body = std::string());

  /**
   * @brief 把会话里待发的字节全部写出去。可以重复调（没东西发就是空操作）。
   *
   * **`recv_stream_data()` 返回之后本层一定会自己冲一轮**，调用方不需要在
   * 每个回调里记着它 —— 规矩落在本层内部，见会话层类注释里那段。
   */
  int flush();

  // -----------------------------------------------------------------
  // 收场与查询
  // -----------------------------------------------------------------

  /// 关掉整条 QUIC 连接（把 `error_code` 发给对端）。0 = 干净关闭。
  int close(int error_code = 0);

  /// 底下那条 QUIC 连接。**不转让所有权**。
  uvcpp_quic_connection* quic() const { return conn_; }
  /// 本端是服务端侧。
  bool server_side() const { return server_side_; }

  /**
   * @brief 三条关键单向流都开出来并绑好了 —— 现在可以发请求 / 答复了。
   *
   * 在 `on_alpn` 之后才可能变真。**握手完成不等于它变真**（额度可能还没到）。
   */
  bool ready() const { return ready_; }
  /// 底层连接已经结束（`on_disconnect` 之前就为真）。
  bool closed() const { return closed_; }

  /// 协商出来的 ALPN（握手完成前是空串）。
  std::string alpn_selected() const { return alpn_; }

  /// 三条关键流各自的流号；还没开出来时返回 -1。
  int64_t control_stream_id() const { return ctl_sid_; }
  int64_t qpack_encoder_stream_id() const { return qenc_sid_; }
  int64_t qpack_decoder_stream_id() const { return qdec_sid_; }

  /// 从 QUIC 收进来的字节数 / 写进 QUIC 的字节数，供测试与诊断用。
  size_t bytes_in() const { return bytes_in_; }
  size_t bytes_out() const { return bytes_out_; }

  /// 编进来的 nghttp3 版本串（形如 `"1.18.0"`）。
  static std::string nghttp3_version();

 private:
  /// 连接层对一条流的账。
  ///
  /// **与会话层那张表不是一回事**：会话层记的是协议状态（头块预算、待发 body），
  /// 这里记的是**QUIC 那两个方向各自收场了没有** —— 那是"什么时候该告诉
  /// nghttp3 这条流关了"的唯一判据（见文件头第 3 条）。
  struct stream_rec {
    bool     local        = false;  ///< 本端发起的流
    bool     request      = false;  ///< 双向请求/响应流（关键单向流为假）
    bool     fin_delivered = false; ///< FIN 已经交给 nghttp3 了（免得交两次）
    /// 读侧的**QUIC 通知**已经收场（最后一个事件到了）。
    ///
    /// **这一位不能拿 `rx_done` 顶替。** `rx_done` 是 nghttp3 说的"读方向完了"，
    /// 它**先于**读侧的最后一个 QUIC 通知到：QUIC 层把"这块数据"和"这块之后
    /// FIN"拆成两次通知（`uvcpp_quic_connection.cpp:99-138` 的合约是**先 DATA
    /// 再 PEER_CLOSED**），而 nghttp3 在**第一次**（`DATA && fin`）就知道读方向
    /// 完了。于是 `rx_done && tx_done` 在第二次通知还没来之前就成立了，
    /// `close_stream()` 会把这条流从 nghttp3 里删掉，紧接着那第二次通知（一个
    /// 零长度、只带 FIN 的 `read_stream2`）落到一条**不存在的流**上 ——
    /// nghttp3 报 `H3_STREAM_CREATION_ERROR`（-609），整条连接以
    /// `H3_FRAME_UNEXPECTED`(0x0103) 收场。
    ///
    /// 所以关闭的条件是**三条**：`rx_done && tx_done && rx_settled`。
    bool     rx_settled   = false;
    bool     rx_done      = false;
    bool     tx_done      = false;
    bool     rx_error     = false;
    bool     tx_error     = false;
    uint64_t rx_code      = 0;
    uint64_t tx_code      = 0;
    /// 服务端侧：这条请求收全、交给用户了。**恰好交一次。**
    bool     dispatched   = false;
    /// 服务端侧：请求头/体攒的账。
    h3_request req;
    bool     headers_done = false;
    bool     overflowed   = false;
    /// 客户端侧：这条响应攒的账；`completed` 是"那条响应已经进过完成队列了"。
    h3_response resp;
    bool     completed    = false;
  };

  /// 待确认的写。**这条 FIFO 是承重的**：`on_write` 对每次 `write_stream()`
  /// 恰好一次，但它只说"哪条流"，不说"哪一笔" —— 而 `add_ack_offset()` 要的是
  /// 字节数，且它必须与当初 `add_write_offset()` 的那笔对上。顺序由 QUIC 内核
  /// 的 `op_marks` 保证（FIFO），所以这里按流各存一条队列即可。
  struct pending_write {
    size_t bytes = 0;
    /// 这一笔带了 FIN —— 它被确认就意味着这条流的**写方向**收场了。
    bool   fin   = false;
  };

  void on_alpn(uvcpp_quic_connection& c, const std::string& alpn);
  void on_read(uvcpp_quic_connection& c, int64_t stream_id,
               const net_read_result& r);
  void on_quic_write(uvcpp_quic_connection& c, int64_t stream_id, int status);
  void on_quic_close(uvcpp_quic_connection& c, int error_code);
  void on_streams_available(uvcpp_quic_connection& c, bool bidi,
                            uint64_t max_streams);
  void on_stop_sending(uvcpp_quic_connection& c, int64_t stream_id,
                       uint64_t app_error_code);

  /// 装会话回调并建出会话（`session_->init()` 也在里面）。拆出来只是为了让
  /// `start()` 读起来短一点。
  void wire_session_callbacks();

  /// 尽力把还没开出来的关键流开出来；额度没到就记着，等 `on_streams_available`。
  void try_open_critical_streams();

  /// 一个方向收场之后：两个都齐了就**排进**待关队列。
  ///
  /// **不在这里直接关。** 这条判断有两个入口，其中一个（读方向）是在
  /// nghttp3 的回调里判的，而 `nghttp3_conn_close_stream2()` 会把正在被处理的
  /// 那条流删掉 —— 在它自己的回调栈里删它就是悬垂。所以一律排进
  /// `close_pending_`，由 `pump_closes()` 在 nghttp3 退栈之后兑现。
  void maybe_close_stream(int64_t stream_id);

  /// 把待关队列里的流兑现掉。**只在 nghttp3 栈外调**。
  void pump_closes();

  /// 服务端侧：一条请求收全了，交给用户。**至多交一次**（`dispatched`）。
  void dispatch_request(int64_t stream_id);

  /// 客户端侧：把一条响应放进完成队列（**至多一次**，见 `completed`）。
  void complete_response(stream_rec& s, int error);

  stream_rec& stream_of(int64_t stream_id);
  /// 本端发起的双向流号是 `(sid & 0x3) == 0`；服务端看到的对端双向流是
  /// `(sid & 0x3) == 0` 且不是本地发起的（QUIC 的流号低位编码方向，见 RFC 9000 §2.1）。
  static bool is_bidi(int64_t stream_id) { return (stream_id & 0x3) == 0; }

  // 与 `uvcpp_h2_connection` / `uvcpp_quic_connection` 同一套存活令牌纪律：
  // QUIC 的回调是**稍后**送进来的，而本对象可能在那之前就没了。所有异步入口都
  // 先取一份令牌、调用返回之后再确认自己还活着。
  std::shared_ptr<char> alive_token();
  static bool           token_alive(const std::shared_ptr<char>& token);

  uvcpp_quic_connection*             conn_ = nullptr;
  bool                               server_side_ = false;
  std::shared_ptr<uvcpp_h3_session>  session_;
  callbacks                          cbs_;

  bool        started_  = false;
  bool        ready_    = false;
  bool        closed_   = false;
  bool        fatal_    = false;
  std::string alpn_;

  /// 还没开出来的关键流条数（3 → 0）。额度没到时停在 1..3。
  int      critical_left_ = 3;
  int64_t  ctl_sid_       = -1;
  int64_t  qenc_sid_      = -1;
  int64_t  qdec_sid_      = -1;

  std::map<int64_t, stream_rec>       streams_;
  std::map<int64_t, std::deque<pending_write>> pending_;
  /// 两个方向都收场、等着告诉 nghttp3 的流（见 `maybe_close_stream`）。
  std::deque<int64_t>                 close_pending_;
  std::deque<h3_response>             completed_;

  /// `drain()` 吐出的多段在交给 `write_stream()` 之前拼一下的落点 —— **只在
  /// 单次 `flush()` 期间有效**（nghttp3 的段指针本身也只活那么久）。
  /// 单段时**不用它**（直接把那一段交给 QUIC，省一次拷贝）。
  std::vector<char> wbuf_;

  size_t bytes_in_  = 0;
  size_t bytes_out_ = 0;

  std::shared_ptr<char> alive_token_;
};

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE

#endif  // SRC_HTTP3_UVCPP_H3_CONNECTION_H
