/**
 * @file src/web/uvcpp_ws_server.cpp
 * @brief WebSocket server with HTTP Upgrade detection + handshake.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Implements the WebSocket server-side handshake (RFC 6455 Section 4):
 *   1. Detect Upgrade: websocket + Connection: Upgrade
 *   2. Compute Sec-WebSocket-Accept = base64(sha1(key + GUID))
 *   3. Send 101 Switching Protocols
 *   4. Wrap TCP client in uvcpp_ws_connection
 */

#include <web/uvcpp_ws_server.h>

#if UVCPP_WEB_ENABLE
#include <cstring>
#include <cstdint>
#include <sstream>
#include <web/uvcpp_http_parser.h>

namespace uvcpp {

// =========================================================================
// SHA-1 (FIPS 180-4) — minimal implementation for WS handshake
// =========================================================================

static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64(const unsigned char* d, size_t n) {
  std::string o; o.reserve(((n+2)/3)*4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)d[i]<<16;
    if (i+1<n) v |= (uint32_t)d[i+1]<<8;
    if (i+2<n) v |= (uint32_t)d[i+2];
    o += B64[(v>>18)&0x3F]; o += B64[(v>>12)&0x3F];
    o += (i+1<n) ? B64[(v>>6)&0x3F] : '=';
    o += (i+2<n) ? B64[v&0x3F] : '=';
  }
  return o;
}

static inline uint32_t rotl(uint32_t x, int n) { return (x<<n)|(x>>(32-n)); }

std::string uvcpp_ws_server::sha1(const std::string& input) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  size_t ml = input.size();
  // Padding
  size_t bl = ((ml + 8 + 63) / 64) * 64;
  unsigned char* msg = new unsigned char[bl]();
  memcpy(msg, input.c_str(), ml);
  msg[ml] = 0x80;
  // Append length in bits (big-endian)
  uint64_t bits = ml * 8;
  for (int i = 0; i < 8; i++)
    msg[bl - 1 - i] = static_cast<unsigned char>((bits >> (i * 8)) & 0xFF);

  for (size_t off = 0; off < bl; off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = ((uint32_t)msg[off+i*4]<<24)|((uint32_t)msg[off+i*4+1]<<16)|((uint32_t)msg[off+i*4+2]<<8)|msg[off+i*4+3];
    for (int i = 16; i < 80; i++)
      w[i] = rotl(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i<20)       { f=(b&c)|((~b)&d); k=0x5A827999; }
      else if (i<40)  { f=b^c^d; k=0x6ED9EBA1; }
      else if (i<60)  { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
      else            { f=b^c^d; k=0xCA62C1D6; }
      uint32_t t = rotl(a,5) + f + e + k + w[i];
      e=d; d=c; c=rotl(b,30); b=a; a=t;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
  }
  delete[] msg;
  unsigned char digest[20];
  for (int i = 0; i < 5; i++) {
    digest[i*4]   = static_cast<unsigned char>((h[i]>>24)&0xFF);
    digest[i*4+1] = static_cast<unsigned char>((h[i]>>16)&0xFF);
    digest[i*4+2] = static_cast<unsigned char>((h[i]>>8)&0xFF);
    digest[i*4+3] = static_cast<unsigned char>(h[i]&0xFF);
  }
  return std::string(reinterpret_cast<char*>(digest), 20);
}

// =========================================================================
// Accept key computation — RFC 6455 Section 4.2.2
// =========================================================================

static const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string uvcpp_ws_server::compute_accept_key(const std::string& client_key) {
  std::string combined = client_key + WS_GUID;
  std::string hash = sha1(combined);
  return base64(reinterpret_cast<const unsigned char*>(hash.c_str()), hash.size());
}

// =========================================================================
// Construction
// =========================================================================

uvcpp_ws_server::uvcpp_ws_server() {
  http_server_ = new uvcpp_http_server();
  owns_http_ = true;
  // Register upgrade handler without touching owns_http_
  http_server_->on_upgrade([this](uvcpp_http_request& req, uvcpp_tcp_client* client) {
    handle_upgrade(req, client);
  });
}

uvcpp_ws_server::~uvcpp_ws_server() {
  if (owns_http_ && http_server_) {
    delete http_server_;
    http_server_ = nullptr;
  }
}

// =========================================================================
// Standalone bind / listen
// =========================================================================

int uvcpp_ws_server::bind(const char* ip, int port) {
  return http_server_->bind(ip, port);
}

int uvcpp_ws_server::listen(int backlog) {
  return http_server_->listen(backlog);
}

// =========================================================================
// Attach to existing HTTP server
// =========================================================================

void uvcpp_ws_server::attach(uvcpp_http_server* http) {
  if (owns_http_ && http_server_) delete http_server_;
  http_server_ = http;
  owns_http_ = false;

  http->on_upgrade([this](uvcpp_http_request& req, uvcpp_tcp_client* client) {
    handle_upgrade(req, client);
  });
}

// =========================================================================
// Upgrade handler
// =========================================================================

void uvcpp_ws_server::handle_upgrade(uvcpp_http_request& req,
                                     uvcpp_tcp_client* client) {
  std::string ws_key = http_get_header(req.headers, "sec-websocket-key");
  if (ws_key.empty()) return;

  std::string accept = compute_accept_key(ws_key);
  std::ostringstream oss;
  oss << "HTTP/1.1 101 Switching Protocols\r\n"
      << "Upgrade: websocket\r\n"
      << "Connection: Upgrade\r\n"
      << "Sec-WebSocket-Accept: " << accept << "\r\n"
      << "\r\n";
  std::string wire = oss.str();

  // Write 101, then start WS connection from the write callback
  // (outside the read callback — safe to call read_start here)
  client->write(wire.c_str(), wire.size(), [this, client](int) {
    auto* conn = new uvcpp_ws_connection(client);
    conn->start();
    if (on_conn_) on_conn_(conn);
  });
}

// =========================================================================
// Connection callback
// =========================================================================

void uvcpp_ws_server::on_connection(std::function<void(uvcpp_ws_connection*)> cb) {
  on_conn_ = std::move(cb);
}

// =========================================================================
// Loop / accessors
// =========================================================================

int uvcpp_ws_server::run(uv_run_mode md) { return http_server_->run(md); }
void uvcpp_ws_server::stop(std::function<void()> on_stopped) { http_server_->stop(on_stopped); }
uvcpp_http_server* uvcpp_ws_server::get_http_server() { return http_server_; }

}  // namespace uvcpp
#endif  // UVCPP_WEB_ENABLE
