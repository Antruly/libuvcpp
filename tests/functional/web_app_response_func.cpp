/**
 * @file tests/functional/web_app_response_func.cpp
 * @brief `uvcpp_web_response` 的功能测试。
 *
 * 纯单元测试，不起网络 —— 这一层的产出是**报文**，直接对
 * `raw().to_string()` 的字节做断言比起一个服务端跑往返更快，也更不容易
 * 被端口/时序问题干扰。
 *
 * 这个文件里有三条断言是**回归护栏**，对应协议层三个真实的坑，改动
 * `sync_meta()` / `add_header()` 时必须仍然全绿：
 *
 *  1. 空 body 的 200 必须发出 `Content-Length: 0`（协议层的条件是
 *     `body.size() > 0`，空体时一个长度头都不发）；
 *  2. 多条 `Set-Cookie` 必须都在报文里（协议层的 set_header 是覆盖语义，
 *     第二条会把第一条顶掉）；
 *  3. 204 / 304 / 1xx 不得自动带 `Content-Length`（RFC 7230 §3.3.2）。
 */
#include <iostream>
#include <stdexcept>
#include <string>

#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_util.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// 序列化成线格式。走非 const 的 raw()，顺带验证 sync_meta 确实被调了。
std::string wire(uvcpp_web_response& r) { return r.raw().to_string(); }

/// 数一数某个头出现了几次（大小写不敏感，按行首匹配）。
int count_header(const std::string& w, const std::string& name) {
  const std::string needle = web_to_lower(name) + ":";
  std::string lower = web_to_lower(w);
  int n = 0;
  size_t pos = 0;
  while ((pos = lower.find(needle, pos)) != std::string::npos) {
    // 只算行首的，避免把 body 里的同名文本算进来。
    if (pos == 0 || lower[pos - 1] == '\n') ++n;
    pos += needle.size();
  }
  return n;
}

bool has(const std::string& w, const std::string& s) {
  return w.find(s) != std::string::npos;
}

/// 取出 body 部分（第一个 \r\n\r\n 之后）。
std::string wire_body(const std::string& w) {
  size_t p = w.find("\r\n\r\n");
  if (p == std::string::npos) return std::string();
  return w.substr(p + 4);
}

// =========================================================================
// 1. 状态行与 reason phrase
// =========================================================================
void test_status_line() {
  uvcpp_web_response r;
  check(r.status_code() == 200, "默认状态码是 200");

  uvcpp_web_response a;
  a.status(404);
  check(a.status_code() == 404, "status(int) 生效");
  check(has(wire(a), "HTTP/1.1 404 Not Found\r\n"), "404 带正确 reason");

  uvcpp_web_response b;
  b.status(http_status::CREATED);
  check(b.status_code() == 201, "status(enum) 生效");
  check(has(wire(b), "201 Created\r\n"), "201 reason 正确");

  uvcpp_web_response c;
  c.status(599);
  check(c.status_code() == 599, "未知状态码仍能设置");
  check(has(wire(c), "599 Unknown\r\n"), "未知状态码 reason 是 Unknown");

  uvcpp_web_response d;
  d.status(200).status_message("Totally Fine");
  check(has(wire(d), "200 Totally Fine\r\n"), "可以覆盖 reason phrase");
}

// =========================================================================
// 2. 回归护栏 1：空 body 必须显式发 Content-Length: 0
// =========================================================================
void test_empty_body_content_length() {
  uvcpp_web_response r;
  r.status(200);
  const std::string w = wire(r);
  check(count_header(w, "content-length") == 1,
        "空 body 的 200 必须发恰好一条 Content-Length");
  check(has(w, "content-length: 0\r\n"), "空 body 的 200 的 CL 必须是 0");
  check(wire_body(w).empty(), "空 body 的 200 确实没有 body");

  // 空 body 但设了 content-type 也一样要发。
  uvcpp_web_response r2;
  r2.status(200).json_str("");
  const std::string w2 = wire(r2);
  check(has(w2, "content-length: 0\r\n"), "空 JSON body 也发 CL: 0");

  // 非空 body 的长度必须准确。
  uvcpp_web_response r3;
  r3.text("hello");
  check(has(wire(r3), "content-length: 5\r\n"), "text body 的 CL 准确");

  // 二进制 body（含 NUL）按字节数算，不是按 C 字符串长度。
  const char bin[] = {'a', '\0', 'b', '\0', 'c'};
  uvcpp_web_response r4;
  r4.binary(bin, sizeof(bin));
  check(has(wire(r4), "content-length: 5\r\n"), "含 NUL 的 body 按字节数计长");
  check(wire_body(wire(r4)).size() == 5, "含 NUL 的 body 完整写出");
}

// =========================================================================
// 3. 回归护栏 3：不允许 body 的状态码
// =========================================================================
void test_bodyless_statuses() {
  uvcpp_web_response r204;
  r204.no_content();
  const std::string w204 = wire(r204);
  check(has(w204, "HTTP/1.1 204 No Content\r\n"), "204 状态行正确");
  check(count_header(w204, "content-length") == 0,
        "204 不得自动带 Content-Length（RFC 7230 3.3.2）");
  check(wire_body(w204).empty(), "204 没有 body");

  // 就算给了 body 也要丢掉。
  uvcpp_web_response r204b;
  r204b.text("should not appear").no_content();
  const std::string w204b = wire(r204b);
  check(wire_body(w204b).empty(), "204 会丢弃被设置的 body");
  check(count_header(w204b, "content-length") == 0,
        "204 丢弃 body 后仍不带 CL");

  // 304 同理，但使用者**显式**设的 CL 要保留（回显原始长度是常见做法）。
  uvcpp_web_response r304;
  r304.status(304).set_header("content-length", "1234");
  const std::string w304 = wire(r304);
  check(has(w304, "content-length: 1234\r\n"), "304 保留使用者显式设的 CL");
  check(wire_body(w304).empty(), "304 没有 body");

  // 1xx
  uvcpp_web_response r1xx;
  r1xx.status(100);
  check(count_header(wire(r1xx), "content-length") == 0, "1xx 不带 CL");
}

// =========================================================================
// 4. HEAD：保留长度，丢掉 body
// =========================================================================
void test_head_only() {
  uvcpp_web_response r;
  r.text("hello world");  // 11 字节
  r.set_head_only(true);
  const std::string w = wire(r);
  check(has(w, "content-length: 11\r\n"),
        "HEAD 的 CL 反映 GET 应有的长度");
  check(!has(w, "hello world"), "HEAD 不得发出 body 字节");
  check(wire_body(w).empty(), "HEAD 的 body 段为空");

  // 长度是在 sync_meta()（发送前）才算的，所以和调 set_head_only 的先后
  // 无关 —— 反过来写同样得到 11。把这条钉住，免得以后有人把长度计算挪到
  // set_head_only() 里去。
  uvcpp_web_response r2;
  r2.set_head_only(true);
  r2.text("hello world");
  check(has(wire(r2), "content-length: 11\r\n"),
        "先 head_only 后设 body，长度依然正确（顺序无关）");
  check(wire_body(wire(r2)).empty(), "先 head_only 后设 body，body 照样被丢");

  // 204 + HEAD：仍然不带 CL。
  uvcpp_web_response r3;
  r3.no_content();
  r3.set_head_only(true);
  check(count_header(wire(r3), "content-length") == 0, "204 + HEAD 仍不带 CL");
}

// =========================================================================
// 5. 回归护栏 2：多条 Set-Cookie
// =========================================================================
void test_multiple_set_cookie() {
  uvcpp_web_response r;
  r.set_cookie("a", "1").set_cookie("b", "2").set_cookie("c", "3");
  const std::string w = wire(r);
  check(count_header(w, "set-cookie") == 3,
        "三条 Set-Cookie 必须都在报文里（覆盖语义会只剩一条）");
  check(has(w, "a=1"), "第一条 cookie 在");
  check(has(w, "b=2"), "第二条 cookie 在");
  check(has(w, "c=3"), "第三条 cookie 在");

  // 普通单值头仍然走覆盖语义。
  uvcpp_web_response r2;
  r2.set_header("x-a", "1").set_header("x-a", "2");
  check(count_header(wire(r2), "x-a") == 1, "set_header 仍是覆盖语义");
  check(has(wire(r2), "x-a: 2"), "set_header 覆盖后是后一个值");

  // add_header 是追加。
  uvcpp_web_response r3;
  r3.add_header("x-b", "1").add_header("x-b", "2");
  check(count_header(wire(r3), "x-b") == 2, "add_header 是追加语义");
}

// =========================================================================
// 6. Cookie 属性
// =========================================================================
void test_cookie_attributes() {
  uvcpp_web_response r;
  r.set_cookie("sid", "abc", "/app", 3600, true, true, "Strict");
  const std::string w = wire(r);
  check(has(w, "sid=abc"), "cookie 名值正确");
  check(has(w, "Path=/app"), "Path 正确");
  check(has(w, "Max-Age=3600"), "Max-Age 正确");
  check(has(w, "HttpOnly"), "HttpOnly 正确");
  check(has(w, "Secure"), "Secure 正确");
  check(has(w, "SameSite=Strict"), "SameSite 正确");

  // max_age < 0 表示会话 cookie，不该输出 Max-Age。
  uvcpp_web_response r2;
  r2.set_cookie("s", "v", "/", -1, false, false, "");
  check(!has(wire(r2), "Max-Age"), "max_age < 0 不输出 Max-Age");
  check(!has(wire(r2), "HttpOnly"), "http_only=false 不输出 HttpOnly");

  // SameSite=None 必须自动补 Secure，否则浏览器整条丢弃。
  uvcpp_web_response r3;
  r3.set_cookie("s", "v", "/", -1, false, false, "None");
  const std::string w3 = wire(r3);
  check(has(w3, "SameSite=None"), "SameSite=None 输出");
  check(has(w3, "Secure"), "SameSite=None 自动补 Secure");

  // 删除 cookie。
  uvcpp_web_response r4;
  r4.clear_cookie("sid");
  const std::string w4 = wire(r4);
  check(has(w4, "Max-Age=0"), "clear_cookie 用 Max-Age=0");
  check(has(w4, "sid="), "clear_cookie 值置空");
}

// =========================================================================
// 7. 状态码 helper
// =========================================================================
void test_status_helpers() {
  // 单独调用 -> 补一段可读的默认文本 body。
  uvcpp_web_response r;
  r.not_found();
  const std::string w = wire(r);
  check(has(w, "404 Not Found\r\n"), "not_found 状态行正确");
  check(has(w, "content-type: text/plain"), "not_found 默认是 text/plain");
  check(wire_body(w) == "404 Not Found", "not_found 默认 body 可读");

  // 使用者自己设了 body -> 必须保留。
  uvcpp_web_response r2;
  uvcpp_json j;
  j["error"] = "no such user";
  r2.json(j).not_found();
  const std::string w2 = wire(r2);
  check(has(w2, "404 Not Found\r\n"), "json + not_found 状态码仍是 404");
  check(wire_body(w2) == uvcpp_json_dump(j), "json + not_found 保留使用者的 JSON");
  check(has(w2, "content-type: application/json"), "json + not_found 保留 JSON 类型");

  // 405 必须带 Allow。
  uvcpp_web_response r3;
  r3.method_not_allowed("GET, HEAD, POST");
  const std::string w3 = wire(r3);
  check(has(w3, "405 Method Not Allowed\r\n"), "405 状态行正确");
  check(has(w3, "allow: GET, HEAD, POST\r\n"), "405 带 Allow 头");

  // 401 带 WWW-Authenticate。
  uvcpp_web_response r4;
  r4.unauthorized("Bearer realm=\"api\"");
  check(has(wire(r4), "www-authenticate: Bearer realm=\"api\"\r\n"),
        "401 带 WWW-Authenticate");

  // 416 带 Content-Range。
  uvcpp_web_response r5;
  r5.range_not_satisfiable(4096);
  const std::string w5 = wire(r5);
  check(has(w5, "416 Range Not Satisfiable\r\n"), "416 状态行正确");
  check(has(w5, "content-range: bytes */4096\r\n"), "416 带 Content-Range");

  // 不传 total 就不该带 Content-Range（读不出来源长度时）。
  uvcpp_web_response r6;
  r6.range_not_satisfiable();
  check(!has(wire(r6), "content-range"), "416 不知道总长时不带 Content-Range");

  // redirect
  uvcpp_web_response r7;
  r7.redirect("/login");
  const std::string w7 = wire(r7);
  check(has(w7, "302 Found\r\n"), "redirect 默认 302");
  check(has(w7, "location: /login\r\n"), "redirect 带 Location");
  check(has(w7, "content-length: 0\r\n"), "redirect 是空 body 且 CL 为 0");

  uvcpp_web_response r8;
  r8.redirect("/perm", 301);
  check(has(wire(r8), "301 Moved Permanently\r\n"), "redirect 可指定状态码");

  // 其余 helper 的状态码扫一遍。
  //
  // `expect_body` 区分两类：**错误**响应必须自带一段能读的说明
  // （空报文对排障毫无帮助），而 2xx 成功码的空 body 是完全正常的
  // （201 常常只带 Location 头）。
  struct {
    int expect;
    bool expect_body;
    void (*call)(uvcpp_web_response&);
  } cases[] = {
      {201, false, [](uvcpp_web_response& r) { r.created(); }},
      {202, false, [](uvcpp_web_response& r) { r.accepted(); }},
      {400, true, [](uvcpp_web_response& r) { r.bad_request(); }},
      {403, true, [](uvcpp_web_response& r) { r.forbidden(); }},
      {409, true, [](uvcpp_web_response& r) { r.conflict(); }},
      {413, true, [](uvcpp_web_response& r) { r.payload_too_large(); }},
      {415, true, [](uvcpp_web_response& r) { r.unsupported_media_type(); }},
      {422, true, [](uvcpp_web_response& r) { r.unprocessable(); }},
      {429, true, [](uvcpp_web_response& r) { r.too_many_requests(); }},
      {500, true, [](uvcpp_web_response& r) { r.server_error(); }},
      {503, true, [](uvcpp_web_response& r) { r.service_unavailable(); }},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    uvcpp_web_response rr;
    cases[i].call(rr);
    if (rr.status_code() != cases[i].expect) {
      std::cerr << "  [FAIL] helper 状态码期望 " << cases[i].expect << " 实际 "
                << rr.status_code() << std::endl;
      ++g_failures;
    }
    if (cases[i].expect_body && rr.body_empty()) {
      std::cerr << "  [FAIL] 错误 helper " << cases[i].expect
                << " 的默认 body 不该为空" << std::endl;
      ++g_failures;
    }
    // 不管有没有 body，都必须能正常序列化出 CL。
    const std::string ww = wire(rr);
    if (count_header(ww, "content-length") != 1) {
      std::cerr << "  [FAIL] helper " << cases[i].expect
                << " 的报文里 Content-Length 不是恰好一条" << std::endl;
      ++g_failures;
    }
  }
}

// =========================================================================
// 8. chunked 时不能覆盖使用者的决定
// =========================================================================
void test_chunked_not_overridden() {
  uvcpp_web_response r;
  r.text("abc").set_header("transfer-encoding", "chunked");
  const std::string w = wire(r);
  check(count_header(w, "content-length") == 0,
        "声明了 chunked 时不得再补 Content-Length");
  check(has(w, "transfer-encoding: chunked\r\n"), "chunked 头保留");
  // chunked 编码必须有终止块。
  check(has(w, "0\r\n\r\n"), "chunked 报文有终止块");

  // 使用者显式设的 CL 也不能被覆盖。
  uvcpp_web_response r2;
  r2.text("abc").set_header("content-length", "3");
  check(count_header(wire(r2), "content-length") == 1,
        "使用者显式设的 CL 不被重复追加");
}

// =========================================================================
// 9. sync_meta 幂等
// =========================================================================
void test_sync_meta_idempotent() {
  uvcpp_web_response r;
  r.text("abc");
  r.sync_meta();
  r.sync_meta();
  const std::string w = wire(r);  // raw() 里还会再调一次
  check(count_header(w, "content-length") == 1, "sync_meta 幂等，不重复加 CL");
  check(has(w, "content-length: 3\r\n"), "sync_meta 幂等且长度正确");
}

// =========================================================================
// 10. body_move 零拷贝
// =========================================================================
void test_body_move_zero_copy() {
  const std::string payload(4096, 'x');
  uvcpp_buf src;
  src.clone_data(payload.data(), payload.size());
  const char* base = src.get_const_data();
  const size_t n = src.size();

  uvcpp_web_response r;
  r.body_move(src, "application/octet-stream");

  check(r.body_size() == n, "body_move 长度正确");
  check(r.body_data() == base,
        "body_move 是所有权转移（同一块内存，不是复制）");
  check(src.size() == 0, "body_move 之后源 buf 被搬空");
  check(has(wire(r), "content-length: 4096\r\n"), "body_move 后 CL 正确");
}

// =========================================================================
// 11. on_sent 回调
// =========================================================================
void test_on_sent() {
  uvcpp_web_response r;
  bool called = false;
  int code = 0;
  size_t bytes = 0;
  r.on_sent([&](const uvcpp_web_sent_info& i) {
    called = true;
    code = i.status_code;
    bytes = i.body_bytes;
  });
  check(r.sent_callback_count() == 1, "on_sent 注册成功");

  uvcpp_web_sent_info info;
  info.status_code = 201;
  info.body_bytes = 42;
  info.ok = true;
  r.notify_sent(info);

  check(called, "on_sent 回调被触发");
  check(code == 201, "回调拿到状态码");
  check(bytes == 42, "回调拿到字节数");

  // 回调抛异常必须被吞掉 —— 发送已经完成，再抛只会把成功的响应变成崩溃。
  // 下面这行会往 stderr 打一条 [ERROR] [RESPONSE] "on_sent callback threw"，
  // **那是预期输出**，不是测试失败。
  uvcpp_web_response r2;
  r2.on_sent([](const uvcpp_web_sent_info&) {
    throw std::runtime_error("boom");
  });
  bool threw = false;
  try {
    uvcpp_web_sent_info i2;
    r2.notify_sent(i2);
  } catch (...) {
    threw = true;
  }
  check(!threw, "on_sent 回调抛异常不会穿出去");

  // 没注册回调时 notify 不能崩（这里是空操作，没有可断言的返回值，
  // 能走到下一行就算过）。
  uvcpp_web_response r3;
  uvcpp_web_sent_info i3;
  r3.notify_sent(i3);
  check(r3.sent_callback_count() == 0, "未注册时回调为空");
}

// =========================================================================
// 12. 其他：头操作、链式、deferred / ended
// =========================================================================
void test_misc() {
  uvcpp_web_response r;
  r.set_header("X-Custom", "v");
  check(has(wire(r), "x-custom: v") || has(wire(r), "X-Custom: v"),
        "自定义头能发出");
  check(r.has_header("x-custom"), "has_header 大小写不敏感");
  check(r.get_header("X-CUSTOM") == "v", "get_header 大小写不敏感");
  check(r.get_header("nope", "def") == "def", "get_header 缺省值");

  // remove_header 要把**所有**同名头都删掉（包括重复的 set-cookie）。
  uvcpp_web_response r2;
  r2.set_cookie("a", "1").set_cookie("b", "2").set_header("x-a", "1");
  r2.remove_header("set-cookie");
  const std::string w2 = wire(r2);
  check(count_header(w2, "set-cookie") == 0, "remove_header 删掉全部同名头");
  check(count_header(w2, "x-a") == 1, "remove_header 不影响其他头");

  // 链式调用返回的是同一个对象。
  uvcpp_web_response r3;
  uvcpp_web_response& ref = r3.status(201).set_header("x", "y").text("hi");
  check(&ref == &r3, "所有 mutator 返回 *this，可链式调用");

  // 生命周期标志。
  uvcpp_web_response r4;
  check(!r4.ended(), "初始未结束");
  check(!r4.deferred(), "初始非 deferred");
  r4.end();
  check(r4.ended(), "end() 生效");
  r4.set_deferred(true);
  check(r4.deferred(), "set_deferred 生效");

  // text/html 的 content-type 带 charset。
  uvcpp_web_response r5;
  r5.html("<p>hi</p>");
  check(has(wire(r5), "text/html; charset=utf-8"), "html 的 CT 带 charset");
  check(has(wire(r5), "content-length: 9\r\n"), "html 的 CL 正确");

  // clear_body 之后 CL 重算为 0。
  uvcpp_web_response r6;
  r6.text("abc");
  r6.clear_body();
  check(has(wire(r6), "content-length: 0\r\n"), "clear_body 后 CL 重算为 0");
}

}  // namespace

int main() {
  std::cout << std::unitbuf;  // 崩了也不要丢缓冲的输出

  test_status_line();
  test_empty_body_content_length();
  test_bodyless_statuses();
  test_head_only();
  test_multiple_set_cookie();
  test_cookie_attributes();
  test_status_helpers();
  test_chunked_not_overridden();
  test_sync_meta_idempotent();
  test_body_move_zero_copy();
  test_on_sent();
  test_misc();

  if (g_failures == 0) {
    std::cout << "[response] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[response] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}
