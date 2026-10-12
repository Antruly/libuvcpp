/**
 * @file tests/functional/reuseport_probe.h
 * @brief 测试侧的平台探针：**内核到底给不给得出 `SO_REUSEPORT` 的分流语义**。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么非要有它
 * --------------
 * QUIC 的扇出（`uvcpp_http_server::listen_quic()` / `uvcpp_quic_server`）在
 * "平台给得出标志"与"平台给不出"之间**必须回落**，而回落之后它报出来的三个数
 * （`is_reuseport()` 假、`quic_fanout()` 假、连接全落在 0 号）**与一个"把标志
 * 悄悄丢掉、根本没试过"的坏实现一模一样**。于是任何一条判据只要分叉在
 * `is_reuseport()` / `quic_fanout()` 上，这两种情况在它眼里就不可分 —— 变异表
 * 里 `M1`（端点循环写死 1）与 `M3`（`flags` 恒 0）正是这么活下来的。
 *
 * 分得开两者的只有一件事：**这个平台上分流到底成不成立**。而它是库外的
 * 事实，由这里自己问一次内核得到 —— 用一条裸 `uv_loop_t`，在同一个
 * `127.0.0.1` 端口上**带同一个标志绑两次**：两次都成功 ⇒ 两次 `bind()` 被归进
 * 了同一个 REUSEPORT 组，分流成立；任何一次失败 ⇒ 不成立。
 *
 * 三条腿的结论（都**不用 `#ifdef` 猜**，一律由这次真绑定定）：
 *   - Windows：libuv 在造 socket **之前**就返 `ERROR_NOT_SUPPORTED` ⇒ 假；
 *   - macOS 等：`uv__sock_reuseport()` 返 `UV_ENOTSUP` ⇒ 假；
 *   - libuv < 1.50：`UVCPP_UDP_REUSEPORT_FLAG` 是 0 ⇒ 直接短路成假（那时连
 *     标志都没有，没什么可分流的）。
 *
 * 用法（**只在"请求了标志"的分支上说话**）：
 *
 *     const bool platform_can = uvcpp_test::platform_honors_reuseport();
 *     if (platform_can) {
 *       CHECK(库报的"带上了标志"为真, "内核给得出时不许报假");
 *     }
 *
 * 不给出的平台上这条**自动跳过** —— 那时候回落本来就是对的，断言反而是错的。
 */

#ifndef UVCPP_TESTS_FUNCTIONAL_REUSEPORT_PROBE_H_
#define UVCPP_TESTS_FUNCTIONAL_REUSEPORT_PROBE_H_

#include <uv.h>

#include "handle/uvcpp_udp.h"  // UVCPP_UDP_REUSEPORT_FLAG

namespace uvcpp_test {

/**
 * @brief 这个平台/这份 libuv **真的**会按四元组分流吗。
 *
 * 自己起一条裸循环（`uvcpp_loop` 没有对外的 `uv_loop_t*` 口子，而且这里本来
 * 也不该借被测对象的循环），绑两次、问一次、拆干净。
 */
inline bool platform_honors_reuseport() {
  if (UVCPP_UDP_REUSEPORT_FLAG == 0) return false;  // 编译期就没有这个标志

  uv_loop_t l;
  if (uv_loop_init(&l) != 0) return false;

  const unsigned int flag = static_cast<unsigned int>(UVCPP_UDP_REUSEPORT_FLAG);
  uv_udp_t a;
  uv_udp_t b;
  bool a_inited = false;
  bool b_inited = false;
  bool both     = false;

  struct sockaddr_in sa;
  if (uv_ip4_addr("127.0.0.1", 0, &sa) == 0 && uv_udp_init(&l, &a) == 0) {
    a_inited = true;
    if (uv_udp_bind(&a, reinterpret_cast<const struct sockaddr*>(&sa), flag) ==
        0) {
      // 端口是内核挑的 ⇒ 读回来，第二个绑它。
      struct sockaddr_storage ss;
      int                     slen = static_cast<int>(sizeof(ss));
      if (uv_udp_getsockname(&a, reinterpret_cast<struct sockaddr*>(&ss),
                             &slen) == 0 &&
          ss.ss_family == AF_INET) {
        struct sockaddr_in sb;
        const int          port =
            ntohs(reinterpret_cast<struct sockaddr_in*>(&ss)->sin_port);
        if (uv_ip4_addr("127.0.0.1", port, &sb) == 0 &&
            uv_udp_init(&l, &b) == 0) {
          b_inited = true;
          both     = (uv_udp_bind(&b,
                                  reinterpret_cast<const struct sockaddr*>(&sb),
                                  flag) == 0);
        }
      }
    }
  }

  if (b_inited) uv_close(reinterpret_cast<uv_handle_t*>(&b), nullptr);
  if (a_inited) uv_close(reinterpret_cast<uv_handle_t*>(&a), nullptr);
  uv_run(&l, UV_RUN_NOWAIT);  // 让上面那两个 close 回调跑掉
  uv_loop_close(&l);
  return both;
}

}  // namespace uvcpp_test

#endif  // UVCPP_TESTS_FUNCTIONAL_REUSEPORT_PROBE_H_
