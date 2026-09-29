/**
 * @file src/quic/uvcpp_quic_connection.cpp
 * @brief 公开面与内核之间的那一层：把内核事件翻译成用户回调，把用户动作转给内核。
 * @author zhuweiye
 * @version 1.4.1
 *
 * 本文件**是**公开面里唯一包含内核头的 `.cpp`，所以它同时背着两件事：
 *
 * 1. **翻译**：内核说的是 `on_stream_data(stream_id, data, len, fin)`，用户看到
 *    的是 `on_read(conn, stream_id, net_read_result)` —— net 层那套事件语义。
 *    两边的形状不同是**有意的**（见 `uvcpp_quic_common.h` 里那段），所以这里
 *    必须有一个人把它们对起来,而且只有这一处。
 * 2. **接线**：`attach()` 把内核的事件槽填成转发到 `cbs` 的 lambda。这段代码
 *    只在 `attach()` 里出现一次 —— 两个端点（client/server）走同一条路,不各写
 *    一份,否则"服务端漏报 PEER_CLOSED"这类漂移迟早发生。
 */

#include "quic/uvcpp_quic_connection.h"

#if UVCPP_QUIC_ENABLE

#include <uv.h>

#include <utility>

#include "quic/uvcpp_quic_session.h"

namespace uvcpp {

struct uvcpp_quic_connection::impl {
  callbacks cbs;

  /// 内核。**本类拥有它** —— 析构里 delete。`nullptr` = 还没挂到端点上。
  quic_detail::quic_session* session = nullptr;

  /// 端点那一跳（见 `attach()` 的说明）。内核报终结 → 先 `cbs.on_close`，
  /// 再这个。
  std::function<void(uvcpp_quic_connection&, int)> on_closed;

  /// 终结只报一次。内核那边（`finalize()`）已经守了一道，这里再守一道不是重复：
  /// 那一道保证的是"内核只报一次",这一道保证的是"本壳只转发一次" —— 而本壳的
  /// `attach()` 可以被调第二次（换内核），换的时候若不重置这个标志,新内核的
  /// 终结就会被老内核留下的 `true` 吃掉。
  bool close_reported = false;

  ~impl() {
    delete session;
    session = nullptr;
  }
};

uvcpp_quic_connection::uvcpp_quic_connection() : impl_(new impl()) {}

uvcpp_quic_connection::~uvcpp_quic_connection() = default;

void uvcpp_quic_connection::set_callbacks(const callbacks& cbs) {
  impl_->cbs = cbs;
}

// =========================================================================
// 端点接线
// =========================================================================

void uvcpp_quic_connection::endpoint::attach(
    uvcpp_quic_connection& c, quic_detail::quic_session* session,
    std::function<void(uvcpp_quic_connection&, int)> on_closed,
    const hooks& h) {
  // 换内核（或摘掉）时先把旧的收掉。**顺序要紧**：旧内核的析构会跑我们填给它的
  // 那些 lambda，而那些 lambda 捕的是 `&c`；先 delete 再改 `c.impl_->session`，
  // 保证"正在析构的那个内核"期间本对象不会指向一个半死的自己。
  impl* self = c.impl_.get();
  delete self->session;
  self->session        = session;
  self->on_closed      = std::move(on_closed);
  self->close_reported = false;

  if (session == nullptr) return;

  // lambda 捕 `&c` 而不是 `this`：`attach` 是**静态**成员函数，这里没有 `this`。
  // 捕引用是安全的 —— `c` 的生命周期覆盖它名下那个内核（`~impl` 里先 delete
  // 内核，而那时 `c` 本体还在析构中，引用仍有效）。
  quic_detail::quic_session_events ev;

  // 这一格没有对应用户回调 —— 用户关心的是"能开始用了"，而那个信号的公开形状
  // 就是 `on_alpn`（握手完成与 ALPN 确定是同一件事的两面）与 `state()`。
  // 不做成第二个回调，是因为两个总是一起跑的通知里，总有一个会被漏写。
  ev.on_handshake_completed = []() {};

  ev.on_alpn = [&c](const std::string& alpn) {
    if (c.impl_->cbs.on_alpn != nullptr) c.impl_->cbs.on_alpn(c, alpn);
  };

  ev.on_stream_open = [&c](int64_t stream_id) {
    if (c.impl_->cbs.on_stream_open != nullptr) {
      c.impl_->cbs.on_stream_open(c, stream_id);
    }
  };

  ev.on_stream_data = [&c](int64_t stream_id, const uint8_t* data,
                           size_t datalen, bool fin) {
    if (c.impl_->cbs.on_read == nullptr) return;
    // 内核把"这块数据"与"这块之后 FIN"放在**一次**通知里（那是 QUIC 帧的形状：
    // STREAM 帧可以同时带数据和 FIN 位），而 `net_read_result` 是**一次一个
    // 事件**的。所以这里可能报两次：先 DATA，再 PEER_CLOSED。
    //
    // 顺序不能反：先把字节交出去，再说"没有更多了"。反过来的话，一个在
    // PEER_CLOSED 里就把缓冲区收掉的调用方会丢掉最后那块数据。
    if (datalen != 0) {
      net_read_result r;
      r.event = net_read_event::DATA;
      r.data  = reinterpret_cast<const char*>(data);
      r.size  = datalen;
      r.error = 0;
      // **QUIC 与 TCP 在这一点上不一样，这不是笔误。** STREAM 帧可以**同时**
      // 带数据与 FIN 位，所以"这块就是最后一块"是这一格自己带着的信息，不是
      // 下一次回调才有的东西。TCP 那条路上它恒为 false（那边确实是两次回调）。
      //
      // HTTP/3 那一层要用它：`nghttp3_conn_read_stream2()` 的 `fin` 形参说的是
      // "这次喂进去的字节就是这条流的结尾"，而 h3 的请求/响应体**恰恰**以
      // "头块 + 数据 + FIN" 这种形状收场 —— 分不出这一位就没法把流正确结束。
      r.fin   = fin;
      c.impl_->cbs.on_read(c, stream_id, r);
    }
    if (fin) {
      net_read_result r;
      r.event = net_read_event::PEER_CLOSED;
      r.data  = nullptr;
      r.size  = 0;
      r.error = 0;
      // 干净收尾 —— 与 `on_stream_reset` 那一格（`fin == false`）成对。
      r.fin   = true;
      // 上面那一跳里用户可能已经把连接关了（那是允许的）—— 但**关**不等于
      // **销毁**：销毁要等内核退栈之后（见 `endpoint::attach` 那条说明）。
      // 所以这里继续用 `c` 是安全的。
      if (c.impl_->cbs.on_read != nullptr) {
        c.impl_->cbs.on_read(c, stream_id, r);
      }
    }
  };

  // 读侧的"没有更多了"由两件事各自的**到达**时刻报，不由流的关闭报：
  // FIN 在 `on_stream_data` 那一格里已经报过 `PEER_CLOSED`，reset 在下面那一格
  // 里报。`on_stream_close` 是"两个方向都收场了"的记账事件，**在读侧它没有
  // 新信息** —— 拿它再报一次只会让调用方对同一条流收两次收尾。
  ev.on_stream_close = nullptr;

  ev.on_stream_reset = [&c](int64_t stream_id, uint64_t app_error_code) {
    if (c.impl_->cbs.on_read == nullptr) return;
    // 对端 reset 了这条流的发送方向。读侧必须当场知道 —— 否则一个"还剩 100
    // 字节没到"的读循环会一直等下去，而那个 100 字节永远不会来。
    //
    // **错误码为 0 时报 `PEER_CLOSED` 而不是 `READ_ERROR`**：QUIC 的应用错误码
    // 是应用自己定的，0 按约定就是"没有错误"（ngtcp2 的原话是 "which generally
    // means success"）。报成 `READ_ERROR` 会让调用方对着一个 `error == 0` 的
    // 错误码去做错误处理。是一条合法读循环的终止条件，不是一个错误。
    net_read_result r;
    r.event = (app_error_code == 0) ? net_read_event::PEER_CLOSED
                                    : net_read_event::READ_ERROR;
    r.data  = nullptr;
    r.size  = 0;
    r.error = static_cast<int>(app_error_code);
    // **RESET 不是 FIN**，即使应用错误码是 0（那种情况下事件名看起来一样）。
    // 这一位就是"读侧到此为止"那两种收场的唯一分界：`true` = 对端说完了，
    // `false` = 对端不要了。HTTP/3 那一层要拿它选路 —— 前者喂
    // `nghttp3_conn_read_stream2(..., fin=1, ...)`，后者走
    // `nghttp3_conn_close_stream()`。
    r.fin   = false;
    c.impl_->cbs.on_read(c, stream_id, r);
  };

  ev.on_write = [&c](int64_t stream_id, int status) {
    if (c.impl_->cbs.on_write != nullptr) {
      c.impl_->cbs.on_write(c, stream_id, status);
    }
  };

  ev.on_close = [&c](int error_code) {
    if (c.impl_->close_reported) return;
    c.impl_->close_reported = true;
    // **先使用者、后端点**：使用者要在这个回调里把"我这边还挂着这条连接"的东西
    // 清掉，而端点要等它清完才动手销毁。反过来的话，使用者会拿到一个已经被端点
    // 回收的引用。
    if (c.impl_->cbs.on_close != nullptr) {
      c.impl_->cbs.on_close(c, error_code);
    }
    if (c.impl_->on_closed != nullptr) c.impl_->on_closed(c, error_code);
  };

  // CID 的发放与退休归端点的路由表 —— 本层只把裸字节转成 `std::string` 递过去。
  // **客户端不填这两个钩子**（`hooks` 默认构造出来就是空的），不是漏了：它只有
  // 一条连接、一个固定对端，没有表可维护。服务端必须填，否则对端换 CID 之后
  // 后续包会被当成新连接。
  ev.on_new_cid = [&c, h](const uint8_t* cid, size_t cidlen, const uint8_t* token,
                          size_t tokenlen) {
    (void)token;
    (void)tokenlen;
    if (h.on_new_cid == nullptr || cid == nullptr || cidlen == 0) return;
    h.on_new_cid(c, std::string(reinterpret_cast<const char*>(cid), cidlen));
  };
  ev.on_remove_cid = [&c, h](const uint8_t* cid, size_t cidlen) {
    if (h.on_remove_cid == nullptr || cid == nullptr || cidlen == 0) return;
    h.on_remove_cid(c, std::string(reinterpret_cast<const char*>(cid), cidlen));
  };

  // 对端放开了流数上限 —— **两条方向合成的同一个用户回调**。
  //
  // 内核把它们分成两格（ngtcp2 的两个回调），公开面收成一格：调用方关心的是
  // "现在能多开几条、往哪个方向"，`bidi` 一位就说清了。分成两个 `std::function`
  // 只会让每个调用点都写一遍同样的两段代码。
  //
  // 从前这两格被显式置空，理由写的是"端点在收到包之后重试 `open_stream()` 就是
  // 全部需要做的事"。那句话对**应用自己**开流的场景成立，对 HTTP/3 **不成立**：
  // h3 必须在握手刚完就开出控制流 + 两条 QPACK 流，而那会儿对端的
  // `initial_max_streams_uni` 可能还没到 —— 那时 `open_stream(false)` 拿到的是
  // `NGTCP2_ERR_STREAM_ID_BLOCKED`，而"等下一个包到了再试"在本层没有切入点
  // （应用看不到包）。所以这一格必须接出来。
  ev.on_streams_bidi_available = [&c](uint64_t max_streams) {
    if (c.impl_->cbs.on_streams_available != nullptr) {
      c.impl_->cbs.on_streams_available(c, /*bidi=*/true, max_streams);
    }
  };
  ev.on_streams_uni_available = [&c](uint64_t max_streams) {
    if (c.impl_->cbs.on_streams_available != nullptr) {
      c.impl_->cbs.on_streams_available(c, /*bidi=*/false, max_streams);
    }
  };

  // 对端发了 STOP_SENDING —— "这条流你别再发了"。
  //
  // 与 `on_read` 那条收尾是**两件不同的事**，两个方向各报一次：`on_read` 报的是
  // "对端不发了"（我这一侧读完了），这一格报的是"对端不要我发了"（我这一侧发
  // 不动了）。合成一个通知会让调用方分不清该关哪一半。
  //
  // 内核那一侧收到 STOP_SENDING 之后**不自动回 RESET_STREAM**：按 RFC 9000
  // §3.5，收到它的一侧"应当"用 RESET_STREAM 回应，但那是应用的判断 ——
  // HTTP/3 里 nghttp3 会要求先取消掉那条流上排队的响应，然后由它指示发什么。
  ev.on_stop_sending = [&c](int64_t stream_id, uint64_t app_error_code) {
    if (c.impl_->cbs.on_stop_sending != nullptr) {
      c.impl_->cbs.on_stop_sending(c, stream_id, app_error_code);
    }
  };

  session->set_events(std::move(ev));
}

quic_detail::quic_session* uvcpp_quic_connection::endpoint::session(
    uvcpp_quic_connection& c) {
  return c.impl_->session;
}

// =========================================================================
// 元数据
// =========================================================================

quic_connection_state uvcpp_quic_connection::state() const {
  if (impl_->session == nullptr) return quic_connection_state::IDLE;
  return impl_->session->state();
}

std::string uvcpp_quic_connection::alpn_selected() const {
  if (impl_->session == nullptr) return std::string();
  return impl_->session->alpn_selected();
}

// =========================================================================
// 流 —— 全部转给内核
// =========================================================================

int64_t uvcpp_quic_connection::open_stream(bool bidi) {
  if (impl_->session == nullptr) return static_cast<int64_t>(UV_ENOTCONN);
  return impl_->session->open_stream(bidi);
}

int uvcpp_quic_connection::write_stream(int64_t stream_id, const char* data,
                                        size_t len, bool end_stream) {
  if (impl_->session == nullptr) return UV_ENOTCONN;
  return impl_->session->write_stream(stream_id, data, len, end_stream);
}

int uvcpp_quic_connection::shutdown_stream(int64_t stream_id,
                                           uint64_t app_error_code) {
  if (impl_->session == nullptr) return UV_ENOTCONN;
  return impl_->session->shutdown_stream(stream_id, app_error_code);
}

int uvcpp_quic_connection::shutdown_stream_read(int64_t stream_id,
                                                uint64_t app_error_code) {
  if (impl_->session == nullptr) return UV_ENOTCONN;
  return impl_->session->shutdown_stream_read(stream_id, app_error_code);
}

uint64_t uvcpp_quic_connection::streams_left(bool bidi) const {
  if (impl_->session == nullptr) return 0;
  return impl_->session->streams_left(bidi);
}

int uvcpp_quic_connection::close(int error_code) {
  // `UV_ENOTCONN` 而不是 `UV_ENOSYS`：没有内核时,"这条路没通"就是实话 ——
  // 这个对象还没挂到任何端点上。`UV_ENOSYS`（"功能没实现"）在本版已经不成立了。
  if (impl_->session == nullptr) return UV_ENOTCONN;
  if (impl_->session->is_closed()) return UV_ENOTCONN;
  impl_->session->close(error_code);
  return 0;
}

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE
