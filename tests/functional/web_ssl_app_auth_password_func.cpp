/**
 * @file tests/functional/web_ssl_app_auth_password_func.cpp
 * @brief PBKDF2-HMAC-SHA256 口令哈希（issue #70 剩余，需要 OpenSSL）。
 *
 * 判据：
 * 1. 同一个口令两次 `hash()` 的盐不同、编码都能通过 `verify()`；
 * 2. 错口令、畸形编码（缺段、算法名不对、非 hex、迭代次数越界）一律 false；
 * 3. 迭代次数与编码头如实反映构造参数。
 *
 * 文件名同时命中 `web_ssl_`（OpenSSL 过滤器）与 `web_app_`（webapp 过滤器），
 * 所以只在两种模块都开着的树上存在。
 */

#include <uvcpp/uvcpp_define.h>

#include <cstdio>
#include <string>

#if UVCPP_WEBAPP_ENABLE && UVCPP_OPENSSL_ENABLE

#include <webapp/uvcpp_web_auth.h>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::printf("  [ ok ] %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("  [FAIL] %s\n", what.c_str());
  }
  std::fflush(stdout);
}

bool has_prefix(const std::string& s, const char* p) {
  return s.compare(0, std::char_traits<char>::length(p), p) == 0;
}

}  // namespace

int main() {
  std::printf("[auth_password] issue #70（PBKDF2）\n");

  uvcpp::uvcpp_pbkdf2_sha256 hasher(1000);
  check(hasher.iterations() == 1000, "iterations() 如实返回");

  const std::string h1 = hasher.hash("p@ssw0rd");
  const std::string h2 = hasher.hash("p@ssw0rd");
  check(!h1.empty() && !h2.empty(), "hash() 产出非空编码");
  check(has_prefix(h1, "pbkdf2-sha256$1000$"), "编码头带算法与迭代次数");
  check(h1 != h2, "两次哈希盐不同（编码逐字节不同）");

  check(hasher.verify("p@ssw0rd", h1), "verify 正确口令（h1）");
  check(hasher.verify("p@ssw0rd", h2), "verify 正确口令（h2）");
  check(!hasher.verify("wrong", h1), "错口令被拒");
  check(!hasher.verify("", h1), "空口令被拒");

  check(!hasher.verify("p@ssw0rd", ""), "空编码被拒");
  check(!hasher.verify("p@ssw0rd", "pbkdf2-sha256$1000$abcd"),
        "缺段编码被拒");
  check(!hasher.verify("p@ssw0rd",
                       "md5$1000$00112233445566778899aabbccddeeff$"
                       "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"),
        "算法名不对被拒");
  check(!hasher.verify("p@ssw0rd",
                       "pbkdf2-sha256$1000$zz$"
                       "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"),
        "非 hex 盐被拒");
  check(!hasher.verify("p@ssw0rd",
                       "pbkdf2-sha256$99999999$00112233445566778899aabbccddeeff$"
                       "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"),
        "迭代次数越界被拒（不做超长计算）");

  if (g_fail == 0) {
    std::printf("[auth_password] ALL PASS\n");
    return 0;
  }
  std::printf("[auth_password] %d check(s) failed\n", g_fail);
  return 1;
}

#else

int main() {
  std::printf("SKIP: 需要 webapp + OpenSSL\n");
  return 3;
}

#endif
