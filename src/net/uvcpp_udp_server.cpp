/**
 * @file src/net/uvcpp_udp_server.cpp
 * @brief Implementation of uvcpp_udp_server.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <net/uvcpp_udp_server.h>
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

static void trampoline_recv(uvcpp_buf* buf, const char* ip, int port,
                             void* arg) {
  auto* cb =
      static_cast<std::function<void(uvcpp_buf*, const char*, int)>*>(arg);
  (*cb)(buf, ip, port);
}

// =========================================================================
// Construction / Destruction
// =========================================================================

uvcpp_udp_server::uvcpp_udp_server() {
  loop_ = new uvcpp_loop();
  udp_  = new uvcpp_udp(loop_);
  status_ = UDP_SERVER_NONE;
}

uvcpp_udp_server::~uvcpp_udp_server() {
  // Close UDP handle (if not already closed by stop())
  if (udp_ != nullptr && !stopped_) {
    if (!udp_->is_closing() && udp_->is_active()) {
      bool close_done = false;
      udp_->close([&close_done](uvcpp_handle*) { close_done = true; });
      for (int i = 0; i < 5000 && !close_done; i++) {
        if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      // Drain remaining endgames
      for (int i = 0; i < 20; i++) {
        if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } else if (udp_->is_closing()) {
      if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
    }
    delete udp_;
    udp_ = nullptr;
  }

  // Close loop
  if (loop_ != nullptr) {
    loop_->loop_close();
    delete loop_;
    loop_ = nullptr;
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

uvcpp_udp* uvcpp_udp_server::get_udp() { return udp_; }
uvcpp_loop* uvcpp_udp_server::get_loop() { return loop_; }
int uvcpp_udp_server::get_status() const { return status_; }
bool uvcpp_udp_server::has_status(int flags) const {
  return (status_ & flags) == flags;
}
int uvcpp_udp_server::get_last_error() const { return last_error_code_; }

// =========================================================================
// Bind
// =========================================================================

int uvcpp_udp_server::bind(const char* ip, int port) {
  if (std::strchr(ip, ':') != nullptr) return bindIpv6(ip, port);
  return bindIpv4(ip, port);
}

int uvcpp_udp_server::bindIpv4(const char* ip, int port) {
  struct sockaddr_in addr;
  int rc = uv_ip4_addr(ip, port, &addr);
  if (rc != 0) { last_error_code_ = rc; set_status(UDP_SERVER_ERROR); return rc; }
  rc = udp_->bind(reinterpret_cast<const struct sockaddr*>(&addr), 0);
  if (rc != 0) { last_error_code_ = rc; set_status(UDP_SERVER_ERROR); return rc; }
  set_status(UDP_SERVER_BOUND);
  return 0;
}

int uvcpp_udp_server::bindIpv6(const char* ip, int port) {
  struct sockaddr_in6 addr;
  int rc = uv_ip6_addr(ip, port, &addr);
  if (rc != 0) { last_error_code_ = rc; set_status(UDP_SERVER_ERROR); return rc; }
  rc = udp_->bind(reinterpret_cast<const struct sockaddr*>(&addr), 0);
  if (rc != 0) { last_error_code_ = rc; set_status(UDP_SERVER_ERROR); return rc; }
  set_status(UDP_SERVER_BOUND);
  return 0;
}

// =========================================================================
// Receive
// =========================================================================

int uvcpp_udp_server::recv_start(
    std::function<void(uvcpp_buf*, const char*, int)> cb) {
  if (cb == nullptr) return UV_EINVAL;

  recv_fn_  = trampoline_recv;
  recv_arg_ = new std::function<void(uvcpp_buf*, const char*, int)>(cb);

  int rc = udp_->recv_start(
      [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
        uvcpp_buf::alloc_buf(buf, sz > 0 ? sz : 4096);
      },
      [this](uvcpp_udp* /*u*/, ssize_t nread, const uv_buf_t* buf,
             const struct sockaddr* addr, unsigned int /*flags*/) {
        if (nread > 0) {
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
          set_status(UDP_SERVER_ERROR);
        }
        if (buf->base != nullptr) uvcpp_free_bytes(buf->base);
      });

  if (rc != 0) { last_error_code_ = rc; return rc; }
  return 0;
}

int uvcpp_udp_server::recv_stop() {
  return udp_->recv_stop();
}

// =========================================================================
// Send
// =========================================================================

namespace {

int send_impl(uvcpp_udp* udp, int& last_err, const char* ip, int port,
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

}  // namespace

int uvcpp_udp_server::send(const char* ip, int port, const char* data,
                            size_t len, std::function<void(int)> cb) {
  if (cb != nullptr) {
    // Async
    uvcpp_buf bufcpp(data, len);
    uv_buf_t* raw = bufcpp.out_uv_buf();
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(raw, true);

    auto* user_cb = new std::function<void(int)>(cb);
    int rc = send_impl(udp_, last_error_code_, ip, port, raw, w,
                       [user_cb](uvcpp_udp_send* wr, int status) {
                         (*user_cb)(status);
                         delete user_cb;
                         delete wr;
                       });
    if (rc != 0) { delete user_cb; return rc; }
    return 0;
  } else {
    return send_wait(ip, port, data, len, 30000);
  }
}

int uvcpp_udp_server::send_wait(const char* ip, int port, const char* data,
                                 size_t len, int timeout_ms) {
  bool done = false;
  int result = 0;

  uvcpp_buf bufcpp(data, len);
  uv_buf_t* raw = bufcpp.out_uv_buf();
  uvcpp_udp_send* w = new uvcpp_udp_send();
  w->set_uv_buf(raw, true);

  int rc = send_impl(udp_, last_error_code_, ip, port, raw, w,
                     [&done, &result](uvcpp_udp_send* wr, int status) {
                       result = status;
                       done = true;
                       delete wr;
                     });
  if (rc != 0) return rc;

  auto start = std::chrono::steady_clock::now();
  while (!done) {
    if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count() >= timeout_ms) {
      last_error_code_ = UV_ETIMEDOUT;
      return UV_ETIMEDOUT;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return result;
}

int uvcpp_udp_server::send(const char* ip, int port, const uvcpp_buf& buf,
                            std::function<void(int)> cb) {
  return send(ip, port, buf.get_const_data(), buf.size(), cb);
}

int uvcpp_udp_server::send(const char* ip, int port, uvcpp_buf* buf,
                            std::function<void(int)> cb) {
  if (buf == nullptr) return UV_EINVAL;
  if (cb != nullptr) {
    // Async with move
    uv_buf_t* raw = buf->out_uv_buf();
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(raw, true);

    auto* user_cb = new std::function<void(int)>(cb);
    int rc = send_impl(udp_, last_error_code_, ip, port, raw, w,
                       [user_cb](uvcpp_udp_send* wr, int status) {
                         (*user_cb)(status);
                         delete user_cb;
                         delete wr;
                       });
    if (rc != 0) { delete user_cb; return rc; }
    return 0;
  } else {
    // Sync with move
    bool done = false;
    int result = 0;

    uv_buf_t* raw = buf->out_uv_buf();
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(raw, true);

    int rc = send_impl(udp_, last_error_code_, ip, port, raw, w,
                       [&done, &result](uvcpp_udp_send* wr, int status) {
                         result = status;
                         done = true;
                         delete wr;
                       });
    if (rc != 0) return rc;

    auto start = std::chrono::steady_clock::now();
    while (!done) {
      if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count() >= 30000) {
        last_error_code_ = UV_ETIMEDOUT;
        return UV_ETIMEDOUT;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return result;
  }
}

// =========================================================================
// Stop
// =========================================================================

void uvcpp_udp_server::stop(std::function<void()> on_stopped) {
  if (!has_status(UDP_SERVER_BOUND)) {
    if (on_stopped) on_stopped();
    return;
  }

  set_status(UDP_SERVER_STOPPING);
  clear_status(UDP_SERVER_BOUND);
  stopped_ = true;

  if (udp_ != nullptr && !udp_->is_closing()) {
    udp_->close([this, on_stopped](uvcpp_handle*) {
      set_status(UDP_SERVER_STOPPED);
      clear_status(UDP_SERVER_STOPPING);
      if (on_stopped) on_stopped();
    });
  } else {
    set_status(UDP_SERVER_STOPPED);
    clear_status(UDP_SERVER_STOPPING);
    if (on_stopped) on_stopped();
  }
}

// =========================================================================
// Loop control
// =========================================================================

int uvcpp_udp_server::run(uv_run_mode md) { return loop_->run(md); }
void uvcpp_udp_server::stop_loop() { loop_->stop(); }

// =========================================================================
// Internal
// =========================================================================

void uvcpp_udp_server::set_status(int flags) { status_ |= flags; }
void uvcpp_udp_server::clear_status(int flags) { status_ &= ~flags; }

}  // namespace uvcpp
