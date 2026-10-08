/**
 * @file tests/functional/quic_gso_func.cpp
 * @brief QUIC 发送侧的 USO/GSO 分段（`UVCPP_ENABLE_UDP_GSO`）—— **只有对端看得见**。
 * @author zhuweiye
 * @version 1.5.2
 *
 * ## 为什么这条用例的判据长这样
 *
 * `setsockopt(IPPROTO_UDP, UDP_SEND_MSG_SIZE, ...)` **成功不等于会切**。本机实测
 * （`_probe/uso_probe.py`）：512、1200、1444、8192 一直到 **65483**（超过任何真实
 * MTU）全部返回 OK，bind 前后都一样。所以"选项装上了"这句本身没有信息量，能
 * 证明机制真生效的只有一件事：**对端收到了几个、各多长**。
 *
 * 判据的形状直接对着缺陷去：
 *
 * | 缺陷 | 本用例怎么红 |
 * |---|---|
 * | 选项号/协议层写错（`IPPROTO_UDP` 写成 `IPPROTO_TCP`） | 选项装不上/不切 ⇒ 第 1 段的前提或形状断言红；库内那份由第 2 段红 |
 * | 分段尺寸取错（拿 MTU 当 `*pgsolen`，或取小了切在包中间） | 收到的数据报长度与期望不符 |
 * | `setsockopt` 返 OK 而协议栈其实不切 | 回环 MTU 是 4294967295 ⇒ 收到**一个** 14440 字节的数据报 |
 *
 * 最后那一行是本用例存在的核心理由：**没有这条断言，一个"设了但没生效"的实现
 * 会全绿**，而它在线上表现为对端收到一个完整的大数据报。本库 QUIC 的接收缓冲
 * 只有 4096 字节（`kRecvBufLen`），那种包在内核那一层就被截断了。
 *
 * ### 第 1 段为什么把那个 syscall 又写了一遍
 *
 * 库内的落地是 `quic_detail::set_udp_gso_seg()`（私有头
 * `quic/uvcpp_quic_session.h`）。**它没有导出** —— `libuvcpp.dll` 的导出表里没有
 * 它，消费者 TU 链接不到（本笔的硬约束是"不新增公开符号"，那是有意的）。于是这里
 * 用**公开 API**（`uvcpp_udp::fileno()` 拿到 socket）在**同一种** socket 上做
 * **同一个** syscall、按**同样的次序**（`bind()` **之后** —— libuv 的
 * `uv_udp_init` 不建 socket，`uv_fileno()` 在 bind 之前返回 `UV_EBADF`；库里的
 * `setup_gso()` 也是在 `init_client`/`init_server` 的末尾、`udp->bind()` 之后
 * 跑的。这一点是被下面 1b 的 `[diag]` 抓出来的：先按"bind 之前"写，实测
 * fileno rc=-4083/sock=-1，整个第 1 段直接退化成 `[skip·前提]`）。
 *
 * 它回答的是第 2 段那个判据的**前提**：这台机器的协议栈认不认这个选项、认了之后
 * 切不切。
 *
 * ### 变异对照：这条用例的牙长在哪、没长在哪
 *
 * `_probe/gso_mutate.py` 的十个变异体逐个跑过（2026-10-09）。**每个用例文件有
 * 自己的失败标记**，所以下表按各文件自己的标记数 —— 只 grep `[FAIL]` 会漏掉别的
 * 文件那种形状，报出"红 0 条"，朝危险方向错。
 *
 * | 变异体 | 结果 | 被谁抓住 |
 * |---|---|---|
 * | `opt-noop`（只返 true、不 syscall） | **红** | 本文件第 2 段：服务端实收 **0** 字节 |
 * | `seg-off-by-one`（分段尺寸写成 `pgsolen + 1`） | **红** | 本文件第 2 段：实收 **8089** / 196608，首个不符在 8089 |
 * | `no-sentoff`（回调里不做发送记账） | **红** | `quic_api_func` 2 条 + 本文件第 2 段实收 **114133** / 196608 |
 * | `send-first-only`（聚合批次只发第一段） | **红** | **`quic_stream_func`**（实收 19615/197842）—— 本文件第 2 段**没抓住**它 |
 * | `agg-unset` `gso-always` `no-cache` `nwrite0-continue` `pgsolen-const` `cb-nonzero` | 绿 | 无 —— 见下 |
 *
 * **`pgsolen-const` 为什么绿：它是等价变异体，不是判据瞎了。** 本机回环的 MTU 是
 * 4294967295，path MTU 从一开始就是 PMTUD 的上限 1444，所以"写死 1444"与"取本次
 * `*pgsolen`"在**这台机器上**是同一条路。反证在同一张表里：`seg-off-by-one` 动的
 * 是同一个值、只偏了一个字节，它**红了** —— 那个尺寸确实落在判据的可观测范围里，
 * 只是 1444 恰好就是正确值。要在这台机器上把这两者分开，得有一台真网卡
 * （见 `doc/quic-guide.md` §1.2 的"坑二"）。
 *
 * `agg-unset` / `no-cache` 同理：一档传输里聚合尺寸只有一种，不重设 == 重设。
 * `gso-always`（跳过第二层运行期闸门）在回环上换不出可观测的差别。
 * `nwrite0-continue` 既没挂死也没报错 —— 这条路径上 `nwrite == 0` 到不了。
 * `cb-nonzero`（把"这条流发不了"返成 −1）同属这一类：本机的流调度到不了
 * 那三种错误，所以两条分支都没被走到。
 *
 * ⚠️ **第 2 段分辨不出"GSO 真的在切"与"库悄悄退回了逐包发"。** 这两种情况下对端
 * 收到的字节流**完全一样**（退回去时数据报仍是 1444 长、个数也一样）。所以第 2 段
 * 绿只说明**内容对**，不说明**机制在用**。真正分辨这两者的是性能那条读数
 * （`_probe/gso_ab.py` 的 `cyc/B`）加上第 1 段那份环境判据 —— 这也是为什么第 2 段
 * 不能替代第 1 段。
 *
 * ### 对照组是必须的
 *
 * `[1a]` 在**任何选项都没设**的 socket 上先发一段 14440 字节，断言对端收到
 * **一个** 14440 的数据报。没有这一步，"后面收到 10 个 1444"也可能是因为回环/
 * 协议栈因为别的原因在切 —— 那就成了"用例在看别的东西"。有了它，1d 那 10 个才
 * 归因得到那个 `setsockopt`。1a 与 1d 走的是**两条不同的 socket**（选项是每
 * socket 的、且设了就一直生效，同一条上做不出干净对照）。
 *
 * ## 第 2 段：真跑一条连接
 *
 * 192 KiB 的单向传输，**逐字节比**。它比第 1 段弱（不直接看数据报），但它是唯一
 * 覆盖"聚合缓冲真的接到了活连接上"的断言 —— 第 1 段碰不到会话。两者缺一不可：
 * 第 1 段证这台机器的机制在，第 2 段证库里那条路接对了地方。
 *
 * 尺寸取 192 KiB 而不是 256 KiB：服务端 `initial_max_stream_data_bidi_remote`
 * 正好是 256 KiB（`uvcpp_quic_session.cpp` 的 `fill_transport_params`），卡在
 * 窗口边界上会把"窗口用尽"混进判据里。192 KiB 仍是 `send_quantum`（64 KiB）的
 * 3 倍，聚合一定会发生。
 *
 * 另外，**现有那几条 QUIC 用例在本配置下本来就走这条路**（`quic_stream_func.cpp`
 * 第 4 段那笔 197 KB 的逐字节回声就是一个整段判据）。本文件不重复它们，只补
 * 它们看不见的那两条：数据报的**长度**与**个数**。
 *
 * ## 前提不成立时
 *
 * - 非 Windows，或 `UVCPP_UDP_GSO_ENABLE=0`（Linux 上显式开也是这个形状）：第 1 段
 *   打印 `[skip·前提]` 并**如实说明没判**，第 2 段照跑 —— 整条用例不是空的。
 * - Windows 且宏为 1（也就是 CMake 那个派生默认值给出的形状）而**选项装不上**：
 *   也打印 `[skip·前提]`（那时库里的探测同样会失败 ⇒ 库会退回逐包发送，不是缺陷）。
 * - 选项**装得上却不切**：**红**。派生的默认值说了"这台机器上开着"，而这条路
 *   一旦不切，对端收到的是一个完整的大数据报 —— 那正是这条用例要挡的事。
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_QUIC_ENABLE

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_udp.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_common.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_buf.h>

#include "loop_drain.h"
#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_checks   = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
  ++g_checks;
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "：期望 " << want << "，实得 " << got
              << std::endl;
    ++g_failures;
  }
}

/// 造一条 \p n 字节、**逐字节随位置变**的载荷。
///
/// 同 `quic_stream_func.cpp` 里 `big_payload()` 的理由：常数填充对"搬错了位置"
/// 免疫（收端照样收到 n 个同样的字节，长度断言全对）。这里两段都要逐字节比。
std::string pattern_payload(size_t n) {
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    s.push_back(static_cast<char>('a' + ((i * 7 + (i >> 6)) % 26)));
  }
  return s;
}

/// 两串第一个不同的下标；完全相同返回 `std::string::npos`。
size_t first_diff(const std::string& got, const std::string& want) {
  const size_t n = got.size() < want.size() ? got.size() : want.size();
  for (size_t i = 0; i < n; ++i) {
    if (got[i] != want[i]) return i;
  }
  return got.size() == want.size() ? std::string::npos : n;
}

std::string lens_to_string(const std::vector<size_t>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i != 0) s += ", ";
    char b[32];
    std::snprintf(b, sizeof(b), "%llu", static_cast<unsigned long long>(v[i]));
    s += b;
  }
  return s;
}

// ===========================================================================
// 第 1 段：机制本身 —— 回环上一对裸 UDP 口
// ===========================================================================

const int    kSeg  = 1444;        ///< PMTUD 爬满之后的 path MTU
const size_t kFull = 10 * 1444;   ///< 10 个满段 = 14440

/// 接收侧攒下来的东西。
struct Sink {
  std::vector<size_t> lens;    ///< 每个数据报的长度，按到达顺序
  std::string         bytes;   ///< 拼起来的全部字节，按到达顺序
  size_t              total    = 0;
  int                 send_rc  = 0;

  void clear() {
    lens.clear();
    bytes.clear();
    total   = 0;
    send_rc = 0;
  }
};

/// 发一整段并收齐。
///
/// 收齐的判据是"字节总数到齐"，之后再多泵 40 ms —— 一个把 14440 切成 20 段的
/// 实现，在总数上也刚好等于 14440，只有等它全到齐才看得出"多"。
void send_payload(uvcpp_loop* loop, uvcpp_udp* tx, const struct sockaddr* peer,
                  Sink* sink, size_t payload_len) {
  sink->clear();
  const std::string payload = pattern_payload(payload_len);
  uv_buf_t buf = uv_buf_init(const_cast<char*>(payload.data()),
                             static_cast<unsigned int>(payload.size()));
  sink->send_rc = tx->try_send(&buf, 1, peer);

  uvcpp_test::wait_until(loop, [&] { return sink->total >= payload_len; }, 2000);
  uvcpp_test::pump_for(loop, 40);
}

/// 核对"这一段的长度形状就是 \p want 那串"。
void check_shape(const Sink& sink, const std::vector<size_t>& want,
                 const std::string& what) {
  check_eq_i(static_cast<long long>(sink.lens.size()),
             static_cast<long long>(want.size()), what + "：数据报个数");
  if (sink.lens.size() != want.size()) {
    std::cerr << "  [diag] 实得 [" << lens_to_string(sink.lens) << "]，期望 ["
              << lens_to_string(want) << "]" << std::endl;
    return;
  }
  check(sink.lens == want, what + "：每一个的长度 —— 实得 [" +
                               lens_to_string(sink.lens) + "]");
}

#if UVCPP_UDP_GSO_ENABLE && defined(_WIN32)

// `UDP_SEND_MSG_SIZE` 是 winsock 的扩展选项号，**MinGW 的 ws2ipdef.h 里没有**
// （本机实测：`grep -rn UDP_SEND_MSG_SIZE /c/msys64/mingw64/include/` 零命中），
// 而 release 的 mingw-x64 腿会走到这里 ⇒ 兜底字面量是必需的。库内那份同样兜
// （`src/quic/uvcpp_quic_session.cpp`）。
#ifndef UDP_SEND_MSG_SIZE
#  define UDP_SEND_MSG_SIZE 2
#endif

/// 与库内 `quic_detail::set_udp_gso_seg()` **逐字同形**的一份（理由见文件头
/// 「第 1 段为什么把那个 syscall 又写了一遍」）。
///
/// @return true = 内核收下了这个分段尺寸；**这不等于它会切**，所以调用方必须
///         当真去数对端的段数 —— 那才是本段的判据。
bool set_uso_seg(uvcpp_udp* udp, int seg) {
  if (udp == nullptr || seg <= 0) return false;
  uv_os_sock_t sock = (uv_os_sock_t)-1;
  if (udp->fileno(sock) != 0) return false;
  if (sock == (uv_os_sock_t)-1) return false;
  const int v = seg;
  return ::setsockopt((SOCKET)sock, IPPROTO_UDP, UDP_SEND_MSG_SIZE,
                      reinterpret_cast<const char*>(&v),
                      static_cast<int>(sizeof(v))) == 0;
}

#endif  // UVCPP_UDP_GSO_ENABLE && _WIN32

void test_split_over_raw_udp() {
#if UVCPP_UDP_GSO_ENABLE && defined(_WIN32)
  uvcpp_loop loop;
  uvcpp_test::loop_drain drain_loop(&loop);

  uvcpp_udp tx(&loop);     ///< 被测的那条：会先设分段尺寸
  uvcpp_udp ctrl(&loop);   ///< 对照组：一个选项都不设
  uvcpp_udp rx(&loop);

  struct sockaddr_in any;
  check_eq_i(uv_ip4_addr("127.0.0.1", 0, &any), 0, "[1] uv_ip4_addr");
  check_eq_i(rx.bind(reinterpret_cast<const struct sockaddr*>(&any), 0), 0,
             "[1] rx.bind");
  check_eq_i(ctrl.bind(reinterpret_cast<const struct sockaddr*>(&any), 0), 0,
             "[1] ctrl.bind");

  check_eq_i(tx.bind(reinterpret_cast<const struct sockaddr*>(&any), 0), 0,
             "[1] tx.bind");

  // -------------------------------------------------------------------
  // 1b. 前提：这台机器收不收这个分段尺寸。
  //
  // ⚠️ **必须在 `bind()` 之后设。** libuv 的 `uv_udp_init` 把
  // `handle->socket` 留成 `INVALID_SOCKET`，**socket 是 bind 的时候才建的** ——
  // bind 之前 `uv_fileno()` 返回 `UV_EBADF`(-4083)、拿出来的还是 -1，根本无
  // 从设起（本机实测：bind 之前 fileno rc=-4083/sock=-1；bind 之后 rc=0 且
  // `setsockopt(UDP_SEND_MSG_SIZE, 1444)` rc=0）。库里的 `setup_gso()` 也是
  // 在 `init_client` / `init_server` 的**末尾**跑的，也就是 `udp->bind()`
  // 之后 —— 这里与它同序。
  // -------------------------------------------------------------------
  if (!set_uso_seg(&tx, kSeg)) {
    std::cout << "  [skip·前提] 第 1 段：UVCPP_UDP_GSO_ENABLE="
              << UVCPP_UDP_GSO_ENABLE
              << " 且本平台是 Windows，而 UDP_SEND_MSG_SIZE=" << kSeg
              << " 设不上 —— 这台机器没有 USO，库里的探测同样会失败（那时库会"
                 "退回逐包发送），本段没判"
              << std::endl;
    return;
  }

  struct sockaddr_in rx_addr;
  int                rx_len = static_cast<int>(sizeof(rx_addr));
  check_eq_i(
      rx.getsockname(reinterpret_cast<struct sockaddr*>(&rx_addr), &rx_len), 0,
      "[1] rx.getsockname");
  check(rx_len > 0 && rx_addr.sin_port != 0, "[1] rx 拿到了内核分配的端口");
  const struct sockaddr* peer =
      reinterpret_cast<const struct sockaddr*>(&rx_addr);

  Sink      sink;
  const int recv_rc = rx.recv_start(
      [](uvcpp_handle*, size_t, uv_buf_t* buf) {
        // **给足 64 KiB**：万一分段没生效，那个 14440 字节的数据报要能被整条收
        // 下来 —— 截断了的话下面看到的会是一条"正常"的短长度，判据就瞎了。
        uvcpp_buf::alloc_buf(buf, 65536);
      },
      [&](uvcpp_udp*, ssize_t nread, const uv_buf_t* buf, const struct sockaddr*,
          unsigned int) {
        if (nread > 0) {
          sink.lens.push_back(static_cast<size_t>(nread));
          sink.total += static_cast<size_t>(nread);
          if (buf != nullptr && buf->base != nullptr) {
            sink.bytes.append(buf->base, static_cast<size_t>(nread));
          }
        }
        if (buf != nullptr && buf->base != nullptr) uvcpp_free_bytes(buf->base);
      });
  check_eq_i(recv_rc, 0, "[1] rx.recv_start");
  if (recv_rc != 0) return;

  const std::string payload_full = pattern_payload(kFull);

  // -------------------------------------------------------------------
  // 1a. **对照组**：`ctrl` 上一个选项都没设。回环 MTU 是 4294967295，所以
  //     14440 字节的一段必须原样变成**一个** 14440 的数据报。
  //
  //     这一步不是装饰：它证明**这套装置看得见一个整的大数据报**。没有它，
  //     1d 那 10 个 1444 也可以解释成"协议栈因为别的原因在切"，用例就在看
  //     一个不是被测物的东西。
  // -------------------------------------------------------------------
  send_payload(&loop, &ctrl, peer, &sink, kFull);
  check_eq_i(sink.send_rc, static_cast<long long>(kFull),
             "[1a] 对照组 try_send 返回全部字节数（一次调用交出去一整段）");
  check_shape(sink, {kFull}, "[1a] 不设选项");

  // -------------------------------------------------------------------
  // 1c. 短于分段尺寸的一段**不该被切**。这条钉的是库里 `send_batch()` 那条
  //     `total <= pgsolen` 的路：单包发送时给的分段尺寸取
  //     `max(本包长度, path MTU)`，所以它整包出去。
  // -------------------------------------------------------------------
  send_payload(&loop, &tx, peer, &sink, 1200);
  check_shape(sink, {1200}, "[1c] 1200 B / seg=1444");

  // -------------------------------------------------------------------
  // 1d. 主体：整除。10 x 1444 = 14440。
  // -------------------------------------------------------------------
  send_payload(&loop, &tx, peer, &sink, kFull);
  check_eq_i(sink.send_rc, static_cast<long long>(kFull),
             "[1d] try_send 返回全部字节数（USO 下也是一次调用）");
  check_shape(sink, std::vector<size_t>(10, kSeg), "[1d] 14440 B / seg=1444");
  // 内容也要对：只数个数的话，"十段乱序"或"边界处掉了几个字节"都能过。
  // 回环上的 UDP 是 FIFO 的，所以拼起来必须与发出去的逐字节相同。
  {
    const size_t d = first_diff(sink.bytes, payload_full);
    check(d == std::string::npos,
          std::string("[1d] 拼起来的字节逐字节相同；第一个不同的偏移 ") +
              (d == std::string::npos ? std::string("无") : std::to_string(d)) +
              "（实收 " + std::to_string(sink.bytes.size()) + "）");
  }

  // -------------------------------------------------------------------
  // 1e. 末段可以短：10 x 1444 + 500。这正是真实聚合的形状 —— 一批里最后一个
  //     包不满 MTU，接收侧必须照收，而计数断言不能要求"全部等长"。
  // -------------------------------------------------------------------
  {
    std::vector<size_t> want(10, static_cast<size_t>(kSeg));
    want.push_back(500);
    send_payload(&loop, &tx, peer, &sink, kFull + 500);
    check_shape(sink, want, "[1e] 14940 B / seg=1444");
    const std::string want_bytes = pattern_payload(kFull + 500);
    const size_t      d          = first_diff(sink.bytes, want_bytes);
    check(d == std::string::npos,
          std::string("[1e] 拼起来的字节逐字节相同；第一个不同的偏移 ") +
              (d == std::string::npos ? std::string("无") : std::to_string(d)));
  }

  rx.recv_stop();
#else
  std::cout << "  [skip·前提] 第 1 段：USO 只在 Windows 上编"
            << "（UVCPP_UDP_GSO_ENABLE=" << UVCPP_UDP_GSO_ENABLE
#if defined(_WIN32)
            << "，本平台是 Windows"
#else
            << "，本平台不是 Windows"
#endif
            << "）—— 本段没判" << std::endl;
#endif
}

// ===========================================================================
// 第 2 段：一条真连接上的 192 KiB 单向传输
// ===========================================================================

void test_big_transfer_over_quic() {
  uvcpp_loop loop;
  uvcpp_test::loop_drain drain_loop(&loop);

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    std::cerr << "  [FAIL] [2] 生成自签证书失败" << std::endl;
    ++g_failures;
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  uvcpp_quic_server server(&loop);
  server.set_ssl_context(&server_ctx);
  check_eq_i(server.bind("127.0.0.1", 0), 0, "[2] bind(127.0.0.1, 0)");

  std::string got;
  int         server_read_errors = 0;

  const int listen_rc = server.listen([&](uvcpp_quic_connection* c) {
    uvcpp_quic_connection::callbacks cbs;
    cbs.on_read = [&](uvcpp_quic_connection&, int64_t, const net_read_result& r) {
      if (r.event == net_read_event::DATA) {
        got.append(r.data, r.size);
      } else if (r.event == net_read_event::READ_ERROR) {
        ++server_read_errors;
        std::cerr << "  [diag] [2] 服务端读到错误事件：" << r.error << std::endl;
      }
    };
    c->set_callbacks(cbs);
  });
  check_eq_i(listen_rc, 0, "[2] listen() 返回 0");
  if (listen_rc != 0) return;

  const int port = server.configured_port();
  check(port > 0, "[2] configured_port() 报内核分配的端口");

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  bool      connected      = false;
  int       connect_status = 12345;
  const int rc             = client.connect("127.0.0.1", port, [&](int status) {
    connected      = true;
    connect_status = status;
  });
  check_eq_i(rc, 0, "[2] connect() 返回 0");
  if (rc != 0) return;
  if (client.connection() == nullptr) {
    check(false, "[2] connect() 之后 connection() 还在");
    return;
  }

  // `connect()` 返回之后、下一次循环迭代之前装回调 —— 见
  // `uvcpp_quic_client.h` 里那条 `@warning`。
  int64_t sid               = -1;
  bool    write_done        = false;
  int     write_status      = 12345;
  int     client_read_errors = 0;
  {
    uvcpp_quic_connection::callbacks cbs;
    cbs.on_read = [&](uvcpp_quic_connection&, int64_t,
                      const net_read_result& r) {
      if (r.event == net_read_event::READ_ERROR) ++client_read_errors;
    };
    cbs.on_write = [&](uvcpp_quic_connection&, int64_t id, int status) {
      if (id == sid) {
        write_done   = true;
        write_status = status;
      }
    };
    client.connection()->set_callbacks(cbs);
  }

  const bool up =
      uvcpp_test::wait_until(&loop, [&] { return connected; }, 5000);
  check(up, "[2] 握手在 deadline 内完成");
  if (!up) return;
  check_eq_i(connect_status, 0, "[2] connect 的 cb 收到 0");

  // 192 KiB = 3 x `stream_send::kChunkSize`(64 KiB)，**一笔**写完。一笔是有意
  // 的：它让发送队列里一次性堆着 192 KiB，于是某一轮 flush 真的有东西可以聚合
  // 成一批（send quantum 是 64 KiB）。分成几笔小写会把聚合压小，测不到这条路。
  const size_t      kBig    = 192 * 1024;
  const std::string payload = pattern_payload(kBig);

  sid = client.connection()->open_stream(true);
  check(sid >= 0, "[2] 开出了一条双向流");
  if (sid < 0) return;

  check_eq_i(client.connection()->write_stream(sid, payload.data(),
                                               payload.size(), true),
             0, "[2] write_stream(192 KiB, FIN) 返回 0");

  const bool arrived =
      uvcpp_test::wait_until(&loop, [&] { return got.size() >= kBig; }, 15000);
  check(arrived, "[2] 192 KiB 在 deadline 内到齐");
  check_eq_i(static_cast<long long>(got.size()),
             static_cast<long long>(kBig), "[2] 服务端收到的字节数");
  {
    const size_t d = first_diff(got, payload);
    check(d == std::string::npos,
          std::string("[2] 逐字节相同；第一个不同的偏移 ") +
              (d == std::string::npos ? std::string("无") : std::to_string(d)));
  }
  check_eq_i(server_read_errors, 0, "[2] 服务端没读到过错误事件");

  check(uvcpp_test::wait_until(&loop, [&] { return write_done; }, 5000),
        "[2] on_write 在 deadline 内跑到");
  check_eq_i(write_status, 0, "[2] on_write 收到 0（对端确认了）");
  check_eq_i(client_read_errors, 0, "[2] 客户端没读到过错误事件");

  check_eq_i(client.close(), 0, "[2] close() 返回 0");
}

}  // namespace

int main() {
  std::cout << "[functional quic_gso] start" << std::endl;
  std::cout << "  [diag] UVCPP_UDP_GSO_ENABLE=" << UVCPP_UDP_GSO_ENABLE
#if defined(_WIN32)
            << " _WIN32=1"
#else
            << " _WIN32=0"
#endif
            << std::endl;

  check_eq_i(quic_crypto_backend_init(), 0, "quic_crypto_backend_init()");

  test_split_over_raw_udp();
  test_big_transfer_over_quic();

  quic_crypto_backend_free();

  std::cout << "[functional quic_gso] checks=" << g_checks
            << " failures=" << g_failures << std::endl;
  if (g_failures == 0) {
    std::cout << "[functional quic_gso] done success=true" << std::endl;
    return 0;
  }
  std::cout << "[functional quic_gso] done success=false" << std::endl;
  return 2;
}

#else  // UVCPP_QUIC_ENABLE

// 关掉 quic 模块时这个文件不该被编译（`tests/functional/CMakeLists.txt` 的过滤器
// 按文件名摘掉它 —— 规则是 `quic`）。真编到了就是配置错：返回非零而不是打一句
// SKIP 再返回 0，后者会把"这个模块一次都没被跑过"伪装成"全绿"。
int main() {
  std::cerr << "[quic_gso] UVCPP_QUIC_ENABLE=0 —— 这个测试文件不该被编译进来"
            << std::endl;
  return 2;
}

#endif  // UVCPP_QUIC_ENABLE
