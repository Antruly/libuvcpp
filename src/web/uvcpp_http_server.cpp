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

#if UVCPP_NGHTTP2_ENABLE
#include "http2/uvcpp_h2_connection.h"
#include "http2/uvcpp_h2_session.h"
#endif

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
#if UVCPP_NGHTTP2_ENABLE
    // h2 层是连接上下文 new 出来的，反过来**没有任何别的表登记它** ⇒ 少了这
    // 一句就是每条还活着的 h2 连接漏一个 nghttp2 会话和它的缓冲区。
    //
    // 能走到这里说明 `remove_ctx` 没跑过：正常收尾（`close_all_clients()`）
    // 会经由客户端的关闭回调跑它，剩下的是"客户端先于服务端被释放"这种路
    // （`release_client` 只 `delete`，一个回调都不发）。
    //
    // 这里**不**唤醒队列里的 `done`：此刻持有者的上下文（webapp 那套）多半
    // 已经在拆，唤醒了也没有接收方。h1 那半同样是丢下队列直接走的 —— 与它
    // 对齐，而不是单独给 h2 编一套。
    delete kv.second.h2;
    kv.second.h2 = nullptr;
#endif
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

std::string uvcpp_http_server::take_upgrade_leftover(uvcpp_tcp_client* client) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return std::string();
  std::string out;
  out.swap(it->second.pending);
  it->second.upgrading = false;
  return out;
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
  // Route on "method + path" (query string stripped); parsing the query is left
  // to the handler (this layer has no query parser of its own — the webapp
  // layer's is `web_parse_query()`). Without this, "GET /api/x?id=1" would
  // never match a route registered as "/api/x".
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
#if UVCPP_NGHTTP2_ENABLE
  // 分流点。**不需要嗅字节**：`uvcpp_tcp_server` 已经保证"握手没完不投递连接"，
  // 走到这里 ALPN 已是终局。
  if (http2_enabled_ && client != nullptr && client->is_tls() &&
      client->tls_alpn_selected() == "h2") {
    on_tcp_connection_h2(client);
    return;
  }
#endif
  conn_ctx ctx;
  ctx.parser = new uvcpp_http_parser(http_parser_mode::PARSE_REQUEST);
  // 上限装在解析器上，而不是在这里的 `on_data` 里比大小：要挡的是"32 MiB 的
  // 头被完整解析一遍"，那就只能在解析**过程中**停，等头收完再看已经晚了。
  ctx.parser->set_max_header_bytes(max_header_bytes_);
  ctx.parser->set_max_url_bytes(max_url_bytes_);
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

  // 单消息暂存按**消息**清，不按读清。
  //
  // 两者不是一回事：一次读里完全可能有两条消息（流水线，或者只是不等响应就把
  // 下一条写出来），而"读在哪结束"由 TCP 分片决定。按读清的话，同一个读里的
  // 第二条请求会**带着上一条的 body**（`body_buf` 是累加的）与上一条的状态
  // （`rejected` / `expect_continue` …）—— 症状是第二条请求收到上一条的 body，
  // 且随着分片位置变化。
  pctx->parser->set_on_message_begin([pctx]() {
    pctx->msg_done = false;
    pctx->headers_done = false;
    pctx->body_buf.clear();
    pctx->body_bytes = 0;
    pctx->body_overflow = false;
    pctx->accept_encoding.clear();
    pctx->is_head = false;
    pctx->rejected = false;
    pctx->close_after_message = false;
    pctx->defer_close_to_message_end = false;
    pctx->expect_continue = false;
    pctx->stream_view_built = false;
    pctx->stream_request = uvcpp_http_request();
  });

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

  // Keep-alive: 上一条消息收完了就重置解析器，好让这次读里的下一条报文能解析。
  //
  // 判据必须是**解析器**收没收到消息尾（`COMPLETE`），不能是"这个连接上处理完过请求
  // 没有"—— 后者是 `msg_done`（在 `on_request_complete` 里置位），它跟"消息停在哪个
  // 字节"无关。坏就坏在两者错位的那一刻：一次读里前几条消息完成后 `msg_done` 已经是
  // 真，而解析器停在**下一条的中间**，于是下个读开头照着 `msg_done` 把半条报文
  // `llhttp_init` 掉 —— 断在头部中间则残尾被当成请求行（400 ＋ 关连接）；断在报文末尾
  // 那个空行上更安静：`llhttp_init` 落到 llhttp 的 `n_start`，那个状态把 `\r`/`\n` 当
  // 空行吞掉，半条报文被抹掉而那条请求不报错、不回、也不进任何表。
  //
  // `msg_done` 本身由 message-begin 那个钩子清（与单消息暂存共用一个清账点）；
  // 这里再清一次只是为了"reset 过就一定干净"，免得哪天 `execute()` 一个字节都
  // 没吃（零长读）时钩子不响，`msg_done` 卡在 true 挡住收尾路径（`:1070`）。
  if (ctx.msg_done && ctx.parser->get_state() == http_parser_state::COMPLETE) {
    ctx.parser->reset();
    ctx.msg_done = false;
  }

  const size_t used = ctx.parser->execute(buf->get_const_data(), buf->size());

  // 升级请求后面跟着的字节（升级请求和第一帧挤在同一个 TCP 段里到达，真实
  // 客户端几乎总是这样）**不能丢**：它们已经被这次读取走了，重新 read_start
  // 是读不回来的。存下来由升级方补投给新协议
  // （`uvcpp_ws_connection::feed_pending`）。
  //
  // `used` 在这里是可信的：升级请求走的是 `HPE_PAUSED_UPGRADE` 分支，那个
  // 分支按 `llhttp_get_error_pos` 算停下位置（实测"升级请求 156 + 首帧 15"
  // 的 171 字节批次报的就是 156）。**不要**把它推广成通用的"已消费字节数"：
  // `HPE_OK` 分支一律 `return len`（那是"消息吃完了"，不是"字节吃完了"），
  // 流水线的下一条请求就落在那条路径上 —— 与本条无关，不在这里动。
  //
  // 只在升级中才存；不是升级连接的话 `pending` 永远是空的。
  if (ctx.upgrading && used < buf->size()) {
    ctx.pending.assign(buf->get_const_data() + used, buf->size() - used);
  }

  // **必须排在 `has_error()` 前面**：撞上限同样会让解析器进 PARSE_ERROR
  // （回调返回非 0 就是这个效果），照 `has_error()` 那条路走会一律回 400，
  // 把"头太大"和"请求畸形"混成一件事，客户端也就无从知道自己该缩哪一样。
  const uvcpp_http_parser::size_limit hit = ctx.parser->limit_hit();
  if (hit != uvcpp_http_parser::size_limit::NONE) {
    // 消息半途被丢下了，永远不会有 `on_request_complete` —— 关闭只能搭在
    // 这次写上（`reject_early` 的默认延迟关闭在这里会等一个不来的事件，连接
    // 就一直挂着：**本层没有闲置清扫**，会来收的是 webapp 层那个，默认 60 s）。
    reject_early(ctx, client,
                 hit == uvcpp_http_parser::size_limit::URL
                     ? http_status::URI_TOO_LONG
                     : http_status::REQUEST_HEADER_FIELDS_TOO_LARGE,
                 /*message_will_complete=*/false);
    return;
  }

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
        // 升级之后，**同一次读**里剩下的字节属于新协议（WS 帧），不属于 HTTP。
        // 记下"这条连接正在升级"，等 `execute()` 返回到手了再把这批字节存起来
        // —— 升级回调是在 `execute()` **里面**同步调的，此刻还不知道剩下多少。
        ctx.upgrading = true;
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
                                     http_status status,
                                     bool message_will_complete) {
  const char* reason = http_status_reason(status);
  const std::string body =
      std::to_string(static_cast<int>(status)) + " " + reason;

  uvcpp_http_response resp =
      uvcpp_http_response::make(status, body.c_str(), body.size());
  resp.set_header("connection", "close");

  ctx.rejected = true;
  // The body is going to arrive no matter what; don't accumulate a byte of it.
  ctx.body_overflow = true;
  ctx.body_buf.clear();

  if (message_will_complete) {
    // Not now. The client is still sending the body, and closing a socket with
    // unread data in its receive buffer makes Windows send an RST — which throws
    // away bytes the peer has buffered but not yet read, including this response.
    // on_request_complete closes once the message has actually ended.
    ctx.close_after_message = true;
  }

  // 消息半途被丢下时反过来：等 `on_request_complete` 就是永远不等。
  // 队列写完由 `pump_write()` 收尾 —— 它认得这个形状（没有 stream handler
  // 且消息没完成 ⇒ 当场关），所以这里只要让 `close_requested` 立起来。
  send_response(client, resp, /*close_after_write=*/!message_will_complete);
}

size_t uvcpp_http_server::send_response(uvcpp_tcp_client* client,
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
    return 0;
  }
  conn_ctx& ctx = it->second;

#if UVCPP_NGHTTP2_ENABLE
  // h2 连接的延迟应答也回到这里 —— 发起它的处理函数手里只有 client 和 resp，
  // 没有流 id，所以流 id 是**随 resp 一起传下来**的（见 resp.stream_id）。
  if (ctx.h2 != nullptr) {
    send_h2_response(client, resp.stream_id, resp);
    // h2 那条路上体一个字节都没动（帧由 h2 会话自己切），所以这里如实报长度。
    return resp.body.size();
  }
#endif

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

  // 体是否**单独**作为第二块写出去（#17 的 `nbufs = 2`）。
  //
  // 判据是一条**逐字节相同**的等价关系：`to_string(false)` 接上 `resp.body`
  // 必须与 `to_string(true)` 一字不差。什么时候成立，看
  // `uvcpp_http_response::to_string` 就清楚 —— 非 chunked（chunked 会补十六进制
  // 帧与终止块，体不是裸字节接在头部后面）且体非空。
  //
  // 这里用 `has_header("transfer-encoding")` 而不是逐字照搬那边"扫到 chunked"
  // 那句：本库只发 chunked 这一种 transfer-encoding，而这个判据只可能**更保守**
  // （判错时退回合并那条路，多一次拷贝），不会悄悄走成"第二块不是那条报文的后半
  // 段"。方向选对了，两边就不会因为将来加一种编码而分叉。
  const bool two_bufs = !ctx.is_head && resp.body.size() > 0 &&
                        !resp.has_header("transfer-encoding");

  // HEAD 走「只序列化头部」那一支，**必须显式传 false**：清空 body 并不足以
  // 让报文正确 —— 一个声明了 `transfer-encoding: chunked` 的 HEAD 响应在
  // `to_string()` 里会照样补出终止块 `0\r\n\r\n`（那是 chunked 的**帧**，
  // 与 body 是否为空无关），于是 HEAD 凭空多出一段 body。这条与
  // `uvcpp_http_response.cpp` 里「空 body 也要发终止块」是同一个改动的两面：
  // 只改那边不改这里，就是修完一个缺陷立刻制造一个新缺陷。
  //
  // 两块那条路上序列化的正是同一个头部块（`to_string(false)` 返回的就是
  // `to_string(true)` 去掉末尾体之后的部分），所以 `two_bufs` 与 HEAD 这一条
  // 不冲突：HEAD 永远走"只序列化头部"。
  std::string wire = resp.to_string(/*include_body=*/!ctx.is_head && !two_bufs);

  // close_after_write=false lets a streaming handler keep the connection open
  // to write the body after the header; it must close the connection itself.
  if (!keep_alive && close_after_write) ctx.close_requested = true;

  // **体的长度必须在这里取走。** 两块那条路会把 `resp.body` 移动出去（移动的
  // 是句柄不是字节），之后 `resp.body.size()` 是 0 —— 而"这次上线了多少 body"
  // 正是调用方（`uvcpp_web_app::send_response` 的访问日志）要记的那个数。以前
  // 它是**在这之后**回头读 `resp.body` 拿到的，那条耦合到这里断了，所以改成
  // 由本函数返回。返回值就是取代那次回头读的东西。
  const size_t body_bytes = resp.body.size();

  if (two_bufs) {
    enqueue_write(ctx, client, std::move(wire), std::move(resp.body));
  } else {
    enqueue_write(ctx, client, std::move(wire));
  }
  return body_bytes;
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

#if UVCPP_NGHTTP2_ENABLE
  // h2 上这条入口不该被走到：调用方手上没有"这条连接唯一的那条流"这个前提，
  // 必须走带 `stream_id` 的重载。真走到了说明调用方还在按 h1 的形状调，此时
  // 下面第一句就会解引用 `ctx.parser`（h2 连接上恒为 nullptr）。
  //
  // 用 stderr 而不是静默返回：调用方在 h1 上走得好好的，切到 h2 忽然少一条
  // 响应、还没有任何提示，是最难查的一类。
  if (ctx.h2 != nullptr) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] begin_stream(client, resp) 在 HTTP/2 连接"
                 "上不能用（分不清是哪条流），请改用带 stream_id 的重载\n");
    return;
  }
#endif

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

void uvcpp_http_server::begin_stream(uvcpp_tcp_client* client, int32_t stream_id,
                                     uvcpp_http_response& resp) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] Warning: stream dropped, connection is "
                 "no longer tracked (already closed?)\n");
    return;
  }
  conn_ctx& ctx = it->second;

#if UVCPP_NGHTTP2_ENABLE
  // h2 上没有"连接级流式"这回事，也没有 chunked 组帧 —— 头部折成 HEADERS、
  // body 一块块折成 DATA，都由会话层做。长度靠 END_STREAM 划界，所以这里
  // **不**像 h1 那样要求调用方设 `transfer-encoding`。
  if (ctx.h2 != nullptr) {
    if (stream_id == 0) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] begin_stream() 在 HTTP/2 连接上收到 "
                   "stream_id=0 —— 分不清是哪条流，本响应已丢弃\n");
      return;
    }
    resp.stream_id = stream_id;
    ctx.h2->send_headers(stream_id, resp);
    return;
  }
#endif  // UVCPP_NGHTTP2_ENABLE

  // 走到这里说明这是一条 h1 连接，而调用方给了一个非 0 的流号 —— 那是 h2 的
  // 身份，在 h1 上没有意义。按 h1 继续会把 `stream_id` 静默丢掉，响应看起来
  // 正常发出、其实挂在了错误的身份上，所以宁可在这里说清楚。
  if (stream_id != 0) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] begin_stream() 收到 stream_id=%d，但这条"
                 "连接是 HTTP/1.1（h2 未启用或 ALPN 没协商出 h2）\n",
                 static_cast<int>(stream_id));
    return;
  }

  begin_stream(client, resp);
}

int uvcpp_http_server::write_stream(uvcpp_tcp_client* client, std::string bytes,
                                    std::function<void(int)> done) {
  return write_stream(client, 0, std::move(bytes), std::move(done));
}

int uvcpp_http_server::write_stream(uvcpp_tcp_client* client, int32_t stream_id,
                                    std::string bytes,
                                    std::function<void(int)> done) {
  auto it = contexts_.find(client);
  if (it == contexts_.end() || it->second.closing) return UV_ECANCELED;
  conn_ctx& ctx = it->second;

#if UVCPP_NGHTTP2_ENABLE
  if (ctx.h2 != nullptr) {
    if (stream_id == 0) return UV_EINVAL;  // h2 上没有"这条连接那条流"这回事
    // 会话层收下了就一定会回调 `done`（正常上线是 0，流中途没了是
    // UV_ECANCELED），所以这里返回 0 是**有保证**的那个 0 —— 与 h1 分支
    // 返回值那行契约逐条对齐。
    const int rv = ctx.h2->send_data(stream_id, bytes.data(), bytes.size(),
                                     /*end_stream=*/false, std::move(done));
    if (rv != 0) return UV_ECANCELED;  // 未受理，`done` 不会被调
    return 0;
  }
#endif  // UVCPP_NGHTTP2_ENABLE

  enqueue_write(ctx, client, std::move(bytes), std::move(done));
  return 0;
}

void uvcpp_http_server::end_stream(uvcpp_tcp_client* client, bool close_after) {
  end_stream(client, 0, close_after);
}

void uvcpp_http_server::end_stream(uvcpp_tcp_client* client, int32_t stream_id,
                                   bool close_after) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return;
  conn_ctx& ctx = it->second;
  if (ctx.closing) return;

#if UVCPP_NGHTTP2_ENABLE
  if (ctx.h2 != nullptr) {
    if (stream_id == 0) return;
    // 空块 + END_STREAM：h2 里"发完了"就是一个零长度的 DATA 帧带 END_STREAM
    // （没有 h1 那种终止块）。`close_after` 在 h2 上**不关连接** —— 见头文件。
    ctx.h2->send_data(stream_id, nullptr, 0, /*end_stream=*/true,
                      [](int) {});
    return;
  }
#endif  // UVCPP_NGHTTP2_ENABLE

  ctx.out_streaming = false;
  if (close_after) ctx.close_requested = true;

  // 队列可能已经空了（最后一块写完了），所以必须主动泵一次：pump_write 会在
  // 空队列 + close_requested 两条同时成立时收尾。
  pump_write(client);
}

// 压缩变体表的总字节上限。#11 第 3 条量出来的是"重复 deflate"这一项，
// 而 deflate 的产物通常比原文小，所以按**压缩后**字节记账真正占的内存只会更少。
#if UVCPP_ZLIB_ENABLE
static constexpr size_t kCompressVariantMaxBytes = 32u * 1024u * 1024u;

// 单条上限。没有它，一个大文件（静态路径可以到 MiB 级）的变体进来就能把整张表
// 冲干净 —— 于是"缓存"在最需要它的大文件上反而永远命中不了，典型的最坏组合。
static constexpr size_t kCompressVariantMaxEntry = 4u * 1024u * 1024u;

// 条数上限。和上面那条**不是一回事**：字节那条只按 body 字节算，而每条另外还挂着
// 一个键串（含着整条路径）和一个 map 节点 —— 一个满是**小文件**的静态根能造出大量
// "压完只剩几十字节"的条目，字节数永远涨不到上限、开销却全在键和节点上。两个上限
// 各封一件事：字节封大文件，条数封碎片。
static constexpr size_t kCompressVariantMaxEntries = 1024;

size_t uvcpp_http_server::compress_variant_total_bytes() const {
  size_t n = 0;
  for (std::map<std::string, compress_variant>::const_iterator it =
           compress_variants_.begin();
       it != compress_variants_.end(); ++it) {
    n += it->second.bytes();
  }
  return n;
}

void uvcpp_http_server::compress_variant_evict() {
  while ((compress_variant_total_bytes() > kCompressVariantMaxBytes ||
          compress_variants_.size() > kCompressVariantMaxEntries) &&
         !compress_variants_.empty()) {
    // 线性找最久未用的那次够用，且不必维护第二个容器与随之而来的迭代器失效问题。
    // 条数上限是 1024，所以最坏情况下这一趟是 O(n²)（每轮 O(n) 找 LRU，而字节那条
    // 腿可能要求删掉上千条才降到线下）—— 但触发它需要"几条接近单条上限的大条目 +
    // 上千条几十字节的小条目"这种混合表，且只在**存入**时进这个循环（deflate 之后
    // 的冷路径），不在请求路径上。
    auto victim = compress_variants_.begin();
    for (auto it = compress_variants_.begin(); it != compress_variants_.end();
         ++it) {
      if (it->second.last_used < victim->second.last_used) victim = it;
    }
    compress_variants_.erase(victim);
  }
}
#endif  // UVCPP_ZLIB_ENABLE

uvcpp_http_server::compress_variant_stat
uvcpp_http_server::compress_variant_stats() const {
#if UVCPP_ZLIB_ENABLE
  compress_variant_stat s;
  s.hits    = compress_variant_hits_;
  s.misses  = compress_variant_misses_;
  s.stored  = compress_variant_stored_;
  s.entries = compress_variants_.size();
  s.bytes   = compress_variant_total_bytes();
  return s;
#else
  // 头文件里明写了"zlib 关掉时返回全零、调用方不必跟着条件编译"，而成员本身
  // 在 `#if` 里 —— 少了这条分支，zlib 一关整个文件就编不过。本机默认树恰好是
  // 开的（CMakeLists 默认 OFF，webapp 树一律 ON），所以只在别的树/CI 上看得见。
  return compress_variant_stat();
#endif
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
  // 这里**不排除 HEAD**：RFC 9110 §9.3.2 要求 HEAD 发与 GET 相同的头字段，
  // 而 `content-length` 与 `content-encoding` 正是压缩决策的结果。对 HEAD 压
  // 一遍只是为了**算出那个长度**，压出来的字节不会被发出去 —— 上游
  // `send_response` 用 `to_string(include_body=false)` 序列化，body 在那里丢掉。
  //
  // 这里曾经有一句 `if (ctx.is_head) return false;`。它当时**够不着**（webapp
  // 层的 `sync_meta()` 已经把 body 清空，上面那道尺寸门先返回），所以删掉它
  // 看不出任何变化 —— 而 `sync_meta()` 那处一改，它就立刻变成真正的拦截点。
  // 两处是同一个缺陷的两条腿，必须一起改。
  if (resp.has_header("content-encoding")) return false;
  // 206 一律不编码。一旦带上 `content-encoding`，`content-range` 的字节坐标
  // 就该按**编码后**的流来算（RFC 9110 §8.4 把内容编码算作表示的一部分），
  // 而今天那条范围是在**原文件**上切的 —— 于是
  // `content-range: bytes 0-4095/253952` 与 `content-length: 1305` 描述的是
  // 两个不同的东西，续传方按前者推后者会接不上。206 **必须**带
  // `content-range`（§15.3.7），躲不开；要自洽就只能先压整个文件再切片，
  // 那正好把范围请求的意义抵消掉。所以与 nginx 同策：**有范围就不编码**。
  // 附带的好处是请求方拿到的正是它要的原始字节（续传到文件、seek 都要原字节，
  // 半路插进来的 gzip 帧只会让"先 200 后 206"的续传拼出坏文件）。
  //
  // 这也把两条路对齐了：流式那条（`send_file_range` → `begin_stream()`）压根
  // 不调用本函数，大文件的 206 本来就不压 —— 同一个状态码在两条路上行为不同
  // 本身就是缺陷，这一句让两边都变成"206 不编码"。
  switch (resp.status_code) {
    case http_status::NO_CONTENT:
    case http_status::NOT_MODIFIED:
    case http_status::PARTIAL_CONTENT:
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

  // 到这里"要压"已经定了。缓存只在**这之后**介入 —— 它记的是"同一份输入算过
  // 没有"，不参与上面任何一个判断。
  //
  // 这也是四个运行时旋钮（`set_compression_enabled` / `set_compress_min_body_size`
  // / `set_compress_excluded_types` / `add_compress_excluded_type`）天然安全的原因：
  // 配置一变，要么在门前就 early-return（压根不查表），要么查不到（miss）——
  // 不存在"配置改了、表里还发旧结果"这回事。**谁要把查表提到门前，这条就破了。**
  //
  // 另外半句（这一条是量出来的）：**提前查表也换不到你要的东西。** 热路径上曾经
  // 有过一次**纯白付**的拷贝 —— 静态层先把整份文件拷进 `resp.body`（248 KB 那
  // 档是 253952 B/响应），而命中那一支下一句 `clear()` 就把它丢掉了。消掉它的
  // 方向是让身体**不拷贝地就位**（`uvcpp_buf` 的共享手柄，见 #17 的 ①），不是把
  // 查表提前：查表再早，只要身体已经拷进来，那次拷贝照样白付。而且它**早不了**：
  // 键里的 `cache_tag` 与门槛要看的 `body.size()` 都是身体就位之后才有的。
  //
  // 键 = `编码字符 + cache_tag`，编码在前（理由见头文件）。今天
  // `http_compress::compress` 没有 level 参数、档位写死 `Z_DEFAULT_COMPRESSION`，
  // 所以 `(编码, tag)` 就是完整的键；**哪天加了档位旋钮，档位也必须进键**，
  // 否则表里会静默发出旧档位压出来的字节。
  const bool cacheable = !resp.cache_tag.empty() &&
                         resp.body.size() <= kCompressVariantMaxEntry;
  const std::string vkey =
      cacheable ? std::string(1, best == http_compress_method::GZIP ? 'g' : 'd') +
                      resp.cache_tag
                : std::string();

  // 头这三件事命中与未命中都要做，且**必须逐字一样** —— 命中路径与现算是同一个
  // 响应形状，差别只在字节从哪来。
  const auto finish_headers = [&resp, best]() {
    resp.set_header("content-encoding",
                    best == http_compress_method::GZIP ? "gzip" : "deflate");
    // Remove any stale Content-Length — to_string() re-adds it from the new
    // (compressed) body size.
    resp.remove_header("content-length");
    // Vary for CDN/proxy cache correctness (RFC 7231 §7.1.4). Set whenever we
    // made an encoding decision, so a cache never serves a compressed body to a
    // client that cannot decode it.
    if (!resp.has_header("vary")) resp.set_header("vary", "accept-encoding");
  };

  if (cacheable) {
    auto it = compress_variants_.find(vkey);
    if (it != compress_variants_.end()) {
      ++compress_variant_hits_;
      it->second.last_used = ++compress_variant_clock_;
      // 递句柄，一个字节都不拷（第 ④ 步）。`share()` 顺手把旧的那份放掉 ——
      // 静态层借来的那份就这么还回去，既不物化、也不多一次拷贝。
      resp.body.share(it->second.data);
      finish_headers();
      return true;
    }
    ++compress_variant_misses_;
  }

  const size_t src_size = resp.body.size();
  auto result = http_compress::compress(resp.body.get_const_data(),
                                        resp.body.size(), best);
  if (!result.success) return false;

  // 压出来的那份字节**只存在一份**：响应与表共用同一个句柄（第 ④ 步）。
  // 这一份拷贝仍在 —— 它是 deflate 的产出落进那个共享串（冷路径一次性的事）；
  // ④ 去掉的是另外两份：存表那份，以及每次命中拷回来的那份。
  const size_t out_n = result.data.size();
  const char *out_p = result.data.get_const_data();
  ::std::shared_ptr<const ::std::string> made =
      (out_p != nullptr && out_n > 0)
          ? ::std::make_shared<const ::std::string>(out_p, out_n)
          : ::std::make_shared<const ::std::string>();
  resp.body.share(made);
  finish_headers();

  // 存进表**不再拷**：存的就是上面那个 `made` —— 响应与表共用同一个句柄。
  // 以前这里要 clone 一份，理由是"响应自己那份还要发出去，不能 move 走"；
  // 句柄把"不能 move"换成了"不必拷"：`shared_ptr<const std::string>` 让两边
  // 同时活着，各持一个引用计数而已。这一份拷贝换来的是之后每次重复请求都省掉
  // 整个 deflate（两个数量级的差价），而现在连这一份也不用付了。
  //
  // 压完不比原文小就不存：那种 body 本来就压不动，存了也只是占地方。
  if (cacheable && resp.body.size() < src_size) {
    compress_variant v;
    v.data = made;
    v.last_used = ++compress_variant_clock_;
    compress_variants_[vkey] = std::move(v);
    ++compress_variant_stored_;
    compress_variant_evict();
  }
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

void uvcpp_http_server::enqueue_write(conn_ctx& ctx, uvcpp_tcp_client* client,
                                      std::string head, uvcpp_buf body,
                                      std::function<void(int)> done) {
  queued_write qw;
  qw.bytes = std::move(head);
  // 移动的是**句柄**（共享视图转引用计数、自有块转所有权），不是字节 ——
  // 这条队列本身就是"零拷贝"能不能成立的地方：这里如果拷了，后面两块写得再
  // 干净也白搭。见 `uvcpp_buf` 里"必须显式写移动"那段。
  qw.body     = std::move(body);
  qw.has_body = true;
  qw.done     = std::move(done);
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
      // close must happen now: **this layer has no idle sweep of its own**, so a
      // bare uvcpp_http_server (nothing above it) would hang for good — the
      // sweep that gets named elsewhere belongs to the webapp layer (60 s).
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
  // **`done` 为空时一个堆分配都不做。** `enqueue_write` 的五个调用点里有四个不传
  // `done`，而普通响应走的正是其中那条 —— 结算器的**全部**作用就是把那个闭包唤醒
  // 一次，而空闭包唤醒了什么都不做。`fire_write_done` 第一句是
  // `if (!d || d->fired) return;`，所以 `wd` 与 `ctx.inflight` 双双留空时，
  // 完成回调、`close_connection`、`remove_ctx` 三条路都走同一条"什么都不做"的早
  // 返回 —— 语义逐条相同。省下的是**每个响应两个堆分配**（`new write_done` 一个、
  // `shared_ptr` 控制块一个，这里没走 `make_shared`）外加两次原子引用计数。
  std::shared_ptr<write_done> wd;
  if (qw.done) {
    wd.reset(new write_done());
    wd->fn = std::move(qw.done);
  }
  ctx.inflight = wd;

  // 1 块时那次写会把字节拷进自己的缓冲，所以 `wire` 不必活过这一句。
  // **2 块时不是**：那边体不拷，写请求持有它（共享视图接引用计数、自有块接
  // 所有权）直到完成回调 —— 所以 `qw.body` 在 `write()` 返回之后即便析构也安全，
  // 前提是它的内容已经被交出去了（见 `uvcpp_tcp_client::write` 的消费语义）。
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
  // 回调提到两个重载外面来：两块那条路与一块那条路是**同一个**结算逻辑，
  // 抄成两份就是给"以后只改了一份"留口子（本仓的 ws 那族就是这么来的）。
  auto on_written = [this, client, wd](int status) {
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
  };

  int rc = qw.has_body
      ? client->write(wire.c_str(), wire.size(), &qw.body, on_written)
      : client->write(wire.c_str(), wire.size(), on_written);

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
#if UVCPP_NGHTTP2_ENABLE
    // h2 层**不拥有** client（那是 tcp_server 的 close manager 的责任），
    // 反过来 client 的上下文拥有 h2 层。对端断开与主动关闭两条路都会汇到这里。
    //
    // h2 这边还没上线的块也要跟上面的 h1 队列一样被唤醒，但**不能就地唤醒**：
    // `done` 跑的是框架流式响应的收尾，它可能再补一笔写，而此刻 ctx 还在、
    // `h2` 却已经没了 ⇒ `write_stream` 会掉进 h1 那条分支，把这笔写挂到一条
    // 根本不是 h1 的连接上（`stream_id` 被静默丢掉）。所以先取走，等 `erase`
    // 之后再跑 —— 那时 `write_stream` 找不到 ctx，如实返回 `UV_ECANCELED`。
    std::vector<std::function<void()>> h2_dropped;
    if (it->second.h2 != nullptr) {
      it->second.h2->take_cancelled_dones(h2_dropped);
    }
    delete it->second.h2;
    it->second.h2 = nullptr;
#endif
    contexts_.erase(it);

    for (size_t i = 0; i < dropped.size(); ++i) {
      if (dropped[i].done) dropped[i].done(UV_ECANCELED);
    }
    fire_write_done(inflight, UV_ECANCELED);
#if UVCPP_NGHTTP2_ENABLE
    for (size_t i = 0; i < h2_dropped.size(); ++i) h2_dropped[i]();
#endif
  }

  if (close_handler_) close_handler_(client);
}

size_t uvcpp_http_server::begin_h2_goaway() {
#if UVCPP_NGHTTP2_ENABLE
  // 这里**不关连接**，只打招呼。关是立刻生效的（`uv_close`），而 GOAWAY 得先
  // 排进队列再等几轮循环才出网 —— 两件事挤在一起做，对端拿到的就是"连接断了"，
  // 正好是这一句想避免的那个歧义。
  size_t n = 0;
  for (auto it = contexts_.begin(); it != contexts_.end(); ++it) {
    uvcpp_h2_connection* h2 = it->second.h2;
    if (h2 == nullptr || h2->closed()) continue;
    if (h2->begin_goaway() == 0) ++n;
  }
  return n;
#else
  return 0;
#endif
}

#if UVCPP_NGHTTP2_ENABLE

void uvcpp_http_server::on_tcp_connection_h2(uvcpp_tcp_client* client) {
  conn_ctx ctx;
  ctx.parser = nullptr;  // 与 h2 互斥：h2 连接上没有 llhttp
  ctx.generation = ++next_generation_;
  contexts_[client] = ctx;

  // 与本层 h1 那条路**次序一致**：登记之后才跑连接钩子，钩子里的
  // `connection_generation(client)` 才拿得到身份。
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

  auto it = contexts_.find(client);
  if (it == contexts_.end()) return;  // 钩子里就把连接关了
  uvcpp_h2_connection* h2 = new uvcpp_h2_connection(client, /*server_side=*/true);
  it->second.h2 = h2;

  // 回调里**一律重新查表**，不缓存 `conn_ctx*`：会话的回调会同步跑用户处理
  // 函数，用户函数可以关掉连接，而 `remove_ctx` 会把那个节点连同 h2 层一起删掉。
  uvcpp_h2_session::callbacks h2c;
  h2c.on_body = [this, client](uvcpp_h2_session&, uvcpp_h2_stream& st,
                               const char* d, size_t n) {
    auto cit = contexts_.find(client);
    if (cit == contexts_.end()) return;
    conn_ctx::h2_stream_state& ss = cit->second.h2_streams[st.stream_id];
    if (ss.overflow) return;  // 已经判过超限：后面的块直接丢，别再填回去
    if (max_body_size_ != 0 && ss.body.size() + n > max_body_size_) {
      // 超限就停止累积，但**继续收** —— 让这条流走到 on_request_end 才能
      // 回一个 413。中途 RST 会让对端拿到连接错误而不是一个明确的答复。
      ss.body.clear();
      ss.overflow = true;
      return;
    }
    ss.body.append(d, n);
  };
  h2c.on_request = [this, client](uvcpp_h2_session&, uvcpp_h2_stream& st, bool) {
    auto cit = contexts_.find(client);
    if (cit == contexts_.end()) return;
    // 流 id 只增不复用，正常这里本来就是空的；复位是为了万一有残留状态，也不会
    // 把上一条流的判定带给新流。**不能 erase** —— 无 body 的请求 `on_request` 和
    // `on_request_end` 是背靠背的（同上），擦掉之后那条请求就没人派发了。
    cit->second.h2_streams[st.stream_id] = conn_ctx::h2_stream_state();
  };
  h2c.on_request_end = [this, client](uvcpp_h2_session&, uvcpp_h2_stream& st) {
    auto cit = contexts_.find(client);
    if (cit == contexts_.end()) return;
    conn_ctx& c = cit->second;

    auto sit = c.h2_streams.find(st.stream_id);
    if (sit == c.h2_streams.end()) return;
    conn_ctx::h2_stream_state& ss = sit->second;

    std::string body;
    body.swap(ss.body);
    const bool overflow = ss.overflow;
    // 条目**留着**：延迟应答还要回来读 `is_head` / `accept_encoding`。

    if (overflow) {
      uvcpp_http_response resp =
          uvcpp_http_response::make(http_status::PAYLOAD_TOO_LARGE,
                                    "413 Payload Too Large", 23);
      send_h2_response(client, st.stream_id, resp);
      return;
    }

    uvcpp_http_request req = st.request;
    req.stream_id = st.stream_id;
    if (!body.empty()) req.body = uvcpp_buf(body.data(), body.size());
    dispatch_h2_request(client, st.stream_id, req);
  };
  h2c.on_close = [this, client](uvcpp_h2_session&, int32_t sid, uint32_t) {
    auto cit = contexts_.find(client);
    if (cit != contexts_.end()) cit->second.h2_streams.erase(sid);
  };

  uvcpp_h2_connection::callbacks cc;
  cc.on_disconnect = [](uvcpp_h2_connection&) {};

  // 对端断开 → 框架的关闭回调 → remove_ctx；主动关走 h2 层的 close()，同样
  // 汇到那里。两条路都负责把 h2 层删掉。
  client->set_on_close([this, client]() { remove_ctx(client); });

  const int rv = h2->start(h2c, cc);
  if (rv != 0) {
    std::fprintf(stderr, "[uvcpp_http_server] h2 start failed: %d\n", rv);
    h2->close_now();
  }
}

void uvcpp_http_server::dispatch_h2_request(uvcpp_tcp_client* client,
                                            int32_t stream_id,
                                            uvcpp_http_request& req) {
  auto it = contexts_.find(client);
  if (it == contexts_.end()) return;
  conn_ctx& ctx = it->second;

  auto sit = ctx.h2_streams.find(stream_id);
  if (sit == ctx.h2_streams.end()) return;
  // 按**这条流**记，不写连接级字段 —— 理由见 h2_stream_state 的说明。
  sit->second.accept_encoding = http_get_header(req.headers, "accept-encoding");
  sit->second.is_head = (req.method == http_method::HTTP_HEAD);

  uvcpp_http_response resp;
  resp.stream_id = stream_id;

  // 与 h1 那条路同一个路由表、同一个兜底处理函数 —— h2 换的是**传输**，
  // 不是"用哪个处理函数"。webapp 就是靠这个兜底处理函数接进来的。
  auto handler = find_handler(req.method, req.url);
  if (handler) {
    handler(req, resp, client);
  } else {
    resp = uvcpp_http_response::not_found();
    resp.stream_id = stream_id;
  }

  if (resp.deferred) return;  // 处理函数稍后自己调 send_response
  send_h2_response(client, stream_id, resp);
}

int uvcpp_http_server::send_h2_response(uvcpp_tcp_client* client,
                                        int32_t stream_id,
                                        uvcpp_http_response& resp) {
  auto it = contexts_.find(client);
  if (it == contexts_.end() || it->second.h2 == nullptr) return UV_EINVAL;
  conn_ctx& ctx = it->second;

  auto sit = ctx.h2_streams.find(stream_id);
  if (sit == ctx.h2_streams.end()) return UV_EINVAL;
  // 先取到局部量：`send_response` 会同步跑回调，回调可以关掉流甚至关掉连接，
  // 那之后 `sit` 就是悬垂引用了。
  const bool head = sit->second.is_head;

  // 压缩是**协议无关**的：h1 那条路做的事这里一件不能少，否则同一个处理函数
  // 在 h2 下会拿到未压缩的响应体。
  //
  // 但 `apply_compression` 读的是**连接级**的 `is_head` / `accept_encoding`
  // （那是 h1 的形状：一条连接同一时刻只有一个请求）。h2 上并发流各说各话，
  // 所以把这条流的值临时借过去 —— 本函数同步跑完，中途没有第二方读这两个字段。
  ctx.is_head         = head;
  ctx.accept_encoding = sit->second.accept_encoding;
  apply_compression(ctx, resp);

  if (head) {
    // HEAD 在 h2 里是"HEADERS 带 END_STREAM、一个 DATA 帧都不发"，但
    // `content-length` 仍按 GET 会有多长写 —— 所以先钉长度再抑制 body。
    if (!resp.has_header("content-length") && resp.body.size() > 0) {
      resp.set_header("content-length",
                      std::to_string(static_cast<unsigned long long>(
                          resp.body.size())));
    }
  } else if (!resp.has_header("content-length") && resp.body.size() == 0) {
    // h2 没有 chunked，空 body 靠 content-length: 0 说明边界。
    resp.set_header("content-length", "0");
  }

  // 连接专属头（含 keep-alive 需要的那条 `connection`）由会话层统一剥掉 ——
  // 这里**不调** h1 那套 keep-alive 判定：h2 的连接是长命的，没有"这条消息
  // 之后要不要关连接"这回事。
  return ctx.h2->send_response(stream_id, resp, /*omit_body=*/head);
}

#endif  // UVCPP_NGHTTP2_ENABLE

void uvcpp_http_server::set_max_body_size(size_t max_bytes) {
  max_body_size_ = max_bytes;
}

size_t uvcpp_http_server::max_body_size() const { return max_body_size_; }

void uvcpp_http_server::set_max_header_bytes(size_t max_bytes) {
  max_header_bytes_ = max_bytes;
}

size_t uvcpp_http_server::max_header_bytes() const { return max_header_bytes_; }

void uvcpp_http_server::set_max_url_bytes(size_t max_bytes) {
  max_url_bytes_ = max_bytes;
}

size_t uvcpp_http_server::max_url_bytes() const { return max_url_bytes_; }

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
