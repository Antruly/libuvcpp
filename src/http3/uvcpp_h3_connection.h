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
class UVCPP_API uvcpp_h3_wt_session;
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
  /**
   * @brief `:protocol`（extended CONNECT，RFC 8441 / RFC 9220，#53）。
   *
   * 非空时 `send_request()` 会把 `:method` 钉成 `CONNECT` 并带上这个伪头；
   * 服务端在 `on_request` 里读它（例如 `"websocket"`）。**本层只做协议形状**，
   * 这条 CONNECT 流上的应用数据由调用方自己读写（`h3_connection` 的
   * `on_body` / `write_stream` 那一侧）。
   */
  std::string protocol;
  /// 普通头，按收到/要发的顺序。
  std::vector<h3_header> headers;
  /// 请求体。**本层会在内存里攒满整个体**（上限 `H3_DEFAULT_MAX_BODY_BYTES`），
  /// 不做流式 —— 本批没有流式请求体那一档。
  std::string body;
  /// trailers（#53）。收到时是本层填的；发送时 `send_request()` 会在请求体
  /// 之后把它们提交出去。
  std::vector<h3_header> trailers;
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
  /// trailers（#53）。服务端填它，`send_response()` 会在响应体之后提交出去。
  std::vector<h3_header> trailers;
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

    /**
     * @brief 收到一条 **extended CONNECT** 请求（`:protocol` 非空，#53）。
     *
     * 与 `on_request` 的区别：这条流不会读侧收场（CONNECT 的数据方向要保持
     * 开着），所以只有**头块收全**这一刻能交给应用；`req.protocol` 是协议名
     * （例如 `"websocket"`）。应用应当在这里建立会话并回 2xx。
     */
    std::function<void(uvcpp_h3_connection&, const h3_request&)>
        on_connect_request;

    /**
     * @brief 收到一条 **HTTP/3 Datagram**（RFC 9297）。
     *
     * @param stream_id 它关联的**请求流**（RFC 9297 §2.1 的 Quarter Stream ID
     *        还原出来的那一条；只有 extended CONNECT 流会走这里）。
     * @param data      payload（四分之一流号那一段已经剥掉）。**只在回调期间有效。**
     *
     * 只有**双方都宣告了 `SETTINGS_H3_DATAGRAM`**、且关联流是 extended CONNECT
     * 时才会交付；其余一律丢弃并计入 `dropped_datagrams()`。
     */
    std::function<void(uvcpp_h3_connection&, int64_t stream_id,
                       const uint8_t* data, size_t datalen)>
        on_datagram;

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

    /**
     * @brief 对端发了 GOAWAY（#53）。
     *
     * @param id 客户端侧是流号：`id` 及以上的流对端保证不会再处理；值为
     *           `NGHTTP3_SHUTDOWN_NOTICE_STREAM_ID` 时只是 shutdown notice
     *           （在途流照常）。服务端侧是 push id（本层不推，通常为 0）。
     *
     * 可能多次回调，`id` 只会不变或变小。
     */
    std::function<void(uvcpp_h3_connection&, int64_t id)> on_goaway;

    /// 收到对端的 SETTINGS（#53）。`settings` 是对端宣告的 h3/QPACK 参数。
    std::function<void(uvcpp_h3_connection&,
                       const uvcpp_h3_remote_settings& settings)>
        on_remote_settings;

    /// 一条因 QPACK 同步被阻塞的流解除了，`consumed` 是放开的字节数（#53）。
    std::function<void(uvcpp_h3_connection&, int64_t stream_id,
                       size_t consumed)>
        on_deferred_consume;
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
   *        **这时 `resp.trailers` 会被忽略**：写方向在头块之后就已经结束，
   *        再提交一个尾随块会被 nghttp3 拒；head-only 的响应也没有"末块之后"
   *        那个位置。
   * @return 0 成功；负值是失败码（`UV_EMSGSIZE` 头块超了我们自己的上限，
   *         其余是 nghttp3 的错误码）。
   *
   * @note `resp.trailers` 非空时，即使 body 为空也会给 nghttp3 一个 data
   *       reader（RFC 9114 §4.1 的第二个 HEADERS 块是合法收尾），所以
   *       "只有尾随头、没有 DATA" 的响应在这条腿上成立。
   */
  int send_response(const h3_response& resp, bool omit_body = false);

  /// 只发 `:status` 的最小响应 + `flush()`。
  int send_status(int64_t stream_id, int status,
                  const std::string& body = std::string());

  /**
   * @brief 提交 trailers（#53），并立刻冲出去。
   *
   * 必须在 `send_response()` / `send_request()` **之后**调；nghttp3 保证它排在
   * 响应体之后。**调用即意味着该方向的发送结束**（RFC 9114 §4.1）。
   *
   * @return 0 成功；`UV_ENOTCONN` 连接没了；`UV_EINVAL` 空名字/空列表；
   *         `UV_EMSGSIZE` 头块超上限；其余是 nghttp3 错误码。
   */
  int send_trailers(int64_t stream_id, const std::vector<h3_header>& trailers);

  // -----------------------------------------------------------------
  // 活流（extended CONNECT / WebSocket over h3，#53）
  // -----------------------------------------------------------------

  /** @brief 提交一个活流响应（服务端 extended CONNECT 的 2xx）。 */
  int send_stream_response(int64_t stream_id,
                           const std::vector<h3_header>& headers);

  /** @brief 客户端：开一条活流请求（extended CONNECT）。返回流号或负错误码。 */
  int64_t send_stream_request(const h3_request& req);

  // -----------------------------------------------------------------
  // HTTP/3 Datagram（RFC 9297）
  // -----------------------------------------------------------------

  /**
   * @brief 宣传 `SETTINGS_H3_DATAGRAM = 1`。**必须在 `start()` 之前调**。
   *
   * 默认 OFF。打开之后：本端愿意收 H3 datagram，且**对端也宣告**之后
   * `send_datagram()` 才可用（见 `peer_h3_datagram()`）。
   */
  void set_h3_datagram_enabled(bool on);
  bool h3_datagram_enabled() const { return h3_datagram_enabled_; }

  /// 对端宣告了 `SETTINGS_H3_DATAGRAM`（且本端也开着）。
  bool peer_h3_datagram() const;

  /**
   * @brief 在一条 **extended CONNECT** 流上发一条不可靠数据报（RFC 9297）。
   *
   * 帧格式：QUIC DATAGRAM（RFC 9221）的载荷 = `Quarter Stream ID` 变长整数
   * （`stream_id / 4`）+ 应用 payload。**不可靠、无顺序、无流控** —— 与 QUIC
   * DATAGRAM 同一条语义。
   *
   * @return 0 已受理（`flush()` 真正上线）；`UV_ENOTSUP` 本端没开 / 对端没宣告 /
   *         对端 QUIC 层没开 DATAGRAM；`UV_EINVAL` 这条流不存在或不是 extended
   *         CONNECT；`UV_EMSGSIZE` 超过对端 `max_datagram_size()`；
   *         `UV_ENOBUFS` QUIC 层发送队列满。
   */
  int send_datagram(int64_t stream_id, const void* data, size_t len);

  /// 一条 H3 datagram 的**总帧上限**（quarter id + payload）；0 = 不可用。
  size_t max_datagram_size() const;

  /// 因流不存在 / 不是 CONNECT / 未协商而被丢弃的入站 datagram 条数。
  uint64_t dropped_datagrams() const { return dropped_datagrams_; }

  /** @brief 往活流里写字节（发送方向保持开着）。 */
  int write_stream_data(int64_t stream_id, const char* data, size_t len);

  /** @brief 关闭活流的发送方向（FIN）。 */
  int end_stream_data(int64_t stream_id);

  /**
   * @brief 把一条流标记成活流，并安装 DATA / 读侧结束的落点。
   *
   * 安装之后这条流的 DATA 不再进 `req.body` / `resp.body`，而是逐块交给
   * `on_data`；读方向结束（对端 FIN / reset）交给 `on_end`。**至多装一次。**
   */
  void set_stream_sink(int64_t stream_id,
                       std::function<void(const char*, size_t)> on_data,
                       std::function<void()> on_end);

  /**
   * @brief 把会话里待发的字节全部写出去。可以重复调（没东西发就是空操作）。
   *
   * **`recv_stream_data()` 返回之后本层一定会自己冲一轮**，调用方不需要在
   * 每个回调里记着它 —— 规矩落在本层内部，见会话层类注释里那段。
   */
  int flush();

  // -----------------------------------------------------------------
  // 平滑退场（GOAWAY，#53）
  // -----------------------------------------------------------------

  /**
   * @brief 发**第一封** GOAWAY（shutdown notice）：对端别再开新流，在途流照常。
   *
   * 返回 0 或错误码；内部已经 `flush()` 过，GOAWAY 帧随下一批字节出去。
   * 隔几个 RTT 之后调 `shutdown()` 发最终那封。
   */
  int shutdown_notice();

  /** @brief 发最终 GOAWAY：本端不再接受新流，已有流跑完。 */
  int shutdown();

  /** @brief 本端所有流都收场了吗（可以安全关 QUIC 连接）。 */
  bool is_drained() const;

  /** @brief 对端发过 GOAWAY 没有（notice 也算）。 */
  bool peer_goaway_received() const;

  /** @brief 对端最近一封 GOAWAY 的 `id`；没收到过时是 0。 */
  int64_t peer_goaway_last_stream_id() const;

  // -----------------------------------------------------------------
  // QPACK / 对端 SETTINGS 观测（#53）
  // -----------------------------------------------------------------

  /** @brief 收到过对端的 SETTINGS 没有。 */
  bool has_remote_settings() const;

  /** @brief 对端宣告的 h3/QPACK 参数（没收到过时全 0）。 */
  uvcpp_h3_remote_settings remote_settings() const;

  /** @brief 因 QPACK 同步被阻塞的流解除过几次（`deferred_consume` 次数）。 */
  uint64_t blocked_stream_events() const;

  /** @brief 这些解除一共放开了多少字节。 */
  uint64_t blocked_stream_bytes() const;

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

  /**
   * @brief 存活令牌：本对象析构之后过期（`lock()` 出来是空，或指向 1）。
   *
   * 给**不拥有本对象**、却持有 `uvcpp_h3_connection*` 的外层会话用 ——
   * `uvcpp_h3_ws_connection`（RFC 9220）就是这一类：它析构时要回头摘流上的
   * 落点，而"连接比它先没"是合法时序。与
   * `uvcpp_h2_connection::life_token()` 同形，用法也一样。
   */
  std::weak_ptr<const char> life_token() {
    return std::weak_ptr<const char>(alive_token());
  }

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
    /// **活流**（extended CONNECT，#53）：DATA 直接进 `sink_data`，不再往
    /// `req.body` / `resp.body` 里攒；读侧结束走 `sink_end`。
    bool raw = false;
    /// 这条流允许 HTTP/3 Datagram（RFC 9297）：extended CONNECT 请求。服务端在
    /// `on_connect_request` 那一刻置位、客户端在提交请求时置位；普通请求一律为假
    /// （RFC 9297 §3 只给 CONNECT 免掉 `Datagram-Flow-Id` 那一套）。
    bool datagram_ok = false;
    /// 收到的 body 字节数与对端宣告的 `content-length`（两个方向共用，
    /// 方向由 `server_side_` 决定）。RFC 9114 §4.1.2 / §4.2：对不上是流错误
    /// （`H3_MESSAGE_ERROR`）。
    size_t body_bytes        = 0;
    size_t expected_body     = 0;
    bool   has_content_length = false;
    /// 长度已经判定非法：这条流不再交付给业务层。
    bool   body_bad          = false;
    /// WebTransport 流所属的会话（**会话流号**；-1 = 不是 WT 流）。见
    /// `uvcpp_h3_wt_session`。
    int64_t wt_session = -1;
    bool    wt_bidi    = false;
    std::function<void(const char*, size_t)> sink_data;
    std::function<void()>                     sink_end;
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
  /// QUIC DATAGRAM 的落点：剥掉 `Quarter Stream ID`，校验关联流后交付。
  void on_datagram(const uint8_t* data, size_t datalen);

  // -----------------------------------------------------------------
  // WebTransport（draft-ietf-webtrans-http3-13）
  // -----------------------------------------------------------------
  friend class uvcpp_h3_wt_session;
  friend class uvcpp_h3_wt_stream;

  /// 一条**新出现的对端流**在读方向上的类型探测（WT 还是 HTTP）。
  struct wt_probe {
    std::string buf;
    bool        bidi = false;
    bool        fin  = false;
  };

  /// 新流分流：0 = 普通 HTTP 流（调用方照旧喂 nghttp3）；1 = 已处理；
  /// 2 = 字节还不够，已缓冲；<0 = 协议错（已 reset）。
  int route_new_stream(int64_t stream_id, const char* data, size_t len,
                       bool fin);
  bool is_peer_bidi(int64_t id) const;
  bool is_peer_uni(int64_t id) const;

  /// 盯一条 CONNECT 流的响应状态（WebTransport 客户端等 2xx）。
  void watch_wt_response(int64_t stream_id,
                         std::function<void(int status, int error)> cb);

  void  attach_wt_session(int64_t session_sid, uvcpp_h3_wt_session* s);
  void  detach_wt_session(int64_t session_sid, uvcpp_h3_wt_session* s);
  /// 开一条 WT 流并写入 `类型 + 会话流号` 前缀；返回流号或负错误码。
  int64_t open_wt_stream(int64_t session_sid, bool bidi);
  int  write_wt_stream(int64_t sid, const char* data, size_t len, bool fin);
  int  shutdown_wt_stream(int64_t sid, uint64_t app_error_code);
  int  shutdown_wt_read(int64_t sid, uint64_t app_error_code);
  void erase_wt_stream(int64_t sid);

  std::map<int64_t, wt_probe>            wt_probes_;
  std::map<int64_t, uvcpp_h3_wt_session*> wt_sessions_;
  /// WebTransport 客户端：会话在等 CONNECT 的响应（2xx）。`complete_response`
  /// 兑现一次并摘掉。
  std::map<int64_t, std::function<void(int, int)> > wt_response_watchers_;

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

  /// 对端 GOAWAY 的最后状态（notice 也记；id 只会不变或变小）。
  bool    peer_goaway_      = false;
  int64_t peer_goaway_id_   = 0;
  /// 在 nghttp3 的调用栈里调了 `shutdown_notice()` / `shutdown()`：推迟到
  /// 栈退干净之后的 `flush()` 里再兑现（写函数禁止重入）。
  bool shutdown_notice_pending_ = false;
  bool shutdown_pending_        = false;

  /// 对端 SETTINGS（#53）。
  bool                    have_remote_settings_ = false;
  uvcpp_h3_remote_settings remote_settings_;
  /// 本端是否宣告了 `SETTINGS_H3_DATAGRAM`（RFC 9297）。
  bool     h3_datagram_enabled_ = false;
  /// 被丢弃的入站 datagram（流不存在 / 不是 CONNECT / 未协商）。
  uint64_t dropped_datagrams_   = 0;
  /// `deferred_consume` 的累计账（只做观测，不拿来还 QUIC 流控）。
  uint64_t blocked_stream_events_ = 0;
  uint64_t blocked_stream_bytes_  = 0;

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
