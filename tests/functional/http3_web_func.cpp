/**
 * @file tests/functional/http3_web_func.cpp
 * @brief P5 的判据：`uvcpp_http_server` 接上 h3 之后，**同一个服务端对象**同时
 *        答 h3（QUIC/UDP）与 h1（TCP），而且两条腿走的是**同一张路由表**。
 * @author zhuweiye
 * @version 1.4.1
 *
 * 与 `http3_request_func.cpp` 的分工：那一条钉的是"h3 本身对不对"（nghttp3 →
 * h3 驱动层 → QUIC）与"客户端接线对不对"；本文件钉的是**服务端**这一侧的接线
 * —— `listen_quic()` / `on_quic_connection()` / `dispatch_h3_request()` /
 * `send_h3_response()` 这一串把 web 层的路由与响应语义搬上 QUIC 的活。
 *
 * **两条腿同时开着**是本文件的骨架，不是顺带：
 *
 * 1. 一个 `uvcpp_http_server` 对象上先 `listen_quic(0, "127.0.0.1")`、再
 *    `bind` + `listen(128)` 开 TCP。两条腿各有各的端口，凭证也是各装各的
 *    （QUIC 那份必须有；TCP 这份这里**故意不装** ⇒ 明文 h1）。
 * 2. `/transport` 这条路由**两条腿都打**：处理函数把 `req.version` 原样写回
 *    body。于是"同一个处理函数在不同传输上看到的是不同版本"这件事被钉住 ——
 *    这正是"处理函数不需要知道请求走的是哪条传输"那条设计的可见后果。
 * 3. h1 那两条往返（h3 连接**活着时**一条、h3 连接**销毁之后**一条）是 h1 不
 *    受影响的护栏：接进来一个 h3 端点不让原先那条路少答一个字。
 *
 * 四条断言各自钉一个接缝：
 *
 * - `negotiated_alpn() == "h3"`：ALPN 是从 **QUIC 自己的** TLS 握手里读的。
 *   空串（那一格没人写过）与 `"h3"` 在功能上是两回事；
 * - 明文 TCP 那条连接的 `negotiated_alpn()` 是**空串**：`h3` 绝不该出现在 TCP
 *   的 ALPN 名单里（对端选了我们就会把 h3 的二进制流喂给 llhttp，表现是
 *   "连上了、写得出去、响应永远不来、哪里都不报错"）；
 * - `req.version` 在服务端分别是 HVER_30 / HVER_11，且 h3 那条腿上处理函数
 *   拿到的 `client` 是 **nullptr**（那条腿上没有 `uvcpp_tcp_client` 这个东西）；
 * - 404 走的兜底那条路上，交付给客户端的**版本仍是 HTTP/3** —— 那段代码里
 *   `not_found()` 造出来的是 h1 形状，版本与流号都得自己钉回去。
 *
 * **单线程**：三条循环（服务端一条、两个客户端各一条）都由本用例的主线程泵，
 * 所以下面那些记录用的容器不需要原子（`web_ssl_h2_server_func.cpp` 那边要，
 * 是因为服务端跑在另一个线程上）。
 */
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_HTTP3_ENABLE

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <quic/uvcpp_quic_common.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_compress.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

#include "loop_drain.h"
#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_checks   = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
  ++g_checks;
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "：期望 " << want << "，实得 " << got
              << std::endl;
    ++g_failures;
  }
}

void check_eq_s(const std::string& got, const std::string& want,
                const std::string& what) {
  ++g_checks;
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "：期望 \"" << want << "\"，实得 \""
              << got << "\"" << std::endl;
    ++g_failures;
  }
}

const int kDeadlineMs = 5000;

/// `/h3` 的固定响应体。**长度是个常量这件事是故意的**：HEAD 那条断言要拿它
/// 对 `content-length`（HEAD 不发体，但要报 GET 会有多长）。
const char kFixedBody[] = "hello-h3-web";

/// `/big` 的字节数。两道门都得过：`> compress_min_body_`（默认 1000+24），
/// 以及"压完明显变小"。内容是一串等值字节，所以解回来能逐字节比 —— 只断言
/// "收到过字节"或"有个 content-encoding 头"都太松（把一个坏掉的压缩器写成
/// 恒等映射也能过）。
const size_t kBigBytes = 4096;

/**
 * @brief 泵**三条**循环，直到 `pred` 为真或超时。
 *
 * `wait_util.h` 有单条与两条的版本，没有三条的：本用例里服务端一条、两个
 * 客户端各一条。第三条塞进谓词里泵 —— 谓词的每轮都会被调，而它只做一次
 * `UV_RUN_NOWAIT`，与 `wait_until_pair` 内部那两次同性质。
 */
bool wait_all(uvcpp_loop* a, uvcpp_loop* b, uvcpp_loop* c,
              std::function<bool()> pred, int deadline_ms) {
  return uvcpp_test::wait_until_pair(
      a, b,
      [&]() {
        if (c != nullptr) c->run(UV_RUN_NOWAIT);
        return pred();
      },
      deadline_ms);
}

/// 什么都不等，纯粹把三条循环各拨几毫秒。
void pump_all(uvcpp_loop* a, uvcpp_loop* b, uvcpp_loop* c, int ms) {
  wait_all(a, b, c, [] { return false; }, ms);
}

/**
 * @brief 服务端处理函数看到的**每一个**请求：传输（由 `client` 是 nullptr 分辨）
 *        与请求版本。
 *
 * 两条腿共用一个处理函数，所以这两条记录天然是"按到达序"的一串 —— 断言按
 * 下标对号入座，也就是把**次序**一起钉住了（h3 那三条必须落在 h1 第一条之后、
 * 第二条之前，因为 h1 那两条分别在 h3 连接活着与销毁之后发）。
 */
struct seen_request {
  bool                    over_h3 = false;  ///< `client == nullptr`
  int                     version = -1;
  std::string             path;
};

void test_web_server_h3_and_h1_over_one_object() {
  // -------------------------------------------------------------------
  // 服务端：**一个对象，两条腿**
  // -------------------------------------------------------------------
  uvcpp_http_server server;
  uvcpp_loop*       sloop = server.get_tcp_server()->get_loop();
  uvcpp_test::loop_drain drain_sloop(sloop);

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    std::cerr << "  [FAIL] 生成自签证书失败" << std::endl;
    ++g_failures;
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  std::vector<seen_request> seen;

  // `/transport` —— 两条腿都打这条。处理函数把 `req.version` 写进 body：
  // "同一个处理函数，两条传输"这句话的可观测形式就是它。
  server.get("/transport",
             [&seen](uvcpp_http_request& req, uvcpp_http_response& resp,
                     uvcpp_tcp_client* c) {
               seen_request s;
               s.over_h3 = (c == nullptr);
               s.version = static_cast<int>(req.version);
               s.path    = req.url;
               seen.push_back(s);
               const std::string body = uvcpp_http_version_str(req.version);
               resp = uvcpp_http_response::ok(body.c_str(), body.size());
             });

  // `post` 那条路由钉两件事：`:method` 映射（认不出来就是 400，而不是走到
  // 这条）与**请求体**真的搬到了 web 层的请求对象里（`h3_to_web_request` 里
  // 那次 `uvcpp_buf` 拷贝）。
  server.post("/echo",
              [&seen](uvcpp_http_request& req, uvcpp_http_response& resp,
                      uvcpp_tcp_client* c) {
                seen_request s;
                s.over_h3 = (c == nullptr);
                s.version = static_cast<int>(req.version);
                s.path    = req.url;
                seen.push_back(s);
                const std::string in =
                    req.body.size() > 0
                        ? std::string(req.body.get_const_data(), req.body.size())
                        : std::string();
                resp = uvcpp_http_response::ok(in.c_str(), in.size());
              });

  // HEAD 要**单独注册**：路由是按"方法 + 路径"精确匹配的（`find_handler`），
  // GET 那条不会接住 HEAD。与 GET 同一个处理函数 —— h3 那条腿上 `omit_body`
  // 由 `send_h3_response()` 按 `req.method` 判，不在这里分叉。
  auto fixed_handler = [](uvcpp_http_request&, uvcpp_http_response& resp,
                          uvcpp_tcp_client*) {
    resp = uvcpp_http_response::ok(kFixedBody, sizeof(kFixedBody) - 1);
  };
  server.get("/h3", fixed_handler);
  server.head("/h3", fixed_handler);

  // `/big` —— 响应压缩那条接缝。**不记进 `seen`**：`seen` 是按到达序对号入座的，
  // 它收的是"服务端看到了什么"，而这条路径的判据是响应头与响应字节。
  //
  // 压缩这一段（`apply_compression_for`）h1/h2/h3 上共用**同一份实现**：本批把它
  // 从 `apply_compression(conn_ctx&, resp)` 里拆出来，因为 h3 这条腿读不到
  // `conn_ctx`（那是 h1 每请求热路上的结构体），只能按 accept-encoding 字符串调。
  // 所以这一条同时钉住"拆得对"。
  server.get("/big", [](uvcpp_http_request&, uvcpp_http_response& resp,
                        uvcpp_tcp_client*) {
    const std::string big(kBigBytes, 'x');
    resp = uvcpp_http_response::ok(big.c_str(), big.size());  // text/plain
  });

  // 凭证**分开装**：QUIC 那条腿必须有（QUIC 没有明文模式），TCP 这条这里
  // 故意不装 ⇒ 明文 h1。两份不是一回事，这一句就是证据。
  server.set_quic_ssl_context(&server_ctx);

  check_eq_i(server.listen_quic(0, "127.0.0.1"), 0,
             "h3: listen_quic(0) 返回 0");
  const int qport = server.quic_listen_port();
  check(qport > 0, "h3: quic_listen_port() 报出内核挑的那个端口");

  check_eq_i(server.bind("127.0.0.1", 0), 0, "h1: bind(127.0.0.1, 0)");
  sockaddr_in name;
  int         namelen = sizeof(name);
  server.get_tcp_server()->get_tcp()->getsockname(
      reinterpret_cast<sockaddr*>(&name), &namelen);
  const int tport = ntohs(name.sin_port);
  check_eq_i(server.listen(128), 0, "h1: listen(128) 返回 0");

  // 两个端口必须是**两个**：h3 走 UDP、h1 走 TCP，各绑各的。内核给
  // `listen_quic(0)` 挑的号与 TCP 那个号撞在一起的概率很低，但真撞上就说明
  // 有一边绑错了地址族 —— 与其偶发，不如钉住。
  check(qport != tport, "两条腿各绑各的端口（UDP 与 TCP 不是同一个号）");

  // -------------------------------------------------------------------
  // h1 客户端：全程活着，用来证明 h3 那条腿的出现与消失都不影响它
  // -------------------------------------------------------------------
  uvcpp_http_client h1c;
  uvcpp_loop*       h1loop = h1c.get_tcp_client()->get_loop();

  int h1_connect_status = 12345;
  check_eq_i(h1c.connect("127.0.0.1", tport,
                         [&](int st) { h1_connect_status = st; }),
             0, "h1: connect() 返回 0");
  check(wait_all(sloop, h1loop, nullptr,
                 [&] { return h1_connect_status != 12345; }, kDeadlineMs),
        "h1: 连接在 deadline 内建起来");
  check_eq_i(h1_connect_status, 0, "h1: connect 的 cb 收到 0");

  // **明文连接没有 ALPN。** 这条同时钉住两件事：`h3` 没被塞进 TCP 那份名单
  // （塞了这里就会有值，而对端选了它 llhttp 会收到一堆二进制），以及本用例
  // 用的确实是"没装 TLS 凭证"的 TCP 那条腿。
  check_eq_s(h1c.negotiated_alpn(), "",
             "h1: 明文 TCP 连接没有 ALPN（`h3` 不该出现在 TCP 上）");

  // 第 1 条 h1 往返（**h3 连接还活着**）
  int         h1a_calls = 0, h1a_err = 12345, h1a_ver = -1;
  std::string h1a_body;
  check_eq_i(h1c.send(uvcpp_http_request::make_get("/transport"),
                      [&](const uvcpp_http_response& r, int e) {
                        ++h1a_calls;
                        h1a_err  = e;
                        h1a_ver  = static_cast<int>(r.version);
                        h1a_body = r.body.to_string();
                      }),
             0, "h1: 第 1 条 send() 返回 0");
  check(wait_all(sloop, h1loop, nullptr, [&] { return h1a_calls > 0; },
                 kDeadlineMs),
        "h1: 第 1 条响应到了");
  check_eq_i(h1a_err, 0, "h1: 第 1 条干净收场");
  check_eq_s(h1a_body, "HTTP/1.1", "h1: 处理函数看到的版本是 HTTP/1.1");
  check_eq_i(h1a_ver, static_cast<int>(uvcpp_http_version::HVER_11),
             "h1: 交付给客户端的响应版本是 HTTP/1.1");

  // -------------------------------------------------------------------
  // h3 客户端：同一个服务端对象的**另一条**腿
  // -------------------------------------------------------------------
  {
    uvcpp_http_client h3c;
    uvcpp_loop*       h3loop = h3c.get_tcp_client()->get_loop();

    h3c.set_http3_enabled(true);
    h3c.set_ssl_context(&client_ctx);

    int h3_connect_status = 12345;
    check_eq_i(h3c.connect("127.0.0.1", qport,
                           [&](int st) { h3_connect_status = st; }),
               0, "h3: connect() 返回 0");
    check(wait_all(sloop, h3loop, h1loop,
                   [&] { return h3_connect_status != 12345; }, kDeadlineMs),
          "h3: 握手在 deadline 内完成");
    check_eq_i(h3_connect_status, 0, "h3: connect 的 cb 收到 0");
    check_eq_s(h3c.negotiated_alpn(), "h3",
               "h3: ALPN 协商出 h3（读的是 QUIC 自己的握手）");

    // ---- 第 1 条：`/transport`，同一个处理函数，另一条传输 -------------
    int         t_calls = 0, t_err = 12345, t_status = 0, t_ver = -1;
    std::string t_body, t_date;
    check_eq_i(h3c.send(uvcpp_http_request::make_get("/transport"),
                        [&](const uvcpp_http_response& r, int e) {
                          ++t_calls;
                          t_err    = e;
                          t_status = static_cast<int>(r.status_code);
                          t_ver    = static_cast<int>(r.version);
                          t_body   = r.body.to_string();
                          t_date   = r.get_header("date");
                        }),
               0, "h3: /transport 的 send() 返回 0");
    check(wait_all(sloop, h3loop, h1loop, [&] { return t_calls > 0; },
                   kDeadlineMs),
          "h3: /transport 的响应到了");
    check_eq_i(t_err, 0, "h3: /transport 干净收场");
    check_eq_i(t_status, 200, "h3: /transport 的 :status");
    check_eq_s(t_body, "HTTP/3",
               "h3: 同一个处理函数看到的版本是 HTTP/3");
    check_eq_i(t_ver, static_cast<int>(uvcpp_http_version::HVER_30),
               "h3: 交付给客户端的响应版本是 HTTP/3");
    // `date` 是 `send_h3_response()` 里 `http_ensure_date()` 那一句的产物，处理
    // 函数自己从没设过它 —— h3 是一条**绕过** `send_response()` 的独立应答路径，
    // 少了那一句就是"三条传输上三种响应形状"。
    check(!t_date.empty(),
          "h3: 响应带 date（send_h3_response() 里那次收尾真的跑了）");

    // ---- 第 2 条：没有路由 ⇒ 兜底 404，版本仍须是 HTTP/3 --------------
    int t404_calls = 0, t404_status = 0, t404_ver = -1;
    check_eq_i(h3c.send(uvcpp_http_request::make_get("/missing"),
                        [&](const uvcpp_http_response& r, int) {
                          ++t404_calls;
                          t404_status = static_cast<int>(r.status_code);
                          t404_ver    = static_cast<int>(r.version);
                        }),
               0, "h3: /missing 的 send() 返回 0");
    check(wait_all(sloop, h3loop, h1loop, [&] { return t404_calls > 0; },
                   kDeadlineMs),
          "h3: /missing 的响应到了");
    check_eq_i(t404_status, 404, "h3: 没注册的路径走兜底处理函数（404）");
    check_eq_i(t404_ver, static_cast<int>(uvcpp_http_version::HVER_30),
               "h3: 兜底那条路上版本也没丢（not_found() 造出来的是 h1 形状）");

    // ---- 第 3 条：POST，钉 `:method` 与请求体 --------------------------
    const char* payload  = "echo-over-h3";
    int         e_calls  = 0, e_status = 0;
    std::string e_body;
    check_eq_i(h3c.send(uvcpp_http_request::make_post(
                            "/echo", payload, std::strlen(payload)),
                        [&](const uvcpp_http_response& r, int) {
                          ++e_calls;
                          e_status = static_cast<int>(r.status_code);
                          e_body   = r.body.to_string();
                        }),
               0, "h3: /echo 的 send() 返回 0");
    check(wait_all(sloop, h3loop, h1loop, [&] { return e_calls > 0; },
                   kDeadlineMs),
          "h3: /echo 的响应到了");
    check_eq_i(e_status, 200,
               "h3: POST 匹配上了 post() 注册的路由（`:method` 映射没歪）");
    check_eq_s(e_body, payload, "h3: 请求体逐字节搬到了服务端的请求对象里");

    // ---- 第 4 条：HEAD ⇒ 只发头块，但长度按 GET 报 ---------------------
    int hd_calls = 0, hd_status = 0, hd_ver = -1;
    std::string hd_body, hd_cl;
    check_eq_i(h3c.send(uvcpp_http_request::make_head("/h3"),
                        [&](const uvcpp_http_response& r, int) {
                          ++hd_calls;
                          hd_status = static_cast<int>(r.status_code);
                          hd_ver    = static_cast<int>(r.version);
                          hd_body   = r.body.to_string();
                          hd_cl     = r.get_header("content-length");
                        }),
               0, "h3: HEAD /h3 的 send() 返回 0");
    check(wait_all(sloop, h3loop, h1loop, [&] { return hd_calls > 0; },
                   kDeadlineMs),
          "h3: HEAD /h3 的响应到了（头块自带 END_STREAM，不等体）");
    check_eq_i(hd_status, 200, "h3: HEAD 的 :status");
    check_eq_i(hd_ver, static_cast<int>(uvcpp_http_version::HVER_30),
               "h3: HEAD 的版本");
    check_eq_s(hd_body, "", "h3: HEAD 一个字节的 body 都不发");
    // 体不发，但长度按 GET 会有多长报 —— HEAD 那条分支里那一段。数字与
    // `kFixedBody` 绑在一起，改了一个另一个跟着变。
    check_eq_s(hd_cl, std::to_string(sizeof(kFixedBody) - 1),
               "h3: HEAD 报的 content-length 是 GET 会有的长度");
    // 再说一句 `hd_body` 那句**不是**什么：`omit_body` 传错（= 把体也发出去）
    // 时它**仍然是空的** —— 实测（反空转表 P5-9）：对端的 nghttp3 按 RFC 9114
    // §4.3 判了"HEAD 的响应不能有体"，那些字节点都没交上来，整条连接随错误
    // 收场。所以那条路线的可观测后果在**下面这一条**上：HEAD 之后同一连接还得
    // 能继续用。这条与 GET /h3 走的是同一个处理函数，不进 `seen`。
    int hd2_calls = 0, hd2_status = 0;
    check_eq_i(h3c.send(uvcpp_http_request::make_get("/h3"),
                        [&](const uvcpp_http_response& r, int) {
                          ++hd2_calls;
                          hd2_status = static_cast<int>(r.status_code);
                        }),
               0, "h3: HEAD 之后同一连接上的 send() 仍然被放行");
    check(wait_all(sloop, h3loop, h1loop, [&] { return hd2_calls > 0; },
                   kDeadlineMs),
          "h3: HEAD 之后同一连接照样答得出下一条");
    check_eq_i(hd2_status, 200, "h3: HEAD 之后下一条的 :status");
#if UVCPP_ZLIB_ENABLE
    // ---- 第 5 条：响应压缩（协议无关的那一段，h3 上照样得生效）---------
    const std::string big_plain(kBigBytes, 'x');
    uvcpp_http_request big_req = uvcpp_http_request::make_get("/big");
    big_req.set_header("accept-encoding", "gzip");
    int         b_calls = 0, b_status = 0;
    size_t      b_len = 0;
    bool        b_round = false;
    std::string b_ce, b_vary;
    check_eq_i(h3c.send(big_req, [&](const uvcpp_http_response& r, int) {
                 ++b_calls;
                 b_status = static_cast<int>(r.status_code);
                 b_ce     = r.get_header("content-encoding");
                 b_vary   = r.get_header("vary");
                 b_len    = r.body.size();
                 if (r.body.size() > 0) {
                   const http_compress_result d = http_compress::decompress(
                       r.body.get_const_data(), r.body.size());
                   b_round = d.success && d.data.size() == big_plain.size() &&
                             std::memcmp(d.data.get_const_data(),
                                         big_plain.data(),
                                         big_plain.size()) == 0;
                 }
               }),
               0, "h3: /big 的 send() 返回 0");
    check(wait_all(sloop, h3loop, h1loop, [&] { return b_calls > 0; },
                   kDeadlineMs),
          "h3: /big 的响应到了");
    check_eq_i(b_status, 200, "h3: /big 的 :status");
    check_eq_s(b_ce, "gzip",
               "h3: accept-encoding: gzip + 4KB 文本 ⇒ 响应真的编码了");
    check_eq_s(b_vary, "accept-encoding", "h3: 压缩那一段顺手挂上 vary");
    check(b_len < kBigBytes, "h3: 实发字节数小于原文（不是只挂了个头）");
    check(b_round,
          "h3: 压出来的字节能逐字节解回原文（三条腿共用同一段实现）");
#endif  // UVCPP_ZLIB_ENABLE
  }
  // h3 客户端在此销毁 ⇒ QUIC 连接被收掉。下面那条 h1 往返证明**服务端**没被
  // 这件事带走：两个端点共用一条循环，h3 那条腿的收尾不能把另一条卡住。
  pump_all(sloop, h1loop, nullptr, 60);

  // -------------------------------------------------------------------
  // h1 第 2 条往返：h3 连接**已经销毁**之后
  // -------------------------------------------------------------------
  int         h1b_calls = 0, h1b_status = 0;
  std::string h1b_body;
  check_eq_i(h1c.send(uvcpp_http_request::make_get("/transport"),
                      [&](const uvcpp_http_response& r, int) {
                        ++h1b_calls;
                        h1b_status = static_cast<int>(r.status_code);
                        h1b_body   = r.body.to_string();
                      }),
             0, "h1: 第 2 条 send() 返回 0");
  check(wait_all(sloop, h1loop, nullptr, [&] { return h1b_calls > 0; },
                 kDeadlineMs),
        "h1: h3 那条腿收掉之后，h1 照旧答（同一个服务端对象）");
  check_eq_i(h1b_status, 200, "h1: 第 2 条的 :status");
  check_eq_s(h1b_body, "HTTP/1.1", "h1: 第 2 条的 body");

#if UVCPP_ZLIB_ENABLE
  // h1 这条腿上**同一份压缩实现**也要真的在。本批把它从
  // `apply_compression(conn_ctx&, resp)` 里拆成了 `apply_compression_for(串, resp)`
  // —— h1 那条路只是转一下。这一条就是那次拆分的护栏：只验 h3 一侧的话，
  // 一个"转发时把 `ctx.accept_encoding` 传丢"的拆分照样能让整条用例全绿。
  {
    const std::string big_plain(kBigBytes, 'x');
    uvcpp_http_request big1 = uvcpp_http_request::make_get("/big");
    big1.set_header("accept-encoding", "gzip");
    int         c_calls = 0;
    size_t      c_len = 0;
    bool        c_round = false;
    std::string c_ce;
    check_eq_i(h1c.send(big1, [&](const uvcpp_http_response& r, int) {
                 ++c_calls;
                 c_ce  = r.get_header("content-encoding");
                 c_len = r.body.size();
                 if (r.body.size() > 0) {
                   const http_compress_result d = http_compress::decompress(
                       r.body.get_const_data(), r.body.size());
                   c_round = d.success && d.data.size() == big_plain.size() &&
                             std::memcmp(d.data.get_const_data(),
                                         big_plain.data(),
                                         big_plain.size()) == 0;
                 }
               }),
               0, "h1: /big 的 send() 返回 0");
    check(wait_all(sloop, h1loop, nullptr, [&] { return c_calls > 0; },
                   kDeadlineMs),
          "h1: /big 的响应到了");
    check_eq_s(c_ce, "gzip",
               "h1: 拆分之后这条腿上的压缩还在（转发没把 accept-encoding 传丢）");
    check(c_len < kBigBytes, "h1: 压缩后的字节数小于原文");
    check(c_round, "h1: 压出来的字节能逐字节解回原文");
  }
#endif  // UVCPP_ZLIB_ENABLE

  // -------------------------------------------------------------------
  // 服务端看到了什么：次序 + 传输身份 + 版本，一起对号入座
  // -------------------------------------------------------------------
  // 四条：h1 两条（分别在 h3 活着与销毁之后）+ h3 两条。HEAD 那条记不进来
  // ——它走的是 `/h3` 那个处理函数，与另外两条不是一个（`seen` 只收
  // `/transport` 与 `/echo` 这两个会记录的）。HEAD 的判据在客户端那侧
  // （状态 200、版本、空体），上面已经钉过了。
  // 次序即"到达序"：h1 第 1 条在最前、h1 第 2 条在最后，两条 h3 夹在中间。
  check_eq_i(static_cast<int>(seen.size()), 4,
             "服务端一共跑了 4 个会记录的处理函数（h1 两条 + h3 两条）");
  if (seen.size() == 4) {
    check_eq_i(seen[0].over_h3 ? 1 : 0, 0, "第 1 条是 h1（h3 连接还活着）");
    check_eq_i(seen[1].over_h3 ? 1 : 0, 1, "第 2 条是 h3（/transport）");
    check_eq_i(seen[2].over_h3 ? 1 : 0, 1, "第 3 条是 h3（POST /echo）");
    check_eq_i(seen[3].over_h3 ? 1 : 0, 0, "第 4 条是 h1（h3 已经销毁）");

    // h3 那条腿给处理函数的是 nullptr（那条传输上没有 `uvcpp_tcp_client`），
    // h1 那条腿给的是真句柄 —— 这是"处理函数能分辨自己在哪条腿上"的唯一依据。
    check_eq_i(seen[1].version,
               static_cast<int>(uvcpp_http_version::HVER_30),
               "h3: 处理函数看到的请求版本是 HVER_30");
    check_eq_i(seen[3].version,
               static_cast<int>(uvcpp_http_version::HVER_11),
               "h1: 处理函数看到的请求版本是 HVER_11");
    check_eq_s(seen[2].path, "/echo", "h3: 处理函数看到的 req.url");
  }

  // 端口那边：两条腿各在听。TCP 那份由 `get_tcp_server()` 报，
  // QUIC 这份只有 `quic_listen_port()` 问得到。
  check(server.has_status(HTTP_SERVER_LISTENING),
        "两条腿都起来之后服务端状态是 LISTENING");
}

/**
 * @brief h3 上**明确不支持**的三样：`set_stream_claim()` 装的认领钩子、
 *        `post_stream()` 注册的流式路由、以及处理函数把 `resp.deferred` 置真。
 *
 * 为什么值得单独一条用例：这三样在本批里都是"**没有入口**"，而"没有入口"最糟的
 * 实现方式是**静默** —— 请求进来、处理函数没跑（或跑了但响应发不出去）、客户端
 * 一直等、服务端一处错误都不打。本用例钉的就是"撞上时是一条 500，不是一次静默
 * 超时"。
 *
 * 四条：三条各自钉一个前提；最后一条是**对照** —— 把三样都撤掉之后，同一条 h3
 * 连接、同一个路径必须答 200。没有这条对照，"一律回 500"也能让上面三条全绿。
 */
void test_unsupported_paths_refuse_explicitly() {
  uvcpp_http_server server;
  uvcpp_loop*       sloop = server.get_tcp_server()->get_loop();
  uvcpp_test::loop_drain drain_sloop(sloop);

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    std::cerr << "  [FAIL] 生成自签证书失败" << std::endl;
    ++g_failures;
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  int ok_calls    = 0;
  int defer_seen  = 0;
  int claim_seen  = 0;
  int stream_seen = 0;

  server.get("/ok", [&ok_calls](uvcpp_http_request&, uvcpp_http_response& resp,
                                uvcpp_tcp_client*) {
    ++ok_calls;
    resp = uvcpp_http_response::ok("ok", 2);
  });
  // 推迟应答要求处理函数手里有"稍后送出去"的入口（h1/h2 上是那个 `client` 与
  // `send_response()`），h3 上没有 —— 它要的是 QUIC 连接句柄。
  server.get("/defer", [&defer_seen](uvcpp_http_request&,
                                     uvcpp_http_response& resp,
                                     uvcpp_tcp_client*) {
    ++defer_seen;
    resp          = uvcpp_http_response::ok("not-now", 7);
    resp.deferred = true;
  });
  server.post_stream("/stream",
                     [&stream_seen](http_stream_event, const char*, size_t,
                                    uvcpp_http_request&, uvcpp_tcp_client*) {
                       ++stream_seen;
                     });

  server.set_quic_ssl_context(&server_ctx);
  check_eq_i(server.listen_quic(0, "127.0.0.1"), 0,
             "refuse: listen_quic(0) 返回 0");
  const int qport = server.quic_listen_port();

  uvcpp_http_client h3c;
  uvcpp_loop*       h3loop = h3c.get_tcp_client()->get_loop();
  h3c.set_http3_enabled(true);
  h3c.set_ssl_context(&client_ctx);
  int st = 12345;
  check_eq_i(h3c.connect("127.0.0.1", qport, [&st](int s) { st = s; }), 0,
             "refuse: connect() 返回 0");
  check(wait_all(sloop, h3loop, nullptr, [&] { return st != 12345; },
                 kDeadlineMs),
        "refuse: 握手在 deadline 内完成");
  check_eq_i(st, 0, "refuse: connect 的 cb 收到 0");

  // 一次往返的帮手。**超时也算失败**：这三条要证明的正是"不是静默超时"，
  // 所以谓词等的是"回调来过"，不是"等够了"。
  struct probe {
    int calls  = 0;
    int status = 0;
  };
  probe p;
  auto roundtrip = [&](const uvcpp_http_request& req) -> bool {
    p.calls  = 0;
    p.status = 0;
    if (h3c.send(req, [&p](const uvcpp_http_response& r, int) {
          ++p.calls;
          p.status = static_cast<int>(r.status_code);
        }) != 0) {
      return false;
    }
    return wait_all(sloop, h3loop, nullptr, [&] { return p.calls > 0; },
                    kDeadlineMs);
  };

  // ---- A) 装着 `set_stream_claim()` 的时候 ----------------------------
  server.set_stream_claim(
      [&claim_seen](uvcpp_http_request&,
                    uvcpp_tcp_client*) -> http_stream_handler {
        ++claim_seen;
        return http_stream_handler();
      });
  check(server.has_stream_claim(), "refuse: 认领钩子装上了");
  check(roundtrip(uvcpp_http_request::make_get("/ok")),
        "refuse: 装着认领钩子时 /ok 也收到了应答（500，不是静默超时）");
  check_eq_i(p.status, 500,
             "refuse: set_stream_claim() 装着时 h3 回 500");
  check_eq_i(ok_calls, 0,
             "refuse: 那一次处理函数根本没跑（拒绝写在分派之前）");
  check_eq_i(claim_seen, 0,
             "refuse: 认领钩子本身也没被问过（它只在 TCP 那条腿上生效）");
  server.clear_stream_claim();

  // ---- B) 撞上 `post_stream()` 注册的流式路由 -------------------------
  check(roundtrip(uvcpp_http_request::make_post("/stream", "x", 1)),
        "refuse: 撞上流式路由的 POST 也收到了应答");
  check_eq_i(p.status, 500, "refuse: post_stream() 那条路由在 h3 上回 500");
  check_eq_i(stream_seen, 0, "refuse: 流式处理函数一次都没被调");

  // ---- C) 处理函数把 `resp.deferred` 置真 ------------------------------
  check(roundtrip(uvcpp_http_request::make_get("/defer")),
        "refuse: deferred 那条也收到了应答");
  check_eq_i(p.status, 500, "refuse: resp.deferred 在 h3 上回 500");
  check_eq_i(defer_seen, 1,
             "refuse: 处理函数跑了（推迟是它自己置的），只是那个推迟兑现不了");

  // ---- D) 对照：三样都不成立时，同一条连接、同一个路径照常 200 ---------
  check(roundtrip(uvcpp_http_request::make_get("/ok")),
        "refuse: 对照那一条的响应到了");
  check_eq_i(p.status, 200,
             "refuse: 三样都不成立时 /ok 照常 200（上面那三条是有条件的）");
  check_eq_i(ok_calls, 1, "refuse: 对照那一条上处理函数跑了，且只跑了一次");
}


/**
 * @brief **只开 h3** 的服务端：`stop()` 必须让 `run()` 回来。
 *
 * 这条钉的是 `stop()` 里那句 `quic_server_->stop()` —— 与"h1 不回归"是两回事，
 * 它管的是 h3 自己的收尾。它的形状值得写清：
 *
 * - `uvcpp_tcp_server::stop()` 在"没在 listen"时是**同步早返回**的，而纯 h3 的
 *   服务端正是这个形状（TCP 那条腿一次都没 `listen()`）。于是停止回调在 `stop()`
 *   返回**之前**就跑完了、`status_` 也立刻是 STOPPED —— 这一半不需要循环。
 * - 但 `uvcpp_quic_server::stop()` 的动作只有 `loop->stop()` 一句：它的可观测
 *   形式就是"`run()` 会不会回来"。少了那一句，纯 h3 的服务端上 `run()` 永远不
 *   回来 —— 这正是"只开 h3 也必须在停止回调里停 QUIC"那句话的后果。
 *
 * 所以这里真跑一次 `run(UV_RUN_DEFAULT)`：停止动作由一条 0ms 定时器在循环**里面**
 * 触发。外面另挂一条 2 秒的看门狗（与 `fs_async_reuse_func.cpp` 同形）—— 那一句
 * 要是没了，看门狗兜住并让本用例**红**，而不是挂死。
 */
void test_h3_only_stop_returns_from_run() {
  uvcpp_http_server server;
  uvcpp_loop*       sloop = server.get_tcp_server()->get_loop();
  uvcpp_test::loop_drain drain_sloop(sloop);

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    std::cerr << "  [FAIL] 生成自签证书失败" << std::endl;
    ++g_failures;
    return;
  }
  server.get("/", [](uvcpp_http_request&, uvcpp_http_response& resp,
                     uvcpp_tcp_client*) {
    resp = uvcpp_http_response::ok("h3-only", 7);
  });
  server.set_quic_ssl_context(&server_ctx);
  check_eq_i(server.listen_quic(0, "127.0.0.1"), 0,
             "h3-only: listen_quic(0) 返回 0");
  // **故意不 `bind()`、不 `listen()`** —— 本用例的全部意义就在"TCP 那条腿没起来"。

  int  stop_cb            = 0;
  int  stop_before_return = 0;
  bool watchdog_fired     = false;

  uvcpp_timer watchdog(sloop);
  check_eq_i(watchdog.init(), 0, "h3-only: 看门狗 init");
  watchdog.start(
      [&](uvcpp_timer*) {
        watchdog_fired = true;
        std::cerr << "[functional http3_web] WATCHDOG：stop() 没能让 run() 回来"
                  << std::endl;
        sloop->stop();  // 兜住：本用例红，但不挂死
      },
      2000, 0);

  uvcpp_timer kick(sloop);
  check_eq_i(kick.init(), 0, "h3-only: 定时器 init");
  kick.start(
      [&](uvcpp_timer*) {
        server.stop([&stop_cb] { ++stop_cb; });
        stop_before_return = stop_cb > 0 ? 1 : 0;
      },
      0, 0);

  sloop->run(UV_RUN_DEFAULT);

  check(!watchdog_fired, "h3-only: 看门狗没咬（那 0ms 之后循环自己停了）");
  check_eq_i(stop_cb, 1, "h3-only: 停止回调恰好一次");
  check_eq_i(stop_before_return, 1,
             "h3-only: TCP 那条腿没 listen 时停止回调在 stop() 返回前就跑完了");
  check_eq_i(server.has_status(HTTP_SERVER_STOPPED) ? 1 : 0, 1,
             "h3-only: 状态落到 STOPPED");
}

}  // namespace

int main() {
  std::cout << "[functional http3_web] start" << std::endl;

  quic_crypto_backend_init();
  test_web_server_h3_and_h1_over_one_object();
  test_unsupported_paths_refuse_explicitly();
  test_h3_only_stop_returns_from_run();
  quic_crypto_backend_free();

  std::cout << "[functional http3_web] checks=" << g_checks
            << " failures=" << g_failures << std::endl;
  if (g_failures == 0) {
    std::cout << "[functional http3_web] done success=true" << std::endl;
    return 0;
  }
  std::cout << "[functional http3_web] done success=false" << std::endl;
  return 2;
}

#else  // UVCPP_HTTP3_ENABLE

// 关掉 http3 模块时这个文件不该被编译（`tests/functional/CMakeLists.txt` 的
// 过滤器按文件名摘掉它 —— 规则是 `http3`）。真编到了就是配置错，返回非零而不是
// 打一句 SKIP 再返回 0：后者会把"这个模块一次都没被跑过"伪装成"全绿"。
int main() {
  std::cerr << "[http3_web] UVCPP_HTTP3_ENABLE=0 —— 这个测试文件不该被编译进来"
            << std::endl;
  return 2;
}

#endif  // UVCPP_HTTP3_ENABLE
