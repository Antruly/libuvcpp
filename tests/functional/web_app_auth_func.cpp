/**
 * @file tests/functional/web_app_auth_func.cpp
 * @brief RBAC + CSRF（issue #70 第二批）。
 *
 * 判据：
 * 1. 无角色访问受保护路由 → 401；有角色但不匹配 → 403；
 * 2. 登录（handler 写角色）后放行；
 * 3. CSRF：不安全方法缺 token → 403；`X-CSRF-Token` 或表单 `_csrf` 正确 → 放行；
 * 4. CSRF Cookie **非 HttpOnly**（前端可读）、与会话 token 一致；
 * 5. 同一 token 换会话（注销后）不再有效（隐式由会话绑定保证）。
 */

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <webapp/uvcpp_session.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_auth.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "  [FAIL] " << what << std::endl;
  }
}

struct reply {
  int                      status = 0;
  std::string              body;
  std::vector<std::string> cookies;

  std::string cookie(const std::string& name) const {
    const std::string key = name + "=";
    for (size_t i = 0; i < cookies.size(); ++i) {
      if (cookies[i].compare(0, key.size(), key) == 0) {
        const size_t b = key.size();
        size_t       e = cookies[i].find(';', b);
        if (e == std::string::npos) e = cookies[i].size();
        return cookies[i].substr(b, e - b);
      }
    }
    return std::string();
  }
  bool cookie_has(const std::string& name, const std::string& needle) const {
    for (size_t i = 0; i < cookies.size(); ++i) {
      if (cookies[i].compare(0, name.size() + 1, name + "=") == 0 &&
          cookies[i].find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
};

reply call(int port, const std::string& path, bool post,
           const std::string& sid = std::string(),
           const std::string& csrf = std::string(),
           const std::string& form_body = std::string()) {
  uvcpp_http_client c;
  reply             out;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return out;
  uvcpp_http_request req =
      post ? uvcpp_http_request::make_post(
                 path, form_body.data(), form_body.size(),
                 "application/x-www-form-urlencoded")
           : uvcpp_http_request::make_get(path);
  std::string cookie;
  if (!sid.empty()) cookie += "sid=" + sid;
  if (!cookie.empty()) req.set_header("cookie", cookie);
  if (!csrf.empty()) req.set_header("x-csrf-token", csrf);
  uvcpp_http_response resp;
  if (c.send_wait(req, resp, 3000) != 0) return out;
  out.status = static_cast<int>(resp.status_code);
  out.body   = resp.body.to_string();
  for (size_t i = 0; i < resp.headers.size(); ++i) {
    std::string k = resp.headers[i].name;
    for (size_t j = 0; j < k.size(); ++j) {
      k[j] = static_cast<char>(std::tolower(static_cast<unsigned char>(k[j])));
    }
    if (k == "set-cookie") out.cookies.push_back(resp.headers[i].value);
  }
  return out;
}

struct TestApp {
  std::shared_ptr<uvcpp_memory_session_store> store =
      std::make_shared<uvcpp_memory_session_store>();
  uvcpp_session_options session_opts;
  uvcpp_roles_options   roles_opts;
  uvcpp_web_app         app;

  explicit TestApp(const std::vector<std::string>& required =
                       std::vector<std::string>{"admin"}) {
    roles_opts.skip_paths.push_back("/login");
    roles_opts.skip_paths.push_back("/me");

    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN);
    app.use(uvcpp_session_middleware(store, session_opts));
    app.use(uvcpp_csrf_protect());
    app.use(uvcpp_require_roles(required, roles_opts));

    app.get("/me", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                      uvcpp_web_next) {
      uvcpp_session* s = uvcpp_session_of(req);
      resp.text("roles=" + (s ? s->get(roles_string()) : std::string()));
      resp.end();
    });
    // 小工具：避免在 lambda 里构造 options 的副本。
    app.post("/login",
             [](uvcpp_web_request& req, uvcpp_web_response& resp,
                uvcpp_web_next) {
               uvcpp_session* s = uvcpp_session_of(req);
               if (s != nullptr) {
                 uvcpp_session_set_roles(*s, std::vector<std::string>{"admin"});
               }
               resp.text(std::string("ok"));
               resp.end();
             });
    app.get("/admin", [](uvcpp_web_request&, uvcpp_web_response& resp,
                         uvcpp_web_next) {
      resp.text(std::string("admin"));
      resp.end();
    });
    app.post("/admin", [](uvcpp_web_request&, uvcpp_web_response& resp,
                          uvcpp_web_next) {
      resp.text(std::string("admin-post"));
      resp.end();
    });
    app.get("/user", [](uvcpp_web_request&, uvcpp_web_response& resp,
                        uvcpp_web_next) {
      resp.text(std::string("user"));
      resp.end();
    });
  }

  static std::string roles_string() { return std::string("roles"); }

  int start() {
    if (app.start_background() != 0) return -1;
    return app.bound_port();
  }
  void shutdown() {
    app.stop();
    app.join();
  }
};

void test_roles_and_csrf() {
  std::cout << "-- RBAC / CSRF --" << std::endl;
  TestApp   t;
  // /user 需要 user 角色：单独注册一条只要求 user 的中间件会污染全链，
  // 这里用相同的全局 admin 规则验证"不匹配 → 403"（admin 用户访问 /user 也 403）。
  const int port = t.start();
  check(port > 0, "app 起来了");
  if (port <= 0) {
    t.shutdown();
    return;
  }

  const reply first = call(port, "/me", false);
  check(first.status == 200, "GET /me 200（roles 中间件放行）");
  const std::string sid  = first.cookie("sid");
  const std::string csrf = first.cookie("csrf");
  check(!sid.empty(), "拿到 sid");
  check(!csrf.empty(), "拿到 csrf token");
  check(first.cookie_has("csrf", "HttpOnly") == false,
        "CSRF Cookie 不是 HttpOnly（JS 要读）");
  check(first.cookie_has("csrf", "SameSite=Strict"), "CSRF Cookie SameSite=Strict");

  check(call(port, "/admin", false, sid).status == 401,
        "无角色访问 /admin → 401");

  check(call(port, "/login", true, sid, "").status == 403,
        "POST /login 缺 CSRF token → 403");
  check(call(port, "/login", true, sid, csrf).status == 200,
        "带 X-CSRF-Token 登录成功");
  check(call(port, "/admin", false, sid).status == 200 &&
            call(port, "/admin", false, sid).body == "admin",
        "admin 角色访问 /admin 200");
  check(call(port, "/user", false, sid).status == 200,
        "admin 命中本中间件 → /user 也放行（OR 语义）");

  // 表单字段路径
  const std::string form = "_csrf=" + csrf;
  check(call(port, "/login", true, sid, "", form).status == 200,
        "表单 _csrf 字段同样通过");

  // 不安全的 POST /admin 也必须带 token
  check(call(port, "/admin", true, sid, "").status == 403,
        "POST /admin 缺 token → 403");
  check(call(port, "/admin", true, sid, csrf).status == 200 &&
            call(port, "/admin", true, sid, csrf).body == "admin-post",
        "POST /admin 带 token → 200");

  t.shutdown();
}

void test_role_mismatch() {
  std::cout << "-- 角色不匹配 → 403 --" << std::endl;
  TestApp   t(std::vector<std::string>{"user"});
  const int port = t.start();
  if (port <= 0) {
    check(false, "app 起来了");
    t.shutdown();
    return;
  }
  const reply first = call(port, "/me", false);
  const std::string sid  = first.cookie("sid");
  const std::string csrf = first.cookie("csrf");
  check(call(port, "/login", true, sid, csrf).status == 200,
        "登录（只拿到 admin 角色）");
  check(call(port, "/admin", false, sid).status == 403,
        "只有 admin、要求 user → 403");
  t.shutdown();
}

}  // namespace

int main() {
  std::cout << "[web_app_auth] start" << std::endl;
  test_roles_and_csrf();
  test_role_mismatch();
  if (g_fail == 0) {
    std::cout << "[web_app_auth] ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << "[web_app_auth] " << g_fail << " check(s) failed" << std::endl;
  return 1;
}

#else  // !WEBAPP

int main() {
  std::cout << "SKIP: webapp disabled" << std::endl;
  return 0;
}

#endif
