/**
 * @file src/webapp/uvcpp_web_auth.h
 * @brief 会话之上的 RBAC 与 CSRF（issue #70 第二批）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * app.use(uvcpp_session_middleware(store, session_opts));
 * app.use(uvcpp_csrf_protect());                 // 先保证有会话
 * app.use(uvcpp_require_roles({"admin"}, opts)); // opts.skip_paths = {"/login"}
 *
 * app.post("/login", [](uvcpp_web_request& req, uvcpp_web_response& resp,
 *                       uvcpp_web_next) {
 *   uvcpp_session* s = uvcpp_session_of(req);
 *   if (s) uvcpp_session_set_roles(*s, {"admin"});   // 业务校验之后
 *   resp.end();
 * });
 * ```
 *
 * ## RBAC 语义
 *
 * - 角色存在会话的数据里（键名可配，默认 `roles`，逗号分隔），由业务在登录
 *   成功时通过 `uvcpp_session_set_roles()` 写入；
 * - `uvcpp_require_roles(list)` 是**任一命中**（OR）语义：会话里有 list 中
 *   任意一个即可；全部都要请传单个角色或用多条中间件；
 * - 没有会话 / 会话里没有角色 → **401**；有角色但不匹配 → **403**；
 * - `skip_paths` 按**归一化后的路径精确匹配**（登录、健康检查这类要放行）。
 *
 * ## CSRF 语义（双提交 + 会话绑定）
 *
 * - 每个会话生成一个随机 token，存在会话里并写一条**非 HttpOnly** Cookie
 *   （前端 JS 需要读它放请求头）；`SameSite` 默认 `Strict`；
 * - 不安全方法（非 GET/HEAD/OPTIONS）必须带 `X-CSRF-Token` 头或表单字段
 *   `_csrf`，且与会话里的 token **逐字节相同**，否则 403；
 * - token 是按会话的，注销/换会话后旧 token 失效；
 * - 安全方法只负责"确保 token 存在并下发 Cookie"。
 *
 * ## 登录失败限流与审计
 *
 * `uvcpp_login_limiter` 是进程内的失败窗口 + 锁定：同一个 key（用户名、IP，
 * 或两者拼起来）在 `window_seconds` 内失败 `max_failures` 次后锁定
 * `lockout_seconds`。`allowed()` 返回是否放行（可带 `retry_after_seconds`）；
 * 事件通过 `set_audit()` 报告（LOGIN_FAILURE / LOGIN_SUCCESS / LOCKED_OUT），
 * 不内置存储，也不猜你的账号模型。
 *
 * ## 密码哈希
 *
 * `uvcpp_pbkdf2_sha256` 是**可选**实现（`UVCPP_OPENSSL_ENABLE=1` 才编）：
 * `pbkdf2-sha256$<iter>$<hex-salt>$<hex-hash>`，盐 16 字节、常量时间比较。
 * 不想要它就把业务自己的校验回调传给登录处理器 —— 本模块不强制算法。
 *
 * ## 不做
 *
 * - 并发登录策略（同一账号最多几个会话）：属于会话存储的用户索引，留后续；
 * - 不做基于路径前缀的通配豁免（`skip_paths` 是精确匹配，避免 `/admin`
 *   豁免时把 `/admin/../secret` 这类归一化绕过也放进来）。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_AUTH_H
#define SRC_WEBAPP_UVCPP_WEB_AUTH_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_WEBAPP_ENABLE

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <webapp/uvcpp_web_handler.h>

namespace uvcpp {

class uvcpp_session;
class uvcpp_web_request;
class uvcpp_web_response;

// ---------------------------------------------------------------------------
// RBAC
// ---------------------------------------------------------------------------

/// 会话里角色列表的键名（默认 `roles`）。
struct UVCPP_API uvcpp_roles_options {
  uvcpp_roles_options();
  std::string              session_key;
  std::vector<std::string> skip_paths;
};

/// 写入角色（覆盖）。空列表 = 清空角色。
UVCPP_API void uvcpp_session_set_roles(
    uvcpp_session& s, const std::vector<std::string>& roles,
    const uvcpp_roles_options& opts = uvcpp_roles_options());

/// 会话里是否有该角色。
UVCPP_API bool uvcpp_session_has_role(
    const uvcpp_session& s, const std::string& role,
    const uvcpp_roles_options& opts = uvcpp_roles_options());

/**
 * @brief 任一角色命中才放行。见文件头。
 *
 * 必须在会话中间件**之后**注册。
 */
UVCPP_API uvcpp_web_middleware uvcpp_require_roles(
    const std::vector<std::string>& roles,
    const uvcpp_roles_options&      opts = uvcpp_roles_options());

// ---------------------------------------------------------------------------
// CSRF
// ---------------------------------------------------------------------------

struct UVCPP_API uvcpp_csrf_options {
  uvcpp_csrf_options();
  std::string token_key;    ///< 会话里的键（默认 `csrf`）
  std::string cookie_name;  ///< 默认 `csrf`（**非** HttpOnly）
  std::string header_name;  ///< 默认 `x-csrf-token`
  std::string form_field;   ///< 默认 `_csrf`
  std::string same_site;    ///< 默认 `Strict`
  bool        cookie_secure; ///< HTTPS 部署置 true
};

/**
 * @brief 防 CSRF 中间件。必须在会话中间件之后。
 */
UVCPP_API uvcpp_web_middleware uvcpp_csrf_protect(
    const uvcpp_csrf_options& opts = uvcpp_csrf_options());

/// 本请求会话的 CSRF token；没有会话/未生成时返回 nullptr。
UVCPP_API const std::string* uvcpp_csrf_token(
    uvcpp_web_request& req, const uvcpp_csrf_options& opts = uvcpp_csrf_options());

// ---------------------------------------------------------------------------
// 登录失败限流 + 审计（#70 剩余）
// ---------------------------------------------------------------------------

/// 审计事件。`key` 就是调用方给限流器的那个 key。
struct UVCPP_API uvcpp_auth_event {
  enum kind_t {
    LOGIN_FAILURE = 0,
    LOGIN_SUCCESS = 1,
    LOCKED_OUT    = 2,
  };
  uvcpp_auth_event();
  kind_t      kind;
  std::string key;
  std::string detail;
  int64_t     at_ms;  ///< epoch 毫秒
};

using uvcpp_auth_audit_cb = std::function<void(const uvcpp_auth_event&)>;

struct UVCPP_API uvcpp_login_limiter_options {
  uvcpp_login_limiter_options();
  int    max_failures;     ///< 窗口内达到这个数就锁定（默认 5）
  int    window_seconds;   ///< 失败计数窗口（默认 300）
  int    lockout_seconds;  ///< 锁定时长（默认 300）
  size_t max_keys;         ///< 表上限；超了淘汰最久没碰的（默认 10000）
};

/**
 * @brief 进程内登录失败限流。线程安全；不依赖事件循环。
 *
 * 用法（账号不存在也记失败，别让 key 的存在与否泄露出去）：
 *
 * ```cpp
 * int wait = 0;
 * if (!limiter.allowed(user, &wait)) { resp.status(429); ... }
 * if (!check_password(...)) { limiter.record_failure(user); ... }
 * else { limiter.record_success(user); }
 * ```
 *
 * **多实例部署时它只是本进程的**；要全局限流得把失败计数放到共享存储里。
 */
class UVCPP_API uvcpp_login_limiter {
 public:
  explicit uvcpp_login_limiter(
      const uvcpp_login_limiter_options& opts = uvcpp_login_limiter_options());
  ~uvcpp_login_limiter();

  uvcpp_login_limiter(const uvcpp_login_limiter&) = delete;
  uvcpp_login_limiter& operator=(const uvcpp_login_limiter&) = delete;

  /// 是否放行。锁定时把剩余秒数写进 `retry_after_seconds`（可空）。
  bool allowed(const std::string& key, int* retry_after_seconds = nullptr);

  /// 记一次失败；达到阈值时进入锁定并发一条 LOCKED_OUT。
  void record_failure(const std::string& key);

  /// 记一次成功：清掉该 key 的失败计数并发一条 LOGIN_SUCCESS。
  void record_success(const std::string& key);

  /// 丢掉某个 key 的全部状态（不发事件）。
  void reset(const std::string& key);

  /// 当前表里的 key 数（测试/观测用）。
  size_t size() const;

  void set_audit(const uvcpp_auth_audit_cb& cb);

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

// ---------------------------------------------------------------------------
// 密码哈希（可选，OpenSSL）
// ---------------------------------------------------------------------------

#if UVCPP_OPENSSL_ENABLE

/**
 * @brief PBKDF2-HMAC-SHA256 的编码/校验。
 *
 * 编码格式：`pbkdf2-sha256$<iterations>$<hex salt>$<hex hash>`。盐每次随机
 * 16 字节；比较用 `CRYPTO_memcmp`（常量时间）。`hash()` 失败返回空串（不抛）。
 */
class UVCPP_API uvcpp_pbkdf2_sha256 {
 public:
  explicit uvcpp_pbkdf2_sha256(int iterations = 120000,
                               size_t salt_bytes = 16,
                               size_t hash_bytes = 32);

  int    iterations() const;
  /// 编码一个新口令。
  std::string hash(const std::string& password) const;
  /// 校验；`encoded` 格式不合法/算法名不对一律 false。
  bool verify(const std::string& password, const std::string& encoded) const;

 private:
  int    iterations_;
  size_t salt_bytes_;
  size_t hash_bytes_;
};

#endif  // UVCPP_OPENSSL_ENABLE

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE
#endif  // SRC_WEBAPP_UVCPP_WEB_AUTH_H
