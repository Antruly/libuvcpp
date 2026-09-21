#include <iostream>
#include <string>
#include <cstring>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_parser.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

using namespace uvcpp;

static bool test_parse_request_line() {
  uvcpp_http_parser parser(http_parser_mode::PARSE_REQUEST);
  const char* raw =
      "GET /index.html HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Accept: */*\r\n"
      "\r\n";
  parser.execute(raw, strlen(raw));
  parser.finish();
  if (!parser.is_complete() || parser.has_error()) return false;
  if (parser.get_method() != http_method::HTTP_GET) return false;
  if (parser.get_url() != "/index.html") return false;
  if (parser.get_uvcpp_http_version() != uvcpp_http_version::HVER_11) return false;
  if (parser.get_headers().size() < 2) return false;
  return true;
}

static bool test_parse_response_line() {
  uvcpp_http_parser parser(http_parser_mode::PARSE_RESPONSE);
  const char* raw =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 5\r\n"
      "\r\n"
      "Hello";
  parser.execute(raw, strlen(raw));
  parser.finish();
  if (!parser.is_complete() || parser.has_error()) return false;
  if (parser.get_status_code() != http_status::OK) return false;
  return true;
}

static bool test_request_to_string() {
  uvcpp_http_request req;
  req.method = http_method::HTTP_GET;
  req.url = "/api/test";
  req.set_header("accept", "application/json");
  std::string s = req.to_string();
  if (s.find("GET /api/test HTTP/1.1") == std::string::npos) return false;
  if (s.find("accept: application/json") == std::string::npos) return false;
  return true;
}

static bool test_response_to_string() {
  uvcpp_http_response resp;
  resp.status_code = http_status::NOT_FOUND;
  resp.set_header("content-type", "text/plain");
  resp.body.clone_data("Not Found", 9);
  std::string s = resp.to_string();
  if (s.find("HTTP/1.1 404") == std::string::npos) return false;
  if (s.find("Not Found") == std::string::npos) return false;
  return true;
}

static bool test_version_enum() {
  if (static_cast<uint8_t>(uvcpp_http_version::HVER_10) != 0) return false;
  if (static_cast<uint8_t>(uvcpp_http_version::HVER_11) != 1) return false;
  if (static_cast<uint8_t>(uvcpp_http_version::HVER_20) != 2) return false;
  if (std::string(uvcpp_http_version_str(uvcpp_http_version::HVER_10)) != "HTTP/1.0") return false;
  if (std::string(uvcpp_http_version_str(uvcpp_http_version::HVER_11)) != "HTTP/1.1") return false;
  return true;
}

static bool test_header_ops() {
  http_headers hdrs;
  http_set_header(hdrs, "Content-Type", "text/html");
  http_set_header(hdrs, "X-Custom", "value");
  if (http_get_header(hdrs, "content-type") != "text/html") return false;
  if (!http_has_header(hdrs, "CONTENT-TYPE")) return false;
  if (http_has_header(hdrs, "not-exists")) return false;
  return true;
}

static bool test_parser_reset() {
  uvcpp_http_parser parser(http_parser_mode::PARSE_REQUEST);
  const char* req1 = "GET /first HTTP/1.1\r\n\r\n";
  parser.execute(req1, strlen(req1));
  parser.finish();
  if (!parser.is_complete()) return false;
  if (parser.get_url() != "/first") return false;
  parser.reset();
  const char* req2 = "POST /second HTTP/1.1\r\n\r\n";
  parser.execute(req2, strlen(req2));
  parser.finish();
  if (!parser.is_complete()) return false;
  if (parser.get_method() != http_method::HTTP_POST) return false;
  if (parser.get_url() != "/second") return false;
  return true;
}

// `std::move` 必须真的把 headers/url/body 搬走，而不是静默退化成一次深拷贝。
//
// 这是个**实测出来的**缺口，不是推的：`UVCPP_DEFINE_COPY_FUNC` 只声明拷贝那一
// 对，而**用户声明了拷贝赋值就会抑制隐式移动赋值** —— 于是 `dst = std::move(src)`
// 绑到 `operator=(const uvcpp_http_request&)`，深拷一遍头向量（1 次 vector 分配
// + 每个头 2 个 string）。库内唯一一处 `std::move`（`uvcpp_http_server.cpp` 把
// 请求视图交给处理器那一步）正好踩在上面，注释写着"别拷第二遍"、代码却拷了。
//
// 前面三条是**前置断言**：源里必须先有东西，"被搬空"才是个有意义的判据 ——
// 在一个本来就空的源上判 `empty()` 是恒真的（见本仓"前置要断言"那条教训）。
static bool test_request_move_semantics() {
  uvcpp_http_request src;
  src.method = http_method::HTTP_POST;
  src.url = "/upload";
  src.set_header("x-probe", "1");
  // 故意放一条超过 SSO（MSVC 15 字符）的头：短头走小字符串优化，
  // 深拷贝与真移动的**分配次数**差就看不出来了。
  src.set_header("x-long-header", "0123456789abcdefghij");
  src.body.clone_data("payload", 7);

  if (src.headers.size() < 2) return false;  // 前置
  if (src.url != "/upload") return false;    // 前置
  if (src.body.size() != 7) return false;    // 前置

  uvcpp_http_request dst;
  dst = std::move(src);

  // 搬过去了：目标拿到全部内容。
  if (dst.headers.size() < 2) return false;
  if (dst.url != "/upload") return false;
  if (dst.body.size() != 7) return false;
  if (dst.get_header("x-long-header") != "0123456789abcdefghij") return false;

  // 源被搬空 —— 这一条就是"真移动"与"深拷贝"的分水岭。
  if (!src.headers.empty()) return false;
  if (!src.url.empty()) return false;
  if (src.body.size() != 0) return false;
  return true;
}

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"parse_request_line", test_parse_request_line},
    {"parse_response_line", test_parse_response_line},
    {"request_to_string", test_request_to_string},
    {"response_to_string", test_response_to_string},
    {"version_enum", test_version_enum},
    {"header_ops", test_header_ops},
    {"parser_reset", test_parser_reset},
    {"request_move_semantics", test_request_move_semantics},
  };
  for (const auto& t : tests) {
    std::cout << "[web_http_parser] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_http_parser] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}

#else  // UVCPP_WEB_ENABLE

int main() {
  std::cout << "[web_http_parser] SKIP (web module disabled)\n";
  return 0;
}

#endif  // UVCPP_WEB_ENABLE
