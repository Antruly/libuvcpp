/**
 * @file src/net/uvcpp_udp_client.cpp
 * @brief Implementation of uvcpp_udp_client.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <net/uvcpp_udp_client.h>
#include <req/uvcpp_udp_send.h>
#include <req/uvcpp_getaddrinfo.h>
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_define.h>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace uvcpp {

// =========================================================================
// Trampolines
// =========================================================================

static void trampoline_send(int status, void* arg) {
  auto* cb = static_cast<std::function<void(int)>*>(arg);
  (*cb)(status);
  delete cb;
}

static void trampoline_recv(uvcpp_buf* buf, const char* ip, int port,
                             void* arg) {
  auto* cb =
      static_cast<std::function<void(uvcpp_buf*, const char*, int)>*>(arg);
  (*cb)(buf, ip, port);
  // cb is NOT deleted — persistent for multiple datagrams
}

// =========================================================================
// Construction / Destruction
// =========================================================================

uvcpp_udp_client::uvcpp_udp_client() {
  loop_ = new uvcpp_loop();
  udp_  = new uvcpp_udp(loop_);
  status_ = UDP_CLIENT_NONE;
}

uvcpp_udp_client::uvcpp_udp_client(uvcpp_loop* external_loop) {
  owns_loop_ = false;
  loop_ = external_loop;
  udp_  = new uvcpp_udp(loop_);
  status_ = UDP_CLIENT_NONE;
}

std::shared_ptr<char> uvcpp_udp_client::alive_token() {
  if (!alive_token_) alive_token_.reset(new char(0));
  return alive_token_;
}

bool uvcpp_udp_client::token_alive(const std::shared_ptr<char>& token) {
  return token && *token == 0;
}

uvcpp_udp_client::~uvcpp_udp_client() {
  // **第一件事就把令牌作废**：见头文件里的说明。此后 libuv 送进来的任何异步
  // 完成/接收回调都直接返回（接收回调还要把已经分配出去的缓冲区还回去）。
  if (alive_token_) *alive_token_ = 1;

  if (recv_started_) {
    udp_->recv_stop();
    recv_started_ = false;
  }

  // Close the UDP handle and pump the loop.
  // Delete the handle AFTER closing the loop to avoid endgame races.
  //
  // 析构里**不等待**：以前这里是 `while (!close_done) { run(NOWAIT); if
  // (elapsed > 5000) break; sleep(1ms); }` 再跟 20 次带 sleep 的迭代 —— 最长 5
  // 秒的墙钟等待，而且是在析构点上跑事件循环。析构点常常就在循环自己的回调里
  // （会话结束时把自己一起回收），那等于让那个回调阻塞，与"不在循环线程上跑耗
  // 时操作、不做密集等待"直接抵触（同 `~uvcpp_ws_client`，cdf785b）。
  //
  // 但泵不能整个去掉：`uv_close` 是延迟的，句柄要到它自己的关闭回调跑过之后才
  // 从 `loop->handle_queue` 上摘下来，而只有循环能放那个回调。所以下面这几次
  // NOWAIT 不是"等待"而是"把已经挂起的关闭回调放掉" —— 它不需要 I/O 也不需要
  // 定时器，一两轮就到。次数有界是为了**绝不卡住**：万一没放完，后续处置与改
  // 之前"等了 5 秒也没等到"是同一条路（底层内存由 `uvcpp_handle::free_handle`
  // 的哨兵机制接手）。
  if (udp_ != nullptr) {
    // **判据只能是 `is_closing()`，不能带上 `is_active()`** —— 与 tcp 侧
    // （`uvcpp_tcp_client.cpp` 里那段同题长注）同一个坑：上面刚调过
    // `recv_stop()`，而它会把「正在读」这个活跃计数减掉，于是"我刚把读停掉、
    // socket 还开着"和"句柄早就关完了"落进同一个 else。后果不是少关一次那么
    // 轻：`delete udp_` 里 `free_handle()` 会走"不活跃也没在关"那条分支，而那
    // 时 loop 已经被删了 —— 它会在**已释放的 loop** 上调 `uv_close`（往
    // `loop->closing_handles` 写），句柄内存与在途请求则永远没人还。
    //
    // 句柄真正关完之后 `_handle` 是 nullptr，`is_closing()` 答 0、`is_active()`
    // 也答 0，走 else 里的一次 NOWAIT 收尾 —— 在"还活着"的前提下，判据带上
    // `is_active()` 提供不了任何额外信息。
    if (!udp_->is_closing()) {
      if (owns_loop_) {
        bool close_done = false;
        udp_->close([&close_done](uvcpp_handle*) { close_done = true; });

        for (int i = 0; i < 64 && !close_done; ++i) {
          if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
        }
      } else {
        // 外部循环：**不能**在这里泵它（它可能正跑在别的对象的事件里，
        // 甚至正跑在它自己的回调栈上）。关闭照常入队，收尾由循环的主人做。
        udp_->close([](uvcpp_handle*) {});
      }
    } else {
      if (owns_loop_ && loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
    }
  }

  // Close loop first, then delete the handle.
  //
  // 关之前按循环自己的判据再泵一遍：上面那几步只保证 **udp 句柄**收尾了，
  // 循环上可能还挂着别人（`async` / `timer` 之类只关了一半的）。判据是
  // `loop_alive()` 而不是 `loop_close()` —— 后者内部会 `stop()`，而 `uv_run`
  // 的 `while (r != 0 && loop->stop_flag == 0)` 是**进 body 之前**判的，
  // 停标志一立那一轮就整段空转，交替调用等于原地打转。
  // 外部循环这条路上这一段**整段跳过**：循环不是我们的，关它等于把同一个循环
  // 上别的对象的句柄连同它们的事件一起废掉；`delete loop_` 更是直接释放别人的
  // 内存。句柄该关的上面已经关过了，剩下的收尾归循环的主人。
  if (loop_ != nullptr && owns_loop_) {
    for (int i = 0; i < 256 && loop_->loop_alive() != 0; ++i) {
      loop_->run(UV_RUN_NOWAIT);
    }
    loop_->loop_close();
    delete loop_;
    loop_ = nullptr;
  }

  if (udp_ != nullptr) {
    delete udp_;
    udp_ = nullptr;
  }

  // Free read cache
  if (read_cache_ != nullptr) {
    delete read_cache_;
    read_cache_ = nullptr;
  }

  // Free pending async callback data
  if (send_arg_ != nullptr) {
    delete static_cast<std::function<void(int)>*>(send_arg_);
    send_arg_ = nullptr;
  }
  if (recv_arg_ != nullptr) {
    delete static_cast<std::function<void(uvcpp_buf*, const char*, int)>*>(
        recv_arg_);
    recv_arg_ = nullptr;
  }
}

// =========================================================================
// Accessors
// =========================================================================

uvcpp_udp* uvcpp_udp_client::get_udp() { return udp_; }
uvcpp_loop* uvcpp_udp_client::get_loop() { return loop_; }
int uvcpp_udp_client::get_status() const { return status_; }
bool uvcpp_udp_client::has_status(int flags) const {
  return (status_ & flags) == flags;
}
int uvcpp_udp_client::get_last_error() const { return last_error_code_; }

// =========================================================================
// Address helpers
// =========================================================================

sockaddr_storage uvcpp_udp_client::getLocalAddrs(std::string& ip, int& port) {
  struct sockaddr_storage local_addr;
  std::memset(&local_addr, 0, sizeof(local_addr));
  int local_addr_len = sizeof(local_addr);

  if (udp_ == nullptr) return local_addr;

  int rc = udp_->getsockname(
      reinterpret_cast<struct sockaddr*>(&local_addr), &local_addr_len);
  if (rc != 0) { last_error_code_ = rc; return local_addr; }

  char local_ip[INET6_ADDRSTRLEN] = {0};
  if (local_addr.ss_family == AF_INET) {
    uv_ip4_name(reinterpret_cast<struct sockaddr_in*>(&local_addr),
                local_ip, sizeof(local_ip));
    ip = local_ip;
    port = ntohs(reinterpret_cast<struct sockaddr_in*>(&local_addr)->sin_port);
  } else if (local_addr.ss_family == AF_INET6) {
    uv_ip6_name(reinterpret_cast<struct sockaddr_in6*>(&local_addr),
                local_ip, sizeof(local_ip));
    ip = local_ip;
    port = ntohs(
        reinterpret_cast<struct sockaddr_in6*>(&local_addr)->sin6_port);
  }
  return local_addr;
}

sockaddr_storage uvcpp_udp_client::getPeerAddrs(std::string& ip, int& port) {
  struct sockaddr_storage peer_addr;
  std::memset(&peer_addr, 0, sizeof(peer_addr));
  int peer_addr_len = sizeof(peer_addr);

  if (udp_ == nullptr) return peer_addr;

  int rc = udp_->getpeername(
      reinterpret_cast<struct sockaddr*>(&peer_addr), &peer_addr_len);
  if (rc != 0) { last_error_code_ = rc; return peer_addr; }

  char peer_ip[INET6_ADDRSTRLEN] = {0};
  if (peer_addr.ss_family == AF_INET) {
    uv_ip4_name(reinterpret_cast<struct sockaddr_in*>(&peer_addr),
                peer_ip, sizeof(peer_ip));
    ip = peer_ip;
    port = ntohs(reinterpret_cast<struct sockaddr_in*>(&peer_addr)->sin_port);
  } else if (peer_addr.ss_family == AF_INET6) {
    uv_ip6_name(reinterpret_cast<struct sockaddr_in6*>(&peer_addr),
                peer_ip, sizeof(peer_ip));
    ip = peer_ip;
    port = ntohs(
        reinterpret_cast<struct sockaddr_in6*>(&peer_addr)->sin6_port);
  }
  return peer_addr;
}

// =========================================================================
// Bind / Connect
// =========================================================================

bool uvcpp_udp_client::resolve_addr(const char* ip, int port,
                                     struct sockaddr_in& addr) {
  return uv_ip4_addr(ip, port, &addr) == 0;
}

int uvcpp_udp_client::bind(const char* ip, int port) {
  if (std::strchr(ip, ':') != nullptr) return bindIpv6(ip, port);
  return bindIpv4(ip, port);
}

int uvcpp_udp_client::bindIpv4(const char* ip, int port) {
  struct sockaddr_in addr;
  int rc = uv_ip4_addr(ip, port, &addr);
  if (rc != 0) {
    last_error_code_ = rc;
    set_status(UDP_CLIENT_ERROR);
    return rc;
  }
  rc = udp_->bind(reinterpret_cast<const struct sockaddr*>(&addr), 0);
  if (rc != 0) {
    last_error_code_ = rc;
    set_status(UDP_CLIENT_ERROR);
    return rc;
  }
  set_status(UDP_CLIENT_BOUND);
  return 0;
}

int uvcpp_udp_client::bindIpv6(const char* ip, int port) {
  struct sockaddr_in6 addr;
  int rc = uv_ip6_addr(ip, port, &addr);
  if (rc != 0) {
    last_error_code_ = rc;
    set_status(UDP_CLIENT_ERROR);
    return rc;
  }
  rc = udp_->bind(reinterpret_cast<const struct sockaddr*>(&addr), 0);
  if (rc != 0) {
    last_error_code_ = rc;
    set_status(UDP_CLIENT_ERROR);
    return rc;
  }
  set_status(UDP_CLIENT_BOUND);
  return 0;
}

int uvcpp_udp_client::resolve_send_addrs(const char* host, int port,
                                         std::vector<sockaddr_storage>& out,
                                         int timeout_ms) {
  out.clear();

  // 数字 IPv4 / IPv6 快速路径（不需要 DNS；`::1` 必须走这一支）。
  struct sockaddr_in addr4;
  if (uv_ip4_addr(host, port, &addr4) == 0) {
    sockaddr_storage ss;
    std::memset(&ss, 0, sizeof(ss));
    std::memcpy(&ss, &addr4, sizeof(addr4));
    out.push_back(ss);
    return 0;
  }
  struct sockaddr_in6 addr6;
  if (uv_ip6_addr(host, port, &addr6) == 0) {
    sockaddr_storage ss;
    std::memset(&ss, 0, sizeof(ss));
    std::memcpy(&ss, &addr6, sizeof(addr6));
    out.push_back(ss);
    return 0;
  }

  // 域名：AF_UNSPEC（A + AAAA），全部收下。同步接口可以阻塞等待。
  uvcpp_getaddrinfo resolver;
  char service[16];
  uvcpp_snprintf(service, sizeof(service), "%d", port);

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  bool done = false;
  int  status = 0;
  struct addrinfo* res = nullptr;
  int rc = resolver.getaddrinfo(
      loop_, host, service, &hints,
      [&done, &status, &res](uvcpp_getaddrinfo*, int st, struct addrinfo* r) {
        status = st;
        res    = r;
        done   = true;
      });
  if (rc != 0) return rc;
  if (!wait_for_condition([&done]() { return done; }, timeout_ms)) {
    return UV_ETIMEDOUT;
  }
  if (status != 0 || res == nullptr) {
    if (res != nullptr) resolver.freeaddrinfo(res);
    return (status != 0) ? status : UV_EAI_NONAME;
  }
  for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
    if (p->ai_addr == nullptr || p->ai_addrlen == 0) continue;
    if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
    sockaddr_storage ss;
    std::memset(&ss, 0, sizeof(ss));
    std::memcpy(&ss, p->ai_addr,
                static_cast<size_t>(p->ai_addrlen) > sizeof(ss)
                    ? sizeof(ss)
                    : static_cast<size_t>(p->ai_addrlen));
    out.push_back(ss);
  }
  resolver.freeaddrinfo(res);
  return out.empty() ? UV_EAI_NONAME : 0;
}

int uvcpp_udp_client::pick_send_addr(
    const std::vector<sockaddr_storage>& addrs,
    sockaddr_storage& out) const {
  if (addrs.empty()) return UV_EAI_NONAME;

  // socket 的地址族：绑定过（或已经发过）就是 `getsockname` 报的那个；
  // 没绑定的句柄会由 libuv 用第一个目标做延迟绑定，取第一个即可。
  struct sockaddr_storage local;
  std::memset(&local, 0, sizeof(local));
  int local_len = static_cast<int>(sizeof(local));
  if (udp_->getsockname(reinterpret_cast<struct sockaddr*>(&local),
                        &local_len) != 0) {
    out = addrs.front();
    return 0;
  }

  const int fam = static_cast<int>(local.ss_family);
  for (std::size_t i = 0; i < addrs.size(); ++i) {
    if (static_cast<int>(addrs[i].ss_family) == fam) {
      out = addrs[i];
      return 0;
    }
  }
  return UV_EAFNOSUPPORT;
}

int uvcpp_udp_client::connect(const char* ip, int port) {
  std::vector<sockaddr_storage> addrs;
  int rc = resolve_send_addrs(ip, port, addrs, 30000);
  if (rc != 0) {
    last_error_code_ = rc;
    return rc;
  }

  sockaddr_storage chosen;
  std::memset(&chosen, 0, sizeof(chosen));
  rc = pick_send_addr(addrs, chosen);
  if (rc != 0) {
    last_error_code_ = rc;
    return rc;
  }
  rc = udp_->connect(reinterpret_cast<const struct sockaddr*>(&chosen));
  if (rc != 0) {
    last_error_code_ = rc;
    return rc;
  }
  set_status(UDP_CLIENT_CONNECTED);
  return 0;
}

// =========================================================================
// Send (to specific address) — async implementation
// =========================================================================

namespace {

int send_sockaddr_impl(uvcpp_udp* udp, int& last_err,
                       const struct sockaddr* addr,
                       uv_buf_t* raw, uvcpp_udp_send* w,
                       std::function<void(uvcpp_udp_send*, int)> cb) {
  if (addr == nullptr ||
      (addr->sa_family != AF_INET && addr->sa_family != AF_INET6)) {
    last_err = UV_EAFNOSUPPORT;
    delete w;
    return UV_EAFNOSUPPORT;
  }
  const int rc = udp->send(w, w->get_uv_buf(), 1, addr, cb);
  if (rc != 0) { last_err = rc; delete w; return rc; }
  return 0;
}

int send_connected_impl(uvcpp_udp* udp, uvcpp_loop* loop, int& last_err,
                         uv_buf_t* raw, uvcpp_udp_send* w,
                         std::function<void(uvcpp_udp_send*, int)> cb) {
  int rc = udp->send(w, w->get_uv_buf(), 1, nullptr, cb);
  if (rc != 0) { last_err = rc; delete w; return rc; }
  return 0;
}

}  // namespace

// =========================================================================
// Send (to specific address, const char*)
// =========================================================================

int uvcpp_udp_client::start_send_to(
    const char* host, int port, uv_buf_t* raw, uvcpp_udp_send* w,
    std::function<void(uvcpp_udp_send*, int)> cb) {
  (void)raw;  // 请求对象里已经有同一块缓冲（`w->get_uv_buf()`）。

  // 数字地址：不开解析器，直接发。
  struct sockaddr_storage numeric;
  std::memset(&numeric, 0, sizeof(numeric));
  bool is_numeric = false;
  struct sockaddr_in a4;
  if (uv_ip4_addr(host, port, &a4) == 0) {
    std::memcpy(&numeric, &a4, sizeof(a4));
    is_numeric = true;
  } else {
    struct sockaddr_in6 a6;
    if (uv_ip6_addr(host, port, &a6) == 0) {
      std::memcpy(&numeric, &a6, sizeof(a6));
      is_numeric = true;
    }
  }
  if (is_numeric) {
    const int src = udp_->send(
        w, w->get_uv_buf(), 1,
        reinterpret_cast<const struct sockaddr*>(&numeric), cb);
    if (src != 0) {
      // 提交失败没有完成回调：请求对象与错误码都由这里收尾（与旧实现一致）。
      last_error_code_ = src;
      delete w;
    }
    return src;
  }

  // 域名：**异步**解析 —— 异步发送路径里绝不能同步等 DNS（#49）。
  struct dns_ctx {
    uvcpp_udp_client* self;
    uvcpp_udp_send*   w;
    std::function<void(uvcpp_udp_send*, int)> cb;
    std::shared_ptr<char> life;
  };
  dns_ctx* c = new dns_ctx{this, w, std::move(cb), alive_token()};

  char service[16];
  uvcpp_snprintf(service, sizeof(service), "%d", port);
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  uvcpp_getaddrinfo* resolver = new uvcpp_getaddrinfo();
  const int rc = resolver->getaddrinfo(
      loop_, host, service, &hints,
      [c](uvcpp_getaddrinfo* req, int status, struct addrinfo* res) {
        std::vector<sockaddr_storage> addrs;
        if (status == 0 && res != nullptr) {
          for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
            if (p->ai_addr == nullptr || p->ai_addrlen == 0) continue;
            if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
            sockaddr_storage ss;
            std::memset(&ss, 0, sizeof(ss));
            std::memcpy(&ss, p->ai_addr,
                        static_cast<size_t>(p->ai_addrlen) > sizeof(ss)
                            ? sizeof(ss)
                            : static_cast<size_t>(p->ai_addrlen));
            addrs.push_back(ss);
          }
        }
        if (res != nullptr) req->freeaddrinfo(res);
        delete req;

        int err = (status != 0) ? status : (addrs.empty() ? UV_EAI_NONAME : 0);
        uvcpp_udp_send*   ww = c->w;
        auto              fn = std::move(c->cb);
        const bool alive = token_alive(c->life);
        uvcpp_udp_client* self = c->self;
        delete c;

        if (!alive) {
          delete ww;  // 客户端已经析构：请求对象没进 libuv，自行释放
          return;
        }
        if (err != 0) {
          fn(ww, err);
          delete ww;  // 不经过请求完成路径，不放没人放
          return;
        }
        // 地址族由 socket 决定（`bind()` 之后不能再换）：第一个候选可能是
        // 另一个地址族（`localhost` → `::1` 而 socket 绑在 `0.0.0.0`）。
        // `uv_udp_send` 对错族的目标**不报同步错**，而是把请求以
        // `EAFNOSUPPORT` 异步结算 —— 所以必须在提交之前挑对地址。
        sockaddr_storage chosen;
        std::memset(&chosen, 0, sizeof(chosen));
        const int perr = self->pick_send_addr(addrs, chosen);
        if (perr != 0) {
          fn(ww, perr);
          delete ww;
          return;
        }
        const int src = self->udp_->send(
            ww, ww->get_uv_buf(), 1,
            reinterpret_cast<const struct sockaddr*>(&chosen), fn);
        if (src != 0) {
          fn(ww, src);
          delete ww;
        }
      });

  if (rc != 0) {
    last_error_code_ = rc;
    delete resolver;
    delete w;
    delete c;
    return rc;
  }
  return 0;
}

int uvcpp_udp_client::send(const char* ip, int port, const char* data,
                            size_t len, std::function<void(int)> cb) {
  if (cb != nullptr) {
    // --- Async ---
    if (has_async_send_cb_) return UV_EALREADY;
    has_async_send_cb_ = true;
    send_fn_  = trampoline_send;
    send_arg_ = new std::function<void(int)>(cb);
    std::shared_ptr<char> life = alive_token();

    uvcpp_buf bufcpp(data, len);
    uv_buf_t* raw = bufcpp.out_uv_buf();
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(raw, true);
    w->set_self_free(true);  // 释放交给 callback_udp_send，见 set_self_free

    int rc = start_send_to(
        ip, port, raw, w,
        [this, life](uvcpp_udp_send* wr, int status) {
          if (!token_alive(life)) {
            return;  // 对象已析构：请求对象由 callback_udp_send 事后释放
          }
          (void)wr;  // 释放由 callback_udp_send 事后做
          if (status != 0) last_error_code_ = status;
          // 次序与 tcp 侧（a9730d3）一致：先把回调取出来再清，且
          // `has_async_send_cb_` 必须在**调用用户回调之前**清 —— 否则回调里
          // 接着发起下一次 send 仍然拿 UV_EALREADY，连接被永久毒化。
          auto  fn  = send_fn_;
          void* arg = send_arg_;
          send_fn_  = nullptr;
          send_arg_ = nullptr;
          has_async_send_cb_ = false;
          if (fn) fn(status, arg);
        });

    if (rc != 0) {
      // 提交失败同样会毒化连接，必须一起清（回调不会来，没人替它清）。
      delete static_cast<std::function<void(int)>*>(send_arg_);
      send_fn_  = nullptr;
      send_arg_ = nullptr;
      has_async_send_cb_ = false;
      return rc;
    }
    return 0;
  } else {
    return send_wait(ip, port, data, len, 30000);
  }
}

int uvcpp_udp_client::send_wait(const char* ip, int port, const char* data,
                                 size_t len, int timeout_ms) {
  if (has_async_send_cb_)
    throw std::runtime_error(
        "uvcpp_udp_client::send_wait: cannot mix sync/async send");

  sync_send_done_   = false;
  sync_send_result_ = 0;
  std::shared_ptr<char> life = alive_token();

  uvcpp_buf bufcpp(data, len);
  uv_buf_t* raw = bufcpp.out_uv_buf();
  uvcpp_udp_send* w = new uvcpp_udp_send();
  w->set_uv_buf(raw, true);
  w->set_self_free(true);  // 释放交给 callback_udp_send，见 set_self_free

  int rc = start_send_to(
      ip, port, raw, w,
      [this, life](uvcpp_udp_send* wr, int status) {
        if (!token_alive(life)) {
          return;  // 对象已析构：请求对象由 callback_udp_send 事后释放
        }
        (void)wr;  // 释放由 callback_udp_send 事后做
        if (status != 0) last_error_code_ = status;
        sync_send_result_ = status;
        sync_send_done_   = true;
      });

  if (rc != 0) return rc;

  if (!wait_for_condition([this]() { return sync_send_done_; }, timeout_ms)) {
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }
  return sync_send_result_;
}

int uvcpp_udp_client::send_addr(const struct sockaddr* addr, const char* data,
                                size_t len, std::function<void(int)> cb) {
  if (addr == nullptr) return UV_EINVAL;
  if (cb != nullptr) {
    if (has_async_send_cb_) return UV_EALREADY;
    has_async_send_cb_ = true;
    send_fn_  = trampoline_send;
    send_arg_ = new std::function<void(int)>(cb);
    std::shared_ptr<char> life = alive_token();

    uvcpp_buf bufcpp(data, len);
    uv_buf_t* raw = bufcpp.out_uv_buf();
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(raw, true);
    w->set_self_free(true);

    int rc = send_sockaddr_impl(
        udp_, last_error_code_, addr, raw, w,
        [this, life](uvcpp_udp_send* wr, int status) {
          if (!token_alive(life)) {
            return;
          }
          (void)wr;
          if (status != 0) last_error_code_ = status;
          auto  fn  = send_fn_;
          void* arg = send_arg_;
          send_fn_  = nullptr;
          send_arg_ = nullptr;
          has_async_send_cb_ = false;
          if (fn) fn(status, arg);
        });

    if (rc != 0) {
      delete static_cast<std::function<void(int)>*>(send_arg_);
      send_fn_  = nullptr;
      send_arg_ = nullptr;
      has_async_send_cb_ = false;
      return rc;
    }
    return 0;
  }
  return send_addr_wait(addr, data, len, 30000);
}

int uvcpp_udp_client::send_addr_wait(const struct sockaddr* addr,
                                     const char* data, size_t len,
                                     int timeout_ms) {
  if (addr == nullptr) return UV_EINVAL;
  if (has_async_send_cb_) {
    throw std::runtime_error(
        "uvcpp_udp_client::send_addr_wait: cannot mix sync/async send");
  }

  sync_send_done_   = false;
  sync_send_result_ = 0;
  std::shared_ptr<char> life = alive_token();

  uvcpp_buf bufcpp(data, len);
  uv_buf_t* raw = bufcpp.out_uv_buf();
  uvcpp_udp_send* w = new uvcpp_udp_send();
  w->set_uv_buf(raw, true);
  w->set_self_free(true);

  int rc = send_sockaddr_impl(
      udp_, last_error_code_, addr, raw, w,
      [this, life](uvcpp_udp_send* wr, int status) {
        if (!token_alive(life)) {
          return;
        }
        (void)wr;
        if (status != 0) last_error_code_ = status;
        sync_send_result_ = status;
        sync_send_done_   = true;
      });

  if (rc != 0) return rc;

  if (!wait_for_condition([this]() { return sync_send_done_; }, timeout_ms)) {
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }
  return sync_send_result_;
}

// =========================================================================
// Send (to specific address, uvcpp_buf overloads)
// =========================================================================

int uvcpp_udp_client::send(const char* ip, int port, const uvcpp_buf& buf,
                            std::function<void(int)> cb) {
  return send(ip, port, buf.get_const_data(), buf.size(), cb);
}

int uvcpp_udp_client::send(const char* ip, int port, uvcpp_buf* buf,
                            std::function<void(int)> cb) {
  if (buf == nullptr) return UV_EINVAL;
  if (cb != nullptr) {
    // --- Async ---
    if (has_async_send_cb_) return UV_EALREADY;
    has_async_send_cb_ = true;
    send_fn_  = trampoline_send;
    send_arg_ = new std::function<void(int)>(cb);
    std::shared_ptr<char> life = alive_token();

    uv_buf_t* raw = buf->out_uv_buf();
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(raw, true);
    w->set_self_free(true);  // 释放交给 callback_udp_send，见 set_self_free

    int rc = start_send_to(
        ip, port, raw, w,
        [this, life](uvcpp_udp_send* wr, int status) {
          if (!token_alive(life)) {
            return;  // 对象已析构：请求对象由 callback_udp_send 事后释放
          }
          (void)wr;  // 释放由 callback_udp_send 事后做
          if (status != 0) last_error_code_ = status;
          // 次序与 tcp 侧（a9730d3）一致：先把回调取出来再清，且
          // `has_async_send_cb_` 必须在**调用用户回调之前**清 —— 否则回调里
          // 接着发起下一次 send 仍然拿 UV_EALREADY，连接被永久毒化。
          auto  fn  = send_fn_;
          void* arg = send_arg_;
          send_fn_  = nullptr;
          send_arg_ = nullptr;
          has_async_send_cb_ = false;
          if (fn) fn(status, arg);
        });

    if (rc != 0) {
      // 提交失败同样会毒化连接，必须一起清（回调不会来，没人替它清）。
      delete static_cast<std::function<void(int)>*>(send_arg_);
      send_fn_  = nullptr;
      send_arg_ = nullptr;
      has_async_send_cb_ = false;
      return rc;
    }
    return 0;
  } else {
    // --- Sync with transfer ---
    if (has_async_send_cb_)
      throw std::runtime_error(
          "uvcpp_udp_client::send: cannot mix sync/async");

    sync_send_done_   = false;
    sync_send_result_ = 0;
    std::shared_ptr<char> life = alive_token();

    uv_buf_t* raw = buf->out_uv_buf();  // transfers ownership
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(raw, true);
    w->set_self_free(true);  // 释放交给 callback_udp_send，见 set_self_free

    int rc = start_send_to(
        ip, port, raw, w,
        [this, life](uvcpp_udp_send* wr, int status) {
          if (!token_alive(life)) {
            return;  // 对象已析构：请求对象由 callback_udp_send 事后释放
          }
          (void)wr;  // 释放由 callback_udp_send 事后做
          if (status != 0) last_error_code_ = status;
          sync_send_result_ = status;
          sync_send_done_   = true;
        });

    if (rc != 0) return rc;

    if (!wait_for_condition([this]() { return sync_send_done_; },
                           30000)) {
      last_error_code_ = UV_ETIMEDOUT;
      return UV_ETIMEDOUT;
    }
    return sync_send_result_;
  }
}

// =========================================================================
// Send (to connected peer, const char*)
// =========================================================================

int uvcpp_udp_client::send(const char* data, size_t len,
                            std::function<void(int)> cb) {
  if (!has_status(UDP_CLIENT_CONNECTED)) return UV_ENOTCONN;
  if (cb != nullptr) {
    if (has_async_send_cb_) return UV_EALREADY;
    has_async_send_cb_ = true;
    send_fn_  = trampoline_send;
    send_arg_ = new std::function<void(int)>(cb);
    std::shared_ptr<char> life = alive_token();

    uvcpp_buf bufcpp(data, len);
    uv_buf_t* raw = bufcpp.out_uv_buf();
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(raw, true);
    w->set_self_free(true);  // 释放交给 callback_udp_send，见 set_self_free

    int rc = send_connected_impl(
        udp_, loop_, last_error_code_, raw, w,
        [this, life](uvcpp_udp_send* wr, int status) {
          if (!token_alive(life)) {
            return;  // 对象已析构：请求对象由 callback_udp_send 事后释放
          }
          (void)wr;  // 释放由 callback_udp_send 事后做
          if (status != 0) last_error_code_ = status;
          // 次序与 tcp 侧（a9730d3）一致：先把回调取出来再清，且
          // `has_async_send_cb_` 必须在**调用用户回调之前**清 —— 否则回调里
          // 接着发起下一次 send 仍然拿 UV_EALREADY，连接被永久毒化。
          auto  fn  = send_fn_;
          void* arg = send_arg_;
          send_fn_  = nullptr;
          send_arg_ = nullptr;
          has_async_send_cb_ = false;
          if (fn) fn(status, arg);
        });

    if (rc != 0) {
      // 提交失败同样会毒化连接，必须一起清（回调不会来，没人替它清）。
      delete static_cast<std::function<void(int)>*>(send_arg_);
      send_fn_  = nullptr;
      send_arg_ = nullptr;
      has_async_send_cb_ = false;
      return rc;
    }
    return 0;
  } else {
    return send_wait(data, len, 30000);
  }
}

int uvcpp_udp_client::send_wait(const char* data, size_t len, int timeout_ms) {
  if (!has_status(UDP_CLIENT_CONNECTED)) return UV_ENOTCONN;
  if (has_async_send_cb_)
    throw std::runtime_error("uvcpp_udp_client::send_wait: cannot mix");

  sync_send_done_   = false;
  sync_send_result_ = 0;
  std::shared_ptr<char> life = alive_token();

  uvcpp_buf bufcpp(data, len);
  uv_buf_t* raw = bufcpp.out_uv_buf();
  uvcpp_udp_send* w = new uvcpp_udp_send();
  w->set_uv_buf(raw, true);
  w->set_self_free(true);  // 释放交给 callback_udp_send，见 set_self_free

  int rc = send_connected_impl(
      udp_, loop_, last_error_code_, raw, w,
      [this, life](uvcpp_udp_send* wr, int status) {
        if (!token_alive(life)) {
          return;  // 对象已析构：请求对象由 callback_udp_send 事后释放
        }
        (void)wr;  // 释放由 callback_udp_send 事后做
        if (status != 0) last_error_code_ = status;
        sync_send_result_ = status;
        sync_send_done_   = true;
      });

  if (rc != 0) return rc;

  if (!wait_for_condition([this]() { return sync_send_done_; }, timeout_ms)) {
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }
  return sync_send_result_;
}

// =========================================================================
// Send (to connected peer, uvcpp_buf overloads)
// =========================================================================

int uvcpp_udp_client::send(const uvcpp_buf& buf,
                            std::function<void(int)> cb) {
  return send(buf.get_const_data(), buf.size(), cb);
}

int uvcpp_udp_client::send(uvcpp_buf* buf,
                            std::function<void(int)> cb) {
  if (buf == nullptr) return UV_EINVAL;
  if (!has_status(UDP_CLIENT_CONNECTED)) return UV_ENOTCONN;
  if (cb != nullptr) {
    if (has_async_send_cb_) return UV_EALREADY;
    has_async_send_cb_ = true;
    send_fn_  = trampoline_send;
    send_arg_ = new std::function<void(int)>(cb);
    std::shared_ptr<char> life = alive_token();

    uv_buf_t* raw = buf->out_uv_buf();
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(raw, true);
    w->set_self_free(true);  // 释放交给 callback_udp_send，见 set_self_free

    int rc = send_connected_impl(
        udp_, loop_, last_error_code_, raw, w,
        [this, life](uvcpp_udp_send* wr, int status) {
          if (!token_alive(life)) {
            return;  // 对象已析构：请求对象由 callback_udp_send 事后释放
          }
          (void)wr;  // 释放由 callback_udp_send 事后做
          if (status != 0) last_error_code_ = status;
          // 次序与 tcp 侧（a9730d3）一致：先把回调取出来再清，且
          // `has_async_send_cb_` 必须在**调用用户回调之前**清 —— 否则回调里
          // 接着发起下一次 send 仍然拿 UV_EALREADY，连接被永久毒化。
          auto  fn  = send_fn_;
          void* arg = send_arg_;
          send_fn_  = nullptr;
          send_arg_ = nullptr;
          has_async_send_cb_ = false;
          if (fn) fn(status, arg);
        });

    if (rc != 0) {
      // 提交失败同样会毒化连接，必须一起清（回调不会来，没人替它清）。
      delete static_cast<std::function<void(int)>*>(send_arg_);
      send_fn_  = nullptr;
      send_arg_ = nullptr;
      has_async_send_cb_ = false;
      return rc;
    }
    return 0;
  } else {
    // --- Sync with transfer ---
    if (has_async_send_cb_)
      throw std::runtime_error(
          "uvcpp_udp_client::send: cannot mix sync/async");

    sync_send_done_   = false;
    sync_send_result_ = 0;
    std::shared_ptr<char> life = alive_token();

    uv_buf_t* raw = buf->out_uv_buf();
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(raw, true);
    w->set_self_free(true);  // 释放交给 callback_udp_send，见 set_self_free

    int rc = send_connected_impl(
        udp_, loop_, last_error_code_, raw, w,
        [this, life](uvcpp_udp_send* wr, int status) {
          if (!token_alive(life)) {
            return;  // 对象已析构：请求对象由 callback_udp_send 事后释放
          }
          (void)wr;  // 释放由 callback_udp_send 事后做
          if (status != 0) last_error_code_ = status;
          sync_send_result_ = status;
          sync_send_done_   = true;
        });

    if (rc != 0) return rc;

    if (!wait_for_condition([this]() { return sync_send_done_; }, 30000)) {
      last_error_code_ = UV_ETIMEDOUT;
      return UV_ETIMEDOUT;
    }
    return sync_send_result_;
  }
}

// =========================================================================
// Receive
// =========================================================================

int uvcpp_udp_client::recv_start(
    std::function<void(uvcpp_buf*, const char*, int)> cb) {
  if (cb != nullptr) {
    // --- Async ---
    if (has_async_recv_cb_) return UV_EALREADY;
    has_async_recv_cb_ = true;

    recv_fn_  = trampoline_recv;
    recv_arg_ = new std::function<void(uvcpp_buf*, const char*, int)>(cb);
    std::shared_ptr<char> life = alive_token();

    ensure_read_cache();

    int rc = udp_->recv_start(
        internal_alloc_cb,
        [this, life](uvcpp_udp* u, ssize_t nread, const uv_buf_t* buf,
               const struct sockaddr* addr, unsigned int flags) {
          if (!token_alive(life)) {
            if (buf->base != nullptr) uvcpp_free_bytes(buf->base);
            return;
          }
          if (nread > 0) {
            set_status(UDP_CLIENT_READABLE);
            uvcpp_buf tmp_buf;
            tmp_buf.clone_data(buf->base, static_cast<size_t>(nread));

            char sender_ip[INET6_ADDRSTRLEN] = {0};
            int sender_port = 0;
            if (addr != nullptr) {
              if (addr->sa_family == AF_INET) {
                auto* in = reinterpret_cast<const struct sockaddr_in*>(addr);
                uv_ip4_name(in, sender_ip, sizeof(sender_ip));
                sender_port = ntohs(in->sin_port);
              } else if (addr->sa_family == AF_INET6) {
                auto* in6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
                uv_ip6_name(in6, sender_ip, sizeof(sender_ip));
                sender_port = ntohs(in6->sin6_port);
              }
            }

            if (recv_fn_) {
              recv_fn_(&tmp_buf, sender_ip, sender_port, recv_arg_);
            }
          } else if (nread < 0) {
            last_error_code_ = static_cast<int>(nread);
            set_status(UDP_CLIENT_ERROR);
          }
          if (buf->base != nullptr) uvcpp_free_bytes(buf->base);
        });

    if (rc != 0) { last_error_code_ = rc; return rc; }
    recv_started_ = true;
    return 0;
  } else {
    // --- Enable sync recv cache ---
    if (has_async_recv_cb_)
      throw std::runtime_error("uvcpp_udp_client::recv_start: cannot mix");
    if (recv_started_) return 0;

    ensure_read_cache();

    std::shared_ptr<char> life = alive_token();
    int rc = udp_->recv_start(
        internal_alloc_cb,
        [this, life](uvcpp_udp* u, ssize_t nread, const uv_buf_t* buf,
               const struct sockaddr* addr, unsigned int flags) {
          if (!token_alive(life)) {
            if (buf->base != nullptr) uvcpp_free_bytes(buf->base);
            return;
          }
          on_internal_recv(u, nread, buf, addr, flags);
        });

    if (rc != 0) { last_error_code_ = rc; return rc; }
    recv_started_ = true;
    return 0;
  }
}

int uvcpp_udp_client::recv_wait(uvcpp_buf& out_buf, int timeout_ms) {
  if (has_async_recv_cb_)
    throw std::runtime_error("uvcpp_udp_client::recv_wait: cannot mix");

  if (!recv_started_) {
    int rc = recv_start(nullptr);
    if (rc != 0) return rc;
  }

  bool has_data = wait_for_condition(
      [this]() { return read_cache_ != nullptr && read_cache_->size() > 0; },
      timeout_ms);

  if (!has_data) {
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }

  out_buf.move_buf(*read_cache_);
  return 0;
}

int uvcpp_udp_client::recv_stop() {
  clear_status(UDP_CLIENT_READABLE);
  recv_started_ = false;
  return udp_->recv_stop();
}

// =========================================================================
// Read cache
// =========================================================================

void uvcpp_udp_client::set_max_read_cache_size(size_t max_size) {
  max_read_cache_size_ = max_size;
}

size_t uvcpp_udp_client::get_max_read_cache_size() const {
  return max_read_cache_size_;
}

void uvcpp_udp_client::ensure_read_cache() {
  if (read_cache_ == nullptr) read_cache_ = new uvcpp_buf();
}

// =========================================================================
// Loop control
// =========================================================================

int uvcpp_udp_client::run(uv_run_mode md) { return loop_->run(md); }
void uvcpp_udp_client::stop() { loop_->stop(); }

// =========================================================================
// Internal helpers
// =========================================================================

void uvcpp_udp_client::set_status(int flags) { status_ |= flags; }
void uvcpp_udp_client::clear_status(int flags) { status_ &= ~flags; }

bool uvcpp_udp_client::wait_for_condition(
    std::function<bool()> condition, int timeout_ms) {
  auto start = std::chrono::steady_clock::now();
  while (true) {
    if (condition()) return true;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    if (elapsed >= timeout_ms) return false;
    if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void uvcpp_udp_client::internal_alloc_cb(uvcpp_handle* /*h*/, size_t sz,
                                          uv_buf_t* buf) {
  uvcpp_buf::alloc_buf(buf, sz > 0 ? sz : 4096);
}

void uvcpp_udp_client::on_internal_recv(uvcpp_udp* /*u*/, ssize_t nread,
                                         const uv_buf_t* buf,
                                         const struct sockaddr* /*addr*/,
                                         unsigned int /*flags*/) {
  if (nread > 0) {
    set_status(UDP_CLIENT_READABLE);
    ensure_read_cache();
    read_cache_->append_data(buf->base, static_cast<size_t>(nread));
    if (read_cache_->size() > max_read_cache_size_) {
      udp_->recv_stop();
      recv_started_ = false;
    }
  } else if (nread < 0) {
    last_error_code_ = static_cast<int>(nread);
    set_status(UDP_CLIENT_ERROR);
  }
  if (buf->base != nullptr) uvcpp_free_bytes(buf->base);
}

}  // namespace uvcpp
