/**
 * @file tests/functional/web_app_session_redis_func.cpp
 * @brief 异步会话中间件 + Redis 共享 store（issue #70 的“多实例登录态一致”）。
 *
 * 判据：
 *
 * 1. 请求走异步中间件：`/login` 写 `uid`，带同一 Cookie 的 `/me` 读得到；
 * 2. **另一个实例**（另一条循环上的第二个 Redis store）按同一个 sid 能 load
 *    出完整的会话数据 —— 这才是“共享存储”的意义；
 * 3. `/logout` 之后旧 Cookie 读不到会话（`/me` 401，第二个实例 load miss）；
 * 4. store 的 `ready()` 在连接建立后为真（用例先轮询它，避免拿“未就绪”当
 *    “存了”）。
 *
 * 环境：`UVCPP_REDIS_TEST_URL`；缺省退 3（SKIP），`UVCPP_REDIS_TEST_REQUIRE=1`
 * 时缺 URL/连接失败都是失败。
 */

#include <uvcpp/uvcpp_define.h>

#include <cstdio>

#if UVCPP_WEBAPP_ENABLE && UVCPP_REDIS_ENABLE

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <webapp/uvcpp_redis_session_store.h>
#include <webapp/uvcpp_session.h>
#include <webapp/uvcpp_web_app.h>

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::printf("  [ ok ] %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("  [FAIL] %s\n", what.c_str());
  }
  std::fflush(stdout);
}

bool pump(uvcpp_loop& loop, const std::function<bool()>& pred, int ms) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!pred() && std::chrono::steady_clock::now() < deadline) {
    loop.run(UV_RUN_NOWAIT);
    if (!pred()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 3; ++i) loop.run(UV_RUN_NOWAIT);
  return pred();
}

bool http_get(int port, const std::string& path, const std::string& cookie,
              uvcpp_http_response& resp) {
  uvcpp_http_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;
  uvcpp_http_request req = uvcpp_http_request::make_get(path);
  req.set_header("connection", "close");
  if (!cookie.empty()) req.set_header("cookie", cookie);
  return c.send_wait_plain(req, resp, 5000) == 0;
}

std::string cookie_from(const uvcpp_http_response& resp) {
  const std::string sc = resp.get_header("set-cookie");
  if (sc.empty()) return std::string();
  const std::size_t semi = sc.find(';');
  return semi == std::string::npos ? sc : sc.substr(0, semi);
}

std::string cookie_value(const std::string& cookie) {
  const std::size_t eq = cookie.find('=');
  if (eq == std::string::npos) return std::string();
  return cookie.substr(eq + 1);
}

}  // namespace

int main() {
  const char* url     = std::getenv("UVCPP_REDIS_TEST_URL");
  const char* require = std::getenv("UVCPP_REDIS_TEST_REQUIRE");
  const bool  required = require != nullptr && std::strcmp(require, "1") == 0;
  if (url == nullptr || *url == '\0') {
    if (required) {
      std::printf("[session_redis] FAIL: REQUIRE=1 但没有 UVCPP_REDIS_TEST_URL\n");
      return 1;
    }
    std::printf("SKIP: 没有 UVCPP_REDIS_TEST_URL\n");
    return 3;
  }

  std::printf("[session_redis] issue #70（共享会话）\n");

  const std::string prefix =
      "uvcpp:test:sess:" +
      std::to_string(static_cast<long>(uv_os_getpid())) + ":";
  uvcpp_redis_session_store_options ropts;
  ropts.key_prefix = prefix;

  uvcpp_session_options sopts;
  sopts.cookie_name = "sid";
  sopts.ttl_seconds = 60;

  uvcpp_web_app app;
  std::shared_ptr<uvcpp_redis_session_store> store(
      new uvcpp_redis_session_store(app.loop(), url, ropts));
  std::string serr;
  check(store->start(&serr) == 0, "store.start（" + serr + "）");

  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);

  app.use(uvcpp_async_session_middleware(store, sopts));

  app.get("/ready", [store](uvcpp_web_request&, uvcpp_web_response& resp,
                            uvcpp_web_next) {
    resp.text(store->ready() ? "1" : "0");
    resp.end();
  });
  app.get("/login", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next) {
    uvcpp_session* s = uvcpp_session_of(req);
    if (s != nullptr) s->set("uid", "alice");
    resp.text("ok");
    resp.end();
  });
  app.get("/me", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                    uvcpp_web_next) {
    uvcpp_session* s = uvcpp_session_of(req);
    if (s == nullptr || !s->has("uid")) {
      resp.status(http_status::UNAUTHORIZED);
      resp.text("no");
      resp.end();
      return;
    }
    resp.text(s->get("uid"));
    resp.end();
  });
  app.get("/logout", [store, sopts](uvcpp_web_request& req,
                                    uvcpp_web_response& resp,
                                    uvcpp_web_next) {
    uvcpp_async_session_logout(req, resp, *store, sopts);
    resp.text("bye");
    resp.end();
  });

  check(app.start_background() == 0, "app.start_background");
  const int port = app.bound_port();
  check(port > 0, "拿到端口");

  // store 就绪再开始，避免“没连上”被误读成“存过了”。
  bool ready = false;
  for (int i = 0; i < 50; ++i) {
    uvcpp_http_response r;
    if (http_get(port, "/ready", "", r) && r.body.to_string() == "1") {
      ready = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  check(ready, "store.ready() 在连接后为真");

  // ---- 登录 + 同实例读回 ------------------------------------------------
  uvcpp_http_response login_resp;
  check(http_get(port, "/login", "", login_resp), "GET /login");
  const std::string cookie = cookie_from(login_resp);
  check(!cookie.empty(), "登录响应带 Set-Cookie");

  bool        me_ok = false;
  std::string uid;
  for (int i = 0; i < 50; ++i) {
    uvcpp_http_response r;
    if (http_get(port, "/me", cookie, r) && r.status_code == http_status::OK) {
      uid = r.body.to_string();
      if (uid == "alice") {
        me_ok = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  check(me_ok, "带 Cookie 的 /me 读回 uid=alice");

  const std::string sid = cookie_value(cookie);
  check(!sid.empty(), "Cookie 里有 sid");

  // ---- 第二个实例：另一条循环上的第二个 store 读到同一会话 -------------
  {
    uvcpp_loop loop_b;
    std::shared_ptr<uvcpp_redis_session_store> store_b(
        new uvcpp_redis_session_store(&loop_b, url, ropts));
    std::string berr;
    check(store_b->start(&berr) == 0, "第二个 store.start");
    check(pump(loop_b, [&] { return store_b->ready(); }, 8000),
          "第二个实例连上 Redis");

    std::shared_ptr<uvcpp_session> loaded(new uvcpp_session());
    bool                          done = false;
    store_b->load(sid, *loaded, [&](bool found) { done = true; (void)found; });
    check(pump(loop_b, [&] { return done; }, 5000), "第二个实例 load 返回");
    check(loaded->get("uid") == "alice",
          "第二个实例看到 uid=alice（共享存储一致）");
    check(loaded->id() == sid, "会话 id 一致");
  }

  // ---- 注销之后旧 Cookie 失效 ------------------------------------------
  {
    uvcpp_http_response r;
    check(http_get(port, "/logout", cookie, r), "GET /logout");
    bool rejected = false;
    for (int i = 0; i < 50; ++i) {
      uvcpp_http_response me;
      if (http_get(port, "/me", cookie, me) &&
          me.status_code == http_status::UNAUTHORIZED) {
        rejected = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(rejected, "注销之后 /me 返回 401");
  }

  app.stop();
  app.join();

  if (g_fail == 0) {
    std::printf("[session_redis] ALL PASS\n");
    return 0;
  }
  std::printf("[session_redis] %d check(s) failed\n", g_fail);
  return 1;
}

#else

int main() {
  std::printf("SKIP: webapp / redis disabled\n");
  return 3;
}

#endif
