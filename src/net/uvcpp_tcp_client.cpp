/**
 * @file src/net/uvcpp_tcp_client.cpp
 * @brief Implementation of uvcpp_tcp_client.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <net/uvcpp_tcp_client.h>
#include <req/uvcpp_connect.h>
#include <req/uvcpp_write.h>
#include <req/uvcpp_getaddrinfo.h>
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_define.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl.h>
#include <ssl/uvcpp_ssl_context.h>
// SSL_ERROR_* 常量：`uvcpp_ssl::read/write/handshake` 把 WANT_READ/WANT_WRITE
// 归一化成 0，光看返回值分不出「还没完」和「对端干净关闭」，必须看这个。
#include <openssl/ssl.h>
#endif

namespace uvcpp {

// =========================================================================
// Trampolines — bridge C-style callbacks to std::function stored on heap.
// These break the std::function copy-chain through libuv's callback layers.
// =========================================================================

static void trampoline_connect(int status, void* arg) {
  auto* cb = static_cast<std::function<void(int)>*>(arg);
  (*cb)(status);
  delete cb;
}

static void trampoline_write(int status, void* arg) {
  auto* cb = static_cast<std::function<void(int)>*>(arg);
  (*cb)(status);
  delete cb;
}

static void trampoline_read(uvcpp_buf* buf, void* arg) {
  auto* cb = static_cast<std::function<void(uvcpp_buf*)>*>(arg);
  (*cb)(buf);
  // Note: cb is NOT deleted here — read callback persists for multiple reads
}

static void trampoline_close(void* arg) {
  auto* cb = static_cast<std::function<void()>*>(arg);
  (*cb)();
  delete cb;
}

// =========================================================================
// Construction / Destruction
// =========================================================================

uvcpp_tcp_client::uvcpp_tcp_client() {
  loop_ = new uvcpp_loop();  // constructor already calls init()

  tcp_ = new uvcpp_tcp(loop_);

  status_ = TCP_CLIENT_NONE;
}

uvcpp_tcp_client::uvcpp_tcp_client(uvcpp_loop* external_loop) {
  owns_loop_ = false;
  loop_ = external_loop;

  tcp_ = new uvcpp_tcp(loop_);

  status_ = TCP_CLIENT_NONE;
}

std::shared_ptr<char> uvcpp_tcp_client::alive_token() {
  if (!alive_token_) alive_token_.reset(new char(0));
  return alive_token_;
}

bool uvcpp_tcp_client::token_alive(const std::shared_ptr<char>& token) {
  return token && *token == 0;
}

uvcpp_tcp_client::~uvcpp_tcp_client() {
  // **第一件事就把令牌作废**：此后 libuv 送进来的任何异步完成回调都只收尾
  // （把请求对象还回去），不再碰本对象的任何成员。
  if (alive_token_) *alive_token_ = 1;

#if UVCPP_OPENSSL_ENABLE
  // TLS 对象归本客户端所有，跟着一起走。注意这里**不发 close_notify**：
  // 发它要先把 wbio 里的字节异步写出去，而析构路径上没有事件循环可等。
  // 所以对端看到的是 TCP 层的断开（`SSL_ERROR_SYSCALL` / 提前 EOF）而不是
  // 干净的 TLS 关闭 —— 这是有意的取舍，写在 `enable_tls` 的文档里。
  if (tls_ssl_ != nullptr) {
    delete tls_ssl_;
    tls_ssl_ = nullptr;
  }
#endif

  // The underlying handle may already be closed and freed (_handle nulled) by
  // an outer owner (e.g. uvcpp_http_client closes the tcp directly). Calling
  // read_stop()/is_closing()/is_active() on a null handle dereferences null,
  // so guard all handle access against it here.
  const bool handle_alive =
      (tcp_ != nullptr && tcp_->get_handle() != nullptr);

  // Stop any active reads
  if (read_started_) {
    if (handle_alive) {
      tcp_->read_stop();
    }
    read_started_ = false;
  }

  // Ensure the TCP handle is fully closed and its endgame processed
  // before we try to close the loop.  libuv asserts the handle queue is
  // empty before uv_loop_close().
  if (tcp_ != nullptr && handle_alive) {
    // **判据只能是 `is_closing()`，不能带上 `is_active()`。**
    //
    // 上面刚调过 `read_stop()`，而 `uv_read_stop` 会摘掉读 watcher —— 句柄在
    // 没有在途写时随之变成"不活跃"。原先的写法是
    // `!is_closing() && is_active()`，于是"我刚把读停掉、socket 还开着"和
    // "句柄早就关完了"这两种完全不同的状态落进了同一个 else（注释里只写了
    // 后者），结果是**析构时根本不关 socket**：对端永远等不到 FIN，服务端的
    // 连接登记表里留着一个死连接，fd 一直占着直到进程退出。
    //
    // 句柄真正关完之后 `uvcpp_handle` 会把内部句柄指针置空，那时
    // `handle_alive` 就是 false，压根走不到这里 —— 所以在 `handle_alive` 为真
    // 的前提下，"不在关闭中"确实等价于"还开着"，`is_active()` 提供不了任何
    // 额外信息，只会把上面那次 read_stop 的副作用误读成"已经关完了"。
    //
    // 这个缺陷由 Phase 4b 的 TLS 回环用例（场景 4，同步接口）撞出来：同步
    // 客户端在析构时正好处于"读过、还开着、已经没有在读"的状态，而异步用例
    // 与写-only 用例都在返回前显式 `close()` 过，所以一直没暴露。
    if (!tcp_->is_closing()) {
      // Handle is still open — close it and pump the loop.
      if (owns_loop_) {
        bool close_done = false;
        tcp_->close([&close_done](uvcpp_handle*) { close_done = true; });

        auto start = std::chrono::steady_clock::now();
        while (!close_done) {
          if (loop_ != nullptr) {
            loop_->run(UV_RUN_NOWAIT);
          }
          auto elapsed =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
          if (elapsed > 5000) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      } else {
        tcp_->close([](uvcpp_handle*) {});
      }
    } else if (tcp_->is_closing()) {
      // Handle was already closed (e.g. by EOF callback) but its
      // endgame hasn't fired yet.  Pump the loop once to let it run.
      if (owns_loop_ && loop_ != nullptr) {
        loop_->run(UV_RUN_NOWAIT);
      }
    }
    // else: handle already fully closed (endgame fired) — nothing to do.
  }

  // Close the loop BEFORE deleting the TCP handle wrapper.
  // This ensures any internal libuv clean-up queued during handle close
  // is fully drained before uv_loop_close() validates the handle queue.
  if (loop_ != nullptr && owns_loop_) {
    loop_->loop_close();
    delete loop_;
    loop_ = nullptr;
  }

  // Now safe to delete the TCP handle — the loop is already closed.
  if (tcp_ != nullptr) {
    delete tcp_;
    tcp_ = nullptr;
  }

  // Free read cache
  if (read_cache_ != nullptr) {
    delete read_cache_;
    read_cache_ = nullptr;
  }

  // Free any pending async callback data
  if (connect_arg_ != nullptr) {
    delete static_cast<std::function<void(int)>*>(connect_arg_);
    connect_arg_ = nullptr;
  }
  if (write_arg_ != nullptr) {
    delete static_cast<std::function<void(int)>*>(write_arg_);
    write_arg_ = nullptr;
  }
  if (read_arg_ != nullptr) {
    delete static_cast<std::function<void(uvcpp_buf*)>*>(read_arg_);
    read_arg_ = nullptr;
  }
  if (close_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_arg_);
    close_arg_ = nullptr;
  }
  if (close_mgr_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_mgr_arg_);
    close_mgr_arg_ = nullptr;
  }
}

// =========================================================================
// Accessors
// =========================================================================

uvcpp_tcp* uvcpp_tcp_client::get_tcp() {
  return tcp_;
}

uvcpp_loop* uvcpp_tcp_client::get_loop() {
  return loop_;
}

int uvcpp_tcp_client::get_status() const {
  return status_;
}

int uvcpp_tcp_client::get_last_error() const {
  return last_error_code_;
}

bool uvcpp_tcp_client::has_status(int flags) const {
  return (status_ & flags) == flags;
}

bool uvcpp_tcp_client::has_read_callback() const {
  // 自动读不算 "用户设了读回调"：它没有消费者，数据是丢掉的。
  return has_async_read_cb_ && !auto_read_installed_;
}

bool uvcpp_tcp_client::is_auto_read() const {
  return auto_read_installed_;
}

bool uvcpp_tcp_client::has_write_callback() const {
  return has_async_write_cb_;
}

bool uvcpp_tcp_client::has_close_callback() const {
  return close_fn_ != nullptr;
}

void uvcpp_tcp_client::mark_accepted() {
  set_status(TCP_CLIENT_CONNECTED | TCP_CLIENT_READABLE | TCP_CLIENT_WRITABLE);
}

// =========================================================================
// Address helpers
// =========================================================================

sockaddr_storage uvcpp_tcp_client::getLocalAddrs(std::string& ip, int& port) {
  struct sockaddr_storage local_addr;
  std::memset(&local_addr, 0, sizeof(local_addr));
  int local_addr_len = sizeof(local_addr);

  if (tcp_ == nullptr) {
    return local_addr;
  }

  int rc = tcp_->getsockname(
      reinterpret_cast<struct sockaddr*>(&local_addr), &local_addr_len);
  if (rc != 0) {
    last_error_code_ = rc;
    return local_addr;
  }

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

sockaddr_storage uvcpp_tcp_client::getPeerAddrs(std::string& ip, int& port) {
  struct sockaddr_storage peer_addr;
  std::memset(&peer_addr, 0, sizeof(peer_addr));
  int peer_addr_len = sizeof(peer_addr);

  if (tcp_ == nullptr) {
    return peer_addr;
  }

  int rc = tcp_->getpeername(
      reinterpret_cast<struct sockaddr*>(&peer_addr), &peer_addr_len);
  if (rc != 0) {
    last_error_code_ = rc;
    return peer_addr;
  }

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
// Read cache
// =========================================================================

void uvcpp_tcp_client::set_max_read_cache_size(size_t max_size) {
  max_read_cache_size_ = max_size;
}

size_t uvcpp_tcp_client::get_max_read_cache_size() const {
  return max_read_cache_size_;
}

void uvcpp_tcp_client::ensure_read_cache() {
  if (read_cache_ == nullptr) {
    read_cache_ = new uvcpp_buf();
  }
}

// =========================================================================
// Connect
// =========================================================================

int uvcpp_tcp_client::connect(const char* ip, int port,
                               std::function<void(int)> cb) {
  if (cb != nullptr) {
    // --- Async mode ---
    if (has_async_connect_cb_) {
      return UV_EALREADY;
    }

    has_async_connect_cb_ = true;
    set_status(TCP_CLIENT_CONNECTING);

    // 数字 IPv4 快速路径（无需 DNS）
    struct sockaddr_in addr4;
    if (uv_ip4_addr(ip, port, &addr4) == 0) {
      int rc = connect_async(reinterpret_cast<const struct sockaddr*>(&addr4),
                             std::move(cb));
      if (rc != 0) has_async_connect_cb_ = false;
      return rc;
    }

    // 主机名：异步 DNS 解析后连接
    uvcpp_getaddrinfo* resolver = new uvcpp_getaddrinfo();
    char service[16];
    snprintf(service, sizeof(service), "%d", port);

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;      // 与历史行为一致，仅 IPv4
    hints.ai_socktype = SOCK_STREAM;

    int rc = resolver->getaddrinfo(
        loop_, ip, service, &hints,
        [this, cb](uvcpp_getaddrinfo* req, int status, struct addrinfo* res) {
          // DNS 失败或无可解析地址
          if (status != 0 || res == nullptr) {
            int err = (status != 0) ? status : UV_EAI_NONAME;
            clear_status(TCP_CLIENT_CONNECTING);
            set_status(TCP_CLIENT_ERROR);
            last_error_code_      = err;
            has_async_connect_cb_ = false;
            if (res != nullptr) req->freeaddrinfo(res);
            delete req;
            cb(err);
            return;
          }

          // 用第一个解析结果发起连接
          int rc2 = connect_async(res->ai_addr, cb);
          req->freeaddrinfo(res);
          delete req;

          if (rc2 != 0) {
            // 连接未能发起（例如 socket 创建失败）
            clear_status(TCP_CLIENT_CONNECTING);
            set_status(TCP_CLIENT_ERROR);
            last_error_code_      = rc2;
            has_async_connect_cb_ = false;
            cb(rc2);
          }
        });

    if (rc != 0) {
      delete resolver;
      clear_status(TCP_CLIENT_CONNECTING);
      set_status(TCP_CLIENT_ERROR);
      last_error_code_      = rc;
      has_async_connect_cb_ = false;
      return rc;
    }

    return 0;
  } else {
    // --- Sync mode ---
    return connect_wait(ip, port, 30000);
  }
}

int uvcpp_tcp_client::connect_wait(const char* ip, int port, int timeout_ms) {
  if (has_async_connect_cb_) {
    throw std::runtime_error(
        "uvcpp_tcp_client::connect_wait: cannot use sync connect after "
        "async connect callback was registered");
  }

  // 解析主机名（数字 IP 快速路径 + DNS 回退），阻塞等待
  sockaddr_storage resolved;
  std::memset(&resolved, 0, sizeof(resolved));
  int rc = resolve_host_sync(ip, port, resolved, timeout_ms);
  if (rc != 0) {
    set_status(TCP_CLIENT_ERROR);
    last_error_code_ = rc;
    return rc;
  }

  // Reset sync state
  sync_connect_done_   = false;
  sync_connect_result_ = 0;

  set_status(TCP_CLIENT_CONNECTING);

  uvcpp_connect* conn = new uvcpp_connect();
  std::shared_ptr<char> life = alive_token();
  rc = tcp_->connect(
      conn, reinterpret_cast<const struct sockaddr*>(&resolved),
      [this, life](uvcpp_connect* r, int status) {
        if (!token_alive(life)) {
          delete r;  // 对象已析构：只把请求对象还回去
          return;
        }
        clear_status(TCP_CLIENT_CONNECTING);
        if (status == 0) {
          set_status(TCP_CLIENT_CONNECTED | TCP_CLIENT_READABLE |
                     TCP_CLIENT_WRITABLE);
        } else {
          set_status(TCP_CLIENT_ERROR);
          last_error_code_ = status;
        }
        sync_connect_result_ = status;
        sync_connect_done_   = true;
        delete r;
      });

  if (rc != 0) {
    clear_status(TCP_CLIENT_CONNECTING);
    set_status(TCP_CLIENT_ERROR);
    last_error_code_ = rc;
    delete conn;
    return rc;
  }

  bool completed = wait_for_condition(
      [this]() { return sync_connect_done_; }, timeout_ms);

  if (!completed) {
    set_status(TCP_CLIENT_ERROR);
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }

  if (sync_connect_result_ != 0) return sync_connect_result_;

#if UVCPP_OPENSSL_ENABLE
  if (tls_ssl_ != nullptr) {
    // 握手同样要在循环里推进。这里只负责把循环转起来等它 —— 推进本身发生在
    // 读事件里（arm_async_read 装的那个回调），与异步路径走的是同一条代码。
    if (!read_started_) {
      const int arc = arm_async_read();
      if (arc != 0) {
        set_status(TCP_CLIENT_ERROR);
        last_error_code_ = arc;
        return arc;
      }
    }
    tls_drive_handshake();
    tls_flush_out();

    if (!tls_handshake_done_ && !has_status(TCP_CLIENT_ERROR)) {
      const bool hs = wait_for_condition(
          [this]() {
            return tls_handshake_done_ || has_status(TCP_CLIENT_ERROR);
          },
          timeout_ms);
      if (!hs) {
        set_status(TCP_CLIENT_ERROR);
        last_error_code_ = UV_ETIMEDOUT;
        return UV_ETIMEDOUT;
      }
    }
    if (!tls_handshake_done_) {
      // 失败原因由 tls_fail 记在 last_error_code_ 上（证书校验失败、
      // 对端不是 TLS 服务端等都归到 UV_EPROTO）。
      if (last_error_code_ == 0) last_error_code_ = UV_EPROTO;
      set_status(TCP_CLIENT_ERROR);
      return last_error_code_;
    }
    tls_deliver_plain();
  }
#endif

  return sync_connect_result_;
}

// -------------------------------------------------------------------------
// 连接辅助：已解析地址的异步连接 / 主机名同步解析
// -------------------------------------------------------------------------

int uvcpp_tcp_client::connect_async(const struct sockaddr* addr,
                                    std::function<void(int)> cb) {
  // Allocate callback data on heap, pass through C-style trampoline
  // to avoid std::function copy-chain corruption through libuv layers.
  connect_fn_  = trampoline_connect;
  connect_arg_ = new std::function<void(int)>(std::move(cb));

  uvcpp_connect* conn = new uvcpp_connect();

  std::shared_ptr<char> life = alive_token();

  int rc = tcp_->connect(
      conn, addr,
      [this, life](uvcpp_connect* r, int status) {
        if (!token_alive(life)) {
          delete r;  // 对象已析构：只把请求对象还回去
          return;
        }
        clear_status(TCP_CLIENT_CONNECTING);
        if (status == 0) {
          set_status(TCP_CLIENT_CONNECTED | TCP_CLIENT_READABLE |
                     TCP_CLIENT_WRITABLE);
        } else {
          set_status(TCP_CLIENT_ERROR);
          last_error_code_ = status;
        }

#if UVCPP_OPENSSL_ENABLE
        if (status == 0 && tls_ssl_ != nullptr) {
          // **握手没完成之前不回调使用者。** 契约是「回调到了就能直接发应用
          // 数据」，而 TLS 连接在握手完成前发不出明文 —— 若在这里就回调，
          // 使用者紧接着的 write() 会撞上还没建立的会话。
          tls_connect_pending_ = true;

          // 握手要靠读事件推进：没人读就永远等不到 ServerHello。
          if (!read_started_) arm_async_read();
          tls_drive_handshake();
          if (token_alive(life)) tls_flush_out();
          if (token_alive(life) && tls_handshake_done_) {
            tls_deliver_plain();  // 此阶段还没有消费者，会先攒着
            if (token_alive(life)) tls_complete_connect();
          }
          // delete r 是最后一句。**此后不再碰 this** —— r 持有的就是正在跑的
          // 这个闭包，释放它就是抽掉自己的存储（既有写法，见 fire_close_*
          // 与计划文件里记的那四处 `delete wr`）。
          delete r;
          return;
        }
#endif

        // Call user callback BEFORE deleting r — the lambda stored in
        // r->m_connect_cb is still alive so 'this' access is safe.
        if (connect_fn_) {
          connect_fn_(status, connect_arg_);
          connect_fn_  = nullptr;
          connect_arg_ = nullptr;
        }
        delete r;
      });

  if (rc != 0) {
    clear_status(TCP_CLIENT_CONNECTING);
    set_status(TCP_CLIENT_ERROR);
    last_error_code_ = rc;
    delete conn;
    if (connect_arg_ != nullptr) {
      delete static_cast<std::function<void(int)>*>(connect_arg_);
      connect_arg_ = nullptr;
    }
    connect_fn_ = nullptr;
    return rc;
  }

  return 0;
}

int uvcpp_tcp_client::resolve_host_sync(const char* host, int port,
                                        sockaddr_storage& out,
                                        int timeout_ms) {
  // 数字 IPv4 快速路径（无需 DNS）
  struct sockaddr_in addr4;
  if (uv_ip4_addr(host, port, &addr4) == 0) {
    std::memcpy(&out, &addr4, sizeof(addr4));
    return 0;
  }

  // 主机名：DNS 解析（阻塞等待完成）
  uvcpp_getaddrinfo resolver;
  char service[16];
  snprintf(service, sizeof(service), "%d", port);

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

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

  std::memcpy(&out, res->ai_addr, res->ai_addrlen);
  resolver.freeaddrinfo(res);
  return 0;
}

// =========================================================================
// Write
// =========================================================================

int uvcpp_tcp_client::write(const char* data, size_t len,
                             std::function<void(int)> cb) {
  if (!has_status(TCP_CLIENT_CONNECTED)) {
    return UV_ENOTCONN;
  }

  if (cb != nullptr) {
    // --- Async mode ---
    if (has_async_write_cb_) {
      return UV_EALREADY;
    }

    has_async_write_cb_ = true;

    write_fn_  = trampoline_write;
    write_arg_ = new std::function<void(int)>(cb);

#if UVCPP_OPENSSL_ENABLE
    if (tls_ssl_ != nullptr) {
      // 把成员恢复原状再报错 —— 调用方收到非 0 就应当认为这次写没发生。
      if (!tls_handshake_done_) {
        has_async_write_cb_ = false;
        delete static_cast<std::function<void(int)>*>(write_arg_);
        write_fn_  = nullptr;
        write_arg_ = nullptr;
        return UV_ENOTCONN;
      }

      const int trc = tls_write_plain(data, len);
      if (trc != 0) {
        last_error_code_ = trc;
        has_async_write_cb_ = false;
        delete static_cast<std::function<void(int)>*>(write_arg_);
        write_fn_  = nullptr;
        write_arg_ = nullptr;
        return trc;
      }

      // 明文已经全交给 SSL 了，密文还在 wbio 里 —— 等它出网才算完成。
      tls_write_pending_ = true;
      tls_write_sync_    = false;
      tls_flush_out();
      return 0;
    }
#endif

    uvcpp_buf bufcpp(data, len);
    uv_buf_t* raw_buf = bufcpp.out_uv_buf();

    uvcpp_write* w = new uvcpp_write();
    w->set_uv_buf(raw_buf, true);

    std::shared_ptr<char> life = alive_token();

    int rc = tcp_->write(
        w, w->get_uv_buf(), 1,
        [this, life](uvcpp_write* wr, int status) {
          // 本对象已经析构：`write_fn_` / `write_arg_` / `last_error_code_`
          // 全都不能碰了。唯一还能做的就是把请求对象（连同它持有的缓冲区）
          // 还回去 —— 不做的话每次断连都漏一块。
          if (!token_alive(life)) {
            delete wr;
            return;
          }
          if (status != 0) {
            last_error_code_ = status;
          }
          if (write_fn_) {
            // 先把成员清干净、**再**回调。次序是有讲究的：
            // - `has_async_write_cb_` 必须在回调**之前**清。回调里最自然的事就是
            //   接着发起下一次写（WS 的发送队列、Phase 3 的流式写都这么做），
            //   而 write() 见到这个标志还立着会直接返回 UV_EALREADY —— 下一次写
            //   会被判成失败。
            // - `write_fn_` / `write_arg_` 也必须先存后清：回调里新装上的回调
            //   属于**下一次**写，如果在回调返回后无条件置空，就会把它擦掉。
            write_callback_t fn  = write_fn_;
            void*            arg = write_arg_;
            write_fn_  = nullptr;
            write_arg_ = nullptr;
            has_async_write_cb_ = false;
            fn(status, arg);
          } else {
            has_async_write_cb_ = false;
          }
          delete wr;
        });

    if (rc != 0) {
      last_error_code_ = rc;
      delete w;
      delete static_cast<std::function<void(int)>*>(write_arg_);
      write_fn_  = nullptr;
      write_arg_ = nullptr;
      // 提交失败这条路上也必须清 —— 与成功路径同一判据。漏了它，一次失败的
      // 异步写会把连接**永久**毒化：此后每一次异步写都拿到 UV_EALREADY。
      has_async_write_cb_ = false;
      return rc;
    }

    return 0;
  } else {
    // --- Sync mode ---
    return write_wait(data, len, 30000);
  }
}

int uvcpp_tcp_client::write_wait(const char* data, size_t len,
                                  int timeout_ms) {
  if (!has_status(TCP_CLIENT_CONNECTED)) {
    return UV_ENOTCONN;
  }

  if (has_async_write_cb_) {
    throw std::runtime_error(
        "uvcpp_tcp_client::write_wait: cannot use sync write after "
        "async write callback was registered");
  }

  // Reset sync state
  sync_write_done_   = false;
  sync_write_result_ = 0;

#if UVCPP_OPENSSL_ENABLE
  if (tls_ssl_ != nullptr) {
    if (!tls_handshake_done_) return UV_ENOTCONN;

    const int trc = tls_write_plain(data, len);
    if (trc != 0) {
      last_error_code_ = trc;
      return trc;
    }
    tls_write_pending_ = true;
    tls_write_sync_    = true;
    tls_flush_out();

    const bool ok = wait_for_condition(
        [this]() { return sync_write_done_; }, timeout_ms);
    if (!ok) {
      // 超时不代表密文没出去 —— 它可能已经出网、只是完成回调还没送到。
      // 如实按超时报告，调用方据此判断连接状态即可。
      tls_write_pending_ = false;
      tls_write_sync_    = false;
      last_error_code_   = UV_ETIMEDOUT;
      return UV_ETIMEDOUT;
    }
    return sync_write_result_;
  }
#endif

  uvcpp_buf bufcpp(data, len);            // stack-allocated helper
  uv_buf_t* raw_buf = bufcpp.out_uv_buf();  // transfers data ownership to raw_buf

  uvcpp_write* w = new uvcpp_write();
  w->set_uv_buf(raw_buf, true);           // w takes ownership of raw_buf and its data

  // 同步写也可能超时返回，把完成回调留在 libuv 队列里 —— 之后对象被销毁，
  // 那个回调照样会来。所以同步路径用同一套令牌。
  std::shared_ptr<char> life = alive_token();

  int rc = tcp_->write(
      w, w->get_uv_buf(), 1,
      [this, life](uvcpp_write* wr, int status) {
        if (!token_alive(life)) {
          delete wr;  // 对象没了：只把请求对象还回去
          return;
        }
        if (status != 0) {
          last_error_code_ = status;
        }
        sync_write_result_ = status;
        sync_write_done_   = true;
        delete wr;  // also frees raw_buf->base and raw_buf (owned)
      });

  if (rc != 0) {
    last_error_code_ = rc;
    delete w;  // also frees raw_buf->base and raw_buf (owned)
    return rc;
  }

  bool completed = wait_for_condition(
      [this]() { return sync_write_done_; }, timeout_ms);

  if (!completed) {
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }

  return sync_write_result_;
}

// uvcpp_buf& overload (copy) — delegates to const char* version
int uvcpp_tcp_client::write(const uvcpp_buf& buf,
                             std::function<void(int)> cb) {
  return write(buf.get_const_data(), buf.size(), cb);
}

int uvcpp_tcp_client::write_wait(const uvcpp_buf& buf, int timeout_ms) {
  return write_wait(buf.get_const_data(), buf.size(), timeout_ms);
}

// uvcpp_buf* overload (zero-copy, transfers ownership of buffer data)
int uvcpp_tcp_client::write(uvcpp_buf* buf,
                             std::function<void(int)> cb) {
  if (buf == nullptr) return UV_EINVAL;
  if (!has_status(TCP_CLIENT_CONNECTED)) return UV_ENOTCONN;

  if (cb != nullptr) {
    // --- Async mode ---
    if (has_async_write_cb_) return UV_EALREADY;

    has_async_write_cb_ = true;
    write_fn_  = trampoline_write;
    write_arg_ = new std::function<void(int)>(cb);

    uv_buf_t* raw = buf->out_uv_buf();  // transfers ownership from buf

    uvcpp_write* w = new uvcpp_write();
    w->set_uv_buf(raw, true);

    std::shared_ptr<char> life = alive_token();

    int rc = tcp_->write(
        w, w->get_uv_buf(), 1,
        [this, life](uvcpp_write* wr, int status) {
          if (!token_alive(life)) {
            delete wr;  // 对象已析构：只把请求对象还回去
            return;
          }
          if (status != 0) last_error_code_ = status;
          // 次序与 const char* 重载一致：先存后清，且 `has_async_write_cb_` 必须在
          // 回调**之前**清 —— 否则回调里接着发起下一次写会拿到 UV_EALREADY。
          write_callback_t fn = write_fn_;
          void* arg = write_arg_;
          write_fn_  = nullptr;
          write_arg_ = nullptr;
          has_async_write_cb_ = false;
          if (fn) fn(status, arg);
          delete wr;
        });

    if (rc != 0) {
      last_error_code_ = rc;
      delete w;
      delete static_cast<std::function<void(int)>*>(write_arg_);
      write_fn_  = nullptr;
      write_arg_ = nullptr;
      has_async_write_cb_ = false;
      return rc;
    }
    return 0;
  } else {
    return write_wait(buf, 30000);
  }
}

int uvcpp_tcp_client::write_wait(uvcpp_buf* buf, int timeout_ms) {
  if (buf == nullptr) return UV_EINVAL;
  if (!has_status(TCP_CLIENT_CONNECTED)) return UV_ENOTCONN;
  if (has_async_write_cb_)
    throw std::runtime_error(
        "uvcpp_tcp_client::write_wait: cannot use sync write after "
        "async write callback was registered");

  sync_write_done_   = false;
  sync_write_result_ = 0;

  uv_buf_t* raw = buf->out_uv_buf();  // transfers ownership from buf

  uvcpp_write* w = new uvcpp_write();
  w->set_uv_buf(raw, true);

  std::shared_ptr<char> life = alive_token();

  int rc = tcp_->write(
      w, w->get_uv_buf(), 1,
      [this, life](uvcpp_write* wr, int status) {
        if (!token_alive(life)) {
          delete wr;  // 对象已析构：只把请求对象还回去
          return;
        }
        if (status != 0) last_error_code_ = status;
        sync_write_result_ = status;
        sync_write_done_   = true;
        delete wr;
      });

  if (rc != 0) {
    last_error_code_ = rc;
    delete w;
    return rc;
  }

  bool completed = wait_for_condition(
      [this]() { return sync_write_done_; }, timeout_ms);

  if (!completed) {
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }

  return sync_write_result_;
}

// =========================================================================
// Read
// =========================================================================

int uvcpp_tcp_client::arm_async_read() {
  ensure_read_cache();

  int rc = tcp_->read_start(
      internal_alloc_cb,
      [this](uvcpp_stream* /*s*/, ssize_t nread, const uv_buf_t* buf) {
        if (nread > 0) {
          set_status(TCP_CLIENT_READABLE);

#if UVCPP_OPENSSL_ENABLE
          // --- TLS 过滤层 ---
          //
          // 所有入站字节都从这一个口子进来（原始读路径与框架层读共用它），
          // 所以过滤器插在这里一处就够：socket 上的密文在这里被解成明文，
          // 下面两条交付路径收发的**已经是明文**。
          if (tls_ssl_ != nullptr) {
            const int trc = tls_feed(buf->base, static_cast<size_t>(nread));
            uvcpp_free_bytes(buf->base);
            if (trc != 0) {
              // tls_fail 已经通知过读侧和关闭回调了。
              // **此后 *this 可能已经不存在** —— 直接返回，不许再碰成员。
              return;
            }
            tls_flush_out();

            // 先交付数据，再补 connect 回调 —— 反过来的话，使用者在 connect
            // 回调里注册的读回调会错过这段明文（它会先被攒进 tls_plain_，
            // 注册时再补投，所以两种次序都对，但先交付更直接）。
            tls_deliver_plain();
            tls_complete_connect();

            // **握手结束通知必须是这个分支的最后一句**：服务端的 on_connection
            // 可能当场把这条连接关掉（鉴权不过要拒绝它），那之后 `*this` 就没了。
            if (tls_handshake_done_ && tls_ready_cb_) tls_notify_ready(0);
            return;
          }
#endif

          // 两条读路径互斥（read_start_events 里挡住混用），所以这里的分支
          // 不会两条都走。
          if (net_read_cb_) {
            // --- 框架层读 ---
            //
            // 直接把 libuv 的缓冲区**借**给回调，回调返回后再释放。
            // 原始路径为了交给用户一个 `uvcpp_buf*` 必须深拷贝一份，而那份
            // 拷贝在回调返回后随即销毁 —— 整包 malloc + memcpy 纯属白做。
            // 这里给的是 (const char*, size_t)，不需要 uvcpp_buf，所以省掉。
            net_read_result r;
            r.event = net_read_event::DATA;
            r.data  = buf->base;
            r.size  = static_cast<size_t>(nread);
            r.error = 0;

            // base 先存局部：回调里可能把这个 client 关掉/删掉，之后就再也
            // 不能碰成员了，但 libuv 的缓冲区仍然必须释放。
            char* base = buf->base;
            try {
              net_read_cb_(*this, r);
            } catch (const std::exception& e) {
              // 用户读回调抛异常不能把 libuv 的循环带崩。
              std::fprintf(stderr,
                           "[uvcpp_tcp_client] read callback threw: %s\n",
                           e.what());
            } catch (...) {
              std::fprintf(
                  stderr,
                  "[uvcpp_tcp_client] read callback threw (unknown)\n");
            }
            uvcpp_free_bytes(base);
          } else {
            // --- 原始读路径（保持不变）---
            uvcpp_buf tmp_buf;
            tmp_buf.clone_data(buf->base, static_cast<size_t>(nread));
            uvcpp_free_bytes(buf->base);

            if (read_fn_) {
              read_fn_(&tmp_buf, read_arg_);
            }
          }
        } else {
          if (nread < 0) {
            last_error_code_ = static_cast<int>(nread);
            if (nread != UV_EOF) {
              set_status(TCP_CLIENT_ERROR);
            }

#if UVCPP_OPENSSL_ENABLE
            // TLS 连接上"对端在握手期间就断了"是第一类常见失败（明文客户端
            // 打 TLS 服务端、非 TLS 对端接到 TLS 连接）。不在这里收尾的话，
            // 等握手的 connect 回调会永远不来。
            if (tls_ssl_ != nullptr && tls_connect_pending_) {
              tls_fail(static_cast<int>(nread), nread == UV_EOF);
              if (buf->base != nullptr) uvcpp_free_bytes(buf->base);
              return;  // tls_fail 可能已经删掉本对象
            }
#endif

            // **框架层读的通知排在关闭回调之前。**
            //
            // 顺序是有意的：框架的关闭回调会 delete 这个 client，而用户的读
            // 回调通常要读一下状态/对端信息（"是谁断的、什么错"）。先通知
            // 读，客户端还活着；再 fire_close_callbacks，让它按既定顺序收尾。
            if (net_read_cb_) {
              net_read_result r;
              r.event = (nread == UV_EOF) ? net_read_event::PEER_CLOSED
                                          : net_read_event::READ_ERROR;
              r.data  = nullptr;
              r.size  = 0;
              r.error = (nread == UV_EOF) ? 0 : static_cast<int>(nread);
              try {
                net_read_cb_(*this, r);
              } catch (const std::exception& e) {
                std::fprintf(stderr,
                             "[uvcpp_tcp_client] read callback threw: %s\n",
                             e.what());
              } catch (...) {
                std::fprintf(
                    stderr,
                    "[uvcpp_tcp_client] read callback threw (unknown)\n");
              }
            }

            // Notify close callbacks on connection close or error.
            // 走统一的触发函数：两处必须用完全一样的槽位处理
            // （见 fire_close_callbacks 里的置空说明）。
            fire_close_callbacks();
          }
          // Free the buffer even on error/EOF
          if (buf->base != nullptr) {
            uvcpp_free_bytes(buf->base);
          }
        }
      });

  // **`UV_EALREADY` 在这里不是失败，是"已经在读"。**
  //
  // libuv 对已经在读的 stream 再调一次 `uv_read_start` 就返回 `UV_EALREADY`，
  // 而 `uvcpp_stream::read_start` 是**先换回调、再调 libuv** 的
  // （`uvcpp_stream.cpp:28-34`）—— 所以拿到 `UV_EALREADY` 时，本次调用其实
  // **已经生效了**：它只是没有第二次 arm。原样把错误码往上传就是一个**假
  // 失败**，而假失败比真失败更坏：调用方没法把它和真失败区分开，只能选择
  // 忽略返回值 —— 忽略返回值正是这条路上"静默收不到数据"的来源。
  //
  // 这条路真的会走到，而且是最自然的那种用法：TLS 连接的握手阶段必须自己
  // arm 一次读（没人读就等不到 ServerHello，见 connect 完成回调里那句
  // `if (!read_started_) arm_async_read();`），于是使用者在 connect 回调里
  // 注册读回调时，底层已经在读了。仓库自己的 `web_ssl_client_func.cpp` 就是
  // 这么写的 —— 它没有检查返回值，所以一直没被发现。
  if (rc == UV_EALREADY) rc = 0;

  if (rc != 0) {
    last_error_code_ = rc;
    return rc;
  }

  read_started_ = true;
  set_status(TCP_CLIENT_READABLE);
  return 0;
}

int uvcpp_tcp_client::read_start(std::function<void(uvcpp_buf*)> cb) {
  // **使用者的显式读请求永远优先于框架的自动读。**
  //
  // 自动读是在服务端 accept 之后、用户 `on_connection` 回调**之前**装上的，
  // 所以用户在这个回调里照常 `read_start(...)` 就行 —— 这里先把自动读卸掉，
  // 再走正常的注册流程。两步都是在同一个 accept 回调里同步发生的，中间没有
  // 循环迭代，所以不会丢掉任何数据。
  if (auto_read_installed_) release_auto_read();

  if (cb != nullptr) {
    // --- Async mode ---
    if (has_async_read_cb_) {
      return UV_EALREADY;
    }

    has_async_read_cb_ = true;
    sync_read_wanted_  = false;  // 异步消费者优先，明文不该再往读缓存里搬

    read_fn_  = trampoline_read;
    read_arg_ = new std::function<void(uvcpp_buf*)>(cb);

    const int rc = arm_async_read();
#if UVCPP_OPENSSL_ENABLE
    // 握手期间到的明文一直攒在 tls_plain_ 里（那会儿还没有消费者）——
    // 现在有消费者了，补投给它。漏了这一步的表现是"服务端在握完手立刻发的
    // 数据不见了"，而 TLS 1.3 恰恰允许这样发。
    if (rc == 0 && tls_ssl_ != nullptr) tls_deliver_plain();
#endif
    return rc;
  } else {
    // --- Enable sync read cache ---
    if (has_async_read_cb_) {
      throw std::runtime_error(
          "uvcpp_tcp_client::read_start: cannot use sync read after "
          "async read callback was registered");
    }

    // 标记要在那个早返回**之前**置上：TLS 连接上 `read_started_` 早就为真
    // （`enable_tls` arm 的），于是这里会直接返回 0 —— 但"有人要同步读"这件
    // 事必须记下来，否则握手窗口里到的明文不知道该往哪儿放。
    sync_read_wanted_ = true;

    if (read_started_) {
      return 0;  // Already started
    }

    ensure_read_cache();

    int rc = tcp_->read_start(
        internal_alloc_cb,
        [this](uvcpp_stream* s, ssize_t nread, const uv_buf_t* buf) {
          on_internal_read(s, nread, buf);
        });

    if (rc != 0) {
      last_error_code_ = rc;
      return rc;
    }

    read_started_ = true;
    set_status(TCP_CLIENT_READABLE);
#if UVCPP_OPENSSL_ENABLE
    // 同上：把握手期间攒下的明文补进读缓存，否则 read_wait 永远等不到它。
    if (tls_ssl_ != nullptr) tls_deliver_plain();
#endif
    return 0;
  }
}

int uvcpp_tcp_client::read_wait(uvcpp_buf& out_buf, int timeout_ms) {
  if (has_async_read_cb_) {
    throw std::runtime_error(
        "uvcpp_tcp_client::read_wait: cannot use sync read after "
        "async read callback was registered");
  }

  // 有人在同步读 —— 置在 `read_start(nullptr)` **之前**：TLS 连接上
  // `read_started_` 已经为真，下面那句根本不会执行，而这一句才是可靠的标记。
  sync_read_wanted_ = true;

  // Ensure read is started
  if (!read_started_) {
    int rc = read_start(nullptr);
    if (rc != 0) {
      return rc;
    }
  }

  // Resume read if it was paused due to cache full
  if (read_cache_paused_) {
    ensure_read_cache();
    if (read_cache_->size() == 0) {
      int rc = tcp_->read_start(
          internal_alloc_cb,
          [this](uvcpp_stream* s, ssize_t nread, const uv_buf_t* buf) {
            on_internal_read(s, nread, buf);
          });
      if (rc != 0) {
        last_error_code_ = rc;
        return rc;
      }
      read_cache_paused_ = false;
      read_started_ = true;
    } else {
      read_cache_paused_ = false;
    }
  }

  // **窗口里攒下的明文必须在这里补投。**
  //
  // `read_start(nullptr)` 内部会调 `tls_deliver_plain()`，但它在 TLS 连接上
  // **根本不会执行** —— `enable_tls` 早就 arm 过读（没人读就等不到 ServerHello），
  // `read_started_` 为真，于是上面那句 `if (!read_started_)` 直接跳过，
  // `tls_plain_` 里的明文**没有任何人来取**，而本函数等的是 `read_cache_`，
  // 两者就此永远错开。
  //
  // 这个窗口真的会发生，而且是最自然的次序：`write_wait()` 自带泵循环，服务端
  // 的回声完全可能在它返回**之前**到达 —— 那一刻 `sync_read_wanted_` 还是假，
  // 明文按设计留在 `tls_plain_` 里等消费者。消费者（本函数）进来后若不补投，
  // 就只剩"等下一个数据块顺带带出去"这一条路；回声是唯一一块数据时，等到的
  // 就是超时。实测（探针，确定性复现）：
  //
  //     read_started=1 sync_read_wanted=1 tls_plain=11 cache=0
  //     [FAIL] sync_roundtrip: read_wait = -4039     ← UV_ETIMEDOUT
  //
  // 11 就是那声回声的长度 —— 数据一直在，只是没人搬。
  //
  // 放在 `sync_read_wanted_ = true` 之后：`tls_deliver_plain()` 靠这个标记
  // 决定"进读缓存"还是"继续留着等真正的消费者"。
#if UVCPP_OPENSSL_ENABLE
  if (tls_ssl_ != nullptr) tls_deliver_plain();
#endif

  // Wait for data in the cache
  bool has_data = wait_for_condition(
      [this]() {
        return read_cache_ != nullptr && read_cache_->size() > 0;
      },
      timeout_ms);

  if (!has_data) {
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }

  // Move cached data into out_buf
  out_buf.move_buf(*read_cache_);

  // Check if cache should be paused (will stop on next overflow)
  // But actually we check after appending, so just ensure cache is clean
  if (read_cache_ != nullptr && read_cache_->size() > max_read_cache_size_) {
    tcp_->read_stop();
    read_cache_paused_ = true;
  }

  return 0;
}

int uvcpp_tcp_client::read_start_events(const uvcpp_net_read_cb& cb) {
  if (cb == nullptr) {
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }

  // 同 read_start：使用者的注册顶掉框架的自动读。而且要排在下面那个
  // "已注册"判断之前，否则用户会拿到一个莫名其妙的 UV_EALREADY。
  if (auto_read_installed_) release_auto_read();

  // 和原始读路径互斥。
  //
  // 两条路共用同一个底层 stream，同时注册的话底层 `read_start` 会把先注册的
  // 那个覆盖掉 —— 用户会以为两个回调都在收数据，实际上只有一个。这里挡在
  // 前面，不让那种事静默发生。
  if (read_fn_ != nullptr || has_async_read_cb_) {
    last_error_code_ = UV_EALREADY;
    return UV_EALREADY;
  }

  net_read_cb_ = cb;
  // 置上这个标志有两个作用：一是让同步读（read_start(nullptr)/read_wait）
  // 被挡住，二是让 read_resume() 知道该重新 arm 而不是当作缓存模式返回 0。
  has_async_read_cb_ = true;
  sync_read_wanted_  = false;  // 异步消费者优先，明文不该再往读缓存里搬
  net_read_paused_   = false;

  const int rc = arm_async_read();
#if UVCPP_OPENSSL_ENABLE
  if (rc == 0 && tls_ssl_ != nullptr) tls_deliver_plain();
#endif
  return rc;
}

int uvcpp_tcp_client::read_stop() {
  clear_status(TCP_CLIENT_READABLE);
  read_started_ = false;
  has_async_read_cb_ = false;
  sync_read_wanted_ = false;  // 停读就是两种模式一起停
  net_read_cb_ = nullptr;  // 框架层读的槽也要清，否则下次 arm 会重复投递
  net_read_paused_ = false;
  // 停读就是停读：自动读也一起没了。用户显式停掉之后不该被框架偷偷恢复。
  auto_read_installed_ = false;
  if (read_arg_ != nullptr) {
    delete static_cast<std::function<void(uvcpp_buf*)>*>(read_arg_);
    read_arg_ = nullptr;
  }
  read_fn_ = nullptr;
  return tcp_->read_stop();
}

int uvcpp_tcp_client::read_pause() {
  clear_status(TCP_CLIENT_READABLE);
  read_started_ = false;
  // 注意：**不清回调**。暂停只是暂时不读，恢复时要接着往同一个回调投递。
  net_read_paused_ = true;
  return tcp_->read_stop();
}

int uvcpp_tcp_client::read_resume() {
  if (!has_async_read_cb_) return 0;  // sync/cache mode: nothing to re-arm
  if (read_started_) return 0;        // already reading
  net_read_paused_ = false;
  const int rc = arm_async_read();
#if UVCPP_OPENSSL_ENABLE
  // 暂停期间也可能有明文被解出来攒着（暂停只挡住 socket 读，挡不住已经
  // 在 rbio 里的字节），恢复时一并补投。
  if (rc == 0 && tls_ssl_ != nullptr) tls_deliver_plain();
#endif
  return rc;
}

void uvcpp_tcp_client::install_auto_read() {
  // 已经有读回调了就不动 —— 自动读只是兜底，不该顶掉任何真实消费者。
  if (read_fn_ != nullptr || has_async_read_cb_) return;

  auto_read_installed_ = true;
  auto_read_warned_    = false;

  // 捕获 this 是安全的：这个 std::function 是成员，生命周期跟着客户端走。
  net_read_cb_ = [this](uvcpp_tcp_client& c, const net_read_result& r) {
    if (!r.is_data()) {
      // PEER_CLOSED / READ_ERROR：不用在这里做什么，框架的关闭回调会收尾。
      return;
    }
    if (auto_read_warned_) return;
    auto_read_warned_ = true;

    // 只在**真的丢了数据**的时候才警告，而且每个连接只警告一次。
    //
    // 这条警告是有用的："对端明明发了数据我这边没反应"以前是无声的 ——
    // 现在至少有一行输出指向那个连接。同时它也不会像"accept 就警告"那样
    // 在每一连接上刷屏（自动读是默认行为，那会是噪音）。
    std::fprintf(stderr,
                 "[uvcpp_tcp_client] Warning: client %p received %lu byte(s) "
                 "with no read callback registered — data is being DISCARDED. "
                 "Register one with read_start()/read_start_events() in the "
                 "server's on_connection callback.\n",
                 static_cast<void*>(&c), static_cast<unsigned long>(r.size));
  };

  has_async_read_cb_ = true;
  net_read_paused_   = false;
  const int rc = arm_async_read();
  if (rc != 0) {
    // 装不上就退回去，别留一个"看起来在自动读、其实没有"的状态。
    net_read_cb_ = nullptr;
    has_async_read_cb_ = false;
    auto_read_installed_ = false;
  }
}

void uvcpp_tcp_client::release_auto_read() {
  if (!auto_read_installed_) return;

  auto_read_installed_ = false;
  auto_read_warned_    = false;
  net_read_cb_         = nullptr;
  net_read_paused_     = false;
  has_async_read_cb_   = false;
  read_started_        = false;

  if (tcp_ != nullptr) {
    tcp_->read_stop();
  }
  clear_status(TCP_CLIENT_READABLE);
}

void uvcpp_tcp_client::set_on_close(std::function<void()> cb) {
  if (close_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_arg_);
    close_arg_ = nullptr;
  }
  close_fn_  = trampoline_close;
  close_arg_ = new std::function<void()>(cb);
}

void uvcpp_tcp_client::set_close_manager(std::function<void()> cb) {
  if (close_mgr_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_mgr_arg_);
    close_mgr_arg_ = nullptr;
  }
  close_mgr_fn_  = trampoline_close;
  close_mgr_arg_ = new std::function<void()>(cb);
}

void uvcpp_tcp_client::clear_close_manager() {
  if (close_mgr_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_mgr_arg_);
    close_mgr_arg_ = nullptr;
  }
  close_mgr_fn_ = nullptr;
}

bool uvcpp_tcp_client::has_close_manager() const {
  return close_mgr_fn_ != nullptr;
}

// =========================================================================
// 关闭观察者（加法式，见头文件）
// =========================================================================

int uvcpp_tcp_client::add_close_observer(std::function<void()> cb) {
  if (!cb) return 0;
  const int id = next_close_observer_id_++;
  close_observers_.push_back(std::make_pair(id, std::move(cb)));
  return id;
}

void uvcpp_tcp_client::remove_close_observer(int id) {
  if (id == 0) return;
  for (size_t i = 0; i < close_observers_.size(); ++i) {
    if (close_observers_[i].first == id) {
      close_observers_.erase(close_observers_.begin() + static_cast<long>(i));
      return;
    }
  }
}

size_t uvcpp_tcp_client::close_observer_count() const {
  return close_observers_.size();
}

// =========================================================================
// Close
// =========================================================================

int uvcpp_tcp_client::close(const std::function<void()>& after_close) {
  // **先看 get_handle() 再看 is_closing()，顺序不能反。**
  //
  // 句柄关完之后（`uvcpp_handle::callback_close` 里）包装对象的 `_handle`
  // 被置成 nullptr，而 `uvcpp_handle::is_closing()` 是把这个指针直接交给
  // libuv 的 `uv_is_closing()` —— 它解引用。所以对一个已经关掉的连接调
  // `close()`，先问 `is_closing()` 就是空指针解引用（段错误）。
  if (tcp_ == nullptr || tcp_->get_handle() == nullptr) {
    // 句柄都没了，不可能再有关闭事件。调用方的收尾照跑一次 —— 不然它会
    // 以为收尾还欠着（比如 http 层的连接上下文永远不摘）。
    if (after_close) after_close();
    return UV_EINVAL;
  }

  if (tcp_->is_closing()) {
    // 已经有人在关了。此刻**绝不能** fire_close_callbacks()：
    //   1. 我们这次的完成回调不会来（uv_close 只认第一次调用）；
    //   2. 关闭事件还飞在队列里，而 fire 里的 close manager 会 delete 本
    //      对象，那个完成回调随后就会踩到已经释放的句柄包装对象。
    // 只把调用方自己的收尾跑掉（幂等），关闭的收尾交给先发起的那一次。
    if (after_close) after_close();
    return 0;
  }

  set_status(TCP_CLIENT_CLOSING);

  // 顺序是有意的：
  //   1. after_close —— 调用方的收尾，此时客户端还活着（能读状态/对端信息）；
  //   2. fire_close_callbacks() —— 用户回调 + 框架的 close manager，后者可能
  //      `delete this`，所以它必须是最后一句，之后不能再碰任何成员。
  tcp_->close([this, after_close](uvcpp_handle*) {
    set_status(TCP_CLIENT_CLOSED);

    if (after_close) {
      try {
        after_close();
      } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "[uvcpp_tcp_client] close completion threw: %s\n",
                     e.what());
      } catch (...) {
        std::fprintf(stderr,
                     "[uvcpp_tcp_client] close completion threw (unknown)\n");
      }
    }

    fire_close_callbacks();  // 此后 *this 可能已经不存在了
  });

  return 0;
}

void uvcpp_tcp_client::fire_close_callbacks() {
  // **必须在调用前把两个槽都置空。**
  //
  // 框架的管理回调会 delete 本客户端，而 ~uvcpp_tcp_client 会释放
  // close_arg_ 指向的那个 std::function；trampoline_close 返回之后还要再
  // delete 它一次 —— 原来的代码就是这样每次连接关闭都 double free 一次。
  // 置空之后析构函数不再碰它，拥有权临时转移到 trampoline 手里，
  // trampoline 那一次 delete 就成了唯一的一次。
  close_callback_t  ufn  = close_fn_;
  void*             uarg = close_arg_;
  close_fn_  = nullptr;
  close_arg_ = nullptr;

  close_callback_t  mfn  = close_mgr_fn_;
  void*             marg = close_mgr_arg_;
  close_mgr_fn_  = nullptr;
  close_mgr_arg_ = nullptr;

  // 观察者同样**先整体取出来**再跑：某个观察者可能又装/摘观察者，边遍历边改
  // 会出事。取出来之后新装的落在（已空的）close_observers_ 里，不会被本次跑到
  // —— 它是在"关闭"发生之后才挂上去的，本来也不该收到这一次。
  std::vector<std::pair<int, std::function<void()> > > obs;
  obs.swap(close_observers_);

  // 顺序是有意的：用户回调先跑，此时客户端还活着（能读状态、对端信息）；
  // 观察者次之（同样还活着，但连接已经关了，不要写）；框架回调最后跑，
  // 它可能会把这个对象删掉。
  if (ufn != nullptr) {
    ufn(uarg);
  }
  for (size_t i = 0; i < obs.size(); ++i) {
    if (obs[i].second) obs[i].second();
  }
  if (mfn != nullptr) {
    mfn(marg);  // 此后 *this 可能已经不存在了 —— 不能再碰任何成员
  }
}

void uvcpp_tcp_client::clear_on_close() {
  if (close_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_arg_);
    close_arg_ = nullptr;
  }
  close_fn_ = nullptr;
}

// =========================================================================
// TLS 过滤层
// =========================================================================
//
// 全部形状由一条约束决定：**socket 归 libuv，SSL 只跟内存打交道**。
// 于是本层是纯转换器 —— 出方向「明文 → SSL_write → wbio → 抽出来 → uv_write」，
// 入方向「uv_read → 喂 rbio → SSL_read → 明文 → 交给上层」。没有任何一处
// 需要等 socket、转圈、或者 uv_poll。
//
// 用户回调（读回调 / connect 回调 / close 回调）随时可能 `delete` 本对象 ——
// 这是本文件里反复出现的约束。所以每个会进用户代码的函数都在进去之前把要用的
// 东西取成局部量，进去之后用存活令牌判一次，确认对象还在才继续碰成员。

#if UVCPP_OPENSSL_ENABLE

int uvcpp_tcp_client::enable_tls(uvcpp_ssl_context* ctx) {
  if (tls_ssl_ != nullptr) return UV_EALREADY;
  if (ctx == nullptr || !ctx->is_ready()) return UV_EINVAL;

  // fd=0：**不装 socket BIO**。装了就回到 SSL_set_fd 那条死路上了。
  tls_ssl_ = new uvcpp_ssl(ctx, 0);
  if (tls_ssl_->raw_ssl() == nullptr) {
    delete tls_ssl_;
    tls_ssl_ = nullptr;
    return UV_ENOMEM;
  }
  if (!tls_ssl_->use_memory_bio()) {
    delete tls_ssl_;
    tls_ssl_ = nullptr;
    return UV_ENOMEM;
  }

  tls_handshake_done_  = false;
  tls_ssl_error_       = 0;
  tls_connect_pending_ = false;

  // 连接已经建立（服务端 accept 之后调用的情形）：握手现在就开始。
  // 客户端此时还没连上，起手是空的 —— 等 connect 完成回调进来再发 ClientHello。
  if (has_status(TCP_CLIENT_CONNECTED)) {
    if (!read_started_) {
      arm_async_read();  // 握手要靠读事件推进，没人读就永远握不上
    }
    tls_drive_handshake();
    tls_flush_out();
    if (tls_handshake_done_) tls_deliver_plain();
    // 服务端在 accept 路径上同步就把握手走完（理论上的可能）时也要通知，
    // 但那一步必须留在最后：通知的回调可能把这条连接释放掉。
    if (tls_handshake_done_) {
      tls_notify_ready(0);
      return 0;
    }
  }
  return 0;
}

void uvcpp_tcp_client::set_tls_ready_callback(
    std::function<void(uvcpp_tcp_client*, int)> cb) {
  tls_ready_cb_ = cb;
}

int uvcpp_tcp_client::tls_last_ssl_error() const { return tls_ssl_error_; }

int uvcpp_tcp_client::tls_drive_handshake() {
  if (tls_ssl_ == nullptr || tls_handshake_done_) return 0;

  const int rc = tls_ssl_->handshake();
  if (rc < 0) {
    // 真出错（不是 WANT_READ/WANT_WRITE —— 那两个在本层被归一化成 0）。
    // 证书校验失败、协议版本不匹配、对端根本不是 TLS 服务端都在这里。
    tls_ssl_error_ = tls_ssl_->last_ssl_error();
    tls_fail(UV_EPROTO, false);
    return -1;
  }
  if (rc == 1) {
    tls_handshake_done_ = true;
    tls_ssl_error_      = tls_ssl_->last_ssl_error();
  }
  return 0;
}

void uvcpp_tcp_client::tls_flush_out() {
  if (tls_ssl_ == nullptr) return;

  // wbio 里新产生的密文全部抽干。memory BIO 无界，所以这一步不会失败。
  char buf[16384];
  size_t n;
  while ((n = tls_ssl_->take_ciphertext(buf, sizeof(buf))) > 0) {
    tls_out_.append(buf, n);
  }

  if (tls_out_.empty() || tls_out_busy_) return;
  if (tcp_ == nullptr || !has_status(TCP_CLIENT_CONNECTED)) return;

  // 同一时刻只投一个密文写。libuv 本身允许多个 uv_write 排队，但"一次一个"
  // 让完成回调的语义变成确定的：回调到了 = 这一批密文已经出网。
  std::string chunk;
  chunk.swap(tls_out_);

  uvcpp_buf bufcpp(chunk.data(), chunk.size());
  uv_buf_t* raw = bufcpp.out_uv_buf();  // 数据所有权转移给 raw

  uvcpp_write* w = new uvcpp_write();
  w->set_uv_buf(raw, true);

  tls_out_busy_ = true;

  std::shared_ptr<char> life = alive_token();
  int rc = tcp_->write(
      w, w->get_uv_buf(), 1,
      [this, life](uvcpp_write* wr, int status) {
        if (!token_alive(life)) {
          delete wr;  // 对象没了：只把请求对象还回去
          return;
        }
        tls_out_busy_ = false;
        if (status != 0) {
          last_error_code_ = status;
          delete wr;
          tls_fail(status, false);
          return;
        }
        delete wr;
        // wbio 里可能还有（比如握手过程中又产生了 Finished）。
        tls_flush_out();
        if (!token_alive(life)) return;
        // 只有在**密文确实全部出网**时才会真正收尾（见 tls_finish_write）。
        tls_finish_write(0);
      });

  if (rc != 0) {
    tls_out_busy_ = false;
    delete w;
    last_error_code_ = rc;
    tls_fail(rc, false);
  }
}

int uvcpp_tcp_client::tls_write_plain(const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int rc = tls_ssl_->write(data + sent, len - sent);
    if (rc > 0) {
      sent += static_cast<size_t>(rc);
      continue;
    }
    tls_ssl_error_ = tls_ssl_->last_ssl_error();
    if (rc == 0) {
      // WANT_READ / WANT_WRITE。memory BIO 下 wbio 无界，所以「因为发不出去
      // 而写不动」不可能发生；剩下的只是对端要做密钥更新。那需要等更多入站
      // 数据，而这里的调用栈是应用层发起的，等不了。
      //
      // TLS 1.3 没有重协商，正常会话走不到这里。真的走到就如实报错 ——
      // 报错好过"以为发出去了其实没有"。
      return UV_EIO;
    }
    return UV_EPROTO;
  }
  return 0;
}

int uvcpp_tcp_client::tls_feed(const char* data, size_t len) {
  if (tls_ssl_ == nullptr || data == nullptr || len == 0) return 0;

  size_t off = 0;
  while (off < len) {
    const size_t fed = tls_ssl_->feed_ciphertext(data + off, len - off);
    if (fed == 0) break;
    off += fed;

    if (!tls_handshake_done_) {
      if (tls_drive_handshake() < 0) return -1;
    }
    if (!tls_handshake_done_) continue;

    // 握手完了就尽量把明文解出来。SSL_read 一次不一定给全，读到 0 为止。
    char buf[16384];
    for (;;) {
      const int rn = tls_ssl_->read(buf, sizeof(buf));
      if (rn > 0) {
        tls_plain_.append(buf, static_cast<size_t>(rn));
        continue;
      }
      tls_ssl_error_ = tls_ssl_->last_ssl_error();
      if (rn == 0 && tls_ssl_error_ == SSL_ERROR_ZERO_RETURN) {
        // 对端发了 close_notify —— 干净的 TLS 关闭
        tls_fail(0, true);
        return -1;
      }
      if (rn < 0) {
        tls_fail(UV_EPROTO, false);
        return -1;
      }
      break;  // WANT_READ：等下一次可读
    }
  }
  return 0;
}

void uvcpp_tcp_client::tls_deliver_plain() {
  if (tls_plain_.empty()) return;

  // **自动读不是消费者。**
  //
  // 它是传输层的兜底，唯一职责是"发现对端断开"（见 install_auto_read 的注释），
  // 收到数据只会打一行 DISCARDED 警告然后扔掉。但在**服务端**它恰好是握手期间
  // 唯一装在 `net_read_cb_` 上的东西（`setup_client_callbacks` 在 accept 时就
  // 装了），而那时连接还没交给上层 —— 于是与 Finished 同一段到达的应用数据被
  // 投给兜底、被丢掉，上层随后注册的读回调再也等不到它（`tls_plain_` 已经空了）。
  //
  // 留在 `tls_plain_` 里，等真正的消费者注册时补投 —— 每个注册路径都会补
  // （`read_start(cb)`、`read_start(nullptr)`、`read_start_events`、
  // `read_resume`），所以留着不会丢。
  if (auto_read_installed_) return;

  // 还没有消费者：**先留着，不能丢**。
  // TLS 1.3 的服务端可以在握完手立刻发应用数据，而客户端的使用者要等
  // connect 回调才有机会注册读回调 —— 中间这段窗口里到的明文必须攒着。
  if (!net_read_cb_ && read_fn_ == nullptr) {
    // 真的有人在同步读时才进读缓存（`read_wait` 就盯着 `read_cache_`）。
    //
    // 这里的判据**不能**是「`read_started_ && !has_async_read_cb_`」，那看着像
    // 同步模式，实际在 TLS 握手窗口里恰好为真（`enable_tls` 自己 arm 了读、
    // 而消费者还没注册）—— 明文会被搬进读缓存，随后注册的异步消费者只认
    // `tls_plain_`，**永远读不到它**。
    if (sync_read_wanted_ && !has_async_read_cb_) {
      ensure_read_cache();
      read_cache_->append_data(tls_plain_.data(), tls_plain_.size());
      tls_plain_.clear();
    }
    return;
  }

  std::string out;
  out.swap(tls_plain_);

  if (net_read_cb_) {
    net_read_result r;
    r.event = net_read_event::DATA;
    r.data  = out.data();
    r.size  = out.size();
    r.error = 0;
    // 把 std::function 拷到局部再调：回调里可能把这个客户端关掉/删掉，
    // 那样成员 `net_read_cb_` 的存储会在 operator() 执行到一半时消失。
    uvcpp_net_read_cb cb = net_read_cb_;
    try {
      cb(*this, r);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[uvcpp_tcp_client] read callback threw: %s\n",
                   e.what());
    } catch (...) {
      std::fprintf(stderr, "[uvcpp_tcp_client] read callback threw (unknown)\n");
    }
    return;
  }

  // 原始读路径：fn/arg 是 C 风格函数指针 + void*，取到局部即可。
  read_callback_t fn  = read_fn_;
  void*           arg = read_arg_;
  if (fn == nullptr) return;

  uvcpp_buf tmp;
  tmp.clone_data(out.data(), out.size());
  fn(&tmp, arg);
}

void uvcpp_tcp_client::tls_complete_connect() {
  if (!tls_connect_pending_) return;
  tls_connect_pending_ = false;

  connect_callback_t fn  = connect_fn_;
  void*              arg = connect_arg_;
  connect_fn_  = nullptr;
  connect_arg_ = nullptr;
  has_async_connect_cb_ = false;
  if (fn != nullptr) fn(0, arg);
}

void uvcpp_tcp_client::tls_notify_ready(int status) {
  if (!tls_ready_cb_) return;

  // 先拷到局部、再清成员 —— 与写/连接完成回调同一约定：回调里最自然的事
  // 就是发起下一次操作，成员还立着会被误读成"还有一次待通知"。
  std::function<void(uvcpp_tcp_client*, int)> cb = tls_ready_cb_;
  tls_ready_cb_ = nullptr;

  // **这是函数里唯一一处触碰 `*this` 之外的东西，之后一律不许再碰成员** ——
  // 回调可能把这条连接关掉并释放掉（服务端拒绝一条握手完成的连接就是常规
  // 用法）。调用点也必须保证这一句是所在回调的最后一句。
  cb(this, status);
}

void uvcpp_tcp_client::tls_finish_write(int status) {
  if (!tls_write_pending_) return;
  // 密文还在路上就先不收尾 —— 否则调用方以为发出去了，实际还在 wbio 里。
  if (status == 0 && (tls_out_busy_ || !tls_out_.empty())) return;

  tls_write_pending_ = false;

  if (tls_write_sync_) {
    tls_write_sync_    = false;
    sync_write_result_ = status;
    sync_write_done_   = true;
    return;
  }

  // 异步：先清成员**再**回调，次序与明文路径完全一致（回调里最自然的事就是
  // 接着发起下一次写，标志还立着会让那次写拿到 UV_EALREADY）。
  if (write_fn_ != nullptr) {
    write_callback_t fn  = write_fn_;
    void*            arg = write_arg_;
    write_fn_  = nullptr;
    write_arg_ = nullptr;
    has_async_write_cb_ = false;
    fn(status, arg);
  } else {
    has_async_write_cb_ = false;
  }
}

void uvcpp_tcp_client::tls_fail(int err, bool peer_closed) {
  const int code = (err != 0) ? err : (peer_closed ? UV_EOF : UV_EPROTO);
  if (err != 0) {
    last_error_code_ = err;
    set_status(TCP_CLIENT_ERROR);
  }

  // 1. 在等握手的 connect 回调 —— 使用者拿到的是"连接失败"，不是成功。
  if (tls_connect_pending_) {
    tls_connect_pending_ = false;
    if (tls_write_pending_) { tls_write_pending_ = false; tls_write_sync_ = false; }
    connect_callback_t fn  = connect_fn_;
    void*              arg = connect_arg_;
    connect_fn_  = nullptr;
    connect_arg_ = nullptr;
    has_async_connect_cb_ = false;
    if (fn != nullptr) fn(code, arg);
    return;
  }

  // 2. 服务器侧：握手失败必须"通知一次，且绝不把连接交给上层"。
  //
  // 这一步只做通知，**不在这里关连接** —— 上面 connect 分支之外的收尾在下面
  // 第 3/4/5 步里走完（读侧通知 → fire_close_callbacks → 服务端的 close
  // manager 摘除并释放），与明文路径遇到读错误时完全同一条路。服务端的
  // ready 回调拿到非 0 状态时只需记账，不需要自己动手收尾。
  if (tls_ready_cb_) tls_notify_ready(code);

  // 3. 在等的写：让它以错误收尾，而不是永远等下去。
  if (tls_write_pending_) tls_finish_write(code);

  // 4. 通知读侧。框架层读排在关闭回调之前（与明文路径同一约定）：
  //    关闭回调可能 delete 本对象，而用户读回调通常要读一下状态。
  if (net_read_cb_) {
    net_read_result r;
    r.event = peer_closed ? net_read_event::PEER_CLOSED
                          : net_read_event::READ_ERROR;
    r.data  = nullptr;
    r.size  = 0;
    r.error = peer_closed ? 0 : code;
    uvcpp_net_read_cb cb = net_read_cb_;
    try {
      cb(*this, r);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[uvcpp_tcp_client] read callback threw: %s\n",
                   e.what());
    } catch (...) {
      std::fprintf(stderr, "[uvcpp_tcp_client] read callback threw (unknown)\n");
    }
  }

  // 5. 此后 *this 可能已经不存在 —— 不能再碰任何成员。
  fire_close_callbacks();
}

#endif  // UVCPP_OPENSSL_ENABLE

// =========================================================================
// Loop control
// =========================================================================

int uvcpp_tcp_client::run(uv_run_mode md) {
  return loop_->run(md);
}

void uvcpp_tcp_client::stop() {
  loop_->stop();
}

// =========================================================================
// Internal helpers
// =========================================================================

void uvcpp_tcp_client::set_status(int flags) {
  status_ |= flags;
}

void uvcpp_tcp_client::clear_status(int flags) {
  status_ &= ~flags;
}

void uvcpp_tcp_client::reset_status() {
  status_ = TCP_CLIENT_NONE;
}

bool uvcpp_tcp_client::wait_for_condition(
    std::function<bool()> condition, int timeout_ms) {
  auto start = std::chrono::steady_clock::now();

  while (true) {
    if (condition()) {
      return true;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    if (elapsed >= timeout_ms) {
      return false;
    }

    // Pump the event loop to process pending callbacks
    // UV_RUN_NOWAIT returns immediately if nothing is ready
    if (loop_ != nullptr) {
      loop_->run(UV_RUN_NOWAIT);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void uvcpp_tcp_client::internal_alloc_cb(uvcpp_handle* /*h*/, size_t sz,
                                          uv_buf_t* buf) {
  uvcpp_buf::alloc_buf(buf, sz > 0 ? sz : 4096);
}

void uvcpp_tcp_client::on_internal_read(uvcpp_stream* /*s*/, ssize_t nread,
                                         const uv_buf_t* buf) {
  if (nread > 0) {
    set_status(TCP_CLIENT_READABLE);

#if UVCPP_OPENSSL_ENABLE
    // 同步读路径（read_start(nullptr) / read_wait）也必须过 TLS 过滤器。
    // 漏掉这里的后果不是"少个功能"，而是**把密文当明文交给使用者** ——
    // 一个静默的错答案，比报错难查得多。
    if (tls_ssl_ != nullptr) {
      const int trc = tls_feed(buf->base, static_cast<size_t>(nread));
      uvcpp_free_bytes(buf->base);
      if (trc != 0) return;  // tls_fail 已经通知过；此后不许再碰成员
      tls_flush_out();
      tls_deliver_plain();  // 同步模式下会攒进 read_cache_
      // 与框架层读路径同一约定：通知留在最后，回调可能释放本对象。
      if (tls_handshake_done_ && tls_ready_cb_) tls_notify_ready(0);
      return;
    }
#endif

    ensure_read_cache();

    // Append data to the read cache
    read_cache_->append_data(buf->base, static_cast<size_t>(nread));
    uvcpp_free_bytes(buf->base);

    // Check if cache exceeds limit — pause reading
    if (read_cache_->size() > max_read_cache_size_) {
      tcp_->read_stop();
      read_cache_paused_ = true;
    }
  } else if (nread < 0) {
    if (nread != UV_EOF) {
      last_error_code_ = static_cast<int>(nread);
      set_status(TCP_CLIENT_ERROR);
    } else {
      // UV_EOF: peer closed gracefully
      set_status(TCP_CLIENT_CLOSED);
    }
    // Notify close callbacks on connection close or error.
    fire_close_callbacks();
    if (buf->base != nullptr) {
      uvcpp_free_bytes(buf->base);
    }
  } else {
    // nread == 0: no data, but EAGAIN/EWOULDBLOCK — nothing to do
    if (buf->base != nullptr) {
      uvcpp_free_bytes(buf->base);
    }
  }
}

}  // namespace uvcpp
