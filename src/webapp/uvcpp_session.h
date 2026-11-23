/**
 * @file src/webapp/uvcpp_session.h
 * @brief 会话存储 + 安全 Cookie 中间件（webapp）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * auto store = std::make_shared<uvcpp::uvcpp_memory_session_store>();
 * uvcpp::uvcpp_session_options opts;   // sid / 86400s / HttpOnly / SameSite=Lax
 * app.use(uvcpp::uvcpp_session_middleware(store, opts));
 *
 * app.post("/login", [](uvcpp::uvcpp_web_request& req, ...) {
 *   uvcpp::uvcpp_session* s = uvcpp::uvcpp_session_of(req);
 *   if (s) s->set("uid", "42");
 * });
 * app.get("/me", [](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
 *                   uvcpp::uvcpp_web_next) {
 *   uvcpp::uvcpp_session* s = uvcpp::uvcpp_session_of(req);
 *   if (!s || !s->has("uid")) { resp.status(uvcpp::http_status::UNAUTHORIZED); }
 *   resp.end();
 * });
 * ```
 *
 * ## 这一层做了/没做
 *
 * **做了**：可插拔 `uvcpp_session_store`（本文件带内存实现）、安全 Cookie
 * （HttpOnly / Secure / SameSite / Path / Max-Age）、过期与滑动续期、注销、
 * 请求级 `uvcpp_session*` 取用。
 *
 * **共享存储**：同步 store 之外还有 `uvcpp_async_session_store` +
 * `uvcpp_async_session_middleware()` —— 加载完成前挂起链，Redis 适配器是
 * `<webapp/uvcpp_redis_session_store.h>`。密码哈希 / 登录限流 / RBAC / CSRF /
 * 审计在同目录的 `uvcpp_web_auth.h`。MySQL/SQLite 会话适配器与并发登录策略
 * 仍是 #70 的后续。
 *
 * ## 语义
 *
 * - **每个请求都有会话**（首次访问就建），Cookie 只在"新建 / 续期 / 注销"
 *   时下发；不 `set()` 任何东西的空会话也会被存下来（上限由 store 兜底）。
 *   不接受"按需延迟创建"的原因：响应可能已经 `end()`，那时再补 `Set-Cookie`
 *   就晚了。
 * - **滑动续期**（`sliding`，默认开）：每次请求把过期时间推到 now+ttl，并在
 *   响应里重发 Max-Age。关掉就是"从创建时刻起算的绝对过期"。
 * - **注销**：`uvcpp_session_logout()` 删 store 里的记录 + 发一条过期 Cookie；
 *   之后同一 id 再拿来也 load 不到。
 * - **换 id**：会话固定（session fixation）防护由业务在登录成功时调
 *   `uvcpp_session::regenerate()`，旧 id 由中间件在响应结束时删掉。
 * - 只在 loop 线程上访问；store 自身对多线程安全（内存实现内部有锁），
 *   这样将来换 Redis 适配器时中间件不用改。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_SESSION_H
#define SRC_WEBAPP_UVCPP_SESSION_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_WEBAPP_ENABLE

#include <cstddef>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <webapp/uvcpp_web_handler.h>

namespace uvcpp {

class uvcpp_web_request;
class uvcpp_web_response;
class uvcpp_session;
class uvcpp_session_store;
class uvcpp_async_session_store;
struct uvcpp_session_options;

// **这两条前置声明必须带 `UVCPP_API`，并且必须排在类里的 friend 声明之前。**
// MSVC 只认"第一条声明"的 DLL 链接属性：先出现不带属性的 friend、后出现带
// 属性的命名空间级声明会报 C2375（redefinition; different linkage），
// 而把属性直接写在 friend 上又会撞 C2487。GCC/clang 对属性位置不敏感，
// 所以本机看不到。
UVCPP_API uvcpp_web_middleware uvcpp_session_middleware(
    std::shared_ptr<uvcpp_session_store> store,
    const uvcpp_session_options&         opts);
UVCPP_API void uvcpp_session_logout(uvcpp_web_request& req,
                                    uvcpp_web_response& resp,
                                    uvcpp_session_store& store,
                                    const uvcpp_session_options& opts);

// 异步版同理：**第一条声明必须带 `UVCPP_API`**，而且必须排在类里的 friend
// 之前（MSVC 只认第一条声明的链接属性；默认实参放到后面的声明里）。
UVCPP_API uvcpp_web_middleware uvcpp_async_session_middleware(
    std::shared_ptr<uvcpp_async_session_store> store,
    const uvcpp_session_options&               opts);
UVCPP_API void uvcpp_async_session_logout(uvcpp_web_request& req,
                                          uvcpp_web_response& resp,
                                          uvcpp_async_session_store& store,
                                          const uvcpp_session_options& opts);

/**
 * @brief 一份会话数据。**不是**线程安全的；只在 loop 线程上用。
 */
class UVCPP_API uvcpp_session {
 public:
  uvcpp_session();

  /// 会话 id（Cookie 里的值）。注销后为空。
  const std::string& id() const { return id_; }
  bool empty() const { return id_.empty(); }

  bool has(const std::string& key) const;
  /// 取一个值；不存在返回空串（要区分"空串值"用 `has()`）。
  std::string get(const std::string& key,
                  const std::string& def = std::string()) const;
  void set(const std::string& key, const std::string& value);
  void erase(const std::string& key);
  void clear();
  const std::map<std::string, std::string>& data() const { return data_; }

  std::time_t created_at() const { return created_at_; }
  std::time_t expires_at() const { return expires_at_; }
  bool expired(std::time_t now) const { return expires_at_ <= now; }

  /**
   * @brief 换一个新 id（登录成功时防会话固定）。
   *
   * 旧 id 的删除由中间件在本次响应结束时完成；调用后**不要**再信任旧 id。
   */
  void regenerate();

  /// 标记注销。中间件看到空 id 就不再 `save()`。
  void invalidate() { id_.clear(); }

  /**
   * @brief 二进制安全的自包含编码（id / 创建与过期时间 / 全部键值）。
   *
   * 给共享存储用：格式由本类定义、不依赖 JSON；`decode()` 失败时对象保持不变。
   * 格式：`u32 id_len | id | i64 created | i64 expires | u32 count |
   *        (u32 klen | k | u32 vlen | v)*`，全部小端。
   */
  std::string encode() const;
  bool        decode(const std::string& in);

 private:
  friend class uvcpp_memory_session_store;
  friend uvcpp_web_middleware uvcpp_async_session_middleware(
      std::shared_ptr<uvcpp_async_session_store> store,
      const uvcpp_session_options&               opts);
  friend void uvcpp_async_session_logout(uvcpp_web_request& req,
                                         uvcpp_web_response& resp,
                                         uvcpp_async_session_store& store,
                                         const uvcpp_session_options& opts);
  friend uvcpp_web_middleware uvcpp_session_middleware(
      std::shared_ptr<uvcpp_session_store> store,
      const uvcpp_session_options&         opts);
  friend void uvcpp_session_logout(uvcpp_web_request& req,
                                   uvcpp_web_response& resp,
                                   uvcpp_session_store& store,
                                   const uvcpp_session_options& opts);

  std::string                        id_;
  std::map<std::string, std::string> data_;
  std::time_t                        created_at_;
  std::time_t                        expires_at_;
};

/**
 * @brief 可插拔的会话存储。
 *
 * `load()` 必须拒掉过期项（返回 false）；`save()` 覆盖同 id 的旧记录。
 * 实现要对多线程安全 —— 中间件虽然只在 loop 线程上跑，但同一个 store 可能被
 * 多条循环共用（`set_loops(n)`）。
 */
class UVCPP_API uvcpp_session_store {
 public:
  virtual ~uvcpp_session_store() {}

  virtual bool load(const std::string& id, uvcpp_session& out) = 0;
  virtual void save(const uvcpp_session& s) = 0;
  virtual void destroy(const std::string& id) = 0;
  virtual size_t size() const = 0;
};

/**
 * @brief 进程内内存存储。`max_sessions` 满了以后按**最早过期**逐出。
 *
 * 默认 10000 条；每条约几百字节量级。单实例部署够用；多实例需要共享存储
 * （Redis 适配器见 issue #70 的后续）。
 */
class UVCPP_API uvcpp_memory_session_store : public uvcpp_session_store {
 public:
  explicit uvcpp_memory_session_store(size_t max_sessions = 10000);
  ~uvcpp_memory_session_store() override;

  bool   load(const std::string& id, uvcpp_session& out) override;
  void   save(const uvcpp_session& s) override;
  void   destroy(const std::string& id) override;
  size_t size() const override;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

/// Cookie / TTL 旋钮。显式构造函数（C++11 聚合初始化资格）。
struct UVCPP_API uvcpp_session_options {
  uvcpp_session_options();

  std::string cookie_name;  ///< 默认 `sid`
  std::string path;         ///< 默认 `/`
  std::string same_site;    ///< 默认 `Lax`；也接受 `Strict` / `None`
  int         ttl_seconds;  ///< 默认 86400
  bool        secure;       ///< 默认 false（HTTPS 部署请置 true）
  bool        http_only;    ///< 默认 true
  bool        sliding;      ///< 默认 true：每次请求续期
};

/**
 * @brief 装会话中间件。应在其它业务中间件**之前**（`use()` 顺序即执行顺序）。
 *
 * 中间件在每个请求上：
 * 1. 读 Cookie → `store.load()`；没有/过期就新建并 `save()`；
 * 2. 把 `uvcpp_session*` 挂到请求属性（`uvcpp_session_of()` 取）；
 * 3. 新建或滑动续期时发 `Set-Cookie`（HttpOnly/Secure/SameSite/Path/Max-Age）；
 * 4. `next()` 返回后 `save()`（处理器里 `set()` 的东西在这里落库）；
 * 5. 会话被 `regenerate()` / 注销时删掉旧 id。
 */
UVCPP_API uvcpp_web_middleware uvcpp_session_middleware(
    std::shared_ptr<uvcpp_session_store> store,
    const uvcpp_session_options&         opts = uvcpp_session_options());

/// 取本请求的会话；中间件没装时返回 nullptr。
UVCPP_API uvcpp_session* uvcpp_session_of(uvcpp_web_request& req);

// ===========================================================================
// 异步会话存储（共享 Redis / 数据库用）
// ===========================================================================

/**
 * @brief 异步会话存储接口（issue #70 的共享存储）。
 *
 * 三个操作的回调都**在同一 loop 线程**上执行、且每个操作**恰好回调一次**。
 * `load` 失败（连接断了、找不到、过期）一律按"未命中"回调 `done(false)` ——
 * 中间件会新建一个会话；这是可用性优先的取舍，写在 `ready()` 上，调用方可以
 * 在未就绪时拒绝请求而不是静默换会话。
 *
 * `save/destroy` 的 `done` 可以省略（响应已经发出去了，失败只影响下一次请求）。
 */
class UVCPP_API uvcpp_async_session_store {
 public:
  virtual ~uvcpp_async_session_store() {}

  /// 连接是否就绪。未就绪时中间件不会尝试 `load`（直接按未命中处理）。
  virtual bool ready() const = 0;

  virtual void load(const std::string& id, uvcpp_session& out,
                    std::function<void(bool found)> done) = 0;

  virtual void save(const uvcpp_session& s,
                    std::function<void(bool ok)> done = nullptr) = 0;

  virtual void destroy(const std::string& id,
                       std::function<void(bool ok)> done = nullptr) = 0;
};

/**
 * @brief 异步会话中间件。语义与同步版一致，只是 `store->load()` 完成前挂起链。
 *
 * 只能在 loop 线程上处理请求（与其它中间件一致）。`next` 的副本会保住上下文，
 * 所以异步加载回调晚到也没关系。
 */
UVCPP_API uvcpp_web_middleware uvcpp_async_session_middleware(
    std::shared_ptr<uvcpp_async_session_store> store,
    const uvcpp_session_options&               opts = uvcpp_session_options());

/// 异步 store 版的注销：删记录 + 过期 Cookie。
UVCPP_API void uvcpp_async_session_logout(
    uvcpp_web_request& req, uvcpp_web_response& resp,
    uvcpp_async_session_store& store,
    const uvcpp_session_options& opts = uvcpp_session_options());

/// 按 options 发/刷新一条会话 Cookie（handler 在 `regenerate()` 之后用）。
UVCPP_API void uvcpp_session_set_cookie(uvcpp_web_response& resp,
                                        const uvcpp_session& s,
                                        const uvcpp_session_options& opts);

/**
 * @brief 注销：删 store 记录、清 Cookie、把请求上的会话标记为空。
 *
 * 要在 `resp.end()` **之前**调（Cookie 头只在那之前能加）。
 */
UVCPP_API void uvcpp_session_logout(
    uvcpp_web_request& req, uvcpp_web_response& resp,
    uvcpp_session_store& store,
    const uvcpp_session_options& opts = uvcpp_session_options());

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE
#endif  // SRC_WEBAPP_UVCPP_SESSION_H
