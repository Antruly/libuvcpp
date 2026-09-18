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
 * @brief 本层记的一条 h2 流。
 *
 * 只在会话存活期间有效；`on_close` 之后指针即失效。
 */
struct UVCPP_API uvcpp_h2_stream {
  int32_t         stream_id = 0;
  h2_stream_state state     = h2_stream_state::OPEN;

  /// 请求头收全后填好（服务端：来自对端；客户端：我们自己提交的那份）。
  uvcpp_http_request request;
  /// 客户端侧才有：响应头收全后填好。
  uvcpp_http_response response;

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
   *         名字/值非法）；其余为 nghttp2 错误码。
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

  // -----------------------------------------------------------------
  // 客户端侧
  // -----------------------------------------------------------------

  /**
   * @brief 提交一个请求。返回新建的 stream id（负值是错误码）。
   *
   * @param body 请求体，由本层持有到发完。空则发 HEADERS + END_STREAM。
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

  /// 按 id 找流；没有返回 nullptr。
  uvcpp_h2_stream* find_stream(int32_t stream_id);

  /// 当前活跃流数。
  size_t stream_count() const;

  /// 最近一次致命错误的原始码（0 表示还没有）。
  int last_error() const;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_NGHTTP2_ENABLE

#endif  // SRC_HTTP2_UVCPP_H2_SESSION_H
