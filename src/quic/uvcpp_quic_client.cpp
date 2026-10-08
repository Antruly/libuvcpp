/**
 * @file src/quic/uvcpp_quic_client.cpp
 * @brief 客户端端点：一条 UDP 口、一个到期定时器、一条连接。
 * @author zhuweiye
 * @version 1.5.1
 *
 * 三件事在这里接起来（内核本身在 `uvcpp_quic_session.cpp`）：
 *
 * 1. **发**：内核给一段要发出去的数据报 + 目的地址，本文件把它拷进一块
 *    `uvcpp_udp_send` 名下的缓冲区、交给 `uvcpp_udp::send`，并在发送回调里把
 *    那块拷贝还掉。**必须拷贝** —— 内核给的指针在回调返回后即失效，而
 *    `uv_udp_send` 是异步的。
 * 2. **收**：UDP 收到的每个数据报原样喂给 `read_pkt`，然后把内核对出来的下一个
 *    到期时刻装进定时器。
 * 3. **到期**：定时器响 → `on_expiry` → 重新装定时器。
 *
 * **本文件里没有一处 `ngtcp2` 类型** —— 内核的公开形状是 `sockaddr` / `int64_t`
 * / 字节缓冲，一条 ngtcp2 的边都不漏出来。这正是 `uvcpp_quic_session.h` 那层
 * 分层的用处。
 *
 * **时序上最要紧的一条**：内核报"连接终结"时，本文件**不能**立刻把它收掉 ——
 * 那一跳是从内核的回调栈里出来的，栈上还压着 `read_pkt` / `on_expiry`，它们
 * 回来以后还要用那个内核。所以终结只**标记**（`pending_teardown`），等
 * `on_recv` / `on_timer` 退到最外层再收。
 */

#include "quic/uvcpp_quic_client.h"

#if UVCPP_QUIC_ENABLE

#include <uv.h>

#include <cstring>
#include <utility>

#include <handle/uvcpp_timer.h>
#include <handle/uvcpp_udp.h>
#include <req/uvcpp_getaddrinfo.h>
#include <req/uvcpp_udp_send.h>
#include <ssl/uvcpp_ssl_context.h>
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_buf.h>

#include "quic/uvcpp_quic_session.h"

namespace uvcpp {

namespace {

/// DNS 解析的等待上限。**这是一条真上限，不是走个过场**：`connect()` 是同步的，
/// 而解析失败/卡住时必须能返回一个错误码，不能把调用方永远挂在里面。
const int kResolveTimeoutMs = 5000;

/// 收到一个数据报时给 libuv 的接收缓冲大小。
///
/// 它必须 ≥ 我们对外的 `max_udp_payload_size`（= 本库的 `kDatagramBufLen`，
/// 1500）—— **不是** ngtcp2 那个下界 `NGTCP2_MAX_UDP_PAYLOAD_SIZE`(1200)：
/// 1200 只是"任何路径都保证能过"的那个值，我们实际会发（也会收）到 1500。
/// 缓冲给小了不会报错，是**静默截断**，表现成"偶尔重传"。4096 有充足余量。
const size_t kRecvBufLen = 4096;

/// 一毫秒的纳秒数。`uv_hrtime()` 与 ngtcp2 的 `timestamp()` 都是纳秒。
const uint64_t kNanosPerMs = 1000000ULL;

/// 按 `sockaddr` 的族给出长度。传进来的地址没有长度参数（内核的签名是
/// `const struct sockaddr*`），而 ngtcp2 要一个明确长度。
int sockaddr_len_of(const struct sockaddr* sa) {
  if (sa == nullptr) return 0;
  if (sa->sa_family == AF_INET) return static_cast<int>(sizeof(sockaddr_in));
  if (sa->sa_family == AF_INET6) return static_cast<int>(sizeof(sockaddr_in6));
  return 0;
}

/// 与对端同族的通配地址（端口 0 = 由内核挑）。
void make_wildcard(int family, sockaddr_storage* out, int* outlen) {
  if (family == AF_INET6) {
    sockaddr_in6 a;
    uv_ip6_addr("::", 0, &a);
    std::memcpy(out, &a, sizeof(a));
    *outlen = sizeof(a);
  } else {
    sockaddr_in a;
    uv_ip4_addr("0.0.0.0", 0, &a);
    std::memcpy(out, &a, sizeof(a));
    *outlen = sizeof(a);
  }
}

}  // namespace

struct uvcpp_quic_client::impl {
  uvcpp_loop* loop      = nullptr;
  bool        owns_loop = true;  ///< 自建那条路为真；共享外部循环时为假

  uvcpp_ssl_context* ssl_ctx = nullptr;
  /// 空 = 用 `quic_default_alpn()`。**不在这里预先塞进去** —— 那样
  /// `set_alpn_protos({})`（"一个都不发"）与"没设过"就分不开了，而这两件事在
  /// TLS 层的结果完全不同（前者握手失败，后者用默认值）。一个空 vector 就是
  /// "没设过"的判据。
  std::vector<std::string> alpn_protos;

  uint64_t idle_timeout_ms = 30000;

  uvcpp_udp*   udp   = nullptr;
  uvcpp_timer* timer = nullptr;
  /// 本对象名下那条连接。**拥有它** —— 收尾时 delete。
  uvcpp_quic_connection* conn = nullptr;

  std::function<void(int)> connect_cb;
  /// `cb` 只跑一次。`connect()` 失败时它**一次都不跑** —— 那条契约由
  /// `quic_api_func.cpp` 钉着。
  bool connect_cb_fired = false;

  /// 内核已经报过终结，连接该收了 —— 但**现在不能收**（见文件头那条）。
  bool pending_teardown = false;

  sockaddr_storage local_sa{};
  int              local_len = 0;
  sockaddr_storage remote_sa{};
  int              remote_len = 0;

  // ---------------------------------------------------------------
  // 工具
  // ---------------------------------------------------------------

  quic_detail::quic_session* sess() {
    if (conn == nullptr) return nullptr;
    return uvcpp_quic_connection::endpoint::session(*conn);
  }

  /// 把循环拨到 `cond` 为真，或超时。**只用于 `connect()` 里那次同步等待**
  /// （DNS 解析）。`UV_RUN_NOWAIT` 保证不会挂在这儿 —— 每一轮都把控制权还给
  /// 本函数，由本函数判超时。
  bool pump_until(std::function<bool()> cond, int timeout_ms) {
    if (loop == nullptr) return false;
    for (int i = 0; i < timeout_ms; ++i) {
      if (cond()) return true;
      loop->run(UV_RUN_NOWAIT);
      // 睡 1ms 的粒度：DNS 解析在 libuv 里是线程池任务，不睡就是拿 CPU 硬转
      // 5000 圈。最坏情况多等 1ms，而 CPU 占用从 100% 降到接近 0。
      uv_sleep(1);
    }
    return cond();
  }

  /**
   * @brief 解析对端地址。字面量走快速路径，主机名走 DNS。
   *
   * 字面量优先是**有意的**：`uv_ip4_addr("1.2.3.4", ...)` 不碰 DNS，所以给
   * 字面量时 `connect()` 是**纯本地**的，没有那 5 秒的等待窗口。用例里给的都是
   * 字面量，所以这条快速路径是被真跑到的，不是理论上的优化。
   */
  int resolve(const char* host, int port, sockaddr_storage* out, int* outlen) {
    if (host == nullptr) return UV_EINVAL;

    sockaddr_in a4;
    if (uv_ip4_addr(host, port, &a4) == 0) {
      std::memcpy(out, &a4, sizeof(a4));
      *outlen = sizeof(a4);
      return 0;
    }
    sockaddr_in6 a6;
    if (uv_ip6_addr(host, port, &a6) == 0) {
      std::memcpy(out, &a6, sizeof(a6));
      *outlen = sizeof(a6);
      return 0;
    }

    uvcpp_getaddrinfo resolver;
    char service[16];
    std::snprintf(service, sizeof(service), "%d", port);

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    // `AF_UNSPEC` 而不是 `AF_INET`：给了主机名就该两种都收。取结果里的第一条
    // ——"多地址轮询/快乐眼球"是 QUIC 的路径管理该做的事，而本层不做连接迁移，
    // 装作会做只会让调用方以为换地址是无缝的。
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    bool             done   = false;
    int              status = 0;
    struct addrinfo* res    = nullptr;
    int rc = resolver.getaddrinfo(
        loop, host, service, &hints,
        [&done, &status, &res](uvcpp_getaddrinfo*, int st, struct addrinfo* r) {
          status = st;
          res    = r;
          done   = true;
        });
    if (rc != 0) return rc;

    if (!pump_until([&done]() { return done; }, kResolveTimeoutMs)) {
      // 超时了。**不能在这里 `freeaddrinfo`** —— 那个请求还在线程池里跑，
      // 它的完成回调迟早会带着一个 `res` 回来，而那时我们已经走了。
      return UV_ETIMEDOUT;
    }
    if (status != 0 || res == nullptr) {
      if (res != nullptr) resolver.freeaddrinfo(res);
      return (status != 0) ? status : UV_EAI_NONAME;
    }

    const int len = static_cast<int>(res->ai_addrlen);
    if (len <= 0 || static_cast<size_t>(len) > sizeof(sockaddr_storage)) {
      resolver.freeaddrinfo(res);
      return UV_EINVAL;
    }
    std::memcpy(out, res->ai_addr, static_cast<size_t>(len));
    *outlen = len;
    resolver.freeaddrinfo(res);
    return 0;
  }

  /// 把内核要发的一个数据报交给 UDP 口。
  void forward(const uint8_t* data, size_t len, const struct sockaddr* peer,
               int peerlen) {
    (void)peerlen;
    if (udp == nullptr || data == nullptr || len == 0) return;

    uv_buf_t bufs[1];
    bufs[0] = uv_buf_init(const_cast<char*>(reinterpret_cast<const char*>(data)),
                          static_cast<unsigned int>(len));

    // ---------------------------------------------------------------------
    // 快路：同步发，一个字节都不拷，也不排完成回调。
    // ---------------------------------------------------------------------
    //
    // `quic_send_fn` 的契约是"`data` 回调返回后即失效"，而 `uv_udp_try_send`
    // 是**同步**的 —— 数据在本次调用内就进了内核，返回之后就不再碰 `data`。
    // 所以这里不需要拷贝。两个调用点给的又都是**栈上**的临时缓冲
    // （`do_flush()` 与 `send_close_packet()` 里的 `uint8_t buf[kDatagramBufLen]`），
    // 于是整条发送路径上每包少一次 `new char[]` + 一次 `memcpy` + 一次 `delete[]`。
    //
    // 顺带省掉的是**每包一次的事件循环轮转**：异步发送要为每个包排一个完成
    // 回调，2 MB 那样的一笔传输就是 1700 多个包、1700 多次多余的循环轮转，
    // 而那些轮转全落在计时窗口里。
    //
    // **只有"发出去了"才走快路**：其余任何返回值（`UV_EAGAIN`、`UV_ENOSYS`、
    // 别的错误码）一律落到下面的慢路。这条不是保守，是必需的 ——
    // 把"快路失败"直接 `return` 掉等于**静默把这个包扔了**，而 QUIC 只会
    // 把它当丢包去重传，症状是"能跑但慢"，查起来很贵。
    //
    // 顺带说清顺序：libuv 在**已经有一条排队的异步发送**时让 `try_send` 返
    // `UV_EAGAIN`（`uv__udp_try_send` 里 `send_queue_count != 0` 那一格），
    // 所以一旦某个包走了慢路，它后面的包也会一直走慢路，不会出现"后发的快路包
    // 插到先发的慢路包前面"。
    const int sent = udp->try_send(bufs, 1, peer);
    if (sent >= 0) return;

    // ---------------------------------------------------------------------
    // 慢路：拷一份交给异步队列。只有上面那条快路走不通时才到这里。
    // ---------------------------------------------------------------------
    char* copy = new char[len];
    std::memcpy(copy, data, len);

    uv_buf_t abufs[1];
    abufs[0] = uv_buf_init(copy, static_cast<unsigned int>(len));

    uvcpp_udp_send* req = new uvcpp_udp_send();
    req->init();
    udp->send(req, abufs, 1, peer, [copy, req](uvcpp_udp_send* r, int status) {
      // 发送结果**不报给内核**：UDP 的失败对 QUIC 来说就是丢包，交给重传处理
      // 才是对的（`send` 报 `UV_EAGAIN` 那种更不该当成连接错误 —— 那只是
      // 这一瞬间发不出去，不是这条连接出了事）。
      (void)status;
      delete[] copy;
      // `self_free_` 默认是 false（`uvcpp_req` 那条约定），所以这里自己收。
      delete r;
    });
  }

  /// 真正的收尾。**只能在最外层退栈之后调**（见文件头）。
  /// @param fire_cb 是否在收尾时把还没跑过的 `connect_cb` 按失败报一次。
  ///        析构那条路传 false —— 对象正在消失，再把用户代码叫回来是在
  ///        别人家的栈上跑用户逻辑。
  void teardown(bool fire_cb) {
    pending_teardown = false;

    if (timer != nullptr) {
      delete timer;
      timer = nullptr;
    }
    // 先收连接（它拥有内核），再收 socket —— 反过来也能对，但先连接能让"内核
    // 还活着的那一小段时间里 socket 已经没了"这个窗口不存在。
    //
    // `delete udp` 发生在**它自己的收包回调里**是安全的：`callback_udp_recv`
    // 把用户回调拷了一份就调，之后什么都不碰（见那边的注释），而
    // `~uvcpp_handle` → `free_handle()` 会把句柄从 wrapper 上摘下来、内存留给
    // libuv 的关闭回调去还。
    if (conn != nullptr) {
      delete conn;
      conn = nullptr;
    }
    if (udp != nullptr) {
      delete udp;
      udp = nullptr;
    }

    if (fire_cb) {
      // 走到这里说明连接终结了，而 `cb` 还没跑过 —— 也就是连握手都没完成。
      // 用 `UV_ECONNABORTED`（"连接被中断"）而不是一个 ngtcp2 的码：这一层
      // 一个 ngtcp2 类型都不该漏出去，而这个码对调用方要说的事是准确的。
      fire_connect(static_cast<int>(UV_ECONNABORTED));
    } else {
      connect_cb       = nullptr;
      connect_cb_fired = true;
    }
  }

  void fire_connect(int status) {
    if (connect_cb_fired) return;
    connect_cb_fired = true;
    // 先把闭包搬出来再调：`cb` 里 delete 掉本对象是合法用法，搬过之后这一句
    // 之后的代码就不再碰 `this`。
    std::function<void(int)> cb = std::move(connect_cb);
    connect_cb = nullptr;
    if (cb) cb(status);
  }

  /// 按内核给出的下一个到期时刻装定时器。`UINT64_MAX` = 没有到期，停掉。
  void rearm_timer() {
    if (timer == nullptr) return;
    quic_detail::quic_session* s = sess();
    if (s == nullptr) {
      timer->stop();
      return;
    }
    const uint64_t expiry = s->next_expiry();
    if (expiry == UINT64_MAX) {
      timer->stop();
      return;
    }
    const uint64_t now = uv_hrtime();
    uint64_t delay_ms = (expiry > now) ? ((expiry - now) / kNanosPerMs) : 0;
    // **至少 1ms**：ngtcp2 的到期时刻可能落在"现在"或"刚刚"，那时 0 会让
    // 定时器在每一轮循环里都立刻再响一次，把一个 PTO 变成一场忙转。
    if (delay_ms == 0) delay_ms = 1;

    timer->start([this](uvcpp_timer*) { on_timer(); }, delay_ms, 0);
  }

  void on_timer() {
    quic_detail::quic_session* s = sess();
    if (s == nullptr) return;
    s->on_expiry(uv_hrtime());
    // `on_expiry` 里可能报了终结（空闲超时那条路）—— 那时 `next_expiry()`
    // 会给出 `UINT64_MAX`，下面这句自然把定时器停掉。
    rearm_timer();
    if (pending_teardown) teardown(true);
  }

  void on_recv(ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr,
               unsigned int flags) {
    (void)flags;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(
        (buf != nullptr) ? buf->base : nullptr);
    const size_t len = (nread > 0) ? static_cast<size_t>(nread) : 0;

    if (nread < 0) {
      // socket 层的读错误（ICMP 反馈之类）。**不当成连接错误** —— QUIC 的重传
      // 就是为丢包准备的，一次 UDP 读失败不说明这条连接出了事。真正该收场时，
      // 内核会靠超时报出来。
      release_buf(buf);
      return;
    }
    if (len == 0) {
      release_buf(buf);
      return;
    }

    quic_detail::quic_session* s = sess();
    if (s != nullptr && !s->is_closed()) {
      // 传 `addr` 而不是让内核用建连接时的那个：收到的包可能来自别的路径，
      // 而"这算不算合法路径"该由内核判（它会丢掉不可用的路径）。本层不替
      // 它猜。
      s->read_pkt(data, len, addr, sockaddr_len_of(addr));
    }
    release_buf(buf);

    if (pending_teardown) {
      teardown(true);
      return;
    }
    // 收到包之后到期时刻几乎一定会变（ACK 计时器）—— 重装。
    rearm_timer();
    fire_connect_if_established();
  }

  /// 握手完成 → 跑 `connect_cb(0)`。**只在这一处报成功**。
  void fire_connect_if_established() {
    if (connect_cb_fired) return;
    quic_detail::quic_session* s = sess();
    if (s == nullptr) return;
    if (s->state() != quic_connection_state::ESTABLISHED) return;
    fire_connect(0);
  }

  static void release_buf(const uv_buf_t* buf) {
    if (buf != nullptr && buf->base != nullptr) {
      uvcpp_free_bytes(buf->base);
    }
  }

  /// 收集 ALPN 候选：没设过就是默认那条。
  std::vector<std::string> effective_alpn() const {
    if (!alpn_protos.empty()) return alpn_protos;
    return std::vector<std::string>(1, std::string(quic_default_alpn()));
  }

  /// `connect()` 里失败之后的那半：把已经建了一半的东西收掉。
  /// **不碰 `connect_cb`** —— 调用方（`connect()`）自己决定要不要清。
  void abort_start() {
    if (conn != nullptr) {
      delete conn;
      conn = nullptr;
    }
    if (timer != nullptr) {
      delete timer;
      timer = nullptr;
    }
    if (udp != nullptr) {
      delete udp;
      udp = nullptr;
    }
  }

  /// `connect()` 的正文。拆出来是为了用**提前返回**而不是 `goto` ——
  /// `goto` 跨过带初始化的声明在 C++ 里是编译错误（本文件里那几处正好都有）。
  int start(const char* host, int port) {
    int rc = resolve(host, port, &remote_sa, &remote_len);
    if (rc != 0) return rc;

    udp = new uvcpp_udp(loop);
    if (udp == nullptr) return UV_ENOMEM;

    // 本端：与对端同族的通配地址 + 端口 0（内核挑）。**不能跨族**：一条
    // `AF_INET` 的 UDP 口发不到一个 IPv6 对端去，libuv 会在 send 时报
    // `UV_EAFNOSUPPORT`，而那时连接已经"建"起来了 —— 报在 `connect()` 里更好查。
    sockaddr_storage wild{};
    int wild_len = 0;
    make_wildcard(remote_sa.ss_family, &wild, &wild_len);

    rc = udp->bind(reinterpret_cast<const struct sockaddr*>(&wild), 0);
    if (rc != 0) return rc;

    {
      sockaddr_storage got{};
      int got_len = static_cast<int>(sizeof(got));
      rc = udp->getsockname(reinterpret_cast<struct sockaddr*>(&got), &got_len);
      if (rc != 0) return rc;
      std::memcpy(&local_sa, &got, static_cast<size_t>(got_len));
      local_len = got_len;
    }

    // 连接对象先建出来，再建内核 —— 因为内核的事件槽要发给它。顺序反过来的话，
    // 握手前几步（也就是 `init_client` 之后那第一次 `flush()`）出的事件会打在
    // 一个还没有接收者的槽上，直接丢掉。
    conn = new uvcpp_quic_connection();
    if (conn == nullptr) return UV_ENOMEM;

    quic_detail::quic_session* s = new quic_detail::quic_session();
    // `endpoint::attach` 是**静态**方法（见 `uvcpp_quic_connection.h` 那条：
    // 友元不传递，端点的接线代码住在 `impl` 里，够不着私有成员）。
    uvcpp_quic_connection::endpoint::attach(*conn, s,
                                            [this](uvcpp_quic_connection&, int) {
      // **不能在这里 delete `conn`** —— 这一跳是从内核的回调栈里出来的，
      // 而栈上还压着 `read_pkt` / `on_expiry`，它们回来以后还要用那个内核。
      pending_teardown = true;
    });

    rc = s->init_client(
        ssl_ctx, effective_alpn(),
        reinterpret_cast<const struct sockaddr*>(&local_sa),
        reinterpret_cast<const struct sockaddr*>(&remote_sa), host,
        idle_timeout_ms,
        [this](const uint8_t* data, size_t len, const struct sockaddr* peer,
               int peerlen) { forward(data, len, peer, peerlen); });
    if (rc != 0) return rc;

    timer = new uvcpp_timer(loop);
    if (timer == nullptr) return UV_ENOMEM;

    rc = udp->recv_start(
        [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
          // 一次数据报一块缓冲。**不给它 `sz`**（libuv 的建议值通常是 65536）
          // —— 那是一次 64KiB 的分配换一个最多 `kDatagramBufLen` 字节的包。
          (void)sz;
          uvcpp_buf::alloc_buf(buf, kRecvBufLen);
        },
        [this](uvcpp_udp*, ssize_t nread, const uv_buf_t* buf,
               const struct sockaddr* addr, unsigned int flags) {
          on_recv(nread, buf, addr, flags);
        });
    if (rc != 0) return rc;

    // 第一个 Initial 包。**走内核自己的 `flush()`**，与后面每一次发包同一条路
    // —— 单独给"首包"写一条路，等于让两条路各自对一次 ngtcp2 的前置条件。
    s->flush();
    rearm_timer();
    return 0;
  }
};

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_quic_client::uvcpp_quic_client() : impl_(new impl()) {
  // 与 `uvcpp_tcp_client()` 同一形状：自建那条路走 `new uvcpp_loop()`，
  // 而 `uvcpp_loop` 的构造函数内部已经调过 `init()`。
  impl_->loop      = new uvcpp_loop();
  impl_->owns_loop = true;
}

uvcpp_quic_client::uvcpp_quic_client(uvcpp_loop* external_loop)
    : impl_(new impl()) {
  impl_->owns_loop = false;
  impl_->loop      = external_loop;
  // `external_loop == nullptr` 不在这里拦：与 `uvcpp_tcp_client(uvcpp_loop*)`
  // 一致 —— 那是个明确的调用方错误，会在 `get_loop()` 的返回值上立刻现形，
  // 而不是被这里悄悄换成一个自建循环（那会让"共享"这条路的语义反过来）。
}

uvcpp_quic_client::~uvcpp_quic_client() {
  // 先把挂在循环上的东西收掉（连接 → 定时器 → socket）。`uvcpp_handle` 那套
  // 会处理"还活着就被 delete"的情形：它把自己从 wrapper 上摘下来（换成哨兵），
  // 内存留给 libuv 的关闭回调去还 —— 见 `uvcpp_handle::free_handle()` 里那三个
  // 哨兵分支。所以这里 `delete` 是安全的，代价是循环上多一个待关闭的句柄，
  // 也就是下面那个泵要解决的。
  impl_->teardown(false);

  if (impl_->loop != nullptr && impl_->owns_loop) {
    // **必须泵**：上面那几个句柄的 `uv_close` 还没跑完，而 `uv_loop_close()`
    // 在队列不干净时返回 `UV_EBUSY`；`~uvcpp_loop` 的既有策略是"泄漏而不释放"
    // （见那边注释），于是漏掉一次泵的后果是被记下来的一条泄漏，不是一个
    // 野指针。有界（256 轮）是因为这个析构不能变成"等对端回话"。
    for (int i = 0; i < 256 && impl_->loop->loop_alive() != 0; ++i) {
      impl_->loop->run(UV_RUN_NOWAIT);
    }
    impl_->loop->loop_close();
    delete impl_->loop;
    impl_->loop = nullptr;
  }
  // 共享那条路上 `delete` 的是**调用方的**循环 —— 绝不能碰。`owns_loop`
  // 就是这条界线，且它只在上面这一处被读。
}

// =========================================================================
// 访问器
// =========================================================================

uvcpp_loop* uvcpp_quic_client::get_loop() { return impl_->loop; }

// =========================================================================
// 配置 —— 真实现（只存值）
// =========================================================================

void uvcpp_quic_client::set_ssl_context(uvcpp_ssl_context* ctx) {
  impl_->ssl_ctx = ctx;
}

void uvcpp_quic_client::set_alpn_protos(const std::vector<std::string>& protos) {
  impl_->alpn_protos = protos;
}

void uvcpp_quic_client::set_idle_timeout(uint64_t ms) {
  impl_->idle_timeout_ms = ms;
}

// =========================================================================
// 传输
// =========================================================================

int uvcpp_quic_client::connect(const char* host, int port,
                               std::function<void(int)> cb) {
  // **一切失败都在返回负值的那几条路上解决**，返回 0 之后就只剩"等回调"。
  if (impl_->loop == nullptr) return UV_EINVAL;
  if (impl_->ssl_ctx == nullptr) return UV_EINVAL;
  if (port < 0 || port > 65535) return UV_EINVAL;
  if (host == nullptr) return UV_EINVAL;
  if (impl_->conn != nullptr) return UV_EALREADY;

  impl_->connect_cb       = std::move(cb);
  impl_->connect_cb_fired = false;

  const int rc = impl_->start(host, port);
  if (rc != 0) {
    impl_->abort_start();
    // **`cb` 不跑** —— `connect()` 返回负值就是"没受理"，此时再回调一次会让
    // 调用方在自己的错误分支里被判一次成功（或者更糟：在 `cb` 里把还没建好的
    // 东西当成建好了用）。这条契约由 `quic_api_func.cpp` 钉着。
    impl_->connect_cb       = nullptr;
    impl_->connect_cb_fired = true;
    return rc;
  }
  return 0;
}

uvcpp_quic_connection* uvcpp_quic_client::connection() { return impl_->conn; }

int uvcpp_quic_client::close() {
  if (impl_->conn == nullptr) return UV_ENOTCONN;
  quic_detail::quic_session* s = impl_->sess();
  if (s == nullptr) return UV_ENOTCONN;
  if (s->is_closed()) return 0;
  // 优雅：CONNECTION_CLOSE 发出去，状态到 CLOSING；对端确认（或到期）之后
  // 内核报终结，那时才收尾。所以调完这里循环还得转 —— 头里那条 `@warning`
  // 说的就是这件事。
  s->close(0);
  impl_->rearm_timer();
  return 0;
}

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE
