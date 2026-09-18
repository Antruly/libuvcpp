/**
 * @file tests/functional/h2_session_func.cpp
 * @brief h2 会话层：请求/响应转换与协议校验。
 *
 * 这个用例**不开 socket、不用 TLS、不跑事件循环** —— 它把两条 `uvcpp_h2_session`
 * 面对面放好，靠 `drain()`/`recv()` 手动搬字节。理由是判据要落在**转换层**上：
 * 一旦掺进 libuv 与 OpenSSL，"响应不对"和"字节没送到"就分不开了。
 * 接 socket 的那条路（ALPN + `uvcpp_h2_connection`）由 `web_ssl_h2_*` 那批覆盖。
 */

#include <cstring>
#include <memory>
#include <iostream>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_NGHTTP2_ENABLE

#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

#include <http2/uvcpp_h2_session.h>

using namespace uvcpp;

namespace {

// =========================================================================
// 一对面对面的会话
// =========================================================================

struct link {
  /// 诊断用：最后一个构造出来的 link。失败时打印它的观测值。
  static link* probe_owner;

  uvcpp_h2_session client{false};
  uvcpp_h2_session server{true};

  /// 服务端看到的东西
  int         req_count   = 0;
  int         body_end    = 0;
  int         rst_count   = 0;
  http_method last_method = http_method::HTTP_GET;
  std::string last_url;
  std::string last_body;
  uvcpp_http_version last_version = uvcpp_http_version::HVER_10;
  http_headers last_headers;
  int32_t      last_stream  = 0;
  bool         last_end     = false;
  std::string  last_host;

  /// 客户端看到的东西
  int         resp_count = 0;
  int         resp_status = 0;
  std::string resp_body;
  http_headers resp_headers;
  bool        resp_end = false;   ///< 整条响应收完（on_response_end）
  int         client_closed = 0;

  /// 已经回过响应的流。**一条流只许回一次** —— `submit_response` 第二次会换掉
  /// 一块正被 nghttp2 的 data provider 按地址引用的缓冲，是 use-after-free。
  /// 库侧现在会返回 UV_EALREADY，这里留一份是为了让用例本身别去撞那条路。
  std::vector<int32_t> replied;

  /// 服务端的响应行为，由各用例设置
  uvcpp_http_response reply;
  bool                reply_omit_body = false;
  /// 关掉自动应答，好让用例在"流还活着"的窗口里手工操作。
  bool                auto_reply      = true;
  bool                reply_as_status = false;
  int                 reply_status    = 200;
  std::string         reply_status_body;

  link() { probe_owner = this; }
  ~link() { if (probe_owner == this) probe_owner = nullptr; }

  bool init() {
    uvcpp_h2_session::callbacks sc;
    sc.on_request     = [this](uvcpp_h2_session& s, uvcpp_h2_stream& st, bool end) {
      ++req_count;
      last_method  = st.request.method;
      last_url     = st.request.url;
      last_version = st.request.version;
      last_headers = st.request.headers;
      last_host    = st.request.get_header("host");
      last_stream  = st.stream_id;
      last_end     = end;
      last_body.clear();
      if (end) maybe_reply(s, st.stream_id);
    };
    // body 是分块来的，只能逐块累积。
    sc.on_body = [this](uvcpp_h2_session&, uvcpp_h2_stream&, const char* d,
                        size_t n) { last_body.append(d, n); };
    sc.on_request_end = [this](uvcpp_h2_session& s, uvcpp_h2_stream& st) {
      ++body_end;
      maybe_reply(s, st.stream_id);
    };
    if (server.init(sc) != 0) return false;

    uvcpp_h2_session::callbacks cc;
    // 注意 `end` 是"头带了 END_STREAM"，不是"响应收完了" —— 收尾只认
    // `on_response_end`，两者在有 body 的响应上必然不同。
    cc.on_response = [this](uvcpp_h2_session&, uvcpp_h2_stream& st, bool) {
      ++resp_count;
      resp_status  = static_cast<int>(st.response.status_code);
      resp_headers = st.response.headers;
    };
    cc.on_body = [this](uvcpp_h2_session&, uvcpp_h2_stream&, const char* d,
                        size_t n) {
      resp_body.append(d, n);
    };
    cc.on_response_end = [this](uvcpp_h2_session&, uvcpp_h2_stream&) {
      resp_end = true;
    };
    cc.on_close = [this](uvcpp_h2_session&, int32_t, uint32_t code) {
      if (code != 0) ++rst_count;
      ++client_closed;
    };
    return client.init(cc) == 0;
  }

  /// 诊断用：失败时把两边看到的东西全打出来（这些值本来就都是成员）。
  void dump(const char* tag) {
    std::cout << "    [dump " << tag << "] req=" << req_count
              << " body_end=" << body_end << " rst=" << rst_count
              << " end=" << (last_end ? 1 : 0) << " url=" << last_url
              << " host=" << last_host << " req_body=" << last_body
              << " svr_streams=" << server.stream_count()
              << " cli_streams=" << client.stream_count() << "\n";
    std::cout << "    [dump " << tag << "] resp=" << resp_count
              << " status=" << resp_status << " resp_end=" << (resp_end ? 1 : 0)
              << " resp_body=" << resp_body
              << " hdrs=" << resp_headers.size() << "\n";
    for (size_t i = 0; i < resp_headers.size(); ++i) {
      std::cout << "      rh[" << i << "] " << resp_headers[i].name << ": "
                << resp_headers[i].value << "\n";
    }
    for (size_t i = 0; i < last_headers.size(); ++i) {
      std::cout << "      qh[" << i << "] " << last_headers[i].name << ": "
                << last_headers[i].value << "\n";
    }
  }

  void maybe_reply(uvcpp_h2_session& s, int32_t sid) {
    if (!auto_reply) return;
    for (size_t i = 0; i < replied.size(); ++i) {
      if (replied[i] == sid) return;
    }
    replied.push_back(sid);
    if (reply_as_status) {
      s.submit_status(sid, reply_status, reply_status_body);
    } else {
      s.submit_response(sid, reply, reply_omit_body);
    }
  }

  /// 两边的字节来回搬，直到双方都没东西可发。
  void pump() {
    for (int i = 0; i < 256; ++i) {
      bool moved = false;
      std::string b;
      if (client.drain(b) == 0 && !b.empty()) {
        server.recv(b.data(), b.size());
        moved = true;
      }
      b.clear();
      if (server.drain(b) == 0 && !b.empty()) {
        client.recv(b.data(), b.size());
        moved = true;
      }
      if (!moved) return;
    }
  }
};

link* link::probe_owner = nullptr;

/// 用例改持有 `link&`：栈上的 link 在用例返回时就没了，而失败信息是**返回之后**
/// 才打印的 —— 那样读的是死内存（本批已经崩过一次）。留一份就都不成问题。
link& make_link() {
  static std::vector<std::unique_ptr<link> > keep;
  keep.push_back(std::unique_ptr<link>(new link()));
  return *keep.back();
}

/// `uvcpp_buf` 的 2 参构造是"拷贝 sz 字节"，用长度常量写容易和字面量长度对不上。
uvcpp_buf mk_buf(const char* s) { return uvcpp_buf(s, std::strlen(s)); }

uvcpp_http_request make_req(http_method m, const std::string& url,
                            const std::string& host = "example.com") {
  uvcpp_http_request r;
  r.method = m;
  r.url    = url;
  r.set_header("host", host);
  return r;
}

// =========================================================================
// 用例
// =========================================================================

bool test_get_simple() {
  link& L = make_link();
  if (!L.init()) return false;

  L.reply.status_code = http_status::OK;
  L.reply.set_header("content-type", "text/plain");
  L.reply.body = mk_buf("world");

  const int32_t sid = L.client.submit_request(make_req(http_method::HTTP_GET, "/hello"), "");
  if (sid <= 0) return false;
  L.pump();

  if (L.req_count != 1) return false;
  if (L.last_method != http_method::HTTP_GET) return false;
  if (L.last_url != "/hello") return false;
  if (L.last_version != uvcpp_http_version::HVER_20) return false;
  if (L.last_host != "example.com") return false;
  if (!L.last_end) return false;

  if (L.resp_count != 1) return false;
  if (L.resp_status != 200) return false;
  if (L.resp_body != "world") return false;
  if (L.resp_end != true) return false;
  // `:status` 不是常规头，绝不能同时出现在 headers 里；`connection` 也绝不该有。
  if (http_get_header(L.resp_headers, ":status") != "") return false;
  if (http_get_header(L.resp_headers, "connection") != "") return false;
  if (http_get_header(L.resp_headers, "transfer-encoding") != "") return false;
  if (http_get_header(L.resp_headers, "content-length") != "5") return false;
  if (http_get_header(L.resp_headers, "content-type") != "text/plain") return false;

  // 两边都结束了 ⇒ 流表该清空。
  return L.server.stream_count() == 0 && L.client.stream_count() == 0;
}

bool test_post_with_body() {
  link& L = make_link();
  if (!L.init()) return false;

  L.reply.status_code = http_status::CREATED;
  L.reply.body        = mk_buf("ok");

  uvcpp_http_request r = make_req(http_method::HTTP_POST, "/submit");
  r.set_header("content-length", "5");
  const int32_t sid = L.client.submit_request(r, "hello");
  if (sid <= 0) return false;
  L.pump();

  if (L.req_count != 1) return false;
  if (L.last_method != http_method::HTTP_POST) return false;
  if (L.last_end) return false;           // 有 body 时 on_request 的 end 是 false
  if (L.body_end != 1) return false;      // 但整体确实收完了
  if (L.last_body != "hello") return false;
  // `content-length` 要原样带给业务层。
  if (http_get_header(L.last_headers, "content-length") != "5") return false;

  if (L.resp_count != 1) return false;
  if (L.resp_status != 201) return false;
  if (L.resp_body != "ok") return false;
  return true;
}

bool test_head_has_no_data() {
  link& L = make_link();
  if (!L.init()) return false;

  // HEAD 的 content-length 是"GET 会有多长"，但一个 DATA 字节都不许发。
  L.reply.status_code = http_status::OK;
  L.reply.set_header("content-length", "12345");
  L.reply.body        = mk_buf("should-not-be-sent");
  L.reply_omit_body   = true;

  const int32_t sid =
      L.client.submit_request(make_req(http_method::HTTP_HEAD, "/x"), "");
  if (sid <= 0) return false;
  L.pump();

  if (L.resp_count != 1) return false;
  if (L.resp_status != 200) return false;
  if (!L.resp_body.empty()) return false;   // 没有 DATA
  if (L.resp_end != true) return false;     // 但 HEADERS 自带 END_STREAM
  if (http_get_header(L.resp_headers, "content-length") != "12345") return false;
  return true;
}

bool test_custom_headers_roundtrip() {
  link& L = make_link();
  if (!L.init()) return false;

  L.reply.status_code = http_status::OK;
  L.reply.set_header("x-server", "uvcpp");

  uvcpp_http_request r = make_req(http_method::HTTP_GET, "/h");
  r.set_header("x-custom", "abc");
  r.set_header("accept", "text/plain");
  const int32_t sid = L.client.submit_request(r, "");
  if (sid <= 0) return false;
  L.pump();

  if (http_get_header(L.last_headers, "x-custom") != "abc") return false;
  if (http_get_header(L.last_headers, "accept") != "text/plain") return false;
  // `:authority` 按设计落进 `headers["host"]` —— 业务层的 h1 代码原样能读。
  // 要求的是**恰好一条**：`:authority` 一条 + 重复出现的常规 `host` 就是走私面。
  if (http_get_header(L.last_headers, "host") != "example.com") return false;
  int host_hdrs = 0;
  for (size_t i = 0; i < L.last_headers.size(); ++i) {
    if (http_name_equal(L.last_headers[i].name, "host")) ++host_hdrs;
  }
  if (host_hdrs != 1) return false;
  // 伪头一个都不许漏进常规头表。
  if (http_get_header(L.last_headers, ":authority") != "") return false;
  if (http_get_header(L.last_headers, ":method") != "") return false;
  if (http_get_header(L.last_headers, ":path") != "") return false;
  if (http_get_header(L.last_headers, ":scheme") != "") return false;
  // 空 body 的响应不该凭空多出 content-length。
  if (http_get_header(L.resp_headers, "content-length") != "") return false;

  if (http_get_header(L.resp_headers, "x-server") != "uvcpp") return false;
  return true;
}

bool test_status_only_reply() {
  link& L = make_link();
  if (!L.init()) return false;

  L.reply_as_status    = true;
  L.reply_status       = 404;
  L.reply_status_body  = "nope";

  const int32_t sid =
      L.client.submit_request(make_req(http_method::HTTP_GET, "/missing"), "");
  if (sid <= 0) return false;
  L.pump();

  if (L.resp_status != 404) return false;
  if (L.resp_body != "nope") return false;
  return true;
}

/// `content-length` 与 DATA 实际长度不符 —— h2→h1 走私的标准手法。
bool test_content_length_mismatch_is_reset() {
  link& L = make_link();
  if (!L.init()) return false;

  L.reply_as_status   = true;
  L.reply_status      = 200;
  L.reply_status_body = "x";

  uvcpp_http_request r = make_req(http_method::HTTP_POST, "/smuggle");
  r.set_header("content-length", "99");  // 撒谎：实际只发 5 字节
  const int32_t sid = L.client.submit_request(r, "hello");
  if (sid <= 0) return false;
  L.pump();

  // 服务端必须拒掉：既不能把它当完整请求交给业务层，也不能回 200。
  if (L.body_end != 0) return false;
  if (L.resp_status == 200) return false;
  // 客户端应当看到这条流以非 0 错误码关闭。
  if (L.rst_count == 0) return false;
  return true;
}

/// 对端宣告的 MAX_CONCURRENT_STREAMS 要能被读到（批 2c 靠它做并发闸门）。
bool test_settings_visible() {
  link& L = make_link();
  if (!L.init()) return false;
  L.pump();
  return L.client.peer_max_concurrent_streams() ==
         H2_DEFAULT_MAX_CONCURRENT_STREAMS;
}

/// 一条流上第二次 `submit_response` 必须被拒。
///
/// 放过去会 `stash_body` 换掉一块正被 nghttp2 的 data provider **按地址**引用的
/// 缓冲 —— 也就是 use-after-free。这不是假想的：本批第一次跑测试时用例就是这么
/// 写的（`on_request` 与 `on_request_end` 各回一次），表现是响应体随机丢失 + 莫名
/// 的 RST，而状态码和 content-length 全对。判据落在**返回值**上，才不依赖"恰好崩"。
///
/// 必须在流还活着时提交 —— 流一关 `on_stream_close` 就把条目删了，那时返回的是
/// `UV_EINVAL`（"没这条流"），根本走不到这道闸门。
bool test_second_response_is_refused() {
  link& L = make_link();
  if (!L.init()) return false;
  L.auto_reply = false;  // 手工控制提交时机

  L.reply.status_code = http_status::OK;
  L.reply.body        = mk_buf("world");

  const int32_t sid =
      L.client.submit_request(make_req(http_method::HTTP_GET, "/x"), "");
  if (sid <= 0) return false;
  L.pump();  // 请求到达服务端；服务端没回，流仍开着

  if (L.req_count != 1) return false;
  if (L.server.stream_count() == 0) return false;  // 前提：流还在

  if (L.server.submit_response(sid, L.reply, false) != 0) return false;
  if (L.server.submit_response(sid, L.reply, false) != UV_EALREADY) return false;

  L.pump();
  // 被拒的那次没有污染任何东西：响应照常、且只到了一次。
  if (L.resp_count != 1) return false;
  if (L.resp_status != 200) return false;
  if (L.resp_body != "world") return false;
  if (!L.resp_end) return false;
  // 服务端主动 RST 也没发生 —— 这正是当初那版用例的现场。
  if (L.rst_count != 0) return false;
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"get_simple", test_get_simple},
    {"post_with_body", test_post_with_body},
    {"head_has_no_data", test_head_has_no_data},
    {"custom_headers_roundtrip", test_custom_headers_roundtrip},
    {"status_only_reply", test_status_only_reply},
    {"content_length_mismatch_is_reset", test_content_length_mismatch_is_reset},
    {"settings_visible", test_settings_visible},
    {"second_response_is_refused", test_second_response_is_refused},
  };
  for (const auto& t : tests) {
    std::cout << "[h2_session] " << t.name << "\n";
    link::probe_owner = nullptr;
    const bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    if (!r && link::probe_owner) link::probe_owner->dump(t.name);
    ok = r && ok;
  }
  std::cout << "[h2_session] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}

#else  // UVCPP_NGHTTP2_ENABLE

int main() {
  std::cout << "[h2_session] SKIP (nghttp2 disabled)\n";
  return 0;
}

#endif  // UVCPP_NGHTTP2_ENABLE
