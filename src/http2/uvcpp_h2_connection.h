/**
 * @file src/http2/uvcpp_h2_connection.h
 * @brief 把 h2 会话接到一条**已完成 TLS 握手且 ALPN 协商出 h2** 的连接上。
 * @author zhuweiye
 * @version 1.1.0
 *
 * 分工：`uvcpp_h2_session` 只管协议（收字节 / 吐字节），本类管**字节从哪来、
 * 往哪去**，以及"一次只有一个异步写在飞"这条约束的落地。
 */

#ifndef SRC_HTTP2_UVCPP_H2_CONNECTION_H
#define SRC_HTTP2_UVCPP_H2_CONNECTION_H

#include <uvcpp/uvcpp_config.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_define.h>

#include "http2/uvcpp_h2_session.h"

#if UVCPP_NGHTTP2_ENABLE

namespace uvcpp {

class uvcpp_tcp_client;
struct net_read_result;

/**
 * @brief 一条 h2 连接的驱动层。
 *
 * **不拥有** `uvcpp_tcp_client` —— 那是 `uvcpp_tcp_server` 的 close manager 的
 * 责任。本类只在它还在的时候用它；对端断开或客户端被框架释放之前，
 * 持有者必须先把本对象销毁（见 `on_disconnect`）。
 *
 * **线程**：只能在 loop 线程上用。
 */
class UVCPP_API uvcpp_h2_connection {
 public:
  struct callbacks {
    /**
     * @brief 底层连接结束了（对端关、读错、或我们关完了）。
     *
     * 回调返回后**不要**再碰本对象 —— 持有者应当在这里把它销毁。
     */
    std::function<void(uvcpp_h2_connection&)> on_disconnect;
  };

  /**
   * @param client      已经握手完并且 `tls_alpn_selected() == "h2"` 的连接。
   * @param server_side true 建服务端会话。
   */
  uvcpp_h2_connection(uvcpp_tcp_client* client, bool server_side);
  ~uvcpp_h2_connection();

  uvcpp_h2_connection(const uvcpp_h2_connection&)            = delete;
  uvcpp_h2_connection& operator=(const uvcpp_h2_connection&) = delete;

  /**
   * @brief 建会话、提交初始 SETTINGS、起读、把首轮字节发出去。
   *
   * @param h2_cbs   协议层回调（会同步跑用户代码，见 `uvcpp_h2_session` 的重入说明）。
   * @param conn_cbs 连接层回调。
   */
  int start(const uvcpp_h2_session::callbacks& h2_cbs, const callbacks& conn_cbs);

  /**
   * @brief 提交一个响应并立刻把待发字节冲出去。
   *
   * 是 `session().submit_response(...)` + `flush()` 的合并 —— 后者忘了调就是
   * "响应提交了但一个字节都没发"这种最难查的静默失败。
   */
  int send_response(int32_t stream_id, const uvcpp_http_response& resp,
                    bool omit_body = false);

  /// `session().submit_status(...)` + `flush()`。
  int send_status(int32_t stream_id, int status, const std::string& body);

  /**
   * @brief 提交流式响应的头部（不结束流）+ `flush()`。
   */
  int send_headers(int32_t stream_id, const uvcpp_http_response& resp);

  /**
   * @brief 提交一块流式 body + `flush()`。
   *
   * @param done 这一块上线之后回调一次。**绝不在本次调用里同步跑** —— 它由
   *             写完成路径执行，语义与 h1 那条路（libuv 写完成）一致。流中途
   *             被 RST 或连接断了也一定会跑，参数 `UV_ECANCELED`。
   */
  int send_data(int32_t stream_id, const char* data, size_t len,
                bool end_stream, std::function<void(int)> done);

  /// 把会话里待发的字节全部写出去。可以重复调用（没东西发就是空操作）。
  ///
  /// **在会话回调里调用是安全的**，只是会被推迟到这一轮 `recv()` 返回之后
  /// （理由见 `uvcpp_h2_session::in_nghttp2()`）—— 排队的字节一个都不会丢，
  /// 不需要调用方自己再补一次。
  int flush();

  /**
   * @brief 只发 GOAWAY，**不关连接** —— 告诉对端"这个连接上不会再接新流了"，
   *        已有的流照跑完。
   *
   * 与 `shutdown()` 的分工：这个是**提前**打招呼的那一半。两者都会把
   * `last_stream_id` 填成本端已处理的最大流号 —— 对端据此能把"我还没处理的
   * 那些"和"已经处理完的那些"分开，前者可以安全重试。
   *
   * 发过一次就不再发第二次（`shutdown()` 之后也不会补发）。**关连接是调用方
   * 的事** —— 本函数只把字节冲出去。
   */
  int begin_goaway();

  /// 主动关：尽量把待发字节（含 GOAWAY）冲出去，再关底层连接。
  void shutdown();

  /// 立刻关，不发任何东西。
  void close_now();

  /**
   * @brief 把还没上线的块的 `done` 全部作废（`UV_ECANCELED`）并**取走**，不跑。
   *
   * 这是"传输层要拆了"这条收尾路的正确顺序：调用方先把 `done` 拿到手，再销毁
   * 本对象，最后在自己的上下文已经拆干净之后逐个跑。**不是**在本函数里跑 ——
   * `done` 跑的是用户代码（框架的流式响应收尾），它完全可能再补一笔写，而那时
   * 调用方的上下文正处在"连接已经没了、表还没摘"的中间态，那笔写会挂到错误的
   * 通路上去。
   *
   * `done` 一律以 `UV_ECANCELED` 为参数（连接没了，这些块这辈子发不出去）。
   * 结果**追加**到 @p out，所以传进来的既有内容会保留。
   */
  void take_cancelled_dones(std::vector<std::function<void()>>& out);

  uvcpp_h2_session& session() { return *session_; }
  uvcpp_tcp_client* client() const { return client_; }
  /// 底层连接已经结束。
  bool closed() const { return closed_; }
  /// 正在关（等最后一笔写出去）。
  bool closing() const { return closing_; }

  /// 输入字节数 / 输出字节数，供测试与诊断用。
  size_t bytes_in() const { return bytes_in_; }
  size_t bytes_out() const { return bytes_out_; }

 private:
  void on_read(uvcpp_tcp_client& c, const net_read_result& r);
  void on_write_done(int status);
  void finish_close();
  void notify_disconnect();

  /**
   * @brief 跑掉会话里已经可以结算的 `done`。
   *
   * **只在没有写在飞的时候调**：有写在飞时有另一条路径（写完成）会来跑，
   * 两边都跑就变成同一块结算两次 —— 而"第一次结算"完全可能已经把整条流式
   * 响应收尾、把上下文销毁掉。
   */
  void run_completed();

  // 与 `uvcpp_tcp_client` 同一套存活令牌纪律（理由见那里 `alive_token_` 的注释）：
  // 异步写完成回调是 libuv **稍后**送进来的，而本对象可能在那之前就没了。
  std::shared_ptr<char> alive_token();
  static bool           token_alive(const std::shared_ptr<char>& token);

  uvcpp_tcp_client*                  client_ = nullptr;
  std::shared_ptr<uvcpp_h2_session>  session_;
  callbacks                          cbs_;

  bool   closed_        = false;
  bool   closing_       = false;
  bool   notified_      = false;
  bool   writing_       = false;
  /// 写完成之后还要不要关底层连接（`shutdown()` 的最后一段）。
  bool   close_after_flush_ = false;
  /// 已经发过 GOAWAY。
  bool   goaway_sent_   = false;
  /// 正在跑 `done`。用户代码可以再回调进 `send_data`，那会绕回 `run_completed()` ——
  /// 没有这个闸就是同一批 `done` 被重入地再跑一轮。
  bool   in_dones_      = false;

  std::string  out_;
  uvcpp_buf*   write_buf_ = nullptr;
  size_t       bytes_in_  = 0;
  size_t       bytes_out_ = 0;

  std::shared_ptr<char> alive_token_;
};

}  // namespace uvcpp

#endif  // UVCPP_NGHTTP2_ENABLE

#endif  // SRC_HTTP2_UVCPP_H2_CONNECTION_H
