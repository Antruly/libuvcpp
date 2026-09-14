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
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_util.h>
#include <webapp/uvcpp_web_ws.h>

#include <cstdio>
#include <exception>
#include <future>
#include <utility>

namespace uvcpp {

namespace {

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

}  // namespace

// =========================================================================
// 配置
// =========================================================================

uvcpp_web_app_config::uvcpp_web_app_config()
    : host("0.0.0.0"),
      port(8080),
      backlog(128),
      max_body_size(16 * 1024 * 1024),
      compression(true),
      compress_min_body_size(1024),
      access_log(true),
      min_log_level(log_level::INFO),
      server_header("uvcpp"),
      shutdown_grace_ms(3000),
      idle_timeout_ms(60000),
      auto_options(true),
      head_as_get(true) {}

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_web_app::uvcpp_web_app()
    : http_(new uvcpp_http_server()),
      ws_server_(nullptr),
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
      shutdown_phase_(0),
      tid_known_(false),
      async_(nullptr),
      idle_timer_(nullptr),
      shutdown_timer_(nullptr),
      shutdown_deadline_ms_(0),
      bound_port_(0) {}

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
  delete async_;
  async_ = nullptr;
  delete idle_timer_;
  idle_timer_ = nullptr;
  delete shutdown_timer_;
  shutdown_timer_ = nullptr;

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

uvcpp_web_app& uvcpp_web_app::set_auto_options(bool enable) {
  cfg_.auto_options = enable;
  return *this;
}

uvcpp_web_app& uvcpp_web_app::set_head_as_get(bool enable) {
  cfg_.head_as_get = enable;
  return *this;
}

#if UVCPP_OPENSSL_ENABLE

namespace {
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

std::shared_ptr<uvcpp_web_static> uvcpp_web_app::serve_static(
    const std::string& prefix, const std::string& root_dir,
    const uvcpp_web_static_options& opts) {
  std::shared_ptr<uvcpp_web_static> st(new uvcpp_web_static(root_dir, opts));

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

  uvcpp_web_app* self = this;
  http_->on_upgrade([self](uvcpp_http_request& req, uvcpp_tcp_client* client) {
    self->on_ws_upgrade(req, client);
  });
  return *this;
}

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
  const std::string path = web_url_decode(raw_path, /*plus_as_space=*/false);

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
  std::shared_ptr<uvcpp_web_ws_request> ws(new uvcpp_web_ws_request(req));
  ws->set_route(*m.pattern);
  for (size_t i = 0; i < m.params.size(); ++i) {
    ws->request().set_param(m.params[i].first, m.params[i].second);
  }

  // 对端地址与 HTTP 请求走**同一个来源**（连接登记表）。
  uvcpp_web_conn_id id = registry_.id_of(client);
  const uvcpp_web_connection* conn = registry_.find(id);
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
        // `upgraded_` 里 —— 而那条连接随后就断了，登记表里也没了它，
        // 这个集合就成了一个只涨不落的泄漏。而这个回调**只在真建了会话时**
        // 才被调用，所以它是唯一正确的登记点。
        uvcpp_web_conn_id cid = self->registry_.id_of(client);
        if (cid != UVCPP_WEB_INVALID_CONN_ID) self->upgraded_.insert(cid);

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
    const int rc = init_on_loop_thread();
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
      std::lock_guard<std::mutex> lk(tid_mutex_);
      tid_known_ = false;
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
  const int rc = init_on_loop_thread();
  if (rc != 0) {
    std::lock_guard<std::mutex> lk(tid_mutex_);
    tid_known_ = false;
    return rc;
  }

  running_ = true;
  const int r = http_->run(md);
  running_ = false;
  loop_started_ = false;
  return r;
}

int uvcpp_web_app::init_on_loop_thread() {
  // 循环线程身份必须最先记下来：下面任何一步失败都不会有回调进来，而这个
  // 标记决定了 `post()` 是就地执行还是投递。
  {
    std::lock_guard<std::mutex> lk(tid_mutex_);
    loop_tid_ = std::this_thread::get_id();
    tid_known_ = true;
  }

  uvcpp_tcp_server* tcp = http_->get_tcp_server();
  uvcpp_loop* loop = tcp->get_loop();
  if (loop == nullptr || tcp->get_tcp() == nullptr) return UV_EINVAL;

  // --- 配置 --------------------------------------------------------
  uvcpp_logger::instance().set_level(cfg_.min_log_level);

  http_->set_max_body_size(cfg_.max_body_size);
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

  // --- loop 亲和的句柄 ---------------------------------------------
  //
  // 放在最后：这两个句柄必须在 loop 线程上建，而前面任何一步失败都不该
  // 留下需要回收的句柄。
  async_ = new uvcpp_async();
  rc = async_->init([this](uvcpp_async*) { drain_posts(); }, loop);
  if (rc != 0) {
    delete async_;
    async_ = nullptr;
    return rc;
  }

  shutdown_timer_ = new uvcpp_timer(loop);

  // 闲置超时扫描器。`idle_timeout_ms == 0` 时**不建句柄** —— 关掉的功能
  // 不该在运行时留下任何开销，哪怕只是一拍一次的空转。
  if (cfg_.idle_timeout_ms > 0) {
    idle_timer_ = new uvcpp_timer(loop);
    const uint64_t iv =
        idle_sweep_interval_ms(cfg_.idle_timeout_ms);
    idle_timer_->start([this](uvcpp_timer*) { idle_sweep(); }, iv, iv);
  }

  started_once_ = true;
  loop_started_ = true;
  stopping_ = false;
  shutdown_phase_ = 0;
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
  std::lock_guard<std::mutex> lk(tid_mutex_);
  return tid_known_ && loop_tid_ == std::this_thread::get_id();
}

void uvcpp_web_app::post(std::function<void()> fn) {
  if (!fn) return;

  std::lock_guard<std::mutex> lk(post_mutex_);
  if (async_ == nullptr) {
    // 循环没起来（或者已经收尾了）。**说出来** —— 静默丢弃投递任务会让
    // "异步处理器永远不回来"变成一桩悬案。
    UVCPP_LOG_WARN(log_category::CORE)
        << "事件循环不可用（未启动或已停止），投递的任务被丢弃";
    return;
  }
  post_queue_.push_back(fn);
  // uv_async_send 是异步信号安全的，且不会阻塞 —— 握着锁调没问题，loop
  // 线程那边的 drain_posts 也要抢这把锁，但它不会回头等我们。
  async_->send();
}

uvcpp_loop* uvcpp_web_app::loop() const {
  return http_ != nullptr ? http_->get_tcp_server()->get_loop() : nullptr;
}

uvcpp_tcp_client* uvcpp_web_app::connection(uvcpp_web_conn_id id) {
  return registry_.client(id);
}

void uvcpp_web_app::send_response(uvcpp_web_context& ctx) {
  uvcpp_web_response& r = ctx.response();

  uvcpp_web_sent_info info;
  info.status_code   = r.status_code();
  info.body_bytes    = r.body_size();
  info.connection_id = ctx.connection_id();
  info.ok            = true;

  uvcpp_tcp_client* client = registry_.client(ctx.connection_id());
  if (client == nullptr) {
    // 连接在响应准备好之前断了（客户端提前走了，或者我们 abort 过）。
    // **照样通知** —— 访问日志中间件挂在这一刻上，丢掉通知就等于这次请求
    // 在日志里凭空消失。ok=false 就是给这种场合用的。
    info.ok = false;
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

  // 交给 HTTP 层序列化并异步写出。压缩、keep-alive 判定、写队列串行化都在
  // 那里面（对 deferred 响应同样成立）。
  http_->send_response(client, r.raw());

  r.notify_sent(info);
}

void uvcpp_web_app::abort_request(uvcpp_web_context& ctx) {
  uvcpp_tcp_client* client = registry_.client(ctx.connection_id());
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
  std::map<uvcpp_web_conn_id, std::shared_ptr<uvcpp_web_context> >::iterator it =
      inflight_.find(ctx.connection_id());
  if (it == inflight_.end()) return;

  // 比指针再摘：同一个连接上换过上下文（HTTP 流水线）时，表里那条已经是
  // 别人了，不能替别人摘号。
  if (it->second.get() != &ctx) return;

  inflight_.erase(it);

  // 这个请求到此为止：半截请求的预算归零，下一次收到字节就是新请求的开头。
  // 放在摘号之后 —— 上面那两个早返回都不该动计时。
  registry_.note_request_done(ctx.connection_id());
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
      registry_.add(client, ip, port, loop_now_ms(loop()));

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
  if (registry_.remove_by_client(client, &id)) {
    UVCPP_LOG_DEBUG(log_category::CORE) << "连接 " << id << " 已断开";
    // 豁免标记跟着连接一起走。不擦的话这个集合只涨不落 —— 连接 id 永不复用，
    // 所以不会误豁免别人，但会一直占内存（长跑服务上就是一条稳定的泄漏）。
    upgraded_.erase(id);
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
  const uvcpp_web_conn_id id = registry_.id_of(client);

  // 记活跃**必须在早返回之前**。放在后面的话，"没注册任何钩子"的服务就永远
  // 不记活跃，闲置超时形同虚设 —— 而那样的服务恰恰是最多的。
  if (len > 0) registry_.note_read(id, loop_now_ms(loop()));

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
  const std::vector<uvcpp_web_conn_id> ids = registry_.ids();

  for (size_t i = 0; i < ids.size(); ++i) {
    const uvcpp_web_conn_id id = ids[i];

    // 在途请求不受影响：客户端在等我们，不是在攻击我们。关掉它只会让一个
    // 正确发起的请求失败 —— 那比慢速攻击更难查。
    if (inflight_.find(id) != inflight_.end()) continue;

    // **已升级成 WS 的连接也豁免。** 60 秒没有消息对 WS 是常态（聊天室、
    // 服务端推送），用 HTTP 的判据去关它是误杀。
    //
    // 代价照实说：WS 连接从此没有任何超时保护 —— 框架不会关掉一条安静但
    // 已经死掉的 WS 连接。要保活得应用层自己发 ping/pong。
    if (upgraded_.find(id) != upgraded_.end()) continue;

    const int64_t since = registry_.activity_since(id);
    if (since == 0) {
      // 登记时没给时间戳。补一个基准，让它从这一刻开始算 —— 而不是拿 0
      // 当"上古时刻"，把每一条刚建立的连接在第一次扫描时就杀掉。
      registry_.touch(id, now);
      continue;
    }

    const int64_t idle_ms = now - since;
    if (idle_ms <= cfg_.idle_timeout_ms) continue;

    uvcpp_tcp_client* client = registry_.client(id);
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

  uvcpp_web_conn_id id = registry_.id_of(client);
  if (id == UVCPP_WEB_INVALID_CONN_ID) {
    // 正常路径到不了这里：连接在 accept 时就登记了。真到了说明有人绕过
    // App 的 accept 钩子直接用了 HTTP 层 —— 现场补一条，比回 500 有用。
    std::string ip;
    int port = 0;
    if (client != nullptr) client->getPeerAddrs(ip, port);
    id = registry_.add(client, ip, port, loop_now_ms(loop()));
    UVCPP_LOG_WARN(log_category::CORE)
        << "请求来自未登记的连接，已补登记为 " << id;
  }

  std::shared_ptr<uvcpp_web_context> ctx = uvcpp_web_context::create(*this, id);

  // 请求体是**搬**过来的（move_buf），不是拷贝 —— 上传大文件时这一步省掉
  // 一倍内存。此后 `req` 的 body 就空了，HTTP 层那边也不会再用它。
  ctx->request().take_from(req);

  const uvcpp_web_connection* conn = registry_.find(id);
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
  if (inflight_.find(id) != inflight_.end()) {
    // 同一个连接上还有没应答完的请求 = 客户端在用 HTTP 流水线。不支持：
    // 前一个上下文会被顶掉，它后面的响应可能再也发不出去。
    UVCPP_LOG_WARN(log_category::REQUEST)
        << "连接 " << id << " 上已有在途请求；HTTP 流水线不受支持，前一个"
        << "请求的上下文被顶掉";
  }
  inflight_[id] = ctx;

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
  const size_t routes = router_.route_count();
  if (routes != routes_seen_ || middleware_gen_ != cache_gen_) {
    chain_cache_.clear();
    routes_seen_  = routes;
    cache_gen_    = middleware_gen_;
    specials_built_ = false;
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
    std::lock_guard<std::mutex> lk(post_mutex_);
    batch.swap(post_queue_);
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
  if (!loop_started_.load()) return;

  UVCPP_LOG_INFO(log_category::CORE)
      << "开始停机：先停监听，宽限 " << cfg_.shutdown_grace_ms
      << " ms 等在途请求（当前 " << inflight_.size() << " 个，活连接 "
      << registry_.size() << " 条）";

  // 1. 不再接受新连接。注意它**不会**关掉已经建立的连接 —— 那正是宽限期
  //    存在的意义。
  uvcpp_tcp_server* tcp = tcp_server();
  if (tcp != nullptr) tcp->stop();

  shutdown_deadline_ms_ =
      loop_now_ms(loop()) + static_cast<int64_t>(cfg_.shutdown_grace_ms);

  // 2. 后面的动作全部交给看门狗定时器。
  //
  //    为什么不就地做完：`stop()` 走的是 `post()`，也就是说这段代码很可能
  //    正在 async 句柄的回调里跑。在这一帧里删 async 句柄就是把它自己正在
  //    执行的 `std::function` 连着析构掉。跳到一个定时器回调上，这个隐患
  //    就不存在了。
  if (shutdown_timer_ == nullptr) {
    finish_shutdown();
    return;
  }
  shutdown_timer_->start([this](uvcpp_timer*) { shutdown_step(); }, 0,
                         kShutdownPollMs);
}

void uvcpp_web_app::shutdown_step() {
  // ---- 第 0 拍：等在途请求跑完，或者宽限期到 ----
  if (shutdown_phase_ == 0) {
    if (!inflight_.empty() && loop_now_ms(loop()) < shutdown_deadline_ms_) {
      return;  // 继续等，看门狗下一拍再看
    }

    shutdown_phase_ = 1;
    if (shutdown_timer_ != nullptr) shutdown_timer_->stop();

    if (!inflight_.empty()) {
      UVCPP_LOG_WARN(log_category::CORE)
          << "宽限期到，仍有 " << inflight_.size()
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

    // 排水一拍：Close 帧出网 → 会话收到写完成 → 自己关掉底层连接 → 终结
    // 回调把它们从会话表和连接登记表里摘掉。这一步做完，下面那句
    // `close_all_clients()` 要处理的就只剩**普通 HTTP** 连接了。
    schedule_shutdown_step(kShutdownDrainMs);
    return;
  }

  // ---- 第 1 拍：关掉剩下的连接（这时 WS 的已经自己走完了） ----
  if (shutdown_phase_ == 1) {
    shutdown_phase_ = 2;

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
  if (shutdown_timer_ == nullptr) {
    shutdown_step();
    return;
  }
  shutdown_timer_->start([this](uvcpp_timer*) { shutdown_step(); },
                         static_cast<uint64_t>(delay_ms), 0);
}

void uvcpp_web_app::finish_shutdown() {
  const uvcpp_tcp_server* tcp = tcp_server();
  if (tcp != nullptr && tcp->client_count() != 0) {
    UVCPP_LOG_WARN(log_category::CORE)
        << "停机收尾时仍有 " << tcp->client_count()
        << " 条连接留在登记表里（预期为 0）";
  }

  if (idle_timer_ != nullptr) {
    idle_timer_->stop();
    delete idle_timer_;
    idle_timer_ = nullptr;
  }

  if (shutdown_timer_ != nullptr) {
    shutdown_timer_->stop();
    delete shutdown_timer_;
    shutdown_timer_ = nullptr;
  }

  // 投递句柄：`delete` 会走 free_handle → uv_close(哨兵)，完成回调在本次
  // 迭代末尾把底层内存还回去。**必须在定时器回调里做，不能在 async 自己的
  // 回调里做**（那会析构正在执行的 std::function）。
  {
    std::lock_guard<std::mutex> lk(post_mutex_);
    delete async_;
    async_ = nullptr;
    // 顺带把还没跑的任务丢掉：循环要停了，它们永远不会被执行，留着只会
    // 让 post_queue_ 的析构去销毁一堆捕获了上下文的闭包。
    post_queue_.clear();
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
