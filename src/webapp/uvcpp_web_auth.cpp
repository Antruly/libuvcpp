/**
 * @file src/webapp/uvcpp_web_auth.cpp
 * @brief RBAC 与 CSRF 中间件的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "webapp/uvcpp_web_auth.h"

#if UVCPP_WEBAPP_ENABLE

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <sstream>

#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_session.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

#if UVCPP_OPENSSL_ENABLE
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

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

// ===========================================================================
// 登录失败限流 + 审计
// ===========================================================================

uvcpp_auth_event::uvcpp_auth_event()
    : kind(LOGIN_FAILURE), at_ms(0) {}

uvcpp_login_limiter_options::uvcpp_login_limiter_options()
    : max_failures(5),
      window_seconds(300),
      lockout_seconds(300),
      max_keys(10000) {}

namespace {

int64_t auth_now_ms() {
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

}  // namespace

struct uvcpp_login_limiter::impl {
  struct entry {
    entry()
        : first_ms(0), last_ms(0), failures(0), locked_until_ms(0) {}
    int64_t first_ms;
    int64_t last_ms;
    int     failures;
    int64_t locked_until_ms;
  };

  explicit impl(const uvcpp_login_limiter_options& o) : opts(o) {
    if (opts.max_failures <= 0) opts.max_failures = 1;
    if (opts.window_seconds <= 0) opts.window_seconds = 1;
    if (opts.lockout_seconds < 0) opts.lockout_seconds = 0;
    if (opts.max_keys == 0) opts.max_keys = 1;
  }

  uvcpp_login_limiter_options opts;
  mutable std::mutex         mu;
  std::map<std::string, entry> table;
  uvcpp_auth_audit_cb        audit;

  /// 调用方持锁。过了窗口就把失败计数归零。
  void roll_window(entry& e, int64_t now) const {
    if (e.failures == 0) return;
    if (now - e.first_ms > static_cast<int64_t>(opts.window_seconds) * 1000) {
      e.failures = 0;
      e.first_ms = now;
    }
  }

  void evict_if_needed() {
    if (table.size() <= opts.max_keys) return;
    // 一次淘汰到上限的 9/10，避免每个新 key 都扫一遍。
    const size_t target = opts.max_keys - opts.max_keys / 10;
    while (table.size() > target) {
      std::map<std::string, entry>::iterator oldest = table.begin();
      for (std::map<std::string, entry>::iterator it = table.begin();
           it != table.end(); ++it) {
        if (it->second.last_ms < oldest->second.last_ms) oldest = it;
      }
      table.erase(oldest);
    }
  }

  void emit(const uvcpp_auth_event& ev) {
    uvcpp_auth_audit_cb cb;
    {
      std::lock_guard<std::mutex> lk(mu);
      cb = audit;
    }
    if (cb) cb(ev);
  }
};

uvcpp_login_limiter::uvcpp_login_limiter(
    const uvcpp_login_limiter_options& opts)
    : impl_(new impl(opts)) {}

uvcpp_login_limiter::~uvcpp_login_limiter() {}

bool uvcpp_login_limiter::allowed(const std::string& key,
                                  int* retry_after_seconds) {
  const int64_t now = auth_now_ms();
  std::lock_guard<std::mutex> lk(impl_->mu);
  std::map<std::string, impl::entry>::iterator it = impl_->table.find(key);
  if (it == impl_->table.end()) return true;
  impl::entry& e = it->second;
  if (e.locked_until_ms > now) {
    if (retry_after_seconds != nullptr) {
      const int64_t left = e.locked_until_ms - now;
      *retry_after_seconds = static_cast<int>((left + 999) / 1000);
    }
    return false;
  }
  impl_->roll_window(e, now);
  return e.failures < impl_->opts.max_failures;
}

void uvcpp_login_limiter::record_failure(const std::string& key) {
  const int64_t now = auth_now_ms();
  uvcpp_auth_event ev;
  ev.kind = uvcpp_auth_event::LOGIN_FAILURE;
  ev.key  = key;
  ev.at_ms = now;
  bool locked_now = false;
  int  failures = 0;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl::entry& e = impl_->table[key];
    e.last_ms = now;
    if (e.first_ms == 0) e.first_ms = now;
    impl_->roll_window(e, now);
    if (e.locked_until_ms > now) {
      // 锁定期间继续失败：不叠加锁定时长，也不再加计数。
      failures = e.failures;
    } else {
      ++e.failures;
      failures = e.failures;
      if (e.failures >= impl_->opts.max_failures) {
        e.locked_until_ms =
            now + static_cast<int64_t>(impl_->opts.lockout_seconds) * 1000;
        locked_now = true;
      }
    }
    impl_->evict_if_needed();
  }
  std::ostringstream oss;
  oss << "failures=" << failures;
  ev.detail = oss.str();
  impl_->emit(ev);
  if (locked_now) {
    uvcpp_auth_event locked;
    locked.kind = uvcpp_auth_event::LOCKED_OUT;
    locked.key  = key;
    locked.at_ms = now;
    std::ostringstream l;
    l << "lockout=" << impl_->opts.lockout_seconds << "s";
    locked.detail = l.str();
    impl_->emit(locked);
  }
}

void uvcpp_login_limiter::record_success(const std::string& key) {
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->table.erase(key);
  }
  uvcpp_auth_event ev;
  ev.kind  = uvcpp_auth_event::LOGIN_SUCCESS;
  ev.key   = key;
  ev.at_ms = auth_now_ms();
  impl_->emit(ev);
}

void uvcpp_login_limiter::reset(const std::string& key) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->table.erase(key);
}

size_t uvcpp_login_limiter::size() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->table.size();
}

void uvcpp_login_limiter::set_audit(const uvcpp_auth_audit_cb& cb) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->audit = cb;
}

#if UVCPP_OPENSSL_ENABLE

namespace {

std::string hex_encode(const unsigned char* p, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(kHex[(p[i] >> 4) & 0xF]);
    out.push_back(kHex[p[i] & 0xF]);
  }
  return out;
}

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool hex_decode(const std::string& s, std::string& out) {
  if (s.size() % 2 != 0) return false;
  out.clear();
  out.reserve(s.size() / 2);
  for (size_t i = 0; i < s.size(); i += 2) {
    const int hi = hex_nibble(s[i]);
    const int lo = hex_nibble(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return true;
}

bool split_encoded(const std::string& encoded, std::string& alg, int& iter,
                   std::string& salt, std::string& hash) {
  const size_t p1 = encoded.find('$');
  if (p1 == std::string::npos) return false;
  const size_t p2 = encoded.find('$', p1 + 1);
  if (p2 == std::string::npos) return false;
  const size_t p3 = encoded.find('$', p2 + 1);
  if (p3 == std::string::npos) return false;
  alg = encoded.substr(0, p1);
  if (alg != "pbkdf2-sha256") return false;
  const std::string is = encoded.substr(p1 + 1, p2 - p1 - 1);
  if (is.empty() || is.size() > 9) return false;
  iter = 0;
  for (size_t i = 0; i < is.size(); ++i) {
    if (is[i] < '0' || is[i] > '9') return false;
    iter = iter * 10 + (is[i] - '0');
  }
  if (iter < 1) return false;
  return hex_decode(encoded.substr(p2 + 1, p3 - p2 - 1), salt) &&
         hex_decode(encoded.substr(p3 + 1), hash) && !salt.empty() &&
         !hash.empty();
}

}  // namespace

uvcpp_pbkdf2_sha256::uvcpp_pbkdf2_sha256(int iterations, size_t salt_bytes,
                                         size_t hash_bytes)
    : iterations_(iterations > 0 ? iterations : 1),
      salt_bytes_(salt_bytes > 0 ? salt_bytes : 16),
      hash_bytes_(hash_bytes > 0 ? hash_bytes : 32) {}

int uvcpp_pbkdf2_sha256::iterations() const { return iterations_; }

std::string uvcpp_pbkdf2_sha256::hash(const std::string& password) const {
  unsigned char salt[64];
  const size_t  n = salt_bytes_ > sizeof(salt) ? sizeof(salt) : salt_bytes_;
  if (RAND_bytes(salt, static_cast<int>(n)) != 1) return std::string();

  unsigned char out[128];
  const size_t  k = hash_bytes_ > sizeof(out) ? sizeof(out) : hash_bytes_;
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                        salt, static_cast<int>(n), iterations_,
                        EVP_sha256(), static_cast<int>(k), out) != 1) {
    return std::string();
  }
  char head[64];
  std::snprintf(head, sizeof(head), "pbkdf2-sha256$%d$", iterations_);
  return std::string(head) + hex_encode(salt, n) + "$" + hex_encode(out, k);
}

bool uvcpp_pbkdf2_sha256::verify(const std::string& password,
                                 const std::string& encoded) const {
  std::string alg, salt, want;
  int         iter = 0;
  if (!split_encoded(encoded, alg, iter, salt, want)) return false;
  // 编码是调用方给的（可能来自数据库/攻击者）：长度与迭代次数都要有上界，
  // 否则一段畸形串就能让这里跑一次超长 PBKDF2 或者只比较前缀。
  if (salt.size() > 64 || want.size() > 128 || iter > 10000000) return false;

  unsigned char out[128];
  const size_t  k = want.size();
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                        reinterpret_cast<const unsigned char*>(salt.data()),
                        static_cast<int>(salt.size()), iter, EVP_sha256(),
                        static_cast<int>(k), out) != 1) {
    return false;
  }
  return CRYPTO_memcmp(out, want.data(), k) == 0;
}

#endif  // UVCPP_OPENSSL_ENABLE

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE
