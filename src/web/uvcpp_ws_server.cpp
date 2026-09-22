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
#include <net/uvcpp_loop_worker.h>
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
  // 没 key 就没有合法的 accept（本函数算出来的是 GUID 的哈希，写进应答只会
  // 让对端以一个看不懂的理由拒绝）。调用方按"缺 key"处理，见 handle_upgrade。
  if (client_key.empty()) return std::string();
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

uvcpp_ws_server::uvcpp_ws_server(uvcpp_http_server* http) {
  // **不装 on_upgrade** —— 分派权留在调用方手里，见头文件里的说明。
  // 也**不接管** http 的生命周期：它由调用方拥有，本对象只借用。
  http_server_ = http;
  owns_http_ = false;
}

uvcpp_ws_server::~uvcpp_ws_server() {
  // **会话必须先于 http 层回收。** 每个会话都挂着一个指向 TCP 客户端的关闭
  // 观察者，而那些客户端归 http 层底下的 tcp_server 管；http 层一旦拆掉，
  // 观察者就成了指向已释放客户端的悬垂回调，而终结时正好要摘掉它。
  //
  // 这一步是**兜底**：正常情况下会话早已在关闭时被回收，只有"循环停了、
  // 终结回调没来得及来"或者"属主直接析构"时才真的删到东西。
  //
  // 用 shutdown()（而不只是 recycle_all()）：它顺带把延迟回收用的 async
  // 句柄释放掉，而这件事必须发生在**销毁 loop 之前** —— loop 归下面的
  // http 层所有，`delete http_server_` 就会把它关掉。
  // 每循环那几片：正常路径上框架已经在每一格收尾时各调过一次
  // `shutdown_sessions_of_loop()`（那些片剩下的是空表 + 空句柄，这次是空操作）。
  // 这里兜的是"没人走过停机状态机"那条路 —— 单循环下它由 `delete http_` 之前
  // 的这一句负责，与从前逐字相同。
  std::vector<uvcpp_ws_sessions*> shards = shard_snapshot();
  for (size_t i = 0; i < shards.size(); ++i) shards[i]->shutdown();

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
  handle_upgrade(req, client, nullptr);
}

void uvcpp_ws_server::handle_upgrade(
    uvcpp_http_request& req, uvcpp_tcp_client* client,
    std::function<void(uvcpp_ws_connection*)> on_ready) {
  std::string ws_key = http_get_header(req.headers, "sec-websocket-key");
  if (ws_key.empty()) return;

  // ---- permessage-deflate 协商（RFC 7692 §7.1.2）----
  // `http_get_header` 是大小写不敏感的（走 `http_name_equal`），头部名不必
  // 自己降大小写。协商失败**不是**握手失败：不应答这个扩展、退化成普通 WS。
  uvcpp_ws_deflate_params dp;
#if UVCPP_ZLIB_ENABLE
  {
    std::vector<uvcpp_ws_ext_offer> offers = ws_parse_extensions(
        http_get_header(req.headers, "sec-websocket-extensions"));
    dp = ws_deflate_accept_server(offers, deflate_cfg_);
  }
#endif

  std::string accept = compute_accept_key(ws_key);
  std::ostringstream oss;
  oss << "HTTP/1.1 101 Switching Protocols\r\n"
      << "Upgrade: websocket\r\n"
      << "Connection: Upgrade\r\n"
      << "Sec-WebSocket-Accept: " << accept << "\r\n";
#if UVCPP_ZLIB_ENABLE
  if (dp.accepted) {
    oss << "Sec-WebSocket-Extensions: " << ws_deflate_response_header(dp) << "\r\n";
  }
#endif
  oss << "\r\n";
  std::string wire = oss.str();

  // Write 101, then start WS connection from the write callback
  // (outside the read callback — safe to call read_start here)
  client->write(wire.c_str(), wire.size(),
                [this, client, dp, on_ready](int status) {
    // 101 没写出去（对端已经断了 / 写失败）就到此为止：连接已经没有意义，
    // 不该再在它上面建一个 WS 会话并回调用户。原先这里忽略 status，照样建
    // 连接、照样回调，用户拿到的是一个发不出也收不到的会话。
    if (status != 0) return;

    // 延迟回收的驱动循环。这里（而不是构造函数里）建 async 句柄是有意的：
    // `uv_async_init` 必须在**循环线程**上做，而本函数是升级回调，正好在
    // 循环线程、且循环正在跑。构造函数里可能在别的线程、循环也可能还没起来。
    // 这一片就是**这条连接所属**的那条循环。句柄也必须建在它上面 ——
    // 从前这里取的是 `http_server_->get_tcp_server()->get_loop()`，也就是
    // **接受者**那条：多循环下连接是转手到工作循环上的，那个句柄会建在别人的
    // 循环上，于是"唤醒回收"踢的是另一条循环，而挂在它上面的会话永远等不到
    // 回收。`client->get_loop()` 才是这条连接自己的循环。
    uvcpp_ws_sessions* shard = shard_for(client->loop_index());
    if (shard != nullptr && client->get_loop() != nullptr) {
      shard->set_loop(client->get_loop());
    }

    auto* conn = new uvcpp_ws_connection(client, ws_role::SERVER);
    // **先接管所有权，再 start()。** 反过来的话，如果对端在我们 start() 的
    // 过程中就断了（关闭观察者立刻回调），会话会不知道把自己交给谁。
    if (shard != nullptr) shard->adopt(conn);
#if UVCPP_ZLIB_ENABLE
    // 双侧**必须**用同一份协商结果：应答里写了什么，本端的压缩器和解压器
    // 就得按那个配。is_server = true 决定窗口位数与 context takeover 的方向。
    if (dp.accepted) conn->enable_compression(true, dp);
#endif
    // 升级请求和第一帧挤在同一个 TCP 段里是常态（真实客户端就是这么发的）：
    // 那批字节已经被 HTTP 解析器那次读取走了，只能在这里补投给新会话 —— 不补
    // 投就是**丢首帧**，而且丢得无声无息（连接好好的，只有第一帧没了）。
    // 必须在**这一刻**取：升级回调是在解析器 `execute()` 里面同步调的，那时
    // 剩余字节还没算出来（见 `take_upgrade_leftover` 的说明）。
    std::string pending;
    if (http_server_ != nullptr) pending = http_server_->take_upgrade_leftover(client);
    conn->start();
    // on_ready 优先：它是**这一条**升级的专属闭包，而 on_conn_ 是全局单槽。
    if (on_ready) {
      on_ready(conn);
    } else if (on_conn_) {
      on_conn_(conn);
    }
    // 补投放在**用户回调之后**：用户正是在那里装 `on_text` 之类，装晚了的
    // 话这一帧就派发给空回调了（等于又丢一次）。
    conn->feed_pending(pending.data(), pending.size());
  });
}

#if UVCPP_ZLIB_ENABLE
void uvcpp_ws_server::set_compression(const uvcpp_ws_deflate_config& cfg) {
  deflate_cfg_ = cfg;
}
uvcpp_ws_deflate_config uvcpp_ws_server::get_compression() const {
  return deflate_cfg_;
}
#endif

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

void uvcpp_ws_server::close_all_sessions(ws_close_code code) {
  // **本线程所在循环那一片**（不在循环线程上时是 0 号，与全仓"答 0 号"那条
  // 回落一致）。多循环下这条 API 不再是"全进程"的意思，要每条循环各调一次 ——
  // 理由与 `uvcpp_http_server::begin_h2_goaway()` 逐字相同（关闭是循环亲和的）。
  // 框架自己的停机不走这里，它逐格调 `close_sessions_of_loop()`。
  close_sessions_of_loop(shard_index_here(), code);
}

void uvcpp_ws_server::close_sessions_of_loop(int loop_index,
                                             ws_close_code code) {
  uvcpp_ws_sessions* shard = shard_if_exists(loop_index);
  if (shard == nullptr) return;
  shard->close_all(code);
}

void uvcpp_ws_server::shutdown_sessions_of_loop(int loop_index) {
  uvcpp_ws_sessions* shard = shard_if_exists(loop_index);
  if (shard == nullptr) return;
  shard->shutdown();
}

void uvcpp_ws_server::abandon_sessions_of_loop(int loop_index) {
  uvcpp_ws_sessions* shard = shard_if_exists(loop_index);
  if (shard == nullptr) return;
  shard->abandon();
}

int uvcpp_ws_server::shard_index_here() {
  // `-1` ＝ "不在任何循环线程上"，与全仓"不在循环线程上就答 0 号"那条回落
  // 一致（0 号是接受者；单循环下它就是唯一那一片 ⇒ 与拆分前逐字相同）。
  const int idx = uvcpp_loop_index_of_this_thread();
  return idx > 0 ? idx : 0;
}

uvcpp_ws_sessions* uvcpp_ws_server::shard_for(int loop_index) {
  if (loop_index < 0) return nullptr;
  std::lock_guard<std::mutex> lk(shards_mu_);
  const size_t i = static_cast<size_t>(loop_index);
  if (i >= shards_.size()) {
    shards_.resize(i + 1);
  }
  if (shards_[i] == nullptr) {
    shards_[i].reset(new uvcpp_ws_sessions());
  }
  return shards_[i].get();
}

uvcpp_ws_sessions* uvcpp_ws_server::shard_if_exists(int loop_index) const {
  if (loop_index < 0) return nullptr;
  std::lock_guard<std::mutex> lk(shards_mu_);
  const size_t i = static_cast<size_t>(loop_index);
  if (i >= shards_.size()) return nullptr;
  return shards_[i].get();
}

std::vector<uvcpp_ws_sessions*> uvcpp_ws_server::shard_snapshot() const {
  // 加锁只护 `shards_` 这个 vector：已经建好的片在 `~uvcpp_ws_server` 之前
  // 不会消失，所以把裸指针拷出来之后再逐个操作是安全的（并发新建只会让快照
  // 少看见一片，那是"读数少算一片"而不是悬垂）。
  //
  // **空洞必须滤掉，不能原样返回。** `shard_for()` 是按需 `resize()` 的：建
  // 1 号片会顺带把 0 号那一格也造出来留在 `nullptr`（多循环下 0 号是接受者，
  // 一条连接都不留，所以它**永远**不会被建）。把这些空指针放进快照，调用方
  // （`session_count()` / `recycled_session_count()` / 析构里的逐个
  // `shutdown()`）就会对着 `nullptr` 解引用 —— 实测就是 n=2 下建第一条 WS
  // 会话时 `mov rax,[rcx+38h]` 打在 `rcx = 0` 上。
  //
  // 滤掉是**正确**的语义而不只是防崩：空的那些格本来就等于"这片不存在"，
  // 与 `shard_if_exists()` 对越界下标答 `nullptr` 是同一条读法。
  std::lock_guard<std::mutex> lk(shards_mu_);
  std::vector<uvcpp_ws_sessions*> out;
  out.reserve(shards_.size());
  for (size_t i = 0; i < shards_.size(); ++i) {
    if (shards_[i] != nullptr) out.push_back(shards_[i].get());
  }
  return out;
}

void uvcpp_ws_server::stop(std::function<void()> on_stopped) {
  // **先给会话发 Close 帧，再停服务。** 顺序反过来（先停循环）的话，Close 帧
  // 就永远发不出去了 —— 对端只看到一条被 RST 的连接，而 RFC 6455 §7.1.4 要求
  // 优雅关闭必须走 Close 握手。帧发出去需要几轮循环，所以这一步只是**发起**：
  // 真正的回收由会话终结回调完成，循环立刻停掉的那些由析构兜底。
  //
  // 1001 GOING_AWAY：`stop()` 的唯一含义就是"服务端要关了"。
  close_all_sessions(ws_close_code::GOING_AWAY);
  http_server_->stop(on_stopped);
}

uvcpp_http_server* uvcpp_ws_server::get_http_server() { return http_server_; }

size_t uvcpp_ws_server::session_count() const {
  // **所有分片求和**：任何一片的 `size()` 都只是某一条循环的局部读数。
  // 求和的是原子量，所以可以从任何线程调（用例就是这么轮询它的）。
  std::vector<uvcpp_ws_sessions*> shards = shard_snapshot();
  size_t n = 0;
  for (size_t i = 0; i < shards.size(); ++i) n += shards[i]->size();
  return n;
}

size_t uvcpp_ws_server::session_count_at(int loop_index) const {
  uvcpp_ws_sessions* shard = shard_if_exists(loop_index);
  return shard != nullptr ? shard->size() : 0;
}

size_t uvcpp_ws_server::recycled_session_count() const {
  // 同上：求和，不是取某一片。
  std::vector<uvcpp_ws_sessions*> shards = shard_snapshot();
  size_t n = 0;
  for (size_t i = 0; i < shards.size(); ++i) n += shards[i]->recycled();
  return n;
}

}  // namespace uvcpp
#endif  // UVCPP_WEB_ENABLE
