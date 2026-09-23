/**
 * @file src/http2/uvcpp_h2_connection.cpp
 * @brief h2 连接的驱动实现。
 * @author zhuweiye
 * @version 1.1.0
 */

#include "http2/uvcpp_h2_connection.h"

#if UVCPP_NGHTTP2_ENABLE


#include "net/uvcpp_net_read.h"
#include "net/uvcpp_tcp_client.h"

namespace uvcpp {

uvcpp_h2_connection::uvcpp_h2_connection(uvcpp_tcp_client* client,
                                         bool              server_side)
    : client_(client), session_(new uvcpp_h2_session(server_side)) {}

uvcpp_h2_connection::~uvcpp_h2_connection() {
  if (alive_token_) *alive_token_ = 1;  // 先置位，再拆成员
  delete write_buf_;
  write_buf_ = nullptr;
}

std::shared_ptr<char> uvcpp_h2_connection::alive_token() {
  if (!alive_token_) alive_token_.reset(new char(0));
  return alive_token_;
}

bool uvcpp_h2_connection::token_alive(const std::shared_ptr<char>& token) {
  return token && *token == 0;
}

// =========================================================================
// 起
// =========================================================================

int uvcpp_h2_connection::start(const uvcpp_h2_session::callbacks& h2_cbs,
                               const callbacks&                   conn_cbs) {
  if (!client_) return UV_EINVAL;
  cbs_ = conn_cbs;

  // 会话的回调先包一层：致命错误在这里就地转成"发 GOAWAY 然后关"。
  // `h2_cbs` 按值捕进来 —— `wrapped` 会被 `init()` 拷进会话，而那个 lambda
  // 之后还要用；捕引用就指向了这个已经退栈的形参。
  uvcpp_h2_session::callbacks wrapped = h2_cbs;
  wrapped.on_fatal = [this, h2_cbs](uvcpp_h2_session&, int code) {
    // 传进来的原回调先跑（日志/诊断），然后走本层的收尾。
    if (h2_cbs.on_fatal) h2_cbs.on_fatal(session(), code);
    shutdown();
  };
  const int rv = session_->init(wrapped);
  if (rv != 0) return rv;

  // `read_start_events` 而不是 `read_start`：只有前者会在对端关闭 / 读出错时
  // 给出明确的 PEER_CLOSED / READ_ERROR。用后者的话"连接没了"是**没有回调**的，
  // 表现为连接对象永远留在表里。
  // 读回调也要存活令牌 —— 与 `flush()` / `finish_close()` / `run_completed()`
  // 同一套。`on_disconnect` 的契约是"持有者在这里销毁本对象"，而持有者**未必**
  // 顺手把底层连接也关掉（`uvcpp_http_client` 就是：它只想让 h2 层消失，
  // 没理由替框架关 `tcp_`）。那种情况下这条闭包还挂在 `tcp_` 上，而它捕的是
  // 已经释放的 `this`；下一次读事件（写错之后 socket 往往还能再投递一次读错）
  // 就是一次 use-after-free。
  const std::shared_ptr<char> life = alive_token();
  const int rrv = client_->read_start_events(
      [this, life](uvcpp_tcp_client& c, const net_read_result& r) {
        if (!token_alive(life)) return;
        on_read(c, r);
      });
  if (rrv != 0) return rrv;

  return flush();  // 把初始 SETTINGS 发出去
}

// =========================================================================
// 收
// =========================================================================

void uvcpp_h2_connection::on_read(uvcpp_tcp_client&, const net_read_result& r) {
  if (closed_) return;

  if (r.event != net_read_event::DATA) {
    // 对端关闭或读错。底层连接的释放归框架，本对象只负责告诉持有者。
    closed_ = true;
    notify_disconnect();
    return;
  }

  bytes_in_ += r.size;

  // 会话必须活过这一次调用：`recv()` 会同步跑用户代码（路由 handler），
  // 用户代码可以关掉连接、进而让持有者销毁本对象。本地持一份 shared_ptr
  // 就不会在回调里把会话拆掉。
  std::shared_ptr<uvcpp_h2_session> self = session_;
  const std::shared_ptr<char>       life = alive_token();

  const int rv = self->recv(r.data, r.size);

  // 到这里为止 `self` 还稳（本地那份），但**本对象**可能已经没了。
  if (!token_alive(life)) return;
  if (closed_) return;

  if (rv != 0) {
    // `recv()` 内部的 on_fatal 已经走过 shutdown()，这里不重复动作。
    return;
  }
  flush();

  // `recv()` 里就能结算掉一批 `done`：对端一个 RST_STREAM / GOAWAY 打过来，
  // 会话会把那条流还没上线的块整体作废（`cancel_stream_out`），而**一个字节都
  // 不需要回**。少了这一句，`flush()` 没起写 → `on_write_done` 不会来 →
  // 那批 `done` 就永远躺在 `completed` 里，等它的人（框架流式响应的
  // `pending_bytes_`）永远等不到，那条流的上下文也就永远不释放。
  //
  // 次序与 `on_write_done` 一致，而且**安全**：`flush()` 真起了写的话
  // `run_completed()` 会因为 `writing_` 直接返回，结算留给写完成那条路 ——
  // 两边都跑就是同一块结算两次。
  run_completed();
}

// =========================================================================
// 发
// =========================================================================

int uvcpp_h2_connection::flush() {
  if (closed_ || writing_) return 0;
  // 回调栈里不冲：`session_` 正跑在 nghttp2 的 `mem_recv` / `mem_send` 上，
  // 这时候重入 `mem_send` 会读到 nghttp2 自己刚释放的 `nghttp2_stream`（完整
  // 页堆下必崩）。推迟是**无损**的 —— `on_read()` 在 `recv()` 返回之后本来就要
  // 冲一次，排队的东西一个都不会丢；`on_write_done()` 那条路同理。
  if (session_->in_nghttp2()) return 0;

  const int rv = session_->drain(out_);
  if (rv != 0) {
    shutdown();
    return rv;
  }
  if (out_.empty()) {
    if (close_after_flush_) finish_close();
    return 0;
  }

  // 这一批帧从连接缓冲里**取走**（`swap` 是 O(1)）。`uvcpp_buf(const char*, size_t)`
  // 是**拷贝**构造，所以本地这份 `pending` 出了本函数就可以放掉。
  //
  // **不留在 `out_` 里**是刻意的：留下的那段时间 `out_` 装的是"已经 serialize、
  // 只等上线"的帧，任何一次重入 `flush()` 都会把它们再 `drain` 一遍、再发一遍。
  // 取走之后这个类别**结构上**不存在 —— 不必去论证"回调会不会同步跑"。
  std::string pending;
  pending.swap(out_);
  write_buf_ = new uvcpp_buf(pending.data(), pending.size());

  writing_              = true;
  const std::shared_ptr<char> life = alive_token();
  const int wr = client_->write(write_buf_, [this, life](int status) {
    if (!token_alive(life)) return;  // 本对象已析构，write_buf_ 也随之释放
    on_write_done(status);
  });

  if (wr != 0) {
    // 没进队列，缓冲区得自己收。
    writing_ = false;
    delete write_buf_;
    write_buf_ = nullptr;
    if (wr == UV_EALREADY) {
      // 有别的写在飞，等它的完成回调再冲。**这一批必须放回去**：`drain()` 是
      // 消费式的（`nghttp2_session_mem_send2` 取走即从出站队列消失），
      // `pending` 就是这些帧**唯一**的副本。丢掉 = 这些帧再也发不出去，而它们
      // 的 `done` 已经带着 0（成功）躺在会话的 `completed` 里了 —— 线上一个
      // 字节都没有，发起方却收到"发出去了"。放回**前面**（`pending` 在前、
      // 这段时间里 `out_` 新攒的帧在后），字节顺序天然保住。
      pending.append(out_);
      out_.swap(pending);
      return wr;
    }
    // 硬失败：这条连接没救了，`pending` 里这些帧这辈子都上不了线，就地丢掉
    // （它出了本函数就没了）。**不能放回 `out_`**：`shutdown()` 里那次
    // `flush()` 会拿它们再写一遍（注定失败的重试），而且失败了 `out_` 仍非空
    // ⇒ 顶上的 `finish_close()` 分支进不去 ⇒ 连接卡在 `closing_` 上永远关不掉。
    shutdown();
    return wr;
  }

  // 到这里 `wr == 0`，但**本对象未必还活着**：TLS 那条路上 `client_->write()`
  // 能**同步**把回调跑完 —— 它的 `tls_flush_out()` 里 `tcp_->write()` 被拒就走
  // `tls_fail` → `tls_finish_write`，后者直接调 `write_fn_`
  // （`src/net/uvcpp_tcp_client.cpp`）。那条路**一定**带非 0 状态，于是
  // `on_write_done` 走 `notify_disconnect()`，而持有者的契约正是在那里销毁本
  // 对象。原始代码 write 之后不碰任何成员，所以没事；这里要记账，就得先问一次
  // 令牌。
  if (!token_alive(life)) return 0;

  // 受理了才记账。`out_` 此刻装的是**这段时间里新攒的**帧（通常为空），不动它。
  // 上面那条同步失败的路上连接已经 `closed_`，字节一个都没出网，不记。
  if (!closed_) bytes_out_ += pending.size();
  return 0;
}

void uvcpp_h2_connection::on_write_done(int status) {
  writing_ = false;
  delete write_buf_;
  write_buf_ = nullptr;

  if (status != 0) {
    closed_ = true;
    notify_disconnect();
    return;
  }
  if (closed_) return;

  // `flush()` 可能一路走到 `finish_close()`，而那里的 `c->close(cb)` 在
  // "句柄已经没了"和"已经在关"两个分支上是**同步**跑 `after_close` 的 ——
  // 它一转手就是 `notify_disconnect()`，而持有者（`uvcpp_http_client`）的
  // 契约正是"在这里把本对象销毁"。所以后面每一句都得先问一次令牌。
  const std::shared_ptr<char> life = alive_token();
  flush();  // 写的过程中可能又攒了东西（窗口更新、RST、对端的 SETTINGS ack）
  if (!token_alive(life)) return;

  // 上一笔写带走的那些流式块到这里才算"交出去了"。放在 `flush()` **之后**：
  // 冲出去的字节里可能就含着刚结算的那一块的收尾（END_STREAM），先跑 `done`
  // 会让发起方在最后一个字节出网之前就以为整条流结束了。
  run_completed();
  if (!token_alive(life)) return;

  if (!writing_ && close_after_flush_) finish_close();
}

// =========================================================================
// 关
// =========================================================================

int uvcpp_h2_connection::send_response(int32_t stream_id,
                                       const uvcpp_http_response& resp,
                                       bool omit_body) {
  const int rv = session_->submit_response(stream_id, resp, omit_body);
  if (rv != 0) return rv;
  return flush();
}

int uvcpp_h2_connection::send_status(int32_t stream_id, int status,
                                     const std::string& body) {
  const int rv = session_->submit_status(stream_id, status, body);
  if (rv != 0) return rv;
  return flush();
}

int uvcpp_h2_connection::send_headers(int32_t stream_id,
                                      const uvcpp_http_response& resp) {
  const int rv = session_->submit_headers(stream_id, resp);
  if (rv != 0) return rv;
  return flush();
}

int uvcpp_h2_connection::send_data(int32_t stream_id, const char* data,
                                   size_t len, bool end_stream,
                                   std::function<void(int)> done) {
  const int rv =
      session_->submit_data(stream_id, data, len, end_stream, std::move(done));
  if (rv != 0) return rv;

  // 到这里这块**已经受理**了，所以本函数只能是"受没受理"的那个 0。
  //
  // **不能把 `flush()` 的 rc 传出去。** 调用方（`uvcpp_http_server::write_stream`
  // 的 h2 分支）把非 0 读作"未受理、`done` 不会被调"，于是自己再结算一次 ——
  // 而这时块已进 nghttp2 的出站队列、`done` 也已经带着 0 躺在 `completed` 里
  // （`run_completed()` 就在下面），同一块会被结算两遍。发送真失败的下场由
  // `done` 的码或拆连接那条路（`cancel_pending_out` → `UV_ECANCELED`）承载，
  // 不该冒充"没受理"。
  flush();
  // `flush()` 没起写（没东西可发：窗口关着、或会话还在等对端）时，这一批
  // `done` 就没人来跑了 —— 补上。起了写的话交给 `on_write_done`，
  // 两边都跑就是重复结算。
  run_completed();
  return 0;
}

void uvcpp_h2_connection::run_completed() {
  if (writing_ || in_dones_) return;
  in_dones_ = true;

  // 逐条判存活：`done` 跑的是用户代码，它完全可能把整条连接（连同本对象）
  // 销毁掉。本地持一份令牌，每一轮都重新问一次"我还活着吗"。
  const std::shared_ptr<char> life = alive_token();
  // 外层是个循环，因为 `done` 里很可能又提交了一块（→ `send_data` →
  // `run_completed`，那一层被 `in_dones_` 挡回去），新结算的那些要在这里
  // 接着跑，否则得等下一次写完成才有机会 —— 而那时候可能已经没有下一笔写了。
  for (;;) {
    std::vector<std::function<void()>> dones;
    session_->take_completed(dones);
    if (dones.empty()) break;
    for (size_t i = 0; i < dones.size(); ++i) {
      dones[i]();
      // **这里一个字都不能写。** `alive_token_` 只在析构里被置 1，所以令牌
      // 死了就等于本对象已经没了 —— 连"收个尾"（比如复位 `in_dones_`）都是往
      // 释放过的内存上写。要收尾的那个对象已经不存在了，没什么可收的。
      if (!token_alive(life)) return;
    }
  }
  in_dones_ = false;
}

int uvcpp_h2_connection::begin_goaway() {
  // 只看 `closed_`，不看 `closing_`：`closing_` 为真时 GOAWAY 是不是已经发过，
  // 由 `goaway_sent_` 说了算 —— 而 `close_now()` 那种"立刻关"是 `closed_`。
  if (closed_ || goaway_sent_) return 0;
  goaway_sent_ = true;
  const int rv = session_->submit_goaway(session_->goaway_code(), std::string());
  if (rv != 0) return rv;
  return flush();
}

void uvcpp_h2_connection::shutdown() {
  if (closed_ || closing_) return;
  closing_ = true;

  // GOAWAY 先排进队列，再靠 flush 把它冲出去 —— 直接 close 会让对端只看到
  // 连接断了，分不清"服务端不再接新流"和"网络挂了"。
  if (!goaway_sent_) {
    // 码从会话上取，不是写死的 NO_ERROR：被控制帧令牌桶拦下来时它会是
    // `ENHANCE_YOUR_CALM`，写死就把那个信号抹成了"我们自己正常退出"。
    session_->submit_goaway(session_->goaway_code(), std::string());
    goaway_sent_ = true;
  }
  close_after_flush_ = true;
  // 剩下的收尾全在 `flush()` 里：要么现在就把队排空（于是就地关），要么等
  // 正在飞的那笔写完成后再冲一次。
  flush();
}

void uvcpp_h2_connection::close_now() {
  if (closed_) return;
  closing_           = true;
  close_after_flush_ = true;
  closed_            = true;
  delete write_buf_;
  write_buf_ = nullptr;
  writing_   = false;
  out_.clear();
  notify_disconnect();
}

void uvcpp_h2_connection::take_cancelled_dones(
    std::vector<std::function<void()>>& out) {
  if (!session_) return;
  session_->cancel_pending_out();

  // 走 `take_completed` 而不是自己跑：这一步之后本对象的 `completed` 必须是空的，
  // 否则那些 `done` 会同时挂在调用方手里和这里，谁先跑到就是另一回事。
  std::vector<std::function<void()>> taken;
  session_->take_completed(taken);
  for (size_t i = 0; i < taken.size(); ++i) out.push_back(std::move(taken[i]));
}

void uvcpp_h2_connection::finish_close() {
  if (closed_) return;
  closed_ = true;

  uvcpp_tcp_client* c = client_;
  if (!c) {
    notify_disconnect();
    return;
  }
  // 通知走 `close()` 的 after_close：它排在框架的关闭回调**之前**，那一刻
  // client 还活着（能安全读它的状态），而框架随后就可能把它 delete 掉。
  // 反过来先 notify 再 close 是不行的 —— 接收方在 on_disconnect 里多半会
  // 销毁本对象，`client_` 就跟着没了。
  const std::shared_ptr<char> life = alive_token();
  c->close([this, life]() {
    if (!token_alive(life)) return;
    notify_disconnect();
  });
}

void uvcpp_h2_connection::notify_disconnect() {
  if (notified_) return;
  notified_ = true;
  if (cbs_.on_disconnect) cbs_.on_disconnect(*this);
}

}  // namespace uvcpp

#endif  // UVCPP_NGHTTP2_ENABLE
