/**
 * @file src/grpc/uvcpp_grpc_server.h
 * @brief gRPC 的**服务端**那一半：把 `uvcpp_grpc.h` 的信封接到 `uvcpp_http_server` 上。
 * @author zhuweiye
 * @version 1.6.10
 *
 * 一条 unary 的 gRPC 应答在线上是**三段**，而且顺序是硬约束：
 *
 *     HEADERS   `:status: 200` + `content-type: application/grpc`
 *     DATA      帧（5 字节前缀 + 消息）
 *     HEADERS   尾随头 `grpc-status`（自带 END_STREAM）
 *
 * 第三段就是 gRPC 与普通 HTTP 的分界：HTTP 层的状态码**永远是 200**，真正的
 * 成败在 `grpc-status` 上，而它按规范必须走尾随头（gRPC 的
 * `PROTOCOL-HTTP2.md` 把 `grpc-status` 定死在那里，RFC 9113 §8.1 提供了"首部
 * 之后还能有一个头块"这条机制）。本库为此在 1.6.9 补了
 * `uvcpp_http_server::send_trailers()`。
 *
 * **这套顺序由 `grpc_send_unary()` 一个函数包办**，不留给每条路由自己拼：
 * 第二段 `write_stream()` 的 `done` 回调是"那批字节真的写完了"的唯一信号，
 * 尾随头必须在它里面发 —— 早一步就是"END_STREAM 在前、字节在后"的协议违例。
 * 把这套顺序留给调用方，等于让每个写 gRPC 路由的人各踩一次同一个坑，而且**踩错
 * 时没有任何本地症状**（对端只是解包失败或者干脆超时）。
 *
 * 本文件需要 web 层（`UVCPP_BUILD_WEB=ON`）：它的每一个参数都是 web 层的类型。
 * 编解码那一半没有这个依赖，见 `uvcpp_grpc.h`。
 */

#pragma once
#ifndef SRC_GRPC_UVCPP_GRPC_SERVER_H
#define SRC_GRPC_UVCPP_GRPC_SERVER_H

#include <uvcpp/uvcpp_config.h>

#include <string>

#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_export.h>
#include <web/uvcpp_http_common.h>

#include <grpc/uvcpp_grpc.h>

#if UVCPP_WEB_ENABLE

namespace uvcpp {

class uvcpp_http_server;
class uvcpp_http_response;
class uvcpp_tcp_client;

// =========================================================================
// 尾随头
// =========================================================================

/// gRPC 的 `content-type`。规范里就是这一个值，**不带参数**（带了参的对端要拒）。
UVCPP_API const char* grpc_content_type();

/**
 * @brief 成功路径的尾随头：**只有** `grpc-status: 0`。
 *
 * 为什么不像失败路径那样再带一个空的 `grpc-message`：那个字段按规范是"对失败
 * 的说明"，成功时它没有内容可说，而一个空值字段是"声明了一件没发生的事"。
 * 真实 gRPC 服务端的成功应答也只有 `grpc-status`。（空值字段本身是合法形状，
 * 只是这里没有必要。）
 */
UVCPP_API http_headers grpc_ok_trailers();

/**
 * @brief 失败路径的尾随头：`grpc-status` + `grpc-message`（已百分号编码）。
 *
 * @param status 取值见 `uvcpp_grpc.h` 的 `grpc_status`。本函数**不**校验取值
 *        范围（对端认不出的码按 `UNKNOWN` 处置，这是规范说的）。
 */
UVCPP_API http_headers grpc_status_trailers(grpc_status status,
                                            const std::string& message);

/**
 * @brief gRPC 的 `grpc-message` 用的那种百分号编码。
 *
 * 规范只允许 `0x20..0x7E` 里**除 `%` 外的**字符原样出现，其余一律 `%XX`
 * （大写十六进制）。所以一句中文的失败说明到了对端手里是一串
 * `%E4%B8%AD...`，而空串仍然是空串。
 */
UVCPP_API std::string grpc_percent_encode(const std::string& s);

// =========================================================================
// 发送
// =========================================================================

/**
 * @brief 发完一整条 unary 应答：`begin_stream` → `write_stream` →（在 `done`
 *        里）`send_trailers`。
 *
 * @param resp 处理函数拿到的那份响应对象。本函数只动它两个字段（`deferred` 与
 *        只读的 `stream_id`），**其余一概不看** —— 状态码与 `content-type` 由
 *        本函数定（gRPC 就是这么定的），body 由 @p message 编码得来。所以处理
 *        函数里写别的都不会生效，这是有意的：一条 gRPC 应答的形状不该有第二种。
 * @param message 要发的**消息字节**（尚未加帧）。空消息是合法的，它会编成
 *        5 个字节的"空帧"。
 *
 * @return `write_stream()` 那一步的结果：**0 = 已入队，尾随头会在写完时自动
 *         发出**；非 0 = 这条流已经没了，**尾随头不会发**（`write_stream()` 的
 *         契约：非 0 时 `done` 不会被调，所以不能指望它兜底）。
 *         `UV_EINVAL`（@p client 为空，或 `resp.stream_id == 0` —— gRPC 不在
 *         HTTP/1.1 上存在）；`UV_EMSGSIZE`（编码后超过单条消息上限，此时
 *         **什么都没发**，调用方还可以改回一条 `RESOURCE_EXHAUSTED`）。
 */
UVCPP_API int grpc_send_unary(uvcpp_http_server& server, uvcpp_tcp_client* client,
                              uvcpp_http_response& resp,
                              const std::string& message);

/**
 * @brief 一条**只有尾随头**的应答（gRPC 里的 "Trailers-Only"）。
 *
 * 与成功那条的差别是它**没有 DATA 段**：先 `begin_stream()` 发一段 HEADERS，
 * 紧接着 `send_trailers()` 发第二段带 END_STREAM 的 HEADERS。中间没有任何数据
 * 在飞，所以两句可以背靠背 —— `send_trailers()` 那条"body 一个字节都不欠"的
 * 前置条件在这一刻当场成立（它的判据只看 DATA 队列空不空，与发过几段头无关）。
 *
 * @return 两段里**先失败**的那个错误码；两段都成则 0。
 */
UVCPP_API int grpc_send_status(uvcpp_http_server& server, uvcpp_tcp_client* client,
                               uvcpp_http_response& resp, grpc_status status,
                               const std::string& message);

// =========================================================================
// 收
// =========================================================================

/**
 * @brief 解一条 unary 请求体：**恰好一帧**，并把该回的状态码一并给出来。
 *
 * 两个出参都是必填的，因为它们就是这个函数存在的理由：失败时要回的那条
 * Trailers-Only 需要**同时**拿到一句人话和一个 gRPC 状态码，分两次调用去凑
 * 只会让每条路由各写一遍那张对照表。
 *
 * 对照表（编解码层的错误码 → gRPC 状态码）：
 *   - `UV_EMSGSIZE` → `RESOURCE_EXHAUSTED`（对端说"这条消息很大"，是容量问题）；
 *   - 其余（前缀坏、压缩标志非 0、帧后还有字节、varint 截断、请求体为空）
 *     → `INVALID_ARGUMENT`（"你发的不是一条合法请求"）。
 *
 * @param message 成功时是解出来的消息字节；失败时被清空。
 * @param fail_status 失败时写回该用的状态码；成功时不写。
 * @param err 失败时是一句给人看的说明 —— 它正是该放进 `grpc-message` 的那句。
 *
 * @return 0；非 0 时就是编解码层那个错误码（不另行翻译，方便调用方自己判
 *         `UV_EMSGSIZE` 这类具体情形）。
 */
UVCPP_API int grpc_parse_unary_body(const uvcpp_buf& body, std::string& message,
                                    grpc_status& fail_status, std::string& err);

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE

#endif  // SRC_GRPC_UVCPP_GRPC_SERVER_H
