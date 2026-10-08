/**
 * @file src/quic/uvcpp_quic_session.h
 * @brief 私有头：一条连接的**状态机内核** —— 唯一持有 `ngtcp2_conn*` 的地方。
 * @author zhuweiye
 * @version 1.5.1
 *
 * 分层：`uvcpp_quic_connection` 是**公开面**（谁看得见、回调叫什么），本类是
 * **内核**（协议怎么跑）。公开面里一个 ngtcp2 类型都不出现，内核里一个用户可见
 * 的名字都不出现 —— 这条界线让"改公开 API"与"改协议实现"互不牵扯。
 *
 * **这个头不许被任何公开头包含**，理由与 `uvcpp_quic_ngtcp2.h` 逐条同源（它
 * 直接把那个头包含进来了，于是连 `<openssl/ssl.h>` 也一起拖进来）。所以它和
 * `uvcpp_quic_ngtcp2.h` 一样是**私有头**：既不能进
 * `CMakeLists.txt` 的安装列表，也不能进 `package_release.py` 的安装集合。
 *
 * **本类的形状是"喂一个数据报进来 / 让它吐数据报出去 / 问它下次到期什么时候"**，
 * 与 socket、计时器、循环全无关系 —— 那三样归端点（`uvcpp_quic_client` /
 * `uvcpp_quic_server`）。所以本类可以脱离 libuv 单独测（`ngtcp2` 自己也这么分层，
 * 它的 `examples/sim.cc` 就是这么干的基本理由）。
 */

#ifndef SRC_QUIC_UVCPP_QUIC_SESSION_H
#define SRC_QUIC_UVCPP_QUIC_SESSION_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_QUIC_ENABLE

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <uv.h>

#include "quic/uvcpp_quic_common.h"
#include "quic/uvcpp_quic_ngtcp2.h"

namespace uvcpp {

class uvcpp_ssl_context;

namespace quic_detail {

/**
 * @brief 把一段要发出去的字节交给端点去发。
 *
 * @param data 数据报内容。**回调返回后即失效**，要留就自己拷。
 * @param len  字节数（一个 UDP 数据报，绝不是一条流的内容）。
 * @param peer 目的地。QUIC 的首包之前客户端还不知道对端地址，所以这一项由
 *             ngtcp2 每次回填（连接迁移之后它会变）—— **不要自己记地址**。
 * @param peerlen `peer` 的长度。
 *
 * 端点负责把它交给 UDP 口（快路 `uv_udp_try_send`，队列满时才排
 * `uv_udp_send`），并自己管缓冲区的生命周期。本类**不持有** socket，也不回读
 * 任何发送结果：UDP 的发送失败对 QUIC 来说就是丢包，交给重传处理是对的
 * （`uv_udp_try_send` 报 `UV_EAGAIN` 那种也不该当成连接错误）。
 *
 * @note 数据**必须在 `send` 返回前就交给内核**（同步的），因为本类给的缓冲
 *       是栈上的临时数组。这正是快路能不拷贝的前提；慢路自己拷一份来满足它。
 */
using quic_send_fn = ::std::function<void(
    const uint8_t* data, size_t len, const struct sockaddr* peer, int peerlen)>;

/**
 * @brief 内核往外报的事件。
 *
 * 全是**已解析**过的语义（"握手完了"、"ALPN 是这个"、"连接结束了"），不是
 * ngtcp2 的原始回调形状 —— 内核自己把 ngtcp2 那一堆回调都消化掉了，公开面
 * 只该看见这几件"应用真的关心"的事。
 *
 * 每个都在 **loop 线程**上、且都在某个 ngtcp2 回调**内部**被调用。于是：
 *
 * @warning **这些回调里不许再调本类的 `flush()` / `close()`。** ngtcp2 明文
 *          禁止在它的回调里调 `ngtcp2_conn_writev_stream` /
 *          `ngtcp2_conn_write_connection_close`（见 `ngtcp2.h` 里各自的说明）。
 *          本类用一个深度计数（`in_callback_`）守住这条：回调期间这些请求只被
 *          **记下来**，等 `read_pkt()` / `on_expiry()` 返回之前统一执行。
 */
struct quic_session_events {
  /// 握手完成（RFC 9001）。此后才能收发应用数据。
  ::std::function<void()> on_handshake_completed;
  /// ALPN 协商完成，参数是对端选定的协议名。
  ::std::function<void(const std::string&)> on_alpn;
  /// 连接终结。参数的三种含义**按符号分**，不是随便塞了一个数：
  ///   - `> 0`：对端的**应用**错误码（它在 CONNECTION_CLOSE 里给的那个数）；
  ///   - `= 0`：干净关闭（对端或本端以 `NO_ERROR` 收场）；
  ///   - `< 0`：传输层级的原因，值就是 ngtcp2 的错误码 —— `NGTCP2_ERR_DRAINING`
  ///     表示"对端关了我们，原因在传输层"，`NGTCP2_ERR_IDLE_CLOSE` 表示空闲
  ///     超时，其余是本端自己报出来的错。
  ::std::function<void(int)> on_close;

  /// 对端开了一条新流（服务端侧才收得到）。
  ::std::function<void(int64_t stream_id)> on_stream_open;
  /// 某条流上有字节可读。`fin` = 对端在这块之后不再发了（收到了 STREAM 的 FIN）。
  /// `data`/`datalen` **只在本回调期间有效**。
  ::std::function<void(int64_t stream_id, const uint8_t* data, size_t datalen,
                       bool fin)>
      on_stream_data;
  /// 某条流彻底关了（两个方向都关完，或重置）。
  ::std::function<void(int64_t stream_id, uint64_t app_error_code)>
      on_stream_close;
  /// **对端**把这条流的发送方向重置了（收到 RESET_STREAM），\p app_error_code
  /// 是它的应用错误码。
  ///
  /// 这一格与 `on_stream_close` 不是一回事，也不能靠后者推：`on_stream_close`
  /// 要等**两个**方向都收场（`stream_close2`），而对端 reset 之后应用往往还在
  /// 等着收自己那一侧的数据 —— 那个等待永远不会结束。读侧的"没有更多了"必须
  /// 由**这个**事件在它到达的那一刻报出去。
  ///
  /// 本端自己 reset 不触发它（ngtcp2 的回调只报远端发起的 reset），所以这里是
  /// 一条干净的单向信号，不需要调用方再拿 tx 错误码去排歧义。
  ::std::function<void(int64_t stream_id, uint64_t app_error_code)>
      on_stream_reset;
  /// 一次 `write_stream()` 尘埃落定。**一次调用对应一次回调**，顺序与调用
  /// 顺序一致（`stream_send::op_marks` 就是这条 FIFO）。
  ///
  /// @param status 0 = 对端确认了这次调用写入的**全部**字节；非 0
  ///               （`NGTCP2_ERR_STREAM_SHUT_WR`）= 这条流在那之前就没了，
  ///               这些字节**永远不会**到对端。
  ///
  /// 这个回调的必要性和 `uvcpp_tcp_client::write()` 那条一样：
  /// `write_stream()` 是"受理"不是"发出去了"，没有完成回调，调用方就只能靠
  /// 猜来决定什么时候能重用缓冲区。报状态而不是只报成功，是为了**不让调用方
  /// 在流被重置之后永远等一个不会来的回调**。
  ::std::function<void(int64_t stream_id, int status)> on_write;
  /// 本端可以多开 `max_streams` 条本地发起的流（对端放开了流数上限）。
  ::std::function<void(uint64_t max_streams)> on_streams_bidi_available;
  ::std::function<void(uint64_t max_streams)> on_streams_uni_available;

  /// **对端**发了 STOP_SENDING（RFC 9000 §19.5）："这条流你别再发了"。
  /// `app_error_code` 是那个帧里带的。
  ///
  /// 与 `on_stream_reset` 方向相反、也**不会**互相替代：reset 报"对端不发了"
  /// （我这一侧读完了），这一格报"对端不要我发了"（我这一侧发不动了）。一次
  /// 对端取消在线上是两个帧，两条通知各来一次是正常的。
  ///
  /// 本端收到它**不自动回 RESET_STREAM** —— 那要发一个帧，而这是应用层的判断
  /// （见 `fill_callbacks` 里那一格的长注释）。
  ::std::function<void(int64_t stream_id, uint64_t app_error_code)>
      on_stop_sending;

  /// ngtcp2 让本端**新发一个 CID** 给对端用（RFC 9000 §5.1.1）。端点必须把它
  /// 连同已有的那几个一起加进路由表 —— 服务端的一条 UDP 口上，**同一个连接的
  /// 每一个 CID 都得指回同一个连接对象**，漏一个就会在"对端换 CID"之后把后续
  /// 包当成新连接（那会表现成一条莫名其妙的第二条连接，而不是丢包）。
  ///
  /// `cid`/`token` 只在本回调期间有效。`token` 是无状态重置令牌，可能为 0 长。
  ///
  /// @note **建连接时也会响**（不是只有 ngtcp2 后来发 NEW_CONNECTION_ID 时才响）：
  ///       `init_server()` 成功后会为**本端 SCID**与**对端首包的原始 DCID**各报
  ///       一次。不这样的话，路由表在握手期间是空的 —— 而对端那段时间正拿原始
  ///       DCID 发包，一个都路由不出去。
  ::std::function<void(const uint8_t* cid, size_t cidlen, const uint8_t* token,
                       size_t tokenlen)>
      on_new_cid;

  /// 一个 CID **退休**了（对端 retire，或本端换新）。端点从路由表摘掉它。
  /// 摘掉之前收到的包仍会被 ngtcp2 丢掉，所以这里晚一步不是错，只是白留一段。
  ::std::function<void(const uint8_t* cid, size_t cidlen)> on_remove_cid;
};

/**
 * @brief 一条 QUIC 连接的状态机内核。
 *
 * **生命周期**：`init_client()` / `init_server()` 二选一调一次（成功之后不能
 * 再调），`ngtcp2_conn` 在那个时候建出来；析构按 OpenSSL → ngtcp2 的顺序收掉。
 * 默认构造出来、没 init 过的对象是"不存在"的，所有动作返回 `UV_ENOTCONN`。
 */
class quic_session {
 public:
  quic_session();
  ~quic_session();

  quic_session(const quic_session&) = delete;
  quic_session& operator=(const quic_session&) = delete;

  void set_events(quic_session_events ev) { events_ = ::std::move(ev); }

  /**
   * @brief 以客户端身份建立内核（不发包）。第一个包由随后的 `flush()` 发。
   * @param local  本端地址（已 `bind` 的那条 UDP 口的地址）。
   * @param remote 对端地址。
   * @param server_name SNI 里的主机名；`nullptr` = 不发 SNI 扩展。
   *
   *   SNI **只能在这里给**：`SSL` 对象由本函数在内部 `SSL_new` 出来，外面
   *   拿不到它，而 `SSL_set_tlsext_host_name()` 必须在 `SSL_new` 之后、握手
   *   之前调。传 `nullptr` 是合法的（对端不靠 SNI 分流时够用），但那时
   *   证书校验也只能靠"不校验"或"自带校验回调"过 —— 虚主机（一个地址上挂多个
   *   证书）必须给名字。
   *
   * @return 0 成功；负值 = libuv/ngtcp2 错误码。
   */
  int init_client(uvcpp_ssl_context* ssl_ctx,
                  const ::std::vector<::std::string>& alpn,
                  const struct sockaddr* local, const struct sockaddr* remote,
                  const char* server_name, uint64_t idle_timeout_ms,
                  quic_send_fn send);

  /**
   * @brief 以服务端身份建立内核。
   *
   * 与客户端的区别不只是"谁是主动方"：服务端是**先收到了首包**才建连接的，
   * 所以连接 ID 由对端首包决定 —— 而且**首包里的两个 CID 都要**，用途不同、
   * 千万别合并成一个。
   *
   * @param client_scid   对端（客户端）首包里的 **SCID**。
   * @param original_dcid 对端首包里的 **DCID**。
   *
   * 方向很容易搞反，所以把两条都写死在这里：
   *
   * - `ngtcp2_conn_server_new()` 的第一个 cid 参数叫 `dcid`，但它要的是
   *   「**客户端首包里的 Source Connection ID**」—— 头里原话是 *"is usually the
   *   Connection ID that appears in client Initial packet as Source Connection
   *   ID"*。所以 `client_scid` 走这条。
   * - `original_dcid` **不走那条**，它进 `transport_params.original_dcid`
   *   （RFC 9000 §7.3 的 `original_destination_connection_id`）。**这一条不设会
   *   直接握手失败**：客户端会拿它和自己收到的首包 DCID 比对，不一致就报
   *   `TRANSPORT_PARAMETER_ERROR`。它同时还是端点做 «首包路由» 的键。
   *
   * 本端自己的 SCID 由本函数**自己随机生成**（18 字节），不从外面传。
   */
  int init_server(uvcpp_ssl_context* ssl_ctx,
                  const ::std::vector<::std::string>& alpn,
                  const struct sockaddr* local, const struct sockaddr* remote,
                  uint64_t idle_timeout_ms, quic_send_fn send,
                  const ngtcp2_cid& client_scid,
                  const ngtcp2_cid& original_dcid);

  // -----------------------------------------------------------------
  // 服务端路由 —— 端点的 CID 表就靠这三条填
  // -----------------------------------------------------------------
  //
  // 服务端的一条 UDP 口上跑着很多条连接，所以每个入包要先按 DCID 找"这是谁的"。
  // 找表这件事是**端点**做的（它拥有那张表），但从包里把 DCID 抠出来、判断
  // "这个 Initial 该不该建新连接"，都是**协议**的事 —— 所以放在这里，而不是让
  // `uvcpp_quic_server.cpp` 自己去认 QUIC 的包头。三条都是静态的：判断"要不要
  // 建新连接"的时候，那个连接还不存在，没有 `this` 可用。

  /**
   * @brief 从入包的头部取出 DCID，端点拿它去查「CID → 连接」表。
   *
   * @param short_dcidlen 短包头里 DCID 的长度。**短包头不把自己的 DCID 长度写在
   *        包上**（RFC 9000 §17.3.1：长度由本端所选的长度决定），所以解它必须先
   *        知道自己发出去的 CID 有多长 —— 就是 `scid_len()`。
   * @return true = \p out 里是 DCID。false = 包太短/畸形，**丢掉**（不是"建新连接"）。
   */
  static bool peek_dcid(const uint8_t* pkt, size_t pktlen, size_t short_dcidlen,
                        ::std::string* out);

  /**
   * @brief 这是一个该建新连接的 Initial 吗？是则取出它的两个 CID。
   *
   * 判据全在 `ngtcp2_accept()` 里（长包头、类型是 Initial、长度够 1200、
   * 没有 token 时 DCID 不短于 8 字节），本函数只负责把结果搬成 `std::string`。
   *
   * @param client_scid   出参：对端首包里的 **SCID** → 建连接时的 `client_scid`。
   * @param original_dcid 出参：对端首包里的 **DCID** → `original_dcid`（同时是
   *        在握完手之前那段时间里的路由键 —— 那时对端还在往这个 CID 发）。
   * @return true = 建；false = 丢掉。
   */
  static bool accept_new(const uint8_t* pkt, size_t pktlen,
                         ::std::string* client_scid,
                         ::std::string* original_dcid);

  /** @brief 本库为一条连接选的 CID 长度（短包头解析要用，见上）。 */
  static size_t scid_len();

  /**
   * @brief 喂一个收到的 UDP 数据报。
   * @return 0 成功；负值 = ngtcp2 错误码。
   */
  int read_pkt(const uint8_t* data, size_t len, const struct sockaddr* peer,
               int peerlen);

  /** @brief 把 ngtcp2 攒下的东西写出去（能写几个包写几个）。 */
  void flush();

  /**
   * @brief 到期计时器响了。
   * @param now_ns `uv_hrtime()` 那种纳秒时钟（与 ngtcp2 的 `timestamp()` 同源）。
   * @return 连接是否因此结束（空闲超时 / 握手超时）。
   */
  bool on_expiry(uint64_t now_ns);

  /**
   * @brief 下一次该醒来的时刻；`UINT64_MAX` = 没有。
   *
   * **不是 `ngtcp2_conn_get_expiry2()` 的直通。** 关闭期那条路 ngtcp2 不管：
   * 它进了 `NGTCP2_CS_CLOSING` 之后 `get_expiry` **只**看丢包检测 / ACK 延迟 /
   * 空闲这些计时器，**没有**"3 × PTO 之后关掉"这一格（本机 ngtcp2 v1.25.0 的
   * `ngtcp2_conn_get_expiry2` 与 `ngtcp2_conn_in_closing_period2` 逐行读过：
   * 前者根本不含关闭期，后者只是个状态查询）。所以本端先关的那一侧要靠**自己**
   * 那个 `close_deadline_` 把关闭期兜住 —— 少了它，一条已经说完了话的连接要一直
   * 等到**空闲超时**（默认 30 s）才肯报 `on_close`。
   *
   * 这曾经是**一条真的漏洞**：`close()` 之后连接停在 CLOSING，只有"碰巧有丢包
   * 检测计时器要到点"时才会走完关闭期。`quic_api_func` 那条用例恰好落在那一格里
   * （刚握完手，PTO 还排着），所以**一直是绿的**，直到 http3 那边连着问了三次
   * 请求、链路静下来之后才把 30 s 的等待暴露出来。
   */
  uint64_t next_expiry() const;

  /**
   * @brief 主动关掉连接（发 CONNECTION_CLOSE）。
   * @param error_code 0 = 正常关闭。
   */
  void close(int error_code);

  quic_connection_state state() const { return state_; }
  ::std::string alpn_selected() const;
  bool established() const { return state_ == quic_connection_state::ESTABLISHED; }
  bool is_closed() const { return state_ == quic_connection_state::CLOSED; }

  // -----------------------------------------------------------------
  // 流
  // -----------------------------------------------------------------

  /**
   * @brief 开一条流。
   * @return 流号（>= 0），或负的 ngtcp2 错误码（`NGTCP2_ERR_STREAM_ID_BLOCKED`
   *         表示对端还没放开流数上限 —— 等 `on_streams_*_available`）。
   */
  int64_t open_stream(bool bidi);

  /**
   * @brief 把一段字节排进某条流的发送队列。
   *
   * **这是"受理"不是"发出去了"** —— 字节被拷进队列，真正上线由 `flush()` 做，
   * 完成由**公开面**的 `on_write` 报（本内核只管到"对端确认了这段"这一步，
   * 见 `settle()`）。所以 `data` 在返回后就可以释放。
   *
   * @param fin true = 这块之后本端不再发了。
   * @return 0 已受理；负值 = 没收下（流不存在 / 已关写方向 / 还没握手完）。
   */
  int write_stream(int64_t stream_id, const char* data, size_t len, bool fin);

  /**
   * @brief 只关发送方向（发 FIN，收方向照常）。
   *
   * 注意与 `write_stream(..., fin=true)` 的区别：那条是"写完这块就关"，
   * 这条是"队列里有没写完的也照样关"（`ngtcp2_conn_shutdown_stream_write`
   * 会丢掉没发完的）。
   *
   * @param app_error_code 随 RESET_STREAM 发出去的应用错误码（0 = 没有错误）。
   */
  int shutdown_stream(int64_t stream_id, uint64_t app_error_code = 0);

  /**
   * @brief 只关**读**方向（发 STOP_SENDING，写方向照常）。
   *
   * @param app_error_code 随 STOP_SENDING 发出去的应用错误码。
   *
   * @note 这条**只排帧、不改本端 `send_q_`**：写方向完全不受影响，与
   *       `shutdown_stream()` 正好各管一半。
   *
   * @note 关掉之后**不会有本地的收尾事件报给 `on_stream_*`** —— ngtcp2 那一格
   *       （`stream_stop_sending`）回调的是"你自己关的读方向"，本层刻意没填：
   *       调用方自己刚下的这个决定，不需要被通知一遍。而且那个回调是在
   *       `ngtcp2_conn_writev_stream` 的写循环**内部**响的，从那里往上报一个
   *       应用事件，等于允许应用在"写"的栈里再调一次 `write_stream()` ——
   *       ngtcp2 明令禁止重入写函数。详见 `fill_callbacks` 里那段。
   */
  int shutdown_stream_read(int64_t stream_id, uint64_t app_error_code = 0);

  /**
   * @brief 本端还能新开多少条本地发起的流（`ngtcp2_conn_get_streams_*_left`）。
   * @param bidi true = 双向流额度，false = 单向流额度。
   */
  uint64_t streams_left(bool bidi) const;

  /// 给本类内部的 ngtcp2 静态回调取回 `this` 用（`crypto_get_conn` 也要）。
  ngtcp2_conn* raw_conn() const { return conn_; }

 private:
  // --- ngtcp2 回调的静态跳板 ---
  //
  // **没有 `self(ngtcp2_conn*)` 这样的函数，而且写不出来**：ngtcp2 v1.25.0
  // **不提供** "从 `ngtcp2_conn*` 取回 user_data" 的接口（`ngtcp2_conn_get_user_data`
  // 这个名字在三个公开头里出现 0 次）。所以每个回调都**必须**用 ngtcp2 传进来的
  // 那个 `user_data` 形参取回 `this`，不能绕道 conn。`crypto_get_conn` 是唯一
  // 例外，因为 OpenSSL crypto 后端回调我们时给的是 `conn_ref`，而
  // `conn_ref->user_data` 是我们自己填的。
  static ngtcp2_conn* crypto_get_conn(ngtcp2_crypto_conn_ref* ref);

  // 签名**逐字**照 `ngtcp2.h` 里的 typedef 抄 —— 有一处例外值得点名：
  // `ngtcp2_rand` 返回 `void`（不是 int），照别的回调顺手写 `int` 会编不过。
  //
  // 这里**没有** `recv_crypto_data` 的跳板：那一格直接用上游给的
  // `ngtcp2_crypto_recv_crypto_data_cb`（它就是 `ngtcp2_crypto_read_write_crypto_data`
  // 的一层包装，专门为"能直接填进这个字段"而存在）。自己再包一层只是多一个
  // 什么都不做的函数。
  static int cb_handshake_completed(ngtcp2_conn* conn, void* user_data);
  static int cb_recv_stream_data(ngtcp2_conn* conn, uint32_t flags,
                                 int64_t stream_id, uint64_t offset,
                                 const uint8_t* data, size_t datalen,
                                 void* user_data, void* stream_user_data);
  static int cb_acked_stream_data_offset(ngtcp2_conn* conn, int64_t stream_id,
                                         uint64_t offset, uint64_t datalen,
                                         void* user_data,
                                         void* stream_user_data);
  static int cb_stream_open(ngtcp2_conn* conn, int64_t stream_id,
                            void* user_data);
  // 对端 reset 了一条流（收到 RESET_STREAM）。名字里的 "recv_" / 没有 "recv_"
  // 是 ngtcp2 自己那套：`stream_reset` 专指**远端发起**的 reset，本端
  // `shutdown_stream_write` 不经过它。
  static int cb_stream_reset(ngtcp2_conn* conn, int64_t stream_id,
                             uint64_t final_size, uint64_t app_error_code,
                             void* user_data, void* stream_user_data);
  // 对端发了 STOP_SENDING。**与 `cb_stream_reset` 方向相反**（那个报"对端不发
  // 了"，这个报"对端不要我发了"），所以是两格，不能合成一格。
  static int cb_recv_stop_sending(ngtcp2_conn* conn, int64_t stream_id,
                                  uint64_t app_error_code, void* user_data,
                                  void* stream_user_data);
  // `stream_close2` 而不是 `stream_close`：前者把收/发两个方向的应用错误码
  // 分开报，后者只有一个合并值。两个都能填，但填了 `_2` 之后 ngtcp2 就不再调
  // 另一个（见 `ngtcp2_callbacks` 的说明），所以只填 `_2`。
  static int cb_stream_close2(ngtcp2_conn* conn, uint32_t flags,
                              int64_t stream_id, uint64_t rx_app_error_code,
                              uint64_t tx_app_error_code, void* user_data,
                              void* stream_user_data);
  static int cb_extend_max_local_streams_bidi(ngtcp2_conn* conn,
                                              uint64_t max_streams,
                                              void* user_data);
  static int cb_extend_max_local_streams_uni(ngtcp2_conn* conn,
                                             uint64_t max_streams,
                                             void* user_data);
  static void cb_rand(uint8_t* dest, size_t destlen,
                      const ngtcp2_rand_ctx* rand_ctx);
  static int cb_get_new_connection_id2(ngtcp2_conn* conn, ngtcp2_cid* cid,
                                       ngtcp2_stateless_reset_token* token,
                                       size_t cidlen, void* user_data);
  // 返回 `int`（不是 void）—— 与 `ngtcp2_remove_connection_id` 的 typedef 一致。
  static int cb_remove_connection_id(ngtcp2_conn* conn, const ngtcp2_cid* cid,
                                     void* user_data);

  /// 建 ngtcp2_conn 之前的公共准备（SSL 那一套），`init_client` / `init_server`
  /// 共用 —— 两边唯一的差别是 `SSL_set_connect_state` / `_accept_state` 与
  /// `ngtcp2_crypto_ossl_configure_{client,server}_session` 的选哪一支。
  int setup_tls(uvcpp_ssl_context* ssl_ctx, bool is_server,
                const ::std::vector<::std::string>& alpn,
                const char* server_name);

  /// 把 `state_` 推进到 CLOSED 并按需报一次 `on_close`。**只报一次**。
  void finalize(int error_code);

  /// 真正干活的 `flush()`：`flush()` 只是它外面那层"现在能不能做"的判断。
  void do_flush();

  /// `close()` 真正干活的那半：备好 `close_ccerr_`、发第一个终端包、进 CLOSING。
  void do_close(int error_code);

  /// 把 `close_ccerr_` 打成包发出去一次。
  /// @return 真发出去了一包返回 true（`0` 与负数都算"没发出去"）。
  bool send_close_packet();

  /// `read_pkt` / `on_expiry` 拿到 ngtcp2 的错误码之后的分诊。
  /// @return 该返回给调用方的错误码（0 = 这场错事已经消化掉了）。
  int handle_conn_error(int rv);

  /// 把 `handshake_done_` 这个标志翻译成状态（HANDSHAKING → ESTABLISHED）。
  ///
  /// **不装计时器** —— 那是端点的事，端点问 `next_expiry()` 就行。名字里的
  /// "state" 指的是 `state_` 这个状态机的状态。
  void refresh_state();

  /// 一段密码学随机数（`ngtcp2_rand` 那条回调没有错误通道，所以这个只尽力而为）。
  static void fill_random(uint8_t* dest, size_t destlen);

  /// 把两边的回调表填起来 —— **必须**是本类的成员（哪怕它一行都不碰 `this`）：
  /// 上面那一批 `cb_*` 跳板是私有的，而 `&quic_session::cb_*` 这种取法只有在
  /// 有访问权的上下文里才成立。摆在文件作用域的自由函数里会撞
  /// "is private within this context"。
  static void fill_callbacks(ngtcp2_callbacks* cbs, bool is_server);

  /**
   * @brief 一条流的发送侧队列。
   *
   * 为什么要自己排队而不是"调用方保证 buffer 活到 ack"：那会把一个协议内部的
   * 时序要求变成**使用者的义务**，而 `write_stream()` 的签名里没有任何东西
   * 提示这件事（`uvcpp_tcp_client::write()` 就是因为这个才要求"回调到了才
   * 能改 buffer"，而那条约束经常被违反）。这里拷一份，代价是一次 memcpy，
   * 换来的是"返回之后随便改"。
   *
   * 队列里那段字节与流偏移的对应关系是**绝对**的（`base_off`），因为
   * ngtcp2 的 `acked_stream_data_offset` 报的也是绝对偏移 —— 用相对下标去对
   * 那条回调，会在有重传时错位。
   *
   * **存储是分块的，不是一条能长的连续缓冲。** 这不是为省事，是 ngtcp2 的
   * 硬要求：交给它的那 `*pdatalen` 个字节必须**原地不动**（`ngtcp2.h` 的原话是
   * "The caller must keep the portion of data covered by |*pdatalen| bytes in
   * tact until acked_stream_data_offset indicates that they are acknowledged"），
   * 因为重传时它**只留了那些指针**，不会自己再存一份数据。而一条能长的连续
   * vector 两条都做不到：尾部 `insert` 超容量会**搬地址**（旧块释放），头部
   * 压缩会**搬字节**。分块把这条约束落到两个具体做法上 ——
   *
   * - 每块出生时 `reserve(kChunkSize)`、**且只写到这里为止** ⇒ 块内地址一生不变；
   * - 只整块丢**全部确认**的块 ⇒ 从不搬字节，而丢一块是 O(1)。
   *
   * 旧形状的两种症状都实测过（`bench_quic --mode=echo --rounds=15
   * --sizes=2097152`）：先是**偶发**崩溃 —— 20 次里 3 次，栈是
   * `ngtcp2_cpymem` ← `ngtcp2_pkt_encode_stream_frame` 从 STREAM 帧的数据
   * 指针上读（`0xC0000005`，目标落在 MEM_RESERVE）；把"地址/字节都不许动"
   * 临时焊死之后 20 次 0 崩。
   *
   * 但**静默坏字节才是常态**，而且比崩溃更坏：把 HEAD 那份存储换回来重编、
   * 跑同一个量具（`--mode=push|echo --rounds=15 --sizes=2097152`，3 次独立
   * 构建 × 2 个方向 = 6 次运行），**6/6 都在 15 轮内报内容不符**，首个不符的
   * 流偏移在 63 605 ～ 1 811 177 之间浮动 —— 坏在哪儿取决于时序，"会坏"则是
   * 必现的。于是它连一个可用于比较的 2 MiB 读数都拿不出来：不是"慢一点"，
   * 是**交付出去的字节是错的**。分块就是那条约束的正式形状。
   */
  struct stream_send {
    /// 每块的容量。取值是折中：块越大分配次数越少，但"最后一块只确认了
    /// 一部分"时要多留一段死字节（上界就是一块）；块越小，一次
    /// `writev_stream` 要描述的连续段越多（ngtcp2 一次最多收 256 段，见
    /// `kMaxStreamVecs`）。
    ///
    /// 实测（`--mode=push --rounds=9 --sizes=2097152`，9 轮最小值）：16 KiB
    /// **25.410 ms** / cyc-B 35.95、64 KiB **21.907 ms** / 30.68、256 KiB
    /// **21.913 ms** / 30.85。64 KiB 与 256 KiB 落在噪声内（差 0.03%），
    /// 取小的那个：一块的**容量**是每条约活的流都要占的（提交量），小四倍
    /// 更划算。16 KiB 那一档慢 16%，说明这个折中不是平的 —— 但拐点在
    /// 64 KiB，往上加没有回报。
    static constexpr size_t kChunkSize = 64 * 1024;
    /// 尚存活的字节，按流偏移升序。除最后一块外每块都是满的 —— 这条是本结构
    /// 的稳态（新块只在旧块写满时才开），`next_sendable_stream()` 用它做除法
    /// 定位，`settle()` 用它做整块丢弃。
    ::std::deque<::std::vector<uint8_t>> chunks;
    /// `chunks.front()[0]` 的流偏移。**只有 `chunks` 空时才等于 `write_off`。**
    uint64_t base_off = 0;
    /// 下一个待写字节的流偏移 = `base_off` + 存活字节数。
    uint64_t write_off = 0;
    /// 已经交给 ngtcp2 的下一个字节的流偏移（`base_off <= sent_off <= write_off`）。
    uint64_t sent_off = 0;
    bool     fin_requested = false;  ///< 调用方要发 FIN
    bool     fin_written = false;    ///< FIN 已经交给 ngtcp2 了
    /// `shutdown_stream()` 调过了。此后 ngtcp2 不再接受这条流的写，排队里
    /// 剩下的字节**也不会再上线**（`shutdown_stream_write` 的语义就是丢掉它们），
    /// 所以这条流要从"可发"里排除，否则每轮 flush 都会在第一轮撞上它。
    bool     closed_write = false;

    /// `[base_off, ack_off)` 这一段是**连续的**、已被对端确认的字节。
    /// 它只从 `base_off` 起算 —— 乱序到达的确认先落在 `holes` 里，等前面的
    /// 洞补上再并进来。理由：块只能在"整块都已确认"时才能丢，
    /// 否则丢掉的那半段对 ngtcp2 来说仍要重传，而它已经不在我们手里了。
    uint64_t ack_off = 0;
    /// 已确认但**还不连续**的区间 `[first, second)`，绝对偏移。
    ::std::map<uint64_t, uint64_t> holes;

    /// 每次 `write_stream()` 受理时，把"这次调用覆盖到的末尾绝对偏移"压进来。
    /// 一次 `on_write` 对应这里的一个元素 —— 与 `uv_write` 一次请求一次回调
    /// 同一形状。用**绝对偏移**而不是块下标，是为了丢块时这里一个数都不用改。
    ::std::vector<uint64_t> op_marks;
    size_t op_done = 0;  ///< `op_marks` 里已经回调过的前缀长度
  };
  ::std::map<int64_t, stream_send> send_q_;

  /// 确认进展：把 `[offset, offset+datalen)` 并进去，并推进 `ack_off`。
  static void note_acked(stream_send& s, uint64_t offset, uint64_t datalen);

  /**
   * @brief 一次确认进展之后收尾：丢掉已确认的**整块**前缀，并把因此完成的
   *        `write_stream()` 通过 `on_write` 报出去。
   *
   * 两件事放一起，是因为它们**共用同一个判据**（`ack_off`）：分开写迟早会出现
   * "报了完成但字节还留着"或反过来的漂移。
   */
  void settle(stream_send& s, int64_t stream_id);

  /// 一次递给 ngtcp2 的连续段数上限。
  ///
  /// ngtcp2 内部收得下 256 段（`NGTCP2_MAX_STREAM_DATACNT`，但它只在自己私有的
  /// `ngtcp2_pkt.h` 里，公开头拿不到），而一个包连 16 KiB 的一块都装不满
  /// （~1200 B），所以 16 段远够用，同时离那个内部上限留着安全余量。
  /// 注意**不能**按这个数去取那个内部宏 —— 那只在 ngtcp2 的私有头里。
  static constexpr size_t kMaxStreamVecs = 16;

  /// 挑一条还有东西可发的流。
  /// @return 有可发的返回 true，并填好 `stream_id` / `datav` / `datavcnt` /
  ///         `handed` / `flags`。`handed` 是这次描述出去的总字节数 —— 调用方
  ///         拿它判 FIN（ngtcp2 只在"给它的数据**全部**编进 STREAM 帧"时才设
  ///         fin 位，见 `.cpp` 里 `do_flush()` 那一格）。
  bool next_sendable_stream(int64_t* stream_id, ngtcp2_vec* datav,
                            size_t* datavcnt, size_t* handed,
                            uint32_t* flags) const;

  /// `uv_hrtime()` 纳秒 —— ngtcp2 的 `timestamp()` 就是它（单位与原点都不重要，
  /// 只要单调且同一个时钟源）。
  static uint64_t now_ns() { return uv_hrtime(); }

  /// RFC 9000 §10.2.1 的关闭期：3 × PTO。公式与常量说明见 `.cpp`。
  ngtcp2_duration closing_period() const;

  // --- 拥有的东西（顺序即析构顺序，别调） ---
  ngtcp2_conn*             conn_ = nullptr;
  ngtcp2_crypto_ossl_ctx*  ossl_ctx_ = nullptr;
  SSL*                     ssl_ = nullptr;  // 由 ossl_ctx_ 持有，这里只为方便

  ngtcp2_crypto_conn_ref   conn_ref_{};

  /**
   * @brief 本端地址 + 对端地址。
   *
   * 存成成员而不是每次现搭：`ngtcp2_conn_client_new` / `_server_new` 拿的是
   * **指针**，而 ngtcp2 的说明里写明它会**自己拷贝一份**路径 —— 但拷的是
   * `ngtcp2_path` 里那两个**指针指向的内容**，所以 `path_.path.local.addr`
   * 必须一直指向一块活得够久的缓冲。`ngtcp2_path_storage` 自带的
   * `local_addrbuf` / `remote_addrbuf` 就是干这个的，本成员负责让它们活到
   * 连接结束。
   *
   * **不能拷贝本结构体**（里面的指针指向自己的缓冲区），所以本类不可拷贝 ——
   * 那个 `= delete` 也顺带把这个坑堵住了。
   */
  ngtcp2_path_storage      path_;

  quic_send_fn             send_;

  quic_session_events      events_;

  quic_connection_state    state_ = quic_connection_state::IDLE;
  bool                     handshake_done_ = false;
  bool                     close_reported_ = false;

  /**
   * @brief 正在 ngtcp2 的某个回调**里面**的层数。
   *
   * 见 `quic_session_events` 那条 `@warning`：非零时不能调 ngtcp2 的写函数，
   * 于是 `flush()` / `close()` 只记意图。写成计数而不是布尔，是因为回调可以
   * 嵌套（`read_pkt` → 我们的回调 → 用户代码 → 又一次 `read_pkt`），退的时候
   * 要一层一层退干净。本仓 `uvcpp_loop::is_running()` 是同一个形状。
   */
  int                      in_callback_ = 0;
  bool                     want_flush_ = false;
  /**
   * @brief 关闭期的终点（`now_ns()` 那种纳秒）。
   *
   * 只在 `do_close()` 里置一次，`next_expiry()` 在 CLOSING 状态下直接报它。
   * 见 `next_expiry()` 的说明：ngtcp2 不给关闭期计时器，这一格是本层自己补的
   * RFC 9000 §10.2.1 那条 3 × PTO。
   */
  uint64_t                 close_deadline_ = UINT64_MAX;
  bool                     want_close_ = false;
  int                      want_close_code_ = 0;

  /**
   * @brief 关连接时用的那个 `ccerr`，以及终端包发过几次。
   *
   * 为什么要留着重发：RFC 9000 §10.2.1 允许端点**重发** CONNECTION_CLOSE，
   * 但只允许有限次 —— 而"丢一个包就再也关不掉"是真实存在的（那条 UDP 口上
   * 谁都不替我们重传终端包）。`3` 就是 RFC 给的次数上限。
   */
  ngtcp2_ccerr             close_ccerr_;
  int                      close_sends_ = 0;
};

}  // namespace quic_detail
}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE

#endif  // SRC_QUIC_UVCPP_QUIC_SESSION_H
