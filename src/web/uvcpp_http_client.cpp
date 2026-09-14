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
#include <sstream>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>

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
    int init_rc = tcp_->connect(host, port, [this, cb](int status) {
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
    if (init_rc != 0) {
      set_status(HTTP_CLIENT_ERROR);
      last_error_code_ = init_rc;
      cb(init_rc);
      return init_rc;
    }
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

  int init_rc = tcp_->connect(host, port, [&done, &result](int status) {
    result = status;
    done = true;
  });
  if (init_rc != 0) {
    // 连接未能发起（例如主机名解析失败）——立即返回，避免空等超时
    last_error_code_ = init_rc;
    return init_rc;
  }

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
#if UVCPP_OPENSSL_ENABLE
  if (ssl_enabled_ && ssl_) {
    return send_wait_ssl(req, resp, timeout_ms);
  }
  // 纯 HTTP 同步请求也走阻塞式 socket I/O，完全绕开 libuv 的异步 read/close
  // 路径，规避连接关闭时析构 close-dance 引发的内存损坏。
  return send_wait_plain(req, resp, timeout_ms);
#else
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
#endif
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

// 设置 socket 阻塞/非阻塞（同步 SSL 路径用）
static void set_socket_blocking(uv_os_sock_t fd, bool blocking) {
#ifdef _WIN32
  u_long mode = blocking ? 0UL : 1UL;
  ioctlsocket(fd, FIONBIO, &mode);
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (blocking) flags &= ~O_NONBLOCK; else flags |= O_NONBLOCK;
  fcntl(fd, F_SETFL, flags);
#endif
}

// 解析 HTTP 响应头，小写化后查找指定 header 的值（返回空串表示不存在）
static std::string get_header_value(const std::string& head,
                                    const std::string& name) {
  std::string lower;
  lower.resize(head.size());
  std::transform(head.begin(), head.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::string needle = name + ":";
  size_t pos = lower.find(needle);
  if (pos == std::string::npos) return "";
  size_t lineEnd = lower.find("\r\n", pos);
  size_t valStart = pos + needle.size();
  size_t valEnd = (lineEnd == std::string::npos) ? lower.size() : lineEnd;
  std::string val = head.substr(valStart, valEnd - valStart);
  // 去除首尾空白
  size_t b = val.find_first_not_of(" \t");
  size_t e = val.find_last_not_of(" \t");
  if (b == std::string::npos) return "";
  return val.substr(b, e - b + 1);
}

// 把响应头块（不含空行的部分）解析进 uvcpp_http_response：
// 状态行 + 逐行 header。阻塞式收发路径（send_wait_plain / send_wait_ssl）
// 读的是裸字节，必须自己把 header 落进 resp —— 否则调用方
// `resp.get_header(...)` 永远是空串，而**异步路径**（on_response_complete）
// 用的是 `parser_->get_headers()`，两条路径的可见性天差地别。
//
// 这个差异一直存在，只是没被看见：`uvcpp_http_client::send_wait` 在
// `UVCPP_OPENSSL_ENABLE` 构建里**总是**走阻塞路径，所以凡是断言响应头的用例
// （content-encoding / allow / server / content-length / etag …）都只在
// 不开 OpenSSL 的构建里绿 —— 那正是 build-webapp 54/54 而 build-ssl 一批
// 失败的真正原因，与压缩本身无关。
static void parse_response_head(const std::string& head,
                                uvcpp_http_response& resp) {
  resp.headers.clear();

  std::istringstream iss(head);
  std::string line;
  bool first = true;
  while (std::getline(iss, line)) {
    if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
    if (first) {
      first = false;
      // 状态行：HTTP/1.1 200 OK
      size_t s1 = line.find(' ');
      if (s1 != std::string::npos) {
        size_t s2 = line.find(' ', s1 + 1);
        std::string code = line.substr(
            s1 + 1, (s2 == std::string::npos) ? std::string::npos : (s2 - s1 - 1));
        int status = 0;
        try { status = std::stoi(code); } catch (...) { status = 0; }
        if (status > 0) {
          resp.status_code = static_cast<http_status>(status);
          resp.status_message = http_status_reason(resp.status_code);
        }
        if (s2 != std::string::npos) {
          std::string reason = line.substr(s2 + 1);
          if (!reason.empty()) resp.status_message = reason;
        }
      }
      continue;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) continue;
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    size_t b = value.find_first_not_of(" \t");
    size_t e = value.find_last_not_of(" \t");
    value = (b == std::string::npos) ? std::string() : value.substr(b, e - b + 1);
    resp.headers.push_back({name, value});
  }
}

// 分块传输编码解码（RFC 7230 §4.1）
static std::string dechunk_body(const std::string& raw) {
  std::string out;
  size_t pos = 0;
  while (pos < raw.size()) {
    // 读 chunk 大小行（十六进制，可带扩展），以 CRLF 结束
    size_t crlf = raw.find("\r\n", pos);
    if (crlf == std::string::npos) break;
    std::string sizeStr = raw.substr(pos, crlf - pos);
    size_t semi = sizeStr.find(';');
    if (semi != std::string::npos) sizeStr = sizeStr.substr(0, semi);
    unsigned long chunkSize = std::strtoul(sizeStr.c_str(), nullptr, 16);
    pos = crlf + 2;
    if (chunkSize == 0) break;  // 终止 chunk
    if (pos + chunkSize > raw.size()) break;
    out.append(raw, pos, chunkSize);
    pos += chunkSize;
    // 跳过 chunk 数据后的 CRLF
    if (pos + 2 <= raw.size() && raw.compare(pos, 2, "\r\n") == 0) pos += 2;
  }
  return out;
}

// 判断从 pos 开始是不是一段**完整**的 chunked 体（走到终止块 0\r\n\r\n）。
// 用途是"够不够"，不是解码本身 —— 解码仍走 dechunk_body。
static bool chunked_complete(const std::string& s, size_t pos) {
  while (pos < s.size()) {
    size_t crlf = s.find("\r\n", pos);
    if (crlf == std::string::npos) return false;  // 大小行还没收全
    std::string sizeStr = s.substr(pos, crlf - pos);
    size_t semi = sizeStr.find(';');
    if (semi != std::string::npos) sizeStr = sizeStr.substr(0, semi);
    unsigned long chunkSize = std::strtoul(sizeStr.c_str(), nullptr, 16);
    pos = crlf + 2;
    if (chunkSize == 0) return true;                   // 终止块已到
    if (pos + chunkSize + 2 > s.size()) return false;  // 数据 + 尾部 CRLF 未收全
    pos += chunkSize + 2;
  }
  return false;
}

// 从阻塞式 reader 上读满**一条** HTTP 响应报文，切出 head / body。
//
// 为什么必须按分帧规则停下来，而不是"读至 EOF"：
// 阻塞式 `*_wait` 系列原先靠"请求里强塞 `Connection: close` + 读至 EOF"来划分
// 报文边界。这条约定让**开 OpenSSL 的构建与不开的构建行为不同** ——
// 不开时 `send_wait` 走异步路径（正常 keep-alive），开了之后无条件落到这两条
// 阻塞路径，于是"同一个连接连发两个请求"必然第二个读空。测试套件里
// `keepalive_sequential_responses` 就是这么红的，而它在 build-webapp 里一直绿。
//
// 现在改成：头读满 → 看 transfer-encoding / content-length 决定还要读多少 →
// 够了一条报文就返回，多余字节不吞、连接留给下一次请求。两者都没有时才退化
// 为读至 EOF（此时上面的循环已经把数据收完，不会再阻塞）。
static bool read_one_message(const std::function<int(char*, size_t)>& rd,
                             std::string& head, std::string& body) {
  std::string all;
  char buf[8192];

  // 阶段 1：读到头部结束
  size_t hdrEnd = std::string::npos;
  while (true) {
    hdrEnd = all.find("\r\n\r\n");
    if (hdrEnd != std::string::npos) break;
    int n = rd(buf, sizeof(buf));
    if (n <= 0) break;  // EOF / 错误 / 超时 —— 不再等
    all.append(buf, static_cast<size_t>(n));
  }

  if (hdrEnd == std::string::npos) {
    // 一个字节都没读到，或对端没给完整的头就断了：head 置空、body 为空，
    // 由调用方按"没有响应"处理（状态码保留其默认值）。
    head = all;
    body.clear();
    return !all.empty();
  }

  head = all.substr(0, hdrEnd);
  const size_t bodyStart = hdrEnd + 4;

  std::string te = get_header_value(head, "transfer-encoding");
  std::string telower;
  telower.resize(te.size());
  std::transform(te.begin(), te.end(), telower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const bool chunked = telower.find("chunked") != std::string::npos;

  bool haveLen = false;
  size_t wantLen = 0;
  if (!chunked) {
    std::string cl = get_header_value(head, "content-length");
    if (!cl.empty()) {
      haveLen = true;
      wantLen = static_cast<size_t>(std::strtoul(cl.c_str(), nullptr, 10));
    }
  }

  // 阶段 2：按分帧规则读够 body
  while (true) {
    if (chunked) {
      if (chunked_complete(all, bodyStart)) break;
    } else if (haveLen) {
      if (all.size() - bodyStart >= wantLen) break;
    } else {
      break;  // 无分帧信息：阶段 1 已读到 EOF，能拿到的都拿到了
    }
    int n = rd(buf, sizeof(buf));
    if (n <= 0) break;
    all.append(buf, static_cast<size_t>(n));
  }

  std::string raw = all.substr(bodyStart);
  if (chunked) {
    body = dechunk_body(raw);
  } else if (haveLen && wantLen < raw.size()) {
    body = raw.substr(0, wantLen);
  } else {
    body = raw;
  }
  return true;
}

int uvcpp_http_client::do_ssl_handshake(int fd) {
  if (!ssl_ctx_) return -1;
  delete ssl_;
  ssl_ = new uvcpp_ssl(ssl_ctx_, fd);
  // 同步路径：把 socket 设为阻塞模式，简化握手与后续 SSL 读写
  set_socket_blocking(fd, true);
  int rc = ssl_->handshake();
  // 非阻塞 socket 下 handshake 可能返回 0（WANT_READ/WANT_WRITE），重试几次兜底
  for (int i = 0; i < 4 && rc == 0; ++i) rc = ssl_->handshake();
  return rc;
}

// ---------------------------------------------------------------------------
// 同步 SSL 发送/接收（阻塞式，仅用于 *_wait 系列）
// ---------------------------------------------------------------------------
int uvcpp_http_client::send_wait_ssl(const uvcpp_http_request& req,
                                      uvcpp_http_response& resp,
                                      int timeout_ms) {
  if (!ssl_) { set_status(HTTP_CLIENT_ERROR); last_error_code_ = -1; return -1; }

  // 设置 socket 收发超时，避免阻塞挂死
  uv_os_sock_t sock;
  if (uvcpp_handle::fileno(tcp_->get_tcp(), sock) == 0) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
  }

  // 构造请求：补 Host；**不再强塞 `Connection: close`** —— 报文边界由分帧规则
  // 划定（见 read_one_message），连接因此可以像异步路径一样复用。
  uvcpp_http_request r = req;
  if (!r.has_header("host")) r.set_header("host", host_);
  std::string raw = r.to_string();

  // 阻塞式 SSL 写入
  size_t sent = 0;
  while (sent < raw.size()) {
    int n = ssl_->write(raw.data() + sent, raw.size() - sent);
    if (n < 0) { set_status(HTTP_CLIENT_ERROR); last_error_code_ = -1; return -1; }
    if (n == 0) break;  // 阻塞 socket 下不应出现，防御性跳出
    sent += static_cast<size_t>(n);
  }

  std::string head, body;
  read_one_message([this](char* p, size_t n) { return ssl_->read(p, n); }, head, body);

  // 状态行 + 全部 header 落进 resp（两条阻塞路径共用，见 parse_response_head）
  parse_response_head(head, resp);
  resp.body.clear();
  resp.body.clone_data(body.data(), body.size());
  set_status(HTTP_CLIENT_COMPLETE);
  clear_status(HTTP_CLIENT_ERROR);
  return 0;
}

// ---------------------------------------------------------------------------
// 同步纯 HTTP 发送/接收（阻塞式 socket I/O，按分帧规则划报文边界）
// ---------------------------------------------------------------------------
int uvcpp_http_client::send_wait_plain(const uvcpp_http_request& req,
                                        uvcpp_http_response& resp,
                                        int timeout_ms) {
  uv_os_sock_t sock;
  if (uvcpp_handle::fileno(tcp_->get_tcp(), sock) != 0) {
    set_status(HTTP_CLIENT_ERROR);
    last_error_code_ = -1;
    return -1;
  }

  // 把 socket 设为阻塞模式，并设置收发超时，避免挂死
  set_socket_blocking(sock, true);
#ifdef _WIN32
  DWORD tv = static_cast<DWORD>(timeout_ms);
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

  // 构造请求：补 Host；**不再强塞 `Connection: close`**（理由同 send_wait_ssl）
  uvcpp_http_request r = req;
  if (!r.has_header("host")) r.set_header("host", host_);
  std::string raw = r.to_string();

  // 阻塞式写入
  size_t sent = 0;
  while (sent < raw.size()) {
    int n = static_cast<int>(::send(sock, raw.data() + sent,
                                    static_cast<int>(raw.size() - sent), 0));
    if (n <= 0) { set_status(HTTP_CLIENT_ERROR); last_error_code_ = -1; return -1; }
    sent += static_cast<size_t>(n);
  }

  std::string head, body;
  read_one_message(
      [sock](char* p, size_t n) {
        return static_cast<int>(::recv(sock, p, static_cast<int>(n), 0));
      },
      head, body);

  // 状态行 + 全部 header 落进 resp（两条阻塞路径共用，见 parse_response_head）
  parse_response_head(head, resp);
  resp.body.clear();
  resp.body.clone_data(body.data(), body.size());
  set_status(HTTP_CLIENT_COMPLETE);
  clear_status(HTTP_CLIENT_ERROR);
  return 0;
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
