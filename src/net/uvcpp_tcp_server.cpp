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

        // Call the user's connection callback
        if (on_connection_fn_) {
          on_connection_fn_(client, on_connection_arg_);
        }

        // Auto-setup client callbacks after user callback returns
        setup_client_callbacks(client);

        // Track the client for cleanup on server stop
        clients_.push_back(client);
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

void uvcpp_tcp_server::setup_client_callbacks(uvcpp_tcp_client* client) {
  if (client == nullptr) return;

  // Check if the user registered a read callback.
  // If not, print a warning — data will not be received.
  if (!client->has_read_callback()) {
    std::fprintf(stderr,
                 "[uvcpp_tcp_server] Warning: no read callback registered "
                 "for new client %p. Data will not be received.\n",
                 static_cast<void*>(client));
  }

  // Install default close callback only if the user didn't set one.
  // The default auto-deletes the client when the connection closes
  // and removes it from the server's client list.
  if (!client->has_close_callback()) {
    client->set_on_close([this, client]() {
      clients_.remove(client);
      delete client;
    });
  }
}

}  // namespace uvcpp
