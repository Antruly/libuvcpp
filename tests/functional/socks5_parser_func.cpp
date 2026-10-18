/**
 * @file tests/functional/socks5_parser_func.cpp
 * @brief SOCKS5 线格式编解码（私有头白盒用例）。
 *
 * 这一层不碰套接字，所以用例也不碰：只喂字节、看返回。要覆盖的形状有四类 ——
 *
 * 1. **往返**：编出来的字节解回去必须逐字段相等（IPv4 / IPv6 / 域名）；
 * 2. **增量**：任何一个**截断前缀**都必须是 `NEED_MORE`，不能是 `ERROR` ——
 *    真连接上帧本来就是分几次到的，把"还没到齐"判成"这帧是坏的"会杀连接；
 * 3. **坏帧**：VER/RSV/ATYP/FRAG 非法的字节必须 `ERROR`，且不越界读；
 * 4. **边界**：域名 255 字节可用、256 字节编不出来；长度恰好等于
 *    `kMax*Bytes` 时仍接受，超过才拒绝。
 */

#include <uv.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "net/uvcpp_socks5_common.h"
#include "net/uvcpp_socks5_types.h"

using namespace uvcpp;
using namespace uvcpp::socks5_detail;

static int g_fails = 0;
static int g_checks = 0;

static const char* parse_status_name(parse_status st) {
  switch (st) {
    case parse_status::NEED_MORE: return "NEED_MORE";
    case parse_status::OK:        return "OK";
    case parse_status::MALFORMED:     return "ERROR";
  }
  return "?";
}

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      ++g_fails;                                                        \
    }                                                                   \
  } while (0)

#define CHECK_EQ_STATUS(expected, actual)                               \
  do {                                                                  \
    ++g_checks;                                                         \
    if ((expected) != (actual)) {                                       \
      std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__,         \
                  parse_status_name(expected), parse_status_name(actual));\
      ++g_fails;                                                        \
    }                                                                   \
  } while (0)

/** @brief 断言 \p buf 的每一个真前缀都解成 `NEED_MORE`（不含完整帧本身）。 */
template <typename ParseFn>
static void expect_all_prefixes_need_more(const std::string& buf,
                                          ParseFn parse) {
  for (std::size_t n = 0; n < buf.size(); ++n) {
    CHECK_EQ_STATUS(parse_status::NEED_MORE, parse(buf.data(), n));
  }
}

// =========================================================================
// classify_host
// =========================================================================
static void test_classify_host() {
  uvcpp_socks5_atyp atyp = uvcpp_socks5_atyp::DOMAIN_NAME;

  CHECK(classify_host("127.0.0.1", atyp));
  CHECK(atyp == uvcpp_socks5_atyp::IPV4);

  CHECK(classify_host("::1", atyp));
  CHECK(atyp == uvcpp_socks5_atyp::IPV6);

  CHECK(classify_host("2001:db8::1", atyp));
  CHECK(atyp == uvcpp_socks5_atyp::IPV6);

  CHECK(classify_host("example.com", atyp));
  CHECK(atyp == uvcpp_socks5_atyp::DOMAIN_NAME);

  CHECK(classify_host("a", atyp));
  CHECK(atyp == uvcpp_socks5_atyp::DOMAIN_NAME);

  // 含 ':' 却不是合法 IPv6 ⇒ 不是"当成域名"，是拒绝（域名里不可能有冒号）。
  CHECK(!classify_host("not:an:ip", atyp));

  // 空主机名没有合法的 ATYP。
  CHECK(!classify_host("", atyp));

  // 域名长度 255 可编，256 不可。
  CHECK(classify_host(std::string(255, 'a'), atyp));
  CHECK(atyp == uvcpp_socks5_atyp::DOMAIN_NAME);
  CHECK(!classify_host(std::string(256, 'a'), atyp));
}

// =========================================================================
// GREETING / METHOD SELECTION
// =========================================================================
static void test_greeting() {
  std::vector<std::uint8_t> methods;
  methods.push_back(0x00);
  const std::string g = encode_greeting(methods);
  CHECK(g.size() == 3);
  CHECK(static_cast<std::uint8_t>(g[0]) == 0x05);
  CHECK(static_cast<std::uint8_t>(g[1]) == 0x01);
  CHECK(static_cast<std::uint8_t>(g[2]) == 0x00);

  std::size_t consumed = 0;
  std::vector<std::uint8_t> out;
  expect_all_prefixes_need_more(g, [&](const char* p, std::size_t n) {
    return parse_greeting(p, n, consumed, out);
  });
  CHECK_EQ_STATUS(parse_status::OK,
                  parse_greeting(g.data(), g.size(), consumed, out));
  CHECK(consumed == g.size());
  CHECK(out.size() == 1 && out[0] == 0x00);

  // 两种方法都要能往返（服务端侧会原样读回来）。
  std::vector<std::uint8_t> two;
  two.push_back(0x00);
  two.push_back(0x02);
  const std::string g2 = encode_greeting(two);
  CHECK_EQ_STATUS(parse_status::OK,
                  parse_greeting(g2.data(), g2.size(), consumed, out));
  CHECK(out.size() == 2 && out[0] == 0x00 && out[1] == 0x02);

  // 255 种方法：帧长恰好 kMaxGreetingBytes，仍然接受。
  std::vector<std::uint8_t> many(255, 0x00);
  const std::string g3 = encode_greeting(many);
  CHECK(g3.size() == kMaxGreetingBytes);
  CHECK_EQ_STATUS(parse_status::OK,
                  parse_greeting(g3.data(), g3.size(), consumed, out));
  CHECK(out.size() == 255);

  // 超过上界 ⇒ ERROR（不是 NEED_MORE：再等也不会变短）。
  std::string huge = g3;
  huge.push_back('\0');
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_greeting(huge.data(), huge.size(), consumed, out));

  // 空方法表、错误版本都是坏的。
  const std::string empty_methods("\x05\x00", 2);
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_greeting(empty_methods.data(), empty_methods.size(),
                                 consumed, out));
  const std::string bad_ver("\x04\x01\x00", 3);
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_greeting(bad_ver.data(), bad_ver.size(), consumed, out));

  // encode_greeting 拒绝空表与超长表。
  CHECK(encode_greeting(std::vector<std::uint8_t>()).empty());
  CHECK(encode_greeting(std::vector<std::uint8_t>(256, 0x00)).empty());

  // 方法选择：0xFF 要映射到 NO_ACCEPTABLE。
  uvcpp_socks5_method sel = uvcpp_socks5_method::NO_AUTH;
  const std::string no_ok("\x05\xff", 2);
  CHECK_EQ_STATUS(parse_status::OK,
                  parse_method_selection(no_ok.data(), no_ok.size(), consumed,
                                         sel));
  CHECK(sel == uvcpp_socks5_method::NO_ACCEPTABLE);
  const std::string one("\x05\x00", 2);
  CHECK_EQ_STATUS(parse_status::OK,
                  parse_method_selection(one.data(), one.size(), consumed, sel));
  CHECK(sel == uvcpp_socks5_method::NO_AUTH);
  const std::string bad("\x04\x00", 2);
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_method_selection(bad.data(), bad.size(), consumed, sel));
  CHECK_EQ_STATUS(parse_status::NEED_MORE,
                  parse_method_selection(one.data(), 1, consumed, sel));
}

// =========================================================================
// RFC 1929 用户名密码
// =========================================================================
static void test_username_password() {
  const std::string req = encode_auth_request("user", "pass");
  const char expect[] = {'\x01', '\x04', 'u', 's', 'e', 'r',
                         '\x04', 'p', 'a', 's', 's'};
  CHECK(req.size() == sizeof(expect));
  CHECK(std::memcmp(req.data(), expect, sizeof(expect)) == 0);

  std::size_t consumed = 0;
  std::string user, pass;
  expect_all_prefixes_need_more(req, [&](const char* p, std::size_t n) {
    return parse_auth_request(p, n, consumed, user, pass);
  });
  CHECK_EQ_STATUS(parse_status::OK,
                  parse_auth_request(req.data(), req.size(), consumed, user,
                                     pass));
  CHECK(user == "user");
  CHECK(pass == "pass");

  // 空口令是合法的（RFC 1929 允许 PLEN=0）。
  const std::string empty_pass = encode_auth_request("u", "");
  CHECK_EQ_STATUS(parse_status::OK,
                  parse_auth_request(empty_pass.data(), empty_pass.size(),
                                     consumed, user, pass));
  CHECK(user == "u" && pass.empty());

  // 空用户名编不出来。
  CHECK(encode_auth_request("", "x").empty());
  CHECK(encode_auth_request(std::string(256, 'u'), "x").empty());
  CHECK(encode_auth_request("u", std::string(256, 'p')).empty());

  // 版本不是 0x01 ⇒ ERROR。
  std::string bad = req;
  bad[0] = '\x02';
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_auth_request(bad.data(), bad.size(), consumed, user,
                                     pass));

  // 应答：两字节，第二字节是状态。
  std::uint8_t status = 0xFF;
  const std::string ok("\x01\x00", 2);
  CHECK_EQ_STATUS(parse_status::OK,
                  parse_auth_response(ok.data(), ok.size(), consumed, status));
  CHECK(status == 0x00);
  const std::string deny("\x01\x01", 2);
  CHECK_EQ_STATUS(parse_status::OK,
                  parse_auth_response(deny.data(), deny.size(), consumed,
                                      status));
  CHECK(status == 0x01);
  CHECK_EQ_STATUS(parse_status::NEED_MORE,
                  parse_auth_response(ok.data(), 1, consumed, status));
  const std::string bad_ver("\x02\x00", 2);
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_auth_response(bad_ver.data(), bad_ver.size(), consumed,
                                      status));
}

// =========================================================================
// REQUEST / REPLY 三种 ATYP
// =========================================================================
static void check_endpoint_eq(const uvcpp_socks5_endpoint& a,
                              const uvcpp_socks5_endpoint& b) {
  CHECK(a.atyp == b.atyp);
  CHECK(a.host == b.host);
  CHECK(a.port == b.port);
}

static void roundtrip_request(uvcpp_socks5_command cmd,
                              const uvcpp_socks5_endpoint& target) {
  const std::string wire = encode_request(cmd, target);
  CHECK(!wire.empty());
  if (wire.empty()) return;

  std::size_t consumed = 0;
  uvcpp_socks5_command got_cmd = uvcpp_socks5_command::CONNECT;
  uvcpp_socks5_endpoint got;
  expect_all_prefixes_need_more(wire, [&](const char* p, std::size_t n) {
    return parse_request(p, n, consumed, got_cmd, got);
  });
  CHECK_EQ_STATUS(parse_status::OK, parse_request(wire.data(), wire.size(),
                                                  consumed, got_cmd, got));
  CHECK(consumed == wire.size());
  CHECK(got_cmd == cmd);
  check_endpoint_eq(got, target);
}

static void test_request() {
  roundtrip_request(uvcpp_socks5_command::CONNECT,
                    uvcpp_socks5_endpoint(uvcpp_socks5_atyp::IPV4,
                                          "127.0.0.1", 8080));
  roundtrip_request(uvcpp_socks5_command::BIND,
                    uvcpp_socks5_endpoint(uvcpp_socks5_atyp::IPV4,
                                          "0.0.0.0", 0));
  roundtrip_request(uvcpp_socks5_command::UDP_ASSOCIATE,
                    uvcpp_socks5_endpoint(uvcpp_socks5_atyp::IPV6, "::1",
                                          65535));
  roundtrip_request(uvcpp_socks5_command::CONNECT,
                    uvcpp_socks5_endpoint(uvcpp_socks5_atyp::DOMAIN_NAME,
                                          "example.com", 443));
  roundtrip_request(uvcpp_socks5_command::CONNECT,
                    uvcpp_socks5_endpoint(uvcpp_socks5_atyp::DOMAIN_NAME,
                                          std::string(255, 'a'), 80));

  // 256 字节域名编不出来。
  std::string out;
  CHECK(!append_address(out, uvcpp_socks5_endpoint(
                                 uvcpp_socks5_atyp::DOMAIN_NAME,
                                 std::string(256, 'a'), 80)));

  // 坏帧：VER、RSV、ATYP。
  const uvcpp_socks5_endpoint t(uvcpp_socks5_atyp::IPV4, "127.0.0.1", 80);
  const std::string good = encode_request(uvcpp_socks5_command::CONNECT, t);
  std::size_t consumed = 0;
  uvcpp_socks5_command cmd = uvcpp_socks5_command::CONNECT;
  uvcpp_socks5_endpoint got;

  std::string bad_ver = good;
  bad_ver[0] = '\x04';
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_request(bad_ver.data(), bad_ver.size(), consumed, cmd,
                                got));

  std::string bad_rsv = good;
  bad_rsv[2] = '\x01';
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_request(bad_rsv.data(), bad_rsv.size(), consumed, cmd,
                                got));

  std::string bad_atyp = good;
  bad_atyp[3] = '\x02';
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_request(bad_atyp.data(), bad_atyp.size(), consumed, cmd,
                                got));

  // 超长帧（262 是上界，263 直接判坏）。
  std::string too_long = good;
  too_long.resize(kMaxRequestBytes + 1, '\0');
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_request(too_long.data(), too_long.size(), consumed, cmd,
                                got));

  // `parse_address` 是同一份实现的布尔外壳：从 ATYP 起头。
  std::size_t off = 0;
  CHECK(parse_address(good.data() + 3, good.size() - 3, off, got));
  CHECK(off == good.size() - 3);
  CHECK(!parse_address(good.data() + 3, 4, off, got));  // 截断
}

static void test_reply() {
  const uvcpp_socks5_endpoint bound(uvcpp_socks5_atyp::IPV4, "192.0.2.1",
                                    1234);
  for (int rep = 0; rep <= 8; ++rep) {
    const uvcpp_socks5_reply r = static_cast<uvcpp_socks5_reply>(rep);
    const std::string wire = encode_reply(r, bound);
    CHECK(!wire.empty());
    std::size_t consumed = 0;
    uvcpp_socks5_reply got_r = uvcpp_socks5_reply::GENERAL_FAILURE;
    uvcpp_socks5_endpoint got_b;
    expect_all_prefixes_need_more(wire, [&](const char* p, std::size_t n) {
      return parse_reply(p, n, consumed, got_r, got_b);
    });
    CHECK_EQ_STATUS(parse_status::OK,
                    parse_reply(wire.data(), wire.size(), consumed, got_r,
                                got_b));
    CHECK(got_r == r);
    check_endpoint_eq(got_b, bound);
  }

  const std::string wire = encode_reply(uvcpp_socks5_reply::SUCCEEDED, bound);
  std::size_t consumed = 0;
  uvcpp_socks5_reply got_r = uvcpp_socks5_reply::GENERAL_FAILURE;
  uvcpp_socks5_endpoint got_b;

  std::string bad_ver = wire;
  bad_ver[0] = '\x04';
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_reply(bad_ver.data(), bad_ver.size(), consumed, got_r,
                              got_b));

  std::string bad_rsv = wire;
  bad_rsv[2] = '\x07';
  CHECK_EQ_STATUS(parse_status::MALFORMED,
                  parse_reply(bad_rsv.data(), bad_rsv.size(), consumed, got_r,
                              got_b));
}

// =========================================================================
// UDP 数据报
// =========================================================================
static void test_udp_datagram() {
  const uvcpp_socks5_endpoint dst(uvcpp_socks5_atyp::IPV4, "10.1.2.3", 53);
  const char payload[] = "dns-query";
  std::string wire;
  CHECK(encode_udp_datagram(wire, dst, payload, sizeof(payload) - 1));

  // 头三字节：RSV(2)=0 + FRAG=0。
  CHECK(wire.size() == 3 + 1 + 4 + 2 + sizeof(payload) - 1);
  CHECK(wire[0] == '\0' && wire[1] == '\0' && wire[2] == '\0');

  uvcpp_socks5_endpoint from;
  std::size_t payload_off = 0;
  // 截断到载荷之前：头越短越要 NEED_MORE，不能是 ERROR。
  expect_all_prefixes_need_more(
      wire.substr(0, 3 + 1 + 4 + 2), [&](const char* p, std::size_t n) {
        return decode_udp_datagram(p, n, from, payload_off);
      });

  CHECK_EQ_STATUS(parse_status::OK, decode_udp_datagram(wire.data(),
                                                        wire.size(), from,
                                                        payload_off));
  check_endpoint_eq(from, dst);
  CHECK(payload_off == 3 + 1 + 4 + 2);
  CHECK(wire.size() - payload_off == sizeof(payload) - 1);
  CHECK(std::memcmp(wire.data() + payload_off, payload,
                    sizeof(payload) - 1) == 0);

  // 没有载荷也是合法的（空数据报）。
  std::string empty_wire;
  CHECK(encode_udp_datagram(empty_wire, dst, nullptr, 0));
  CHECK_EQ_STATUS(parse_status::OK, decode_udp_datagram(empty_wire.data(),
                                                        empty_wire.size(),
                                                        from, payload_off));
  CHECK(payload_off == empty_wire.size());

  // RSV 非 0 / FRAG 非 0 ⇒ ERROR（本实现不做分片重组）。
  std::string bad_rsv = wire;
  bad_rsv[1] = '\x01';
  CHECK_EQ_STATUS(parse_status::MALFORMED, decode_udp_datagram(
                                           bad_rsv.data(), bad_rsv.size(), from,
                                           payload_off));
  std::string frag = wire;
  frag[2] = '\x01';
  CHECK_EQ_STATUS(parse_status::MALFORMED, decode_udp_datagram(
                                           frag.data(), frag.size(), from,
                                           payload_off));

  // 域名与 IPv6 两种 ATYP 也要能往返。
  const uvcpp_socks5_endpoint dom(uvcpp_socks5_atyp::DOMAIN_NAME, "example.org",
                                  5353);
  std::string wire6;
  CHECK(encode_udp_datagram(wire6, dom, payload, sizeof(payload) - 1));
  CHECK_EQ_STATUS(parse_status::OK, decode_udp_datagram(wire6.data(),
                                                        wire6.size(), from,
                                                        payload_off));
  check_endpoint_eq(from, dom);

  const uvcpp_socks5_endpoint v6(uvcpp_socks5_atyp::IPV6, "::1", 9);
  std::string wirev6;
  CHECK(encode_udp_datagram(wirev6, v6, payload, sizeof(payload) - 1));
  CHECK_EQ_STATUS(parse_status::OK, decode_udp_datagram(wirev6.data(),
                                                        wirev6.size(), from,
                                                        payload_off));
  check_endpoint_eq(from, v6);
}

// =========================================================================
// 错误映射
// =========================================================================
static void test_error_mapping() {
  CHECK(reply_to_error(uvcpp_socks5_reply::SUCCEEDED) == 0);
  CHECK(reply_to_error(uvcpp_socks5_reply::CONNECTION_REFUSED) ==
        UV_ECONNREFUSED);
  CHECK(reply_to_error(uvcpp_socks5_reply::HOST_UNREACHABLE) ==
        UV_EHOSTUNREACH);
  CHECK(reply_to_error(uvcpp_socks5_reply::NOT_ALLOWED) == UV_EACCES);
  CHECK(reply_to_error(uvcpp_socks5_reply::TTL_EXPIRED) == UV_ETIMEDOUT);
  CHECK(reply_to_error(uvcpp_socks5_reply::COMMAND_UNSUPPORTED) == UV_ENOTSUP);
  // 保留值按一般失败处理。
  CHECK(reply_to_error(static_cast<uvcpp_socks5_reply>(0x7f)) == UV_EPROTO);

  CHECK(error_to_reply(0) == uvcpp_socks5_reply::SUCCEEDED);
  CHECK(error_to_reply(UV_ECONNREFUSED) ==
        uvcpp_socks5_reply::CONNECTION_REFUSED);
  CHECK(error_to_reply(UV_ENETUNREACH) == uvcpp_socks5_reply::NETWORK_UNREACHABLE);
  CHECK(error_to_reply(UV_EHOSTUNREACH) == uvcpp_socks5_reply::HOST_UNREACHABLE);
  CHECK(error_to_reply(UV_ETIMEDOUT) == uvcpp_socks5_reply::HOST_UNREACHABLE);
  CHECK(error_to_reply(UV_EACCES) == uvcpp_socks5_reply::NOT_ALLOWED);
  CHECK(error_to_reply(UV_EAFNOSUPPORT) == uvcpp_socks5_reply::ATYP_UNSUPPORTED);
  CHECK(error_to_reply(UV_ENOTSUP) == uvcpp_socks5_reply::COMMAND_UNSUPPORTED);
  CHECK(error_to_reply(UV_EIO) == uvcpp_socks5_reply::GENERAL_FAILURE);

  // 每个 REP 都有一句人话，且两两不同（重复的话日志里就分不清了）。
  std::vector<std::string> texts;
  for (int rep = 0; rep <= 8; ++rep) {
    const char* s = reply_string(static_cast<uvcpp_socks5_reply>(rep));
    CHECK(s != nullptr && s[0] != '\0');
    for (std::size_t i = 0; i < texts.size(); ++i) CHECK(texts[i] != s);
    texts.push_back(s);
  }
}

int main() {
  test_classify_host();
  test_greeting();
  test_username_password();
  test_request();
  test_reply();
  test_udp_datagram();
  test_error_mapping();

  if (g_fails == 0) {
    std::printf("ALL OK (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("%d/%d checks FAILED\n", g_fails, g_checks);
  return 1;
}
