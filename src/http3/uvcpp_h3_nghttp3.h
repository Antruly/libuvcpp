/**
 * @file src/http3/uvcpp_h3_nghttp3.h
 * @brief 私有头：唯一一处把 `<nghttp3/nghttp3.h>` 拉进来的地方。
 * @author zhuweiye
 * @version 1.4.1
 *
 * **这个头不许被任何公开头包含，也不许出现在 `src/http3/` 之外的 `.cpp` 里。**
 * 公开头里出现它，使用者和测试 TU 就都要 nghttp3 的 include 路径与
 * `NGHTTP3_STATICLIB` —— 本库为此把 nghttp3 压成静态、链接标了 PRIVATE，
 * 就是为了让 `libuvcpp.so` 的运行时依赖集一条都不多。把包含关系漏进公开头，
 * 前面那些就全白做了。
 *
 * **与 `uvcpp_h2_nghttp2.h` 的一处实质差别，别照着那个头抄：MSVC 的 `ssize_t`
 * 垫片这里不需要。** 实测 nghttp3 v1.18.0 的 `lib/includes/nghttp3/nghttp3.h`
 * 里 `ssize_t` 出现**零次** —— 它自己的长度类型是
 * `typedef ptrdiff_t nghttp3_ssize;`（`:82`）。照着 h2 那段写
 * `#define ssize_t int` 不会"更保险"，只会在 POSIX 上撞
 * `typedef _ssize_t ssize_t;`（这个头 `:49` 无条件 include `<sys/types.h>`）
 * 而直接编译不过。
 *
 * 另一处差别也不必处理：nghttp3 自己在 `:36-38` 就带了
 * `#ifdef __cplusplus` / `extern "C" {`，所以这里不用像别处那样手工包一层。
 */

#ifndef SRC_HTTP3_UVCPP_H3_NGHTTP3_H
#define SRC_HTTP3_UVCPP_H3_NGHTTP3_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_HTTP3_ENABLE

#include <nghttp3/nghttp3.h>

namespace uvcpp {
namespace h3_detail {

/**
 * @brief `NGHTTP3_VERSION` 的字面量，形如 `"1.18.0"`。
 *
 * 做成函数而不是让宏漏出去，理由与 `quic_detail::version_literal()` 逐字相同：
 * nghttp3 只以宏的形式给出自己的版本（没有 `nghttp3_version()` 这样的函数），
 * 所以想拿一个"编进来的是哪个版本"的证据只能用宏；而这个标识符不该成为本库的
 * 编译契约 —— 它只是 nghttp3 的实现细节。
 */
inline const char* version_literal() { return NGHTTP3_VERSION; }

}  // namespace h3_detail
}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE

#endif  // SRC_HTTP3_UVCPP_H3_NGHTTP3_H
