/**
 * @file src/quic/uvcpp_quic_server.cpp
 * @brief 服务端端点：一条 UDP 口、一张「CID → 连接」的表、一个到期定时器。
 * @author zhuweiye
 * @version 1.5.1
 *
 * 与 `uvcpp_quic_client.cpp` 的三件事同构（发 / 收 / 到期），只多一层**分派**：
 * 一条口上跑着很多条连接，所以每个入包要先按 DCID 找到"这是谁的"。找表的键由
 * 内核的 `on_new_cid` / `on_remove_cid` 事件送来，本文件只管维护那张表。
 *
 * **本文件里没有一处 `ngtcp2` 类型。** 从包里认 DCID、判断"这个 Initial 该不该
 * 建新连接"，都由内核以 `std::string` 的形状提供（`peek_dcid` / `accept_new`）——
 * 服务端不需要认识 QUIC 的包头长什么样。
 *
 * **本层不做两件事**（都不是漏了，见 `doc/quic-guide.md` §8）：
 *
 * 1. **不发 Version Negotiation 包。** 对端说的版本我们听不懂时，`ngtcp2_accept`
 *    会拒掉、包被丢掉，而不是回一个带支持版本的协商包。
 * 2. **不用 `IP_PKTINFO` 指定回复的源地址。** ngtcp2 自己的例子服务端用
 *    `sendmsg` 的控制消息把源地址钉到"客户端发来的那个目的地址"上；而
 *    `uvcpp_udp::send` 走的是 `uv_udp_send`，没有控制消息那条路。绑定具体地址
 *    （`bind("127.0.0.1", …)`）时两者等价；绑定通配地址时回复的源地址由内核按
 *    路由挑，多网卡的机器上可能是另一个地址。要跑通配且多网卡，这条得补。
 */

#include "quic/uvcpp_quic_server.h"

#if UVCPP_QUIC_ENABLE

#include <uv.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <handle/uvcpp_timer.h>
#include <handle/uvcpp_udp.h>
#include <req/uvcpp_udp_send.h>
#include <ssl/uvcpp_ssl_context.h>
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_buf.h>

#include "quic/uvcpp_quic_session.h"

namespace uvcpp {

namespace {

/// 地址字面量的最大长度：`INET6_ADDRSTRLEN` 是 46，IPv6 带 `%scope` 还可以更长。
/// 取 128 是余量，不是精度 —— 这里只是一块给 `uv_inet_pton` / `uv_ip4_addr`
/// 落结果的暂存区。
const size_t kAddrLen = 128;

/// 收到一个数据报时给 libuv 的接收缓冲大小。同 `uvcpp_quic_client.cpp`：要
/// ≥ 我们对外的 `max_udp_payload_size`（1500），给小了是静默截断。
const size_t kRecvBufLen = 4096;

/// 一毫秒的纳秒数。`uv_hrtime()` 与 ngtcp2 的 `timestamp()` 都是纳秒。
const uint64_t kNanosPerMs = 1000000ULL;

/**
 * @brief 把一个字面量按指定地址族校验一遍。
 * @return 字面量属于 \p family 返回 true。
 *
 * 只做校验，结果丢掉 —— `bind()` 只登记，建 `sockaddr` 是 `listen()` 的事。
 * 写成函数而不是在三个 `bind*` 里各写一遍，是为了让"三个入口用的是同一把尺子"
 * 这件事一眼可查。
 */
bool literal_fits(int family, const char* ip) {
  if (ip == nullptr) return false;
  char buf[kAddrLen];
  return uv_inet_pton(family, ip, buf) == 0;
}

/// 按 `sockaddr` 的族给出长度（内核的签名里地址没有长度，而 ngtcp2 要一个）。
int sockaddr_len_of(const struct sockaddr* sa) {
  if (sa == nullptr) return 0;
  if (sa->sa_family == AF_INET) return static_cast<int>(sizeof(sockaddr_in));
  if (sa->sa_family == AF_INET6) return static_cast<int>(sizeof(sockaddr_in6));
  return 0;
}

/// 用登记的字面量填一个 `sockaddr`。先用 v4 试、再用 v6 —— 与 `bind()` 里那条
/// "自动判断族"同一个顺序，所以这里不可能填出与 `bind()` 判断不符的族。
int fill_addr(const std::string& ip, int port, sockaddr_storage* out, int* outlen) {
  sockaddr_in a4;
  if (uv_ip4_addr(ip.c_str(), port, &a4) == 0) {
    std::memcpy(out, &a4, sizeof(a4));
    *outlen = sizeof(a4);
    return 0;
  }
  sockaddr_in6 a6;
  if (uv_ip6_addr(ip.c_str(), port, &a6) == 0) {
    std::memcpy(out, &a6, sizeof(a6));
    *outlen = sizeof(a6);
    return 0;
  }
  return UV_EINVAL;
}

}  // namespace

struct uvcpp_quic_server::impl {
  uvcpp_loop* loop      = nullptr;
  bool        owns_loop = true;

  uvcpp_ssl_context* ssl_ctx = nullptr;
  /// 空 = 用 `quic_default_alpn()`。理由见 `uvcpp_quic_client.cpp` 里同名成员。
  std::vector<std::string> alpn_protos;

  /// 登记下来的地址（`bind()` 写的）。**不是**"已绑定"—— 绑定发生在 `listen()`。
  std::string ip;
  int         port = 0;

  uint64_t idle_timeout_ms = 30000;

  uvcpp_udp*   udp   = nullptr;
  uvcpp_timer* timer = nullptr;
  bool         listening = false;

  /// 这一个监听 socket 上 `UDP_SEND_MSG_SIZE` 当前是多少。0 = 没设过。
  ///
  /// 这里**只有一份**，因为服务端所有连接共用同一个 socket —— 也正是
  /// 缓存必须放在端点而不能放在会话上的原因（见 `uvcpp_quic_client.cpp`
  /// 里同名成员的说明）。
  int gso_seg = 0;

  std::function<void(uvcpp_quic_connection*)> connection_cb;

  /// 本端绑定地址的快照（`listen()` 那一刻取的）。每条连接的 `ngtcp2_path` 的
  /// local 那一半都来自它 —— QUIC 的路径是 (local, remote) 对，而远端那一半每个
  /// 连接不同。
  sockaddr_storage local_sa{};
  int              local_len = 0;

  /// 本端 SCID 的长度。**短包头的 DCID 长度不写在包上**（RFC 9000 §17.3.1），
  /// 解它必须先知道这个值。第一次从 `on_new_cid` 学到（本端所有 SCID 等长）。
  size_t cid_len = 0;

  /// 路由表：CID 字节串 → 连接。一条连接有多个键（本端每个 SCID + 对端首包的
  /// 原始 DCID）。**表里的指针不拥有连接** —— 所有权在 `owned` 那张。
  std::map<std::string, uvcpp_quic_connection*> by_cid;

  /// 所有权：连接对象全部在这里，键就是裸指针（表里那张不方便按对象删）。
  std::map<uvcpp_quic_connection*, std::unique_ptr<uvcpp_quic_connection>> owned;

  /// 已经报过终结、等着销毁的连接。**不能当场删** —— 那一跳是从内核的回调栈里
  /// 出来的，栈上还压着 `read_pkt` / `on_expiry`。攒到最外层再收。
  std::vector<uvcpp_quic_connection*> doomed;

  ~impl() {
    // 连接先走：它们的析构会拆内核（也就会跑我们填给内核的那些 lambda，而那些
    // lambda 捕着 `this` 的成员）。所以顺序是"连接全没了"再让 `by_cid` 自己析构。
    // 收 UDP 之前先把连接收掉，也是同一条：这一步之后不会再有 `on_recv`。
    owned.clear();
    by_cid.clear();
  }

  // ---------------------------------------------------------------
  // 工具
  // ---------------------------------------------------------------

  std::vector<std::string> effective_alpn() const {
    if (!alpn_protos.empty()) return alpn_protos;
    return std::vector<std::string>(1, std::string(quic_default_alpn()));
  }

  static void release_buf(const uv_buf_t* buf) {
    if (buf != nullptr && buf->base != nullptr) {
      uvcpp_free_bytes(buf->base);
    }
  }

  /// 把内核要发的一个数据报交给 UDP 口。与客户端那份逐字同理（含"为什么快路
  /// 可以不拷"与"发送结果不报给内核"两条理由），见 `uvcpp_quic_client.cpp`。
  void forward(const uint8_t* data, size_t len, const struct sockaddr* peer,
               int peerlen) {
    (void)peerlen;
    if (udp == nullptr || data == nullptr || len == 0) return;

    uv_buf_t bufs[1];
    bufs[0] = uv_buf_init(const_cast<char*>(reinterpret_cast<const char*>(data)),
                          static_cast<unsigned int>(len));

    // 快路：同步发、零拷贝、无完成回调。逐字同 `uvcpp_quic_client.cpp` 那份
    // （含"为什么能安全地不拷"与"为什么只有成功才走快路"两条理由），见那边的注释。
    const int sent = udp->try_send(bufs, 1, peer);
    if (sent >= 0) return;

    // 慢路：拷一份交给异步队列。
    char* copy = new char[len];
    std::memcpy(copy, data, len);

    uv_buf_t abufs[1];
    abufs[0] = uv_buf_init(copy, static_cast<unsigned int>(len));

    uvcpp_udp_send* req = new uvcpp_udp_send();
    req->init();
    udp->send(req, abufs, 1, peer, [copy, req](uvcpp_udp_send* r, int status) {
      (void)status;
      delete[] copy;
      delete r;
    });
  }

  /// 按内核给出的下一个到期时刻装定时器。**取所有连接里最早的那个** —— 一条口
  /// 上一个定时器，不然定时器数会随连接数长。`UINT64_MAX` = 全都没有到期。
  void rearm_timer() {
    if (timer == nullptr) return;

    uint64_t earliest = UINT64_MAX;
    for (auto it = owned.begin(); it != owned.end(); ++it) {
      quic_detail::quic_session* s =
          uvcpp_quic_connection::endpoint::session(*it->first);
      if (s == nullptr) continue;
      const uint64_t e = s->next_expiry();
      if (e < earliest) earliest = e;
    }
    if (earliest == UINT64_MAX) {
      timer->stop();
      return;
    }

    const uint64_t now = uv_hrtime();
    uint64_t delay_ms = (earliest > now) ? ((earliest - now) / kNanosPerMs) : 0;
    // 至少 1ms：到期时刻可能落在"现在"或"刚刚"，0 会让定时器在每一轮循环里都
    // 立刻再响一次，把一个 PTO 变成一场忙转。同 `uvcpp_quic_client.cpp`。
    if (delay_ms == 0) delay_ms = 1;
    timer->start([this](uvcpp_timer*) { on_timer(); }, delay_ms, 0);
  }

  void on_timer() {
    const uint64_t now = uv_hrtime();

    // 先取一份快照：`on_expiry` 会往 `doomed` 里塞连接，但**不会**当场删 ——
    // 所以这里遍历 `owned` 是安全的。到期还没到的连接就跳过（`handle_expiry`
    // 的契约是"到期了才调"）。
    for (auto it = owned.begin(); it != owned.end(); ++it) {
      quic_detail::quic_session* s =
          uvcpp_quic_connection::endpoint::session(*it->first);
      if (s == nullptr) continue;
      if (s->next_expiry() > now) continue;
      s->on_expiry(now);
    }

    reap();
    rearm_timer();
  }

  /// 收掉已经报过终结的连接：先摘它的所有路由键，再让它析构。
  void reap() {
    if (doomed.empty()) return;
    std::vector<uvcpp_quic_connection*> d;
    d.swap(doomed);

    for (size_t i = 0; i < d.size(); ++i) {
      uvcpp_quic_connection* c = d[i];
      // 摘键：`by_cid` 是 map，得整张扫一遍（一个连接有多个键）。连接数不大时
      // 这就是最优解 —— 换成"连接 → 键集合"的反向表只是把同一份工作量换了个
      // 地方放，还多一个要同步的东西。
      for (auto it = by_cid.begin(); it != by_cid.end();) {
        if (it->second == c) {
          it = by_cid.erase(it);
        } else {
          ++it;
        }
      }
      owned.erase(c);  // unique_ptr：真删
    }
  }

  // ---------------------------------------------------------------
  // 收包与分派
  // ---------------------------------------------------------------

  void on_recv(ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr,
               unsigned int flags) {
    (void)flags;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(
        (buf != nullptr) ? buf->base : nullptr);
    const size_t len = (nread > 0) ? static_cast<size_t>(nread) : 0;
    const int    addrlen = sockaddr_len_of(addr);

    if (nread <= 0 || len == 0) {
      // socket 层的读错误（同客户端：**不当成连接错误** —— UDP 读失败对 QUIC
      // 就是丢包，该收场时靠超时报出来）。
      release_buf(buf);
      return;
    }

    bool routed = false;

    // ① 已存在的连接？DCID 就是路由键。
    std::string dcid;
    if (quic_detail::quic_session::peek_dcid(data, len, cid_len, &dcid)) {
      std::map<std::string, uvcpp_quic_connection*>::iterator it =
          by_cid.find(dcid);
      if (it != by_cid.end()) {
        quic_detail::quic_session* s =
            uvcpp_quic_connection::endpoint::session(*it->second);
        if (s != nullptr) s->read_pkt(data, len, addr, addrlen);
        routed = true;
      }
    }

    // ② 不是？那可能是一个该建新连接的 Initial。
    if (!routed) {
      std::string client_scid;
      std::string original_dcid;
      if (quic_detail::quic_session::accept_new(data, len, &client_scid,
                                                &original_dcid)) {
        uvcpp_quic_connection* c = create_conn(client_scid, original_dcid, addr);
        if (c != nullptr) {
          // **先把连接交给使用者装回调，再喂首包。** 反过来的话，握手期间那些
          // 事件（`on_alpn`、甚至 `on_read` —— 0.5-RTT 时服务端就能发数据）会
          // 打在还没装回调的槽上，直接丢掉。这条与客户端那条
          // "`connect()` 返回之后、下一次循环迭代之前装回调"是同一条理由的
          // 两个方向。
          if (connection_cb != nullptr) connection_cb(c);
          quic_detail::quic_session* s =
              uvcpp_quic_connection::endpoint::session(*c);
          if (s != nullptr) s->read_pkt(data, len, addr, addrlen);
        }
      }
      // 两条都没命中：丢掉。**不报错、不计数** —— 一条口上收到陌生 CID 是常态
      // （端口扫描、过期的重传、别的东西打错端口），不是本层该处理的事件。
    }

    release_buf(buf);
    reap();
    rearm_timer();
  }

  /// 建一条服务端连接。失败返回 `nullptr`（对象自己收干净了）。
  ///
  /// `peer` 的长度不在这里传：内核的 `init_server` 自己按地址的族算（它要的
  /// 是 `const struct sockaddr*`，长度是它内部的事）。
  uvcpp_quic_connection* create_conn(const std::string& client_scid,
                                     const std::string& original_dcid,
                                     const struct sockaddr* peer) {
    if (cid_len == 0) cid_len = client_scid.size();

    std::unique_ptr<uvcpp_quic_connection> holder(new uvcpp_quic_connection());
    uvcpp_quic_connection* raw = holder.get();

    quic_detail::quic_session* s = new quic_detail::quic_session();

    // 路由钩子。**捕 `raw` 而不是事后去查表**：内核报 CID 的时候连接对象已经
    // 在手里了，而"反查"要拿连接指针去扫 `by_cid`，那正是 `reap()` 在做的事。
    uvcpp_quic_connection::endpoint::hooks hk;
    hk.on_new_cid = [this, raw](uvcpp_quic_connection&,
                                const std::string& cid) { by_cid[cid] = raw; };
    hk.on_remove_cid = [this](uvcpp_quic_connection&, const std::string& cid) {
      by_cid.erase(cid);
    };

    uvcpp_quic_connection::endpoint::attach(
        *raw, s,
        [this, raw](uvcpp_quic_connection&, int) {
          doomed.push_back(raw);
        },
        hk);

    ngtcp2_cid scid;
    ngtcp2_cid odcid;
    scid.datalen = client_scid.size();
    std::memcpy(scid.data, client_scid.data(), client_scid.size());
    odcid.datalen = original_dcid.size();
    std::memcpy(odcid.data, original_dcid.data(), original_dcid.size());

    const int rc = s->init_server(
        ssl_ctx, effective_alpn(),
        reinterpret_cast<const struct sockaddr*>(&local_sa), peer,
        idle_timeout_ms,
        [this](const uint8_t* data, size_t len, const struct sockaddr* p,
               int plen) { forward(data, len, p, plen); },
        [this](int seg) -> bool {
          if (seg == gso_seg) return true;  // 没变就别去撞系统调用
          if (!quic_detail::set_udp_gso_seg(udp, seg)) return false;
          gso_seg = seg;
          return true;
        },
        scid, odcid);
    if (rc != 0) {
      // `attach` 已经把内核的所有权接过来了，`holder` 一析构就连它一起收
      // （连接层的 `~impl` 里 delete 内核）。所以这里**不要**再 delete `s`。
      return nullptr;
    }

    // 所有权进 `owned`。**放在 `init_server` 之后**：失败那条路上 `holder`
    // 自己要负责把半成品收干净，进了表反而要再摘一次。
    owned[raw] = std::move(holder);
    return raw;
  }

  // ---------------------------------------------------------------
  // 起监听
  // ---------------------------------------------------------------

  int start_listen() {
    if (listening) return UV_EALREADY;

    const std::string bind_ip = ip.empty() ? std::string("0.0.0.0") : ip;
    sockaddr_storage sa{};
    int salen = 0;
    int rc = fill_addr(bind_ip, port, &sa, &salen);
    if (rc != 0) return rc;

    udp = new uvcpp_udp(loop);
    if (udp == nullptr) return UV_ENOMEM;
    gso_seg = 0;  // 换了 socket，上一份缓存作废

    rc = udp->bind(reinterpret_cast<const struct sockaddr*>(&sa), 0);
    if (rc != 0) return rc;

    // 取回内核实际给的本端地址。`bind(ip, 0)` 时这里才有真端口；**每条连接的
    // `ngtcp2_path` 的 local 那一半就用它**。
    {
      sockaddr_storage got{};
      int got_len = static_cast<int>(sizeof(got));
      rc = udp->getsockname(reinterpret_cast<struct sockaddr*>(&got), &got_len);
      if (rc != 0) return rc;
      std::memcpy(&local_sa, &got, static_cast<size_t>(got_len));
      local_len = got_len;
      // `configured_port()` 报"内核给我的那个"。ip 不动：登记的是通配地址时，
      // "绑上的具体地址"是"哪个都行"，报出来反而误导。
      if (got.ss_family == AF_INET) {
        port = ntohs(reinterpret_cast<sockaddr_in*>(&got)->sin_port);
      } else if (got.ss_family == AF_INET6) {
        port = ntohs(reinterpret_cast<sockaddr_in6*>(&got)->sin6_port);
      }
    }

    timer = new uvcpp_timer(loop);
    if (timer == nullptr) return UV_ENOMEM;

    rc = udp->recv_start(
        [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
          (void)sz;
          uvcpp_buf::alloc_buf(buf, kRecvBufLen);
        },
        [this](uvcpp_udp*, ssize_t nread, const uv_buf_t* buf,
               const struct sockaddr* addr,
               unsigned int flags) { on_recv(nread, buf, addr, flags); });
    if (rc != 0) return rc;

    listening = true;
    return 0;
  }
};

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_quic_server::uvcpp_quic_server() : impl_(new impl()) {
  impl_->loop      = new uvcpp_loop();
  impl_->owns_loop = true;
}

uvcpp_quic_server::uvcpp_quic_server(uvcpp_loop* external_loop)
    : impl_(new impl()) {
  impl_->owns_loop = false;
  impl_->loop      = external_loop;
}

uvcpp_quic_server::~uvcpp_quic_server() {
  // 顺序：连接（连带内核）→ 定时器 → socket → 循环。连接先走是因为它们的析构
  // 会拆内核，而内核可能还想发包（那时 socket 还活着，包发得出去；反过来 socket
  // 先没了，内核的发包回调就落在一个已经关掉的句柄上）。
  impl_->owned.clear();
  impl_->by_cid.clear();
  impl_->doomed.clear();
  if (impl_->timer != nullptr) {
    delete impl_->timer;
    impl_->timer = nullptr;
  }
  if (impl_->udp != nullptr) {
    delete impl_->udp;
    impl_->udp = nullptr;
  }

  if (impl_->loop != nullptr && impl_->owns_loop) {
    // 与 `~uvcpp_quic_client` 逐字同理：把上面那几个 `uv_close` 泵完再
    // `loop_close()`，否则 `uv_loop_close()` 报 `UV_EBUSY`，而 `~uvcpp_loop`
    // 的策略是"泄漏而不释放"。有界 256 轮 —— 这个析构不能变成"等对端回话"。
    for (int i = 0; i < 256 && impl_->loop->loop_alive() != 0; ++i) {
      impl_->loop->run(UV_RUN_NOWAIT);
    }
    impl_->loop->loop_close();
    delete impl_->loop;
    impl_->loop = nullptr;
  }
}

// =========================================================================
// 访问器
// =========================================================================

uvcpp_loop* uvcpp_quic_server::get_loop() { return impl_->loop; }

std::string uvcpp_quic_server::configured_ip() const { return impl_->ip; }

int uvcpp_quic_server::configured_port() const { return impl_->port; }

// =========================================================================
// Bind —— 校验 + 登记
// =========================================================================

int uvcpp_quic_server::bind(const char* ip, int port) {
  // **先全部校验、再一次性赋值**：中间任何一步失败都不许留下半套状态。
  // 一半"新地址 + 旧端口"是最难查的那种：调用方拿到负的返回码，以为没生效，
  // 而 `configured_*()` 报出来的东西已经变了。
  if (port < 0 || port > 65535) return UV_EINVAL;

  std::string addr = (ip != nullptr) ? std::string(ip) : std::string("0.0.0.0");

  // 自动判断族：先当 IPv4 试，不行再当 IPv6 试。**两边都不行才是错** ——
  // 不能只用"含不含冒号"来分，那会把 `"::ffff:1.2.3.4"` 这类写法和
  // `"1.2.3.4"` 之外的所有畸形输入混进同一个分支。
  if (!literal_fits(AF_INET, addr.c_str()) &&
      !literal_fits(AF_INET6, addr.c_str())) {
    return UV_EINVAL;
  }

  impl_->ip   = addr;
  impl_->port = port;
  return 0;
}

int uvcpp_quic_server::bindIpv4(const char* ip, int port) {
  if (port < 0 || port > 65535) return UV_EINVAL;
  const char* addr = (ip != nullptr) ? ip : "0.0.0.0";
  if (!literal_fits(AF_INET, addr)) return UV_EINVAL;
  impl_->ip   = addr;
  impl_->port = port;
  return 0;
}

int uvcpp_quic_server::bindIpv6(const char* ip, int port) {
  if (port < 0 || port > 65535) return UV_EINVAL;
  const char* addr = (ip != nullptr) ? ip : "::";
  if (!literal_fits(AF_INET6, addr)) return UV_EINVAL;
  impl_->ip   = addr;
  impl_->port = port;
  return 0;
}

// =========================================================================
// 配置 —— 只存值
// =========================================================================

void uvcpp_quic_server::set_ssl_context(uvcpp_ssl_context* ctx) {
  impl_->ssl_ctx = ctx;
}

void uvcpp_quic_server::set_alpn_select_protos(
    const std::vector<std::string>& protos) {
  impl_->alpn_protos = protos;
}

void uvcpp_quic_server::set_idle_timeout(uint64_t ms) {
  impl_->idle_timeout_ms = ms;
}

// =========================================================================
// Listen / Run
// =========================================================================

int uvcpp_quic_server::listen(
    std::function<void(uvcpp_quic_connection*)> connection_cb) {
  if (impl_->loop == nullptr) return UV_EINVAL;
  // 与客户端 `connect()` 那条同源：**没设 TLS 上下文就是没配**。QUIC 没有明文
  // 模式，退化成一个不加密的服务端不是"宽容"，是假装成功。
  if (impl_->ssl_ctx == nullptr) return UV_EINVAL;
  if (impl_->listening) return UV_EALREADY;

  impl_->connection_cb = std::move(connection_cb);

  const int rc = impl_->start_listen();
  if (rc != 0) {
    // 起了一半也要收干净，并且**把回调清掉**：`listen()` 返回负值就是"没收
    // 连接"，此时再回调一次会让调用方在自己的错误分支里被判一次成功。
    impl_->connection_cb = nullptr;
    impl_->listening     = false;
    if (impl_->udp != nullptr) {
      delete impl_->udp;
      impl_->udp = nullptr;
    }
    if (impl_->timer != nullptr) {
      delete impl_->timer;
      impl_->timer = nullptr;
    }
    return rc;
  }
  return 0;
}

int uvcpp_quic_server::run(uv_run_mode md) {
  if (impl_->loop == nullptr) return UV_EINVAL;
  return impl_->loop->run(md);
}

void uvcpp_quic_server::stop() {
  if (impl_->loop != nullptr) impl_->loop->stop();
}

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE
