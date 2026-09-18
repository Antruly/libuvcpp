/**
 * @file src/webapp/uvcpp_web_ws_client.cpp
 * @brief WebSocket 客户端（框架层）的实现。
 * @author zhuweiye
 * @version 1.1.0
 */

#include <webapp/uvcpp_web_ws_client.h>

#include <chrono>
#include <thread>

#include <handle/uvcpp_timer.h>
#include <web/uvcpp_ws_connection.h>

namespace uvcpp {

namespace {
/** @brief 退避计算的硬上限（约 12 天），只为防翻倍溢出成负数。 */
const long long kMaxBackoffCeiling = 1LL << 30;
}  // namespace

uvcpp_web_ws_client::uvcpp_web_ws_client()
    : inner_(nullptr),
      timer_(nullptr),
      user_closed_(false),
      attempts_(0),
      restart_pending_(false),
      in_inner_run_(false),
      stopping_(false)
#if UVCPP_OPENSSL_ENABLE
      ,
      ssl_ctx_(nullptr)
#endif
{
  inner_ = new uvcpp_ws_client();
}

uvcpp_web_ws_client::~uvcpp_web_ws_client() {
  // 析构里**不等待**，也不做任何墙钟等待 —— 与协议层同一条约定。底层的
  // 析构自己会把会话回收干净（那里是有界的 NOWAIT 迭代）。
  //
  // 注意：如果本对象是在自己的回调里被析构的，`run()` 那一帧脚下的对象没了。
  // `delete inner_` 本身现在兜得住（协议层会把 loop/tcp/会话整块交出去、不再
  // 回收，见那里的注释），但**本层这一帧兜不住** —— 那是"正在跑 `run()` 的
  // 包装对象"被删，不是底层的事。所以这一条写在类注释里，是使用者的约束，
  // 不是本层能兜住的：兜它需要把析构推迟到循环之外，而析构点就是最后一刻，
  // 没有"之后"可用。
  user_closed_ = true;  // 万一还有回调在途，别再排重连了
  cancel_reconnect();
  delete inner_;
  inner_ = nullptr;
}

// =========================================================================
// 回调设置 —— 存下来，每次换客户端重装一遍
// =========================================================================

uvcpp_web_ws_client& uvcpp_web_ws_client::on_open(
    std::function<void(uvcpp_ws_connection*)> cb) {
  on_open_ = std::move(cb);
  return *this;
}

uvcpp_web_ws_client& uvcpp_web_ws_client::on_text(
    std::function<void(const std::string&)> cb) {
  on_text_ = std::move(cb);
  if (inner_ != nullptr) inner_->on_text(on_text_);
  return *this;
}

uvcpp_web_ws_client& uvcpp_web_ws_client::on_binary(
    std::function<void(const uint8_t*, size_t)> cb) {
  on_bin_ = std::move(cb);
  if (inner_ != nullptr) inner_->on_binary(on_bin_);
  return *this;
}

uvcpp_web_ws_client& uvcpp_web_ws_client::on_close(
    std::function<void(ws_close_code, const std::string&)> cb) {
  on_close_ = std::move(cb);
  if (inner_ != nullptr) {
    // 夹一层：先让使用者收口，再决定要不要重连。协议层只管把"对端怎么走的"
    // 报上来，重连是框架层的事，两者不能混在一个回调里。
    inner_->on_close([this](ws_close_code code, const std::string& reason) {
      handle_close(code, reason);
    });
  }
  return *this;
}

uvcpp_web_ws_client& uvcpp_web_ws_client::on_error(
    std::function<void(int, const std::string&)> cb) {
  on_error_ = std::move(cb);
  if (inner_ != nullptr) {
    // 夹一层：协议错误会让会话终结（而且**不会**走 on_close —— 那是本端发起
    // 的结束），这条连接一样得按策略重连，所以排重连的点在这里补上。
    inner_->on_error([this](int err, const std::string& msg) {
      if (on_error_) on_error_(err, msg);
      schedule_reconnect();
    });
  }
  return *this;
}

uvcpp_web_ws_client& uvcpp_web_ws_client::set_reconnect(
    const uvcpp_web_ws_reconnect& cfg) {
  reconnect_ = cfg;
  if (!reconnect_.enabled) cancel_reconnect();
  return *this;
}

uvcpp_web_ws_client& uvcpp_web_ws_client::on_reconnect(
    std::function<void(int, int)> cb) {
  on_reconnect_ = std::move(cb);
  return *this;
}

void uvcpp_web_ws_client::install() {
  if (inner_ == nullptr) return;
  // 一律经由夹层装（`on_close`/`on_error` 的夹层里带着重连决策），所以这几个
  // setter 就是"装到当前客户端上"的唯一入口。
  inner_->on_text(on_text_);
  inner_->on_binary(on_bin_);
  on_close(on_close_);  // 即使 on_close_ 为空也要装：夹层负责排重连
  on_error(on_error_);  // 同上
#if UVCPP_OPENSSL_ENABLE
  if (ssl_ctx_ != nullptr) inner_->set_ssl_context(ssl_ctx_);
#endif
#if UVCPP_ZLIB_ENABLE
  inner_->set_compression(deflate_cfg_);
#endif
}

#if UVCPP_OPENSSL_ENABLE
uvcpp_web_ws_client& uvcpp_web_ws_client::set_ssl_context(
    uvcpp_ssl_context* ctx) {
  ssl_ctx_ = ctx;
  if (inner_ != nullptr) inner_->set_ssl_context(ctx);
  return *this;
}
#endif

#if UVCPP_ZLIB_ENABLE
uvcpp_web_ws_client& uvcpp_web_ws_client::set_compression(
    const uvcpp_ws_deflate_config& cfg) {
  deflate_cfg_ = cfg;
  if (inner_ != nullptr) inner_->set_compression(cfg);
  return *this;
}

uvcpp_ws_deflate_config uvcpp_web_ws_client::get_compression() const {
  // 读底层的当前值：本层存的那份是"意图"，底层那份才是真的在用的。
  if (inner_ != nullptr) return inner_->get_compression();
  return deflate_cfg_;
}
#endif

// =========================================================================
// 连接 / 重连
// =========================================================================

int uvcpp_web_ws_client::connect(const std::string& url,
                                 std::function<void(int)> cb) {
  url_ = url;
  connect_cb_ = std::move(cb);
  user_closed_ = false;
  attempts_ = 0;
  cancel_reconnect();

  // 换客户端会连带删掉正在跑的那个循环，所以**在回调里调本函数**（`run()` 的
  // 栈上有 `inner_->run()`）只能登记，等这一轮循环返回后再做。不登记而是硬换
  // 的话就是 `uv_run` 重入 —— libuv 会直接断言掉。
  if (in_inner_run_) {
    restart_pending_ = true;
    return 0;
  }
  do_restart();
  return 0;
}

int uvcpp_web_ws_client::connect_wait(const std::string& url, int timeout_ms) {
  bool done = false;
  int err = 0;
  connect(url, [&done, &err](int e) {
    err = e;
    done = true;
  });

  // 阻塞轮询。这里**故意**用墙钟 + 1ms 睡眠：本函数的存在意义就是"在脚本/测试
  // 里把异步的连接当成同步的用"，除此之外没有别的调用场合（见头文件）。
  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  while (!done) {
    run(UV_RUN_NOWAIT);
    if (done) break;
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
    if (elapsed >= timeout_ms) return UV_ETIMEDOUT;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return err;
}

void uvcpp_web_ws_client::do_restart() {
  restart_pending_ = false;

  // 顺序要紧：定时器挂在**旧**循环上，必须先把它关掉（`uv_close`），再删
  // 底层客户端 —— 底层的析构会拨几轮 NOWAIT，正好把定时器那一次关闭回调
  // 放掉。反过来的话，`uv_loop_close` 会因为句柄队列里还留着一个没关的定时器
  // 而返回 UV_EBUSY（然后那个循环就泄漏了）。
  cancel_reconnect();
  delete inner_;

  inner_ = new uvcpp_ws_client();
  install();
  // `url_` 为空 = 构造之后还没 `connect()` 过；`user_closed_` = 使用者中途
  // `close()` 过（可能就发生在 `on_reconnect` 回调里）。两种都不该再发起连接 ——
  // 尤其是后者：不挡的话"主动关掉之后还会自己连回来"。
  if (url_.empty() || user_closed_) return;
  inner_->connect(url_, [this](uvcpp_ws_connection* conn, int error) {
    handle_connect_result(conn, error);
  });
}

void uvcpp_web_ws_client::handle_connect_result(uvcpp_ws_connection* conn,
                                                 int error) {
  if (error == 0) {
    attempts_ = 0;
    cancel_reconnect();
    if (on_open_) on_open_(conn);
  } else {
    // 使用者自己 close() 取消的那次**不是**"连接出错"：协议层也只把它结算给
    // connect 回调（UV_ECANCELED），不再报一次错 —— 本层照旧，免得"我主动取消
    // 的"和"连不上"混在一起。
    if (!user_closed_ && on_error_) on_error_(error, std::string());
    schedule_reconnect();
  }
  if (connect_cb_) {
    std::function<void(int)> cb = std::move(connect_cb_);
    connect_cb_ = nullptr;
    cb(error);
  }
}

void uvcpp_web_ws_client::handle_close(ws_close_code code,
                                       const std::string& reason) {
  // **先**交给使用者（他可能在这里 close()，那就是"别再重连了"），**再**排
  // 重连 —— 顺序反过来的话，使用者的 close() 会跟重连打架。
  if (on_close_) on_close_(code, reason);
  schedule_reconnect();
}

// =========================================================================
// 重连策略
// =========================================================================

int uvcpp_web_ws_client::next_delay_ms() const {
  long long delay = reconnect_.delay_ms;
  if (delay < 0) delay = 0;

  if (reconnect_.backoff && attempts_ > 1) {
    // attempts_ 已经自增过：第 1 次用 delay_ms，第 2 次 2×，第 3 次 4×……
    for (int i = 1; i < attempts_; ++i) {
      delay *= 2;
      if (delay >= kMaxBackoffCeiling) break;
    }
  }

  long long cap = reconnect_.max_delay_ms;
  if (cap <= 0) cap = kMaxBackoffCeiling;  // <=0 = 不设上限，但仍要防溢出
  if (delay > cap) delay = cap;
  if (delay > kMaxBackoffCeiling) delay = kMaxBackoffCeiling;
  return static_cast<int>(delay);
}

void uvcpp_web_ws_client::schedule_reconnect() {
  if (!reconnect_.enabled) return;
  if (user_closed_) return;
  if (restart_pending_) return;  // 已经排着了
  if (timer_ != nullptr) return;  // 已经排着了（另一条路径进来的）

  // 次数用尽：**报一次**再停。不报的话现象就是"客户端安静地不再连了"，
  // 而那正是最难查的一种。
  if (reconnect_.max_attempts > 0 && attempts_ >= reconnect_.max_attempts) {
    if (on_error_) {
      on_error_(WEB_WS_ERR_RECONNECT_EXHAUSTED, std::string("重连次数用尽"));
    }
    return;
  }
  if (inner_ == nullptr) return;

  ++attempts_;
  const int delay = next_delay_ms();
  if (on_reconnect_) on_reconnect_(attempts_, delay);

  // 回调里可能把重连关了、或者干脆 close() 了 —— 那就别再排这一次。
  if (!reconnect_.enabled || user_closed_) return;

  // 定时器跟着底层客户端换：换客户端会连带删掉它所在的循环，留着一个挂在
  // 已删循环上的句柄就是悬垂。
  timer_ = new uvcpp_timer(inner_->get_loop());
  timer_->start([this](uvcpp_timer*) { on_reconnect_timer(); },
                static_cast<uint64_t>(delay), 0);
}

void uvcpp_web_ws_client::cancel_reconnect() {
  if (timer_ == nullptr) return;
  timer_->stop();
  delete timer_;
  timer_ = nullptr;
}

void uvcpp_web_ws_client::on_reconnect_timer() {
  // 定时器回调跑在底层循环里 —— 这里**不能**换客户端，只能登记。
  restart_pending_ = true;
}

// =========================================================================
// 收发
// =========================================================================

int uvcpp_web_ws_client::send_text(const char* data, size_t len,
                                   std::function<void(int)> cb) {
  if (inner_ == nullptr) {
    if (cb) cb(UV_ENOTCONN);
    return UV_ENOTCONN;
  }
  return inner_->send_text(data, len, std::move(cb));
}

int uvcpp_web_ws_client::send_text(const std::string& text,
                                   std::function<void(int)> cb) {
  return send_text(text.data(), text.size(), std::move(cb));
}

int uvcpp_web_ws_client::send_binary(const char* data, size_t len,
                                     std::function<void(int)> cb) {
  if (inner_ == nullptr) {
    if (cb) cb(UV_ENOTCONN);
    return UV_ENOTCONN;
  }
  return inner_->send_binary(data, len, std::move(cb));
}

void uvcpp_web_ws_client::close(ws_close_code code,
                                const std::string& reason) {
  user_closed_ = true;
  restart_pending_ = false;  // 还没发出去的那次重连一并撤掉
  cancel_reconnect();
  // 底下那层自己是三态的（有会话发 Close 帧；没有会话直接取消连接并当场
  // 结算 connect 回调），本层照转，不再自己加一层状态。
  if (inner_ != nullptr) inner_->close(code, reason);
}

// =========================================================================
// 状态
// =========================================================================

bool uvcpp_web_ws_client::is_open() const { return session() != nullptr; }

int uvcpp_web_ws_client::get_status() const {
  return inner_ != nullptr ? inner_->get_status() : WS_CLIENT_NONE;
}

int uvcpp_web_ws_client::get_last_error() const {
  return inner_ != nullptr ? inner_->get_last_error() : 0;
}

int uvcpp_web_ws_client::reconnect_attempts() const { return attempts_; }

uvcpp_ws_connection* uvcpp_web_ws_client::session() const {
  return inner_ != nullptr ? inner_->session() : nullptr;
}

// =========================================================================
// Loop
// =========================================================================

uvcpp_loop* uvcpp_web_ws_client::get_loop() {
  return inner_ != nullptr ? inner_->get_loop() : nullptr;
}

int uvcpp_web_ws_client::run(uv_run_mode md) {
  if (md != UV_RUN_DEFAULT) {
    // 单次 tick：上一次调用已经返回了，所以待办的换客户端在这里落下来是安全的。
    if (restart_pending_) do_restart();
    if (inner_ == nullptr) return 0;
    in_inner_run_ = true;
    const int rc = inner_->run(md);
    in_inner_run_ = false;
    return rc;
  }

  // UV_RUN_DEFAULT：进入前就被 stop() 过就直接返回（`uv_run` 对 stop_flag 也
  // 是这个语义），并且把请求消费掉 —— 这样 stop() 之后还能再 run()。
  if (stopping_) {
    stopping_ = false;
    return 0;
  }

  int rc = 0;
  while (!stopping_) {
    // **换客户端的唯一时机**：两次 `uv_run` 之间。此时底层循环没有在跑，
    // 删掉它（连同它的 loop）是合法的。
    if (restart_pending_) do_restart();
    if (inner_ == nullptr) {
      rc = 0;
      break;
    }
    in_inner_run_ = true;
    rc = inner_->run(UV_RUN_ONCE);
    in_inner_run_ = false;
    // rc == 0 = 循环里没有活句柄了（连完了、关干净了、也没排重连）。
    if (!restart_pending_ && rc == 0) break;
  }
  in_inner_run_ = false;
  stopping_ = false;
  return rc;
}

void uvcpp_web_ws_client::stop() {
  stopping_ = true;
  if (inner_ != nullptr) inner_->stop();
}

}  // namespace uvcpp
