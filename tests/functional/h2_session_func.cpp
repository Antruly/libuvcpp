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
  /// 客户端看到的那条流的 RST 码。只数次数证明不了"为什么被拒" ——
  /// 一次协议错误和一次超预算在只计数器的判据里长得一模一样。
  uint32_t    last_rst_code = 0;
  /// 两边的致命错误次数。绝大多数用例要求它是 0 —— 一个"绿"的用例如果同时
  /// 触发过致命路径，那它绿的很可能不是它想测的那条路。
  int         fatal_count = 0;
  /// 最后一次致命错误的原始码。只数次数的话，"-905 CONTINUATION 过多"和
  /// "-902 FLOODED"在诊断里长得一样，而修法完全不同。
  int         last_fatal = 0;
  /// `pump()` 搬过的字节数。用它区分"帧没发出来"和"发出来了但对端没收"。
  size_t      pumped = 0;
  /// `drain()` 报过的错（非零即"有东西发不出来"）。
  int         drain_err = 0;
  /// 服务端 `submit_*` 那个返回值。本层有几条失败是**必须同步**报出来的
  /// （而不是"给个流号然后永远等不到"），不记下来就无从断言。
  int         last_submit_rc = 0;

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

  /// @param server_budget 服务端**愿意接收**的头部列表上限。默认与库一致
  ///        （64 KiB）。调小它是为了在"客户端发得出去"的前提下测服务端那条防线
  ///        —— 发方向另有 64 KiB 的上限（`H2_MAX_SEND_HEADER_BLOCK`），拿一个
  ///        超过它的头部块去撞收方向，撞到的是发方向，根本到不了服务端。
  bool init(size_t server_budget = 64u * 1024u) {
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
    sc.on_fatal = [this](uvcpp_h2_session&, int c) { ++fatal_count; last_fatal = c; };
    if (server.init(sc, server_budget) != 0) return false;

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
      last_rst_code = code;
      ++client_closed;
    };
    cc.on_fatal = [this](uvcpp_h2_session&, int c) { ++fatal_count; last_fatal = c; };
    return client.init(cc) == 0;
  }

  /// 诊断用：失败时把两边看到的东西全打出来（这些值本来就都是成员）。
  void dump(const char* tag) {
    std::cout << "    [dump " << tag << "] req=" << req_count
              << " body_end=" << body_end << " rst=" << rst_count
              << " end=" << (last_end ? 1 : 0) << " url=" << last_url
              << " host=" << last_host << " req_body=" << last_body
              << " svr_streams=" << server.stream_count()
              << " cli_streams=" << client.stream_count()
              << " pumped=" << pumped << " fatals=" << fatal_count
              << " last_fatal=" << last_fatal << " drain_err=" << drain_err
              << "\n";
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
      last_submit_rc = s.submit_status(sid, reply_status, reply_status_body);
    } else {
      last_submit_rc = s.submit_response(sid, reply, reply_omit_body);
    }
  }

  /// 两边的字节来回搬，直到双方都没东西可发。
  ///
  /// `drain()` 的**返回值必须记下来**：它非零时一个字节都搬不动，而"搬不动"
  /// 和"本来就没事可搬"在这里长得一模一样 —— 曾经就是这么把"客户端根本没发出
  /// HEADERS"读成"服务端没拦住"的。
  void pump() {
    for (int i = 0; i < 256; ++i) {
      bool moved = false;
      std::string b;
      const int cr = client.drain(b);
      if (cr != 0) drain_err = cr;
      if (cr == 0 && !b.empty()) {
        pumped += b.size();
        server.recv(b.data(), b.size());
        moved = true;
      }
      b.clear();
      const int sr = server.drain(b);
      if (sr != 0) drain_err = sr;
      if (sr == 0 && !b.empty()) {
        pumped += b.size();
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

// =========================================================================
// 控制帧洪泛（批 2d）
// =========================================================================

/// 空 SETTINGS 帧的线上字节（RFC 9113 §6.5：9 字节帧头 + 0 字节载荷）。
///
/// 洪泛用例挑它是因为它**定长、无 HPACK、无流号** —— 用例不必先学会编码头块就
/// 能把"连接级控制帧"灌进去。它不是假想的攻击面：SETTINGS 逼着我们回 ACK，
/// PING 同样，攻击者花一个包换我们一个包，是个放大器。
const unsigned char kEmptySettings[9] = {0x00, 0x00, 0x00, 0x04, 0x00,
                                         0x00, 0x00, 0x00, 0x00};

/// 在一串已发出的帧里找第一个 @p type 类型的帧，返回载荷起点；没有（或被截断）
/// 返回 -1。
long find_frame(const std::string& buf, unsigned char type) {
  size_t i = 0;
  while (i + 9 <= buf.size()) {
    const size_t len = (static_cast<size_t>(static_cast<unsigned char>(buf[i])) << 16) |
                       (static_cast<size_t>(static_cast<unsigned char>(buf[i + 1])) << 8) |
                       static_cast<size_t>(static_cast<unsigned char>(buf[i + 2]));
    if (i + 9 + len > buf.size()) return -1;
    if (static_cast<unsigned char>(buf[i + 3]) == type)
      return static_cast<long>(i + 9);
    i += 9 + len;
  }
  return -1;
}

/// 控制帧洪泛必须在**有限帧数内**被拦住，而收尾发出去的 GOAWAY 要带
/// `ENHANCE_YOUR_CALM`。
///
/// 这个用例钉两件事，缺一不可：
///   1. **有界** —— 200 个帧里必须在中途就被拦下，不能一路收到底；
///   2. **可归因** —— 收尾的 GOAWAY 带的是 `ENHANCE_YOUR_CALM` 而不是
///      `NO_ERROR`。少了第 2 条，一次洪泛在线上长得和"服务端自己优雅退出"
///      一模一样，对端没有任何可归因的信号。
bool test_control_flood_is_capped() {
  uvcpp_h2_session server{true};
  int fatals   = 0;
  int last_err = 0;
  uvcpp_h2_session::callbacks sc;
  sc.on_fatal = [&fatals, &last_err](uvcpp_h2_session&, int code) {
    ++fatals;
    last_err = code;
  };
  if (server.init(sc) != 0) return false;

  // RFC 9113 §3.4 的客户端连接前奏。**少了这 24 字节，nghttp2 对第一个字节就回
  // BAD_CLIENT_MAGIC(-903)** —— 那时 `tripped_at` 会是 1、`fatals` 会是 200，
  // 而"洪泛被拦住了"和"前奏根本没送对"在只看计数器的判据里长得一模一样。
  // 这一条就是这么被抓出来的（详见 test_fatal_error_notifies_once）。
  const char kMagic[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  if (server.recv(kMagic, sizeof(kMagic) - 1) != 0) return false;

  const int kSent = 200;
  int tripped_at  = 0;
  for (int i = 1; i <= kSent; ++i) {
    const int rv = server.recv(reinterpret_cast<const char*>(kEmptySettings),
                               sizeof(kEmptySettings));
    if (rv != 0 && tripped_at == 0) tripped_at = i;
  }

  // 初值 64（一个 64 KiB 的连接窗口对这类帧没有任何约束，只能自己数）。
  // 下面这个循环在微秒级跑完，而补充速率是 32/秒 ⇒ 抖动能带进来的额外帧是个位数，
  // 所以给 ±8 的带宽。**真正的判据是"远小于 200"**，那个数字才是防线。
  if (tripped_at < 60 || tripped_at > 72) {
    std::cerr << "  [diag] flood: tripped_at=" << tripped_at << " (want 60..72)"
              << " fatals=" << fatals << " last_err=" << last_err
              << " goaway_code=" << server.goaway_code() << std::endl;
    return false;
  }
  // 只通知一次：`on_fatal` 的接收方会去发 GOAWAY + 关连接，逐帧重复通知等于
  // 让它在一次洪泛里被叫几百次。
  if (fatals != 1 || last_err != static_cast<int>(H2_ERR_ENHANCE_YOUR_CALM) ||
      server.goaway_code() != H2_ERR_ENHANCE_YOUR_CALM) {
    std::cerr << "  [diag] flood: fatals=" << fatals << " (want 1)"
              << " last_err=" << last_err
              << " goaway_code=" << server.goaway_code() << std::endl;
    return false;
  }

  if (server.submit_goaway(server.goaway_code(), std::string()) != 0) {
    std::cerr << "  [diag] flood: submit_goaway rejected" << std::endl;
    return false;
  }
  std::string out;
  if (server.drain(out) != 0) {
    std::cerr << "  [diag] flood: drain failed" << std::endl;
    return false;
  }
  const long off = find_frame(out, 0x07);  // GOAWAY
  if (off < 0 || off + 8 > static_cast<long>(out.size())) {
    std::cerr << "  [diag] flood: GOAWAY frame not found off=" << off
              << " drained=" << out.size() << " bytes" << std::endl;
    return false;
  }
  // 载荷 = 4 字节 last-stream-id + 4 字节 error code（RFC 9113 §6.8）。
  const uint32_t code =
      (static_cast<uint32_t>(static_cast<unsigned char>(out[off + 4])) << 24) |
      (static_cast<uint32_t>(static_cast<unsigned char>(out[off + 5])) << 16) |
      (static_cast<uint32_t>(static_cast<unsigned char>(out[off + 6])) << 8) |
      static_cast<uint32_t>(static_cast<unsigned char>(out[off + 7]));
  if (code != H2_ERR_ENHANCE_YOUR_CALM) {
    std::cerr << "  [diag] flood: GOAWAY error code=" << code << " (want 0x0b)"
              << std::endl;
    return false;
  }
  return true;
}

/// 致命错误**也只通知一次**（与洪泛同一条理由：接收方要关连接，那是一次性动作）。
///
/// 这一条同时钉住两件事，而它们是同一个根因的两面：
///   1. 不送前奏就是致命错误（BAD_CLIENT_MAGIC），不是"什么都没发生"；
///   2. 会话已经致命之后继续喂字节，`recv` 照旧返回错误，但**不再**重复通知。
/// 第二条曾经是错的（200 次 recv 换来 200 次 `on_fatal`）。它暴露出来的原因很典型：
/// 上面那个洪泛用例漏了前奏，于是它的 `fatals` 计数其实一直在数**致命错误**路径，
/// 而不是它自己声称的洪泛路径 —— 计数对上了，理由却是错的。
bool test_fatal_error_notifies_once() {
  uvcpp_h2_session server{true};
  int fatals   = 0;
  int last_err = 0;
  uvcpp_h2_session::callbacks sc;
  sc.on_fatal = [&fatals, &last_err](uvcpp_h2_session&, int code) {
    ++fatals;
    last_err = code;
  };
  if (server.init(sc) != 0) return false;

  // 故意跳过前奏，直接来一个 SETTINGS。
  const int rv = server.recv(reinterpret_cast<const char*>(kEmptySettings),
                             sizeof(kEmptySettings));
  if (rv >= 0) {
    std::cerr << "  [diag] fatal: 缺前奏的 SETTINGS 竟然被接受了 (rv=" << rv
              << ")" << std::endl;
    return false;
  }
  if (last_err != -903) {  // NGHTTP2_ERR_BAD_CLIENT_MAGIC
    std::cerr << "  [diag] fatal: last_err=" << last_err << " (want -903)"
              << std::endl;
    return false;
  }

  // 再喂 50 次：错误照旧返回，通知**不许**再涨。
  for (int i = 0; i < 50; ++i) {
    const int again = server.recv(reinterpret_cast<const char*>(kEmptySettings),
                                  sizeof(kEmptySettings));
    if (again >= 0) return false;
  }
  if (fatals != 1) {
    std::cerr << "  [diag] fatal: on_fatal 被叫了 " << fatals << " 次 (want 1)"
              << std::endl;
    return false;
  }
  return true;
}

/// 正常量的控制帧**不许**被拦下来。上面那个用例只证明"会拦"，不证明"拦得准" ——
/// 阈值写错成 1 的话它照样绿。
bool test_normal_control_traffic_passes() {
  link& L = make_link();
  if (!L.init()) return false;

  // 一次完整的请求/响应会带来 SETTINGS + SETTINGS ACK + WINDOW_UPDATE 若干。
  // 再手工叠 8 条流，把控制帧数量抬到几十 —— 仍然远低于 64 的突发额度。
  L.reply.status_code = http_status::OK;
  L.reply.body        = mk_buf("ok");
  for (int i = 0; i < 8; ++i) {
    if (L.client.submit_request(make_req(http_method::HTTP_GET, "/ok"), "") <= 0)
      return false;
  }
  L.pump();

  if (L.req_count != 8) return false;
  if (L.resp_count != 8) return false;
  // 一次都没被拦：没有 fatal、响应全都到齐、流表清空。
  if (L.fatal_count != 0) return false;
  if (L.resp_end != true) return false;
  return L.server.stream_count() == 0 && L.client.stream_count() == 0;
}

// =========================================================================
// 头部块的两个方向
//
// 这是**两条独立的防线**，判据和修法都不一样，所以分开钉：
//
//   - **收方向**：`SETTINGS_MAX_HEADER_LIST_SIZE` 在 nghttp2 里是**零强制**的
//     （它只把值存进 `local_settings`，接收路径从不累加、也没跟它比过），
//     所以 `h2_header_budget` 是唯一防线。越界 ⇒ 本层发 RST(0x0b)；
//   - **发方向**：nghttp2 自己的 `max_send_header_block_length`（64 KiB），
//     超了它会**把整帧静默丢掉** —— 见 `uvcpp_h2_session.cpp::header_block_fits`
//     那段注释。本层在 `submit_*` 里先拦一道，换成同步的 `UV_EMSGSIZE`。
//
// 两边在本文件里都一条用例都没有过。
// =========================================================================

/// 带一个超大头部值的请求。值用重复的同一个字符是**刻意的**：HPACK 会把它压到
/// 几十字节上线，于是用例只考"解码后的字节数"（预算算的就是这个），不掺进分帧
/// 与拥塞因素。压缩比越高越说明这条防线的意义 —— 一个几十字节的帧能在内存里
/// 炸出上百 KB，这正是 HPACK bomb。
uvcpp_http_request make_req_with_pad(size_t value_len) {
  uvcpp_http_request r = make_req(http_method::HTTP_GET, "/pad");
  r.set_header("x-pad", std::string(value_len, 'a'));
  return r;
}

/// 服务端收方向超预算必须 RST，且**应用层一个字节都不该看见**。
///
/// 服务端预算压到 1 KiB、负载只用 4 KiB：这样它**远在发方向那个 64 KiB 之内**，
/// 撞到的确实是收方向这条防线。先前拿 96 KiB 去撞，撞到的是发方向 ——
/// 客户端因为 nghttp2 静默丢帧而一个字节都没发出去，服务端根本没收到东西。
/// 用例红在 A 上，断言写的却是 B，这种绿/红都不说明任何事。
///
/// 三条判据缺一不可：
///   - 只钉"被 RST"的话，把预算设成 0、见谁都拒的实现同样绿；
///   - 只钉"应用没看见"的话，帧根本没发出去同样绿；
///   - 只钉"没有致命错误"的话，越预算被当成连接级错误、整条连接被拆掉的实现
///     会把这条一起绿掉 —— 而越预算是一条**流**的事（RFC 9113 §10.5）。
///
/// 这条还顺带钉住一件不相干的事（`rst_count` 那一格同时管着两边）：**客户端
/// 必须在上面的 RST 到达之前就把这条流登记进自己的表**。请求是空 body 的 GET，
/// 而 `submit_request` 原先只在"带 body"那条路上登记 —— 于是 `on_stream_close`
/// 在 `streams.find()` 那一步静默返回，`cbs.on_close` 一声不吭。`uvcpp_http_client`
/// 全靠那个回调给挂起的请求结算，所以那不是"少一个通知"，是**回调永远不来**。
bool test_header_budget_rejects_oversize() {
  link& L = make_link();
  const size_t kServerBudget = 1024;
  if (!L.init(kServerBudget)) return false;

  const int32_t sid = L.client.submit_request(make_req_with_pad(4u * 1024u), "");
  if (sid <= 0) {  // 前置：客户端自己就把请求挡了，那就测不到服务端这条防线
    std::cerr << "  [diag] submit_request 返回 " << sid << "（前置不成立）"
              << std::endl;
    return false;
  }
  L.pump();
  // 前置之二：那批字节**确实过去了**。握手本身不到 200 字节，所以搬过的量真
  // 超过 1 KiB 就只能是那条带头部块的请求 —— 没过去的话，下面"应用没看见"
  // 是必然的，整条用例会在什么都没测到的前提下变绿。
  if (L.pumped <= kServerBudget) {
    std::cerr << "  [diag] 只搬过 " << L.pumped << " 字节（≤" << kServerBudget
              << "），头部块根本没发出去" << std::endl;
    return false;
  }

  if (L.rst_count != 1) {
    std::cerr << "  [diag] rst_count=" << L.rst_count << " (want 1)" << std::endl;
    return false;
  }
  // 0x0b = ENHANCE_YOUR_CALM。用例不 include nghttp2 的头（它没被传播过来），
  // 所以写值不写宏 —— 这个值本身也是线上契约的一部分。
  if (L.last_rst_code != 0x0b) {
    std::cerr << "  [diag] rst code=" << L.last_rst_code << " (want 0x0b)"
              << std::endl;
    return false;
  }
  // 越预算的请求**不许**落到应用层：`on_request` 是"头收全了"。
  if (L.req_count != 0 || L.body_end != 0) {
    std::cerr << "  [diag] 应用层看见了越预算的请求: req=" << L.req_count
              << " body_end=" << L.body_end << std::endl;
    return false;
  }
  // 而且它不该把连接带走。
  return L.fatal_count == 0;
}

/// 合法但接近上限的头部列表必须放行。上面那条只证明"会拦"—— 阈值写成 0 或 1
/// 时它照样绿；这一条把阈值从另一侧钉住，顺带证明预算**不跨流累积**（同一连接
/// 上连发两条 32 KiB 的请求，各自都该过）。
bool test_header_budget_normal_passes() {
  link& L = make_link();
  if (!L.init()) return false;

  L.reply.status_code = http_status::OK;
  L.reply.body        = mk_buf("ok");

  for (int i = 0; i < 2; ++i) {
    // 32 KiB：一半预算，仍然是个"大"头部列表。
    if (L.client.submit_request(make_req_with_pad(32u * 1024u), "") <= 0)
      return false;
  }
  L.pump();

  if (L.rst_count != 0) {
    std::cerr << "  [diag] 合法头部被 RST 了 " << L.rst_count << " 次" << std::endl;
    return false;
  }
  if (L.req_count != 2 || L.resp_count != 2) {
    std::cerr << "  [diag] req=" << L.req_count << " resp=" << L.resp_count
              << " (want 2/2)" << std::endl;
    return false;
  }
  return L.fatal_count == 0;
}

/// 发方向超上限必须在 `submit_request` 里**同步**退掉。
///
/// 这条钉的是 nghttp2 的静默丢帧：超限的 HEADERS 被丢掉整帧、按 REFUSED_STREAM
/// 关掉流、然后既然继续跑 —— 既不通知我们（没注册 `on_frame_not_send`），也不发
/// RST_STREAM（对端连这条流都不知道）。而这一切发生在**发**的时候，那时
/// `submit_request` 早就把一个正的流号还给调用方了。改之前这里拿到的是 1，
/// 然后两边永远等下去，线上一个字节都没有。
bool test_oversize_submit_is_refused() {
  link& L = make_link();
  if (!L.init()) return false;

  const int32_t sid = L.client.submit_request(make_req_with_pad(96u * 1024u), "");
  if (sid != UV_EMSGSIZE) {
    std::cerr << "  [diag] 超上限的 submit_request 返回 " << sid << "（want "
              << static_cast<int>(UV_EMSGSIZE) << " = UV_EMSGSIZE）" << std::endl;
    return false;
  }
  const size_t before = L.pumped;
  L.pump();

  // 失败必须是**干净**的：没有半开的流留下来……
  if (L.client.stream_count() != 0) {
    std::cerr << "  [diag] 客户端留下了 " << L.client.stream_count() << " 条流"
              << std::endl;
    return false;
  }
  // ……那 96 KiB 也一个字节都没上线（握手不到 200 字节）……
  if (L.pumped - before > 1024) {
    std::cerr << "  [diag] 被拒之后还排出去 " << (L.pumped - before)
              << " 字节" << std::endl;
    return false;
  }
  // ……对端自然什么都没看见。
  if (L.req_count != 0 || L.resp_count != 0 || L.rst_count != 0) {
    std::cerr << "  [diag] 对端看见了东西: req=" << L.req_count
              << " resp=" << L.resp_count << " rst=" << L.rst_count << std::endl;
    return false;
  }
  if (L.drain_err != 0) {
    std::cerr << "  [diag] drain 报错 " << L.drain_err << std::endl;
    return false;
  }
  return L.fatal_count == 0;
}

/// 服务端的响应头同样不能静默消失，而且**失败之后那条流必须还能用**。
///
/// 后半句是这条用例真正的价值：`submit_response`/`submit_headers` 原本一进门就
/// 把流置成 `SENT`/`HEADERS_SENT`。若把越界检查放在置位之后，失败会留下一条
/// "说过要发、其实一个字节没发"的流 —— 重试被 `UV_EALREADY` 挡住，对端永远等
/// 一条不会来的响应。所以这里退掉之后**换一份正常头部重试，必须成功**。
bool test_oversize_response_is_refused() {
  link& L = make_link();
  if (!L.init()) return false;

  L.reply.status_code = http_status::OK;
  L.reply.body        = mk_buf("ok");
  L.reply.set_header("x-pad", std::string(96u * 1024u, 'a'));

  if (L.client.submit_request(make_req(http_method::HTTP_GET, "/big"), "") <= 0)
    return false;
  L.pump();
  if (L.last_submit_rc != UV_EMSGSIZE) {
    std::cerr << "  [diag] 服务端的超限响应返回 " << L.last_submit_rc
              << "（want " << static_cast<int>(UV_EMSGSIZE) << "）" << std::endl;
    return false;
  }

  // 退得干净 ⇒ 同一块头部换掉之后，同一条流上重试必须成功。
  L.reply.headers.clear();
  const int rc = L.server.submit_response(L.last_stream, L.reply);
  if (rc != 0) {
    std::cerr << "  [diag] 换一份正常头部重试被拒: " << rc << std::endl;
    return false;
  }
  L.pump();
  if (L.resp_count != 1 || L.resp_status != 200 || L.resp_body != "ok") {
    std::cerr << "  [diag] resp=" << L.resp_count << " status=" << L.resp_status
              << " body=" << L.resp_body << std::endl;
    return false;
  }
  return L.fatal_count == 0 && L.rst_count == 0;
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
    {"control_flood_is_capped", test_control_flood_is_capped},
    {"fatal_error_notifies_once", test_fatal_error_notifies_once},
    {"normal_control_traffic_passes", test_normal_control_traffic_passes},
    {"header_budget_rejects_oversize", test_header_budget_rejects_oversize},
    {"header_budget_normal_passes", test_header_budget_normal_passes},
    {"oversize_submit_is_refused", test_oversize_submit_is_refused},
    {"oversize_response_is_refused", test_oversize_response_is_refused},
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
