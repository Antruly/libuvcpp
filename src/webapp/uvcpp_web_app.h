/**
 * @file src/webapp/uvcpp_web_app.h
 * @brief Web 应用：把路由、中间件、连接登记、日志、生命周期装到一起的东西。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一层是给**应用作者**用的。上面几层是给框架作者用的：
 *
 * | 层 | 使用者要面对的东西 |
 * |---|---|
 * | `src/web/`（http/ws/ssl） | `uvcpp_http_request`、手拼响应报文、自己 match 路径 |
 * | `src/webapp/`（本层） | 只写 `handler`：收请求、填响应 |
 *
 * 最小可用示例（跑起来就是一个能对外服务的 web 服务）：
 *
 * @code
 *   #include <webapp/uvcpp_web_app.h>
 *   using namespace uvcpp;
 *
 *   int main() {
 *     uvcpp_web_app app;
 *     app.set_port(8080)
 *        .set_max_body_size(8 * 1024 * 1024)
 *        .use(web_middleware_access_log())
 *        .use(web_middleware_error_handler(
 *            [](uvcpp_web_request&, uvcpp_web_response& resp, const std::string& what) {
 *              resp.status(500).json_str("{\"error\":\"internal\"}");
 *              resp.end();
 *            }));
 *
 *     app.get("/hello", [](uvcpp_web_request& req, uvcpp_web_response& resp,
 *                          uvcpp_web_next next) {
 *       resp.json_str("{\"hello\":\"world\"}");
 *       resp.end();
 *     });
 *
 *     app.get("/user/:id", [](uvcpp_web_request& req, uvcpp_web_response& resp,
 *                             uvcpp_web_next next) {
 *       const std::string* id = req.param("id");
 *       resp.text(id ? *id : "");
 *       resp.end();
 *     });
 *
 *     app.start();            // 起后台线程跑事件循环
 *     app.join();             // 主线程在这儿等
 *     return 0;
 *   }
 * @endcode
 *
 * 几条必须知道的规则
 * ------------------
 * **一、`resp.end()` 要调。** 它不是"发送"，是"我写完了"的标记。框架收尾时
 * 才真正把响应发出去（这样中间件才有机会在最后改状态码、加头）。不调不会
 * 丢响应，但会打一条 WARN —— 那是在提示你"这个处理器可能少写了一半"。
 *
 * **二、重活不要放在 handler 里。** 整条链跑在事件循环线程上（`advance()`
 * 是同步循环），任何阻塞都会卡住**所有**连接。要读盘、要算、要访问数据库，
 * 用 `uvcpp_work` 丢到工作线程池，完成回调（跑在 loop 线程上）里接着写响应、
 * 调 `next()`：
 *
 * @code
 *   uvcpp_web_app app;
 *   app.get("/slow", [&app](uvcpp_web_request& req, uvcpp_web_response& resp,
 *                           uvcpp_web_next next) {
 *     // 把 next **按值捕获**进下面的闭包 —— 框架据此知道"这一环会异步
 *     // 恢复"，链挂起等它，而不是当成"处理器写完了"。
 *     //
 *     // resp 的引用在异步回调里同样有效：响应对象的生存期等于上下文的，
 *     // 而 next 里钉着上下文。
 *     uvcpp_web_response* rp  = &resp;
 *     uvcpp_work*         w   = new uvcpp_work();
 *     w->init();
 *     w->queue_work(app.loop(),
 *                   [](uvcpp_work*) { /* 工作线程：这里可以放心阻塞 *\/ },
 *                   [next, rp](uvcpp_work* w, int) {
 *                     rp->json_str("{\"ok\":true}");
 *                     rp->end();
 *                     next();   // 接着跑链（后面没有中间件就是收尾发送）
 *                     delete w;
 *                   });
 *   });
 * @endcode
 *
 * `next()` 在哪个线程调都行 —— 框架会自动投回 loop 线程。
 *
 * **三、拿不到裸连接。** 框架里处处是 `uvcpp_web_conn_id` 而不是
 * `uvcpp_tcp_client*`：连接断开之后，那个指针的地址会被新连接复用，异步
 * 回调再拿它写就等于把 A 的响应发给 B。真要动原始 socket，走
 * `ctx.raw_client()` —— 它会先查登记表，连接没了就返回 `nullptr`。
 *
 * **四、注册路由要在 `start()` 之前。** 路由表本身不是线程安全的；运行中
 * 注册会打乱链缓存（框架有路由数量校验兜底，但那是安全网不是用法）。
 *
 * 线程模型
 * --------
 * - `start()` / `start_background()`：起一个 `std::thread`，bind + `uv_run`。
 *   调用方阻塞到 bind 结果出来，所以返回后 `bound_port()` 立刻可用。
 * - `run()`：不起线程，在**当前线程**跑循环（想自己掌控线程时用）。
 * - `stop()`：**线程安全**。内部通过 `uvcpp_async` 把收尾动作投递到 loop
 *   线程，所以从任何线程调都行。
 * - 析构会兜底 `stop()` + `join()`，但别指望它：等到析构才停，说明服务已经
 *   多跑了一段不确定的时间。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_APP_H
#define SRC_WEBAPP_UVCPP_WEB_APP_H

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>
#include <web/uvcpp_http_server.h>
#include <web/uvcpp_ws_server.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_connection.h>
#include <webapp/uvcpp_web_context.h>
#include <webapp/uvcpp_web_middleware.h>
#include <webapp/uvcpp_web_router.h>
#include <webapp/uvcpp_web_static.h>
#include <webapp/uvcpp_web_ws.h>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_context.h>
#endif

namespace uvcpp {

class uvcpp_async;
class uvcpp_timer;

// =========================================================================
// 配置
// =========================================================================

/**
 * @brief 启动参数。
 *
 * 用**显式构造函数**给默认值，而不是成员初始化器 —— 成员初始化器（NSDMI）
 * 会让结构体失去 C++11 的聚合初始化资格，`uvcpp_web_app_config cfg{8080}`
 * 就编译不过了。
 *
 * 想按字段初始化再微调，可以这样：
 * @code
 *   uvcpp_web_app_config cfg;
 *   cfg.port = 9000;
 *   cfg.max_body_size = 1024;
 *   uvcpp_web_app app(cfg);
 * @endcode
 * 或者干脆用 App 的链式 setter（两者改的是同一份配置）。
 */
struct UVCPP_API uvcpp_web_app_config {
  /** @brief 监听地址。`"0.0.0.0"` = 所有 IPv4 网卡；含 `:` 则按 IPv6 处理。 */
  std::string host;

  /**
   * @brief 监听端口。**传 0 表示由系统分配**，实际端口从 `bound_port()` 取。
   *        测试里这么用可以避免端口撞车。
   */
  int port;

  /** @brief `listen()` 的 backlog。 */
  int backlog;

  /**
   * @brief 请求体上限（字节），超过直接回 413 且**不路由**。0 = 不限。
   *
   * 默认 16 MiB 而不是"不限"：不限意味着一个恶意请求就能把内存吃光
   * （HTTP 层是边解析边把 body 攒在内存里的）。要传大文件，请明确地调大它，
   * 而不是把上限关掉 —— Phase 3 的流式上传才是大文件的正解。
   */
  size_t max_body_size;

  /** @brief 是否自动压缩响应体（需要构建时开了 zlib）。 */
  bool compression;

  /** @brief 触发压缩的最小 body 大小（字节）。太小的响应压了反而更大。 */
  size_t compress_min_body_size;

  /** @brief 是否自动装访问日志中间件。 */
  bool access_log;

  /** @brief 全局最低日志等级。 */
  log_level min_log_level;

  /**
   * @brief 每个响应都会带上的 `Server` 头。空字符串 = 不加。
   *
   * @warning 别填版本号 —— 那等于主动告诉扫描器该用哪个 CVE。
   */
  std::string server_header;

  /**
   * @brief 优雅关闭的宽限时间（毫秒）。
   *
   * 停止时先不再接受新连接，给在途请求这段时间收尾；超时后剩下的连接一律
   * 强制关闭。0 = 不等，立刻强关。
   */
  int shutdown_grace_ms;

  /**
   * @brief 闲置超时（毫秒）。超过就直接关连接。**0 = 关闭**。
   *
   * 挡的是慢速攻击（slowloris）：不设超时的话，一个只发了半行请求头就再也不
   * 发字节的连接可以永久占住一个连接槽和一份解析器状态 —— 几百个这样的连接
   * 就能让服务不再接受新连接。默认 60 秒。
   *
   * 计时规则（两条，别只记一条）：
   *   - **连接之间**的空闲从「最近一次收到字节」算起；
   *   - **正在收的请求**从「这个请求的第一个字节」算起 —— 慢速滴字节
   *     （每 30 秒发一个字符）刷新不了它，整个请求必须在超时内收完。
   *
   * 正在处理的请求（`inflight_count()` 里的）**不受影响**：那种情况是服务
   * 端自己慢，怪不到客户端头上，关连接只会让正确的请求失败。
   *
   * @warning 上传大文件时这条会变成"整个上传必须在 N 毫秒内完成"。Phase 3
   *          的流式上传会用背压另行处理；在那之前要传大文件就把这个值放大。
   */
  int idle_timeout_ms;

  /** @brief 未命中任何路由但该路径支持别的方法时，自动答 OPTIONS。 */
  bool auto_options;

  /** @brief HEAD 请求没有专门注册时，回退到同路径的 GET handler（丢掉 body）。 */
  bool head_as_get;

  uvcpp_web_app_config();
};

// =========================================================================
// 原始数据钩子的返回值
// =========================================================================

/**
 * @brief 原始数据钩子看完一段字节之后，这些字节该怎么处置。
 *
 * 单独一个类型（而不是 `bool`）就是为了**没机会写反**：底层
 * `uvcpp_http_server` 的约定是「true = 放行」，如果这层也透传一个 bool，
 * 使用者每写一次都要在脑子里做一次取反 —— 写反了不会有任何报错，只是
 * 数据静默地走错路（该吞的被解析成 400，该解析的被吞掉）。
 */
enum class uvcpp_web_raw_action {
  /** 交给 HTTP 解析器（默认语义：我只是看看）。 */
  PASS,
  /** 我接管了：这一块不进解析器，由我自己处理。 */
  CONSUME
};

/**
 * @brief 原始数据接管回调（`set_raw_data_claim()` 用）。
 *
 * @param id     连接 id。
 * @param client 底层连接（要直接回字节时用；**别存起来**，见 `App` 的说明）。
 * @param data   收到的字节，只读。
 * @param len    字节数。
 * @return 见 `uvcpp_web_raw_action`。
 */
using uvcpp_web_raw_data_claim = std::function<uvcpp_web_raw_action(
    uvcpp_web_conn_id id, uvcpp_tcp_client* client, const char* data,
    size_t len)>;

// =========================================================================
// App
// =========================================================================

/**
 * @brief Web 应用。
 *
 * 继承 `uvcpp_web_context_host`，也就是「事件循环 + 连接登记表 + 发送」这
 * 三件事的实现方。context 通过这个接口回调过来，所以 `uvcpp_web_response`
 * 那一层不需要认识 App。
 */
class UVCPP_API uvcpp_web_app : public uvcpp_web_context_host {
 public:
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_web_app)

  uvcpp_web_app();
  explicit uvcpp_web_app(const uvcpp_web_app_config& cfg);

  /**
   * @brief 析构。
   *
   * **会兜底 `stop()` + `join()`** —— 但这不是给你偷懒用的：等到析构才停，
   * 说明服务已经多跑了一段不确定的时间。正常路径上请显式停。
   */
  virtual ~uvcpp_web_app();

  // -----------------------------------------------------------------
  // 配置（链式）
  // -----------------------------------------------------------------

  const uvcpp_web_app_config& config() const { return cfg_; }

  uvcpp_web_app& set_host(const std::string& host);
  uvcpp_web_app& set_port(int port);
  uvcpp_web_app& set_backlog(int backlog);
  uvcpp_web_app& set_max_body_size(size_t bytes);
  uvcpp_web_app& set_compression(bool enable);
  uvcpp_web_app& set_compress_min_body_size(size_t bytes);
  uvcpp_web_app& set_access_log(bool enable);
  uvcpp_web_app& set_log_level(log_level level);
  uvcpp_web_app& set_server_header(const std::string& value);
  uvcpp_web_app& set_shutdown_grace_ms(int ms);
  uvcpp_web_app& set_idle_timeout_ms(int ms);
  uvcpp_web_app& set_auto_options(bool enable);
  uvcpp_web_app& set_head_as_get(bool enable);

#if UVCPP_OPENSSL_ENABLE
  // -----------------------------------------------------------------
  // TLS（HTTPS）。开起来之后 WSS 自动可用（TLS 在传输层，WS 在它上面）
  // -----------------------------------------------------------------

  /**
   * @brief 用 PEM 文件开 HTTPS：`cert_file` 是证书**链**，`key_file` 是私钥。
   *
   * **在调用点就把证书读进来并校验**，而不是拖到 `start()`：
   *   - 路径写错、证书和私钥不配对这类错误，部署时最想立刻知道，而不是
   *     等到第一个请求进来才在握手阶段失败（那时只能看到一个连接被拒）；
   *   - 失败时会**把失败原因写进日志**（`log_category::SSL`），并把 app 标记
   *     成"TLS 不可用"，于是 `start()` 返回 `UV_EINVAL` 而不是静默地以明文
   *     提供服务 —— 后者是这里最坏的失败模式（你以为在跑 HTTPS）。
   *
   * @warning 链式 setter 无法返回错误，所以才要"当场校验 + `start()` 兜底"
   *          这两道。用 `ssl_error()` 取具体原因。
   */
  uvcpp_web_app& enable_ssl(const std::string& cert_file,
                            const std::string& key_file);

  /**
   * @brief 用 PEM **内容**开 HTTPS，不读文件。
   *
   * 给证书来自环境变量 / KMS / 容器 secret 的部署方式用。
   */
  uvcpp_web_app& enable_ssl_pem(const std::string& cert_pem,
                                const std::string& key_pem);

  /**
   * @brief 现场生成自签证书开 HTTPS。
   *
   * **只用于测试和受控内网。** 自签证书没有任何 CA 为它背书，任何合规的
   * 客户端默认都会拒绝它；把它放到公网等于让用户自己决定要不要无视警告。
   * 正式环境用 `enable_ssl()` 配真证书。
   *
   * @param host 证书的 CN。客户端按主机名校验时它必须和访问用的域名一致。
   * @param bits RSA 密钥位数。
   */
  uvcpp_web_app& enable_self_signed(const std::string& host = "localhost",
                                    int bits = 2048);

  /** @brief 是否配置了 TLS（不代表已经成功 —— 见 `ssl_error()`）。 */
  bool ssl_enabled() const;

  /** @brief 当前的 TLS 上下文；没配或创建失败时为 nullptr。 */
  uvcpp_ssl_context* ssl_context() const { return ssl_ctx_.get(); }

  /** @brief TLS 配置失败的原因（空 = 没出错）。 */
  const std::string& ssl_error() const { return ssl_error_; }
#endif  // UVCPP_OPENSSL_ENABLE

  // -----------------------------------------------------------------
  // 路由
  // -----------------------------------------------------------------

  /**
   * @brief 拿路由表本体（注册路由、分组、查表）。
   *
   * @note 只想注册几条路由的话用下面那几个转发方法更省事。
   */
  uvcpp_web_router& router() { return router_; }
  const uvcpp_web_router& router() const { return router_; }

  /**
   * @brief 加一个全局中间件。
   *
   * 顺序 = 执行顺序：**先注册的在最外层**。访问日志要覆盖全链就第一个注册。
   *
   * @note 已经发出去的请求用的是**当时**组装好的链。运行中加中间件会让链
   *       缓存作废并重建（旧链保留到在途请求跑完），所以行为是安全的，只是
   *       同一时刻可能有两种链在跑 —— 中间件注册请在 `start()` 之前做完。
   */
  uvcpp_web_app& use(const uvcpp_web_middleware& mw);

  uvcpp_web_app& get(const std::string& pattern,
                     const uvcpp_web_handler& handler);
  uvcpp_web_app& post(const std::string& pattern,
                      const uvcpp_web_handler& handler);
  uvcpp_web_app& put(const std::string& pattern,
                     const uvcpp_web_handler& handler);
  uvcpp_web_app& del(const std::string& pattern,
                     const uvcpp_web_handler& handler);
  uvcpp_web_app& patch(const std::string& pattern,
                       const uvcpp_web_handler& handler);
  uvcpp_web_app& head(const std::string& pattern,
                      const uvcpp_web_handler& handler);
  uvcpp_web_app& options(const std::string& pattern,
                         const uvcpp_web_handler& handler);
  /** @brief 注册所有方法都命中同一 handler 的路由。 */
  uvcpp_web_app& any(const std::string& pattern,
                     const uvcpp_web_handler& handler);

  /** @brief 前缀分组：`app.group("/api").get("/x", h)` → 注册 `/api/x`。 */
  uvcpp_web_router group(const std::string& prefix) const {
    return router_.group(prefix);
  }

  /**
   * @brief 把一个目录挂到 URL 前缀上（Range/ETag/304/LRU/安全边界一整套）。
   *
   * ```cpp
   * uvcpp_web_static_options o;
   * o.cache_control = "public, max-age=31536000, immutable";
   * app.serve_static("/assets", "./public", o);
   * ```
   *
   * 注册的是**四条**路由：`<前缀>` 与 `<前缀>/*path`，各带 GET 和 HEAD。
   * 为什么前缀那一版也要注册：`/assets` 和 `/assets/` 都要能取到索引文件，
   * 只注册通配那条的话 `/assets` 会 404（通配至少要吃一段）。
   *
   * 命中静态路由后**一定会给出响应**（含 404），不会 `next()` 到别的路由 ——
   * 它是这个前缀下的兜底。想让别的路由优先，把它们注册在静态之前即可
   * （静态字面量 > 参数 > 通配的优先级保证 `app.get("/assets/special")`
   * 仍然赢过 `/assets/*path`）。
   *
   * @param prefix   URL 前缀。空串或 `"/"` 表示挂到根。
   * @param root_dir 文档根（相对路径按**进程工作目录**解析）。
   * @param opts     选项，见 `uvcpp_web_static_options`。
   * @return 静态服务对象，想手动 `clear_cache()` 或读命中统计时留着它；
   *         路由里的 handler 自己也持有一份，所以即使你不接也会活到 App 结束。
   */
  std::shared_ptr<uvcpp_web_static> serve_static(
      const std::string& prefix, const std::string& root_dir,
      const uvcpp_web_static_options& opts = uvcpp_web_static_options());

  // -----------------------------------------------------------------
  // WebSocket
  // -----------------------------------------------------------------

  /**
   * @brief 打开 WebSocket 支持：让 HTTP 升级请求按**路径**分派到 WS 路由。
   *
   * 幂等；`websocket()` 会隐式调它，所以通常不必自己调。
   *
   * **TLS 不需要额外工作**：TLS 过滤器在传输层（`uvcpp_tcp_client`），WS 跑在
   * 它上面，所以 `enable_ssl()` 一旦生效，这里的 WS 自动就是 **WSS**。
   *
   * 升级请求的**兜底行为**：路径没有命中任何 WS 路由时，它被当成一个**普通
   * HTTP 请求**走正常路由（命中就执行，没有就 404）。所以升级请求不会哑掉，
   * 也不会因为"注册过 WS"就让同路径的 HTTP 路由失效。
   *
   * @note 升级成功后，那条连接**不再受闲置超时约束**（`set_idle_timeout_ms`）。
   *       60 秒没有消息对 WS 是常态（聊天室、服务端推送），用 HTTP 的判据去
   *       关它是误杀。
   *       @warning 代价是**WS 连接从此没有任何超时保护** —— 框架不会关掉一条
   *       安静但已死的 WS 连接。需要保活就自己在应用层发 ping/pong。
   */
  uvcpp_web_app& enable_wss();

  /**
   * @brief 注册一条 WebSocket 路由。**隐式打开 WS 支持**（等于先调 `enable_wss()`）。
   *
   * ```cpp
   * app.websocket("/chat/:room", [](uvcpp_web_ws_request& ws) {
   *   const std::string room = ws.param("room") ? *ws.param("room") : "";
   *   uvcpp_ws_connection* c = ws.connection();
   *   c->on_text([room](const std::string& msg) { broadcast(room, msg); });
   *   c->on_close([room](ws_close_code code, const std::string&) { leave(room, code); });
   * });
   * ```
   *
   * 模式语法与 HTTP 路由**完全相同**（`:name` 单段参数、`*name` 多段通配、
   * 静态字面量），优先级规则也一样（静态 > 参数 > 通配，与注册顺序无关）。
   * 但两张表是**独立**的：同一个模式可以同时有 HTTP 路由和 WS 路由，互不影响 ——
   * 升级请求只会去打 WS 表。
   *
   * @note 注册必须在 `start()` 之前完成（与 HTTP 路由同一条约定）。
   */
  uvcpp_web_app& websocket(const std::string& pattern,
                           const uvcpp_web_ws_handler& handler);

  /**
   * @brief WS 服务对象（配置逃生口：压缩策略、会话数……）。
   *
   * 没调过 `enable_wss()` / `websocket()` 时返回 nullptr。
   */
  uvcpp_ws_server* ws_server() { return ws_server_; }

  /** @brief 是否已经打开 WS 支持。 */
  bool wss_enabled() const { return ws_server_ != nullptr; }

  /**
   * @brief 当前活动的 WS 会话数。
   *
   * @note 这是**实时**值，读到的可能是"已经开始关闭但还没回收"的会话。测试里
   *       等它归零要用带上限的重试循环，不要只读一次。
   */
  size_t ws_session_count() const;

  /**
   * @brief 累计已回收的 WS 会话数（单调递增）。
   *
   * 与 `ws_session_count()` 配对使用才能闭环：活动数归零 + 已回收数等于建过
   * 的总数，才说明会话真的被释放了（只数活动数是看不出来的 —— 泄漏的会话
   * 一样会让活动数看着正常）。
   */
  size_t ws_recycled_session_count() const;

  // -----------------------------------------------------------------
  // 连接 / 原始数据钩子
  // -----------------------------------------------------------------

  /**
   * @brief 新连接被接受时回调 —— **早于该连接上的任何请求**。
   *
   * 可以注册多个，按注册顺序调用。`id` 是框架给这条连接发的号（从 1 起，
   * 永不复用）。
   *
   * @warning 在 loop 线程上调用；这里做的任何阻塞都会卡住所有连接。回调里
   *          抛异常会被吞掉并记一条 ERROR，不影响这个连接继续服务。
   */
  uvcpp_web_app& on_connection(
      const std::function<void(uvcpp_web_conn_id id,
                               uvcpp_tcp_client* client)>& cb);

  /**
   * @brief 连接断开时回调（对端断开、主动关闭、出错，**都会**触发，恰好一次）。
   *
   * 触发点在框架从登记表里摘掉这条连接**之后**，所以回调里 `id` 已经查不
   * 到活连接了（`connection(id)` 返回 `nullptr`，这是刻意的）。
   */
  uvcpp_web_app& on_connection_close(
      const std::function<void(uvcpp_web_conn_id id,
                               uvcpp_tcp_client* client)>& cb);

  /**
   * @brief 观察原始 TCP 数据 —— **在 HTTP 解析之前**。
   *
   * 给抓包式排查、协议嗅探、按字节做访问控制用的。可以注册多个（扇出），
   * 每个都拿到同一份字节。
   *
   * @param data 收到的字节。**只读** —— 改写它会破坏解析器的缓冲算术
   *             （它认为"我交出去的字节就是收到的字节"）。
   *
   * @warning 跑在 loop 线程上。要真分析（特征扫描、落盘）就 `memcpy` 一份
   *          丢给 `uvcpp_work`，别在这儿做。
   */
  uvcpp_web_app& on_raw_tcp_data(
      const std::function<void(uvcpp_web_conn_id id, uvcpp_tcp_client* client,
                               const char* data, size_t len)>& cb);

  /**
   * @brief 接管原始数据：返回 `CONSUME` 表示**这一块数据不进 HTTP 解析器**。
   *
   * 用来在 HTTP 之前接自己的协议（自定义握手、端口复用）。单槽，重复设置
   * 会替换上一个。
   *
   * 已经注册的观察者**仍然会先看到**这些字节（它们只是看），然后才轮到本
   * 回调决定去向 —— 也就是说"接管"影响的是解析器，不是观察者。
   *
   * 返回值刻意**不用 `bool`**：底层 `uvcpp_http_server` 的钩子是
   * 「true = 放行」的反向约定，一层 `bool` 传上来谁都得记两遍，写反了还
   * 一声不响（数据要么被吞、要么被当 HTTP 解析成 400）。枚举没有这个余地。
   *
   * @warning 接管了就要负责：这一块被吃掉之后解析器永远看不到它，而 HTTP
   *          层仍然挂在这个连接上等着收请求。用完记得把连接交还或者关掉。
   */
  uvcpp_web_app& set_raw_data_claim(const uvcpp_web_raw_data_claim& cb);

  /** @brief 撤掉接管回调（数据照常进解析器）。 */
  uvcpp_web_app& clear_raw_data_claim();

  // -----------------------------------------------------------------
  // 生命周期
  // -----------------------------------------------------------------

  /**
   * @brief 起后台线程并开始服务。**阻塞到 bind 有结果为止**。
   *
   * @return 0 成功；否则 bind/listen 的 libuv 错误码（负数）。失败时不会有
   *         线程残留，可以直接改配置重试。
   *
   * @note **一个实例只能启动一次。** 停掉之后不能再 `start()`（监听句柄、
   *       loop 亲和的句柄在停机时都释放了）。再传 `UV_EBUSY`。要重新起服务
   *       就构造一个新的 `uvcpp_web_app`。
   */
  int start();
  /** @brief `start()` 的别名（名字更直白）。 */
  int start_background();

  /**
   * @brief 在**当前线程**跑事件循环（阻塞到 `stop()`）。
   *
   * 想自己掌控线程时用这个而不是 `start()`。注意：这时候 loop 线程就是你
   * 现在这个线程，`stop()` 要么从别的线程调、要么在某个回调里调。
   */
  int run(uv_run_mode md = UV_RUN_DEFAULT);

  /**
   * @brief 优雅停机：不再接受新连接 → 等在途请求收尾（最多
   *        `shutdown_grace_ms`）→ 关掉剩下的连接 → 停循环。**线程安全**。
   *
   * 幂等：重复调没有副作用。还没启动时是空操作。
   */
  void stop();

  /**
   * @brief 等后台线程结束。`start()`/`run()` 之后必须调（析构也会兜底）。
   *
   * @warning 从 loop 线程自己调会死锁（等自己结束）。正常情况下不会有人
   *          这么干，但这是个值得记住的坑。
   */
  void join();

  /** @brief 实际监听端口（端口填 0 时由系统分配，从这里取）。未启动返回 0。 */
  int bound_port() const;

  /** @brief 是否正在服务（`start()` 之后、循环退出之前）。 */
  bool running() const;

  /** @brief 循环线程是否已经起来（`post()` 能不能用的判断依据）。 */
  bool loop_started() const;

  // -----------------------------------------------------------------
  // 底层访问（框架不封闭）
  // -----------------------------------------------------------------

  uvcpp_http_server* http_server() { return http_; }
  uvcpp_tcp_server* tcp_server();

  /** @brief 连接登记表（只读）。 */
  const uvcpp_web_connection_registry& connections() const { return registry_; }

  /** @brief 当前活连接数。 */
  size_t connection_count() const { return registry_.size(); }

  /** @brief 在途请求数（上下文还挂在框架上的）。 */
  size_t inflight_count() const { return inflight_.size(); }

  // -----------------------------------------------------------------
  // uvcpp_web_context_host
  // -----------------------------------------------------------------

  virtual bool on_loop_thread() const;
  virtual void post(std::function<void()> fn);
  virtual uvcpp_loop* loop() const;
  virtual uvcpp_tcp_client* connection(uvcpp_web_conn_id id);
  virtual void send_response(uvcpp_web_context& ctx);
  virtual void abort_request(uvcpp_web_context& ctx);
  virtual void context_finished(uvcpp_web_context& ctx);

 private:
  // --- 组装 ------------------------------------------------------

  /** @brief 在 loop 线程上建句柄、装钩子、bind、listen。 */
  int init_on_loop_thread();

  /** @brief 新连接被接受（HTTP 层的 accept 钩子）：发 id、登记、跑用户钩子。 */
  void on_accept(uvcpp_tcp_client* client);

  /** @brief 连接断开：摘登记表、跑用户钩子。 */
  void on_close(uvcpp_tcp_client* client);

  /** @brief HTTP 层兜底入口：所有请求都到这儿，再在框架内路由。 */
  void on_http_request(uvcpp_http_request& req, uvcpp_http_response& resp,
                       uvcpp_tcp_client* client);

  /**
   * @brief HTTP 层升级入口：按路径把升级请求分派到 WS 路由。
   *
   * **框架独占 `on_upgrade` 这个单槽。** `uvcpp_ws_server` 自己也想要它
   * （`attach()`），但那样分派权就归它了 —— 而它只能给出一条全局的
   * `on_connection`，做不了"按路径选处理器"。所以框架自己攥着这个槽，
   * 只在路径命中时把请求转交 `ws_server_->handle_upgrade()`。
   */
  void on_ws_upgrade(uvcpp_http_request& req, uvcpp_tcp_client* client);

  /** @brief 路径已命中的升级：校验 key、建请求视图、交给 `ws_server_`。 */
  void dispatch_ws(const web_route_match& m,
                   const uvcpp_web_ws_handler& handler,
                   uvcpp_http_request& req,
                   uvcpp_tcp_client* client);

  /** @brief 把匹配结果变成一次链的执行。 */
  void dispatch(const std::shared_ptr<uvcpp_web_context>& ctx,
                const web_route_match& m);

  /** @brief HTTP 层的原始数据钩子实现（扇出 + 接管判断 + 记活跃）。 */
  bool handle_raw_data(uvcpp_tcp_client* client, const char* data, size_t len);

  /** @brief 闲置超时扫描（定时器每拍跑一次）。 */
  void idle_sweep();

  // --- 链缓存 ----------------------------------------------------

  /**
   * @brief 让链缓存跟上路由表的变化。
   *
   * 缓存是按 `const uvcpp_web_handler*` 索引的，而那个指针指向路由表内部
   * —— **注册新路由可能让路由表重新分配**，旧指针要么悬垂，要么（更坏）
   * 被新路由的处理器复用，于是缓存命中到**别人的链**。所以每次派发前拿
   * 路由总数比一下，变了就整表作废。
   */
  void sync_chains();
  const std::vector<uvcpp_web_handler>* build_chain(
      const uvcpp_web_handler* route_handler);
  void build_special_chains();

  // --- 停机 ------------------------------------------------------

  void begin_shutdown();
  void shutdown_step();
  /** @brief 再等 `delay_ms` 毫秒跑下一拍 `shutdown_step()`（看门狗驱动）。 */
  void schedule_shutdown_step(int delay_ms);
  void finish_shutdown();
  void drain_posts();

  // --- 状态 ------------------------------------------------------

  uvcpp_web_app_config cfg_;

#if UVCPP_OPENSSL_ENABLE
  /**
   * @brief 记录 TLS 配置失败：写日志、存原因、把上下文清空。
   *
   * 清空是关键的一半 —— 只要 `ssl_ctx_` 为空而 `ssl_requested_` 为真，
   * `start()` 就会失败，绝不会退化成"以为在跑 HTTPS，其实是明文"。
   */
  void ssl_fail(const std::string& why);

  /**
   * @brief 调 `enable_ssl*()` 时建好的 TLS 上下文（start() 时装到传输层）。
   *
   * **声明位置在 `http_` 之前是刻意的**：成员按声明顺序构造、**逆序析构**，
   * 所以这样写 TLS 上下文会**比** `http_`（它里面的 `uvcpp_tcp_server` 持有
   * 本上下文的裸指针）**晚一步销毁**。实际安全性还有一层保障 —— 每个已建立
   * 的 SSL 对象（`SSL_new`）都持有 ctx 的引用计数 —— 但"被引用者先于引用者
   * 销毁"是不该依赖引用计数去兜的形状。
   */
  std::shared_ptr<uvcpp_ssl_context> ssl_ctx_;
  bool        ssl_requested_ = false;
  std::string ssl_error_;
#endif  // UVCPP_OPENSSL_ENABLE

  uvcpp_http_server* http_;
  uvcpp_web_router   router_;
  std::vector<uvcpp_web_middleware> middlewares_;

  /**
   * @brief WS 服务（`enable_wss()` 时惰性创建，nullptr = 没开 WS）。
   *
   * **必须比 `http_` 先销毁**（`~uvcpp_web_app` 里显式 `delete ws_server_`
   * 再 `delete http_`）：`uvcpp_ws_server` 的析构会 `sessions_.shutdown()`，
   * 而它那个延迟回收用的 async 句柄必须死在 loop 之前 —— loop 归 `http_`
   * 底下的 tcp_server 所有。
   */
  uvcpp_ws_server* ws_server_;

  /**
   * @brief WS 路由表。与 `router_` 是**两张独立的表**。
   *
   * 表里存的 handler 是**占位用的空操作** —— 它的唯一职责是让
   * `uvcpp_web_router` 帮我做模式解析、参数绑定和优先级排序。真正的处理器
   * 按**模式串**存在 `ws_handlers_` 里，用匹配结果的 `pattern` 去查。
   *
   * 为什么按模式串查而不是按 `const uvcpp_web_handler*` 做身份映射：后者有
   * "新注册一条路由让表重新分配、旧指针被新处理器复用"的隐患（`chain_cache_`
   * 那一整套作废逻辑就是为它才存在的）。模式串是值，没有这个问题。
   */
  uvcpp_web_router ws_router_;
  std::map<std::string, uvcpp_web_ws_handler> ws_handlers_;

  /**
   * @brief 已经升级成 WS 的连接 id —— `idle_sweep()` 要跳过它们。
   *
   * **为什么不直接从登记表里摘掉**（`registry_.remove(id)`）：行为上两者等价
   * （`idle_sweep` 都不再扫它），但摘掉会让 `on_connection_close` 拿到的 id
   * 变成 0（`remove_by_client` 那时返回 false，关闭回调只能报 0），而用户
   * 钩子的契约是"拿到这条连接的 id"。留着真实 id 还让 `connection_count()`
   * 如实包含这些**开着的**连接。
   */
  std::set<uvcpp_web_conn_id> upgraded_;

  uvcpp_web_connection_registry registry_;
  std::map<uvcpp_web_conn_id, std::shared_ptr<uvcpp_web_context> > inflight_;

  // 链缓存的存储。用 deque：push_back 不会让已有元素的地址失效，而上下文
  // 只存 `const std::vector<uvcpp_web_handler>*` —— 地址必须稳。
  std::deque<std::vector<uvcpp_web_handler> > chain_storage_;
  std::map<const uvcpp_web_handler*,
           const std::vector<uvcpp_web_handler>*> chain_cache_;
  const std::vector<uvcpp_web_handler>* not_found_chain_;
  const std::vector<uvcpp_web_handler>* method_not_allowed_chain_;
  const std::vector<uvcpp_web_handler>* auto_options_chain_;
  bool   specials_built_;
  size_t cache_gen_;      ///< 建缓存时的中间件代数
  size_t routes_seen_;    ///< 建缓存时的路由总数
  size_t middleware_gen_; ///< 每加一个中间件 +1

  /**
   * @brief 兜底用的空链。
   *
   * 上下文只存链的**指针**，所以不能给它一个临时量（`run(std::vector<...>())`
   * 那样的写法会立刻悬垂）。正常路径上永远用不到它。
   */
  std::vector<uvcpp_web_handler> empty_chain_;

  // 钩子
  std::vector<std::function<void(uvcpp_web_conn_id, uvcpp_tcp_client*)> >
      connection_cbs_;
  std::vector<std::function<void(uvcpp_web_conn_id, uvcpp_tcp_client*)> >
      connection_close_cbs_;
  std::vector<std::function<void(uvcpp_web_conn_id, uvcpp_tcp_client*,
                                 const char*, size_t)> > raw_data_cbs_;
  uvcpp_web_raw_data_claim raw_data_claim_;

  // 线程
  std::thread thread_;
  bool        thread_started_;   ///< thread_ 能不能 join
  bool        threading_;        ///< true = start() 起的线程，false = run() 就地跑
  bool        started_once_;     ///< 已经成功启动过（实例只能起一次）

  // 这几个跨线程读写（`stop()` 可能来自任何线程），所以是原子而不是 bool
  // —— 声明成 volatile 只能保证"不被优化掉"，保证不了可见性和原子性。
  std::atomic<bool> loop_started_;  ///< 事件循环已经在跑（句柄已建）
  std::atomic<bool> running_;       ///< 正在服务
  std::atomic<bool> stopping_;      ///< 停机流程已启动（幂等用）

  /** 循环线程 id。只有 loop 线程会写，别的线程读 —— 所以用 mutex 护住。 */
  mutable std::mutex tid_mutex_;
  std::thread::id    loop_tid_;
  bool               tid_known_;

  /** 跨线程投递用的 async 句柄（**只在 loop 线程上建**）。 */
  uvcpp_async* async_;
  mutable std::mutex  post_mutex_;
  std::deque<std::function<void()> > post_queue_;

  /**
   * @brief 闲置超时扫描器（只在 loop 线程上建）。
   *
   * 循环跑（间隔见 `idle_sweep_interval_ms()`）；`idle_timeout_ms` 为 0 时
   * 压根不建 —— 关掉的功能在运行时不该有任何开销。
   */
  uvcpp_timer* idle_timer_;

  /** 停机看门狗（只在 loop 线程上建）。 */
  uvcpp_timer* shutdown_timer_;
  int64_t      shutdown_deadline_ms_;

  /**
   * @brief 停机进行到第几拍（看门狗每拍调一次 `shutdown_step()`）。
   *
   * | 值 | 含义 |
   * |---|---|
   * | 0 | 等在途请求跑完 / 宽限期到 |
   * | 1 | WS Close 帧已排队，**排水期**（等它们真出网、会话自行关闭） |
   * | 2 | 其余连接已发起关闭，**排水期**（等 `uv_close` 完成回调） |
   * | 3 | 收尾（`finish_shutdown()`） |
   */
  int          shutdown_phase_;

  int bound_port_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_APP_H
