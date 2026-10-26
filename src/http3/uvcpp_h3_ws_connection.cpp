/**
 * @file src/http3/uvcpp_h3_ws_connection.cpp
 * @brief WebSocket over HTTP/3（RFC 9220，#53）。
 */

#include "http3/uvcpp_h3_ws_connection.h"

#if UVCPP_HTTP3_ENABLE

#include <cstring>
#include <utility>

#include <web/uvcpp_http_request.h>

namespace uvcpp {

namespace {

h3_header make_status(int status) {
  h3_header h;
  h.name  = ":status";
  h.value = std::to_string(status);
  h.kind  = h3_header_kind::STATUS;
  return h;
}

h3_header make_regular(const std::string& name, const std::string& value) {
  h3_header h;
  h.name  = name;
  h.value = value;
  h.kind  = h3_header_kind::REGULAR;
  return h;
}

}  // namespace

uvcpp_h3_ws_connection::uvcpp_h3_ws_connection(uvcpp_h3_connection* h3,
                                               int64_t sid, callbacks cbs)
    : h3_(h3),
      life_(h3->life_token()),
      sid_(sid),
      cbs_(std::move(cbs)) {
  parser_.set_on_frame(
      [this](const uvcpp_ws_frame& f) { on_frame(f); });
  parser_.set_on_error([this](int code, const char* reason) {
    fail(code, reason != nullptr ? std::string(reason) : std::string());
  });
}

bool uvcpp_h3_ws_connection::linked() const {
  const std::shared_ptr<const char> life = life_.lock();
  return h3_ != nullptr && life && *life == 0;
}

uvcpp_h3_ws_connection::~uvcpp_h3_ws_connection() {
  // 摘掉流上的 sink：h3 连接里不能留一个指向已析构对象的 this。
  //
  // **先问令牌**：连接比本对象先销毁是合法时序（webapp 在连接收尾之后才回收
  // 会话），那时 `h3_` 已经悬垂 —— 唯一安全的判据只有连接自己那枚令牌。
  if (linked()) h3_->set_stream_sink(sid_, nullptr, nullptr);
}

std::unique_ptr<uvcpp_h3_ws_connection> uvcpp_h3_ws_connection::accept(
    uvcpp_h3_connection& h3, const h3_request& req, callbacks cbs) {
  if (req.protocol != "websocket") return nullptr;

  std::vector<h3_header> hs;
  hs.push_back(make_status(200));
  // 回显子协议（如果请求里提了）。RFC 8441/9220 不使用 `Sec-WebSocket-Accept`。
  for (size_t i = 0; i < req.headers.size(); ++i) {
    if (req.headers[i].name == "sec-websocket-protocol") {
      hs.push_back(make_regular("sec-websocket-protocol", req.headers[i].value));
      break;
    }
  }
  const int rc = h3.send_stream_response(req.stream_id, hs);
  if (rc != 0) return nullptr;

  std::unique_ptr<uvcpp_h3_ws_connection> ws(
      new uvcpp_h3_ws_connection(&h3, req.stream_id, std::move(cbs)));
  uvcpp_h3_ws_connection* raw = ws.get();
  h3.set_stream_sink(
      req.stream_id,
      [raw](const char* d, size_t n) { raw->on_data(d, n); },
      [raw]() { raw->on_end(); });
  return ws;
}

std::unique_ptr<uvcpp_h3_ws_connection> uvcpp_h3_ws_connection::accept(
    uvcpp_h3_connection& h3, int64_t stream_id, const uvcpp_http_request& req,
    callbacks cbs) {
  if (req.protocol != "websocket" || stream_id < 0) return nullptr;

  std::vector<h3_header> hs;
  hs.push_back(make_status(200));
  // 回显子协议（如果请求里提了）。与上面那个重载逐字相同 —— 两边的头类型
  // 不同（`h3_header` / `http_header`），但取的就是那一个名字与值。
  const std::string proto = http_get_header(req.headers, "sec-websocket-protocol");
  if (!proto.empty()) hs.push_back(make_regular("sec-websocket-protocol", proto));
  const int rc = h3.send_stream_response(stream_id, hs);
  if (rc != 0) return nullptr;

  std::unique_ptr<uvcpp_h3_ws_connection> ws(
      new uvcpp_h3_ws_connection(&h3, stream_id, std::move(cbs)));
  uvcpp_h3_ws_connection* raw = ws.get();
  h3.set_stream_sink(
      stream_id, [raw](const char* d, size_t n) { raw->on_data(d, n); },
      [raw]() { raw->on_end(); });
  return ws;
}

std::unique_ptr<uvcpp_h3_ws_connection> uvcpp_h3_ws_connection::connect(
    uvcpp_h3_connection& h3, const std::string& path,
    const std::string& authority, callbacks cbs) {
  h3_request req;
  req.method    = "CONNECT";
  req.protocol  = "websocket";
  req.authority = authority;
  req.path      = path.empty() ? std::string("/") : path;
  const int64_t sid = h3.send_stream_request(req);
  if (sid < 0) return nullptr;

  std::unique_ptr<uvcpp_h3_ws_connection> ws(
      new uvcpp_h3_ws_connection(&h3, sid, std::move(cbs)));
  uvcpp_h3_ws_connection* raw = ws.get();
  h3.set_stream_sink(
      sid, [raw](const char* d, size_t n) { raw->on_data(d, n); },
      [raw]() { raw->on_end(); });
  return ws;
}

int uvcpp_h3_ws_connection::send_frame(const uvcpp_ws_frame& f) {
  if (closed_ || !linked()) return -1;
  // RFC 8441 §5：h2/h3 上的 WebSocket 帧**不需要掩码**（两个方向都不掩）。
  uvcpp_ws_frame out = f;
  out.masked = false;
  const size_t n = uvcpp_ws_parser::calc_frame_size(out);
  std::string bytes(n, '\0');
  if (n > 0) uvcpp_ws_parser::build_frame(&bytes[0], out);
  return h3_->write_stream_data(sid_, bytes.data(), bytes.size());
}

int uvcpp_h3_ws_connection::send_text(const char* data, size_t len) {
  uvcpp_ws_frame f;
  f.opcode  = ws_opcode::TEXT;
  f.fin     = true;
  f.payload = uvcpp_buf(data != nullptr ? data : "", data != nullptr ? len : 0);
  return send_frame(f);
}

int uvcpp_h3_ws_connection::send_binary(const void* data, size_t len) {
  uvcpp_ws_frame f;
  f.opcode  = ws_opcode::BINARY;
  f.fin     = true;
  f.payload = uvcpp_buf(data != nullptr ? static_cast<const char*>(data) : "",
                       data != nullptr ? len : 0);
  return send_frame(f);
}

int uvcpp_h3_ws_connection::send_ping(const void* data, size_t len) {
  uvcpp_ws_frame f;
  f.opcode  = ws_opcode::PING;
  f.fin     = true;
  f.payload = uvcpp_buf(data != nullptr ? static_cast<const char*>(data) : "",
                       data != nullptr ? len : 0);
  return send_frame(f);
}

int uvcpp_h3_ws_connection::send_pong(const void* data, size_t len) {
  uvcpp_ws_frame f;
  f.opcode  = ws_opcode::PONG;
  f.fin     = true;
  f.payload = uvcpp_buf(data != nullptr ? static_cast<const char*>(data) : "",
                       data != nullptr ? len : 0);
  return send_frame(f);
}

int uvcpp_h3_ws_connection::close(ws_close_code code,
                                  const std::string& reason) {
  if (closed_ || !linked()) return -1;
  uvcpp_ws_frame f;
  f.opcode = ws_opcode::CLOSE;
  f.fin    = true;
  f.set_close_payload(code, reason);
  close_sent_ = true;
  const int rc = send_frame(f);
  h3_->end_stream_data(sid_);
  closed_ = true;
  return rc;
}

void uvcpp_h3_ws_connection::on_data(const char* data, size_t len) {
  if (closed_ || data == nullptr || len == 0) return;
  // 解析器每帧之后停在 COMPLETE，必须 reset 才能接着解下一帧（与
  // `uvcpp_ws_connection::on_tcp_data` 的循环同形）。
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

void uvcpp_h3_ws_connection::on_end() {
  if (closed_) return;
  closed_ = true;
  // 对端没发 Close 就结束了：按 RFC 6455 §7.1.5 报 1006。
  if (cbs_.on_close) cbs_.on_close(ws_close_code::ABNORMAL_CLOSE, std::string());
}

void uvcpp_h3_ws_connection::on_frame(const uvcpp_ws_frame& frame) {
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
          if (linked()) h3_->end_stream_data(sid_);
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
    in_message_  = true;
    message_op_  = frame.opcode;
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

void uvcpp_h3_ws_connection::fail(int code, const std::string& why) {
  if (closed_) return;
  if (cbs_.on_error) cbs_.on_error(code, why);
  uvcpp_ws_frame f;
  f.opcode = ws_opcode::CLOSE;
  f.fin    = true;
  f.set_close_payload(static_cast<ws_close_code>(code), why);
  close_sent_ = true;
  send_frame(f);
  if (linked()) h3_->end_stream_data(sid_);
  closed_ = true;
}

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE
