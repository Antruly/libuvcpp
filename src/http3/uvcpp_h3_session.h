/**
 * @file src/http3/uvcpp_h3_session.h
 * @brief h3 会话：nghttp3 的包装。收字节进来，吐待发字节出去。
 * @author zhuweiye
 * @version 1.4.1
 *
 * 这一层**只跟内存说话**：不碰 socket、不碰 libuv、不碰 QUIC 类型。收字节走
 * `recv_stream_data()`，取待发字节走 `drain()`，中间的所有协议动作都在这两句
 * 之间发生 —— 与 `uvcpp_h2_session` 同构，与 `uvcpp_tcp_client` 那套
 * 「socket 归 libuv，SSL 只跟内存说话」也同构。
 *
 * **本头是私有的**，这一点与 `uvcpp_h2_session.h`（那个是装出去的）不同：
 * 它包含 `uvcpp_h3_nghttp3.h`，字段里直接写着 `nghttp3_conn*`。装上等于把
 * "使用者不需要 nghttp3"这个结论作废，而且会摆出一个根本没有稳定 ABI 的类 ——
 * 排除它的理由与 `uvcpp_quic_session.h` 逐字相同。
 *
 * **这处排除与 `tests/tools/package_release.py` 的 `PRIVATE_HEADERS` 是一对**：
 * 那个脚本自己按文件名清单摘私有头，两处必须一起改。
 */

#ifndef SRC_HTTP3_UVCPP_H3_SESSION_H
#define SRC_HTTP3_UVCPP_H3_SESSION_H

#include <uvcpp/uvcpp_config.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_export.h>

#include "http3/uvcpp_h3_common.h"
#include "http3/uvcpp_h3_nghttp3.h"

#if UVCPP_HTTP3_ENABLE

namespace uvcpp {

// =========================================================================
// 待发字节
// =========================================================================

/**
 * @brief `drain()` 吐出来的一块待发字节：属于哪条流、要不要带 FIN、若干段。
 *
 * **段指针指向 nghttp3 的内部缓冲，只在本次 `drain()` 返回之后、下一次
 * `drain()` 调用之前有效。** 调用方必须在同一轮里把它们交给 QUIC（
 * `uvcpp_quic_connection::write_stream()` 会把字节**拷进**自己的发送队列，
 * 所以"交给它"就算数了）。存起来下一轮再发是**悬垂指针**。
 */
struct UVCPP_API h3_out_chunk {
  /// 这条块属于哪条流。
  int64_t stream_id = -1;
  /// 这块发完就该把这条流的发送方向关掉（nghttp3 已经没有任何待发了）。
  bool    fin       = false;
  /// 有效段数。**可以为 0** —— 那是"只发一个 FIN、一个字节的数据都没有"，
  /// 它是一次合法的待发（见 `drain()` 的说明）。
  size_t  veccnt    = 0;
  /// 各段总字节数。
  size_t  total     = 0;
  /// 段指针。前 `veccnt` 个有效。
  const uint8_t* base[H3_MAX_WRITE_VECS];
  size_t         len[H3_MAX_WRITE_VECS];
};

// =========================================================================
// 会话
// =========================================================================

/**
 * @brief 一条 HTTP/3 连接上的协议会话。
 *
 * **线程**：所有成员都只能在 loop 线程上调用。
 *
 * **重入**（这一条是本层最容易踩的地方，与 `uvcpp_h2_session` 的同一段同源）：
 * `recv_stream_data()` 会**同步**回调进 `callbacks`，而回调最终会跑到用户代码
 * （路由 handler），用户代码可以关掉连接 —— 于是持有本对象的
 * `uvcpp_h3_connection` 可能在这个调用**还没返回**的时候被析构。
 *
 * 所以调用方必须像 `uvcpp_h2_connection::on_read()` 那样先取好局部量再进来，
 * 并在回调返回之后**先确认自己还活着**，再碰成员。
 *
 * **写函数的禁止重入**：`drain()` 在 nghttp3 的调用栈里（`in_nghttp3()` 为真）
 * 会直接返回 0，一个字节都不给。这是 nghttp3 的硬要求（在它自己的回调里调它的
 * 写函数是未定义行为），也是 `uvcpp_h2_session::in_nghttp2()` 那条规矩的同一个
 * 形状。**代价是"回调里提交了东西"这件事只能等调用栈退干净再兑现**：本层不替
 * 调用方记这个债（记了也只是把它藏起来），规矩落成一句话 ——
 *
 * > **每次 `recv_stream_data()` 返回之后，调用方必须再冲一轮 `drain()`**，
 * > 而不是"看 `want_write()` 决定冲不冲"。
 *
 * 这一句看着啰嗦，但它同时解决三件事：回调里提交的响应能出去、刚被
 * `on_header_budget_exceeded` 推起来的 STOP_SENDING 能出去、以及 `on_reset_stream`
 * 要求发的 RESET_STREAM 能出去。漏掉它的表现是"响应永远不出去，而且哪里都不报错"。
 * 空冲一轮的代价只是一次 `nghttp3_conn_writev_stream()` 返回 0。
 *
 * **被禁的只有收/发那一对。** `submit_request()` / `submit_response()` /
 * `shutdown_stream_read()` / `shutdown_stream_write()` / `close_stream()` 在回调里
 * **可以**直接调 —— nghttp3 自己的服务端样例就是在 `end_headers` 回调里直接
 * `nghttp3_conn_submit_response()` 的（`ngtcp2/examples/http3_server_proto_codec.cc`，
 * `http_end_request_headers()` 那一支）。这条区别要留着：如果一律禁止，路由 handler
 * 就没法在收到请求的地方当场答复，只能绕一圈回调外再派发一次。
 */
class UVCPP_API uvcpp_h3_session {
 public:
  /**
   * @brief 回调集合。
   *
   * 每个回调都在 **loop 线程**上、在 `recv_stream_data()` / `drain()` 的调用栈内
   * 被调用。回调里传出来的 `std::string` / `h3_header` 都是**拷贝好的值**，
   * 返回之后照样能用（nghttp3 的 `nghttp3_rcbuf*` 只在本回调期间有效，这一点
   * 在跳板里当场兑现了）。
   */
  struct callbacks {
    /// 一个新的头块开始（`h3_header_budget` 在这里重置）。
    std::function<void(uvcpp_h3_session&, int64_t stream_id)> on_begin_headers;

    /// 收到一个头字段。
    std::function<void(uvcpp_h3_session&, int64_t stream_id,
                       const h3_header& field)>
        on_header;

    /**
     * @brief 一个头块收全了。
     * @param fin 头块之后**这条流也没有 body 了**（对端在 HEADERS 上带了 FIN）。
     */
    std::function<void(uvcpp_h3_session&, int64_t stream_id, bool fin)>
        on_end_headers;

    /// 收到一段 body（服务端侧是请求体、客户端侧是响应体）。`data` 只在本次
    /// 回调内有效。
    std::function<void(uvcpp_h3_session&, int64_t stream_id, const char* data,
                       size_t len)>
        on_body;

    /**
     * @brief 读侧收场了：服务端侧 = 请求收全，客户端侧 = 响应收全。
     *
     * 与 `on_end_headers` 的 `fin` **不是**同一件事：那个说的是"头块之后就没有
     * body 了"，这个说的是"这条流的接收方向整个结束了"（头 + 可能若干块 body
     * + FIN）。只带 body 的请求里 `on_end_headers` 的 `fin` 是 false，而
     * `on_end_stream` 照样会来。
     */
    std::function<void(uvcpp_h3_session&, int64_t stream_id)> on_end_stream;

    /**
     * @brief 一条流关掉了。
     *
     * 回到这里之后，这条流在本层的记录立即失效。两个方向的收场**分开报**，
     * 理由见 `h3_stream_close_info`。
     */
    std::function<void(uvcpp_h3_session&, const h3_stream_close_info& info)>
        on_stream_close;

    /**
     * @brief nghttp3 要求我们给对端发 STOP_SENDING。
     *
     * 触发它的是**我们**先说的话："这条流的读方向我不要了"——也就是
     * `shutdown_stream_read()`（它在正常路径上的消费者是下面那条
     * `on_header_budget_exceeded`）。调用方收到它应当去
     * `uvcpp_quic_connection::shutdown_stream_read(sid, code)` —— 那是把 STOP_SENDING
     * 真的发到线上的唯一一条路（本层不碰 QUIC）。
     */
    std::function<void(uvcpp_h3_session&, int64_t stream_id,
                       uint64_t app_error_code)>
        on_stop_sending;

    /// nghttp3 要求我们给对端发 RESET_STREAM（"这条流的发送方向到此为止"）。
    /// 调用方应当去 `uvcpp_quic_connection::shutdown_stream()`。
    std::function<void(uvcpp_h3_session&, int64_t stream_id,
                       uint64_t app_error_code)>
        on_reset_stream;

    /// 某条流上 `datalen` 字节已经被对端**确认**了（可以按它推进写侧记账）。
    /// 本层的消费者只有账本，没有别的动作。
    std::function<void(uvcpp_h3_session&, int64_t stream_id, uint64_t datalen)>
        on_acked;

    /**
     * @brief 一个头块超了预算（`h3_header_budget`）。
     *
     * **一条流至多报一次**，而且报完之后这条流就不再往上交了（`on_header` /
     * `on_end_headers` 都不会再来）。调用方收到它应当：
     * `shutdown_stream_read(sid, H3_EXCESSIVE_LOAD)` 把读方向掐掉（于是
     * `on_stop_sending` 会带着同一个错误码来，线上真的发出 STOP_SENDING），
     * 并且把这条请求**当成不存在**处理。
     *
     * 做成回调而不是本层自己关，是因为"关掉"在 h3 里是**两件事**：告诉 nghttp3
     * （本层能做）与真的发 STOP_SENDING（只有 QUIC 那边能做）。
     */
    std::function<void(uvcpp_h3_session&, int64_t stream_id, size_t used)>
        on_header_budget_exceeded;

    /**
     * @brief 连接级致命错误：本层已经无法继续处理这条连接。
     *
     * 触发点是 `nghttp3_conn_read_stream2()` / `nghttp3_conn_writev_stream()`
     * 返回负值 —— 按 nghttp3 的文档，那意味着**连接**要关，不是某条流。
     *
     * `nghttp3_error` 是原始负值，直接透传便于日志定位；要发给对端的是
     * `quic_app_error_code()`（nghttp3 自己的 `nghttp3_err_infer_quic_app_error_code()`
     * 翻译出来的那个）。**收到它就只有一个正确动作：用那个错误码关掉 QUIC 连接。**
     */
    std::function<void(uvcpp_h3_session&, int nghttp3_error)> on_fatal;
  };

  /// @param server_side true 建服务端会话（等对端提请求），false 建客户端会话。
  explicit uvcpp_h3_session(bool server_side);
  ~uvcpp_h3_session();

  uvcpp_h3_session(const uvcpp_h3_session&)            = delete;
  uvcpp_h3_session& operator=(const uvcpp_h3_session&) = delete;

  /**
   * @brief 注册回调并建出 nghttp3 的连接对象。
   *
   * `nghttp3_settings_default()` 之后我们**只改三个**值（QPACK 动态表容量、
   * 阻塞流数、`max_field_section_size`），其余一律用上游默认 —— 抄一份完整的
   * settings 字面量只会把上游的默认值冻在 1.18.0 这一刻。
   *
   * @param max_field_section_size 我们**宣告**出去的上限。注意它只是礼数值，
   *        收方向的强制由 `h3_header_budget` 自己做（见 `uvcpp_h3_common.h`）。
   * @return 0 成功；负值是 nghttp3 的错误码（`NGHTTP3_ERR_NOMEM`。
   */
  int init(const callbacks& cbs,
           uint64_t max_field_section_size = H3_DEFAULT_MAX_FIELD_SECTION_SIZE,
           size_t qpack_max_dtable_capacity = H3_DEFAULT_QPACK_MAX_DTABLE_CAPACITY,
           size_t qpack_blocked_streams = H3_DEFAULT_QPACK_BLOCKED_STREAMS);

  // -----------------------------------------------------------------
  // 收
  // -----------------------------------------------------------------

  /**
   * @brief 把对端在**某一条流**上来的字节喂进去。
   *
   * @param fin 这一批字节就是这条流的结尾（QUIC 的 STREAM 帧可以同时带数据与
   *            FIN 位，所以这一位来自**这一次**投递，不是下一次回调）。
   * @param ts  当前时间戳（纳秒，**必须单调不减**）。nghttp3 只拿它做自己的
   *            限速与指标（v1.12 起那条 "glitch" 令牌桶），传 0 是合法的。
   *
   * @return 消费掉的字节数（**不是** `len`：nghttp3 会把"为 QPACK 记账"的那些
   *         字节排除在外，见它自己的文档）；负值是错误码，且**一律按连接级
   *         致命处理**（`on_fatal` 已经报过了）。
   *
   * @warning 返回值**不要**拿去还 QUIC 的流控 —— 本库的 QUIC 层在
   *          `cb_recv_stream_data` 里已经按收到的字节数当场还过了
   *          （`ngtcp2_conn_extend_max_stream_offset`）。再还一次是重复记功，
   *          对端窗口会涨得比它发得快。这一位在本层只作记账/诊断。
   */
  int64_t recv_stream_data(int64_t stream_id, const char* data, size_t len,
                           bool fin, uint64_t ts = 0);

  // -----------------------------------------------------------------
  // 发
  // -----------------------------------------------------------------

  /**
   * @brief 取出一块待发字节。
   *
   * @return 1 = `out` 里有东西要发；0 = **现在没有待发的了**；负值是 nghttp3 的
   *         错误码（连接级致命，`on_fatal` 已经报过）。
   *
   * 两种"有东西"要知道：
   *   - `veccnt > 0`：正常数据块（`fin` 可能同时为真 —— 最后一块带 FIN）。
   *   - `veccnt == 0 && fin`：**只有 FIN 的块**。它必须被真的发出去
   *     （`uvcpp_quic_connection::write_stream(sid, nullptr, 0, true)`），而且
   *     紧接着**必须**调 `add_write_offset(sid, 0)` —— 少了那一句，这个 FIN
   *     永远发不出去，而 nghttp3 那边看起来一切正常。nghttp3 自己的文档把这条
   *     写死了："It is important to call this function even if n is 0 in this
   *     case."
   *
   * 在 `in_nghttp3()` 为真时恒返回 0（重入保护），调用方必须自己安排"回来再冲
   * 一次"（见类注释里那段）。
   */
  int drain(h3_out_chunk& out);

  /**
   * @brief 告诉 nghttp3：刚才 `drain()` 吐出的这块，QUIC **真的收下了** `n` 字节。
   *
   * 只有 FIN 没有数据的块传 0 —— 那不是"什么都没发生"，是那一条流唯一一次
   * 能表达"发送方向到此为止"的机会。
   */
  int add_write_offset(int64_t stream_id, size_t n);

  /// 告诉 nghttp3：对端已经确认了某条流上的 `n` 字节（写侧记账，会触发 `on_acked`）。
  int add_ack_offset(int64_t stream_id, uint64_t n);

  // -----------------------------------------------------------------
  // 三条关键单向流与设置
  // -----------------------------------------------------------------

  /**
   * @brief 把一条**本地发起的**单向流绑成 h3 的控制流。
   *
   * 握手刚完就要做，而且必须在 `set_max_client_streams_bidi()` 之前 —— 顺序反了
   * 会让对端在收到我们的 SETTINGS **之前**先看到它开的流（那是
   * `H3_MISSING_SETTINGS`）。
   */
  int bind_control_stream(int64_t stream_id);

  /// 把两条**本地发起的**单向流绑成 QPACK 编码流与解码流（顺序就是这两个形参）。
  int bind_qpack_streams(int64_t qenc_stream_id, int64_t qdec_stream_id);

  /**
   * @brief 告诉会话"对端最多还能开这么多条双向流"（**累计**值）。
   *
   * 服务端在 QUIC 握手给出 `initial_max_streams_bidi` 之后调一次，之后每次
   * `on_streams_available(bidi=true)` 再调。
   */
  void set_max_client_streams_bidi(uint64_t max_streams);

  /// 内部资源的上界（nghttp3 拿它限并发，不是协议上的 SETTINGS）。
  void set_max_concurrent_streams(size_t n);

  // -----------------------------------------------------------------
  // 提出请求 / 提交响应
  // -----------------------------------------------------------------

  /**
   * @brief 客户端：在一条双向流上提交请求（头块 + 可选 body）。
   *
   * **`body` 由本层持有**到这条流关闭 —— 不是调用方持有，也不是"拷进 nghttp3
   * 就不管了"：nghttp3 的 `read_data` 回调拿的是我们自己缓冲里的段，它的文档
   * 要求那些字节活到 `acked_stream_data` 到达。所以传进来的 `std::string` 会被
   * 移进来，调用方可以当场析构它自己的那份。
   *
   * @return 0 成功；`UV_EINVAL` 头部非法（空名字等）；`UV_EMSGSIZE` 头块超了
   *         `H3_MAX_SEND_HEADER_BLOCK`；其余负值是 nghttp3 的错误码。
   */
  int submit_request(int64_t stream_id, const std::vector<h3_header>& headers,
                     std::string body = std::string());

  /**
   * @brief 服务端：提交响应（头块 + 可选 body）。
   *
   * @param omit_body true = 只发头块、不发 body（HEAD、204、304 走这一支）。
   *        **`content-length` 仍按调用方写的原样发出** —— HEAD 的
   *        `content-length` 语义上等于 GET 的长度，那是调用方的责任。
   */
  int submit_response(int64_t stream_id, const std::vector<h3_header>& headers,
                      std::string body = std::string(), bool omit_body = false);

  /// 提交一个只有 `:status` 的最小响应（错误页那一类）。body 可以为空。
  int submit_status(int64_t stream_id, int status, std::string body = std::string());

  // -----------------------------------------------------------------
  // 收场
  // -----------------------------------------------------------------

  /**
   * @brief 告诉 nghttp3：这条流的**读方向**没了（对端发了 RESET_STREAM）。
   *
   * 那不是"关掉这条流"：写方向照走（RFC 9114 §4.1 明写两个方向独立）。
   */
  int shutdown_stream_read(int64_t stream_id);

  /// 告诉 nghttp3：这条流的**写方向**没了（对端发了 STOP_SENDING）。
  void shutdown_stream_write(int64_t stream_id);

  /**
   * @brief 告诉 nghttp3：这条流整个关了（QUIC 那边已经收场）。
   *
   * @param info 两个方向各自的收场。**`rx_error` / `tx_error` 是有意义的** ——
   *        它们决定 nghttp3 收到的是
   *        `NGHTTP3_STREAM_CLOSE_FLAG_{RX,TX}_APP_ERROR_CODE_SET` 置位与否，而
   *        `cb_stream_close2` 会把那一位置原样报回来。"干净收尾"必须表达成
   *        **不置位**，不能表达成"错误码写 0"：`0` 在 h3 里压根不是合法应用错误码
   *        （合法值从 `0x0100` 起），所以用 0 当"没有"是在借一个不存在的编码。
   *
   *        **nghttp3 会据此回调 `on_stream_close`**，所以这条路的收场通知是
   *        "本层自己回声报出来的"，不是另一条独立的信号。
   *
   * @return 0 成功；`UV_ENOENT` 表示这条流 nghttp3 那边已经没有记录（那是**正常**
   *         的重复收尾 —— 两个方向各来一次 QUIC 通知，第二次时 nghttp3 早已清掉，
   *         与"关错了流"要分开，所以单独给一个码）；负数是 nghttp3 的错误码，
   *         且已经按连接级致命处理过（`on_fatal` 报过了）。
   */
  int close_stream(const h3_stream_close_info& info);

  // -----------------------------------------------------------------
  // 查询
  // -----------------------------------------------------------------

  /// 当前是否正跑在 nghttp3 的调用栈里（`recv_stream_data()` 或 `drain()` 内部）。
  bool in_nghttp3() const;

  /// 会话有没有进过致命态。
  bool fatal() const;

  /// 最近一次致命错误的原始码（0 表示还没有）。
  int last_error() const;

  /// 上次致命错误该发给对端的 **h3 应用错误码**（`nghttp3_err_infer_quic_app_error_code`）。
  uint64_t quic_app_error_code() const;

  /// 编进来的 nghttp3 版本串（形如 `"1.18.0"`）。
  std::string nghttp3_version() const;

  /// `nghttp3_strerror` 的包装 —— 这一句是本模块唯一**直接调进 libnghttp3 的
  /// 函数**，少链了 `nghttp3_static` 就会在链接期红，而不是运行期。
  static std::string error_string(int code);

  /// 某条流当前的状态；流不存在时返回 `h3_stream_state::CLOSED`。
  h3_stream_state stream_state(int64_t stream_id) const;

  /// 本层正在跟踪的流数。
  size_t stream_count() const;

 private:
  /// 一条流在本层的账。
  ///
  /// **按 `stream_id` 存，不靠 `stream_user_data`。** 对端发起的流（服务端看到
  /// 的请求流）在 nghttp3 那边 `stream_user_data` 是 NULL —— `submit_response()`
  /// 根本没收这个参数（对比 `submit_request()` 有）—— 而我们的 `cb_read_data()`
  /// 必须知道"这块 body 是哪条流的、发到哪儿了"。所以唯一可靠的键是流号本身。
  struct stream_rec {
    h3_stream_state state = h3_stream_state::OPEN;
    /// 收方向的头部预算。`on_begin_headers` 时按 `max_field_section_size_` 重置。
    h3_header_budget budget;
    /// 发方向的 body 与读游标。**我们持有它**到 nghttp3 说这些字节被确认了
    /// （`nghttp3_read_data_callback` 的文档要求"retain data until ...
    /// `acked_stream_data`"）。`out_body` 为空 = 没交过 body，`dr` 传 nullptr。
    std::string      out_body;
    size_t           out_offset = 0;
    /// `on_header_budget_exceeded` 只报一次，靠这一位。
    bool             budget_reported = false;
  };

  /// 跳板。全部是 `static` + `conn_user_data → this`，每个都套 `cb_guard`
  /// （见 `.cpp` 顶上那段）。
  static int cb_begin_headers(nghttp3_conn* conn, int64_t stream_id,
                              void* conn_user_data, void* stream_user_data);
  static int cb_recv_header(nghttp3_conn* conn, int64_t stream_id, int32_t token,
                            nghttp3_rcbuf* name, nghttp3_rcbuf* value,
                            uint8_t flags, void* conn_user_data,
                            void* stream_user_data);
  static int cb_end_headers(nghttp3_conn* conn, int64_t stream_id, int fin,
                            void* conn_user_data, void* stream_user_data);
  static int cb_recv_data(nghttp3_conn* conn, int64_t stream_id,
                          const uint8_t* data, size_t datalen,
                          void* conn_user_data, void* stream_user_data);
  static int cb_end_stream(nghttp3_conn* conn, int64_t stream_id,
                           void* conn_user_data, void* stream_user_data);
  static int cb_stream_close2(nghttp3_conn* conn, uint32_t flags,
                              int64_t stream_id, uint64_t rx_app_error_code,
                              uint64_t tx_app_error_code, void* conn_user_data,
                              void* stream_user_data);
  static int cb_stop_sending(nghttp3_conn* conn, int64_t stream_id,
                             uint64_t app_error_code, void* conn_user_data,
                             void* stream_user_data);
  static int cb_reset_stream(nghttp3_conn* conn, int64_t stream_id,
                             uint64_t app_error_code, void* conn_user_data,
                             void* stream_user_data);
  static int cb_acked_stream_data(nghttp3_conn* conn, int64_t stream_id,
                                  uint64_t datalen, void* conn_user_data,
                                  void* stream_user_data);
  static nghttp3_ssize cb_read_data(nghttp3_conn* conn, int64_t stream_id,
                                    nghttp3_vec* vec, size_t veccnt,
                                    uint32_t* pflags, void* conn_user_data,
                                    void* stream_user_data);

  /// 记一次致命错误并报一次 `on_fatal`（**只报一次**）。
  int mark_fatal(int nghttp3_error);

  /// 取（必要时建）一条流的记录。`std::map` 的节点不搬家，所以返回的引用在
  /// 后续 insert 之后依然有效。
  stream_rec& stream_of(int64_t stream_id);

  /// 真正做提交的那一处（`submit_request` / `submit_response` 只差一个函数指针）。
  int submit(const std::vector<h3_header>& headers, std::string& body,
             bool omit_body, int64_t stream_id, bool request_side);

  nghttp3_conn*  conn_        = nullptr;
  bool           server_side_ = false;
  callbacks      cbs_;
  /// nghttp3 调用栈的深度（`cb_guard`）。>0 时**不许**再调它的任何函数。
  int            in_callback_ = 0;
  bool           fatal_       = false;
  int            last_error_  = 0;
  uint64_t       max_field_section_size_ = H3_DEFAULT_MAX_FIELD_SECTION_SIZE;
  std::map<int64_t, stream_rec> streams_;
};

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE

#endif  // SRC_HTTP3_UVCPP_H3_SESSION_H
