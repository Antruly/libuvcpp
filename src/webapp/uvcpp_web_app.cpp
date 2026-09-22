/**
 * @file src/webapp/uvcpp_web_app.cpp
 * @brief uvcpp_web_app 实现。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 一条请求的完整路径（读这个文件时对着看）：
 *
 *   TCP accept
 *     └─ tcp_server 登记客户端 + 装 close manager + 起读
 *         └─ http_server::on_tcp_connection
 *             ├─ **本文件** on_accept()：发连接 id、进登记表、跑用户的连接钩子
 *             ├─ 建 parser / conn_ctx
 *             └─ set_on_close(remove_ctx)
 *   HTTP 报文解析完
 *     └─ http_server::on_request_complete → 兜底 handler（本文件 on_http_request）
 *         ├─ resp.deferred = true          ← 从此发送由框架负责
 *         ├─ id → context，take_from(req)  ← 请求体零拷贝搬过来
 *         ├─ router_.match() → 选链
 *         └─ context.run(chain)
 *             └─ 链跑完 → context::finish() → host.send_response()
 *                 └─ 查登记表 → http_server::send_response() → 异步写
 *   连接断开（对端断 / 主动关 / 出错，三条路）
 *     └─ tcp_client::fire_close_callbacks
 *         ├─ 用户槽：http_server::remove_ctx → close_handler_(client)
 *         │      └─ **本文件** on_close()：摘登记表、跑用户的断开钩子
 *         └─ 框架槽：tcp_server::release_client → delete client
 *
 * 停机：
 *   stop()（任意线程）→ post → async 回调 → begin_shutdown（loop 线程）
 *     → 关监听 → 看门狗每 20ms 检查在途请求 → 收尾（两拍：先关连接，
 *       下一拍再删句柄、停循环）
 */

#include <webapp/uvcpp_web_app.h>

#include <uv.h>

#include <handle/uvcpp_async.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_web_file.h>
#include <webapp/uvcpp_web_multipart.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_upload.h>
#include <webapp/uvcpp_web_util.h>
#include <webapp/uvcpp_web_ws.h>

#include <cstdio>
#include <exception>
#include <future>
#include <utility>

namespace uvcpp {

namespace {

/**
 * @brief 流式响应的字节出口 —— `uvcpp_web_stream_sink` 在本层的落地。
 *
 * 之所以要这一层而不是让 `uvcpp_web_response` 直接拿 `uvcpp_tcp_client*`：
 * 响应对象是**业务可见**的，而"用户代码永远拿不到裸 client 指针"是框架的
 * 一条硬规矩（连接随时可能被对端关掉，指针存下来就是悬垂）。出口拿的是
 * **conn id**，每次都现查 —— 查不到就是"连接已经没了"，这正是那三个方法
 * 各自要处理的第一个分支。
 *
 * 生命周期：由 `uvcpp_web_response::set_stream_sink()` 接管，随响应一起
 * 析构；而响应活在 `uvcpp_web_context` 里，流式期间被 `hold()` 钉住。
 */
class app_stream_sink : public uvcpp_web_stream_sink {
 public:
  /// `stream_id` 是 h2 的流号（0 = HTTP/1.1）。出口拿到的是**建这个 sink
  /// 那一刻**的流号，之后不再变 —— 这是对的：一条流式响应自始至终属于同一条
  /// 流，而 `uvcpp_web_response` 也活不到"复用给另一条流"的时候。
  app_stream_sink(uvcpp_web_app* app, uvcpp_web_conn_id id, int32_t stream_id)
      : app_(app), id_(id), stream_id_(stream_id) {}

  virtual int stream_begin(uvcpp_http_response& head) {
    if (app_ == nullptr) return UV_ECANCELED;
    app_->stream_begin(id_, stream_id_, head);
    return 0;
  }

  virtual int stream_write(const std::string& bytes,
                           const std::function<void(int)>& done) {
    if (app_ == nullptr) return UV_ECANCELED;
    return app_->stream_write(id_, stream_id_, bytes, done);
  }

  virtual void stream_end(bool close_after) {
    if (app_ == nullptr) return;
    app_->stream_end(id_, stream_id_, close_after);
  }

  virtual void stream_attach_file(uvcpp_web_file_transfer* t) {
    if (app_ == nullptr) return;
    app_->stream_attach_file(id_, t);
  }

  virtual void stream_detach_file(uvcpp_web_file_transfer* t) {
    if (app_ == nullptr) return;
    app_->stream_detach_file(id_, t);
  }

  virtual uvcpp_loop* stream_loop() const {
    return app_ != nullptr ? app_->loop() : nullptr;
  }

 private:
  uvcpp_web_app* const    app_;
  const uvcpp_web_conn_id id_;
  const int32_t           stream_id_;
};

/** @brief 看门狗轮询间隔（毫秒）。 */
const uint64_t kShutdownPollMs = 20;

/** @brief 关掉连接之后，留给关闭完成回调跑完的时间（毫秒）。 */
const uint64_t kShutdownDrainMs = 10;

/**
 * @brief 闲置超时扫描的间隔（毫秒）。
 *
 * 取超时的四分之一：既让"超时到被关掉"的延迟不超过 25%（用户配 60 秒不会
 * 变成等 120 秒才关），又不至于让扫描本身成为开销。上下都夹住 —— 测试里
 * 配 100 毫秒超时的时候也不能扫得比 200 毫秒还慢，那用例就得等好几拍。
 */
uint64_t idle_sweep_interval_ms(int timeout_ms) {
  if (timeout_ms <= 0) return 0;
  int64_t iv = static_cast<int64_t>(timeout_ms) / 4;
  if (iv < 200) iv = 200;
  if (iv > 30000) iv = 30000;
  return static_cast<uint64_t>(iv);
}

/** @brief 循环时间的毫秒值。**只能在 loop 线程上调**（读的是循环缓存的时间）。 */
int64_t loop_now_ms(uvcpp_loop* loop) {
  return static_cast<int64_t>(uv_now(reinterpret_cast<uv_loop_t*>(
      loop->get_handle())));
}

/**
 * @brief 把"尺寸上限"那一族的解析结果映射成框架要回的状态码；不是上限族则返回 0。
 *
 * **只映射上限四条，不碰畸形报文那几条**（`ERROR_BAD_HEADER` /
 * `ERROR_NOT_FORM_DATA` / `ERROR_MISSING_NAME` / `ERROR_BOUNDARY_NEVER_FOUND` /
 * `ERROR_SINK_ABORTED`）：那几条保持步骤 4 定下的行为 —— 交给用户的
 * `on_end`（此时 `req.upload() == nullptr`），由用户自己决定回什么。整包接管
 * 错误分类会是一次静默的行为变更，而本步要加的只是"新增的上限由框架负责"。
 *
 * 两档状态码的理由：文件数 / 字段数 / 单字段 / 总长都是"你给的东西太大" → 413；
 * 而部件头超长是"报文本身不成形" → 400。与 `upload_route()` 文档里那张表一致。
 *
 * 返回 0 而不是枚举，是为了调用点只写一次判空 —— 这个函数的返回值只有一个
 * 用途（交给 `abort()`），多一个类型就多一处要读的声明。
 */
int upload_limit_status(uvcpp_web_multipart_result r) {
  switch (r) {
    case uvcpp_web_multipart_result::ERROR_TOO_MANY_FILES:
    case uvcpp_web_multipart_result::ERROR_TOO_MANY_FIELDS:
    case uvcpp_web_multipart_result::ERROR_FIELD_TOO_LARGE:
      return static_cast<int>(http_status::PAYLOAD_TOO_LARGE);
    case uvcpp_web_multipart_result::ERROR_HEADER_TOO_LONG:
      return static_cast<int>(http_status::BAD_REQUEST);
    default:
      return 0;
  }
}

}  // namespace

// =========================================================================
// 配置
// =========================================================================

uvcpp_web_app_config::uvcpp_web_app_config()
    : host("0.0.0.0"),
      port(8080),
      backlog(128),
      max_body_size(16 * 1024 * 1024),
      max_header_bytes(16 * 1024),
      max_url_bytes(8 * 1024),
      compression(true),
      compress_min_body_size(1024),
      access_log(true),
      min_log_level(log_level::INFO),
      server_header("uvcpp"),
      shutdown_grace_ms(3000),
      idle_timeout_ms(60000),
      max_pipelined_requests(8),
      auto_options(true),
      head_as_get(true) {}

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_web_app::uvcpp_web_app()
    : http_(new uvcpp_http_server()),
      work_limit_(new uvcpp_web_work_limit()),
      ws_server_(nullptr),
      stream_routes_seen_(0),
      upload_dir_(),
      upload_dir_real_(),
      upload_dir_unsafe_(false),
      upload_fsync_(true),
      max_upload_size_(0),
      max_file_size_(0),
      max_upload_files_(32),
      max_form_fields_(128),
      max_field_size_(1024 * 1024),
      max_part_header_bytes_(8192),
      specials_built_(false),
      cache_gen_(0),
      routes_seen_(0),
      middleware_gen_(0),
      not_found_chain_(nullptr),
      method_not_allowed_chain_(nullptr),
      auto_options_chain_(nullptr),
      thread_started_(false),
      threading_(false),
      started_once_(false),
      loop_started_(false),
      running_(false),
      stopping_(false),
      bound_port_(0) {
  // 流式路由表用**普通路由的语义**（参数、通配、静态优先），但关掉两个
  // "为一个完整请求做决定"的开关：
  //
  //   - `head_as_get`：HEAD 没有 body，"HEAD 落到 GET 处理器"在流式这一侧
  //     毫无意义；开着只会让一个 HEAD 请求被认领、然后永远等不到 body。
  //   - `auto_options`：自动 OPTIONS 是"看到路径就替用户回 405/Allow"，那是
  //     对完整请求的兜底。流式表是**精确认领**表 —— 没注册就不该认领，
  //     交给普通路由去决定。
  stream_router_.set_head_as_get(false);
  stream_router_.set_auto_options(false);

  // 0 号那格现在就有：`loop()` 在 `start()` 之前也要答得出东西（它今天直接
  // 返回 tcp_server 那条循环），而 `slot_here()` 的 n == 1 快路假设它非空。
  // 在**构造函数体**里建而不是初始化列表里，是因为它要读 `http_`。
  loops_.push_back(std::unique_ptr<loop_slot>(new loop_slot(0)));
  loops_[0]->loop = http_->get_tcp_server()->get_loop();
}

uvcpp_web_app::uvcpp_web_app(const uvcpp_web_app_config& cfg)
    : uvcpp_web_app() {
  cfg_ = cfg;
}

uvcpp_web_app::~uvcpp_web_app() {
  // 兜底停机。正常路径上调用方已经显式停过了，这里只是保证不会有人"忘了
  // 停"就把对象析构掉（那样 `~uvcpp_tcp_server` 会在另一个线程还在 uv_run
  // 里的时候去泵循环、关循环 —— 那是真正的竞态）。
  stop();
  join();

  // 到这儿循环已经停了（或者压根没起来），可以安全拆句柄。
  //
  // 顺序不能反：**先删句柄包装对象，再删 http_ 层** —— 后者的析构会关掉并
  // 销毁 loop，之后任何 `uv_close`/`uv_timer_stop` 都是在用一个不存在的
  // 循环。
  for (size_t i = 0; i < loops_.size(); ++i) {
    delete loops_[i]->async;
    loops_[i]->async = nullptr;
    delete loops_[i]->idle_timer;
    loops_[i]->idle_timer = nullptr;
    delete loops_[i]->shutdown_timer;
    loops_[i]->shutdown_timer = nullptr;
  }

  // **WS 必须先于 http_ 拆。** `~uvcpp_ws_server` 会 `sessions_.shutdown()`，
  // 那一步要释放延迟回收用的 async 句柄（走 `uv_close`）—— 而 loop 归
  // `http_` 底下的 tcp_server 所有，`delete http_` 会把它关掉。顺序反了就是
  // 在一个已经销毁的循环上 `uv_close`。
  //
  // 这也是为什么 ws_server_ 是**裸指针 + 显式 delete** 而不是智能指针：
  // 它的销毁时机由这条顺序约束决定，不能交给成员逆序析构去猜。
  delete ws_server_;
  ws_server_ = nullptr;

  delete http_;
  http_ = nullptr;
}

// =========================================================================
// 配置 setter
// =========================================================================

uvcpp_web_app& uvcpp_web_app::set_host(const std::string& host) {
  cfg_.host = host;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_port(int port) {
  cfg_.port = port;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_backlog(int backlog) {
  cfg_.backlog = backlog > 0 ? backlog : 128;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_max_body_size(size_t bytes) {
  cfg_.max_body_size = bytes;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_max_header_bytes(size_t bytes) {
  cfg_.max_header_bytes = bytes;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_max_url_bytes(size_t bytes) {
  cfg_.max_url_bytes = bytes;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_compression(bool enable) {
  cfg_.compression = enable;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_compress_min_body_size(size_t bytes) {
  cfg_.compress_min_body_size = bytes;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_access_log(bool enable) {
  cfg_.access_log = enable;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_log_level(log_level level) {
  cfg_.min_log_level = level;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_server_header(const std::string& value) {
  cfg_.server_header = value;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_shutdown_grace_ms(int ms) {
  cfg_.shutdown_grace_ms = ms > 0 ? ms : 0;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_idle_timeout_ms(int ms) {
  // 负数按"关掉"处理而不是夹成 1 毫秒：后者会让服务在用户笔误时把每条连接
  // 立刻杀掉，而"关掉"至少是能看出问题的（连接不断、只是不再有保护）。
  cfg_.idle_timeout_ms = ms > 0 ? ms : 0;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_max_pipelined_requests(size_t n) {
  // 0 = 不限（与超时、长度上限一致：0 一律读作"关掉这个限制"）。真要"一条都
  // 不许流水线"没有意义 —— 那等于把 HTTP/1.1 的 keep-alive 也一并禁了。
  cfg_.max_pipelined_requests = n;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_auto_options(bool enable) {
  cfg_.auto_options = enable;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_head_as_get(bool enable) {
  cfg_.head_as_get = enable;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_http2_enabled(bool enable) {
  http2_requested_ = enable;
  return *this;
}

bool uvcpp_web_app::http2_enabled() const {
#if UVCPP_NGHTTP2_ENABLE
  return http2_requested_;
#else
  return false;
#endif
}

uvcpp_web_app& uvcpp_web_app::set_work_limit(size_t limit) {
  work_limit_->set_limit(limit);
  return *this;
}

#if UVCPP_OPENSSL_ENABLE

namespace {

/// 框架默认宣告的 ALPN 名单，**顺序即优先级**（`h2` 在前 = 能协商就上 h2）。
/// 用户自己设过名单时不碰它（见 `uvcpp_ssl_context::has_alpn_select` 的说明）。
const std::vector<std::string> kDefaultAlpn{"h2", "http/1.1"};

/// `set_http2_enabled(false)` 之后宣告的名单。**不是空名单** —— 理由见
/// `start()` 里那段：空名单会让握手完全没有协商结果，与"答复你只有 1.1"
/// 是两件事。留成单独一个常量而不是写成 `kDefaultAlpn` 的子集，是为了让
/// "关掉 h2 之后到底宣告什么"在代码里只有一个答案。
const std::vector<std::string> kHttp11Alpn{"http/1.1"};

/// 新建 TLS 上下文时的最低协议版本。
///
/// 不用库的默认值（`TLS_1_2`）以外的更松档位：TLS 1.0/1.1 早已被各大浏览器
/// 废弃，允许它们等于给自己留一条降级攻击的路。想放宽请显式调用
/// `ssl_context()->set_min_version()` —— 让它是一次显式决定。
const tls_version kTlsFloor = tls_version::TLS_1_2;
}  // namespace

void uvcpp_web_app::ssl_fail(const std::string& why) {
  // 两件事必须一起做：记下来给调用方查，并把上下文清空。
  //
  // 只记不清空的话，一个"证书其实没读进来、但 ssl_ctx_ 还留着"的实现会让
  // start() 顺利通过，然后以**明文**提供服务 —— 那是这里最坏的失败模式。
  ssl_error_ = why;
  ssl_ctx_.reset();
  ssl_requested_ = true;  // 请求过就要为它负责，不能悄悄降级
  UVCPP_LOG_ERROR(log_category::SSL) << "TLS 配置失败：" << why;
}

uvcpp_web_app& uvcpp_web_app::enable_ssl(const std::string& cert_file,
                                         const std::string& key_file) {
  std::shared_ptr<uvcpp_ssl_context> ctx(
      new uvcpp_ssl_context(tls_mode::SERVER, kTlsFloor));
  if (!ctx->is_ready()) {
    ssl_fail("SSL_CTX 创建失败：" + ctx->get_last_error());
    return *this;
  }
  if (!ctx->load_certificate_file(cert_file)) {
    ssl_fail("证书读取失败 `" + cert_file + "`：" + ctx->get_last_error());
    return *this;
  }
  if (!ctx->load_private_key_file(key_file)) {
    ssl_fail("私钥读取失败 `" + key_file + "`：" + ctx->get_last_error());
    return *this;
  }
  // 两个文件都在，但可能来自不同的一套 —— OpenSSL 要到**握手时**才会发现，
  // 所以在这里主动查一次（见 check_private_key 的说明）。
  if (!ctx->check_private_key()) {
    ssl_fail("证书与私钥不配对：`" + cert_file + "` / `" + key_file +
             "`：" + ctx->get_last_error());
    return *this;
  }

  ssl_ctx_ = ctx;
  ssl_requested_ = true;
  ssl_error_.clear();
  return *this;
}

uvcpp_web_app& uvcpp_web_app::enable_ssl_pem(const std::string& cert_pem,
                                             const std::string& key_pem) {
  std::shared_ptr<uvcpp_ssl_context> ctx(
      new uvcpp_ssl_context(tls_mode::SERVER, kTlsFloor));
  if (!ctx->is_ready()) {
    ssl_fail("SSL_CTX 创建失败：" + ctx->get_last_error());
    return *this;
  }
  if (!ctx->load_certificate_data(cert_pem)) {
    ssl_fail("证书内容解析失败：" + ctx->get_last_error());
    return *this;
  }
  if (!ctx->load_private_key_data(key_pem)) {
    ssl_fail("私钥内容解析失败：" + ctx->get_last_error());
    return *this;
  }
  if (!ctx->check_private_key()) {
    ssl_fail("证书与私钥不配对：" + ctx->get_last_error());
    return *this;
  }

  ssl_ctx_ = ctx;
  ssl_requested_ = true;
  ssl_error_.clear();
  return *this;
}

uvcpp_web_app& uvcpp_web_app::enable_self_signed(const std::string& host,
                                                 int bits) {
  std::shared_ptr<uvcpp_ssl_context> ctx(
      new uvcpp_ssl_context(tls_mode::SERVER, kTlsFloor));
  if (!ctx->is_ready()) {
    ssl_fail("SSL_CTX 创建失败：" + ctx->get_last_error());
    return *this;
  }
  if (!ctx->generate_self_signed(host, bits)) {
    ssl_fail("自签证书生成失败（CN=" + host + "）：" + ctx->get_last_error());
    return *this;
  }

  // 自签是"明确知道自己在做什么"的选择，每次都要说一声 —— 免得它被顺手
  // 复制到生产配置里。日志等级给 WARN 而不是 INFO：这是要被人看见的。
  UVCPP_LOG_WARN(log_category::SSL)
      << "已启用自签证书（CN=" << host << "）。仅适用于测试/受控内网 —— "
      << "合规的客户端默认会拒绝自签证书。生产环境请用 enable_ssl() 配真证书。";

  ssl_ctx_ = ctx;
  ssl_requested_ = true;
  ssl_error_.clear();
  return *this;
}

bool uvcpp_web_app::ssl_enabled() const {
  return ssl_requested_ && static_cast<bool>(ssl_ctx_);
}

uvcpp_web_app& uvcpp_web_app::set_tls_handshake_timeout_ms(int ms) {
  // `http_` 在构造时就建好了，所以这里拿得到 tcp_server —— 与
  // `set_max_pipelined_requests` 那类"只改配置、等 start() 生效"的 setter
  // 不同，这一条是**立刻写进 tcp_server** 的：tcp_server 在 accept 时才读它，
  // 只要在 start() 之前调就来得及。
  uvcpp_tcp_server* ts = tcp_server();
  if (ts != nullptr) ts->set_tls_handshake_timeout_ms(ms);
  return *this;
}

#endif  // UVCPP_OPENSSL_ENABLE

// =========================================================================
// 路由 / 中间件
// =========================================================================

uvcpp_web_app& uvcpp_web_app::use(const uvcpp_web_middleware& mw) {
  middlewares_.push_back(mw);
  // 代数一变，链缓存整表作废（下次派发时重建）。**不在这里重建** —— 重建
  // 会往 chain_storage_ 里塞新链，而现在可能正有请求用着旧链在跑；那本身
  // 是安全的（deque 不会让旧元素失地址），但没必要每次 use() 都做。
  ++middleware_gen_;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::get(const std::string& pattern,
                                  const uvcpp_web_handler& handler) {
  router_.get(pattern, handler);
  return *this;
}

uvcpp_web_app& uvcpp_web_app::post(const std::string& pattern,
                                   const uvcpp_web_handler& handler) {
  router_.post(pattern, handler);
  return *this;
}

uvcpp_web_app& uvcpp_web_app::put(const std::string& pattern,
                                  const uvcpp_web_handler& handler) {
  router_.put(pattern, handler);
  return *this;
}

uvcpp_web_app& uvcpp_web_app::del(const std::string& pattern,
                                  const uvcpp_web_handler& handler) {
  router_.del(pattern, handler);
  return *this;
}

uvcpp_web_app& uvcpp_web_app::patch(const std::string& pattern,
                                    const uvcpp_web_handler& handler) {
  router_.patch(pattern, handler);
  return *this;
}

uvcpp_web_app& uvcpp_web_app::head(const std::string& pattern,
                                   const uvcpp_web_handler& handler) {
  router_.head(pattern, handler);
  return *this;
}

uvcpp_web_app& uvcpp_web_app::options(const std::string& pattern,
                                      const uvcpp_web_handler& handler) {
  router_.options(pattern, handler);
  return *this;
}

uvcpp_web_app& uvcpp_web_app::any(const std::string& pattern,
                                  const uvcpp_web_handler& handler) {
  router_.any(pattern, handler);
  return *this;
}

// =========================================================================
// 流式路由
// =========================================================================

uvcpp_web_app& uvcpp_web_app::stream_route(http_method method,
                                           const std::string& pattern,
                                           const uvcpp_web_handler& handler) {
  if (!handler) {
    UVCPP_LOG_WARN(log_category::ROUTER)
        << "拒绝注册空流式 handler：" << pattern;
    return *this;
  }

  // **包装层：框架替用户把 `next` 留住。**
  //
  // 用户在 headers 时机被调用，那时 body 还没到，它只能"同步返回、把回调挂
  // 上"。而 `advance()` 判定"这个 handler 是不是要异步续跑"的依据是**它返回
  // 时 `next` 的引用计数有没有变多**（见 `uvcpp_web_context.cpp` 的注释）。
  // 用户没留 `next`，框架就会判定"这一环结束了"，当场 `finish()` 并发一个
  // 空响应 —— 而上传还在继续，后续每个 body 块都会往一个已经发出去的响应上
  // 写。这不是"可能出问题"，是**每条流式请求都会中**。
  //
  // 所以在用户 handler 外面裹一层：先 `bind_resume(next)` 把它存进流对象
  // （引用计数变多 → 框架判定挂起），再调用户 handler。之后链一直挂到
  // `on_end`/中止，由 `stream_resume_chain()` 取走并续跑。
  //
  // 注意包装是**按值捕获用户 handler**，而路由表里存的就是这个包装 —— 所以
  // 用户 handler 的生存期跟着路由表走，与普通路由完全一致。
  //
  // `next` 仍然原样传给用户：想自己留一份做额外的事（比如"收完 body 之后先
  // 查个库再回"）是允许的，链会多挂一道，两条路都能把它推下去。
  uvcpp_web_handler wrapper =
      [handler](uvcpp_web_request& req, uvcpp_web_response& resp,
                uvcpp_web_next next) {
        uvcpp_web_stream* s = req.stream();
        if (s != nullptr) s->bind_resume(next);
        handler(req, resp, next);
      };

  // 路由总数由 `sync_chains()` 盯着（见那里的注释）：缓存是按
  // `const uvcpp_web_handler*` 索引的，而注册新路由可能让表重新分配。
  stream_router_.add(method, pattern, wrapper);
  return *this;
}

uvcpp_web_app& uvcpp_web_app::post_stream(const std::string& pattern,
                                          const uvcpp_web_handler& handler) {
  return stream_route(http_method::HTTP_POST, pattern, handler);
}

// =========================================================================
// 上传路由
// =========================================================================

uvcpp_web_app& uvcpp_web_app::set_upload_dir(const std::string& dir) {
  upload_dir_ = dir;
  upload_dir_real_.clear();
  upload_dir_unsafe_ = false;

  // 解析一次。`allow_missing = true`：目录不存在时要的是一个可信的候选路径，
  // 而不是"配置失败" —— 目录该不该存在是部署问题，而包含判断两者都需要。
  // 解析不了（例如给的是不含分隔符的相对名）就退回原串：那时
  // `join_path(upload_dir_, leaf)` 按字面前缀必然满足包含关系，判断照样成立。
  if (!dir.empty() && !web_real_path(dir, upload_dir_real_, true)) {
    upload_dir_real_ = dir;
  }
  check_upload_dir_containment();
  return *this;
}

void uvcpp_web_app::check_upload_dir_containment() {
  if (upload_dir_.empty() || upload_dir_real_.empty()) return;
  if (static_roots_real_.empty()) return;

  for (size_t i = 0; i < static_roots_real_.size(); ++i) {
    if (!web_is_within_root(static_roots_real_[i], upload_dir_real_)) continue;

    // **只置位，不清位**：这条一旦成立就是配置错误，中途把静态根摘掉不该让
    // 已经报过的错消失（用户可能只是还没看到那条日志）。要解除得重新
    // `set_upload_dir()`。
    if (upload_dir_unsafe_) return;
    upload_dir_unsafe_ = true;
    UVCPP_LOG_ERROR(log_category::UPLOAD)
        << "上传目录落在静态文档根里，上传已被拒绝：" << upload_dir_real_
        << " 位于 " << static_roots_real_[i] << " 之下。"
        << "继续下去等于把用户上传的文件直接挂成静态资源（上传成功即可被任意 "
           "GET）。请把上传目录移出文档根，或不要把它挂在同一个前缀下。";
    return;
  }
}

uvcpp_web_app& uvcpp_web_app::set_upload_fsync(bool enable) {
  upload_fsync_ = enable;
  return *this;
}

// 六条尺寸上限。它们的语义差别（413 / 400 / 截断）写在头文件的那张表里，
// 判定点则分布在两处：总长是**框架自己**在 `wire_upload()` 的钩子里判的
// （每收一块之前判，超出的字节一个都不落盘），其余五条转给解析器
// （`uvcpp_web_multipart` 自己判相位、自己转 PART_BODY_SKIP）。
//
// 为什么总长不能一起转给解析器：解析器的 `received_` 是"喂进来多少"，
// 判定只能发生在**喂之后** —— 那意味着超出上限的那一块会先落盘再被删掉。
// 而"先落盘再删"正是这一条上限要防的东西（磁盘写入本身就是资源）。

uvcpp_web_app& uvcpp_web_app::set_max_upload_size(uint64_t bytes) {
  max_upload_size_ = bytes;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_max_file_size(uint64_t bytes) {
  max_file_size_ = bytes;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_max_upload_files(size_t n) {
  max_upload_files_ = n;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_max_form_fields(size_t n) {
  max_form_fields_ = n;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_max_field_size(uint64_t bytes) {
  max_field_size_ = bytes;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_max_part_header_bytes(size_t n) {
  max_part_header_bytes_ = n;
  return *this;
}

std::string uvcpp_web_app::route_key(http_method method,
                                     const std::string& pattern) {
  // 注册与派发两处必须算出**逐字节相同**的键，所以只留这一个实现。用方法号
  // 而不是方法名：`http_method` 是枚举，`static_cast<int>` 没有查表也没有
  // 大小写问题，而方法名（"POST"）要先有个转换函数、还得保证两处用的同一个。
  return std::to_string(static_cast<int>(method)) + " " + pattern;
}

std::shared_ptr<uvcpp_web_context> uvcpp_web_app::active_body_ctx(
    uvcpp_web_conn_id id, int32_t stream_id) const {
  const loop_slot* s = slot_of(id);
  if (s == nullptr) return std::shared_ptr<uvcpp_web_context>();
  std::map<uvcpp_web_conn_id, out_queue >::const_iterator q =
      s->inflight.find(id);
  if (q == s->inflight.end()) return std::shared_ptr<uvcpp_web_context>();

  for (out_queue::const_iterator e = q->second.begin();
       e != q->second.end(); ++e) {
    // `shared_ptr` 的 const 不会穿透到被指对象，所以拿到的是可写的上下文。
    // h2：一个连接上并存多条在收体的流，"第一条命中的"不等于"我要的那条"。
    if (stream_id != 0 && e->ctx->response().stream_id() != stream_id) continue;
    uvcpp_web_stream* s = e->ctx->stream();
    if (s == nullptr || s->aborted()) continue;
    // 「body 还没彻底交完」有两种形态，见头文件里的说明：正在收（delivered_end_
    // 还是假），以及收完了但 `on_end` 被框架扣着等异步落盘（end_deferred_）。
    if (!s->delivered_end() || s->end_deferred()) return e->ctx;
  }
  return std::shared_ptr<uvcpp_web_context>();
}

uvcpp_web_stream* uvcpp_web_app::live_stream(uvcpp_web_conn_id id,
                                             int32_t        stream_id) const {
  std::shared_ptr<uvcpp_web_context> c = active_body_ctx(id, stream_id);
  return c ? c->stream() : nullptr;
}

bool uvcpp_web_app::enqueue_inflight(
    uvcpp_web_conn_id id, const std::shared_ptr<uvcpp_web_context>& ctx) {
  loop_slot* s = slot_of(id);
  // id 无效 ⇒ 不登记。正常路径不可达：两个调用点（`on_http_request` 与
  // `dispatch_stream`）都先保证 id 是有效的（没登记就先补登记）。返回 false
  // 是"没超上限"那一支，走这一支不会把上下文扣住不发。
  if (s == nullptr) return false;
  out_queue& q = s->inflight[id];
  const size_t cap = cfg_.max_pipelined_requests;
  // 0 = 不限（与超时、长度上限一处口径）。
  const bool over = (cap != 0 && q.size() >= cap);
  q.push_back(out_entry(ctx));
  return over;
}

size_t uvcpp_web_app::inflight_total() const {
  // **所有格子求和**（设计稿 §6 第 2 条）：`inflight_count()` 是对外的聚合量，
  // 读某一格的 `size()` 会静默少报别的循环上的在途请求。
  size_t n = 0;
  for (size_t i = 0; i < loops_.size(); ++i) {
    const std::map<uvcpp_web_conn_id, out_queue >& m = loops_[i]->inflight;
    for (std::map<uvcpp_web_conn_id, out_queue >::const_iterator q = m.begin();
         q != m.end(); ++q) {
      n += q->second.size();
    }
  }
  return n;
}

void uvcpp_web_app::flush_out(uvcpp_web_conn_id id) {
  loop_slot* s = slot_of(id);
  if (s == nullptr) return;

  // 已经在续发了：这一层是上面某次 `release()` 追上来的，让它接着迭代就行。
  if (s->flushing) return;

  // RAII 而不是"结尾手动清"：`send_response()` 会跑 `notify_sent()`，而访问日志
  // 中间件是**用户代码**，可以抛。抛出去之后 `flushing` 要是留在真，此后**这一格**
  // 的续发全部静默失效 —— 响应一条都发不出去，而且看不出是谁干的。
  //
  // 闸门与队必须同格（都用 `slot_of(id)`）：闸门的作用域是"这一格的队"的递归
  // 深度。取本线程那一格的话，跨循环调用时会在**别人的队**上跑迭代，而闸门
  // 却记在自己头上。
  struct guard {
    bool* flag;
    explicit guard(bool* f) : flag(f) { *flag = true; }
    ~guard() { *flag = false; }
  } g(&s->flushing);

  for (;;) {
    std::map<uvcpp_web_conn_id, out_queue >::iterator q =
        s->inflight.find(id);
    // 队列空了（`context_finished()` 会把空键摘掉），或者队首还没定稿 —— 停。
    // 后一种是正常的：队首还在跑异步处理器。
    if (q == s->inflight.end() || q->second.empty()) return;
    if (!q->second.front().response_ready) return;

    // 本栈握一份：下面 `release()` 很可能还掉最后一份 shared_ptr，把我们这个
    // 迭代器指向的元素连着销毁掉。**先清 `response_ready` 再动手** —— 它也要在
    // 元素还可能活着的时候写。
    std::shared_ptr<uvcpp_web_context> ctx = q->second.front().ctx;
    q->second.front().response_ready = false;

    // `send_response()` 里那道闸门现在放行了（它就是队首）。注意它是**直接**被
    // 调的，不是走 `finish()` —— 那条路已经被 `finished_` 挡成空操作了。
    send_response(*ctx);

    // 还掉"排队时那次 hold"。之后不得再碰 ctx，所以循环顶部重新 find。
    ctx->release();
  }
}

uvcpp_web_app& uvcpp_web_app::upload_route(http_method method,
                                           const std::string& pattern,
                                           const uvcpp_web_handler& handler) {
  // 先按普通流式路由注册（匹配、参数、优先级、链缓存全部白拿），再记一笔
  // "命中的时候要挂 multipart 机制"。
  stream_route(method, pattern, handler);
  upload_routes_.insert(route_key(method, pattern));
  return *this;
}

uvcpp_web_app& uvcpp_web_app::post_upload(const std::string& pattern,
                                          const uvcpp_web_handler& handler) {
  return upload_route(http_method::HTTP_POST, pattern, handler);
}

void uvcpp_web_app::reject_pipelining(uvcpp_web_context& ctx) {
  const uvcpp_web_conn_id id = ctx.connection_id();

  uvcpp_web_response& r = ctx.response();
  r.status(503);
  r.text("too many pipelined requests");
  // 不复用这条连接：同一个读缓冲里多半还排着更多请求，而它们一个都不该被处理
  // （正因为到上限才拒的）。`connection: close` 会被协议层的 keep-alive 判定
  // 读到（`uvcpp_http_server.cpp` 的 `has_header("connection")` 那一支），
  // 于是这条响应写完就关。
  r.set_header("connection", "close");
  r.end();

  // 空链 → `advance()` 立刻收尾 → `finish()`。不跑用户的路由链是有意的：
  // 这条请求根本没被受理，跑业务代码只会产生副作用和一个注定被丢掉的响应。
  ctx.run(empty_chain_);

  UVCPP_LOG_WARN(log_category::REQUEST)
      << "连接 " << id << " 的在途请求已达上限 "
      << cfg_.max_pipelined_requests << "，回 503 并关闭连接";
}

void uvcpp_web_app::reject_upload(uvcpp_web_context& ctx, int status,
                                  const std::string& message) {
  uvcpp_web_response& r = ctx.response();
  r.status(status);
  r.text(message);
  // 剩下的 body 没人消费了。不复用这条连接 —— 复用只会让下一个请求读到这次
  // 上传的残留字节，而那是纯粹的静默数据损坏。
  r.set_header("connection", "close");
  r.end();

  // **空链**：`run()` → `advance()` 发现没有下一环 → `finish()` → 把上面这份
  // 响应发出去。不跑用户的 handler 是有意的 —— 这条路由声明了"我只收
  // multipart"，格式不对时把请求交给用户，用户唯一能做的就是自己再判一次
  // 然后自己回 415，而这个判断框架刚刚已经做过了。
  ctx.run(empty_chain_);

  UVCPP_LOG_WARN(log_category::UPLOAD)
      << "上传路由拒绝了一次请求（status " << status << "，连接 "
      << ctx.connection_id() << "）：" << message;
}

bool uvcpp_web_app::wire_upload(uvcpp_web_context& ctx,
                                uvcpp_web_stream& stream) {
  uvcpp_web_request& req = ctx.request();

  if (upload_dir_.empty()) {
    // 配错了，不是请求错了 —— 500 而不是 4xx。上面 `upload_route()` 的文档
    // 说了必须配，这里兜住"忘了配"。
    UVCPP_LOG_ERROR(log_category::UPLOAD)
        << "上传路由没有配置落盘目录（`set_upload_dir()`），无法处理 "
        << req.path();
    reject_upload(ctx, 500, "上传未配置落盘目录");
    return false;
  }

  // 配置期就判定的安全错误（上传目录落在静态文档根里）。`set_upload_dir()`
  // 那时已经打过一条 ERROR，这里不再重复打日志 —— 但必须**拒绝**，因为
  // "接受一个把上传物暴露成静态资源的配置"正是这条检查要防的事。
  if (upload_dir_unsafe_) {
    reject_upload(ctx, 500,
                  "上传目录位于静态文档根内，按配置已被拒绝（详见启动日志）");
    return false;
  }

  // `web_multipart_boundary()` 一次回答两个问题，但**分两次回**给调用方：
  // 媒体类型不对是 415（"换个格式"），媒体类型对而 boundary 缺失/畸形是
  // 400（"修你的报文"）。合成一个 400 会让排障的人往错的方向找。
  const std::string boundary = web_multipart_boundary(req.content_type());
  if (boundary.empty()) {
    if (!req.is_multipart()) {
      reject_upload(ctx, 415,
                    "上传路由只接受 multipart/form-data，收到的是：" +
                        (req.content_type().empty() ? std::string("(无 Content-Type)")
                                                    : req.content_type()));
    } else {
      reject_upload(ctx, 400, "multipart/form-data 缺少可用的 boundary 参数");
    }
    return false;
  }

  // 用三重载：真实路径在**配置期**已经解析好了（`set_upload_dir()` 里），
  // 这里再解析一次就是每请求一次同步 realpath —— 那正是本项目第一条硬要求
  // 禁止的形状。传错会让落盘路径的包含判断失去意义，所以那一份必须来自
  // 配置期那唯一的一次解析。
  std::shared_ptr<uvcpp_web_upload> up =
      uvcpp_web_upload::create(loop(), upload_dir_, upload_dir_real_);
  if (up == nullptr) {
    UVCPP_LOG_ERROR(log_category::UPLOAD)
        << "上传会话创建失败（目录 " << upload_dir_ << "）";
    reject_upload(ctx, 500, "上传会话创建失败");
    return false;
  }
  up->set_fsync(upload_fsync_);

  const uvcpp_web_conn_id id     = ctx.connection_id();
  // 本条上传所属的 h2 流号（0 = HTTP/1.1）。下面每个按 id 找回上下文的动作都
  // 必须带上它 —— 一条 h2 连接上可以同时有好几条流在收体。
  const int32_t          sid = ctx.response().stream_id();

  // 工作池名额（**每提交一次 fs 操作取一个**，不是整个会话占一个）：会话绝大
  // 部分时间在等网络，把它算成一个长期在途任务会让闸门凭空窄掉一大截。
  //
  // 裸指针是有意的：闸门归 App 所有，而 App 的析构次序保证 `delete http_`
  // （循环就此关闭、此后不会有任何 libuv 回调）发生在成员析构**之前**，所以
  // 会话手里这个指针要么在循环活着时被用，要么根本不会被用。
  up->set_work_limit(work_limit_.get());

  // 背压通道：会话说"暂停读 / 继续读 / 掐断"时落到流层。
  //
  // **按 id 查，不捕 `&stream`**：会话的异步完成回调按值捕了
  // `shared_from_this()`，所以 `up` 可能比流对象活得久（停机、或者连接在落盘
  // 期间被别的路径收走），捕引用就会在那种时候读一个悬垂的流对象。见
  // `live_stream()` 的注释。
  uvcpp_web_upload_flow fl;
  fl.pause = [this, id, sid]() {
    uvcpp_web_stream* s = live_stream(id, sid);
    if (s != nullptr) s->pause();
  };
  fl.resume = [this, id, sid]() {
    uvcpp_web_stream* s = live_stream(id, sid);
    if (s != nullptr) s->resume();
  };
  fl.abort = [this, id, sid](int status) {
    // "暂停了还在灌"导致的缓冲越界（`set_max_pending_bytes`）走到这里：会话
    // 已经判定这是不该发生的事，流层负责回状态码并关连接。
    uvcpp_web_stream* s = live_stream(id, sid);
    if (s != nullptr) s->abort(status);
  };
  up->set_flow(fl);

  // 解析器与会话的生存期：**由流对象的框架钩子兜住**。
  //
  // 钩子是按值存在流对象里的 `std::function`，下面三个 lambda 按值捕获
  // `shared_ptr<uvcpp_web_upload>` —— 于是"会话活到最后一个回调跑完"这件事
  // 不需要额外的生命周期设计，它跟着流对象走，而流对象的生存期等于上下文。
  //
  // 解析器（`uvcpp_web_multipart`）不可拷贝，所以放进 `shared_ptr` 再捕获；
  // 它持有 sink 的**裸指针**（`up.get()`），而 `up` 由同一个 `shared_ptr`
  // 兜着 —— 两个捕获活在同一批钩子里，一起生一起死。
  std::shared_ptr<uvcpp_web_multipart> mp(new uvcpp_web_multipart());
  if (!mp->set_boundary(boundary)) {
    // 边界长度超 RFC 2046 的 70 字节上限（或者空）。这是**对端**的问题，
    // 不是配置问题 —— 400。
    reject_upload(ctx, 400, "multipart boundary 超出 RFC 2046 的长度上限");
    return false;
  }
  mp->set_sink(up.get());

  // 五条**解析器**上限（第六条"总长"不在这里 —— 它必须在喂**之前**判，见
  // 下面 `hooks.on_data` 的注释）。
  //
  // 全 App 一份、没有按路由覆盖：这几条限制的是"这台服务器愿意为一次上传付出
  // 多少资源"，与"哪个路由"无关；做成按路由只会多出一份必须同步维护的配置，
  // 而配置漂移正是上限最容易失效的方式。
  mp->set_max_part_header_bytes(max_part_header_bytes_);
  mp->set_max_file_count(max_upload_files_);
  mp->set_max_field_count(max_form_fields_);
  mp->set_max_file_size(max_file_size_);
  mp->set_max_field_size(max_field_size_);

  // 会话的完成回调 = 上传的**唯一**收口点。两条路都到这里：
  //   - 解析正常结束：`notify_parse_done(true)` → 落盘写完 → 成功；
  //   - 落盘失败：会话自己按失败收场。
  //
  // 拿到结果之后要做的是同一件事：把结果挂到请求上（失败时挂 nullptr），
  // 再**放出被扣住的用户 on_end**。放出的动作按 conn id 找上下文再做，
  // 而不是捕获 `&stream`/`&ctx` —— 见 `release_end()` 的注释。
  // 这个 lambda **故意不捕获 `up`**：`done_cb_` 是会话自己的成员，捕获它就
  // 成了 `up → done_cb_ → up` 的引用环，会话永远不会析构。不捕获是安全的 ——
  // 回调跑的这一刻，"谁调起来的"（某个异步完成回调，或者流对象的钩子）必然
  // 还握着一份 `up`。
  up->set_done_callback([this, id, sid](const uvcpp_web_upload_result& r, bool ok,
                                        const std::string& err) {
    // **按"谁在收请求体"找，不是按 conn id 一查一条。** 流水线之后一条连接上
    // 可以同时挂着好几条上下文，按 id 取到的那条很可能是**别人** —— 那会把这次
    // 上传的结果挂到隔壁请求头上。这一刻 `end_deferred_` 为真，`active_body_ctx()`
    // 的判据正是照这个写的；h2 上更是必须连流号一起对，否则两条并发上传会串。
    //
    // 本栈握一份 shared_ptr：`release_end()` 会跑用户的 `on_end`，用户在里面把
    // 响应收尾，上下文可能当场收场。
    std::shared_ptr<uvcpp_web_context> cp = active_body_ctx(id, sid);
    if (!cp) {
      // 上下文已经没了（停机、或者连接在落盘期间被别的路径收走）。此时
      // **什么都不做**是对的：没有响应要发，临时文件的所有权也已经定了
      // （失败时框架删、成功时留在盘上等 handler —— 而 handler 已经不在了）。
      UVCPP_LOG_WARN(log_category::UPLOAD)
          << "落盘完成时连接 " << id << " 的上下文已收场，结果被丢弃"
          << (ok ? "（文件留在 " + upload_dir_ + "）" : "（临时文件已删）");
      return;
    }

    uvcpp_web_context& c = *cp;
    c.request().set_upload(ok ? &r : nullptr);
    if (!ok) {
      UVCPP_LOG_WARN(log_category::UPLOAD)
          << "上传失败（连接 " << id << "）：" << err;
    }
    // `release_end()` 会跑用户的 `on_end`，用户在里面读 `req.upload()`。
    uvcpp_web_stream* s = c.stream();
    if (s != nullptr) s->release_end();
  });

  uvcpp_web_stream_hooks hooks;

  hooks.on_data = [this, mp, up, &stream](const char* data, size_t len) {
    // ① 总长上限：**判在喂之前**，而且判的是"喂完会不会超"。
    //
    // 为什么不能像其余五条那样转给解析器：解析器里的计数是"已经喂进来多少"，
    // 它唯一能判的时机是喂**之后** —— 那意味着越界的那一块会先落盘、再连同
    // 半截文件一起被删掉。而"先落盘再删"正是这条上限要防的东西（磁盘写入本身
    // 就是资源，收到 1 GiB 再拒绝等于让人白写了 1 GiB）。
    //
    // 为什么不能依赖 `body_limit` 中间件：它判的是**声明的** `content-length`，
    // 对 chunked（没有声明值）无条件放行 —— 而 chunked 恰恰是最自然的攻击
    // 形态。流式请求又整个绕过了 http 层的 `max_body_size_`。所以框架必须有
    // 自己的字节计数，这就是它。
    if (max_upload_size_ > 0 &&
        mp->received() + static_cast<uint64_t>(len) > max_upload_size_) {
      stream.abort(static_cast<int>(http_status::PAYLOAD_TOO_LARGE));
      return;
    }

    // ② 喂。解析器自己判相位、边界，以及上面装进去的那五条上限。
    const uvcpp_web_multipart_result fr = mp->feed(data, len);

    // ③ 上限族的错误 → **框架**回状态码并关连接（用户的 `on_end` 不再触发）。
    //
    // `abort()` 会同步走到 `hooks.on_abort` → `notify_parse_done(false)` →
    // `fail()`，也就是在本函数的栈上重入会话去删掉本次的临时文件 ——
    // 那正是"413 之后盘上不留半截文件"的实现处。`up` 由 `shared_ptr` 兜着，
    // 所以这里不必担心它被这一步销毁。
    const int status = upload_limit_status(fr);
    if (status != 0) stream.abort(status);
  };

  hooks.on_end = [mp, up]() -> bool {
    // 返回 true = "用户的 on_end 我先扣着"。这一步必须扣着：下面这句只是把
    // 解析器收尾 + 把最后一笔写提交出去，真正的 fsync/close 还在线程池上，
    // 而用户在 on_end 里读 `req.upload()` 时必须读到**填好的**结果。
    const uvcpp_web_multipart_result fr = mp->finish();
    const bool ok = (fr == uvcpp_web_multipart_result::DONE);
    up->notify_parse_done(ok);
    return true;
  };

  hooks.on_abort = [up]() {
    // 对端断了 / 框架掐断 → 会话按失败收场，**本次创建的临时文件全部删掉**。
    // 这是"失败时框架删"那个决策的落地处。
    up->notify_parse_done(false);
  };

  stream.set_framework_hooks(hooks);
  req.set_upload(nullptr);  // 成功之前恒为空：结果在 on_end 那一刻才存在
  return true;
}

http_stream_handler uvcpp_web_app::claim_stream(uvcpp_http_request& req,
                                                uvcpp_tcp_client* client) {
  // 一条流式路由都没注册过：**一次路径解析都不做**。
  //
  // 这条早返回不是优化而是必需的：钩子对**每一个**请求都会被问到（框架恒装
  // 它），而绝大多数服务根本不收 body。少了它，每个 GET 都要白付一次
  // `web_split_path_query` + `web_url_decode`（两次分配）。
  if (stream_router_.route_count() == 0) return http_stream_handler();

  // 路径算法必须与 `uvcpp_web_request::take_from` 和 `on_ws_upgrade` **完全
  // 一致**，否则同一个 URL 在流式表和普通表里会匹配到不同的路径。
  std::string raw_path;
  std::string query_string;
  web_split_path_query(req.url, raw_path, query_string);
  const std::string path =
      web_collapse_slashes(web_url_decode(raw_path, /*plus_as_space=*/false));

  web_route_match m = stream_router_.match(req.method, path);
  if (m.result != web_route_result::MATCHED || m.handler == nullptr) {
    return http_stream_handler();  // 不认领：请求走原来的累积路径
  }

  return dispatch_stream(m, req, client);
}

http_stream_handler uvcpp_web_app::dispatch_stream(const web_route_match& m,
                                                   uvcpp_http_request& req,
                                                   uvcpp_tcp_client* client) {
  uvcpp_web_conn_id id = reg_here().id_of(client);
  if (id == UVCPP_WEB_INVALID_CONN_ID) {
    // 与 `on_http_request` 同一条兜底：正常路径到不了（连接在 accept 时就
    // 登记了），真到了说明有人绕过 App 的 accept 钩子直接用了 HTTP 层。
    std::string ip;
    int port = 0;
    if (client != nullptr) client->getPeerAddrs(ip, port);
    id = reg_here().add(client, ip, port, loop_now_ms(loop()));
    UVCPP_LOG_WARN(log_category::CORE)
        << "流式请求来自未登记的连接，已补登记为 " << id;
  }

  std::shared_ptr<uvcpp_web_context> ctx = uvcpp_web_context::create(*this, id);

  // 此刻的 `req` 只有 method/url/version/headers —— body 一个字节都还没到，
  // 而且**永远不会**进 `ctx->request().body_*()`（HTTP 层不会再累积它）。
  //
  // 这一次 `take_from` 是**搬**（头向量与 URL 都换手），所以认领之后 HTTP 层
  // 那份 `stream_request` 的头与 URL 就空了。本 App 侧没有读它的地方：返回给
  // HTTP 层的接管 handler **三个都只捕获 `ctx`**，形参 `uvcpp_http_request&`
  // 连名字都没写。**用户自己的流式处理函数不一样** —— 它拿到的就是那个对象，
  // 要读头/URL 就得在 HEADERS 那一次的事件里读完（见 `uvcpp_web_request.h`
  // 的「关于拷贝」一节）。
  ctx->request().take_from(req);

  const uvcpp_web_connection* conn = reg_find(id);
  if (conn != nullptr) {
    ctx->request().set_peer(conn->peer_ip,
                            static_cast<unsigned int>(conn->peer_port));
  }

  for (size_t i = 0; i < m.params.size(); ++i) {
    ctx->request().set_param(m.params[i].first, m.params[i].second);
  }

  // 声明长度能拿到就告诉流对象，拿不到（chunked）就标成"长度未知" ——
  // 进度回调会据此把 total 报成 0，而不是报一个假的 0。
  const uint64_t declared = static_cast<uint64_t>(ctx->request().content_length());
  uvcpp_web_stream* stream = ctx->attach_stream(declared, /*has_total=*/declared > 0);

  // ---- 上传路由：把 multipart 解析 + 落盘挂上去 ----
  //
  // 位置很关键：在 `ctx->run()` **之前**。格式不对时框架在这里就把 415/400
  // 回了，用户的 handler 一次都不跑 —— 这条路由声明过"我只收 multipart"，
  // 格式不对时把请求交给用户，用户唯一能做的就是自己再判一次。
  if (m.pattern != nullptr &&
      upload_routes_.find(route_key(req.method, *m.pattern)) !=
          upload_routes_.end()) {
    if (!wire_upload(*ctx, *stream)) {
      // `wire_upload` 已经在里面填好响应、走空链发出去了。这里只需要**接住
      // 后续的 body**：连接正在关闭，但关闭完成之前还可能来几块，而 HTTP 层
      // 认领之后就必须由我们消费到底（不接的话它没有别的去处）。
      return [](http_stream_event, const char*, size_t, uvcpp_http_request&,
                uvcpp_tcp_client*) {};
    }
  }

  // 停顿保护的两半（见 `uvcpp_web_connection.h` 的 `activity_since`）：
  // 这一标记让闲置扫描改按 `last_read_ms` 计时，于是 `idle_timeout_ms` 对
  // 上传的含义变成"**停顿**多久算死"，而不是"整个上传必须在多久内传完"。
  //
  // 没有它，一个持续有字节的慢上传会被整段预算误杀；有了它但 `inflight`
  // 那份豁免还在的话，卡死的上传又永远关不掉 —— 两处都要改才成立。
  //
  // 流水线之后这条豁免变成"**这条连接上还有任何在途请求**"（`inflight` 的键
  // 还在就成立），所以第二条请求也会把第一条的上传一起豁免掉。收场时由
  // `context_finished()` 按事实重报一次（那里会连 `request_start_ms` 一起重新
  // 起算），不在这里补。
  reg_mark_streaming(id, true);

  sync_chains();

  const std::vector<uvcpp_web_handler>* chain = build_chain(m.handler);
  if (chain == nullptr) {
    // 正常路径到不了：`m.handler` 非空是上面刚判过的。留着兜住"路由表在
    // 认领途中被改"这种极端情况。
    UVCPP_LOG_ERROR(log_category::ROUTER)
        << "流式路由没有可用的处理器链，回 500";
    ctx->response().server_error();
    ctx->response().end();
    chain = &empty_chain_;
  }

  // 与 `dispatch()` 同一个约定：**挂号必须在 `run()` 之前**，而且挂在**队尾**
  // —— 组内顺序就是请求到达顺序，也就是响应必须发出去的顺序。
  if (enqueue_inflight(id, ctx)) {
    reject_pipelining(*ctx);
    // 连接正在关闭，但关闭完成之前还可能来几块 body：HTTP 层把这条连接认领给
    // 我们之后，body 就必须有人消费到底（与 `wire_upload()` 失败那条路径同一
    // 个理由）。空 handler 就是"照单收下、丢掉"。
    return [](http_stream_event, const char*, size_t, uvcpp_http_request&,
              uvcpp_tcp_client*) {};
  }

  ctx->run(*chain);

  // 返回给 HTTP 层的接管 handler。**它捕获 shared_ptr 是本设计的关键**：
  // 这个 handler 由 HTTP 层的 `conn_ctx::stream_handler` 持有（一个
  // `std::function`），只要消息没结束，上下文就不会因为 App 侧摘号而析构
  // —— 于是"用户 handler 已经同步跑完、body 还在路上"这个窗口里，
  // `ctx` 一定活着。
  //
  // 除了 `ctx` 什么都不捕获：`m` 里的东西（params、handler 指针）在调用户
  // handler 之前就已经用完并转交给请求对象了，而这个 handler 只做"把后续
  // 事件转给上下文"这一件事。
  return [ctx](http_stream_event ev, const char* data, size_t len,
               uvcpp_http_request&, uvcpp_tcp_client*) {
    switch (ev) {
      case http_stream_event::BODY:
        // 走 `ctx->stream_deliver()` 而不是直接碰流对象：那一层带着
        // "已经收尾 / 已中止 → 直接丢弃"的判断。链可能因为中间件短路而提前
        // 跑完，而 body 还会照来 —— 那不是错误，不该报出来。
        ctx->stream_deliver(data, len);
        break;
      case http_stream_event::END:
        ctx->stream_end();
        break;
      default:
        // HEADERS 不会再到达：认领发生在它内部，返回时已经用掉了。
        break;
    }
  };
}

std::shared_ptr<uvcpp_web_static> uvcpp_web_app::serve_static(
    const std::string& prefix, const std::string& root_dir,
    const uvcpp_web_static_options& opts) {
  std::shared_ptr<uvcpp_web_static> st(new uvcpp_web_static(root_dir, opts));

  // 把 App 的工作池闸门装上。静态路径**只能拒绝**（请求已经整包读完了，
  // 没有退路），所以它拿到的是一个"满了就回 503"的闸门 —— 与上传路径
  // `stream->pause()` 的分工不同，理由见 `uvcpp_web_work_limit`。
  st->set_work_limit(work_limit_);

  // 记下这个文档根的**真实路径**（构造时已经解析过一次，这里只是取回来，
  // 不重新解析）。用途是"上传目录不得落在静态根里"那条检查 —— 它必须在
  // `serve_static()` 这里也查一遍，因为用户完全可能先 `set_upload_dir()`
  // 再挂静态根，那样 `set_upload_dir()` 那一刻还不知道有这个根。
  if (!st->root_real().empty()) {
    static_roots_real_.push_back(st->root_real());
    check_upload_dir_containment();
  }

  // 前缀归一化：保证前导 `/`、去掉尾部 `/`（根前缀除外）。
  // 尾部斜杠必须去掉 —— 否则 `p + "/*path"` 会拼出 `/assets//*path`，
  // 而路由器是按段切的，空段会被直接吃掉，拼出来的模式就不是想表达的那个了。
  std::string p = prefix;
  if (p.empty()) p = "/";
  if (p[0] != '/') p = "/" + p;
  while (p.size() > 1 && p[p.size() - 1] == '/') p.resize(p.size() - 1);

  // 循环要到请求时才取 `loop()`：注册期循环还不存在（它在 `start()` 里才由
  // tcp_server 建出来），所以这里按值捕获 App 指针，发请求时再问它要。
  uvcpp_web_app* self = this;

  // `st` 按值捕获：handler 被路由器持有，路由器是 App 的成员，两者同生共死，
  // 所以不存在"handler 还活着而静态对象已经没了"的窗口。
  uvcpp_web_handler h = [self, st](uvcpp_web_request& req,
                                   uvcpp_web_response& resp,
                                   uvcpp_web_next next) {
    st->serve(req, resp, next, self->loop());
  };

  if (p == "/") {
    // 根挂载要单独注册 `/`：通配至少吃一段，`/*path` 匹配不到它。
    this->get("/", h);
    this->head("/", h);
    this->get("/*path", h);
    this->head("/*path", h);
  } else {
    this->get(p, h);
    this->head(p, h);
    this->get(p + "/*path", h);
    this->head(p + "/*path", h);
  }

  return st;
}

// =========================================================================
// WebSocket
// =========================================================================

uvcpp_web_app& uvcpp_web_app::enable_wss() {
  // 幂等：`websocket()` 会隐式调到这里，用户也可能自己先调一次。
  if (ws_server_ != nullptr) return *this;

  // 构造时**不接管** on_upgrade（那是下面这一行的事）：分派权归框架，
  // 因为框架要按路径选处理器，而 ws_server 只能给出一条全局的 on_connection。
  ws_server_ = new uvcpp_ws_server(http_);

#if UVCPP_ZLIB_ENABLE
  // 把攒下来的压缩策略打过去。默认值就是 `uvcpp_ws_server` 自己的默认值
  // （两边都是 `uvcpp_ws_deflate_config{}`），所以没调过 setter 时这一句是
  // 空操作 —— 它的意义是让**调用顺序无关**：`set_ws_compression()` 在这之前
  // 调也照样生效。
  ws_server_->set_compression(ws_deflate_cfg_);
#endif

  uvcpp_web_app* self = this;
  http_->on_upgrade([self](uvcpp_http_request& req, uvcpp_tcp_client* client) {
    self->on_ws_upgrade(req, client);
  });
  return *this;
}

#if UVCPP_ZLIB_ENABLE
uvcpp_web_app& uvcpp_web_app::set_ws_compression(
    const uvcpp_ws_deflate_config& cfg) {
  ws_deflate_cfg_ = cfg;
  // 服务已建好就直接打过去；还没建就等 `enable_wss()` 那一句。
  if (ws_server_ != nullptr) ws_server_->set_compression(cfg);
  return *this;
}

uvcpp_ws_deflate_config uvcpp_web_app::get_ws_compression() const {
  // 建好了读**它**的当前值 —— 逃生口 `ws_server()->set_compression()` 改过
  // 的话，读存下来那份就在这里说谎了。
  if (ws_server_ != nullptr) return ws_server_->get_compression();
  return ws_deflate_cfg_;
}
#endif

uvcpp_web_app& uvcpp_web_app::websocket(
    const std::string& pattern, const uvcpp_web_ws_handler& handler) {
  if (!handler) {
    UVCPP_LOG_WARN(log_category::WEBSOCKET)
        << "拒绝注册空 WS handler：" << pattern;
    return *this;
  }

  enable_wss();

  // 路由表里放的是**占位 handler**：它永远不会被调用（WS 分派不走
  // `uvcpp_web_router` 的执行路径，只借它的匹配），存在的唯一理由是
  // `uvcpp_web_router::add` 会拒绝空 handler。
  //
  // 用 `any()` 而不是 `get()`：升级请求按 RFC 6455 §4.1 是 GET，但让它的
  // 匹配方法无关，可以省掉一个"哪天升级用了别的方法就静默 404"的暗坑。
  if (!ws_router_.any(pattern, [](uvcpp_web_request&, uvcpp_web_response&,
                                  uvcpp_web_next) {})) {
    return *this;  // 模式非法，add 已经记了 WARN
  }

  // 键必须是 `add()` 实际存进去的那个串 —— 也就是匹配结果里 `pattern` 会指向
  // 的串。注册时 `add()` 会补前导 `/`、拼上分组前缀，所以这里跟着归一化一次，
  // 否则 `websocket("chat")` 注册进去、`/chat` 匹配出来按 `"/chat"` 查表就查不到。
  ws_handlers_[ws_router_.full_pattern(pattern)] = handler;
  return *this;
}

void uvcpp_web_app::on_ws_upgrade(uvcpp_http_request& req,
                                  uvcpp_tcp_client* client) {
  // **先算路径，且不碰请求。** 用的是与 `uvcpp_web_request::take_from` 完全
  // 相同的两个自由函数，所以 WS 路由与 HTTP 路由看到的路径一定是同一个。
  //
  // 为什么强调"不碰请求"：没命中就要回落成普通 HTTP 请求，那条路要能拿到
  // 完整的 body。先 `take_from()` 再回落的话，body 已经被搬走一次了。
  std::string raw_path;
  std::string query_string;
  web_split_path_query(req.url, raw_path, query_string);
  const std::string path =
      web_collapse_slashes(web_url_decode(raw_path, /*plus_as_space=*/false));

  bool hit = false;
  if (ws_server_ != nullptr && !ws_handlers_.empty()) {
    web_route_match m = ws_router_.match(http_method::HTTP_GET, path);
    if (m.result == web_route_result::MATCHED && m.pattern != nullptr) {
      auto it = ws_handlers_.find(*m.pattern);
      if (it != ws_handlers_.end()) {
        hit = true;
        dispatch_ws(m, it->second, req, client);
      }
    }
  }

  if (hit) return;

  // ---- 没命中：回落成普通 HTTP 请求 ----
  //
  // 这一条不能省。升级请求落到这里通常意味着"客户端连错了路径"或者"这段
  // 路径本来就是个普通 GET 路由"。**什么都不做是最坏的选择** —— 连接会哑在
  // 那里，既不断也不回，客户端只能等超时。
  //
  // 走 `on_http_request()` 而不是自己回 404：这样同路径上注册的普通 GET 路由
  // 照样能命中（`app.get("/x")` + 客户端发 `Upgrade: websocket` 到 `/x`），
  // 真没有路由时它自己会回 404。
  uvcpp_http_response resp;
  on_http_request(req, resp, client);
}

void uvcpp_web_app::dispatch_ws(const web_route_match& m,
                                const uvcpp_web_ws_handler& handler,
                                uvcpp_http_request& req,
                                uvcpp_tcp_client* client) {
  // **缺 `Sec-WebSocket-Key` 时必须自己回 400。**
  // `uvcpp_ws_server::handle_upgrade` 对缺 key 的请求是**静默 return** 的
  // （它那时还没写过任何字节，也没人告诉它该回什么），于是连接会哑掉。
  // 校验放在这里，因为框架这一侧才知道"这是个该被拒绝的升级请求"。
  if (http_get_header(req.headers, "sec-websocket-key").empty()) {
    UVCPP_LOG_WARN(log_category::WEBSOCKET)
        << "升级请求缺少 Sec-WebSocket-Key，回 400："
        << (m.pattern != nullptr ? *m.pattern : std::string("?"));
    // 直接交给 HTTP 层发（与 `send_response(ctx)` 同一个出口）：压缩、keep-alive
    // 判定、写串行化都在那里面。这里没有 `uvcpp_web_context`（请求压根没进路由），
    // 所以拿不到那条统一出口，只能走 HTTP 层的。
    uvcpp_web_response resp;
    resp.status(http_status::BAD_REQUEST)
        .set_header("connection", "close")
        .text("Bad Request");
    http_->send_response(client, resp.raw());
    return;
  }

  // 会话是**异步**建起来的（101 的写完成回调里），所以请求视图得活到那一刻。
  // 用 shared_ptr 挂着，由那个回调持有 —— 闭包销毁时它自然释放，不需要
  // 任何清理路径（包括"101 写失败、回调根本没被调用"那条）。
  //
  // **视图从一份副本上建，不从 `req` 本身建。** `uvcpp_web_ws_request` 的构造
  // 走的是 `req_.take_from(raw)`，而 take_from 是**搬**：它会把 `headers` 与
  // `url` 从源请求上换手。可下面 `ws_server_->handle_upgrade(req, ...)` 还要从
  // **同一个** `req` 上读 `Sec-WebSocket-Key` 与 `Sec-WebSocket-Extensions`
  // —— 缺 key 时 `handle_upgrade` 第一句就 `return`，对端一个字节都收不到
  // （症状是"升级请求没有响应，连接也不断"）。升级是每条连接一次的事，这一次
  // 拷贝不在热路径上。
  uvcpp_http_request ws_src(req);
  std::shared_ptr<uvcpp_web_ws_request> ws(new uvcpp_web_ws_request(ws_src));
  ws->set_route(*m.pattern);
  for (size_t i = 0; i < m.params.size(); ++i) {
    ws->request().set_param(m.params[i].first, m.params[i].second);
  }

  // 对端地址与 HTTP 请求走**同一个来源**（连接登记表）。
  uvcpp_web_conn_id id = reg_here().id_of(client);
  const uvcpp_web_connection* conn = reg_find(id);
  if (conn != nullptr) {
    ws->set_peer(conn->peer_ip, static_cast<unsigned int>(conn->peer_port));
  }

  uvcpp_web_app* self = this;
  ws_server_->handle_upgrade(
      req, client,
      [self, ws, handler, client](uvcpp_ws_connection* c) {
        ws->bind_connection(c);

        // **豁免闲置超时要在这里登记，不能在外面登记。**
        // 外面登记的话，101 写失败（对端提前断开）时这个 id 会永远留在
        // `upgraded` 里 —— 而那条连接随后就断了，登记表里也没了它，
        // 这个集合就成了一个只涨不落的泄漏。而这个回调**只在真建了会话时**
        // 才被调用，所以它是唯一正确的登记点。
        uvcpp_web_conn_id cid = self->reg_here().id_of(client);
        // 按 **id** 找格，不按本线程 —— 与下面 `idle_sweep()` / `on_close()`
        // 那两处读它的地方同一把钥匙（三者必须落在同一份集合上）。id 无效
        // 时 `slot_of()` 给 nullptr，登记自然也跳过。
        loop_slot* up = self->slot_of(cid);
        if (up != nullptr) up->upgraded.insert(cid);

        UVCPP_LOG_INFO(log_category::WEBSOCKET)
            << "WS 升级成功 " << ws->route() << " (" << ws->peer_ip() << ")";
        handler(*ws);
      });
}

size_t uvcpp_web_app::ws_session_count() const {
  return ws_server_ != nullptr ? ws_server_->session_count() : 0;
}

size_t uvcpp_web_app::ws_recycled_session_count() const {
  return ws_server_ != nullptr ? ws_server_->recycled_session_count() : 0;
}

// =========================================================================
// 钩子
// =========================================================================

uvcpp_web_app& uvcpp_web_app::on_connection(
    const std::function<void(uvcpp_web_conn_id, uvcpp_tcp_client*)>& cb) {
  if (cb) connection_cbs_.push_back(cb);
  return *this;
}

uvcpp_web_app& uvcpp_web_app::on_connection_close(
    const std::function<void(uvcpp_web_conn_id, uvcpp_tcp_client*)>& cb) {
  if (cb) connection_close_cbs_.push_back(cb);
  return *this;
}

uvcpp_web_app& uvcpp_web_app::on_raw_tcp_data(
    const std::function<void(uvcpp_web_conn_id, uvcpp_tcp_client*,
                             const char*, size_t)>& cb) {
  if (cb) raw_data_cbs_.push_back(cb);
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_raw_data_claim(
    const uvcpp_web_raw_data_claim& cb) {
  raw_data_claim_ = cb;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::clear_raw_data_claim() {
  raw_data_claim_ = uvcpp_web_raw_data_claim();
  return *this;
}

// =========================================================================
// 生命周期
// =========================================================================

int uvcpp_web_app::start() { return start_background(); }

int uvcpp_web_app::start_background() {
  if (thread_started_ || loop_started_.load() || started_once_) return UV_EBUSY;

  threading_ = true;

  // promise 交到线程手里：`set_value` 之后调用方才能解除阻塞，而 promise
  // 必须活得比线程久 —— 所以用 shared_ptr，而不是栈上那个（它的生存期到
  // 本函数返回就结束了）。
  std::shared_ptr<std::promise<int> > ready =
      std::make_shared<std::promise<int> >();
  std::future<int> fut = ready->get_future();

  thread_ = std::thread([this, ready]() {
    // 与 `run()` 同一个次序，理由见那里（每进程那半里有 `set_loops(n)`
    // 这个放行点）。
    int rc = init_process_once();
    if (rc == 0) rc = init_on_loop_thread();
    // `running_` 必须和 `loop_started_`/`bound_port_` 一样，在**放行调用方之前**
    // 置位。它们三个是同一批"已经就绪"的标志（前两个在 init_on_loop_thread()
    // 里就置好了），而 `running()` 的契约写的是"`start()` 之后、循环退出之前" ——
    // 放在 set_value 后面就有个真实的窗口：`start_background()` 已经返回 0，
    // 调用方（以及测试）立刻查 `running()` 却还是 false。套件负载高时实测能撞上。
    if (rc == 0) running_ = true;
    ready->set_value(rc);  // 无论成败都要放行调用方
    if (rc != 0) {
      // 失败路径上循环还没跑过，没有任何回调会来收尾；把线程身份标记清掉，
      // 这个对象就回到"没启动"的状态，调用方改完配置还能重试。
      loop_slot& s = slot_here();
      std::lock_guard<std::mutex> lk(s.tid_mutex);
      s.tid_known = false;
      return;
    }

    UVCPP_LOG_INFO(log_category::CORE)
        << "开始服务 http://" << cfg_.host << ":" << bound_port_;
    http_->run(UV_RUN_DEFAULT);
    running_ = false;
    loop_started_ = false;
    UVCPP_LOG_INFO(log_category::CORE) << "事件循环已退出";
  });

  thread_started_ = true;

  const int rc = fut.get();  // 阻塞到 bind/listen 有结果

  if (rc != 0) {
    if (thread_.joinable()) thread_.join();
    thread_started_ = false;
    UVCPP_LOG_ERROR(log_category::CORE)
        << "启动失败，libuv 错误码 " << rc << "（" << uv_strerror(rc) << "）";
  }
  return rc;
}

int uvcpp_web_app::run(uv_run_mode md) {
  if (thread_started_ || loop_started_.load() || started_once_) return UV_EBUSY;

  threading_ = false;

  // 两半的先后是**语义要求**，不是随手排的：每进程那半里有 `set_loops(n)`
  // 这一步，而工作循环的线程正是在 `set_loops()` 里被放行的（`w->start()`，
  // `src/net/uvcpp_tcp_server.cpp:332`；`listen()` 在它之后，只管 bind +
  // `uv_listen()`）—— 放行之后它们立刻就会走到请求路径上，读的必须已经是
  // 冻结好的那份配置（设计稿 §4.1.1 甲）。
  int rc = init_process_once();
  if (rc == 0) rc = init_on_loop_thread();
  if (rc != 0) {
    loop_slot& s = slot_here();
    std::lock_guard<std::mutex> lk(s.tid_mutex);
    s.tid_known = false;
    return rc;
  }

  running_ = true;
  const int r = http_->run(md);
  running_ = false;
  loop_started_ = false;
  return r;
}

int uvcpp_web_app::init_process_once() {
  uvcpp_tcp_server* tcp = http_->get_tcp_server();
  uvcpp_loop* loop = tcp->get_loop();
  if (loop == nullptr || tcp->get_tcp() == nullptr) return UV_EINVAL;

  // --- 配置 --------------------------------------------------------
  uvcpp_logger::instance().set_level(cfg_.min_log_level);

  // `UV_THREADPOOL_SIZE` 必须在**进程启动之前**设好 —— libuv 第一次用到线程池
  // 时读一次就缓存住了，之后改环境变量毫无效果，也没有运行时的扩容 API。
  // 所以"现在"是最后一个还能提醒的时机：等业务跑起来再说就已经晚了。
  //
  // 这条 WARN 就是计划风险表里那句一直没落地的"不提供假的
  // set_threadpool_size()，改为启动时提醒"。
  //
  // **位置在 `set_level` 之后**：用户把等级调到 ERR/OFF 是在说"只告诉我出错
  // 了"，那这条配置建议就该跟着闭嘴 —— 放在 set_level 前面的话它永远按上一档
  // 阈值打印，`OFF` 也挡不住，就成了"配置了静默却还在被念"。
  if (!uvcpp_web_work_limit::threadpool_size_is_set()) {
    const size_t pool = uvcpp_web_work_limit::threadpool_size();
    UVCPP_LOG_WARN(log_category::CORE)
        << "UV_THREADPOOL_SIZE 未设置：libuv 线程池只有 " << pool
        << " 个线程，所有异步文件 IO / DNS 都排在它后面（当前工作池在途上限 "
        << work_limit_->limit() << "）。它**只在进程启动前设置才生效**"
        << "（libuv 只读一次，没有运行时扩容 API），现在改本进程已无效 —— "
        << "请在启动环境里设置，例如 UV_THREADPOOL_SIZE=" << (pool * 4)
        << "。";
  }

  // 上传总长没设上限的提醒。**位置同样是 `set_level` 之后**，理由与上面那条
  // `UV_THREADPOOL_SIZE` 一字不差（那里写过一个更长的版本，这里不重复）。
  //
  // 为什么默认值选"0 = 不限 + 提醒"而不是给一个默认上限：任何具体数字都会
  // **静默**否掉合法的大文件上传，而"默认配置下上传路径是个 DoS 面"这件事
  // 本身是可发现的 —— 让它在启动日志里说出来，比替用户猜一个数字好。
  //
  // 为什么不打在 `upload_route()` 注册的那一刻（计划里原本这么写）：注册发生在
  // 用户的 main 里，而日志等级是**到这里才应用**的。打在注册处的话，用户设了
  // ERR/OFF 还会被这条 INFO 级建议反复念叨，等于配置不生效。
  if (!upload_routes_.empty() && max_upload_size_ == 0) {
    UVCPP_LOG_WARN(log_category::UPLOAD)
        << "注册了 " << upload_routes_.size()
        << " 条上传路由，但没有设置上传总长上限（`set_max_upload_size()`）："
        << "默认 0 = 不限，上传体量只受对端与磁盘限制。其余五条上限仍然生效。";
  }

  // 「上传目录不在静态根里」这条检查的**第三个**时刻。前两个是
  // `set_upload_dir()` 与 `serve_static()`，但配置顺序有六种，只有三处合起来
  // 才覆盖完 —— 例如"先 serve_static() 再 set_upload_dir() 再 serve_static()"
  // 中间那次会被这里兜住。日志级别在**上面**才应用，所以打在这里的那条
  // ERROR 也才真正受用户配置控制（这与 `upload_route()` 那条 WARN 同一个理由）。
  check_upload_dir_containment();

  http_->set_max_body_size(cfg_.max_body_size);
  http_->set_max_header_bytes(cfg_.max_header_bytes);
  http_->set_max_url_bytes(cfg_.max_url_bytes);
#if UVCPP_ZLIB_ENABLE
  http_->set_compression_enabled(cfg_.compression);
  http_->set_compress_min_body_size(cfg_.compress_min_body_size);
#else
  if (cfg_.compression) {
    UVCPP_LOG_INFO(log_category::HTTP)
        << "构建时未启用 zlib，本配置下的 compression 不生效（响应将以明文发送）";
  }
#endif

  router_.set_auto_options(cfg_.auto_options);
  router_.set_head_as_get(cfg_.head_as_get);

  // 访问日志要覆盖整条链，所以插到**最前面**（中间件按注册序执行，第一个
  // 在最外层）。放在这里而不是 use() 里，是因为它是配置驱动的：
  // `set_access_log(false)` 应当能关掉它。
  if (cfg_.access_log) {
    middlewares_.insert(middlewares_.begin(), web_middleware_access_log());
    ++middleware_gen_;
  }

  // --- 钩子 --------------------------------------------------------

  // 连接层钩子：全部扇出到本对象的方法上，用户钩子在方法内部再扇出。
  http_->on_connection([this](uvcpp_tcp_client* client) { on_accept(client); });
  http_->on_connection_close(
      [this](uvcpp_tcp_client* client) { on_close(client); });

  // 原始数据钩子**无条件装**：装了之后没注册任何观察者/接管者的开销只是
  // 一次函数调用。反过来（按需装）会让"start 之后才注册观察者"这件事静默
  // 失效 —— 那种 bug 很难查。
  http_->set_raw_data_hook([this](uvcpp_tcp_client* client, const char* data,
                                  size_t len) {
    return handle_raw_data(client, data, len);
  });

  // 兜底 handler：**所有**请求都到这儿，路由在框架内做。HTTP 层的路由表
  // 因此恒为空，扫描成本为零，优先级/参数/405 也只有一份实现。
  http_->on_request([this](uvcpp_http_request& req, uvcpp_http_response& resp,
                           uvcpp_tcp_client* client) {
    on_http_request(req, resp, client);
  });

  // 流式认领钩子，与上面的兜底 handler 是**互补**的两条路：headers 解析完时
  // 先问这个钩子，命中就认领（此后兜底 handler 不会再被调用），没命中才轮到
  // 它。两条都装，`on_http_request` 对非流式请求的行为因此**一个字节都没变**。
  http_->set_stream_claim([this](uvcpp_http_request& req,
                                 uvcpp_tcp_client* client) {
    return claim_stream(req, client);
  });

  // --- 监听 --------------------------------------------------------

#if UVCPP_OPENSSL_ENABLE
  // TLS 必须排在 bind/listen **之前**：上下文是 accept 时读的，listen 之后再
  // 装就来不及了（accept 回调已经在跑）。
  //
  // 这里第二道校验是刻意的。`enable_ssl*()` 已经当场校验过一次，但链式
  // setter 的返回值只有一个 `*this`，它报不了错 —— 所以"用户以为配好了、
  // 其实没配上"这件事只有在这里才拦得住。**绝不能静默继续**：那样服务会以
  // 明文跑起来，而调用方以为自己开的是 HTTPS，这比启动失败坏得多。
  if (ssl_requested_) {
    if (!ssl_ctx_ || !ssl_ctx_->is_ready()) {
      UVCPP_LOG_ERROR(log_category::SSL)
          << "已请求 TLS 但上下文不可用，拒绝以明文启动：" << ssl_error_;
      return UV_EINVAL;
    }

    // HTTP/2 只在 TLS 上做（不做 h2c），所以"宣告 h2"这件事**挂在这里**：
    // 没有 TLS 就没有 ALPN，也就没有可协商的东西。
    //
    // 两件事必须同时做，缺一不可：
    //   1. **宣告**这份名单 —— 让对端在握手里挑一个；用户自己设过就是他的
    //      协议策略，比框架默认更权威，不覆盖。
    //   2. **打开**协议层 —— 只宣告不打开，等于把一条真 h2 连接喂给 llhttp，
    //      客户端拿到的是连接级错误而不是干净的降级；反过来只打开不宣告，
    //      `h2` 根本协商不出来，永远走不到。
    //
    // 名单**永远设**，不是一个"有 h2 才设"的条件：`set_http2_enabled(false)`
    // 的语义是"我不提供 h2"，不是"我什么协议都不宣告"。退成空名单时 OpenSSL
    // 压根不会调我们的选择回调，客户端拿到的是一个**没有协商结果**的握手 ——
    // 那看起来像这台服务器不支持 ALPN，与"明确答复你只有 http/1.1"是两件事。
    if (!ssl_ctx_->has_alpn_select()) {
      ssl_ctx_->set_alpn_select_protos(http2_enabled() ? kDefaultAlpn
                                                        : kHttp11Alpn);
    }
    if (http2_enabled()) http_->set_http2_enabled(true);

    tcp->set_ssl_context(ssl_ctx_.get());
  }
#endif  // UVCPP_OPENSSL_ENABLE

  int rc = http_->bind(cfg_.host.c_str(), cfg_.port);
  if (rc != 0) return rc;

  rc = http_->listen(cfg_.backlog);
  if (rc != 0) return rc;

  // 端口填 0 时由系统分配，实际端口只有 bind 之后才知道 —— 从监听句柄上
  // 读回来，调用方（`start()` 的等待方）拿到的是真实端口。
  bound_port_ = cfg_.port;
  if (cfg_.port == 0) {
    struct sockaddr_storage addr;
    int namelen = static_cast<int>(sizeof(addr));
    if (tcp->get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&addr),
                                    &namelen) == 0) {
      if (addr.ss_family == AF_INET) {
        bound_port_ = ntohs(
            reinterpret_cast<struct sockaddr_in*>(&addr)->sin_port);
      } else if (addr.ss_family == AF_INET6) {
        bound_port_ = ntohs(
            reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_port);
      }
    }
  }

  // 每进程的那半到这里为止。下面全是"每循环一份"的，换到另一个函数里 ——
  // 分界是「这个状态该有几份」，理由写在 `init_process_once()` 的声明处。
  return 0;
}

int uvcpp_web_app::init_on_loop_thread() {
  // 循环线程身份必须最先记下来：下面任何一步失败都不会有回调进来，而这个
  // 标记决定了 `post()` 是就地执行还是投递。
  //
  // **它现在落在 `listen()` 之后**（那一步在上面的每进程那半里）。这是安全的：
  // 循环要到调用方接着调 `http_->run()` 才跑起来，在那之前不会有任何回调进来，
  // 所以"标记晚了一小段"没有可观察后果。n>1 时也是同一个道理 —— 工作循环在
  // `set_loops()` 里就被放行了（`w->start()`，`src/net/uvcpp_tcp_server.cpp:332`），
  // 但它们记的是**自己那条线程**的身份。
  loop_slot& slot = slot_here();
  {
    std::lock_guard<std::mutex> lk(slot.tid_mutex);
    slot.loop_tid = std::this_thread::get_id();
    slot.tid_known = true;
  }

  uvcpp_tcp_server* tcp = http_->get_tcp_server();
  uvcpp_loop* loop = tcp->get_loop();
  if (loop == nullptr || tcp->get_tcp() == nullptr) return UV_EINVAL;

  // --- loop 亲和的句柄 ---------------------------------------------
  //
  // 放在最后：这两个句柄必须在 loop 线程上建，而前面任何一步失败都不该
  // 留下需要回收的句柄。
  int rc = 0;
  slot.async = new uvcpp_async();
  rc = slot.async->init([this](uvcpp_async*) { drain_posts(); }, loop);
  if (rc != 0) {
    delete slot.async;
    slot.async = nullptr;
    return rc;
  }
  // 这一格归属的那条循环。0 号在构造函数里就装过了（同一个指针），这里再
  // 写一次是为了让"`init_on_loop_thread()` 之后这格是完整的"这条成立 ——
  // 工作循环那几格没有构造函数这一步，全靠这里。
  slot.loop = loop;

  slot.shutdown_timer = new uvcpp_timer(loop);

  // 闲置超时扫描器。`idle_timeout_ms == 0` 时**不建句柄** —— 关掉的功能
  // 不该在运行时留下任何开销，哪怕只是一拍一次的空转。
  if (cfg_.idle_timeout_ms > 0) {
    slot.idle_timer = new uvcpp_timer(loop);
    const uint64_t iv =
        idle_sweep_interval_ms(cfg_.idle_timeout_ms);
    slot.idle_timer->start([this](uvcpp_timer*) { idle_sweep(); }, iv, iv);
  }

  started_once_ = true;
  loop_started_ = true;
  stopping_ = false;
  slot.shutdown_phase = 0;
  return 0;
}

void uvcpp_web_app::stop() {
  if (!loop_started_.load()) return;  // 没起来 / 已经停了

  // 幂等：只认第一个进来的。用 exchange 而不是 load+store，否则两个线程
  // 同时调 stop() 会各投递一次。
  if (stopping_.exchange(true)) return;

  if (on_loop_thread()) {
    begin_shutdown();
    return;
  }

  // 跨线程：唯一的 libuv 线程安全入口是 uv_async_send，所以包成一个任务
  // 投过去。（这也顺便说明 `post()` 是线程安全的。）
  post([this]() { begin_shutdown(); });
}

void uvcpp_web_app::join() {
  if (!thread_started_) return;

  if (thread_.joinable()) {
    if (thread_.get_id() == std::this_thread::get_id()) {
      // 从 loop 线程自己调 join() 就是等自己结束 —— 死锁。说清楚比挂住好。
      UVCPP_LOG_ERROR(log_category::CORE)
          << "join() 在事件循环线程上被调用，会死锁；已忽略";
      return;
    }
    thread_.join();
  }
  thread_started_ = false;
}

int uvcpp_web_app::bound_port() const { return bound_port_; }

bool uvcpp_web_app::running() const { return running_.load(); }

bool uvcpp_web_app::loop_started() const { return loop_started_.load(); }

uvcpp_tcp_server* uvcpp_web_app::tcp_server() {
  return http_ != nullptr ? http_->get_tcp_server() : nullptr;
}

// =========================================================================
// uvcpp_web_context_host
// =========================================================================

bool uvcpp_web_app::on_loop_thread() const {
  const loop_slot& s = slot_here();
  std::lock_guard<std::mutex> lk(s.tid_mutex);
  return s.tid_known && s.loop_tid == std::this_thread::get_id();
}

void uvcpp_web_app::post(std::function<void()> fn) {
  if (!fn) return;

  // 投给**发起者所在的那条循环**（设计稿 §5.2）。请求路径上那就是这条连接
  // 自己的循环；从非循环线程投递时 `slot_here()` 给的是 0 号，与今天"只有
  // 一条循环"时的行为逐字相同。
  loop_slot& s = slot_here();
  std::lock_guard<std::mutex> lk(s.post_mutex);
  if (s.async == nullptr) {
    // 循环没起来（或者已经收尾了）。**说出来** —— 静默丢弃投递任务会让
    // "异步处理器永远不回来"变成一桩悬案。
    UVCPP_LOG_WARN(log_category::CORE)
        << "事件循环不可用（未启动或已停止），投递的任务被丢弃";
    return;
  }
  s.post_queue.push_back(fn);
  // uv_async_send 是异步信号安全的，且不会阻塞 —— 握着锁调没问题，loop
  // 线程那边的 drain_posts 也要抢这把锁，但它不会回头等我们。
  s.async->send();
}

uvcpp_loop* uvcpp_web_app::loop() const {
  // **本线程那条循环**，不是"唯一那条"。请求路径上调用者就跑在这条连接的
  // 循环线程上，所以这就是连接自己那条（`serve_static()` 的 handler 要它，
  // 见设计稿 §4.1.1 己）。不在循环线程上时 `slot_here()` 给 0 号 —— 与今天
  // 逐字相同。`loop` 在构造函数里就装好了，所以 `start()` 之前也答得出。
  return slot_here().loop;
}

uvcpp_web_app::loop_slot& uvcpp_web_app::slot_here() {
  // n == 1：恒等，而且**一次锁都不多加**（§7："n=1 时不许出现多循环的
  // 残留"）。这一格在构造函数里就建好了，所以 `loops_` 恒非空。
  if (loops_.size() == 1) return *loops_[0];

  const std::thread::id me = std::this_thread::get_id();
  for (size_t i = 0; i < loops_.size(); ++i) {
    loop_slot& s = *loops_[i];
    std::lock_guard<std::mutex> lk(s.tid_mutex);
    if (s.tid_known && s.loop_tid == me) return s;
  }
  return *loops_[0];  // 不在任何循环线程上 ⇒ 0 号（§5.2）
}

const uvcpp_web_app::loop_slot& uvcpp_web_app::slot_here() const {
  return const_cast<uvcpp_web_app*>(this)->slot_here();
}

// =========================================================================
// 登记表：本循环那一份，与"按 id 找份"（规则见头文件那一节）
// =========================================================================

uvcpp_web_connection_registry& uvcpp_web_app::reg_here() {
  return slot_here().registry;
}

const uvcpp_web_connection_registry& uvcpp_web_app::reg_here() const {
  return slot_here().registry;
}

uvcpp_web_app::loop_slot* uvcpp_web_app::slot_of(uvcpp_web_conn_id id) {
  // 0 是 `UVCPP_WEB_INVALID_CONN_ID`，它的高段也是 0 —— 不能让它落进 0 号
  // 那一格去查（那一格里查不到 0 这条记录，白跑一趟事小，"没登记过"与
  // "0 号循环的连接"混成一件事是错的语义）。
  if (id == UVCPP_WEB_INVALID_CONN_ID) return nullptr;
  const int idx = uvcpp_web_connection_registry::loop_of(id);
  if (idx < 0 || static_cast<size_t>(idx) >= loops_.size()) return nullptr;
  return loops_[static_cast<size_t>(idx)].get();
}

const uvcpp_web_app::loop_slot* uvcpp_web_app::slot_of(
    uvcpp_web_conn_id id) const {
  return const_cast<uvcpp_web_app*>(this)->slot_of(id);
}

uvcpp_web_connection_registry* uvcpp_web_app::reg_of(uvcpp_web_conn_id id) {
  loop_slot* s = slot_of(id);
  return s != nullptr ? &s->registry : nullptr;
}

const uvcpp_web_connection_registry* uvcpp_web_app::reg_of(
    uvcpp_web_conn_id id) const {
  return const_cast<uvcpp_web_app*>(this)->reg_of(id);
}

uvcpp_tcp_client* uvcpp_web_app::reg_client(uvcpp_web_conn_id id) const {
  const uvcpp_web_connection_registry* r = reg_of(id);
  return r != nullptr ? r->client(id) : nullptr;
}

const uvcpp_web_connection* uvcpp_web_app::reg_find(uvcpp_web_conn_id id) const {
  const uvcpp_web_connection_registry* r = reg_of(id);
  return r != nullptr ? r->find(id) : nullptr;
}

bool uvcpp_web_app::reg_note_read(uvcpp_web_conn_id id, int64_t now_ms) {
  uvcpp_web_connection_registry* r = reg_of(id);
  return r != nullptr && r->note_read(id, now_ms);
}

bool uvcpp_web_app::reg_note_request_done(uvcpp_web_conn_id id) {
  uvcpp_web_connection_registry* r = reg_of(id);
  return r != nullptr && r->note_request_done(id);
}

bool uvcpp_web_app::reg_mark_streaming(uvcpp_web_conn_id id, bool streaming) {
  uvcpp_web_connection_registry* r = reg_of(id);
  return r != nullptr && r->mark_streaming(id, streaming);
}

bool uvcpp_web_app::reg_is_streaming(uvcpp_web_conn_id id) const {
  const uvcpp_web_connection_registry* r = reg_of(id);
  return r != nullptr && r->is_streaming(id);
}

bool uvcpp_web_app::reg_touch(uvcpp_web_conn_id id, int64_t now_ms) {
  uvcpp_web_connection_registry* r = reg_of(id);
  return r != nullptr && r->touch(id, now_ms);
}

int64_t uvcpp_web_app::reg_activity_since(uvcpp_web_conn_id id) const {
  const uvcpp_web_connection_registry* r = reg_of(id);
  return r != nullptr ? r->activity_since(id) : 0;
}

size_t uvcpp_web_app::connection_count() const {
  size_t n = 0;
  for (size_t i = 0; i < loops_.size(); ++i) n += loops_[i]->registry.size();
  return n;
}

size_t uvcpp_web_app::connection_count_at(int loop_index) const {
  if (loop_index < 0 || static_cast<size_t>(loop_index) >= loops_.size()) {
    return 0;
  }
  return loops_[static_cast<size_t>(loop_index)]->registry.size();
}

uvcpp_tcp_client* uvcpp_web_app::connection(uvcpp_web_conn_id id) {
  return reg_client(id);
}

void uvcpp_web_app::send_response(uvcpp_web_context& ctx) {
  // -------------------------------------------------------------------
  // 发送闸门：**只有队首能发**
  // -------------------------------------------------------------------
  //
  // 流水线要求响应按请求顺序发出（RFC 7230 §6.3.2）。协议层的写队列本来就是
  // 按调用顺序 FIFO 的，所以顺序只要在这一层守住：前面还有别人的响应没发出去，
  // 这一条就先排着。
  //
  // 排队的做法是 `hold()` 一次然后返回。`finish()` 接着会看到 `hold_count_ > 0`，
  // 于是走"挂起收场"那一支（`pending_release_`）而**不**调 `context_finished()`
  // —— 上下文因此仍留在 `inflight` 里，`idle_sweep()` 的在途豁免与停机的宽限期
  // 都不用为"排队中的响应"另做一套判断（反过来，要是把它摘出表去另外记一笔，
  // 闲置超时就会把它当成"完全安静的连接"在 `idle_timeout_ms` 后杀掉）。
  //
  // 两种情况**不**排队，直接往下走：
  //   * 上下文不在表里（例如 `wire_upload()` 内部直接回错那条路径）—— 没人会
  //     来 flush 它，排了就是永远发不出去；
  //   * 它已经是队首 —— 那正是要发的一条。
  //
  // **h2 上这条闸门整条不适用。** 它守的是 HTTP/1.1 的流水线顺序，而 h2 的
  // 每条流各自独立：乱序响应不但合法，而且是必须允许的 —— 照 h1 排的话，
  // 一条慢流（比如一个正在等异步数据的 SSE）会把同一条连接上**后面所有**流的
  // 响应一起扣住，直到它自己超时。那是把 h2 用成了 h1。
  //
  // 跳过排队**不影响登记**：上下文照旧进 `inflight`，所以闲置豁免与停机
  // 宽限期那两处判断一行都不用改。
  if (ctx.response().stream_id() == 0) {
    // 队那一格与闸门同源（都问 id）。id 无效时 `slot_of()` 给 nullptr ⇒
    // 没有可排的队，与"它不在表里"是同一支：直接往下走。
    loop_slot* s = slot_of(ctx.connection_id());
    if (s != nullptr) {
      std::map<uvcpp_web_conn_id, out_queue >::iterator q =
          s->inflight.find(ctx.connection_id());
      if (q != s->inflight.end() && !q->second.empty()) {
        out_queue::iterator me = q->second.begin();
        while (me != q->second.end() && me->ctx.get() != &ctx) ++me;
        if (me != q->second.end() && me != q->second.begin()) {
          // 已经排过的别再 hold 一次：`flush_out()` 只还一次。
          if (!me->response_ready) {
            me->response_ready = true;
            ctx.hold();
          }
          return;
        }
      }
    }
  }

  uvcpp_web_response& r = ctx.response();

  // 先把 metadata 定下来（`sync_meta()` 是幂等的，发送前本来就一定要调一次）。
  // 放在 `info` 构造**之前**，是为了让下面那条"连接已断"的提前返回也拿到
  // 同一个基准：那条路上 `raw()` 不会被调用（见 `:1602` 分支）。
  //
  // 注意：**`sync_meta()` 不再替我们丢掉 HEAD 的 body 了**（那正是修 HEAD 与
  // GET 头不一致时让出去的 —— 压缩需要真 body 才能算出 GET 会发的长度）。
  // 所以 `body_bytes` 的"HEAD 时为 0"现在是**显式换算**出来的，见下面两处
  // `head_only() ? 0 : …`；不能再靠"读到的天然就是 0"。
  r.sync_meta();

  uvcpp_web_sent_info info;
  info.status_code   = r.status_code();
  info.connection_id = ctx.connection_id();
  info.ok            = true;

  uvcpp_tcp_client* client = reg_client(ctx.connection_id());
  if (client == nullptr) {
    // 连接在响应准备好之前断了（客户端提前走了，或者我们 abort 过）。
    // **照样通知** —— 访问日志中间件挂在这一刻上，丢掉通知就等于这次请求
    // 在日志里凭空消失。ok=false 就是给这种场合用的。
    info.ok         = false;
    info.body_bytes = r.head_only() ? 0 : r.body_size();
    UVCPP_LOG_WARN(log_category::RESPONSE)
        << "连接 " << ctx.connection_id() << " 已断开，响应被丢弃（status "
        << info.status_code << "）";
    r.notify_sent(info);
    return;
  }

  // 统一的 Server 头。响应自己设过就不覆盖 —— 有人会故意藏掉这个信息，
  // 那是他的选择。
  if (!cfg_.server_header.empty() && !r.has_header("server")) {
    r.set_header("server", cfg_.server_header);
  }

  // -------------------------------------------------------------------
  // 流式（chunked）响应：走另一条出口
  // -------------------------------------------------------------------
  //
  // 它不能走 `http_->send_response()`：那个函数**一次性**把整条报文
  // （头部 + body + 终止块）序列化进一个字符串，而流式的 body 此刻还
  // 不存在 —— 后面每一块都是异步产生的。
  if (r.streaming()) {
    const uvcpp_web_conn_id id = ctx.connection_id();

    // 流号从响应上取，不从 `ctx.request()` 取：两者此刻必然相等（派发时
    // 一起落的），而响应身上那个才是本层真正会用的那份。
    r.set_stream_sink(new app_stream_sink(this, id, r.stream_id()));

    // **按值捕指针而不是按引用捕那两个引用参数。** `ctx` 是引用形参、`r`
    // 是引用局部变量，"按引用捕获一个引用"在各编译器上的落地并不一致
    // （C++11 对闭包里是否为引用实体分配存储是未指定的）。显式取地址、
    // 按值捕指针，语义就没有第二种读法。
    //
    // 这两个指针的存活由 hold()/release() 保证：release() 就在这个回调
    // 的最后一句，而它只可能被 `stream_done_` 那道闸门放行一次。
    uvcpp_web_context* const cp = &ctx;
    uvcpp_web_response* const rp = &r;

    // 收尾回调。**这里是 notify_sent 被推迟到的时刻** —— 对整包响应而言
    // 它在头部入队之后就触发，但流式响应那时一个 body 字节都还没出去，
    // `body_bytes` 只能记 0、`ok` 只能记真。访问日志要的是"这条响应完整
    // 地是 200、N 字节"，那个答案只有在这里才成立。
    r.set_stream_finished_cb([cp, rp, id](int status) {
      uvcpp_web_sent_info si;
      si.status_code   = rp->status_code();
      // HEAD 上一字节都没发（`stream_bytes_written()` 记的是"GET 本该发
      // 多少"），而 `body_bytes` 的契约是"实际写入连接的字节数、HEAD 时
      // 为 0"。两个口径都要维持，所以这里做一次换算。
      si.body_bytes    = rp->head_only() ? 0 : rp->stream_bytes_written();
      si.connection_id = id;
      si.streamed      = true;
      si.ok            = (status == 0);
      rp->notify_sent(si);
      // **这一句之后不得再碰上面任何东西。** release() 可能当场把上下文
      // 销毁掉 —— `si`、`rp`、`cp` 指的全是它的成员。
      cp->release();
    });

    // **钉住上下文。** 链早就收尾了（handler 返回、`finish()` 跑过），
    // 正常情况下 context 此刻已经被回收 —— 而这条流还要用它来算
    // "整条流什么时候结束"。hold/release 是框架既有的机制
    // （`uvcpp_web_context::hold()`），这里第一次在生产代码里用：它为
    // "链结束了但请求还没结束"提供了一个显式的、有配对的表达，比在
    // 别处挂一个新的挂起标志要诚实得多。
    ctx.hold();

    // 把 handler 里同步写下的那批字节发出去（同时也发出头部）。此后新
    // 的块由 `write_chunk` → sink → 本类的 stream_write() 继续驱动。
    // **这一句之后不得再碰 ctx** —— 若这条流当场跑完（HEAD、或者 handler
    // 已经调过 end() 且没有待发字节），release() 会在这一句**内部**跑，
    // 上下文可能当场析构（见 `uvcpp_web_context::release()` 的注释：
    // "之后不要再碰任何成员"）。
    r.pump_stream();
    return;
  }

  // **流号已经不在这里设了** —— 它由 `on_http_request()` 在派发**之前**落到
  // 响应上（那时还没有 body，`raw()` 的 `sync_meta()` 会写死一个
  // `content-length: 0`，所以走的是 `set_stream_id()`）。放在这里就太晚了：
  // 流式响应的组帧方式在 `write_chunk()` 那一刻就定下了，而那是处理函数体内的
  // 事。
  //
  // 这个字段对 `uvcpp_http_server::send_response()` 同样关键：它只有
  // `(client, resp)` 两个参数，h2 连接上靠 `resp.stream_id` 找回是哪条流
  // （见 `send_h2_response`）；为 0 时连接被判成 h1，响应体走
  // `to_string()` 序列化成一条 HTTP/1.1 报文，而 h2 对端在等 HEADERS/DATA 帧
  // —— 一个字节都解析不出来，请求**静默挂死**（没有错误、没有日志，只有超时）。

  // 交给 HTTP 层序列化并异步写出。压缩、keep-alive 判定、写队列串行化都在
  // 那里面（对 deferred 响应同样成立）。
  // **`body_bytes` 取的是 `send_response` 的返回值，不再回头读 `resp.body`。**
  // 压缩跑在上面那个调用**内部**（`uvcpp_http_server::apply_compression`），它把
  // `resp.body` 换成了压缩后的字节，所以"上线多少"只有那一刻知道 —— 以前是靠
  // "`raw()` 返回引用、之后再读一次"拿到的。那条耦合现在断了：头/体分开走
  // （`nbufs = 2`）时体会被**移动**出去，函数返回后 `resp.body` 是空的，再读
  // 一次会静默得到 0（gzip 过的响应会记成 0 字节，而报文里明写着发的是压缩体
  // —— 比"记成压缩前的长度"更难看）。返回值就是为此取代那次读的。
  //
  // 在压缩之前采集当然也不行，理由同上：那时读到的是压缩前的长度。
  //
  // HEAD 那一支要再换算一次：压缩算过了（那正是 HEAD 的 `content-length` 与
  // GET 一致的原因），但字节**一个都没上线**，而契约是"实际写入连接的字节数、
  // HEAD 时为 0"。与流式那处的 `head_only() ? 0 : stream_bytes_written()` 同形状。
  const size_t sent_body_bytes = http_->send_response(client, r.raw());
  info.body_bytes = r.head_only() ? 0 : sent_body_bytes;

  r.notify_sent(info);
}

// =====================================================================
// 流式响应的三个出口（`uvcpp_web_stream_sink` 在框架侧的落地）
// =====================================================================
//
// 三个函数长得一样：**按 conn id 查活连接，查不到就拒绝**。拒绝的含义分工
// 得很清楚：
//
//   * 返回 0 —— 受理了，`done` 由 http 层的写完成回调负责调（**恰好一次**）。
//   * 返回非 0 —— **没受理，`done` 不会再被任何人调**。调用方
//     （`uvcpp_web_response::flush_stream`）会据此自己结算这块字节。
//
// 所以这三个函数**自己绝不调 `done`**。两边都调一次是真正的 bug，不是"多算
// 一遍"：第一次结算可能让整条流到达收尾条件，`maybe_finish_stream()` 随即
// 跑 `ctx.release()` 把上下文连同这个响应对象一起销毁，而 `flush_stream`
// 还没返回 —— 第二句就是对已释放对象的读。
//
// 这条分工必须写明，是因为它**没有写在 `uvcpp_http_server` 的头文件里**：
// 那里 `write_stream` 的注释说"连接没了 done 仍会被调用，参数 UV_ECANCELED"，
// 而实现（`uvcpp_http_server.cpp:570-574`）是查到 `contexts_.end()` 就直接
// `return UV_ECANCELED`，从头到尾没碰过 `done`。以实现为准。

void uvcpp_web_app::stream_begin(uvcpp_web_conn_id id, int32_t stream_id,
                                 uvcpp_http_response& head) {
  uvcpp_tcp_client* client = reg_client(id);
  if (client == nullptr) {
    // 连接已经没了。`stream_begin` 是唯一不带回调的出口，失败由
    // `pump_stream()` 记进 `stream_status_`，收尾时体现为 ok=false。
    UVCPP_LOG_DEBUG(log_category::RESPONSE)
        << "stream_begin：连接 " << id << " 已断开，头部丢弃";
    return;
  }
  http_->begin_stream(client, stream_id, head);
}

int uvcpp_web_app::stream_write(uvcpp_web_conn_id id, int32_t stream_id,
                                std::string bytes,
                                std::function<void(int)> done) {
  uvcpp_tcp_client* client = reg_client(id);
  if (client == nullptr) {
    UVCPP_LOG_DEBUG(log_category::RESPONSE)
        << "stream_write：连接 " << id << " 已断开，这一块丢弃";
    // 见上面那段分工：**不调 done**，返回非 0 让调用方去结算。
    // （`done` 在这里被析构掉 —— 它由 std::function 按值持有，随形参一起
    //  释放，不会有谁再去读它。）
    (void)done;
    return UV_ECANCELED;
  }
  return http_->write_stream(client, stream_id, std::move(bytes),
                             std::move(done));
}

void uvcpp_web_app::stream_end(uvcpp_web_conn_id id, int32_t stream_id,
                               bool close_after) {
  uvcpp_tcp_client* client = reg_client(id);
  if (client == nullptr) {
    // 连接已经没了，"结束"这件事已经由断开本身完成了。**这里必须是空操作**
    // —— 收尾路径上唯一要做的事是关连接，而它已经关着了。
    UVCPP_LOG_DEBUG(log_category::RESPONSE)
        << "stream_end：连接 " << id << " 已断开，无需收尾";
    return;
  }
  http_->end_stream(client, stream_id, close_after);
}

void uvcpp_web_app::stream_attach_file(uvcpp_web_conn_id id,
                                       uvcpp_web_file_transfer* t) {
  if (t == nullptr) return;
  loop_slot* s = slot_of(id);
  if (s == nullptr) return;
  s->file_transfers[id].push_back(t);
}

void uvcpp_web_app::stream_detach_file(uvcpp_web_conn_id id,
                                       uvcpp_web_file_transfer* t) {
  loop_slot* s = slot_of(id);
  if (s == nullptr) return;
  std::map<uvcpp_web_conn_id, std::vector<uvcpp_web_file_transfer*> >::iterator
      it = s->file_transfers.find(id);
  if (it == s->file_transfers.end()) return;
  std::vector<uvcpp_web_file_transfer*>& v = it->second;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == t) {
      v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
  }
  if (v.empty()) s->file_transfers.erase(it);
}

void uvcpp_web_app::abort_request(uvcpp_web_context& ctx) {
  uvcpp_tcp_client* client = reg_client(ctx.connection_id());
  if (client == nullptr) {
    UVCPP_LOG_DEBUG(log_category::REQUEST)
        << "abort：连接 " << ctx.connection_id() << " 已经断开，无需处理";
    return;
  }

  UVCPP_LOG_DEBUG(log_category::REQUEST)
      << "abort：关闭连接 " << ctx.connection_id();

  // 走 `client->close()`（完整关闭流程），不是 `get_tcp()->close()` ——
  // 后者绕过框架的关闭回调，连接登记表就永远摘不掉这一条。
  client->close();
  // 之后不要再碰 client：它的所有权在 tcp_server 手里，关闭完成回调里会被
  // delete。
}

void uvcpp_web_app::context_finished(uvcpp_web_context& ctx) {
  // **先把 id 取到本地。** 下面的 `erase` 很可能还掉 ctx 的最后一份
  // shared_ptr，对象当场析构；那之后再去问 `ctx.connection_id()` 就是
  // use-after-free —— PageHeap 下崩在 `context_finished+0xd8`（`mov rdx,
  // [rbp+18h]`，rbp 就是 &ctx），普通堆下只是静默读到已释放内存。
  // 本文件下面 `ctx.stream()` 那处早就是这么防的，这行漏了。
  const uvcpp_web_conn_id id = ctx.connection_id();

  // 队那一格按 **id** 定位（与入队的 `enqueue_inflight()` 同一把钥匙）。
  // id 无效 ⇒ 没登记过 ⇒ 与"不在表里"同一支。
  loop_slot* slot = slot_of(id);
  if (slot == nullptr) return;

  std::map<uvcpp_web_conn_id, out_queue >::iterator q =
      slot->inflight.find(id);
  if (q == slot->inflight.end()) return;

  // **按上下文身份在队里找**，不是按 id 取第一条。流水线之后一条连接上同时
  // 挂着好几条，按 id 拿到的很可能是**别人** —— 替别人摘号会让那一条永远出不了
  // 队（它的响应再也发不出去）。找不到就什么都不做：这个上下文本来就不在这条
  // 连接的名册上（例如 `wire_upload()` 内部直接回错那条路径）。
  out_queue::iterator me = q->second.begin();
  while (me != q->second.end() && me->ctx.get() != &ctx) ++me;
  if (me == q->second.end()) return;

  // 在 `erase` 之前记下：只有队首腾出来才谈得上续发。
  const bool was_front = (me == q->second.begin());

  // **收场时必须松开背压。** `read_pause()` 作用在**连接**上，而 keep-alive 的
  // 连接会带着这个状态去服务下一个请求 —— 不松开就是那条连接**从此不再读到任何
  // 字节**，症状是"第一个上传之后，同一条连接上的后续请求全部静默超时"，而且
  // 服务端看起来一切正常（没有报错、没有日志）。
  //
  // 上传会话自己会在做完时松开（见 `uvcpp_web_upload::complete_if_ready()`），
  // 但那是"应该"；这里是**不变式**：任何一条路径上的请求都不许把连接留在暂停
  // 态。放在 `erase` **之前** —— 那之后上下文可能当场被销毁。
  uvcpp_web_stream* s = ctx.stream();
  if (s != nullptr) s->resume();

  q->second.erase(me);
  // **空队列要连键一起摘掉。** `idle_sweep()` 的豁免与停机宽限的判据都是
  // `inflight.find(id) != end()` / `!inflight.empty()`，留一个空队列在表里
  // 会让那条连接被**永久**豁免闲置超时 —— 表只涨不落，长跑服务上就是稳定的泄漏。
  if (q->second.empty()) slot->inflight.erase(q);

  // 这个请求到此为止：半截请求的预算归零，下一次收到字节就是新请求的开头。
  // 放在摘号之后 —— 上面那些早返回都不该动计时。用本地 id，不要再用 ctx。
  reg_note_request_done(id);

  // 队里还有人 = 上面那句清掉的其实是**后面那条请求**的两个标量（它们是按
  // 连接存的，不是按请求）。按事实重报一次，否则流水线里第二条请求的
  // 「整段预算」和「停顿保护」会一起消失：后续字节经 `note_read()` 重新起算
  // `request_start_ms` 之后，一个正常推进的大上传会被当成慢速攻击杀掉。
  //
  // 只在真的还有在途请求时才走这一支 —— 所以单请求与顺序 keep-alive 的行为
  // 与改前**逐字节相同**，这段逻辑只在流水线下生效。
  if (slot->inflight.find(id) != slot->inflight.end()) {
    reg_note_read(id, loop_now_ms(loop()));
    reg_mark_streaming(id, active_body_ctx(id) != nullptr);
  }

  // 队首腾出来了：把后面**已经定稿**的响应按到达顺序接着发出去。
  if (was_front) flush_out(id);
}

// =========================================================================
// 连接
// =========================================================================

void uvcpp_web_app::on_accept(uvcpp_tcp_client* client) {
  std::string ip;
  int port = 0;
  if (client != nullptr) client->getPeerAddrs(ip, port);

  // 建立时刻就是这条连接的第一个活动基准：连上就再也不发字节的客户端，
  // 从这一刻起算闲置（否则它永远不超时，白占一个连接槽）。
  const uvcpp_web_conn_id id =
      reg_here().add(client, ip, port, loop_now_ms(loop()));

  UVCPP_LOG_DEBUG(log_category::CORE)
      << "新连接 " << id << " ← " << ip << ":" << port;

  for (size_t i = 0; i < connection_cbs_.size(); ++i) {
    try {
      connection_cbs_[i](id, client);
    } catch (const std::exception& e) {
      UVCPP_LOG_ERROR(log_category::CORE)
          << "连接钩子抛出异常（连接 " << id << "）：" << e.what();
    } catch (...) {
      UVCPP_LOG_ERROR(log_category::CORE)
          << "连接钩子抛出未知异常（连接 " << id << "）";
    }
  }
}

void uvcpp_web_app::on_close(uvcpp_tcp_client* client) {
  uvcpp_web_conn_id id = UVCPP_WEB_INVALID_CONN_ID;
  // 这条连接是在**本循环**的登记表上摘下来的（`reg_here()`），所以这一格
  // 就是它的格子 —— 下面四张按 id 索引的表全在这一格上，与 `reg_here()`
  // 同一把钥匙。用 `slot_of(id)` 也行（id 高段就是本循环号），这里取本线程
  // 那格是因为"摘表"这件事已经证明了这个 id 归本循环。
  loop_slot& slot = slot_here();
  if (reg_here().remove_by_client(client, &id)) {
    // 在途的分片下发：先**把名册摘下来再逐条取消**。cancel() 会同步走到
    // `on_done` → `stream_detach_file` → 擦这个 map 条目，边遍历边擦就是
    // 迭代器失效。摘下来之后名册归本栈所有，谁再动 map 都不影响。
    {
      std::map<uvcpp_web_conn_id,
               std::vector<uvcpp_web_file_transfer*> >::iterator ft =
          slot.file_transfers.find(id);
      if (ft != slot.file_transfers.end()) {
        std::vector<uvcpp_web_file_transfer*> pending;
        pending.swap(ft->second);
        slot.file_transfers.erase(ft);
        for (size_t i = 0; i < pending.size(); ++i) {
          if (pending[i] != nullptr) pending[i]->cancel();
        }
      }
    }

    UVCPP_LOG_DEBUG(log_category::CORE) << "连接 " << id << " 已断开";
    // 豁免标记跟着连接一起走。不擦的话这个集合只涨不落 —— 连接 id 永不复用，
    // 所以不会误豁免别人，但会一直占内存（长跑服务上就是一条稳定的泄漏）。
    slot.upgraded.erase(id);

    // 上传收到一半对端就断了：得告诉流对象（3b 的落盘要靠它删掉半截文件）。
    //
    // **先把要动的条目摘成一份名单，再逐条动**（与上面 `file_transfers` 同一
    // 个套路）。原因有两层：
    //
    //   1. `stream_abort()` 会一路走到 `stream_resume_chain()` → 链收尾 →
    //      `finish()` → `context_finished()` → **在队里摘掉这一条**，也就是在
    //      调用过程中把那个元素（连同它持有的那份引用）销毁掉。边遍历边动就是
    //      读已释放内存 —— 这也是本地必须持有 `shared_ptr` 副本的原因。
    //   2. 流水线之后一条连接上可能挂着好几条（见 `inflight`），所以是**名单**
    //      而不是改前的"那一条"。
    //
    // **这条连接的队列不能整队摘走。** 一个"响应在流式"的上下文靠表里这份
    // shared_ptr 活着（`web_app_stream_resp_func` 钉着"连接断开后连接计数 0、
    // 在途计数 1"），而它的收尾回调捕的是裸指针、只靠 `hold()` 计数保活 ——
    // 整队 erase 会让那个回调读悬垂对象。所以这里只做改前就有的那一件事。
    //
    // 其余条目（排队等发响应的、还在跑异步处理器的）**原样留在队里**，由它们
    // 自己依次走完"队首查连接 → 已断开 → 丢弃 + WARN → 出队 → 续发下一条"。
    {
      std::vector<std::shared_ptr<uvcpp_web_context> > victims;
      std::map<uvcpp_web_conn_id, out_queue >::iterator q =
          slot.inflight.find(id);
      if (q != slot.inflight.end()) {
        for (out_queue::iterator e = q->second.begin();
             e != q->second.end(); ++e) {
          // `streaming()` 是 `stream_ != nullptr`，它**不**区分"收请求体"和
          // "发响应流"；后者由 `stream_abort()` 自己挡掉（请求体早已交付完，
          // 那句 `delivered_end()` 的早返回正是这里靠的）。
          if (e->ctx->streaming()) victims.push_back(e->ctx);
        }
      }
      for (size_t i = 0; i < victims.size(); ++i) victims[i]->stream_abort();
    }
  }

  // 回调排在**摘表之后**：此刻 `connection(id)` 已经查不到了，这是刻意的
  // —— 断开之后就不该再有"往这条连接上写"的机会。
  for (size_t i = 0; i < connection_close_cbs_.size(); ++i) {
    try {
      connection_close_cbs_[i](id, client);
    } catch (const std::exception& e) {
      UVCPP_LOG_ERROR(log_category::CORE)
          << "断开钩子抛出异常（连接 " << id << "）：" << e.what();
    } catch (...) {
      UVCPP_LOG_ERROR(log_category::CORE)
          << "断开钩子抛出未知异常（连接 " << id << "）";
    }
  }
}

bool uvcpp_web_app::handle_raw_data(uvcpp_tcp_client* client, const char* data,
                                    size_t len) {
  const uvcpp_web_conn_id id = reg_here().id_of(client);

  // 记活跃**必须在早返回之前**。放在后面的话，"没注册任何钩子"的服务就永远
  // 不记活跃，闲置超时形同虚设 —— 而那样的服务恰恰是最多的。
  if (len > 0) reg_note_read(id, loop_now_ms(loop()));

  if (raw_data_cbs_.empty() && !raw_data_claim_) return true;

  // 观察者先跑：它们只是看，不影响数据去向。
  for (size_t i = 0; i < raw_data_cbs_.size(); ++i) {
    try {
      raw_data_cbs_[i](id, client, data, len);
    } catch (const std::exception& e) {
      UVCPP_LOG_ERROR(log_category::RAW)
          << "原始数据观察者抛出异常（连接 " << id << "）：" << e.what();
    } catch (...) {
      UVCPP_LOG_ERROR(log_category::RAW)
          << "原始数据观察者抛出未知异常（连接 " << id << "）";
    }
  }

  if (raw_data_claim_) {
    try {
      // **唯一一处极性映射**：对外是 `CONSUME` 才叫接管，对 HTTP 层是
      // 「false 才叫接管」（见 `http_raw_data_hook` 的说明）。整个框架里
      // 只在这一行做取反，别的地方都别再做第二次。
      return raw_data_claim_(id, client, data, len) ==
                     uvcpp_web_raw_action::PASS
                 ? true
                 : false;
    } catch (const std::exception& e) {
      // 接管者自己炸了。**放行给解析器**：一个坏掉的钩子不该把数据吞掉，
      // 那会让连接彻底卡死而且看不出原因。
      UVCPP_LOG_ERROR(log_category::RAW)
          << "原始数据接管者抛出异常（连接 " << id << "），本块数据按放行处理："
          << e.what();
      return true;
    } catch (...) {
      UVCPP_LOG_ERROR(log_category::RAW)
          << "原始数据接管者抛出未知异常（连接 " << id << "），本块数据按放行处理";
      return true;
    }
  }

  return true;
}

// =========================================================================
// 闲置超时（slowloris 防御）
// =========================================================================

void uvcpp_web_app::idle_sweep() {
  if (cfg_.idle_timeout_ms <= 0) return;

  // 停机流程自己会关连接，而且它关心的是"在途请求收完没有"，不是"客户端
  // 有多久没说话"。让两个关闭者同时动手只会把日志搅乱。
  if (stopping_.load()) return;

  const int64_t now = loop_now_ms(loop());

  // 取一份 id 快照再遍历：下面的 `close()` 是异步的（真正摘表在关闭完成
  // 回调里），但**不依赖这一点** —— 快照让"遍历中表被改"这件事根本不可能。
  const std::vector<uvcpp_web_conn_id> ids = reg_here().ids();

  // 扫的是**本循环**的登记表（上面那句），所以下面两张按 id 索引的表也取
  // **本循环那一格** —— 与本线程同一把钥匙，而不是 `slot_of(id)`。这也是
  // 每格各挂一个 `idle_timer` 的原因：一拍只扫自己的表。
  loop_slot& slot = slot_here();

  for (size_t i = 0; i < ids.size(); ++i) {
    const uvcpp_web_conn_id id = ids[i];

    // 在途请求不受影响：客户端在等我们，不是在攻击我们。关掉它只会让一个
    // 正确发起的请求失败 —— 那比慢速攻击更难查。
    //
    // **例外是流式收体。** 一个正在上传的请求也在 `inflight` 里，但它不是在
    // "等我们"，而是在"往我们这儿送"——整体豁免等于让一个卡住的上传永远挂着
    // （连接、缓冲、context 全部不回收）。流式连接改按"停顿多久没进展"判定：
    // 持续有字节就活得下去，停下来才关。判据从登记表读，不另设集合，
    // 免得两处状态对不上。
    if (slot.inflight.find(id) != slot.inflight.end() && !reg_is_streaming(id))
      continue;

    // **已升级成 WS 的连接也豁免。** 60 秒没有消息对 WS 是常态（聊天室、
    // 服务端推送），用 HTTP 的判据去关它是误杀。
    //
    // 代价照实说：WS 连接从此没有任何超时保护 —— 框架不会关掉一条安静但
    // 已经死掉的 WS 连接。要保活得应用层自己发 ping/pong。
    if (slot.upgraded.find(id) != slot.upgraded.end()) continue;

    const int64_t since = reg_activity_since(id);
    if (since == 0) {
      // 登记时没给时间戳。补一个基准，让它从这一刻开始算 —— 而不是拿 0
      // 当"上古时刻"，把每一条刚建立的连接在第一次扫描时就杀掉。
      reg_touch(id, now);
      continue;
    }

    const int64_t idle_ms = now - since;
    if (idle_ms <= cfg_.idle_timeout_ms) continue;

    uvcpp_tcp_client* client = reg_client(id);
    if (client == nullptr) continue;

    UVCPP_LOG_WARN(log_category::CORE)
        << "连接 " << id << " 已经 " << idle_ms << " ms 没有进展（上限 "
        << cfg_.idle_timeout_ms << " ms），关闭";

    // 走 `client->close()`：完整的关闭流程，断开钩子和登记表摘除都会照常
    // 发生。直接 `get_tcp()->close()` 会绕过框架的关闭回调，这条记录就永远
    // 留在表里 —— 而本函数的全部意义就是别让记录永远留在表里。
    client->close();
  }
}

// =========================================================================
// 请求派发
// =========================================================================

void uvcpp_web_app::on_http_request(uvcpp_http_request& req,
                                    uvcpp_http_response& resp,
                                    uvcpp_tcp_client* client) {
  // **必须是第一件事。** HTTP 层在兜底 handler 返回后会检查它：置了
  // deferred 就不再自己发送，也不会再碰 req/resp。框架的所有响应都是
  // deferred（异步中间件要求"先挂起、后发送"），所以这一行等于声明了
  // "这个响应由我负责"。
  resp.deferred = true;

  uvcpp_web_conn_id id = reg_here().id_of(client);
  if (id == UVCPP_WEB_INVALID_CONN_ID) {
    // 正常路径到不了这里：连接在 accept 时就登记了。真到了说明有人绕过
    // App 的 accept 钩子直接用了 HTTP 层 —— 现场补一条，比回 500 有用。
    std::string ip;
    int port = 0;
    if (client != nullptr) client->getPeerAddrs(ip, port);
    id = reg_here().add(client, ip, port, loop_now_ms(loop()));
    UVCPP_LOG_WARN(log_category::CORE)
        << "请求来自未登记的连接，已补登记为 " << id;
  }

  std::shared_ptr<uvcpp_web_context> ctx = uvcpp_web_context::create(*this, id);

  // 请求体是**搬**过来的（move_buf），不是拷贝 —— 上传大文件时这一步省掉
  // 一倍内存。此后 `req` 的 body 就空了，HTTP 层那边也不会再用它。
  ctx->request().take_from(req);

  // **流号必须在处理函数跑之前落到响应上，这是唯一不晚的时刻。**
  //
  // 两件事都要它：① 流式响应的**组帧方式**由协议决定（h1 的 chunked 帧 vs
  // h2 的裸字节），而 `write_chunk()` 出现在处理函数体内 —— 那时框架还没装
  // sink，问不出协议；② `send_response()` 靠 `resp.stream_id` 找回这条流。
  //
  // 走 `set_stream_id()` 而不是 `response().raw().stream_id = ...`：`raw()`
  // 的非 const 版会先 `sync_meta()`，此刻 body 还空着，那会写死一个
  // `content-length: 0`，之后处理函数设的 body 长度就再也改不动了。
  ctx->response().set_stream_id(req.stream_id);

  const uvcpp_web_connection* conn = reg_find(id);
  if (conn != nullptr) {
    ctx->request().set_peer(conn->peer_ip,
                            static_cast<unsigned int>(conn->peer_port));
  }

  sync_chains();

  web_route_match m =
      router_.match(ctx->request().method(), ctx->request().path());
  dispatch(ctx, m);
}

void uvcpp_web_app::dispatch(
    const std::shared_ptr<uvcpp_web_context>& ctx, const web_route_match& m) {
  const std::vector<uvcpp_web_handler>* chain = nullptr;

  switch (m.result) {
    case web_route_result::MATCHED: {
      if (m.handler == nullptr) break;  // 不该发生，退到 404

      // 路径参数交给请求对象，handler 里用 `req.param("id")` 取。
      for (size_t i = 0; i < m.params.size(); ++i) {
        ctx->request().set_param(m.params[i].first, m.params[i].second);
      }
      // HEAD 回退到 GET：body 照算（内容长度要和 GET 一致）但不发出去。
      if (m.head_of_get) ctx->response().set_head_only(true);

      chain = build_chain(m.handler);
      break;
    }
    case web_route_result::AUTO_OPTIONS:
      chain = auto_options_chain_;
      break;
    case web_route_result::METHOD_NOT_ALLOWED:
      chain = method_not_allowed_chain_;
      break;
    case web_route_result::NOT_FOUND:
    default:
      chain = not_found_chain_;
      break;
  }

  if (chain == nullptr) {
    // 正常路径到不了：`sync_chains()` 在派发前一定把三条特殊链建好了，
    // MATCHED 的 handler 也是路由器给的。留着是为了兜住"路由表在派发途中
    // 被改空"这种极端情况 —— 回 500，而不是让请求永远悬着。
    UVCPP_LOG_ERROR(log_category::ROUTER)
        << "路由结果 " << web_route_result_name(m.result)
        << " 没有可用的处理器链，回 500";
    ctx->response().server_error();
    ctx->response().end();
    chain = &empty_chain_;  // 成员变量的地址稳定，符合 run() 的指针约定
  }

  // **挂号必须在 run() 之前。** 纯同步的链会在 run() 里一路跑完并 finish()，
  // 那时 `context_finished()` 就来摘号了 —— 先 run 后挂号等于把一个没人
  // 认领的上下文塞进表里（泄漏），或者把后来的摘号吃掉。
  const uvcpp_web_conn_id id = ctx->connection_id();
  if (enqueue_inflight(id, ctx)) {
    // 到上限：回 503 + 关连接，不跑用户的链。响应仍然排在它该在的位置上
    // （能走到这里说明队里已经有人，它一定不是队首）。
    reject_pipelining(*ctx);
    return;
  }

  ctx->run(*chain);
}

// =========================================================================
// 链缓存
// =========================================================================

void uvcpp_web_app::sync_chains() {
  // 两个失效来源：
  //   1. 中间件变了（`use()` / 访问日志插队）—— 链的组成变了；
  //   2. 路由总数变了 —— 缓存是按 `const uvcpp_web_handler*` 索引的，而那个
  //      指针指向路由表内部；注册新路由可能让表重新分配，旧指针要么悬垂，
  //      要么**被新路由的处理器复用**，于是缓存命中到别人的链（跑错业务
  //      代码，而且不报错）。总数校验把这种情况挡在门外。
  //
  // 流式路由表**也**要算进来：它的 handler 同样被 `build_chain()` 缓存，
  // 同样住在会被重新分配的表里。只看 `router_` 的话，注册一条流式路由不会
  // 作废缓存 —— 而 `stream_router_.add()` 完全可能让它的表重新分配，缓存里
  // 那个 `const uvcpp_web_handler*` 就指向了已释放的内存。
  const size_t routes = router_.route_count();
  const size_t streams = stream_router_.route_count();
  if (routes != routes_seen_ || streams != stream_routes_seen_ ||
      middleware_gen_ != cache_gen_) {
    chain_cache_.clear();
    routes_seen_        = routes;
    stream_routes_seen_ = streams;
    cache_gen_          = middleware_gen_;
    specials_built_     = false;
  }

  if (!specials_built_) build_special_chains();
}

const std::vector<uvcpp_web_handler>* uvcpp_web_app::build_chain(
    const uvcpp_web_handler* route_handler) {
  if (route_handler == nullptr) return nullptr;

  std::map<const uvcpp_web_handler*,
           const std::vector<uvcpp_web_handler>*>::iterator it =
      chain_cache_.find(route_handler);
  if (it != chain_cache_.end()) return it->second;

  // 组装：全局中间件按注册序 + 路由处理器。拷的是 std::function（每个都
  // 带着自己捕获的状态），所以链一旦建好就不再依赖路由表 —— 这是"每个
  // 请求不重复拷贝中间件"的关键。
  std::vector<uvcpp_web_handler> chain;
  chain.reserve(middlewares_.size() + 1);
  for (size_t i = 0; i < middlewares_.size(); ++i) {
    chain.push_back(middlewares_[i]);
  }
  chain.push_back(*route_handler);

  // 存进 deque 再取地址：deque 的 push_back 不会让已有元素搬家，所以上下文
  // 手里那些链指针在整个进程生命周期内都有效（旧链永远不删也是这个原因
  // —— 可能还有请求正在用它）。
  chain_storage_.push_back(chain);
  const std::vector<uvcpp_web_handler>* stored = &chain_storage_.back();
  chain_cache_[route_handler] = stored;
  return stored;
}

void uvcpp_web_app::build_special_chains() {
  // 三条特殊链：404 / 405 / 自动 OPTIONS。它们的终端处理器要**重新查一次
  // 路由表**（405 和 OPTIONS 需要 Allow 头），所以不能只靠派发时的匹配结果。

  // --- 404 --------------------------------------------------------
  {
    std::vector<uvcpp_web_handler> c(middlewares_);
    c.push_back([this](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
      (void)next;
      UVCPP_LOG_INFO(log_category::ROUTER)
          << "404 " << http_method_str(req.method()) << " " << req.path();
      resp.not_found();
      resp.end();
    });
    chain_storage_.push_back(c);
    not_found_chain_ = &chain_storage_.back();
  }

  // --- 405 --------------------------------------------------------
  {
    std::vector<uvcpp_web_handler> c(middlewares_);
    c.push_back([this](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
      (void)next;
      // 用**原始方法**重算：路由器会算出这个路径实际支持哪些方法，
      // Allow 头就是那个列表（含 HEAD/OPTIONS 的自动补齐）。
      web_route_match m = router_.match(req.method(), req.path());
      UVCPP_LOG_INFO(log_category::ROUTER)
          << "405 " << http_method_str(req.method()) << " " << req.path()
          << "（Allow: " << m.allow << "）";
      resp.method_not_allowed(m.allow);
      resp.end();
    });
    chain_storage_.push_back(c);
    method_not_allowed_chain_ = &chain_storage_.back();
  }

  // --- 自动 OPTIONS -----------------------------------------------
  {
    std::vector<uvcpp_web_handler> c(middlewares_);
    c.push_back([this](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
      (void)next;
      // 此刻 req.method() 就是 OPTIONS，所以这次匹配走的正是 AUTO_OPTIONS
      // 分支，`allow` 已经填好了。
      web_route_match m = router_.match(req.method(), req.path());
      UVCPP_LOG_DEBUG(log_category::ROUTER)
          << "自动应答 OPTIONS " << req.path() << "（Allow: " << m.allow << "）";
      resp.status(204);
      if (!m.allow.empty()) resp.set_header("allow", m.allow);
      resp.end();
    });
    chain_storage_.push_back(c);
    auto_options_chain_ = &chain_storage_.back();
  }

  specials_built_ = true;
}

// =========================================================================
// 跨线程投递
// =========================================================================

void uvcpp_web_app::drain_posts() {
  // 一次换出一批再执行：执行期间可能有新任务进来（fn 自己又 post 了一个），
  // 那些留给下一轮由 uv_async_send 再唤醒 —— 不然一个自我投递的任务就能让
  // 这个循环永远转下去，把事件循环饿死。
  std::deque<std::function<void()> > batch;
  {
    // 这个回调是**这条循环的 async** 唤起来的，所以就地取本线程那格 ——
    // 与投递方（`post()`）取的是同一格。
    loop_slot& s = slot_here();
    std::lock_guard<std::mutex> lk(s.post_mutex);
    batch.swap(s.post_queue);
  }

  for (size_t i = 0; i < batch.size(); ++i) {
    try {
      batch[i]();
    } catch (const std::exception& e) {
      UVCPP_LOG_ERROR(log_category::CORE)
          << "投递的任务抛出异常：" << e.what();
    } catch (...) {
      UVCPP_LOG_ERROR(log_category::CORE) << "投递的任务抛出未知异常";
    }
  }
}

// =========================================================================
// 停机
// =========================================================================

void uvcpp_web_app::begin_shutdown() {
  loop_slot& slot = slot_here();
  if (!loop_started_.load()) return;

  UVCPP_LOG_INFO(log_category::CORE)
      << "开始停机：先停监听，宽限 " << cfg_.shutdown_grace_ms
      << " ms 等在途请求（当前 " << inflight_total() << " 个，活连接 "
      << connection_count() << " 条）";

  // 1. 不再接受新连接。注意它**不会**关掉已经建立的连接 —— 那正是宽限期
  //    存在的意义。
  uvcpp_tcp_server* tcp = tcp_server();
  if (tcp != nullptr) tcp->stop();

  slot.shutdown_deadline_ms =
      loop_now_ms(loop()) + static_cast<int64_t>(cfg_.shutdown_grace_ms);

  // 2. 后面的动作全部交给看门狗定时器。
  //
  //    为什么不就地做完：`stop()` 走的是 `post()`，也就是说这段代码很可能
  //    正在 async 句柄的回调里跑。在这一帧里删 async 句柄就是把它自己正在
  //    执行的 `std::function` 连着析构掉。跳到一个定时器回调上，这个隐患
  //    就不存在了。
  if (slot.shutdown_timer == nullptr) {
    finish_shutdown();
    return;
  }
  slot.shutdown_timer->start([this](uvcpp_timer*) { shutdown_step(); }, 0,
                             kShutdownPollMs);
}

void uvcpp_web_app::shutdown_step() {
  loop_slot& slot = slot_here();
  // ---- 第 0 拍：等在途请求跑完，或者宽限期到 ----
  if (slot.shutdown_phase == 0) {
    // 等的判据是**本格**的在途队列：这个看门狗是本循环的，等待与放行都只
    // 覆盖本循环的连接。日志里那个数则是**全进程**聚合（`inflight_total()`）
    // —— 运维要看的是"还剩多少活儿"，不是"本循环还剩多少"。
    if (!slot.inflight.empty() &&
        loop_now_ms(loop()) < slot.shutdown_deadline_ms) {
      return;  // 继续等，看门狗下一拍再看
    }

    slot.shutdown_phase = 1;
    if (slot.shutdown_timer != nullptr) slot.shutdown_timer->stop();

    if (!slot.inflight.empty()) {
      UVCPP_LOG_WARN(log_category::CORE)
          << "宽限期到，仍有 " << inflight_total()
          << " 个请求在途（多半卡在异步处理器里），强制关闭连接";
    }

    // ---- 1. **先让 WS 会话道别，再关任何连接。** ----
    //
    // 顺序在这里是**行为性的**，不是风格问题。`close_all_sessions()` 只是
    // 把 Close 帧排进各会话的发送队列，帧真正出网要几轮循环；而下面那句
    // `close_all_clients()` 会**立刻**把底层连接全部关掉 —— 先关连接的话，
    // 那些帧一个字节都发不出去，对端只看到一条被断开的连接（1006），
    // RFC 6455 §7.1.4 的优雅关闭就成了一句空话。
    //
    // 这一步必须早于 `close_all_clients()`，而且中间要留出一拍排水时间
    // （见下面 phase 1 → phase 2 的过渡）。
    //
    // 1001 GOING_AWAY：对端据此能分辨"服务器在停机"和"对方正常告别"，
    // RFC 6455 §7.4.1 对它的定义正是前者。
    if (ws_server_ != nullptr) {
      UVCPP_LOG_DEBUG(log_category::WEBSOCKET)
          << "停机：给 " << ws_server_->session_count() << " 个 WS 会话发 Close";
      ws_server_->close_all_sessions(ws_close_code::GOING_AWAY);
    }

    // ---- h2 同理，只是道别的形状不同：一条 GOAWAY。 ----
    //
    // 它在 `close_all_clients()` **之前**发，理由与上面 WS 那段逐字相同；
    // 不同之处在于 GOAWAY 还多带一个信息：`last_stream_id` 是本端已处理的最大
    // 流号，对端据此能分辨"我发过但你没处理"的那几条 —— 那些可以安全重试，
    // 而其余的不能（RFC 7540 §6.8）。没有它，一次停机在客户端看起来和拔网线
    // 完全一样，在飞的请求只能一律按"结果未知"处理。
    //
    // 这一句同样**不关**连接（见 `begin_h2_goaway()` 的说明），关是下面
    // phase 1 的事。
    if (http_ != nullptr) {
      const size_t n = http_->begin_h2_goaway();
      if (n != 0) {
        UVCPP_LOG_DEBUG(log_category::CORE)
            << "停机：给 " << n << " 条 h2 连接发 GOAWAY";
      }
    }

    // 排水一拍：Close 帧出网 → 会话收到写完成 → 自己关掉底层连接 → 终结
    // 回调把它们从会话表和连接登记表里摘掉。这一步做完，下面那句
    // `close_all_clients()` 要处理的就只剩**普通 HTTP** 连接了。
    schedule_shutdown_step(kShutdownDrainMs);
    return;
  }

  // ---- 第 1 拍：关掉剩下的连接（这时 WS 的已经自己走完了） ----
  if (slot.shutdown_phase == 1) {
    slot.shutdown_phase = 2;

    // 在途请求的上下文**不动**：它们可能还在等工作线程的结果。连接关了
    // 之后它们再发响应会查到"连接已断开"，走丢弃路径并记一条警告。
    uvcpp_tcp_server* tcp = tcp_server();
    if (tcp != nullptr) {
      const size_t n = tcp->close_all_clients();
      UVCPP_LOG_DEBUG(log_category::CORE) << "已发起关闭 " << n << " 条连接";
    }

    // 再等一拍：`uv_close` 的完成回调排在本次迭代末尾，立刻停循环的话它们
    // 可能来不及跑（连接对象、登记表就此留成半截状态）。
    schedule_shutdown_step(kShutdownDrainMs);
    return;
  }

  // ---- 第 2 拍：连接该关的都关完了，收尾 ----
  finish_shutdown();
}

/**
 * @brief 再等 \p delay_ms 之后跑下一拍停机步骤。
 *
 * 没有看门狗定时器时（理论上只会在 App 没起来时发生）直接同步续跑，
 * 免得停机停在一半。
 */
void uvcpp_web_app::schedule_shutdown_step(int delay_ms) {
  loop_slot& slot = slot_here();
  if (slot.shutdown_timer == nullptr) {
    shutdown_step();
    return;
  }
  slot.shutdown_timer->start([this](uvcpp_timer*) { shutdown_step(); },
                             static_cast<uint64_t>(delay_ms), 0);
}

void uvcpp_web_app::finish_shutdown() {
  loop_slot& slot = slot_here();
  const uvcpp_tcp_server* tcp = tcp_server();
  if (tcp != nullptr && tcp->client_count() != 0) {
    UVCPP_LOG_WARN(log_category::CORE)
        << "停机收尾时仍有 " << tcp->client_count()
        << " 条连接留在登记表里（预期为 0）";
  }

  if (slot.idle_timer != nullptr) {
    slot.idle_timer->stop();
    delete slot.idle_timer;
    slot.idle_timer = nullptr;
  }

  if (slot.shutdown_timer != nullptr) {
    slot.shutdown_timer->stop();
    delete slot.shutdown_timer;
    slot.shutdown_timer = nullptr;
  }

  // 投递句柄：`delete` 会走 free_handle → uv_close(哨兵)，完成回调在本次
  // 迭代末尾把底层内存还回去。**必须在定时器回调里做，不能在 async 自己的
  // 回调里做**（那会析构正在执行的 std::function）。
  {
    loop_slot& s = slot_here();
    std::lock_guard<std::mutex> lk(s.post_mutex);
    delete s.async;
    s.async = nullptr;
    // 顺带把还没跑的任务丢掉：循环要停了，它们永远不会被执行，留着只会
    // 让 post_queue 的析构去销毁一堆捕获了上下文的闭包。
    s.post_queue.clear();
  }

  // WS 的 Close 帧**不在这里发** —— 见 `shutdown_step()` 的第 0 拍。
  //
  // 放在这里的版本曾经"看起来对"（确实排在了 `stop_loop()` 之前），但实际
  // 一帧都发不出去：那时 `close_all_clients()` 早已把底层连接全关了，帧没有
  // 可写的 socket。用例 `app_shutdown_closes_sessions` 就是专门钉这一点的
  // （对端必须收到 Close 帧，而不是被断开）。
  //
  // 走到这里时 WS 会话已经道别并自行关闭，所以只需兜底回收：循环马上要停，
  // 终结回调可能来不及跑。

  loop_started_ = false;

  UVCPP_LOG_INFO(log_category::CORE) << "停机完成，事件循环即将退出";

  // 停循环。`uv_stop` 在**下一次**迭代开头生效，所以本次迭代的收尾（包括
  // 上面那些 uv_close 的完成回调）会正常跑完。
  if (http_ != nullptr) http_->get_tcp_server()->stop_loop();
}

}  // namespace uvcpp
