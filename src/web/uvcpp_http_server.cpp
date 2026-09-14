/**
 * @file src/web/uvcpp_http_server.cpp
 * @brief Implementation of uvcpp_http_server — HTTP/1.1 server.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_http_server.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_http_parser.h>
#include <web/uvcpp_http_compress.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace uvcpp {

uvcpp_http_server::uvcpp_http_server() {
  tcp_server_ = new uvcpp_tcp_server();
}

uvcpp_http_server::~uvcpp_http_server() {
  for (auto& kv : contexts_) {
    delete kv.second.parser;
  }
  contexts_.clear();
  delete tcp_server_;
  tcp_server_ = nullptr;
}

int uvcpp_http_server::bind(const char* ip, int port) {
  return tcp_server_->bind(ip, port);
}

int uvcpp_http_server::bindIpv4(const char* ip, int port) {
  return tcp_server_->bindIpv4(ip, port);
}

int uvcpp_http_server::bindIpv6(const char* ip, int port) {
  return tcp_server_->bindIpv6(ip, port);
}

int uvcpp_http_server::listen(int backlog) {
  int rc = tcp_server_->listen(
      [this](uvcpp_tcp_client* client) { on_tcp_connection(client); },
      backlog);
  if (rc == 0) status_ = HTTP_SERVER_LISTENING;
  return rc;
}

void uvcpp_http_server::on_request(http_request_handler handler) {
  default_handler_ = std::move(handler);
}

void uvcpp_http_server::on_upgrade(upgrade_handler_t handler) {
  upgrade_handler_ = std::move(handler);
}

void uvcpp_http_server::get(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_GET, path, std::move(h)});
}
void uvcpp_http_server::post(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_POST, path, std::move(h)});
}
void uvcpp_http_server::post_stream(const std::string& path, http_stream_handler h) {
  routes_.push_back({http_method::HTTP_POST, path, {}, std::move(h)});
}
void uvcpp_http_server::put(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_PUT, path, std::move(h)});
}
void uvcpp_http_server::del(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_DELETE, path, std::move(h)});
}
void uvcpp_http_server::options(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_OPTIONS, path, std::move(h)});
}
void uvcpp_http_server::patch(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_PATCH, path, std::move(h)});
}
void uvcpp_http_server::head(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_HEAD, path, std::move(h)});
}

http_request_handler uvcpp_http_server::find_handler(
    http_method method, const std::string& path) {
  // Route on "method + path" (query string stripped); handlers parse the query
  // themselves via parse_query(req.url). Without this, a request like
  // "GET /api/x?id=1" would never match a route registered as "/api/x".
  std::string p = path;
  size_t q = p.find('?');
  if (q != std::string::npos) p = p.substr(0, q);
  for (const auto& r : routes_) {
    if (r.method == method && r.path == p) return r.handler;
  }
  return default_handler_;
}

http_stream_handler uvcpp_http_server::find_stream_handler(
    http_method method, const std::string& path) {
  // Match on "method + path" (query string stripped), like find_handler.
  std::string p = path;
  size_t q = p.find('?');
  if (q != std::string::npos) p = p.substr(0, q);
  for (const auto& r : routes_) {
    if (r.method == method && r.path == p && r.stream_handler)
      return r.stream_handler;
  }
  return http_stream_handler();
}

void uvcpp_http_server::on_tcp_connection(uvcpp_tcp_client* client) {
  conn_ctx ctx;
  ctx.parser = new uvcpp_http_parser(http_parser_mode::PARSE_REQUEST);
  // 先自增再赋值：0 要留给"不在登记表里"，不然"第 0 条连接"和"没连接"就分不开了。
  ctx.generation = ++next_generation_;
  contexts_[client] = ctx;

  // 连接级钩子在**登记之后**才跑：钩子的用途就是"给这条连接做簿记"，而簿记
  // 的第一步是拿到它的身份 —— `connection_generation(client)` 得先有效。
  // （放在登记之前的话它恒返回 0，等于这个钩子拿不到连接标识，与文档说的
  // 「比任何请求都早的簿记点」自相矛盾。）仍然早于该连接上的第一个请求。
  //
  // 失败不能影响服务 —— 钩子是观察者，不是参与者，所以异常就地吞掉。
  if (connection_handler_) {
    try {
      connection_handler_(client);
    } catch (const std::exception& e) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] on_connection callback threw: %s\n",
                   e.what());
    } catch (...) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] on_connection callback threw (unknown)\n");
    }
  }

  conn_ctx* pctx = &contexts_[client];
  pctx->parser->set_on_body([this, pctx, client](const char* at, size_t len) {
    if (pctx->stream_handler) {
      pctx->stream_handler(http_stream_event::BODY, at, len,
                           pctx->stream_request, client);
      return;
    }
    // Over the cap: keep feeding the parser so the message can still complete,
    // but stop growing the buffer. The request is answered with 413 and never
    // routed, so a truncated body is fine.
    if (pctx->body_overflow) return;
    pctx->body_bytes += len;
    if (max_body_size_ != 0 && pctx->body_bytes > max_body_size_) {
      pctx->body_overflow = true;
      pctx->body_buf.clear();  // release what was buffered so far
      return;
    }
    pctx->body_buf.append_data(at, len);
  });
  pctx->parser->set_on_headers_complete([this, pctx, client]() {
    pctx->headers_done = true;
    // Resolve a streaming route now that method + url are known; when matched,
    // deliver HEADERS and hand body chunks directly to the handler.
    auto sh = find_stream_handler(pctx->parser->get_method(),
                                  pctx->parser->get_url());
    if (sh) {
      pctx->stream_handler = sh;
      pctx->stream_request.method  = pctx->parser->get_method();
      pctx->stream_request.url     = pctx->parser->get_url();
      pctx->stream_request.version = pctx->parser->get_uvcpp_http_version();
      pctx->stream_request.headers = pctx->parser->get_headers();
      sh(http_stream_event::HEADERS, nullptr, 0, pctx->stream_request, client);
    }
  });
  pctx->parser->set_on_message_complete([this, client]() {
    on_request_complete(client);
  });

  client->read_start([this, client](uvcpp_buf* buf) {
    if (buf && buf->size() > 0) on_connection_data(client, buf);
  });
  client->set_on_close([this, client]() { remove_ctx(client); });
}

void uvcpp_http_server::on_connection_data(uvcpp_tcp_client* client,
                                            uvcpp_buf* buf) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return;
  conn_ctx& ctx = it->second;

  // Raw-data hook: fires ahead of parsing, so a hook can observe the bytes
  // (return true) or take the connection over (return false). This is the only
  // place outside the parser that sees a connection's raw bytes.
  if (raw_data_hook_) {
    if (!raw_data_hook_(client, buf->get_const_data(), buf->size())) {
      return;  // consumed by the hook; the parser never sees this chunk
    }
  }

  // For keep-alive: if previous request was completed, reset parser and the
  // per-message state that goes with it.
  if (ctx.msg_done) {
    ctx.parser->reset();
    ctx.msg_done = false;
    ctx.headers_done = false;
    ctx.body_buf.clear();
    ctx.body_bytes = 0;
    ctx.body_overflow = false;
    ctx.accept_encoding.clear();
    ctx.is_head = false;
  }

  ctx.parser->execute(buf->get_const_data(), buf->size());

  if (ctx.parser->has_error()) {
    // A malformed request must not be answered by hand-rolled bytes: route it
    // through the normal response path so keep-alive/close handling, HEAD
    // suppression and write serialization all apply. `Connection: close` is
    // forced because the parser's state is no longer trustworthy.
    uvcpp_http_response resp = uvcpp_http_response::make(
        http_status::BAD_REQUEST, "Bad Request", 11);
    resp.set_header("connection", "close");
    send_response(client, resp);
  }
}

void uvcpp_http_server::on_request_complete(uvcpp_tcp_client* client) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return;
  conn_ctx& ctx = it->second;
  ctx.msg_done = true;

  // Streaming route: deliver END; the handler responds asynchronously.
  if (ctx.stream_handler) {
    ctx.stream_handler(http_stream_event::END, nullptr, 0, ctx.stream_request, client);
    ctx.stream_handler = nullptr;
    ctx.stream_request = uvcpp_http_request();
    return;
  }

  // Body over the cap: the buffer was truncated, so there is nothing sane to
  // route. Reject and close.
  if (ctx.body_overflow) {
    uvcpp_http_response resp = uvcpp_http_response::make(
        http_status::PAYLOAD_TOO_LARGE, "413 Payload Too Large", 23);
    resp.set_header("connection", "close");
    ctx.body_buf.clear();
    send_response(client, resp);
    return;
  }

  uvcpp_http_request req;
  req.method  = ctx.parser->get_method();
  req.url     = ctx.parser->get_url();
  req.version = ctx.parser->get_uvcpp_http_version();
  req.headers = ctx.parser->get_headers();
  req.body.clone(ctx.body_buf);

  // Capture what send_response() needs later: for a deferred response the
  // parser is reset (and its headers replaced) before the response is sent,
  // so these cannot be read back off the parser at send time.
  ctx.accept_encoding = http_get_header(req.headers, "accept-encoding");
  ctx.is_head = (req.method == http_method::HTTP_HEAD);

  uvcpp_http_response resp;

  // Check for WebSocket upgrade BEFORE routing
  if (upgrade_handler_) {
    std::string up = http_get_header(req.headers, "upgrade");
    if (!up.empty()) {
      bool is_ws = (up.size() == 9);
      for (size_t i = 0; is_ws && i < 9; i++)
        if (std::tolower(static_cast<unsigned char>(up[i])) != "websocket"[i]) is_ws = false;
      if (is_ws) {
        upgrade_handler_(req, client);
        return;
      }
    }
  }

  auto handler = find_handler(req.method, req.url);
  if (handler) {
    handler(req, resp, client);
  } else {
    resp = uvcpp_http_response::not_found();
  }

  // Compression is applied inside send_response(), NOT here: a deferred
  // response returns below and is sent later, so compressing at this point
  // would silently skip every async handler's response.

  // Deferred response: handler set deferred; it calls send_response later.
  if (resp.deferred) return;

  send_response(client, resp);
}

void uvcpp_http_server::send_response(uvcpp_tcp_client* client,
                                      uvcpp_http_response& resp,
                                      bool close_after_write) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) {
    // The connection is already gone. Callers reach this by responding after
    // the client disconnected, or twice for one request — a real bug worth
    // hearing about rather than a silently vanished response.
    std::fprintf(stderr,
                 "[uvcpp_http_server] Warning: response dropped, connection "
                 "is no longer tracked (already closed?)\n");
    return;
  }
  conn_ctx& ctx = it->second;

  // Server -> client compression. Done here so deferred responses are covered.
  apply_compression(ctx, resp);

  // Keep-Alive (RFC 7230 Section 6.3):
  // HTTP/1.1 defaults to keep-alive unless Connection: close
  // HTTP/1.0 defaults to close unless Connection: keep-alive
  bool keep_alive;
  if (resp.has_header("connection")) {
    // Respect an explicit header set by the handler (e.g. streaming endpoints
    // force close). Compare case-insensitively — field values are not required
    // to preserve case.
    keep_alive = !http_name_equal(resp.get_header("connection"), "close");
  } else {
    // Only consult the parser when the response does not decide for itself:
    // after a parse error its keep-alive state is not meaningful.
    keep_alive = ctx.parser->should_keep_alive();
    resp.set_header("connection", keep_alive ? "keep-alive" : "close");
  }

  if (ctx.is_head) {
    // A HEAD response carries the headers a GET would, including its
    // Content-Length, but no body. Pin Content-Length before dropping the
    // body, otherwise it would be computed as 0.
    if (!resp.has_header("content-length") && resp.body.size() > 0) {
      resp.set_header("content-length",
                      std::to_string(static_cast<unsigned long long>(
                          resp.body.size())));
    }
    resp.body.clear();
  } else if (!resp.has_header("content-length") &&
             !resp.has_header("transfer-encoding") && resp.body.size() == 0) {
    // An empty body still needs an explicit Content-Length: 0, otherwise a
    // keep-alive client cannot tell where this response ends.
    resp.set_header("content-length", "0");
  }

  std::string wire = resp.to_string();

  // close_after_write=false lets a streaming handler keep the connection open
  // to write the body after the header; it must close the connection itself.
  if (!keep_alive && close_after_write) ctx.close_requested = true;

  enqueue_write(ctx, client, std::move(wire));
}

bool uvcpp_http_server::apply_compression(conn_ctx& ctx,
                                          uvcpp_http_response& resp) {
#if UVCPP_ZLIB_ENABLE
  if (!compress_enabled_) return false;
  if (resp.body.size() < compress_min_body_) return false;
  // Bodies that must not be transformed: the client asked for no body, or the
  // status has no body by definition, or the handler already encoded it.
  if (ctx.is_head) return false;
  if (resp.has_header("content-encoding")) return false;
  switch (resp.status_code) {
    case http_status::NO_CONTENT:
    case http_status::NOT_MODIFIED:
      return false;
    default:
      break;
  }
  if (static_cast<int>(resp.status_code) < 200) return false;

  http_compress_method best =
      http_compress::parse_accept_encoding(ctx.accept_encoding);
  if (best == http_compress_method::NONE) return false;

  const std::vector<std::string>& excluded =
      compress_excluded_types_.empty() ? http_compress::default_excluded_mime_types()
                                       : compress_excluded_types_;
  if (!http_compress::should_compress(resp.content_type(), excluded)) return false;

  auto result = http_compress::compress(resp.body.get_const_data(),
                                        resp.body.size(), best);
  if (!result.success) return false;

  resp.body.clear();
  resp.body.clone(result.data);
  resp.set_header("content-encoding",
                  best == http_compress_method::GZIP ? "gzip" : "deflate");
  // Remove any stale Content-Length — to_string() re-adds it from the new
  // (compressed) body size.
  resp.remove_header("content-length");
  // Vary for CDN/proxy cache correctness (RFC 7231 §7.1.4). Set whenever we
  // made an encoding decision, so a cache never serves a compressed body to a
  // client that cannot decode it.
  if (!resp.has_header("vary")) resp.set_header("vary", "accept-encoding");
  return true;
#else
  (void)ctx;
  (void)resp;
  return false;
#endif
}

void uvcpp_http_server::enqueue_write(conn_ctx& ctx, uvcpp_tcp_client* client,
                                      std::string wire) {
  ctx.write_queue.push_back(std::move(wire));
  pump_write(client);
}

void uvcpp_http_server::pump_write(uvcpp_tcp_client* client) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return;
  conn_ctx& ctx = it->second;

  // uvcpp_tcp_client holds at most one async write at a time; the queue is
  // what makes back-to-back keep-alive responses safe.
  if (ctx.write_pending || ctx.closing) return;

  if (ctx.write_queue.empty()) {
    if (ctx.close_requested) close_connection(client);
    return;
  }

  ctx.write_pending = true;
  std::string wire = std::move(ctx.write_queue.front());
  ctx.write_queue.pop_front();

  // The write copies the bytes into its own buffer, so `wire` need not outlive
  // this call.
  int rc = client->write(wire.c_str(), wire.size(),
                         [this, client](int status) {
                           auto c = contexts_.find(client);
                           if (c == contexts_.end()) return;
                           conn_ctx& cc = c->second;
                           cc.write_pending = false;
                           if (status != 0) {
                             // The peer is gone: nothing still queued can be
                             // delivered, and the connection needs retiring.
                             cc.write_queue.clear();
                             close_connection(client);
                             return;
                           }
                           if (cc.close_requested) {
                             close_connection(client);
                           } else {
                             pump_write(client);
                           }
                         });

  if (rc != 0) {
    // The write never started, so its completion callback will never fire.
    // Drop the queue and retire the connection instead of leaking it.
    ctx.write_pending = false;
    ctx.write_queue.clear();
    close_connection(client);
  }
}

void uvcpp_http_server::close_connection(uvcpp_tcp_client* client) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return;
  conn_ctx& ctx = it->second;
  if (ctx.closing) return;  // close is issued exactly once
  ctx.closing = true;
  ctx.write_queue.clear();

  if (client->get_tcp() == nullptr) {
    remove_ctx(client);
    return;
  }
  // 走 `client->close()`，**不是** `client->get_tcp()->close(...)`。
  //
  // 后者只调 libuv 的关闭，读回调的 `nread < 0` 分支永远不会来 —— 而那是
  // 原来唯一会通知框架的地方。结果是：服务端主动关的连接，客户端对象既不被
  // 摘除也不被释放，永远留在 tcp_server 的登记表里（每个 `Connection: close`
  // 的响应漏一个，越跑越大）。`client->close()` 在关闭完成时补上这个通知。
  //
  // remove_ctx 不用在这里传：它已经挂在客户端的 on_close 上（见
  // on_tcp_connection），主动关闭和对端断开两条路都会经由 fire_close_callbacks
  // 恰好跑到它一次。传进来反而会让 on_connection_close 触发两次。
  client->close();
}

uint64_t uvcpp_http_server::connection_generation(
    uvcpp_tcp_client* client) const {
  if (client == nullptr) return 0;
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return 0;
  return it->second.generation;
}

bool uvcpp_http_server::is_connected(uvcpp_tcp_client* client) const {
  return connection_generation(client) != 0;
}

void uvcpp_http_server::remove_ctx(uvcpp_tcp_client* client) {
  if (close_handler_) close_handler_(client);
  auto it = contexts_.find(client);
  if (it != contexts_.end()) {
    delete it->second.parser;
    contexts_.erase(it);
  }
}

void uvcpp_http_server::set_max_body_size(size_t max_bytes) {
  max_body_size_ = max_bytes;
}

size_t uvcpp_http_server::max_body_size() const { return max_body_size_; }

void uvcpp_http_server::set_raw_data_hook(http_raw_data_hook hook) {
  raw_data_hook_ = std::move(hook);
}

void uvcpp_http_server::clear_raw_data_hook() { raw_data_hook_ = nullptr; }

bool uvcpp_http_server::has_raw_data_hook() const {
  return static_cast<bool>(raw_data_hook_);
}

void uvcpp_http_server::on_connection_close(
    std::function<void(uvcpp_tcp_client*)> cb) {
  close_handler_ = std::move(cb);
}

void uvcpp_http_server::on_connection(
    std::function<void(uvcpp_tcp_client*)> cb) {
  connection_handler_ = std::move(cb);
}

int uvcpp_http_server::run(uv_run_mode md) { return tcp_server_->run(md); }
void uvcpp_http_server::stop(std::function<void()> on_stopped) {
  status_ = HTTP_SERVER_STOPPING;
  tcp_server_->stop([this, on_stopped]() {
    status_ = HTTP_SERVER_STOPPED;
    if (on_stopped) on_stopped();
  });
}
int uvcpp_http_server::get_status() const { return status_; }
bool uvcpp_http_server::has_status(int flags) const { return (status_ & flags) == flags; }
uvcpp_tcp_server* uvcpp_http_server::get_tcp_server() { return tcp_server_; }

#if UVCPP_ZLIB_ENABLE
void uvcpp_http_server::set_compression_enabled(bool enable) {
  compress_enabled_ = enable;
}
bool uvcpp_http_server::is_compression_enabled() const {
  return compress_enabled_;
}
void uvcpp_http_server::set_compress_min_body_size(size_t min_size) {
  compress_min_body_ = min_size;
}
void uvcpp_http_server::set_compress_excluded_types(
    const std::vector<std::string>& types) {
  compress_excluded_types_ = types;
}
void uvcpp_http_server::add_compress_excluded_type(const std::string& mime_type) {
  compress_excluded_types_.push_back(mime_type);
}
#endif

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
