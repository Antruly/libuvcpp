/**
 * @file src/capi/uvcpp_c_http3.h
 * @brief http3 模块的 C 门面：`uvcpp_c_h3_connection` 一个句柄，外加两个
 *        **调用方所有**的构造器（请求 / 响应）与一枚**回调期**的请求视图。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它对应哪些 C++ 能力
 * -------------------
 *   | 这里 | C++ 那侧 |
 *   |------|----------|
 *   | `uvcpp_c_h3_connection_new()` | `uvcpp_h3_connection(quic_conn, server_side)` |
 *   | `uvcpp_c_h3_conn_start()` | `uvcpp_h3_connection::start(cbs)` |
 *   | `uvcpp_c_h3_conn_send_request()` | `uvcpp_h3_connection::send_request()` |
 *   | `uvcpp_c_h3_conn_take_completed()` | `uvcpp_h3_connection::take_completed()` |
 *   | `uvcpp_c_h3_conn_completed_count()` | `uvcpp_h3_connection::completed_count()` |
 *   | `uvcpp_c_h3_conn_send_response()` | `uvcpp_h3_connection::send_response()` |
 *   | `uvcpp_c_h3_conn_send_status()` | `uvcpp_h3_connection::send_status()` |
 *   | `uvcpp_c_h3_conn_flush()` / `_close()` | 同名方法 |
 *   | `uvcpp_c_h3_conn_ready()` / `_closed()` / `_alpn_selected()` / `_server_side()` | 同名方法 |
 *   | `uvcpp_c_h3_conn_control_stream_id()` / `_qpack_*_stream_id()` | 同名方法 |
 *   | `uvcpp_c_h3_conn_bytes_in()` / `_bytes_out()` | 同名方法 |
 *   | `uvcpp_c_h3_version()` | `uvcpp_h3_connection::nghttp3_version()` |
 *
 * **为什么只给一个句柄（`uvcpp_h3_session` 不出现）**
 * --------------------------------------------------
 * 与 `uvcpp_c_http2.h` 同一条理由，且更强：`uvcpp_h3_session` 是私有头里的类
 * （**根本没装出去**），它的 `drain()` 是"吐出字节"，那意味着 C 侧要自己把它接
 * 到 QUIC 的 `write_stream()` 上 —— 那是把驱动层重写一遍，而"一次 `write_stream()`
 * 对一次 `on_write`"那条账恰好是最容易写错的一段。所以 C 面只有驱动层这一个
 * 句柄。
 *
 * 传输从哪来
 * ----------
 * `uvcpp_c_h3_connection_new(conn, server_side)` 收的是一枚
 * **借来的** `uvcpp_c_quic_connection*`（见 `uvcpp_c_quic.h` 文件头那一段）——
 * 与 C++ 一样，**本层不拥有它**。两条规矩随之而来：
 *
 *   1. **服务端侧**要在 `listen` 的连接回调里建它（那时手上有连接句柄）；
 *      **客户端侧**要在 `_client_connect()` 返回之后、下一次泵循环之前建它
 *      （`uvcpp_quic_client.h` 那段 `@warning` 说的"唯一安全窗口"）。
 *   2. `on_disconnect` 里**必须** `uvcpp_c_h3_connection_free()` 掉它
 *      （C++ 的原话是"持有者应当在这里把它销毁"）。这一层替不了你：
 *      本对象不拥有那条 QUIC 连接，而 QUIC 连接一没，本对象就没有立足点了。
 *
 * 句柄的三种身份
 * --------------
 *   - `uvcpp_c_h3_connection`：**调用方建、调用方废**（`_new` / `_free`）。
 *   - `uvcpp_c_h3_request` / `uvcpp_c_h3_response`：**调用方建、调用方废**，
 *     是构造器不是视图 —— 填好交出去时**立刻拷贝**（规矩 4 第一类）。响应那一
 *     枚是两用的：服务端填它去 `_send_response()`，客户端拿它接
 *     `_take_completed()` 交回来的东西。
 *   - `uvcpp_c_h3_request_view`：**回调期句柄**（与批 2 的 `uvcpp_c_req`、
 *     批 3a 的 `uvcpp_c_h2_stream` 同一类）。只在 `on_request` 里有效，回调一
 *     返回立刻反登记 + 毒化，再用是 `UVCPP_C_E_STALE`，**不是崩溃**。要留 body
 *     或某个头就当场拷走。
 *
 * **客户端怎么知道响应到了**（这一条是本头最要紧的契约）
 * ------------------------------------------------------
 * C++ 侧 h3 **没有**"响应到了"这个回调：完成是**拉取**的
 * （`take_completed()` 一次一条、`completed_count()` 报积压）。C# 侧照着写会
 * 撞上一个问题 —— 它在回调之外没有任何时机去拉。所以这里把时机写死：
 *
 *   > **`on_stream_close` 响的时候，那条流的响应已经在完成队列里了。**
 *
 * 这不是"大概齐"：`uvcpp_h3_connection` 在把流关闭通知交给你之前，先保证这条
 * 流的响应进了队列（正常收全走 `on_end_stream`，中途被掐走那条补一次
 * `error != 0` 的）—— 实现见 `src/http3/uvcpp_h3_connection.cpp` 的
 * `sc.on_stream_close` 那一段。于是 C 侧的写法是：
 *
 *   ```c
 *   void on_stream_close(void* ud, uvcpp_c_h3_connection* h,
 *                        const uvcpp_c_h3_stream_close_info* info) {
 *     uvcpp_c_h3_response* r = uvcpp_c_h3_response_new(0);
 *     if (uvcpp_c_h3_conn_take_completed(h, r) >= 1) { …读状态码与 body… }
 *     uvcpp_c_h3_response_free(r);
 *   }
 *   ```
 *
 * **一条请求恰好在这里出现一次**（正常收全是一次、失败也是一次，不会两次也不会
 * 零次）—— 与 C++ 那条"恰好一次"逐字相同。别的时机也能拉：
 * `on_disconnect`（连接断掉时那些没收回来的请求会在队列里各出现一次），或者任何
 * 一次回调里（拉取语义：取不到就是还没到）。**不要在回调之外、在另一个线程上拉**
 * —— 规矩 5。
 *
 * 不提供（刻意）
 * --------------
 *   - **trailers**（尾随头）与 **server push**（`PUSH_PROMISE`）：C++ 侧这一批
 *     就没有（`doc/http3-guide.md` 那张"没做的"表逐条列着），C 面不会凭空长出来。
 *   - **优先级 / 依赖树**：h3 里已经删掉了那一套，没有可转发的东西。
 *   - **流式请求体 / 响应体**：C++ 侧 h3 在内存里攒满整个体（上限
 *     `H3_DEFAULT_MAX_BODY_BYTES`，64MB），本层如实跟着它，不假装有分块。
 *   - `uvcpp_h3_connection::quic()`：把底下那条 QUIC 连接交出去就是**第二份
 *     所有权模型** —— 而 h3 对象与 QUIC 连接的生命周期本来就绑在一起
 *     （`on_disconnect` 里必须销毁本对象）。要那条连接，就在建 h3 之前自己
 *     拿着（客户端）或从连接回调里拿（服务端）。
 *   - **web 客户端的 h3 模式**（`uvcpp_http_client::set_http3_enabled()`、
 *     `uvcpp_http_server::listen_quic()`）：那是 `uvcpp_c_web.h` 那一批的事，
 *     本轮**没做**。理由不是"忘了"，是它要往符号面锁里加一种**按配置条件成立
 *     的片**（那些符号只在 `WEB && HTTP3` 同时为真时存在，而现在的锁是"每个
 *     模块一片、按本模块的开关判"），那是一个机制改动，值得单独一批做。
 *     C++ 的 web 层那条路（连 QUIC 端口、`set_http3_enabled(true)`、看
 *     `negotiated_alpn() == "h3"`）本身是通的，只是没有 C 入口；要用就先用
 *     本头这套原生 QUIC + h3 的组合，它能拼出同一件事。
 */

#pragma once
#ifndef SRC_CAPI_UVCPP_C_HTTP3_H
#define SRC_CAPI_UVCPP_C_HTTP3_H

#include <stddef.h>
#include <stdint.h>

#include "capi/uvcpp_c_common.h"
#include "capi/uvcpp_c_quic.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief h3 驱动层句柄。**调用方建、调用方废**。 */
typedef struct uvcpp_c_h3_connection uvcpp_c_h3_connection;

/** @brief 请求构造器。**调用方建、调用方废**。 */
typedef struct uvcpp_c_h3_request uvcpp_c_h3_request;

/** @brief 响应构造器 / 接收容器。**调用方建、调用方废**。 */
typedef struct uvcpp_c_h3_response uvcpp_c_h3_response;

/** @brief `on_request` 递出来的**回调期**请求视图。不可保存，不可 free。 */
typedef struct uvcpp_c_h3_request_view uvcpp_c_h3_request_view;

/**
 * @brief 一条流关闭时，两个方向各自是怎么收场的（`h3_stream_close_info` 的
 *        平整版：`bool` 在这里是 `int`，取值 0/1）。
 *
 * 与 C++ 逐字段对应。**两个方向分开报是必须的**：一条请求的收场在 h3 里是两件
 * 独立的事（对端还发不发 vs 我还发不发），折成一个"流关了"，调用方就分不出
 * "请求体收全了"和"对端把请求取消了"。
 */
typedef struct uvcpp_c_h3_stream_close_info {
  uint32_t size; /**< = `sizeof(uvcpp_c_h3_stream_close_info)`。**必须最先填的是
                      `uvcpp_c_h3_callbacks.size`；这一栏是形状自检，不是回调表
                      那一条规矩。** */
  int64_t  stream_id;          /**< 哪条流。 */
  int      rx_error;           /**< 非 0 = 读方向被 `RESET_STREAM` / 掐断（不是干净 FIN）。 */
  int      tx_error;           /**< 非 0 = 写方向没能干净收场。 */
  uint64_t rx_app_error_code;  /**< `rx_error` 为真时才有意义。 */
  uint64_t tx_app_error_code;  /**< `tx_error` 为真时才有意义。 */
} uvcpp_c_h3_stream_close_info;

/**
 * @brief 回调表（**以 `uint32_t size` 打头**，规矩见 `uvcpp_c_common.h` 规矩 3）。
 *
 * 四格，与 `uvcpp_h3_connection::callbacks` 一一对应。全部在 **loop 线程**上
 * 调用，而且**很可能在某个内部调用还没返回的时候**跑起来。
 */
typedef struct uvcpp_c_h3_callbacks {
  uint32_t size; /**< = `sizeof(uvcpp_c_h3_callbacks)`。**必须最先填。** */

  /**
   * @brief 收到一条**完整的**请求（服务端侧才有意义）。客户端侧留空。
   *
   * @param req **回调期**视图：只在这次回调里有效。要留就当场拷走。
   *
   * 触发点是这条流**读方向的收场**，不是"头块收全"—— 于是拿到它的时候 body
   * 一定是全的，不需要再等一次。响应用 `_send_response()` / `_send_status()`
   * 在这条回调里发（也可以在别的时机发，只要流还活着）。
   */
  void (*on_request)(void* user_data, uvcpp_c_h3_connection* h,
                     const uvcpp_c_h3_request_view* req);

  /**
   * @brief 一条流关掉了（两个方向都收场之后）。
   *
   * **客户端侧：这条流的响应此刻已经在完成队列里了** —— 见文件头那一段。
   */
  void (*on_stream_close)(void* user_data, uvcpp_c_h3_connection* h,
                          const uvcpp_c_h3_stream_close_info* info);

  /**
   * @brief 连接级致命错误：本层已经没法在这条连接上继续了。
   *
   * @param error_code 按**符号**分两种意思：`> 0` 是要发给对端的 h3 应用错误码；
   *        `< 0` 是本端的失败码（nghttp3 / ngtcp2 / libuv）。
   *
   * 本层收到之后**会自己把 QUIC 连接关掉**（用那个应用错误码，那是唯一正确的
   * 动作），所以调用方通常只需要记日志。随后一定会收到一次 `on_disconnect`。
   */
  void (*on_error)(void* user_data, uvcpp_c_h3_connection* h, int error_code);

  /**
   * @brief 底层 QUIC 连接结束了。**恰好一次。**
   *
   * 回调里**必须** `uvcpp_c_h3_connection_free(h)`。返回之后不要再碰它。
   */
  void (*on_disconnect)(void* user_data, uvcpp_c_h3_connection* h);
} uvcpp_c_h3_callbacks;

/**
 * @brief 编进来的 nghttp3 是哪个版本（形如 `"1.18.0"`）。缓冲区约定见
 *        `uvcpp_c_common.h`。
 */
UVCPP_C_API int uvcpp_c_h3_version(char* buf, size_t cap);

/* ========================================================================
 * 连接
 * ======================================================================== */

/**
 * @brief 把 h3 接到一条 QUIC 连接上。
 *
 * @param conn        一枚**借来的** `uvcpp_c_quic_connection*`（见文件头）。
 * @param server_side 非 0 建服务端会话（等对端提请求），0 建客户端会话。
 * @return 句柄；失败返回 NULL（`conn` 为空、或它底下还没有连接）。
 *
 * 建出来只是"拿住了"，**还没有装回调、还没有建会话** —— 那是 `_start()`。
 */
UVCPP_C_API uvcpp_c_h3_connection* uvcpp_c_h3_connection_new(
    uvcpp_c_quic_connection* conn, int server_side);

/**
 * @brief 废掉它。**`on_disconnect` 里必须要做的事。**
 *
 * 已经废过 / 传空指针分别得到 `E_STALE` / `E_INVALID_ARG`。
 */
UVCPP_C_API int uvcpp_c_h3_connection_free(uvcpp_c_h3_connection* h);

/**
 * @brief 装回调、建会话、装 QUIC 回调，并把首轮待发字节发出去。
 *
 * 三条关键单向流**不在这里开** —— 它们要等 ALPN。所以 `_start()` 之后
 * `_conn_ready()` 还是 0，**这是正常的，不是失败**。
 *
 * @return 0 成功；`UV_EINVAL` 没给 QUIC 连接；负值是 nghttp3 的错误码。
 */
UVCPP_C_API int uvcpp_c_h3_conn_start(uvcpp_c_h3_connection* h,
                                      const uvcpp_c_h3_callbacks* cbs,
                                      void* user_data);

/**
 * @brief 客户端：提交一次请求（内部开一条双向流、立刻冲一次）。
 *
 * @param req 交出去时**立刻拷贝**，返回后随便处置。
 * @return 成功返回**流号**（拿它去和 `_take_completed()` 交回来的
 *         `_response_stream_id()` 对号）；负值是失败码 ——
 *         `UV_EAGAIN` 表示关键流还没开出来（`_conn_ready()` 还是 0，**这是正常
 *         会撞上的**，等 `_conn_ready()` 为真再发），`UV_ENOTCONN` 表示连接已经
 *         没了，`NGTCP2_ERR_STREAM_ID_BLOCKED` 是又一种正常结果（对端还没放开
 *         双向流额度）。
 */
UVCPP_C_API int64_t uvcpp_c_h3_conn_send_request(uvcpp_c_h3_connection* h,
                                                 const uvcpp_c_h3_request* req);

/**
 * @brief 客户端：取一条**已经收全**的响应（拉到 `out` 里）。
 *
 * @return `1` = 取到了一条（`out` 被填）；`0` = 队列是空的；
 *         负值 = 错误码（句柄无效 / `out` 为空）。
 *
 * **"一次一条"是刻意的**：队列里可能有几条同时到齐，一次抽干会让调用方在处理
 * 第一条时看不见第二条的到达。
 *
 * `out` 会先被重置（连状态码一起清掉），再把这一条填进去 —— 所以同一个
 * `uvcpp_c_h3_response` 可以循环用，不需要每次 `_new()`。
 */
UVCPP_C_API int uvcpp_c_h3_conn_take_completed(uvcpp_c_h3_connection* h,
                                               uvcpp_c_h3_response* out);

/**
 * @brief 队列里还积着几条。
 *
 * **注意它会被 `_take_completed()` 掏空**（那条会取走一条）：想判"取走之后没有
 * 第二条"，就要**先**读这个数、**再**调那条 —— 反过来的话这个判断恒为真。
 */
UVCPP_C_API int uvcpp_c_h3_conn_completed_count(const uvcpp_c_h3_connection* h);

/**
 * @brief 服务端：提交响应并立刻冲出去。
 *
 * @param resp      要发的那一条。**流号必须已经设好**（`_set_stream_id()`），
 *                  否则拿到 `UV_EINVAL` —— 那条流是 0 号流（真流）还是"没设"
 *                  是分得开的，见 `_set_stream_id()`。
 * @param omit_body 非 0 = 只发头块（HEAD / 204 / 304 走这一支）。
 * @return 0 成功；`UV_EMSGSIZE` 头块超了我们自己的上限（64KB），其余是 nghttp3
 *         的错误码。
 */
UVCPP_C_API int uvcpp_c_h3_conn_send_response(uvcpp_c_h3_connection* h,
                                              const uvcpp_c_h3_response* resp,
                                              int omit_body);

/** @brief 只发 `:status` 的最小响应 + 冲一次。`body` 可以为 NULL / 0。 */
UVCPP_C_API int uvcpp_c_h3_conn_send_status(uvcpp_c_h3_connection* h,
                                            int64_t stream_id, int status,
                                            const char* body, size_t body_len);

/**
 * @brief 把会话里待发的字节全部写出去。可以重复调（没东西发就是空操作）。
 *
 * **本层在每次收数据之后已经自己冲过一轮**，所以它不是"必须记得调"的那一类；
 * 给出来是为了与 C++ 的公开面逐个对上。
 */
UVCPP_C_API int uvcpp_c_h3_conn_flush(uvcpp_c_h3_connection* h);

/**
 * @brief 关掉整条 QUIC 连接（把 `error_code` 发给对端）。0 = 干净关闭。
 *
 * 之后 `_conn_closed()` 会变真、`on_disconnect` 会响一次 —— 所以循环还得再转
 * 几轮（要与 C++ 那条"优雅关闭"的语义一致，本层不能替你等）。
 */
UVCPP_C_API int uvcpp_c_h3_conn_close(uvcpp_c_h3_connection* h, int error_code);

/** @brief 三条关键单向流都开出来并绑好了 —— 现在可以发请求 / 答复了（1/0）。 */
UVCPP_C_API int uvcpp_c_h3_conn_ready(const uvcpp_c_h3_connection* h);

/** @brief 底层连接已经结束（`on_disconnect` 之前就为真）（1/0）。 */
UVCPP_C_API int uvcpp_c_h3_conn_closed(const uvcpp_c_h3_connection* h);

/** @brief 本端是服务端侧（1/0）。 */
UVCPP_C_API int uvcpp_c_h3_conn_server_side(const uvcpp_c_h3_connection* h);

/** @brief 协商出来的 ALPN（握手完成前长度为 0；握手完成但没有协议名也是 0）。 */
UVCPP_C_API int uvcpp_c_h3_conn_alpn_selected(const uvcpp_c_h3_connection* h,
                                              char* buf, size_t cap);

/** @brief 三条关键流各自的流号；还没开出来时返回 -1。 */
UVCPP_C_API int64_t uvcpp_c_h3_conn_control_stream_id(
    const uvcpp_c_h3_connection* h);
UVCPP_C_API int64_t uvcpp_c_h3_conn_qpack_encoder_stream_id(
    const uvcpp_c_h3_connection* h);
UVCPP_C_API int64_t uvcpp_c_h3_conn_qpack_decoder_stream_id(
    const uvcpp_c_h3_connection* h);

/** @brief 从 QUIC 收进来的字节数 / 写进 QUIC 的字节数，供测试与诊断用。 */
UVCPP_C_API uint64_t uvcpp_c_h3_conn_bytes_in(const uvcpp_c_h3_connection* h);
UVCPP_C_API uint64_t uvcpp_c_h3_conn_bytes_out(const uvcpp_c_h3_connection* h);

/* ========================================================================
 * 构造器：请求 / 响应
 * ========================================================================
 * 两者都是**调用方所有**的句柄：`_new()` 建、`_free()` 废，与别的句柄一样登记在
 * 活句柄表里（所以 `uvcpp_c_live_handle_count()` 的收支平衡把它们也算进去）。
 * 每个 `_set_*` 都是"覆盖"语义，不是"追加"。
 */

/**
 * @brief 建一个请求构造器。
 * @param method 例如 `"GET"`。**不能为空。**
 * @param path   例如 `"/"`、`"/api/x?a=1"`。**不能为空。**
 *
 * 两个参数任一为空 → 返回 **NULL**（与 `uvcpp_c_h2_request_new()` 逐字同一条：
 * 失败原因不在这里报）。也不替谁填默认值 —— C++ 的 `h3_request` 这两栏默认是
 * 空串，本层照抄，**不发明第二套默认**（一个悄悄变成 `GET /` 的构造器，比一次
 * 明确的 NULL 难查得多）。
 *
 * `scheme` / `authority` 不在这里定：那两栏的默认也是空串，由发送路径按 h3 的
 * 规矩处理（`scheme` 空则填 `"https"`，`authority` 空则不发那个伪头）。
 */
UVCPP_C_API uvcpp_c_h3_request* uvcpp_c_h3_request_new(const char* method,
                                                       const char* path);

/** @brief 废掉它。已经废过 / 传空指针分别得到 `E_STALE` / `E_INVALID_ARG`。 */
UVCPP_C_API int uvcpp_c_h3_request_free(uvcpp_c_h3_request* req);

/** @brief 设 `:scheme`。一般不填（默认 `"https"`）。 */
UVCPP_C_API int uvcpp_c_h3_request_set_scheme(uvcpp_c_h3_request* req,
                                              const char* scheme);

/**
 * @brief 设 `:authority`（一般就是 `host:port`）。**为空 = 不发这个伪头**
 *        （RFC 9114 允许省），发布出去的场景请填上。
 */
UVCPP_C_API int uvcpp_c_h3_request_set_authority(uvcpp_c_h3_request* req,
                                                 const char* authority);

/**
 * @brief 设一个普通头（同名覆盖，大小写不敏感）。
 *
 * **不要往这里塞伪头**（`":method"` 这类）：h3 要求伪头自成一段，而这条列表里
 * 带 `:` 开头的项会被原样发出去。
 */
UVCPP_C_API int uvcpp_c_h3_request_set_header(uvcpp_c_h3_request* req,
                                              const char* name,
                                              const char* value);

/** @brief 设/覆盖请求体（可以是二进制，`len` 说了算）。NULL / 0 = 空 body。 */
UVCPP_C_API int uvcpp_c_h3_request_set_body(uvcpp_c_h3_request* req,
                                            const void* data, size_t len);

/**
 * @brief 建一个响应构造器。
 * @param status 初始状态码；**0 表示"还没定"**（客户端侧接
 *        `_take_completed()` 交回来的东西时就是从这个值开始）。
 *
 * 流号**没有初值**（内部是 `-1`，不是 0：QUIC 里 0 是一条**真的**流）——
 * 服务端那条路要先 `_set_stream_id()` 再 `_send_response()`，见那两条。
 */
UVCPP_C_API uvcpp_c_h3_response* uvcpp_c_h3_response_new(int status);

/** @brief 废掉它。 */
UVCPP_C_API int uvcpp_c_h3_response_free(uvcpp_c_h3_response* resp);

/** @brief 改状态码。 */
UVCPP_C_API int uvcpp_c_h3_response_set_status(uvcpp_c_h3_response* resp,
                                               int status);

/**
 * @brief 指定这一条响应答复的是哪条流。**服务端 `_send_response()` 之前必须调。**
 *
 * @param stream_id `on_request` 里那条视图的 `_stream_id()`。要**非负**：h3 的
 *        `send_response()` 拒收 `stream_id < 0`，因为那是"这条响应还没归属"
 *        的日子值 —— 一条没有流号的响应发出去只能靠猜，猜错就是把 A 的答复发到
 *        B 的流上。拿不到流号却去发，会拿到 `UV_EINVAL`，不是发到 0 号流上。
 *
 * **为什么不把流号做成 `_send_response()` 的一个参数**：与 C++ 逐字对齐 ——
 * `h3_response` 那个结构体本来就带 `stream_id`（客户端从 `_take_completed()`
 * 拿到的那一条也要靠它说"答复的是哪条流"，见 `_response_stream_id()`）。这一枚
 * 句柄是**同一形状两用**的，加一个参数就等于承认它有第二种形状。
 */
UVCPP_C_API int uvcpp_c_h3_response_set_stream_id(uvcpp_c_h3_response* resp,
                                                  int64_t stream_id);

/** @brief 设一个普通头（同名覆盖）。 */
UVCPP_C_API int uvcpp_c_h3_response_set_header(uvcpp_c_h3_response* resp,
                                               const char* name,
                                               const char* value);

/** @brief 设/覆盖 `content-type`（等价于设同名头，单独给一个是为了少打一遍字）。 */
UVCPP_C_API int uvcpp_c_h3_response_set_content_type(uvcpp_c_h3_response* resp,
                                                     const char* content_type);

/** @brief 设/覆盖响应体。 */
UVCPP_C_API int uvcpp_c_h3_response_set_body(uvcpp_c_h3_response* resp,
                                             const void* data, size_t len);

/**
 * @brief 清空（状态码、头列表、body 全清），可以接着重用。
 *
 * `_take_completed()` 内部已经先做这一步，所以这一个是给"服务端要发好几条
 * 响应、想复用一个构造器"那种写法用的。
 */
UVCPP_C_API int uvcpp_c_h3_response_reset(uvcpp_c_h3_response* resp);

/* ---- 读（客户端侧从 `_take_completed()` 拿到的那一条）---- */

/**
 * @brief 状态码。
 *
 * @return 状态码；**没拿到**时返回 `UVCPP_C_E_NOT_FOUND` —— 不是 `UVCPP_C_OK`，
 *         也不是默认的那个 200。请求在收到响应头之前就被掐断时（`_error()`
 *         非 0），业务层拿到的必须是"没有状态码"。与批 3a 的
 *         `uvcpp_c_h2_stream_response_status()` 同一条。
 */
UVCPP_C_API int uvcpp_c_h3_response_status(const uvcpp_c_h3_response* resp);

/** @brief 这条响应挂在哪条流上（`_send_request()` 交回来的那个号）。 */
UVCPP_C_API int64_t uvcpp_c_h3_response_stream_id(
    const uvcpp_c_h3_response* resp);

/**
 * @brief 这次请求的收场：0 = 正常；非 0 = 没能干净收场
 *        （`UV_ECANCELED`：对端 reset / 被 STOP_SENDING 掐掉 / 连接在半路没了）。
 *
 * **有它才不会挂死调用方** —— 一条失败的请求也要在 `_take_completed()` 里出现
 * 一次，否则"取不到就是还没回来"与"取不到就是永远回不来了"这两件事分不开。
 */
UVCPP_C_API int uvcpp_c_h3_response_error(const uvcpp_c_h3_response* resp);

/** @brief body 的字节数（二进制安全，`len` 说了算）。 */
UVCPP_C_API size_t uvcpp_c_h3_response_body_size(
    const uvcpp_c_h3_response* resp);

/**
 * @brief 拷出 body。调用方给缓冲区，见 `uvcpp_c_common.h`（返回**真实长度**，
 *        只有 `cap` 够大时才写）。
 */
UVCPP_C_API int uvcpp_c_h3_response_body(const uvcpp_c_h3_response* resp,
                                         void* buf, size_t cap);

/** @brief 取一个响应头（名字大小写不敏感）。查不到返回 `UVCPP_C_E_NOT_FOUND`。 */
UVCPP_C_API int uvcpp_c_h3_response_header(const uvcpp_c_h3_response* resp,
                                           const char* name, char* buf,
                                           size_t cap);

/**
 * @brief 有没有这个头（1/0）。**空值也算"有"** ——
 *        "没给"与"给了个空串"是两件事（与批 2 的 `_header()` 同一个理由）。
 */
UVCPP_C_API int uvcpp_c_h3_response_has_header(const uvcpp_c_h3_response* resp,
                                              const char* name);

/* ========================================================================
 * 请求视图（**回调期**，只在 `on_request` 里有效）
 * ========================================================================
 * 一列只读访问器。伪头是**具名**的那几个（与 C++ 侧把 `method` / `scheme` /
 * `authority` / `path` 拆成四个成员一致）：没人愿意在一条头列表里做字符串比较。
 * 全部按"调用方给缓冲区"那条约定交回字符串；body 那条是原始字节。
 *
 * **回调一返回，这些函数对那个视图一律返回 `UVCPP_C_E_STALE`。**
 */

/** @brief 请求的方法名（`"GET"` / `"POST"`……）。 */
UVCPP_C_API int uvcpp_c_h3_request_view_method(
    const uvcpp_c_h3_request_view* req_view, char* buf, size_t cap);

/** @brief `:scheme`（对端没发就是空串）。 */
UVCPP_C_API int uvcpp_c_h3_request_view_scheme(
    const uvcpp_c_h3_request_view* req_view, char* buf, size_t cap);

/** @brief `:authority`（对端没发就是空串）。 */
UVCPP_C_API int uvcpp_c_h3_request_view_authority(
    const uvcpp_c_h3_request_view* req_view, char* buf, size_t cap);

/** @brief `:path`。 */
UVCPP_C_API int uvcpp_c_h3_request_view_path(
    const uvcpp_c_h3_request_view* req_view, char* buf, size_t cap);

/** @brief 这条请求在哪条流上（答复时要用它）。 */
UVCPP_C_API int64_t uvcpp_c_h3_request_view_stream_id(
    const uvcpp_c_h3_request_view* req_view);

/** @brief 请求体的字节数。 */
UVCPP_C_API size_t uvcpp_c_h3_request_view_body_size(
    const uvcpp_c_h3_request_view* req_view);

/** @brief 拷出请求体。规则同 `uvcpp_c_h3_response_body()`。 */
UVCPP_C_API int uvcpp_c_h3_request_view_body(
    const uvcpp_c_h3_request_view* req_view, void* buf, size_t cap);

/** @brief 取一个普通头（名字大小写不敏感）。查不到返回 `UVCPP_C_E_NOT_FOUND`。 */
UVCPP_C_API int uvcpp_c_h3_request_view_header(
    const uvcpp_c_h3_request_view* req_view, const char* name, char* buf,
    size_t cap);

/** @brief 有没有这个普通头（1/0）。空值也算"有"。 */
UVCPP_C_API int uvcpp_c_h3_request_view_has_header(
    const uvcpp_c_h3_request_view* req_view, const char* name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SRC_CAPI_UVCPP_C_HTTP3_H */
