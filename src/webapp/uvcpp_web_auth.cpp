/**
 * @file src/webapp/uvcpp_web_auth.cpp
 * @brief RBAC 与 CSRF 中间件的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "webapp/uvcpp_web_auth.h"

#if UVCPP_WEBAPP_ENABLE

#include <cctype>
#include <mutex>
#include <random>
#include <sstream>

#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_session.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

namespace uvcpp {

uvcpp_roles_options::uvcpp_roles_options() : session_key("roles") {}

uvcpp_csrf_options::uvcpp_csrf_options()
    : token_key("csrf"),
      cookie_name("csrf"),
      header_name("x-csrf-token"),
      form_field("_csrf"),
      same_site("Strict"),
      cookie_secure(false) {}

namespace {

std::string random_token() {
  static std::mutex         mu;
  static std::random_device rd;
  std::lock_guard<std::mutex> lk(mu);
  char                        buf[33];
  for (int i = 0; i < 32; i += 8) {
    const unsigned v = static_cast<unsigned>(rd());
    for (int k = 0; k < 8; ++k) {
      const unsigned nib = (v >> (k * 4)) & 0xfu;
      buf[i + k] = static_cast<char>(nib < 10 ? '0' + nib : 'a' + (nib - 10));
    }
  }
  buf[32] = '\0';
  return std::string(buf);
}

std::vector<std::string> split_roles(const std::string& v) {
  std::vector<std::string> out;
  std::string              cur;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (v[i] != ' ' && v[i] != '\t') {
      cur += v[i];
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool secure_eq(const std::string& a, const std::string& b) {
  // 长度不等也走完整循环，尽量不把"差在哪一位"泄漏给计时侧信道。
  const size_t n = a.size() > b.size() ? a.size() : b.size();
  unsigned     diff = a.size() == b.size() ? 0u : 1u;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    diff |= static_cast<unsigned>(ca ^ cb);
  }
  return diff == 0;
}

bool is_safe_method(http_method m) {
  return m == http_method::HTTP_GET || m == http_method::HTTP_HEAD ||
         m == http_method::HTTP_OPTIONS;
}

}  // namespace

// ---------------------------------------------------------------------------
// RBAC
// ---------------------------------------------------------------------------

void uvcpp_session_set_roles(uvcpp_session& s,
                             const std::vector<std::string>& roles,
                             const uvcpp_roles_options& opts) {
  std::string v;
  for (size_t i = 0; i < roles.size(); ++i) {
    if (i != 0) v += ',';
    v += roles[i];
  }
  if (v.empty()) {
    s.erase(opts.session_key);
  } else {
    s.set(opts.session_key, v);
  }
}

bool uvcpp_session_has_role(const uvcpp_session& s, const std::string& role,
                            const uvcpp_roles_options& opts) {
  if (role.empty() || !s.has(opts.session_key)) return false;
  const std::vector<std::string> have = split_roles(s.get(opts.session_key));
  for (size_t i = 0; i < have.size(); ++i) {
    if (have[i] == role) return true;
  }
  return false;
}

uvcpp_web_middleware uvcpp_require_roles(
    const std::vector<std::string>& roles, const uvcpp_roles_options& opts) {
  return [roles, opts](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    for (size_t i = 0; i < opts.skip_paths.size(); ++i) {
      if (req.path() == opts.skip_paths[i]) {
        next();
        return;
      }
    }
    uvcpp_session* s = uvcpp_session_of(req);
    // 没有会话、或会话里**根本没有角色键** = 未认证 → 401；
    // 有角色键但一个都不匹配 = 已认证但没权限 → 403。
    if (s == nullptr || s->empty() || !s->has(opts.session_key)) {
      resp.status(http_status::UNAUTHORIZED);
      resp.text(std::string("unauthorized"));
      resp.end();
      return;  // 不调 next：链到此为止
    }
    for (size_t i = 0; i < roles.size(); ++i) {
      if (uvcpp_session_has_role(*s, roles[i], opts)) {
        next();
        return;
      }
    }
    resp.status(http_status::FORBIDDEN);
    resp.text(std::string("forbidden"));
    resp.end();
  };
}

// ---------------------------------------------------------------------------
// CSRF
// ---------------------------------------------------------------------------

const std::string* uvcpp_csrf_token(uvcpp_web_request& req,
                                    const uvcpp_csrf_options& opts) {
  uvcpp_session* s = uvcpp_session_of(req);
  if (s == nullptr || s->empty() || !s->has(opts.token_key)) return nullptr;
  // session 的 get 按值返回；用请求级属性缓存一份，保证返回指针活到请求结束。
  const std::string token = s->get(opts.token_key);
  std::shared_ptr<std::string> held =
      req.attribute_as<std::string>("uvcpp.csrf.token");
  if (!held || *held != token) {
    held = std::make_shared<std::string>(token);
    req.set_attribute("uvcpp.csrf.token", held);
  }
  return held.get();
}

uvcpp_web_middleware uvcpp_csrf_protect(const uvcpp_csrf_options& opts) {
  return [opts](uvcpp_web_request& req, uvcpp_web_response& resp,
                uvcpp_web_next next) {
    uvcpp_session* s = uvcpp_session_of(req);
    if (s == nullptr) {
      // 没装会话中间件是配置错误：**拒绝**而不是静默放行，否则 CSRF 形同虚设。
      resp.status(http_status::INTERNAL_SERVER_ERROR);
      resp.text(std::string("csrf: session middleware missing"));
      resp.end();
      return;
    }
    std::string token = s->get(opts.token_key);
    if (token.empty()) {
      token = random_token();
      s->set(opts.token_key, token);
      // 新 token：立刻刷新双提交 Cookie。非 HttpOnly —— 前端 JS 要读它。
      resp.set_cookie(opts.cookie_name, token, "/", -1, /*http_only=*/false,
                      opts.cookie_secure, opts.same_site);
    } else {
      const std::string* ck = req.cookie(opts.cookie_name);
      if (ck == nullptr || *ck != token) {
        resp.set_cookie(opts.cookie_name, token, "/", -1, /*http_only=*/false,
                        opts.cookie_secure, opts.same_site);
      }
    }

    if (is_safe_method(req.method())) {
      next();
      return;
    }

    std::string supplied = req.header(opts.header_name);
    if (supplied.empty()) {
      const std::string* f = req.form(opts.form_field);
      if (f != nullptr) supplied = *f;
    }
    if (!secure_eq(supplied, token)) {
      resp.status(http_status::FORBIDDEN);
      resp.text(std::string("csrf token mismatch"));
      resp.end();
      return;
    }
    next();
  };
}

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE
