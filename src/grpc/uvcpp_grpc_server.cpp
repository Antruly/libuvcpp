/**
 * @file src/grpc/uvcpp_grpc_server.cpp
 * @brief `src/grpc/uvcpp_grpc_server.h` 的实现。
 * @author zhuweiye
 * @version 1.6.10
 */

#include <grpc/uvcpp_grpc_server.h>

#if UVCPP_WEB_ENABLE

#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

namespace uvcpp {

namespace {

/// 百分号编码用的十六进制表。**大写**：gRPC 规范里的例子就是大写，而小写虽然
/// 解出来一样，却会让"逐字节比对"这类判据（比如对端的日志）平白多一种形状。
const char kHexUpper[] = "0123456789ABCDEF";

}  // namespace

const char* grpc_content_type() { return "application/grpc"; }

std::string grpc_percent_encode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    // 可打印 ASCII 里除 `%` 之外原样走（`%` 自己是转义引导符，必须转）。
    if (c >= 0x20u && c <= 0x7Eu && c != '%') {
      out += static_cast<char>(c);
      continue;
    }
    out += '%';
    out += kHexUpper[c >> 4];
    out += kHexUpper[c & 0x0Fu];
  }
  return out;
}

http_headers grpc_ok_trailers() {
  http_headers t;
  http_header  h;
  h.name  = "grpc-status";
  h.value = "0";
  t.push_back(h);
  return t;
}

http_headers grpc_status_trailers(grpc_status status, const std::string& message) {
  http_headers t;
  http_header  h;
  h.name  = "grpc-status";
  h.value = std::to_string(static_cast<int>(status));
  t.push_back(h);
  h.name  = "grpc-message";
  h.value = grpc_percent_encode(message);
  t.push_back(h);
  return t;
}

// =========================================================================
// 发送
// =========================================================================

int grpc_send_unary(uvcpp_http_server& server, uvcpp_tcp_client* client,
                    uvcpp_http_response& resp, const std::string& message) {
  if (client == nullptr) return UV_EINVAL;
  // `stream_id == 0` 就是 HTTP/1.1（见 `uvcpp_http_response::stream_id`）。gRPC
  // 不在 h1 上存在 —— 没有尾随头，`grpc-status` 无处安放。硬走下去的话，
  // `begin_stream()` 会把非 0 的流号直接丢掉、`write_stream()` 按 h1 那套把
  // 帧字节当普通 body 发出去：对端收到一条"看着成功"的 200，而 gRPC 那一层
  // 永远等不到状态码。**宁可在这一句上失败得明明白白。**
  if (resp.stream_id == 0) return UV_EINVAL;

  // **先编码，再动状态。** 编不出来（消息太大）时此刻什么都还没发生，调用方
  // 还能改回一条 `RESOURCE_EXHAUSTED`。反过来的话首部已经上线、HTTP 状态码
  // 改不了了，只剩"截断 body 再关连接"这一条 —— 对 gRPC 客户端而言那是一次
  // **传输失败**，不是一次带状态的应答。
  std::string payload;
  const int   enc = grpc_frame_encode(message, payload);
  if (enc != 0) return enc;

  // 这条响应由本函数收尾：尾随头在 `write_stream()` 的 `done` 里发。没有这一句，
  // 处理函数返回时框架会拿那个空的 `resp` 去回一条 200 空响应，而下面那条流上
  // 的第二段头块就会撞在"这条流已经收过尾了"上。
  resp.deferred = true;

  // 另起一份头部对象，不复用 `resp`：`begin_stream()` 读的是 `status_code` 与
  // `headers`，而处理函数完全可能在调本函数之前往 `resp` 里放过别的东西
  // （比如 `resp = uvcpp_http_response::ok(...)`）。gRPC 这一层的形状没有第二种。
  uvcpp_http_response head;
  head.status_code = http_status::OK;
  head.set_header("content-type", grpc_content_type());
  server.begin_stream(client, resp.stream_id, head);

  const int32_t sid = resp.stream_id;
  return server.write_stream(client, sid, payload,
                             [&server, client, sid](int rc) {
                               // 非 0 = 这批字节没写出去（连接没了或正在关）。
                               // 尾随头跟着作废：此时发它要么被拒，要么变成一条
                               // 只带状态码的"应答" —— 两种都不该发生。
                               if (rc != 0) return;
                               server.send_trailers(client, sid, grpc_ok_trailers());
                             });
}

int grpc_send_status(uvcpp_http_server& server, uvcpp_tcp_client* client,
                     uvcpp_http_response& resp, grpc_status status,
                     const std::string& message) {
  if (client == nullptr) return UV_EINVAL;
  if (resp.stream_id == 0) return UV_EINVAL;  // 同 `grpc_send_unary()` 那条注释

  resp.deferred = true;

  uvcpp_http_response head;
  head.status_code = http_status::OK;
  head.set_header("content-type", grpc_content_type());
  server.begin_stream(client, resp.stream_id, head);

  // 没有 DATA 段 ⇒ DATA 队列是空的 ⇒ `send_trailers()` 的前置条件当场成立。
  return server.send_trailers(client, resp.stream_id,
                              grpc_status_trailers(status, message));
}

// =========================================================================
// 收
// =========================================================================

int grpc_parse_unary_body(const uvcpp_buf& body, std::string& message,
                          grpc_status& fail_status, std::string& err) {
  message.clear();
  err.clear();
  fail_status = grpc_status::INVALID_ARGUMENT;

  const char*  data = body.get_const_data();
  const size_t len  = body.size();
  if (data == nullptr || len == 0) {
    err = "gRPC unary 请求体是空的（至少该有一个 5 字节的帧前缀）";
    return UV_EINVAL;
  }

  const int rc = grpc_frame_decode_exact(data, len, message, &err);
  if (rc == 0) return 0;

  // 只有"太大"这一类是容量问题，其余都是"你发的不是一条合法请求"。
  if (rc == UV_EMSGSIZE) fail_status = grpc_status::RESOURCE_EXHAUSTED;
  return rc;
}

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
