/**
 * @file src/http2/uvcpp_h2_ws_connection.cpp
 * @brief WebSocket over HTTP/2（RFC 8441，#58）。
 */

#include <http2/uvcpp_h2_ws_connection.h>

#if UVCPP_NGHTTP2_ENABLE

#include <cstring>
#include <utility>

#include <http2/uvcpp_h2_connection.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

namespace uvcpp {

uvcpp_h2_ws_connection::uvcpp_h2_ws_connection(uvcpp_h2_connection* h2,
                                               int32_t sid, callbacks cbs)
    : h2_(h2),
      life_(h2->life_token()),
      sid_(sid),
      cbs_(std::move(cbs)) {
  parser_.set_on_frame([this](const uvcpp_ws_frame& f) { on_frame(f); });
  parser_.set_on_error([this](int code, const char* reason) {
    fail(code, reason != nullptr ? std::string(reason) : std::string());
  });
}

bool uvcpp_h2_ws_connection::linked() const {
  const std::shared_ptr<const char> life = life_.lock();
  return h2_ != nullptr && life && *life == 0;
}

uvcpp_h2_ws_connection::~uvcpp_h2_ws_connection() {
  // 摘掉流上的落点：h2 会话里不能留一个指向已析构对象的 this。
  //
  // **先问令牌**：连接比本对象先销毁是合法时序（应用先拆了连接，会话晚一步
  // 才被回收）。那时 `h2_` 已经悬垂，唯一安全的判据只有连接自己那枚令牌。
  if (linked()) h2_->session().set_stream_sink(sid_, nullptr, nullptr);
}

std::unique_ptr<uvcpp_h2_ws_connection> uvcpp_h2_ws_connection::accept(
    uvcpp_h2_connection& h2, int32_t stream_id, const uvcpp_http_request& req,
    callbacks cbs) {
  if (req.protocol != "websocket" || stream_id <= 0) return nullptr;

  uvcpp_http_response resp;
  resp.version     = uvcpp_http_version::HVER_20;
  resp.status_code = http_status::OK;
  // RFC 8441 不使用 `Sec-WebSocket-Accept`；流上不再发 body 头。
  //
  // 子协议必须**挑一个**客户端提过的名字回，不能把整串提议原样回过去
  // （RFC 6455 §4.1；整串在两个客户端都合规时才现形）。这里取客户端列表里的
  // **第一个** —— 一个合法的服务端选择。需要按服务端偏好选的那条路在 h1 的
  // `uvcpp_ws_server::set_subprotocols()`，h2/h3 暂不接那张表。
  std::string proto = req.get_header("sec-websocket-protocol");
  const size_t comma = proto.find(',');
  if (comma != std::string::npos) proto.erase(comma);
  while (!proto.empty() && (proto[proto.size() - 1] == ' ' ||
                            proto[proto.size() - 1] == '\t')) {
    proto.erase(proto.size() - 1);
  }
  while (!proto.empty() && (proto[0] == ' ' || proto[0] == '\t')) {
    proto.erase(0, 1);
  }
  if (!proto.empty()) resp.set_header("sec-websocket-protocol", proto);

  const int rc = h2.send_headers(stream_id, resp);
  if (rc != 0) return nullptr;
  h2.flush();

  std::unique_ptr<uvcpp_h2_ws_connection> ws(
      new uvcpp_h2_ws_connection(&h2, stream_id, std::move(cbs)));
  uvcpp_h2_ws_connection* raw = ws.get();
  h2.session().set_stream_sink(
      stream_id, [raw](const char* d, size_t n) { raw->on_data(d, n); },
      [raw]() { raw->on_end(); });
  return ws;
}

std::unique_ptr<uvcpp_h2_ws_connection> uvcpp_h2_ws_connection::connect(
    uvcpp_h2_connection& h2, const std::string& path,
    const std::string& authority, callbacks cbs) {
  uvcpp_http_request req;
  req.method   = http_method::HTTP_CONNECT;
  req.protocol = "websocket";
  req.version  = uvcpp_http_version::HVER_20;
  req.url      = path.empty() ? std::string("/") : path;
  if (!authority.empty()) req.set_header("host", authority);

  const int32_t sid = h2.session().submit_request(req, std::string());
  if (sid < 0) return nullptr;  // 含 UV_EPROTONOSUPPORT（对端没宣告）
  h2.flush();

  std::unique_ptr<uvcpp_h2_ws_connection> ws(
      new uvcpp_h2_ws_connection(&h2, sid, std::move(cbs)));
  uvcpp_h2_ws_connection* raw = ws.get();
  h2.session().set_stream_sink(
      sid, [raw](const char* d, size_t n) { raw->on_data(d, n); },
      [raw]() { raw->on_end(); });
  return ws;
}

int uvcpp_h2_ws_connection::send_frame(const uvcpp_ws_frame& f) {
  if (closed_ || !linked()) return -1;
  // RFC 8441 §5：h2 上的 WebSocket 帧**不需要掩码**（两个方向都不掩）。
  uvcpp_ws_frame out = f;
  out.masked = false;
  const size_t n = uvcpp_ws_parser::calc_frame_size(out);
  std::string bytes(n, '\0');
  if (n > 0) uvcpp_ws_parser::build_frame(&bytes[0], out);
  return h2_->send_data(sid_, bytes.data(), bytes.size(), /*end_stream=*/false,
                        nullptr);
}

int uvcpp_h2_ws_connection::send_text(const char* data, size_t len) {
  uvcpp_ws_frame f;
  f.opcode  = ws_opcode::TEXT;
  f.fin     = true;
  f.payload = uvcpp_buf(data != nullptr ? data : "", data != nullptr ? len : 0);
  return send_frame(f);
}

int uvcpp_h2_ws_connection::send_binary(const void* data, size_t len) {
  uvcpp_ws_frame f;
  f.opcode  = ws_opcode::BINARY;
  f.fin     = true;
  f.payload = uvcpp_buf(data != nullptr ? static_cast<const char*>(data) : "",
                        data != nullptr ? len : 0);
  return send_frame(f);
}

int uvcpp_h2_ws_connection::send_ping(const void* data, size_t len) {
  uvcpp_ws_frame f;
  f.opcode  = ws_opcode::PING;
  f.fin     = true;
  f.payload = uvcpp_buf(data != nullptr ? static_cast<const char*>(data) : "",
                        data != nullptr ? len : 0);
  return send_frame(f);
}

int uvcpp_h2_ws_connection::send_pong(const void* data, size_t len) {
  uvcpp_ws_frame f;
  f.opcode  = ws_opcode::PONG;
  f.fin     = true;
  f.payload = uvcpp_buf(data != nullptr ? static_cast<const char*>(data) : "",
                        data != nullptr ? len : 0);
  return send_frame(f);
}

int uvcpp_h2_ws_connection::close(ws_close_code code,
                                  const std::string& reason) {
  if (closed_ || !linked()) return -1;
  uvcpp_ws_frame f;
  f.opcode = ws_opcode::CLOSE;
  f.fin    = true;
  f.set_close_payload(code, reason);
  close_sent_ = true;
  const int rc = send_frame(f);
  h2_->send_data(sid_, nullptr, 0, /*end_stream=*/true, nullptr);
  closed_ = true;
  return rc;
}

void uvcpp_h2_ws_connection::on_data(const char* data, size_t len) {
  if (closed_ || data == nullptr || len == 0) return;
  size_t off = 0;
  while (off < len && !closed_) {
    const size_t used = parser_.execute(data + off, len - off);
    const ws_parser_state st = parser_.get_state();
    if (st == ws_parser_state::COMPLETE) {
      if (used == 0) break;
      parser_.reset();
      off += used;
      continue;
    }
    if (st == ws_parser_state::PARSE_ERROR) break;  // on_error 已经报过
    break;
  }
}

void uvcpp_h2_ws_connection::on_end() {
  if (closed_) return;
  closed_ = true;
  // 对端没发 Close 就结束了：按 RFC 6455 §7.1.5 报 1006。
  if (cbs_.on_close) cbs_.on_close(ws_close_code::ABNORMAL_CLOSE, std::string());
}

void uvcpp_h2_ws_connection::on_frame(const uvcpp_ws_frame& frame) {
  if (closed_) return;

  if (frame.is_control_frame()) {
    switch (frame.opcode) {
      case ws_opcode::PING:
        send_pong(frame.payload.get_const_data(), frame.payload.size());
        if (cbs_.on_ping) {
          cbs_.on_ping(frame.payload.get_const_udata(), frame.payload.size());
        }
        break;
      case ws_opcode::PONG:
        if (cbs_.on_pong) {
          cbs_.on_pong(frame.payload.get_const_udata(), frame.payload.size());
        }
        break;
      case ws_opcode::CLOSE: {
        const ws_close_code code = frame.get_close_code();
        const std::string reason = frame.get_close_reason();
        if (!close_sent_) {
          uvcpp_ws_frame reply;
          reply.opcode = ws_opcode::CLOSE;
          reply.fin    = true;
          reply.set_close_payload(code, reason);
          close_sent_ = true;
          send_frame(reply);
          if (linked()) {
            h2_->send_data(sid_, nullptr, 0, /*end_stream=*/true, nullptr);
          }
        }
        closed_ = true;
        if (cbs_.on_close) cbs_.on_close(code, reason);
        break;
      }
      default:
        break;
    }
    return;
  }

  // 数据帧：RSV1 没有协商 permessage-deflate，必须为 0；RSV2/3 从未协商。
  if (frame.rsv1 || frame.rsv2 || frame.rsv3) {
    fail(static_cast<int>(ws_close_code::PROTOCOL_ERROR),
         "RSV bit set but no extension negotiated");
    return;
  }

  if (frame.opcode == ws_opcode::CONTINUATION) {
    if (!in_message_) {
      fail(static_cast<int>(ws_close_code::PROTOCOL_ERROR),
           "CONTINUATION with no message in progress");
      return;
    }
    message_.append(frame.payload.get_const_data(), frame.payload.size());
  } else {
    if (in_message_) {
      fail(static_cast<int>(ws_close_code::PROTOCOL_ERROR),
           "new data frame while fragmented message in progress");
      return;
    }
    in_message_ = true;
    message_op_ = frame.opcode;
    message_.assign(frame.payload.get_const_data(), frame.payload.size());
  }

  if (!frame.fin) return;

  in_message_ = false;
  if (message_op_ == ws_opcode::TEXT) {
    if (cbs_.on_text) cbs_.on_text(message_);
  } else if (cbs_.on_binary) {
    cbs_.on_binary(reinterpret_cast<const uint8_t*>(message_.data()),
                   message_.size());
  }
  message_.clear();
}

void uvcpp_h2_ws_connection::fail(int code, const std::string& why) {
  if (closed_) return;
  if (cbs_.on_error) cbs_.on_error(code, why);
  uvcpp_ws_frame f;
  f.opcode = ws_opcode::CLOSE;
  f.fin    = true;
  f.set_close_payload(static_cast<ws_close_code>(code), why);
  close_sent_ = true;
  send_frame(f);
  if (linked()) {
    h2_->send_data(sid_, nullptr, 0, /*end_stream=*/true, nullptr);
  }
  closed_ = true;
}

}  // namespace uvcpp

#endif  // UVCPP_NGHTTP2_ENABLE
