/**
 * @file src/grpc/uvcpp_grpc_server.cpp
 * @brief `src/grpc/uvcpp_grpc_server.h` 的实现。
 * @author zhuweiye
 * @version 1.6.11
 */

#include <grpc/uvcpp_grpc_server.h>

#if UVCPP_WEB_ENABLE

#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>
#if UVCPP_WEBAPP_ENABLE
#include <webapp/uvcpp_web_response.h>
#endif

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
  // 的线上协议定死 HTTP/2（gRPC 的 `PROTOCOL-HTTP2.md`），本模块不接 h1 ——
  // 即便 HTTP 层从 1.9.0 起能在 h1 上发 chunked 尾随头（RFC 9112 §7.1.2），
  // gRPC 客户端也不会到那条腿上收 `grpc-status`。硬走下去的话，
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
// 发送：web 框架那一侧
// =========================================================================

#if UVCPP_WEBAPP_ENABLE

namespace {

/**
 * @brief 摘掉**描述 body 的那几个**头：`content-length` / `transfer-encoding` /
 *        `content-encoding`。
 *
 * 为什么非摘不可，而且只摘这三个：`uvcpp_web_response::sync_meta()` 的规则是
 * "**已存在** `Content-Length` 或 `Transfer-Encoding: chunked` 时不动它" —— 那条
 * 规则对普通响应是对的（使用者在做分块/流式时不能被覆盖），但对这两条
 * `grpc_web_send_*` 是**致命的**：处理函数完全可能先设过一个
 * `content-length: 999` 再调本函数，于是这条应答带着"999 字节"的声明上线，而
 * 实际只发得出去 7 个 —— 对端会一直等剩下 992 个字节。**这是线上错帧，不是
 * 头表里多个字段**。`content-encoding` 同理：gRPC 自己的压缩走 `grpc-encoding`，
 * 一个 HTTP 层的 `content-encoding` 会让对端按别的算法去解这段帧。
 *
 * 其余的头**有意留着**：它们就是 gRPC 的 initial metadata（`:status` 之后、
 * DATA 之前那一段头块），服务端给客户端捎带应答元数据是规范内的用法，
 * 而这一段头块正是由 `set_header()` 攒出来的。
 */
void drop_body_framing_headers(uvcpp_web_response& resp) {
  resp.remove_header("content-length");
  resp.remove_header("transfer-encoding");
  resp.remove_header("content-encoding");
}

}  // namespace

int grpc_web_send_unary(uvcpp_web_response& resp, const std::string& message) {
  // 先编码、再动响应 —— 与底层版同一条理由：编不出来时此刻 `resp` 还是干净的，
  // 调用方还能改回一条 `RESOURCE_EXHAUSTED`。
  std::string payload;
  const int   enc = grpc_frame_encode(message, payload);
  if (enc != 0) return enc;

  drop_body_framing_headers(resp);
  resp.status(http_status::OK);
  resp.body(payload);
  // 放在 `body()` **之后**：那两版 `body()` 在 ct 非空时**也会**设 content-type，
  // 顺序反了就会被它盖成别的值。这里不传 ct，也就没有那一层歧义，但把这一句
  // 写在后面，读的人不必去核对 `body()` 的默认参数。
  resp.set_content_type(grpc_content_type());
  resp.set_trailers(grpc_ok_trailers());
  return 0;
}

int grpc_web_send_status(uvcpp_web_response& resp, grpc_status status,
                         const std::string& message) {
  // 不设 body：body 为空 + 有尾随头 ⇒ 框架走两段那条路（Trailers-Only）。
  // 显式清一次而不是"指望它是空的" —— 处理函数完全可能先往里放过东西
  // （比如一个 `json_str(...)` 的报错体），而 gRPC 这条应答的形状只有一种。
  resp.clear_body();
  drop_body_framing_headers(resp);
  resp.status(http_status::OK);
  resp.set_content_type(grpc_content_type());
  resp.set_trailers(grpc_status_trailers(status, message));
  return 0;
}

#endif  // UVCPP_WEBAPP_ENABLE

// =========================================================================
// 收
// =========================================================================

namespace {

/// `grpc_parse_unary_body()` 两个重载的**唯一**实现 —— 两份重载的差别只是入参
/// 形状（`uvcpp_buf` 与 `std::string`），语义一个字都不许分叉。
int parse_unary_bytes(const char* data, size_t len, std::string& message,
                      grpc_status& fail_status, std::string& err) {
  message.clear();
  err.clear();
  fail_status = grpc_status::INVALID_ARGUMENT;

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

}  // namespace

int grpc_parse_unary_body(const uvcpp_buf& body, std::string& message,
                          grpc_status& fail_status, std::string& err) {
  return parse_unary_bytes(body.get_const_data(), body.size(), message,
                           fail_status, err);
}

#if UVCPP_WEBAPP_ENABLE

int grpc_parse_unary_body(const std::string& body, std::string& message,
                          grpc_status& fail_status, std::string& err) {
  // `std::string` 的 `data()` 在 C++11 之后对空串也可能返回非空指针，而
  // `parse_unary_bytes()` 判的是 `data == nullptr || len == 0` —— 后者先成立，
  // 所以这里不必再单独处理空串。
  return parse_unary_bytes(body.data(), body.size(), message, fail_status, err);
}

#endif  // UVCPP_WEBAPP_ENABLE

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
