/**
 * @file tests/functional/web_app_ws_client_func.cpp
 * @brief 框架层 WebSocket 客户端（`uvcpp_web_ws_client`）的功能用例。
 *
 * 这个文件回答的问题
 * ------------------
 * 协议层的客户端（`uvcpp_ws_client`）已经有自己的用例（`web_ws_client_api_func`），
 * 本文件不重测协议，只测**框架层多出来的那三样**：
 *
 *   - 回调装在**客户端**上：`connect()` 之前装、之后装都得生效；
 *   - **自动重连**：服务端走了要不要自动回来、退避怎么算、次数用尽怎么办、
 *     使用者 `close()` 之后还连不连；
 *   - 重连之后回调**照旧生效**（每次都换了新的底层客户端，回调得重装一遍）。
 *
 * 驱动方式
 * --------
 * 服务端跑在 `start_background()` 自己的线程上（测试拨不动它），客户端的事件
 * 循环由**测试线程**拨（`cli.run(UV_RUN_NOWAIT)` + 1ms 睡眠 + 墙钟上限）。
 * 这与 `web_app_ws_func.cpp:851` 那条真客户端用例是同一套驱动方式 —— 本层
 * 重连的核心动作（换底层客户端）就发生在这种"一轮一轮拨"的缝里。
 *
 * 崩溃定位：每条用例先打名字再跑（`std::unitbuf`），进程中途挂掉也能从最后
 * 一行看出死在哪一条。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include <web/uvcpp_ws_client.h>
#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_ws.h>
#include <webapp/uvcpp_web_ws_client.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

std::string url_of(int port, const std::string& path = "/echo") {
  return "ws://127.0.0.1:" + std::to_string(port) + path;
}

/** @brief 拨客户端循环直到条件成立或超时（客户端循环归本线程）。 */
bool pump_cli(uvcpp_web_ws_client& cli, const std::function<bool()>& done,
              int timeout_ms = 5000) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!done()) {
    cli.run(UV_RUN_NOWAIT);
    if (done()) return true;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count() >= timeout_ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

/** @brief 原地拨 n 毫秒（用来证明"什么也没发生"）。 */
void pump_for(uvcpp_web_ws_client& cli, int ms) {
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    cli.run(UV_RUN_NOWAIT);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count() >= ms) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// =========================================================================
// 回显服务端（跑在自己的线程上）
// =========================================================================

struct echo_server {
  uvcpp_web_app app;
  std::atomic<int> upgrades{0};
  std::atomic<int> texts{0};

  /** @brief 起服务；\p port 传 0 由系统分配。返回实际端口，0 = 失败。 */
  int start(int port = 0) {
    app.set_port(port);
    app.websocket("/echo", [this](uvcpp_web_ws_request& ws) {
      upgrades.fetch_add(1);
      uvcpp_ws_connection* c = ws.connection();
      if (c == nullptr) return;
      c->on_text([this, c](const std::string& m) {
        texts.fetch_add(1);
        c->send_text(m.c_str(), m.size());
      });
    });
    if (app.start_background() != 0) return 0;
    return app.bound_port();
  }

  void stop() {
    app.stop();
    app.join();
  }
};

/**
 * @brief 起一个服务端拿一个"刚刚还在用、现在已经空出来"的端口。
 *
 * 用来制造"连得上但没人听"的场景：比随便挑一个端口可靠 —— 它刚刚确实被
 * 监听过，而监听者已经退干净了。
 */
int port_that_is_now_free() {
  echo_server s;
  const int port = s.start();
  if (port <= 0) return 0;
  s.stop();
  return port;
}

// =========================================================================
// 1. 回调在 connect 之前装：连上就生效
// =========================================================================
void test_callbacks_before_connect() {
  echo_server srv;
  const int port = srv.start();
  if (port <= 0) {
    check(false, "服务端起得来");
    return;
  }

  uvcpp_web_ws_client cli;
  int opens = 0;
  int got = 0;
  int closes = 0;
  int errors = 0;
  std::string last;
  uvcpp_ws_connection* opened = nullptr;

  // **connect 之前**装（异步的常见写法：先把 handler 备好再连）。
  cli.on_open([&](uvcpp_ws_connection* c) {
        ++opens;
        opened = c;
        cli.send_text("hello", 5);
      })
      .on_text([&](const std::string& m) {
        ++got;
        last = m;
      })
      .on_close([&](ws_close_code, const std::string&) { ++closes; })
      .on_error([&](int, const std::string&) { ++errors; });

  check(!cli.is_open(), "连之前 is_open() 为假");
  check(cli.session() == nullptr, "连之前没有会话");
  check(cli.url().empty(), "连之前 url() 为空串");

  const int rc = cli.connect(url_of(port));
  check(rc == 0, "connect() 返回 0");
  check(cli.url() == url_of(port), "connect() 之后 url() 是连的那条");

  const bool ok = pump_cli(cli, [&] { return got >= 1; });
  check(ok, "收到回显（超时前）");
  check(opens == 1, "on_open 恰好跑了一次（实际 " + std::to_string(opens) + "）");
  check(opened != nullptr && opened == cli.session(),
        "on_open 拿到的就是 session()");
  check(cli.is_open(), "连上之后 is_open() 为真");
  check(cli.get_status() == WS_CLIENT_OPEN, "状态是 WS_CLIENT_OPEN");
  check(last == "hello", "回显内容一致（实际 '" + last + "'）");
  check(errors == 0, "一路没有错误回调");

  // 使用者自己 close()：**不**走 on_close（那回答的是"对端怎么走的"）。
  cli.close();
  pump_for(cli, 200);
  check(!cli.is_open(), "close() 之后 is_open() 为假");
  check(cli.session() == nullptr, "close() 之后没有会话");
  check(closes == 0,
        "自己 close() 不触发 on_close（与协议层一致；实际 "
        + std::to_string(closes) + " 次）");
  check(srv.texts.load() == 1, "服务端收到 1 条文本");

  srv.stop();
}

// =========================================================================
// 2. 回调在 connect 之后装：照样生效（装在会话上）
// =========================================================================
void test_callbacks_after_connect() {
  echo_server srv;
  const int port = srv.start();
  if (port <= 0) {
    check(false, "服务端起得来");
    return;
  }

  uvcpp_web_ws_client cli;
  bool connected = false;
  const int rc = cli.connect(url_of(port), [&](int err) {
    connected = (err == 0);
  });
  check(rc == 0, "connect() 返回 0");
  check(pump_cli(cli, [&] { return connected; }), "连接回调报成功");
  check(cli.is_open(), "已经连上");

  // 连上**之后**才装收消息的回调。
  int got = 0;
  std::string last;
  cli.on_text([&](const std::string& m) {
    ++got;
    last = m;
  });
  check(cli.send_text("late", 4) == 0, "send_text 返回 0");
  check(pump_cli(cli, [&] { return got >= 1; }), "回显收得到");
  check(last == "late", "回显内容一致（实际 '" + last + "'）");

  cli.close();
  pump_for(cli, 100);
  srv.stop();
}

// =========================================================================
// 3. 没有会话时发送：**不静默丢**，返回 UV_ENOTCONN 且当场回调
// =========================================================================
void test_send_without_session() {
  uvcpp_web_ws_client cli;
  int cb_err = 0;
  int cb_calls = 0;
  const int rc = cli.send_text("nobody", 6, [&](int e) {
    ++cb_calls;
    cb_err = e;
  });
  check(rc == UV_ENOTCONN, "没有会话时 send_text 返回 UV_ENOTCONN");
  check(cb_calls == 1, "回调**当场**跑了一次（不是被丢掉）");
  check(cb_err == UV_ENOTCONN, "回调拿到的也是 UV_ENOTCONN");

  int bin_cb = 0;
  check(cli.send_binary("x", 1, [&](int e) {
          ++bin_cb;
          check(e == UV_ENOTCONN, "二进制那条也是 UV_ENOTCONN");
        }) == UV_ENOTCONN,
        "send_binary 同样返回 UV_ENOTCONN");
  check(bin_cb == 1, "二进制那条的回调也跑了");
}

// =========================================================================
// 4. 连不上：错误报上来，connect 回调**只跑一次**
// =========================================================================
void test_connect_refused() {
  const int port = port_that_is_now_free();
  if (port <= 0) {
    check(false, "拿得到一个空出来的端口");
    return;
  }

  uvcpp_web_ws_client cli;
  int cb_calls = 0;
  int cb_err = 0;
  int err_calls = 0;
  int last_err = 0;
  cli.on_error([&](int e, const std::string&) {
    ++err_calls;
    last_err = e;
  });

  cli.connect(url_of(port), [&](int e) {
    ++cb_calls;
    cb_err = e;
  });
  check(pump_cli(cli, [&] { return cb_calls >= 1; }, 4000), "失败被报出来");
  check(cb_err != 0, "connect 回调拿到的是错误码（实际 " +
                         std::to_string(cb_err) + "）");
  check(cb_err < 0, "是 libuv 的错误码（负数）");
  check(err_calls >= 1, "on_error 也报了");
  check(last_err < 0, "on_error 拿到的同样是 libuv 错误码");

  pump_for(cli, 200);
  check(cb_calls == 1,
        "connect 回调只跑一次，不因为后面的事重复（实际 "
        + std::to_string(cb_calls) + "）");
  check(!cli.is_open(), "没连上，is_open() 为假");
  check(cli.reconnect_attempts() == 0, "没开重连就不该有重连次数");
}

// =========================================================================
// 5. 各种状态下析构客户端：不许崩、不许把循环关不干净
// =========================================================================
//
// 这条用例的断言很弱（只能断言"没崩、状态自洽"）—— 因为它真正的对象是
// **析构序列**：没连过的、连失败的、连上又没关的，三种句柄状态都得能干净地
// 收掉。原先 `uvcpp_ws_client` 的析构对"句柄开着但不在跑"（`is_active()` 为
// 假）那种状态是**直接跳过关闭**的，于是 `uv_close` 落在了已经关掉的 loop 上
// —— 那是一次写已释放内存。这条用例钉住的就是那个序列。
void test_destroy_without_connect() {
  {
    uvcpp_web_ws_client cli;  // 从没 connect() 过
    check(cli.get_status() == WS_CLIENT_NONE, "没连过的状态是 WS_CLIENT_NONE");
    check(cli.get_loop() != nullptr, "没连过也有循环（构造时就建了）");
    check(cli.session() == nullptr, "没连过没有会话");
  }

  {
    const int port = port_that_is_now_free();
    uvcpp_web_ws_client cli;
    bool failed = false;
    cli.connect(url_of(port), [&](int e) { failed = (e != 0); });
    pump_cli(cli, [&] { return failed; }, 4000);
    check(failed, "连失败之后才析构（句柄开着但没在跑）");
  }

  {
    echo_server srv;
    const int port = srv.start();
    if (port > 0) {
      uvcpp_web_ws_client cli;
      bool connected = false;
      cli.connect(url_of(port), [&](int e) { connected = (e == 0); });
      pump_cli(cli, [&] { return connected; });
      check(connected, "连上之后直接析构（会话还开着）");
    }
    srv.stop();
  }
}

// =========================================================================
// 6. 服务端换了一个（同端口）：自动重连回来，回调照旧生效
// =========================================================================
void test_reconnect_after_restart() {
  echo_server srv1;
  const int port = srv1.start();
  if (port <= 0) {
    check(false, "第一个服务端起得来");
    return;
  }

  uvcpp_web_ws_client cli;
  int opens = 0;
  int got = 0;
  int closes = 0;
  int last_close_code = 0;
  int reconnects = 0;
  std::string last;

  cli.on_open([&](uvcpp_ws_connection*) {
        ++opens;
        // 每次连上（包括重连之后）都发一条 —— 用它证明新会话真的能用。
        cli.send_text("ping", 4);
      })
      .on_text([&](const std::string& m) {
        ++got;
        last = m;
      })
      .on_close([&](ws_close_code code, const std::string&) {
        ++closes;
        last_close_code = static_cast<int>(code);
      })
      .on_reconnect([&](int, int) { ++reconnects; });

  uvcpp_web_ws_reconnect rc;
  rc.enabled = true;
  rc.delay_ms = 50;
  rc.max_delay_ms = 200;
  rc.backoff = true;
  rc.max_attempts = 0;  // 不限
  cli.set_reconnect(rc);

  cli.connect(url_of(port));
  check(pump_cli(cli, [&] { return got >= 1; }), "第一次连上并收到回显");
  check(opens == 1, "on_open 跑过 1 次");

  // 把服务端整个停掉：对端（服务端）会发 Close 帧，客户端应当据此排重连。
  srv1.stop();
  check(pump_cli(cli, [&] { return closes >= 1; }),
        "对端走了，on_close 报了出来");
  check(last_close_code == static_cast<int>(ws_close_code::GOING_AWAY),
        "Close 码是 1001（实际 " + std::to_string(last_close_code) + "）");
  check(!cli.is_open(), "这时候没连上");

  // 同一个端口上起一个新的服务端。
  echo_server srv2;
  check(srv2.start(port) == port, "同一个端口能重新监听");

  check(pump_cli(cli, [&] { return opens >= 2 && got >= 2; }, 6000),
        "自动重连回来，并且新会话上又走通一次回显");
  check(reconnects >= 1, "on_reconnect 报过（实际 " +
                             std::to_string(reconnects) + " 次）");
  check(cli.reconnect_attempts() == 0, "连上之后重连次数清零");
  check(cli.is_open(), "重连之后 is_open() 为真");
  check(last == "ping", "重连之后的回显内容一致（实际 '" + last + "'）");
  check(closes == 1, "中间只报了一次对端关闭（实际 " +
                         std::to_string(closes) + "）");

  cli.close();
  pump_for(cli, 100);
  srv2.stop();
}

// =========================================================================
// 7. 使用者 close() 之后：**不再**重连
// =========================================================================
void test_close_cancels_reconnect() {
  const int port = port_that_is_now_free();
  if (port <= 0) {
    check(false, "拿得到一个空出来的端口");
    return;
  }

  uvcpp_web_ws_client cli;
  int reconnects = 0;
  cli.on_reconnect([&](int, int) { ++reconnects; });

  uvcpp_web_ws_reconnect rc;
  rc.enabled = true;
  rc.delay_ms = 40;
  rc.max_attempts = 0;
  cli.set_reconnect(rc);

  int cb_err = 0;
  cli.connect(url_of(port), [&](int e) { cb_err = e; });
  check(pump_cli(cli, [&] { return cb_err != 0; }, 4000), "第一次失败报了出来");
  check(cli.reconnect_attempts() == 1,
        "失败之后排了第 1 次重连（实际 " +
            std::to_string(cli.reconnect_attempts()) + "）");

  cli.close();
  const int after_close = cli.reconnect_attempts();
  // 排队的那次延迟 40ms —— 等 400ms 足够它跑 10 次了，一次都不该有。
  pump_for(cli, 400);
  check(cli.reconnect_attempts() == after_close,
        "close() 之后重连次数不再增长（" + std::to_string(after_close) +
            " → " + std::to_string(cli.reconnect_attempts()) + "）");
  check(reconnects <= 1, "close() 之后 on_reconnect 不再被叫（实际 " +
                             std::to_string(reconnects) + "）");
}

// =========================================================================
// 8. 重连的节奏：次数、退避、用尽之后报一次
// =========================================================================
void test_reconnect_backoff_and_exhaustion() {
  const int port = port_that_is_now_free();
  if (port <= 0) {
    check(false, "拿得到一个空出来的端口");
    return;
  }

  // --- 8a：不退避，等多久就是多久 ---
  {
    uvcpp_web_ws_client cli;
    std::vector<int> attempts;
    std::vector<int> delays;
    int exhausted = 0;
    cli.on_reconnect([&](int a, int d) {
          attempts.push_back(a);
          delays.push_back(d);
        })
        .on_error([&](int e, const std::string&) {
          if (e == WEB_WS_ERR_RECONNECT_EXHAUSTED) ++exhausted;
        });

    uvcpp_web_ws_reconnect rc;
    rc.enabled = true;
    rc.delay_ms = 30;
    rc.backoff = false;
    rc.max_attempts = 3;
    cli.set_reconnect(rc);

    cli.connect(url_of(port));
    check(pump_cli(cli, [&] { return exhausted >= 1; }, 6000),
          "次数用尽之后报了一次 WEB_WS_ERR_RECONNECT_EXHAUSTED");
    check(attempts.size() == 3, "排了 3 次（实际 " +
                                    std::to_string(attempts.size()) + "）");
    check(!attempts.empty() && attempts[0] == 1, "次数从 1 起");
    bool all30 = !delays.empty();
    for (size_t i = 0; i < delays.size(); ++i) {
      if (delays[i] != 30) all30 = false;
    }
    check(all30, "backoff=false 时每次等待都是 delay_ms");
    check(cli.reconnect_attempts() == 3, "用尽之后次数不再增长");
    pump_for(cli, 100);
    check(exhausted == 1, "用尽只报一次（实际 " + std::to_string(exhausted) + "）");
  }

  // --- 8b：指数退避 + 上限 ---
  {
    uvcpp_web_ws_client cli;
    std::vector<int> delays;
    cli.on_reconnect([&](int, int d) { delays.push_back(d); });

    uvcpp_web_ws_reconnect rc;
    rc.enabled = true;
    rc.delay_ms = 20;
    rc.max_delay_ms = 80;
    rc.backoff = true;
    rc.max_attempts = 5;
    cli.set_reconnect(rc);

    cli.connect(url_of(port));
    check(pump_cli(cli, [&] { return delays.size() >= 5; }, 8000),
          "排满了 5 次");
    // 20 → 40 → 80（封顶）→ 80 → 80
    const int expect[5] = {20, 40, 80, 80, 80};
    bool ok = delays.size() == 5;
    for (int i = 0; ok && i < 5; ++i) ok = (delays[i] == expect[i]);
    std::string got;
    for (size_t i = 0; i < delays.size(); ++i) {
      if (i) got += ",";
      got += std::to_string(delays[i]);
    }
    check(ok, "退避是 20,40,80,80,80（实际 " + got + "）");
  }
}

// =========================================================================
// 9. connect_wait：阻塞版能连上，连不上就报错
// =========================================================================
void test_connect_wait() {
  echo_server srv;
  const int port = srv.start();
  if (port > 0) {
    uvcpp_web_ws_client cli;
    check(cli.connect_wait(url_of(port), 5000) == 0, "connect_wait 返回 0");
    check(cli.is_open(), "connect_wait 返回时已经连上");
    int got = 0;
    cli.on_text([&](const std::string&) { ++got; });
    cli.send_text("sync", 4);
    check(pump_cli(cli, [&] { return got >= 1; }), "阻塞连上之后照样能收发");
    cli.close();
    pump_for(cli, 100);
  }

  const int dead = port_that_is_now_free();
  if (dead > 0) {
    uvcpp_web_ws_client cli;
    const int rc = cli.connect_wait(url_of(dead), 5000);
    check(rc != 0, "连不上时 connect_wait 返回非 0");
    check(rc < 0, "是 libuv 的错误码（实际 " + std::to_string(rc) + "）");
    check(!cli.is_open(), "没连上");
  }

  srv.stop();
}

// =========================================================================
// 10. 连接还在建立中就被 close()：算取消，且**不再**重连
// =========================================================================

/**
 * 与用例 7 的区别：那里 close() 之前 connect 已经失败并结算过了，本用例
 * **一次循环都还没拨**就 close() —— 连接还在建立中。此时协议层按"取消"结算
 * （`UV_ECANCELED`），而本层必须把这次取消和"连不上"分开：使用者自己取消的
 * 不报 `on_error`、也不许再排重连。少了这条，`close()` 之后那条迟到的取消
 * 回调会自己把重连又排起来 —— 使用者就永远关不掉一个开着重连的客户端。
 */
void test_close_during_connect_cancels() {
  echo_server srv;
  const int port = srv.start();
  if (port <= 0) {
    check(false, "服务端起得来");
    return;
  }

  uvcpp_web_ws_client cli;
  int opens = 0;
  int errors = 0;
  int reconnects = 0;
  int cb_calls = 0;
  int cb_err = 0;
  cli.on_open([&](uvcpp_ws_connection*) { ++opens; })
      .on_error([&](int, const std::string&) { ++errors; })
      .on_reconnect([&](int, int) { ++reconnects; });

  uvcpp_web_ws_reconnect rc;
  rc.enabled = true;
  rc.delay_ms = 40;
  rc.max_attempts = 0;
  cli.set_reconnect(rc);

  cli.connect(url_of(port), [&](int e) {
    ++cb_calls;
    cb_err = e;
  });
  check(!cli.is_open(), "还没拨循环，会话还不存在");
  cli.close();

  check(pump_cli(cli, [&] { return cb_calls >= 1; }, 4000),
        "建立中被 close()：connect 回调照样结算");
  check(cb_err == UV_ECANCELED,
        "结算成 UV_ECANCELED（实际 " + std::to_string(cb_err) + "）");
  check(opens == 0, "被取消的连接不报 on_open");

  // 排队那次是 40ms —— 等 300ms 足够它跑 7 次，一次都不该有。
  pump_for(cli, 300);
  check(errors == 0, "使用者自己取消的不报 on_error（实际 " +
                         std::to_string(errors) + " 次）");
  check(reconnects == 0 && cli.reconnect_attempts() == 0,
        "取消之后不会自己重连（on_reconnect " + std::to_string(reconnects) +
            " 次，attempts " + std::to_string(cli.reconnect_attempts()) + "）");
  srv.stop();
}

// =========================================================================
// 11. 用 run(UV_RUN_DEFAULT) 驱动整条重连链路
// =========================================================================

/** @brief 有界轮询一个由**别的线程**更新的条件（本线程不拨任何循环）。 */
bool wait_for(const std::function<bool()>& done, int ms) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!done()) {
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count() >= ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

/**
 * 其余用例都用 `run(UV_RUN_NOWAIT)` 自己拨循环（测试里好控制），但**生产写法
 * 是 `cli.run()`**。本层在 `UV_RUN_DEFAULT` 里不是简单转发：它按"一次一轮"
 * 驱动，好让**换底层客户端**这个动作落在两次循环之间（`uv_run` 不可重入）。
 * 也就是说，只有这条路才走得到那段泵循环 —— 本用例专门走它。
 *
 * 客户端跑在**自己的线程**上（`run(UV_RUN_DEFAULT)` 本来就该这么用），测试
 * 线程只负责停/起服务端。这样"对端掉线"发生在客户端循环正跑着的时候 ——
 * 别在**没人拨循环**的时候去停服务端：服务端停机要等对端把关闭握手走完，
 * 而对端循环停着就永远走不完，测试会挂在那里。
 */
void test_run_default_drives_reconnect() {
  echo_server srv1;
  const int port = srv1.start();
  if (port <= 0) {
    check(false, "服务端起得来");
    return;
  }

  // 堆上分配：重连没发生时这条用例要在**不 join、不析构**的前提下收场
  // （理由见下面的失败路径），所以得能故意把它漏掉。
  std::unique_ptr<uvcpp_web_ws_client> cli(new uvcpp_web_ws_client());
  std::atomic<int> opens{0};
  std::atomic<int> got{0};
  std::atomic<int> closes{0};
  cli->on_open([&](uvcpp_ws_connection*) {
        // 每次连上（第一次和重连之后）都发一条，好证明这条链路是通的。
        cli->send_text(opens.fetch_add(1) + 1 == 1 ? "a" : "b", 1);
      })
      .on_text([&](const std::string&) {
        // 第二次回显到手 = 重连链路走通了 → 让 run() 返回。
        if (got.fetch_add(1) + 1 >= 2) cli->stop();
      })
      .on_close([&](ws_close_code, const std::string&) { closes.fetch_add(1); });

  uvcpp_web_ws_reconnect rc;
  rc.enabled = true;
  rc.delay_ms = 50;
  rc.max_delay_ms = 50;
  rc.max_attempts = 0;
  cli->set_reconnect(rc);

  cli->connect(url_of(port));
  std::thread driver([&] { cli->run(); });  // 生产写法：UV_RUN_DEFAULT

  check(wait_for([&] { return opens.load() >= 1 && got.load() >= 1; }, 5000),
        "run(UV_RUN_DEFAULT) 驱动第一次连接与回显");

  // 对端走了 —— 客户端此刻正在自己的循环里跑着。
  srv1.stop();
  echo_server srv2;
  check(srv2.start(port) == port, "同一个端口能重新监听");

  // 掉线 → 排重连 → 定时器到期 → **换底层客户端** → 连上 srv2 → 回显，
  // 全过程都在同一次 run() 调用里。
  const bool reconnected =
      wait_for([&] { return opens.load() >= 2 && got.load() >= 2; }, 8000);
  check(reconnected, "重连之后又连上并走通一次回显（opens " +
                         std::to_string(opens.load()) + " got " +
                         std::to_string(got.load()) + "）");

  if (!reconnected) {
    // run() 的唯一出口是使用者主动 stop()（`on_text` 里那句）。重连没发生的
    // 时候它就一直不出来：底层循环还活着，而 `UV_RUN_ONCE` 在没有定时器时是
    // **无超时 poll** —— 实测（2026-09-17）跨线程 `stop()` 也唤不醒它，
    // `uv_stop` 只置标志、不踢 poll。于是 join() 会挂死。
    //
    // 这里**故意**既不 join 也不析构：detach 让 driver 线程随进程消失，
    // 漏掉 cli 免得在它还在跑循环的时候析构。用例已经报过 FAIL，进程随后
    // 带着失败码退出 —— 把"挂死"换成"失败"，这是本用例能给出的最诚实的
    // 收场。（同一条性质在正式代码里也存在：关掉重连、对端走掉之后，
    // `run()` 同样不会自己返回。）
    driver.detach();
    cli.release();
    return;
  }

  driver.join();  // on_text 里 stop() 过，run() 已经返回
  check(closes.load() >= 1, "中间报过对端关闭");
  check(cli->reconnect_attempts() == 0, "连上之后重连次数清零");
  check(cli->is_open(), "重连之后 is_open() 为真");

  cli->close();
  pump_for(*cli, 200);  // 把关闭握手走完再停服务端（理由同函数注释）
  srv2.stop();
}

struct test_case {
  const char* name;
  void (*fn)();
};

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;

  std::string only;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
      only = argv[++i];
    }
  }

  const test_case tests[] = {
      {"callbacks_before_connect", test_callbacks_before_connect},
      {"callbacks_after_connect", test_callbacks_after_connect},
      {"send_without_session", test_send_without_session},
      {"connect_refused", test_connect_refused},
      {"destroy_without_connect", test_destroy_without_connect},
      {"reconnect_after_restart", test_reconnect_after_restart},
      {"close_cancels_reconnect", test_close_cancels_reconnect},
      {"reconnect_backoff_and_exhaustion",
       test_reconnect_backoff_and_exhaustion},
      {"connect_wait", test_connect_wait},
      {"close_during_connect_cancels", test_close_during_connect_cancels},
      {"run_default_drives_reconnect", test_run_default_drives_reconnect},
  };
  const int count = static_cast<int>(sizeof(tests) / sizeof(tests[0]));

  int ran = 0;
  for (int i = 0; i < count; ++i) {
    if (!only.empty() && only != tests[i].name) continue;
    ++ran;
    std::cout << "[" << tests[i].name << "] " << std::flush;
    const int before = g_failures;
    tests[i].fn();
    std::cout << (g_failures == before ? "PASS" : "FAIL") << std::endl;
  }

  if (ran == 0) {
    std::cerr << "[web_app_ws_client] --only " << only << " 没匹配到任何用例"
              << std::endl;
    return 2;
  }

  if (g_failures == 0) {
    std::cout << "[web_app_ws_client] ALL PASS (" << ran << " cases)"
              << std::endl;
    return 0;
  }
  std::cout << "[web_app_ws_client] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}

#else

#include <iostream>

int main() {
  std::cerr << "[web_app_ws_client] UVCPP_WEBAPP_ENABLE=0 —— 构建配置有问题，"
               "这个测试文件不该被编译进来" << std::endl;
  return 2;
}

#endif  // UVCPP_WEBAPP_ENABLE
