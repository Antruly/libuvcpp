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
  const int rrv = client_->read_start_events(
      [this](uvcpp_tcp_client& c, const net_read_result& r) { on_read(c, r); });
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
}

// =========================================================================
// 发
// =========================================================================

int uvcpp_h2_connection::flush() {
  if (closed_ || writing_) return 0;

  const int rv = session_->drain(out_);
  if (rv != 0) {
    shutdown();
    return rv;
  }
  if (out_.empty()) {
    if (close_after_flush_) finish_close();
    return 0;
  }

  bytes_out_ += out_.size();
  // `uvcpp_buf(const char*, size_t)` 是**拷贝**构造，所以 `out_` 可以立刻清空 ——
  // 缓冲区在写完成回调里才被释放。
  write_buf_ = new uvcpp_buf(out_.data(), out_.size());
  out_.clear();

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
    if (wr == UV_EALREADY) return wr;  // 有别的写在飞，等它的完成回调再冲
    shutdown();
    return wr;
  }
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

  flush();  // 写的过程中可能又攒了东西（窗口更新、RST、对端的 SETTINGS ack）

  // 上一笔写带走的那些流式块到这里才算"交出去了"。放在 `flush()` **之后**：
  // 冲出去的字节里可能就含着刚结算的那一块的收尾（END_STREAM），先跑 `done`
  // 会让发起方在最后一个字节出网之前就以为整条流结束了。
  run_completed();

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

  const int frv = flush();
  // `flush()` 没起写（没东西可发：窗口关着、或会话还在等对端）时，这一批
  // `done` 就没人来跑了 —— 补上。起了写的话交给 `on_write_done`，
  // 两边都跑就是重复结算。
  run_completed();
  return frv;
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
      if (!token_alive(life)) {
        in_dones_ = false;
        return;
      }
    }
  }
  in_dones_ = false;
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
