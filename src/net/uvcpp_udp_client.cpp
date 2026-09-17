/**
 * @file src/net/uvcpp_udp_client.cpp
 * @brief Implementation of uvcpp_udp_client.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <net/uvcpp_udp_client.h>
#include <req/uvcpp_udp_send.h>
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
      bool close_done = false;
      udp_->close([&close_done](uvcpp_handle*) { close_done = true; });

      for (int i = 0; i < 64 && !close_done; ++i) {
        if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
      }
    } else {
      if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
    }
  }

  // Close loop first, then delete the handle.
  //
  // 关之前按循环自己的判据再泵一遍：上面那几步只保证 **udp 句柄**收尾了，
  // 循环上可能还挂着别人（`async` / `timer` 之类只关了一半的）。判据是
  // `loop_alive()` 而不是 `loop_close()` —— 后者内部会 `stop()`，而 `uv_run`
  // 的 `while (r != 0 && loop->stop_flag == 0)` 是**进 body 之前**判的，
  // 停标志一立那一轮就整段空转，交替调用等于原地打转。
  if (loop_ != nullptr) {
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

int uvcpp_udp_client::connect(const char* ip, int port) {
  struct sockaddr_in addr;
  int rc = uv_ip4_addr(ip, port, &addr);
  if (rc != 0) {
    last_error_code_ = rc;
    return rc;
  }
  rc = udp_->connect(reinterpret_cast<const struct sockaddr*>(&addr));
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

int send_to_addr_impl(uvcpp_udp* udp, uvcpp_loop* loop, int& last_err,
                       const char* ip, int port,
                       uv_buf_t* raw, uvcpp_udp_send* w,
                       std::function<void(uvcpp_udp_send*, int)> cb) {
  struct sockaddr_in addr;
  int rc = uv_ip4_addr(ip, port, &addr);
  if (rc != 0) { last_err = rc; delete w; return rc; }

  rc = udp->send(w, w->get_uv_buf(), 1,
                 reinterpret_cast<const struct sockaddr*>(&addr), cb);
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

    int rc = send_to_addr_impl(
        udp_, loop_, last_error_code_, ip, port, raw, w,
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

  int rc = send_to_addr_impl(
      udp_, loop_, last_error_code_, ip, port, raw, w,
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

    int rc = send_to_addr_impl(
        udp_, loop_, last_error_code_, ip, port, raw, w,
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

    int rc = send_to_addr_impl(
        udp_, loop_, last_error_code_, ip, port, raw, w,
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
