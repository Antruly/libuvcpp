/**
 * @file src/http3/uvcpp_h3_ws_server.cpp
 * @brief Implementation of the HTTP/3 WebSocket server wrapper (#58).
 */

#include <http3/uvcpp_h3_ws_server.h>

#if UVCPP_HTTP3_ENABLE

#include <http3/uvcpp_h3_connection.h>
#include <http3/uvcpp_h3_ws_connection.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_context.h>

#include <map>
#include <memory>
#include <string>
#include <utility>

namespace uvcpp {

struct uvcpp_h3_ws_server::impl {
  /// 一条登记在册的 ws 会话。
  ///
  /// 地址在 `sessions` 里**稳定**（`std::map` 的节点不动），所以 ws 的回调可以
  /// 按引用捕它；会话被 erase 时它的回调随 ws 一起销毁，不会有人再用到。
  struct session {
    std::unique_ptr<uvcpp_h3_ws_connection> ws;
    bool close_notified = false;
  };

  struct conn_state {
    std::unique_ptr<uvcpp_h3_connection> h3;
    std::map<int64_t, session> sessions;
  };

  explicit impl(uvcpp_loop* loop) : loop_(loop), quic_(loop) {}

  uvcpp_loop* loop_;
  uvcpp_quic_server quic_;
  uvcpp_h3_ws_server::callbacks cbs_;
  bool listening_ = false;

  /// 存活令牌：析构的第一件事置 1，此后迟到的回调直接返回。
  std::shared_ptr<char> alive_ = std::make_shared<char>(0);

  std::map<uvcpp_h3_connection*, std::unique_ptr<conn_state>> conns_;

  static bool alive(const std::shared_ptr<char>& t) { return t && *t == 0; }

  conn_state* find(uvcpp_h3_connection& h3) {
    auto it = conns_.find(&h3);
    return it == conns_.end() ? nullptr : it->second.get();
  }

  /// 通知一条会话关闭。**恰好一次**；不销毁会话本身。
  void notify_close(session& s, ws_close_code code, const std::string& why) {
    if (s.close_notified || s.ws == nullptr) return;
    s.close_notified = true;
    if (cbs_.on_close) cbs_.on_close(*s.ws, code, why);
  }

  void on_quic_connection(uvcpp_quic_connection* qc);
};

void uvcpp_h3_ws_server::impl::on_quic_connection(uvcpp_quic_connection* qc) {
  std::unique_ptr<conn_state> st(new conn_state);
  st->h3.reset(new uvcpp_h3_connection(qc, /*server_side=*/true));

  std::shared_ptr<char> life = alive_;
  uvcpp_h3_connection::callbacks hc;

  // 本封装只做 WebSocket：普通请求回 404。
  hc.on_request = [this, life](uvcpp_h3_connection& h3, const h3_request& req) {
    if (!alive(life)) return;
    h3.send_status(req.stream_id, 404);
  };

  hc.on_connect_request = [this, life](uvcpp_h3_connection& h3,
                                       const h3_request& req) {
    if (!alive(life)) return;
    if (req.protocol != "websocket") {
      h3.send_status(req.stream_id, 404);
      return;
    }
    conn_state* st = find(h3);
    if (st == nullptr) return;
    const int64_t sid = req.stream_id;

    // 先建登记项（节点地址稳定），再 accept；accept 不会同步回调用户代码。
    session& s = st->sessions[sid];

    uvcpp_h3_ws_connection::callbacks wc;
    wc.on_text = [this, life, &s](const std::string& m) {
      if (!alive(life) || s.ws == nullptr) return;
      if (cbs_.on_text) cbs_.on_text(*s.ws, m);
    };
    wc.on_binary = [this, life, &s](const uint8_t* d, size_t n) {
      if (!alive(life) || s.ws == nullptr) return;
      if (cbs_.on_binary) cbs_.on_binary(*s.ws, d, n);
    };
    wc.on_ping = [this, life, &s](const uint8_t* d, size_t n) {
      if (!alive(life) || s.ws == nullptr) return;
      if (cbs_.on_ping) cbs_.on_ping(*s.ws, d, n);
    };
    wc.on_pong = [this, life, &s](const uint8_t* d, size_t n) {
      if (!alive(life) || s.ws == nullptr) return;
      if (cbs_.on_pong) cbs_.on_pong(*s.ws, d, n);
    };
    wc.on_close = [this, life, &s](ws_close_code code, const std::string& why) {
      if (!alive(life)) return;
      notify_close(s, code, why);
      // 用户回调之后不再碰 s / ws。
    };
    wc.on_error = [this, life, &s](int code, const std::string& why) {
      if (!alive(life) || s.ws == nullptr) return;
      if (cbs_.on_error) cbs_.on_error(*s.ws, code, why);
    };

    std::unique_ptr<uvcpp_h3_ws_connection> ws =
        uvcpp_h3_ws_connection::accept(h3, req, wc);
    if (!ws) {
      // accept 已按 500 的路径交给 h3 层。
      st->sessions.erase(sid);
      return;
    }
    s.ws = std::move(ws);
    if (alive(life) && cbs_.on_open) cbs_.on_open(*s.ws, req.path);
  };

  // 流两个方向都收场：先按 1006 通知（若还没被 Close 帧通知过），再销毁会话。
  hc.on_stream_close = [this, life](uvcpp_h3_connection& h3,
                                    const h3_stream_close_info& info) {
    if (!alive(life)) return;
    conn_state* st = find(h3);
    if (st == nullptr) return;
    auto it = st->sessions.find(info.stream_id);
    if (it == st->sessions.end()) return;
    notify_close(it->second, ws_close_code::ABNORMAL_CLOSE, "stream closed");
    if (!alive(life)) return;
    st->sessions.erase(it);  // 销毁 ws —— 必须在 h3 还活着的时候
  };

  hc.on_disconnect = [this, life](uvcpp_h3_connection& h3) {
    if (!alive(life)) return;
    auto it = conns_.find(&h3);
    if (it == conns_.end()) return;
    conn_state* st = it->second.get();

    for (auto& kv : st->sessions) {
      if (!alive(life)) return;
      notify_close(kv.second, ws_close_code::ABNORMAL_CLOSE, "connection closed");
    }
    if (!alive(life)) return;

    // 顺序是硬要求：ws 会话不拥有 h3 连接，反过来会碰到已释放的连接。
    st->sessions.clear();
    conns_.erase(it);  // 销毁 h3 —— 在它自己的 on_disconnect 里销毁是文档要求
  };

  const int rc = st->h3->start(hc);
  if (rc != 0) {
    // h3 起不来（例如连接已经结束）：把这条 QUIC 连接收掉，不登记。
    if (qc != nullptr) qc->close();
    return;
  }
  conns_[st->h3.get()] = std::move(st);
}

// =========================================================================
// Public surface
// =========================================================================

uvcpp_h3_ws_server::uvcpp_h3_ws_server(uvcpp_loop* loop)
    : impl_(new impl(loop)) {}

uvcpp_h3_ws_server::~uvcpp_h3_ws_server() {
  // 先作废令牌：此后任何迟到的回调都直接返回。
  if (impl_ && impl_->alive_) *impl_->alive_ = 1;

  // 每条连接：先销毁 ws 会话，再销毁 h3 连接（顺序同 on_disconnect）。
  if (impl_) {
    for (auto& kv : impl_->conns_) {
      kv.second->sessions.clear();
    }
    impl_->conns_.clear();
  }
}

uvcpp_loop* uvcpp_h3_ws_server::get_loop() { return impl_->loop_; }

uvcpp_quic_server* uvcpp_h3_ws_server::get_quic_server() { return &impl_->quic_; }

void uvcpp_h3_ws_server::set_ssl_context(uvcpp_ssl_context* ctx) {
  impl_->quic_.set_ssl_context(ctx);
}

int uvcpp_h3_ws_server::bind(const char* ip, int port) {
  return impl_->quic_.bind(ip, port);
}

int uvcpp_h3_ws_server::configured_port() const {
  return impl_->quic_.configured_port();
}

int uvcpp_h3_ws_server::listen(callbacks cbs) {
  if (impl_->listening_) return UV_EALREADY;
  impl_->cbs_ = std::move(cbs);

  std::shared_ptr<char> life = impl_->alive_;
  impl* self = impl_.get();
  const int rc = impl_->quic_.listen([self, life](uvcpp_quic_connection* qc) {
    if (!impl::alive(life)) return;
    self->on_quic_connection(qc);
  });
  if (rc == 0) impl_->listening_ = true;
  else impl_->cbs_ = callbacks();
  return rc;
}

void uvcpp_h3_ws_server::stop() { impl_->quic_.stop(); }

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE
