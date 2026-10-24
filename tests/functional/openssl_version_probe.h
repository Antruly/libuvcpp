/**
 * @file tests/functional/openssl_version_probe.h
 * @brief `OpenSSL_version()` 的版本号取法，1.1.1 与 3.x 都编得过。
 *
 * `OPENSSL_VERSION_STRING` 是 **3.0 才有的索引**（`OPENSSL_VERSION`=0、
 * `OPENSSL_ENGINES_DIR`=5 之后就断档了）；Win7 线用 1.1.1w 编测试时它是
 * 未声明标识符，`web_ssl_*_func.cpp` 十处启动横幅全部编不过（C2065）。
 *
 * 1.1.1 里同一含义的是 `OPENSSL_VERSION`（=0）。两者返回的文本形状不同：
 * 3.x 的 `OPENSSL_VERSION_STRING` 是 "3.0.11"，而 `OPENSSL_VERSION` 在**两个
 * 版本里**都是 "OpenSSL 3.0.11 19 Sep 2023" / "OpenSSL 1.1.1w 11 Sep 2023"
 * （自带 "OpenSSL " 前缀）。所以这里统一去掉前缀，调用点照旧只写
 * `"OpenSSL " << uvcpp_test::openssl_version_text()`。
 */
#ifndef TESTS_FUNCTIONAL_OPENSSL_VERSION_PROBE_H
#define TESTS_FUNCTIONAL_OPENSSL_VERSION_PROBE_H

#include <cstring>

#include <openssl/crypto.h>

namespace uvcpp_test {

inline const char* openssl_version_text() {
#if defined(OPENSSL_VERSION_STRING)
  return OpenSSL_version(OPENSSL_VERSION_STRING);
#else
  static const char kPrefix[] = "OpenSSL ";
  const char* text = OpenSSL_version(OPENSSL_VERSION);
  if (text && std::strncmp(text, kPrefix, sizeof(kPrefix) - 1) == 0) {
    return text + sizeof(kPrefix) - 1;
  }
  return text;
#endif
}

}  // namespace uvcpp_test

#endif  // TESTS_FUNCTIONAL_OPENSSL_VERSION_PROBE_H
