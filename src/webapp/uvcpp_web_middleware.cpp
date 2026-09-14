/**
 * @file src/webapp/uvcpp_web_middleware.cpp
 * @brief 内置中间件的实现。
 * @author zhuweiye
 * @version 1.0.0
 */
#include <webapp/uvcpp_web_middleware.h>

#include <atomic>
#include <chrono>
#include <cstdio>

#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

namespace uvcpp {

namespace {

/// steady_clock 的毫秒读数。**不能用 system_clock** —— 它会被系统对时
/// 往回拨，那样算出来的耗时会变成负数。
uint64_t now_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

/// 按状态码决定这条访问日志该记成什么等级。
log_level level_for_status(int status) {
  if (status >= 500) return log_level::ERR;
  if (status >= 400) return log_level::WARN;
  return log_level::INFO;
}

/**
 * @brief 客户端带来的 request-id 是否可接受。
 *
 * 字符集限定 `[A-Za-z0-9._-]`、长度 1~64。这个值是**攻击者完全可控**的，
 * 而它会被写进响应头、又会进日志：
 *   - 不查字符集 → 里面塞 `\r\n` 就是响应头注入（HTTP 响应拆分）；
 *   - 不查长度 → 塞 1MB 进去就是一条日志撑爆磁盘。
 */
bool is_acceptable_request_id(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                    c == '-';
    if (!ok) return false;
  }
  return true;
}

/**
 * @brief 生成一个 request-id。
 *
 * 不需要密码学随机：它的用途是**关联**，不是认证。但同一毫秒内的并发请求
 * 必须拿到不同的值，所以时间戳后面接一个进程内单调递增的序号。
 */
std::string generate_request_id() {
  static std::atomic<uint64_t> seq(0);
  const uint64_t n = ++seq;
  const uint64_t t = now_ms();
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%012llx%06llx",
                static_cast<unsigned long long>(t & 0xffffffffffffULL),
                static_cast<unsigned long long>(n & 0xffffffULL));
  return std::string(buf);
}

}  // namespace

// =========================================================================
// CORS 配置
// =========================================================================

web_cors_options::web_cors_options()
    : origin("*"),
      methods("GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS"),
      headers(),
      expose_headers(),
      credentials(false),
      max_age(-1) {}

// =========================================================================
// 1. 访问日志
// =========================================================================

uvcpp_web_middleware web_middleware_access_log() {
  return [](uvcpp_web_request& req, uvcpp_web_response& resp,
            uvcpp_web_next next) {
    const uint64_t start = now_ms();
    // 捕获请求指针：它和 resp 都由 context 持有，寿命覆盖 sent 回调。
    const uvcpp_web_request* rq = &req;

    resp.on_sent([rq, start](const uvcpp_web_sent_info& info) {
      const uint64_t elapsed = now_ms() - start;
      // 用 uvcpp_logf 而不是流式宏：等级要按状态码算出来，流式宏做不到
      // 「一个分支一个宏」还只拼一次字符串。logf 内部先判 is_enabled，
      // 被过滤掉时连格式化都不做。
      uvcpp_logf(level_for_status(info.status_code), log_category::REQUEST,
                 "%s %s -> %d (%lluB, %llums)%s", rq->method_name(),
                 rq->path().c_str(), info.status_code,
                 static_cast<unsigned long long>(info.body_bytes),
                 static_cast<unsigned long long>(elapsed),
                 info.ok ? "" : " [SEND-FAILED]");
    });

    next();
  };
}

// =========================================================================
// 2. 错误兜底
// =========================================================================

uvcpp_web_middleware
web_middleware_error_handler(const uvcpp_web_error_handler& handler) {
  return [handler](uvcpp_web_request&, uvcpp_web_response& resp,
                   uvcpp_web_next next) {
    resp.set_error_handler(handler);
    next();
  };
}

// =========================================================================
// 3. CORS
// =========================================================================

uvcpp_web_middleware web_middleware_cors(const web_cors_options& options) {
  return [options](uvcpp_web_request& req, uvcpp_web_response& resp,
                   uvcpp_web_next next) {
    const std::string& origin = options.origin;

    // "*" 与凭据不能共存（Fetch 规范禁止）。不回显请求的 Origin 来"救"这个
    // 组合：那等于允许任意站点带凭据读本服务的响应，比不配 CORS 更糟。
    // 这里保留 "*"、丢掉凭据。
    const bool credentials = options.credentials && origin != "*";
    if (options.credentials && origin == "*") {
      UVCPP_LOG_WARN(log_category::HTTP)
          << "CORS 配置非法：Access-Control-Allow-Origin 为 \"*\" 时不能带 "
             "credentials（Fetch 规范禁止），已忽略 credentials";
    }

    resp.set_header("access-control-allow-origin", origin);
    if (credentials) {
      resp.set_header("access-control-allow-credentials", "true");
    }
    // 响应随 Origin 变化，缓存必须按它分桶。origin 是 "*" 时每个请求拿到
    // 的都一样，不需要 Vary。
    if (origin != "*") {
      resp.set_header("vary", "Origin");
    }

    // 只有带 Access-Control-Request-Method 的 OPTIONS 才是**预检**。不带它
    // 的 OPTIONS 是普通请求，要放行给路由层（那边有自动 OPTIONS 回 Allow）。
    const bool preflight =
        req.method() == http_method::HTTP_OPTIONS &&
        req.has_header("access-control-request-method");

    if (!preflight) {
      if (!options.expose_headers.empty()) {
        resp.set_header("access-control-expose-headers", options.expose_headers);
      }
      next();
      return;
    }

    resp.set_header("access-control-allow-methods", options.methods);

    if (!options.headers.empty()) {
      resp.set_header("access-control-allow-headers", options.headers);
    } else {
      // 回显：客户端要什么就给什么。没有该头时给 "*"（现代浏览器接受）。
      std::string asked = req.header("access-control-request-headers");
      resp.set_header("access-control-allow-headers",
                      asked.empty() ? std::string("*") : asked);
    }

    if (options.max_age >= 0) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%ld", options.max_age);
      resp.set_header("access-control-max-age", buf);
    }

    // 预检就地答掉，不进路由 —— 路由层没有为 OPTIONS 注册任何东西，
    // 放行过去只会得到 405/404。
    resp.status(204).end();
  };
}

uvcpp_web_middleware web_middleware_cors() {
  return web_middleware_cors(web_cors_options());
}

// =========================================================================
// 4. 请求 ID
// =========================================================================

uvcpp_web_middleware web_middleware_request_id(const std::string& header_name,
                                               bool trust_client) {
  return [header_name, trust_client](uvcpp_web_request& req,
                                     uvcpp_web_response& resp,
                                     uvcpp_web_next next) {
    std::string id;
    if (trust_client) id = req.header(header_name);

    if (!id.empty() && !is_acceptable_request_id(id)) {
      UVCPP_LOG_WARN(log_category::HEADER)
          << "客户端提供的 " << header_name
          << " 不合法（长度或字符集），已丢弃并重新生成";
      id.clear();
    }
    if (id.empty()) id = generate_request_id();

    resp.set_header(header_name, id);
    next();
  };
}

// =========================================================================
// 5. 请求体上限
// =========================================================================

uvcpp_web_middleware web_middleware_body_limit(size_t max_bytes) {
  return [max_bytes](uvcpp_web_request& req, uvcpp_web_response& resp,
                     uvcpp_web_next next) {
    if (max_bytes == 0) {  // 0 = 不限
      next();
      return;
    }

    // 声明值和实际值都看：两者不一致本身就是攻击信号，取大的判。
    const size_t declared = req.content_length();
    const size_t actual = req.body_size();
    const size_t over = declared > actual ? declared : actual;

    if (over > max_bytes) {
      UVCPP_LOG_WARN(log_category::BODY)
          << "请求体超限：" << over << " > " << max_bytes << "，已回 413 并终止链";
      resp.payload_too_large().end();
      return;  // 不调 next()：链到此为止
    }
    next();
  };
}

// =========================================================================
// 6. 标识头
// =========================================================================

uvcpp_web_middleware web_middleware_powered_by(const std::string& value) {
  return [value](uvcpp_web_request&, uvcpp_web_response& resp,
                 uvcpp_web_next next) {
    resp.set_header("x-powered-by", value);
    next();
  };
}

}  // namespace uvcpp
