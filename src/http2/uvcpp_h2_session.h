/**
 * @file src/http2/uvcpp_h2_session.h
 * @brief h2 会话：nghttp2 的 mem-API 包装 + 请求/响应转换。
 * @author zhuweiye
 * @version 1.1.0
 *
 * 这一层**只跟内存说话**，不碰 socket、不碰 libuv。收字节走 `recv()`，
 * 取待发字节走 `drain()`，中间的所有协议动作都在这两句话之间发生。
 * 与 `uvcpp_tcp_client` 那套「socket 归 libuv，SSL 只跟内存说话」同构。
 *
 * 本头**不包含** `<nghttp2/nghttp2.h>` —— nghttp2 的类型全部藏在 `impl` 里。
 * 公开面里连一个 nghttp2 的枚举都没有（`int` 错误码是它的原生返回类型，
 * 但不暴露任何依赖）。
 */

#ifndef SRC_HTTP2_UVCPP_H2_SESSION_H
#define SRC_HTTP2_UVCPP_H2_SESSION_H

#include <uvcpp/uvcpp_config.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_export.h>

#include "http2/uvcpp_h2_common.h"
#include "web/uvcpp_http_request.h"
#include "web/uvcpp_http_response.h"

#if UVCPP_NGHTTP2_ENABLE

namespace uvcpp {

// =========================================================================
// 一条流
// =========================================================================

/**
 * @brief 一条流在**对端响应头到达之前**该有的那个响应对象。
 *
 * 不能直接用 `uvcpp_http_response()`：它的默认构造是 `200 OK`，那是本层替对端
 * 说了一句它从没说过的话 —— 流在响应头之前被 RST_STREAM 掉（`REFUSED_STREAM`
 * 正是如此）时，这个 200 会一路交付到业务层。`HTTP_STATUS_NONE` 才是实话。
 *
 * 做成一个函数而不是在成员上写初始化列表，是因为流有三条创建路径
 * （`stream_of()` 建流、`on_begin_headers` 收到头、`submit_request` 发出去），
 * 漏掉任何一条就是一个编出来的 200。
 */
inline uvcpp_http_response h2_response_not_received() {
  uvcpp_http_response r;
  r.version        = uvcpp_http_version::HVER_20;
  r.status_code    = HTTP_STATUS_NONE;
  r.status_message = http_status_reason(HTTP_STATUS_NONE);
  return r;
}

/**
 * @brief 本层记的一条 h2 流。
 *
 * 只在会话存活期间有效；`on_close` 之后指针即失效。
 */
struct UVCPP_API uvcpp_h2_stream {
  int32_t         stream_id = 0;
  h2_stream_state state     = h2_stream_state::OPEN;

  /// 请求头收全后填好（服务端：来自对端；客户端：我们自己提交的那份）。
  uvcpp_http_request request;
  /**
   * @brief 客户端侧才有：对端发来的响应。
   *
   * 对端的响应头到达**之前** `status_code` 是 `HTTP_STATUS_NONE`，不是默认构造
   * 的那个 `200` —— 流在这之前被 RST 时，业务层拿到的必须是"没有状态码"。
   */
  uvcpp_http_response response = h2_response_not_received();

  /// 已经收到的请求体字节数。**必须按流记** —— 连接级一个标量在并发流下
  /// 会互相记成溢出（计划批 2d 点名的那条）。
  size_t body_bytes = 0;
  /// 对端宣告的 `content-length`；`has_content_length` 为假时无意义。
  size_t expected_body      = 0;
  bool   has_content_length = false;
  /// 本层已经拒了它（发过 RST_STREAM），业务层不该再看到。
  bool   rejected = false;
  /// 请求头里是否出现过常规头（伪头出现在它之后即协议错误）。
  bool   seen_regular_header = false;
  /// 头部预算，`on_begin_headers` 时重置（CONTINUATION 属同一块，不重置）。
  h2_header_budget budget;
};

// =========================================================================
// 会话
// =========================================================================

/**
 * @brief 一个 HTTP/2 连接上的协议会话。
 *
 * **线程**：所有成员都只能在 loop 线程上调用。
 *
 * **重入**：`recv()` / `drain()` 会**同步**回调进 `callbacks`，而回调最终会跑到
 * 用户代码（路由 handler）。用户代码可以关掉连接 —— 于是持有本对象的
 * `uvcpp_h2_connection` 可能在这个调用**还没返回**的时候被析构。
 *
 * 所以调用方必须像 `uvcpp_tcp_client` 那样先取好局部量再进来：
 *
 * @code
 *   auto self = session_;              // 本地持一份 shared_ptr
 *   int rv = self->recv(data, len);    // 回调里连接可以死，会话死不了
 *   if (!token_alive(life_)) return 0; // 连接真死了 —— 到此为止，别再碰成员
 * @endcode
 *
 * 这条不变式是本层最容易踩的地方：`recv()` 的返回值本身**也不能**在
 * 连接已死之后用来做判断依据，因为那时 `this` 已经没了。
 */
class UVCPP_API uvcpp_h2_session {
 public:
  /**
   * @brief 回调集合。
   *
   * 每个回调都在 **loop 线程**上、在 `recv()` / `drain()` 的调用栈内被调用。
   */
  struct callbacks {
    /**
     * @brief 请求头收全（服务端侧）。
     * @param end_stream 对端是否已 END_STREAM（即这条流没有 body）。
     */
    std::function<void(uvcpp_h2_session&, uvcpp_h2_stream&, bool end_stream)>
        on_request;

    /// 请求体收完（服务端侧的 END_STREAM 到了）。只在有 body 时触发。
    std::function<void(uvcpp_h2_session&, uvcpp_h2_stream&)> on_request_end;

    /// 响应头收全（客户端侧）。
    std::function<void(uvcpp_h2_session&, uvcpp_h2_stream&, bool end_stream)>
        on_response;

    /**
     * @brief 整条响应收完（客户端侧）。
     *
     * 与 `on_request_end` 对称。**没有它就分不出"响应到头了"和"响应全收完了"**：
     * 带 body 的响应里 `on_response` 的 `end_stream` 恒为 false，而 DATA 的
     * END_STREAM 又不经过任何回调 —— 调用方只能靠 `on_close` 反推，那在
     * 服务端主动 RST 时和正常结束长得一样。
     */
    std::function<void(uvcpp_h2_session&, uvcpp_h2_stream&)> on_response_end;

    /**
     * @brief 收到一段 body。
     *
     * 服务端侧是请求体、客户端侧是响应体 —— 由 `on_request`/`on_response`
     * 先到过来区分，不用再开两个槽。`data` 只在本次回调内有效。
     */
    std::function<void(uvcpp_h2_session&, uvcpp_h2_stream&,
                       const char* data, size_t len)>
        on_body;

    /**
     * @brief 流结束（两端的 END_STREAM 都发完，或收到 RST_STREAM）。
     *
     * 回到这里之后 `uvcpp_h2_stream&` 立即失效。
     */
    std::function<void(uvcpp_h2_session&, int32_t stream_id,
                       uint32_t error_code)>
        on_close;

    /**
     * @brief 连接级致命错误：本层已经无法继续处理这条连接。
     *
     * 触发者有三类：`mem_recv` 返回负值（含 `-905` 过量的 CONTINUATION、
     * `-902` FLOODED）、nghttp2 判定会话必须终止、以及 `want_read`/`want_write`
     * 双双为假。**收到它就只有一个正确动作：发 GOAWAY（能发就发）然后关连接。**
     *
     * @param nghttp2_error 原始的负值错误码，直接透传便于日志定位。
     */
    std::function<void(uvcpp_h2_session&, int nghttp2_error)> on_fatal;
  };

  /**
   * @param server_side  true 建服务端会话（等对端提请求），false 建客户端会话。
   */
  explicit uvcpp_h2_session(bool server_side);
  ~uvcpp_h2_session();

  uvcpp_h2_session(const uvcpp_h2_session&)            = delete;
  uvcpp_h2_session& operator=(const uvcpp_h2_session&) = delete;

  /**
   * @brief 注册回调并提交初始 SETTINGS。
   *
   * @param max_header_list_size 我们**愿意接收**的头部列表上限。
   *
   * 注意这个值只是**宣告**出去的礼貌值，收方向的强制由 `h2_header_budget`
   * 自己做（见 `uvcpp_h2_common.h` 里那段说明）。
   */
  int init(const callbacks& cbs,
           size_t max_header_list_size = 64u * 1024u,
           uint32_t max_concurrent_streams = H2_DEFAULT_MAX_CONCURRENT_STREAMS,
           uint32_t initial_window_size = 0);

  /**
   * @brief 喂入对端来的字节。
   *
   * @return 0 成功；**负值一律按致命处理**。
   *
   * 为什么"任何负值都致命"而不是逐码判：`nghttp2_session_mem_recv2` 有若干
   * 返回值只表示"这条流有问题"，但 `-905`（CONTINUATION 过多）这种是**连接级
   * 的**，而它偏偏不在返回值上区分 —— 它只是 `return` 出来，既不发 RST 也不发
   * GOAWAY。把"看起来像流级"的错误当流级处理，就会留下一条状态已经错乱的
   * 连接继续用。所以这里统一收紧：负值 ⇒ 会话作废。
   */
  int recv(const char* data, size_t len);

  /**
   * @brief 把当前所有待发字节榨干到 @p out。
   *
   * 内部循环到 `mem_send` 返回 0 为止。合并成**一次** `write()` 是本层的硬
   * 契约：`uvcpp_tcp_client::write` 一次只允许一个异步写在飞，TLS 层还再叠一层。
   *
   * @return 0 成功；负值为 nghttp2 错误码（同样按致命处理）。
   */
  int drain(std::string& out);

  /**
   * @brief 当前是否正跑在 nghttp2 的调用栈里（`recv()` 的 `mem_recv` 或
   * `drain()` 的 `mem_send`），也就是随时可能有回调在用户代码上。
   *
   * 为真时调用方**不许**再调 `drain()` —— 那等于在 nghttp2 自己的栈里重入
   * `mem_send`，官方没保证过这种用法，实测会在"回调里把 RST_STREAM 冲出去"
   * 那条路上读一块 nghttp2 刚释放的 `nghttp2_stream`。推迟到这两句返回之后再
   * 冲即可：`recv()` 的调用方 `uvcpp_h2_connection::on_read()` 本来就在它返回
   * 之后冲一次。
   */
  bool in_nghttp2() const;

  // -----------------------------------------------------------------
  // 服务端侧
  // -----------------------------------------------------------------

  /**
   * @brief 提交一条完整响应。
   *
   * body 由本层持有到该流发完（不是调用方持有），所以 @p resp 可以在本函数
   * 返回后立刻析构。
   *
   * @param omit_body true 时只发 HEADERS + END_STREAM，一个 DATA 字节都不发。
   *                  HEAD 响应与 204/304 走这一支。**注意 `content-length`
   *                  仍按 @p resp 里写的原样发出** —— HEAD 的
   *                  `content-length` 语义上等于 GET 的长度，那是调用方的责任。
   *
   * @return 0 成功；`UV_EINVAL` 表示响应里有不能进 h2 的字段（连接专属头、
   *         名字/值非法）；`UV_EMSGSIZE` 表示头部块超过
   *         `H2_MAX_SEND_HEADER_BLOCK` —— **这两个失败都保证流状态没被改过**，
   *         调用方换一份头部重试是安全的；其余为 nghttp2 错误码。
   *
   * 转换**完全不经过 `uvcpp_http_response::to_string()`** —— 那是纯 HTTP/1.1
   * 的序列化器（有状态行、有 reason phrase、会吐 `Transfer-Encoding`）。
   */
  int submit_response(int32_t stream_id, const uvcpp_http_response& resp,
                      bool omit_body = false);

  /**
   * @brief 提交一个 5xx/4xx 之类的最小响应（只有 `:status` 与 `content-length`）。
   * @param body 可以为空。
   */
  int submit_status(int32_t stream_id, int status, const std::string& body);

  /**
   * @brief 提交流式响应的**头部**，不结束流。
   *
   * 与 `submit_response` 的三点差别，都是"后面还有 body"带来的：
   *   - 不补 `content-length`：长度还不知道，补一个就是在说谎。调用方要么自己
   *     给（已知长度），要么什么都不给 —— h2 里没有 `transfer-encoding`，
   *     "直到 END_STREAM 为止"本身就是边界；
   *   - 不发 DATA 帧、不置 END_STREAM；
   *   - 不做 204/304 的 body 抑制检查 —— 那些状态码本来就不该走流式这条路。
   *
   * @return 0 成功；`UV_EINVAL` 字段非法、`UV_EMSGSIZE` 头部块超上限（同样保证
   *         流状态没被改过，见 `submit_response`）；其余为 nghttp2 错误码。
   */
  int submit_headers(int32_t stream_id, const uvcpp_http_response& resp);

  /**
   * @brief 提交一块流式 body。
   *
   * @param end_stream 这块发完即 END_STREAM。**提交过 end_stream 之后这条流
   *                   不再接受新的 `submit_data`**（再发就是协议违例）。
   * @param done       这一块**真正交给传输层之后**调用一次；失败也必须调，
   *                   参数为错误码。**本层保证它绝不在本次调用里同步跑** ——
   *                   它由 `take_completed()` 取走、由传输层在写完成之后调，
   *                   所以调用方（`uvcpp_web_response::flush_stream`）读到
   *                   的语义与 h1 那条路（libuv 写完成）一致。
   */
  int submit_data(int32_t stream_id, const char* data, size_t len,
                  bool end_stream, std::function<void(int)> done);

  /**
   * @brief 取走已经可以回调的那些块的 `done`（已绑好结果码）。
   *
   * 传输层在**写完成之后**调用它并逐个执行。分开两步是因为 `submit_data`
   * 是在用户代码的调用栈里跑的，而 `done` 会把控制权交回同一段用户代码 ——
   * 同步回调正是 `uvcpp_web_stream_sink::stream_write` 明令禁止的那件事。
   *
   * 结果码在**入队时**就绑定了：正常上线是 0，流中途被 RST / 连接断了是
   * `UV_ECANCELED`。放进 `done` 之后再让传输层补一个码，就会出现"这块到底
   * 是发出去了还是被取消的"两个答案。
   */
  void take_completed(std::vector<std::function<void()>>& out);

  /**
   * @brief 把所有还没上线的块整体作废（`UV_ECANCELED`），并塞进 `completed`。
   *
   * 语义与"对端把每条流都 RST 了"逐字相同（走的就是 `cancel_stream_out()`），
   * 用途只有一个：**传输层要拆了**。连接一没，那些块这辈子都发不出去，而
   * "每一块的 `done` 必须恰好跑一次"是 `submit_data` 写明的契约 —— 少了这一步，
   * 发起方（框架的流式响应）等一个永远不来的回调，那条流的上下文永远不释放。
   *
   * 调用方随后用 `take_completed()` 把 `done` 取走跑掉。**幂等**：再调一次没有
   * 任何可作废的块。
   */
  void cancel_pending_out();

  // -----------------------------------------------------------------
  // 客户端侧
  // -----------------------------------------------------------------

  /**
   * @brief 提交一个请求。返回新建的 stream id（负值是错误码）。
   *
   * @param body 请求体，由本层持有到发完。空则发 HEADERS + END_STREAM。
   *
   * @return 新流的 id；`UV_EINVAL` 表示有不能进 h2 的字段；`UV_EMSGSIZE` 表示
   *         头部块超过 `H2_MAX_SEND_HEADER_BLOCK`；`UV_ENOTCONN` 表示这个会话
   *         已经开不出新流了（收到过对端的 GOAWAY，或本端流号用尽）。
   *         **三者都是同步的、且什么都没发生** —— 没有流被建、没有字节被排队、
   *         没有回调会被叫。
   */
  int32_t submit_request(const uvcpp_http_request& req, std::string body);

  // -----------------------------------------------------------------
  // 通用
  // -----------------------------------------------------------------

  int submit_rst(int32_t stream_id, uint32_t error_code);
  /// 提交 GOAWAY。@p error_code 传 0 即 NO_ERROR。
  int submit_goaway(uint32_t error_code, const std::string& debug = std::string());

  /// 还需要读对端的数据吗。
  bool want_read() const;
  /// 还有待发帧吗。
  bool want_write() const;

  /// 对端宣告的 `SETTINGS_MAX_CONCURRENT_STREAMS`（还没收到就是它的初值）。
  uint32_t peer_max_concurrent_streams() const;

  /**
   * @brief 对端有没有发过 GOAWAY。
   *
   * 收到之后本会话**不再接受新请求**（`submit_request` 一律返回 `UV_ENOTCONN`），
   * 但连接上已有的流照跑完 —— GOAWAY 关的是"新流"，不是"连接"。持有者该做的
   * 是另起一条连接而不是拆这条。
   *
   * 之所以单给一个可查询的位：`GOAWAY(NO_ERROR)` 是**正常的**优雅退出，跟
   * 断线、超时都不该混为一谈，而"错误码是 0"这点本身分辨不出"收到过 GOAWAY"
   * 和"什么都没收到"。
   */
  bool peer_goaway_received() const;
  /// 对端 GOAWAY 里的错误码；没收到过时是 `NO_ERROR`。
  uint32_t peer_goaway_error_code() const;
  /// 对端 GOAWAY 里的 `last_stream_id`；没收到过时是 0。
  int32_t peer_goaway_last_stream_id() const;

  /// 按 id 找流；没有返回 nullptr。
  uvcpp_h2_stream* find_stream(int32_t stream_id);

  /// 当前活跃流数。
  size_t stream_count() const;

  /// 最近一次致命错误的原始码（0 表示还没有）。
  int last_error() const;

  /**
   * @brief 收尾时该发出去的 GOAWAY 错误码。
   *
   * 正常关闭是 `NO_ERROR`（0）；被控制帧令牌桶拦下来时是 `ENHANCE_YOUR_CALM`。
   * **连接层收尾时要读它并原样发给对端** —— 不读的话，一次洪泛在线上长得和
   * "服务端自己正常退出"一模一样，对端拿不到任何可归因的信号。
   */
  uint32_t goaway_code() const;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_NGHTTP2_ENABLE

#endif  // SRC_HTTP2_UVCPP_H2_SESSION_H
