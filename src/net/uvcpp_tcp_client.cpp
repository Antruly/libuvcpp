/**
 * @file src/net/uvcpp_tcp_client.cpp
 * @brief Implementation of uvcpp_tcp_client.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <net/uvcpp_tcp_client.h>
#include <req/uvcpp_connect.h>
#include <req/uvcpp_write.h>
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_define.h>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

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

uvcpp_tcp_client::~uvcpp_tcp_client() {
  // Stop any active reads
  if (read_started_) {
    tcp_->read_stop();
    read_started_ = false;
  }

  // Ensure the TCP handle is fully closed and its endgame processed
  // before we try to close the loop.  libuv asserts the handle queue is
  // empty before uv_loop_close().
  if (tcp_ != nullptr) {
    if (!tcp_->is_closing() && tcp_->is_active()) {
      // Handle is still active — close it and pump the loop.
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
  return has_async_read_cb_;
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

    struct sockaddr_in addr;
    int rc = uv_ip4_addr(ip, port, &addr);
    if (rc != 0) {
      set_status(TCP_CLIENT_ERROR);
      last_error_code_ = rc;
      clear_status(TCP_CLIENT_CONNECTING);
      return rc;
    }

    // Allocate callback data on heap, pass through C-style trampoline
    // to avoid std::function copy-chain corruption through libuv layers.
    connect_fn_  = trampoline_connect;
    connect_arg_ = new std::function<void(int)>(cb);

    uvcpp_connect* conn = new uvcpp_connect();

    rc = tcp_->connect(
        conn, reinterpret_cast<const struct sockaddr*>(&addr),
        [this](uvcpp_connect* r, int status) {
          clear_status(TCP_CLIENT_CONNECTING);
          if (status == 0) {
            set_status(TCP_CLIENT_CONNECTED | TCP_CLIENT_READABLE |
                       TCP_CLIENT_WRITABLE);
          } else {
            set_status(TCP_CLIENT_ERROR);
            last_error_code_ = status;
          }
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
      delete static_cast<std::function<void(int)>*>(connect_arg_);
      connect_fn_  = nullptr;
      connect_arg_ = nullptr;
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

  // Reset sync state
  sync_connect_done_   = false;
  sync_connect_result_ = 0;

  set_status(TCP_CLIENT_CONNECTING);

  struct sockaddr_in addr;
  int rc = uv_ip4_addr(ip, port, &addr);
  if (rc != 0) {
    set_status(TCP_CLIENT_ERROR);
    last_error_code_ = rc;
    clear_status(TCP_CLIENT_CONNECTING);
    return rc;
  }

  uvcpp_connect* conn = new uvcpp_connect();
  rc = tcp_->connect(
      conn, reinterpret_cast<const struct sockaddr*>(&addr),
      [this](uvcpp_connect* r, int status) {
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

  return sync_connect_result_;
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

    uvcpp_buf bufcpp(data, len);
    uv_buf_t* raw_buf = bufcpp.out_uv_buf();

    uvcpp_write* w = new uvcpp_write();
    w->set_uv_buf(raw_buf, true);

    int rc = tcp_->write(
        w, w->get_uv_buf(), 1,
        [this](uvcpp_write* wr, int status) {
          if (status != 0) {
            last_error_code_ = status;
          }
          if (write_fn_) {
            write_fn_(status, write_arg_);
            write_fn_  = nullptr;
            write_arg_ = nullptr;
          }
          has_async_write_cb_ = false;
          delete wr;
        });

    if (rc != 0) {
      last_error_code_ = rc;
      delete w;
      delete static_cast<std::function<void(int)>*>(write_arg_);
      write_fn_  = nullptr;
      write_arg_ = nullptr;
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

  uvcpp_buf bufcpp(data, len);            // stack-allocated helper
  uv_buf_t* raw_buf = bufcpp.out_uv_buf();  // transfers data ownership to raw_buf

  uvcpp_write* w = new uvcpp_write();
  w->set_uv_buf(raw_buf, true);           // w takes ownership of raw_buf and its data

  int rc = tcp_->write(
      w, w->get_uv_buf(), 1,
      [this](uvcpp_write* wr, int status) {
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

    int rc = tcp_->write(
        w, w->get_uv_buf(), 1,
        [this](uvcpp_write* wr, int status) {
          if (status != 0) last_error_code_ = status;
          if (write_fn_) {
            write_fn_(status, write_arg_);
            write_fn_  = nullptr;
            write_arg_ = nullptr;
          }
          delete wr;
        });

    if (rc != 0) {
      last_error_code_ = rc;
      delete w;
      delete static_cast<std::function<void(int)>*>(write_arg_);
      write_fn_  = nullptr;
      write_arg_ = nullptr;
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

  int rc = tcp_->write(
      w, w->get_uv_buf(), 1,
      [this](uvcpp_write* wr, int status) {
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

int uvcpp_tcp_client::read_start(std::function<void(uvcpp_buf*)> cb) {
  if (cb != nullptr) {
    // --- Async mode ---
    if (has_async_read_cb_) {
      return UV_EALREADY;
    }

    has_async_read_cb_ = true;

    read_fn_  = trampoline_read;
    read_arg_ = new std::function<void(uvcpp_buf*)>(cb);

    ensure_read_cache();

    int rc = tcp_->read_start(
        internal_alloc_cb,
        [this](uvcpp_stream* /*s*/, ssize_t nread, const uv_buf_t* buf) {
          if (nread > 0) {
            set_status(TCP_CLIENT_READABLE);

            uvcpp_buf tmp_buf;
            tmp_buf.clone_data(buf->base, static_cast<size_t>(nread));
            uvcpp_free_bytes(buf->base);

            if (read_fn_) {
              read_fn_(&tmp_buf, read_arg_);
            }
          } else {
            if (nread < 0) {
              last_error_code_ = static_cast<int>(nread);
              if (nread != UV_EOF) {
                set_status(TCP_CLIENT_ERROR);
              }
              // Notify close callback on connection close or error
              if (close_fn_) {
                close_fn_(close_arg_);
                close_fn_  = nullptr;
                close_arg_ = nullptr;
              }
            }
            // Free the buffer even on error/EOF
            if (buf->base != nullptr) {
              uvcpp_free_bytes(buf->base);
            }
          }
        });

    if (rc != 0) {
      last_error_code_ = rc;
      return rc;
    }

    read_started_ = true;
    set_status(TCP_CLIENT_READABLE);
    return 0;
  } else {
    // --- Enable sync read cache ---
    if (has_async_read_cb_) {
      throw std::runtime_error(
          "uvcpp_tcp_client::read_start: cannot use sync read after "
          "async read callback was registered");
    }

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
    return 0;
  }
}

int uvcpp_tcp_client::read_wait(uvcpp_buf& out_buf, int timeout_ms) {
  if (has_async_read_cb_) {
    throw std::runtime_error(
        "uvcpp_tcp_client::read_wait: cannot use sync read after "
        "async read callback was registered");
  }

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

int uvcpp_tcp_client::read_stop() {
  clear_status(TCP_CLIENT_READABLE);
  read_started_ = false;
  has_async_read_cb_ = false;
  if (read_arg_ != nullptr) {
    delete static_cast<std::function<void(uvcpp_buf*)>*>(read_arg_);
    read_arg_ = nullptr;
  }
  read_fn_ = nullptr;
  return tcp_->read_stop();
}

void uvcpp_tcp_client::set_on_close(std::function<void()> cb) {
  if (close_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_arg_);
    close_arg_ = nullptr;
  }
  close_fn_  = trampoline_close;
  close_arg_ = new std::function<void()>(cb);
}

void uvcpp_tcp_client::clear_on_close() {
  if (close_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_arg_);
    close_arg_ = nullptr;
  }
  close_fn_ = nullptr;
}

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
    // Notify close callback on connection close or error
    if (close_fn_) {
      close_fn_(close_arg_);
      close_fn_  = nullptr;
      close_arg_ = nullptr;
    }
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
