/**
 * @file src/capi/uvcpp_c_web.h
 * @brief web 层的 C 门面：`uvcpp_http_client` 与 `uvcpp_ws_client` 的精选面。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它对应哪些 C++ 能力
 * -------------------
 *   | 这里 | C++ 那侧 |
 *   |------|----------|
 *   | `uvcpp_c_http_client_new()` | `uvcpp_http_client()`（自带一条循环）|
 *   | `uvcpp_c_http_client_connect()` | `uvcpp_http_client::connect(host, port, cb)` |
 *   | `uvcpp_c_http_client_get()` | `uvcpp_http_client::get(path, cb)` |
 *   | `uvcpp_c_http_client_post()` | `uvcpp_http_client::post(path, body, len, ct, cb)` |
 *   | `uvcpp_c_http_client_run()` / `_stop()` | 同名的 `run()` / `stop()` |
 *   | `uvcpp_c_http_client_close()` | `get_tcp_client()->close()`（C++ 侧没有 `close()`）|
 *   | `uvcpp_c_ws_client_*` | `uvcpp_ws_client` 的同名方法 |
 *
 * **为什么服务端不在这里**
 * ------------------------
 * 这一层**只给客户端**。服务端的 C 面有两条完全不同的路，而两条都已经在
 * 别处到位了，再开第三条只会多出一份要维护的真相：
 *
 *   - **应用服务器**（路由、中间件、静态、ws 路由、延迟应答）走
 *     `uvcpp_c_webapp.h` —— 那是 webapp 层，功能是 `uvcpp_http_server` 的超集，
 *     而且它的 C 面已经把"谁持有连接、谁回收"这件事定成了**一条**规矩。
 *   - **裸 HTTP/1.1 服务器**（`uvcpp_http_server`）需要一套"C 侧回调拿到
 *     `uvcpp_http_request&` / `uvcpp_http_response&`、而那两个对象在框架里是
 *     按值持有在连接对象上的"生命周期规则 —— 那是**第二条**所有权模型
 *     （第一条是批 1 的"谁建谁废；服务端交出来的连接由框架回收"）。本层
 *     不做第二条，理由与 `uvcpp_c_net.h` 里"不提供 `take_client()`"同一条。
 *
 * 回调期句柄
 * ----------
 * `uvcpp_c_http_response` 与 `uvcpp_c_req` / `uvcpp_c_resp` 同一类：**只在
 * 那一次回调里有效**，回调一返回立刻反登记 + 毒化。要留 body 就当场拷走
 * （`uvcpp_c_http_response_body()` 给的指针回调返回后失效）—— 规矩 4 第二类。
 *
 * **`uvcpp_c_ws_client` 刻意不给连接句柄**：C++ 侧 `connect()` 的回调递出一个
 * `uvcpp_ws_connection*`，而"这条会话被回收了"这件事在 C 面**不可观测**
 * （与 `uvcpp_c_webapp.h` 里 `uvcpp_c_ws_conn` 那一段是同一条边界）。所以本层
 * 的收发都从**客户端**这一层走（`uvcpp_c_ws_client_send_text()` 转发给当前
 * 会话），事件表里也不带连接参数。这样一来 C 面手里从来就没有一枚会悬垂的
 * 连接句柄 —— 少了广播 / 服务端主动推送那类能力，换来的是"不可能用出一个
 * 悬垂指针"。需要那条能力时走 `uvcpp_ws_client` 的 C++ 面。
 *
 * 不提供（刻意）
 * --------------
 *   - `get_status()` 的位标志（`HTTP_CLIENT_CONNECTED` 那套）：与 `uvcpp_c_net.h`
 *     同一条理由 —— 位掩码一暴露就是第二份真相源。要问的两件事各有一个
 *     专门的函数（`_is_connected()` / `_last_error()`）。
 *   - `send_wait()` / `get_wait()` / `post_wait()` / `connect_wait()`：同步等
 *     要阻塞当前线程，而**这一层的每一种客户端都自带循环**，在回调里调它等于
 *     把循环卡死。异步那一套已经把这件事说清楚了，同步的只留 C++ 面。
 *     （批 1 的 `uvcpp_c_tcp_client_connect_wait()` 之所以给，是因为那个客户端的
 *     `run()` 与 `connect_wait()` 本来就是二选一的用法；这里不是。）
 *   - **流式响应体**（`on_body` 之类）：本批只给"整条响应到齐"这一次回调。
 *     要按块读就得再定一套"块回调 + 结束回调"的表，而 C++ 侧那条路
 *     （`send()` 的回调只在 `on_response_complete()` 时响一次）本来就不是
 *     流式的，本层如实跟着它，不假装有。
 *   - `set_ssl_context()` / `set_http2_enabled()` / `set_http3_enabled()` /
 *     `set_compression_enabled()` / ALPN 读数：全部要 C++ 类型或另一套握手，
 *     留给各自的批次（h2 / h3 那一批会给 `uvcpp_c_http2.h` / `uvcpp_c_http3.h`）。
 *     **因此本批的客户端只跑明文 HTTP/1.1**，`_is_connected()` 说的就是
 *     "TCP 连上了"。
 *   - `get_tcp_client()`：把底下的 TCP 客户端交出去就是第二条所有权模型。
 *     本层拿它做的事只有一件（`close()`），那是 `uvcpp_c_http_client_close()`。
 */

#pragma once
#ifndef SRC_CAPI_UVCPP_C_WEB_H
#define SRC_CAPI_UVCPP_C_WEB_H

#include <stddef.h>
#include <stdint.h>

#include "capi/uvcpp_c_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief HTTP 客户端句柄（**自带一条循环**）。定义在 `.cpp` 里。 */
typedef struct uvcpp_c_http_client uvcpp_c_http_client;

/**
 * @brief 一条 HTTP 响应（**回调期句柄**）。
 *
 * `uvcpp_c_http_response_cb` 递出来的就是它，回调返回后立刻失效
 * （再用是 `UVCPP_C_E_STALE`，不是崩溃）。
 */
typedef struct uvcpp_c_http_response uvcpp_c_http_response;

/** @brief WebSocket 客户端句柄（**自带一条循环**）。 */
typedef struct uvcpp_c_ws_client uvcpp_c_ws_client;

/* ------------------------------------------------------------------------
 * 回调
 * ------------------------------------------------------------------------ */

/**
 * @brief 一条响应到齐了（或这次请求失败了）。
 *
 * @param user_data 注册时给的那个指针，原样传回。
 * @param resp      响应句柄。**`error != 0` 时它可能是 NULL**（连接都没建立
 *                  起来就没有响应可言）。
 * @param error     0 = 成功；否则是 libuv 错误码或本层错误码。
 *
 * 在**循环线程**上调用。
 */
typedef void (*uvcpp_c_http_response_cb)(void* user_data,
                                         uvcpp_c_http_response* resp,
                                         int error);

/**
 * @brief 异步连接的结果。0 = 成功；否则 libuv 错误码。
 */
typedef void (*uvcpp_c_web_status_cb)(void* user_data, int status);

/* ------------------------------------------------------------------------
 * HTTP 客户端
 * ------------------------------------------------------------------------ */

/**
 * @brief 建一个**自带事件循环**的 HTTP/1.1 客户端。
 * @return 句柄；失败返回 NULL（内存不足）。
 */
UVCPP_C_API uvcpp_c_http_client* uvcpp_c_http_client_new(void);

/**
 * @brief 放掉它（连带它的循环与连接）。
 *
 * 与别的句柄同一条规矩：第二次 `free` 返回 `UVCPP_C_E_STALE`；在回调里
 * `free` 返回 `UVCPP_C_E_STATE`。
 */
UVCPP_C_API int uvcpp_c_http_client_free(uvcpp_c_http_client* client);

/**
 * @brief 要不要在响应之后留着连接（默认要）。
 *
 * 关掉的话每应答完一条就断开 —— **这让 `run()` 能自然返回**（循环里没有活跃
 * 句柄了）。开着的时候收完响应连接还在，得靠 `close()` 或 `stop()` 才能收摊。
 */
UVCPP_C_API int uvcpp_c_http_client_set_keep_alive(uvcpp_c_http_client* client,
                                                   int enable);

/**
 * @brief 异步连接。立刻返回，`cb` 在循环线程上被调用**一次**。
 *
 * @param host 主机名或点分 IP。**本批的 `uvcpp_http_client` 自己不做 DNS 解析
 *             （C++ 侧走的是 `uvcpp_tcp_client::connect` 那条 `uv_ip4_addr` 之路），
 *             所以这里给 **IPv4 点分串**最稳（`"127.0.0.1"`）。
 * @param port 1..65535。
 * @return `UVCPP_C_OK` 表示**已经开始**；结果在 `cb` 里。
 */
UVCPP_C_API int uvcpp_c_http_client_connect(uvcpp_c_http_client* client,
                                            const char* host, int port,
                                            uvcpp_c_web_status_cb cb,
                                            void* user_data);

/**
 * @brief 发一条 GET。**必须先连接**（没连上返回 `UV_ENOTCONN`）。
 *
 * @param path 路径（含 query），如 `"/text?a=1"`。
 * @return 0 = 已经发出去（结果在 `cb` 里）；负数 = 没发出去。
 */
UVCPP_C_API int uvcpp_c_http_client_get(uvcpp_c_http_client* client,
                                        const char* path,
                                        uvcpp_c_http_response_cb cb,
                                        void* user_data);

/**
 * @brief 发一条 POST。
 *
 * @param body 立刻被拷走（规矩 4 第一类）；`len` 为 0 时允许为 NULL。
 * @param content_type NULL = `application/octet-stream`（与 C++ 侧默认一致）。
 */
UVCPP_C_API int uvcpp_c_http_client_post(uvcpp_c_http_client* client,
                                         const char* path, const void* body,
                                         size_t len, const char* content_type,
                                         uvcpp_c_http_response_cb cb,
                                         void* user_data);

/**
 * @brief 跑这条循环，直到 `stop()`、或（关掉 keep-alive 之后）所有请求都答完
 *        且连接自然断开。
 */
UVCPP_C_API int uvcpp_c_http_client_run(uvcpp_c_http_client* client);

/** @brief 让 `run()` 停下来。可以从别的线程调。 */
UVCPP_C_API int uvcpp_c_http_client_stop(uvcpp_c_http_client* client);

/**
 * @brief 主动断开底下的连接。
 *
 * C++ 侧没有这个口子（`uvcpp_http_client` 靠 keep-alive 复用连接），本层加它
 * 是因为"自带循环的 C 客户端"最常见的收摊写法就是"收完最后一个响应就关"。
 * 已经关了或还没连过是**无操作**，不是错误。
 */
UVCPP_C_API int uvcpp_c_http_client_close(uvcpp_c_http_client* client);

/** @brief TCP 连上了没有（1/0）。负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_http_client_is_connected(uvcpp_c_http_client* client);

/** @brief 上次出错的错误码（0 = 没出过错）。 */
UVCPP_C_API int uvcpp_c_http_client_last_error(uvcpp_c_http_client* client);

/* ------------------------------------------------------------------------
 * HTTP 响应（回调期）
 * ------------------------------------------------------------------------ */

/** @brief 状态码（200 / 404 …）。负数 = 错误码（句柄已失效）。 */
UVCPP_C_API int uvcpp_c_http_response_status_code(uvcpp_c_http_response* resp);

/** @brief 状态短语（`"OK"`），走"调用方给缓冲区"那套约定。 */
UVCPP_C_API int uvcpp_c_http_response_status_message(
    uvcpp_c_http_response* resp, char* buf, size_t cap);

/**
 * @brief 取一个响应头。
 * @return 值的长度（>= 0）；`UVCPP_C_E_NOT_FOUND` = **没有这个头**
 *         （与"有、但值是空串"分得开）。
 */
UVCPP_C_API int uvcpp_c_http_response_header(uvcpp_c_http_response* resp,
                                             const char* name, char* buf,
                                             size_t cap);

/** @brief 有没有这个头（1/0）；负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_http_response_has_header(uvcpp_c_http_response* resp,
                                                 const char* name);

/** @brief `Content-Type`，走"调用方给缓冲区"那套约定。 */
UVCPP_C_API int uvcpp_c_http_response_content_type(
    uvcpp_c_http_response* resp, char* buf, size_t cap);

/**
 * @brief 响应体（**二进制安全**，可能含 NUL）。
 *
 * @param data [出] 指向缓冲区的指针。**只在那次回调里有效**。
 * @param len  [出] 字节数。
 * @return `UVCPP_C_OK`；负数 = 错误码。
 */
UVCPP_C_API int uvcpp_c_http_response_body(uvcpp_c_http_response* resp,
                                           const char** data, size_t* len);

/* ------------------------------------------------------------------------
 * WebSocket 客户端
 * ------------------------------------------------------------------------ */

/**
 * @brief WS 客户端会报的那几件事。
 *
 * 与 `uvcpp_c_ws_events`（webapp 那侧）的差别只有一处：**这里没有连接参数**
 * —— 客户端侧的会话句柄不给 C 面（见文件开头），收发都从客户端走。
 *
 * 与别的事件表同一条规矩：`size` 打头、逐字段看、没覆盖的格子当"不关心"。
 * `data` / `reason` / `what` 都**只在本次回调里有效**。
 */
typedef struct uvcpp_c_ws_client_events {
  /** @brief **必须**是 `sizeof(uvcpp_c_ws_client_events)` 或你那份结构体的字节数。 */
  uint32_t size;

  /** @brief 收到一条文本消息（二进制安全：可能含 NUL，按 `len` 走）。 */
  void (*on_text)(void* user_data, const char* data, size_t len);
  /** @brief 收到一条二进制消息。 */
  void (*on_binary)(void* user_data, const char* data, size_t len);
  /** @brief 连接关了。`code` 的数值与 `uvcpp_c_webapp.h` 里
   *         `enum uvcpp_c_ws_close_code` 逐条相同（1000 = 正常）。 */
  void (*on_close)(void* user_data, int code, const char* reason,
                   size_t reason_len);
  /** @brief 出错。`status` 是错误码，`what` 是一句话。 */
  void (*on_error)(void* user_data, int status, const char* what,
                   size_t what_len);
} uvcpp_c_ws_client_events;

/** @brief 建一个**自带事件循环**的 WS 客户端。 */
UVCPP_C_API uvcpp_c_ws_client* uvcpp_c_ws_client_new(void);

/** @brief 放掉它（连带它的循环与会话）。 */
UVCPP_C_API int uvcpp_c_ws_client_free(uvcpp_c_ws_client* client);

/**
 * @brief 装事件表。`table` 为 NULL = 全部摘掉。
 *
 * 可以在 `connect()` **之前**调（异步的常见写法：先把回调备好再连），也可以
 * 之后调；关掉再连一次照旧生效 —— 这与 C++ 侧 `uvcpp_ws_client` 的
 * "回调存在客户端上、每建一个会话装一次"是同一条。
 */
UVCPP_C_API int uvcpp_c_ws_client_set_events(uvcpp_c_ws_client* client,
                                             const uvcpp_c_ws_client_events* table,
                                             void* user_data);

/**
 * @brief 连一个 `ws://host:port/path`。
 *
 * @return `UVCPP_C_OK` 表示已经开始；握手结果在 `cb` 里（`status == 0` 才算连上）。
 *         非法的 URL（不是 `ws://`、没有 host）当场返回 `UVCPP_C_E_INVALID_ARG`。
 */
UVCPP_C_API int uvcpp_c_ws_client_connect(uvcpp_c_ws_client* client,
                                          const char* url,
                                          uvcpp_c_web_status_cb cb,
                                          void* user_data);

/**
 * @brief 发一条文本消息。
 * @return 0 = 已入队；`UV_ENOTCONN` = 还没有会话（**不静默丢**）。
 */
UVCPP_C_API int uvcpp_c_ws_client_send_text(uvcpp_c_ws_client* client,
                                            const char* data, size_t len);

/** @brief 发一条二进制消息。语义与上面那条相同。 */
UVCPP_C_API int uvcpp_c_ws_client_send_binary(uvcpp_c_ws_client* client,
                                              const char* data, size_t len);

/**
 * @brief 优雅关闭：发 Close 帧。可以重复调（只生效一次）。
 *
 * 真正的结束（对端回 Close / 连接断掉）稍后由 `on_close` 报出来。
 */
UVCPP_C_API int uvcpp_c_ws_client_close(uvcpp_c_ws_client* client, int code,
                                        const char* reason);

/** @brief 跑这条循环，直到 `stop()` 或所有会话都结束。 */
UVCPP_C_API int uvcpp_c_ws_client_run(uvcpp_c_ws_client* client);

/** @brief 让 `run()` 停下来。可以从别的线程调。 */
UVCPP_C_API int uvcpp_c_ws_client_stop(uvcpp_c_ws_client* client);

/** @brief 当前活动的会话数（0 或 1）；负数 = 错误码（句柄已失效）。 */
UVCPP_C_API int uvcpp_c_ws_client_session_count(uvcpp_c_ws_client* client);

/** @brief 握手完成了没有（1/0）；负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_ws_client_is_open(uvcpp_c_ws_client* client);

/** @brief 上次出错的错误码（0 = 没出过错）。 */
UVCPP_C_API int uvcpp_c_ws_client_last_error(uvcpp_c_ws_client* client);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* SRC_CAPI_UVCPP_C_WEB_H */
