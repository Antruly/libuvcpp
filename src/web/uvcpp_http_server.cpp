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

namespace {

/**
 * @brief Whether the message being parsed carries a body the server will read.
 *
 * `get_content_length()` returns 0 both for "no body" and for "chunked", so it
 * cannot answer this on its own — the Transfer-Encoding header is what tells
 * the two apart.
 */
bool message_has_body(const uvcpp_http_parser* parser) {
  if (parser->get_content_length() > 0) return true;
  return http_name_equal(http_get_header(parser->get_headers(),
                                         "transfer-encoding"),
                         "chunked");
}

}  // namespace

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

    // Nothing registered for this exact method+path: give the claim hook a
    // chance. The framework installs one to reach its own router, which has
    // path parameters and wildcards that post_stream() cannot express.
    bool built_view = false;
    if (!sh && stream_claim_) {
      // The hook needs a request view, and the hook decides from it — so unlike
      // the stream-route path we must build it before knowing whether it is
      // used. `stream_view_built` lets on_request_complete reuse this one
      // instead of copying the header vector a second time.
      pctx->stream_request.method  = pctx->parser->get_method();
      pctx->stream_request.url     = pctx->parser->get_url();
      pctx->stream_request.version = pctx->parser->get_uvcpp_http_version();
      pctx->stream_request.headers = pctx->parser->get_headers();
      pctx->stream_view_built = true;
      built_view = true;
      try {
        sh = stream_claim_(pctx->stream_request, client);
      } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "[uvcpp_http_server] stream claim hook threw: %s\n",
                     e.what());
        sh = http_stream_handler();
      } catch (...) {
        std::fprintf(stderr,
                     "[uvcpp_http_server] stream claim hook threw (unknown)\n");
        sh = http_stream_handler();
      }
    }

    if (sh) {
      pctx->stream_handler = sh;
      if (!built_view) {
        pctx->stream_request.method  = pctx->parser->get_method();
        pctx->stream_request.url     = pctx->parser->get_url();
        pctx->stream_request.version = pctx->parser->get_uvcpp_http_version();
        pctx->stream_request.headers = pctx->parser->get_headers();
        pctx->stream_view_built = true;
      }
      sh(http_stream_event::HEADERS, nullptr, 0, pctx->stream_request, client);
      return;  // claimed: the body is no longer ours to police
    }

    // --- Nothing claimed it, so the server buffers the body itself and the
    // --- rules below apply. Order matters: a request we are going to reject
    // --- must not also be told to continue.
    if (!check_expect_header(*pctx, client)) return;           // 417, answered
    if (reject_oversized_declared(*pctx, client)) return;      // early 413

    // Expect: 100-continue with a body worth waiting for — tell the client it
    // may send. Raw bytes, not send_response(): a 1xx carries no Content-Length
    // and send_response() would add one for the empty body.
    if (pctx->expect_continue && message_has_body(pctx->parser)) {
      enqueue_write(*pctx, client, "HTTP/1.1 100 Continue\r\n\r\n");
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
    // Headers-time state must not survive into the next message on this
    // connection: a rejected message that somehow did not close must not
    // suppress the next message's routing, and a leftover 100-continue flag
    // would send a spurious interim response.
    ctx.rejected = false;
    ctx.close_after_message = false;
    ctx.defer_close_to_message_end = false;
    ctx.expect_continue = false;
    ctx.stream_view_built = false;
    ctx.stream_request = uvcpp_http_request();
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

  // Already answered during the headers callback (417 unknown Expect, 413 by
  // declared Content-Length). Everything below would answer a second time.
  // The close was deferred to here on purpose: the client has now stopped
  // sending, so closing no longer risks an RST that would destroy the response
  // sitting in its receive buffer.
  if (ctx.rejected) {
    if (ctx.close_after_message) {
      ctx.close_requested = true;
      pump_write(client);  // closes now, or after the pending write completes
    }
    return;
  }

  // Streaming route: deliver END; the handler responds asynchronously.
  if (ctx.stream_handler) {
    ctx.stream_handler(http_stream_event::END, nullptr, 0, ctx.stream_request, client);
    ctx.stream_handler = nullptr;
    ctx.stream_request = uvcpp_http_request();

    // The handler answered from a BODY callback and asked for the connection to
    // close. That close was parked by pump_write() because the peer was still
    // sending; it is safe now.
    if (ctx.defer_close_to_message_end) {
      ctx.defer_close_to_message_end = false;
      ctx.close_requested = true;
      pump_write(client);
    }
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

  // The claim hook made us build the request view at headers time. Reuse it —
  // rebuilding would clone the header vector a second time for every request,
  // just to throw the first copy away.
  uvcpp_http_request req;
  if (ctx.stream_view_built) {
    req = std::move(ctx.stream_request);
    ctx.stream_request = uvcpp_http_request();
    ctx.stream_view_built = false;
  } else {
    req.method  = ctx.parser->get_method();
    req.url     = ctx.parser->get_url();
    req.version = ctx.parser->get_uvcpp_http_version();
    req.headers = ctx.parser->get_headers();
  }
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

bool uvcpp_http_server::check_expect_header(conn_ctx& ctx,
                                            uvcpp_tcp_client* client) {
  // Expect is an HTTP/1.1 mechanism (RFC 7231 §5.1.1): in a 1.0 message the
  // header has no defined meaning, so it is ignored rather than failed. That
  // matters because clients from that era send things like "Expect: 100-continue"
  // through proxies that do not understand it.
  if (ctx.parser->get_uvcpp_http_version() != uvcpp_http_version::HVER_11)
    return true;

  std::string expect = http_get_header(ctx.parser->get_headers(), "expect");
  if (expect.empty()) return true;

  if (http_name_equal(expect, "100-continue")) {
    ctx.expect_continue = true;
    return true;
  }

  // RFC 7231 §5.1.1: an expectation the server cannot meet must be answered
  // 417 rather than silently ignored — the client is waiting for a signal that
  // a naive implementation would never send, and would hang until its own
  // timeout.
  reject_early(ctx, client, http_status::EXPECTATION_FAILED);
  return false;
}

bool uvcpp_http_server::reject_oversized_declared(conn_ctx& ctx,
                                                  uvcpp_tcp_client* client) {
  if (max_body_size_ == 0) return false;
  const uint64_t declared = ctx.parser->get_content_length();
  if (declared == 0 || declared <= max_body_size_) return false;

  // Refuse on the declared length, before the body arrives. Waiting for the
  // message to complete (the old path) means buffering up to the cap and then
  // reading the rest to nowhere — a client streaming 10 GB at a 1 MB limit is
  // answered immediately instead of after it has sent 10 GB.
  reject_early(ctx, client, http_status::PAYLOAD_TOO_LARGE);
  return true;
}

void uvcpp_http_server::reject_early(conn_ctx& ctx, uvcpp_tcp_client* client,
                                     http_status status) {
  const char* reason = http_status_reason(status);
  const std::string body =
      std::to_string(static_cast<int>(status)) + " " + reason;

  uvcpp_http_response resp =
      uvcpp_http_response::make(status, body.c_str(), body.size());
  resp.set_header("connection", "close");

  ctx.rejected = true;
  // Not now. The client is still sending the body, and closing a socket with
  // unread data in its receive buffer makes Windows send an RST — which throws
  // away bytes the peer has buffered but not yet read, including this response.
  // on_request_complete closes once the message has actually ended.
  ctx.close_after_message = true;
  // The body is going to arrive no matter what; don't accumulate a byte of it.
  ctx.body_overflow = true;
  ctx.body_buf.clear();

  send_response(client, resp, /*close_after_write=*/false);
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

  // HEAD 走「只序列化头部」那一支，**必须显式传 false**：清空 body 并不足以
  // 让报文正确 —— 一个声明了 `transfer-encoding: chunked` 的 HEAD 响应在
  // `to_string()` 里会照样补出终止块 `0\r\n\r\n`（那是 chunked 的**帧**，
  // 与 body 是否为空无关），于是 HEAD 凭空多出一段 body。这条与
  // `uvcpp_http_response.cpp` 里「空 body 也要发终止块」是同一个改动的两面：
  // 只改那边不改这里，就是修完一个缺陷立刻制造一个新缺陷。
  std::string wire = resp.to_string(/*include_body=*/!ctx.is_head);

  // close_after_write=false lets a streaming handler keep the connection open
  // to write the body after the header; it must close the connection itself.
  if (!keep_alive && close_after_write) ctx.close_requested = true;

  enqueue_write(ctx, client, std::move(wire));
}

void uvcpp_http_server::begin_stream(uvcpp_tcp_client* client,
                                     uvcpp_http_response& resp) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] Warning: stream dropped, connection is "
                 "no longer tracked (already closed?)\n");
    return;
  }
  conn_ctx& ctx = it->second;

  // keep-alive 的判定与 `send_response` 同一套（RFC 7230 §6.3）：handler 自己
  // 设了 `connection` 就听它的，否则看这条请求的解析结果。这里**不**决定
  // 关不关 —— 那由 end_stream(close_after) 定，否则头部写完那一刻队列是空的，
  // 非 keep-alive 的流会在第一块 body 之前就被关掉。
  bool keep_alive;
  if (resp.has_header("connection")) {
    keep_alive = !http_name_equal(resp.get_header("connection"), "close");
  } else {
    keep_alive = ctx.parser->should_keep_alive();
    resp.set_header("connection", keep_alive ? "keep-alive" : "close");
  }

  // **不**走 apply_compression：body 还不存在（见头文件里的说明）。
  //
  // **不**补 `content-length: 0`：长度语义由调用方选（已知长度就设
  // content-length，未知就设 transfer-encoding: chunked），本层不猜。

  // HEAD 走的就是"只序列化头部"那一支：头部与 GET 逐字节相同，body 一个
  // 字节都不发。`out_streaming` 仍然置位，好让 write_stream 的调用方拿到
  // 一致的语义（HEAD 上框架层会把 write_chunk 变成空操作，见 uvcpp_web_response）。
  ctx.out_streaming = true;

  enqueue_write(ctx, client, resp.to_string(/*include_body=*/false));
}

int uvcpp_http_server::write_stream(uvcpp_tcp_client* client, std::string bytes,
                                    std::function<void(int)> done) {
  auto it = contexts_.find(client);
  if (it == contexts_.end() || it->second.closing) return UV_ECANCELED;
  enqueue_write(it->second, client, std::move(bytes), std::move(done));
  return 0;
}

void uvcpp_http_server::end_stream(uvcpp_tcp_client* client, bool close_after) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return;
  conn_ctx& ctx = it->second;
  if (ctx.closing) return;

  ctx.out_streaming = false;
  if (close_after) ctx.close_requested = true;

  // 队列可能已经空了（最后一块写完了），所以必须主动泵一次：pump_write 会在
  // 空队列 + close_requested 两条同时成立时收尾。
  pump_write(client);
}

bool uvcpp_http_server::apply_compression(conn_ctx& ctx,
                                          uvcpp_http_response& resp) {
#if UVCPP_ZLIB_ENABLE
  if (!compress_enabled_) return false;

  // 流式响应（`transfer-encoding` 已经在头部里声明了）**不压缩**：body 还不
  // 存在，此刻按 `content_type()` 判定并打上 `content-encoding: gzip` 是
  // **声明了一件没发生的事**；而真要压一个流，得每块一个 deflate 上下文，
  // 那是另一个特性。
  //
  // 注意这是**第二道**门。第一道是结构性的：`begin_stream()` 这条入口压根
  // 不调用本函数。所以一个"忘了在 begin_stream 里设 transfer-encoding"的
  // 调用方会让这一句静默失效 —— 但那时它也没在走流式协议。
  if (resp.has_header("transfer-encoding")) return false;

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
                                      std::string wire,
                                      std::function<void(int)> done) {
  queued_write qw;
  qw.bytes = std::move(wire);
  qw.done = std::move(done);
  ctx.write_queue.push_back(std::move(qw));
  pump_write(client);
}

void uvcpp_http_server::fire_write_done(const std::shared_ptr<write_done>& d,
                                        int status) {
  if (!d || d->fired) return;
  d->fired = true;

  // 先换到栈上再调 —— 本仓已经踩过四次"正在执行的闭包被自己析构"（`delete
  // wr`、`delete w`、`~uvcpp_ws_client` 泵循环、`uvcpp_fs` 的 `operator=`）。
  // 这里的 `fn` 归 `d` 所有，而 `d` 是 `shared_ptr`，调用方手上那一份保证了
  // 它在回调期间活着；但"恰好一次"这个语义本身就该由这一行显式表达出来。
  std::function<void(int)> fn;
  fn.swap(d->fn);
  if (fn) fn(status);
}

void uvcpp_http_server::pump_write(uvcpp_tcp_client* client) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return;
  conn_ctx& ctx = it->second;

  // uvcpp_tcp_client holds at most one async write at a time; the queue is
  // what makes back-to-back keep-alive responses safe.
  if (ctx.write_pending || ctx.closing) return;

  if (ctx.write_queue.empty()) {
    // 流式响应里"队列空了"是常态（头部写完、下一块还没来），不是收尾信号。
    if (ctx.out_streaming) return;
    if (ctx.close_requested) {
      // The response that requested the close may have been written while the
      // peer was still sending its body (a claimed stream stopping an upload
      // early). Closing here would RST, and the RST discards that response from
      // the peer's receive buffer. Hand the close to the end of the message
      // instead — but only when there is a stream handler that will still be
      // alive to receive it. A malformed message never completes, and there the
      // close must happen now or the connection hangs until the idle sweep.
      if (ctx.stream_handler && !ctx.msg_done) {
        ctx.defer_close_to_message_end = true;
        ctx.close_requested = false;
      } else {
        close_connection(client);
      }
    }
    return;
  }

  ctx.write_pending = true;
  queued_write qw = std::move(ctx.write_queue.front());
  ctx.write_queue.pop_front();

  std::string wire = std::move(qw.bytes);

  // 在途这一块的 `done` 装进一次性结算器，并且 **ctx 也留一份**：网络层的完成
  // 回调在"客户端已被析构"时会直接早返回（`if (!token_alive(life))`），而服务端
  // 连接恰恰是被 tcp_server 的 close manager 删掉的 —— 没有 ctx 这一份兜底，
  // 对端在写入途中断开时这一块的 `done` 就永远不响了。见 write_done 的注释。
  std::shared_ptr<write_done> wd(new write_done());
  wd->fn = std::move(qw.done);
  ctx.inflight = wd;

  // The write copies the bytes into its own buffer, so `wire` need not outlive
  // this call.
  //
  // ---------------------------------------------------------------------
  // 重入次序铁律（本仓已踩过三次同族：`delete wr`、`delete w`、
  // `~uvcpp_ws_client` 里泵循环）。这个闭包持着 `conn_ctx&` —— 一个指向
  // `contexts_` 内部的引用 —— 而 `done` 里最自然的动作恰恰是**再写一块**
  // （重入 pump_write）或**关连接**（close_connection）。所以次序只能是：
  //
  //   ① 结算这次写（**只在这一步碰 contexts_**，出块前把要用的值拷出来）
  //   ② 在**不持有任何引用**的那一刻调用 done
  //   ③ 重新 find 再决定下一步（done 可能已经关了连接、也可能又写了新块）
  //
  // 回调之后**不许再碰 cc**。
  // ---------------------------------------------------------------------
  int rc = client->write(
      wire.c_str(), wire.size(), [this, client, wd](int status) {
        bool gone_from_table = false;
        bool peer_gone      = false;
        {
          auto c = contexts_.find(client);
          if (c == contexts_.end()) {
            gone_from_table = true;
          } else {
            conn_ctx& cc = c->second;
            cc.write_pending = false;
            // **先把在途这一块摘下来，再关连接。** 反过来的话
            // close_connection 会把它当成"还没结算的在途块"用 UV_ECANCELED
            // 唤醒，而这里明明拿到了真实结果（ECONNRESET 之类）—— 调用方
            // 就再也分不清"对端在写的时候走了"和"框架自己取消的"。
            cc.inflight.reset();
            if (status != 0) {
              // The peer is gone: nothing still queued can be delivered, and
              // the connection needs retiring. Any `done` still parked in the
              // queue is woken by close_connection() with UV_ECANCELED.
              peer_gone = true;
              close_connection(client);
            }
          }
        }

        if (gone_from_table) {
          // 连接已经从表里消失；这一块的下场仍然是"不会写出去了"。
          fire_write_done(wd, status != 0 ? status : UV_ECANCELED);
          return;
        }
        fire_write_done(wd, peer_gone ? status : 0);
        if (peer_gone) return;

        auto c2 = contexts_.find(client);
        if (c2 == contexts_.end() || c2->second.closing) return;

        // **不在这里判 `close_requested`。** 这里是"某一块写完了"，而
        // `close_requested` 的语义是"**我排的队写完**之后关"，不是"下一个写
        // 完成时关"。一个 handler 完全可能（而且流式响应里就是常态）在**头部
        // 还在途**的时候就把整条响应写完并调 `end_stream(close_after=true)`：
        // 那时队列里还压着 body 的每一块，而本次写完成的是**头部**。早先在这里
        // 判它，等于把整个 body 当成"写完头部之后的余量"丢掉 —— 线上症状是
        // 只剩一个头部块、连接随即关闭（`web_stream_response_func` 的
        // `stream_roundtrip` 就是这么红的）。
        //
        // 交给 pump_write 的空队列分支去收尾：它只在**队列真的空了**时才看
        // `close_requested`，而且那条分支还带着"对端还在发 → 把关闭推迟到
        // 消息结束"的判定（上传路径需要），在这里重写一遍必然漏掉那个判定。
        pump_write(client);
      });

  if (rc != 0) {
    // The write never started, so its completion callback will never fire.
    // Drop the queue and retire the connection instead of leaking it.
    // close_connection() 会把队列里每块的 done —— 以及在途那一块 —— 唤醒成
    // UV_ECANCELED。**但这一块的下场是本次真实的 rc**，所以先把它从 ctx 上摘
    // 下来（`fire_write_done` 的幂等因此由"谁先拿到"决定，而不是由时序碰运气），
    // 关完连接再用真实 rc 结算。
    ctx.write_pending = false;
    ctx.inflight.reset();
    close_connection(client);
    fire_write_done(wd, rc);
  }
}

void uvcpp_http_server::close_connection(uvcpp_tcp_client* client) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return;
  conn_ctx& ctx = it->second;
  if (ctx.closing) return;  // close is issued exactly once
  ctx.closing = true;

  // 队列里还没发出去的块，它们的 done **永远不会**被触发（连接要关了）。
  // 先整批搬出来，再等关闭动作做完之后逐个唤醒 —— 不能在这里边遍历边调：
  // done 里可能又往队列里写（那时 closing 已为真，pump_write 直接返回，
  // 不会递归回来），也可能让 close_connection 走 remove_ctx 那条路把 ctx 抹掉。
  std::deque<queued_write> dropped;
  dropped.swap(ctx.write_queue);

  // **在途那一块也要一起摘。** 它不在队列里 —— 它的 done 已经交给网络层的
  // 写完成回调了，而那条回调有一条早返回（`if (!token_alive(life)) return;`）：
  // 服务端连接是被 tcp_server 的 close manager 删掉的，于是"对端在写入途中
  // 断开"这条最普通的路会让它**永远不响**。同样必须在这里搬出来再唤醒，理由
  // 与 dropped 一样：erase(it) 之后 ctx 不可再用。
  std::shared_ptr<write_done> inflight;
  inflight.swap(ctx.inflight);

  const bool no_tcp = (client->get_tcp() == nullptr);

  if (no_tcp) {
    remove_ctx(client);  // 会 erase(it)，之后 ctx 不可再用
  }

  // 到这里 ctx 可能已经失效，所以下面**只读 dropped 与 inflight**。
  for (size_t i = 0; i < dropped.size(); ++i) {
    if (dropped[i].done) dropped[i].done(UV_ECANCELED);
  }
  fire_write_done(inflight, UV_ECANCELED);

  if (no_tcp) return;
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
  // 对端断开走的是**这条路**（`set_on_close` → 这里），不是 close_connection。
  // 所以队列里那几块的 done 必须在这里唤醒 —— 少了这一步，它们永远不响，而
  // 等它们的人（`send_file` 的传输对象、流式响应的调用方）就**永久挂起**。
  // 这不是理论风险：一个下载到一半的对端断开，服务端此刻队列里通常还压着
  // 好几块。
  //
  // 次序与 close_connection 一致：**先把 ctx 摘掉，再唤醒**。这样 done 里再写
  // （`write_stream` 找不到 ctx，如实返回非 0）或再关（`close_connection` 找
  // 不到 ctx，直接返回）都安全，不会踩到正在被销毁的 `conn_ctx`；而
  // `close_handler_`（框架的断连通知）排在最后，与"先唤醒、后通知框架"的既有
  // 次序一致 —— 否则 done 可能在框架已经拆掉这条连接的上下文之后才跑。
  auto it = contexts_.find(client);
  if (it != contexts_.end()) {
    std::deque<queued_write> dropped;
    dropped.swap(it->second.write_queue);
    // 在途那一块跟着一起摘（理由见 write_done 的注释）：网络层的写完成回调
    // 在"客户端已被析构"时会早返回，而这正是对端断开时发生的事。
    std::shared_ptr<write_done> inflight;
    inflight.swap(it->second.inflight);
    delete it->second.parser;
    contexts_.erase(it);

    for (size_t i = 0; i < dropped.size(); ++i) {
      if (dropped[i].done) dropped[i].done(UV_ECANCELED);
    }
    fire_write_done(inflight, UV_ECANCELED);
  }

  if (close_handler_) close_handler_(client);
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

void uvcpp_http_server::set_stream_claim(http_stream_claim hook) {
  stream_claim_ = std::move(hook);
}

void uvcpp_http_server::clear_stream_claim() { stream_claim_ = nullptr; }

bool uvcpp_http_server::has_stream_claim() const {
  return static_cast<bool>(stream_claim_);
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
