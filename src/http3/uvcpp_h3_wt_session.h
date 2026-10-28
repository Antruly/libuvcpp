/**
 * @file src/http3/uvcpp_h3_wt_session.h
 * @brief WebTransport over HTTP/3（draft-ietf-webtrans-http3-13 语义）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 一条 WebTransport 会话 = 一条 `:protocol = webtransport` 的 extended CONNECT
 * 流（下称 **会话流**），上面挂三类东西：
 *
 *   - **双向/单向流**：每条是独立的 QUIC 流，开头是
 *     `WT_STREAM(0x41)` / `WT_UNI_STREAM(0x54)` 变长整数 + 会话流号（变长整数）；
 *   - **datagram**：HTTP/3 Datagram（RFC 9297），四分之一流号指向会话流；
 *   - **capsule**：会话流上的 HTTP capsule —— `WT_CLOSE_SESSION(0x2843)` 与
 *     `WT_DRAIN_SESSION(0x78ae)`。
 *
 * ## 支持到哪一步（如实列出）
 *
 * - 会话建立：客户端 `connect()`（`:protocol = webtransport`）与
 *   服务端 `accept()`（在 `uvcpp_h3_connection::callbacks::on_connect_request`
 *   里，`req.protocol == "webtransport"`）；
 * - 双向 / 单向流、datagram、Close/Drain capsule；
 * - **没有**：`WT_MAX_SESSIONS` / `WT_AVAILABLE_PROTOCOLS` / `WT_PROTOCOL` 子协议
 *   协商、重定向、0-RTT、`reset_stream_at` 的逐流调优；
 * - 浏览器互操作未在本仓验证过（本仓没有浏览器）。Draft 版本演进时
 *   `:protocol` 与流类型码都变过 —— 这一版按 **draft-13** 的取值实现
 *   （`webtransport` / `0x41` / `0x54` / `0x2843` / `0x78ae`）。
 *
 * ## 生命周期
 *
 * 本类**不拥有** `uvcpp_h3_connection`：h3 连接必须先于会话销毁（或者先
 * `close()`）。会话拥有它创建的 / 收到的所有 `uvcpp_h3_wt_stream` —— 那些对象
 * 在流关闭之后由会话回收，指针随即失效；回调里可以用，**不要存**。
 *
 * **线程**：只能在 h3 连接所属的循环线程上调用。
 */

#pragma once
#ifndef SRC_HTTP3_UVCPP_H3_WT_SESSION_H
#define SRC_HTTP3_UVCPP_H3_WT_SESSION_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_HTTP3_ENABLE

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

class uvcpp_h3_connection;
struct h3_request;
class uvcpp_h3_wt_stream;

/**
 * @brief 一条 WebTransport 流（双向或单向）。
 *
 * 归会话所有；关闭之后指针失效。`on_data` / `on_end` 只能在建立时（`on_stream`
 * 回调里）或第一次写之前装 —— 之后会话不再回头检查它们（与 `uvcpp_ws_connection`
 * 的纪律一致）。
 */
class UVCPP_API uvcpp_h3_wt_stream {
 public:
  ~uvcpp_h3_wt_stream();
  uvcpp_h3_wt_stream(const uvcpp_h3_wt_stream&)            = delete;
  uvcpp_h3_wt_stream& operator=(const uvcpp_h3_wt_stream&) = delete;

  int64_t stream_id() const { return sid_; }
  bool    bidirectional() const { return bidi_; }
  bool    is_open() const { return open_; }

  /// 往这条流上写。单向流上写就是唯一方向；双向流两个方向都能写。
  int write(const void* data, size_t len);
  /// 关闭发送方向（FIN）。
  int end();
  /// 重置整条流（发 RESET_STREAM + STOP_SENDING，应用错误码透传）。
  int reset(uint32_t app_error_code);

  void on_data(std::function<void(const uint8_t*, size_t)> cb) {
    on_data_ = std::move(cb);
  }
  void on_end(std::function<void()> cb) { on_end_ = std::move(cb); }

 private:
  friend class uvcpp_h3_wt_session;
  friend class uvcpp_h3_connection;

  uvcpp_h3_wt_stream(uvcpp_h3_connection* conn, int64_t sid, bool bidi)
      : conn_(conn), sid_(sid), bidi_(bidi) {}

  void deliver_data(const uint8_t* data, size_t len);
  void deliver_end();
  void mark_closed() { open_ = false; }

  uvcpp_h3_connection*                          conn_ = nullptr;
  int64_t                                       sid_  = -1;
  bool                                          bidi_ = true;
  bool                                          open_ = true;
  std::function<void(const uint8_t*, size_t)>   on_data_;
  std::function<void()>                         on_end_;
};

/**
 * @brief 一条 WebTransport 会话（draft-ietf-webtrans-http3-13）。
 *
 * 见文件头对"支持到哪一步"的说明。
 */
class UVCPP_API uvcpp_h3_wt_session {
 public:
  struct callbacks {
    /// 对端开了一条流。**流归会话所有**；回调里装 `on_data` / `on_end`。
    std::function<void(uvcpp_h3_wt_session&, uvcpp_h3_wt_stream&, bool bidi)>
        on_stream;
    /// 会话收到一条 datagram（payload 不含 HTTP/3 的封装）。
    std::function<void(uvcpp_h3_wt_session&, const uint8_t*, size_t)>
        on_datagram;
    /**
     * @brief 会话结束。**恰好一次。**
     *
     * @param code 对端的应用错误码（干净关闭 / FIN 是 0）。
     * @param reason `WT_CLOSE_SESSION` 里的 UTF-8 说明（最多 1024 字节）。
     */
    std::function<void(uvcpp_h3_wt_session&, uint32_t code,
                       const std::string& reason)>
        on_close;
  };

  ~uvcpp_h3_wt_session();
  uvcpp_h3_wt_session(const uvcpp_h3_wt_session&)            = delete;
  uvcpp_h3_wt_session& operator=(const uvcpp_h3_wt_session&) = delete;

  /**
   * @brief 服务端：接受一条 `req.protocol == "webtransport"` 的 extended CONNECT。
   *
   * 发 200、装上会话流的 capsule 落点、登记到 h3 连接。失败返回 `nullptr`。
   *
   * **前提**：h3 连接启动前调过 `set_h3_datagram_enabled(true)` —— 没有 datagram
   * 的 WebTransport 会话仍然可以只用流（draft 允许），`send_datagram()` 会返回
   * `UV_ENOTSUP`。
   */
  static std::unique_ptr<uvcpp_h3_wt_session> accept(
      uvcpp_h3_connection& h3, const h3_request& req, callbacks cbs);

  /**
   * @brief 客户端：开一条 WebTransport 会话。
   *
   * 发 `:protocol = webtransport` 的 extended CONNECT；**不等 2xx** —— 与
   * `uvcpp_h3_ws_connection::connect()` 同一约定：会话立刻可用，服务端拒绝时
   * 流会被 RST，随后回调 `on_close`（错误码来自 reset）。
   */
  static std::unique_ptr<uvcpp_h3_wt_session> connect(
      uvcpp_h3_connection& h3, const std::string& path,
      const std::string& authority, callbacks cbs);

  /// 开一条双向流。返回的流归会话所有；失败返回 `nullptr`（额度/连接问题）。
  uvcpp_h3_wt_stream* open_bidi_stream();
  /// 开一条单向流。
  uvcpp_h3_wt_stream* open_uni_stream();

  /**
   * @brief 发一条 datagram（RFC 9297，四分之一流号指向会话流）。
   *
   * 与 `uvcpp_h3_connection::send_datagram(session_stream_id(), ...)` 等价；
   * 返回码见那一条。
   */
  int send_datagram(const void* data, size_t len);

  /**
   * @brief 发 `WT_CLOSE_SESSION` 并 FIN 会话流。
   *
   * @param code   32 位应用错误码（0 = 正常关闭）。
   * @param reason UTF-8，**超过 1024 字节会被拒绝**（`UV_EINVAL`）。
   */
  int close(uint32_t code = 0, const std::string& reason = std::string());

  /// 发 `WT_DRAIN_SESSION`（只是通知，会话仍然可用）。
  int drain();

  bool    is_open() const { return open_; }
  int64_t stream_id() const { return sid_; }
  /// 本会话上被丢弃的入站 datagram 条数（流被关/会话已终止之后的那些）。
  uint64_t dropped_datagrams() const { return dropped_datagrams_; }

 private:
  friend class uvcpp_h3_connection;

  uvcpp_h3_wt_session(uvcpp_h3_connection* h3, int64_t sid, callbacks cbs);

  /// 会话流上的原始字节（capsule 流）。由 h3 连接递进来。
  void on_capsule_data(const char* data, size_t len);
  void on_capsule_end();
  /// 对端开了一条 WT 流（h3 连接分派过来）。
  void on_peer_stream(int64_t stream_id, bool bidi);
  /// 一条 WT 流上的数据 / 收尾（h3 连接分派过来）。
  void on_stream_data(int64_t stream_id, const uint8_t* data, size_t len);
  void on_stream_end(int64_t stream_id);
  /// 一条 datagram（h3 连接按会话流号分派过来）。
  void on_datagram(const uint8_t* data, size_t len);

  uvcpp_h3_wt_stream* make_stream(int64_t stream_id, bool bidi);
  void destroy_stream(int64_t stream_id);
  void terminate(uint32_t code, const std::string& reason);
  int  send_capsule(uint64_t type, const std::string& body);

  uvcpp_h3_connection* h3_ = nullptr;
  int64_t              sid_ = -1;
  callbacks            cbs_;
  bool                 open_ = true;
  bool                 close_sent_ = false;
  bool                 terminated_ = false;
  uint64_t             dropped_datagrams_ = 0;

  /// capsule 的解析缓冲（会话流上的字节可能被拆成任意段）。
  std::string capsule_buf_;

  /// 活了的所有流（对端开的 + 本端开的）。`std::map` 节点不动，回调可以按引用捕。
  std::map<int64_t, std::unique_ptr<uvcpp_h3_wt_stream> > streams_;
};

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE
#endif  // SRC_HTTP3_UVCPP_H3_WT_SESSION_H
