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
#include <web/uvcpp_ws_ext.h>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_context.h>
#endif

namespace uvcpp {

/**
 * @brief 从 101 的**原始报文**里取一个头的值（名字大小写不敏感）。
 *
 * 这里没有把裸报文转成 `http_headers` 的入口可用（`http_get_header` 只吃
 * `http_headers`），而握手只需要一个头，手扫一遍比引一个解析器划算。
 * 复用 `token_equal` 而不是再写一遍大小写折叠：HTTP 头的比较规则只该有一处。
 */
static std::string grab_raw_header(const std::string& raw, const char* name) {
  size_t pos = raw.find("\r\n");           // 跳过状态行
  if (pos == std::string::npos) return std::string();
  pos += 2;
  while (pos < raw.size()) {
    size_t eol = raw.find("\r\n", pos);
    if (eol == std::string::npos) eol = raw.size();
    const size_t colon = raw.find(':', pos);
    if (colon != std::string::npos && colon < eol) {
      if (uvcpp_ws_ext_detail::token_equal(raw.substr(pos, colon - pos), name)) {
        size_t v  = colon + 1;
        size_t ve = eol;
        while (v < ve && (raw[v] == ' ' || raw[v] == '\t')) ++v;
        while (ve > v && (raw[ve - 1] == ' ' || raw[ve - 1] == '\t')) --ve;
        return raw.substr(v, ve - v);
      }
    }
    pos = eol + 2;
  }
  return std::string();
}

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
  // 会话终结（对端关、协议错误、自己 close、析构兜底）都要把"当前会话"清掉，
  // 否则本层会拿着一个已经终结的会话继续转发。
  sessions_.set_retire_observer(
      [this](uvcpp_ws_connection* c) { on_session_retired(c); });
}

uvcpp_ws_client::~uvcpp_ws_client() {
  // 析构里**不等待**。以前这里是
  //   `for (i < 5000) { loop_->run(UV_RUN_NOWAIT); sleep(1ms); }`
  // —— 最长 5 秒的墙钟等待，而且是在析构点上跑事件循环。析构点经常就在循环
  // 自己的回调里（会话关闭时连自己一起回收），那等于让那个回调阻塞 5 秒，
  // 与"不在循环线程上跑耗时操作、不做密集等待"直接抵触。
  //
  // 但 `uv_close` 是**延迟**的：句柄要到它自己的关闭回调跑过之后才从
  // `loop->handle_queue` 上摘下来。所以下面这几次 NOWAIT 迭代不是"等待"，
  // 而是"把已经挂起的关闭回调放掉" —— 关闭回调不需要 I/O 也不需要定时器，
  // 一两轮就到。全程没有 sleep、不看墙钟。
  //
  // 次数有界是为了**绝不卡住**：万一没放完，后面的处置与改之前"等了 5 秒也没
  // 等到"是同一条路（句柄的底层内存由 `uvcpp_handle::free_handle` 的哨兵机制
  // 接手，见那里）。
  // **判据只能是 `is_closing()`，不能带上 `!is_active()`。** 这一条与
  // `uvcpp_tcp_client` 析构里那处是同一个坑（见那里的长注释），而这里是它漏
  // 掉的一处：
  //
  //   "没在跑"（`is_active()` 假）**不等于**"已经关了"。刚 `uv_tcp_init` 出来、
  //   或者 connect 失败之后的句柄正是这个样子 —— 句柄**还在** `loop->handle_queue`
  //   上，只是没有在途 I/O。旧判据在这种状态下直接跳过关闭，于是下面
  //   `loop_close()` 返回 `UV_EBUSY`（循环关不掉），`delete loop_` 又把
  //   `uv_loop_t` 那块内存还给了分配器；紧接着 `delete tcp_` 走
  //   `uvcpp_handle::free_handle` 的"(1) init 过但没启动"那一路去 `uv_close` ——
  //   而 `uv_close` 会往 `handle->loop` 里写，那个 loop 已经释放了。
  //   *（末两句是当时的顺序，今天已不成立 —— `delete tcp_` 现在排在关循环
  //   **之前**。留着的理由是判据那条教训本身仍然成立：`is_active()` 假只说明
  //   "没在跑"，漏关句柄照样会让 `loop_close()` 拿到 `UV_EBUSY`、循环泄漏。）*
  //
  // 实测（2026-09-16，本文件临时插桩）：没连过 / 连失败之后析构，
  // `is_active()=0 is_closing()=0 handle=1`，`uv_loop_close` 返回 **-4082
  // (UV_EBUSY)**；本批的重连用例里每失败一次就复现一次。
  //
  // 先问 `get_handle()` 再问 `is_closing()`：句柄关完之后 `_handle` 会被置空，
  // 而 `is_closing()` 是把它直接交给 `uv_is_closing()` —— 它解引用。
  // ---------------------------------------------------------------------
  // **本对象是在它自己的某个回调里被析构的** —— 这是另一条完全不同的路，
  // 上面那套收尾动作在这一路上一件都不能做。判据是循环还在跑（`run()` 没
  // 返回），也就是栈上还压着某个回调。
  //
  // 这一路的三层悬垂，全部**实测**过（`on_text` 里 `delete cli`，
  // `web_ws_client_api_func.cpp` 的 [P]；Release 裸跑一次都不崩，
  // **PageHeap 下一次 SEGFAULT** —— 典型的静默破坏）：
  //
  //   1. **释放 loop**：重入的那几轮 `uv_run` 会把关闭回调放完，
  //      `uv_loop_close()` 就成功了，`~uvcpp_loop` 随即把 `uv_loop_t`
  //      还给分配器 —— 而外层那一帧 `uv_run` 返回后接着用它。**崩在这里**。
  //   2. **回收会话**：`sessions_.shutdown()` → `recycle_all()` 当场 `delete`
  //      会话，可会话的 `deliver_message()` 正是就地执行着它自己的
  //      `std::function`（就是压在我们下面那一层）——删掉正在执行的闭包。
  //   3. **`delete tcp_`**：活着的会话还引用着它。
  //
  // 所以这一路**整块交出去，一个都不拆**：loop、tcp、会话全部留给循环。
  // 这是**有意的泄漏**（与 `~uvcpp_loop` 里"`uv_loop_close()` 关不掉就不释放
  // 那块内存"同一条策略）—— 泄漏换掉三个必然发生的 use-after-free。
  //
  // 本层不禁这种写法，所以它必须**不崩**：这条是有用例钉着的（[P]）。
  if (loop_ != nullptr && loop_->is_running()) {
    sessions_.abandon();
    loop_ = nullptr;
    tcp_  = nullptr;
    return;
  }

  if (tcp_ != nullptr) {
    auto* raw = tcp_->get_tcp();
    if (raw != nullptr && raw->get_handle() != nullptr && !raw->is_closing()) {
      bool done = false;
      raw->close([&done](uvcpp_handle*) { done = true; });
      for (int i = 0; i < 64 && !done; ++i) {
        loop_->run(UV_RUN_NOWAIT);
      }
    }
  }

  // **会话要在 loop 还活着的时候回收。** 上面的关闭流程会把关闭观察者唤醒，
  // 会话随即终结并被记入待回收表 —— 正常情况下这里已经是空的。没走到那条路的
  // （对端还在连、或者客户端根本没连上过）由 recycle_all() 兜底，它逐个终结，
  // 不发 Close 帧：循环马上要关了，帧发不出去。
  //
  // shutdown() 顺带把延迟回收的 async 句柄释放掉，这也**必须**发生在
  // `loop_close()` 之前 —— 在一个已经关掉的 loop 上 `uv_close` 是未定义行为。
  sessions_.shutdown();
  // 让 async 句柄那一次 uv_close 的完成回调跑掉（不跑就只是句柄内存留在
  // 回收站里，直到 loop 关闭 —— 不影响正确性，但没必要留着）。
  //
  // 原来是固定 8 轮；改成按循环自己的判据泵（`loop_alive()`）——固定轮数是
  // "猜够不够"，而这里要的是"确实排空了"。循环上除了这个 async 还可能有别
  // 的收尾（本层下面还有 `delete tcp_`），一轮都排不掉就没得补了。
  // **判据不能用 `loop_close()`**：它内部会 `stop()`，而 `uv_run` 的
  // `while (r != 0 && loop->stop_flag == 0)` 是**进 body 之前**判的，停标志一
  // 立那一轮就整段空转。
  if (loop_ != nullptr) {
    for (int i = 0; i < 256 && loop_->loop_alive() != 0; ++i) {
      loop_->run(UV_RUN_NOWAIT);
    }
  }

  // **`tcp_` 必须趁循环还活着删掉**（同一个形状，见 `~uvcpp_http_client` 那段
  // 长注释）：`tcp_ = new uvcpp_tcp_client(loop_)` 借的是本对象的循环，
  // `~uvcpp_tcp_client` 第一件事就是 `loop_->is_running()`；先删循环再删它，
  // 那一次读就是释放后使用（完整页堆下 `0xC0000005`，裸跑看不见）。
  delete tcp_; tcp_ = nullptr;
  // 本层不再持有 `uvcpp_ssl` —— TLS 由 `tcp_` 的过滤器持有，随它一起析构。
  // 这一步之后到 `loop_close()` 之间不再泵，所以 `tcp_` 析构万一排进来的那笔
  // 关闭最多让 `loop_close()` 返回 `UV_EBUSY`，不会把已删对象的回调跑起来。
  if (loop_) { loop_->loop_close(); delete loop_; loop_ = nullptr; }
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
#if UVCPP_OPENSSL_ENABLE
  // **TLS 交给 `uvcpp_tcp_client` 自己的过滤器，本层不要再开一个 fd-based 的
  // `uvcpp_ssl` 握手。** 原先那段是
  //     new uvcpp_ssl(ctx, sock) → handshake() → if (rc <= 0) 失败
  // 两个错叠在一起：
  //
  //  1. `uvcpp_ssl::handshake()` 的契约是「1 = 握手完成，0 = **还需要更多 I/O**，
  //     < 0 = 真出错」，`uvcpp_ssl.h:87-93` 专门写着"判失败要用 `< 0`，不能用
  //     `!= 1`：0 是「还没完」，是常态"。非阻塞 socket 上第一次 `SSL_connect`
  //     **必然**返回 0（ClientHello 得先出网、ServerHello 得先回来），于是每一次
  //     `wss://` 连接都在这一句被判成失败。
  //  2. 就算把判据改成 `< 0` 也还是错：fd-based BIO 下握手要跨多轮 I/O，而在同一
  //     轮里紧接着 `tcp_->write()` 会把**明文**的 HTTP 升级请求塞进一条还没建好
  //     的 TLS 连接。
  //
  // `enable_tls()` 把两件事一起解决了：过滤层走 memory BIO（socket 由 libuv
  // 独占），握手由读事件推进，并且**客户端的 connect 回调被推迟到握手完成之后**
  // —— 见 `uvcpp_tcp_client.h:160-172`，以及 `tests/functional/web_ssl_app_ws_func.cpp:189-192`
  // 那段注释（它自己就在用 `tcp.enable_tls()` 手搓 wss，所以这条路径是有实测的）。
  // 于是下面回调里发升级请求时，TLS 一定已经建立。
  //
  // 只在还没装过时装：`enable_tls()` 第二次返回 `UV_EALREADY`。同一个 `tcp_`
  // 在重连时会复用（框架层 `uvcpp_web_ws_client::do_restart()` 是换一个全新的
  // `uvcpp_ws_client`，不走这条）。
  if (use_tls_ && !tcp_->is_tls()) {
    const int trc = tcp_->enable_tls(ssl_ctx_);
    if (trc != 0) { on_handshake_complete(trc); return; }
  }
#endif

  tcp_->connect(host.c_str(), port, [this](int st) {
    if (st != 0) { on_handshake_complete(st); return; }
    // 走到这里时，若开了 TLS，握手已经完成（`enable_tls()` 把 connect 回调
    // 推迟到了握手之后）。
    // Send HTTP upgrade request
    std::string req =
        "GET " + ws_path_ + " HTTP/1.1\r\n"
        "Host: " + ws_host_ + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + ws_key_ + "\r\n"
        "Sec-WebSocket-Version: 13\r\n";
#if UVCPP_ZLIB_ENABLE
    {
      // 提议 permessage-deflate（RFC 7692 §7.1.2）。`enabled=false` 时
      // `ws_deflate_request_header` 返回空串，这一行就不加。
      const std::string ext = ws_deflate_request_header(deflate_cfg_);
      if (!ext.empty()) req += "Sec-WebSocket-Extensions: " + ext + "\r\n";
    }
#endif
    req += "\r\n";
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
  if (handshake_buf_.find(" 101 ") == std::string::npos &&
      handshake_buf_.compare(0, 12, "HTTP/1.1 101") != 0) {
    on_handshake_complete(-2);
    handshake_buf_.clear();
    return;
  }

#if UVCPP_ZLIB_ENABLE
  // 校验服务端的扩展应答。三种结局：
  //   accepted → 启用压缩
  //   没应答   → 正常降级，普通 WS
  //   invalid  → 服务端答了但答得不对，**握手失败**（见头文件里的说明）
  {
    const std::string ext = grab_raw_header(handshake_buf_, "sec-websocket-extensions");
    deflate_params_ = ws_deflate_accept_client(ws_parse_extensions(ext), deflate_cfg_);
    if (deflate_params_.invalid) {
      handshake_buf_.clear();
      on_handshake_complete(-3);   // 应答非法：宁可不连，也不连成一个必错的会话
      return;
    }
  }
#endif

  // **头之后的字节不能丢。** 应答和第一帧常常落在同一个 TCP 段里（对端
  // "回完 101 紧接着推一条"，在 loopback 上几乎是必然），而那批字节已经被
  // 握手读回调整批取走了：这里 clear() 掉就等于把第一帧吃了 —— 表现是"连上
  // 了但第一条消息永远不来"，而它跟"对端没发"长得一模一样。留下来的这段由
  // `on_handshake_complete` 补投给新会话（`uvcpp_ws_connection::start`）。
  handshake_tail_.assign(handshake_buf_, end + 4, std::string::npos);
  handshake_buf_.clear();
  on_handshake_complete(0);
}

void uvcpp_ws_client::on_handshake_complete(int error) {
  // 补投用的那一段先搬出来：下面每个分支都不该再留着它（失败分支等于丢掉，
  // 那是应该的 —— 连接都不成立了）。
  const std::string tail = std::move(handshake_tail_);
  handshake_tail_.clear();

  if (error == 0) {
    if (has_status(WS_CLIENT_CLOSED) || has_status(WS_CLIENT_CLOSING)) {
      // 握手完成之前调用方就 `close()` 了（取消那次已经在 `close()` 里结算
      // 过，这里只是运输层还来不及被拆掉）。这次连接**不算成立**：会话照旧
      // 接管（生命周期归 `sessions_`），但立刻关掉，回调不叫第二次。
      auto* cancelled = new uvcpp_ws_connection(tcp_);
      sessions_.adopt(cancelled);
      cancelled->close(ws_close_code::NORMAL);
      return;
    }
    status_ = WS_CLIENT_OPEN;
    // 延迟回收的驱动循环。在这里（握手完成回调，也就是循环线程上）建 async
    // 句柄是有意的：`uv_async_init` 必须在循环线程上做，而 `connect()` 可能
    // 是从别的线程调进来的（那时候句柄还没法建）。
    sessions_.set_loop(loop_);
    auto* conn = new uvcpp_ws_connection(tcp_);
    // **先接管所有权再 start()**：反过来的话，对端若在我们 start() 的过程中
    // 就断了（关闭观察者立刻回调），会话会不知道把自己交给谁。
    sessions_.adopt(conn);
    session_ = conn;
    // 回调要在 start() **之前**装好：对端可能一升级完就把第一帧发过来了。
    install_callbacks(conn);
#if UVCPP_ZLIB_ENABLE
    // 与服务端对称：应答里谈定了什么，两边就得按那个配。is_server=false
    // 决定窗口位数与 context takeover 的方向 —— 搞反不会报错，只会解出乱码。
    if (deflate_params_.accepted) conn->enable_compression(false, deflate_params_);
#endif
    // **这里必须 arm 读**，不能像原先那样"等第一次发帧时再懒启动"：只收不发的
    // 客户端（订阅、推送、纯监听）从头到尾不会调 send_frame，于是一帧都读不到
    // —— 表现是"连上了但永远没有消息"，而它跟"对端没发"长得一模一样。
    // 服务端本来就是升级完直接 start()（`uvcpp_ws_server.cpp:230`），这里与它
    // 对齐；`send_frame` 里那句懒启动保留，作为别的调用路径的兜底（幂等）。
    conn->start();
    if (connect_cb_) { auto cb = std::move(connect_cb_); connect_cb_ = nullptr; cb(conn, 0); }
    // 同一段里跟着 101 一起到达的字节（`handshake_tail_`）在这里补投 —— 它们
    // 已经从内核缓冲里被握手读回调取走了，重新读是读不回来的。放在 connect
    // 回调**之后**：调用方可能正是在那个回调里装收消息的回调（本层的转发回调
    // 虽然早装好了，但转发到的是那一刻才有的那个 `std::function`）。
    // 回调里把客户端关掉/删掉的话，`feed_pending` 自己会挡掉。
    conn->feed_pending(tail.data(), tail.size());
  } else {
    // 调用方自己 `close()` 取消的那次连接**不是**"连接出错"：终态保持 CLOSED
    // （取消时已经结算过回调了，这里叫不到第二次）。
    if (!has_status(WS_CLIENT_CLOSED)) status_ = WS_CLIENT_ERROR;
    last_error_ = error;
    if (connect_cb_) { auto cb = std::move(connect_cb_); connect_cb_ = nullptr; cb(nullptr, error); }
  }
}

// =========================================================================
// 会话 / 收发 / 关闭：转发给当前会话
// =========================================================================

void uvcpp_ws_client::install_callbacks(uvcpp_ws_connection* conn) {
  if (conn == nullptr) return;
  // 没设过的槽跳过：装一个空的 std::function 与"没装"等价，只是白占一层。
  if (on_text_)  conn->on_text(on_text_);
  if (on_bin_)   conn->on_binary(on_bin_);
  if (on_close_) conn->on_close(on_close_);
  if (on_error_) conn->on_error(on_error_);
}

void uvcpp_ws_client::on_session_retired(uvcpp_ws_connection* conn) {
  // 同时只可能有一个会话，这句只为让"清错对象"不可能发生。
  if (conn != session_) return;
  session_ = nullptr;
  // 会话没了就是这个状态 —— 不管是本端 `close()` 发起的，还是对端走的。
  // `WS_CLIENT_CLOSING` 只是"已经发起、还没结束"，终结观察者就是它的落点。
  status_ = WS_CLIENT_CLOSED;
}

uvcpp_ws_connection* uvcpp_ws_client::session() const { return session_; }

int uvcpp_ws_client::send_text(const char* data, size_t len,
                               std::function<void(int)> cb) {
  if (session_ == nullptr) {
    if (cb) cb(UV_ENOTCONN);
    return UV_ENOTCONN;
  }
  return session_->send_text(data, len, std::move(cb));
}

int uvcpp_ws_client::send_binary(const char* data, size_t len,
                                 std::function<void(int)> cb) {
  if (session_ == nullptr) {
    if (cb) cb(UV_ENOTCONN);
    return UV_ENOTCONN;
  }
  return session_->send_binary(data, len, std::move(cb));
}

void uvcpp_ws_client::on_text(std::function<void(const std::string&)> cb) {
  on_text_ = std::move(cb);
  // 已经连上了就当场装上（"connect 之后再装"和"connect 之前装"都得成立）。
  if (session_ != nullptr) session_->on_text(on_text_);
}

void uvcpp_ws_client::on_binary(std::function<void(const uint8_t*, size_t)> cb) {
  on_bin_ = std::move(cb);
  if (session_ != nullptr) session_->on_binary(on_bin_);
}

void uvcpp_ws_client::on_close(
    std::function<void(ws_close_code, const std::string&)> cb) {
  on_close_ = std::move(cb);
  if (session_ != nullptr) session_->on_close(on_close_);
}

void uvcpp_ws_client::on_error(std::function<void(int, const std::string&)> cb) {
  on_error_ = std::move(cb);
  if (session_ != nullptr) session_->on_error(on_error_);
}

void uvcpp_ws_client::close(ws_close_code code, const std::string& reason) {
  if (status_ == WS_CLIENT_CLOSED || status_ == WS_CLIENT_CLOSING) return;

  if (session_ != nullptr) {
    session_->close(code, reason);
    // 真正的结束由终结观察者落点（对端回 Close、或连接关掉）。
    status_ = WS_CLIENT_CLOSING;
    return;
  }

  // 还没有会话（连接建立中，或者从没连上过）：没有 Close 帧可发 —— 关底层
  // 连接。建立中的话这就是**取消**这次连接，状态**直接落终态**：没有任何在途
  // 操作能把它推到 CLOSED，留在 CLOSING 就是个谎（"还在关"而其实已经关了）。
  if (tcp_ != nullptr) tcp_->close();
  status_ = WS_CLIENT_CLOSED;

  // 正在等 connect 回调的调用方必须收到结果，不能悬着。底层取消会不会回一个
  // UV_ECANCELED 是实现细节（libuv 的 uv_close 会取消在途请求，但那条路依赖
  // 句柄还没被回收），所以这里自己结算一次；底下真也回来了的话
  // `connect_cb_` 已经被搬空，不会再叫第二次。
  if (connect_cb_) {
    auto cb = std::move(connect_cb_);
    connect_cb_ = nullptr;
    last_error_ = UV_ECANCELED;
    cb(nullptr, UV_ECANCELED);
  }
}

int uvcpp_ws_client::run(uv_run_mode md) { return loop_->run(md); }
void uvcpp_ws_client::stop() { loop_->stop(); }
uvcpp_loop* uvcpp_ws_client::get_loop() { return loop_; }
int uvcpp_ws_client::get_status() const { return status_; }
bool uvcpp_ws_client::has_status(int flags) const { return (status_ & flags) == flags; }
size_t uvcpp_ws_client::session_count() const { return sessions_.size(); }
size_t uvcpp_ws_client::recycled_session_count() const { return sessions_.recycled(); }
int uvcpp_ws_client::get_last_error() const { return last_error_; }

#if UVCPP_ZLIB_ENABLE
void uvcpp_ws_client::set_compression(const uvcpp_ws_deflate_config& cfg) {
  deflate_cfg_ = cfg;
}
uvcpp_ws_deflate_config uvcpp_ws_client::get_compression() const {
  return deflate_cfg_;
}
#endif

#if UVCPP_OPENSSL_ENABLE
void uvcpp_ws_client::set_ssl_context(uvcpp_ssl_context* ctx) { ssl_ctx_ = ctx; }
#endif

}  // namespace uvcpp
#endif
