/**
 * @file tests/functional/tcp_try_write_fastpath_func.cpp
 * @brief `uv_try_write` 快速路径：打开它**不得改变任何可观察行为**。
 *
 * 快速路径做的事很小 —— `write()` 里先用 `uv_try_write` 试一次，套接字一次
 * 吃得下整条就不必把字节拷进自有缓冲（那一份拷贝是它唯一的收益）。吃下的部分
 * 越大，要拷的那份越小；全吃下就是不拷。
 *
 * **完成回调从哪来这件事一点没变**：吃下之后余量（整条吃下时为 0）照旧交给
 * 一次真正的 `uv_write`，所以回调仍由 libuv 自己的写完成机制交付 —— 时机、
 * 背压记账、`write_reqs_pending` 全部原样。（另有一版让懒建的 `uvcpp_idle` 在
 * 下一轮交付，省下的拷贝一样多，但**交付会晚一轮**，而晚一轮是可观测的：
 * `uvcpp_ws_server::handle_upgrade` 在 101 那笔写的完成回调里建 WS 会话，
 * 晚一轮就出现"对端已收到 101、服务端会话还没建"的窗口 —— 同一份二进制里切换
 * 交付方式实测，`web_ws_deflate_func` 由 20/20 掉到 11/20（另 5 次断言失败、
 * 4 次段错误）。所以那一版被否掉，不是因为它省得少，而是因为它动了时序。）
 *
 * **契约必须一模一样**：
 *
 *   1. 交付总是在 `write()` 返回**之后**（同步交付会让"回调里接着写下一笔"
 *      这种最自然的用法变成递归）；
 *   2. 乱序、少交、多交都不允许；
 *   3. 非 0 返回码的含义不变 —— **非 0 ⇒ 这次写没提交**。所以 `uv_try_write`
 *      的负返回（EAGAIN / EALREADY / 真实错误）一概不许从 `write()` 漏出去；
 *   4. 部分接受时**余量只能是 `data + n, len - n`**。
 *
 * 第 4 条是这份用例存在的主要理由。把余量写成整条 `data, len` 是一种"缓冲不紧
 * 时完全看不出来、一紧就错"的形状：前 n 个字节会**发两遍**，对端收到重复内容。
 * 所以第 2 条判据刻意制造"对端不读、内核缓冲装不下"的窗口，让 `uv_try_write`
 * 只能部分接受。
 *
 * ---------------------------------------------------------------------------
 * 长度门槛（`UVCPP_TRY_WRITE_MIN_BYTES`，默认 32 KiB）：
 *
 * 快路径不是"报文越短越该用"。它每笔写固定**多**一次系统调用（整条吃下时
 * 那次 `uv_write` 走 0 长度，而 libuv 在两个平台上都不对 0 长度短路），换来
 * 的是省掉一份 `len` 字节的拷贝 —— 两项相抵的零点在 16 KiB 附近（详细的账见
 * `.cpp` 里那段注释），所以长度没过门槛的报文**连试都不试**。本用例里 16 字节
 * 那几笔走的就是这条"不试发"支路，4 MiB 那一档走试发支路，两条都有判据看着。
 * ---------------------------------------------------------------------------
 *
 * ---------------------------------------------------------------------------
 * 关于"变异撞红"：本文件里每条判据都能被一次**定向**的改坏撞红。下表的每一行
 * 都在本机**真跑过**（改一处、重编、跑全量用例，跑完还原）：
 *
 * | 改坏点什么 | 撞红的判据 | 实测结果 |
 * |---|---|---|
 * | 快路径里内联交付（自己同步调 `fn(0, arg)`） | 判据 1b、1e、1 收尾 | 红：第二笔写拿到 rc=0（在途标志被提前清掉）、链式写深度涨到 43、字节总数多出 16 |
 * | 余量算成整条 `data, len` | 判据 1 收尾、2 | 红：4 MiB 那档对端收到 8388608 字节，正好是 4194304 的两倍（重发前缀） |
 * | 负返回从 `write()` 漏出去 | 判据 1e、1 收尾、2、3、4 | 红：`write()` 直接返回 `-4084`（`UV_EALREADY`，客户端状态被毒化），一共 20 条判据连锁红 |
 * | 门槛写反（小报文反而试发、大报文不试发） | 判据 1c | 红：4000 笔 16 字节报文 attempted=4000、skipped=0 |
 * | 快路径整个去掉（源码里改成 `#if 0`） | 判据 1c、1d | 红：skipped=0 且 attempted=0 —— 覆盖率判据把它抓住了 |
 * | 用开关关掉（`UVCPP_ENABLE_TRY_WRITE=OFF`，不是改坏） | —— | 全绿，这是对的，见下 |
 *
 * **"用开关把快路径整个关掉"撞不红任何行为判据 —— 这是对的，不是漏了。**
 * 快速路径是纯粹的性能改动，可观察语义**按设计**与关掉时完全一致；黑盒用例
 * 本来就分不出二者（真能分出来，说明有别的行为差异，那才是缺陷）。所以
 * "快路径真的被走过"不能靠行为断言锁，只能靠**运行期覆盖率**。
 *
 * 覆盖率用产品侧的只读计数 `uvcpp_tcp_client::try_write_stats()`
 * （`attempted` / `consumed` / `skipped` / `bytes`，进程级累计）：判据 1c 断言
 * "小报文一律不试发"，判据 1d 断言"大报文真的试发过、且真的吃下过字节"。开关
 * 关掉时四个计数**恒为 0**，两种开关下各断言一次。
 *
 * （曾经有过一版覆盖率断言是死的：它挂在 `#ifdef UVCPP_TEST_COPYPROBE` 里，
 * 而那个宏只有带拷贝探针的本机构建才定义、探针不在上游 —— 于是上游 CI 上
 * "快路径失效"没有任何一条判据看着。作者在 PR #15 里选了"给产品加只读计数"
 * 这条更小的路，(b) 而不是把探针提上来 (a)。）
 * ---------------------------------------------------------------------------
 *
 * ---------------------------------------------------------------------------
 * 为什么 `UVCPP_ENABLE_TRY_WRITE=OFF` 时**不**把本用例从目标列表里摘掉：
 *
 * 本仓的既有约定是"关掉某模块就把它对应的用例从 `FUNC_SOURCES` 里摘掉，让
 * 「没测」表现为用例不存在，而不是表现为一个空跑的 `#else` 分支通过"。**这里
 * 是那条约定的例外，理由恰好是反过来的**：本用例断言的全是"两种实现下都
 * 必须成立"的契约，所以关掉开关时它**照样要跑、照样要绿** —— 那正是"打开开关
 * 不改变任何可观察行为"这句话唯一的证据。要是也把它摘掉，"关掉开关时那些
 * 不变量被破坏"就没人看着了。
 *
 * 代价是关掉开关时文件名里的 `fastpath` 有点名不副实：它测的是**契约**，不是
 * 快路径本身（后者见上面那段 ⚠️）。
 * ---------------------------------------------------------------------------
 */
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_tcp.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <req/uvcpp_write.h>
#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_define.h>
#include "loop_drain.h"
#include "wait_util.h"

using namespace uvcpp;
using namespace uvcpp_test;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

const char kMsg[] = "0123456789abcdef";
const size_t kMsgLen = sizeof(kMsg) - 1;

/// "大报文"的长度：必须**过门槛**才会走试发支路，所以由门槛本身派生出来，
/// 改 `UVCPP_TRY_WRITE_MIN_BYTES` 时用例不用跟着改。默认 64 KiB。
const size_t kBigLen = uvcpp_tcp_client::kTryWriteMinBytes * 2;

/// 判据 1e 那条大报文链的链长（写在整个链的回调里接着发起下一笔）。
const int kDeepChain = 64;

/// FNV-1a 64 位。用一个**位置相关**的图案，于是"重复了一段前缀"和"少了一段"
/// 这两种错都会改变哈希，不只是字节数对不上。
const uint64_t kFnvBasis = 14695981039346656037ULL;

inline uint64_t fnv1a(uint64_t h, const char* p, size_t n) {
  const unsigned char* q = reinterpret_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; ++i) {
    h ^= q[i];
    h *= 1099511628211ULL;
  }
  return h;
}

inline char pattern_at(size_t i) {
  return static_cast<char>((i * 131u) ^ (i >> 7) ^ (i >> 19));
}

/// 造一段 `len` 字节的位置相关图案（重发前缀、少发一段都会改变哈希）。
inline std::vector<char> make_pattern(size_t len, size_t salt) {
  std::vector<char> v(len);
  for (size_t i = 0; i < len; ++i) v[i] = pattern_at(i + salt);
  return v;
}

/**
 * @brief 服务端的收账本。
 *
 * **只有测试线程碰它**（服务端循环也是本线程泵的），所以用普通成员而不是
 * 原子量 —— 加了原子量反而会让人以为它支持跨线程。
 *
 * `capture` 非空时把新到的字节另存一份：判据 2 需要**逐字节**比对内容，
 * 而"收到了多少字节"这个数在"重发前缀"那种错下是会变的，光比字节数不够直观。
 */
struct sink {
  size_t   total  = 0;
  uint64_t hash   = kFnvBasis;
  int      chunks = 0;
  bool     eof    = false;
  std::vector<char>* capture = nullptr;
};

/// @brief 把两边泵到「对端账本静置」：连续 \p settle_ms 毫秒 \p s.total 不再涨。
///
/// **"排干"要用这一条。** 本文件原来那个 `pump_both(n)`（本次删除）收的是
/// **圈数**不是毫秒，它只保证"泵了 n 圈"，而"对端把已经发出去的字节收完"是
/// **另一件事** —— 完成包的投递与对端读出都比圈数慢。没排干的字节会窜进
/// **下一段的 capture 窗口**，报出来的形状与被测缺陷**逐字相同**。本文件因此
/// 假红过两次：判据 4 那次**当场抓到了输出**（对端只收到 458465/2162688）；
/// 判据 2 那次是对端**多**收 131072 = 2 × 64 KiB，成因当时只是推断 —— 两次
/// 的共同点都是"等的是本地、量的是对端"。
///
/// 上限是**墙钟**（`wait_util.h` 那条约定）。返回值就是"前提有没有成立"，
/// 调用点可以把它断言出来。
inline bool drain_pair(uvcpp_loop* a, uvcpp_loop* b, const sink& s,
                       int settle_ms = 150, int limit_ms = 8000) {
  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  size_t last = s.total;
  long long last_change = 0;
  for (;;) {
    const long long now = elapsed_ms(t0);
    if (now >= limit_ms) return false;
    if (s.total != last) {
      last = s.total;
      last_change = now;
    } else if (now - last_change >= settle_ms) {
      return true;
    }
    pump_for_pair(a, b, 5);
  }
}

}  // namespace

int main() {
  std::cout << "[tcp_try_write_fastpath] start" << std::endl;

  auto progress = [](const char* what) {
    std::cout << "[tcp_try_write_fastpath] " << what << std::endl;
  };

  uvcpp_loop loop;

  {
    uvcpp_test::loop_drain drain_loop(&loop);
    loop.init();

    sink rec;
    uvcpp_tcp_server server;
    std::atomic<int> accepted{0};

    int rc = server.bindIpv4("127.0.0.1", 0);
    if (rc != 0) {
      std::cerr << "  [FAIL] server bind rc=" << rc << std::endl;
      return 2;
    }
    sockaddr_in name;
    int namelen = sizeof(name);
    server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
    const int port = ntohs(name.sin_port);

    rc = server.listen(
        [&rec, &accepted](uvcpp_tcp_client* cli) {
          accepted.fetch_add(1);
          // 读**装上**了，但数据进不进来由"服务端循环有没有被泵"决定 ——
          // 判据 2 利用这根闸门来制造"对端不读"的窗口。
          cli->read_start([&rec](uvcpp_buf* buf) {
            if (buf == nullptr) {
              rec.eof = true;
              return;
            }
            const size_t n = buf->size();
            rec.total += n;
            rec.chunks += 1;
            rec.hash = fnv1a(rec.hash, buf->get_const_data(), n);
            if (rec.capture != nullptr) {
              rec.capture->insert(rec.capture->end(), buf->get_const_data(),
                                  buf->get_const_data() + n);
            }
          });
        },
        128);
    if (rc != 0) {
      std::cerr << "  [FAIL] server listen rc=" << rc << std::endl;
      return 2;
    }

    uvcpp_tcp_client client(&loop);
    std::atomic<bool> connected{false};
    std::atomic<int>  connect_status{-99};

    rc = client.connect("127.0.0.1", port, [&](int st) {
      connect_status.store(st);
      connected.store(true);
    });
    if (rc != 0) {
      std::cerr << "  [FAIL] client connect rc=" << rc << std::endl;
      return 2;
    }

    wait_until_pair(server.get_loop(), &loop,
                    [&] { return connected.load() && accepted.load() > 0; },
                    kWaitMs);
    check(connect_status.load() == 0,
          "connect status = " + std::to_string(connect_status.load()));
    check(accepted.load() == 1,
          "server accepted " + std::to_string(accepted.load()) + " connections");
    if (connect_status.load() != 0) return 2;

    uvcpp_loop* sloop = server.get_loop();

    // =====================================================================
    // 判据 1：深队列 —— 交付次序、重入深度、在途写不得被顶掉
    // =====================================================================
    const int kN = 4000;
    {
      progress("判据1 深队列：4000 笔链式写（每笔都在上一笔的回调里发起）");

      // `live` 数的是"此刻压在栈上的完成回调有几个"。
      //
      // 正确的实现里它是恒为 1 的：回调从循环里被调到，回调里 `step(i+1)` 只是
      // **提交**下一笔就返回了，下一笔的回调是**另一帧**。同步交付的实现里
      // 它是 kN —— 每层回调都压在上一层里。
      int live = 0;
      int max_live = 0;
      int done = 0;
      int bad_status = 0;
      std::vector<int> order;
      order.reserve(kN);

      std::function<void(int)> step = [&](int i) {
        if (i >= kN) return;
        const int wrc = client.write(kMsg, kMsgLen, [&, i](int st) {
          ++live;
          if (live > max_live) max_live = live;
          if (st != 0) ++bad_status;
          order.push_back(i);
          ++done;
          step(i + 1);  // 回调里接着写下一笔 —— 最自然的用法
          --live;
        });
        if (wrc != 0) ++bad_status;
      };

      step(0);
      check(live == 0, "判据1: 起步调用返回后 live=" + std::to_string(live));

      // 两边都要泵：对端得读，否则内核缓冲一满链就停了。
      for (int i = 0; i < 100000 && done < kN; ++i) {
        sloop->run(UV_RUN_NOWAIT);
        loop.run(UV_RUN_NOWAIT);
      }

      std::cout << "  [note] 重入深度 max_live=" << max_live << "（kN=" << kN
                << "；同步交付的实现这里会逼近 " << kN << "）" << std::endl;

      check(max_live <= 2,
            "判据1: 完成回调被**同步交付**了 —— 回调栈深度涨到 " +
                std::to_string(max_live) +
                "。正确的实现里它只能是 1（每次交付都是新的一帧）");
      check(done == kN,
            "判据1: 链式写没跑完，done=" + std::to_string(done) + "/" +
                std::to_string(kN));
      check(bad_status == 0,
            "判据1: 有 " + std::to_string(bad_status) + " 笔写的状态非 0");

      bool in_order = (order.size() == static_cast<size_t>(kN));
      for (size_t i = 0; in_order && i < order.size(); ++i) {
        in_order = (order[i] == static_cast<int>(i));
      }
      check(in_order, "判据1: 完成次序与提交次序不一致（乱序交付）");
    }

    // ---------------------------------------------------------------------
    // 判据 1c：门槛 —— 小于门槛的报文**连试都不试**
    // ---------------------------------------------------------------------
    //
    // 跑到这里只发过 16 字节的报文（判据 1 的那 kN 笔），全都小于门槛。所以
    // 此刻的账必须是"试发次数一次都没动过、跳过的次数至少 kN 次"。
    //
    // 这条**不是**行为契约（快路径怎么走外面看不出来），它是覆盖率断言：产品侧
    // 的只读计数是这件事在上游唯一能自动断言的抓手。开关关掉时四个计数恒为 0
    // ——两种开关下各断言一次，"开着不改变语义"那句话才有证据。
    {
      const uvcpp_tcp_client::try_write_stat tw =
          uvcpp_tcp_client::try_write_stats();
      std::cout << "  [note] 小报文段之后的计数：attempted=" << tw.attempted
                << " consumed=" << tw.consumed << " skipped=" << tw.skipped
                << " bytes=" << tw.bytes << "（门槛 "
                << uvcpp_tcp_client::kTryWriteMinBytes << " 字节）" << std::endl;
#if UVCPP_TRY_WRITE_ENABLE
      check(tw.attempted == 0,
            "判据1c: 门槛失效 —— " + std::to_string(kN) + " 笔 " +
                std::to_string(kMsgLen) + " 字节的报文，试发被走了 " +
                std::to_string(tw.attempted) + " 次");
      check(tw.skipped >= static_cast<uint64_t>(kN),
            "判据1c: 小于门槛的报文没走'不试发'支路，skipped=" +
                std::to_string(tw.skipped) + "（应 ≥ " + std::to_string(kN) +
                "）");
#else
      check(tw.attempted == 0 && tw.consumed == 0 && tw.skipped == 0 &&
                tw.bytes == 0,
            "判据1c: 开关关掉了，四个计数应当恒为 0（attempted=" +
                std::to_string(tw.attempted) + " skipped=" +
                std::to_string(tw.skipped) + "）");
#endif
    }

    // ---------------------------------------------------------------------
    // 判据 1b：在途的那一笔不许被后一笔顶掉
    // ---------------------------------------------------------------------
    //
    // 快路径把"待交付的闭包"存在 `write_fn_` / `write_arg_` 里。要是忘了把
    // `has_async_write_cb_` 立起来，第二笔写会**覆盖**第一笔的闭包 —— 丢的是
    // **回调**而不是数据，所以平时完全看不出来（对端收到的字节照样对）。
    // 也正因为它不可见，才要单独钉一条。
    //
    // 第一笔用**过门槛的大报文**：门槛之下根本不进快路径，用 16 字节钉这条就
    // 等于没钉（第二笔拿到 UV_EALREADY 只是因为普通路径的在途标志）。第二笔的
    // 长度无所谓 —— 它在任何路径逻辑之前就被标志挡下了。
    const std::vector<char> big = make_pattern(kBigLen, 7);
    {
      std::atomic<int> a{0};
      std::atomic<int> b{0};
      const int rc1 = client.write(big.data(), big.size(),
                                   [&a](int) { a.fetch_add(1); });
      const int rc2 = client.write(kMsg, kMsgLen, [&b](int) { b.fetch_add(1); });
      check(rc1 == 0, "判据1b: 第一笔在途写提交失败 rc=" + std::to_string(rc1));
      check(rc2 == UV_EALREADY,
            "判据1b: 在途期间的第二笔写应当拿到 UV_EALREADY，实际 rc=" +
                std::to_string(rc2) +
                "（0 说明第一笔的待交付闭包被顶掉了，那一笔的回调永远不会响）");

      // 等的是**本地**那个回调（`a`），所以这里用条件等待而不是圈数。
      wait_until_pair(sloop, &loop, [&] { return a.load() != 0; }, 2000);
      check(a.load() == 1,
            "判据1b: 第一笔在途写的回调响了 " + std::to_string(a.load()) + " 次");
      check(b.load() == 0,
            "判据1b: 被拒的第二笔写居然交付了回调（" + std::to_string(b.load()) +
                " 次）");
      // 这笔 64 KiB 也要在对端收干净再往下走 —— 它落在判据 2 那条探测窗口
      // 之前，漏下去就会伪装成"重发的前缀"。
      drain_pair(sloop, &loop, rec);
    }

    // ---------------------------------------------------------------------
    // 判据 1d：快路径**真的被走过**（覆盖率断言，两种开关下各来一次）
    // ---------------------------------------------------------------------
    //
    // 上面判据 1b 那笔是过门槛的大报文，所以试发支路此刻**必然**被走过一次
    // ——不管套接字收下了多少。这一条替换掉的是原先那个挂在 `#ifdef
    // UVCPP_TEST_COPYPROBE` 里、在上游永远不编译的覆盖率断言（探针不在上游）。
    //
    // `consumed` / `bytes` 只记数、**不断言**：吃下多少取决于内核缓冲此刻有多
    // 空，那是平台相关的（Windows 上"整条吃下"之外的那一半根本到不了，见下面
    // 判据 2 那段）。"有没有被你走到"是可判的，"省了多少"不是。
    {
      const uvcpp_tcp_client::try_write_stat tw =
          uvcpp_tcp_client::try_write_stats();
      std::cout << "  [note] 大报文之后的计数：attempted=" << tw.attempted
                << " consumed=" << tw.consumed << " skipped=" << tw.skipped
                << " bytes=" << tw.bytes << std::endl;
#if UVCPP_TRY_WRITE_ENABLE
      check(tw.attempted > 0,
            "判据1d: 过门槛的大报文一笔都没试发 —— 快路径被去掉或被短路了");
#else
      check(tw.attempted == 0 && tw.consumed == 0 && tw.skipped == 0 &&
                tw.bytes == 0,
            "判据1d: 开关关掉了，四个计数应当恒为 0（attempted=" +
                std::to_string(tw.attempted) + "）");
#endif
    }

    // ---------------------------------------------------------------------
    // 判据 1e：**大报文**的链式写也不许递归（内联同步交付的落点）
    // ---------------------------------------------------------------------
    //
    // 判据 1 用 16 字节报文钉"回调里接着写下一笔"这种最自然的用法；门槛引入
    // 之后那几笔根本不进快路径，所以这里用大报文再钉一遍：把交付做成同步
    // （在快路径里自己调 `fn(0, arg)`）时，深度会等于链长，`max_live` 当场红。
    {
      progress("判据1e 大报文链式写：交付不得同步");
      const int kDeep = kDeepChain;
      int live = 0;
      int max_live = 0;
      int done = 0;
      std::atomic<int> bad_status{0};
      std::function<void(int)> step = [&](int i) {
        if (i >= kDeep) return;
        // `i` **必须按值捕获**：回调是在 `step(i)` 返回之后才响的，按引用捕获
        // 拿到的是已经出栈的形参 —— 那是悬垂引用，链会跑飞（实测 done 冲到 72）。
        const int wrc = client.write(big.data(), big.size(), [&, i](int st) {
          ++live;
          if (live > max_live) max_live = live;
          if (st != 0) bad_status.fetch_add(1);
          ++done;
          step(i + 1);
          --live;
        });
        if (wrc != 0) bad_status.fetch_add(1);
      };
      step(0);
      check(live == 0, "判据1e: 起步调用返回后 live=" + std::to_string(live));

      for (int i = 0; i < 200000 && done < kDeep; ++i) {
        sloop->run(UV_RUN_NOWAIT);
        loop.run(UV_RUN_NOWAIT);
      }
      std::cout << "  [note] 大报文链式写 max_live=" << max_live
                << "（链长 " << kDeep << "；同步交付会等于链长）" << std::endl;
      check(done == kDeep, "判据1e: 链式写没跑完，done=" + std::to_string(done) +
                               "/" + std::to_string(kDeep));
      check(max_live <= 2,
            "判据1e: 完成回调被**同步交付**了 —— 回调栈深度涨到 " +
                std::to_string(max_live) +
                "（正确的实现里每次交付都是新的一帧）");
      check(bad_status.load() == 0,
            "判据1e: 有 " + std::to_string(bad_status.load()) +
                " 笔写的状态非 0");
    }

    // 判据 2 之前先把判据 1 那几笔的字节在对端收干净，这样后面开 capture 之后
    // 收到的字节就**只**属于当前这一段。
    // 上限一律是**墙钟**且留足余量：整个用例要能在 `ctest --timeout 30` 里跑完，
    // 所以各处失败上限之和必须明显小于 30 秒（详见 wait_util.h 那段表）。
    //
    // 这个总数的断言同时是"余量算错"的落点：把余量写成整条 `data, len` 会把
    // 前 n 字节发两遍，总数直接对不上（判据 1b 那笔大报文、判据 1e 那 64 笔
    // 都在里面）。
    const size_t kAfterS1 = static_cast<size_t>(kN) * kMsgLen +
                            static_cast<size_t>(1 + kDeepChain) * kBigLen;
    wait_until_pair(sloop, &loop, [&] { return rec.total >= kAfterS1; }, 5000);
    check(rec.total == kAfterS1,
          "判据1 收尾: 对端收到 " + std::to_string(rec.total) + " 字节，应为 " +
              std::to_string(kAfterS1));

    // =====================================================================
    // 判据 2：部分接受的余量只能是 `data + n, len - n`
    // =====================================================================
    //
    // ---------------------------------------------------------------------
    // 先说一件**实测出来的、与直觉相反**的事（它决定了这条判据的形状）。
    //
    // 原本的设计是"4 MiB 远超内核能替对端保管的量，所以对端不读时
    // `uv_try_write` 只能吃下一部分" —— **这个前提在 Windows 回环上不成立**。
    // 单独一个探针（`tmp_twsend_probe`，见 PR 说明）量到：
    //
    //   - **单次** `uv_try_write` 的返回只有两种：整条吃下、或者负数。
    //     4 MiB / 16 MiB / 64 MiB 全部一次吃下；显式
    //     `setsockopt(SO_SNDBUF, 4096)`（getsockopt 读回 4096，确实设上了）
    //     之后**照样**一次吃下 64 MiB。对端一个字节都没读。
    //   - 反复调用**才会**撞墙，而且是**负数**：1 MiB 一笔，第 5 笔起
    //     `EAGAIN`（累计才吞了 4 MiB）。那是"整条回退"那一支，不是部分接受。
    //
    // 也就是说：Windows 回环上 `0 < n < len` **够不着**，`data + n` 那半段
    // 余量算术在本机是**死代码**（Linux 上 `send()` 的发送缓冲是硬上限，部分
    // 接受是常态，那半段是活代码）。
    //
    // 所以这一条分两层，**不能只留一层**：
    //   a. 内容一致性（哪个平台都跑）：4 MiB 一笔下去，对端收到的必须**不多
    //      不少、逐字节一致**。管的是"快路径整条吃下之后有没有重复/丢失/错序"。
    //   b. 部分接受可达性（先探测、再决定断不断言）：见下面 `partial_probe`。
    //      可达就断言"余量不得重发前缀"——`wp = data; wl = len;` 那种改法在这
    //      里会红；不可达就**把这件事明说**，而不是让一段空跑的分支冒充
    //      "测过了"（本仓对"没测表现为通过"有过多次教训，见 functional 的
    //      CMakeLists 里那几段注释）。
    // ---------------------------------------------------------------------
    {
      progress("判据2 部分接受的余量只能是 data+n, len-n");

      // ---- 2a 先探测这个平台上部分接受可不可达 ----
      //
      // 同一条服务端、另开一条**用完就扔**的连接：对端一次都不泵（= 不读），
      // 反复 try_write 直到返回"非整条"。
      bool partial_reachable = false;
      {
        std::vector<char> scratch(64 * 1024, 'x');
        uvcpp_tcp_client scratch_cli(&loop);
        std::atomic<bool> sup{false};
        const int crc = scratch_cli.connect("127.0.0.1", port,
                                            [&sup](int st) { sup.store(st == 0); });
        check(crc == 0, "判据2 探测: 建连 rc=" + std::to_string(crc));
        wait_until_pair(sloop, &loop, [&] { return sup.load(); }, 3000);

        uv_buf_t sb = uv_buf_init(scratch.data(),
                                  static_cast<unsigned int>(scratch.size()));
        int first_negative_at = -1;
        int non_full = -1;
        for (int i = 0; i < 512; ++i) {
          const int n = scratch_cli.get_tcp()->try_write(&sb, 1);
          if (n == static_cast<int>(scratch.size())) continue;  // 整条吃下
          if (n < 0) {
            first_negative_at = i + 1;
            break;
          }
          non_full = n;  // 0 < n < len：部分接受
          partial_reachable = true;
          break;
        }
        std::cout << "  [note] 部分接受可达性探测（对端不读）：";
        if (partial_reachable) {
          std::cout << "出现部分接受：n=" << non_full << " / " << scratch.size();
        } else if (first_negative_at > 0) {
          std::cout << "整条吃下若干笔后在第 " << first_negative_at
                    << " 笔拿到**负数**（走「整条回退」那一支，不是部分接受）";
        } else {
          std::cout << "512 笔全部整条吃下（既没部分接受也没撞墙）";
        }
        std::cout << std::endl;

        scratch_cli.close();
        pump_for_pair(sloop, &loop, 200);
      }
      // 探测这几笔（最多 7 × 64 KiB，对端一次都没读）也落在对端账本里，
      // **必须收干净**再开 capture。
      //
      // 收不干净的后果是判据 2 **假红**：漏下的字节会在 capture 窗口里到账，
      // 判据 2 于是以"对端多收了若干字节"的形状报出来 —— 与"余量算成整条
      // `data, len`"**一模一样**。全量 ctest 里撞到过一次：多出 131072 字节，
      // 正好是 2 × 64 KiB —— 探测块大小与 libuv 在 Windows 上单次读的大小
      // 都是 64 KiB，所以这是**推断**的成因（下面那条前提断言就是为了把它
      // 从推断变成当场可判：真凶若在库侧，前提是绿的、判据 2 仍然红）。
      //
      // 这里原来用的是 `pump_both`：它收的是**圈数**不是毫秒（本文件头那条
      // "上限一律墙钟"的约定），圈数会在完成包投递之前先跑完。改成 `drain_pair`
      // （静置判据），并且**把前提断言出来** —— 静置之后总数还在涨，说明是
      // 探测没排干，而不是被测的那件事错了。
      drain_pair(sloop, &loop, rec);
      const size_t after_probe = rec.total;
      pump_for_pair(sloop, &loop, 200);
      check(rec.total == after_probe,
            "判据2 前提: 探测那几笔没收干净 —— 静置 200ms 后又到账 " +
                std::to_string(rec.total - after_probe) +
                " 字节；它们会窜进判据 2 的窗口，把「余量算错」的形状借给它");

      const size_t kBigLen = 4u * 1024u * 1024u;
      std::vector<char> big(kBigLen);
      for (size_t i = 0; i < kBigLen; ++i) big[i] = pattern_at(i);
      const uint64_t big_hash = fnv1a(kFnvBasis, big.data(), big.size());

      std::vector<char> got;
      const size_t base_total = rec.total;
      rec.capture = &got;

      std::atomic<int> fired{0};
      std::atomic<int> status{99};

      const int wrc = client.write(big.data(), big.size(), [&](int st) {
        status.store(st);
        fired.fetch_add(1);
      });
      check(wrc == 0, "判据2: 提交 rc=" + std::to_string(wrc));

      // ---- 只泵客户端。服务端一次都不泵 = 对端不读。 ----
      // （这一段在 Windows 上不会改变任何结果 —— 单次调用总是整条吃下；
      //  在 Linux 上正是它把"部分接受"逼出来。）
      for (int i = 0; i < 4000; ++i) loop.run(UV_RUN_NOWAIT);

      // ---- 放服务端开始读 ----
      wait_until_pair(sloop, &loop,
                      [&] { return (rec.total - base_total) >= big.size(); },
                      5000);
      wait_until_pair(sloop, &loop, [&] { return fired.load() != 0; }, 2000);
      pump_for_pair(sloop, &loop, 50);  // 上面那条已经钉住"到齐"，这里只是静置
      rec.capture = nullptr;

      std::cout << "  [note] 4 MiB 一次写：交付状态=" << status.load()
                << " 对端共收到 " << (rec.total - base_total) << " 字节"
                << std::endl;

      if (!partial_reachable) {
        std::cout << "  [note] 本平台够不着「部分接受」——余量算术在本机无覆盖"
                     "（那半段在 Linux 上是活代码）。**不要把这个绿读成"
                     "「余量算对了」**，它只证明整条吃下这条路上的字节没错。"
                  << std::endl;
      }

      // ---- 核心断言：收到的必须是**不多不少**这 4 MiB，且逐字节一致 ----
      //
      // 余量算成 `data, len` 的写法在这里必红：前 n 个字节被发了两遍，对端
      // 收到 `len + n` 字节。
      //
      // 失败信息要能自己说清"多出来那部分是谁的"：探测那几笔灌的是 'x'，
      // 4 MiB 那份是 `pattern_at()`，两者的头长得完全不同 —— 所以把开头
      // 连续 'x' 的个数打出来，两种成因当场分得开。
      size_t lead_x = 0;
      while (lead_x < got.size() && got[lead_x] == 'x') ++lead_x;
      check(rec.total - base_total == big.size(),
            "判据2: 对端收到 " + std::to_string(rec.total - base_total) +
                " 字节，应为 " + std::to_string(big.size()) +
                " —— 多出来那部分是被**重发的前缀**（余量算成整条 `data, len`"
                " 正是这个形状）；收上来那串开头有 " + std::to_string(lead_x) +
                " 个 'x' 则是探测那几笔没排干（见上面那条前提断言）");
      check(got.size() == big.size(),
            "判据2: 攒下来的字节流长度为 " + std::to_string(got.size()) +
                "，应为 " + std::to_string(big.size()));
      check(fnv1a(kFnvBasis, got.data(), got.size()) == big_hash,
            "判据2: 收到的字节流与发出去的 4 MiB 不一致（内容被改动或次序错了）");
      check(fired.load() == 1,
            "判据2: 完成回调响了 " + std::to_string(fired.load()) + " 次");
      check(status.load() == 0,
            "判据2: 完成状态 = " + std::to_string(status.load()));
    }

    // 把判据 2 的尾巴收干净。**必须真收干净**：判据 3 的 `base_total` 是在这
    // 之后采样的，漏下去的字节会算进判据 3 的窗口。
    drain_pair(sloop, &loop, rec);

    // =====================================================================
    // 判据 3：交付确实在 write() 返回**之后**（不是同步交付）
    // =====================================================================
    //
    // 这条同时把"write() 返回到交付之间隔了几轮循环"量出来。被断言的只有
    // **"不在 `write()` 内部"**（库的既有契约：完成回调总是异步的，见
    // uvcpp_write 的"重入次序铁律"）。轮数本身**故意不断言**：它是实现细节，
    // 随平台、随发起点而变 —— 从循环外发起本机实测 1 轮（下面那行 note 就是它）；
    // 从**读回调里**发起可以做到 0 轮（libuv 在"发起时就能直接合成完成"的情况下
    // 会把请求挂进 pending 队列当轮处理）。本文档要守的是它**不在 `write()` 里**，
    // 不是它等于几 —— 把 0 或 1 写进断言，换个发起点或换个平台就翻。
    //
    // 快速路径**不改变这个数**：交付仍然由那次 `uv_write` 交（见 .cpp 里的
    // 说明），所以开/关快速路径量到的是同一个数。这一段用的是 16 字节报文，
    // 在门槛之下 —— 它量到的是**原路径**的轮数；试发支路那边的时序由判据 1b
    // 与 1e 钉（那两处用大报文，且断言的是"交付不在 write() 里、也不递归"）。
    // 这里把数记到 stdout，供人看趋势。
    {
      progress("判据3 交付在 write() 返回之后");

      std::vector<char> got;
      const size_t base_total = rec.total;
      rec.capture = &got;

      std::atomic<int> fired{0};
      std::atomic<int> status{99};
      const int wrc = client.write(kMsg, kMsgLen, [&](int st) {
        status.store(st);
        fired.fetch_add(1);
      });
      check(wrc == 0, "判据3: 提交 rc=" + std::to_string(wrc));

      // **一个循环轮都没泵。** 回调响了就是同步交付。
      check(fired.load() == 0,
            "判据3: write() 还没返回回调就响了 —— 交付是同步的");

      int iters = 0;
      while (fired.load() == 0 && iters < 100000) {
        loop.run(UV_RUN_NOWAIT);
        ++iters;
      }
      std::cout << "  [note] write() 返回 → 完成回调之间隔了 " << iters
                << " 轮循环" << std::endl;

      check(fired.load() == 1,
            "判据3: 完成回调响了 " + std::to_string(fired.load()) + " 次");
      check(status.load() == 0,
            "判据3: 完成状态 = " + std::to_string(status.load()));
      check(iters >= 1, "判据3: 交付发生在 write() 内部");

      wait_until_pair(sloop, &loop,
                      [&] { return (rec.total - base_total) >= kMsgLen; }, 2000);
      pump_for_pair(sloop, &loop, 50);  // 同上：到齐已由上面那条钉住
      rec.capture = nullptr;
      check(got.size() == kMsgLen, "判据3: 对端收到 " + std::to_string(got.size()) +
                                       " 字节，应为 " + std::to_string(kMsgLen));
      check(got.size() == kMsgLen && std::memcmp(got.data(), kMsg, kMsgLen) == 0,
            "判据3: 对端收到的那 " + std::to_string(kMsgLen) + " 字节内容不符");
    }

    // 判据 3 那 16 字节也要收干净：判据 4 的 `base_total` 采在这之后，漏下去
    // 就是"多收了若干字节"——与本判据要抓的"重发前缀"是同一个形状。
    drain_pair(sloop, &loop, rec);

    // =====================================================================
    // 判据 4：`uv_try_write` 的负返回不许从 `write()` 漏出去
    // =====================================================================
    //
    // 触发方式不需要任何平台代码，而且**是确定性的**，不是碰运气：
    //
    //   先裸提一笔 `uv_write`（绕开 uvcpp_tcp_client 自己的在途标志），
    //   **一轮都不泵**。libuv 在 Windows 上写请求一律走 IOCP，别说完不完
    //   成，完成包都还没被取走过 —— 于是 `write_reqs_pending > 0` 是**必然**
    //   的，而 `uv__tcp_try_write` 见它在就先返回 `UV_EAGAIN`
    //   （libuv `src/win/tcp.c`，单次 `WSASend`、没有内部循环）。
    //
    // 于是紧接着那次 `client.write()` 里，快路径第一步拿到的就是负数。
    // 契约要求它**落回原路径**：返回 0（= 已提交），真实结果由回退那次
    // `uv_write` 的完成回调如实交付。漏了这一步的写法会把 `UV_EAGAIN` 从
    // `write()` 直接抛出去 —— 调用方按契约会认为"这次写没发生"，而字节其实
    // 已经发出去了（这里没发出去，但那只是这一次的巧合）。
    {
      progress("判据4 try_write 拿到负返回时仍走原路径");

      const size_t kHoldLen = 2u * 1024u * 1024u;
      std::vector<char> hold(kHoldLen);
      for (size_t i = 0; i < kHoldLen; ++i) hold[i] = pattern_at(i + 7);

      uvcpp_buf view(hold.data(), hold.size());
      uvcpp_write* w = new uvcpp_write();
      w->set_uv_buf(view.out_uv_buf(), true);
      w->set_self_free(true);

      std::atomic<int> hold_done{0};
      const int hrc = client.get_tcp()->write(
          w, w->get_uv_buf(), 1,
          [&hold_done](uvcpp_write* wr, int /*st*/) {
            (void)wr;
            hold_done.fetch_add(1);
          });
      check(hrc == 0, "判据4: 占位写提交 rc=" + std::to_string(hrc));
      if (hrc != 0) delete w;

      // 一轮都没泵 —— 上面那笔此刻必然还挂在 `write_reqs_pending` 里。
      //
      // 这笔的报文**必须过门槛**：门槛之下根本不进快路径，负返回那条路也就
      // 测不到（实测过，用 16 字节钉这条时它仍然全绿）。
      std::atomic<int> fired{0};
      std::atomic<int> status{99};
      const int wrc = client.write(big.data(), big.size(), [&](int st) {
        status.store(st);
        fired.fetch_add(1);
      });
      check(wrc == 0,
            "判据4: try_write 拿到的负返回从 write() 漏出去了 —— 实际 rc=" +
                std::to_string(wrc) +
                "（契约：非 0 ⇒ 这次写没提交；而它其实已经落回原路径了）");

      std::vector<char> got;
      const size_t base_total = rec.total;
      rec.capture = &got;

      wait_until_pair(sloop, &loop, [&] { return fired.load() != 0; }, 4000);
      wait_until_pair(sloop, &loop, [&] { return hold_done.load() != 0; }, 4000);

      // **下面那条判据量的是对端账本，所以这里等的必须是对端。** 上面两条等的
      // 是**本地**完成回调 —— libuv 收下这笔写就响，它响了不等于对端收完了。
      // 从前这里接着一个 `pump_both(..., 40000)`（≈ 80 ms），实测撞到过对端才
      // 收到 458465/2162688 就断言，报出来像"字节丢了"，而写路径一个字节没错。
      const size_t want_len = kHoldLen + big.size();
      const bool arrived =
          wait_until_pair(sloop, &loop,
                          [&] { return (rec.total - base_total) >= want_len; },
                          8000);
      check(arrived,
            "判据4 前提: 对端 8s 内只收到 " +
                std::to_string(rec.total - base_total) + "/" +
                std::to_string(want_len) +
                " 字节 —— 下面那条判据量的是对端账本，等的是对端、不是本地回调");
      rec.capture = nullptr;

      check(fired.load() == 1,
            "判据4: 回退那笔写的回调响了 " + std::to_string(fired.load()) + " 次");
      check(status.load() == 0,
            "判据4: 回退那笔写的完成状态 = " + std::to_string(status.load()));
      check(hold_done.load() == 1,
            "判据4: 占位写的回调响了 " + std::to_string(hold_done.load()) + " 次");

      // 内容：占位的 2 MiB 后面**紧跟**那笔大报文，一个不多一个不少。
      // 余量算错或"负返回也当成功"都会在这里露出来。
      const size_t kBackLen = big.size();
      const uint64_t want = fnv1a(fnv1a(kFnvBasis, hold.data(), hold.size()),
                                  big.data(), big.size());
      check(rec.total - base_total == kHoldLen + kBackLen,
            "判据4: 对端共收到 " + std::to_string(rec.total - base_total) +
                " 字节，应为 " + std::to_string(kHoldLen + kBackLen));
      check(got.size() == kHoldLen + kBackLen,
            "判据4: 攒下来的字节流长度为 " + std::to_string(got.size()) +
                "，应为 " + std::to_string(kHoldLen + kBackLen));
      check(fnv1a(kFnvBasis, got.data(), got.size()) == want,
            "判据4: 对端收到的字节流与「占位块 + 回退那笔」的拼接不一致");
    }

    // ---------------------------------------------------------------------
    // 收尾：句柄全摘干净之后，循环必须能干净关掉
    // ---------------------------------------------------------------------
    //
    // 这一条钉的是"快路径的 idle 没在自己那一帧停掉"那类缺陷：
    //
    //   - idle 挂在 `loop->idle_handles` 上 ⇒ `uv_backend_timeout` 再也不肯返回
    //     非零（循环从此不能阻塞，100% 空转），`uv_loop_close()` 一路 `UV_EBUSY`；
    //   - idle 只停不关、或者每笔写新建一个又不关 ⇒ 句柄越积越多，同样是 EBUSY。
    //
    // 客户端与服务端都在上面的作用域里析构完，`drain_loop` 兜底拨过收尾队列，
    // 此刻循环上不该再有任何句柄或请求。
    progress("收尾 循环干净关闭");
  }

  const int lrc = loop.loop_close();
  check(lrc == 0,
        "收尾: loop_close rc = " + std::to_string(lrc) +
            "（UV_EBUSY = 还有句柄/请求挂在队列上 —— 快路径的 idle 没在自己"
            "那一帧停掉、或没在析构里关掉）");

  if (g_failures == 0) {
    std::cout << "[tcp_try_write_fastpath] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[tcp_try_write_fastpath] FAIL (" << g_failures << ")"
            << std::endl;
  return 2;
}
