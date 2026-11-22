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
 * ## 不做
 *
 * - 密码哈希、登录失败限流、审计事件：需要选型/业务模型，留 #70 后续；
 * - 不做基于路径前缀的通配豁免（`skip_paths` 是精确匹配，避免 `/admin`
 *   豁免时把 `/admin/../secret` 这类归一化绕过也放进来）。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_AUTH_H
#define SRC_WEBAPP_UVCPP_WEB_AUTH_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_WEBAPP_ENABLE

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

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE
#endif  // SRC_WEBAPP_UVCPP_WEB_AUTH_H
