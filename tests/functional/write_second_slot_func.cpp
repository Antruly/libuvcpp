/**
 * @file tests/functional/write_second_slot_func.cpp
 * @brief uvcpp_write 的第 2 块槽位：换占用者时必须把前一个放掉。
 *
 * `append_uv_buf_owned()` 与 `append_uv_buf_view()` 都是"把第 2 块交给本请求"
 * 的入口，而两者的实现都是往**同一个槽位**直接赋值：
 *
 *     second_owner = bf;    // 上一块的头就此没人放了
 *     hold_        = hold;  // 上一份引用被放掉，但 second_owner 不会被清
 *
 * 于是"再调一次"这件事在两种入口下各有各的错：
 *
 *   - **owned → owned：第一块泄漏。** 析构只放 `second_owner` 指的那一个
 *     （`~uvcpp_write` 里那两句），被覆盖掉的那个指针连同它 `base` 指的整块
 *     缓冲再也没人释放。第二块走的是"整条响应体"这种尺寸，泄漏量与响应体
 *     同阶，不是漏几个字节的头。
 *   - **view → owned：`hold_` 不会被放掉。** 那一份 `shared_ptr` 会被本请求
 *     白持到析构，而它早就不再是第 2 块了。
 *
 * 树内今天只有 `uvcpp_tcp_client::write` 一处调它、且只调一次，所以线上打不到
 * —— 但两个都是**公开 API**，"第二次调用"不是一个需要费劲才能构造的用法，
 * 而它的后果（泄漏 / 白持）在单次调用的路径上完全看不出来。
 *
 * ---------------------------------------------------------------------------
 * 判据怎么看见"被释放了"：
 *
 * 泄漏是"少了一次 free"，没有返回值、没有断言可看，所以本用例**不**靠
 * "跑完之后感觉没事"来判 —— 它读内存池**向系统要的总字节数**
 * （`get_stats()` 的第 1 个出参：要 span 时涨、整段归还时落回去）。
 *
 * 块故意取 300 KiB（走大对象路径，一块独占一段 span），于是**漏掉的那一块**会让
 * 它那段 span 永远不归还，字节数就停在涨过的地方：
 *
 *   | 判据 | 期望 | 缺陷版本 |
 *   |---|---|---|
 *   | [0] 对照组：故意不释放 8 × 300 KiB   | 计数**必须涨过 1 MiB** | 绿（对照）|
 *   | [1] owned → owned ×8，析构后         | 涨**不得超过 1 MiB** | **红 2490368** |
 *   | [3] owned → view ×8，析构后          | 涨**不得超过 1 MiB** | 绿（护栏）|
 *
 * [3] 在缺陷版本上**本来就是绿的**（`second_owner` 还指着那块，析构会放掉它），
 * 它不是缺口判据，是**护栏**：修复如果放错了对象（例如把新接手的那块放了），
 * 它会红。同理 [4] 是"同一指针交接两次不得变成 double free"的护栏 ——
 * 那一条在缺陷版本上也是绿的，所以它证明不了缺口，只约束修法。
 *
 * [0] 是这份用例里最要紧的一条：没有它，[1][3] 在"计数器根本没动"（量错了对象、
 * 池没生效、单位变了）时也会全绿 —— 一个恒不涨的判据什么都没判。
 * **这条不是假设出来的**：本判据的第一版读的是 `get_stats()` 的**活跃块数**，
 * [0] 当场红给我看 —— 那个计数在进程里已经是负的（`-16`，它有重复扣减），
 * 于是它对 64 字节的分配根本不响应。换成总字节数之后 [0] 才是有牙的。
 *
 * 计数只在内存池开启时存在。默认树（`build-webapp`，走 `UVCPP_BUILD_EXPAND=ON`）
 * 是**开着**的，所以 [1][3] 在默认验证路径上有效；内存池关掉的那种构建里它们
 * 会**明确打印跳过**，不冒充通过 —— 那正是本仓 `web_ssl_*` 用例吃过一次的亏
 * （`#else` 的 SKIP main 让"没测过"表现为"通过"）。
 *
 * [2][4][5][6] 与分配器无关，两种构建下都跑。
 */
#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <cstring>
#include <uv.h>
#include "uvcpp/uvcpp_buf.h"
#include "uvcpp/uvcpp_alloc.h"
#include "req/uvcpp_write.h"
#if UVCPP_ENABLE_MEMORY_POOL
#include <expand/uvcpp_page_heap.h>
#endif

using namespace uvcpp;

static int g_fail = 0;

static void check(bool ok, const std::string &msg) {
  if (!ok) {
    ++g_fail;
    std::cout << "[fail] " << msg << "\n";
  }
}

#if UVCPP_ENABLE_MEMORY_POOL
/// 内存池向系统要过的总字节数（要 span 时涨，整段归还时落回去）。
static size_t bytes_from_system() {
  size_t total = 0, in_use = 0, free_spans = 0;
  uvcpp_memory_pool_enterprise::instance().get_stats(total, in_use, free_spans);
  return total;
}

/// 大对象档：一块独占一段 span，所以漏掉一块 = 一段 span 永远不归还。
static const size_t kBigBlock = 300 * 1024;
static const int kRounds = 8;
/// 与 `expand_memory_pool_func.cpp` 同一个余量：留给 span 管理与线程缓存。
static const size_t kSlack = 1024 * 1024;
#endif

/// 造一块"第 2 块"用的自有缓冲：内容固定，尺寸可指定。
static uv_buf_t *make_owned_block(size_t n, char fill) {
  uvcpp_buf b;
  std::string s(n, fill);
  b.clone_data(s.data(), s.size());
  return b.out_uv_buf();  // 块连同所有权交出来
}

int main() {
  // ---------------------------------------------------------------------
  // [5] 树内真实用法：只 append 一次。这是**对照组** —— 它必须一直是绿的，
  //     修复不该动这条路上的任何东西。
  // ---------------------------------------------------------------------
  {
    uvcpp_write w;
    uvcpp_buf head;
    head.clone_data("H", 1);
    w.set_uv_buf(head.out_uv_buf(), true);

    uv_buf_t *body = make_owned_block(64, 'A');
    w.append_uv_buf_owned(body);

    check(w.get_uv_nbufs() == 2, "[5] 只调一次时块数应为 2");
    uv_buf_t *bufs = w.get_uv_bufs();
    check(bufs[1].len == 64 && bufs[1].base != nullptr,
          "[5] 第 2 块的内容范围不对");
    check(memcmp(bufs[1].base, std::string(64, 'A').data(), 64) == 0,
          "[5] 第 2 块的字节不对");
  }

  // ---------------------------------------------------------------------
  // [6] 换占用者之后，交出去的数组必须是"第 1 块 + 新交接的那块"。
  //
  // 这条与所有权无关，两种构建都跑 —— 它钉住"新占用者确实接上了"，
  // 免得修复把第 2 块一起丢掉（那会让 [1][3] 因为"根本没接上"而变绿）。
  // ---------------------------------------------------------------------
  {
    uvcpp_write w;
    uvcpp_buf head;
    head.clone_data("H", 1);
    w.set_uv_buf(head.out_uv_buf(), true);

    uv_buf_t *first = make_owned_block(32, 'A');
    w.append_uv_buf_owned(first);
    uv_buf_t *second = make_owned_block(48, 'B');
    w.append_uv_buf_owned(second);

    check(w.get_uv_nbufs() == 2, "[6] 换占用者之后块数仍应为 2");
    uv_buf_t *bufs = w.get_uv_bufs();
    check(bufs[1].len == 48, "[6] 第 2 块应是后交接的那块（长度 48）");
    check(memcmp(bufs[1].base, std::string(48, 'B').data(), 48) == 0,
          "[6] 第 2 块的字节应是后交接那块的内容");
  }

  // ---------------------------------------------------------------------
  // [2] view → owned：本请求不再持那份引用了，`hold_` 必须被放掉。
  //
  // 判据用 `use_count()`：测试自己留着那个 `shared_ptr`，所以它是**外面**能
  // 看见的读数。修复前这里是 2（本请求还持着），修复后是 1。
  // ---------------------------------------------------------------------
  {
    uvcpp_write w;
    uvcpp_buf head;
    head.clone_data("H", 1);
    w.set_uv_buf(head.out_uv_buf(), true);

    auto sp = std::make_shared<const std::string>(std::string(40, 'V'));
    w.append_uv_buf_view(
        uv_buf_init(const_cast<char *>(sp->data()), sp->size()), sp);
    check(sp.use_count() == 2,
          "[2] 前提不成立：append_uv_buf_view 之后本请求应持有一份引用");

    uv_buf_t *owned = make_owned_block(24, 'O');
    w.append_uv_buf_owned(owned);

    check(sp.use_count() == 1,
          "[2] 第 2 块改成 owned 之后，旧的共享引用没被放掉（use_count=" +
              std::to_string(sp.use_count()) +
              "）—— 本请求会白持到析构");
  }

  // ---------------------------------------------------------------------
  // [4] 同一个指针交接两次：不得双重释放。
  //
  // "把同一块的所有权交出去两次"本来就是调用方的错，但修复不能把一次错误
  // 变成一次 double free —— 那比泄漏严重得多，而且在池上直接崩。
  // 这条在**缺陷版本**上是绿的（它只 free 一次），所以它不证明缺口，
  // 它是**修复的护栏**：任何"先放旧的再接管新的"的实现都必须先比一下指针。
  // ---------------------------------------------------------------------
  {
    uvcpp_write w;
    uvcpp_buf head;
    head.clone_data("H", 1);
    w.set_uv_buf(head.out_uv_buf(), true);

    uv_buf_t *body = make_owned_block(16, 'S');
    w.append_uv_buf_owned(body);
    w.append_uv_buf_owned(body);  // 同一个头，第二次

    check(w.get_uv_nbufs() == 2, "[4] 同一指针交接两次之后块数应为 2");
    uv_buf_t *bufs = w.get_uv_bufs();
    check(bufs[1].len == 16 && memcmp(bufs[1].base, std::string(16, 'S').data(), 16) == 0,
          "[4] 同一指针交接两次之后第 2 块的内容不对");
  }  // 析构：必须只释放一次（双释放会在池上崩，在 malloc 上多半也崩）

#if UVCPP_ENABLE_MEMORY_POOL
  // ---------------------------------------------------------------------
  // [0] 对照组：这个计数器**有牙**吗？
  //
  // 故意不释放 8 × 300 KiB，`bytes_from_system()` 必须涨过 1 MiB。不先跑
  // 这一条，[1][3] 在"计数器根本没动"时也会全绿 —— 那两条判据就什么都没判。
  // ---------------------------------------------------------------------
  {
    const size_t before = bytes_from_system();
    std::vector<void *> leaked;
    for (int i = 0; i < kRounds; ++i) {
      void *p = uvcpp_alloc_bytes(kBigBlock);
      std::memset(p, 0x5A, kBigBlock);
      leaked.push_back(p);
    }
    const size_t after = bytes_from_system();
    check(after > before + kSlack,
          "[0] 对照组失效：故意漏掉 " + std::to_string(kRounds) + " × " +
              std::to_string(kBigBlock) + " 字节之后，向系统要的字节数没涨过 " +
              std::to_string(kSlack) + "（" + std::to_string(before) + " -> " +
              std::to_string(after) + "）—— 下面 [1][3] 就什么都没判");
    for (void *p : leaked) {
      uvcpp_free_bytes(p);  // 别把这份对照泄漏留给后面的判据
    }
  }

  // ---------------------------------------------------------------------
  // [1] owned → owned：析构之后，那几段 span 必须还回去。
  //
  // 这一条就是缺陷本体。缺陷版本里每一轮的第一块既没被 free 也没出现在任何
  // 地方，于是它独占的那段 span 永远不归还 —— 8 轮就是 2.4 MiB。
  // ---------------------------------------------------------------------
  {
    const size_t before = bytes_from_system();
    for (int i = 0; i < kRounds; ++i) {
      uvcpp_write w;
      uvcpp_buf head;
      head.clone_data("H", 1);
      w.set_uv_buf(head.out_uv_buf(), true);

      uv_buf_t *first = make_owned_block(kBigBlock, 'A');
      w.append_uv_buf_owned(first);
      uv_buf_t *second = make_owned_block(kBigBlock, 'B');
      w.append_uv_buf_owned(second);
    }
    const size_t after = bytes_from_system();
    const size_t growth = (after > before) ? (after - before) : 0;
    check(growth <= kSlack,
          "[1] owned → owned 之后第一块泄漏了：向系统要的字节数涨了 " +
              std::to_string(growth) + "（" + std::to_string(kRounds) +
              " 轮 × " + std::to_string(kBigBlock) + " 字节）");
  }

  // ---------------------------------------------------------------------
  // [3] owned → view：同上，前一块同样必须被放掉。
  // ---------------------------------------------------------------------
  {
    const size_t before = bytes_from_system();
    for (int i = 0; i < kRounds; ++i) {
      uvcpp_write w;
      uvcpp_buf head;
      head.clone_data("H", 1);
      w.set_uv_buf(head.out_uv_buf(), true);

      uv_buf_t *owned = make_owned_block(kBigBlock, 'A');
      w.append_uv_buf_owned(owned);

      auto sp = std::make_shared<const std::string>(std::string(kBigBlock, 'V'));
      w.append_uv_buf_view(
          uv_buf_init(const_cast<char *>(sp->data()), sp->size()), sp);
    }
    const size_t after = bytes_from_system();
    const size_t growth = (after > before) ? (after - before) : 0;
    check(growth <= kSlack,
          "[3] owned → view 之后前一块泄漏了：向系统要的字节数涨了 " +
              std::to_string(growth));
  }
#else
  std::cout << "[skip] [0][1][3] 需要内存池（向系统要的字节数）—— 本构建 "
               "UVCPP_ENABLE_MEMORY_POOL=0，这三条没跑\n";
#endif

  if (g_fail != 0) {
    std::cout << "[functional write_second_slot] FAILED：" << g_fail << " 条判据\n";
    return 1;
  }
  std::cout << "[functional write_second_slot] all checks passed\n";
  return 0;
}
