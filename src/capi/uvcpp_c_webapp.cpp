/**
 * @file src/capi/uvcpp_c_webapp.cpp
 * @brief `uvcpp_c_webapp.h` 的实现：app / 路由 / 中间件 / req / resp / next /
 *        延迟应答 / 静态目录 / WebSocket。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这个文件里有四件事值得先读一遍（其余都是逐条对着 C++ 方法搬的）
 * ----------------------------------------------------------------------
 * 1. **回调期句柄：进回调登记、出回调毒化。**
 *    `uvcpp_c_req` / `uvcpp_c_resp` / `uvcpp_c_next` / `uvcpp_c_ws_req` /
 *    `uvcpp_c_ws_conn` 这五枚都在**栈上**造，进用户回调前 `register_head`、出
 *    来 `unregister_head`。于是"回调返回之后再用它"给的是
 *    `UVCPP_C_E_STALE`（`alive()` 先查表，表里没有就一个字节都不读），**不是
 *    段错误**。C# 侧把 `IntPtr` 存进字段再用是必然会发生的事，这一层把它变成
 *    一条能写进日志的错误码。
 *
 *    毒化必须**在异常路径上也发生**，所以下面每一处登记都配一个 RAII 的
 *    `FrameScope`，而不是"回调后面补一句 unregister" —— 后者在用户回调抛异常
 *    时会把一枚栈地址留在登记表里，而那块栈下一轮就被复用了。
 *
 * 2. **延迟应答 = 留住 `next` 的一份副本。**
 *    `uvcpp_c_deferred` 里那一枚 `uvcpp_web_next` 是 `new` 出来的**副本**。
 *    按框架的判据（`uvcpp_web_context::advance()` 里那个 `kept_next` 引用计数
 *    差），留住副本就等于"这一环会异步恢复" ⇒ 链挂起、上下文由那份 `next`
 *    自己续命。`resume()` 把它**取走**（`swap`，不是再拷一份）再调，于是
 *    "链继续"与"凭据归还"是同一件事，不会留下引用环。
 *
 *    ★ 刻意**不**用 `uvcpp_web_response::set_deferred()`：那一格在框架里
 *    没有任何读取点（`uvcpp_web_response.h` 自己就写着"这一枚没有消费者"）。
 *    真正决定"框架替不替你发"的只有上面那条"留没留 next"。
 *
 * 3. **活得比回调久的回调，一律按值捕获。**
 *    `on_sent` / `on_drain` / `send_file` 的完成回调都可能在本文件那些句柄
 *    毒化之后才被框架调用。所以它们捕获的是**C 函数指针 + user_data 的值**，
 *    绝不捕获句柄地址 —— 否则就是"用一块已经出栈的栈内存"。
 *    `uvcpp_c_ws_connection` 上挂的那四个事件回调同理（那里捕获的是
 *    `uvcpp_c_ws_events` 表的一份拷贝 + 连接指针：连接指针是安全的，因为那些
 *    回调本身就存在那个连接对象里面 —— 对象不在了，回调也没人会调）。
 *
 * 4. **回调期间不许 `free`。** 每个进用户代码的跳板都把 `app->cb_depth` 加一
 *    减一；`uvcpp_c_app_free()` 看见非零就返回 `UVCPP_C_E_STATE`。与
 *    `uvcpp_c_net.cpp` 那条同源，只是这边没有"框架交出来的对象"要回收，所以
 *    不需要坟场，只留计数。
 */

#include "capi/uvcpp_c_webapp.h"

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_context.h>
#include <webapp/uvcpp_web_handler.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_static.h>
#include <webapp/uvcpp_web_ws.h>
#include <web/uvcpp_ws_connection.h>

#include "capi/uvcpp_c_internal.h"

using uvcpp_c_detail::alive;
using uvcpp_c_detail::arm_loop_thread;
using uvcpp_c_detail::copy_out;
using uvcpp_c_detail::field_present;
using uvcpp_c_detail::register_head;
using uvcpp_c_detail::table_size_ok;
using uvcpp_c_detail::thread_ok;
using uvcpp_c_detail::unregister_head;

// ---------------------------------------------------------------------------
// 魔数
// ---------------------------------------------------------------------------
//
// 每个不透明句柄一枚，四字节小端写出来都是能读的 ASCII（core dump 里认得出）。

#define UVCPP_C_MAGIC_APP      UVCPP_C_MAGIC('e', 'a', 'p', '1')
#define UVCPP_C_MAGIC_REQ      UVCPP_C_MAGIC('e', 'q', 's', '1')
#define UVCPP_C_MAGIC_RESP     UVCPP_C_MAGIC('e', 'p', 's', '1')
#define UVCPP_C_MAGIC_NEXT     UVCPP_C_MAGIC('e', 'n', 'x', '1')
#define UVCPP_C_MAGIC_DEFERRED UVCPP_C_MAGIC('e', 'd', 'f', '1')
#define UVCPP_C_MAGIC_WS_REQ   UVCPP_C_MAGIC('e', 'w', 'q', '1')
#define UVCPP_C_MAGIC_WS_CONN  UVCPP_C_MAGIC('e', 'w', 'c', '1')

namespace {

namespace uv = uvcpp;

// 注意这里**没有** `struct uvcpp_c_app;` 之类的声明：头文件里那些
// `typedef struct uvcpp_c_xxx uvcpp_c_xxx;` 已经把标签声明在**全局**作用域了，
// 在匿名命名空间里再写一遍就等于声明了另一个类型（`{匿名}::uvcpp_c_app`），
// 于是"引用 uvcpp_c_app 有歧义"。下面那一堆定义必须写在全局作用域上，
// 才和头里那枚 typedef 是同一个类型。

/**
 * @brief 挂在 app 上的那些绑定的公共基类。
 *
 * 存在的理由只有一个：`uvcpp_c_app` 里那份"要删的绑定"表是**一张**，而表里
 * 混着两种东西（普通路由/中间件用的 `route_binding`、WS 升级用的 `ws_binding`）。
 * 没有虚析构的话删除就得 `reinterpret_cast` 成其中一种 —— 今天两个结构体布局
 * 恰好一样所以"能跑"，但那是**不定义行为**，而且将来谁给其中一个加个成员就
 * 会静默地删错大小。
 */
struct binding_base {
  virtual ~binding_base() {}
};

/// 一枚 C 路由回调 + 它的用户数据。**堆上**，由 app 持有，app 废掉时一起删。
struct route_binding : binding_base {
  uvcpp_c_route_cb cb;
  void*            user_data;

  route_binding() : cb(nullptr), user_data(nullptr) {}
};

/// 一次回调期帧：把 req / resp / next 三枚回调期句柄要用的东西放在一起。
/// 它自己也在栈上，`uvcpp_c_next` 里存一枚指向它的指针（给 `defer` 用）。
struct callback_frame {
  uv::uvcpp_web_request*  req;
  uv::uvcpp_web_response* resp;
  uvcpp_c_app*            app;
};

}  // namespace

// ---------------------------------------------------------------------------
// 句柄的真身
// ---------------------------------------------------------------------------

struct uvcpp_c_app {
  uvcpp_c_detail::handle_head head;  ///< 必须在偏移 0（`alive()` 按它读）

  uv::uvcpp_web_app* app;
  int  cb_depth;   ///< 正在跑的回调层数（> 0 时不许 free）
  int  started;    ///< start() / start_background() 调过（决定 free 要不要收尾）

  /// 注册时 `new` 出来的绑定，app 废掉时逐个 `delete`（**必须排在 C++ app
  /// 析构之后** —— 那些绑定地址正被路由里的闭包捕获着）。
  std::vector<binding_base*> routes;
};

struct uvcpp_c_req {
  uvcpp_c_detail::handle_head head;
  uv::uvcpp_web_request* req;
  uvcpp_c_app*           app;
};

struct uvcpp_c_resp {
  uvcpp_c_detail::handle_head head;
  uv::uvcpp_web_response* resp;
  uvcpp_c_app*            app;
};

struct uvcpp_c_next {
  uvcpp_c_detail::handle_head head;
  uv::uvcpp_web_next* next;   ///< 指向栈上那一份（回调期）
  callback_frame*     frame;  ///< 同一层栈上的帧，`defer` 从这里取 req / resp
  uvcpp_c_app*        app;
  int                 used;   ///< 已经 run / defer 过（一枚 next 只能用一次）
};

struct uvcpp_c_deferred {
  uvcpp_c_detail::handle_head head;

  uvcpp_c_app*        app;
  uv::uvcpp_web_next* held;    ///< 堆上的那种副本：链挂起、上下文由它续命
  int                 resumed; ///< `_resume()` 调过

  /// 延迟期间的两枚句柄。**按值内嵌**在这里，于是地址稳定；
  /// `_deferred_req()` / `_deferred_resp()` 返回的就是它们的地址。
  uvcpp_c_req  req_handle;
  uvcpp_c_resp resp_handle;
};

// `uvcpp_c_ws_conn` 排在前面：下面那个 `uvcpp_c_ws_req` **按值**内嵌一枚它，
// 那是"升级回调里那枚连接句柄就是本次请求句柄的一个成员"这条设计，
// 而按值内嵌要求类型已经完整。
struct uvcpp_c_ws_conn {
  uvcpp_c_detail::handle_head head;
  uv::uvcpp_ws_connection* conn;
  uvcpp_c_app*             app;
};

struct uvcpp_c_ws_req {
  uvcpp_c_detail::handle_head head;
  uv::uvcpp_web_ws_request* ws;
  uvcpp_c_app*              app;
  uvcpp_c_ws_conn           conn;  ///< 同一层栈上，`ws_req_conn()` 返回它的地址
  int                       has_conn;
};

static_assert(offsetof(uvcpp_c_app, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_req, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_resp, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_next, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_deferred, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_ws_req, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_ws_conn, head) == 0, "handle_head 必须在偏移 0");

namespace {

/// @brief 回调期间挂上的"不许 free"计数（构造 +1，析构 -1）。
class CallbackScope {
 public:
  explicit CallbackScope(uvcpp_c_app* a) : app_(a) {
    if (app_ != nullptr) ++app_->cb_depth;
  }
  ~CallbackScope() {
    if (app_ != nullptr) --app_->cb_depth;
  }
  CallbackScope(const CallbackScope&) = delete;
  CallbackScope& operator=(const CallbackScope&) = delete;

 private:
  uvcpp_c_app* app_;
};

/**
 * @brief 回调期句柄的登记/毒化守卫。
 *
 * 三枚（或五枚）句柄登记进表，**离开作用域时全部摘掉** —— 包括用户回调抛异常
 * 那条路径。这是文件开头第 1 条那条纪律的机制：写成"回调后面补一句"就会在
 * 异常路径上漏，而漏掉的是一枚栈地址。
 */
class FrameScope {
 public:
  FrameScope() : n_(0) {}
  ~FrameScope() {
    for (int i = n_ - 1; i >= 0; --i) unregister_head(slots_[i]);
  }

  /** @brief 登记一枚句柄（最多 5 枚：req / resp / next / ws_req / ws_conn）。 */
  template <typename H>
  H* add(H* h, uint32_t magic) {
    register_head(h, magic);
    arm_loop_thread(h);
    if (n_ < 5) slots_[n_++] = reinterpret_cast<uvcpp_c_detail::handle_head*>(h);
    return h;
  }

  FrameScope(const FrameScope&) = delete;
  FrameScope& operator=(const FrameScope&) = delete;

 private:
  uvcpp_c_detail::handle_head* slots_[5];
  int                          n_;
};

/** @brief 把 C++ 的发送结果摊平成 C 的结构体（逐字段，不整块 memcpy）。 */
void fill_sent_info(const uv::uvcpp_web_sent_info& src, uvcpp_c_sent_info* out) {
  out->size          = static_cast<uint32_t>(sizeof(uvcpp_c_sent_info));
  out->status_code   = src.status_code;
  out->body_bytes    = src.body_bytes;
  out->connection_id = src.connection_id;
  out->ok            = src.ok ? 1 : 0;
  out->streamed      = src.streamed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 路由跳板
// ---------------------------------------------------------------------------

void route_trampoline(uvcpp_c_app* app, const route_binding* b,
                      uv::uvcpp_web_request& req, uv::uvcpp_web_response& resp,
                      uv::uvcpp_web_next next);

/**
 * @brief 把一个 C 路由回调包成 `uvcpp_web_handler` 挂上去。
 *
 * 绑定对象在堆上、由 app 持有：闭包捕获它的地址，而它活得比 app 短一瞬
 * （`uvcpp_c_app_free()` 先删 C++ app 再删绑定），所以路由被销毁时不会碰到
 * 一个已经释放的绑定。
 */
int add_route(uvcpp_c_app* a,
              uv::uvcpp_web_app& (uv::uvcpp_web_app::*reg)(
                  const std::string&, const uv::uvcpp_web_handler&),
              const char* pattern, uvcpp_c_route_cb cb, void* user_data) {
  if (!alive(a, UVCPP_C_MAGIC_APP) || a->app == nullptr)
    return UVCPP_C_E_STALE;
  if (pattern == nullptr || cb == nullptr) return UVCPP_C_E_INVALID_ARG;

  route_binding* b = new (std::nothrow) route_binding();
  if (b == nullptr) return UVCPP_C_E_NO_MEMORY;
  b->cb        = cb;
  b->user_data = user_data;

  uvcpp_c_app*   app_ptr     = a;
  route_binding* binding_ptr = b;
  (a->app->*reg)(std::string(pattern),
                 [app_ptr, binding_ptr](uv::uvcpp_web_request& r,
                                        uv::uvcpp_web_response& p,
                                        uv::uvcpp_web_next n) {
                   route_trampoline(app_ptr, binding_ptr, r, p, n);
                 });
  a->routes.push_back(b);
  return UVCPP_C_OK;
}

void route_trampoline(uvcpp_c_app* app, const route_binding* b,
                      uv::uvcpp_web_request& req, uv::uvcpp_web_response& resp,
                      uv::uvcpp_web_next next) {
  callback_frame frame;
  frame.req = &req;
  frame.resp = &resp;
  frame.app  = app;

  uvcpp_c_req  creq;
  uvcpp_c_resp cresp;
  uvcpp_c_next cnext;
  creq.req = &req;
  creq.app = app;
  cresp.resp = &resp;
  cresp.app  = app;
  cnext.next  = &next;
  cnext.frame = &frame;
  cnext.app   = app;
  cnext.used  = 0;

  {
    FrameScope scope;
    scope.add(&creq, UVCPP_C_MAGIC_REQ);
    scope.add(&cresp, UVCPP_C_MAGIC_RESP);
    scope.add(&cnext, UVCPP_C_MAGIC_NEXT);

    CallbackScope guard(app);
    b->cb(b->user_data, &creq, &cresp, &cnext);
  }
  // 三枚句柄到这里已经毒化。用户回调里 `defer()` 拿走的那份副本是**另外一份**
  // （`uvcpp_c_deferred` 里那一枚），不受影响。
}

// ---------------------------------------------------------------------------
// WebSocket 跳板
// ---------------------------------------------------------------------------

struct ws_binding : binding_base {
  uvcpp_c_ws_cb cb;
  void*         user_data;

  ws_binding() : cb(nullptr), user_data(nullptr) {}
};

void ws_trampoline(uvcpp_c_app* app, const ws_binding* b,
                   uv::uvcpp_web_ws_request& wsreq) {
  uvcpp_c_ws_req creq;
  creq.ws  = &wsreq;
  creq.app = app;
  uv::uvcpp_ws_connection* c = wsreq.connection();
  creq.conn.conn   = c;
  creq.conn.app    = app;
  creq.has_conn    = (c != nullptr) ? 1 : 0;

  FrameScope scope;
  scope.add(&creq, UVCPP_C_MAGIC_WS_REQ);
  if (creq.has_conn) scope.add(&creq.conn, UVCPP_C_MAGIC_WS_CONN);

  CallbackScope guard(app);
  b->cb(b->user_data, &creq);
}

/**
 * @brief 在一条 WS 事件回调里临时造一枚连接句柄，跑完就毒化。
 *
 * 这就是"连接句柄是回调期句柄"那句承诺的机制 —— 本文件里没有任何一条路径会把
 * 连接句柄存下来（存下来就是悬垂，因为框架没有把会话销毁暴露到 C 面）。
 */
class WsConnScope {
 public:
  WsConnScope(uvcpp_c_app* a, uv::uvcpp_ws_connection* c) {
    h_.conn = c;
    h_.app  = a;
    register_head(&h_, UVCPP_C_MAGIC_WS_CONN);
    arm_loop_thread(&h_);
  }
  ~WsConnScope() { unregister_head(&h_); }
  uvcpp_c_ws_conn* get() { return &h_; }
  WsConnScope(const WsConnScope&) = delete;
  WsConnScope& operator=(const WsConnScope&) = delete;

 private:
  uvcpp_c_ws_conn h_;
};

/// 事件表里"这一格调用方给没给"——按 `size` 逐字段看（规矩 3）。
struct ws_table_view {
  uvcpp_c_ws_events ev;  ///< 按值拷出来的一份
  int has_text;
  int has_binary;
  int has_close;
  int has_error;
};

bool read_ws_table(const uvcpp_c_ws_events* src, ws_table_view* out) {
  if (src == nullptr) return false;
  if (!table_size_ok(src->size)) return false;

  out->ev = uvcpp_c_ws_events();
  out->ev.size = static_cast<uint32_t>(sizeof(uvcpp_c_ws_events));
  out->has_text = out->has_binary = out->has_close = out->has_error = 0;

  if (field_present(src->size, &uvcpp_c_ws_events::on_text)) {
    out->ev.on_text = src->on_text;
    out->has_text   = (src->on_text != nullptr) ? 1 : 0;
  }
  if (field_present(src->size, &uvcpp_c_ws_events::on_binary)) {
    out->ev.on_binary = src->on_binary;
    out->has_binary   = (src->on_binary != nullptr) ? 1 : 0;
  }
  if (field_present(src->size, &uvcpp_c_ws_events::on_close)) {
    out->ev.on_close = src->on_close;
    out->has_close   = (src->on_close != nullptr) ? 1 : 0;
  }
  if (field_present(src->size, &uvcpp_c_ws_events::on_error)) {
    out->ev.on_error = src->on_error;
    out->has_error   = (src->on_error != nullptr) ? 1 : 0;
  }
  return true;
}

}  // namespace

// ===========================================================================
// app：建与废
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_app* uvcpp_c_app_new(void) {
  UVCPP_C_TRY
    uvcpp_c_app* h = new (std::nothrow) uvcpp_c_app();
    if (h == nullptr) return nullptr;
    h->app = new (std::nothrow) uv::uvcpp_web_app();
    if (h->app == nullptr) {
      delete h;
      return nullptr;
    }
    h->cb_depth = 0;
    h->started  = 0;
    register_head(h, UVCPP_C_MAGIC_APP);
    return h;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_app_free(uvcpp_c_app* app) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP)) return UVCPP_C_E_STALE;
    if (app->cb_depth > 0) return UVCPP_C_E_STATE;  // 回调里不许自毁，见文件开头

    // 还没停就替调用方收尾一次：删一个正在跑的 app 会炸在框架内部，
    // 而"忘了 stop"是这个 API 最可能被犯的错。
    if (app->started && app->app != nullptr) {
      app->app->stop();
      app->app->join();
    }

    unregister_head(app);
    delete app->app;  // 路由表（含我们那些闭包）在这里销毁
    app->app = nullptr;
    for (size_t i = 0; i < app->routes.size(); ++i) delete app->routes[i];
    app->routes.clear();
    delete app;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// app：配置
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_app_set_host(uvcpp_c_app* app,
                                                const char* host) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_host(host != nullptr ? std::string(host) : std::string());
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_port(uvcpp_c_app* app, int port) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    if (port < 0 || port > 65535) return UVCPP_C_E_INVALID_ARG;
    app->app->set_port(port);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_backlog(uvcpp_c_app* app,
                                                   int backlog) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_backlog(backlog);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_max_body_size(uvcpp_c_app* app,
                                                         size_t bytes) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_max_body_size(bytes);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_max_header_bytes(uvcpp_c_app* app,
                                                            size_t bytes) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_max_header_bytes(bytes);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_max_url_bytes(uvcpp_c_app* app,
                                                         size_t bytes) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_max_url_bytes(bytes);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_compression(uvcpp_c_app* app,
                                                       int enable) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_compression(enable != 0);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_access_log(uvcpp_c_app* app,
                                                      int enable) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_access_log(enable != 0);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_server_header(uvcpp_c_app* app,
                                                         const char* value) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_server_header(value != nullptr ? std::string(value)
                                                 : std::string());
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_shutdown_grace_ms(uvcpp_c_app* app,
                                                             int ms) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_shutdown_grace_ms(ms);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_idle_timeout_ms(uvcpp_c_app* app,
                                                           int ms) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_idle_timeout_ms(ms);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_auto_options(uvcpp_c_app* app,
                                                        int enable) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_auto_options(enable != 0);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_head_as_get(uvcpp_c_app* app,
                                                       int enable) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_head_as_get(enable != 0);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_max_pipelined_requests(
    uvcpp_c_app* app, size_t n) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_max_pipelined_requests(n);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_upload_dir(uvcpp_c_app* app,
                                                      const char* dir) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    if (dir == nullptr) return UVCPP_C_E_INVALID_ARG;
    app->app->set_upload_dir(std::string(dir));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_max_upload_size(uvcpp_c_app* app,
                                                           uint64_t bytes) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_max_upload_size(bytes);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_max_file_size(uvcpp_c_app* app,
                                                         uint64_t bytes) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->set_max_file_size(bytes);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_set_loops(uvcpp_c_app* app, int n) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    // `set_loops()` 会给负数/越界/装晚了返回 libuv 的错误码，原样透传
    // —— 那比翻译成我们自己的码信息量大（`UV_EBUSY` 就是"装晚了"）。
    return app->app->set_loops(n);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_loop_count(uvcpp_c_app* app) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    return app->app->loop_count();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// app：路由与中间件
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_app_get(uvcpp_c_app* app, const char* pattern,
                                           uvcpp_c_route_cb cb,
                                           void* user_data) {
  UVCPP_C_TRY
    return add_route(app, &uv::uvcpp_web_app::get, pattern, cb, user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_post(uvcpp_c_app* app,
                                            const char* pattern,
                                            uvcpp_c_route_cb cb,
                                            void* user_data) {
  UVCPP_C_TRY
    return add_route(app, &uv::uvcpp_web_app::post, pattern, cb, user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_put(uvcpp_c_app* app, const char* pattern,
                                           uvcpp_c_route_cb cb,
                                           void* user_data) {
  UVCPP_C_TRY
    return add_route(app, &uv::uvcpp_web_app::put, pattern, cb, user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_del(uvcpp_c_app* app, const char* pattern,
                                           uvcpp_c_route_cb cb,
                                           void* user_data) {
  UVCPP_C_TRY
    return add_route(app, &uv::uvcpp_web_app::del, pattern, cb, user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_patch(uvcpp_c_app* app,
                                             const char* pattern,
                                             uvcpp_c_route_cb cb,
                                             void* user_data) {
  UVCPP_C_TRY
    return add_route(app, &uv::uvcpp_web_app::patch, pattern, cb, user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_head(uvcpp_c_app* app,
                                            const char* pattern,
                                            uvcpp_c_route_cb cb,
                                            void* user_data) {
  UVCPP_C_TRY
    return add_route(app, &uv::uvcpp_web_app::head, pattern, cb, user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_options(uvcpp_c_app* app,
                                               const char* pattern,
                                               uvcpp_c_route_cb cb,
                                               void* user_data) {
  UVCPP_C_TRY
    return add_route(app, &uv::uvcpp_web_app::options, pattern, cb, user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_any(uvcpp_c_app* app,
                                           const char* pattern,
                                           uvcpp_c_route_cb cb,
                                           void* user_data) {
  UVCPP_C_TRY
    return add_route(app, &uv::uvcpp_web_app::any, pattern, cb, user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_use(uvcpp_c_app* app,
                                           uvcpp_c_route_cb cb,
                                           void* user_data) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    if (cb == nullptr) return UVCPP_C_E_INVALID_ARG;

    route_binding* b = new (std::nothrow) route_binding();
    if (b == nullptr) return UVCPP_C_E_NO_MEMORY;
    b->cb        = cb;
    b->user_data = user_data;

    uvcpp_c_app*   app_ptr     = app;
    route_binding* binding_ptr = b;
    app->app->use([app_ptr, binding_ptr](uv::uvcpp_web_request& r,
                                         uv::uvcpp_web_response& p,
                                         uv::uvcpp_web_next n) {
      route_trampoline(app_ptr, binding_ptr, r, p, n);
    });
    app->routes.push_back(b);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_post_upload(uvcpp_c_app* app,
                                                   const char* pattern,
                                                   uvcpp_c_route_cb cb,
                                                   void* user_data) {
  UVCPP_C_TRY
    return add_route(app, &uv::uvcpp_web_app::post_upload, pattern, cb,
                     user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_serve_static(
    uvcpp_c_app* app, const char* prefix, const char* root_dir,
    const uvcpp_c_static_options* options) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    if (prefix == nullptr || root_dir == nullptr) return UVCPP_C_E_INVALID_ARG;

    // 先按 C++ 的默认值造一份，再把调用方真给了的格子盖上去 —— 于是"没给"
    // 就真的是"用默认值"，而不是"用 0"（0 在好几个格子上是另一个意思）。
    uv::uvcpp_web_static_options o;
    if (options != nullptr) {
      if (!table_size_ok(options->size)) return UVCPP_C_E_INVALID_ARG;

      if (field_present(options->size, &uvcpp_c_static_options::index_files)) {
        o.index_files.clear();
        const char* list = options->index_files;
        if (list != nullptr) {
          std::string cur;
          for (const char* p = list;; ++p) {
            if (*p == ',' || *p == '\0') {
              // 空段跳过（"a,,b" 与尾逗号都不该造出一个空文件名来）。
              if (!cur.empty()) o.index_files.push_back(cur);
              cur.clear();
              if (*p == '\0') break;
            } else {
              cur.push_back(*p);
            }
          }
        }
      }
      if (field_present(options->size, &uvcpp_c_static_options::spa_fallback)) {
        o.spa_fallback = (options->spa_fallback != 0);
      }
      if (field_present(options->size, &uvcpp_c_static_options::spa_file) &&
          options->spa_file != nullptr) {
        o.spa_file = options->spa_file;
      }
      if (field_present(options->size, &uvcpp_c_static_options::max_file_size) &&
          options->max_file_size != 0) {
        o.max_file_size = options->max_file_size;
      }
      if (field_present(options->size, &uvcpp_c_static_options::etag)) {
        o.etag = (options->etag != 0);
      }
      if (field_present(options->size, &uvcpp_c_static_options::last_modified)) {
        o.last_modified = (options->last_modified != 0);
      }
      if (field_present(options->size, &uvcpp_c_static_options::range)) {
        o.range = (options->range != 0);
      }
      if (field_present(options->size, &uvcpp_c_static_options::cache_control) &&
          options->cache_control != nullptr) {
        o.cache_control = options->cache_control;
      }
      if (field_present(options->size,
                        &uvcpp_c_static_options::cache_max_entries)) {
        o.cache_max_entries = options->cache_max_entries;
      }
      if (field_present(options->size, &uvcpp_c_static_options::cache_max_bytes)) {
        o.cache_max_bytes = options->cache_max_bytes;
      }
      if (field_present(options->size, &uvcpp_c_static_options::dotfiles)) {
        switch (options->dotfiles) {
          case UVCPP_C_DOTFILE_HIDE:
            o.dotfiles = uv::uvcpp_web_dotfile_policy::HIDE;
            break;
          case UVCPP_C_DOTFILE_DENY:
            o.dotfiles = uv::uvcpp_web_dotfile_policy::DENY;
            break;
          case UVCPP_C_DOTFILE_ALLOW:
            o.dotfiles = uv::uvcpp_web_dotfile_policy::ALLOW;
            break;
          default:
            return UVCPP_C_E_INVALID_ARG;  // 不认识的策略不猜
        }
      }
      if (field_present(options->size,
                        &uvcpp_c_static_options::follow_symlinks)) {
        o.follow_symlinks = (options->follow_symlinks != 0);
      }
      if (field_present(options->size, &uvcpp_c_static_options::add_charset)) {
        o.add_charset = (options->add_charset != 0);
      }
    }

    // 返回的 shared_ptr 刻意丢掉：路由里的 handler 自己也持有一份，静态服务
    // 会活到 app 结束；而"清缓存 / 读命中统计"是单元测试用的观测口，不属于
    // 对外 ABI（见头里的"不提供"）。
    app->app->serve_static(std::string(prefix), std::string(root_dir), o);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_websocket(uvcpp_c_app* app,
                                                 const char* pattern,
                                                 uvcpp_c_ws_cb cb,
                                                 void* user_data) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    if (pattern == nullptr || cb == nullptr) return UVCPP_C_E_INVALID_ARG;

    ws_binding* b = new (std::nothrow) ws_binding();
    if (b == nullptr) return UVCPP_C_E_NO_MEMORY;
    b->cb        = cb;
    b->user_data = user_data;

    uvcpp_c_app* app_ptr     = app;
    ws_binding*  binding_ptr = b;
    app->app->websocket(std::string(pattern),
                        [app_ptr, binding_ptr](uv::uvcpp_web_ws_request& w) {
                          ws_trampoline(app_ptr, binding_ptr, w);
                        });
    // 与路由共用同一张"要删的绑定"表（`binding_base` 那一层虚析构就是为它
    // 存在的）：两者同生共死，都挂在 app 上。
    app->routes.push_back(b);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// app：起停与读数
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_app_start(uvcpp_c_app* app) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    const int rc = app->app->start();
    if (rc == 0) app->started = 1;
    return rc;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_start_background(uvcpp_c_app* app) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    const int rc = app->app->start_background();
    if (rc == 0) app->started = 1;
    return rc;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_stop(uvcpp_c_app* app) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->stop();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_join(uvcpp_c_app* app) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    app->app->join();
    app->started = 0;  // 已经收尾，再 free 就不必重复 stop/join
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_bound_port(uvcpp_c_app* app) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    return app->app->bound_port();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_app_running(uvcpp_c_app* app) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    return app->app->running() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API size_t uvcpp_c_app_connection_count(uvcpp_c_app* app) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr) return 0;
    return app->app->connection_count();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API size_t uvcpp_c_app_connection_count_at(uvcpp_c_app* app,
                                                              int loop_index) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr) return 0;
    return app->app->connection_count_at(loop_index);
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API size_t uvcpp_c_app_inflight_count(uvcpp_c_app* app) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr) return 0;
    return app->app->inflight_count();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API size_t uvcpp_c_app_ws_session_count(uvcpp_c_app* app) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr) return 0;
    return app->app->ws_session_count();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_app_post_task(uvcpp_c_app* app,
                                                 uvcpp_c_void_cb cb,
                                                 void* user_data) {
  UVCPP_C_TRY
    if (!alive(app, UVCPP_C_MAGIC_APP) || app->app == nullptr)
      return UVCPP_C_E_STALE;
    if (cb == nullptr) return UVCPP_C_E_INVALID_ARG;
    // 多循环下 `post(fn)` 从非循环线程投的是 0 号 —— 那是**错**的那条循环，
    // 所以这里如实拒绝，不假装。理由见头文件。
    if (app->app->loop_count() > 1) return UVCPP_C_E_UNSUPPORTED;

    uvcpp_c_app* app_ptr = app;
    app->app->post([app_ptr, cb, user_data]() {
      CallbackScope guard(app_ptr);
      cb(user_data);
    });
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// req：读请求
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_req_method_name(uvcpp_c_req* req, char* buf,
                                                   size_t cap) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    const char* m = req->req->method_name();
    return copy_out(m != nullptr ? std::string(m) : std::string(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_path(uvcpp_c_req* req, char* buf,
                                            size_t cap) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    return copy_out(req->req->path(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_raw_path(uvcpp_c_req* req, char* buf,
                                                size_t cap) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    return copy_out(req->req->raw_path(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_query_string(uvcpp_c_req* req, char* buf,
                                                    size_t cap) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    return copy_out(req->req->query_string(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_query(uvcpp_c_req* req, const char* name,
                                             char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    const std::string* v = req->req->query(name);
    // 查不到给 `E_NOT_FOUND`，**不是** 0：0 是"查到了、值是空串"。
    if (v == nullptr) return UVCPP_C_E_NOT_FOUND;
    return copy_out(*v, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_header(uvcpp_c_req* req, const char* name,
                                              char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!req->req->has_header(name)) return UVCPP_C_E_NOT_FOUND;
    return copy_out(req->req->header(name), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_has_header(uvcpp_c_req* req,
                                                  const char* name) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    return req->req->has_header(name) ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_content_type(uvcpp_c_req* req, char* buf,
                                                    size_t cap) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    return copy_out(req->req->content_type(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API size_t uvcpp_c_req_content_length(uvcpp_c_req* req) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr) return 0;
    if (!thread_ok(req)) return 0;
    return req->req->content_length();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_req_host(uvcpp_c_req* req, char* buf,
                                            size_t cap) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    return copy_out(req->req->host(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_peer_ip(uvcpp_c_req* req, char* buf,
                                               size_t cap) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    return copy_out(req->req->peer_ip(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API unsigned int uvcpp_c_req_peer_port(uvcpp_c_req* req) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr) return 0;
    if (!thread_ok(req)) return 0;
    return req->req->peer_port();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_req_param(uvcpp_c_req* req, const char* name,
                                             char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    const std::string* v = req->req->param(name);
    if (v == nullptr) return UVCPP_C_E_NOT_FOUND;
    return copy_out(*v, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_cookie(uvcpp_c_req* req, const char* name,
                                              char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    const std::string* v = req->req->cookie(name);
    if (v == nullptr) return UVCPP_C_E_NOT_FOUND;
    return copy_out(*v, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_is_keep_alive(uvcpp_c_req* req) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    return req->req->is_keep_alive() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_body(uvcpp_c_req* req, const char** data,
                                            size_t* len) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    if (data == nullptr || len == nullptr) return UVCPP_C_E_INVALID_ARG;
    *data = req->req->body_data();
    *len  = req->req->body_size();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_req_body_empty(uvcpp_c_req* req) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_REQ) || req->req == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;
    return req->req->body_empty() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// resp：写响应
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_resp_status(uvcpp_c_resp* resp, int code) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->status(code);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_status_message(uvcpp_c_resp* resp,
                                                       const char* msg) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (msg == nullptr) return UVCPP_C_E_INVALID_ARG;
    resp->resp->status_message(std::string(msg));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_status_code(uvcpp_c_resp* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    return resp->resp->status_code();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_set_header(uvcpp_c_resp* resp,
                                                   const char* name,
                                                   const char* value) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    resp->resp->set_header(name, value != nullptr ? value : "");
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_add_header(uvcpp_c_resp* resp,
                                                   const char* name,
                                                   const char* value) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    resp->resp->add_header(name, value != nullptr ? value : "");
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_remove_header(uvcpp_c_resp* resp,
                                                      const char* name) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    resp->resp->remove_header(name);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_has_header(uvcpp_c_resp* resp,
                                                   const char* name) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    return resp->resp->has_header(name) ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_get_header(uvcpp_c_resp* resp,
                                                   const char* name, char* buf,
                                                   size_t cap) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    std::string v = resp->resp->get_header(name);  // 没有时给的是空串
    // 与请求头同一条规矩：**没有**这个头与"有、但值是空串"要分得开。
    if (!resp->resp->has_header(name)) return UVCPP_C_E_NOT_FOUND;
    return copy_out(v, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_set_content_type(uvcpp_c_resp* resp,
                                                         const char* ct) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (ct == nullptr) return UVCPP_C_E_INVALID_ARG;
    resp->resp->set_content_type(std::string(ct));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_content_type(uvcpp_c_resp* resp,
                                                     char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    return copy_out(resp->resp->content_type(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_set_cookie(
    uvcpp_c_resp* resp, const char* name, const char* value, const char* path,
    long max_age, int http_only, int secure, const char* same_site) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr || value == nullptr) return UVCPP_C_E_INVALID_ARG;
    resp->resp->set_cookie(
        std::string(name), std::string(value),
        path != nullptr ? std::string(path) : std::string("/"), max_age,
        http_only != 0, secure != 0,
        same_site != nullptr ? std::string(same_site) : std::string("Lax"));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_body(uvcpp_c_resp* resp,
                                             const void* data, size_t len,
                                             const char* content_type) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->body(static_cast<const char*>(data), len, content_type);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_text(uvcpp_c_resp* resp,
                                             const char* s) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->text(s != nullptr ? std::string(s) : std::string());
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_html(uvcpp_c_resp* resp,
                                             const char* s) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->html(s != nullptr ? std::string(s) : std::string());
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_json_str(uvcpp_c_resp* resp,
                                                 const char* json) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->json_str(json != nullptr ? std::string(json) : std::string());
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_binary(uvcpp_c_resp* resp,
                                               const void* data, size_t len,
                                               const char* content_type) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->binary(
        static_cast<const char*>(data), len,
        content_type != nullptr ? std::string(content_type)
                                : std::string("application/octet-stream"));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API size_t uvcpp_c_resp_body_size(uvcpp_c_resp* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr) return 0;
    if (!thread_ok(resp)) return 0;
    return resp->resp->body_size();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_clear_body(uvcpp_c_resp* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->clear_body();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_redirect(uvcpp_c_resp* resp,
                                                 const char* url, int code) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (url == nullptr) return UVCPP_C_E_INVALID_ARG;
    resp->resp->redirect(std::string(url), code == 0 ? 302 : code);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_not_found(uvcpp_c_resp* resp,
                                                  const char* what) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->not_found();
    if (what != nullptr) resp->resp->text(std::string(what));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_bad_request(uvcpp_c_resp* resp,
                                                    const char* what) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->bad_request();
    if (what != nullptr) resp->resp->text(std::string(what));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_forbidden(uvcpp_c_resp* resp,
                                                  const char* what) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->forbidden();
    if (what != nullptr) resp->resp->text(std::string(what));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_payload_too_large(uvcpp_c_resp* resp,
                                                          const char* what) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->payload_too_large();
    if (what != nullptr) resp->resp->text(std::string(what));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_server_error(uvcpp_c_resp* resp,
                                                     const char* what) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->server_error();
    if (what != nullptr) resp->resp->text(std::string(what));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_end(uvcpp_c_resp* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (resp->resp->ended()) return UVCPP_C_E_STATE;  // 恰好一次
    resp->resp->end();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_ended(uvcpp_c_resp* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    return resp->resp->ended() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_on_sent(uvcpp_c_resp* resp,
                                                uvcpp_c_sent_cb cb,
                                                void* user_data) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (cb == nullptr) return UVCPP_C_E_INVALID_ARG;
    // **按值捕获 cb / user_data**：这个回调在本文件那些句柄毒化之后才可能被
    // 框架调用，捕获句柄地址就是用一块已经出栈的栈内存。见文件开头第 3 条。
    resp->resp->on_sent([cb, user_data](const uv::uvcpp_web_sent_info& info) {
      uvcpp_c_sent_info out;
      fill_sent_info(info, &out);
      cb(user_data, &out);
    });
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_begin_chunked(uvcpp_c_resp* resp,
                                                      const char* content_type) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    resp->resp->begin_chunked(
        content_type != nullptr ? std::string(content_type)
                                : std::string("text/plain; charset=utf-8"));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_write_chunk(uvcpp_c_resp* resp,
                                                    const void* data,
                                                    size_t len) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    const bool ok =
        resp->resp->write_chunk(static_cast<const char*>(data), len);
    // `write_chunk` 的 `false` **不是失败**（数据已经收下了），它说的是"缓冲
    // 到了高水位，该等 `on_drain` 再写"。所以映射成 1 而不是错误码 ——
    // 本层"非负 = 成功"那条约定正好装得下这个信息。
    return ok ? UVCPP_C_OK : 1;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_on_drain(uvcpp_c_resp* resp,
                                                 uvcpp_c_void_cb cb,
                                                 void* user_data) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (cb == nullptr) return UVCPP_C_E_INVALID_ARG;
    resp->resp->on_drain([cb, user_data]() { cb(user_data); });
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_streaming(uvcpp_c_resp* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    return resp->resp->streaming() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API uint64_t
uvcpp_c_resp_stream_bytes_written(uvcpp_c_resp* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr) return 0;
    if (!thread_ok(resp)) return 0;
    return static_cast<uint64_t>(resp->resp->stream_bytes_written());
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_send_file(
    uvcpp_c_resp* resp, const char* path,
    void (*done)(void* user_data, int status, uint64_t bytes_sent),
    void* user_data) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (path == nullptr) return UVCPP_C_E_INVALID_ARG;
    // 同上：按值捕获，绝不捕获 resp 句柄。
    resp->resp->send_file(
        std::string(path),
        [done, user_data](int status, uint64_t bytes) {
          if (done != nullptr) done(user_data, status, bytes);
        });
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_resp_send_file_range(
    uvcpp_c_resp* resp, const char* path, uint64_t first, uint64_t last,
    void (*done)(void* user_data, int status, uint64_t bytes_sent),
    void* user_data) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (path == nullptr) return UVCPP_C_E_INVALID_ARG;
    resp->resp->send_file_range(
        std::string(path), first, last,
        [done, user_data](int status, uint64_t bytes) {
          if (done != nullptr) done(user_data, status, bytes);
        });
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// next：继续走链
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_next_run(uvcpp_c_next* next) {
  UVCPP_C_TRY
    if (!alive(next, UVCPP_C_MAGIC_NEXT) || next->next == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(next)) return UVCPP_C_E_WRONG_THREAD;
    if (next->used) return UVCPP_C_E_STATE;  // 一枚 next 只能用一次
    next->used = 1;
    (*next->next)();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// deferred：延迟应答
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_deferred* uvcpp_c_defer(uvcpp_c_next* next) {
  UVCPP_C_TRY
    if (!alive(next, UVCPP_C_MAGIC_NEXT) || next->next == nullptr) return nullptr;
    if (next->used) return nullptr;
    if (next->frame == nullptr || next->frame->req == nullptr ||
        next->frame->resp == nullptr) {
      return nullptr;
    }
    next->used = 1;

    uvcpp_c_deferred* d = new (std::nothrow) uvcpp_c_deferred();
    if (d == nullptr) return nullptr;
    // **堆上的那一份副本就是"链挂起"本身**：它持着上下文的 shared_ptr，
    // 框架在 handler 返回时量到的引用计数差就是它。
    d->held = new (std::nothrow) uv::uvcpp_web_next(*next->next);
    if (d->held == nullptr) {
      delete d;
      return nullptr;
    }
    d->app     = next->app;
    d->resumed = 0;

    register_head(d, UVCPP_C_MAGIC_DEFERRED);
    // 内嵌的两枚句柄要**各自登记**：`_deferred_req()` 返回的就是它们的地址，
    // 而 `alive()` 只认表里有的地址。
    d->req_handle.req = next->frame->req;
    d->req_handle.app = d->app;
    register_head(&d->req_handle, UVCPP_C_MAGIC_REQ);
    arm_loop_thread(&d->req_handle);
    d->resp_handle.resp = next->frame->resp;
    d->resp_handle.app  = d->app;
    register_head(&d->resp_handle, UVCPP_C_MAGIC_RESP);
    arm_loop_thread(&d->resp_handle);

    return d;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API uvcpp_c_req* uvcpp_c_deferred_req(
    uvcpp_c_deferred* deferred) {
  UVCPP_C_TRY
    if (!alive(deferred, UVCPP_C_MAGIC_DEFERRED)) return nullptr;
    // 线程规则**没有例外**：这枚句柄指向的还是上下文里那个按值持有的请求，
    // 只能在那条循环的线程上碰。检查落在内嵌那枚句柄自己的记录上。
    if (!thread_ok(&deferred->req_handle)) return nullptr;
    return &deferred->req_handle;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API uvcpp_c_resp* uvcpp_c_deferred_resp(
    uvcpp_c_deferred* deferred) {
  UVCPP_C_TRY
    if (!alive(deferred, UVCPP_C_MAGIC_DEFERRED)) return nullptr;
    if (!thread_ok(&deferred->resp_handle)) return nullptr;
    return &deferred->resp_handle;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_deferred_post(uvcpp_c_deferred* deferred,
                                                 uvcpp_c_deferred_cb cb,
                                                 void* user_data) {
  UVCPP_C_TRY
    if (!alive(deferred, UVCPP_C_MAGIC_DEFERRED)) return UVCPP_C_E_STALE;
    if (cb == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (deferred->resumed) return UVCPP_C_E_STATE;
    if (deferred->app == nullptr || deferred->app->app == nullptr)
      return UVCPP_C_E_STALE;
    // 多循环下 C 面没有"这条请求在哪号循环上"，`post(fn)` 会落到 0 号 ——
    // 那是**错**的那条。如实拒绝，见头文件。
    if (deferred->app->app->loop_count() > 1) return UVCPP_C_E_UNSUPPORTED;

    uvcpp_c_deferred* d = deferred;
    deferred->app->app->post([d, cb, user_data]() {
      CallbackScope guard(d->app);
      cb(user_data, d);
    });
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_deferred_resume(
    uvcpp_c_deferred* deferred) {
  UVCPP_C_TRY
    if (!alive(deferred, UVCPP_C_MAGIC_DEFERRED)) return UVCPP_C_E_STALE;
    if (deferred->resumed) return UVCPP_C_E_STATE;
    deferred->resumed = 1;
    // 把凭据**取走**再调（`swap` 不是再拷一份）：于是"链继续"与"凭据归还"
    // 是同一件事，本对象不再持有上下文的一份引用，不会留下引用环。
    uv::uvcpp_web_next held;
    held.swap(*deferred->held);
    held();  // 不在循环线程上时，框架自己会把它投回**正确的那条**循环
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_deferred_free(uvcpp_c_deferred* deferred) {
  UVCPP_C_TRY
    if (!alive(deferred, UVCPP_C_MAGIC_DEFERRED)) return UVCPP_C_E_STALE;
    unregister_head(&deferred->resp_handle);
    unregister_head(&deferred->req_handle);
    unregister_head(deferred);
    // `held` 若是"放弃"（没 resume）那条路，这里就是最后一次引用被丢掉 ——
    // 上下文**不会**因此析构：框架的在途表还持着一份。于是这条请求一直挂着
    // （头里写明了那是泄漏，不是崩溃）。
    delete deferred;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// WebSocket：请求与连接
// ===========================================================================

namespace {

/// 一条 WS 跳板的公共前段（每个 `uvcpp_c_ws_req_*` 都从这里过一遍）。
#define UVCPP_C_WS_REQ_GUARD(req)                                            \
  if (!alive((req), UVCPP_C_MAGIC_WS_REQ) || (req)->ws == nullptr)           \
  return UVCPP_C_E_STALE;                                                    \
  if (!thread_ok(req)) return UVCPP_C_E_WRONG_THREAD;

#define UVCPP_C_WS_CONN_GUARD(conn)                                          \
  if (!alive((conn), UVCPP_C_MAGIC_WS_CONN) || (conn)->conn == nullptr)      \
  return UVCPP_C_E_STALE;                                                    \
  if (!thread_ok(conn)) return UVCPP_C_E_WRONG_THREAD;

}  // namespace

extern "C" UVCPP_C_API int uvcpp_c_ws_req_path(uvcpp_c_ws_req* req, char* buf,
                                               size_t cap) {
  UVCPP_C_TRY
    UVCPP_C_WS_REQ_GUARD(req)
    return copy_out(req->ws->path(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_req_route(uvcpp_c_ws_req* req, char* buf,
                                                size_t cap) {
  UVCPP_C_TRY
    UVCPP_C_WS_REQ_GUARD(req)
    return copy_out(req->ws->route(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_req_param(uvcpp_c_ws_req* req,
                                                const char* name, char* buf,
                                                size_t cap) {
  UVCPP_C_TRY
    UVCPP_C_WS_REQ_GUARD(req)
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    const std::string* v = req->ws->param(name);
    if (v == nullptr) return UVCPP_C_E_NOT_FOUND;
    return copy_out(*v, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_req_query(uvcpp_c_ws_req* req,
                                                const char* name, char* buf,
                                                size_t cap) {
  UVCPP_C_TRY
    UVCPP_C_WS_REQ_GUARD(req)
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    const std::string* v = req->ws->query(name);
    if (v == nullptr) return UVCPP_C_E_NOT_FOUND;
    return copy_out(*v, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_req_cookie(uvcpp_c_ws_req* req,
                                                 const char* name, char* buf,
                                                 size_t cap) {
  UVCPP_C_TRY
    UVCPP_C_WS_REQ_GUARD(req)
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    const std::string* v = req->ws->cookie(name);
    if (v == nullptr) return UVCPP_C_E_NOT_FOUND;
    return copy_out(*v, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_req_header(uvcpp_c_ws_req* req,
                                                 const char* name, char* buf,
                                                 size_t cap) {
  UVCPP_C_TRY
    UVCPP_C_WS_REQ_GUARD(req)
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!req->ws->request().has_header(name)) return UVCPP_C_E_NOT_FOUND;
    return copy_out(req->ws->header(name), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_req_peer_ip(uvcpp_c_ws_req* req, char* buf,
                                                  size_t cap) {
  UVCPP_C_TRY
    UVCPP_C_WS_REQ_GUARD(req)
    return copy_out(req->ws->peer_ip(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API uvcpp_c_ws_conn* uvcpp_c_ws_req_conn(
    uvcpp_c_ws_req* req) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_WS_REQ)) return nullptr;
    if (!thread_ok(req)) return nullptr;
    if (!req->has_conn) return nullptr;
    // 返回的就是升级回调里那一枚（同一层栈上、已经登记过的），所以它的生死
    // 与本次回调一致 —— 与头里"回调期句柄"那句是同一条。
    return &req->conn;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_conn_set_events(
    uvcpp_c_ws_conn* conn, const uvcpp_c_ws_events* events, void* user_data) {
  UVCPP_C_TRY
    UVCPP_C_WS_CONN_GUARD(conn)
    if (events == nullptr) return UVCPP_C_E_INVALID_ARG;

    ws_table_view view;
    // `read_ws_table()` 里就是规矩 3 的那套"逐字段看 size"。
    if (!read_ws_table(events, &view)) return UVCPP_C_E_INVALID_ARG;

    uv::uvcpp_ws_connection* c = conn->conn;
    uvcpp_c_app*             a = conn->app;

    // 四枚回调都可能在本文件那些句柄毒化之后才被框架调用，所以它们捕获的是
    // **表的一份拷贝 + 连接指针**（连接指针安全：这些闭包就存在那个连接对象
    // 里，对象不在了它们也没人会调）。句柄本身在回调里**临时造**一枚，
    // 见 `WsConnScope`。
    if (view.has_text) {
      const uvcpp_c_ws_events ev = view.ev;
      c->on_text([a, c, ev, user_data](const std::string& s) {
        WsConnScope scope(a, c);
        CallbackScope guard(a);
        ev.on_text(user_data, scope.get(), s.data(), s.size());
      });
    }
    if (view.has_binary) {
      const uvcpp_c_ws_events ev = view.ev;
      c->on_binary([a, c, ev, user_data](const uint8_t* d, size_t n) {
        WsConnScope scope(a, c);
        CallbackScope guard(a);
        ev.on_binary(user_data, scope.get(), reinterpret_cast<const char*>(d), n);
      });
    }
    if (view.has_close) {
      const uvcpp_c_ws_events ev = view.ev;
      c->on_close([a, c, ev, user_data](uv::ws_close_code code,
                                        const std::string& reason) {
        WsConnScope scope(a, c);
        CallbackScope guard(a);
        ev.on_close(user_data, scope.get(), static_cast<int>(code),
                    reason.data(), reason.size());
      });
    }
    if (view.has_error) {
      const uvcpp_c_ws_events ev = view.ev;
      c->on_error([a, c, ev, user_data](int status, const std::string& what) {
        WsConnScope scope(a, c);
        CallbackScope guard(a);
        ev.on_error(user_data, scope.get(), status, what.data(), what.size());
      });
    }
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_conn_send_text(uvcpp_c_ws_conn* conn,
                                                     const char* data,
                                                     size_t len) {
  UVCPP_C_TRY
    UVCPP_C_WS_CONN_GUARD(conn)
    return conn->conn->send_text(data, len);  // int 原样透传（0 = 已入队）
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_conn_send_binary(uvcpp_c_ws_conn* conn,
                                                       const char* data,
                                                       size_t len) {
  UVCPP_C_TRY
    UVCPP_C_WS_CONN_GUARD(conn)
    return conn->conn->send_binary(data, len);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_conn_send_ping(uvcpp_c_ws_conn* conn) {
  UVCPP_C_TRY
    UVCPP_C_WS_CONN_GUARD(conn)
    return conn->conn->send_ping();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_conn_send_pong(uvcpp_c_ws_conn* conn) {
  UVCPP_C_TRY
    UVCPP_C_WS_CONN_GUARD(conn)
    return conn->conn->send_pong();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_conn_close(uvcpp_c_ws_conn* conn, int code,
                                                 const char* reason,
                                                 size_t reason_len) {
  UVCPP_C_TRY
    UVCPP_C_WS_CONN_GUARD(conn)
    const std::string r =
        (reason != nullptr && reason_len > 0) ? std::string(reason, reason_len)
                                              : std::string();
    conn->conn->close(static_cast<uv::ws_close_code>(code), r);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_conn_terminate(uvcpp_c_ws_conn* conn) {
  UVCPP_C_TRY
    UVCPP_C_WS_CONN_GUARD(conn)
    conn->conn->terminate();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_conn_is_open(uvcpp_c_ws_conn* conn) {
  UVCPP_C_TRY
    UVCPP_C_WS_CONN_GUARD(conn)
    return conn->conn->is_open() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_conn_set_max_message_size(
    uvcpp_c_ws_conn* conn, size_t bytes) {
  UVCPP_C_TRY
    UVCPP_C_WS_CONN_GUARD(conn)
    conn->conn->set_max_message_size(bytes);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

#undef UVCPP_C_WS_REQ_GUARD
#undef UVCPP_C_WS_CONN_GUARD
