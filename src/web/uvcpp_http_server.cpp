/**
 * @file src/web/uvcpp_http_server.cpp
 * @brief Implementation of uvcpp_http_server — HTTP/1.1 server.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_http_server.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_http_parser.h>
#include <cctype>
#include <cstring>

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
  for (const auto& r : routes_) {
    if (r.method == method && r.path == path) return r.handler;
  }
  return default_handler_;
}

void uvcpp_http_server::on_tcp_connection(uvcpp_tcp_client* client) {
  conn_ctx ctx;
  ctx.parser = new uvcpp_http_parser(http_parser_mode::PARSE_REQUEST);
  contexts_[client] = ctx;

  conn_ctx* pctx = &contexts_[client];
  pctx->parser->set_on_body([pctx](const char* at, size_t len) {
    pctx->body_buf.append_data(at, len);
  });
  pctx->parser->set_on_headers_complete([pctx]() {
    pctx->headers_done = true;
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

  // For keep-alive: if previous request was completed, reset parser
  if (ctx.msg_done) {
    ctx.parser->reset();
    ctx.msg_done = false;
    ctx.headers_done = false;
    ctx.body_buf.clear();
  }

  ctx.parser->execute(buf->get_const_data(), buf->size());

  if (ctx.parser->has_error()) {
    uvcpp_http_response resp = uvcpp_http_response::make(
        http_status::BAD_REQUEST, "Bad Request", 11);
    std::string wire = resp.to_string();
    client->write(wire.c_str(), wire.size());
  }
}

void uvcpp_http_server::on_request_complete(uvcpp_tcp_client* client) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return;
  conn_ctx& ctx = it->second;
  ctx.msg_done = true;

  uvcpp_http_request req;
  req.method  = ctx.parser->get_method();
  req.url     = ctx.parser->get_url();
  req.version = ctx.parser->get_uvcpp_http_version();
  req.headers = ctx.parser->get_headers();
  req.body.clone(ctx.body_buf);

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

  // Keep-Alive logic (RFC 7230 Section 6.3):
  // HTTP/1.1 defaults to keep-alive unless Connection: close
  // HTTP/1.0 defaults to close unless Connection: keep-alive
  bool keep_alive = ctx.parser->should_keep_alive();
  if (keep_alive) {
    resp.set_header("connection", "keep-alive");
  } else {
    resp.set_header("connection", "close");
  }

  std::string wire = resp.to_string();
  client->write(wire.c_str(), wire.size());

  // If not keep-alive, close after write completes
  if (!keep_alive) {
    client->get_tcp()->close([](uvcpp_handle*) {});
  }
  // For keep-alive: ctx stays in contexts_ map, parser reset on next data
}

uvcpp_http_server::conn_ctx*
uvcpp_http_server::get_or_create_ctx(uvcpp_tcp_client* client) {
  auto it = contexts_.find(client);
  if (it != contexts_.end()) return &it->second;
  return nullptr;
}

void uvcpp_http_server::remove_ctx(uvcpp_tcp_client* client) {
  auto it = contexts_.find(client);
  if (it != contexts_.end()) {
    delete it->second.parser;
    contexts_.erase(it);
  }
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

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
