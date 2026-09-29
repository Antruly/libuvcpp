/**
 * @file src/quic/uvcpp_quic_ngtcp2.h
 * @brief 私有头：唯一一处把 `<ngtcp2/ngtcp2.h>` 拉进来的地方。
 * @author zhuweiye
 * @version 1.4.1
 *
 * **这个头不许被任何公开头包含，也不许出现在 `src/quic/` 之外的 `.cpp` 里。**
 * 理由与 `src/http2/uvcpp_h2_nghttp2.h` 逐条同源，但这里多一条本模块独有的：
 *
 * 1. **公开头里出现它，使用者就得配两套搜索路径** —— ngtcp2 的 `lib/includes` 与
 *    `crypto/includes`（下面第二个 include 还要 `<openssl/ssl.h>`，于是连
 *    OpenSSL 的头路径也一起要）。本库为此把 ngtcp2 压成静态、链接标了 PRIVATE，
 *    就是为了让 `libuvcpp.so` 的运行时依赖集与消费方的编译依赖集一条都不多。
 *    把包含关系漏进公开头，前面那些就全白做了 ——
 *    `CMakeLists.txt` 里那段 `target_link_libraries(... PRIVATE
 *    $<BUILD_INTERFACE:ngtcp2_static> ...)` 是配着这条存在的。
 *
 * 2. **`ssize_t` 那个 MSVC 坑这里没有**（这里与 nghttp2 那处不同，值得说明白，
 *    免得后来人照着 `uvcpp_h2_nghttp2.h` 把那段 `#define ssize_t int` 抄过来）。
 *    实测 ngtcp2 v1.25.0 的 `lib/includes/ngtcp2/ngtcp2.h` 里 `ssize_t` 出现
 *    **零次**，它用的是 `size_t` / `ptrdiff_t` / 自带的定长类型。抄过来不会"更保险"，
 *    只会在 POSIX 上撞 `typedef _ssize_t ssize_t;` 而编不过。
 *
 * 与 nghttp2 的一处**实质差别**：ngtcp2 把库版本只以宏的形式给出
 * （`NGTCP2_VERSION`，在它生成的 `version.h` 里），**没有** `ngtcp2_version()`
 * 这样的函数。所以想拿一个"真调进库里"的证据，只能用别的东西 —— 见
 * `uvcpp_quic_common.h` 里 `quic_error_string()` 的说明。
 */

#ifndef SRC_QUIC_UVCPP_QUIC_NGTCP2_H
#define SRC_QUIC_UVCPP_QUIC_NGTCP2_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_QUIC_ENABLE

// 传输状态机本身。
#include <ngtcp2/ngtcp2.h>
// crypto 后端：把 ngtcp2 的 crypto 回调接到 OpenSSL 的 EVP/QUIC 那套上。
// 这一句同时把 `<openssl/ssl.h>` 拉进来 —— 这是本头比 `uvcpp_h2_nghttp2.h`
// 多出来的那点剂量，也是它更不能被公开头包含的原因。
//
// **只有 mainline OpenSSL ≥ 3.2 才走这条路**：quictls/LibreSSL 那份是
// `ngtcp2_crypto_quictls.h`、对应的目标叫 `ngtcp2_crypto_quictls_static`。
// `CMakeLists.txt` 里那段 QUIC API 预检（复刻 ngtcp2 自己的分支顺序）是专门
// 用来把这两种树分开的，改那边之前先读这里。
#include <ngtcp2/ngtcp2_crypto_ossl.h>
// 与上面那句**不是**二选一，两个都要。
//
// `ngtcp2_crypto_ossl.h` 只把 crypto 的**后端**（`ngtcp2_crypto_ossl_ctx` 那一族
// 上下文管理函数）给出来，它自己只 include `ngtcp2.h` 与 `<openssl/ssl.h>`，
// **不**include 这个头。而我们要填进 `ngtcp2_callbacks` 的那一堆
// `ngtcp2_crypto_*_cb`（`encrypt` / `decrypt` / `hp_mask` / `recv_retry` /
// `client_initial` / …）与 struct 成员用的 `ngtcp2_crypto_conn_ref` 全在
// `ngtcp2_crypto.h` 里。少了这一句的报错是"`ngtcp2_crypto_encrypt_cb` 未声明"
// 这种，看着像名字写错了，其实是头没进来。
#include <ngtcp2/ngtcp2_crypto.h>

namespace uvcpp {
namespace quic_detail {

/**
 * @brief `NGTCP2_VERSION` 的字面量，形如 `"1.25.0"`。
 *
 * 做成一个函数而不是让宏漏出去，是为了让 `NGTCP2_VERSION` 这个标识符不出现在
 * 本头之外 —— 它只在**编译期**属于 ngtcp2 的实现细节，不该成为本库的编译契约。
 */
inline const char* version_literal() { return NGTCP2_VERSION; }

}  // namespace quic_detail
}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE

#endif  // SRC_QUIC_UVCPP_QUIC_NGTCP2_H
