/**
 * @file src/quic/uvcpp_quic_session.cpp
 * @brief `quic_session` 的实现 —— 本模块里唯一直接驱动 ngtcp2 与 OpenSSL 的地方。
 * @author zhuweiye
 * @version 1.5.1
 *
 * 读它的顺序：`init_client` / `init_server` 看骨架 → `read_pkt` 与 `do_flush`
 * 看数据怎么进出 → `finalize` / `do_close` 看收尾 → 最末那一片 `cb_*` 看 ngtcp2
 * 怎么叫回来。
 *
 * **有三件事比代码本身更值得先知道：**
 *
 * 1. **不能在 ngtcp2 的回调里调它的写函数。** `ngtcp2_conn_writev_stream` 与
 *    `ngtcp2_conn_write_connection_close` 在 `ngtcp2.h` 里各自写死了 "must not
 *    be called from inside the callback functions"。所以本文件里**每一个**
 *    `cb_*` 跳板都套一层 `cb_guard`（`in_callback_` 加一），而 `flush()` /
 *    `close()` 看到它非零时只**记意图**（`want_flush_` / `want_close_`），等
 *    `read_pkt()` / `on_expiry()` 退到最外层再兑现。这不是防重入的保险丝，
 *    是 ngtcp2 的硬要求 —— 用户完全可能在 `on_stream_data` 里直接 `close()`。
 *
 * 2. **本仓是 C++11**（`CMakeLists.txt` 的 `CMAKE_CXX_STANDARD 11`），而 ngtcp2
 *    自己的例子（`_local_deps/ngtcp2/examples/`，本文件的调用序列参照它们）是
 *    C++20/23：指定初始化器、`std::span`、`std::expected`、`std::println`、
 *    `if` 带初始化语句。**一个都不能抄。** 所以 `ngtcp2_callbacks` /
 *    `ngtcp2_settings` / `ngtcp2_path` 一律 `memset` 之后逐字段赋值。
 *
 * 3. **服务端首包里的两个 CID 用途不同**，见 `init_server` 的注释。
 */

#include "quic/uvcpp_quic_session.h"

#if UVCPP_QUIC_ENABLE

#include <cstring>
#include <string>

#include <ssl/uvcpp_ssl_context.h>

namespace uvcpp {
namespace quic_detail {

namespace {

/// 服务端自己 SCID 的长度。
///
/// 18 这个数是照着 ngtcp2 的例子取的（`examples/server.cc` 里的
/// `NGTCP2_SV_SCIDLEN`）—— 它是**例子自己的常量，不是 ngtcp2 的宏**，ngtcp2
/// 只规定上限 `NGTCP2_MAX_CIDLEN`(20) 与 Initial 里 DCID 的下限
/// `NGTCP2_MIN_INITIAL_DCIDLEN`(8)。所以这里不是"抄了一个宏"，是"选了一个值"。
const size_t kServerScidLen = 18;
const size_t kClientScidLen = 18;
const size_t kInitialDcidLen = 18;

/// 一个数据报的上限。**三处取同一个值**：`writev_stream` 的落点缓冲、
/// `settings.max_tx_udp_payload_size`、以及我们对外的 `max_udp_payload_size`
/// 传输参数（`fill_transport_params()`）。
///
/// 下界是硬的：ngtcp2 要求 `max_tx_udp_payload_size` **至少**
/// `NGTCP2_MAX_UDP_PAYLOAD_SIZE`(1200)，落点缓冲也不能比它小。
///
/// 上界取 1500（典型以太网 MTU）。这个数是从一条**缺陷**里定出来的：ngtcp2
/// 自带的 PMTUD 探测表是 {1406, 1342, 1232, 1444}，而 `conn_start_pmtud()` 把
/// `min(对端 max_udp_payload_size, 本端 max_tx_udp_payload_size)` 当作硬上限 ——
/// 上限一旦 ≤ 1200，**四档探测全部越界被跳过**，PMTUD 当场判 `finished` 并被
/// `conn_stop_pmtud()` 关掉，数据报此后永远钉在 1200 字节。所以这里以前那句
/// "多出来的 300 只是回旋余地、不会变成一个 1500 字节的数据报"是**自证**的：
/// 它之所以成立，恰恰因为 PMTUD 已经被这条上限关死了。
///
/// 也不能取到远超 1444（比如 `NGTCP2_MAX_TX_UDP_PAYLOAD_SIZE`=65527）：探测表
/// 最大就到 1444，多出来的只是给"对端可以发更大"留的口子，而我们的接收缓冲
/// 只有 `kRecvBufLen`(4096)。
///
/// ⚠️ 落点缓冲**必须 ≥ 探测表的最大一档**：`conn_write_pmtud_probe()` 里有一句
/// `if (probelen > destlen) return 0;` —— 缓冲比 1444 小的话探测包会**静默**发
/// 不出去，PMTUD 又变回死的，而且一点声音都没有。
const size_t kDatagramBufLen = 1500;

// 这两条守的是**同一个缺陷的两个入口**，都是"改一个数就把 PMTUD 静默关掉"，
// 而且关掉之后一切照常工作、只是慢——正是要靠编译器拦下来的那种。
//
// 1) 钉回下限 1200 ⇒ `conn_start_pmtud()` 的硬上限 ≤1200 ⇒ 探测表四档全越界。
static_assert(kDatagramBufLen > NGTCP2_MAX_UDP_PAYLOAD_SIZE,
              "数据报上限不能钉回 NGTCP2_MAX_UDP_PAYLOAD_SIZE(1200)：那会把 "
              "ngtcp2 自带的 PMTUD 整条关掉（它的探测表四档都在 1200 以上），"
              "2 MB 一笔要多发约 17% 的数据报。见 kDatagramBufLen 的说明。");
// 2) 缓冲小于探测表最大一档 ⇒ `conn_write_pmtud_probe()` 里
//    `if (probelen > destlen) return 0;` 把探测包悄悄丢掉，同上。
static_assert(kDatagramBufLen >= 1444,
              "落点缓冲必须 ≥ ngtcp2 自带 PMTUD 探测表的最大一档(1492-48=1444)，"
              "否则探测包会被 conn_write_pmtud_probe() 静默丢弃，PMTUD 又变回死的。");

/// 单次 `do_flush()` 的轮数上限。正常两三轮就空，这条只是防挂死 ——
/// ngtcp2 万一一直报"还能写"，loop 线程不能停在这儿。
const unsigned kMaxFlushRounds = 128;

/// RFC 9000 §10.2.1 允许的 CONNECTION_CLOSE 重发次数上限。
const int kMaxCloseSends = 3;

/// `stream_send::op_marks` 里"已经报过 `on_write` 的前缀"攒到多长才真的从头部
/// 挪掉。见 `settle()` 第 2 条：每来一次确认就 erase 一遍是 O(待报数)，批量写
/// 的场景下会变成 O(n²)。这个数只决定"多久挪一次"，不影响任何语义。
const size_t kOpMarkCompactMin = 256;

/// RFC 9002 §6.1.2 的 `kGranularity`：rttvar 那一项的下限。
const ngtcp2_duration kGranularity = 1 * NGTCP2_MILLISECONDS;

/// RFC 9000 §18.2 给 `max_ack_delay` 的默认值，也就是 `fill_transport_params()`
/// **没有改**的那个值（它只改流量控制的几格）。PTO 的公式里有它一项。
const ngtcp2_duration kDefaultMaxAckDelay = 25 * NGTCP2_MILLISECONDS;

/// 把 `sockaddr` 的族翻译成长度。传进来的地址没有长度参数（签名是
/// `const struct sockaddr*`），而 ngtcp2 要 `ngtcp2_socklen`。
/// 只认 AF_INET / AF_INET6 —— 别的族 QUIC 走不了，返回 0 让调用方拒绝。
int sockaddr_len(const struct sockaddr* sa) {
  if (sa == nullptr) return 0;
  if (sa->sa_family == AF_INET) return static_cast<int>(sizeof(struct sockaddr_in));
  if (sa->sa_family == AF_INET6) {
    return static_cast<int>(sizeof(struct sockaddr_in6));
  }
  return 0;
}

/// 指过去，不拷贝 —— `src` 的生命周期由调用方保证（`path_` 那两个缓冲区就是
/// 为此存在的）。
ngtcp2_addr make_addr(const struct sockaddr* src, int srclen) {
  ngtcp2_addr a;
  a.addr = const_cast<ngtcp2_sockaddr*>(
      reinterpret_cast<const ngtcp2_sockaddr*>(src));
  a.addrlen = static_cast<ngtcp2_socklen>(srclen);
  return a;
}

/// 把 `in_callback_` 的加减配成对 —— 回调里有好几条 early return，
/// 手写 `--` 迟早漏一次，而漏一次的表现是"之后所有 flush 都只记意图、不发包"，
/// 也就是一条**静默不发数据**的连接，查起来很贵。
struct cb_guard {
  int& depth;
  explicit cb_guard(int& d) : depth(d) { ++depth; }
  ~cb_guard() { --depth; }
  cb_guard(const cb_guard&) = delete;
  cb_guard& operator=(const cb_guard&) = delete;
};

/// 「收到多少个 ACK-eliciting 包才**立刻**回一个 ACK」的那个下限。
///
/// ngtcp2 的默认值是 2（`ngtcp2_settings_default()`），对应 RFC 9000 §13.2.1
/// 那句 "an endpoint SHOULD send an ACK frame after receiving at least two
/// ack-eliciting packets"。那个默认值在**回环 / 局域网**上代价很大，原因是一
/// 道乘法：`ngtcp2_conn_ack_delay_expiry()` 的到期时刻是
/// `first_unacked_ts + min(max_ack_delay, max(srtt/8, 1ns))`，而回环上 `srtt`
/// 只有几十微秒 ⇒ 延迟 ACK 的窗口塌到微秒级，`ack_thresh` 就成了**唯一**还在
/// 起作用的闸门，于是接收端几乎每收两个包就回一个 ACK。
///
/// 实测（`build-quicwin`，MinGW Release，2 MiB push × 15 轮，同机同轮交错，
/// 内核实发数据报数 + 服务端 send 计数）：服务端每轮发出的控制包在
/// `ack_thresh=2` 时是 **821** 个，改成 16 之后是 **161** 个；客户端载荷包数
/// 不变（1623）。总数据报 2445 → 1783 / 轮，中位耗时 19.25 ms → 13.81 ms
/// （+38% 吞吐）。再往上加到 32 / 64 / 128 都不再有系统差（16 已经在平台
/// 上），所以取**到达平台的最小值**——离上面那句 SHOULD 的偏离最小。
///
/// **为什么在广域网上几乎无影响**：那里 `srtt/8` 是毫秒量级（30 ms RTT ⇒
/// 3.75 ms），远大于包间隔，**延迟 ACK 计时器**才是那个闸门 —— 无论
/// `ack_thresh` 取多少，ACK 都会在 `srtt/8` 之内出去。也就是说这一档只在低
/// RTT 链路上真正生效，而那里的瓶颈本来就是"每个数据报一次 syscall"。
///
/// **代价说清楚**：ACK 变稀 ⇒ 对端检测丢包、推进拥塞窗口的反馈变慢。上界是
/// 延迟 ACK 计时器（`max_ack_delay` = 25 ms，RFC 9000 §18.2 的默认值），协议
/// 允许；但**有丢包的链路上这是一笔真实取舍**，不是白拿。本机回环无丢包，
/// 量到的全是收益 —— 别把这个数读成"公网也 +38%"。
const size_t kAckThreshold = 16;

/**
 * @brief 把两边共用的流控/传输参数装上。
 *
 * 数值取的是"够测试与一般 RPC 用"的一档，不是调优过的值：窗口按流 256 KiB、
 * 按连接 1 MiB，双向与单向各允许对端开 100 条。
 *
 * **"把窗口开大"这条优化试过了，本机量不出收益，所以没改。** 本仓的
 * `bench/bench_quic.cpp`（`--mode=push`，2 MB 单流）在 1 MiB/4 MiB 与
 * 256 KiB/1 MiB 两档下各跑 3×15 轮，最小值差在 ±3% 以内 —— 也就是噪声。
 * 原因读码就能看到：接收端在每个 `recv_stream_data` 里**立刻**
 * `ngtcp2_conn_extend_max_stream_offset()`（见那个回调），所以流水线一旦铺开，
 * 窗口更新就连续到达，发送端不会每 256 KiB 停一次；窗口大小只影响**铺开那一下**
 * 的深度，而那是一次性的。curl 把窗口提到 10× 是另一个形状的负载（它的接收端
 * 不是"接到就还"）。
 *
 * 同一个函数里还装了 `max_udp_payload_size`（单个数据报的上限）。它和上面那组
 * 窗口**不是一回事**：窗口是"这一档够用了"的取舍，而它是"我们到底收得下多大
 * 的包"的**事实陈述**，默认那 65527 在我们这儿是假的（接收缓冲只有 4096）。
 * 见下面那一处赋值。
 *
 * ⚠️ 这几个数是**对外可见的传输参数**（进 Initial 包），动它等于改线上一档行为。
 * 想动，先拿 `bench_quic` 量出跨过噪声的收益。
 */
void fill_transport_params(ngtcp2_transport_params* params,
                           uint64_t idle_timeout_ms) {
  ngtcp2_transport_params_default(params);
  params->initial_max_stream_data_bidi_local = 256 * 1024;
  params->initial_max_stream_data_bidi_remote = 256 * 1024;
  params->initial_max_stream_data_uni = 256 * 1024;
  params->initial_max_data = 1024 * 1024;
  params->initial_max_streams_bidi = 100;
  params->initial_max_streams_uni = 100;
  // 我们**收**得下的单个数据报上限，也就是对端被允许发过来的最大值。
  //
  // 默认值是 `NGTCP2_DEFAULT_MAX_RECV_UDP_PAYLOAD_SIZE`(65527)，而两只端点的
  // 接收缓冲都只有 `kRecvBufLen`(4096) —— 今天撞不上，因为对端也是本库、也把
  // 自己钉在同一个上限上。但那是**靠对端自觉**：一个照 RFC 走的第三方实现完全
  // 可以合法地发一个 65527 字节的数据报（我们自己就是这么宣布的），我们会在
  // 内核那一层就被截断，AEAD 校验失败后被当成丢包重传 —— 不是内存安全缺陷，
  // 是一处**静默的性能悬崖**，而且只在跨实现时出现。宣布成我们真收得下的那一
  // 档才是诚实的。
  //
  // 取 `kDatagramBufLen` 而不是 `kRecvBufLen`：两个端点的发送上限与接收上限
  // 因此是同一个数，行为对称，也不会出现"对端发 4096、我们只探到 1444"这种
  // 单边拉长的形状。
  params->max_udp_payload_size = kDatagramBufLen;
  params->max_idle_timeout =
      static_cast<ngtcp2_duration>(idle_timeout_ms) * NGTCP2_MILLISECONDS;
  // CID 的可用数量。1 是协议允许的最小值，8 是常见默认 —— 取 8 是为了让
  // 服务端那张路由表真的有多个键要维护（只留 1 个的话，"所有 CID 都指回
  // 同一个连接"这条正确性在测试里根本压不到）。
  params->active_connection_id_limit = 8;
}

}  // namespace

/// 两边共用的回调表。
///
/// **这一格是"必须逐条填对"的地方**：漏掉 `encrypt` / `decrypt` / `hp_mask`
/// 里任何一个，连接会在发第一个包时失败（不是静默降级）；漏掉
/// `recv_client_initial`（服务端）或 `client_initial`（客户端），Initial 密钥
/// 就永远派生不出来 —— 而那两个派生函数**不是公开 API**，没有别的地方能补。
///
/// 定义在匿名命名空间**之外**：它要取 `&quic_session::cb_*`，而那些跳板是私有的。
void quic_session::fill_callbacks(ngtcp2_callbacks* cbs, bool is_server) {
  std::memset(cbs, 0, sizeof(*cbs));

  if (is_server) {
    // 服务端从对端首包里派生 Initial 密钥的唯一入口。
    cbs->recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
  } else {
    // 客户端**第一个包**的 Initial 密钥唯一入口。它必须发生在
    // `ngtcp2_conn_set_tls_native_handle` 之后 —— 也就是第一次
    // `writev_stream` 之前。顺序错了会在这里失败。
    cbs->client_initial = ngtcp2_crypto_client_initial_cb;
  }

  // TLS 那一半直接交给上游的包装函数，不自己再包一层。
  cbs->recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;

  // 加解密/头保护的六件套。缺一不可，且**必须**是 crypto 后端提供的那几个 ——
  // 自己实现就等于在库里再写一份 AEAD。
  cbs->encrypt = ngtcp2_crypto_encrypt_cb;
  cbs->decrypt = ngtcp2_crypto_decrypt_cb;
  cbs->hp_mask = ngtcp2_crypto_hp_mask_cb;
  cbs->update_key = ngtcp2_crypto_update_key_cb;
  cbs->delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
  cbs->delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;

  // 那条"收到 Retry 就换 token 重发 Initial"的路。不填的话服务端一 Retry
  // 客户端就再也连不上（表现为一条永远握不上手的连接，没有任何报错）。
  cbs->recv_retry = ngtcp2_crypto_recv_retry_cb;
  cbs->version_negotiation = ngtcp2_crypto_version_negotiation_cb;
  cbs->get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;

  // 本层真正关心、要往外报的几件事。
  cbs->handshake_completed = &quic_session::cb_handshake_completed;
  cbs->recv_stream_data = &quic_session::cb_recv_stream_data;
  cbs->acked_stream_data_offset = &quic_session::cb_acked_stream_data_offset;
  cbs->stream_open = &quic_session::cb_stream_open;
  // 只填 `_2` 那一格：填了它之后 ngtcp2 就不再调另一个（`stream_close`），
  // 两个都填只会让同一次关闭报两遍。
  cbs->stream_close2 = &quic_session::cb_stream_close2;
  cbs->extend_max_local_streams_bidi =
      &quic_session::cb_extend_max_local_streams_bidi;
  cbs->extend_max_local_streams_uni =
      &quic_session::cb_extend_max_local_streams_uni;

  // 连接 ID 的发放与退休：服务端那条 UDP 口的路由表靠这两条维护。
  cbs->get_new_connection_id2 = &quic_session::cb_get_new_connection_id2;
  cbs->remove_connection_id = &quic_session::cb_remove_connection_id;

  // 随机数。ngtcp2 用它生成 Initial 的包号混淆与 CID —— 返回 void，
  // 没有错误通道，所以实现里只能尽力而为。
  cbs->rand = &quic_session::cb_rand;

  // 对端重置了一条流：**读侧的收尾信号**。它到得比 `stream_close2` 早，
  // 而对端 reset 之后本端常常还在等着自己那一半的数据 —— 那个等待只能由这一格
  // 结束。没有它，"对端不要了"与"对端还在慢慢发"在读侧长得一模一样。
  cbs->stream_reset = &quic_session::cb_stream_reset;

  // 对端叫我们别再发了（STOP_SENDING）。**方向与上面那格相反**，所以是另一格：
  // reset 报"我读不动了"，这一格报"你别发了"。少了它，一条被对端取消的流上，
  // 本层会继续把队列里的字节排出去 —— 而 RFC 9000 §3.5 要的是"回应一个
  // RESET_STREAM，然后停下"。
  //
  // **线上的那个 RESET_STREAM 不用我们发**：ngtcp2 在自己的收帧路径里
  // （`conn_recv_stop_sending`）先调这一格、紧接着就自己 `conn_reset_stream()`
  // 排好那个帧（除非那条流的数据早已 FIN 且全部被确认 —— 那种情况下本来就没
  // 什么好取消的）。所以这一格是**纯通知**：本层把它转给应用，让应用去取消它
  // 自己那份还没交给 `write_stream()` 的内容。这里**不要**再调
  // `shutdown_stream()` —— 那会重复排一个 RESET_STREAM。
  cbs->recv_stop_sending = &quic_session::cb_recv_stop_sending;

  // **故意没填的几格，写在这里免得后来人以为是漏了：**
  //
  // - `path_validation`：本层不做连接迁移 —— 地址对不上的包直接按"不可用路径"
  //   丢掉（见 `read_pkt` 里那段）。
  // - `stream_stop_sending`：**"本端自己把读方向关了"的事后通知**，不是对端的
  //   动作（对端的那个是上面刚填的 `recv_stop_sending`）。不填有两条理由，
  //   第二条是硬的：
  //     1. 没有消费者 —— 关读方向这件事**就是调用方自己刚下的决定**，再通知
  //        它一遍等于把同一个事实说两次。
  //     2. **它是从写循环里响的。** ngtcp2 在 `ngtcp2_conn_writev_stream` 的
  //        内部循环里调它（`ngtcp2_conn.c` 那个 `conn_call_stream_stop_sending`
  //        就在排队 STOP_SENDING 帧的那一段里），而我们的应用回调全都是
  //        "用户可以顺手再 `write_stream()` 一下"的形状 —— 从那一格往上报，
  //        就等于允许在 `ngtcp2_conn_writev_stream` 的栈里再调一次它，而
  //        ngtcp2 明令禁止重入写函数。`do_flush()` 那条路上没有 `in_callback_`
  //        护栏（那个护栏守的是**收包**那半边），所以这条重入真的会走到 ngtcp2
  //        里去。要接它得先给写路径也加一道"回调期间只记意图"的闸，那是另一件
  //        事，本批不做。
  // - `extend_max_remote_streams_bidi` / `extend_max_stream_data`：放开流数与
  //   放开流控窗口这两件事**一定**随着某个包到达，而每个包处理完都会走一次
  //   `do_flush()` —— 被卡住的那条流在下一轮自然被重新拾起，不需要专门通知。
  // - `recv_rx_key` / `recv_tx_key`：ngtcp2 的例子拿它们打印密钥（`--show-secret`）。
  //   ngtcp2 **自己**装密钥，这两格纯粹是通知。不填不影响握手。
  // - `extend_max_stream_data`：流控窗口放开这件事**一定**是随着某个包到的，
  //   而每个包处理完都会走一次 `do_flush()` —— 于是被卡住的那条流在下一轮
  //   自然被重新拾起，不需要专门的通知。
  // - `recv_stateless_reset2`：无状态重置不在本批范围。
}

// =========================================================================
// 构造 / 析构
// =========================================================================

quic_session::quic_session() {
  // `ngtcp2_path_storage` 是没有构造函数的 POD，默认初始化留下的是**不定值**，
  // 而里面的 `ngtcp2_addr::addr` 会被一路传到 ngtcp2。先清零，`init_*` 再
  // 用 `ngtcp2_path_storage_init` 正式装一次。
  std::memset(&path_, 0, sizeof(path_));
  std::memset(&conn_ref_, 0, sizeof(conn_ref_));
  std::memset(&close_ccerr_, 0, sizeof(close_ccerr_));
}

quic_session::~quic_session() {
  // 顺序不能反：`conn_` 里存着 `ossl_ctx_` 作为 TLS 原生句柄，先把它删掉。
  if (conn_ != nullptr) {
    ngtcp2_conn_del(conn_);
    conn_ = nullptr;
  }
  if (ossl_ctx_ != nullptr) {
    // SSL 归 `ossl_ctx_` 持有（`SSL_new` 出来的，不在任何 SSL_CTX 的池子里），
    // 所以必须**手工** `SSL_free`。顺序照 `examples/tls_session_base_ossl.cc`
    // 的析构抄：先断掉 app_data（那里指回本对象），再 free，最后删 ctx。
    SSL* ssl = ngtcp2_crypto_ossl_ctx_get_ssl(ossl_ctx_);
    if (ssl != nullptr) {
      SSL_set_app_data(ssl, nullptr);
      SSL_free(ssl);
    }
    ngtcp2_crypto_ossl_ctx_del(ossl_ctx_);
    ossl_ctx_ = nullptr;
  }
  ssl_ = nullptr;
}

// =========================================================================
// TLS 那一套
// =========================================================================

ngtcp2_conn* quic_session::crypto_get_conn(ngtcp2_crypto_conn_ref* ref) {
  if (ref == nullptr || ref->user_data == nullptr) return nullptr;
  quic_session* self = static_cast<quic_session*>(ref->user_data);
  return self->conn_;
}

int quic_session::setup_tls(uvcpp_ssl_context* ssl_ctx, bool is_server,
                            const std::vector<std::string>& alpn,
                            const char* server_name) {
  if (ssl_ctx == nullptr || ssl_ctx->raw_ctx() == nullptr) {
    // QUIC 没有明文模式（ALPN 是 TLS 扩展），没配上下文就是没配。
    return UV_EINVAL;
  }

  // ALPN 要在 `SSL_new` **之前**装到 SSL_CTX 上：装的是 ctx 的默认值，
  // 新建的 `SSL` 继承它。
  //
  // 服务端只在调用方自己没装过选择回调时装 —— `set_alpn_select_protos` 写的是
  // `SSL_CTX_set_alpn_select_cb`，装两次是后者覆盖前者，会把调用方自己的
  // 选择逻辑吃掉。
  if (!alpn.empty()) {
    if (is_server) {
      if (!ssl_ctx->has_alpn_select()) {
        (void)ssl_ctx->set_alpn_select_protos(alpn);
      }
    } else {
      (void)ssl_ctx->set_alpn_protos(alpn);
    }
  }

  ssl_ = SSL_new(ssl_ctx->raw_ctx());
  if (ssl_ == nullptr) return UV_ENOMEM;

  // 原生句柄那一层间接：ngtcp2 v1.25.0 的 conn 里存的是
  // `ngtcp2_crypto_ossl_ctx*`，**不是**裸 `SSL*`。先建 ctx（SSL 可以后填，
  // 所以这里先传 nullptr 再 set），随后的两个 configure 函数会在 SSL 上装好
  // QUIC 的 TLS 回调与 ALPN 扩展。
  if (ngtcp2_crypto_ossl_ctx_new(&ossl_ctx_, nullptr) != 0) {
    SSL_free(ssl_);
    ssl_ = nullptr;
    return UV_ENOMEM;
  }
  ngtcp2_crypto_ossl_ctx_set_ssl(ossl_ctx_, ssl_);

  const int rv = is_server ? ngtcp2_crypto_ossl_configure_server_session(ssl_)
                           : ngtcp2_crypto_ossl_configure_client_session(ssl_);
  if (rv != 0) {
    SSL_free(ssl_);
    ssl_ = nullptr;
    ngtcp2_crypto_ossl_ctx_set_ssl(ossl_ctx_, nullptr);
    ngtcp2_crypto_ossl_ctx_del(ossl_ctx_);
    ossl_ctx_ = nullptr;
    return UV_EINVAL;
  }

  // ngtcp2 回调我们时给的是这个 `conn_ref`，它再指回 `conn_` —— 这条链是
  // OpenSSL crypto 后端唯一能找回来的路，所以 `user_data` 必须是 this。
  conn_ref_.get_conn = &quic_session::crypto_get_conn;
  conn_ref_.user_data = this;
  SSL_set_app_data(ssl_, &conn_ref_);

  if (is_server) {
    SSL_set_accept_state(ssl_);
  } else {
    SSL_set_connect_state(ssl_);
    // SNI 只能在**这里**给：SSL 是本函数内部 new 出来的，外面拿不到，
    // 而 `SSL_set_tlsext_host_name` 必须在握手之前调。
    if (server_name != nullptr && server_name[0] != '\0') {
      (void)SSL_set_tlsext_host_name(ssl_, server_name);
    }
  }

  return 0;
}

// =========================================================================
// 建立
// =========================================================================

int quic_session::init_client(uvcpp_ssl_context* ssl_ctx,
                              const std::vector<std::string>& alpn,
                              const struct sockaddr* local,
                              const struct sockaddr* remote,
                              const char* server_name, uint64_t idle_timeout_ms,
                              quic_send_fn send) {
  // 只能建一次。再调会漏掉上一次的 conn_ 与 SSL —— 那是明确的调用方错误，
  // 用一个错误码挡在前面，好过静默泄漏。
  if (conn_ != nullptr || ossl_ctx_ != nullptr) return UV_EALREADY;
  if (send == nullptr) return UV_EINVAL;

  const int locallen = sockaddr_len(local);
  const int remotelen = sockaddr_len(remote);
  if (locallen == 0 || remotelen == 0) return UV_EINVAL;

  int rv = setup_tls(ssl_ctx, false, alpn, server_name);
  if (rv != 0) return rv;

  send_ = send;

  // 路径必须是**我们自己的**缓冲区：ngtcp2 会把 `ngtcp2_path` 的两个指针
  // 一存到底，后续每次写包都可能回填远端地址（连接迁移时它会变）。用栈上的
  // 临时变量装它会立刻变成野指针。
  ngtcp2_path_storage_init(
      &path_, reinterpret_cast<const ngtcp2_sockaddr*>(local),
      static_cast<ngtcp2_socklen>(locallen),
      reinterpret_cast<const ngtcp2_sockaddr*>(remote),
      static_cast<ngtcp2_socklen>(remotelen), nullptr);

  // 客户端首包的 DCID：随便挑一个随机值。RFC 9000 §7.2 只要求 Initial 的
  // DCID **至少 8 字节**（`NGTCP2_MIN_INITIAL_DCIDLEN`），没有别的约束 ——
  // 服务端会把它记成 `original_destination_connection_id` 回给我们校验。
  ngtcp2_cid dcid;
  dcid.datalen = kInitialDcidLen;
  fill_random(dcid.data, kInitialDcidLen);
  // 本端 SCID：此后对端发包时往 DCID 里填它。
  ngtcp2_cid scid;
  scid.datalen = kClientScidLen;
  fill_random(scid.data, kClientScidLen);

  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  settings.initial_ts = now_ns();
  // 数据报上限。**不能钉回 `NGTCP2_MAX_UDP_PAYLOAD_SIZE`(1200)** —— 那等于把
  // ngtcp2 自带的 PMTUD 整条关掉（它拿这个值当硬上限，而探测表四档都在 1200
  // 以上），理由与代价见 `kDatagramBufLen`。
  settings.max_tx_udp_payload_size = kDatagramBufLen;
  // ACK 频率。**默认那个 2 在低 RTT 链路上太密** —— 理由、实测数字与代价见 `kAckThreshold`。
  settings.ack_thresh = kAckThreshold;

  ngtcp2_transport_params params;
  fill_transport_params(&params, idle_timeout_ms);

  ngtcp2_callbacks cbs;
  fill_callbacks(&cbs, false);

  rv = ngtcp2_conn_client_new(&conn_, &dcid, &scid, &path_.path,
                              NGTCP2_PROTO_VER_V1, &cbs, &settings, &params,
                              nullptr, this);
  if (rv != 0) {
    conn_ = nullptr;
    return rv;
  }

  // 这一步必须在第一次 `writev_stream` **之前**：Initial 密钥由
  // `cb_client_initial` 派生，而它要拿 TLS 原生句柄。
  ngtcp2_conn_set_tls_native_handle(conn_, ossl_ctx_);

  state_ = quic_connection_state::HANDSHAKING;
  return 0;
}

int quic_session::init_server(uvcpp_ssl_context* ssl_ctx,
                              const std::vector<std::string>& alpn,
                              const struct sockaddr* local,
                              const struct sockaddr* remote,
                              uint64_t idle_timeout_ms, quic_send_fn send,
                              const ngtcp2_cid& client_scid,
                              const ngtcp2_cid& original_dcid) {
  if (conn_ != nullptr || ossl_ctx_ != nullptr) return UV_EALREADY;
  if (send == nullptr) return UV_EINVAL;

  const int locallen = sockaddr_len(local);
  const int remotelen = sockaddr_len(remote);
  if (locallen == 0 || remotelen == 0) return UV_EINVAL;
  if (client_scid.datalen == 0 || original_dcid.datalen == 0) return UV_EINVAL;

  int rv = setup_tls(ssl_ctx, true, alpn, nullptr);
  if (rv != 0) return rv;

  send_ = send;

  ngtcp2_path_storage_init(
      &path_, reinterpret_cast<const ngtcp2_sockaddr*>(local),
      static_cast<ngtcp2_socklen>(locallen),
      reinterpret_cast<const ngtcp2_sockaddr*>(remote),
      static_cast<ngtcp2_socklen>(remotelen), nullptr);

  // 本端（服务端）自己的 SCID：新挑一个随机值。此后对端发包时往 DCID 里填它。
  ngtcp2_cid server_scid;
  server_scid.datalen = kServerScidLen;
  fill_random(server_scid.data, kServerScidLen);

  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  settings.initial_ts = now_ns();
  // 数据报上限。**不能钉回 `NGTCP2_MAX_UDP_PAYLOAD_SIZE`(1200)** —— 那等于把
  // ngtcp2 自带的 PMTUD 整条关掉（它拿这个值当硬上限，而探测表四档都在 1200
  // 以上），理由与代价见 `kDatagramBufLen`。
  settings.max_tx_udp_payload_size = kDatagramBufLen;
  // ACK 频率。**默认那个 2 在低 RTT 链路上太密** —— 理由、实测数字与代价见 `kAckThreshold`。
  settings.ack_thresh = kAckThreshold;

  ngtcp2_transport_params params;
  fill_transport_params(&params, idle_timeout_ms);
  // RFC 9000 §7.3 的 `original_destination_connection_id`：**服务端必须发**，
  // 而且值必须是对端**第一个 Initial 包里的 DCID**。客户端拿到会跟自己记的
  // 比一下，不一致就报 `TRANSPORT_PARAMETER_ERROR` 直接断掉 —— 也就是说这里
  // 填错了不是"不太规范"，是**握不上手**。
  params.original_dcid = original_dcid;
  params.original_dcid_present = 1;

  ngtcp2_callbacks cbs;
  fill_callbacks(&cbs, true);

  // 第一个 cid 参数叫 `dcid`，但按 ngtcp2 的说明它要的是
  // "客户端首包里的 **Source** Connection ID" —— 也就是 `client_scid`。
  // 头里原话：*"|dcid| is a Destination Connection ID, and is usually the
  // Connection ID that appears in client Initial packet as Source Connection ID."*
  rv = ngtcp2_conn_server_new(&conn_, &client_scid, &server_scid, &path_.path,
                              NGTCP2_PROTO_VER_V1, &cbs, &settings, &params,
                              nullptr, this);
  if (rv != 0) {
    conn_ = nullptr;
    return rv;
  }

  ngtcp2_conn_set_tls_native_handle(conn_, ossl_ctx_);

  state_ = quic_connection_state::HANDSHAKING;

  // 两张路由键都报到表里（见 `on_new_cid` 的 `@note`）：
  //   - `server_scid`：对端收到我们的第一个包之后就会改用它当 DCID；
  //   - `original_dcid`：**在那之前**对端一直用它，也就是端点的表在这条连接刚
  //     建出来的那一瞬间就必须有它 —— 服务端递给本函数的 `pkt` 之后紧接着的
  //     重传用的还是这个 CID。
  // 两者都指回同一条连接，这正是 RFC 9000 §5.1 说的"一个连接可以有多个 CID"。
  if (events_.on_new_cid) {
    events_.on_new_cid(server_scid.data, server_scid.datalen, nullptr, 0);
    events_.on_new_cid(original_dcid.data, original_dcid.datalen, nullptr, 0);
  }
  return 0;
}

// =========================================================================
// 服务端路由
// =========================================================================

bool quic_session::peek_dcid(const uint8_t* pkt, size_t pktlen,
                             size_t short_dcidlen, std::string* out) {
  if (pkt == nullptr || pktlen == 0) return false;

  ngtcp2_pkt_hd hd;
  std::memset(&hd, 0, sizeof(hd));

  // 首字节的最高位是 RFC 9000 §17.2 的 Header Form：1 = 长包头。
  // ngtcp2 把这个位定义在内部的 `ngtcp2_pkt.h` 里（不对外），所以这里写它的
  // 值 —— 它是**协议冻结的一个位**，不是某个版本的上游实现细节。
  if ((pkt[0] & 0x80u) != 0) {
    // 长包头：DCID 长度写在包上（§17.2 的 DCID Length 那一字节），照它解。
    if (ngtcp2_pkt_decode_hd_long(&hd, pkt, pktlen) < 0) return false;
  } else {
    // 短包头：长度不在包上，用本端自己选的 CID 长度去解（§17.3.1）。
    if (short_dcidlen == 0) return false;
    if (ngtcp2_pkt_decode_hd_short(&hd, pkt, pktlen, short_dcidlen) < 0) {
      return false;
    }
  }

  if (hd.dcid.datalen == 0) return false;
  out->assign(reinterpret_cast<const char*>(hd.dcid.data), hd.dcid.datalen);
  return true;
}

bool quic_session::accept_new(const uint8_t* pkt, size_t pktlen,
                              std::string* client_scid,
                              std::string* original_dcid) {
  if (pkt == nullptr || pktlen == 0) return false;

  ngtcp2_pkt_hd hd;
  // `ngtcp2_accept` 只认长包头里的 Initial，且要求包长到 1200（Initial 的下限）
  // —— 客户端那边由 ngtcp2 自己填充到 1200，所以这条不会误杀。畸形的、重传的
  // 非首包、以及 0-RTT 一律返回非 0，这里就当"不是新连接"。
  if (ngtcp2_accept(&hd, pkt, pktlen) != 0) return false;

  if (hd.scid.datalen == 0 || hd.dcid.datalen == 0) return false;
  client_scid->assign(reinterpret_cast<const char*>(hd.scid.data),
                      hd.scid.datalen);
  original_dcid->assign(reinterpret_cast<const char*>(hd.dcid.data),
                        hd.dcid.datalen);
  return true;
}

size_t quic_session::scid_len() { return kServerScidLen; }

// =========================================================================
// 收发
// =========================================================================

int quic_session::read_pkt(const uint8_t* data, size_t len,
                           const struct sockaddr* peer, int peerlen) {
  if (conn_ == nullptr) return UV_ENOTCONN;
  if (state_ == quic_connection_state::CLOSED) return UV_ENOTCONN;
  if (state_ == quic_connection_state::DRAINING) return UV_ENOTCONN;
  if (len == 0) return 0;

  // 收到的包来自哪条路径。远端那一半用**实参里那个地址**（不是 `path_` 里存的）：
  // 服务端的一条 UDP 口上跑着很多条连接，`path_` 只是建连接那一刻的快照。
  // 本层不做连接迁移，所以地址对不上时 ngtcp2 会按"不可用路径"丢掉 —— 丢掉
  // 是对的（静默换路径比丢包危险）。
  ngtcp2_path path = path_.path;
  if (peer != nullptr && peerlen > 0) {
    path.remote = make_addr(peer, peerlen);
  }
  ngtcp2_pkt_info pi;
  std::memset(&pi, 0, sizeof(pi));

  // 这一调用会同步跑我们那一堆回调（也就同步跑用户的 `on_*`），
  // 而回调里不能调 ngtcp2 的写函数 —— 由各个 `cb_*` 里的 `cb_guard` 守住。
  const int rv = ngtcp2_conn_read_pkt(conn_, &path, &pi, data, len, now_ns());
  if (rv != 0) {
    const int mapped = handle_conn_error(rv);
    if (mapped != 0) return mapped;
  }

  // 回调期间攒下的意图到这里才兑现。**先关后发**：已经要关了就不必再发数据。
  if (want_close_) {
    const int code = want_close_code_;
    want_close_ = false;
    want_flush_ = false;
    do_close(code);
  } else {
    flush();
  }
  return 0;
}

void quic_session::flush() {
  if (conn_ == nullptr) return;
  // 在回调里：只记意图。理由见文件头第 1 条 —— 这是 ngtcp2 的硬要求。
  if (in_callback_ > 0) {
    want_flush_ = true;
    return;
  }
  do_flush();
}

void quic_session::do_flush() {
  if (conn_ == nullptr) return;
  // 这几个状态下一个包都不该由本层发出：
  // - CLOSED / DRAINING：连接已经终结，只等计时器（RFC 9000 §10.2.2 明写
  //   draining 期的端点 "MUST NOT send"）。
  // - CLOSING：终端包已经发过，重发归 `on_expiry` 管（次数有限）。
  if (state_ == quic_connection_state::CLOSED ||
      state_ == quic_connection_state::DRAINING ||
      state_ == quic_connection_state::CLOSING) {
    return;
  }

  const ngtcp2_tstamp ts = now_ns();
  uint8_t buf[kDatagramBufLen];
  ngtcp2_path_storage ps;
  ngtcp2_path_storage_zero(&ps);
  ngtcp2_pkt_info pi;
  std::memset(&pi, 0, sizeof(pi));

  // 第一轮**永远**发一次，哪怕一条流都没有数据：握手包（CRYPTO）、ACK、
  // HANDSHAKE_DONE 这些都要靠这一轮出去。之后每一轮只在"还有流数据待发"
  // 时才继续 —— 否则这个循环没有终止判据。
  //
  // **但挑流这件事每一轮都要做，第一轮也不例外。** 这里从前是第一轮空跑
  // （`stream_id` 留在 -1），于是有一条只在静下来的连接上才现形的错：新攒的
  // 流数据第一次 `writev_stream` 就撞上"此刻没有非流帧要发"的 0 返回值，被
  // 下面那条 `nwrite == 0 → break` 当成收工 —— 字节留在队列里，等一个永远
  // 不会来的包。`quic_stream_func.cpp` 第 3 段就是从回调外面写的那条路，
  // 它一测就红。
  //
  // 挑流不花钱：没有可发的流时它返回 false，第一轮照样会发出那一次空请求。
  bool first = true;
  for (unsigned round = 0; round < kMaxFlushRounds; ++round) {
    int64_t stream_id = -1;
    ngtcp2_vec datav[kMaxStreamVecs];
    size_t datavcnt = 0;
    size_t handed = 0;
    uint32_t flags = 0;

    const bool have = next_sendable_stream(&stream_id, datav, &datavcnt,
                                           &handed, &flags);
    if (!have && !first) break;
    first = false;

    ngtcp2_ssize datalen = -1;
    // **不用 `NGTCP2_WRITE_STREAM_FLAG_MORE`。** 用了它之后，ngtcp2 要求
    // "在写满一个包之前不许调任何别的 API、只能一直调这个直到它返回 0 或一个
    // 正数"，而且 `NGTCP2_ERR_WRITE_MORE` 那条分支必须原参重试 —— 那是把多个
    // STREAM 帧塞进同一个包用的优化。反过来，不用它时 ngtcp2 的默认行为就是
    // "一个包一个 STREAM 帧"（它自己的文档用词就是 "by default"），语义简单
    // 得多：每一轮要么拿到一个**完整的包**，要么 0，要么一个错误码。
    const ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
        conn_, &ps.path, &pi, buf, sizeof(buf), &datalen, flags, stream_id,
        datavcnt != 0 ? datav : nullptr, datavcnt, ts);

    if (nwrite < 0) {
      if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED ||
          nwrite == NGTCP2_ERR_STREAM_NOT_FOUND ||
          nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
        // 这三种都是"**这一条流**现在发不了"，不是连接错误。ngtcp2 允许换一条
        // 流继续往同一个包里装 —— 但那是 `MORE` 那条路（见上）。这里直接收工：
        // 剩下的流会在下一个包到达时被重新拾起，而流控/流状态的每一次变化
        // 都**必须**由一个包带来，所以那一刻一定会来。
        if (nwrite == NGTCP2_ERR_STREAM_NOT_FOUND ||
            nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
          // 这一条对我们来说已经不存在/不能写了。留着它，每一轮 flush 都会在
          // 第一轮撞同一堵墙，把它后面那些流**全部饿死**。
          send_q_.erase(stream_id);
        }
        break;
      }
      // 其余都是连接级错误：发一个终包再收尾。
      handle_conn_error(static_cast<int>(nwrite));
      break;
    }

    if (datalen >= 0 && stream_id >= 0) {
      std::map<int64_t, stream_send>::iterator it = send_q_.find(stream_id);
      if (it != send_q_.end()) {
        stream_send& s = it->second;
        const size_t n = static_cast<size_t>(datalen);
        // `*pdatalen` 按 ngtcp2 的契约不可能超过给它的那段（`handed`）。这里
        // 仍然夹一道：`sent_off > write_off` 会让 `write_off - sent_off` 下溢成
        // 一个天文数字，而那个数会原样变成下一轮递给 ngtcp2 的长度 —— 正是
        // 越界读的形状（见头里 `stream_send` 的注释）。少记一笔最多让这段
        // 字节晚一轮出去，不会越界。
        if (n <= handed) s.sent_off += n;
        // FIN 落地的判据：ngtcp2 只在"给它的数据**全部**被编进 STREAM 帧"时才
        // 会设 fin 位（文档原话："If all given data is encoded as STREAM frame in
        // dest, and if flags & FIN is nonzero, fin flag is set"）。所以这里比的是
        // "这次编进去的字节数 == 这次给它的字节数"，不是"队列空了"。
        if (s.fin_requested && !s.fin_written &&
            (datavcnt == 0 || n == handed)) {
          s.fin_written = true;
        }
      }
    }

    // 0 = 成功但这一轮没有东西要发（拥塞窗口满、或被放大攻击限流）。
    // **不是失败**，也不该再转下去。
    if (nwrite == 0) break;

    if (ps.path.remote.addr != nullptr) {
      send_(buf, static_cast<size_t>(nwrite), ps.path.remote.addr,
            static_cast<int>(ps.path.remote.addrlen));
    }
  }

  // 文档要求：调过 `writev_stream` 之后必须调它，否则 PTO / 拥塞控制的
  // 发包时刻不会推进，表现为"包发完了但重传计时器永远不响"。
  ngtcp2_conn_update_pkt_tx_time(conn_, ts);
}

// =========================================================================
// 到期
// =========================================================================

ngtcp2_duration quic_session::closing_period() const {
  if (conn_ == nullptr) return 3 * kGranularity;
  // ngtcp2 v1.25.0 里 `compute_pto` 是这个形状（`ngtcp2_conn.c` 的
  // `compute_pto()` + `ngtcp2_conn_compute_pto()`），但那个函数**不是公开
  // API**，本层拿不到，所以照它的公式自己算一遍。三项里 `max_ack_delay` 取的
  // 是默认值：`PTO` 用的是**对端**的 `max_ack_delay`，而握手后的连接里那个
  // 值就是两侧 `transport_params_default()` 给出来的 25 ms（两边都是本库）。
  ngtcp2_conn_info  ci;
  ngtcp2_conn_get_conn_info2(conn_, &ci);
  const ngtcp2_duration var =
      (4 * ci.rttvar > kGranularity) ? 4 * ci.rttvar : kGranularity;
  return 3 * (ci.smoothed_rtt + var + kDefaultMaxAckDelay);
}

uint64_t quic_session::next_expiry() const {
  if (conn_ == nullptr) return UINT64_MAX;
  if (is_closed()) return UINT64_MAX;
  // 关闭期由本层自己兜（见头里那段说明）：CLOSING 状态下不再看 ngtcp2 的
  // 那几个计时器 —— 它们的下一个到期点是空闲超时，那是 30 s 量级的东西，
  // 而 RFC 9000 §10.2.1 要的是 3 × PTO。
  if (state_ == quic_connection_state::CLOSING) return close_deadline_;
  return ngtcp2_conn_get_expiry2(conn_);
}

bool quic_session::on_expiry(uint64_t now) {
  if (conn_ == nullptr) return true;
  if (is_closed()) return true;

  const int rv = ngtcp2_conn_handle_expiry(conn_, static_cast<ngtcp2_tstamp>(now));
  if (rv == NGTCP2_ERR_IDLE_CLOSE) {
    // 空闲超时。**这一格什么都不发** —— ngtcp2 在 `handle_expiry` 的说明里
    // 写死了："it means that an idle timer has fired for this particular
    // connection. In this case, drop the connection **without** calling
    // `ngtcp2_conn_write_connection_close`."（RFC 9000 §10.1：空闲关闭不算
    // 一次连接错误，所以也没有包要发。）
    finalize(static_cast<int>(NGTCP2_ERR_IDLE_CLOSE));
    return true;
  }
  if (rv != 0) {
    handle_conn_error(rv);
    return is_closed();
  }

  if (state_ == quic_connection_state::CLOSING) {
    // 关闭期到了：按 RFC 9000 §10.2.1 重发终端包（有次数上限），发不动了就
    // 收尾。**注意不要走到 `do_flush()`** —— CLOSING 状态下它什么都不做。
    if (close_sends_ >= kMaxCloseSends || !send_close_packet()) {
      finalize(0);
      return true;
    }
    return false;
  }

  // 其余情况：到期多半意味着"有东西可以重发了"，照常吐一轮。
  do_flush();
  return is_closed();
}

// =========================================================================
// 关闭
// =========================================================================

void quic_session::close(int error_code) {
  if (conn_ == nullptr) return;
  if (is_closed() || state_ == quic_connection_state::CLOSING ||
      state_ == quic_connection_state::DRAINING) {
    return;
  }
  if (in_callback_ > 0) {
    // 用户就是在 `on_stream_data` 里调的 `close()`。这里**不能**直接做 ——
    // 见文件头第 1 条。记下来，等最外层那两处兑现。
    want_close_ = true;
    want_close_code_ = error_code;
    return;
  }
  do_close(error_code);
}

void quic_session::do_close(int error_code) {
  if (conn_ == nullptr || is_closed()) return;

  ngtcp2_ccerr_default(&close_ccerr_);
  if (error_code != 0) {
    // 非 0 = 应用层错误码，走 CONNECTION_CLOSE 的 0x1D 那型
    // （`NGTCP2_CCERR_TYPE_APPLICATION`）。
    ngtcp2_ccerr_set_application_error(
        &close_ccerr_, static_cast<uint64_t>(error_code), nullptr, 0);
  }
  // error_code == 0 时用 `ccerr_default`：TRANSPORT 型 + `NGTCP2_NO_ERROR`，
  // 也就是一个干净的关闭。

  close_sends_ = 0;
  (void)send_close_packet();
  state_ = quic_connection_state::CLOSING;
  // 关闭期的终点。**必须在 state_ 之后** —— `next_expiry()` 就是按 state_ 分
  // 支去报这一格的，先置它再置 state_ 的话中间那一瞬会报出一个还没算好的值。
  close_deadline_ = now_ns() + static_cast<uint64_t>(closing_period());
}

bool quic_session::send_close_packet() {
  if (conn_ == nullptr) return false;

  uint8_t buf[kDatagramBufLen];
  ngtcp2_path_storage ps;
  ngtcp2_path_storage_zero(&ps);
  ngtcp2_pkt_info pi;
  std::memset(&pi, 0, sizeof(pi));

  const ngtcp2_ssize nwrite = ngtcp2_conn_write_connection_close(
      conn_, &ps.path, &pi, buf, sizeof(buf), &close_ccerr_, now_ns());
  ++close_sends_;

  // 0 是**成功**：头里原话 "Otherwise, it does not produce any data, and
  // returns 0." —— 比如已经处在关闭态、或这一轮没有东西要发。别把 0 当失败
  // 去报错，那会在一条已经关掉的连接上刷出一串假错误。
  //
  // `NGTCP2_ERR_CLOSING` 也算"没东西发"：ngtcp2 进了关闭态之后**不再受理第二次
  // 关闭**（`ngtcp2_conn.c` 里那个 `case NGTCP2_CS_CLOSING: return
  // NGTCP2_ERR_CLOSING;`）。所以调用方那个"重发 CONNECTION_CLOSE 最多
  // `kMaxCloseSends` 次"的循环**实际上只会走一轮** —— 不是漏了，是上游不给第二次
  // 机会，而第一包已经发出去了。把这条写在这里，是为了下一个读到那个循环的人
  // 不必再去查一遍上游。
  if (nwrite == NGTCP2_ERR_CLOSING) return false;
  if (nwrite <= 0) return false;
  if (ps.path.remote.addr == nullptr) return false;

  send_(buf, static_cast<size_t>(nwrite), ps.path.remote.addr,
        static_cast<int>(ps.path.remote.addrlen));
  return true;
}

int quic_session::handle_conn_error(int rv) {
  switch (rv) {
    case NGTCP2_ERR_RETRY:
      // 服务端回了 Retry：ngtcp2 自己把状态换好了，我们只要**再写一个包**
      // 就行（新的 Initial 带着 token）。返回 0 让调用方照常 flush。
      return 0;

    case NGTCP2_ERR_DISCARD_PKT:
      // 这个包本身没用，连接好着。
      return 0;

    case NGTCP2_ERR_CLOSING:
      // **本端自己关了之后，来的包一律是这个。** `ngtcp2_conn_read_pkt` 在
      // 连接进入 `NGTCP2_CS_CLOSING` 后第一件事就是返回它（`ngtcp2_conn.c`
      // 里那个 `switch (conn->state)`），而 `ngtcp2_conn_write_connection_close`
      // 一旦把连接推进 CLOSING 就**不再受理第二次关闭**（再调同样返回它）。
      //
      // 这句话是上面那条 `read_pkt` 行为的原因，也是这里必须吞掉它的原因：
      // **它不是错误，是"我这边正在关，这个包我不看了"** —— 丢弃就对了。当成
      // 致命错误处理等于"调完 `close()` 之后收到对端的任何一个包就把连接立刻
      // 判死"，关闭期（RFC 9000 §10.2.1）一个字节都走不完。上游文档里那句
      // "no further packet transmission is allowed" 说的正是这个状态。
      //
      // 代价是**本端关的连接永远看不到对端在 CONNECTION_CLOSE 里给的错误码**：
      // 承载它的那个包也在这个状态里被丢掉了。所以对端主动关时才有的那套
      // 「`> 0` = 应用错误码」的解读，只适用于**对端先关**那条路。
      return 0;

    case NGTCP2_ERR_DRAINING: {
      // 对端发了 CONNECTION_CLOSE：进入 draining 期（RFC 9000 §10.2.2）。
      // 这一段**一个包都不许发**，`do_flush()` 里那条判断守着它。
      //
      // 报给上层的东西按这条规则定：
      //   > 0 = 对端的**应用**错误码（它自己给的那个数）
      //   = 0 = 干净关闭（NO_ERROR）
      //   < 0 = 传输层级的原因，`NGTCP2_ERR_DRAINING` 表示"对端关了我们，
      //         原因在传输层"。
      int code = static_cast<int>(NGTCP2_ERR_DRAINING);
      const ngtcp2_ccerr* ccerr = ngtcp2_conn_get_ccerr2(conn_);
      if (ccerr != nullptr) {
        if (ccerr->type == NGTCP2_CCERR_TYPE_APPLICATION) {
          if (ccerr->error_code != 0) code = static_cast<int>(ccerr->error_code);
        } else if (ccerr->error_code == NGTCP2_NO_ERROR) {
          code = 0;
        }
      }
      state_ = quic_connection_state::DRAINING;
      finalize(code);
      return 0;
    }

    case NGTCP2_ERR_DROP_CONN:
      // ngtcp2 说"这个连接必须丢掉"（比如无状态重置）。没有包要发。
      finalize(static_cast<int>(NGTCP2_ERR_DROP_CONN));
      return 0;

    default:
      break;
  }

  // 其余都是连接级错误（`NGTCP2_ERR_CRYPTO` —— 比如证书校验没过、
  // `NGTCP2_ERR_HANDSHAKE_TIMEOUT`、协议错误……）。按 ngtcp2 的说明，
  // "If any other negative error is returned, call
  // `ngtcp2_conn_write_connection_close` to get terminal packet"。
  //
  // 但要**先判断现在能不能发**：这个函数也可能是在回调里被调到的
  // （`handle_expiry` 那条路），那时写函数是禁的。禁的时候就直接收尾 ——
  // 对端会靠自己的空闲超时收场，而我们这一侧的账已经清了。
  if (conn_ != nullptr && in_callback_ == 0 &&
      state_ != quic_connection_state::CLOSING) {
    ngtcp2_ccerr_default(&close_ccerr_);
    ngtcp2_ccerr_set_liberr(&close_ccerr_, rv, nullptr, 0);
    close_sends_ = 0;
    (void)send_close_packet();
    state_ = quic_connection_state::CLOSING;
  }
  finalize(rv);
  return rv;
}

void quic_session::finalize(int error_code) {
  // **只报一次**：这条路上有好几个入口（对端关、我们关完、超时、协议错），
  // 而同一个连接终结两次会让上层的"摘表 + 销毁"跑第二遍。
  if (close_reported_) return;
  close_reported_ = true;
  state_ = quic_connection_state::CLOSED;
  if (events_.on_close) events_.on_close(error_code);
}

void quic_session::refresh_state() {
  // 唯一的用途：把"握手完成"这一个标志翻译成状态。
  // 用我们**自己**那个 `handshake_done_`（由 `cb_handshake_completed` 置）
  // 而不是 `ngtcp2_conn_get_handshake_completed()` —— 两者含义相同，但前者
  // 正是 `on_handshake_completed` 已经发出去的那件事，同源就不会漂。
  if (state_ == quic_connection_state::HANDSHAKING && handshake_done_) {
    state_ = quic_connection_state::ESTABLISHED;
  }
}

// =========================================================================
// 查询
// =========================================================================

std::string quic_session::alpn_selected() const {
  if (ssl_ == nullptr) return std::string();
  const unsigned char* proto = nullptr;
  unsigned int protolen = 0;
  SSL_get0_alpn_selected(ssl_, &proto, &protolen);
  if (proto == nullptr || protolen == 0) return std::string();
  return std::string(reinterpret_cast<const char*>(proto), protolen);
}

// =========================================================================
// 流
// =========================================================================

int64_t quic_session::open_stream(bool bidi) {
  if (conn_ == nullptr) return static_cast<int64_t>(UV_ENOTCONN);
  int64_t stream_id = -1;
  // `NGTCP2_ERR_STREAM_ID_BLOCKED` 如实透传：那是"对端还没放开流数上限"，
  // 与"出错了"是两回事，调用方要能分开（等 `on_streams_*_available`）。
  const int rv = bidi ? ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr)
                      : ngtcp2_conn_open_uni_stream(conn_, &stream_id, nullptr);
  if (rv != 0) return static_cast<int64_t>(rv);
  return stream_id;
}

int quic_session::write_stream(int64_t stream_id, const char* data, size_t len,
                               bool fin) {
  if (conn_ == nullptr) return UV_ENOTCONN;
  if (state_ == quic_connection_state::CLOSED ||
      state_ == quic_connection_state::CLOSING ||
      state_ == quic_connection_state::DRAINING) {
    return UV_ENOTCONN;
  }
  if (len != 0 && data == nullptr) return UV_EINVAL;

  std::map<int64_t, stream_send>::iterator it = send_q_.find(stream_id);
  if (it == send_q_.end()) {
    // 第一次往这条流上写：建条目。不校验流号存不存在 —— 那要问 ngtcp2，
    // 而问它的时机是 `do_flush()`（`NGTCP2_ERR_STREAM_NOT_FOUND` 那条路会
    // 把条目摘掉）。在这里多问一次只是把同一个判断写两遍。
    it = send_q_.insert(std::make_pair(stream_id, stream_send())).first;
  }
  stream_send& s = it->second;

  if (s.closed_write) return static_cast<int>(NGTCP2_ERR_STREAM_SHUT_WR);
  if (s.fin_requested || s.fin_written) {
    // FIN 之后本端不再发 —— 这是 QUIC 的流语义，不是本层的规矩。
    return static_cast<int>(NGTCP2_ERR_STREAM_SHUT_WR);
  }

  if (len != 0) {
    // 往最后一块里塞，塞满就开新块。**每块出生时 `reserve(kChunkSize)` 且
    // 此后只写到这里为止** —— 这两条合起来保证块内地址一生不变，而那是
    // 递给 ngtcp2 的指针能一直有效的前提（见头里 `stream_send` 的注释）。
    // 用 `insert` 而不是 `resize`+`memcpy`：写入的是**从未递出去过**的那一段
    // （起点 `write_off >= sent_off`），所以既不搬字节也不碰已有数据。
    size_t done = 0;
    while (done < len) {
      if (s.chunks.empty() ||
          s.chunks.back().size() == stream_send::kChunkSize) {
        s.chunks.emplace_back();
        s.chunks.back().reserve(stream_send::kChunkSize);
      }
      ::std::vector<uint8_t>& c = s.chunks.back();
      const size_t room = stream_send::kChunkSize - c.size();
      const size_t take = room < (len - done) ? room : (len - done);
      c.insert(c.end(), data + done, data + done + take);
      done += take;
    }
    s.write_off += len;
  }
  // 记一个"这次调用覆盖到哪儿"的界标（绝对偏移 = 这次受理之后的下一个字节）。
  // 一次 `on_write` 对应一个，与 `uv_write` 一次请求一次回调同一形状。
  // 用绝对偏移而不是块下标，是为了丢块时这里一个数都不用改。
  s.op_marks.push_back(s.write_off);
  if (fin) s.fin_requested = true;

  // **攒下的字节要有人推动才会出去。** 这一句是"写"这条路上唯一能把
  // `send_q_` 变成数据报的地方：ngtcp2 自己不发包，端点的 `on_recv` /
  // `on_timer` 也只是把包喂进来、把到期时刻兑现。少了它，从**回调外面**
  // （刚 `connect()` 完、或者用户自己的定时器里）调 `write_stream()` 就会
  // 一直躺在队列里，直到下一个包恰好到达 —— 而在一条已经静下来的连接上，
  // 那个包永远不会来，等到的是空闲超时。
  //
  // 在回调里调（用户在 `on_read` 里回写）时它只记意图，真正的发包在
  // `read_pkt` 的尾巴上 —— 理由见文件头第 1 条，这里是那条规则的另一个入口。
  flush();
  return 0;
}

int quic_session::shutdown_stream(int64_t stream_id, uint64_t app_error_code) {
  if (conn_ == nullptr) return UV_ENOTCONN;

  std::map<int64_t, stream_send>::iterator it = send_q_.find(stream_id);
  if (it != send_q_.end()) {
    // 与 `write_stream(..., fin=true)` 的区别就在这里：那条是"把排队的写完
    // 再关"，这条是"排队的也不要了"。`ngtcp2_conn_shutdown_stream_write`
    // 的语义正是后者（没发完的丢掉），所以队列里剩下的字节直接作废 ——
    // 但条目本身留着：已发出去、还没被确认的那些字节仍要靠它对齐
    // `acked_stream_data_offset`。
    it->second.closed_write = true;
  }
  const int rv =
      ngtcp2_conn_shutdown_stream_write(conn_, 0, stream_id, app_error_code);
  if (rv != 0) return rv;
  // RESET_STREAM 帧是 `shutdown_stream_write` **排上**的，不是它发出去的 ——
  // 与 `write_stream()` 那条同一个道理（那里的注释解释了为什么这一句非有不可）。
  flush();
  return 0;
}

int quic_session::shutdown_stream_read(int64_t stream_id,
                                       uint64_t app_error_code) {
  if (conn_ == nullptr) return UV_ENOTCONN;

  // **与 `shutdown_stream()` 的对称处和不对称处各一条。**
  //
  // 对称的是形状：都是一句 ngtcp2 调用 + 一次 flush（帧是排上的，不是发出去
  // 的，理由同上）。
  //
  // 不对称的是**没有** `send_q_` 那一段：关读方向完全不碰发送队列 —— 那正是
  // 这条 API 存在的意义（"我读不动了"与"我不发了"是两件事）。写方向此后照常，
  // 排队里的字节照常上线、照常有 `on_write`。
  const int rv =
      ngtcp2_conn_shutdown_stream_read(conn_, 0, stream_id, app_error_code);
  if (rv != 0) return rv;
  flush();
  return 0;
}

uint64_t quic_session::streams_left(bool bidi) const {
  if (conn_ == nullptr) return 0;
  // 用 `_2` 那两个重载：不带后缀的版本收的是非 const `ngtcp2_conn*`，而且
  // ngtcp2 自己标了弃用（"Use ngtcp2_conn_get_streams_*_left2 instead"）。
  // 本层是 `const` 成员函数，取 `const` 重载还省掉一次不必要的
  // const_cast 味道。
  return bidi ? ngtcp2_conn_get_streams_bidi_left2(conn_)
              : ngtcp2_conn_get_streams_uni_left2(conn_);
}

bool quic_session::next_sendable_stream(int64_t* stream_id, ngtcp2_vec* datav,
                                        size_t* datavcnt, size_t* handed,
                                        uint32_t* flags) const {
  // 按流号升序取**第一条**能发的。不做轮转：一条流发完再轮到下一条，
  // 对"先来的先出去"这件事更直观，而本层也不打算在这里做调度公平性
  // （真要做，那是给 `ngtcp2_conn_writev_stream` 配 `MORE` 那套的地方）。
  for (std::map<int64_t, stream_send>::const_iterator it = send_q_.begin();
       it != send_q_.end(); ++it) {
    const stream_send& s = it->second;
    if (s.closed_write) continue;
    const uint64_t remaining = s.write_off - s.sent_off;
    const bool has_fin = s.fin_requested && !s.fin_written;
    if (remaining == 0 && !has_fin) continue;

    *stream_id = it->first;
    size_t cnt = 0;
    size_t total = 0;
    if (remaining != 0) {
      // 把 `[sent_off, write_off)` 描述成至多 `kMaxStreamVecs` 段连续内存。
      //
      // 除最后一块外每块都是满的（本结构的稳态），所以"第几块 + 块内偏移"
      // 一次除法就算得出来。**描不完也没关系**：一次调用只可能编进一个包
      // （~1200 B），剩下的下一轮再描 —— 而每轮都是从同一个 `sent_off` 起算，
      // 所以描述永远是"从下一个待发字节开始"的，不会漏。
      const uint64_t rel = s.sent_off - s.base_off;
      size_t idx = static_cast<size_t>(rel / stream_send::kChunkSize);
      size_t off = static_cast<size_t>(rel % stream_send::kChunkSize);
      for (; idx < s.chunks.size() && cnt < kMaxStreamVecs; ++idx) {
        const ::std::vector<uint8_t>& c = s.chunks[idx];
        if (off >= c.size()) break;  // 只在最后一块上可能（见上面的稳态）
        datav[cnt].base = const_cast<uint8_t*>(c.data() + off);
        datav[cnt].len = c.size() - off;
        total += datav[cnt].len;
        ++cnt;
        off = 0;
      }
    } else {
      // 只剩一个 FIN 要发：**空数据 + FIN 位**是合法的一次调用（ngtcp2 文档：
      // "Empty data is treated specially, and it is only accepted if no data,
      // including the empty data, is submitted to a stream or FIN is set"）。
    }
    *datavcnt = cnt;
    *handed = total;
    *flags = has_fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0;
    return true;
  }
  return false;
}

void quic_session::note_acked(stream_send& s, uint64_t offset, uint64_t datalen) {
  if (datalen == 0) return;
  uint64_t start = offset;
  const uint64_t end = offset + datalen;
  if (end <= s.ack_off) return;  // 整个区间都已经在连续前缀里了
  if (start < s.ack_off) start = s.ack_off;

  if (start == s.ack_off) {
    s.ack_off = end;
  } else {
    // 前面还有洞：先记下来（与已有的洞取并集），等前面的补上再并进 `acked`。
    std::map<uint64_t, uint64_t>::iterator it = s.holes.lower_bound(start);
    if (it != s.holes.begin()) {
      std::map<uint64_t, uint64_t>::iterator prev = it;
      --prev;
      if (prev->second >= start) {
        if (prev->second >= end) return;  // 完全被已有区间盖住了
        start = prev->first;
        s.holes.erase(prev);
      }
    }
    uint64_t merged_end = end;
    it = s.holes.lower_bound(start);
    while (it != s.holes.end() && it->first <= merged_end) {
      if (it->second > merged_end) merged_end = it->second;
      s.holes.erase(it++);
    }
    s.holes[start] = merged_end;
    return;
  }

  // `acked` 涨上去了：把因此变连续的洞一口气并进来。
  // 从 `begin()` 而不是 `lower_bound(acked)` 起扫 —— 后者会**漏掉**那些起点
  // 在旧 `acked` 之下、终点在它之上的区间，而那种区间恰恰是最该被并掉的。
  // 这里的小 map 元素数是个位数，多扫几下不值得省。
  for (;;) {
    std::map<uint64_t, uint64_t>::iterator it = s.holes.begin();
    if (it == s.holes.end() || it->first > s.ack_off) break;
    if (it->second > s.ack_off) s.ack_off = it->second;
    s.holes.erase(it);
  }
}

void quic_session::settle(stream_send& s, int64_t stream_id) {
  // 1) 丢掉已确认的**整块前缀**。`ack_off` 之后一个字节都不动 —— 那些还要重传。
  //
  // **这里不每来一次确认就搬一遍字节。** 旧版是一条连续 `vector`，前端
  // `erase(begin, begin+drop)` 的代价是 O(剩余字节) 的 memmove，而本函数每个
  // 包头都会跑一次（每个包都带确认），于是一笔 n 字节的传输要搬 O(n²) 的字节：
  // 实测 2 MB 单流整笔 168 ms，其中约 160 ms 是这一句。
  //
  // 分块之后丢一块是 O(1)（`deque::pop_front` + 归还一块），摊还到每字节最多
  // 一次分配一次归还 —— 与"攒够再挪一遍"同一量级，但**同时**满足 ngtcp2 那条
  // "交给它的数据必须原地不动"的硬要求（旧版的挪动正违反它，见头里
  // `stream_send` 的注释）。判据是"这一块**整个**落在确认前缀里"：只确认了一半
  // 的块不能丢，它后半段对 ngtcp2 来说仍要重传，而它已经不在我们手里了。
  while (!s.chunks.empty()) {
    const uint64_t chunk_end =
        s.base_off + static_cast<uint64_t>(s.chunks.front().size());
    if (chunk_end > s.ack_off) break;
    s.chunks.pop_front();
    s.base_off = chunk_end;
  }

  // 2) 把因此完成的 `write_stream()` 报出去。`op_marks` 天然升序，因此
  //    回调顺序就是调用顺序。
  while (s.op_done < s.op_marks.size() && s.ack_off >= s.op_marks[s.op_done]) {
    ++s.op_done;
    if (events_.on_write) events_.on_write(stream_id, 0);
  }
  // 界标用绝对偏移，所以第 1 条丢块不影响它们。已经回调过的那些
  // **攒够一批再挪**，理由与第 1 条逐字相同：每来一次确认就从头部 erase
  // 一遍是 O(待报数)，调用方"一次写、等一次回调"的常见形状下它一直很小，
  // 而批量写的场景会把它变成 O(n²)。`op_done` 本身就是那个待报前缀的长度。
  //
  // `op_marks` **不是**递给 ngtcp2 的东西，所以它这里怎么挪都不违反那条
  // "原地不动"的约束 —— 两条约束的适用面别混。
  if (s.op_done >= kOpMarkCompactMin) {
    s.op_marks.erase(s.op_marks.begin(),
                     s.op_marks.begin() + static_cast<ptrdiff_t>(s.op_done));
    s.op_done = 0;
  }
}

// =========================================================================
// 随机数
// =========================================================================

void quic_session::fill_random(uint8_t* dest, size_t destlen) {
  if (dest == nullptr || destlen == 0) return;

  // `uv_random` 的同步形式：`req` 与 `cb` 都传 nullptr。
  if (uv_random(nullptr, nullptr, dest, destlen, 0, nullptr) == 0) return;

  // 走到这里说明系统的 CSPRNG 拿不到。**没有错误通道能报出去**（`ngtcp2_rand`
  // 的 typedef 返回 void，ngtcp2 也没有给这条路准备失败码），所以只能退到
  // 一个弱填充 —— 它不是"安全的降级"，只是"别让 CID / 包号混淆全零"的兜底。
  // 真要上生产，得让"uv_random 失败"这件事在别处能被看见。
  for (size_t i = 0; i < destlen; ++i) {
    dest[i] = static_cast<uint8_t>(uv_hrtime() >> (8 * (i % 8)));
  }
}

// =========================================================================
// ngtcp2 回调的跳板
// =========================================================================

int quic_session::cb_handshake_completed(ngtcp2_conn* conn, void* user_data) {
  (void)conn;
  quic_session* self = static_cast<quic_session*>(user_data);
  cb_guard guard(self->in_callback_);

  self->handshake_done_ = true;
  self->refresh_state();

  // ALPN 是**协商**出来的，不是我们设进去的那个 —— 读 SSL 里的结果，
  // 不要回头去看配置。
  if (self->events_.on_alpn != nullptr) {
    const std::string alpn = self->alpn_selected();
    if (!alpn.empty()) self->events_.on_alpn(alpn);
  }
  if (self->events_.on_handshake_completed != nullptr) {
    self->events_.on_handshake_completed();
  }
  return 0;
}

int quic_session::cb_recv_stream_data(ngtcp2_conn* conn, uint32_t flags,
                                      int64_t stream_id, uint64_t offset,
                                      const uint8_t* data, size_t datalen,
                                      void* user_data, void* stream_user_data) {
  (void)offset;
  (void)stream_user_data;
  quic_session* self = static_cast<quic_session*>(user_data);
  cb_guard guard(self->in_callback_);

  if (self->events_.on_stream_data != nullptr) {
    self->events_.on_stream_data(
        stream_id, data, datalen,
        (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0);
  }

  // 流控：收到的每一个字节都要**还回去**，否则窗口涨到头之后对端就再也发不动
  // 了。两条的顺序与返回值都不同（前者 `int`、后者 `void`），照签名写：
  // 前者的失败是真失败（比如 stream_id 指向一条本地单向流），要报给 ngtcp2。
  if (datalen > 0) {
    if (ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen) != 0) {
      return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    ngtcp2_conn_extend_max_offset(conn, datalen);
  }
  return 0;
}

int quic_session::cb_acked_stream_data_offset(ngtcp2_conn* conn,
                                              int64_t stream_id, uint64_t offset,
                                              uint64_t datalen, void* user_data,
                                              void* stream_user_data) {
  (void)conn;
  (void)stream_user_data;
  quic_session* self = static_cast<quic_session*>(user_data);
  cb_guard guard(self->in_callback_);

  std::map<int64_t, stream_send>::iterator it = self->send_q_.find(stream_id);
  if (it == self->send_q_.end()) return 0;

  note_acked(it->second, offset, datalen);
  // 注意 `settle()` 会调用户的 `on_write`，而用户代码完全可能在里面
  // `write_stream()` 同一条流 —— `send_q_` 是 `std::map`，节点不搬家，
  // 所以这个引用在插入之后依然有效。
  self->settle(it->second, stream_id);
  return 0;
}

int quic_session::cb_stream_open(ngtcp2_conn* conn, int64_t stream_id,
                                 void* user_data) {
  (void)conn;
  quic_session* self = static_cast<quic_session*>(user_data);
  cb_guard guard(self->in_callback_);
  if (self->events_.on_stream_open != nullptr) {
    self->events_.on_stream_open(stream_id);
  }
  return 0;
}

int quic_session::cb_stream_reset(ngtcp2_conn* conn, int64_t stream_id,
                                  uint64_t final_size, uint64_t app_error_code,
                                  void* user_data, void* stream_user_data) {
  (void)conn;
  (void)final_size;
  (void)stream_user_data;
  quic_session* self = static_cast<quic_session*>(user_data);
  cb_guard guard(self->in_callback_);

  // 对端把发送方向重置了。**这里不动 `send_q_`**：这条流本端可能还有自己在发
  // 的东西，写方向的收场归 `stream_close2` 管（那时剩余的 `on_write` 会以
  // `NGTCP2_ERR_STREAM_SHUT_WR` 收场）。这一格只报读方向。
  if (self->events_.on_stream_reset != nullptr) {
    self->events_.on_stream_reset(stream_id, app_error_code);
  }
  return 0;
}

int quic_session::cb_recv_stop_sending(ngtcp2_conn* conn, int64_t stream_id,
                                       uint64_t app_error_code, void* user_data,
                                       void* stream_user_data) {
  (void)conn;
  (void)stream_user_data;
  quic_session* self = static_cast<quic_session*>(user_data);
  cb_guard guard(self->in_callback_);

  // **这里不排 RESET_STREAM。** ngtcp2 在调完本回调之后立刻就自己排了那个帧
  // （见 `fill_callbacks` 里那一格的长注释）。本层再调一次
  // `ngtcp2_conn_shutdown_stream_write` 只会往线上多塞一个 RESET_STREAM。
  //
  // **也不动 `send_q_`。** 队列里那些已经被 `write_stream()` 受理的字节，
  // 它们的收场由 `stream_close2` 统一报（那时剩下的 `on_write` 会以
  // `NGTCP2_ERR_STREAM_SHUT_WR` 收场）。在这里就地清掉队列，反而会让那些
  // 完成通知少报 —— 上层在等它们。
  if (self->events_.on_stop_sending != nullptr) {
    self->events_.on_stop_sending(stream_id, app_error_code);
  }
  return 0;
}

int quic_session::cb_stream_close2(ngtcp2_conn* conn, uint32_t flags,
                                   int64_t stream_id, uint64_t rx_app_error_code,
                                   uint64_t tx_app_error_code, void* user_data,
                                   void* stream_user_data) {
  (void)conn;
  (void)flags;
  (void)tx_app_error_code;
  (void)stream_user_data;
  quic_session* self = static_cast<quic_session*>(user_data);
  cb_guard guard(self->in_callback_);

  std::map<int64_t, stream_send>::iterator it = self->send_q_.find(stream_id);
  if (it != self->send_q_.end()) {
    stream_send& s = it->second;
    // 还没被确认的那些 `write_stream()` **永远不会完成**了。现在就说清楚 ——
    // 让调用方在 `on_write` 上等一个不回来的东西是最坏的一种 API 行为。
    for (size_t i = s.op_done; i < s.op_marks.size(); ++i) {
      if (self->events_.on_write != nullptr) {
        self->events_.on_write(stream_id,
                               static_cast<int>(NGTCP2_ERR_STREAM_SHUT_WR));
      }
    }
    self->send_q_.erase(it);
  }

  // 报**收方向**那个应用错误码：发送方向的是我们自己 reset 时给的，调用方
  // 本来就知道；收方向这个是唯一一条它拿不到的信息。
  if (self->events_.on_stream_close != nullptr) {
    self->events_.on_stream_close(stream_id, rx_app_error_code);
  }
  return 0;
}

int quic_session::cb_extend_max_local_streams_bidi(ngtcp2_conn* conn,
                                                   uint64_t max_streams,
                                                   void* user_data) {
  (void)conn;
  quic_session* self = static_cast<quic_session*>(user_data);
  cb_guard guard(self->in_callback_);
  if (self->events_.on_streams_bidi_available != nullptr) {
    // `max_streams` 是**累计值**（"本端最多能开多少条"），不是这一回的增量
    // —— ngtcp2 的文档写的是 "the cumulative number of bidirectional streams"。
    self->events_.on_streams_bidi_available(max_streams);
  }
  return 0;
}

int quic_session::cb_extend_max_local_streams_uni(ngtcp2_conn* conn,
                                                  uint64_t max_streams,
                                                  void* user_data) {
  (void)conn;
  quic_session* self = static_cast<quic_session*>(user_data);
  cb_guard guard(self->in_callback_);
  if (self->events_.on_streams_uni_available != nullptr) {
    self->events_.on_streams_uni_available(max_streams);
  }
  return 0;
}

int quic_session::cb_get_new_connection_id2(ngtcp2_conn* conn, ngtcp2_cid* cid,
                                            ngtcp2_stateless_reset_token* token,
                                            size_t cidlen, void* user_data) {
  (void)conn;
  quic_session* self = static_cast<quic_session*>(user_data);
  cb_guard guard(self->in_callback_);

  if (cid == nullptr || cidlen == 0 || cidlen > NGTCP2_MAX_CIDLEN) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  fill_random(cid->data, cidlen);
  cid->datalen = cidlen;
  if (token != nullptr) {
    fill_random(token->data, NGTCP2_STATELESS_RESET_TOKENLEN);
  }

  // 报给端点：**新发的每一个 CID 都要进路由表**，否则对端换到它之后，
  // 那些包会在服务端被当成一条新连接（表现是凭空多出一条连接，不是丢包）。
  if (self->events_.on_new_cid != nullptr) {
    self->events_.on_new_cid(cid->data, cidlen,
                             token != nullptr ? token->data : nullptr,
                             token != nullptr
                                 ? static_cast<size_t>(
                                       NGTCP2_STATELESS_RESET_TOKENLEN)
                                 : static_cast<size_t>(0));
  }
  return 0;
}

int quic_session::cb_remove_connection_id(ngtcp2_conn* conn,
                                          const ngtcp2_cid* cid,
                                          void* user_data) {
  (void)conn;
  quic_session* self = static_cast<quic_session*>(user_data);
  cb_guard guard(self->in_callback_);
  if (cid != nullptr && self->events_.on_remove_cid != nullptr) {
    self->events_.on_remove_cid(cid->data, cid->datalen);
  }
  return 0;
}

void quic_session::cb_rand(uint8_t* dest, size_t destlen,
                           const ngtcp2_rand_ctx* rand_ctx) {
  // 注意这一格**没有 `user_data` 形参**（`ngtcp2_rand` 的 typedef 就三个参数），
  // 所以它连 `self` 都拿不到 —— 只能是静态函数，也只能调用静态的 `fill_random`。
  (void)rand_ctx;
  fill_random(dest, destlen);
}

}  // namespace quic_detail
}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE
