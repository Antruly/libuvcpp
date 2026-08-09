/**
 * @file src/web/uvcpp_http_client.cpp
 * @brief Implementation of uvcpp_http_client — HTTP/1.1 client.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_http_client.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_http_parser.h>
#include <web/uvcpp_http_compress.h>
#include <handle/uvcpp_tcp.h>
#include <req/uvcpp_write.h>
#include <chrono>
#include <cstring>
#include <thread>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl.h>
#include <ssl/uvcpp_ssl_context.h>
#endif

namespace uvcpp {

// =========================================================================
// Construction / Destruction
// =========================================================================

uvcpp_http_client::uvcpp_http_client() {
  loop_   = new uvcpp_loop();
  tcp_    = new uvcpp_tcp_client(loop_);
  parser_ = new uvcpp_http_parser(http_parser_mode::PARSE_RESPONSE);
}

uvcpp_http_client::~uvcpp_http_client() {
  // Close TCP if still active
  if (tcp_ != nullptr && !has_status(HTTP_CLIENT_CLOSED)) {
    uvcpp_tcp* raw_tcp = tcp_->get_tcp();
    if (raw_tcp != nullptr && !raw_tcp->is_closing() && raw_tcp->is_active()) {
      bool close_done = false;
      raw_tcp->close([&close_done](uvcpp_handle*) { close_done = true; });
      for (int i = 0; i < 5000 && !close_done; i++) {
        loop_->run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      for (int i = 0; i < 20; i++) {
        loop_->run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } else if (raw_tcp != nullptr && raw_tcp->is_closing()) {
      loop_->run(UV_RUN_NOWAIT);
    }
  }

  // Close loop before deleting wrappers
  if (loop_ != nullptr) {
    loop_->loop_close();
    delete loop_;
    loop_ = nullptr;
  }

  delete tcp_;
  tcp_ = nullptr;
  delete parser_;
  parser_ = nullptr;
#if UVCPP_OPENSSL_ENABLE
  delete ssl_;
  ssl_ = nullptr;
#endif
}

// =========================================================================
// Connect
// =========================================================================

int uvcpp_http_client::connect(const char* host, int port,
                                std::function<void(int)> cb) {
  host_ = host;
  port_ = port;
  last_error_code_ = 0;

  if (cb) {
    tcp_->connect(host, port, [this, cb](int status) {
      if (status != 0) { set_status(HTTP_CLIENT_ERROR); last_error_code_ = status; cb(status); return; }
#if UVCPP_OPENSSL_ENABLE
      if (ssl_enabled_) {
        uv_os_sock_t sock;
        if (uvcpp_handle::fileno(tcp_->get_tcp(), sock) != 0 ||
            do_ssl_handshake(static_cast<int>(sock)) <= 0) {
          set_status(HTTP_CLIENT_ERROR); last_error_code_ = -1; cb(-1); return;
        }
      }
#endif
      set_status(HTTP_CLIENT_CONNECTED);
      clear_status(HTTP_CLIENT_ERROR);
      cb(0);
    });
    return 0;
  }
  return connect_wait(host, port);
}

int uvcpp_http_client::connect_wait(const char* host, int port,
                                     int timeout_ms) {
  host_ = host;
  port_ = port;
  last_error_code_ = 0;

  bool done = false;
  int result = 0;

  tcp_->connect(host, port, [&done, &result](int status) {
    result = status;
    done = true;
  });

  auto start = std::chrono::steady_clock::now();
  while (!done) {
    loop_->run(UV_RUN_NOWAIT);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count() >= timeout_ms) {
      last_error_code_ = UV_ETIMEDOUT;
      return UV_ETIMEDOUT;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (result == 0) {
    set_status(HTTP_CLIENT_CONNECTED);
#if UVCPP_OPENSSL_ENABLE
      if (ssl_enabled_) {
        uv_os_sock_t sock;
        if (uvcpp_handle::fileno(tcp_->get_tcp(), sock) != 0 ||
            do_ssl_handshake(static_cast<int>(sock)) <= 0) {
          set_status(HTTP_CLIENT_ERROR); last_error_code_ = -1; return -1;
        }
      }
#endif
    clear_status(HTTP_CLIENT_ERROR);
  } else {
    set_status(HTTP_CLIENT_ERROR);
    last_error_code_ = result;
  }
  return result;
}

// =========================================================================
// Send — async
// =========================================================================

int uvcpp_http_client::send(const uvcpp_http_request& req,
                             std::function<void(const uvcpp_http_response&, int)> cb) {
  if (!has_status(HTTP_CLIENT_CONNECTED)) {
    last_error_code_ = UV_ENOTCONN;
    return UV_ENOTCONN;
  }

  // Reset state for new request
  last_error_code_ = 0;
  clear_status(HTTP_CLIENT_COMPLETE);
  clear_status(HTTP_CLIENT_ERROR);
  body_buf_.clear();
  response_headers_done_ = false;
  pending_resp_ = uvcpp_http_response();
  user_cb_ = std::move(cb);

  // Reset parser and install callbacks
  parser_->reset();
  parser_->set_on_body([this](const char* at, size_t len) {
    body_buf_.append_data(at, len);
  });
  parser_->set_on_headers_complete([this]() {
    response_headers_done_ = true;
  });
  parser_->set_on_message_complete([this]() {
    on_response_complete();
  });

  // Read handler — only set on first request; keep-alive reuses existing
  if (!has_status(HTTP_CLIENT_RECEIVING)) {
    tcp_->read_start([this](uvcpp_buf* buf) {
      if (buf && buf->size() > 0) {
        on_tcp_data(buf);
      }
    });
  }

  // Serialize request and write
  set_status(HTTP_CLIENT_SENDING);

#if UVCPP_ZLIB_ENABLE
  // If compression enabled, add Accept-Encoding (on a mutable copy)
  uvcpp_http_request req_copy = req;
  if (compress_enabled_ && !req_copy.has_header("accept-encoding")) {
    req_copy.set_header("accept-encoding", "gzip, deflate");
  }
  std::string raw = req_copy.to_string();
#else
  std::string raw = req.to_string();
#endif

  tcp_->write(raw.c_str(), raw.size(), [this](int status) {
    if (status != 0) {
      set_status(HTTP_CLIENT_ERROR);
      last_error_code_ = status;
      if (user_cb_) {
        user_cb_(pending_resp_, status);
        user_cb_ = nullptr;
      }
      return;
    }
    clear_status(HTTP_CLIENT_SENDING);
    set_status(HTTP_CLIENT_RECEIVING);
  });

  return 0;
}

// =========================================================================
// Send — sync
// =========================================================================

int uvcpp_http_client::send_wait(const uvcpp_http_request& req,
                                  uvcpp_http_response& resp,
                                  int timeout_ms) {
  bool done = false;
  int result_err = 0;

  int rc = send(req, [&done, &result_err, &resp](
                          const uvcpp_http_response& r, int err) {
    result_err = err;
    if (err == 0) resp = r;
    done = true;
  });

  if (rc != 0) return rc;

  auto start = std::chrono::steady_clock::now();
  while (!done) {
    loop_->run(UV_RUN_NOWAIT);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count() >= timeout_ms) {
      last_error_code_ = UV_ETIMEDOUT;
      return UV_ETIMEDOUT;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return result_err;
}

// =========================================================================
// TCP data handler
// =========================================================================

void uvcpp_http_client::on_tcp_data(uvcpp_buf* buf) {
  if (buf == nullptr || buf->size() == 0) return;

  const char* data = buf->get_const_data();
  size_t len = buf->size();
  parser_->execute(data, len);

  if (parser_->has_error()) {
    set_status(HTTP_CLIENT_ERROR);
    last_error_code_ = UV_EINVAL;
    if (user_cb_) {
      user_cb_(pending_resp_, last_error_code_);
      user_cb_ = nullptr;
    }
  }
}

void uvcpp_http_client::on_tcp_close(uvcpp_tcp_client* /*client*/) {
  set_status(HTTP_CLIENT_CLOSED);
  clear_status(HTTP_CLIENT_CONNECTED);

  // If we have a pending callback and response isn't complete,
  // this is an unexpected close → report error
  if (user_cb_ && !has_status(HTTP_CLIENT_COMPLETE)) {
    set_status(HTTP_CLIENT_ERROR);
    last_error_code_ = UV_ECONNRESET;
    user_cb_(pending_resp_, UV_ECONNRESET);
    user_cb_ = nullptr;
  }
}

void uvcpp_http_client::on_response_complete() {
  set_status(HTTP_CLIENT_COMPLETE);
  clear_status(HTTP_CLIENT_RECEIVING);

  pending_resp_.version        = parser_->get_uvcpp_http_version();
  pending_resp_.status_code    = parser_->get_status_code();
  pending_resp_.status_message = http_status_reason(pending_resp_.status_code);
  pending_resp_.headers        = parser_->get_headers();
  pending_resp_.body.clone(body_buf_);

  // Detect keep-alive from response header
  std::string conn = http_get_header(pending_resp_.headers, "connection");
  if (http_name_equal(conn, "close")) {
    keep_alive_ = false;
  }

#if UVCPP_ZLIB_ENABLE
  // Auto-decompress response body if Content-Encoding is set
  if (compress_enabled_) {
    std::string ce = http_get_header(pending_resp_.headers, "content-encoding");
    if (!ce.empty()) {
      http_compress_method enc = http_compress_method::NONE;
      {
        std::string cel = ce;
        for (auto& c : cel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (cel == "gzip" || cel == "x-gzip")
          enc = http_compress_method::GZIP;
        else if (cel == "deflate" || cel == "x-deflate")
          enc = http_compress_method::DEFLATE;
      }
      if (enc != http_compress_method::NONE &&
          pending_resp_.body.size() >= compress_min_body_) {
        auto result = http_compress::decompress(
            pending_resp_.body.get_const_data(),
            pending_resp_.body.size());
        if (result.success) {
          pending_resp_.body.clear();
          pending_resp_.body.clone(result.data);
          pending_resp_.remove_header("content-encoding");
          pending_resp_.remove_header("content-length");
        }
      }
    }
  }
#endif

  if (user_cb_) {
    auto cb = std::move(user_cb_);
    user_cb_ = nullptr;
    cb(pending_resp_, 0);
  }
}

// =========================================================================
// Convenience: GET / POST
// =========================================================================

int uvcpp_http_client::get(const std::string& path,
                            std::function<void(const uvcpp_http_response&, int)> cb) {
  return send(uvcpp_http_request::make_get(path), cb);
}

int uvcpp_http_client::get_wait(const std::string& path,
                                 uvcpp_http_response& resp,
                                 int timeout_ms) {
  return send_wait(uvcpp_http_request::make_get(path), resp, timeout_ms);
}

int uvcpp_http_client::post(const std::string& path,
                             const char* body, size_t len,
                             const std::string& content_type,
                             std::function<void(const uvcpp_http_response&, int)> cb) {
  return send(uvcpp_http_request::make_post(path, body, len, content_type), cb);
}

int uvcpp_http_client::post_wait(const std::string& path,
                                  const char* body, size_t len,
                                  const std::string& content_type,
                                  uvcpp_http_response& resp,
                                  int timeout_ms) {
  return send_wait(uvcpp_http_request::make_post(path, body, len, content_type),
                   resp, timeout_ms);
}

// =========================================================================
// Loop / Status
// =========================================================================

int uvcpp_http_client::run(uv_run_mode md) { return loop_->run(md); }
void uvcpp_http_client::stop() { loop_->stop(); }
int uvcpp_http_client::get_status() const { return status_; }

bool uvcpp_http_client::has_status(int flags) const {
  return (status_ & flags) == flags;
}

int uvcpp_http_client::get_last_error() const { return last_error_code_; }
uvcpp_tcp_client* uvcpp_http_client::get_tcp_client() { return tcp_; }
void uvcpp_http_client::set_keep_alive(bool enable) { keep_alive_ = enable; }

void uvcpp_http_client::set_status(int flags) { status_ |= flags; }
void uvcpp_http_client::clear_status(int flags) { status_ &= ~flags; }

// =========================================================================
// SSL / HTTPS support
// =========================================================================

#if UVCPP_OPENSSL_ENABLE

void uvcpp_http_client::set_ssl_context(uvcpp_ssl_context* ctx) {
  ssl_ctx_ = ctx;
  ssl_enabled_ = true;
}

bool uvcpp_http_client::is_ssl_enabled() const { return ssl_enabled_; }

int uvcpp_http_client::do_ssl_handshake(int fd) {
  if (!ssl_ctx_) return -1;
  delete ssl_;
  ssl_ = new uvcpp_ssl(ssl_ctx_, fd);
  return ssl_->handshake();
}

int uvcpp_http_client::ssl_read(char* buf, size_t len) {
  if (!ssl_) return -1;
  return ssl_->read(buf, len);
}

int uvcpp_http_client::ssl_write(const char* data, size_t len) {
  if (!ssl_) return -1;
  return ssl_->write(data, len);
}

#endif

// =========================================================================
// Compression support
// =========================================================================

#if UVCPP_ZLIB_ENABLE
void uvcpp_http_client::set_compression_enabled(bool enable) {
  compress_enabled_ = enable;
}
bool uvcpp_http_client::is_compression_enabled() const {
  return compress_enabled_;
}
#endif

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
