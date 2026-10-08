/**
 * @file bench/bench_quic.cpp
 * @brief QUIC 传输层的批量吞吐量具。
 * @author zhuweiye
 * @version 1.5.1
 *
 * 存在的理由：`doc/quic-guide.md` 与目录里此前**没有任何 QUIC 量具** ——
 * 大载荷吞吐的数字只能从库外的一份靶场（`bench-cpp/`，不在本仓）里引，
 * 于是"改一处到底有没有用"在本仓里无法回答。这个文件把那条能力补上。
 *
 * ## 它量什么
 *
 * 三种模式，**默认是单向推**：
 *
 * - `push`：客户端开一条双向流、`write_stream(N)` 带 FIN，服务端只收不回。
 *   量的是**纯发送方向**。
 * - `echo`：服务端把收到的原样回写。量的是**往返**（issue #34 那张表用的
 *   就是这个形状）。
 * - `udpfloor`：**地板**，不是本库的读数。裸 `uv_udp` 对打同尺寸的报文，把
 *   QUIC 那两档夹在中间才看得出"协议本身值多少、实现又占多少"。少了它，
 *   `echo` 的 42.6 ms 只是个孤立的数。
 *
 * 前两种模式的**计时起点与终点都是事件，不是墙钟猜测**：
 *
 * | 模式 | 起点 | 终点 |
 * |---|---|---|
 * | `push` | `write_stream()` 返回前 | 服务端累计收到 N 字节那一跳 |
 * | `echo` | 同上 | 客户端累计收回 N 字节那一跳 |
 *
 * ## 为什么每个尺寸都换一条新连接
 *
 * 拥塞窗口是**连接级**的。同一条连接上先跑 2 MB 再跑 8 KB，第二笔量到的是
 * 被第一笔养肥的 cwnd，而不是"8 KB 该有多快"。所以每个尺寸都重开一条连接，
 * 并且**丢掉的第一次**（冷启动那一轮）不计入统计 —— 它会把握手尾巴与
 * 初始窗口一起量进去，而那不是稳态吞吐。
 *
 * ## 统计口径
 *
 * 每档跑 `--rounds` 轮（默认 7），报**最小值**与中位数。最小值是这里唯一
 * 该看的数：这台机器上有别的进程、有内核定时器粒度、有内存分配器的抖动，
 * 它们只会让某一轮变慢，不会让它变快（见记忆里「绝对墙钟 + 多轮最小值」
 * 那条）。绝对数字只在同一台机器、同一次运行内可比。
 *
 * ## 用法
 *
 * ```
 * uvcpp_bench_quic [--mode=push|echo|udpfloor] [--rounds=N] [--sizes=8192,65536,...]
 *                  [--pkt=N]        # 只对 --mode=udpfloor 有意义，见下
 * ```
 *
 * `--pkt=N` 定 `udpfloor` 每个数据报装多少字节。**必须与 QUIC 那一档实际用的数据报
 * 一样大** —— 地板本身也按包数付内核税，拿不同档的地板当分母是不可比的。默认值只
 * 是个起点，量的是哪一档以输出里那行自报的 `pkt=` 为准。
 */

#include <uv.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_QUIC_ENABLE

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <handle/uvcpp_udp.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_common.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>

using namespace uvcpp;

namespace {

/// 单次传输的墙钟上限。最大那档（2 MB）在实测里是百毫秒级，5 秒是"明显出
/// 问题"与"还没跑完"的分界。
const uint64_t kCaseDeadlineNs = 5000ULL * 1000000ULL;

/// 握手 + 首轮的等待上限。
const uint64_t kSetupDeadlineNs = 5000ULL * 1000000ULL;

enum Mode { kPush = 0, kEcho = 1, kUdpFloor = 2 };

/**
 * @brief 泵循环直到 \p pred 为真或墙钟超时。
 *
 * **`UV_RUN_ONCE`（阻塞式），不是 `UV_RUN_NOWAIT` 自旋。**
 *
 * 这是量具口径的一部分，不是随手选的：真实应用跑的是 `uv_run(UV_RUN_DEFAULT)`，
 * 它无事可做时**阻塞在 poll 里**，而不是空转。自旋式（`UV_RUN_NOWAIT` 紧循环）
 * 每一圈都要发一次 `WSAPoll`/`epoll_wait` 系统调用，那些调用会被算进被测时间
 * 里 —— 于是"库快了多少"被泵自己的开销稀释掉。
 *
 * 代价是 `UV_RUN_ONCE` 在没有待处理事件时会一直阻塞，而本函数的截止时间判据
 * 在它返回**之后**才跑。所以装置里另有一个 1 ms 的重复定时器（`Rig::watchdog_`）
 * 保证循环至多阻塞 ~1 ms 就会醒一次。定时器不参与计时，只在"真的卡住了"时
 * 把控制权还回来。
 */
template <typename Pred>
bool pump_until(uvcpp_loop* loop, Pred pred, uint64_t deadline_ns) {
  const uint64_t t0 = uv_hrtime();
  for (;;) {
    if (pred()) return true;
    loop->run(UV_RUN_ONCE);
    if (pred()) return true;
    if (uv_hrtime() - t0 > deadline_ns) return false;
  }
}

/// 当前线程的**周期数**（Windows 的 `QueryThreadCycleTime`）。
///
/// 这是本装置里唯一能分辨"库内少干了活"的量具，前两版都撞了墙：
/// - `std::clock()`：MinGW 下 `CLOCKS_PER_SEC` 是 1000，分辨率 1 ms ——
///   对着 8 KB 那档直接报 0.000。
/// - `GetProcessTimes`：单位是 100 ns，但**刷新粒度是调度时片**(默认 15.6 ms)，
///   于是小档报 0，大档只会在 15.625 的整数倍上跳。
/// - `QueryThreadCycleTime`：硬件周期计数，纳米级分辨率。TCP/UDP 的收发在
///   Windows libuv 上走 IOCP、不另起线程，所以整条路径的活都记在这个线程上。
///
/// **它不是墙钟**：线程睡在 `WSAPoll` 里的时间是零。正因为如此，才能把
/// "墙钟被内核钉住、但库内确实少干了活"这件事量出来。
/// 绝对周期数只在本机、同一次会话内可比。
uint64_t thread_cycles_now() {
#ifdef _WIN32
  ULONG64 c = 0;
  if (::QueryThreadCycleTime(::GetCurrentThread(), &c) == 0) return 0;
  return static_cast<uint64_t>(c);
#else
  return static_cast<uint64_t>(std::clock()) * (1000000000ULL / CLOCKS_PER_SEC);
#endif
}

/// 载荷第 \p i 个字节。
///
/// **按位置取值，不是常数填充** —— 这一条是量具的判据，不是装饰。全 `'x'` 的
/// 载荷对"搬错了位置"天然免疫：库把某一段字节搬到偏移 ±k 的地方，收端照样
/// 收到 n 个 `'x'`，计数全对。发送侧的存储刚刚从"一条连续 `vector`"改成
/// "定长分块"（见 `stream_send` 的注释），而分块引入的正是**边界**：一块的
/// 末尾、下一块的开头、一次 `writev_stream` 描述的至多 16 段。位置相关的
/// 载荷才分得清这些边界有没有搬对。收发两侧调同一个函数、同一套下标（每条流
/// 自己的绝对偏移，每轮从 0 起），所以比较是逐字节确定的。
inline char payload_byte(size_t i) {
  // 乘一个奇数再取高位：既随位置变，又不至于像 `i % 26` 那样每 26 字节
  // 重复一次（周期性载荷会把"整块错位 26 字节"这类错误放过）。
  return static_cast<char>((static_cast<uint64_t>(i) * 2654435761ULL >> 24) & 0xFF);
}

std::vector<size_t> parse_sizes(const char* s) {
  std::vector<size_t> out;
  const char* p = s;
  while (*p != '\0') {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(p, &end, 10);
    if (end == p) break;
    if (v != 0) out.push_back(static_cast<size_t>(v));
    p = end;
    if (*p == ',') ++p;
  }
  return out;
}

/// 一档尺寸的一轮测量结果。
struct Round {
  double ms = 0.0;
  /// 与 `ms` 同一个窗口内的**本线程周期数**。见 `thread_cycles_now()`。
  ///
  /// 墙钟在这台机器上被内核钉住了：回环上每包光收发就是两三个微秒的系统调用，
  /// 库内省下的一次分配、一次 `memcpy`，在墙钟上根本冒不出噪声（实测把整条
  /// 发送快路停掉，墙钟差不出 3%）。**周期数才分辨得出"少干了活"** ——
  /// 这正是记忆里 `[[rig-cannot-resolve-library-changes]]` 缺的那把尺子。
  double cycles = 0.0;
  size_t recv_calls = 0;  ///< 收侧 `on_read(DATA)` 跑了几次
};

/**
 * @brief 一整套装置：一条循环、一个服务端、一个客户端、一条连接。
 *
 * 所有状态都是成员，回调全是无捕 lambda 路径上的 `this` —— 这个对象活到
 * 最后，端点在它之后析构。
 */
struct Rig {
  uvcpp_loop               loop;
  /// 只保证"循环不会永远阻塞"的重复定时器，见 `pump_until`。1 ms 的周期在
  /// 活跃传输里根本轮不到它响（事件比它密得多），所以它不进被测时间。
  uvcpp_timer              watchdog;
  uvcpp_ssl_context        server_ctx;
  uvcpp_ssl_context        client_ctx;
  uvcpp_quic_server*       server = nullptr;
  uvcpp_quic_client*       client = nullptr;
  uvcpp_quic_connection*   server_conn = nullptr;
  uvcpp_quic_connection*   client_conn = nullptr;

  Mode   mode    = kPush;
  size_t target  = 0;   ///< 本次要传的字节数
  size_t srv_got = 0;   ///< 服务端已收字节
  size_t cli_got = 0;   ///< 客户端已收字节（echo 用）
  size_t srv_calls = 0; ///< 服务端 on_read(DATA) 次数
  size_t cli_calls = 0; ///< 客户端 on_read(DATA) 次数
  bool   srv_done = false;
  bool   cli_done = false;
  bool   connected = false;
  int    connect_status = 12345;
  int    write_status = 12345;
  bool   write_acked = false;
  /// 第一个内容不符的**流偏移**（`SIZE_MAX` = 一个都没错）。
  ///
  /// 收侧逐个字节对着 `payload_byte()` 比。见那个函数上的注释：这是"分块边界
  /// 有没有搬对"的唯一判据，而计数式判据对错位全免疫（字节数照样对）。
  size_t srv_bad = SIZE_MAX;
  size_t cli_bad = SIZE_MAX;

  explicit Rig(Mode m) : watchdog(&loop),
                         server_ctx(tls_mode::SERVER, tls_version::TLS_1_3),
                         client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3),
                         mode(m) {}

  /// 逐字节比对一段收到的数据，返回**第一个**不符处的流偏移；全对返回
  /// `SIZE_MAX`。`at` 是这段数据在流里的起始绝对偏移（本装置每条流每轮都从 0
  /// 起算，且只发一条流，所以收侧累计的字节数就是它）。
  static size_t scan_payload(const char* p, size_t n, size_t at) {
    for (size_t i = 0; i < n; ++i) {
      if (p[i] != payload_byte(at + i)) return at + i;
    }
    return SIZE_MAX;
  }

  /// 本装置两处内容判据里第一个不符的偏移（两个方向取先出现的那个）。
  size_t first_bad() const {
    if (srv_bad != SIZE_MAX && cli_bad != SIZE_MAX) {
      return srv_bad < cli_bad ? srv_bad : cli_bad;
    }
    return srv_bad != SIZE_MAX ? srv_bad : cli_bad;
  }

  bool setup() {
    if (!server_ctx.generate_self_signed("localhost", 2048)) return false;
    client_ctx.set_verify_mode(tls_verify_mode::NONE);

    // 先武装看门狗，再建端点：`pump_until` 从第一次调用起就依赖它。
    watchdog.start([](uvcpp_timer*) {}, 1, 1);

    server = new uvcpp_quic_server(&loop);
    server->set_ssl_context(&server_ctx);
    if (server->bind("127.0.0.1", 0) != 0) return false;

    const int lrc = server->listen([this](uvcpp_quic_connection* c) {
      server_conn = c;
      uvcpp_quic_connection::callbacks cbs;
      cbs.on_read = [this](uvcpp_quic_connection& conn, int64_t id,
                           const net_read_result& r) {
        if (r.event != net_read_event::DATA) return;
        ++srv_calls;
        if (srv_bad == SIZE_MAX) {
          const size_t bad = scan_payload(r.data, r.size, srv_got);
          if (bad != SIZE_MAX) srv_bad = bad;
        }
        srv_got += r.size;
        if (mode == kEcho && r.size != 0) {
          // 原样回写。**不带 FIN** —— 收尾由客户端那侧的 FIN 之后单独走，
          // 这里回 FIN 会让"客户端收回 N 字节"变成"客户端收回 N 字节且流已关"，
          // 两者在计时上是两件事。
          conn.write_stream(id, r.data, r.size, false);
        }
        if (srv_got >= target) srv_done = true;
      };
      c->set_callbacks(cbs);
    });
    if (lrc != 0) return false;

    const int port = server->configured_port();
    if (port <= 0) return false;

    client = new uvcpp_quic_client(&loop);
    client->set_ssl_context(&client_ctx);
    const int rc = client->connect("127.0.0.1", port, [this](int status) {
      connected      = true;
      connect_status = status;
    });
    if (rc != 0) return false;

    {
      uvcpp_quic_connection::callbacks cbs;
      cbs.on_read = [this](uvcpp_quic_connection&, int64_t,
                           const net_read_result& r) {
        if (r.event != net_read_event::DATA) return;
        ++cli_calls;
        if (cli_bad == SIZE_MAX) {
          const size_t bad = scan_payload(r.data, r.size, cli_got);
          if (bad != SIZE_MAX) cli_bad = bad;
        }
        cli_got += r.size;
        if (cli_got >= target) cli_done = true;
      };
      cbs.on_write = [this](uvcpp_quic_connection&, int64_t, int status) {
        write_acked  = true;
        write_status = status;
      };
      client->connection()->set_callbacks(cbs);
    }
    client_conn = client->connection();

    if (!pump_until(&loop, [this] { return connected; }, kSetupDeadlineNs)) {
      return false;
    }
    return connect_status == 0;
  }

  /// 跑一轮：开新流、写 \p n 字节带 FIN、等终点事件。
  /// @param out 收到结果；超时返回 false。
  bool once(size_t n, Round* out) {
    target    = n;
    srv_got   = 0;
    cli_got   = 0;
    srv_calls = 0;
    cli_calls = 0;
    srv_done  = false;
    cli_done  = false;
    write_acked  = false;
    write_status = 12345;
    srv_bad   = SIZE_MAX;
    cli_bad   = SIZE_MAX;

    // 位置相关的载荷 —— 见 `payload_byte()` 上的注释。
    std::vector<char> payload(n);
    for (size_t i = 0; i < n; ++i) payload[i] = payload_byte(i);

    const int64_t id = client_conn->open_stream(true);
    if (id < 0) return false;

    const uint64_t t0 = uv_hrtime();
    const uint64_t c0 = thread_cycles_now();
    const int wrc = client_conn->write_stream(id, payload.data(), n, true);
    if (wrc != 0) return false;

    const bool ok = pump_until(
        &loop,
        [this] { return (mode == kEcho) ? cli_done : srv_done; },
        kCaseDeadlineNs);
    const uint64_t t1 = uv_hrtime();
    const uint64_t c1 = thread_cycles_now();

    // 让"写完被确认"这件事也走完再收回这一轮 —— 否则它的收尾会串到下一轮，
    // 下一轮的计时里就混进了上一轮的尾巴。
    pump_until(&loop, [this] { return write_acked; }, kSetupDeadlineNs);

    if (!ok) return false;
    // 内容不符也判这一轮失败，并且**不能混进计时**：错位的字节数一模一样，
    // 混进去只会得到一份看起来正常的数字。
    if (first_bad() != SIZE_MAX) return false;
    out->ms = static_cast<double>(t1 - t0) / 1e6;
    out->cycles = static_cast<double>(c1 - c0);
    out->recv_calls = (mode == kEcho) ? cli_calls : srv_calls;
    return true;
  }

  ~Rig() {
    delete client;
    delete server;
  }
};

// =========================================================================
// 裸 UDP 地板
// =========================================================================

/// QUIC 的 ACK 包大致就这么大（短头 + 一个 ACK 帧）。
const size_t kAckLikeLen = 64;

/**
 * @brief 本机 UDP 路径的**吞吐地板**：除掉 QUIC，只剩内核。
 *
 * 形状尽量贴住 QUIC 那笔传输：A 连着发 `total` 字节、每包 `pkt` 字节，B 每收到
 * 一个就回一个 64 字节的小包（QUIC 的 ACK 就那么大），等 A 收齐同样多个回包。
 * 中间没有任何我们的代码 —— B 收到就原样回，A 收到只计数。
 *
 * **它存在的理由**：量出 `uvcpp_bench_quic` 那个"每包恒定 ~12 µs"里，有多少是
 * 内核的、有多少是库的。库里的任何优化都不可能让 QUIC 比这条路径更快，所以
 * 这个数是所有后续优化的天花板；没有它，就分不清"还能再快"和"已经到头了"。
 *
 * 收侧和发侧都走 `uv_udp_try_send` 快路，与 QUIC 那条发送路径同一个形状。
 *
 * **在飞包数是有界的**（`kFloorWindow`）：不留窗口时，一段超过对端接收缓冲的突发
 * 会被内核静默丢掉，`a_got` 于是永远追不上 `to_send` —— 症状是大尺寸一律
 * `ROUND FAILED`，而它读起来像"QUIC 更快"。这条与 QUIC 那笔传输的真实差别是：
 * 那条路有拥塞窗口在限流，这条路没有，所以窗口得由量具自己给。
 */
struct UdpFloor {
  uvcpp_loop  loop;
  uvcpp_timer watchdog;
  uvcpp_udp   a;
  uvcpp_udp   b;

  struct sockaddr_storage peer_b;
  struct sockaddr_storage peer_a;

  size_t   pkt     = 1200;
  size_t   total   = 0;
  size_t   to_send = 0;
  size_t   sent    = 0;
  size_t   a_got   = 0;
  uint64_t t0      = 0;
  bool     done    = false;
  bool     armed   = false;

  ::std::vector<char> payload;

  UdpFloor() : watchdog(&loop), a(&loop), b(&loop) {}

  static void free_buf(const uv_buf_t* buf) {
    if (buf != nullptr && buf->base != nullptr) uvcpp_free_bytes(buf->base);
  }

  /// 收侧每块缓冲的大小。与 QUIC 那两份（`src/quic/uvcpp_quic_*.cpp` 里的
  /// `kRecvBufLen`）取同一个值，才能和它比。
  static const size_t kFloorRecvBufLen = 4096;

  /// 回一个小包给 `peer`。收侧走快路（发不出去就丢 —— 这只是一把量尺，
  /// 重传不在这条路径的语义里）。
  void reply(const struct sockaddr* peer) {
    char small[kAckLikeLen];
    std::memset(small, 0, sizeof(small));
    uv_buf_t bufs[1];
    bufs[0] = uv_buf_init(small, static_cast<unsigned int>(sizeof(small)));
    b.try_send(bufs, 1, peer);
  }

  /// 在飞包数的上限。**必须大于 1**（否则量的是串行往返，不是吞吐），又**必须足够
  /// 小**，让一段突发塞得进对端的接收缓冲 —— 否则内核静默丢包，`a_got` 永远追不上
  /// `to_send`，这一轮就只剩"看门狗超时"这一个结局，而它长得像"QUIC 更慢"。
  ///
  /// 这条是实测出来的：不留窗口时，64 KiB（54 × 1200 B）还能跑完，256 KiB 起
  /// **每个尺寸都 ROUND FAILED** —— 断点正落在本机 UDP 接收缓冲的默认值（64 KiB）上。
  /// 取 16 是给接收缓冲更小的机器留余量，同时远大于 1（10 µs/包对，16 个在飞
  /// 就是 160 µs 的流水线，足以盖住往返延迟）。
  static const size_t kFloorWindow = 16;

  /// 把还没发的尽量发出去，但**在飞不超过 `kFloorWindow`**；内核队列满时改走
  /// 异步，完成时回到这里继续。收侧每收到一个回包也会再叫一次（见 `once()` 那边
  /// 的读回调）—— 窗口是在那里被推着往前走的。
  void pump_sends() {
    while (sent < to_send && sent - a_got < kFloorWindow) {
      uv_buf_t bufs[1];
      bufs[0] = uv_buf_init(payload.data(),
                            static_cast<unsigned int>(pkt));
      const int rc =
          a.try_send(bufs, 1, reinterpret_cast<const struct sockaddr*>(&peer_b));
      if (rc >= 0 || rc != UV_EAGAIN) {
        ++sent;
        continue;
      }
      // 队列满：发一个异步的，等它的完成回调把我们叫回来。
      char* copy = new char[pkt];
      std::memcpy(copy, payload.data(), pkt);
      uv_buf_t abufs[1];
      abufs[0] = uv_buf_init(copy, static_cast<unsigned int>(pkt));
      uvcpp_udp_send* req = new uvcpp_udp_send();
      req->init();
      a.send(req, abufs, 1, reinterpret_cast<const struct sockaddr*>(&peer_b),
             [this, copy, req](uvcpp_udp_send* r, int status) {
               (void)status;
               delete[] copy;
               delete r;
               ++sent;
               pump_sends();
             });
      return;
    }
  }

  bool setup(size_t pkt_size, size_t n) {
    pkt   = pkt_size;
    total = n;
    payload.assign(pkt, 'x');

    watchdog.start([](uvcpp_timer*) {}, 1, 1);

    struct sockaddr_in sa;
    if (uv_ip4_addr("127.0.0.1", 0, &sa) != 0) return false;

    if (b.bind(reinterpret_cast<const struct sockaddr*>(&sa), 0) != 0)
      return false;
    int blen = static_cast<int>(sizeof(peer_b));
    if (b.getsockname(reinterpret_cast<struct sockaddr*>(&peer_b), &blen) != 0)
      return false;
    if (b.recv_start(
            [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
              (void)sz;
              uvcpp_buf::alloc_buf(buf, kFloorRecvBufLen);
            },
            [this](uvcpp_udp*, ssize_t nread, const uv_buf_t* buf,
                   const struct sockaddr* addr, unsigned int flags) {
              (void)flags;
              if (nread > 0 && addr != nullptr) reply(addr);
              free_buf(buf);
            }) != 0)
      return false;

    if (a.bind(reinterpret_cast<const struct sockaddr*>(&sa), 0) != 0)
      return false;
    int alen = static_cast<int>(sizeof(peer_a));
    if (a.getsockname(reinterpret_cast<struct sockaddr*>(&peer_a), &alen) != 0)
      return false;
    if (a.recv_start(
            [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
              (void)sz;
              uvcpp_buf::alloc_buf(buf, kFloorRecvBufLen);
            },
            [this](uvcpp_udp*, ssize_t nread, const uv_buf_t* buf,
                   const struct sockaddr*, unsigned int flags) {
              (void)flags;
              if (nread > 0) {
                ++a_got;
                if (a_got >= to_send) {
                  done = true;
                } else {
                  // 收到一个回包 = 窗口里空出一个位置。连续发的形状靠这里维持：
                  // 少了这一句，`pump_sends()` 只在开跑时被调一次，`to_send` 一
                  // 超过窗口就永远发不完。
                  pump_sends();
                }
              }
              free_buf(buf);
            }) != 0)
      return false;

    armed = true;
    return true;
  }

  /// 跑一轮：记时从第一个包发出开始。
  /// @return 墙钟毫秒；超时返回 -1。
  double once() {
    sent   = 0;
    a_got  = 0;
    done   = false;
    to_send = total / pkt;
    if (to_send == 0) return -1.0;

    const uint64_t start = uv_hrtime();
    t0 = start;
    pump_sends();
    if (!pump_until(&loop, [this] { return done; }, kCaseDeadlineNs)) return -1.0;
    return static_cast<double>(uv_hrtime() - start) / 1e6;
  }
};

double pct(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  const size_t i = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
  return sorted[i];
}

}  // namespace

int main(int argc, char** argv) {
  Mode                mode   = kPush;
  int                 rounds = 7;
  /// `udpfloor` 每个数据报装多少字节。**必须与 QUIC 那一档量到的数据报一样大**，
  /// 否则两条读数不可比：地板本身也是按包数付内核税的。
  ///
  /// 默认 1444 是 QUIC 那一侧放开数据报上限之后 PMTUD 停的那一档（见
  /// `doc/benchmark-rig.md`）；跑之前先看输出的 `pkt=` 那几个字，量出来的地板
  /// 是哪一档由它自报，不靠默认值。以前这里是 1200 —— 那是"数据报被钉在 1200"
  /// 那个版本的配套口径，留着会让默认比较**默认就不公平**。
  size_t              floor_pkt = 1444;
  std::vector<size_t> sizes;
  sizes.push_back(1024);
  sizes.push_back(8192);
  sizes.push_back(65536);
  sizes.push_back(262144);
  sizes.push_back(2097152);

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strncmp(a, "--mode=", 7) == 0) {
      const char* m = a + 7;
      if (std::strcmp(m, "echo") == 0) {
        mode = kEcho;
      } else if (std::strcmp(m, "udpfloor") == 0) {
        mode = kUdpFloor;
      } else {
        mode = kPush;
      }
    } else if (std::strncmp(a, "--rounds=", 9) == 0) {
      rounds = std::atoi(a + 9);
      if (rounds < 1) rounds = 1;
    } else if (std::strncmp(a, "--sizes=", 8) == 0) {
      std::vector<size_t> got = parse_sizes(a + 8);
      if (!got.empty()) sizes = got;
    } else if (std::strncmp(a, "--pkt=", 6) == 0) {
      const long v = std::atol(a + 6);
      if (v > 0) floor_pkt = static_cast<size_t>(v);
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", a);
      return 2;
    }
  }

  // 裸 UDP 地板：不走 `Rig`（那里一条连接都没有），单独一条路。
  if (mode == kUdpFloor) {
    std::printf("uvcpp bare-UDP floor  rounds=%d  pkt=%llu\n", rounds,
                static_cast<unsigned long long>(floor_pkt));
    std::printf("%10s %12s %12s %10s %12s\n", "bytes", "min_ms", "med_ms",
                "MB/s", "us/pkt");
    std::printf("-----------------------------------------------------------"
                "-------------------\n");
    bool bad = false;
    for (size_t si = 0; si < sizes.size(); ++si) {
      const size_t n   = sizes[si];
      const size_t pkt = (n < floor_pkt) ? n : floor_pkt;
      UdpFloor     f;
      if (!f.setup(pkt, n)) {
        std::printf("%10llu  SETUP FAILED\n",
                    static_cast<unsigned long long>(n));
        bad = true;
        continue;
      }
      f.once();  // 冷的那一轮丢掉
      std::vector<double> ms;
      bool                ok = true;
      for (int r = 0; r < rounds; ++r) {
        const double v = f.once();
        if (v < 0.0) {
          ok = false;
          break;
        }
        ms.push_back(v);
      }
      if (!ok) {
        std::printf("%10llu  ROUND FAILED\n",
                    static_cast<unsigned long long>(n));
        bad = true;
        continue;
      }
      std::sort(ms.begin(), ms.end());
      const double best = ms.front();
      const double n_pkt = static_cast<double>(n / pkt);
      std::printf("%10llu %12.3f %12.3f %10.2f %12.3f\n",
                  static_cast<unsigned long long>(n), best, pct(ms, 0.5),
                  (static_cast<double>(n) / (1024.0 * 1024.0)) / (best / 1000.0),
                  n_pkt > 0 ? (best * 1000.0 / n_pkt) : 0.0);
      std::fflush(stdout);
    }
    return bad ? 1 : 0;
  }

  std::printf("uvcpp QUIC throughput rig  mode=%s  rounds=%d\n",
              (mode == kEcho) ? "echo" : "push", rounds);
  std::printf("%10s %12s %12s %10s %10s %12s\n", "bytes", "min_ms",
              "med_ms", "MB/s", "recv_calls", "B/call");
  std::printf("-----------------------------------------------------------"
              "-------------------\n");

  bool any_failed = false;

  for (size_t si = 0; si < sizes.size(); ++si) {
    const size_t n = sizes[si];
    Rig          rig(mode);
    if (!rig.setup()) {
      std::printf("%10llu  SETUP FAILED\n",
                  static_cast<unsigned long long>(n));
      any_failed = true;
      continue;
    }

    // 冷启动那一轮：只用来把 cwnd 养起来，不进统计。它的耗时单独打出来，
    // 因为"第一条流有多快"本身是另一个有意义的问题。
    Round cold;
    const bool cold_ok = rig.once(n, &cold);

    std::vector<double> ms;
    std::vector<double> cpu;
    std::vector<size_t> calls;
    bool all_ok = true;
    for (int r = 0; r < rounds; ++r) {
      Round out;
      if (!rig.once(n, &out)) {
        all_ok = false;
        break;
      }
      ms.push_back(out.ms);
      cpu.push_back(out.cycles);
      calls.push_back(out.recv_calls);
    }
    if (!all_ok) {
      // 两种失败**分开报**：超时/看门狗与"内容不符"是完全不同的两件事，
      // 而它们在计数式判据下长得一样（字节数都会停在同一个地方）。
      const size_t bad = rig.first_bad();
      if (bad != SIZE_MAX) {
        std::printf("%10llu  DATA MISMATCH 第一个不符在流偏移 %llu\n",
                    static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(bad));
      } else {
        std::printf("%10llu  ROUND FAILED\n",
                    static_cast<unsigned long long>(n));
      }
      any_failed = true;
      continue;
    }

    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    const double best = sorted.front();
    const double med  = pct(sorted, 0.5);
    // 收侧回调次数取**中位数那一轮**的（各轮可能有 ±1 的差）。
    size_t calls_rep = 0;
    for (size_t i = 0; i < ms.size(); ++i) {
      if (ms[i] == med) {
        calls_rep = calls[i];
        break;
      }
    }
    if (calls_rep == 0 && !calls.empty()) calls_rep = calls[0];

    // 周期数报**最小值**：它是"这一档最少干过多少活"，与墙钟的最小值同一个口径。
    double cyc_best = cpu.empty() ? 0.0 : cpu[0];
    for (size_t i = 0; i < cpu.size(); ++i) {
      if (cpu[i] < cyc_best) cyc_best = cpu[i];
    }

    const double mbps = (static_cast<double>(n) / (1024.0 * 1024.0)) /
                        (best / 1000.0);
    std::printf("%10llu %12.3f %12.3f %10.2f %10llu %12.1f\n",
                static_cast<unsigned long long>(n), best, med, mbps,
                static_cast<unsigned long long>(calls_rep),
                calls_rep ? static_cast<double>(n) /
                                static_cast<double>(calls_rep)
                          : 0.0);
    std::printf("           (cyc/B %.2f  cold r1: %s%.3f ms)\n",
                (n != 0) ? cyc_best / static_cast<double>(n) : 0.0,
                cold_ok ? "" : "FAILED ", cold_ok ? cold.ms : 0.0);
    std::fflush(stdout);
  }

  return any_failed ? 1 : 0;
}

#else

int main() {
  std::printf("UVCPP_QUIC_ENABLE=0: nothing to measure.\n");
  return 0;
}

#endif  // UVCPP_QUIC_ENABLE
