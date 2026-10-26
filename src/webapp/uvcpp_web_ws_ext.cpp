/**
 * @file src/webapp/uvcpp_web_ws_ext.cpp
 * @brief extended CONNECT 上的 WebSocket 路由（RFC 8441 / RFC 9220，#58）。
 *
 * 这一层只做**接线**：把 h2/h3 的 CONNECT 流包装成传输无关的
 * `uvcpp_web_ws_session`，再交给 `websocket_connect()` 注册的处理器。
 * 帧的收发与 `uvcpp_h2_ws_connection` / `uvcpp_h3_ws_connection` 逐字相同，
 * 这里一个字节都不碰。
 */

#include <webapp/uvcpp_web_app.h>

#include <utility>

#include <webapp/uvcpp_web_util.h>
#include <webapp/uvcpp_web_ws.h>

#if UVCPP_NGHTTP2_ENABLE
#include <http2/uvcpp_h2_connection.h>
#include <http2/uvcpp_h2_ws_connection.h>
#endif

#if UVCPP_HTTP3_ENABLE
#include <http3/uvcpp_h3_connection.h>
#include <http3/uvcpp_h3_ws_connection.h>
#endif

namespace uvcpp {

namespace {

/**
 * @brief 两个传输适配器的共同部分：用户回调槽 + 收尾记账。
 *
 * 用户是在**会话建好之后**才装回调的（处理器拿到引用那一刻），而 h2/h3 的
 * ws 连接要求回调在 accept 时给全 —— 所以 accept 那边挂的是转到这里的
 * trampoline，真回调存在本类里，装晚了也不丢。
 */
class ext_session_base : public uvcpp_web_ws_session {
 public:
  ext_session_base(std::function<void(uint64_t)> reap, uint64_t id)
      : reap_(std::move(reap)), id_(id) {}

  void on_text(std::function<void(const std::string&)> cb) override {
    text_cb_ = std::move(cb);
  }
  void on_binary(std::function<void(const uint8_t*, size_t)> cb) override {
    binary_cb_ = std::move(cb);
  }
  void on_ping(std::function<void(const uint8_t*, size_t)> cb) override {
    ping_cb_ = std::move(cb);
  }
  void on_pong(std::function<void(const uint8_t*, size_t)> cb) override {
    pong_cb_ = std::move(cb);
  }
  void on_close(
      std::function<void(ws_close_code, const std::string&)> cb) override {
    close_cb_ = std::move(cb);
  }
  void on_error(std::function<void(int, const std::string&)> cb) override {
    error_cb_ = std::move(cb);
  }

 protected:
  void deliver_text(const std::string& m) {
    if (text_cb_) text_cb_(m);
  }
  void deliver_binary(const uint8_t* d, size_t n) {
    if (binary_cb_) binary_cb_(d, n);
  }
  void deliver_ping(const uint8_t* d, size_t n) {
    if (ping_cb_) ping_cb_(d, n);
  }
  void deliver_pong(const uint8_t* d, size_t n) {
    if (pong_cb_) pong_cb_(d, n);
  }
  void deliver_error(int code, const std::string& why) {
    if (error_cb_) error_cb_(code, why);
  }
  /// 会话终结：跑用户的 `on_close`，然后请框架**推迟**回收本对象。
  void terminate(ws_close_code code, const std::string& reason) {
    if (terminated_) return;
    terminated_ = true;
    if (close_cb_) close_cb_(code, reason);
    if (reap_) reap_(id_);
  }

  std::function<void(uint64_t)>                      reap_;
  uint64_t                                           id_;
  bool                                               terminated_ = false;
  std::function<void(const std::string&)>            text_cb_;
  std::function<void(const uint8_t*, size_t)>        binary_cb_;
  std::function<void(const uint8_t*, size_t)>        ping_cb_;
  std::function<void(const uint8_t*, size_t)>        pong_cb_;
  std::function<void(ws_close_code, const std::string&)> close_cb_;
  std::function<void(int, const std::string&)>       error_cb_;
};

#if UVCPP_NGHTTP2_ENABLE
class h2_ext_session : public ext_session_base {
 public:
  h2_ext_session(std::function<void(uint64_t)> reap, uint64_t id)
      : ext_session_base(std::move(reap), id) {}

  bool start(uvcpp_h2_connection& h2, int32_t sid,
             const uvcpp_http_request& req) {
    uvcpp_h2_ws_connection::callbacks cbs;
    cbs.on_text   = [this](const std::string& m) { deliver_text(m); };
    cbs.on_binary = [this](const uint8_t* d, size_t n) { deliver_binary(d, n); };
    cbs.on_ping   = [this](const uint8_t* d, size_t n) { deliver_ping(d, n); };
    cbs.on_pong   = [this](const uint8_t* d, size_t n) { deliver_pong(d, n); };
    cbs.on_error  = [this](int code, const std::string& why) {
      deliver_error(code, why);
    };
    cbs.on_close = [this](ws_close_code code, const std::string& reason) {
      terminate(code, reason);
    };
    ws_ = uvcpp_h2_ws_connection::accept(h2, sid, req, std::move(cbs));
    return ws_ != nullptr;
  }

  uvcpp_http_version transport() const override {
    return uvcpp_http_version::HVER_20;
  }
  bool is_open() const override { return ws_ && ws_->is_open(); }

  int send_text(const char* data, size_t len) override {
    if (!ws_) return -1;
    return ws_->send_text(data, len);
  }
  int send_binary(const void* data, size_t len) override {
    if (!ws_) return -1;
    return ws_->send_binary(data, len);
  }
  int send_ping(const void* data, size_t len) override {
    if (!ws_) return -1;
    return ws_->send_ping(data, len);
  }
  int send_pong(const void* data, size_t len) override {
    if (!ws_) return -1;
    return ws_->send_pong(data, len);
  }
  int close(ws_close_code code, const std::string& reason) override {
    if (!ws_) return -1;
    return ws_->close(code, reason);
  }

 private:
  std::unique_ptr<uvcpp_h2_ws_connection> ws_;
};
#endif  // UVCPP_NGHTTP2_ENABLE

#if UVCPP_HTTP3_ENABLE
class h3_ext_session : public ext_session_base {
 public:
  h3_ext_session(std::function<void(uint64_t)> reap, uint64_t id)
      : ext_session_base(std::move(reap), id) {}

  bool start(uvcpp_h3_connection& h3, int64_t sid,
             const uvcpp_http_request& req) {
    uvcpp_h3_ws_connection::callbacks cbs;
    cbs.on_text   = [this](const std::string& m) { deliver_text(m); };
    cbs.on_binary = [this](const uint8_t* d, size_t n) { deliver_binary(d, n); };
    cbs.on_ping   = [this](const uint8_t* d, size_t n) { deliver_ping(d, n); };
    cbs.on_pong   = [this](const uint8_t* d, size_t n) { deliver_pong(d, n); };
    cbs.on_error  = [this](int code, const std::string& why) {
      deliver_error(code, why);
    };
    cbs.on_close = [this](ws_close_code code, const std::string& reason) {
      terminate(code, reason);
    };
    ws_ = uvcpp_h3_ws_connection::accept(h3, sid, req, std::move(cbs));
    return ws_ != nullptr;
  }

  uvcpp_http_version transport() const override {
    return uvcpp_http_version::HVER_30;
  }
  bool is_open() const override { return ws_ && ws_->is_open(); }

  int send_text(const char* data, size_t len) override {
    if (!ws_) return -1;
    return ws_->send_text(data, len);
  }
  int send_binary(const void* data, size_t len) override {
    if (!ws_) return -1;
    return ws_->send_binary(data, len);
  }
  int send_ping(const void* data, size_t len) override {
    if (!ws_) return -1;
    return ws_->send_ping(data, len);
  }
  int send_pong(const void* data, size_t len) override {
    if (!ws_) return -1;
    return ws_->send_pong(data, len);
  }
  int close(ws_close_code code, const std::string& reason) override {
    if (!ws_) return -1;
    return ws_->close(code, reason);
  }

 private:
  std::unique_ptr<uvcpp_h3_ws_connection> ws_;
};
#endif  // UVCPP_HTTP3_ENABLE

}  // namespace

// =========================================================================
// uvcpp_web_app：路由、钩子、回收
// =========================================================================

uvcpp_web_app& uvcpp_web_app::websocket_connect(
    const std::string& pattern, const uvcpp_web_ws_ext_handler& handler) {
  if (!handler) {
    UVCPP_LOG_WARN(log_category::WEBSOCKET)
        << "拒绝注册空 extended CONNECT handler：" << pattern;
    return *this;
  }

  // 与 `websocket()` 同一条占位约定：借 `uvcpp_web_router` 做模式解析与优先级，
  // 真处理器按模式串存在 `ws_ext_handlers_`。
  if (!ws_ext_router_.any(pattern, [](uvcpp_web_request&, uvcpp_web_response&,
                                      uvcpp_web_next) {})) {
    return *this;  // 模式非法，add 已经记了 WARN
  }
  ws_ext_handlers_[ws_ext_router_.full_pattern(pattern)] = handler;
  enable_ws_ext();
  return *this;
}

void uvcpp_web_app::enable_ws_ext() {
  if (ws_ext_hooks_installed_ || http_ == nullptr) return;
  ws_ext_hooks_installed_ = true;

#if UVCPP_NGHTTP2_ENABLE
  http_->set_h2_extended_connect_handler(
      [this](uvcpp_h2_connection& h2, int32_t sid,
             const uvcpp_http_request& req) {
        return dispatch_ws_connect(req, &h2, sid, nullptr);
      });
#endif
#if UVCPP_HTTP3_ENABLE
  http_->set_h3_extended_connect_handler(
      [this](uvcpp_h3_connection& h3, const uvcpp_http_request& req) {
        return dispatch_ws_connect(req, nullptr, 0, &h3);
      });
#endif
}

bool uvcpp_web_app::dispatch_ws_connect(const uvcpp_http_request& req,
                                        uvcpp_h2_connection* h2,
                                        int32_t h2_stream_id,
                                        uvcpp_h3_connection* h3) {
  if (ws_ext_handlers_.empty()) return false;

  // 路径归一化与 HTTP / h1-WS 路由走同一个函数 —— 三种传输看到的路径一致。
  std::string path;
  std::string query_string;
  web_split_path_query(req.url, path, query_string);
  web_normalize_path(path);

  const web_route_match m = ws_ext_router_.match(http_method::HTTP_GET, path);
  if (m.result != web_route_result::MATCHED || m.pattern == nullptr) return false;
  std::map<std::string, uvcpp_web_ws_ext_handler>::const_iterator hit =
      ws_ext_handlers_.find(*m.pattern);
  if (hit == ws_ext_handlers_.end()) return false;

  // 请求视图从**副本**上建（`uvcpp_web_ws_request` 的构造是 take_from，会搬走
  // body）。h2/h3 的 extended CONNECT 按 RFC 8441 §4 没有请求体，这一次拷贝
  // 实际上只搬头。
  uvcpp_http_request src(req);
  std::unique_ptr<uvcpp_web_ws_request> ws(new uvcpp_web_ws_request(src));
  ws->set_route(*m.pattern);
  for (size_t i = 0; i < m.params.size(); ++i) {
    ws->request().set_param(m.params[i].first, m.params[i].second);
  }

#if UVCPP_NGHTTP2_ENABLE
  if (h2 != nullptr) {
    // 对端地址与 HTTP 请求走**同一个来源**（连接登记表）；h2 连接底下就是一条
    // `uvcpp_tcp_client`，所以这条路与 h1 逐字相同。
    uvcpp_tcp_client* client = h2->client();
    if (client != nullptr) {
      const uvcpp_web_conn_id cid = reg_here().id_of(client);
      const uvcpp_web_connection* conn = reg_find(cid);
      if (conn != nullptr) {
        ws->set_peer(conn->peer_ip, static_cast<unsigned int>(conn->peer_port));
      }
    }
  }
#endif
  // h3 没有 TCP 客户端，`peer_ip()` 留空（与 h3 的 HTTP 请求同一条口径）。

  const uint64_t id = ++next_ws_ext_id_;
  std::function<void(uint64_t)> reap = [this](uint64_t i) { reap_ws_ext(i); };

  std::unique_ptr<uvcpp_web_ws_session> session;
#if UVCPP_NGHTTP2_ENABLE
  if (h2 != nullptr) {
    std::unique_ptr<h2_ext_session> s(new h2_ext_session(reap, id));
    if (!s->start(*h2, h2_stream_id, req)) return false;
    session = std::move(s);
  }
#endif
#if UVCPP_HTTP3_ENABLE
  if (h3 != nullptr) {
    std::unique_ptr<h3_ext_session> s(new h3_ext_session(reap, id));
    // h3 的流号就在请求对象上（`h3_to_web_request` 已经填好）。
    if (!s->start(*h3, req.stream_id, req)) return false;
    session = std::move(s);
  }
#endif
  if (!session) return false;

  ws->bind_session(session.get());
  uvcpp_web_ws_session* raw = session.get();
  {
    std::lock_guard<std::mutex> lk(ws_ext_sessions_mu_);
    ws_ext_sessions_[id] = std::move(session);
  }

  UVCPP_LOG_INFO(log_category::WEBSOCKET)
      << "WS extended CONNECT 建立 " << ws->route()
      << (raw->transport() == uvcpp_http_version::HVER_30 ? " (h3)" : " (h2)");

  // 处理器返回后 `ws` 视图即失效（契约与 `websocket()` 相同）；会话本身归
  // `ws_ext_sessions_`。**顺序**：先把会话登记进表再跑处理器 —— 处理器里立刻
  // `close()` 会同步走到 `on_close` → 投递回收，那时表里必须已经有它。
  hit->second(*ws, *raw);
  return true;
}

void uvcpp_web_app::reap_ws_ext(uint64_t id) {
  // **推迟到下一轮**：这个函数是从 ws 连接的回调里同步进来的，在这里销毁会话
  // 等于把正在执行的连接回调连同它的 `this` 一起拆掉。投递到本循环的队列，
  // 等回调栈退干净再删。
  post([this, id]() {
    std::unique_ptr<uvcpp_web_ws_session> dead;
    {
      std::lock_guard<std::mutex> lk(ws_ext_sessions_mu_);
      std::map<uint64_t, std::unique_ptr<uvcpp_web_ws_session> >::iterator it =
          ws_ext_sessions_.find(id);
      if (it == ws_ext_sessions_.end()) return;
      dead = std::move(it->second);
      ws_ext_sessions_.erase(it);
    }
    // `dead` 在这里析构 —— 已经离开上面那把锁，而且是在回调栈退干净之后。
  });
}

}  // namespace uvcpp
