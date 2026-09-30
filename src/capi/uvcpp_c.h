/**
 * @file src/capi/uvcpp_c.h
 * @brief C ABI 的伞头：按本库编进了哪些模块，把对应的那几份头拉进来。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 只 include 这一个头就够了 —— 它按 `uvcpp_config.h` 里这一份构建的真实开关
 * 决定拉哪几份（那份头是 configure 期生成的，写的是"这份库里到底有什么"，
 * 不是"你可能想要什么"）。所以：
 *
 *   - `#include <capi/uvcpp_c.h>` 之后，凡是你这份库里开了的模块，它的
 *     `uvcpp_c_*` 声明都在；关掉的模块**一个字都不出现**（而不是"声明在、
 *     一调就失败"）。**这道 `#if` 在本文件里**，子头自己没有模块守卫 ——
 *     子头是"这个模块的 C 面长什么样"，而"这份库里有没有这个模块"只有
 *     `uvcpp_config.h` 知道；两处都写就是两份会各自漂的开关。
 *   - 想精确控制（比如只看 net 那一份）就直接 include 子头 —— 那时**绕过**了
 *     上面这道判断，所以要么你确实知道自己在干什么，要么先看一眼
 *     `UVCPP_CAPI_ENABLE` 那一段。
 *
 * 一份最小的用法（C# 侧 P/Invoke 之前先自己在 C 里验一遍的那种）：
 *
 * @code
 *   #include <capi/uvcpp_c.h>
 *
 *   static void on_read(void* ud, uvcpp_c_tcp_client* c,
 *                       const uvcpp_c_read_result* r) {
 *     if (r->event == UVCPP_C_READ_DATA) fwrite(r->data, 1, r->size, stdout);
 *   }
 *
 *   int main(void) {
 *     if (uvcpp_c_abi_version() != UVCPP_C_ABI_VERSION) return 1;  // 头与库对不上
 *     uvcpp_c_tcp_client* c = uvcpp_c_tcp_client_new();
 *     uvcpp_c_tcp_client_events ev = {0};
 *     ev.size = (uint32_t)sizeof(ev);
 *     ev.on_read = on_read;
 *     uvcpp_c_tcp_client_set_events(c, &ev);
 *     uvcpp_c_tcp_client_connect_wait(c, "127.0.0.1", 80, 5000);
 *     uvcpp_c_tcp_client_run(c);
 *     uvcpp_c_tcp_client_free(c);
 *     return 0;
 *   }
 * @endcode
 *
 * 逐条读一遍 `uvcpp_c_common.h` 开头的"五条承重规矩"再动手 —— 尤其是**异常
 * 不过边界**与**回调里的指针只在那一次有效**这两条，它们是 FFI 侧最常见的两个
 * 崩溃来源。
 */

#pragma once
#ifndef SRC_CAPI_UVCPP_C_H
#define SRC_CAPI_UVCPP_C_H

#include <uvcpp/uvcpp_config.h>

#include "capi/uvcpp_c_common.h"

/* net：TCP 客户端与服务端。批 1 落地。 */
#if UVCPP_NET_ENABLE
#  include "capi/uvcpp_c_net.h"
#endif

/* webapp：app / 路由 / 中间件 / req / resp / next / 延迟应答 / 静态 / ws 路由。
 * 批 2 落地。注意它**依赖** net 与 web 两层（`uvcpp_web_app` 自己就骑在
 * `uvcpp_http_server` 上），所以 CMake 的守卫链保证 `UVCPP_CAPI_ENABLE=ON` 时
 * 这两个宏必然是 1 —— 这里不必再写"webapp 开了但 web 没开"的分支。 */
#if UVCPP_WEBAPP_ENABLE
#  include "capi/uvcpp_c_webapp.h"
#endif

/* web：HTTP 客户端与 WebSocket 客户端（**只给客户端**，服务端走 webapp 那份）。
 * 批 2 落地。 */
#if UVCPP_WEB_ENABLE
#  include "capi/uvcpp_c_web.h"
#endif

/* http2：会话与连接。批 3 落地。
 *
 * 它**不住在 net 或 web 之下**：`uvcpp_h2_connection` 是骑在一条已经握手完的
 * `uvcpp_tcp_client` 上的（ALPN 协商出 h2），请求 / 响应类型又是 web 的。
 * 那两个宏在 `UVCPP_CAPI_ENABLE=ON` 时必然是 1（守卫链保证），所以这里只判
 * NGHTTP2 自己那一格。 */
#if UVCPP_NGHTTP2_ENABLE
#  include "capi/uvcpp_c_http2.h"
#endif

/* quic / http3 的子头在后续批次里加到这里，形状同上：
 * 一个 `#if UVCPP_XXX_ENABLE` 包一份 `#include`，不带任何别的逻辑。 */

#endif  /* SRC_CAPI_UVCPP_C_H */
