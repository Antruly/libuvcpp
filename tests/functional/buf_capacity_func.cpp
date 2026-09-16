/**
 * @file tests/functional/buf_capacity_func.cpp
 * @brief uvcpp_buf 的容量复用：append 的均摊代价、以及**所有权交接**处的容量清零。
 *
 * 缺陷形状（清单第 8 条）：`resize()` 是「精确大小」重新分配（无容量增长），
 * 于是 `append_data` 每次都要新分配一块、把旧内容整体搬过去、再释放旧块 ——
 * 攒 n 个分片就是 O(n²) 的拷贝量与 n 次分配。`src/web/uvcpp_ws_parser.cpp:159`
 * 那段注释记着它真实发生过：一个较大的**带掩码**帧能让服务端在 `resize` 里抛
 * `std::bad_alloc`（进程直接终止）。改法是给 `uvcpp_buf` 加容量（`capacity_`），
 * 不够时按容量翻倍。
 *
 * 判据分两族，缺一不可：
 *   A. 行为面：[1b] 逐块 append 到 2.56 MB 之后**逐字节**核对内容；[6][7][8] 核
 *      对 resize 复用、缩小再放大、insert/rewrite 的内容 —— 容量化不能改坏语义。
 *   B. 代价面：[1a][1b] 的**绝对墙钟**上界。为什么必须是绝对值：成本如果是两边
 *      都付的（在公共前置里），"被测减基线"的差会把它减掉 —— 见
 *      `~uvcpp_udp_server` 那轮踩过的坑。
 *      实测（本机，内存池开启，Release）：
 *        - 改之后：6000 × 64 字节 0.42 ms、40000 × 64 字节 2.24 ms；
 *        - 改之前（变异 M1 = "把容量复用与翻倍整个去掉"）：
 *          6000 × 64 字节 **187.1 ms**（375 KB 要搬约 1.1 GB），
 *          40000 那一档**跑不完** —— 一路新分配 + 全量搬运，最后在 `resize`
 *          里 `std::bad_alloc` 终止进程（0xC0000409），与
 *          `src/web/uvcpp_ws_parser.cpp` 记的带掩码大帧是同一个形状。
 *      阈值 30 ms / 1500 ms 夹在中间，够宽。
 *      [1a] 那个小规模轮**必须排在 [1b] 前面**：缺陷版本在 [1b] 上直接死掉，
 *      排后面的话"墙钟判据本身有效"就没有证据了（只剩"进程死了"）。
 *
 * 交接点那一族（[3][4][5][5b]）是这次改动新引入的风险面。它们都是确定性的
 * （比对地址与内容），不依赖时间。同理 [7][9] 钉住"复用容量的那条分支必须
 * 照旧清零"（去掉那两句 memset，这两条必红）。
 *
 * 变异验证（每条都是"改回缺陷写法 → 重建 → 跑"，6 条）：
 *   | 变异 | 结果 |
 *   |---|---|
 *   | M1 去掉容量复用 + 翻倍        | 抓住（[1a] 印 187.1 ms，随后 [1b] 终止进程）|
 *   | M2 复用分支不清零              | 抓住（[7]、[9] 各红一条）|
 *   | M3 out_uv_buf 不清 capacity_   | **漏** —— 等价变异，理由见 [3] 的说明 |
 *   | M4 move_buf 不清源 capacity_   | **漏** —— 同上 |
 *   | M5 set_data 把视图记成自有块    | 抓住（[5b]：同尺寸分配把同一块又发了出去）|
 *   | M6 clone_buf 退回别名写法       | 抓住（[6]:地址相同 + 改源影响副本）|
 *   M3/M4 之所以是等价变异：`capacity_` 只有复用分支读，而那条分支的前置是
 *   `base != nullptr`；交接时 base 已经被置空，残留的容量取不到。只有把那条
 *   前置去掉，M3/M4 才会变成真缺陷。[5b] 是在 M5 上**实测过**才写成判据的：
 *   原来那版 [5] 用的是栈上的视图，而"释放栈上的指针"在内存池里恰好是无声的
 *   （`free_mem` 拿指针前面的垃圾当块头读，读不出事），所以 M5 一开始是漏的。
 *
 * [2] 里的自追加（`b.append(b)`）原先结果是错的（源指针在搬块之后就失效了），
 * 一并修掉 —— 它和容量是同一件事：都是"搬块之后谁还想得起来源在哪"。
 *
 * 本用例覆盖不到、如实记录的边界：
 *   - `set_data` 的"不持有"只有内部约定，没有运行时标记能让外部把它和自有块
 *     区分开；[5] 只能验证"视图 + 扩容 = 另起自有块"，验证不了别的。
 */
#include <iostream>
#include <cstring>
#include <string>
#include <chrono>
#include <uv.h>
#include "uvcpp/uvcpp_buf.h"
#include "uvcpp/uvcpp_alloc.h"

using namespace uvcpp;

static int g_fail = 0;

static void check(bool ok, const std::string& msg) {
  if (!ok) {
    ++g_fail;
    std::cout << "[fail] " << msg << "\n";
  }
}

static const size_t kChunk = 64;
static const size_t kChunks = 40000;       // 2.5 MB
/// 小规模那一轮：缺陷版本在 40000 这一档是**跑不完**的（内存池一路重新分配，
/// 直接 std::bad_alloc 终止进程），所以墙钟判据得在一个缺陷版本也能跑完的
/// 规模上再钉一次，否则"判据本身有效"就没有证据。
static const size_t kSmallChunks = 6000;   // 375 KB
/// 绝对墙钟上界（毫秒）。见文件头里的实测数字。40000 这档只作兜底：真正的
/// 区分度在小规模那档。
static const double kMaxAppendMs = 1500.0;
static const double kMaxAppendSmallMs = 30.0;

static char pat(size_t chunk, size_t j) {
  return static_cast<char>((chunk * 7 + j * 13) & 0xFF);
}

/// 逐块 append n 个 kChunk 字节的分片，返回耗时（毫秒）。
static double append_chunks(uvcpp_buf& b, size_t n) {
  char chunk[kChunk];
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < kChunk; ++j) {
      chunk[j] = pat(i, j);
    }
    b.append_data(chunk, kChunk);
  }
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0).count();
}

int main() {
  // 不带缓冲：用例中途死掉时，前面已经跑过的判据不能跟着缓冲一起消失。
  std::cout.setf(std::ios::unitbuf);
  std::cout << "[functional buf_capacity] start（内存池 "
            << UVCPP_ENABLE_MEMORY_POOL << "）\n";

  // ---------------------------------------------------------------------
  // [1a] 小规模的那一轮**必须排在前面**：缺陷版本在下面 [1b] 的规模上会直接
  //      终止进程，排后面就永远看不到墙钟判据自己的输出 —— 那样"判据有效"
  //      就只剩"进程死了"这一条证据。
  // ---------------------------------------------------------------------
  {
    // 注意不能叫 `small`：windows.h 里它是 `#define small char`。
    uvcpp_buf small_buf;
    const double ms = append_chunks(small_buf, kSmallChunks);
    std::cout << "[note] " << kSmallChunks << " 次 " << kChunk
              << " 字节 append：" << ms << " ms，size=" << small_buf.size() << "\n";
    check(small_buf.size() == kSmallChunks * kChunk, "[1a] append 之后的长度不对");
    check(ms < kMaxAppendSmallMs,
          "[1a] append 攒 375 KB 花了 " + std::to_string(ms) + " ms，超过 " +
              std::to_string(kMaxAppendSmallMs) + " ms（容量没有复用？）");
  }

  // ---------------------------------------------------------------------
  // [1b] 逐块 append：内容 + 代价
  // ---------------------------------------------------------------------
  {
    std::cout << "[1b] 逐块 append 到 " << (kChunks * kChunk / 1024) << " KB\n";
    uvcpp_buf big;
    const double ms = append_chunks(big, kChunks);

    const size_t want = kChunks * kChunk;
    std::cout << "[note] " << kChunks << " 次 " << kChunk << " 字节 append："
              << ms << " ms，size=" << big.size() << "（期望 " << want << "）\n";
    check(big.size() == want, "[1b] append 之后的长度不对");

    size_t bad = 0, bad_at = 0;
    const char* d = big.get_const_data();
    for (size_t i = 0; i < kChunks; ++i) {
      for (size_t j = 0; j < kChunk; ++j) {
        if (d[i * kChunk + j] != pat(i, j)) {
          if (bad == 0) bad_at = i * kChunk + j;
          ++bad;
        }
      }
    }
    check(bad == 0, "[1b] 内容不对：首个不符位置 " + std::to_string(bad_at) +
                        "，共 " + std::to_string(bad) + " 字节不符");
    // 前置断言：走到这里说明"攒了 2.56 MB"，不是"循环没跑"。
    check(d != nullptr && d[0] == pat(0, 0) && d[want - 1] == pat(kChunks - 1, kChunk - 1),
          "[1b] 首尾字节不符（前提：这一轮真的攒满了）");
    check(ms < kMaxAppendMs,
          "[1b] append 攒 2.56 MB 花了 " + std::to_string(ms) + " ms，超过 " +
              std::to_string(kMaxAppendMs) + " ms（容量没有复用？）");
  }

  // ---------------------------------------------------------------------
  // [2] append / append_data 的边界（0 字节、自追加）
  // ---------------------------------------------------------------------
  {
    uvcpp_buf b;
    b.append_data("ABCD", 4);
    b.append_data(nullptr, 0);
    check(b.size() == 4 && memcmp(b.get_const_data(), "ABCD", 4) == 0,
          "[2] 追加 0 字节不该改变内容");
    b.append(b);
    check(b.size() == 8 && memcmp(b.get_const_data(), "ABCDABCD", 8) == 0,
          "[2] 自追加结果不对");

    uvcpp_buf empty;
    uvcpp_buf dst;
    dst.append_data("xy", 2);
    dst.append(empty);
    check(dst.size() == 2 && memcmp(dst.get_const_data(), "xy", 2) == 0,
          "[2] 追加空缓冲不该改变内容");
  }

  // ---------------------------------------------------------------------
  // [3] out_uv_buf 交出所有权之后：那块内存不能再被这个对象碰到
  //
  // 真正拦住这件事的是复用分支的 `base != nullptr` 前置（交出去时 base 被置空）。
  // `capacity_ = 0` 是同一个不变量的另一半：[3][4] 的变异（去掉清零）实测是
  // **等价变异** —— base 已经是空了，残留的容量取不到。两条都留着，是因为
  // 不能指望"以后每个改这条复用分支的人都会记得那条前置"。
  // ---------------------------------------------------------------------
  {
    uvcpp_buf o;
    o.append_data("AAAA", 4);
    uv_buf_t* raw = o.out_uv_buf();
    check(raw != nullptr && raw->len == 4 && raw->base != nullptr, "[3] out_uv_buf 结果不对");
    char* given = raw->base;
    o.append_data("BBBB", 4);  // 交出去之后再写
    check(o.size() == 4 && o.get_const_data() != given,
          "[3] 交出块之后再 append 用回了旧块（capacity_ 没清零）");
    check(memcmp(given, "AAAA", 4) == 0,
          "[3] 已经交出去的块被改写了");
    uvcpp_buf::free_buf(raw);
    uvcpp_free_bytes(raw);
  }

  // ---------------------------------------------------------------------
  // [4] move_buf：容量跟着块一起走（源与目标的边界见 [3] 的说明）
  // ---------------------------------------------------------------------
  {
    uvcpp_buf s;
    s.append_data("AAAA", 4);
    uvcpp_buf d;
    d.move_buf(s);
    char* moved = d.get_data();
    check(s.size() == 0 && s.get_data() == nullptr, "[4] move_buf 之后源不是空的");
    check(d.size() == 4 && memcmp(moved, "AAAA", 4) == 0, "[4] move_buf 之后目标内容不对");

    s.append_data("BBBB", 4);  // 源再写
    check(s.get_const_data() != moved, "[4] 源在 move 之后又用回了交出去的块");
    check(memcmp(moved, "AAAA", 4) == 0, "[4] 交出去的块被源改写了");

    uvcpp_buf d2;
    d2.move_buf(d);
    d.append_data("CCCC", 4);
    check(d2.size() == 4 && memcmp(d2.get_const_data(), "AAAA", 4) == 0,
          "[4] 二次 move 之后目标内容不对");
    // move 之后目标要能接着追加（容量是真的转移过来了，不是只搬了指针）
    d2.append_data("EE", 2);
    check(d2.size() == 6 && memcmp(d2.get_const_data(), "AAAAEE", 6) == 0,
          "[4] move 之后追加的结果不对");
  }

  // ---------------------------------------------------------------------
  // [5] set_data 是外部视图：扩容要另起自有块，且不许动外部内存
  // ---------------------------------------------------------------------
  {
    char ext[8];
    memcpy(ext, "ABCDEFG", 7);
    ext[7] = '\0';
    uvcpp_buf v;
    v.set_data(ext, 7);  // 栈上的地址，绝不是它自己的块
    check(v.size() == 7 && v.get_const_data() == ext, "[5] set_data 之后不是外部视图");
    v.append_data("HI", 2);
    check(v.size() == 9 && memcmp(v.get_const_data(), "ABCDEFGHI", 9) == 0,
          "[5] 外部视图 + append 的内容不对");
    check(v.get_const_data() != ext, "[5] 扩容时写进了外部缓冲");
    check(memcmp(ext, "ABCDEFG", 7) == 0, "[5] 外部缓冲被改写");

    uvcpp_buf w;
    w.set_data(nullptr, 0);
    check(w.size() == 0, "[5] set_data(nullptr, 0) 之后长度应为 0");
  }

  // ---------------------------------------------------------------------
  // [5b] 上面那条只证明"写没写外部缓冲"，证不了"有没有把外部块还回分配器"：
  //      栈上的块被 free 掉正好是**无声**的（内存池的 free_mem 会拿着指针前面
  //      的垃圾当块头读，多数情况下读不出什么事来）。所以这里用一块**真的从
  //      分配器要来的**内存做视图 —— 它若被还回去，紧接着的同尺寸分配就会把
  //      同一块再发一次。
  // ---------------------------------------------------------------------
  {
    char* ext2 = (char*)uvcpp_alloc_bytes(64);
    memcpy(ext2, "OWNED", 5);
    uvcpp_buf v2;
    v2.set_data(ext2, 64);
    v2.append_data("tail", 4);
    check(v2.size() == 68, "[5b] 视图 + append 之后的长度不对");
    char* again = (char*)uvcpp_alloc_bytes(64);
    check(again != ext2,
          "[5b] 外部视图的块被扩容路径还给了分配器（同尺寸分配又把它发了出去）");
    uvcpp_free_bytes(again);
    // ext2 刻意不释放：修好的代码里它一直活着（谁分配谁释放），而变异版本里它
    // 已经被还回去了 —— 这里再释放一次就变成二次释放，那是变异自己造成的。
  }

  // ---------------------------------------------------------------------
  // [6] clone_buf 是深拷贝；clone_data / clone 的内容
  // ---------------------------------------------------------------------
  {
    uvcpp_buf src;
    src.append_data("hello", 5);
    uvcpp_buf cp;
    cp.clone_buf(src);
    check(cp.size() == 5 && memcmp(cp.get_const_data(), "hello", 5) == 0,
          "[6] clone_buf 内容不对");
    check(cp.get_data() != src.get_data(), "[6] clone_buf 没有另起一块（不是深拷贝）");
    src.rewrite_data(0, "J", 1);
    check(memcmp(cp.get_const_data(), "hello", 5) == 0, "[6] 改源影响了副本");

    uvcpp_buf c2;
    c2.clone(src);
    check(c2.size() == 5 && memcmp(c2.get_const_data(), "Jello", 5) == 0,
          "[6] clone(uvcpp_buf) 内容不对");
    uvcpp_buf c3;
    c3.clone_data("world", 5);
    check(c3.size() == 5 && memcmp(c3.get_const_data(), "world", 5) == 0,
          "[6] clone_data 内容不对");
    uv_buf_t raw;
    raw.base = const_cast<char*>("rawdata");
    raw.len = 7;
    uvcpp_buf c4;
    c4.clone_data(raw);
    check(c4.size() == 7 && memcmp(c4.get_const_data(), "rawdata", 7) == 0,
          "[6] clone_data(uv_buf_t) 内容不对");
    uvcpp_buf empty;
    uvcpp_buf c5;
    c5.clone_data(nullptr, 0);
    c5.clone(empty);
    c5.clone_buf(empty);
    check(c5.size() == 0, "[6] 拷贝空缓冲之后长度应为 0");
  }

  // ---------------------------------------------------------------------
  // [7] resize 复用容量：缩小再放大，新露出来的部分必须清零
  // ---------------------------------------------------------------------
  {
    uvcpp_buf r;
    r.append_data("ABCDEFGH", 8);
    r.resize(4);
    check(r.size() == 4 && memcmp(r.get_const_data(), "ABCD", 4) == 0,
          "[7] 缩小之后的内容不对");
    r.resize(8);
    const char want[8] = {'A', 'B', 'C', 'D', 0, 0, 0, 0};
    check(r.size() == 8 && memcmp(r.get_const_data(), want, 8) == 0,
          "[7] 放大回去时新露出来的字节没有清零");
    r.resize(4);  // 不在可见长度内的字节不该影响 to_string
    check(r.to_string() == "ABCD", "[7] to_string 长度不对");

    uvcpp_buf f;
    for (int i = 0; i < 100; ++i) {
      f.resize(1000);
      f.resize(10);
    }
    check(f.size() == 10, "[7] 反复 resize 之后的长度不对");
    f.resize(0);
    check(f.size() == 0 && f.get_data() == nullptr, "[7] resize(0) 之后不是空的");
    uvcpp_buf g;
    g.append_data("zz", 2);
    g.clear();
    check(g.size() == 0 && g.get_data() == nullptr, "[7] clear 之后不是空的");
  }

  // ---------------------------------------------------------------------
  // [8] 走 resize 的既有接口：insert_data / rewrite_data / operator+
  // ---------------------------------------------------------------------
  {
    uvcpp_buf i;
    i.append_data("ABCDEF", 6);
    i.insert_data(3, "xy", 2);
    check(i.size() == 8 && memcmp(i.get_const_data(), "ABCxyDEF", 8) == 0,
          "[8] insert_data 结果不对");
    i.insert_data(i.size(), "!", 1);  // 追加位
    check(i.size() == 9 && memcmp(i.get_const_data(), "ABCxyDEF!", 9) == 0,
          "[8] insert_data 到末尾的结果不对");
    i.rewrite_data(0, "ZZ", 2);
    check(memcmp(i.get_const_data(), "ZZCxyDEF!", 9) == 0, "[8] rewrite_data 结果不对");
    i.rewrite_data(i.size() - 1, "?", 1);
    check(memcmp(i.get_const_data(), "ZZCxyDEF?", 9) == 0, "[8] 末尾 rewrite_data 结果不对");

    uvcpp_buf a("abc"), b("def");
    uvcpp_buf s = a + b;
    check(s.size() == 6 && memcmp(s.get_const_data(), "abcdef", 6) == 0,
          "[8] operator+ 结果不对");

    uvcpp_buf asn;
    asn = i;
    check(asn.size() == 9 && memcmp(asn.get_const_data(), "ZZCxyDEF?", 9) == 0,
          "[8] 拷贝赋值结果不对");
    asn = asn;  // 自赋值
    check(asn.size() == 9 && memcmp(asn.get_const_data(), "ZZCxyDEF?", 9) == 0,
          "[8] 自赋值之后内容不对");
  }

  // ---------------------------------------------------------------------
  // [9] 复用容量时，上一轮写过的字节必须被清掉
  //
  // 序列是刻意排的：先让 [8,10) 变成**曾经可见**的内容，再缩小、再放大到
  // 落在同一个已分配块里 —— 那两个字节能证明"复用分支确实清了零"，而不是
  // 靠"新分配的内存本来可能是 0"。去掉复用分支的 memset，这里必红。
  // ---------------------------------------------------------------------
  {
    uvcpp_buf p;
    p.append_data("SECRET", 6);  // 首次分配：容量恰好 6
    p.resize(12);                // 翻倍到 12
    p.rewrite_data(8, "XY", 2);  // [8,10) = "XY"（此刻可见）
    p.resize(6);                 // 缩小：块保留，[8,10) 不再是可见内容
    check(p.to_string() == "SECRET", "[9] 缩小之后 to_string 不对");
    p.resize(10);  // 放大回去，落在同一个块里
    size_t nz = 0;
    for (size_t k = 6; k < 10; ++k) {
      if (p.get_const_data()[k] != 0) ++nz;
    }
    check(nz == 0, "[9] resize 复用容量时新露出来的区域残留了 " +
                       std::to_string(nz) + " 个非零字节（上一轮的 'XY'）");
  }

  if (g_fail != 0) {
    std::cout << "[functional buf_capacity] FAILED：" << g_fail << " 条判据\n";
    return 1;
  }
  std::cout << "[functional buf_capacity] all checks passed\n";
  return 0;
}
