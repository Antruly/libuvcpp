/**
 * @file tests/functional/web_app_middleware_func.cpp
 * @brief 内置中间件的功能测试。
 *
 * 纯单元测试，不起网络 —— 中间件就是 `std::function`，直接构造
 * request/response 调它，比跑真连接快得多，也不会被端口/时序干扰。
 *
 * 这个文件里有几条断言是**回归护栏**：
 *
 *  1. `body_limit` / CORS 预检**不调 `next()`** —— 这是"终止链"的语义，
 *     写成 `next()` 之后再 reject 会让后面的 handler 覆盖掉 413；
 *  2. `request_id` **必须**拒绝带 CRLF / 超长的客户端 ID —— 那是响应头
 *     注入的口子（见 `test_request_id_injection`）；
 *  3. `on_sent` **可叠加** —— 访问日志和业务代码都会挂它，覆盖语义会让
 *     其中一个静默失效；
 *  4. CORS 的 `"*"` + credentials 组合必须被拒（不然等于任意站点可带凭据
 *     读本服务）。
 */
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_middleware.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

// -------------------------------------------------------------------------
// 把日志收进内存的 sink，用来断言中间件真的打了日志。
// -------------------------------------------------------------------------
class capturing_sink : public uvcpp_log_sink {
 public:
  void write(const uvcpp_log_record& record) override {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back(record);
  }
  size_t count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
  }
  uvcpp_log_record at(size_t idx) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (idx >= records_.size()) return uvcpp_log_record();
    return records_[idx];
  }
  /// 有没有哪条记录的正文里含 needle。
  bool message_contains(const std::string& needle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < records_.size(); ++i) {
      if (records_[i].message.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<uvcpp_log_record> records_;
};

/**
 * @brief 装一个 sink，并把等级放到最详细，析构时还原。
 *
 * RAII 而不是手工成对调用：中间途 `return` 或断言失败都不会把全局 sink
 * 留在测试对象上（那会让后面的用例往已析构的对象写日志）。
 */
class log_capture_guard {
 public:
  explicit log_capture_guard(capturing_sink* sink) {
    uvcpp_logger::instance().set_sink(sink);
    uvcpp_logger::instance().set_level(log_level::TRACE);
  }
  ~log_capture_guard() {
    uvcpp_logger::instance().set_sink(nullptr);  // 还原成内置控制台
    uvcpp_logger::instance().set_level(log_level::INFO);
  }

 private:
  log_capture_guard(const log_capture_guard&);
  log_capture_guard& operator=(const log_capture_guard&);
};

// -------------------------------------------------------------------------
// 测试用的请求/响应构造
// -------------------------------------------------------------------------

/// 造一个 GET 请求。
void make_get(uvcpp_web_request& req, const std::string& path) {
  uvcpp_http_request raw;
  raw.method = http_method::HTTP_GET;
  raw.url = path;
  req.take_from(raw);
}

/// 造一个带 body 的 POST 请求。
void make_post(uvcpp_web_request& req, const std::string& path,
               const std::string& body) {
  uvcpp_http_request raw;
  raw.method = http_method::HTTP_POST;
  raw.url = path;
  // **用 clone_data（深拷贝）而不是 set_data** —— set_data 是"外部视图"，
  // 指向 body 的缓冲区；那个 std::string 出了本函数就没了，take_from 再
  // 把它 move 走，请求里留下的就是一个悬垂指针。
  raw.body.clone_data(body.c_str(), body.size());
  req.take_from(raw);
}

/// 把响应的 body 读成 std::string（response 没有 body_str()，只有裸指针+长度）。
std::string body_of(const uvcpp_web_response& r) {
  const char* p = r.body_data();
  if (p == nullptr || r.body_size() == 0) return std::string();
  return std::string(p, r.body_size());
}

/// 在响应头里找一个值（大小写不敏感）。不存在返回 "<missing>"。
std::string head(const uvcpp_web_response& r, const std::string& name) {
  const std::string v = r.get_header(name);
  return v.empty() && !r.has_header(name) ? std::string("<missing>") : v;
}

/// 跑一遍「中间件 + 后续 handler」，返回后续 handler 有没有被执行。
bool run(uvcpp_web_middleware& mw, uvcpp_web_request& req,
         uvcpp_web_response& resp, int* ran_out) {
  int ran = 0;
  bool next_called = false;
  uvcpp_web_next next = [&]() {
    next_called = true;
    ++ran;
  };
  mw(req, resp, next);
  if (ran_out) *ran_out = ran;
  return next_called;
}

// =========================================================================
// 1. 访问日志
// =========================================================================
void test_access_log() {
  capturing_sink sink;
  log_capture_guard guard(&sink);

  uvcpp_web_middleware mw = web_middleware_access_log();
  uvcpp_web_request req;
  make_get(req, "/user/42");
  uvcpp_web_response resp;

  int ran = 0;
  check(run(mw, req, resp, &ran), "access_log 放行到下一个");
  check(ran == 1, "后续 handler 被执行");

  // 中间件本身不该在放行时就打日志 —— 那时状态码还没定。
  check(sink.count() == 0, "放行时还不打日志（状态码未定）");

  // 注册了 on_sent，响应发出后才打。
  check(resp.sent_callback_count() == 1, "access_log 挂上了 on_sent");

  resp.status(200);
  uvcpp_web_sent_info info;
  info.status_code = 200;
  info.body_bytes = 128;
  info.ok = true;
  resp.notify_sent(info);

  check(sink.count() == 1, "响应发出后打了一条日志");
  const uvcpp_log_record rec = sink.at(0);
  check(rec.category == log_category::REQUEST, "模块是 REQUEST");
  check(rec.level == log_level::INFO, "200 记成 INFO");
  check(rec.message.find("GET") != std::string::npos, "日志含方法");
  check(rec.message.find("/user/42") != std::string::npos, "日志含路径");
  check(rec.message.find("200") != std::string::npos, "日志含状态码");
  check(rec.message.find("128B") != std::string::npos, "日志含字节数");

  // 等级随状态码走 —— 这是「按模块自动分到 info/warn/error」的落地点。
  sink.clear();
  uvcpp_web_response r404;
  uvcpp_web_middleware mw404 = web_middleware_access_log();
  int ran404 = 0;
  run(mw404, req, r404, &ran404);
  uvcpp_web_sent_info i404;
  i404.status_code = 404;
  i404.ok = true;
  r404.notify_sent(i404);
  check(sink.count() == 1 && sink.at(0).level == log_level::WARN, "404 记成 WARN");

  sink.clear();
  uvcpp_web_response r500;
  uvcpp_web_middleware mw500 = web_middleware_access_log();
  int ran500 = 0;
  run(mw500, req, r500, &ran500);
  uvcpp_web_sent_info i500;
  i500.status_code = 500;
  i500.ok = true;
  r500.notify_sent(i500);
  check(sink.count() == 1 && sink.at(0).level == log_level::ERR, "500 记成 ERR");
  check(sink.message_contains("500"), "500 日志正文正确");

  // 发送失败要标出来，否则「客户端收到 0 字节」在日志里看不出异常。
  sink.clear();
  uvcpp_web_response rf;
  uvcpp_web_middleware mwf = web_middleware_access_log();
  int ranf = 0;
  run(mwf, req, rf, &ranf);
  uvcpp_web_sent_info ifail;
  ifail.status_code = 200;
  ifail.ok = false;
  rf.notify_sent(ifail);
  check(sink.message_contains("SEND-FAILED"), "发送失败被标记出来");
}

// =========================================================================
// 2. on_sent 可叠加（访问日志 + 业务代码同时挂钩）
// =========================================================================
void test_on_sent_is_additive() {
  uvcpp_web_response r;
  int a = 0, b = 0;
  r.on_sent([&](const uvcpp_web_sent_info&) { ++a; });
  r.on_sent([&](const uvcpp_web_sent_info&) { ++b; });
  check(r.sent_callback_count() == 2, "两个回调都注册上了");

  uvcpp_web_sent_info info;
  r.notify_sent(info);
  check(a == 1 && b == 1, "两个回调都被触发（覆盖语义会只剩后一个）");
  check(r.sent_callback_count() == 0, "触发后列表清空");

  // 重复 notify 是空操作 —— "已发出"是一次性事件，重复投递会让访问日志记两遍。
  r.notify_sent(info);
  check(a == 1 && b == 1, "重复 notify 不再触发");

  // 一个回调抛异常不能连累后面的。
  uvcpp_web_response r2;
  int after = 0;
  r2.on_sent([](const uvcpp_web_sent_info&) { throw std::runtime_error("boom"); });
  r2.on_sent([&](const uvcpp_web_sent_info&) { ++after; });
  // 这行会往控制台打一条 [ERROR] [RESPONSE]，**那是预期输出**。
  r2.notify_sent(info);
  check(after == 1, "前一个回调抛异常，后一个照样触发");

  // 传空回调不该占一个位置。
  uvcpp_web_response r3;
  r3.on_sent(uvcpp_web_sent_cb());
  check(r3.sent_callback_count() == 0, "空回调被忽略");
}

// =========================================================================
// 3. 错误兜底
// =========================================================================
void test_error_handler() {
  uvcpp_web_middleware mw = web_middleware_error_handler(
      [](uvcpp_web_request&, uvcpp_web_response& resp, const std::string& what) {
        resp.status(500).text(std::string("caught: ") + what);
      });

  uvcpp_web_request req;
  make_get(req, "/boom");
  uvcpp_web_response resp;

  check(!resp.has_error_handler(), "装之前没有错误处理器");
  int ran = 0;
  check(run(mw, req, resp, &ran), "error_handler 放行到下一个");
  check(resp.has_error_handler(), "装之后有了");

  // 框架捕获到异常后会这样调用它。
  resp.error_handler()(req, resp, "std::runtime_error: boom");
  check(resp.status_code() == 500, "错误处理器把异常转成 500");
  check(body_of(resp).find("boom") != std::string::npos, "异常信息进了 body");
}

// =========================================================================
// 4. CORS
// =========================================================================
void test_cors_simple() {
  uvcpp_web_middleware mw = web_middleware_cors();
  uvcpp_web_request req;
  make_get(req, "/api/x");
  uvcpp_web_response resp;

  int ran = 0;
  check(run(mw, req, resp, &ran), "简单请求放行");
  check(head(resp, "access-control-allow-origin") == "*", "ACAO = *");
  check(head(resp, "access-control-allow-credentials") == "<missing>",
        "默认不带凭据");
  check(head(resp, "vary") == "<missing>", "origin 为 * 时不发 Vary");
}

void test_cors_preflight() {
  web_cors_options o;
  o.origin = "https://example.com";
  o.max_age = 600;
  uvcpp_web_middleware mw = web_middleware_cors(o);

  uvcpp_http_request raw;
  raw.method = http_method::HTTP_OPTIONS;
  raw.url = "/api/x";
  http_header h;
  h.name = "access-control-request-method";
  h.value = "POST";
  raw.headers.push_back(h);
  http_header h2;
  h2.name = "access-control-request-headers";
  h2.value = "x-token, content-type";
  raw.headers.push_back(h2);

  uvcpp_web_request req;
  req.take_from(raw);
  uvcpp_web_response resp;

  int ran = 0;
  check(!run(mw, req, resp, &ran), "预检**不**放行（就地答掉）");
  check(ran == 0, "后续 handler 没被执行");
  check(resp.status_code() == 204, "预检回 204");
  check(head(resp, "access-control-allow-origin") == "https://example.com",
        "具体来源被回写");
  check(head(resp, "vary") == "Origin", "非 * 时发 Vary: Origin");
  check(head(resp, "access-control-allow-methods").find("POST") !=
            std::string::npos,
        "ACAM 含 POST");
  check(head(resp, "access-control-allow-headers") == "x-token, content-type",
        "默认回显请求要的头");
  check(head(resp, "access-control-max-age") == "600", "max-age 生效");
}

void test_cors_explicit_headers_override_echo() {
  web_cors_options o;
  o.headers = "x-token";
  uvcpp_web_middleware mw = web_middleware_cors(o);

  uvcpp_http_request raw;
  raw.method = http_method::HTTP_OPTIONS;
  raw.url = "/api/x";
  http_header h;
  h.name = "access-control-request-method";
  h.value = "PUT";
  raw.headers.push_back(h);
  http_header h2;
  h.name = "access-control-request-headers";
  h.value = "x-evil";
  raw.headers.push_back(h2);

  uvcpp_web_request req;
  req.take_from(raw);
  uvcpp_web_response resp;
  int ran = 0;
  run(mw, req, resp, &ran);
  check(head(resp, "access-control-allow-headers") == "x-token",
        "显式配置覆盖回显（不回显 x-evil）");
}

void test_cors_plain_options_is_not_preflight() {
  uvcpp_web_middleware mw = web_middleware_cors();

  // 不带 Access-Control-Request-Method 的 OPTIONS 是**普通请求**，
  // 要放行给路由层（那边回 Allow）。
  uvcpp_http_request raw;
  raw.method = http_method::HTTP_OPTIONS;
  raw.url = "/api/x";
  uvcpp_web_request req;
  req.take_from(raw);
  uvcpp_web_response resp;

  int ran = 0;
  check(run(mw, req, resp, &ran), "普通 OPTIONS 放行到路由层");
  check(resp.status_code() != 204, "普通 OPTIONS 不被就地答成 204");
}

void test_cors_star_with_credentials_rejected() {
  capturing_sink sink;
  log_capture_guard guard(&sink);

  web_cors_options o;
  o.credentials = true;  // origin 保持 "*"
  uvcpp_web_middleware mw = web_middleware_cors(o);

  uvcpp_web_request req;
  make_get(req, "/api/x");
  uvcpp_web_response resp;
  int ran = 0;
  run(mw, req, resp, &ran);

  check(head(resp, "access-control-allow-origin") == "*", "保留 *");
  check(head(resp, "access-control-allow-credentials") == "<missing>",
        "\"*\" + credentials 被拒（不回 ACAC）");
  check(sink.message_contains("credentials"), "打了 WARN 说明原因");

  // 具体来源 + credentials 是合法的，要真的发出去。
  web_cors_options o2;
  o2.origin = "https://ok.example";
  o2.credentials = true;
  uvcpp_web_middleware mw2 = web_middleware_cors(o2);
  uvcpp_web_request req2;
  make_get(req2, "/api/x");
  uvcpp_web_response resp2;
  int ran2 = 0;
  run(mw2, req2, resp2, &ran2);
  check(head(resp2, "access-control-allow-credentials") == "true",
        "具体来源 + credentials 正常发出");
  check(head(resp2, "access-control-allow-origin") == "https://ok.example",
        "回写具体来源");
}

// =========================================================================
// 5. 请求 ID
// =========================================================================
void test_request_id_generates() {
  uvcpp_web_middleware mw = web_middleware_request_id();
  uvcpp_web_request req;
  make_get(req, "/x");
  uvcpp_web_response resp;

  int ran = 0;
  check(run(mw, req, resp, &ran), "request_id 放行");

  const std::string id = head(resp, "X-Request-Id");
  check(id != "<missing>", "响应里有 X-Request-Id");
  check(id.size() == 18, "生成的 ID 长度固定（12 位时间 + 6 位序号）");

  // 两次请求要拿到不同的 ID。
  uvcpp_web_request req2;
  make_get(req2, "/x");
  uvcpp_web_response resp2;
  int ran2 = 0;
  run(mw, req2, resp2, &ran2);
  check(head(resp2, "X-Request-Id") != id, "两次请求的 ID 不同");
}

void test_request_id_echoes_client() {
  uvcpp_web_middleware mw = web_middleware_request_id();
  uvcpp_http_request raw;
  raw.method = http_method::HTTP_GET;
  raw.url = "/x";
  http_header h;
  h.name = "x-request-id";
  h.value = "abc-123_XY.z";
  raw.headers.push_back(h);

  uvcpp_web_request req;
  req.take_from(raw);
  uvcpp_web_response resp;
  int ran = 0;
  run(mw, req, resp, &ran);
  check(head(resp, "X-Request-Id") == "abc-123_XY.z",
        "合法客户端 ID 被采信（分布式追踪需要）");

  // trust_client = false 时一律自己生成。
  uvcpp_web_middleware mw2 = web_middleware_request_id("X-Request-Id", false);
  uvcpp_web_request req2;
  uvcpp_http_request raw2;
  raw2.method = http_method::HTTP_GET;
  raw2.url = "/x";
  http_header h2;
  h2.name = "x-request-id";
  h2.value = "abc-123";
  raw2.headers.push_back(h2);
  req2.take_from(raw2);
  uvcpp_web_response resp2;
  int ran2 = 0;
  run(mw2, req2, resp2, &ran2);
  check(head(resp2, "X-Request-Id") != "abc-123",
        "trust_client=false 时不采信客户端 ID");
}

void test_request_id_injection() {
  capturing_sink sink;
  log_capture_guard guard(&sink);

  uvcpp_web_middleware mw = web_middleware_request_id();

  // **回归护栏**：客户端带来的 ID 会进响应头，不校验就是响应头注入。
  const char* evil[] = {
      "abc\r\nX-Injected: yes",  // CRLF 注入
      "abc\ndef",                // 裸 LF
      "abc\0def",                // 内嵌 NUL（std::string 里是真的 NUL）
      "has space",               // 空格
      "quote\"inject",           // 引号
      "<script>",                // 角括号
  };
  const size_t evil_len[] = {22, 7, 7, 9, 12, 8};
  const size_t count = sizeof(evil) / sizeof(evil[0]);

  for (size_t i = 0; i < count; ++i) {
    uvcpp_http_request raw;
    raw.method = http_method::HTTP_GET;
    raw.url = "/x";
    http_header h;
    h.name = "x-request-id";
    h.value.assign(evil[i], evil_len[i]);
    raw.headers.push_back(h);

    uvcpp_web_request req;
    req.take_from(raw);
    uvcpp_web_response resp;
    int ran = 0;
    run(mw, req, resp, &ran);

    const std::string got = head(resp, "X-Request-Id");
    check(got != "<missing>", "非法 ID 被替换成了生成的 ID");
    check(got.find('\r') == std::string::npos &&
              got.find('\n') == std::string::npos,
          "响应头里没有 CR/LF");
    check(got.find("X-Injected") == std::string::npos, "注入内容没被带进去");
  }

  // 超长 ID 也要丢（1MB 的 ID 就是一条撑爆磁盘的日志）。
  uvcpp_http_request raw;
  raw.method = http_method::HTTP_GET;
  raw.url = "/x";
  http_header h;
  h.name = "x-request-id";
  h.value.assign(4096, 'a');
  raw.headers.push_back(h);
  uvcpp_web_request req;
  req.take_from(raw);
  uvcpp_web_response resp;
  int ran = 0;
  run(mw, req, resp, &ran);
  check(head(resp, "X-Request-Id").size() == 18, "超长 ID 被丢弃并重新生成");
}

// =========================================================================
// 6. 请求体上限
// =========================================================================
void test_body_limit() {
  capturing_sink sink;
  log_capture_guard guard(&sink);

  uvcpp_web_middleware mw = web_middleware_body_limit(16);

  // 没超限 -> 放行。
  uvcpp_web_request ok;
  make_post(ok, "/u", "0123456789");
  uvcpp_web_response resp_ok;
  int ran_ok = 0;
  check(run(mw, ok, resp_ok, &ran_ok), "未超限时放行");
  check(resp_ok.status_code() != 413, "未超限时状态码不是 413");

  // 超限 -> 413 且**不放行**。
  uvcpp_web_request big;
  make_post(big, "/u", std::string(100, 'x'));
  uvcpp_web_response resp_big;
  int ran_big = 0;
  const bool passed = run(mw, big, resp_big, &ran_big);
  check(!passed, "超限时**不**放行（终止链）");
  check(ran_big == 0, "后续 handler 没被执行");
  check(resp_big.status_code() == 413, "超限回 413");
  check(!resp_big.body_empty(), "413 带默认 body");
  check(sink.message_contains("413"), "打了 WARN 说明已终止链");

  // 0 = 不限。
  uvcpp_web_middleware off = web_middleware_body_limit(0);
  uvcpp_web_request huge;
  make_post(huge, "/u", std::string(100000, 'x'));
  uvcpp_web_response resp_huge;
  int ran_huge = 0;
  check(run(off, huge, resp_huge, &ran_huge), "0 表示不限，放行");
  check(resp_huge.status_code() != 413, "0 时不会回 413");

  // 边界：正好等于上限要通过（判定是 > 而不是 >=）。
  uvcpp_web_middleware exact = web_middleware_body_limit(10);
  uvcpp_web_request eq;
  make_post(eq, "/u", "0123456789");
  uvcpp_web_response resp_eq;
  int ran_eq = 0;
  check(run(exact, eq, resp_eq, &ran_eq), "正好等于上限时放行");
}

// =========================================================================
// 7. 标识头
// =========================================================================
void test_powered_by() {
  uvcpp_web_middleware mw = web_middleware_powered_by();
  uvcpp_web_request req;
  make_get(req, "/x");
  uvcpp_web_response resp;
  int ran = 0;
  run(mw, req, resp, &ran);
  check(head(resp, "x-powered-by") == "uvcpp", "默认值");

  uvcpp_web_middleware mw2 = web_middleware_powered_by("myapp");
  uvcpp_web_request req2;
  make_get(req2, "/x");
  uvcpp_web_response resp2;
  int ran2 = 0;
  run(mw2, req2, resp2, &ran2);
  check(head(resp2, "x-powered-by") == "myapp", "自定义值");
}

// =========================================================================
// 8. 中间件串起来（把 chain 手工跑一遍）
// =========================================================================
void test_chain_composition() {
  capturing_sink sink;
  log_capture_guard guard(&sink);

  // 模拟框架的链：access_log -> cors -> body_limit -> 业务 handler。
  // 这里用递归而不是 context 的「索引 + 重入标志」—— 那是 Phase 1.7 的
  // 实现，本测试只验证中间件之间能正确组合。
  std::vector<uvcpp_web_middleware> chain;
  chain.push_back(web_middleware_access_log());
  chain.push_back(web_middleware_cors());
  chain.push_back(web_middleware_body_limit(32));

  uvcpp_web_request req;
  make_post(req, "/api/x", "hello");
  uvcpp_web_response resp;

  bool biz_ran = false;
  struct Runner {
    static void go(const std::vector<uvcpp_web_middleware>& chain, size_t i,
                   uvcpp_web_request& req, uvcpp_web_response& resp,
                   bool* biz_ran) {
      if (i >= chain.size()) {
        // 链尾的业务 handler。
        *biz_ran = true;
        resp.status(200).text("ok");
        return;
      }
      uvcpp_web_next next = [&chain, i, &req, &resp, biz_ran]() {
        go(chain, i + 1, req, resp, biz_ran);
      };
      chain[i](req, resp, next);
    }
  };
  Runner::go(chain, 0, req, resp, &biz_ran);

  check(biz_ran, "业务 handler 跑到了");
  check(resp.status_code() == 200, "最终状态码 200");
  check(head(resp, "access-control-allow-origin") == "*", "CORS 头在");
  check(resp.sent_callback_count() == 1, "access_log 的回调还挂着");

  resp.notify_sent(uvcpp_web_sent_info());
  check(sink.count() == 1, "链跑完后访问日志打了一条");
  check(sink.at(0).message.find("/api/x") != std::string::npos,
        "访问日志记的是真实路径");

  // 超限请求：body_limit 应该挡住业务 handler。
  uvcpp_web_request big;
  make_post(big, "/api/x", std::string(1000, 'x'));
  uvcpp_web_response resp_big;
  bool biz_ran_big = false;
  Runner::go(chain, 0, big, resp_big, &biz_ran_big);
  check(!biz_ran_big, "超限请求没进业务 handler");
  check(resp_big.status_code() == 413, "超限请求回 413");
  // CORS 头在 body_limit 之前就加了，所以 413 也带跨域头 —— 这是对的，
  // 否则浏览器只能看到 CORS 错误，看不到真正的 413。
  check(head(resp_big, "access-control-allow-origin") == "*",
        "413 也带 CORS 头（否则前端看不到真正的错误）");
}

}  // namespace

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  test_access_log();
  test_on_sent_is_additive();
  test_error_handler();
  test_cors_simple();
  test_cors_preflight();
  test_cors_explicit_headers_override_echo();
  test_cors_plain_options_is_not_preflight();
  test_cors_star_with_credentials_rejected();
  test_request_id_generates();
  test_request_id_echoes_client();
  test_request_id_injection();
  test_body_limit();
  test_powered_by();
  test_chain_composition();

  if (g_failures == 0) {
    std::cout << "[middleware] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[middleware] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}
