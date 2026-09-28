/**
 * @file src/quic/uvcpp_quic_ngtcp2.cpp
 * @brief `src/quic/` 里唯一 include 私有头（也就是唯一看见 ngtcp2 的 TU）。
 * @author zhuweiye
 * @version 1.4.1
 *
 * 这里只放**"后端自述"**那三个函数：编进来的是哪个 ngtcp2、它的错误码叫什么、
 * 它的 crypto 后端在不在。QUIC 的状态机一行都没有 —— 见 `uvcpp_quic_common.h`
 * 开头那段"这一版交付的是什么"。
 */

#include "quic/uvcpp_quic_ngtcp2.h"

#if UVCPP_QUIC_ENABLE

#include <string>

#include "quic/uvcpp_quic_common.h"

namespace uvcpp {

std::string quic_ngtcp2_version_string() {
  // 只有编译期的一半 —— 拿不到比宏更多的东西，ngtcp2 没有版本**函数**。
  // 所以这个函数**不是**链接证据，这一点在头文件的注释里写死了，别把它当门禁。
  return std::string(quic_detail::version_literal());
}

std::string quic_error_string(int code) {
  // `ngtcp2_strerror` 住在 `ngtcp2_err.c` 里，是 libngtcp2 导出的符号。
  // 这一句是本文件里**第一处真调进 ngtcp2** 的地方 —— 少链了 ngtcp2_static,
  // 这个 TU 就编不出可执行文件（未定义符号），用例在链接期红,而不是运行期。
  return std::string(ngtcp2_strerror(code));
}

int quic_crypto_backend_init() {
  // 第二处真调用，落在**另一个**目标（ngtcp2_crypto_ossl_static）上 —— 为什么
  // 这个目标比前一个更需要被盯住（"树不对时它根本不会被建出来"），见头文件里
  // 这个函数的注释。
  return ngtcp2_crypto_ossl_init();
}

void quic_crypto_backend_free() {
  ngtcp2_crypto_ossl_free();
}

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE
