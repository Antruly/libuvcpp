/**
 * @file tests/functional/web_app_request_func.cpp
 * @brief webapp 请求封装（`uvcpp_web_request`）与 JSON（`uvcpp_web_json`）的
 *        功能测试。
 *
 * 这一层的价值全在几个**容易写错又不容易发现**的细节上，所以用例也是冲着
 * 这些去的：
 *
 * - `take_from()` 必须真的**搬走** body（源 buf 变空），而不是深拷一份。
 *   写成 clone 的话功能上完全正常，只是每次上传白扔一倍内存 —— 只有断言
 *   源 buf 已经空了才能把这种退化钉住。
 * - body 是**二进制安全**的：内嵌 NUL、非 UTF-8 字节都要原样保留。
 *   任何走 `strlen`/`c_str()` 的实现都会在这里露馅。
 * - `Accept-Encoding` 的 q 值：`gzip;q=0` 是**明确拒绝**，不是"没提过"。
 * - `Connection: keep-alive, Upgrade` 必须按 token 匹配，整串比较会误判。
 * - `Content-Length` 溢出必须判非法，不能夹到 SIZE_MAX。
 * - JSON 深度预扫描要跳过字符串字面量，`{"a":"[[[[[["}` 的深度是 1。
 */
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_request.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq(const std::string& got, const std::string& want, const char* what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: [" << want
              << "]\n         实际: [" << got << "]" << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const char* what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

// =========================================================================
// 构造辅助
// =========================================================================

/// 造一个 http 层请求，body 用 str 的**全部**字节（含其中的 NUL）。
///
/// 必须用 `clone_data` 而不是 `set_data`：后者是**不持有所有权的视图**
/// （直接把指针存进 `uv_buf_t`，见 uvcpp_buf.cpp:202 的注释），而这里的
/// body 来自一个临时 `std::string`，函数一返回就析构 —— 用 set_data 等于
/// 让请求体指向已释放的堆内存，读它是未定义行为。
uvcpp_http_request make_http_req(http_method method, const std::string& url,
                                 const std::string& body = std::string()) {
  uvcpp_http_request r;
  r.method = method;
  r.url = url;
  r.version = uvcpp_http_version::HVER_11;
  if (!body.empty()) {
    r.body.clone_data(body.data(), body.size());
  }
  return r;
}

/// 造一个带二进制的 body（显式长度，允许内嵌 NUL）。
uvcpp_http_request make_http_req_raw(http_method method, const std::string& url,
                                     const char* data, size_t len) {
  uvcpp_http_request r;
  r.method = method;
  r.url = url;
  r.version = uvcpp_http_version::HVER_11;
  if (len > 0) {
    r.body.clone_data(data, len);
  }
  return r;
}

/// 造一个已填充的 web 层请求。
///
/// 用输出参数而不是返回值：`uvcpp_web_request` 持有从源请求搬来的
/// body，故意删了拷贝构造，而 C++11 的返回值优化**不是强制的**——
/// `return w;` 仍然需要一个可访问的拷贝/移动构造函数。
void make_web_req(uvcpp_web_request& w, http_method method,
                  const std::string& url,
                  const std::string& body = std::string()) {
  uvcpp_http_request src = make_http_req(method, url, body);
  w.take_from(src);
}

// =========================================================================
// 1. 零拷贝接管
// =========================================================================
void test_take_from_zero_copy() {
  std::cout << "[request] take_from 零拷贝接管" << std::endl;

  std::string payload(4096, 'x');
  payload[0] = 'A';
  payload[payload.size() - 1] = 'Z';

  uvcpp_http_request src = make_http_req(http_method::HTTP_POST, "/upload",
                                         payload);
  // 前置条件：源确实有数据，否则下面的断言会因为"本来就是空的"而假通过
  check_eq_i(static_cast<long long>(src.body.size()), 4096, "前置：源有 body");

  // 记下源缓冲区地址 —— 接管之后，web 层应当持有**同一块**内存。
  const char* src_base = src.body.get_const_data();

  uvcpp_web_request w;
  w.take_from(src);

  check_eq_i(static_cast<long long>(w.body_size()), 4096, "接管后字节数一致");
  check(w.body_data() != nullptr, "接管后指针非空");
  check_eq(std::string(w.body_data(), w.body_size()), payload, "字节完全一致");

  // 核心不变量：源已经被**搬空**。
  //
  // 把 take_from 里的 `body_.move_buf(src.body)` 改成
  // `body_ = src.body`（深拷），功能上一切照旧，只有这两条会红 ——
  // 这正是它们存在的意义：钉住"没有多付一次大块拷贝"。
  check_eq_i(static_cast<long long>(src.body.size()), 0,
             "接管后源 buf 必须为空（否则说明是拷贝不是移动）");
  check(src.body.get_const_data() == nullptr,
        "接管后源 buf 的指针必须为 nullptr");

  // 同一块内存：地址不变才说明是所有权转移
  check(w.body_data() == src_base,
        "接管后应指向原来那块内存（所有权转移，不是重新分配）");

  // method / url / headers 是拷贝，源仍然完好
  check_eq_i(static_cast<int>(w.method()), static_cast<int>(http_method::HTTP_POST),
             "method 正确");
  check_eq(w.raw_url(), "/upload", "raw_url 正确");
  check_eq_i(static_cast<long long>(src.url.size()), 7,
             "源请求的 url 不受影响（只有 body 被搬走）");
}

// =========================================================================
// 2. 二进制安全
// =========================================================================
void test_binary_body() {
  std::cout << "[request] body 二进制安全" << std::endl;

  // 内嵌 NUL + 高位字节（0xFF 不是合法 UTF-8）
  const char raw[] = {'a', '\0', 'b', '\0', '\0', 'c',
                      static_cast<char>(0xFF), static_cast<char>(0x01)};
  const size_t raw_len = sizeof(raw);

  uvcpp_http_request src =
      make_http_req_raw(http_method::HTTP_POST, "/bin", raw, raw_len);

  uvcpp_web_request w;
  w.take_from(src);

  check_eq_i(static_cast<long long>(w.body_size()), static_cast<long long>(raw_len),
             "body_size 是字节数，不是 strlen");
  check(!w.body_empty(), "有 body 时 body_empty 为 false");
  check(std::memcmp(w.body_data(), raw, raw_len) == 0, "字节逐一对上");

  // body_str() 同样二进制安全 —— 走 c_str()/strlen 的实现会在这里少一截
  const std::string s = w.body_str();
  check_eq_i(static_cast<long long>(s.size()), static_cast<long long>(raw_len),
             "body_str 长度是字节数");
  check(std::memcmp(s.data(), raw, raw_len) == 0, "body_str 字节对上");

  // 空 body
  uvcpp_web_request e; make_web_req(e, http_method::HTTP_GET, "/");
  check(e.body_empty(), "无 body 时 body_empty 为 true");
  check_eq_i(static_cast<long long>(e.body_size()), 0, "无 body 时 size 为 0");
  check(e.body_data() == nullptr, "无 body 时 data 为 nullptr");
  check(e.body_str().empty(), "无 body 时 body_str 为空");
}

// =========================================================================
// 3. 路径 / 查询串拆分与解码
// =========================================================================
void test_path_and_query() {
  std::cout << "[request] 路径拆分与解码" << std::endl;

  {
    uvcpp_web_request w;
    make_web_req(w, http_method::HTTP_GET, "/a%20b/c?x=1&y=2");
    check_eq(w.raw_url(), "/a%20b/c?x=1&y=2", "raw_url 保留原始形态");
    check_eq(w.raw_path(), "/a%20b/c", "raw_path 未解码");
    check_eq(w.path(), "/a b/c", "path 已解码");
    check_eq(w.query_string(), "x=1&y=2", "query_string 不含 '?'");
  }

  {
    // 路径里的 '+' 是**字面加号**（plus_as_space=false）；
    // 查询串里的 '+' 是空格。两者的规则不同，必须分开。
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_GET, "/a+b?k=a+b");
    check_eq(w.path(), "/a+b", "路径中的 '+' 是字面加号");
    const std::string* k = w.query("k");
    check(k != nullptr && *k == "a b", "查询串中的 '+' 是空格");
  }

  {
    // %2F 解码成 '/'，因此它**充当分隔符**。这是有意选择（Express 等
    // 框架同样在解码后的路径上匹配），在这里钉住，防止被无意改掉。
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_GET, "/api/a%2Fb");
    check_eq(w.path(), "/api/a/b", "%2F 解码成 '/' 并成为分隔符");
    check_eq(w.raw_path(), "/api/a%2Fb", "同时 raw_path 仍保留原样");
  }

  {
    // 无查询串
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_GET, "/only/path");
    check_eq(w.path(), "/only/path", "无查询串时 path 正确");
    check(w.query_string().empty(), "无查询串时 query_string 为空");
    check(w.query_params().empty(), "无查询串时参数列表为空");
    check(w.query("nope") == nullptr, "查不到的参数返回 nullptr");
  }
}

// =========================================================================
// 4. 查询串解析
// =========================================================================
void test_query_params() {
  std::cout << "[request] 查询串解析" << std::endl;

  uvcpp_web_request w;
  make_web_req(w, http_method::HTTP_GET,
               "/s?tag=a&tag=b&empty=&novalue&sp=%20&pct=%2B");

  // 重复键：**保序、都保留**。做成 map 就会丢一个。
  const std::vector<std::pair<std::string, std::string> >& ps = w.query_params();
  check_eq_i(static_cast<long long>(ps.size()), 6, "重复键全部保留");

  check_eq(*w.query("tag"), "a", "重复键取第一个");
  check_eq(*w.query("empty"), "", "空值参数存在且为空串");
  check_eq(*w.query("sp"), " ", "%20 解码成空格");
  check_eq(*w.query("pct"), "+", "%2B 解码成字面加号");

  // "novalue" 这种没有 '=' 的项：存在，值为空
  const std::string* nv = w.query("novalue");
  check(nv != nullptr, "无 '=' 的键仍然存在");
  check(nv != nullptr && nv->empty(), "无 '=' 的键值为空串");

  // 惰性缓存：重复取不改变结果
  check_eq_i(static_cast<long long>(w.query_params().size()), 6,
             "二次读取结果一致（缓存正确）");
}

// =========================================================================
// 5. 报头与缓存
// =========================================================================
void test_headers() {
  std::cout << "[request] 报头读取与缓存" << std::endl;

  uvcpp_http_request src = make_http_req(http_method::HTTP_POST, "/x", "body");
  src.set_header("Content-Type", "application/json; charset=utf-8");
  src.set_header("Accept-Encoding", "gzip, deflate");
  src.set_header("Host", "example.com:8080");
  src.set_header("Content-Length", "4");
  src.set_header("X-Custom", "v1");

  uvcpp_web_request w;
  w.take_from(src);

  // 大小写不敏感
  check_eq(w.header("content-type"), "application/json; charset=utf-8",
           "头名大小写不敏感");
  check_eq(w.header("X-CUSTOM"), "v1", "任意头大小写不敏感");
  check_eq(w.header("nonexistent", "dflt"), "dflt", "缺失头返回默认值");

  check(w.has_header("HOST"), "has_header 大小写不敏感");
  check(!w.has_header("X-Absent"), "has_header 缺失返回 false");

  // 构造时缓存的那几个
  check_eq(w.content_type(), "application/json; charset=utf-8",
           "content_type 缓存完整值（含 charset）");
  check_eq(w.host(), "example.com:8080", "host 已缓存");

  // 字符串版本的 header 也能拿到全部头
  check(!w.headers().empty(), "headers() 能拿到完整头列表");

  // raw() 是逃生口：原始请求仍可访问
  check_eq(w.raw().url, "/x", "raw() 暴露原始 url");
  check(w.raw().get_header("x-custom") == "v1", "raw() 上的头查询仍然有效");
}

// =========================================================================
// 6. Content-Length
// =========================================================================
void test_content_length() {
  std::cout << "[request] Content-Length 解析" << std::endl;

  {
    uvcpp_http_request src = make_http_req(http_method::HTTP_POST, "/x", "abcd");
    src.set_header("Content-Length", "4");
    uvcpp_web_request w;
    w.take_from(src);
    check_eq_i(static_cast<long long>(w.content_length()), 4, "正常值");
  }

  {
    // 前后空白是允许的（RFC 7230 §3.2.4 允许 OWS）
    uvcpp_http_request src = make_http_req(http_method::HTTP_POST, "/x", "");
    src.set_header("Content-Length", "  12  ");
    uvcpp_web_request w;
    w.take_from(src);
    check_eq_i(static_cast<long long>(w.content_length()), 12, "允许前后空白");
  }

  {
    // **溢出**：20 位数字远超 size_t 的十进制度量。用 strtoull 的实现会
    // 在这里返回 ULLONG_MAX（一个攻击者可控的值被静默夹到"最大值"），
    // 本实现必须判非法 → 0。
    uvcpp_http_request src = make_http_req(http_method::HTTP_POST, "/x", "");
    src.set_header("Content-Length", "99999999999999999999");
    uvcpp_web_request w;
    w.take_from(src);
    check_eq_i(static_cast<long long>(w.content_length()), 0,
               "溢出必须判非法为 0，不能夹到最大值");
  }

  {
    // 非数字
    uvcpp_http_request src = make_http_req(http_method::HTTP_POST, "/x", "");
    src.set_header("Content-Length", "12x");
    uvcpp_web_request w;
    w.take_from(src);
    check_eq_i(static_cast<long long>(w.content_length()), 0, "非数字判非法");
  }

  {
    // 负数（带符号）同样非法
    uvcpp_http_request src = make_http_req(http_method::HTTP_POST, "/x", "");
    src.set_header("Content-Length", "-5");
    uvcpp_web_request w;
    w.take_from(src);
    check_eq_i(static_cast<long long>(w.content_length()), 0, "负数判非法");
  }

  {
    // 头缺失
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_GET, "/x");
    check_eq_i(static_cast<long long>(w.content_length()), 0, "头缺失时为 0");
  }

  {
    // 声明值与实际 body 不一致：两个接口各报各的，调用方能发现异常
    uvcpp_http_request src = make_http_req(http_method::HTTP_POST, "/x", "abc");
    src.set_header("Content-Length", "100");
    uvcpp_web_request w;
    w.take_from(src);
    check_eq_i(static_cast<long long>(w.content_length()), 100,
               "content_length 是**声明值**");
    check_eq_i(static_cast<long long>(w.body_size()), 3,
               "body_size 是**实际值**");
  }
}

// =========================================================================
// 7. Accept-Encoding 与 q 值
// =========================================================================
void test_accepts_encoding() {
  std::cout << "[request] Accept-Encoding 与 q 值" << std::endl;

  {
    // 头缺失：只有 identity 可接受（RFC 7231 §5.3.4）
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_GET, "/x");
    check(w.accepts_encoding("identity"), "无头时 identity 可接受");
    check(!w.accepts_encoding("gzip"), "无头时 gzip 不可接受");
  }

  {
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.set_header("Accept-Encoding", "gzip, deflate, br");
    uvcpp_web_request w;
    w.take_from(src);
    check(w.accepts_encoding("gzip"), "gzip 可接受");
    check(w.accepts_encoding("deflate"), "deflate 可接受");
    check(w.accepts_encoding("br"), "br 可接受");
    check(w.accepts_encoding("identity"), "identity 仍然可接受");
    check(!w.accepts_encoding("zstd"), "未列出的编码不可接受");
  }

  {
    // **q=0 是明确拒绝**，不是"没提过"。把它当成"没提过"就会给一个
    // 明确说了不要 gzip 的客户端发 gzip。
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.set_header("Accept-Encoding", "gzip;q=0, deflate");
    uvcpp_web_request w;
    w.take_from(src);
    check(!w.accepts_encoding("gzip"), "gzip;q=0 是明确拒绝");
    check(w.accepts_encoding("deflate"), "同一头里 deflate 仍可接受");
  }

  {
    // 带参数项：按 ';' 分段找 q=，不能被别的参数值里的 "q=" 骗到
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.set_header("Accept-Encoding", "gzip;q=0.5;note=aq=1");
    uvcpp_web_request w;
    w.take_from(src);
    check(w.accepts_encoding("gzip"), "gzip;q=0.5 可接受");
  }

  {
    // '*' 通配
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.set_header("Accept-Encoding", "*");
    uvcpp_web_request w;
    w.take_from(src);
    check(w.accepts_encoding("gzip"), "通配 '*' 接受 gzip");
    check(w.accepts_encoding("br"), "通配 '*' 接受 br");
  }

  {
    // 通配 q=0。RFC 7231 §5.3.4 明确写了 identity 的例外规则：
    // 「无内容编码的表示默认可接受，**除非** Accept-Encoding 里用
    // `identity;q=0` 或 `*;q=0`（且没有更具体的 identity 条目）把它排除」。
    // 所以 `*;q=0` 连 identity 一起拒绝 —— 客户端在说"什么编码都别用，
    // 包括别用 identity"，实践中就是"别给我 body"。
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.set_header("Accept-Encoding", "*;q=0");
    uvcpp_web_request w;
    w.take_from(src);
    check(!w.accepts_encoding("gzip"), "通配 q=0 拒绝 gzip");
    check(!w.accepts_encoding("identity"), "通配 q=0 也拒绝 identity（RFC 7231）");
  }

  {
    // 而显式的 `identity;q=0.5` 比通配更具体，必须赢过通配的 q=0。
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.set_header("Accept-Encoding", "identity;q=0.5, *;q=0");
    uvcpp_web_request w;
    w.take_from(src);
    check(w.accepts_encoding("identity"), "更具体的 identity 条目覆盖通配");
    check(!w.accepts_encoding("gzip"), "通配 q=0 仍然拒绝其它编码");
  }

  {
    // 显式拒绝 identity
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.set_header("Accept-Encoding", "identity;q=0");
    uvcpp_web_request w;
    w.take_from(src);
    check(!w.accepts_encoding("identity"), "identity;q=0 是明确拒绝");
  }

  {
    // 大小写不敏感
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.set_header("Accept-Encoding", "GZIP");
    uvcpp_web_request w;
    w.take_from(src);
    check(w.accepts_encoding("gzip"), "编码名大小写不敏感");
  }
}

// =========================================================================
// 8. keep-alive 判定
// =========================================================================
void test_keep_alive() {
  std::cout << "[request] keep-alive 判定" << std::endl;

  // HTTP/1.1 默认保持
  {
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_GET, "/x");
    check(w.is_keep_alive(), "HTTP/1.1 无 Connection 头 → 保持");
  }

  // HTTP/1.1 + close
  {
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.set_header("Connection", "close");
    uvcpp_web_request w;
    w.take_from(src);
    check(!w.is_keep_alive(), "HTTP/1.1 + Connection: close → 关闭");
  }

  // **按 token 匹配**：整串比较会把 "keep-alive, Upgrade" 判成关闭。
  // 这是 websocket 升级请求的实际形态。
  {
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.set_header("Connection", "keep-alive, Upgrade");
    uvcpp_web_request w;
    w.take_from(src);
    check(w.is_keep_alive(), "Connection 是逗号列表，必须按 token 匹配");
  }

  // 上一条在 HTTP/1.1 下**打不出**整串比较的错 —— 1.1 只要没写 close
  // 就保持，整串比较把 has_keep_alive 算成 false 也照样返回 true。
  // 换成 HTTP/1.0（默认关闭，只有显式 keep-alive 才保持）才能让
  // "必须按 token 匹配"这件事真正决定结果。
  {
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.version = uvcpp_http_version::HVER_10;
    src.set_header("Connection", "keep-alive, Upgrade");
    uvcpp_web_request w;
    w.take_from(src);
    check(w.is_keep_alive(),
          "HTTP/1.0 下 'keep-alive, Upgrade' 必须识别出 keep-alive token");
  }

  // close 出现在列表任意位置都要认
  {
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.set_header("Connection", "Upgrade, close");
    uvcpp_web_request w;
    w.take_from(src);
    check(!w.is_keep_alive(), "列表中的 close 必须生效");
  }

  // HTTP/1.0 默认关闭
  {
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.version = uvcpp_http_version::HVER_10;
    uvcpp_web_request w;
    w.take_from(src);
    check(!w.is_keep_alive(), "HTTP/1.0 默认关闭");
  }

  // HTTP/1.0 + keep-alive
  {
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x");
    src.version = uvcpp_http_version::HVER_10;
    src.set_header("Connection", "Keep-Alive");
    uvcpp_web_request w;
    w.take_from(src);
    check(w.is_keep_alive(), "HTTP/1.0 + keep-alive → 保持");
  }
}

// =========================================================================
// 9. 表单与 Cookie
// =========================================================================
void test_form_and_cookies() {
  std::cout << "[request] 表单与 Cookie" << std::endl;

  {
    uvcpp_http_request src = make_http_req(
        http_method::HTTP_POST, "/login", "user=alice&pass=a+b%21&empty=");
    src.set_header("Content-Type",
                   "application/x-www-form-urlencoded; charset=utf-8");
    uvcpp_web_request w;
    w.take_from(src);

    check(w.is_form(), "is_form 忽略 charset 参数");
    check(!w.is_multipart(), "urlencoded 不是 multipart");
    check(!w.is_json(), "urlencoded 不是 json");

    check_eq(*w.form("user"), "alice", "表单字段");
    check_eq(*w.form("pass"), "a b!", "表单中的 '+' 是空格，%21 是 '!'");
    check_eq(*w.form("empty"), "", "空值字段");
    check(w.form("nope") == nullptr, "不存在的字段返回 nullptr");
    check_eq_i(static_cast<long long>(w.form_params().size()), 3, "字段数");
  }

  {
    // 非表单 content-type：不解析，返回空列表（而不是拿二进制乱解一通）
    uvcpp_http_request src =
        make_http_req(http_method::HTTP_POST, "/x", "user=alice");
    src.set_header("Content-Type", "text/plain");
    uvcpp_web_request w;
    w.take_from(src);
    check(!w.is_form(), "text/plain 不是表单");
    check(w.form_params().empty(), "非表单不解析 body");
    check(w.form("user") == nullptr, "非表单取不到字段");
  }

  {
    // multipart 只做识别，Phase 1 不解析（Phase 3 做流式解析）
    uvcpp_http_request src = make_http_req(http_method::HTTP_POST, "/up", "");
    src.set_header("Content-Type",
                   "multipart/form-data; boundary=----abc123");
    uvcpp_web_request w;
    w.take_from(src);
    check(w.is_multipart(), "识别 multipart/form-data");
    check(!w.is_form(), "multipart 不是 urlencoded");
  }

  {
    uvcpp_http_request src = make_http_req(http_method::HTTP_GET, "/x", "");
    src.set_header("Cookie", "sid=abc123; theme=dark; empty=");
    uvcpp_web_request w;
    w.take_from(src);

    check_eq(*w.cookie("sid"), "abc123", "Cookie 取值");
    check_eq(*w.cookie("theme"), "dark", "Cookie 第二个值");
    check_eq(*w.cookie("empty"), "", "空值 Cookie");
    check(w.cookie("nope") == nullptr, "不存在的 Cookie 返回 nullptr");
  }

  {
    // Cookie 头缺失
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_GET, "/x");
    check(w.cookie("sid") == nullptr, "无 Cookie 头时返回 nullptr");
  }
}

// =========================================================================
// 10. JSON 判定与解析
// =========================================================================
void test_json_basic() {
  std::cout << "[request] JSON 判定与解析" << std::endl;

  {
    uvcpp_http_request src = make_http_req(
        http_method::HTTP_POST, "/api", "{\"name\":\"alice\",\"age\":30}");
    src.set_header("Content-Type", "application/json");
    uvcpp_web_request w;
    w.take_from(src);

    check(w.is_json(), "application/json");

    uvcpp_json j;
    json_status st = json_status::EMPTY;
    check(w.json(j, &st), "解析成功");
    check(st == json_status::OK, "状态是 OK");
    check_eq(uvcpp_json_get_string(j, "name", ""), "alice", "取字符串");
    check(uvcpp_json_get_bool(j, "missing", true), "缺失布尔取默认值");

    long long age = -1;
    check(uvcpp_json_get_int(j, "age", age), "取整数成功");
    check_eq_i(age, 30, "整数值");
  }

  {
    // +json 结构化后缀（RFC 6839）
    uvcpp_http_request src =
        make_http_req(http_method::HTTP_POST, "/api", "{}");
    src.set_header("Content-Type", "application/vnd.api+json");
    uvcpp_web_request w;
    w.take_from(src);
    check(w.is_json(), "application/vnd.api+json 按 JSON 处理");
  }

  {
    // charset 参数不影响判定
    uvcpp_http_request src = make_http_req(http_method::HTTP_POST, "/api", "{}");
    src.set_header("Content-Type", "application/json; charset=UTF-8");
    uvcpp_web_request w;
    w.take_from(src);
    check(w.is_json(), "带 charset 的 application/json");
  }

  {
    uvcpp_http_request src = make_http_req(http_method::HTTP_POST, "/x", "{}");
    src.set_header("Content-Type", "text/plain");
    uvcpp_web_request w;
    w.take_from(src);
    check(!w.is_json(), "text/plain 不是 JSON");
  }

  // --- 状态分类 ---
  {
    // 空 body 报 EMPTY 而不是 SYNTAX：调用方对这两种情况要返回不同响应
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_POST, "/api");
    uvcpp_json j;
    json_status st = json_status::OK;
    check(!w.json(j, &st), "空 body 解析失败");
    check(st == json_status::EMPTY, "空 body 的状态是 EMPTY（不是 SYNTAX）");
  }

  {
    uvcpp_web_request w;
    make_web_req(w, http_method::HTTP_POST, "/api", "{\"a\": }");
    uvcpp_json j;
    json_status st = json_status::OK;
    check(!w.json(j, &st), "语法错误解析失败");
    check(st == json_status::SYNTAX, "语法错误的状态是 SYNTAX");
  }

  {
    // 便捷版：失败返回 null，不抛异常
    uvcpp_web_request w;
    make_web_req(w, http_method::HTTP_POST, "/api", "not json at all");
    check(w.json().is_null(), "便捷版失败时返回 null");
  }

  {
    // 失败时**不改动** out（调用方可以先放一个默认值进去）
    uvcpp_json j = uvcpp_json::object();
    j["keep"] = "me";
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_POST, "/api", "{{{");
    check(!w.json(j, nullptr), "解析失败");
    check_eq(uvcpp_json_get_string(j, "keep", ""), "me",
             "失败时不改动输出对象");
  }
}

// =========================================================================
// 11. JSON 深度防护
// =========================================================================
void test_json_limits() {
  std::cout << "[request] JSON 深度与大小防护" << std::endl;

  // 嵌套扫描必须**跳过字符串字面量**
  {
    const char* s = "{\"a\":\"[[[[[[[[[[\"}";
    check_eq_i(static_cast<long long>(uvcpp_json_max_nesting(s, std::strlen(s))),
               1, "字符串里的括号不计深度");
  }

  // 转义引号不能提前结束字符串状态
  {
    const char* s = "{\"a\":\"x\\\"[[[[[[\"}";
    check_eq_i(static_cast<long long>(uvcpp_json_max_nesting(s, std::strlen(s))),
               1, "转义引号不结束字符串");
  }

  {
    // 用代码拼而不是手写字面量：手数括号个数是这种用例最容易错的地方
    // （写的时候就把 6 层看成了 7 层）。
    const std::string s = std::string(7, '[') + std::string(7, ']');
    check_eq_i(static_cast<long long>(uvcpp_json_max_nesting(s.data(), s.size())),
               7, "数组嵌套计数");
  }

  {
    const char* s = "{{{}}}";
    check_eq_i(static_cast<long long>(uvcpp_json_max_nesting(s, std::strlen(s))),
               3, "对象嵌套计数");
  }

  {
    // 不配对的多余闭合括号不该把深度算成负数
    const char* s = "]]]]";
    check_eq_i(static_cast<long long>(uvcpp_json_max_nesting(s, std::strlen(s))),
               0, "多余闭合括号不产生负深度");
  }

  // 超过默认深度（64）：必须在**解析之前**被拒，否则递归下降解析器会爆栈
  {
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += '[';
    for (int i = 0; i < 200; ++i) deep += ']';

    uvcpp_web_request w; make_web_req(w, http_method::HTTP_POST, "/api", deep);
    uvcpp_json j;
    json_status st = json_status::OK;
    check(!w.json(j, &st), "超深 JSON 被拒");
    check(st == json_status::TOO_DEEP, "状态是 TOO_DEEP");
  }

  // 恰好等于上限应当通过（边界是闭区间）
  {
    const size_t depth = 64;
    std::string ok;
    for (size_t i = 0; i < depth; ++i) ok += '[';
    for (size_t i = 0; i < depth; ++i) ok += ']';

    uvcpp_json j;
    check(uvcpp_json_parse(ok.data(), ok.size(), j) == json_status::OK,
          "恰好等于 max_depth 应当通过");
  }

  // 显式传更小的上限
  {
    const std::string s = "[[[]]]";
    uvcpp_json j;
    json_parse_options opts(2, 8u * 1024u * 1024u);
    check(uvcpp_json_parse(s.data(), s.size(), j, opts) ==
              json_status::TOO_DEEP,
          "自定义 max_depth 生效");
  }

  // 超过 max_bytes
  {
    const std::string s = "[\"aaaaaaaaaa\"]";
    uvcpp_json j;
    json_parse_options opts(64, 4);
    check(uvcpp_json_parse(s.data(), s.size(), j, opts) ==
              json_status::TOO_LARGE,
          "自定义 max_bytes 生效");
  }

  // nullptr / 零长度
  {
    uvcpp_json j;
    check(uvcpp_json_parse(static_cast<const char*>(nullptr), 0, j) ==
              json_status::EMPTY,
          "nullptr 输入返回 EMPTY 而不是崩溃");
  }
}

// =========================================================================
// 12. JSON 序列化与取值
// =========================================================================
void test_json_dump_and_get() {
  std::cout << "[request] JSON 序列化与取值" << std::endl;

  {
    uvcpp_json j;
    j["a"] = 1;
    j["b"] = "two";
    check_eq(uvcpp_json_dump(j), "{\"a\":1,\"b\":\"two\"}", "紧凑序列化");
    check(!uvcpp_json_dump(j, 2).empty(), "带缩进序列化");
    check(!uvcpp_json_dump(j, 0).empty(), "indent<=0 等价于紧凑");
  }

  {
    // 非法 UTF-8 的字符串会让 dump() 抛 type_error.316。调用点常在响应
    // 发送路径上，那里绝不能抛 —— 必须吞掉并返回空串。
    uvcpp_json j;
    j["bad"] = std::string("\xff\xfe");
    const std::string out = uvcpp_json_dump(j);
    check(out.empty(), "非法 UTF-8 时返回空串而不是抛异常");
  }

  {
    // 取值函数：缺失键、类型不符都给默认值，且**不修改**源对象
    uvcpp_json j;
    j["s"] = "str";
    j["i"] = 42;
    j["d"] = 1.5;
    j["b"] = true;

    check_eq(uvcpp_json_get_string(j, "s", "def"), "str", "取字符串");
    check_eq(uvcpp_json_get_string(j, "missing", "def"), "def", "缺失键用默认值");
    check_eq(uvcpp_json_get_string(j, "i", "def"), "def", "类型不符用默认值");

    check(uvcpp_json_get_bool(j, "b", false), "取布尔");
    check(!uvcpp_json_get_bool(j, "missing", false), "缺失布尔用默认值");

    long long iv = 0;
    check(uvcpp_json_get_int(j, "i", iv) && iv == 42, "取整数");
    check(uvcpp_json_get_int(j, "d", iv) && iv == 1, "浮点截断成整数");
    check(!uvcpp_json_get_int(j, "s", iv), "字符串不是整数");

    double dv = 0.0;
    check(uvcpp_json_get_double(j, "d", dv) && dv == 1.5, "取浮点");
    check(uvcpp_json_get_double(j, "i", dv) && dv == 42.0, "整数也能当浮点读");
    check(!uvcpp_json_get_double(j, "missing", dv), "缺失浮点返回 false");

    // **关键**：整个取值过程不得给对象插入任何新键。
    // nlohmann 的 `j["k"]` 在非 const 对象上遇到缺失键会插入 null，
    // 上面的取值函数用 find() 就是为了避免这个。
    check_eq(uvcpp_json_dump(j), "{\"b\":true,\"d\":1.5,\"i\":42,\"s\":\"str\"}",
             "取值不修改源对象（不插入 null）");
  }

  {
    // 非对象上取值
    uvcpp_json arr = uvcpp_json::array();
    arr.push_back(1);
    check_eq(uvcpp_json_get_string(arr, "x", "def"), "def",
             "数组上取键返回默认值");
    check(uvcpp_json_get_string(uvcpp_json(), "x", "def") == "def",
          "null 上取键返回默认值");
  }

  {
    // 状态名可读
    check_eq(json_status_name(json_status::OK), "OK", "状态名 OK");
    check_eq(json_status_name(json_status::TOO_DEEP), "TOO_DEEP",
             "状态名 TOO_DEEP");
  }
}

// =========================================================================
// 13. 方法名 / 版本名 / 对端 / 路径参数
// =========================================================================
void test_misc() {
  std::cout << "[request] 方法名/版本名/对端/路径参数" << std::endl;

  {
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_DELETE, "/x");
    check_eq(w.method_name(), "DELETE", "method_name");
    check_eq(w.version_name(), "HTTP/1.1", "version_name");
  }

  {
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_GET, "/x");
    check(w.peer_ip().empty(), "未填充时 peer_ip 为空");
    check_eq_i(static_cast<long long>(w.peer_port()), 0, "未填充时 peer_port 为 0");

    w.set_peer("127.0.0.1", 54321);
    check_eq(w.peer_ip(), "127.0.0.1", "set_peer 生效");
    check_eq_i(static_cast<long long>(w.peer_port()), 54321, "端口生效");
  }

  {
    uvcpp_web_request w; make_web_req(w, http_method::HTTP_GET, "/user/7");
    check(w.param("id") == nullptr, "未填充时取不到路径参数");
    check(w.params().empty(), "未填充时参数列表为空");

    w.set_param("id", "7");
    w.set_param("name", "alice");
    check_eq(*w.param("id"), "7", "路径参数 id");
    check_eq(*w.param("name"), "alice", "路径参数 name");
    check_eq_i(static_cast<long long>(w.params().size()), 2, "参数个数");
  }
}

}  // namespace

int main() {
  // 每个插入都立刻 flush：这个二进制跑在管道里时 stdout 是块缓冲的，
  // 一旦崩溃，崩溃点之前的若干行会连同缓冲区一起丢掉 —— 那就再也查不出
  // 是哪个用例炸的。
  std::cout << std::unitbuf;

  test_take_from_zero_copy();
  test_binary_body();
  test_path_and_query();
  test_query_params();
  test_headers();
  test_content_length();
  test_accepts_encoding();
  test_keep_alive();
  test_form_and_cookies();
  test_json_basic();
  test_json_limits();
  test_json_dump_and_get();
  test_misc();

  if (g_failures == 0) {
    std::cout << "[request] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[request] FAIL (" << g_failures << " checks failed)" << std::endl;
  return 2;
}
