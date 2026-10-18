/**
 * @file tests/functional/redis_resp_func.cpp
 * @brief RESP2/RESP3 的增量解析与命令编码 —— 纯字节级用例，不碰网络。
 *
 * 这一层刻意测得很细：客户端那层一旦把应答切错、或者把"还不够"错判成"坏了"，
 * 症状是"偶尔少一条应答"或"连接莫名其妙断掉"，那两种都要靠真实服务器才复现，
 * 而归因会指向网络。把这些判据钉在解析器上，客户端用例就只需要管时序。
 */

#include <redis/uvcpp_redis.h>
#include <redis/uvcpp_redis_resp.h>

#include <uv.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++g_fail;
    std::cout << "[redis_resp] FAIL: " << what << "\n";
  }
}

bool feed_one(uvcpp::uvcpp_redis_parser& p, const std::string& data,
              uvcpp::uvcpp_redis_value& out, bool* push = nullptr) {
  std::size_t off = 0;
  const uvcpp::uvcpp_redis_parser::result r =
      p.next(data.data(), data.size(), off, out, push);
  if (r != uvcpp::uvcpp_redis_parser::DONE) return false;
  return off == data.size();
}

// =========================================================================
// RESP2
// =========================================================================
void test_resp2_types() {
  uvcpp::uvcpp_redis_parser p;
  uvcpp::uvcpp_redis_value v;

  check(feed_one(p, "+OK\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::STATUS &&
            v.as_string() == "OK",
        "STATUS");

  check(feed_one(p, "-ERR unknown command 'foo'\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::ERROR_REPLY && v.is_error() &&
            v.as_string() == "ERR unknown command 'foo'",
        "ERROR");

  check(feed_one(p, ":1000\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::INTEGER &&
            v.as_integer() == 1000,
        "INTEGER");
  check(feed_one(p, ":-1\r\n", v) && v.as_integer() == -1, "INTEGER negative");

  check(feed_one(p, "$5\r\nhello\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::BULK &&
            v.as_string() == "hello",
        "BULK");

  check(feed_one(p, "$-1\r\n", v) && v.is_null(), "BULK null");

  check(feed_one(p, "*3\r\n$3\r\nfoo\r\n:42\r\n$-1\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::ARRAY && v.size() == 3 &&
            v.at(0).as_string() == "foo" && v.at(1).as_integer() == 42 &&
            v.at(2).is_null(),
        "ARRAY");

  check(feed_one(p, "*-1\r\n", v) && v.is_null(), "ARRAY null");

  // 空 bulk 与"空"是两件事：`$0\r\n\r\n` 是一个长度为零的字符串。
  check(feed_one(p, "$0\r\n\r\n", v) && v.type() == uvcpp::uvcpp_redis_type::BULK &&
            v.as_string().empty() && !v.is_null(),
        "BULK empty");

  // 二进制安全：`\0` 与 CRLF 都只是字节。
  const std::string bin("a\0b\r\nc", 7);
  std::string frame = "$7\r\n" + bin + "\r\n";
  check(feed_one(p, frame, v) && v.as_string() == bin, "BULK binary safe");

  // 越界/类型不符一律给零值，不抛、不崩。
  uvcpp::uvcpp_redis_value n = v.at(99);
  check(n.is_null(), "at() out of range gives null");
  check(v.as_integer() == 0 && v.as_double() == 0.0 && !v.as_bool(),
        "wrong-type accessors give zero");
}

// =========================================================================
// RESP3
// =========================================================================
void test_resp3_types() {
  uvcpp::uvcpp_redis_parser p;
  uvcpp::uvcpp_redis_value v;

  check(feed_one(p, "_\r\n", v) && v.is_null(), "RESP3 null");

  check(feed_one(p, "#t\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::BOOLEAN && v.as_bool(),
        "boolean true");
  check(feed_one(p, "#f\r\n", v) && !v.as_bool(), "boolean false");

  check(feed_one(p, ",3.5\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::DOUBLE &&
            v.as_double() == 3.5,
        "double");
  check(feed_one(p, ",inf\r\n", v) && v.as_double() > 1e308, "double inf");

  check(feed_one(p, "(123456789012345678901234567890\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::BULK &&
            v.as_string() == "123456789012345678901234567890",
        "big number as bulk");

  check(feed_one(p, "~2\r\n$1\r\na\r\n$1\r\nb\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::SET && v.size() == 2,
        "set");

  check(feed_one(p, "%2\r\n$1\r\na\r\n:1\r\n$1\r\nb\r\n:2\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::MAP && v.size() == 2 &&
            v.items().size() == 2 &&
            v.items()[0].first.as_string() == "a" &&
            v.items()[0].second.as_integer() == 1 &&
            v.items()[1].first.as_string() == "b" &&
            v.items()[1].second.as_integer() == 2,
        "map");

  check(feed_one(p, "!21\r\nSYNTAX invalid syntax\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::ERROR_REPLY &&
            v.as_string() == "SYNTAX invalid syntax",
        "blob error");

  // 逐字字符串的 `txt:` 标签要被剥掉，剩下的才是负载。
  check(feed_one(p, "=15\r\ntxt:Some string\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::BULK &&
            v.as_string() == "Some string",
        "verbatim string");

  // 属性帧只是元数据，后面那个值才是应答。
  check(feed_one(p, "|1\r\n$3\r\nttl\r\n:10\r\n+OK\r\n", v) &&
            v.type() == uvcpp::uvcpp_redis_type::STATUS &&
            v.as_string() == "OK",
        "attribute skipped");

  // push 帧要能被认出来 —— 客户端靠它把消息与命令应答分开。
  bool push = false;
  uvcpp::uvcpp_redis_value m;
  check(feed_one(p, ">3\r\n$7\r\nmessage\r\n$4\r\nnews\r\n$5\r\nhello\r\n", m,
                 &push) &&
            push && m.size() == 3 && m.at(0).as_string() == "message",
        "push frame");

  // 普通数组不是 push。
  push = true;
  check(feed_one(p, "*1\r\n+OK\r\n", m, &push) && !push, "array is not push");
}

// =========================================================================
// 增量：分片到达
// =========================================================================
void test_fragmentation() {
  const std::string frame = "*2\r\n$3\r\nfoo\r\n$5\r\nhello\r\n";
  uvcpp::uvcpp_redis_parser p;

  // 逐字节喂：每一个前缀都必须是 NEED_MORE，而且 `off` 不许动。
  for (std::size_t cut = 0; cut < frame.size(); ++cut) {
    std::size_t off = 0;
    uvcpp::uvcpp_redis_value v;
    const uvcpp::uvcpp_redis_parser::result r =
        p.next(frame.data(), cut, off, v);
    check(r == uvcpp::uvcpp_redis_parser::NEED_MORE,
          "prefix " + std::to_string(cut) + " is NEED_MORE");
    check(off == 0, "prefix " + std::to_string(cut) + " leaves off alone");
  }

  std::size_t off = 0;
  uvcpp::uvcpp_redis_value v;
  const uvcpp::uvcpp_redis_parser::result r =
      p.next(frame.data(), frame.size(), off, v);
  check(r == uvcpp::uvcpp_redis_parser::DONE && off == frame.size() &&
            v.size() == 2 && v.at(1).as_string() == "hello",
        "complete frame after partial prefixes");

  // 粘包：一条缓冲里两个值，`off` 必须停在第一个的末尾。
  const std::string two = "+OK\r\n:7\r\n";
  std::size_t o = 0;
  uvcpp::uvcpp_redis_value a;
  uvcpp::uvcpp_redis_value b;
  check(p.next(two.data(), two.size(), o, a) ==
            uvcpp::uvcpp_redis_parser::DONE &&
            o == 5 && a.as_string() == "OK",
        "first of two values, off after it");
  check(p.next(two.data(), two.size(), o, b) ==
            uvcpp::uvcpp_redis_parser::DONE &&
            o == two.size() && b.as_integer() == 7,
        "second of two values");
}

// =========================================================================
// 坏数据
// =========================================================================
bool is_bad(const std::string& data, void (*tweak)(uvcpp::uvcpp_redis_parser&) = nullptr) {
  uvcpp::uvcpp_redis_parser p;
  if (tweak != nullptr) tweak(p);
  std::size_t off = 0;
  uvcpp::uvcpp_redis_value v;
  return p.next(data.data(), data.size(), off, v) ==
         uvcpp::uvcpp_redis_parser::BAD;
}

void shrink_bulk(uvcpp::uvcpp_redis_parser& p) { p.max_bulk = 4; }
void shrink_elems(uvcpp::uvcpp_redis_parser& p) { p.max_elements = 2; }

void test_malformed() {
  check(is_bad("?nonsense\r\n"), "unknown type byte");
  check(is_bad("$abc\r\n"), "bulk length not an integer");
  check(is_bad("$-2\r\n"), "negative bulk length");
  check(is_bad("*-2\r\n"), "negative array length");
  check(is_bad("*abc\r\n"), "array length not an integer");
  check(is_bad("*?\r\n"), "streamed aggregate rejected");
  check(is_bad("$?\r\n"), "streamed bulk rejected");
  check(is_bad("$3\r\nabcd\r\n"), "bulk missing CRLF after data");
  check(is_bad(":x\r\n"), "integer not an integer");
  check(is_bad(",abc\r\n"), "double not a double");
  check(is_bad("#x\r\n"), "boolean neither t nor f");
  // `_` 后面紧跟的 `x` 是下一个值的开头，不该判坏，只是这一次没有消费完。
  check(!is_bad("_\r\nx"), "null followed by next value is not an error");
  check(is_bad("$10\r\nshort\r\n", shrink_bulk), "bulk over the limit");
  check(is_bad("*5\r\n:1\r\n:2\r\n:3\r\n:4\r\n:5\r\n", shrink_elems),
        "aggregate over the limit");

  // 深度：64 层是上限，65 层必须判坏。
  std::string deep;
  for (int i = 0; i < 65; ++i) deep += "*1\r\n";
  deep += "+OK\r\n";
  check(is_bad(deep), "nesting over the limit");
}

// =========================================================================
// 编码
// =========================================================================
void test_encode() {
  std::string out;
  uvcpp::uvcpp_redis_parser::encode_command({"SET", "k", "v"}, out);
  check(out == "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n",
        "encode_command SET k v");

  std::string bin("a\0b", 3);
  out.clear();
  uvcpp::uvcpp_redis_parser::encode_command({bin}, out);
  check(out == std::string("*1\r\n$3\r\na\0b\r\n", 13), "encode binary arg");

  out.clear();
  uvcpp::uvcpp_redis_parser::encode_command({"PING"}, out);
  check(out == "*1\r\n$4\r\nPING\r\n", "encode single arg");

  std::vector<std::vector<std::string>> cmds;
  cmds.push_back(std::vector<std::string>{"PING"});
  cmds.push_back(std::vector<std::string>{"ECHO", "hi"});
  out.clear();
  uvcpp::uvcpp_redis_parser::encode_commands(cmds, out);
  check(out == "*1\r\n$4\r\nPING\r\n*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n",
        "encode_commands pipeline");
}

}  // namespace

int main() {
  test_resp2_types();
  test_resp3_types();
  test_fragmentation();
  test_malformed();
  test_encode();

  if (g_fail != 0) {
    std::cout << "[redis_resp] " << g_fail << " check(s) FAILED\n";
    return 1;
  }
  std::cout << "[redis_resp] ALL PASSED\n";
  return 0;
}
