/**
 * @file src/net/uvcpp_tcp_server.cpp
 * @brief Implementation of uvcpp_tcp_server.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <net/uvcpp_tcp_server.h>
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_define.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_context.h>
#endif

namespace uvcpp {

// =========================================================================
// Trampoline — bridge C-style callback to std::function stored on heap
// =========================================================================

static void trampoline_connection(uvcpp_tcp_client* client, void* arg) {
  auto* cb =
      static_cast<std::function<void(uvcpp_tcp_client*)>*>(arg);
  (*cb)(client);
  // cb is NOT deleted — persists for multiple connections
}

// =========================================================================
// Construction / Destruction
// =========================================================================

uvcpp_tcp_server::uvcpp_tcp_server() {
  loop_ = new uvcpp_loop();  // constructor already calls init()
  tcp_  = new uvcpp_tcp(loop_);
  status_ = TCP_SERVER_NONE;
}

uvcpp_tcp_server::~uvcpp_tcp_server() {
  // Close the server TCP handle, pumping the loop until its close
  // endgame fires.  Then pump a few more times to drain any remaining
  // client close endgames that may have been queued during the pump.
  if (tcp_ != nullptr && !stopped_) {
    if (!tcp_->is_closing() && tcp_->is_active()) {
      bool close_done = false;
      tcp_->close([&close_done](uvcpp_handle*) { close_done = true; });
      for (int i = 0; i < 5000 && !close_done; i++) {
        if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      // Drain any remaining close endgames (e.g. from clients whose
      // close callbacks fired during the pump above).
      for (int i = 0; i < 20; i++) {
        if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }

  // Close and delete the loop BEFORE deleting any wrapper objects.
  // Pumping above has processed all close endgames, so the loop's
  // handle queue should be empty.
  if (loop_ != nullptr) {
    loop_->loop_close();
    delete loop_;
    loop_ = nullptr;
  }

  // Now safe to delete all wrappers — their handles are fully closed
  // and the loop is already destroyed.
  if (tcp_ != nullptr) {
    delete tcp_;
    tcp_ = nullptr;
  }

  for (auto* client : clients_) {
    delete client;
  }
  clients_.clear();

  if (on_connection_arg_ != nullptr) {
    delete static_cast<std::function<void(uvcpp_tcp_client*)>*>(
        on_connection_arg_);
    on_connection_arg_ = nullptr;
  }
}

// =========================================================================
// Accessors
// =========================================================================

uvcpp_tcp* uvcpp_tcp_server::get_tcp() {
  return tcp_;
}

uvcpp_loop* uvcpp_tcp_server::get_loop() {
  return loop_;
}

int uvcpp_tcp_server::get_status() const {
  return status_;
}

bool uvcpp_tcp_server::has_status(int flags) const {
  return (status_ & flags) == flags;
}

int uvcpp_tcp_server::get_last_error() const {
  return last_error_code_;
}

// =========================================================================
// Bind
// =========================================================================

int uvcpp_tcp_server::bind(const char* ip, int port) {
  // Auto-detect IPv4 vs IPv6: if the IP string contains ':', it's IPv6
  if (std::strchr(ip, ':') != nullptr) {
    return bindIpv6(ip, port);
  }
  return bindIpv4(ip, port);
}

int uvcpp_tcp_server::bindIpv4(const char* ip, int port) {
  int rc = tcp_->bindIpv4(ip, port);
  if (rc != 0) {
    last_error_code_ = rc;
    set_status(TCP_SERVER_ERROR);
    return rc;
  }
  set_status(TCP_SERVER_LISTENING);
  return 0;
}

int uvcpp_tcp_server::bindIpv6(const char* ip, int port) {
  int rc = tcp_->bindIpv6(ip, port);
  if (rc != 0) {
    last_error_code_ = rc;
    set_status(TCP_SERVER_ERROR);
    return rc;
  }
  set_status(TCP_SERVER_LISTENING);
  return 0;
}

// =========================================================================
// Listen
// =========================================================================

int uvcpp_tcp_server::listen(
    std::function<void(uvcpp_tcp_client*)> connection_cb, int backlog) {
  if (connection_cb == nullptr) {
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }

  // Store the user callback via trampoline to avoid std::function
  // copy-chain corruption through libuv's callback layers.
  on_connection_fn_  = trampoline_connection;
  on_connection_arg_ =
      new std::function<void(uvcpp_tcp_client*)>(connection_cb);

  int rc = tcp_->listen(
      [this](uvcpp_stream* s, int status) {
        if (status < 0) {
          last_error_code_ = status;
          set_status(TCP_SERVER_ERROR);
          return;
        }

        // Create a new client sharing the server's loop
        uvcpp_tcp_client* client = new uvcpp_tcp_client(loop_);

        // Accept the pending connection into the client's TCP handle
        int accept_rc = s->accept(client->get_tcp());
        if (accept_rc != 0) {
          last_error_code_ = accept_rc;
          delete client;
          return;
        }

        // Mark the client as connected — sets CONNECTED | READABLE |
        // WRITABLE so that write() and read_start() work correctly.
        client->mark_accepted();

        // **登记 + 装管理回调要排在用户回调之前。**
        //
        // 原来是反过来的，于是用户在 on_connection 里立刻关掉这个连接
        // （比如鉴权不过要拒绝它）时：关事件先于 push_back 发生，客户端
        // 要么没被登记、要么登记了一个已经关闭的；用户回调抛异常则两步
        // 都不执行，直接泄漏。先登记就先受管，后面怎么走都不会漏。
        clients_.push_back(client);
        setup_client_callbacks(client);

#if UVCPP_OPENSSL_ENABLE
        // --- TLS：装过滤器，并且**不把握手没完的连接交给上层**。 ---
        //
        // **必须排在 `setup_client_callbacks` 之后。** `enable_tls()` 在连接
        // 已建立时会顺手 `arm_async_read()` 来推进握手，而那个函数装的是
        // **原始**读回调并置上 `read_started_`；先调它的话，后面
        // `setup_client_callbacks` 想给框架层读（`read_start_events`）就会被
        // 两条读路径互斥的检查挡住 —— 上层的读回调永远装不上，HTTPS 请求
        // 到了也没人解析。反过来先装读则没有这个问题：握手靠读事件推进，
        // 而两种读路径都会把入站字节喂给过滤器（见 `arm_async_read`）。
        //
        // 失败时走 `close_client_on_callback_error`（关闭 + 回收）而不是
        // `delete`：此刻句柄是活的，且已经进了登记表。
        bool tls_handshake_pending = false;
        if (ssl_ctx_ != nullptr) {
          const int tls_rc = client->enable_tls(ssl_ctx_);
          if (tls_rc != 0) {
            last_error_code_ = tls_rc;
            close_client_on_callback_error(client);
            return;
          }
          tls_handshake_pending = !client->is_tls_handshake_done();
          if (tls_handshake_pending) {
            // 通知只是把 `on_connection` 挂到"握手成功"那一刻。
            client->set_tls_ready_callback(
                [this](uvcpp_tcp_client* c, int st) {
                  on_tls_handshake_done(c, st);
                });
          }
        }
        if (tls_handshake_pending) return;  // 握手完成时再交付
#endif

        deliver_connection(client);
      },
      backlog);

  if (rc != 0) {
    last_error_code_ = rc;
    set_status(TCP_SERVER_ERROR);
    // Clean up the heap-allocated callback
    delete static_cast<std::function<void(uvcpp_tcp_client*)>*>(
        on_connection_arg_);
    on_connection_fn_  = nullptr;
    on_connection_arg_ = nullptr;
    return rc;
  }

  return 0;
}

// =========================================================================
// Stop
// =========================================================================

size_t uvcpp_tcp_server::close_all_clients() {
  // 先取快照：`client->close()` 的收尾是**异步**的（等循环转到关闭完成回调），
  // 所以这一轮里 clients_ 不会被改；但用户的 on_close 回调可能重入到这里，
  // 快照 + 逐个 owns_client 判断能挡住那种情况。
  std::vector<uvcpp_tcp_client*> snapshot(clients_.begin(), clients_.end());

  size_t closed = 0;
  for (size_t i = 0; i < snapshot.size(); ++i) {
    uvcpp_tcp_client* c = snapshot[i];
    if (c == nullptr || !owns_client(c)) continue;  // 途中被 take 走的不动

    uvcpp_tcp* t = c->get_tcp();
    if (t == nullptr || t->get_handle() == nullptr) {
      // 句柄已经关完了，人却还在登记表里 —— 说明那次关闭没有走框架的收尾
      // （比如有人直接 `get_tcp()->close(...)`）。连接已经是死的，这里顺手
      // 归还：摘除 + 释放，不用再发起关闭。
      //
      // 这个判断不是洁癖：`uvcpp_handle::is_closing()` 会把句柄指针直接交给
      // libuv，而句柄关完之后那个指针是 nullptr —— 对一个已经关掉的连接调
      // `close()` 就是空指针解引用。先看 get_handle() 才是安全的顺序。
      release_client(c);
      continue;
    }

    c->close();
    ++closed;
  }
  return closed;
}

void uvcpp_tcp_server::release_client(uvcpp_tcp_client* client) {
  if (client == nullptr) return;
  clients_.remove(client);
  delete client;
}

void uvcpp_tcp_server::stop(std::function<void()> on_stopped) {
  if (!has_status(TCP_SERVER_LISTENING)) {
    if (on_stopped) on_stopped();
    return;
  }

  set_status(TCP_SERVER_STOPPING);
  clear_status(TCP_SERVER_LISTENING);

  // Close the server TCP handle — this stops accepting new connections
  stopped_ = true;
  if (tcp_ != nullptr && !tcp_->is_closing()) {
    tcp_->close([this, on_stopped](uvcpp_handle*) {
      set_status(TCP_SERVER_STOPPED);
      clear_status(TCP_SERVER_STOPPING);
      if (on_stopped) on_stopped();
    });
  } else {
    set_status(TCP_SERVER_STOPPED);
    clear_status(TCP_SERVER_STOPPING);
    if (on_stopped) on_stopped();
  }
}

// =========================================================================
// Loop control
// =========================================================================

int uvcpp_tcp_server::run(uv_run_mode md) {
  return loop_->run(md);
}

void uvcpp_tcp_server::stop_loop() {
  loop_->stop();
}

// =========================================================================
// Internal helpers
// =========================================================================

void uvcpp_tcp_server::set_status(int flags) {
  status_ |= flags;
}

void uvcpp_tcp_server::clear_status(int flags) {
  status_ &= ~flags;
}

void uvcpp_tcp_server::install_client_manager(uvcpp_tcp_client* client) {
  if (client == nullptr) return;

  // 走 close_manager 槽，**不是**用户的 set_on_close。
  // 用户设不设自己的 on_close 都影响不到这里 —— 这正是原来那个缺陷的修法：
  // 以前两者共用一个槽，用户一设自己的回调，框架的释放就被整条跳过，
  // 于是每连接泄漏一个 client，而且用户顺手 delete 时还会 double free。
  client->set_close_manager([this, client]() { release_client(client); });
}

void uvcpp_tcp_server::setup_client_callbacks(uvcpp_tcp_client* client) {
  if (client == nullptr) return;

  // **无条件**装框架的释放逻辑。客户端的生命周期由服务端负责，这一点不由
  // 用户有没有注册回调决定。用户的观察回调是另一个槽，两者都会被触发。
  install_client_manager(client);

  // 起读。
  //
  // 这一段的顺序值得说明：它跑在用户的 on_connection **之前**，所以下面装的
  // 只是兜底，用户在 on_connection 里注册自己的读回调时会把兜底顶掉（见
  // uvcpp_tcp_client::read_start 里的 release_auto_read）。
  //
  // 为什么一定要有人读：断开只在读回调的 `nread < 0` 分支里被发现。没有读，
  // 连接断了服务端也不会知道 —— 客户端对象留在登记表里，关闭回调永远不触发，
  // 于是每个连接泄漏一个对象。以前这一点完全靠用户自觉，现在默认就装好。
  if (read_cb_) {
    // 上层框架（webapp）注册的共用回调优先。
    client->read_start_events(read_cb_);
  } else if (auto_read_) {
    // 兜底自动读：只为发现断开；数据没有消费者，收到时会打一次警告。
    client->install_auto_read();
  } else if (!client->has_read_callback()) {
    // 用户明确关掉了自动读，自己也还没设 —— 把后果说清楚，别让它静默。
    std::fprintf(stderr,
                 "[uvcpp_tcp_server] Warning: auto_read is off and no read "
                 "callback is registered for new client %p. Disconnects will "
                 "NOT be detected and the client will not be released.\n",
                 static_cast<void*>(client));
  }
}

void uvcpp_tcp_server::deliver_connection(uvcpp_tcp_client* client) {
  // Call the user's connection callback.
  // 它可能已经把这个客户端 take_client() 走了 —— 那样下面就不能再
  // 碰 client。所以这里除了 catch 分支之外不再引用它。
  if (!on_connection_fn_) return;
  try {
    on_connection_fn_(client, on_connection_arg_);
  } catch (const std::exception& e) {
    std::fprintf(stderr,
                 "[uvcpp_tcp_server] on_connection callback threw: %s\n",
                 e.what());
    close_client_on_callback_error(client);
  } catch (...) {
    std::fprintf(stderr,
                 "[uvcpp_tcp_server] on_connection callback threw (unknown)\n");
    close_client_on_callback_error(client);
  }
}

#if UVCPP_OPENSSL_ENABLE
void uvcpp_tcp_server::set_ssl_context(uvcpp_ssl_context* ctx) {
  ssl_ctx_ = ctx;
}

void uvcpp_tcp_server::on_tls_handshake_done(uvcpp_tcp_client* client,
                                             int status) {
  if (client == nullptr) return;

  if (status != 0) {
    // 握手失败：**这条连接从此不再交给上层**。
    //
    // 只记账，不在这里动手收尾 —— 通知是从 `tls_fail()` 中间发出来的，
    // 它后面还要走"通知读侧 → fire_close_callbacks"，服务端的 close
    // manager（`release_client`）会在那里把客户端摘除并释放。在这里再关
    // 一次只会把收尾做两遍。
    //
    // 常见的两种成因：对端根本不是 TLS（明文客户端打 TLS 端口，比如把
    // HTTP 请求直接写进来），以及证书协商/校验失败。两者的可诊断信息都
    // 在 `tls_last_ssl_error()` 里 —— 但那个得在客户端还活着时读，这里
    // 已经晚了（通知之后 tls_fail 还会继续，但不保证原样返回）。所以只
    // 记错误码，需要细节请在客户端的读回调里看。
    last_error_code_ = status;
    return;
  }

  deliver_connection(client);
}
#endif  // UVCPP_OPENSSL_ENABLE

// =========================================================================
// 读
// =========================================================================

void uvcpp_tcp_server::set_read_callback(const uvcpp_net_read_cb& cb) {
  read_cb_ = cb;
}

void uvcpp_tcp_server::clear_read_callback() { read_cb_ = nullptr; }

bool uvcpp_tcp_server::has_read_callback() const {
  return static_cast<bool>(read_cb_);
}

void uvcpp_tcp_server::set_auto_read(bool enable) { auto_read_ = enable; }

bool uvcpp_tcp_server::auto_read() const { return auto_read_; }

int uvcpp_tcp_server::pause_read(uvcpp_tcp_client* client) {
  // 只对自己的连接动手：对别人的（或已经 take 走的）连接调 read_stop 会
  // 停掉一个我们不了解的读路径，那种错很难查，不如直接拒掉。
  if (client == nullptr || !owns_client(client)) return UV_EINVAL;
  return client->read_pause();
}

int uvcpp_tcp_server::resume_read(uvcpp_tcp_client* client) {
  if (client == nullptr || !owns_client(client)) return UV_EINVAL;
  return client->read_resume();
}

void uvcpp_tcp_server::close_client_on_callback_error(
    uvcpp_tcp_client* client) {
  if (client == nullptr || !owns_client(client)) {
    // 所有权已经被 take_client() 取走了 —— 那是调用方的东西，不归我们回收。
    return;
  }

  // 先清掉管理槽，保证"摘除 + 释放"只发生一次（完成回调里那次）。
  client->clear_close_manager();

  uvcpp_tcp* tcp = client->get_tcp();
  if (tcp == nullptr) {
    clients_.remove(client);
    delete client;
    return;
  }

  // 不在这里同步 delete：此刻句柄还是活的，而我们在 libuv 的 accept 回调
  // 里。交给关闭流程收尾。
  tcp->close([this, client](uvcpp_handle*) {
    clients_.remove(client);
    delete client;
  });
}

// =========================================================================
// 客户端所有权
// =========================================================================

size_t uvcpp_tcp_server::client_count() const { return clients_.size(); }

bool uvcpp_tcp_server::owns_client(const uvcpp_tcp_client* client) const {
  if (client == nullptr) return false;
  for (std::list<uvcpp_tcp_client*>::const_iterator it = clients_.begin();
       it != clients_.end(); ++it) {
    if (*it == client) return true;
  }
  return false;
}

uvcpp_tcp_client* uvcpp_tcp_server::take_client(uvcpp_tcp_client* client) {
  if (client == nullptr) return nullptr;
  if (!owns_client(client)) return nullptr;  // 不归我们管 —— 所有权不变

  // 摘出登记表，再清掉管理槽。两步都做完之后，服务端就真的不再碰它了。
  clients_.remove(client);
  client->clear_close_manager();
  return client;
}

bool uvcpp_tcp_server::return_client(uvcpp_tcp_client* client) {
  if (client == nullptr) return false;
  if (owns_client(client)) return false;  // 已经在管了，重复交还

  uvcpp_tcp* tcp = client->get_tcp();
  const int status = client->get_status();
  const bool dead = (tcp == nullptr) || (tcp->get_handle() == nullptr) ||
                    ((status & TCP_CLIENT_CLOSED) != 0) ||
                    ((status & TCP_CLIENT_CLOSING) != 0) ||
                    ((status & TCP_CLIENT_ERROR) != 0);

  if (dead) {
    // 已经关掉的连接不会再产生任何事件，重新登记只会留个空壳。
    // **这里就是"由框架决定删除还是复用"的落点** —— 将来要加客户端
    // 对象池，复用的分支接在这里。
    delete client;
    return true;
  }

  clients_.push_back(client);
  install_client_manager(client);
  return true;
}

}  // namespace uvcpp
