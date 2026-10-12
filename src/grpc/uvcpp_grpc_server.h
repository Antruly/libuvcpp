/**
 * @file src/grpc/uvcpp_grpc_server.h
 * @brief gRPC 的**服务端**那一半：把 `uvcpp_grpc.h` 的信封接到 `uvcpp_http_server` 上。
 * @author zhuweiye
 * @version 1.6.11
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
 *
 * 底下那两条 `grpc_web_send_*`（web 框架那一侧）**还要** webapp
 * （`UVCPP_BUILD_WEBAPP=ON`）：它们的参数是 `uvcpp_web_response`，而 webapp 是
 * 可选模块且默认关。所以这一份头里住了三个依赖面不同的东西，各自的开关写在
 * 各自那一段前面 —— 与 `CMakeLists.txt` 里"只摘后半截"的摘法同一个理由。
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
#if UVCPP_WEBAPP_ENABLE
class uvcpp_web_response;
#endif

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
// 发送：web 框架那一侧
// =========================================================================
//
// 这一小节挂在**第二个**开关下：它要 `uvcpp_web_response`，那是 webapp 层的类型。
// webapp 是可选模块（`UVCPP_BUILD_WEBAPP`，默认 OFF），而 web 层不是 —— 所以
// "web 开着、webapp 关着"是一份合法且是**默认**的构建，那时这两个函数既不在
// 库里也不在这里，与整个文件在 web 关掉时的处境同形。
#if UVCPP_WEBAPP_ENABLE

/**
 * @brief 与 `grpc_send_unary()` 同一件事，但用的是 `uvcpp_web_app` 的响应对象。
 *
 * 两个版本的差别只有一个，而且是被框架结构定死的：**谁负责把三段的顺序排对**。
 *
 *   - 底层版（`grpc_send_unary()`）面对的是裸 `uvcpp_http_server`，没有人会在
 *     处理函数之后替它发送，所以它自己 `begin_stream → write_stream →（在
 *     `done` 里）send_trailers`，并把 `resp.deferred` 置真。
 *   - 本版面对的是 `uvcpp_web_response`，它的发送者是 `uvcpp_web_app`：框架在
 *     处理函数返回后统一发。所以这里只**声明**这条应答长什么样
 *     （`status` / `content-type` / body / `set_trailers()`），三段顺序由
 *     `uvcpp_web_app::send_response()` 包办 —— 那也是整个框架里唯一能同时
 *     看见 `write_stream` 的 `done` 与 `send_trailers()` 的地方。
 *
 * 头表里**描述 body 的那三个**（`content-length` / `transfer-encoding` /
 * `content-encoding`）会被本函数摘掉，理由见 `.cpp` 里那条注释（留着就是线上
 * 错帧）。**其余的头原地保留** —— 它们就是 gRPC 的 initial metadata，服务端
 * 捎带应答元数据是规范内的用法；处理函数在这之前设的 `x-*` 会照常上线。
 *
 * @return 0；`UV_EMSGSIZE`（编码后超过单条消息上限，此时 **@p resp 一个字段都
 *         没动**，调用方还可以改回一条 `RESOURCE_EXHAUSTED`）。
 *         **注意返回 0 只表示"这条应答已经声明好了"** —— 字节还没上线，与底层版
 *         "已入队"的口径不同。
 */
UVCPP_API int grpc_web_send_unary(uvcpp_web_response& resp,
                                  const std::string& message);

/**
 * @brief 与 `grpc_send_status()` 同一件事，但用的是 `uvcpp_web_response`。
 *
 * 一条**只有尾随头**的应答（Trailers-Only）：HTTP 状态码 200、body 为空、
 * 尾随块里带 `grpc-status`。框架看到 body 为空 + 有尾随头，就只发两段
 * （HEADERS → HEADERS），中间不插 DATA。
 *
 * 处理函数先前放进 body 的东西会被清掉，描述 body 的那三个头（见上一条）同样
 * 摘掉 —— 一条 Trailers-Only 带着 `content-length: 999` 上线是最坏的一种错帧
 * （对端会等一批永远不来的字节）。其余的头照上一条的规矩保留。
 *
 * 恒返回 0（这一段编码没有失败路径）。
 */
UVCPP_API int grpc_web_send_status(uvcpp_web_response& resp, grpc_status status,
                                   const std::string& message);

/**
 * @brief 收的那一半的 web 版：`grpc_parse_unary_body()` 的 `std::string` 重载。
 *
 * 存在的理由纯粹是**参数类型对不上**，而且不是能绕过去的那种：`uvcpp_web_request`
 * 暴露的是 `body_str()`（`std::string`），而底层那版收 `uvcpp_buf` ——
 * `uvcpp_web_request::take_from()` 把 body **搬**进了自己的 `body_`，源请求的
 * `src_.body` 那之后就空了（`uvcpp_web_request.h:120` 明写"调用之后不得再使用"），
 * 所以 `req.raw().body` 既不是"另一个能拿到同一份字节的入口"，也不该被碰。
 * 让每个 webapp 里写 gRPC 路由的人自己把 `std::string` 包回 `uvcpp_buf`
 * 只为了喂这一句，是把库内的类型选择漏到了使用者面前。
 *
 * 语义与 `uvcpp_buf` 那版**逐字相同**（同一个实现，只是入参形状不同）：
 * 空 body 与"`data` 为空"都是 `UV_EINVAL` + `INVALID_ARGUMENT`。
 */
UVCPP_API int grpc_parse_unary_body(const std::string& body, std::string& message,
                                    grpc_status& fail_status, std::string& err);

#endif  // UVCPP_WEBAPP_ENABLE

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
