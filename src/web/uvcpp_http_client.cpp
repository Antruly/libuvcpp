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
#include <vector>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl.h>
#include <ssl/uvcpp_ssl_context.h>
#endif

// h2 的头只在这一层出现。公开头里只有前置声明 —— 见 uvcpp_http_client.h 那段。
#if UVCPP_NGHTTP2_ENABLE
#include <http2/uvcpp_h2_connection.h>
#include <http2/uvcpp_h2_session.h>
#endif

namespace uvcpp {

// =========================================================================
// Construction / Destruction
// =========================================================================

uvcpp_http_client::uvcpp_http_client() {
  loop_   = new uvcpp_loop();
  tcp_    = new uvcpp_tcp_client(loop_);
  parser_ = new uvcpp_http_parser(http_parser_mode::PARSE_RESPONSE);
  // **对端在响应收完之前断开，只有这条路看得见。** 本层注册的是传统
  // `read_start`（`send()` 里那条），而传统式在 `nread < 0` 时**不调数据回调**，
  // 只跑 `fire_close_callbacks()`（`uvcpp_tcp_client.cpp:1280`）—— 不装观察者
  // 就是"连接没了、`send()` 的回调永远不来、调用方无限等"（异步路径没有超时）。
  // 观察者是**叠加**的，不碰数据通路；这也是 `uvcpp_ws_connection` 的做法。
  //
  // 不改成 `read_start_events`：它与传统式互斥（`UV_EALREADY`），而 h2 层在
  // 同一个 `tcp_` 上装的正是事件式 —— 换了会让 `start_h2()` 当场自毁。
  close_observer_id_ = tcp_->add_close_observer([this]() { on_tcp_close(); });
}

uvcpp_http_client::~uvcpp_http_client() {
  // 第一件事就摘掉：下面关 `tcp_` 那段 dance 会 `loop_->run(UV_RUN_NOWAIT)`
  // 泵若干轮，不摘掉的话观察者有机会在**本对象析构到一半**时被叫起来。
  if (tcp_ != nullptr && close_observer_id_ != 0) {
    tcp_->remove_close_observer(close_observer_id_);
  }
  close_observer_id_ = 0;

#if UVCPP_NGHTTP2_ENABLE
  // 次序在拆 `tcp_` **之前**：h2 层往 `tcp_` 上注册过读回调（捕的是它自己的
  // `this`），而 `tcp_` 反过来不拥有它。倒了就是"回调指向已经释放的对象"。
  delete h2_;
  h2_ = nullptr;
#endif
  // Close TCP if still active
  if (tcp_ != nullptr && !has_status(HTTP_CLIENT_CLOSED)) {
    uvcpp_tcp* raw_tcp = tcp_->get_tcp();
    // 判据只能是 `is_closing()`，不能带上 `is_active()`：`uv_*_init` 过、还没
    // connect 的句柄两样都是"否"，带上去它就被判成"早就关完了"，于是 socket
    // 不关、句柄留在 handle_queue 上 —— 紧跟着的 `loop_close()` 返回 UV_EBUSY，
    // 循环内存被释放后 `delete tcp_` 里的 `uv_close()` 还往队列上写，
    // 那就是 use-after-free。实测 2026-09-16：43 处 `closed=0` 全走这条。
    if (raw_tcp != nullptr && !raw_tcp->is_closing()) {
      bool close_done = false;
      raw_tcp->close([&close_done](uvcpp_handle*) { close_done = true; });
      // **有界、不睡眠、不看墙钟**（与 `~uvcpp_tcp_client` 同一形状）。原先是
      // 5000 轮 + 每次 1 毫秒睡眠，后面还跟着一段固定的 20 × 1 毫秒 ——
      // 也就是说每一次"连着、没关就析构"都至少睡 20 毫秒，而且它正是第 10 条
      // 从 `~uvcpp_tcp_client` 里拿掉的那个形状（析构里的 `sleep_for` 就是
      // "在循环线程上跑耗时操作"）。
      //
      // 判据只能用 `close_done`，**不能**改用 `loop_alive()`：句柄 `uv_close`
      // 之后还在 `endgame_handles` 上，而它正是 `uv__loop_alive()` 的一项，
      // 拿它当条件等于把"关闭完成回调放了没有"和"循环还活着"混为一谈。
      for (int i = 0; i < 256 && !close_done; ++i) {
        loop_->run(UV_RUN_NOWAIT);
      }
    } else if (raw_tcp != nullptr && raw_tcp->is_closing()) {
      loop_->run(UV_RUN_NOWAIT);
    }
  }

  // 关之前再按循环自己的判据泵一遍：上面那几步只保证本客户端的 tcp 句柄收尾
  // 了，循环上可能还挂着别人（实测同类形状：一个只关了一半的 `async`）。
  // 判据是 `loop_alive()` 而不是 `loop_close()` —— 后者内部会 `stop()`，
  // 而 `uv_run` 的 `while (r != 0 && loop->stop_flag == 0)` 是**进 body 之前**
  // 判的，停标志一立那一轮就整段空转，交替调用等于原地打转。
  if (loop_ != nullptr) {
    for (int i = 0; i < 256 && loop_->loop_alive() != 0; ++i) {
      loop_->run(UV_RUN_NOWAIT);
    }
  }

  // **`tcp_` 必须趁循环还活着删掉，顺序不能倒过来。**
  //
  // `tcp_` 是本对象自己 `new uvcpp_tcp_client(loop_)` 出来的 —— 借的是本对象的
  // 循环（`owns_loop_ = false`），而 `~uvcpp_tcp_client` 的**第一件事**就是
  // `loop_->is_running()`（`src/net/uvcpp_tcp_client.cpp:126`，读的是
  // `uvcpp_loop::run_depth_`）。先 `delete loop_` 再 `delete tcp_`，那一次读
  // 就是**释放后使用**：裸跑因为那块页还在、读到个"否"而活着，完整页堆把释放过
  // 的页 unmap 掉就当场 `0xC0000005`。实测 2026-09-17（`tests/tools/run_pageheap_gate.py`）：
  // `build-webapp` 74 个用例里 **11 个 `web_*`** 全崩在这一句，cdb 的栈是
  // `~uvcpp_http_client+0x11a → ~uvcpp_tcp_client+0x35`（`cmp dword ptr [rax+0F8h],0`，
  // `rax` 是那个已经删掉的循环、`rcx` 才是 `this`）。
  //
  // 换过来之后为什么安全：删 `tcp_` 时循环**一定还分配着**（`delete loop_` 排在
  // 后面）；而这一步之后到 `loop_close()` 之间**不再泵任何一轮**，所以万一上面
  // 那次收尾没跑完、`tcp_` 的析构往循环上排了一笔 `uv_close`，那笔也只会让
  // `loop_close()` 返回 `UV_EBUSY`、由 `~uvcpp_loop` 按既有策略"泄漏而不释放"，
  // **不会**把已删对象的关闭回调跑起来。
  delete tcp_;
  tcp_ = nullptr;

  if (loop_ != nullptr) {
    loop_->loop_close();
    delete loop_;
    loop_ = nullptr;
  }
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
#if UVCPP_NGHTTP2_ENABLE
  // 对端道别与否是**每连接**的事实，不能跨连接留着。
  peer_goaway_      = false;
  peer_goaway_code_ = 0;
  peer_goaway_last_ = 0;
#endif

  if (cb) {
#if UVCPP_OPENSSL_ENABLE
    // TLS 必须**在 connect 之前**装上。
    //
    // `uvcpp_tcp_client` 的握手是 memory BIO + 读事件驱动的：装晚了没有人去
    // 推进它，而它把握手回调压住、直到会话真的建立才调 `cb` —— 于是下面那句
    // "回调到了就能直接发"在 TLS 上也照样成立。
    //
    // 以前这里是反过来的：先明文 connect，再在完成回调里**阻塞式**握手，
    // 然后 `send()` 把明文请求写进这条已经加密的 socket。对端按畸形记录丢掉，
    // 症状是"连上了、写成功了、永远等不到响应" —— 没有任何一处报错。
    if (ssl_enabled_ && ssl_ctx_ != nullptr) {
      if (tcp_->is_tls()) {
        // 同一个 client 上第二次 connect：底层还挂着上一条会话的密钥，
        // 复用它是"用旧密钥加密"这种静默错答案。宁可不做。
        set_status(HTTP_CLIENT_ERROR);
        last_error_code_ = UV_ENOTSUP;
        cb(UV_ENOTSUP);
        return UV_ENOTSUP;
      }
      const int trc = tcp_->enable_tls(ssl_ctx_);
      if (trc != 0) {
        set_status(HTTP_CLIENT_ERROR);
        last_error_code_ = trc;
        cb(trc);
        return trc;
      }
      // 说 HTTP/1.1，还是允许协商 h2，由 `set_http2_enabled()` 决定；**默认钉死
      // http/1.1**（理由与 `do_ssl_handshake` 那条相同）。靠"用户的 context 上
      // 大概没设 ALPN"是不行的：同一个 context 很可能同时给别的用途
      // （`set_ssl_context` 收的就是别人的 ctx），那时协商出 h2 而本层照发
      // HTTP/1.1 报文 —— 服务端按二进制帧解析，症状是"连上了、写成功了、
      // 永远等不到响应"，一处报错都没有。
      //
      // 开了 h2 也**仍然把 `http/1.1` 留在名单里**：这是协商不是强制，服务端不认
      // h2 就照常走 h1，那不是错误。
      std::vector<std::string> alpn;
#if UVCPP_NGHTTP2_ENABLE
      if (http2_enabled_) alpn.push_back("h2");
#endif
      alpn.push_back("http/1.1");
      if (!tcp_->set_tls_alpn_protos(alpn)) {
        set_status(HTTP_CLIENT_ERROR);
        last_error_code_ = UV_EINVAL;
        cb(UV_EINVAL);
        return UV_EINVAL;
      }
    }
#endif
    int init_rc = tcp_->connect(host, port, [this, cb](int status) {
      if (status != 0) { set_status(HTTP_CLIENT_ERROR); last_error_code_ = status; cb(status); return; }
      set_status(HTTP_CLIENT_CONNECTED);
      clear_status(HTTP_CLIENT_ERROR);
#if UVCPP_NGHTTP2_ENABLE
      // ALPN 是握手内谈完的，走到这里已是终局 —— 不需要嗅字节，也不需要等。
      negotiated_alpn_ = tcp_->tls_alpn_selected();
      if (negotiated_alpn_ == "h2") {
        // h2 层必须在**socket 可用之后**才建：它的 `start()` 会调
        // `read_start_events()`，而那个在没连上的 client 上返回 UV_ENOTCONN。
        const int hrc = start_h2();
        if (hrc != 0) {
          // **连 CONNECTED 一起摘掉。** 协商出来的就是 h2，只是我们起不来 ——
          // 留着 CONNECTED，后面的 `send()` 会走 h1 那条路，把 HTTP/1.1 明文写进
          // 一条对端按二进制帧解析的连接。那正是本文件反复交代过的"写成功了、
          // 响应等不到，一处报错都没有"。
          clear_status(HTTP_CLIENT_CONNECTED);
          set_status(HTTP_CLIENT_ERROR);
          last_error_code_ = hrc;
          cb(hrc);
          return;
        }
      }
#endif
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
#if UVCPP_NGHTTP2_ENABLE
  peer_goaway_      = false;
  peer_goaway_code_ = 0;
  peer_goaway_last_ = 0;
#endif

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

#if UVCPP_OPENSSL_ENABLE
  // `send_wait` 里那个守卫的**反面**，两边都要有。
  //
  // `connect_wait()` + `set_ssl_context()` 建的是本层自己的 fd-based **阻塞**
  // 会话（`do_ssl_handshake`）：密钥在 `ssl_` 里，socket 被设成阻塞模式。而这条
  // 异步路径只调 `tcp_->write()` —— 它**不经过** `ssl_`，于是明文请求进了加密
  // socket，调用方看到的是"写成功、响应永远不来"（正是 `ff107fe` 修掉的那个
  // 缺陷，只是换了入口）。附带一层：那条 socket 是阻塞的，从这里写会在循环
  // 线程上挂住。
  //
  // 只置 `last_error_code_`、不把 status 打成 ERROR —— 与上面 `UV_ENOTCONN`
  // 那条一致：客户端没坏，只是这个入口对它不适用（`send_wait()` 照旧能用）。
  if (ssl_enabled_ && ssl_ != nullptr) {
    last_error_code_ = UV_ENOTSUP;
    return UV_ENOTSUP;
  }
#endif

#if UVCPP_NGHTTP2_ENABLE
  // 协商成 h2 的连接走另一条路：`parser_` / `pending_resp_` / `user_cb_` 这套
  // 是 llhttp 的附属品，在 h2 上全是空的。响应按流 id 归位（`h2_streams_`），
  // 所以这里**不能**碰下面那些状态复位 —— 那些是按"一条连接一个在飞请求"写的。
  if (h2_active_) return send_h2(req, std::move(cb));
#endif

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
  // HEAD 的响应有 Content-Length 却没有 body（RFC 7231 §4.3.2）—— 得让解析器
  // 知道"刚发出去的是 HEAD"，否则它会一直等那个永远不来的 body，回调永远不
  // 落地，请求只会超时。必须在 reset() **之后**：llhttp_init 会把标志清掉。
  parser_->set_request_method(req.method);
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
#if UVCPP_NGHTTP2_ENABLE
  // 与"异步 connect + send_wait"是**同一条纪律**：阻塞式 socket I/O 要独占
  // socket，驱动不了 h2 那条内存 BIO 会话。这里必须报错，**不能**落到
  // `send_wait_plain` —— 那会往一条正跑着二进制帧的连接里写 HTTP/1.1 明文，
  // 症状同样是"写成功了、响应等不到"，一处报错都没有。
  if (h2_active_) {
    set_status(HTTP_CLIENT_ERROR);
    last_error_code_ = UV_ENOTSUP;
    return UV_ENOTSUP;
  }
#endif
#if UVCPP_OPENSSL_ENABLE
  if (ssl_enabled_ && ssl_) {
    return send_wait_ssl(req, resp, timeout_ms);
  }
  if (ssl_enabled_) {
    // 连接是**异步 TLS** 建的（`connect(host, port, cb)` + `set_ssl_context`）：
    // 会话在 `uvcpp_tcp_client` 的 memory BIO 里，而阻塞式的 SSL_read/SSL_write
    // 要独占 socket，接管不了它。这里必须报错，**不能**落到 send_wait_plain ——
    // 那正是本轮在修的 #2：把明文写进一条已经加密的连接，而调用方只看到
    // "请求发出去了、响应等不到"。换了个入口，错答案一模一样。
    set_status(HTTP_CLIENT_ERROR);
    last_error_code_ = UV_ENOTSUP;
    return UV_ENOTSUP;
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

void uvcpp_http_client::on_tcp_close() {
  // 只会被叫一次：`fire_close_callbacks()` 是把观察者表整个 swap 出去再跑的
  // （`uvcpp_tcp_client.cpp:1735`），跑过的观察者不会再被叫第二遍。
  //
  // **不置 `HTTP_CLIENT_CLOSED`** —— 见 `on_h2_disconnect` 那段：那个标志在本类里
  // 是"`tcp_` 的句柄已经关完了"，而这条路上句柄**还开着**（`uvcpp_tcp_client.cpp`
  // 的 `nread < 0` 分支只通知、不 `uv_close`）。置上会让析构跳过关闭那一段，
  // 句柄留在 loop 上，`loop_close()` 拿到 `UV_EBUSY`、循环内存被泄漏。
  // 清 `CONNECTED` 才是这条路上该说的话：连接没了，后面的 `send()` 应当
  // 拿到 `UV_ENOTCONN`，而不是写进一条死 socket。
  clear_status(HTTP_CLIENT_CONNECTED);

  // h2 连接上这条观察者也会响（h2 层用的是 `read_start_events`，`nread < 0`
  // 分支照样往下走 `fire_close_callbacks()`），但断开已经由 `on_h2_disconnect`
  // 结算过了，而那条路上 `user_cb_` 恒为空 —— 这里什么都不做才是对的。
  // 把错误码提到这道门外面就会把 `UV_ECANCELED` 覆盖成 h1 那套 `UV_ECONNRESET`。
  if (!user_cb_ || has_status(HTTP_CLIENT_COMPLETE)) return;

  set_status(HTTP_CLIENT_ERROR);
  last_error_code_ = UV_ECONNRESET;
  // **先挪走再调**（与 `on_response_complete` 同形状）：用户回调里可以析构本
  // 对象，调完再碰 `user_cb_` 就是往已释放的内存上写。
  std::function<void(const uvcpp_http_response&, int)> cb = std::move(user_cb_);
  user_cb_ = nullptr;
  cb(pending_resp_, UV_ECONNRESET);
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
                             std::string& head, std::string& body,
                             bool head_request) {
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

  // HEAD 的响应按 RFC 7231 §4.3.2 **没有 body** —— 不论头里写着什么
  // `Content-Length`（那描述的是对应 GET 的 body）或 `Transfer-Encoding`。
  // 少了这一句，下面 `haveLen` 那一支会一直读到超时：服务端不会发 body，
  // 我们却按头里的长度等它。异步路径的对应物是 `set_request_method()`
  // （它让 llhttp 置 F_SKIPBODY），两条路都得告诉解析器"这条不领 body"。
  if (head_request) {
    body.clear();
    return true;
  }

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

  // 显式钉死 http/1.1，而不是靠"从来没设过 ALPN"。这条路是**阻塞 + 真 fd**，
  // h2 的 TLS 层必须走内存 BIO（`uvcpp_tcp_client` 那条），两者不能混。
  // 写出来之后，将来往 ctx 上加了客户端 ALPN 也不会意外把这条路径带进 h2。
  ssl_->set_alpn_protos({"http/1.1"});
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
  read_one_message([this](char* p, size_t n) { return ssl_->read(p, n); }, head,
                   body, req.method == http_method::HTTP_HEAD);

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
      head, body, req.method == http_method::HTTP_HEAD);

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

// =========================================================================
// HTTP/2（低层手动那一层）
// =========================================================================

#if UVCPP_NGHTTP2_ENABLE

void uvcpp_http_client::set_http2_enabled(bool on) { http2_enabled_ = on; }

bool uvcpp_http_client::http2_enabled() const { return http2_enabled_; }

const std::string& uvcpp_http_client::negotiated_alpn() const {
  return negotiated_alpn_;
}

// `h2_` 还在时问活的那一份，拆了之后答抄下来的那一份 —— 两个时机都得对：
// 连接还开着的时候调用方要能**提前**知道对端在收摊（好把请求挪走），
// 断开之后要能**回头**分辨这次断开是告别还是断线。
bool uvcpp_http_client::peer_goaway_received() const {
  if (h2_ != nullptr) return h2_->session().peer_goaway_received();
  return peer_goaway_;
}

uint32_t uvcpp_http_client::peer_goaway_error_code() const {
  if (h2_ != nullptr) return h2_->session().peer_goaway_error_code();
  return peer_goaway_code_;
}

int32_t uvcpp_http_client::peer_goaway_last_stream_id() const {
  if (h2_ != nullptr) return h2_->session().peer_goaway_last_stream_id();
  return peer_goaway_last_;
}

int uvcpp_http_client::start_h2() {
  h2_ = new uvcpp_h2_connection(tcp_, /*server_side=*/false);

  uvcpp_h2_session::callbacks sc;
  // 只装收响应要用的三个。`on_request` / `on_request_end` 是服务端侧的槽
  // （`uvcpp_h2_session` 按 `server_side` 分流），客户端会话不会调它们。
  sc.on_body = [this](uvcpp_h2_session& s, uvcpp_h2_stream& st,
                      const char* d, size_t n) { on_h2_body(s, st, d, n); };
  // 交付用户的动作**只能**放这里。带 body 的响应里 `on_response` 的
  // `end_stream` 恒为 false，而 DATA 的 END_STREAM 不经过它 —— 装在那边就是
  // "有 body 的响应永远等不到回调"。没有 body 的响应（HEAD、204、304）里
  // HEADERS 自带 END_STREAM，会话会把两个回调背靠背地跑，走这条也一样。
  sc.on_response_end = [this](uvcpp_h2_session& s, uvcpp_h2_stream& st) {
    on_h2_response_end(s, st);
  };
  sc.on_close = [this](uvcpp_h2_session& s, int32_t id, uint32_t ec) {
    on_h2_stream_close(s, id, ec);
  };
  // 连接级致命错误由 `uvcpp_h2_connection::start` 包一层转成 `shutdown()`；
  // 连接真的没了会走下面的 `on_disconnect`，这里不需要另做动作。
  sc.on_fatal = [](uvcpp_h2_session&, int) {};

  uvcpp_h2_connection::callbacks cc;
  cc.on_disconnect = [this](uvcpp_h2_connection& c) { on_h2_disconnect(c); };

  const int rv = h2_->start(sc, cc);
  if (rv != 0) {
    delete h2_;
    h2_ = nullptr;
    return rv;
  }
  h2_active_ = true;
  return 0;
}

int uvcpp_http_client::send_h2(
    const uvcpp_http_request& req,
    std::function<void(const uvcpp_http_response&, int)> cb) {
  if (h2_ == nullptr) {
    last_error_code_ = UV_ENOTCONN;
    return UV_ENOTCONN;
  }

  // `:authority` 取自 `host` 头，而 h1 那条 `to_string()` 在缺 host 时会自己
  // 补一个（从 URL 里抠，抠不到就写 localhost）。h2 这条没有那层兜底，缺了
  // 就是**空** `:authority`，服务端一律判畸形。我们手上正好有权威来源 ——
  // `connect()` 的 host。
  uvcpp_http_request out = req;
  if (!out.has_header("host")) out.set_header("host", host_);

  const int32_t sid = h2_->session().submit_request(out, req.body.to_string());
  if (sid < 0) {
    last_error_code_ = sid;
    return sid;
  }

  h2_streams_[sid].cb = std::move(cb);

  const int frv = h2_->flush();
  if (frv != 0) {
    // **不在这里把这条流抹掉。** `flush()` 真出错时它内部已经 `shutdown()` 了，
    // 结算会由 `on_h2_disconnect` 带着错误码跑掉；在这里 erase 等于让发起方
    // 永远等不到回调。
    last_error_code_ = frv;
    return frv;
  }
  return 0;
}

void uvcpp_http_client::on_h2_body(uvcpp_h2_session&, uvcpp_h2_stream& st,
                                   const char* data, size_t len) {
  auto it = h2_streams_.find(st.stream_id);
  if (it == h2_streams_.end()) return;
  it->second.body.append_data(data, len);
}

void uvcpp_http_client::on_h2_response_end(uvcpp_h2_session&,
                                           uvcpp_h2_stream& st) {
  auto it = h2_streams_.find(st.stream_id);
  if (it == h2_streams_.end()) return;

  // 先把要交付的东西全部攒齐、把条目摘走，**再**碰用户代码：用户回调里完全
  // 可以把整个 client 析构掉，那之后 `h2_streams_` 与 `st` 都不该再被读到。
  uvcpp_http_response resp;
  resp.version        = uvcpp_http_version::HVER_20;
  // 一条连接上同时有好几条流在飞，回调又是**逐条**的，所以这个身份字段是
  // 调用方唯一的"这条回应的是哪次请求"的凭据（h1 那条路上它恒为 0）。
  resp.stream_id      = st.stream_id;
  resp.status_code    = st.response.status_code;
  resp.status_message = http_status_reason(resp.status_code);
  resp.headers        = st.response.headers;
  resp.body.clone(it->second.body);
  std::function<void(const uvcpp_http_response&, int)> cb =
      std::move(it->second.cb);
  h2_streams_.erase(it);

  set_status(HTTP_CLIENT_COMPLETE);
  clear_status(HTTP_CLIENT_RECEIVING);
  clear_status(HTTP_CLIENT_ERROR);

  if (cb) cb(resp, 0);
}

void uvcpp_http_client::on_h2_stream_close(uvcpp_h2_session& s,
                                           int32_t stream_id,
                                           uint32_t error_code) {
  // 正常收尾的那条流在 `on_h2_response_end` 里已经被摘走了 —— 能在这里找到
  // 条目，就说明它**没有**收尾（RST_STREAM / 协议错误 / 连接断）。
  auto it = h2_streams_.find(stream_id);
  if (it == h2_streams_.end()) return;

  uvcpp_http_response resp;
  resp.version   = uvcpp_http_version::HVER_20;
  resp.stream_id = stream_id;
  // 已经解析出来的那部分照给：调用方至少能看见状态码和响应头 —— 比扔一个
  // 默认构造的 `OK` 过去诚实。`find_stream` 在 `on_close` 里还没被 erase。
  if (uvcpp_h2_stream* hs = s.find_stream(stream_id)) {
    resp.status_code    = hs->response.status_code;
    resp.status_message = http_status_reason(resp.status_code);
    resp.headers        = hs->response.headers;
  }

  std::function<void(const uvcpp_http_response&, int)> cb =
      std::move(it->second.cb);
  h2_streams_.erase(it);

  // `NO_ERROR` 的关闭不是对端的错（多半是我们自己在收摊），报 CANCELED；
  // 带错误码的是对端明确拒了这条流，那是协议层面的失败。
  const int err = (error_code == 0) ? UV_ECANCELED : UV_EPROTO;
  last_error_code_ = err;
  if (cb) cb(resp, err);
}

void uvcpp_http_client::on_h2_disconnect(uvcpp_h2_connection&) {
  // 连接没了，先把 h2 这一整套摘干净 —— 下面要跑用户回调，而用户完全可以在
  // 回调里把整个 client 析构掉，那之后一个成员都不能碰。
  //
  // 拆之前先把对端道别与否抄下来 —— 这一句正是"断开之后还查得到"的全部来源。
  peer_goaway_      = h2_->session().peer_goaway_received();
  peer_goaway_code_ = h2_->session().peer_goaway_error_code();
  peer_goaway_last_ = h2_->session().peer_goaway_last_stream_id();

  // `delete h2_` 是**被允许**的：`on_disconnect` 的契约就是"持有者在这里销毁
  // 本对象"，它返回后 `uvcpp_h2_connection` 不再碰自己任何一个成员。
  delete h2_;
  h2_ = nullptr;
  h2_active_ = false;

  // 摘掉 CONNECTED：`send()` 的头一道守卫就是它，于是断开之后的发送拿到的是
  // `UV_ENOTCONN` 而不是"往一条死连接上写明文"。**不置 `HTTP_CLIENT_CLOSED`**
  // —— 那个标志在本类里是"`tcp_` 的句柄已经关完了"，析构靠它决定要不要走关闭
  // 那一套；在这儿置上会让 socket 不关、句柄留在 loop 上，`loop_close()` 就
  // 返回 `UV_EBUSY`。
  clear_status(HTTP_CLIENT_CONNECTED);
  set_status(HTTP_CLIENT_ERROR);
  last_error_code_ = UV_ECANCELED;

  std::map<int32_t, h2_stream_state> pending;
  pending.swap(h2_streams_);

  for (auto& kv : pending) {
    if (!kv.second.cb) continue;
    uvcpp_http_response resp;
    resp.version = uvcpp_http_version::HVER_20;
    kv.second.cb(resp, UV_ECANCELED);
  }
}

#endif  // UVCPP_NGHTTP2_ENABLE

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
