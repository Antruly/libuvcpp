/**
 * @file src/capi/uvcpp_c_quic.h
 * @brief QUIC 传输层的 C 门面：客户端、服务端、连接，外加一枚 TLS 配置句柄。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它对应哪些 C++ 能力
 * -------------------
 *   | 这里 | C++ 那侧 |
 *   |------|----------|
 *   | `uvcpp_c_quic_client_*` | `uvcpp_quic_client` 的同名方法 |
 *   | `uvcpp_c_quic_server_*` | `uvcpp_quic_server` 的同名方法 |
 *   | `uvcpp_c_quic_conn_*` | `uvcpp_quic_connection` 的同名方法 |
 *   | `uvcpp_c_quic_tls_*` | `uvcpp_ssl_context`（**只开 QUIC 够用的那几栏**）|
 *   | `uvcpp_c_quic_crypto_init()` / `_free()` | `quic_crypto_backend_init()` / `_free()` |
 *
 * 句柄的第四种身份：**借来的**
 * ---------------------------
 * 前三批只有两种句柄：`_new()` 建 / `_free()` 废的（调用方所有），以及只在一次
 * 回调里有效的（回调期句柄）。QUIC 这一层多出第三种，而它是被 C++ 那侧的所有权
 * 逼出来的：
 *
 *   - `uvcpp_quic_client::connection()` 返回的指针**属于客户端**，
 *   - `uvcpp_quic_server::listen(cb)` 交出来的指针**属于服务端**
 *     （原话："不转让所有权，在它的 `on_close` 之后即失效"）。
 *
 * 也就是说 `uvcpp_quic_connection` **没有哪一刻是调用方所有的**。照抄成
 * "`_new()` / `_free()`"就是凭空造一个所有权：C# 侧在回调里 free 一下，底下的
 * C++ 对象还在，服务端那边下一次读就踩在已释放内存上。
 *
 * 所以 `uvcpp_c_quic_connection` 是**借来的句柄**：
 *
 *   1. **本层不提供 `_free()`**（这个类型上一个 free 函数都没有）。你不可能
 *      走错那一步，因为它不存在 —— 比"文档里写着别 free"硬。
 *   2. 它由端点（客户端那一枚在 `_client_new()` 时建好，服务端每一枚在
 *      `on_connection` 里建）持有，**底层连接一没就立刻反登记 + 毒化**。
 *   3. 毒化之后再用它，一律 `UVCPP_C_E_STALE` —— 不是崩溃。所以"把句柄存进
 *      自己的结构体、连接断了之后再拿它写"这条路**不会炸**，只会拿到一个
 *      明确的错误码。
 *   4. 它对 `uvcpp_c_live_handle_count()` 是**计入**的：端点释放时还没摘干净的
 *      借出句柄会让收尾那条收支平衡断言红掉（`capi_mutation.py` 的 M18 就是
 *      这一条）。
 *
 * **第 2 条"底层连接一没就毒化"有两个入口**，而这件事得写下来，因为它跨文件：
 * 裸 QUIC 时由本模块自己那套回调里的 `on_close` 收尾；一旦装上 h3
 * （`uvcpp_c_http3.h`），h3 的 `start()` 会把连接上那张回调表**整个换成它自己
 * 的**，于是这里那套跳板一次都不响，改由 h3 那侧在它的 `on_disconnect` 里叫一声。
 * 两边都收不到的症状是同一个：借出的句柄永远留在表里，收支平衡红。
 * 完整的理由写在 `src/capi/uvcpp_c_quic.cpp` 的文件头第 2 条。
 *
 * 客户端那一枚还有一个细节：它在 `_client_new()` 时就存在（那时还没连上），
 * 于是"还没连上就问它要流号"必须有一个明确的答复 —— 是 `UVCPP_C_E_STATE`，
 * 不是 `E_STALE`（句柄是活的，只是底下还没有连接）。
 *
 * TLS：为什么这一批必须给
 * -----------------------
 * QUIC 的版本协商走 TLS 的 ALPN 扩展，所以**没有明文 QUIC** —— 服务端不装证书
 * 时 `listen()` 直接失败（C++ 侧原话："不设就是没配"）。批 1/2 的 C 面一句 TLS
 * 设置都没有（那时唯一能装 TLS 的是 `uvcpp_tcp_client`，而它连 TLS 的 C 入口
 * 都刻意没给），到这一批"不给就没法用"了。
 *
 * 给的是**最小的那一套**：一枚 `uvcpp_c_quic_tls` 句柄 + 三个建法 + 三个改法。
 * `uvcpp_ssl_context` 上其余那些（密码套件、协议版本区间、SNI 回调、会话票据）
 * **不给** —— 这一层要的是"一条回环连接能建起来"，不是"把 OpenSSL 的旋钮搬一遍"。
 * 客户端**默认不校验证书**（C++ 的 `tls_mode::CLIENT` 默认是 `PEER`，本层建
 * 客户端上下文时显式改成 `NONE`），要校验就 `_set_ca_file()` + `_set_verify(1)`。
 * 理由：P/Invoke 那一侧的常见用法是"连自家内网、证书是自签的"，而默认校验会让
 * 第一次跑起来就是一条 `E_...` 握手失败，看不出是证书问题。
 *
 * 不提供（刻意）
 * --------------
 *   - **0-RTT / 连接迁移 / datagram / multipath**：C++ 侧本来就没有
 *     （`doc/quic-guide.md` §8 那张"没做的"表逐条列着），C 面不会凭空长出来。
 *   - `uvcpp_loop*` 的那两个构造函数（`uvcpp_quic_client(uvcpp_loop*)`）：本层
 *     至今没有 `uvcpp_c_loop` 这个句柄类型，为了两个构造函数造一个，等于把
 *     "循环归谁"这件事摆到 C 面上 —— 而端点本来就各带一条循环。要多循环就用
 *     端点自己那条。
 *   - `uvcpp_quic_connection::endpoint`（`attach()` / `session()` / `hooks`）：
 *     参数与返回值都是**私有内核类型**（`quic_detail::quic_session`），C 侧看不
 *     见也不该看见。
 *   - `on_alpn` 之外的 `std::string` 出口（`alpn_selected()`）走"调用方给缓冲区"
 *     那条约定，不返回堆上的 `char*`。
 */

#pragma once
#ifndef SRC_CAPI_UVCPP_C_QUIC_H
#define SRC_CAPI_UVCPP_C_QUIC_H

#include <stddef.h>
#include <stdint.h>

#include "capi/uvcpp_c_common.h"

/* `uvcpp_c_read_result` / `uvcpp_c_read_event` 就是这一层要用的读事件形状 ——
 * QUIC 的流在应用层看到的与一条 TCP 连接**逐字相同**（C++ 侧 `uvcpp_quic_common.h`
 * 那段"复用是有意的"讲的就是这件事），所以这里 include net 那份头**复用同一个
 * 结构体**，而不是另造一个长得一样的。`UVCPP_ENABLE_QUIC=ON` 本来就要求
 * `UVCPP_BUILD_NET=ON`，这份 include 在任何能编出本头的配置里都成立。 */
#include "capi/uvcpp_c_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 客户端句柄。**调用方建、调用方废**。 */
typedef struct uvcpp_c_quic_client uvcpp_c_quic_client;

/** @brief 服务端句柄。**调用方建、调用方废**。 */
typedef struct uvcpp_c_quic_server uvcpp_c_quic_server;

/**
 * @brief TLS 配置句柄。**调用方建、调用方废**，端点**不拥有**它
 *        （它的生命周期必须盖住用到它的每一条连接）。
 */
typedef struct uvcpp_c_quic_tls uvcpp_c_quic_tls;

/**
 * @brief 一条 QUIC 连接的句柄。**借来的** —— 见文件头那一段。
 *
 * 没有配套的 `_free()`，这是刻意的。
 */
typedef struct uvcpp_c_quic_connection uvcpp_c_quic_connection;

/** @brief `uvcpp_c_quic_conn_state()` 的取值，与 C++ 的 `quic_connection_state` 逐条对应。 */
enum uvcpp_c_quic_state {
  UVCPP_C_QUIC_IDLE = 0,        /**< 还没有对端。 */
  UVCPP_C_QUIC_HANDSHAKING = 1, /**< 握手进行中。 */
  UVCPP_C_QUIC_ESTABLISHED = 2, /**< 握手完成，可以收发应用数据。 */
  UVCPP_C_QUIC_CLOSING = 3,     /**< 已发 CONNECTION_CLOSE，等对端确认。 */
  UVCPP_C_QUIC_DRAINING = 4,    /**< 收到对端的 CONNECTION_CLOSE，等计时器到期。 */
  UVCPP_C_QUIC_CLOSED = 5       /**< 已终结，对象可以销毁。 */
};

/* ========================================================================
 * 客户端、服务端共用的两件
 * ======================================================================== */

/**
 * @brief 初始化 ngtcp2 的 OpenSSL crypto 后端。
 *
 * **进程内调一次，在任何客户端 / 服务端之前。** 内部按名字 `EVP_*_fetch` 一批
 * 算法实现（AES-GCM/CCM、ChaCha20-Poly1305、SHA-256/384、HKDF）存进进程级静态
 * 变量。没调它就用 QUIC 的后果不是"报个错"，而是握手一半失败。
 *
 * @warning **不要调两次。** 上游不查重：第二次会把同一批对象再 fetch 一遍、
 *          覆盖掉第一次的指针。要"调了再调"就先 `_free()`。
 * @return 0 成功。上游的实现从不失败（fetch 不到也只是留 NULL），所以非 0 只
 *         可能来自将来更严的版本 —— 照样该看返回值。
 */
UVCPP_C_API int uvcpp_c_quic_crypto_init(void);

/**
 * @brief 释放上面那一批 EVP 对象。可重复调用。本层不在静态析构里替你调它。
 *
 * @return `UVCPP_C_OK`。**返回 `int` 而不是 `void`**，与这一层的其它导出同一个
 *         形状（C++ 那侧是 `void`）——本层每个导出都在 `try/catch` 收口里，而
 *         收口宏的形状是"出错就 return 一个值"；为了一个 `void` 单开一套宏不划算，
 *         何况 FFI 侧"每次调用都能拿到状态码"本身就更省事。
 */
UVCPP_C_API int uvcpp_c_quic_crypto_free(void);

/**
 * @brief 编进来的 ngtcp2 是哪个版本（形如 `"1.25.0"`）。缓冲区约定见
 *        `uvcpp_c_common.h`。
 *
 * 它证明"ngtcp2 的头路径到得了"，**不证明链上了** —— 要证明链接，用
 * `uvcpp_c_quic_error_string()`（那个真调进 libngtcp2 的符号）。
 */
UVCPP_C_API int uvcpp_c_quic_ngtcp2_version(char* buf, size_t cap);

/**
 * @brief 把 ngtcp2 的错误码翻成它自己的文本（上游 `ngtcp2_strerror()` 的转发）。
 *
 * `code = 0` 得到 `"NO_ERROR"`（一个稳定字面量）。认不出的码得到上游原文
 * `"(unknown)"` —— 别把它当成这一层的文案。
 */
UVCPP_C_API int uvcpp_c_quic_error_string(int code, char* buf, size_t cap);

/* ========================================================================
 * TLS 配置
 * ========================================================================
 * 三种建法 + 三种改法。全部是"建好之后交给端点"的形状，端点不拥有它。
 */

/**
 * @brief 建一个**服务端** TLS 上下文，装一段证书链与它的私钥。
 *
 * @param cert_file PEM 证书链路径。**不能为空。**
 * @param key_file  PEM 私钥路径。**不能为空。**
 *
 * 装完立刻做一次 `SSL_CTX_check_private_key()`：证书与私钥**不配对**在 OpenSSL
 * 里不影响建上下文、也不影响那两个 load 的返回值，它只在**每一次握手**时失败
 * （表现是"服务起来了、端口在听、每条连接建完就被拒"）。在配置阶段调一次，
 * 就把它变成一次明确的构造失败。
 *
 * @return 句柄；失败（文件打不开、格式不对、两者不配对）返回 **NULL**。
 *         与别的 "失败即 NULL" 构造器一样，失败原因不在这里报。
 */
UVCPP_C_API uvcpp_c_quic_tls* uvcpp_c_quic_tls_server_new(const char* cert_file,
                                                          const char* key_file);

/**
 * @brief 建一个**服务端** TLS 上下文，现场生成一张自签证书。
 *
 * 给测试与本地验证用（`generate_self_signed()` 的转发）：不必往仓里塞 PEM
 * 文件，也就不必让用例依赖"证书在哪"。生产环境请用 `_server_new()`。
 *
 * @param common_name 证书的 CN，例如 `"localhost"`。为空时按 `"localhost"` 走。
 * @return 句柄；失败返回 NULL（生成 RSA 密钥可能失败）。
 */
UVCPP_C_API uvcpp_c_quic_tls* uvcpp_c_quic_tls_server_selfsigned(
    const char* common_name);

/**
 * @brief 建一个**客户端** TLS 上下文。**默认不校验对端证书。**
 *
 * @param ca_file 信任库 PEM 路径；**可以为 NULL**（那就是"不校验"，也正是默认）。
 *                给了它并不等于开始校验 —— 还要 `_set_verify(tls, 1)`。
 *
 * 默认关闭校验是刻意的：C++ 的 `tls_mode::CLIENT` 默认是 `PEER`（并已装入系统
 * 信任库），本层把它显式改成 `NONE`。P/Invoke 那一侧的常见场景是"连自家内网、
 * 证书是自签的"，默认校验会让第一次跑起来就是一条握手失败，而失败原因在 C 侧
 * 只表现为一个负的错误码。要校验就两步：`_set_ca_file()` + `_set_verify(1)`。
 */
UVCPP_C_API uvcpp_c_quic_tls* uvcpp_c_quic_tls_client_new(const char* ca_file);

/** @brief 废掉句柄。已经废过 / 传空指针分别得到 `E_STALE` / `E_INVALID_ARG`。 */
UVCPP_C_API int uvcpp_c_quic_tls_free(uvcpp_c_quic_tls* tls);

/** @brief 装信任库（PEM）。@return 0 成功；文件打不开或格式不对返回负值。 */
UVCPP_C_API int uvcpp_c_quic_tls_set_ca_file(uvcpp_c_quic_tls* tls,
                                             const char* ca_file);

/**
 * @brief 开 / 关对端证书校验。
 *
 * @param mode 非 0 = 校验对端证书（`PEER`），0 = 不校验（`NONE`）。
 *             **主机的名字不在这条路上**：本层不提供 `PEER_STRICT` 那一档
 *             （它多出来的"钉主机名"是每条连接各自的事，不是上下文的属性）。
 */
UVCPP_C_API int uvcpp_c_quic_tls_set_verify(uvcpp_c_quic_tls* tls, int mode);

/* ========================================================================
 * 客户端
 * ======================================================================== */

/** @brief 建一个客户端（自带一条循环）。失败返回 NULL。 */
UVCPP_C_API uvcpp_c_quic_client* uvcpp_c_quic_client_new(void);

/**
 * @brief 释放客户端。
 *
 * 会**顺手把借出去的连接句柄收回来**（那一枚在 `_new()` 时就建好了，见文件头）。
 * 已经废过 / 传空指针分别得到 `E_STALE` / `E_INVALID_ARG`。
 *
 * **在回调里调它得到 `E_STATE`**（`uvcpp_c_quic_client_*` 那几条回调也一样）：
 * 端点句柄是调用方所有的、随时可以在回调之外再释放一次，所以"回调里不许释放"
 * 比"先记下来回头再删"少一处说不清的时序。要停止就用
 * `uvcpp_c_quic_client_close()` / `uvcpp_c_quic_client_stop()`。
 */
UVCPP_C_API int uvcpp_c_quic_client_free(uvcpp_c_quic_client* client);

/**
 * @brief 装 TLS 上下文。**不设就是没配** —— 不设时 `_connect()` 会失败。
 *
 * @param tls 生命周期**必须盖住每一条连接**，端点不拥有它。清空传 NULL。
 */
UVCPP_C_API int uvcpp_c_quic_client_set_tls(uvcpp_c_quic_client* client,
                                            uvcpp_c_quic_tls* tls);

/**
 * @brief 宣告本端支持的 ALPN 协议名，**顺序即优先级**。
 *
 * @param protos     长度已知的 C 串数组（每个都以 NUL 结尾）。
 * @param proto_count 数组元素个数。**0 或 NULL = 恢复默认**，也就是回到"从来没
 *                    调过"那个状态（用 `quic_default_alpn()` 的 `"h3"`）。
 *
 * C++ 那侧收的是 `std::vector<std::string>`；这里收 `const char* const*` 是
 * "C 里怎么给一组字符串"的最小形状，**不是为了更灵活**。
 *
 * @note **这一条与 C++ 有一处刻意的不同，得说清楚。** C++ 那边"没调过"与"传一
 *       个空列表"是**两件事**：前者用默认的 `"h3"`，后者是"一个都不发"（对端
 *       无从选择，通常直接握手失败）。C 里没有"没调过"这个可观察的状态 ——
 *       调用方给的就是这个数组长度，`0` 与"忘了调"在函数体里长得一模一样。
 *       本层把它定成**恢复默认**：空列表在 C++ 侧那个语义是个脚枪（"清空"与
 *       "阉掉自己"写成同一件事），而"一个协议名都不发"从来不会是想要的。
 */
UVCPP_C_API int uvcpp_c_quic_client_set_alpn_protos(
    uvcpp_c_quic_client* client, const char* const* protos, size_t proto_count);

/**
 * @brief 空闲超时（毫秒）。**默认 30000**（30 秒）；`0` = 不设超时。
 *
 * 只在 `_connect()` **之前**调有效：握手一开始，传输参数就发出去了。
 */
UVCPP_C_API int uvcpp_c_quic_client_set_idle_timeout(uvcpp_c_quic_client* client,
                                                     uint64_t ms);

/**
 * @brief 发起连接；**握手完成**时回调一次。
 *
 * @param cb 收到 `status == 0` 表示握手完成，可以 `_connection()` 拿来开流了。
 *           非 0 是失败码（握手超时、被拒、TLS 配置不对……）。
 *           **必须给**（C++ 侧那个参数也是必需参数，没有"不关心"这一档）。
 * @param user_data 原样传回。
 *
 * 只能在**循环线程**上调用（规矩 5）。回调也在循环线程上。
 */
UVCPP_C_API int uvcpp_c_quic_client_connect(uvcpp_c_quic_client* client,
                                            const char* host, int port,
                                            uvcpp_c_status_cb cb,
                                            void* user_data);

/**
 * @brief 取那条连接句柄（**借来的**，见文件头）。
 *
 * **永远返回同一个指针**（它在 `_new()` 时就建好了），所以可以存下来。
 * 还没连上时用它做任何事都得到 `UVCPP_C_E_STATE`（句柄是活的，只是底下还没有
 * 连接）；连接关掉之后得到 `UVCPP_C_E_STALE`。
 *
 * @return 句柄；客户端本身无效时返回 NULL。
 */
UVCPP_C_API uvcpp_c_quic_connection* uvcpp_c_quic_client_connection(
    uvcpp_c_quic_client* client);

/**
 * @brief 关客户端（连带那条连接），对应 C++ 的无参 `uvcpp_quic_client::close()`。
 *
 * @return 0 = 关掉了（或本来就没东西可关）；`UV_ENOTCONN` = 没连过。
 *
 * 关闭是**优雅的**：CONNECTION_CLOSE 要发出去、`on_close` 要跑一次，所以调完
 * 之后循环还得再转几轮。要想给对端一个非 0 的应用错误码，用
 * `uvcpp_c_quic_conn_close(conn, code)` —— **那是另一条路**，C++ 侧两条本来就
 * 分开（端点的 `close()` 不收码，连接那枚才收）。
 */
UVCPP_C_API int uvcpp_c_quic_client_close(uvcpp_c_quic_client* client);

/**
 * @brief 泵客户端的循环，**阻塞**直到它自然停（没有活句柄了）或有人 `_stop()`。
 *
 * C++ 的 `uvcpp_quic_client` **没有** `run()`（它把循环借给你，用 `get_loop()`）；
 * C 面没有 `uvcpp_c_loop` 这个句柄，所以这里补一对。等价于
 * `get_loop()->run(UV_RUN_DEFAULT)` / `stop()`，与 `uvcpp_c_tcp_server_run()`
 * 同形 —— 服务端那条同理。
 *
 * **不要在两个线程上同时泵同一个端点的循环**（libuv 的规矩，不是本层的）。
 */
UVCPP_C_API int uvcpp_c_quic_client_run(uvcpp_c_quic_client* client);

/**
 * @brief 只泵一轮（`UV_RUN_NOWAIT`）就返回，不阻塞。
 *
 * 这是 `uvcpp_c_tcp_server_run()` 之外的另一半：**用它可以自己写"泵到某个条件
 * 成立为止"**（C++ 的功能测试里那个 `wait_until` 就是 `UV_RUN_NOWAIT` + 睡
 * 1ms）。C 面没有定时器、也没有 `uvcpp_c_loop`，所以"轮询式驱动"这条路必须由
 * 这一条打开，否则"等握手完成再发请求"在纯 C 里就没法写。
 *
 * @return `uv_run()` 的返回值：非 0 = 这一轮里还有活句柄。
 */
UVCPP_C_API int uvcpp_c_quic_client_run_once(uvcpp_c_quic_client* client);

/** @brief 停循环（`get_loop()->stop()`），让 `_run()` 返回。 */
UVCPP_C_API int uvcpp_c_quic_client_stop(uvcpp_c_quic_client* client);

/* ========================================================================
 * 服务端
 * ======================================================================== */

/** @brief 建一个服务端（自带一条循环）。失败返回 NULL。 */
UVCPP_C_API uvcpp_c_quic_server* uvcpp_c_quic_server_new(void);

/**
 * @brief 释放服务端（顺手把还在手上的借用句柄全部收回来）。
 *
 * 已经废过 / 传空指针分别得到 `E_STALE` / `E_INVALID_ARG`；**在回调里调它得到
 * `E_STATE`**（理由与 `uvcpp_c_quic_client_free()` 那一段逐字相同）。
 */
UVCPP_C_API int uvcpp_c_quic_server_free(uvcpp_c_quic_server* server);

/**
 * @brief 装 TLS 上下文。**不设就是没配**，那时 `listen()` 直接失败
 *        —— QUIC 没有明文模式（ALPN 是 TLS 扩展）。
 */
UVCPP_C_API int uvcpp_c_quic_server_set_tls(uvcpp_c_quic_server* server,
                                            uvcpp_c_quic_tls* tls);

/**
 * @brief 给出愿意接受的 ALPN 协议名并装选择回调（顺序即优先级）。
 *
 * 参数与"0 / NULL = 恢复默认"那条约定同客户端那一枚。
 *
 * **挑不中的后果是握手仍然成功**：本层装的选择回调在没有交集时返回
 * `SSL_TLSEXT_ERR_NOACK`（`src/ssl/uvcpp_ssl_context.cpp` 那条），不是 fatal
 * —— 于是老式的、根本不发 ALPN 扩展的对端也能把握手走完（它对 h1 的那些回环
 * 用例是必需的）。代价是"没有协议名可用"这件事不报错，只是 `_conn_alpn_selected()`
 * 交回空串 —— **对 h3 来说那就是这条连接没法用**，所以 h3 那一层自己还要在
 * `on_alpn` 里判一下（见 `uvcpp_c_http3.h` 的 `start()` 一段）。
 */
UVCPP_C_API int uvcpp_c_quic_server_set_alpn_protos(
    uvcpp_c_quic_server* server, const char* const* protos, size_t proto_count);

/**
 * @brief 空闲超时（毫秒）。**默认 30000**（30 秒）；`0` = 不设超时。
 *
 * 它是服务端收掉"对端已经走了但没告别"的连接**唯一**的手段（UDP 上没有断开这
 * 件事）。只对它 `_listen()` **之后**建出来的连接有效。
 */
UVCPP_C_API int uvcpp_c_quic_server_set_idle_timeout(uvcpp_c_quic_server* server,
                                                     uint64_t ms);

/**
 * @brief 登记要绑的地址与端口。**不创建、不绑任何 socket。**
 *
 * @param ip   IPv4 / IPv6 的**字面量**。NULL = 通配 IPv4（`"0.0.0.0"`）。
 *             域名不收（UDP 的绑定要一个 `sockaddr`，"解析出多个地址时选哪个"
 *             不是这里该猜的）。
 * @param port `0..65535`。**`0` 是"由内核挑一个"**，挑中的在 `_listen()` 之后由
 *             `_configured_port()` 报出来（回环用例的常规写法）。
 * @return 0 成功；`UV_EINVAL` 地址或端口不合法（地址经 `uv_inet_pton()` 真校验，
 *         所以 `"999.1.1.1"` 当场被拒，而不是等到 `listen()` 才炸）。
 *
 * 端口被占用要到 `_listen()` 才暴露 —— 所以访问器叫 `_configured_port()` 而不是
 * `bound_port()`，而 `_configured_ip()` 报的是**登记值**。
 */
UVCPP_C_API int uvcpp_c_quic_server_bind(uvcpp_c_quic_server* server,
                                         const char* ip, int port);

/**
 * @brief 端口。名字与 C++ 的 `configured_port()` **逐字相同**，是因为它俩的
 *        语义都有一处话要说：`_listen()` 之前报的是**登记值**，之后报的是
 *        **内核实际给的那个**（没登记过就是 0）。
 *
 * 于是 `bind("127.0.0.1", 0)` + `listen()` + 读它 = 回环用例发现端口的常规写法。
 * 名字之所以**不叫 `local_port()`**：**端口被占用要到 `listen()` 才暴露**，所以
 * `bind()` 返回 0 不等于"这个端口归我了"，而"local"这个名字会把它说成后者
 * （C++ 侧那段注释写得更长，两边是同一句话）。
 */
UVCPP_C_API int uvcpp_c_quic_server_configured_port(uvcpp_c_quic_server* server);

/** @brief 登记的地址字面量（"调用方给缓冲区"那条约定）；没登记过时长度为 0。 */
UVCPP_C_API int uvcpp_c_quic_server_configured_ip(uvcpp_c_quic_server* server,
                                                  char* buf, size_t cap);

/**
 * @brief 开始接收连接。
 *
 * @param cb 每收到一条**新**连接时调一次。它给的那个 `uvcpp_c_quic_connection*`
 *           是**借来的**（见文件头），在这个底层连接被关掉之前一直有效。
 * @param user_data 原样传回。
 *
 * @warning **`cb` 在连接刚建出来（首包到达）时跑，不在握手完成时跑。**
 *          所以 `on_alpn` / `on_read` 这些必须在**它里面**用
 *          `uvcpp_c_quic_conn_set_callbacks()` 装上 —— 它们都排在它之后。
 *          这一点 C++ 侧写得比这里更长，两边是同一句话。
 *
 * @return 0 = 已经在收；负值 = 没起来（没设 TLS 上下文、端口被占用、地址不合法）。
 *         **返回负值时 `cb` 一次都不会被调。**
 */
UVCPP_C_API int uvcpp_c_quic_server_listen(uvcpp_c_quic_server* server,
                                           void (*cb)(void* user_data,
                                                      uvcpp_c_quic_connection* c),
                                           void* user_data);

/**
 * @brief 跑循环，**这个调用会阻塞**，直到 `_stop()`。
 *
 * 与 `uvcpp_c_tcp_server_run()` 同形：服务端那一侧在主线程上跑循环，客户端
 * 在别的线程里跑它自己那条。
 */
UVCPP_C_API int uvcpp_c_quic_server_run(uvcpp_c_quic_server* server);

/**
 * @brief 只泵一轮（`UV_RUN_NOWAIT`）就返回。语义与客户端那一枚逐个对应
 *        （见 `uvcpp_c_quic_client_run_once()` 的说明）。
 */
UVCPP_C_API int uvcpp_c_quic_server_run_once(uvcpp_c_quic_server* server);

/**
 * @brief 停循环（`uvcpp_quic_server::stop()`）。**不关已有的连接** —— 要收尾就
 *        逐个 `uvcpp_c_quic_conn_close()`，或者释放服务端。
 */
UVCPP_C_API int uvcpp_c_quic_server_stop(uvcpp_c_quic_server* server);

/* ========================================================================
 * 连接（**借来的句柄**）
 * ========================================================================
 * 一个 `_free()` 都没有 —— 见文件头第 1 条。
 */

/**
 * @brief 装回调（**以 `uint32_t size` 打头**，规则见 `uvcpp_c_common.h` 规矩 3）。
 *
 * 七条与 C++ 的 `uvcpp_quic_connection::callbacks` 一一对应，语义逐字照抄
 * （`src/quic/uvcpp_quic_connection.h` 里每一条的 `@brief` 才是权威）。全部在
 * **循环线程**上调用。
 *
 * @param user_data 原样传给**所有**回调的第一个参数（整张表共用一个，与批 2/3a
 *                  同一形状）。
 */
typedef struct uvcpp_c_quic_callbacks {
  uint32_t size; /**< = `sizeof(uvcpp_c_quic_callbacks)`。**必须最先填。** */

  /**
   * @brief 收到一条流上的数据（**带 `stream_id`** —— 这是 QUIC 与 TCP 的本质
   *        差别：一条连接上并行跑着很多条流）。
   *
   * @param result 形状与 net 那层**同一个结构体**（`uvcpp_c_read_result`）：
   *               `event` 是 DATA / PEER_CLOSED / READ_ERROR，`fin` 在 QUIC 上
   *               **会有真值**（STREAM 帧可以同时带数据与 FIN 位）。
   *               `result->data` **只在那次回调里有效**，要留就当场拷走。
   */
  void (*on_read)(void* user_data, uvcpp_c_quic_connection* c,
                  int64_t stream_id, const uvcpp_c_read_result* result);

  /** @brief 对端放开了流额度：`bidi` 非 0 是双向额度，否则单向；`max_streams` 是总额。 */
  void (*on_streams_available)(void* user_data, uvcpp_c_quic_connection* c,
                               int bidi, uint64_t max_streams);

  /** @brief 对端对一条流发了 STOP_SENDING（读方向被掐掉），带它的应用错误码。 */
  void (*on_stop_sending)(void* user_data, uvcpp_c_quic_connection* c,
                          int64_t stream_id, uint64_t app_error_code);

  /** @brief 流被建出来（本端或对端）。 */
  void (*on_stream_open)(void* user_data, uvcpp_c_quic_connection* c,
                         int64_t stream_id);

  /**
   * @brief 一次 `_write_stream()` 的结果（**一次调用一次回调**）。
   *
   * @param status 0 成功；非 0 是失败码。
   */
  void (*on_write)(void* user_data, uvcpp_c_quic_connection* c,
                   int64_t stream_id, int status);

  /**
   * @brief 握手协商出的 ALPN。
   *
   * @param alpn 只在这次回调里有效（C++ 侧是 `const std::string&`）。
   *
   * **h3 那三条关键流要在这一条之后才开得出来** —— 见 `uvcpp_c_http3.h`。
   */
  void (*on_alpn)(void* user_data, uvcpp_c_quic_connection* c,
                  const char* alpn);

  /**
   * @brief 连接结束了（对端关、超时、或本端关完）。**恰好一次。**
   *
   * @param error_code **按符号分两种意思**：`> 0` 是要发给对端的应用错误码
   *                   （对端发的 CONNECTION_CLOSE），`< 0` 是本端的失败码。
   *
   * 回调返回后**不要再碰这条连接上的任何句柄** —— 那时它已经被本层毒化，
   * 再用是 `E_STALE`（不会崩）。
   */
  void (*on_close)(void* user_data, uvcpp_c_quic_connection* c, int error_code);
} uvcpp_c_quic_callbacks;

/**
 * @brief 装回调。可以在连接活着的任何时刻**重装**（覆盖）。
 *
 * 底下还没有连接时（客户端 `_connect()` 之前的那个窗口）调它也**不是错误**：
 * 表先记在句柄里，接上时自然生效 —— "先记表、再 `_connect()`"是预期写法。
 *
 * @warning **装了 h3 之后不要再调它。** `uvcpp_c_h3_connection_new()` +
 *          `_conn_start()` 会把连接上这张表**整个换成 h3 自己的**（h3 要吃
 *          `on_read` / `on_alpn`，这是 C++ 侧本来就有的分工），这时候再装一次
 *          就是把 h3 的驱动拆掉一半 —— 症状是请求发出去没人回，而不是一条错误码。
 */
UVCPP_C_API int uvcpp_c_quic_conn_set_callbacks(uvcpp_c_quic_connection* c,
                                                const uvcpp_c_quic_callbacks* cbs,
                                                void* user_data);

/**
 * @brief 连接状态；取值见 `enum uvcpp_c_quic_state`。
 *
 * **底下还没有连接时返回 `UVCPP_C_QUIC_IDLE`**（"还没有对端"）—— 那不是错误码。
 * 句柄本身已经废了才返回 `E_STALE`。
 */
UVCPP_C_API int uvcpp_c_quic_conn_state(const uvcpp_c_quic_connection* c);

/** @brief 协商出的 ALPN（"调用方给缓冲区"那条约定；握手完成前长度为 0）。 */
UVCPP_C_API int uvcpp_c_quic_conn_alpn_selected(const uvcpp_c_quic_connection* c,
                                                char* buf, size_t cap);

/**
 * @brief 开一条流。
 *
 * @param bidi 非 0 = 双向流，0 = 单向流（**只发不收**：单向流没有读方向，
 *             对端也不会在上面 `_write_stream()` 回来）。
 * @return 成功返回**流号**（非负）；失败返回负的错误码
 *         （`NGTCP2_ERR_STREAM_ID_BLOCKED` 是其中一种**正常**结果：对端还没放开
 *         额度 —— 等 `on_streams_available` 再试）。
 *
 * 流号也可以用**负**的（QUIC 规范里服务端发起的流就是负数）—— 所以判断成功
 * 要看 `< 0` 是失败，而不是 `> 0` 是成功。
 */
UVCPP_C_API int64_t uvcpp_c_quic_conn_open_stream(uvcpp_c_quic_connection* c,
                                                  int bidi);

/**
 * @brief 往一条流里写。
 *
 * @param end_stream 非 0 = 这块之后本端的写方向就结束了（FIN）。
 * @return 0 = 已受理，**一次 `on_write` 会来**（成功或失败都来一次）；
 *         非 0 = 失败码，那时 `on_write` **不会**来。受理与"发到对端"是两件事。
 */
UVCPP_C_API int uvcpp_c_quic_conn_write_stream(uvcpp_c_quic_connection* c,
                                               int64_t stream_id,
                                               const char* data, size_t len,
                                               int end_stream);

/**
 * @brief 重置一条流的写方向（发 RESET_STREAM），带上应用错误码。
 *
 * 与 `_shutdown_stream_read()` 的区别：那个掐的是**读**（发 STOP_SENDING）。
 */
UVCPP_C_API int uvcpp_c_quic_conn_shutdown_stream(uvcpp_c_quic_connection* c,
                                                  int64_t stream_id,
                                                  uint64_t app_error_code);

/** @brief 掐掉一条流的读方向（发 STOP_SENDING）。 */
UVCPP_C_API int uvcpp_c_quic_conn_shutdown_stream_read(uvcpp_c_quic_connection* c,
                                                       int64_t stream_id,
                                                       uint64_t app_error_code);

/** @brief 还剩多少条流可以开（`bidi` 非 0 问双向）。 */
UVCPP_C_API uint64_t uvcpp_c_quic_conn_streams_left(
    const uvcpp_c_quic_connection* c, int bidi);

/**
 * @brief 关掉这条连接（把 `error_code` 发给对端）。0 = 干净关闭。
 *
 * @return 0 = 已受理。**真正的结束不是这里返回的时候** —— `on_close` 之后才算。
 */
UVCPP_C_API int uvcpp_c_quic_conn_close(uvcpp_c_quic_connection* c,
                                        int error_code);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SRC_CAPI_UVCPP_C_QUIC_H */
