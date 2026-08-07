/**
 * @file src/web/uvcpp_ws_client.cpp
 * @brief WebSocket client — HTTP Upgrade + WS connection.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_ws_client.h>

#if UVCPP_WEB_ENABLE
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <web/uvcpp_http_parser.h>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl.h>
#include <ssl/uvcpp_ssl_context.h>
#endif

namespace uvcpp {

// Simple base64 encode (for Sec-WebSocket-Key)
static std::string base64_encode(const unsigned char* data, size_t len) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = (static_cast<uint32_t>(data[i]) << 16);
    if (i + 1 < len) v |= (static_cast<uint32_t>(data[i + 1]) << 8);
    if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
    out += tbl[(v >> 18) & 0x3F];
    out += tbl[(v >> 12) & 0x3F];
    out += (i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=';
    out += (i + 2 < len) ? tbl[v & 0x3F] : '=';
  }
  return out;
}

// Generate a pseudo-random 16-byte WebSocket key
static std::string generate_ws_key() {
  unsigned char key[16];
  for (int i = 0; i < 16; i++)
    key[i] = static_cast<unsigned char>(std::rand() % 256);
  return base64_encode(key, 16);
}

uvcpp_ws_client::uvcpp_ws_client() {
  loop_ = new uvcpp_loop();
  tcp_  = new uvcpp_tcp_client(loop_);
  std::srand(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
}

uvcpp_ws_client::~uvcpp_ws_client() {
  if (tcp_ && !has_status(WS_CLIENT_CLOSED)) {
    auto* raw = tcp_->get_tcp();
    if (raw && !raw->is_closing() && raw->is_active()) {
      bool done = false;
      raw->close([&done](uvcpp_handle*) { done = true; });
      for (int i = 0; i < 5000 && !done; i++) { loop_->run(UV_RUN_NOWAIT); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
      for (int i = 0; i < 20; i++) { loop_->run(UV_RUN_NOWAIT); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
  }
  if (loop_) { loop_->loop_close(); delete loop_; loop_ = nullptr; }
  delete tcp_; tcp_ = nullptr;
#if UVCPP_OPENSSL_ENABLE
  delete ssl_; ssl_ = nullptr;
#endif
}

int uvcpp_ws_client::connect(const std::string& url,
                              std::function<void(uvcpp_ws_connection*, int)> cb) {
  // Parse URL: ws:// or wss://
  std::string u = url;
  std::string host = "127.0.0.1";
  int port = 80;
  std::string path = "/";
  bool use_tls = false;

  if (u.compare(0, 5, "ws://") == 0) { u = u.substr(5); }
  else if (u.compare(0, 6, "wss://") == 0) {
    u = u.substr(6); port = 443; use_tls = true;
#if !UVCPP_OPENSSL_ENABLE
    last_error_ = -1; return -1;
#endif
  }

  size_t slash = u.find('/');
  if (slash != std::string::npos) { path = u.substr(slash); u = u.substr(0, slash); }
  size_t colon = u.find(':');
  if (colon != std::string::npos) { host = u.substr(0, colon); port = std::stoi(u.substr(colon + 1)); }
  else host = u;

  ws_host_ = host; ws_path_ = path;
  ws_key_ = generate_ws_key();
  connect_cb_ = std::move(cb);
  use_tls_ = use_tls;

#if UVCPP_OPENSSL_ENABLE
  if (use_tls && !ssl_ctx_) { last_error_ = -1; on_handshake_complete(-1); return -1; }
#endif

  status_ = WS_CLIENT_CONNECTING;
  do_handshake(host, port, path, ws_key_);
  return 0;
}

int uvcpp_ws_client::connect_wait(const std::string& url,
                                   uvcpp_ws_connection*& out_conn,
                                   int timeout_ms) {
  bool done = false;
  int err = 0;
  connect(url, [&](uvcpp_ws_connection* c, int e) { out_conn = c; err = e; done = true; });
  auto t0 = std::chrono::steady_clock::now();
  while (!done) {
    loop_->run(UV_RUN_NOWAIT);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= timeout_ms)
      { last_error_ = UV_ETIMEDOUT; return UV_ETIMEDOUT; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return err;
}

void uvcpp_ws_client::do_handshake(const std::string& host, int port,
                                    const std::string& path, const std::string& key) {
  tcp_->connect(host.c_str(), port, [this](int st) {
    if (st != 0) { on_handshake_complete(st); return; }

#if UVCPP_OPENSSL_ENABLE
    if (use_tls_) {
      uv_os_sock_t sock;
      if (tcp_->get_tcp() && uvcpp_handle::fileno(tcp_->get_tcp(), sock) == 0) {
        delete ssl_; ssl_ = new uvcpp_ssl(ssl_ctx_, static_cast<int>(sock));
        int rc = ssl_->handshake();
        if (rc <= 0) { on_handshake_complete(-1); return; }
      } else { on_handshake_complete(-1); return; }
    }
#endif

    // Send HTTP upgrade request
    std::string req =
        "GET " + ws_path_ + " HTTP/1.1\r\n"
        "Host: " + ws_host_ + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + ws_key_ + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    tcp_->write(req.c_str(), req.size(), [this](int) {
      // Read the 101 response
      tcp_->read_start([this](uvcpp_buf* buf) {
        if (buf && buf->size() > 0) on_handshake_data(buf);
      });
    });
  });
}

void uvcpp_ws_client::on_handshake_data(uvcpp_buf* buf) {
  // Accumulate partial response chunks
  handshake_buf_.append(buf->get_const_data(), buf->size());
  // Wait for complete HTTP response (headers terminated by \r\n\r\n)
  size_t end = handshake_buf_.find("\r\n\r\n");
  if (end == std::string::npos) return;  // more data needed
  // Check for "101 Switching Protocols" status
  if (handshake_buf_.find(" 101 ") != std::string::npos ||
      handshake_buf_.compare(0, 12, "HTTP/1.1 101") == 0) {
    on_handshake_complete(0);
  } else {
    on_handshake_complete(-2);
  }
  handshake_buf_.clear();
}

void uvcpp_ws_client::on_handshake_complete(int error) {
  if (error == 0) {
    status_ = WS_CLIENT_OPEN;
    auto* conn = new uvcpp_ws_connection(tcp_);
    // start() will be called lazily on first send_frame (avoids
    // calling read_stop from within the active read callback)
    if (connect_cb_) { auto cb = std::move(connect_cb_); connect_cb_ = nullptr; cb(conn, 0); }
  } else {
    status_ = WS_CLIENT_ERROR;
    last_error_ = error;
    if (connect_cb_) { auto cb = std::move(connect_cb_); connect_cb_ = nullptr; cb(nullptr, error); }
  }
}

int uvcpp_ws_client::run(uv_run_mode md) { return loop_->run(md); }
void uvcpp_ws_client::stop() { loop_->stop(); }
uvcpp_loop* uvcpp_ws_client::get_loop() { return loop_; }
int uvcpp_ws_client::get_status() const { return status_; }
bool uvcpp_ws_client::has_status(int flags) const { return (status_ & flags) == flags; }
int uvcpp_ws_client::get_last_error() const { return last_error_; }

#if UVCPP_OPENSSL_ENABLE
void uvcpp_ws_client::set_ssl_context(uvcpp_ssl_context* ctx) { ssl_ctx_ = ctx; }
#endif

}  // namespace uvcpp
#endif
