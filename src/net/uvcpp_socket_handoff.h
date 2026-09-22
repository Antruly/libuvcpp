/**
 * @file src/net/uvcpp_socket_handoff.h
 * @brief 把一条**已接受**的连接转手给另一条循环（跨平台的那一段差异）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * `uv_accept` 要求 server 与 client 在**同一条循环**上（POSIX 是硬断言，见
 * `libuv:unix/stream.c`），所以"把连接交给别的循环"不能把句柄直接递过去 ——
 * 必须在接受者这条循环上先把 socket 取出来，再到目标循环上重新装成句柄。
 *
 * 取出来这件事两个平台的做法不同，原因也各不相同：
 *
 *   - **POSIX**：`uv_tcp_open` 只拒绝"该 fd 已经被**同一条**循环持有"
 *     （`libuv:unix/core.c` 的按循环查重），同一个 fd 号交给两条循环不会被拒
 *     —— 但两条循环各自关闭时会 `close()` 同一个 fd 两次。所以必须 `dup()`。
 *     dup 出来的 fd 与原件共用同一个 file description，socket 状态是同一份。
 *   - **Windows**：完成端口的关联**是一次性的** —— 一个 socket 对象只能属于
 *     一个完成端口，重复关联一律 `ERROR_INVALID_PARAMETER`（与在途 I/O、端口
 *     是否活着都无关，见 `doc/worker-process-design.md` §9）。所以"改关联"这条
 *     路不存在，只能拿一份**新的 socket 对象**。
 *
 * @warning Windows 这条路造出来的 socket 在 libuv 眼里是 **imported** 的
 *          （`uv_tcp_open` → `uv__tcp_set_socket(..., imported=1)`），而
 *          `libuv/libuv#5282` 那条待修缺陷就在这条血统上被观测到。**注意别把
 *          "imported 才是问题所在"当结论**：`uv__tcp_set_socket` 里那个
 *          `SetFileCompletionNotificationModes(...SYNC_BYPASS_IOCP...)`
 *          （`_local_deps/libuv/src/win/tcp.c:120-124`）只看
 *          `UV_HANDLE_EMULATE_IOCP` 与 LSP 两项，**两种血统都会走到**。
 */

#pragma once
#ifndef SRC_NET_UVCPP_SOCKET_HANDOFF_H
#define SRC_NET_UVCPP_SOCKET_HANDOFF_H

#include <uvcpp/uvcpp_config.h>

#include <uv.h>

namespace uvcpp {

class uvcpp_tcp;

/**
 * @brief 从一条刚被 `uv_accept` 接受的临时句柄上取一份"可以交给别的循环"的 socket。
 *
 * **必须在关掉那个临时句柄之前调**：`uv_close` 会同步关掉它那份 fd
 * （`unix/stream.c` 的 `uv__stream_close`），关完再取就是 `EBADF` / `WSAENOTSOCK`。
 *
 * 成功时 \p out_sock 归调用方，且它是**独立的一份**：接受者关掉自己那份不会
 * 影响它（Windows：另一个 socket 对象；POSIX：另一个 fd，同一个 file description）。
 * 失败时 \p out_sock 保持无效值，中途造出来的东西一定已经收干净。
 *
 * @param accepted 接受者循环上的临时句柄（还没取过 fd 的）。
 * @param out_sock 出参。
 * @return 0 或 libuv 错误码。
 */
int uvcpp_handoff_extract(uvcpp_tcp* accepted, uv_os_sock_t& out_sock);

/**
 * @brief 关掉一个**裸** socket（还没装进任何句柄的那种）。
 *
 * 给转手末尾那些失败路径用：`uv_tcp_open` 失败时 socket 已经交出来了，但它不属于
 * 任何句柄，没人会替它收尾（libuv 在 `uv_tcp_open` 失败时**不接管**它）。
 */
void uvcpp_handoff_close_raw(uv_os_sock_t sock);

/** @brief "无效 socket"的字面量（POSIX 是 int 的 -1，Windows 是 SOCKET 的 ~0）。 */
uv_os_sock_t uvcpp_handoff_invalid_socket();

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_SOCKET_HANDOFF_H
