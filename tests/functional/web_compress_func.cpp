/**
 * @file tests/functional/web_compress_func.cpp
 * @brief HTTP gzip + WebSocket permessage-deflate tests (UVCPP_ZLIB_ENABLE=1 only)
 *
 * 关于「互操作」测试
 * ------------------
 * 本文件原先只有"自己压、自己解"的往返断言。那种断言对 RFC 7692 合规性
 * **零信息量** —— 实测中 `compress`（用 Z_FINISH 写出 BFINAL=1 的结束块）
 * 和 `decompress`（要求 Z_STREAM_END 却从不补回被剥掉的尾巴）两个错误恰好
 * 互相抵消，往返全绿，而和任何真实浏览器都通不了。
 *
 * 所以下面所有涉及线格式的用例都改用一个**独立的、按 RFC 7692 手写的**
 * 参照实现（`ref_deflater` / `ref_inflater`，直接用 zlib，不走 uvcpp 代码）：
 * 本端产物喂给参照接收方，参照发送方的产物喂给本端。往返自测只保留一条，
 * 其余全部换成互操作。
 */
#include <iostream>
#include <string>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_ZLIB_ENABLE
#include <cstring>
#include <zlib.h>

#include <web/uvcpp_ws_frame.h>
#include <web/uvcpp_ws_parser.h>
using namespace uvcpp;

static const char k_tail[4] = {0x00, 0x00, (char)0xff, (char)0xff};

// =========================================================================
// 参照实现：按 RFC 7692 §7.2.1 / §7.2.2 手工实现收发两侧，不经过 uvcpp。
// =========================================================================

/** @brief 合规发送方：raw deflate + Z_SYNC_FLUSH + 剥掉尾巴。有状态（context takeover）。 */
struct ref_deflater {
  z_stream zs;
  bool inited;

  ref_deflater() : inited(false) { std::memset(&zs, 0, sizeof(zs)); }
  ~ref_deflater() { close(); }

  bool open(int bits) {
    inited = (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -bits, 8,
                           Z_DEFAULT_STRATEGY) == Z_OK);
    return inited;
  }
  void close() {
    if (inited) { deflateEnd(&zs); inited = false; }
  }
  bool reset() { return deflateReset(&zs) == Z_OK; }

  /** @brief 产出一条消息的线上负载（已剥尾）。 */
  bool feed(const std::string& msg, std::string& wire) {
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(msg.data()));
    zs.avail_in = static_cast<uInt>(msg.size());
    std::string acc;
    int ret;
    do {
      unsigned char t[4096];
      zs.next_out = t;
      zs.avail_out = sizeof(t);
      ret = deflate(&zs, Z_SYNC_FLUSH);
      if (ret != Z_OK) return false;
      acc.append(reinterpret_cast<const char*>(t), sizeof(t) - zs.avail_out);
    } while (zs.avail_out == 0);

    if (acc.size() < 4) return false;
    if (acc.compare(acc.size() - 4, 4, k_tail, 4) != 0) return false;
    wire.assign(acc.data(), acc.size() - 4);
    return true;
  }
};

/** @brief 合规接收方：补回尾巴 + raw inflate。有状态。 */
struct ref_inflater {
  z_stream zs;
  bool inited;

  ref_inflater() : inited(false) { std::memset(&zs, 0, sizeof(zs)); }
  ~ref_inflater() { close(); }

  bool open(int bits) {
    inited = (inflateInit2(&zs, -bits) == Z_OK);
    return inited;
  }
  void close() {
    if (inited) { inflateEnd(&zs); inited = false; }
  }
  bool reset() { return inflateReset(&zs) == Z_OK; }

  /**
   * @brief 解一条消息。
   * @param saw_stream_end  出参：zlib 是否报 Z_STREAM_END（= 对方误写了 BFINAL=1，
   *                        context takeover 下这条流已经报废）
   * @return 0 成功；-1 数据非法
   */
  int feed(const std::string& wire, std::string& msg, bool* saw_stream_end) {
    std::string in = wire;
    in.append(k_tail, 4);  // §7.2.2：收方必须补回来
    zs.next_in = reinterpret_cast<Bytef*>(&in[0]);
    zs.avail_in = static_cast<uInt>(in.size());
    msg.clear();
    int ret = Z_OK;
    do {
      unsigned char t[4096];
      zs.next_out = t;
      zs.avail_out = sizeof(t);
      ret = inflate(&zs, Z_SYNC_FLUSH);
      if (ret != Z_OK && ret != Z_BUF_ERROR && ret != Z_STREAM_END) {
        if (saw_stream_end) *saw_stream_end = false;
        return -1;
      }
      msg.append(reinterpret_cast<const char*>(t), sizeof(t) - zs.avail_out);
    } while (zs.avail_out == 0);
    if (saw_stream_end) *saw_stream_end = (ret == Z_STREAM_END);
    return 0;
  }
};

/**
 * @brief 构造一条**逼着编码器用长距离匹配**的消息（用于识别窗口位数）。
 *
 * 600 字节伪随机串重复三遍：第二、三遍会被编码成距离 600 / 1200 的匹配。
 * 窗口位数 9 的编码器最大距离只有 512-262=250，**发不出**这种流 —— 于是
 * "用 15 位还是 9 位"在**产物字节**上就是可见的（实测：这份输入 15 位压出
 * 396 字节、9 位压出 1085 字节）。
 *
 * 注意这只在**编码**方向可观测。解码方向看不到窗口位数：实测 classic
 * zlib 1.3.1 的 inflate 对 9..15 之间的窗口参数不做距离校验（非
 * INFLATE_STRICT 构建下 `dmax` 恒为 32768），`inflateInit2(-9)` 能完整解出
 * windowBits=15 的流。所以解压方向改用越界参数来证明，见
 * `test_window_bits_direction` 的 (B) 段。
 */
static std::string distance_msg() {
  static const char k_alpha[] = "abcdefghijklmnopqrstuvwxyz";
  std::string base;
  unsigned v = 0x12345678u;
  for (int i = 0; i < 600; ++i) {
    v = v * 1103515245u + 12345u;
    base.push_back(k_alpha[(v >> 16) % 26]);
  }
  return base + base + base;
}

// =========================================================================
// Test 1: Enable compression and get extension header
// =========================================================================
static bool test_compression_enable() {
  uvcpp_ws_parser parser;
  parser.enable_compression(false /*is_server*/, true, false, 15, 13);
  if (!parser.is_compression_enabled()) return false;

  std::string ext = parser.get_extension_header();
  if (ext.find("permessage-deflate") == std::string::npos) return false;
  if (ext.find("client_no_context_takeover") == std::string::npos) return false;
  if (ext.find("server_max_window_bits=13") == std::string::npos) return false;
  return true;
}

// =========================================================================
// Test 2: Compression round-trip (compress → decompress) — 唯一的自测往返
// =========================================================================
static bool test_compress_roundtrip() {
  uvcpp_ws_parser parser;
  parser.enable_compression(false, false, false, 15, 15);

  // Create a message that compresses well
  std::string original(1000, 'A');
  for (int i = 0; i < 1000; i++) original[i] = (char)('A' + (i % 26));

  uvcpp_buf compressed;
  const unsigned char* in = reinterpret_cast<const unsigned char*>(original.c_str());
  int rc = parser.compress(in, original.size(), compressed);
  if (rc != 0) return false;
  if (compressed.size() == 0) return false;

  uvcpp_buf decompressed;
  const unsigned char* cin = compressed.get_const_udata();
  rc = parser.decompress(cin, compressed.size(), decompressed);
  if (rc != 0) return false;

  if (decompressed.size() != original.size()) return false;
  if (decompressed.to_string() != original) return false;
  return true;
}

// =========================================================================
// Test 3: Compress very small message
// =========================================================================
static bool test_compress_small() {
  uvcpp_ws_parser parser;
  parser.enable_compression(true, true, true, 15, 15);

  std::string short_msg = "Hi";
  uvcpp_buf compressed;
  const unsigned char* in = reinterpret_cast<const unsigned char*>(short_msg.c_str());
  int rc = parser.compress(in, short_msg.size(), compressed);
  if (rc != 0) return false;

  uvcpp_buf decompressed;
  rc = parser.decompress(compressed.get_const_udata(), compressed.size(), decompressed);
  if (rc != 0) return false;
  if (decompressed.to_string() != short_msg) return false;
  return true;
}

// =========================================================================
// Test 3b: 空消息 —— RFC 7692 §7.2.3.6 规定压完是**单个 0x00 字节**
// =========================================================================
static bool test_compress_empty() {
  uvcpp_ws_parser parser;
  parser.enable_compression(false, false, false, 15, 15);

  uvcpp_buf c;
  if (parser.compress(nullptr, 0, c) != 0) return false;
  if (c.size() != 1) return false;
  if (c.get_const_udata()[0] != 0x00) return false;

  uvcpp_buf d;
  if (parser.decompress(c.get_const_udata(), c.size(), d) != 0) return false;
  if (d.size() != 0) return false;
  return true;
}

// =========================================================================
// Test 4: Decompress garbage must actually fail (原先这条是恒真断言)
// =========================================================================
static bool test_compress_errors() {
  uvcpp_ws_parser parser;
  parser.enable_compression(false, false, false, 15, 15);

  uvcpp_buf out;
  unsigned char garbage[] = {0xFF, 0xFF, 0xFF, 0xFF};
  int rc = parser.decompress(garbage, 4, out);
  if (rc == 0) return false;      // 保留块类型 BTYPE=3 → 必须报错
  if (out.size() != 0) return false;  // 失败时 out 必须为空（契约）

  // 参照接收方对同一份垃圾也必须失败 —— 证明断言不是把"正常输入"判成错
  ref_inflater ri;
  if (!ri.open(15)) return false;
  std::string m;
  bool sse = false;
  if (ri.feed(std::string(reinterpret_cast<char*>(garbage), 4), m, &sse) == 0) return false;

  // 出错后上下文必须已重置，后续消息照常可解
  ref_deflater rd;
  if (!rd.open(15)) return false;
  std::string wire;
  if (!rd.feed("after-error", wire)) return false;
  uvcpp_buf d;
  if (parser.decompress(reinterpret_cast<const unsigned char*>(wire.data()),
                        wire.size(), d) != 0) return false;
  if (d.to_string() != "after-error") return false;
  return true;
}

// =========================================================================
// Test 5: Multiple compress/decompress cycles (context reuse)
// =========================================================================
static bool test_compress_multi() {
  uvcpp_ws_parser parser;
  parser.enable_compression(false, false, false, 15, 15);

  ref_inflater ri;                 // 参照接收方，与 parser 独立
  if (!ri.open(15)) return false;

  for (int i = 0; i < 5; i++) {
    std::string msg(100, 'A' + (char)(i % 26));
    uvcpp_buf c, d;
    int rc = parser.compress(reinterpret_cast<const unsigned char*>(msg.c_str()), msg.size(), c);
    if (rc != 0) return false;
    rc = parser.decompress(c.get_const_udata(), c.size(), d);
    if (rc != 0) return false;
    if (d.to_string() != msg) return false;

    // 本端产物必须能被一个独立实现的合规接收方解出
    std::string got;
    bool sse = false;
    if (ri.feed(std::string(reinterpret_cast<const char*>(c.get_const_udata()), c.size()),
                got, &sse) != 0) return false;
    if (got != msg) return false;
  }
  return true;
}

// =========================================================================
// Test 6: WS frame with compression (build → parse round-trip)
// =========================================================================
static bool test_ws_frame_compress() {
  // Build a frame with RSV1 set (compressed flag)
  uvcpp_ws_frame f;
  f.opcode = ws_opcode::TEXT;
  f.rsv1 = true;  // Mark as compressed
  f.payload.clone_data("compressed_data", 15);

  size_t sz = uvcpp_ws_parser::calc_frame_size(f);
  char* buf = new char[sz];
  size_t written = uvcpp_ws_parser::build_frame(buf, f);

  // Parse it back
  uvcpp_ws_parser p;
  p.execute(buf, written);
  delete[] buf;

  auto& pf = p.get_current_frame();
  return pf.opcode == ws_opcode::TEXT &&
         pf.rsv1 == true &&
         pf.payload.size() == 15 &&
         pf.payload.to_string() == "compressed_data";
}

// =========================================================================
// Test 7: 发送方向互操作 —— 本端产物必须是合规对端能收的形状
// =========================================================================
static bool test_interop_send() {
  std::string msg = distance_msg();
  uvcpp_ws_parser parser;
  parser.enable_compression(false /*is_server*/, false, false, 15, 15);

  uvcpp_buf out;
  if (parser.compress(reinterpret_cast<const unsigned char*>(msg.data()),
                      msg.size(), out) != 0) return false;

  const std::string wire(reinterpret_cast<const char*>(out.get_const_udata()), out.size());

  // (1) 尾巴必须已经剥掉 —— 没剥的话对端补一次就多出 4 个字节
  if (wire.size() >= 4 && wire.compare(wire.size() - 4, 4, k_tail, 4) == 0) return false;

  // (2) 合规接收方必须解得出来
  ref_inflater ri;
  if (!ri.open(15)) return false;
  std::string got;
  bool sse = false;
  if (ri.feed(wire, got, &sse) != 0) return false;
  if (got != msg) return false;

  // (3) **关键**：绝不能是 Z_STREAM_END —— 那表示本端写了 BFINAL=1 的结束块，
  //     context takeover 下对端的流到此为止，后续消息全废。
  if (sse) return false;

  // (4) 同一份输入、同一份 zlib 参数，产物应当逐字节相同（own = client = 15）
  ref_deflater rd;
  if (!rd.open(15)) return false;
  std::string ref_wire;
  if (!rd.feed(msg, ref_wire)) return false;
  if (ref_wire != wire) return false;
  return true;
}

// =========================================================================
// Test 8: 接收方向互操作 —— 合规对端发来的负载必须 rc == 0
// =========================================================================
static bool test_interop_recv() {
  std::string msg = distance_msg();
  ref_deflater rd;
  if (!rd.open(15)) return false;
  std::string wire;
  if (!rd.feed(msg, wire)) return false;

  uvcpp_ws_parser parser;
  parser.enable_compression(false, false, false, 15, 15);
  uvcpp_buf out;
  if (parser.decompress(reinterpret_cast<const unsigned char*>(wire.data()),
                        wire.size(), out) != 0) return false;
  if (out.to_string() != msg) return false;
  return true;
}

// =========================================================================
// Test 9: 窗口位数方向 —— own 用自己这侧、peer 用对端那侧
//
// 这一组存在的理由：改造前的 `compress` 固定用 `client_max_window_bits_`、
// `decompress` 固定用 `server_max_window_bits_`，完全不管本端是客户端还是
// 服务端。`client_*` / `server_*` 是**按方向**命名的协商结果，只有知道角色
// 才能把两个数摆对。
//
// 怎么把方向测出来（两把不同的尺子，因为两侧的可观测性不一样）：
//
//  (A) 压缩侧：窗口位数**直接改变产物字节**。用 15 位和 9 位压同一份输入，
//      产物不同（1800 字节的输入分别得到 396 / 1085 字节）。所以拿
//      "自己这侧位数"的参照产物做逐字节比对即可。
//
//  (B) 解压侧：**行为上观察不到**。实测 classic zlib 1.3.1 的 `inflateInit2`
//      对 9..15 之间的窗口位数不做距离校验（`inflateInit2(-9)` 能完整解出
//      windowBits=15 的流，因为非 INFLATE_STRICT 构建下 `dmax` 恒为 32768）。
//      所以"9 位解压器必须拒绝 15 位的流"是**错的**断言。
//      改用 zlib 自己的参数校验：把某一侧设成越界值 99，`deflateInit2` /
//      `inflateInit2` 都会返回 Z_STREAM_ERROR。
//      于是"把自己这侧设成 99 → compress 必须失败"和"把对端这侧设成 99 →
//      decompress 必须失败"就分别钉死了两个方向用了哪一个数。
// =========================================================================
static bool test_window_bits_direction() {
  const std::string msg = distance_msg();
  const unsigned char* min =
      reinterpret_cast<const unsigned char*>(msg.data());

  // 前置条件：这份输入必须真的能区分 15 位和 9 位编码器，否则 (A) 是空的
  ref_deflater d15, d9;
  if (!d15.open(15) || !d9.open(9)) return false;
  std::string w15, w9;
  if (!d15.feed(msg, w15) || !d9.feed(msg, w9)) return false;
  if (w15 == w9) return false;

  // ---- (A) 现实取值：产物必须等同"自己这侧位数"的参照产物 ----
  struct { bool is_server; int cbits; int sbits; int own_bits; } cases[] = {
    {true,   9, 15, 15},   // 服务端：own = server_max_window_bits
    {true,  15,  9,  9},   // 服务端：own 跟着 server 走，不跟 client 走
    {false,  9, 15,  9},   // 客户端：own = client_max_window_bits
    {false, 15,  9, 15},   // 客户端：own 跟着 client 走
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const std::string expect = (cases[i].own_bits == 15) ? w15 : w9;
    const std::string other  = (cases[i].own_bits == 15) ? w9  : w15;

    uvcpp_ws_parser p;
    p.enable_compression(cases[i].is_server, false, false,
                         cases[i].cbits, cases[i].sbits);
    uvcpp_buf out;
    if (p.compress(min, msg.size(), out) != 0) return false;
    const std::string got(reinterpret_cast<const char*>(out.get_const_udata()),
                          out.size());
    if (got != expect) return false;   // 用了错的一侧
    if (got == other) return false;
  }

  // ---- (A2) 解压不得因任何一组协商取值而出错（回归护栏；(B) 才是方向证明）----
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    uvcpp_ws_parser p;
    p.enable_compression(cases[i].is_server, false, false,
                         cases[i].cbits, cases[i].sbits);
    const std::string* payloads[2] = {&w9, &w15};
    for (int k = 0; k < 2; ++k) {
      uvcpp_buf d;
      const std::string& w = *payloads[k];
      if (p.decompress(reinterpret_cast<const unsigned char*>(w.data()),
                       w.size(), d) != 0) return false;
      if (d.to_string() != msg) return false;
    }
  }

  // ---- (B) 越界取值：直接钉死"用了哪一个数" ----
  // 自己这侧越界 → compress 必须失败（若误用对端那侧的合法值就会成功）
  {
    uvcpp_ws_parser svr;
    svr.enable_compression(true, false, false, 15, 99);   // own = server = 99
    uvcpp_buf out;
    if (svr.compress(min, msg.size(), out) == 0) return false;

    uvcpp_ws_parser cli;
    cli.enable_compression(false, false, false, 99, 15);  // own = client = 99
    if (cli.compress(min, msg.size(), out) == 0) return false;

    // 而同样的实例把对端那侧设成 99 时，compress 不受影响（自己这侧合法）
    uvcpp_ws_parser ok_svr;
    ok_svr.enable_compression(true, false, false, 99, 15);  // own = server = 15
    if (ok_svr.compress(min, msg.size(), out) != 0) return false;

    uvcpp_ws_parser ok_cli;
    ok_cli.enable_compression(false, false, false, 15, 99);  // own = client = 15
    if (ok_cli.compress(min, msg.size(), out) != 0) return false;
  }
  // 对端这侧越界 → decompress 必须失败（若误用自己那侧的合法值就会成功）
  {
    uvcpp_ws_parser svr;
    svr.enable_compression(true, false, false, 99, 15);   // peer = client = 99
    uvcpp_buf d;
    if (svr.decompress(reinterpret_cast<const unsigned char*>(w15.data()),
                       w15.size(), d) == 0) return false;
    if (d.size() != 0) return false;

    uvcpp_ws_parser cli;
    cli.enable_compression(false, false, false, 15, 99);  // peer = server = 99
    if (cli.decompress(reinterpret_cast<const unsigned char*>(w15.data()),
                       w15.size(), d) == 0) return false;

    // 自己这侧越界不影响解压（解压只看对端那侧）
    uvcpp_ws_parser ok_svr;
    ok_svr.enable_compression(true, false, false, 15, 99);  // peer = client = 15
    if (ok_svr.decompress(reinterpret_cast<const unsigned char*>(w15.data()),
                          w15.size(), d) != 0) return false;
    if (d.to_string() != msg) return false;

    uvcpp_ws_parser ok_cli;
    ok_cli.enable_compression(false, false, false, 99, 15);  // peer = server = 15
    if (ok_cli.decompress(reinterpret_cast<const unsigned char*>(w15.data()),
                          w15.size(), d) != 0) return false;
    if (d.to_string() != msg) return false;
  }
  return true;
}

// =========================================================================
// Test 10: context takeover —— 未协商 no_context_takeover 时上下文必须延续
// =========================================================================
static bool test_context_takeover() {
  const std::string m1 = distance_msg();
  const std::string m2 = m1.substr(0, 700) + "abcdefghij";

  // ---- 默认（允许 takeover）：参照发送方连续两条共用上下文，本端必须都解得开 ----
  {
    ref_deflater rd;
    if (!rd.open(15)) return false;
    std::string w1, w2;
    if (!rd.feed(m1, w1) || !rd.feed(m2, w2)) return false;

    uvcpp_ws_parser p;
    p.enable_compression(false, false, false, 15, 15);
    uvcpp_buf d;
    if (p.decompress(reinterpret_cast<const unsigned char*>(w1.data()),
                     w1.size(), d) != 0) return false;
    if (d.to_string() != m1) return false;
    // 第二条依赖第一条建立的字典；本端若在中途重置了上下文，这里会失败
    if (p.decompress(reinterpret_cast<const unsigned char*>(w2.data()),
                     w2.size(), d) != 0) return false;
    if (d.to_string() != m2) return false;

    // ---- 发送侧也必须**真的复用**上下文，不能只是"对端解得开" ----
    // 只测往返抓不到"每条消息都重置"：重置后的流是自包含的，对端照样解得出来，
    // 区别只在压缩率和产物字节上。所以拿参照实现（共享上下文）的产物逐字节比对。
    // 第一条本来就该一致（都是从零开始），**第二条**才是判据。
    uvcpp_ws_parser enc;
    enc.enable_compression(false, false, false, 15, 15);
    uvcpp_buf c1, c2;
    if (enc.compress(reinterpret_cast<const unsigned char*>(m1.data()),
                     m1.size(), c1) != 0) return false;
    const std::string g1(reinterpret_cast<const char*>(c1.get_const_udata()),
                         c1.size());
    if (g1 != w1) return false;
    if (enc.compress(reinterpret_cast<const unsigned char*>(m2.data()),
                     m2.size(), c2) != 0) return false;
    const std::string g2(reinterpret_cast<const char*>(c2.get_const_udata()),
                         c2.size());
    if (g2 != w2) return false;
  }

  // ---- 协商了 client_no_context_takeover（本端是服务端）----
  {
    // 合规对端：每条消息重置 → 两条流互不依赖
    ref_deflater d_reset;
    if (!d_reset.open(15)) return false;
    std::string s1, s2;
    if (!d_reset.feed(m1, s1)) return false;
    if (!d_reset.reset()) return false;
    if (!d_reset.feed(m2, s2)) return false;
    // 独立验证 s2 确实自包含：一个被重置过的解压器必须能单独解出它
    {
      ref_inflater ri;
      if (!ri.open(15)) return false;
      std::string chk;
      bool sse = false;
      if (ri.feed(s2, chk, &sse) != 0) return false;
      if (chk != m2) return false;
    }

    // 违约对端：不重置 → h2 依赖 h1 建立的历史（这正是上面第一个块用的那对）
    ref_deflater d_state;
    if (!d_state.open(15)) return false;
    std::string h1, h2;
    if (!d_state.feed(m1, h1) || !d_state.feed(m2, h2)) return false;

    // 先独立确认 h2 的"依赖历史"性质是实测成立的 —— 否则下面的拒绝断言是空的
    {
      ref_inflater ri;
      if (!ri.open(15)) return false;
      std::string chk;
      bool sse = false;
      if (ri.feed(h1, chk, &sse) != 0) return false;
      ri.reset();
      std::string chk2;
      if (ri.feed(h2, chk2, &sse) == 0) return false;  // 重置后必须解不开
    }

    // 合规流：两条都收得下
    {
      uvcpp_ws_parser p;
      p.enable_compression(true /*is_server*/, true /*client_no_ctxt*/, false, 15, 15);
      uvcpp_buf d;
      if (p.decompress(reinterpret_cast<const unsigned char*>(s1.data()),
                       s1.size(), d) != 0) return false;
      if (d.to_string() != m1) return false;
      if (p.decompress(reinterpret_cast<const unsigned char*>(s2.data()),
                       s2.size(), d) != 0) return false;
      if (d.to_string() != m2) return false;
    }

    // ⭐ 违约流必须被**拒绝**：协商了 no_context_takeover 就意味着本端每次都要
    // 丢掉历史。一个不重置的实现会拿窗口里的陈年字节把这条消息拼出来（内容
    // 看似合理但来自上一条消息），而不是报错。只测"合规流能解开"抓不到这一点
    // —— 实测变异 W8（去掉重置）下那些断言全绿，只有下面这条能抓住。
    {
      uvcpp_ws_parser p;
      p.enable_compression(true, true /*client_no_ctxt*/, false, 15, 15);
      uvcpp_buf d;
      if (p.decompress(reinterpret_cast<const unsigned char*>(h1.data()),
                       h1.size(), d) != 0) return false;
      if (d.to_string() != m1) return false;
      if (p.decompress(reinterpret_cast<const unsigned char*>(h2.data()),
                       h2.size(), d) == 0) return false;
      if (d.size() != 0) return false;
    }
  }

  // ---- 自己这侧协商了 no_context_takeover 时，**发送**侧必须每条重置 ----
  // 这是上面解码侧断言的镜像，同样只有这一种形状能测出来：只做自测往返时，
  // 一个"从不重置"的实现压出来的第二条会引用第一条的字典，而本端自己的解压
  // 器保留着那份历史，于是自测往返**照样通过**。真正的对端已经被我们告知
  // "不会复用上下文"，它早就把解压器重置了 —— 解我们第二条时会以
  // "invalid distance too far back" 失败。所以这里必须用一个**每条消息之间
  // 重置**的参照接收方来解我们连续压的两条。
  {
    uvcpp_ws_parser enc;
    enc.enable_compression(true /*is_server*/, false, true /*server_no_ctxt*/, 15, 15);
    uvcpp_buf c1, c2;
    if (enc.compress(reinterpret_cast<const unsigned char*>(m1.data()),
                     m1.size(), c1) != 0) return false;
    if (enc.compress(reinterpret_cast<const unsigned char*>(m2.data()),
                     m2.size(), c2) != 0) return false;

    ref_inflater ri;  // 模拟"被告知不会复用上下文"的对端
    if (!ri.open(15)) return false;
    std::string g1, g2;
    bool sse = false;
    if (ri.feed(std::string(reinterpret_cast<const char*>(c1.get_const_udata()),
                            c1.size()), g1, &sse) != 0) return false;
    if (g1 != m1) return false;
    ri.reset();  // ⭐ 对端会重置；本端第二条若引用了第一条的字典，这里就会失败
    if (ri.feed(std::string(reinterpret_cast<const char*>(c2.get_const_udata()),
                            c2.size()), g2, &sse) != 0) return false;
    if (g2 != m2) return false;
  }
  return true;
}

// =========================================================================
// Test 11: 解压结果恰好填满内部缓冲（4096 的整数倍）
//
// 解压循环的条件是"输出缓冲被填满就再解一轮"。当解出来的长度**恰好**是 4096
// 的整数倍时，最后一轮把缓冲填满从而再进一轮，而输入此时已耗尽 ——
// inflate 返回的是 Z_BUF_ERROR（"无法推进"）而不是 Z_OK。把 Z_BUF_ERROR 当
// 错误处理，这类尺寸的消息就会全部解不开。变异 W12 正是如此，而**只有**尺寸
// 是 4096 整数倍的用例能抓住它（4095 / 4097 都在最后一轮之前就退出了循环）。
// =========================================================================
static bool test_decompress_chunk_boundary() {
  const size_t sizes[] = {4096, 8192, 3 * 4096};
  for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
    std::string m;
    m.reserve(sizes[k]);
    for (size_t i = 0; i < sizes[k]; ++i) m.push_back((char)('a' + (i % 7)));

    uvcpp_ws_parser e, d;
    e.enable_compression(false, false, false, 15, 15);
    d.enable_compression(false, false, false, 15, 15);
    uvcpp_buf c, o;
    if (e.compress(reinterpret_cast<const unsigned char*>(m.data()),
                   m.size(), c) != 0) return false;
    if (d.decompress(c.get_const_udata(), c.size(), o) != 0) return false;
    if (o.size() != m.size()) return false;
    if (o.to_string() != m) return false;

    // 合规参照接收方必须给出同样的结果（确认这不是"本端自己一套规矩"）
    ref_inflater ri;
    if (!ri.open(15)) return false;
    std::string got;
    bool sse = false;
    if (ri.feed(std::string(reinterpret_cast<const char*>(c.get_const_udata()),
                            c.size()), got, &sse) != 0) return false;
    if (got != m) return false;
  }
  return true;
}

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"compression_enable", test_compression_enable},
    {"compress_roundtrip", test_compress_roundtrip},
    {"compress_small", test_compress_small},
    {"compress_empty", test_compress_empty},
    {"compress_errors", test_compress_errors},
    {"compress_multi", test_compress_multi},
    {"ws_frame_compress", test_ws_frame_compress},
    {"interop_send", test_interop_send},
    {"interop_recv", test_interop_recv},
    {"window_bits_direction", test_window_bits_direction},
    {"context_takeover", test_context_takeover},
    {"decompress_chunk_boundary", test_decompress_chunk_boundary},
  };
  for (const auto& t : tests) {
    std::cout << "[web_compress] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_compress] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}
#else
int main() {
  std::cout << "[web_compress] SKIP (zlib disabled)\n";
  return 0;
}
#endif
