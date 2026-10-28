/**
 * @file tests/functional/web_app_trailers_func.cpp
 * @brief h1 上的尾随头：`set_trailers()` 走 chunked，字段挂在终止块之后。
 *
 * 1.9.0 之前 h1 的尾随头是"丢弃 + 一句 WARN"（`send_trailers()` 在 h1 上返
 * `UV_ENOTSUP`）。现在框架自己把响应改成 `transfer-encoding: chunked`、body
 * 按 chunk 发出、字段跟在 `0\r\n` 后面（RFC 9112 §7.1.2）。这一份用例钉的是
 * **线上字节**，不是"调用返回 0"：
 *
 *   1. 有 body：`<hex>\r\n` + body + `\r\n`，再 `0\r\n` + 字段 + `\r\n`；
 *      首部里**没有** `content-length`（CL 与 TE 不得共存），字段也**不在**
 *      首部里（它必须是第二段）；
 *   2. 空 body：只发终止块，前面一个 chunk 都没有；
 *   3. HEAD：字段被丢弃，响应回到普通 h1 形状（有 `content-length`、无 TE）
 *      —— HEAD 没有末块可挂；
 *   4. 204：同样丢弃（1xx/204/304 不许 chunked）；
 *   5. 同一条连接上的下一条请求照常得到响应 —— 终止块真的把报文切干净了，
 *      keep-alive 没有因为尾随头而错位。
 *
 * 文件名里的 `web_app_` 是**必须的**（`tests/functional/CMakeLists.txt` 的命名
 * 约定）：这条用例要 webapp 层，而 webapp 是可选模块且默认关。
 */

#include <chrono>
#include <functional>
#include <iostream>
#include <string>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include <net/uvcpp_tcp_client.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (cond) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void info(const std::string& what) {
  std::cout << "  [info] " << what << std::endl;
}

std::string hexsp(const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  std::string        out;
  const size_t       n = s.size() < 24 ? s.size() : 24;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (i != 0) out += ' ';
    out += kHex[c >> 4];
    out += kHex[c & 0x0F];
  }
  if (s.size() > n) out += " ...";
  return out;
}

std::string to_lower(const std::string& s) {
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] + 32);
  }
  return out;
}

long long ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

http_headers make_trailers() {
  http_headers tr;
  http_header  h;
  h.name  = "x-trailer-1";
  h.value = "alpha";
  tr.push_back(h);
  // 空值字段也要发出来：`name: \r\n` 是合法的，而"没发"与"发了个空值"在
  // 别的地方长得一模一样。
  h.name  = "x-trailer-2";
  h.value = "";
  tr.push_back(h);
  return tr;
}

// =========================================================================
// 服务端
// =========================================================================

struct TestApp {
  uvcpp_web_app app;

  void build() {
    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN);

    app.get("/with-body",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.text("hello");
              resp.set_trailers(make_trailers());
            });

    app.get("/empty",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.set_trailers(make_trailers());
            });

    // 同一条 GET 路由也接 HEAD（路由层的 `head_of_get`）。HEAD 上尾随头必须
    // 被丢弃，头部与 GET 的形状一致（含 content-length、无 TE）。
    app.get("/head",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.text("hello");
              resp.set_trailers(make_trailers());
            });

    app.get("/no-body",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.status(http_status::NO_CONTENT);
              resp.set_trailers(make_trailers());
            });

    app.get("/plain",
            [](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
              resp.text("plain");
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

// =========================================================================
// 客户端（阻塞、真 socket）
// =========================================================================

/// 把 @p req 写出去，累积读到 @p pred 成立或超时。连接由调用方持有，所以
/// keep-alive 那一条可以在同一条连接上接着发下一条请求。
bool read_until(uvcpp_tcp_client& c, std::string& got,
                const std::function<bool(const std::string&)>& pred,
                int deadline_ms) {
  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  for (;;) {
    if (pred(got)) return true;
    if (ms_since(t0) >= deadline_ms) return false;

    uvcpp_buf b;
    const int rv = c.read_wait(b, 30);
    if (rv == 0) {
      if (b.size() > 0 && b.get_const_data() != nullptr) {
        got.append(b.get_const_data(), b.size());
      }
    } else if (rv != UV_ETIMEDOUT) {
      return false;
    }
  }
}

bool one_shot(int port, const std::string& req, std::string& got,
              const std::function<bool(const std::string&)>& pred,
              int deadline_ms) {
  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;
  if (c.write_wait(req.data(), req.size(), 3000) != 0) return false;
  return read_until(c, got, pred, deadline_ms);
}

std::string get_req(const char* method, const char* path) {
  return std::string(method) + " " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

// =========================================================================
// 判据
// =========================================================================

/// 1) 有 body：chunk 帧 + 终止块 + 字段，且字段不在首部里。
void test_body_and_trailers() {
  std::cout << "[1] h1：body 按 chunk 发，字段挂在终止块之后" << std::endl;

  TestApp   t;
  t.build();
  const int port = t.start();
  if (port <= 0) {
    check(false, "uvcpp_web_app 起来了");
    return;
  }

  const std::string kTerminal =
      "0\r\nx-trailer-1: alpha\r\nx-trailer-2: \r\n\r\n";
  std::string got;
  const bool  ok = one_shot(
      port, get_req("GET", "/with-body"), got,
      [&kTerminal](const std::string& s) {
        return s.find(kTerminal) != std::string::npos;
      },
      3000);

  const std::string low = to_lower(got);
  info("收到的前 64 字节：" + hexsp(got));

  check(ok, "收到了终止块 + 两段尾随字段");
  check(got.compare(0, 12, "HTTP/1.1 200") == 0, "状态码 200");
  check(got.find("5\r\nhello\r\n") != std::string::npos,
        "body 是一个 chunk：`5\\r\\n` + hello + `\\r\\n`");
  check(low.find("transfer-encoding: chunked") != std::string::npos,
        "首部里有 transfer-encoding: chunked");
  check(low.find("content-length") == std::string::npos,
        "首部里**没有** content-length（CL 与 TE 不得共存）");

  const size_t hdr_end = got.find("\r\n\r\n");
  const size_t tr      = got.find("x-trailer-1");
  check(hdr_end != std::string::npos && tr != std::string::npos && tr > hdr_end,
        "尾随字段在首部之后（是第二段，不是普通响应头）");

  t.shutdown();
}

/// 2) 空 body：只有终止块。
void test_trailers_only() {
  std::cout << "[2] h1：空 body 的尾随头（只有终止块）" << std::endl;

  TestApp   t;
  t.build();
  const int port = t.start();
  if (port <= 0) {
    check(false, "uvcpp_web_app 起来了");
    return;
  }

  const std::string kTerminal =
      "0\r\nx-trailer-1: alpha\r\nx-trailer-2: \r\n\r\n";
  std::string got;
  const bool  ok = one_shot(
      port, get_req("GET", "/empty"), got,
      [&kTerminal](const std::string& s) {
        return s.find(kTerminal) != std::string::npos;
      },
      3000);

  const std::string low = to_lower(got);
  info("收到的前 64 字节：" + hexsp(got));

  check(ok, "空 body 的响应也把字段发出来了");
  check(low.find("transfer-encoding: chunked") != std::string::npos,
        "空 body 也走 chunked（否则没有末块可挂字段）");
  check(low.find("content-length") == std::string::npos,
        "没有 content-length");
  // 终止块前面不该有 chunk。`0\r\n` 之前紧挨着的是首部的 `\r\n\r\n`。
  const size_t p = got.find(kTerminal);
  check(p != std::string::npos && p >= 4 &&
            got.compare(p - 4, 4, "\r\n\r\n") == 0,
        "`0\\r\\n` 紧接在首部之后（中间没有数据块）");

  t.shutdown();
}

/// 3) HEAD：字段丢弃，老老实实回到 CL 模式。
void test_head_drops_trailers() {
  std::cout << "[3] h1：HEAD 上尾随头被丢弃" << std::endl;

  TestApp   t;
  t.build();
  const int port = t.start();
  if (port <= 0) {
    check(false, "uvcpp_web_app 起来了");
    return;
  }

  std::string got;
  const bool  ok = one_shot(
      port, get_req("HEAD", "/head"), got,
      [](const std::string& s) { return s.find("\r\n\r\n") != std::string::npos; },
      3000);

  const std::string low = to_lower(got);
  info("收到的前 64 字节：" + hexsp(got));

  check(ok, "HEAD 收到了响应头");
  check(got.compare(0, 12, "HTTP/1.1 200") == 0, "状态码 200");
  check(low.find("content-length: 5") != std::string::npos,
        "HEAD 的 content-length 描述 GET 会发的长度（5）");
  check(low.find("transfer-encoding") == std::string::npos,
        "没有 transfer-encoding（HEAD 不发末块）");
  check(low.find("x-trailer-1") == std::string::npos,
        "尾随字段没有漏进 HEAD 的响应");

  t.shutdown();
}

/// 4) 204：同样丢弃。
void test_204_drops_trailers() {
  std::cout << "[4] h1：204 上尾随头被丢弃" << std::endl;

  TestApp   t;
  t.build();
  const int port = t.start();
  if (port <= 0) {
    check(false, "uvcpp_web_app 起来了");
    return;
  }

  std::string got;
  const bool  ok = one_shot(
      port, get_req("GET", "/no-body"), got,
      [](const std::string& s) { return s.find("\r\n\r\n") != std::string::npos; },
      3000);

  const std::string low = to_lower(got);
  info("收到的前 64 字节：" + hexsp(got));

  check(ok, "204 收到了响应头");
  check(got.compare(0, 12, "HTTP/1.1 204") == 0, "状态码 204");
  check(low.find("transfer-encoding") == std::string::npos,
        "没有 transfer-encoding（204 不许 chunked）");
  check(low.find("content-length") == std::string::npos,
        "没有 content-length（RFC 9110 §15.3.5 禁止 204 带 CL）");
  check(low.find("x-trailer-1") == std::string::npos,
        "尾随字段被丢弃");

  t.shutdown();
}

/// 5) keep-alive：终止块之后，同一条连接的下一条请求照常。
void test_keep_alive_after_trailers() {
  std::cout << "[5] h1：带尾随头的响应之后，同一条连接还能接着用" << std::endl;

  TestApp   t;
  t.build();
  const int port = t.start();
  if (port <= 0) {
    check(false, "uvcpp_web_app 起来了");
    return;
  }

  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) {
    check(false, "连上服务端");
    t.shutdown();
    return;
  }

  const std::string kTerminal =
      "0\r\nx-trailer-1: alpha\r\nx-trailer-2: \r\n\r\n";
  std::string first;
  const std::string req1 = get_req("GET", "/with-body");
  check(c.write_wait(req1.data(), req1.size(), 3000) == 0, "写出第 1 条请求");
  check(read_until(
            c, first,
            [&kTerminal](const std::string& s) {
              return s.find(kTerminal) != std::string::npos;
            },
            3000),
        "第 1 条响应完整（终止块 + 字段）");

  // 第 1 条的字节里**恰好**一个终止块：多一个就说明报文没切干净。
  size_t hits = 0;
  for (size_t p = first.find(kTerminal); p != std::string::npos;
       p = first.find(kTerminal, p + 1)) {
    ++hits;
  }
  check(hits == 1, "第 1 条响应里恰好一个终止块（没有多发）");

  const std::string req2 = get_req("GET", "/plain");
  check(c.write_wait(req2.data(), req2.size(), 3000) == 0,
        "在同一条连接上写出第 2 条请求");
  std::string second;
  check(read_until(
            c, second,
            [](const std::string& s) {
              return s.find("plain") != std::string::npos;
            },
            3000),
        "第 2 条请求拿到了响应（keep-alive 没有错位）");
  check(second.compare(0, 12, "HTTP/1.1 200") == 0, "第 2 条也是 200");
  check(second.find("plain") != std::string::npos, "第 2 条的 body 正确");
  check(second.find("x-trailer-1") == std::string::npos,
        "第 2 条响应里没有上一条的尾随字段");

  t.shutdown();
}

}  // namespace

int main() {
  std::cout << "web_app 的 h1 尾随头（chunked 末块带字段）" << std::endl;

  test_body_and_trailers();
  test_trailers_only();
  test_head_drops_trailers();
  test_204_drops_trailers();
  test_keep_alive_after_trailers();

  if (g_failures == 0) {
    std::cout << "全部通过" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " 条失败" << std::endl;
  return 1;
}

#else  // !UVCPP_WEBAPP_ENABLE

int main() {
  std::cout << "SKIP: webapp 未启用" << std::endl;
  return 0;
}

#endif
