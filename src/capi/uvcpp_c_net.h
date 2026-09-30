/**
 * @file src/capi/uvcpp_c_net.h
 * @brief net 层的 C 门面：`uvcpp_tcp_client` 与 `uvcpp_tcp_server` 的精选面。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它对应哪些 C++ 能力
 * -------------------
 *   | 这里 | C++ 那侧 |
 *   |------|----------|
 *   | `uvcpp_c_tcp_client_new()` | `uvcpp_tcp_client()`（自带一条循环）|
 *   | `uvcpp_c_tcp_client_connect()` | `uvcpp_tcp_client::connect(ip, port, cb)` |
 *   | `uvcpp_c_tcp_client_connect_wait()` | `uvcpp_tcp_client::connect_wait(ip, port, ms)` |
 *   | `uvcpp_c_tcp_client_write()` | `uvcpp_tcp_client::write(data, len, cb)` |
 *   | `uvcpp_c_tcp_client_set_events()` 的 `on_read` | `uvcpp_tcp_client::read_start_events(cb)` |
 *   | `uvcpp_c_tcp_client_set_events()` 的 `on_close` | `uvcpp_tcp_client::set_on_close(cb)` |
 *   | `uvcpp_c_tcp_server_*` | `uvcpp_tcp_server` 的同名方法 |
 *
 * 读事件的形状与 C++ 侧**逐字对应**：`uvcpp_c_read_result` 就是
 * `uvcpp_net_read.h` 里 `net_read_result` 的扁平版（`event` / `data` / `size` /
 * `error` / `fin`）。那三档事件的语义（`DATA` / `PEER_CLOSED` / `READ_ERROR`）
 * 与"`fin` 为什么存在"见那个头的说明 —— 这里一个字节都不改写。
 *
 * 回调的**生命周期**（规矩 4 的第二类，最容易踩的一条）
 * ----------------------------------------------------
 *   - `uvcpp_c_read_cb` 的 `result->data` **只在那次回调里有效**（底层缓冲区
 *     回调返回后就复用/释放）。要留就当场 memcpy 走。
 *   - `uvcpp_c_connect_cb` 给的 `client` 句柄**在这条连接的生命周期内一直有效**，
 *     但它在连接关闭时会被本层毒化 —— 之后再用它是 `UVCPP_C_E_STALE`，不是崩溃。
 *     所以"把句柄存进自己的结构体、连接断开后再拿它写"这条路是**明确不允许**的，
 *     允许的是"存着、每次用之前接住 `E_STALE`"。
 *   - **不要在回调里 free 任何句柄**：服务端交出来的连接由框架持有
 *     （`uvcpp_c_tcp_client_free()` 对它会返回 `UVCPP_C_E_STATE`，见下面）。
 *
 * 不提供（刻意）
 * --------------
 *   - `get_status()` 的那套位标志：位掩码一暴露就是第二份真相源，而调用方真正
 *     要问的两件事（"连上了吗"、"上次错是什么"）各有专门的函数。
 *   - `take_client()` / `return_client()`：把连接的所有权从服务端挪到调用方，
 *     在 C 里需要一条"谁 delete"的完整规矩。这一层不做 —— 拿到的连接句柄在
 *     断开时由本层回收。
 *   - `set_ssl_context()` / `enable_tls()`：TLS 需要 `uvcpp_ssl_context`，它的
 *     生命周期与 C 侧的 `uvcpp_c_*` 句柄是两套东西。等 web 那一批（批 2）把
 *     ssl 的 C 面定下来再一起给。**因此本批的 `is_tls()` 恒为 0、
 *     `alpn_selected()` 恒为空串** —— 不是占位，是实话：这一层没有任何一句
 *     能让连接装上 TLS，所以从这里拿到的每条连接都真的没装。
 *   - `uvcpp_buf` 家族（`read_start(cb)` / `write(uvcpp_buf*)` / `read_wait()`）：
 *     全是 C++ 类型，C 侧不出现。
 */

#pragma once
#ifndef SRC_CAPI_UVCPP_C_NET_H
#define SRC_CAPI_UVCPP_C_NET_H

#include <stddef.h>
#include <stdint.h>

#include "capi/uvcpp_c_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 不透明句柄。**定义不在这里**（在 `uvcpp_c_net.cpp` 里），头里永远是
 *        个不完整类型 —— 于是你没法在栈上造一个，也没法看见里面的指针。
 */
typedef struct uvcpp_c_tcp_client uvcpp_c_tcp_client;

/** @brief 服务端句柄。同上。 */
typedef struct uvcpp_c_tcp_server uvcpp_c_tcp_server;

/* ------------------------------------------------------------------------
 * 读事件
 * ------------------------------------------------------------------------ */

/** @brief 一次读事件的性质。值与 C++ 侧 `net_read_event` 一一对应（0/1/2）。 */
enum uvcpp_c_read_event {
  UVCPP_C_READ_DATA        = 0,  /**< 有数据，`data` / `size` 有效。 */
  UVCPP_C_READ_PEER_CLOSED = 1,  /**< 对端**正常**关闭（TCP FIN）。 */
  UVCPP_C_READ_ERROR       = 2   /**< 读出错，`error` 是 libuv 错误码（负值）。 */
};

/**
 * @brief 一次读事件的内容。与 C++ 的 `net_read_result` 同一套字段。
 *
 * `data` 只在 `UVCPP_C_READ_DATA` 时非 NULL，且**只在本次回调里有效**。
 * `fin` 对 TCP 恒为 0（TCP 一份数据与一次收尾是两次独立的读事件）—— 留着这一格
 * 是为了与 QUIC / HTTP-3 那几层**同一个结构体**，见 `uvcpp_net_read.h` 的说明。
 */
typedef struct uvcpp_c_read_result {
  int         event;  /**< @ref uvcpp_c_read_event */
  const char* data;   /**< DATA 时有效，其余为 NULL */
  size_t      size;   /**< DATA 时的字节数（二进制安全，可能含 NUL）*/
  int         error;  /**< READ_ERROR 时的 libuv 错误码；其余为 0 */
  int         fin;    /**< 非 0 表示对端在这块数据之后干净收尾（TCP 恒 0）*/
} uvcpp_c_read_result;

/**
 * @brief 读回调。
 *
 * @param user_data 注册时给的那个指针，原样传回。
 * @param client    事件所属的连接句柄。**只在本次回调里保证有效**。
 * @param result    事件内容；`result->data` 回调返回后失效。
 *
 * 在**循环线程**上调用，而且是在热路上：**不要在这里做重活**（拷贝、立刻返回）。
 */
typedef void (*uvcpp_c_read_cb)(void* user_data,
                                uvcpp_c_tcp_client* client,
                                const uvcpp_c_read_result* result);

/**
 * @brief 一次异步操作的完成：`connect` / `write`。
 *
 * @param status 0 成功；否则是 libuv 错误码（如 `UV_ECONNREFUSED`）。
 */
typedef void (*uvcpp_c_status_cb)(void* user_data, int status);

/** @brief 无参数通知（连接关闭）。 */
typedef void (*uvcpp_c_notify_cb)(void* user_data);

/** @brief 服务端接受了一条连接。 */
typedef void (*uvcpp_c_connect_cb)(void* user_data, uvcpp_c_tcp_client* client);

/* ------------------------------------------------------------------------
 * 事件表 —— 以 `size` 打头的版本化结构体
 * ------------------------------------------------------------------------
 * 用法只有一条：**第一格写 `sizeof(你用的那个结构体)`**，例如
 *
 *     uvcpp_c_tcp_client_events ev = {0};
 *     ev.size        = (uint32_t)sizeof(ev);
 *     ev.on_read     = my_read;
 *     ev.on_read_user_data = ctx;
 *     uvcpp_c_tcp_client_set_events(c, &ev);
 *
 * 本层只读 `size` 覆盖到的格子（规矩 3），所以：
 *   - 你**可以**只填前几格：用了 `off_t`-式的手写小结构体也行，但更简单的做法
 *     是像上面那样先 `{0}` 再逐格赋值、并把 `size` 写成这个**类型**的大小；
 *   - 以后这个结构体尾部加字段**不会**破坏你今天编出来的代码；
 *   - 某一格是 NULL = "这个回调我不关心"，那一次就不回调（不是错误）。
 *
 * `size` 连第一格都没盖住（`size < 4`）按 `UVCPP_C_E_INVALID_ARG` 拒绝：
 * 一张连自己多大都说不清的表，没有一格是可信的。
 */

/** @brief `uvcpp_c_tcp_client_set_events()` 的事件表。 */
typedef struct uvcpp_c_tcp_client_events {
  /** @brief **必须**是 `sizeof(uvcpp_c_tcp_client_events)` 或你那份结构体的字节数。 */
  uint32_t size;

  /** @brief 读事件（数据 / 对端关闭 / 读错误）。 */
  uvcpp_c_read_cb on_read;
  /** @brief 传给 `on_read` 的第一个参数。 */
  void* on_read_user_data;

  /** @brief 连接关闭的通知。 */
  uvcpp_c_notify_cb on_close;
  /** @brief 传给 `on_close` 的第一个参数。 */
  void* on_close_user_data;
} uvcpp_c_tcp_client_events;

/** @brief `uvcpp_c_tcp_server_set_events()` 的事件表。 */
typedef struct uvcpp_c_tcp_server_events {
  /** @brief **必须**是 `sizeof(uvcpp_c_tcp_server_events)` 或你那份结构体的字节数。 */
  uint32_t size;

  /** @brief 每接受一条连接回调一次。 */
  uvcpp_c_connect_cb on_connection;
  /** @brief 传给 `on_connection` 的第一个参数。 */
  void* on_connection_user_data;

  /** @brief 所有连接**共用**的一份读回调（与 C++ 侧 `set_read_callback` 同义）。 */
  uvcpp_c_read_cb on_read;
  /** @brief 传给 `on_read` 的第一个参数。 */
  void* on_read_user_data;
} uvcpp_c_tcp_server_events;

/* ------------------------------------------------------------------------
 * 客户端
 * ------------------------------------------------------------------------ */

/**
 * @brief 建一个**自带事件循环**的客户端。
 *
 * @return 句柄；失败返回 NULL（内存不足）。
 *
 * 自带循环的意思：`uvcpp_c_tcp_client_run()` 会一直跑到 `uvcpp_c_tcp_client_stop()`
 * 被调用；`uvcpp_c_tcp_client_free()` 会把循环一起收掉。这是 C 侧最常用的形状
 * （C# 里一个 `Task` 跑一条循环）。
 */
UVCPP_C_API uvcpp_c_tcp_client* uvcpp_c_tcp_client_new(void);

/**
 * @brief 释放客户端。
 *
 * 对**服务端交出来的**连接句柄会返回 `UVCPP_C_E_STATE` 而不释放（那条连接归
 * 框架，会在它断开时被本层回收）；第二次 `free` 同一个句柄返回
 * `UVCPP_C_E_STALE`。
 *
 * @warning 调用方**自己**保证此时没有别的线程正在用它。
 */
UVCPP_C_API int uvcpp_c_tcp_client_free(uvcpp_c_tcp_client* client);

/**
 * @brief 装事件表。`table` 为 NULL = 把这一组回调全部摘掉。
 *
 * 可以在 `connect` 之前或之后调；`on_read` 一旦装上就开始读（这是 C++ 侧
 * `read_start_events()` 的行为，本层照抄 —— 包括"不读的连接看不见对端断开"
 * 这条：`on_close` 也不响）。
 */
UVCPP_C_API int uvcpp_c_tcp_client_set_events(uvcpp_c_tcp_client* client,
                                              const uvcpp_c_tcp_client_events* table);

/**
 * @brief 异步连接。立刻返回，`cb` 在循环线程上被调用一次。
 *
 * @param ip   IPv4 / IPv6 地址串（本层**不做 DNS 解析**，与 C++ 侧一致）。
 * @param port 1..65535。
 * @param cb   完成回调。**必须给**（C 侧没有"回调给 NULL 就同步等"那条路 ——
 *             同步等要的是 `uvcpp_c_tcp_client_connect_wait()`）。
 * @return `UVCPP_C_OK` 表示**已经开始**连接；结果在 `cb` 里。
 */
UVCPP_C_API int uvcpp_c_tcp_client_connect(uvcpp_c_tcp_client* client,
                                           const char* ip, int port,
                                           uvcpp_c_status_cb cb, void* user_data);

/**
 * @brief 同步连接，最长等 `timeout_ms`。
 *
 * @return 0 成功；否则 libuv 错误码。**会阻塞当前线程**，只能在循环**还没跑**
 *         的时候用（在回调里调它会把循环卡死）。
 */
UVCPP_C_API int uvcpp_c_tcp_client_connect_wait(uvcpp_c_tcp_client* client,
                                                const char* ip, int port,
                                                int timeout_ms);

/**
 * @brief 异步写。`data` 的内容**立刻被拷走**（规矩 4 第一类），返回后你可以
 *        随便释放它。
 *
 * 一条连接上**同时只允许一笔异步写在途**：上一笔的完成回调还没跑时再发起返回
 * `UV_EALREADY`，而且这笔字节不会被发出、也不会有回调 —— 它是拒收，不是排队。
 * 要连着写就在完成回调里续投（那**是**合法的：回调是在标志清掉之后才调的）。
 * 这与 C++ 侧的规矩完全一致，见 `src/net/uvcpp_tcp_client.h` 的 `write()`。
 */
UVCPP_C_API int uvcpp_c_tcp_client_write(uvcpp_c_tcp_client* client,
                                         const void* data, size_t len,
                                         uvcpp_c_status_cb cb, void* user_data);

/** @brief 同步写，最长等 `timeout_ms`。同上：只能在循环还没跑的时候用。 */
UVCPP_C_API int uvcpp_c_tcp_client_write_wait(uvcpp_c_tcp_client* client,
                                              const void* data, size_t len,
                                              int timeout_ms);

/** @brief 暂停读（背压）。连接不动，只是不再投递读事件。 */
UVCPP_C_API int uvcpp_c_tcp_client_read_pause(uvcpp_c_tcp_client* client);

/** @brief 恢复读。 */
UVCPP_C_API int uvcpp_c_tcp_client_read_resume(uvcpp_c_tcp_client* client);

/**
 * @brief 停掉读。与"暂停"的区别是**背压之外的意义**：停掉之后对端断开也看不见。
 *        想知道连接死活就别停读（停读那条路上 `on_read` / `on_close` 都不会响）。
 */
UVCPP_C_API int uvcpp_c_tcp_client_read_stop(uvcpp_c_tcp_client* client);

/**
 * @brief 关掉这条连接。
 *
 * 关闭是**异步**的：这个调用返回时连接可能还没关完，`on_close` 稍后在循环线程
 * 上响。可以重复调（第二次及以后什么也不做）。
 */
UVCPP_C_API int uvcpp_c_tcp_client_close(uvcpp_c_tcp_client* client);

/**
 * @brief 跑这条循环，直到 `uvcpp_c_tcp_client_stop()` 或有别的路径让它停下。
 *
 * **必须在建这个客户端的那条线程上跑**（libuv 的循环不是线程安全的）。
 * 本层在这里记下"循环线程"，之后所有非 `post` 的调用都在这个线程上检查。
 */
UVCPP_C_API int uvcpp_c_tcp_client_run(uvcpp_c_tcp_client* client);

/** @brief 让 `run()` 停下来。可以从别的线程调（libuv 的 `uv_stop` 会唤醒循环）。 */
UVCPP_C_API int uvcpp_c_tcp_client_stop(uvcpp_c_tcp_client* client);

/** @brief 是不是已连上（1/0）。错误码见返回值小于 0 的情况。 */
UVCPP_C_API int uvcpp_c_tcp_client_is_connected(uvcpp_c_tcp_client* client);

/** @brief 上次出错的 libuv 错误码（0 = 没出过错）。 */
UVCPP_C_API int uvcpp_c_tcp_client_last_error(uvcpp_c_tcp_client* client);

/** @brief 这条连接走的是不是 TLS（1/0）。**本批恒为 0** —— 见文件开头的"不提供"。 */
UVCPP_C_API int uvcpp_c_tcp_client_is_tls(uvcpp_c_tcp_client* client);

/**
 * @brief TLS 协商出来的 ALPN 协议名，走"调用方给缓冲区"那套约定
 *        （见 `uvcpp_c_common.h`）。
 *
 * **本批恒为长度 0**（连 `cap = 1` 的缓冲区都只会拿到一个空串）—— 理由与
 * `uvcpp_c_tcp_client_is_tls()` 同一条，见文件开头的"不提供"。
 */
UVCPP_C_API int uvcpp_c_tcp_client_alpn_selected(uvcpp_c_tcp_client* client,
                                                 char* buf, size_t cap);

/* ------------------------------------------------------------------------
 * 服务端
 * ------------------------------------------------------------------------ */

/**
 * @brief 建一个服务端。它**自带事件循环**，形状与客户端那边一样。
 *
 * 典型顺序：`set_events` → `bind` → `listen` → `run`。
 */
UVCPP_C_API uvcpp_c_tcp_server* uvcpp_c_tcp_server_new(void);

/** @brief 释放服务端（会连带关掉循环与所有连接）。**必须先 `stop()` 过**。 */
UVCPP_C_API int uvcpp_c_tcp_server_free(uvcpp_c_tcp_server* server);

/** @brief 装事件表。`table` 为 NULL = 全部摘掉。可以在 `listen` 之前或之后调。 */
UVCPP_C_API int uvcpp_c_tcp_server_set_events(uvcpp_c_tcp_server* server,
                                              const uvcpp_c_tcp_server_events* table);

/**
 * @param ip   `"0.0.0.0"` 表示所有 IPv4 地址，`"::"` 表示所有 IPv6 地址。
 * @param port 0 表示让系统挑一个（挑中的那个用 `uvcpp_c_tcp_server_local_port()`
 *             问回来 —— 测试就是这么用的）。
 */
UVCPP_C_API int uvcpp_c_tcp_server_bind(uvcpp_c_tcp_server* server,
                                        const char* ip, int port);

/**
 * @brief 绑定后真正的端口（`bind` 时给 0 的情形）。
 * @return 端口号（> 0），或错误码。没绑上时返回 `UVCPP_C_E_STATE`。
 */
UVCPP_C_API int uvcpp_c_tcp_server_local_port(uvcpp_c_tcp_server* server);

/**
 * @brief 开始监听。`backlog` <= 0 时用 128（与 C++ 侧的默认值一致）。
 *
 * 连接回调来自事件表的 `on_connection`；读回调是**所有连接共用**的那一份
 * （与 C++ 侧 `set_read_callback` 同义）。
 */
UVCPP_C_API int uvcpp_c_tcp_server_listen(uvcpp_c_tcp_server* server, int backlog);

/**
 * @brief 设成 n 条循环（SO_REUSEPORT 扇出）。**只能在 `listen` 之前调**。
 *
 * @warning 设成 n > 1 之后，回调会在 n 条不同的线程上跑，而 C 侧那些
 *          `uvcpp_c_tcp_client*` 句柄的同一条纪律仍然成立：**一个句柄只在它
 *          自己那条线程上用**。跨连接共享的状态要靠调用方自己加锁。
 */
UVCPP_C_API int uvcpp_c_tcp_server_set_loops(uvcpp_c_tcp_server* server, int n);

/** @brief 当前设了几条循环（没设过就是 1）。 */
UVCPP_C_API int uvcpp_c_tcp_server_loop_count(uvcpp_c_tcp_server* server);

/**
 * @brief 跑循环，直到 `uvcpp_c_tcp_server_stop()`。
 * @note 多循环时它会一起跑（C++ 侧同形）。
 */
UVCPP_C_API int uvcpp_c_tcp_server_run(uvcpp_c_tcp_server* server);

/** @brief 停：关监听、关所有连接、让 `run()` 返回。 */
UVCPP_C_API int uvcpp_c_tcp_server_stop(uvcpp_c_tcp_server* server);

/** @brief 当前挂着的连接数（>= 0），或错误码。 */
UVCPP_C_API int uvcpp_c_tcp_server_client_count(uvcpp_c_tcp_server* server);

/** @brief 关掉所有连接，返回关掉的条数（>= 0），或错误码。 */
UVCPP_C_API int uvcpp_c_tcp_server_close_all_clients(uvcpp_c_tcp_server* server);

/** @brief 上次出错的 libuv 错误码（0 = 没出过错）。 */
UVCPP_C_API int uvcpp_c_tcp_server_last_error(uvcpp_c_tcp_server* server);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* SRC_CAPI_UVCPP_C_NET_H */
