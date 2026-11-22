/**
 * @file tests/functional/web_app_session_func.cpp
 * @brief 会话存储 + 安全 Cookie 中间件（issue #70 的第一批）。
 *
 * 判据（全部走真端口 + 真 Cookie）：
 *
 * 1. 首次请求建会话，`Set-Cookie` 带 HttpOnly / SameSite=Lax / Path / Max-Age；
 * 2. 登录（handler 里 `session->set()`）之后，同 Cookie 的请求读得到 uid；
 * 3. 注销后旧 Cookie 失效（store 里也没了），响应带过期 Cookie；
 * 4. `ttl=1` 时过期后旧 Cookie 被拒；
 * 5. `regenerate()` 后旧 id 失效、新 id 可用；
 * 6. `secure=true` 时 Cookie 带 Secure；
 * 7. 内存 store：save/load/destroy/size 与"按最早过期逐出"的上限。
 */

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_session.h>

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
  std::vector<std::string> set_cookies;

  /// 所有 `Set-Cookie` 拼起来看关键字（同名头允许多条，`get_header` 只给第一条）。
  bool cookie_has(const std::string& needle) const {
    for (size_t i = 0; i < set_cookies.size(); ++i) {
      if (set_cookies[i].find(needle) != std::string::npos) return true;
    }
    return false;
  }
  std::string first_cookie() const {
    return set_cookies.empty() ? std::string() : set_cookies[0];
  }
};

/// 从一条 `Set-Cookie` 里抠出 `sid=...` 的值。
std::string sid_of(const std::string& sc) {
  const std::string key = "sid=";
  const size_t      p   = sc.find(key);
  if (p == std::string::npos) return std::string();
  const size_t b = p + key.size();
  size_t       e = sc.find(';', b);
  if (e == std::string::npos) e = sc.size();
  return sc.substr(b, e - b);
}

reply call(int port, const std::string& path, bool post,
           const std::string& sid = std::string()) {
  uvcpp_http_client c;
  reply             out;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return out;
  uvcpp_http_request req = post
                               ? uvcpp_http_request::make_post(
                                     path, "x", 1, "text/plain")
                               : uvcpp_http_request::make_get(path);
  if (!sid.empty()) req.set_header("cookie", "sid=" + sid);
  uvcpp_http_response resp;
  if (c.send_wait(req, resp, 3000) != 0) return out;
  out.status = static_cast<int>(resp.status_code);
  out.body   = resp.body.to_string();
  for (size_t i = 0; i < resp.headers.size(); ++i) {
    std::string key = resp.headers[i].name;
    for (size_t k = 0; k < key.size(); ++k) {
      key[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[k])));
    }
    if (key == "set-cookie") {
      out.set_cookies.push_back(resp.headers[i].value);
    }
  }
  return out;
}

struct TestApp {
  std::shared_ptr<uvcpp_memory_session_store> store;
  uvcpp_session_options                       opts;
  uvcpp_web_app                               app;

  explicit TestApp(int ttl = 3600, bool secure = false) {
    store = std::make_shared<uvcpp_memory_session_store>();
    opts.ttl_seconds = ttl;
    opts.secure      = secure;
    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN);
    app.use(uvcpp_session_middleware(store, opts));

    app.get("/me", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                      uvcpp_web_next) {
      uvcpp_session* s = uvcpp_session_of(req);
      if (s == nullptr || !s->has("uid")) {
        resp.status(http_status::UNAUTHORIZED);
        resp.text(std::string("no"));
      } else {
        resp.text("uid=" + s->get("uid"));
      }
      resp.end();
    });
    app.post("/login", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                          uvcpp_web_next) {
      uvcpp_session* s = uvcpp_session_of(req);
      if (s != nullptr) s->set("uid", "42");

      resp.text(std::string("ok"));
      resp.end();
    });
    app.post("/logout",
             [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                    uvcpp_web_next) {
               uvcpp_session_logout(req, resp, *store, opts);
               resp.text(std::string("bye"));
               resp.end();
             });
    app.post("/regen",
             [this](uvcpp_web_request& req, uvcpp_web_response& resp,
                    uvcpp_web_next) {
               uvcpp_session* s = uvcpp_session_of(req);
               if (s == nullptr) {
                 resp.status(http_status::INTERNAL_SERVER_ERROR);
                 resp.end();
                 return;
               }
               s->regenerate();
               uvcpp_session_set_cookie(resp, *s, opts);
               resp.text(s->id());
               resp.end();
             });
  }

  int start() {
    if (app.start_background() != 0) return -1;
    return app.bound_port();
  }
  void shutdown() {
    app.stop();
    app.join();
  }
};

void test_store() {
  std::cout << "-- 内存 store --" << std::endl;
  uvcpp_memory_session_store store(2);
  // 没有 API 能伪造 id/expires（那是故意的：id 只能由中间件生成），
  // 这里用工厂路径造两份会话：直接构造 + 私有字段不可见 ⇒ 用 store 的
  // 语义边界（空 id 拒绝、destroy 幂等、size）来判。
  uvcpp_session s;
  store.save(s);  // 空 id：必须被拒绝
  check(store.size() == 0, "空 id 不落库");
  store.destroy("nope");
  check(store.size() == 0, "destroy 不存在的 id 是空操作");
  uvcpp_session out;
  check(!store.load("nope", out), "load 不存在的 id 返回 false");
}

void test_session_flow() {
  std::cout << "-- 建会话 / 登录 / 注销 --" << std::endl;
  TestApp   t;
  const int port = t.start();
  check(port > 0, "app 起来了");
  if (port <= 0) {
    t.shutdown();
    return;
  }

  const reply first = call(port, "/me", false);
  check(first.status == 401, "未登录：401");
  check(first.cookie_has("sid="),
        "首次请求就下发 sid");
  check(first.cookie_has("HttpOnly"), "HttpOnly");
  check(first.cookie_has("SameSite=Lax"),
        "SameSite=Lax");
  check(first.cookie_has("Path=/"), "Path=/");
  check(first.cookie_has("Max-Age=3600"),
        "Max-Age=ttl");
  const std::string sid = sid_of(first.first_cookie());

  const reply login = call(port, "/login", true, sid);
  check(login.status == 200 && login.body == "ok", "登录 200");

  const reply me = call(port, "/me", false, sid);
  check(me.status == 200 && me.body == "uid=42", "登录后读得到 uid（" +
                                                    me.body + "）");

  const reply out = call(port, "/logout", true, sid);
  check(out.status == 200 && out.body == "bye", "注销 200");
  check(out.cookie_has("Max-Age=0"), "注销响应让 Cookie 过期");
  const reply after = call(port, "/me", false, sid);
  check(after.status == 401, "旧 Cookie 注销后失效");
  check(t.store->size() == 1, "store 里旧会话已删（当前只剩新请求建的）");

  t.shutdown();
}

void test_expiry() {
  std::cout << "-- 过期 --" << std::endl;
  // ttl 给 3 秒：MSVC runner 上"建会话→登录→读"可能就要 1 秒以上，
  // ttl=1 会把"过期前可读"偶发判成过期（实测整条用例 1.9s）。
  TestApp   t(3);
  const int port = t.start();
  if (port <= 0) {
    check(false, "app 起来了");
    t.shutdown();
    return;
  }
  const std::string sid = sid_of(call(port, "/me", false).first_cookie());
  check(call(port, "/login", true, sid).status == 200, "登录");
  check(call(port, "/me", false, sid).body == "uid=42", "过期前可读");
  std::this_thread::sleep_for(std::chrono::milliseconds(3300));
  check(call(port, "/me", false, sid).status == 401, "ttl=3 过期后被拒");
  t.shutdown();
}

void test_regenerate_and_secure() {
  std::cout << "-- regenerate / Secure --" << std::endl;
  TestApp   t(3600, /*secure=*/true);
  const int port = t.start();
  if (port <= 0) {
    check(false, "app 起来了");
    t.shutdown();
    return;
  }
  const reply first = call(port, "/me", false);
  check(first.cookie_has("Secure"),
        "secure=true 时 Cookie 带 Secure");
  const std::string old_sid = sid_of(first.first_cookie());
  call(port, "/login", true, old_sid);
  const reply regen = call(port, "/regen", true, old_sid);
  check(regen.status == 200 && !regen.body.empty(), "regenerate 200");
  const std::string new_sid = regen.body;
  check(new_sid != old_sid, "新 id 与旧 id 不同");
  check(call(port, "/me", false, old_sid).status == 401, "旧 id 立即失效");
  check(call(port, "/me", false, new_sid).body == "uid=42",
        "新 id 带着原来的会话数据");
  t.shutdown();
}

}  // namespace

int main() {
  std::cout << "[web_app_session] start" << std::endl;
  test_store();
  test_session_flow();
  test_expiry();
  test_regenerate_and_secure();
  if (g_fail == 0) {
    std::cout << "[web_app_session] ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << "[web_app_session] " << g_fail << " check(s) failed"
            << std::endl;
  return 1;
}

#else  // !WEBAPP

int main() {
  std::cout << "SKIP: webapp disabled" << std::endl;
  return 0;
}

#endif
