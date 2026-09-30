/**
 * @file src/capi/uvcpp_c_http2.h
 * @brief http2 模块的 C 门面：`uvcpp_c_h2_connection` 一个句柄，外加两个
 *        **调用方所有**的构造器（请求 / 响应）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它对应哪些 C++ 能力
 * -------------------
 *   | 这里 | C++ 那侧 |
 *   |------|----------|
 *   | `uvcpp_c_h2_connection_new()` | `uvcpp_h2_connection(client, server_side)` |
 *   | `uvcpp_c_h2_conn_start()` | `uvcpp_h2_connection::start(h2_cbs, conn_cbs)` |
 *   | `uvcpp_c_h2_conn_send_status()` | `uvcpp_h2_connection::send_status()` |
 *   | `uvcpp_c_h2_conn_send_response()` | `uvcpp_h2_connection::send_response()` |
 *   | `uvcpp_c_h2_conn_send_headers()` | `uvcpp_h2_connection::send_headers()` |
 *   | `uvcpp_c_h2_conn_send_data()` | `uvcpp_h2_connection::send_data()` |
 *   | `uvcpp_c_h2_conn_submit_request()` | `session().submit_request()` |
 *   | `uvcpp_c_h2_conn_submit_rst()` | `session().submit_rst()` |
 *   | `uvcpp_c_h2_conn_submit_goaway()` | `session().submit_goaway()` |
 *   | `uvcpp_c_h2_conn_pause_stream()` / `_resume_stream()` | 同名的连接层方法 |
 *   | `uvcpp_c_h2_conn_flush()` / `_begin_goaway()` / `_shutdown()` / `_close_now()` | 同名的连接层方法 |
 *
 * **为什么只给一个句柄（`uvcpp_h2_session` 不单独出现）**
 * ------------------------------------------------------
 * C++ 侧是**两层**：`uvcpp_h2_session` 是协议层（吃字节、吐字节），
 * `uvcpp_h2_connection` 是驱动层（把它接在一条 `uvcpp_tcp_client` 上，替你
 * `recv()` / `drain()` / `flush()`）。照抄成两个 C 句柄有两条坏处，都不是洁癖：
 *
 *   1. **会话层的 `drain()` 是"吐出字节"，那意味着 C 侧要自己写 socket。**
 *      `uvcpp_c_net.h` 的 `tcp_client` 有它自己的一套回调与所有权模型，把它与
 *      "会话吐字节"拼起来是使用方自己写一遍驱动层 —— 而驱动层里那些
 *      "写完成之后再结算 `done`"的顺序恰好是最容易写错的一段。
 *   2. **两个句柄指向同一份内部状态时，"先 free 哪个"就成了第二份真相。**
 *      批 2 的 `uvcpp_c_web.h` 已经因为同一条理由不给 `uvcpp_ws_connection`。
 *
 * 所以 C 面只有**驱动层**这一个句柄，会话层那些操作挂在它名下
 * （`_submit_request` / `_submit_rst` / `_submit_goaway` / `_pause_stream` /
 * `_resume_stream`），文档行里写清"这一条转发给 `session()`"。
 * `session().take_completed()` **不在 C 面上**：驱动层自己就在写完成路径上跑它
 * （`uvcpp_h2_connection::run_completed()`），使用方没有插手的余地，也就没有
 * "忘了跑它"这条错路。
 *
 * 传输从哪来
 * ----------
 * `uvcpp_c_h2_connection_new(client, server_side)` 收的是一个既有的
 * `uvcpp_c_tcp_client*` —— 与 C++ 一样，**本层不拥有它**，连接什么时候被回收是
 * 交出去那一方（`uvcpp_tcp_server` 的 close manager，或者使用方自己）的责任。
 * 对端断开时 `on_disconnect` 会响，**回调返回后这个 `uvcpp_c_h2_connection`
 * 必须被 free 掉**（C++ 侧的原话是"持有者应当在这里把它销毁"）。
 *
 * 句柄的三种身份
 * --------------
 *   - `uvcpp_c_h2_connection`：**调用方建、调用方废**（`_new` / `_free`）。
 *   - `uvcpp_c_h2_request` / `uvcpp_c_h2_response`：**调用方建、调用方废**，
 *     是构造器不是视图 —— 建好之后填字段，交出去时**立刻拷贝**（规矩 4 第一类），
 *     交完之后你可以继续改它、也可以直接 free。
 *   - `uvcpp_c_h2_stream`：**回调期句柄**（与批 2 的 `uvcpp_c_req` 同一类）。
 *     只在递出它的那次回调里有效，回调一返回立刻反登记 + 毒化，再用是
 *     `UVCPP_C_E_STALE`，**不是崩溃**。要留 body 就当场拷走。
 *
 * 不提供（刻意）
 * --------------
 *   - `uvcpp_h2_session` 这个**独立句柄**（理由见上）。
 *   - `recv()` / `drain()` / `want_read()` / `want_write()`：传输层的事，
 *     驱动层自己做。C 侧要的是"接上就能跑"，不是"自己拼一根流水线"。
 *   - **优先级 / 依赖 / push**：`uvcpp_h2_session` 本来就没有这几项
 *     （`doc/http2-guide.md` 的"真实现"那张表写着），C 面不会凭空长出来。
 *   - `on_begin_headers` / `on_frame_recv` 那类**逐帧**回调：本层的回调表只给
 *     `doc/http2-guide.md` 里已经定成公开面的那七条。
 *   - `find_stream()` / `window_size()` / `set_max_out_stream_bytes()` 之外那些
 *     纯诊断的读数：`_stream_count()` / `_last_error()` / `_bytes_in()` /
 *     `_bytes_out()` 够用，其余走 C++ 面。
 */

#pragma once
#ifndef SRC_CAPI_UVCPP_C_HTTP2_H
#define SRC_CAPI_UVCPP_C_HTTP2_H

#include <stddef.h>
#include <stdint.h>

#include "capi/uvcpp_c_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 一条 h2 连接的驱动层句柄。**调用方建、调用方废**，定义在 `.cpp` 里。 */
typedef struct uvcpp_c_h2_connection uvcpp_c_h2_connection;

/**
 * @brief 本层记的一条 h2 流（**回调期句柄**）。
 *
 * 只在递出它的那次回调里有效，回调返回后立刻失效（再用是
 * `UVCPP_C_E_STALE`，不是崩溃）。
 */
typedef struct uvcpp_c_h2_stream uvcpp_c_h2_stream;

/** @brief 一个 h2 请求构造器（**调用方建、调用方废**）。 */
typedef struct uvcpp_c_h2_request uvcpp_c_h2_request;

/**
 * @brief 一个 h2 响应构造器（**调用方建、调用方废**）。
 *
 * 服务端回话用它；客户端也可以用它检查"对端发来的响应"—— 不过那是
 * `uvcpp_c_h2_stream_response_status()` 那条只读路，不需要构造器。
 */
typedef struct uvcpp_c_h2_response uvcpp_c_h2_response;

/** @brief `uvcpp_c_h2_stream_state()` 的取值，与 C++ 的 `h2_stream_state` 逐条对应。 */
enum uvcpp_c_h2_stream_state {
  UVCPP_C_H2_OPEN = 0,          /**< 请求头已收全，业务处理中（可能正在收 body）。 */
  UVCPP_C_H2_HEADERS_SENT = 1,  /**< 流式响应：头部已发，body 还要一块块补。 */
  UVCPP_C_H2_SENT = 2,          /**< 响应已一次提交完。 */
  UVCPP_C_H2_CLOSED = 3,        /**< 两端都已结束。**当前从不出现**（见 C++ 注）。 */
  UVCPP_C_H2_REJECTED = 4       /**< 协议层拒了它。**当前从不出现**（见 C++ 注）。 */
};

/* ========================================================================
 * 回调
 * ======================================================================== */

/**
 * @brief h2 协议层的回调表（**以 `uint32_t size` 打头**）。
 *
 * `size` 规则见 `uvcpp_c_common.h` 规矩 3：填 `sizeof(uvcpp_c_h2_callbacks)`，
 * 本层逐字段检查你这一格覆盖到了没有。**没覆盖的当"不关心"**，永远不会被调。
 * 所以将来往表尾加槽**不会**破坏今天编出来的客户端 —— 这是刻意的。
 *
 * 七条与 C++ 的 `uvcpp_h2_session::callbacks` 一一对应，语义逐字照抄
 * （`src/http2/uvcpp_h2_session.h` 里每一条的 `@brief` 才是权威，这里只写
 * C 侧要多知道的那一两句）。全部在**循环线程**上调用。
 *
 * @warning 回调里可以关连接（C++ 侧那一大段"重入"说明就是讲这个）。关完之后
 *          **不要再调用**任何 `uvcpp_c_h2_conn_*`：那时对端断开的路会走到
 *          `on_disconnect`，而按契约你应当在那里面 free 掉句柄。本层对已
 *          free 的句柄一律回 `UVCPP_C_E_STALE`，不会崩 —— 但业务逻辑上你已经
 *          没东西可发了。
 */
typedef struct uvcpp_c_h2_callbacks {
  uint32_t size;  /**< = `sizeof(uvcpp_c_h2_callbacks)`。**必须最先填。** */

  /** 收到请求头（服务端）或响应头（客户端）。`end_stream` 为真表示没有 body。 */
  void (*on_request)(void* user_data, uvcpp_c_h2_connection* c,
                     uvcpp_c_h2_stream* st, int end_stream);

  /** 请求收完（**没有它就分不出"头到了"和"请求全到了"**）。 */
  void (*on_request_end)(void* user_data, uvcpp_c_h2_connection* c,
                         uvcpp_c_h2_stream* st);

  /** 收到响应头（客户端侧）。`end_stream` 语义同上。 */
  void (*on_response)(void* user_data, uvcpp_c_h2_connection* c,
                      uvcpp_c_h2_stream* st, int end_stream);

  /** 响应收完（客户端侧）。与 `on_request_end` 对称。 */
  void (*on_response_end)(void* user_data, uvcpp_c_h2_connection* c,
                          uvcpp_c_h2_stream* st);

  /**
   * @brief 收到一段 body。服务端侧是请求体、客户端侧是响应体。
   *
   * @param data 指针**只在那次回调里有效**，要留就当场拷走（规矩 4 第二类）。
   */
  void (*on_body)(void* user_data, uvcpp_c_h2_connection* c,
                  uvcpp_c_h2_stream* st, const char* data, size_t len);

  /**
   * @brief 流结束（两端的 END_STREAM 都发完，或收到 RST_STREAM）。
   *
   * 回到这里之后那个 `uvcpp_c_h2_stream*`（如果这一轮还给过）立即失效 ——
   * 所以这里递的是 `stream_id` 而不是句柄。
   */
  void (*on_close)(void* user_data, uvcpp_c_h2_connection* c,
                   int32_t stream_id, uint32_t error_code);

  /**
   * @brief 连接级致命错误（当前实际只有两类触发点，见 C++ 侧注释）。
   *
   * **收到它就只有一个正确动作：发 GOAWAY（能发就发）然后关连接。**
   */
  void (*on_fatal)(void* user_data, uvcpp_c_h2_connection* c, int nghttp2_error);
} uvcpp_c_h2_callbacks;

/** @brief 连接层的回调表（**以 `uint32_t size` 打头**，规则同上）。 */
typedef struct uvcpp_c_h2_conn_callbacks {
  uint32_t size;  /**< = `sizeof(uvcpp_c_h2_conn_callbacks)`。**必须最先填。** */

  /**
   * @brief 底层连接结束了（对端关、读错、或我们关完了）。
   *
   * **回调返回后不要再用这个句柄**，按契约应当在这里
   * `uvcpp_c_h2_conn_free()`。
   */
  void (*on_disconnect)(void* user_data, uvcpp_c_h2_connection* c);
} uvcpp_c_h2_conn_callbacks;

/* ========================================================================
 * 生命周期
 * ======================================================================== */

/**
 * @brief 在一条已有的 TCP 连接上建一层 h2 驱动。
 *
 * @param client      既有的 `uvcpp_c_tcp_client`。**本层不拥有它**，也不会替你
 *                    关它；它必须比本句柄活得久。
 *                    C++ 侧要求这条连接"已经握手完并且协商出的 ALPN 是 h2"，
 *                    本层不替你检查这件事（`_start()` 会照常提交 SETTINGS，
 *                    对端不是 h2 的话表现是读回来的字节解析失败）。
 * @param server_side 非 0 = 建服务端会话（等对端提请求），0 = 客户端。
 *
 * @return 句柄；失败返回 NULL（参数不合法、句柄已失效、分配失败）。
 *         **失败的原因不在这里报**（这是一个"失败即 NULL"的构造器，与 C++
 *         那侧的构造函数同形）—— 句柄是空的，就没有 `_last_error()` 可问。
 */
UVCPP_C_API uvcpp_c_h2_connection* uvcpp_c_h2_connection_new(
    struct uvcpp_c_tcp_client* client, int server_side);

/**
 * @brief 释放句柄。
 *
 * **不会**替你对端断连、也不发 GOAWAY：要优雅收尾先
 * `uvcpp_c_h2_conn_shutdown()` —— 与 C++ 侧"先 `shutdown()` 再销毁"是同一条
 * （`_free()` 只做 C++ 对象的析构）。用户给的回调**一个都不会**被调。
 *
 * 已经 free 过、或者传空指针，都返回 `UVCPP_C_E_STALE` / `UVCPP_C_E_INVALID_ARG`。
 */
UVCPP_C_API int uvcpp_c_h2_conn_free(uvcpp_c_h2_connection* c);

/**
 * @brief 建会话、提交初始 SETTINGS、起读、把首轮字节发出去。
 *
 * 两个表都可以传 NULL（当"这一格都不关心"），但不能是**填错 `size` 的表** ——
 * `size < sizeof(uint32_t)` 一律 `UVCPP_C_E_INVALID_ARG`（理由见
 * `uvcpp_c_internal.h::table_size_ok`）。
 *
 * @param user_data 原样传给**两个表里所有**回调的第一个参数。整张表共用一个
 *                  而不是每格一个：批 2 的 `uvcpp_c_ws_client_set_events()` 定的
 *                  就是这个形状，而"一个句柄一份上下文"本来就是最常见的用法
 *                  （每格一个的话，七条回调要填七遍同一个指针）。
 *
 * 只能在**循环线程**上调用（规矩 5）。至少调一次之后回调才会来。
 */
UVCPP_C_API int uvcpp_c_h2_conn_start(uvcpp_c_h2_connection* c,
                                      const uvcpp_c_h2_callbacks* h2_cbs,
                                      const uvcpp_c_h2_conn_callbacks* conn_cbs,
                                      void* user_data);

/* ========================================================================
 * 服务端：回话
 * ======================================================================== */

/**
 * @brief 提交一个最小响应（只有 `:status` 与 `content-length`）并**立刻冲出去**。
 *
 * 是 `session().submit_status()` + `flush()` 的合并 —— 后者忘了调就是"响应提交
 * 了但一个字节都没发"这种最难查的静默失败（C++ 侧原话）。C 面只给合并版。
 *
 * @param body / body_len 可以为空（NULL / 0）。
 * @return 0 成功；负值见 `uvcpp_c_strerror()` 与 libuv 错误码。
 */
UVCPP_C_API int uvcpp_c_h2_conn_send_status(uvcpp_c_h2_connection* c,
                                            int32_t stream_id, int status,
                                            const char* body, size_t body_len);

/**
 * @brief 提交一个完整响应并**立刻冲出去**。
 *
 * @param resp      先用 `uvcpp_c_h2_response_new()` 建好、填好。交出去时
 *                  **立刻拷贝**，返回后你可以继续改它或者 free 它。
 * @param omit_body 非 0 = 只发头部，不发 body（HEAD 请求与 304 那一类）。
 *
 * 内层失败有两种是**可重试**的（C++ 侧写明"保证流状态没被改过"）：
 * `UV_EINVAL`（响应里有不能进 h2 的字段，比如连接专属头）与 `UV_EMSGSIZE`
 * （头部块超过上限）—— 换一份头再调一次是安全的。
 */
UVCPP_C_API int uvcpp_c_h2_conn_send_response(uvcpp_c_h2_connection* c,
                                              int32_t stream_id,
                                              const uvcpp_c_h2_response* resp,
                                              int omit_body);

/**
 * @brief 提交流式响应的**头部**（不结束流）+ 冲一次。
 *
 * 之后用 `uvcpp_c_h2_conn_send_data()` 一块块补 body，最后一块带 `end_stream`。
 * 顺序反了（先 `send_response` 再 `send_headers`）会拿到 `UV_EALREADY`。
 */
UVCPP_C_API int uvcpp_c_h2_conn_send_headers(uvcpp_c_h2_connection* c,
                                             int32_t stream_id,
                                             const uvcpp_c_h2_response* resp);

/**
 * @brief 提交一块流式 body + 冲一次。
 *
 * @param done 这块上线后回调**一次**，**绝不在本次调用里同步跑**（由写完成路径
 *             执行）。流被 RST 或连接断了也一定跑，参数是 `UV_ECANCELED`。
 *             可以传 NULL（等于"我不关心这一块的结果"）。
 * @return **只表示"受没受理"**（不是 `flush()` 的结果）：
 *           0 = 已受理，`done` 必跑一次；
 *           非 0 = 没受理，`done` **不会被调**，调用方自己收尾。
 *         `UV_ENOBUFS` 表示这条流的待发队列超了 `_set_max_out_stream_bytes()`
 *         那个上界（与前两种一样保证"什么都没发生"）。
 */
UVCPP_C_API int uvcpp_c_h2_conn_send_data(uvcpp_c_h2_connection* c,
                                          int32_t stream_id, const char* data,
                                          size_t len, int end_stream,
                                          void (*done)(void* user_data,
                                                       int status),
                                          void* done_user_data);

/* ========================================================================
 * 客户端：发请求
 * ======================================================================== */

/**
 * @brief 提交一个请求（转发给 `session().submit_request()`）。
 *
 * @param req / body  交出去时立刻拷贝，返回后随便处置。
 * @return **成功时是流号**（正数），失败时是负的错误码。所以这里不能拿
 *         ">= 0" 当成功 —— 流号是奇数起步的正数，而 0 不是合法流号。
 *
 * @warning **`req` 里必须带 `host` 头**，本层由它填 `:authority`。C++ 那侧
 *          （`uvcpp_http_client`）自己在缺 host 时会从 URL 抠一个补上；本层
 *          **没有那层兜底**，缺了就是空 `:authority`，对端一律判畸形请求。
 *          这是本层唯一一处"必须传对否则静默异常"的地方，所以单独写一条。
 *
 * 提交之后**顺手冲一次**（与 `_send_status()` / `_send_response()` 同一条：
 * "提交了但一个字节都没发"是本层最难查的一种静默失败，所以凡是有合并版的都给
 * 合并版）。冲失败时返回**冲的错误码**（负数）—— 那时流已经建了，只是字节没
 * 出去；C++ 侧的 `uvcpp_http_client` 也是这么返回的。
 */
UVCPP_C_API int32_t uvcpp_c_h2_conn_submit_request(uvcpp_c_h2_connection* c,
                                                   const uvcpp_c_h2_request* req,
                                                   const char* body,
                                                   size_t body_len);

/* ========================================================================
 * 流控制与收尾
 * ======================================================================== */

/**
 * @brief `session().pause_stream()` —— **不** flush。
 *
 * 暂停不产生任何字节，冲一次只会把无关的帧顺手发出去、让判据变浑（C++ 原话）。
 * 幂等；对不存在或已关闭的流返回 `UV_EINVAL`。
 */
UVCPP_C_API int uvcpp_c_h2_conn_pause_stream(uvcpp_c_h2_connection* c,
                                             int32_t stream_id);

/**
 * @brief `session().resume_stream()` + **flush**。
 *
 * **必须用这一个**，不要指望"暂停的反面"自己会出网：C++ 侧的
 * `session().resume_stream()` 只是把 WINDOW_UPDATE 排进 nghttp2 的出站队列，
 * 而应用侧最常见的调用时机（自己的定时器里）没有任何人会替你冲一次 ——
 * 表现是"对端永远等不到窗口、那条流挂死"，**而且不报任何错**。
 */
UVCPP_C_API int uvcpp_c_h2_conn_resume_stream(uvcpp_c_h2_connection* c,
                                              int32_t stream_id);

/**
 * @brief 给一条流发 RST_STREAM。
 *
 * 与 C++ 一样**不**自动 flush（`uvcpp_h2_connection` 里没有 `send_rst` 这个
 * 合并版，本层不凭空空造一个）；要发出去紧接着 `_flush()`。
 */
UVCPP_C_API int uvcpp_c_h2_conn_submit_rst(uvcpp_c_h2_connection* c,
                                           int32_t stream_id,
                                           uint32_t error_code);

/** @brief 把会话里待发的字节全部写出去。可以重复调（没东西发就是空操作）。 */
UVCPP_C_API int uvcpp_c_h2_conn_flush(uvcpp_c_h2_connection* c);

/** @brief 只发 GOAWAY、**不关连接**：不再接新流，已有的流照跑完。发过一次就不再发。 */
UVCPP_C_API int uvcpp_c_h2_conn_begin_goaway(uvcpp_c_h2_connection* c);

/**
 * @brief 直接提交一条 GOAWAY（`session().submit_goaway()`）。
 *
 * 与 `_begin_goaway()` 的分工：那个是"提前打招呼"（本层替你填 `last_stream_id`），
 * 这个是"你要自己指定错误码"（比如被控制帧令牌桶拦下来时发
 * `ENHANCE_YOUR_CALM`）。也不自动 flush。
 *
 * @param debug / debug_len GOAWAY 的调试正文，可空。
 */
UVCPP_C_API int uvcpp_c_h2_conn_submit_goaway(uvcpp_c_h2_connection* c,
                                              uint32_t error_code,
                                              const char* debug,
                                              size_t debug_len);

/**
 * @brief 主动关：尽量把待发字节（含 GOAWAY）冲出去，再关底层连接。
 *
 * 返回 `int` 而不是 `void`：这一层**每一个**导出函数都返回错误码（见
 * `uvcpp_c_common.h` 那句"0 或正数 = 成功，负数 = 失败"），为两个函数开一条
 * 不报错的例外，等于让调用方少一个"句柄其实已经失效"的信号。
 *
 * **真正的结束不是这里返回的时候** —— 待发字节冲完才关底层连接，那之后
 * `on_disconnect` 才来。返回 0 只表示"我受理了"。
 */
UVCPP_C_API int uvcpp_c_h2_conn_shutdown(uvcpp_c_h2_connection* c);

/** @brief 立刻关，不发任何东西。同样返回错误码，同样不是"已经关完"。 */
UVCPP_C_API int uvcpp_c_h2_conn_close_now(uvcpp_c_h2_connection* c);

/* ========================================================================
 * 只读
 * ======================================================================== */

/** @brief 底层连接已经结束（1/0）。 */
UVCPP_C_API int uvcpp_c_h2_conn_closed(const uvcpp_c_h2_connection* c);

/** @brief 正在关（等最后一笔写出去）（1/0）。 */
UVCPP_C_API int uvcpp_c_h2_conn_closing(const uvcpp_c_h2_connection* c);

/** @brief 当前活跃流数。 */
UVCPP_C_API size_t uvcpp_c_h2_conn_stream_count(const uvcpp_c_h2_connection* c);

/** @brief 最近一次致命错误的原始码（0 表示还没有）。 */
UVCPP_C_API int uvcpp_c_h2_conn_last_error(const uvcpp_c_h2_connection* c);

/** @brief 输入 / 输出字节数（供测试与诊断用）。 */
UVCPP_C_API size_t uvcpp_c_h2_conn_bytes_in(const uvcpp_c_h2_connection* c);
UVCPP_C_API size_t uvcpp_c_h2_conn_bytes_out(const uvcpp_c_h2_connection* c);

/** @brief 收到过对端的 GOAWAY 吗（1/0）。"没收到"与"收到的是 NO_ERROR"是两件事。 */
UVCPP_C_API int uvcpp_c_h2_conn_peer_goaway_received(
    const uvcpp_c_h2_connection* c);

/** @brief 对端 GOAWAY 里的错误码；没收到过时是 0（NO_ERROR）。 */
UVCPP_C_API uint32_t uvcpp_c_h2_conn_peer_goaway_error_code(
    const uvcpp_c_h2_connection* c);

/** @brief 对端 GOAWAY 里的 `last_stream_id`；没收到过时是 0。 */
UVCPP_C_API int32_t uvcpp_c_h2_conn_peer_goaway_last_stream_id(
    const uvcpp_c_h2_connection* c);

/* ========================================================================
 * 回调期句柄：一条流
 * ========================================================================
 * 全部返回**负数 = 错误码**、**非负 = 别的东西**（长度 / 状态码 / 计数）。
 * 拿这些函数去问一个**已经出了回调**的流句柄，一律 `UVCPP_C_E_STALE`。
 */

/** @brief 流号。 */
UVCPP_C_API int32_t uvcpp_c_h2_stream_id(const uvcpp_c_h2_stream* st);

/** @brief 当前状态，取值见 `enum uvcpp_c_h2_stream_state`。 */
UVCPP_C_API int uvcpp_c_h2_stream_state(const uvcpp_c_h2_stream* st);

/**
 * @brief 协议层拒过这条流吗（1/0）。
 *
 * 拒过的流业务层**不该再看到** —— 所以正常路径上它恒为 0，这个函数存在的
 * 意义是"如果哪天真看到了 1，能一眼认出来"。
 */
UVCPP_C_API int uvcpp_c_h2_stream_rejected(const uvcpp_c_h2_stream* st);

/** @brief 这条流的**收方向**被 `_pause_stream()` 暂停了吗（1/0）。 */
UVCPP_C_API int uvcpp_c_h2_stream_paused(const uvcpp_c_h2_stream* st);

/** @brief 已经收到的 body 字节数（按流记 —— 连接级一个标量在并发流下会记成溢出）。 */
UVCPP_C_API size_t uvcpp_c_h2_stream_body_bytes(const uvcpp_c_h2_stream* st);

/**
 * @brief 对端宣告的 `content-length`。**只在服务端那一侧有意义。**
 *
 * @param out 非空时写出那个数（仅在返回 0 时）。
 * @return 对端给了 `content-length` → 0；**没给** → `UVCPP_C_E_NOT_FOUND`。
 *         "没给"与"给了 0"是两件事：前者是流式体，后者是一条空 body，
 *         业务上要分得开（与批 2 的 `_header()` 同一个理由）。
 *
 * ★ **这一条是不对称的，而且这个不对称来自下一层**：`uvcpp_h2_stream` 的
 *   `has_content_length` / `expected_body` 这一对**只在解请求头时填**
 *   （`uvcpp_h2_session.cpp:408-413` 在 `handle_regular()` 里，那条路只有
 *   `server_side` 才走）。客户端侧解响应头走的是另一条分支，那里只把
 *   `content-length` 原样收进 `response.headers`。
 *
 *   所以：**服务端**（请求方向）这一句给出对端宣告的请求体长度；
 *   **客户端**在这一句上**永远**拿到 `E_NOT_FOUND` —— 那不是本层的缺口，是
 *   "这一层没有维护这个数"。客户端要知道响应体的长度，请读
 *   `uvcpp_c_h2_stream_response_header(st, "content-length", …)`：
 *  那是 `content-length` **这个头**本身，两边都收得到，而且它是什么就说什么
 *   （连"解出来是 0"也照实给），与"这个数由谁来维护"是两回事。
 *
 *   把这条写在这里而不是让它成为"用了才发现"的意外：一个永远返回
 *   `E_NOT_FOUND` 的接口如果不说明，读代码的人会以为客户端那条路上有个 bug。
 */
UVCPP_C_API int uvcpp_c_h2_stream_expected_body(const uvcpp_c_h2_stream* st,
                                                size_t* out);

/** @brief 请求的方法名（`"GET"` / `"POST"`……）。调用方给缓冲区，见 `uvcpp_c_common.h`。
 *         名字与批 2 的 `uvcpp_c_req_method_name()` 对齐。 */
UVCPP_C_API int uvcpp_c_h2_stream_request_method_name(const uvcpp_c_h2_stream* st,
                                                      char* buf, size_t cap);

/** @brief 请求的 URL（h2 侧是 `:path`）。调用方给缓冲区，规则同上。 */
UVCPP_C_API int uvcpp_c_h2_stream_request_path(const uvcpp_c_h2_stream* st,
                                               char* buf, size_t cap);

/** @brief 取一个请求头（名字大小写不敏感）。查不到返回 `UVCPP_C_E_NOT_FOUND`。 */
UVCPP_C_API int uvcpp_c_h2_stream_request_header(const uvcpp_c_h2_stream* st,
                                                 const char* name, char* buf,
                                                 size_t cap);

/** @brief 请求里有这个头吗（1/0）。空值也算"有"。 */
UVCPP_C_API int uvcpp_c_h2_stream_request_has_header(const uvcpp_c_h2_stream* st,
                                                     const char* name);

/**
 * @brief 对端发来的响应的状态码（**客户端侧**）。
 *
 * @return 状态码；**对端的响应头还没到**时返回
 *         `UVCPP_C_E_NOT_FOUND` —— 不是 `UVCPP_C_OK`，也不是默认的那个
 *         200。流在这之前被 RST 时，业务层拿到的必须是"没有状态码"。
 */
UVCPP_C_API int uvcpp_c_h2_stream_response_status(const uvcpp_c_h2_stream* st);

/** @brief 取响应头（客户端侧，名字大小写不敏感）。查不到返回 `UVCPP_C_E_NOT_FOUND`。 */
UVCPP_C_API int uvcpp_c_h2_stream_response_header(const uvcpp_c_h2_stream* st,
                                                  const char* name, char* buf,
                                                  size_t cap);

/* ========================================================================
 * 构造器：请求 / 响应
 * ========================================================================
 * 两者都是**调用方所有**的句柄：`_new()` 建、`_free()` 废，与别的句柄一样登记在
 * 活句柄表里（所以 `uvcpp_c_live_handle_count()` 的收支平衡把它们也算进去）。
 * 每个 `_set_*` 都是"覆盖"语义，不是"追加"。
 */

/** @brief 建一个请求构造器。失败返回 NULL。 */
UVCPP_C_API uvcpp_c_h2_request* uvcpp_c_h2_request_new(const char* method,
                                                       const char* path);

/** @brief 废掉它。再返回 `UVCPP_C_E_INVALID_ARG` / `_E_STALE`。 */
UVCPP_C_API int uvcpp_c_h2_request_free(uvcpp_c_h2_request* req);

/** @brief 设一个头（同名覆盖，大小写不敏感）。**`host` 必须设**（见 `_submit_request`）。 */
UVCPP_C_API int uvcpp_c_h2_request_set_header(uvcpp_c_h2_request* req,
                                              const char* name,
                                              const char* value);

/* **没有 `uvcpp_c_h2_request_set_body()`**：请求体是 `_submit_request()` 的一个
 * 参数（与 C++ 侧 `submit_request(req, std::string body)` 逐个对应）。给它开一个
 * "存在构造器里"的入口会是**第二份真相** —— 那时"交出去的到底是哪一份"就成了
 * 一个要读实现才知道的问题。响应那一侧相反：C++ 侧的 body 本来就在
 * `uvcpp_http_response` 里，所以 `_set_body()` 是对着来的。 */

/** @brief 建一个响应构造器。失败返回 NULL。 */
UVCPP_C_API uvcpp_c_h2_response* uvcpp_c_h2_response_new(int status);

/** @brief 废掉它。 */
UVCPP_C_API int uvcpp_c_h2_response_free(uvcpp_c_h2_response* resp);

/** @brief 改状态码。 */
UVCPP_C_API int uvcpp_c_h2_response_set_status(uvcpp_c_h2_response* resp,
                                               int status);

/** @brief 设一个头（同名覆盖）。h2 里不能出现的连接专属头会在提交时由内层拒掉。 */
UVCPP_C_API int uvcpp_c_h2_response_set_header(uvcpp_c_h2_response* resp,
                                               const char* name,
                                               const char* value);

/** @brief 设/覆盖 `content-type`（等价于设同名头，单独给一个是为了少打一遍字）。 */
UVCPP_C_API int uvcpp_c_h2_response_set_content_type(uvcpp_c_h2_response* resp,
                                                     const char* content_type);

/** @brief 设/覆盖响应体。 */
UVCPP_C_API int uvcpp_c_h2_response_set_body(uvcpp_c_h2_response* resp,
                                             const void* data, size_t len);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* SRC_CAPI_UVCPP_C_HTTP2_H */
