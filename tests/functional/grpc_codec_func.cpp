/**
 * @file tests/functional/grpc_codec_func.cpp
 * @brief `src/grpc/uvcpp_grpc.h` 的编解码：**判据全部落在确定的字节上**。
 *
 * 为什么文件名里没有 `web_`/`h2_` 这些前缀：这一半（帧 + varint + 字段）**不依赖
 * 任何模块开关**，任何配置的树上都在编、都在装（见 `CMakeLists.txt` 里 gRPC 那
 * 一段：只摘服务端那一半）。于是按仓库那套"关掉某模块就把它的用例从目标列表里
 * 摘掉"的约定，这条**没有**对应的过滤器 —— 它在每一棵树上都必须注册、都必须跑。
 *
 * 这条路上的错误有个共同特征：**本地全绿、对端全错**。帧长度是大端，写成小端
 * 在 x86 上"看着能跑"（自己编自己解），只有对端解出来是 256 倍；负的 `int32`
 * 要符号扩展到 64 位，漏了这一步自己编自己解**照样往返成功**（本模块的读侧会
 * 截断到 32 位），而对端的 protobuf 解出一个正数。所以判据不能是"往返相等"，
 * 必须是**逐字节的十六进制**（下面每一处 `hex(...)` 的比较），往返只是补充。
 *
 * 反例的形状（写下来，免得后来者把判据换成更宽松的那种）：
 *   - 把"负 int32"那一条换成 `get(put(-12345)) == -12345` 就**不再是判据** ——
 *     把符号扩展删掉、改成按 `uint32_t` 编 5 字节，那个往返仍然成立。真正的锚点
 *     是 `-1` 那 11 个字节的**逐字节**比较（`08` + `FF×9` + `01`）。
 *   - 把"跳过未知字段"那一条删掉，只看第一个字段的实现会照常通过前几条 ——
 *     所以那条判据必须把要找的字段**放在后面**，前面挡一个长度前缀字段。
 */

#include <iostream>
#include <string>

#include <uv.h>

#include <grpc/uvcpp_grpc.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (cond) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void info(const std::string& what) {
  std::cout << "  [info] " << what << std::endl;
}

/// 小写、无分隔的十六进制。判据落在字节上时，这个比"长度对不对"有信息量得多：
/// 大端写成了小端、符号扩展漏了，都会在这里**原形毕露**。
std::string hex(const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  std::string        out;
  out.reserve(s.size() * 2);
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    out += kHex[c >> 4];
    out += kHex[c & 0x0F];
  }
  return out;
}

/// 从一段字节造 `std::string`（**带 NUL**）：帧的载荷不是 C 串，判据里必须有
/// 含 NUL 的那一条，否则"按 `strlen` 走"的实现能骗过全部往返比较。
std::string bytes_of(const char* p, size_t n) { return std::string(p, n); }

// =========================================================================
// 第 1 轮：帧前缀
// =========================================================================

void test_frame_prefix() {
  std::cout << "[1] 帧前缀：5 字节、大端" << std::endl;

  std::string out;
  const int   rc = grpc_frame_encode("", out);
  check(rc == 0, "空消息编码成功");
  check(hex(out) == "0000000000", "空消息 = 00 00 00 00 00（逐字节）");
  check(out.size() == GRPC_FRAME_HEADER_SIZE, "正好 GRPC_FRAME_HEADER_SIZE 个字节");

  // 长度写在大端：300 = 0x0000012C，前面还有那个压缩标志字节。写成小端的话
  // 这里是 "002c010000"。
  out.clear();
  const std::string m300(300, 'x');
  grpc_frame_encode(m300, out);
  check(hex(out.substr(0, GRPC_FRAME_HEADER_SIZE)) == "000000012c",
        "300 字节的前缀是 00 00 00 01 2C（**大端**，不是本机序）");
  check(out.size() == GRPC_FRAME_HEADER_SIZE + 300, "整帧 = 前缀 + 300");

  // 追加语义：连着编两帧，缓冲区里是两帧首尾相接。
  out.clear();
  grpc_frame_encode("ab", out);
  grpc_frame_encode("c", out);
  check(hex(out) == "00000000026162" "000000000163",
        "两次编码是**追加上去**的（两帧首尾相接）");
}

// =========================================================================
// 第 2 轮：解帧（含 NUL 的载荷、两种"还没收全"）
// =========================================================================

void test_frame_decode() {
  std::cout << "[2] 解帧：含 NUL 的载荷、UV_ENODATA、恰好一帧" << std::endl;

  std::string payload;
  for (int i = 0; i < 256; ++i) payload += static_cast<char>(i);  // 含着 \0
  std::string frame;
  grpc_frame_encode(payload, frame);

  std::string got;
  size_t      used = 0;
  check(grpc_frame_decode(frame.data(), frame.size(), got, used) == 0, "解出来");
  check(got == payload, "含 NUL 的 256 字节载荷逐字节往返（帧按长度走，不按 C 串）");
  check(used == frame.size(), "consumed 是整帧长度（前缀 + 载荷）");

  // 少一个字节：是"再读一段"，不是格式错。
  size_t    used2 = 999;
  std::string e2;
  const int rc2 = grpc_frame_decode(frame.data(), frame.size() - 1, got, used2, &e2);
  check(rc2 == UV_ENODATA, "载荷少一个字节 → UV_ENODATA（等下一段，不是格式错）");
  info("少一个字节时 err = " + e2);

  // 前缀本身还没收全。
  const int rc3 = grpc_frame_decode(frame.data(), 3, got, used2);
  check(rc3 == UV_ENODATA, "前缀只收到 3 个字节 → UV_ENODATA");

  // 压缩标志非 0：本库不做消息压缩，明说比"照解"强。
  std::string compressed = frame;
  compressed[0] = '\x01';
  const int rc4 = grpc_frame_decode(compressed.data(), compressed.size(), got, used2);
  check(rc4 == UV_EINVAL, "压缩标志 0x01 → UV_EINVAL");

  // 恰好一帧：多一个字节都不行。
  std::string extra = frame;
  extra += 'x';
  got = "sentinel";
  const int rc5 = grpc_frame_decode_exact(extra.data(), extra.size(), got);
  check(rc5 == UV_EINVAL, "帧后多一个字节 → UV_EINVAL（unary 的请求体只有一帧）");
  check(got.empty(), "失败时出参被清空（不留下「解了一半」的字节）");
  check(grpc_frame_decode_exact(frame.data(), frame.size(), got) == 0, "恰好一帧 → 0");
  check(got == payload, "恰好一帧时载荷正确");
}

// =========================================================================
// 第 3 轮：长度前缀上限（**不许分配**）
// =========================================================================

void test_frame_length_cap() {
  std::cout << "[3] 长度前缀报了个大数：拒掉，且不分配" << std::endl;

  // 前缀自洽地说"载荷有 2 GiB"，而后面一个字节都没有。这里若先分配再等载荷，
  // 一个 5 字节的报文就能让服务端吃掉 2 GiB —— 所以判据是**错误码**，
  // 而且调用的那一刻手里只有 5 个字节。
  const std::string bogus = bytes_of("\x00\x7f\xff\xff\xff", 5);
  std::string       got   = "sentinel";
  size_t            used  = 0;
  std::string       err;
  const int rc = grpc_frame_decode(bogus.data(), bogus.size(), got, used, &err);
  check(rc == UV_EMSGSIZE, "前缀报 2 GiB → UV_EMSGSIZE");
  check(got.empty() && used == 0, "一个字节都没拷出来、库内进度保持 0");
  info("err = " + err);

  // 上限是可配的：把上限调到 8，前缀报 4 就过得去了。
  std::string small;
  grpc_frame_encode("abcd", small);
  check(grpc_frame_decode(small.data(), small.size(), got, used, nullptr, 4u) == 0,
        "上限配成 4 时，4 字节的载荷正常解出（上限不是写死的）");
  check(grpc_frame_decode(small.data(), small.size(), got, used, nullptr, 3u) ==
            UV_EMSGSIZE,
        "上限配成 3 时同一个报文被拒");
}

// =========================================================================
// 第 4 轮：流式——用 consumed 推进游标
// =========================================================================

void test_streaming_decode() {
  std::cout << "[4] 流式：一次解多帧" << std::endl;

  std::string buf;
  grpc_frame_encode("ab", buf);
  grpc_frame_encode("", buf);
  grpc_frame_encode("cde", buf);

  std::string got;
  size_t      used = 0;
  std::string acc;
  size_t      pos = 0;
  int         n   = 0;
  while (pos < buf.size()) {
    if (grpc_frame_decode(buf.data() + pos, buf.size() - pos, got, used) != 0) break;
    acc += got;
    acc += "|";
    pos += used;
    ++n;
  }
  check(n == 3, "三帧都被解出来（含中间那个空帧）");
  check(pos == buf.size(), "游标正好走到头（consumed 的定义与实现一致）");
  check(acc == "ab||cde|", "顺序与内容都对");
}

// =========================================================================
// 第 5 轮：varint 对照表
// =========================================================================

struct VarintCase {
  uint64_t    value;
  const char* hex;
};

void test_varint_table() {
  std::cout << "[5] varint：对照表" << std::endl;

  // 取自 protobuf 编码规范的边界值：7 位一组的分界（127/128、16383/16384）、
  // 32 位满值、64 位满值。逐字节比较，不是"编了再解"。
  const VarintCase kCases[] = {
      {0ull, "00"},
      {1ull, "01"},
      {127ull, "7f"},
      {128ull, "8001"},                          // 跨过 7 位边界
      {300ull, "ac02"},
      {16383ull, "ff7f"},
      {16384ull, "808001"},                      // 跨过 14 位边界
      {0xFFFFFFFFull, "ffffffff0f"},             // 32 位满值 → 5 字节
      {0xFFFFFFFFFFFFFFFFull, "ffffffffffffffffff01"},  // 64 位满值 → 10 字节
  };

  for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
    std::string enc;
    grpc_varint_encode(enc, kCases[i].value);
    const std::string want(kCases[i].hex);
    check(hex(enc) == want, "varint(" + std::to_string(kCases[i].value) + ") = " + want);

    uint64_t    back = 0;
    size_t      used = 0;
    const int   rc   = grpc_varint_decode(enc.data(), enc.size(), back, used);
    check(rc == 0 && back == kCases[i].value && used == enc.size(),
          "  解回来（值与字节数都对）");
  }
}

// =========================================================================
// 第 6 轮：varint 的两种拒绝
// =========================================================================

void test_varint_rejects() {
  std::cout << "[6] varint：越界与截断" << std::endl;

  uint64_t  v    = 0;
  size_t    used = 0;
  std::string err;

  // 第 10 个字节还挂着续位：64 位装不下 ⇒ 格式错（不是"继续等"）。
  const std::string ten_ff(10, '\xff');
  check(grpc_varint_decode(ten_ff.data(), ten_ff.size(), v, used, &err) == UV_EINVAL,
        "10 个 0xFF → UV_EINVAL（第 10 个字节仍挂着续位）");
  info("err = " + err);

  const std::string ten_80(10, '\x80');
  check(grpc_varint_decode(ten_80.data(), ten_80.size(), v, used, &err) == UV_EINVAL,
        "10 个 0x80 → UV_EINVAL");

  // 第 10 个字节只带最高那一位，所以它只能是 0x00 / 0x01。
  std::string nine_ff(9, '\xff');
  nine_ff += '\x02';
  check(grpc_varint_decode(nine_ff.data(), nine_ff.size(), v, used, &err) == UV_EINVAL,
        "第 10 个字节 = 0x02 → UV_EINVAL（越界）");
  info("err = " + err);

  // 截断：手里的字节不够 10 个，而且还没读完 ⇒ 该等，不该判错。
  const std::string trunc = bytes_of("\x80", 1);
  check(grpc_varint_decode(trunc.data(), trunc.size(), v, used, &err) == UV_ENODATA,
        "一个孤零零的 0x80 → UV_ENODATA（再给几个字节可能就读完了）");

  // 11 个字节：第 10 个已经把 64 位用满，第 11 个根本轮不到。
  const std::string eleven(11, '\x80');
  check(grpc_varint_decode(eleven.data(), eleven.size(), v, used, &err) == UV_EINVAL,
        "11 个 0x80 → UV_EINVAL（第 10 个就该收尾）");
}

// =========================================================================
// 第 7 轮：字段编码——逐字节
// =========================================================================

void test_field_encode() {
  std::cout << "[7] 字段编码：tag 与值逐字节" << std::endl;

  std::string out;
  check(grpc_field_put_int32(out, 1, 2) == 0, "字段 1 = 2 编成功");
  check(hex(out) == "0802", "字段 1 = 2 → 08 02（tag = (1<<3)|0 = 0x08）");

  out.clear();
  grpc_field_put_int32(out, 2, 3);
  check(hex(out) == "1003", "字段 2 = 3 → 10 03");

  // 字段号 16 ⇒ tag = 128，它自己得用两字节 varint 编 —— tag 不是定长的一字节。
  out.clear();
  grpc_field_put_int32(out, 16, 1);
  check(hex(out) == "800101", "字段 16 → tag 自己也是 varint（80 01），再跟 01");

  // **符号扩展**：这一条是整个文件里最难自己发现的一条。
  out.clear();
  grpc_field_put_int32(out, 1, -1);
  check(hex(out) == "08ffffffffffffffffff01",
        "字段 1 = -1 → 08 后跟 10 字节 FF×9 01（负数是 64 位符号扩展，不是 1 字节）");

  // 字段号 0 根本不存在。
  out.clear();
  check(grpc_field_put_int32(out, 0, 1) == UV_EINVAL, "字段号 0 → UV_EINVAL");
  check(out.empty(), "  而且一个字节都没写进去");
}

// =========================================================================
// 第 8 轮：字段读取——跳过与覆盖
// =========================================================================

void test_field_decode() {
  std::cout << "[8] 字段读取：找得到、找不到、跳过、取最后一个" << std::endl;

  // 线上真会收到的形状：HttpArena 那条 grpc-sum 请求的载荷就是这 4 个字节。
  const std::string sum = bytes_of("\x08\x01\x10\x02", 4);  // 1=1, 2=2
  int32_t           v   = 0;
  check(grpc_field_get_int32(sum.data(), sum.size(), 1, v) == 0 && v == 1,
        "字段 1 → 1");
  check(grpc_field_get_int32(sum.data(), sum.size(), 2, v) == 0 && v == 2,
        "字段 2 → 2");
  check(grpc_field_get_int32(sum.data(), sum.size(), 3, v) == UV_ENOENT,
        "字段 3 不在消息里 → UV_ENOENT（**这不是错误**，缺省值是调用方的事）");

  // 跳过长度前缀字段（wire type 2）。要找的字段**放在后面** —— 只看第一个字段的
  // 实现在这里才会露馅。
  std::string skip;
  skip += '\x0a';          // tag: 字段 1, wire type 2
  skip += '\x03';          // 3 字节载荷
  skip += "abc";
  skip += '\x10';          // tag: 字段 2, wire type 0
  skip += '\x07';
  v = 0;
  check(grpc_field_get_int32(skip.data(), skip.size(), 2, v) == 0 && v == 7,
        "前面的长度前缀字段被跳过，后面的字段 2 找得到");

  // 跳过 64 位定长（wire type 1）与 32 位定长（wire type 5）。
  std::string fx;
  fx += '\x09';            // tag: 字段 1, wire type 1
  fx.append(8, '\xAA');
  fx += '\x10';
  fx += '\x05';
  check(grpc_field_get_int32(fx.data(), fx.size(), 2, v) == 0 && v == 5,
        "8 字节定长字段被跳过");

  std::string f5;
  f5 += '\x0d';            // tag: 字段 1, wire type 5
  f5.append(4, '\xAA');
  f5 += '\x10';
  f5 += '\x06';
  check(grpc_field_get_int32(f5.data(), f5.size(), 2, v) == 0 && v == 6,
        "4 字节定长字段被跳过");

  // 同一个字段号出现两次：protobuf 对奇异标量字段的语义是**后者覆盖前者**。
  // 取第一个的实现在这里解出一个过时的值。
  std::string rep;
  rep += '\x08';
  rep += '\x01';
  rep += '\x08';
  rep += '\x09';
  v = 0;
  check(grpc_field_get_int32(rep.data(), rep.size(), 1, v) == 0 && v == 9,
        "字段 1 出现两次 → 取最后一个（protobuf 的覆盖语义）");

  // 认不出的 wire type 判非法：3/4 是已废弃的 group，长度未知，猜不得。
  const std::string grp = bytes_of("\x0b", 1);  // 字段 1, wire type 3
  check(grpc_field_get_int32(grp.data(), grp.size(), 1, v) == UV_EINVAL,
        "wire type 3（group）→ UV_EINVAL");

  // 字段号 0 不存在：一个全零的缓冲区必须被认出来是坏的，而不是"解出个 0"。
  const std::string zero = bytes_of("\x00\x00", 2);
  check(grpc_field_get_int32(zero.data(), zero.size(), 1, v) == UV_EINVAL,
        "tag 为 0（字段号 0）→ UV_EINVAL");

  // 长度前缀字段说自己有 10 字节、实际只剩 2 —— 载荷被截断，判 UV_ENODATA。
  std::string cut;
  cut += '\x0a';
  cut += '\x0a';
  cut += "ab";
  check(grpc_field_get_int32(cut.data(), cut.size(), 1, v) == UV_ENODATA,
        "长度前缀字段的载荷被截断 → UV_ENODATA");

  // 负数往返（补充判据，**不是**符号扩展的锚点 —— 锚点在 [7] 的逐字节那条）。
  std::string neg;
  grpc_field_put_int32(neg, 3, -12345);
  int32_t back = 0;
  check(grpc_field_get_int32(neg.data(), neg.size(), 3, back) == 0 && back == -12345,
        "负 int32 往返（补充：编了再解，对「两处一起错」不敏感）");

  // 字段号 0 在**读**这一侧也要拒。
  check(grpc_field_get_int32(sum.data(), sum.size(), 0, v) == UV_EINVAL,
        "读的时候问字段号 0 → UV_EINVAL");
}

}  // namespace

int main() {
  std::cout << "gRPC 编解码（src/grpc/uvcpp_grpc.h）" << std::endl;

  test_frame_prefix();
  test_frame_decode();
  test_frame_length_cap();
  test_streaming_decode();
  test_varint_table();
  test_varint_rejects();
  test_field_encode();
  test_field_decode();

  if (g_failures == 0) {
    std::cout << "全部通过" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " 条失败" << std::endl;
  return 1;
}
