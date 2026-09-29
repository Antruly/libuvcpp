/**
 * @file tests/functional/http3_request_func.cpp
 * @brief HTTP/3 的端到端里程碑：一条 QUIC 连接上真跑一次 GET / 收一次响应。
 * @author zhuweiye
 * @version 1.4.1
 *
 * 这条用例是三层的**接缝**测试：nghttp3（h3 语义）→ `uvcpp_h3_connection`
 * （关键流、写序列化、按方向的收场记账）→ `uvcpp_quic_connection`（流与连接）。
 * 它不碰 web 层 —— 那是 `http3_web_func.cpp` 的事。这样分层是为了让"h3 本身
 * 对不对"与"web 层接得对不对"各有一条独立的红。
 *
 * 断言分两段：
 *
 * **第一段（一条 GET）**，六条，每一条钉一个具体的接缝：
 *
 * 1. 握手在 deadline 内完成 —— 底下那条 QUIC 链路没被本层带坏；
 * 2. 服务端 handler 真的跑了，而且 `method == "GET"`、`path == "/"` ——
 *    钉住 QPACK 的 `:method` / `:path` token 映射（映射错了会表现为"handler 收到
 *    一个 method 是空串的请求"，那是个不会自己报错的状态）；
 * 3. 客户端拿到的 `status == 200` —— 钉住 `:status`（它在 QPACK 里的 token 与
 *    前两个不同）；
 * 4. body **恰好等于** `"hello-h3"` —— 不是"收到过字节"。分片、截断、多收一次
 *    重传都能让"收到过字节"为真；
 * 5. `take_completed()` 对这条流**恰好给一次**：取到之后再取是空（钉住那个
 *    "一条请求恰好一条完成"的队列）；
 * 6. 两侧的关键流都开出来了（`ready()`），且三条流的流号**低位各不相同** ——
 *    钉住"控制流 + QPACK 编码流 + QPACK 解码流"是三条**不同的**单向流。
 *
 * **第二段（两条并发请求）**，钉的是"记账按流分开"。写侧那本账
 * （`pending_` 每流一条 FIFO）如果退化成"一个全局计数器"，一条 GET 上是看不出来
 * 的 —— 两条请求同时在飞、响应体不同，才会让"记到别人头上"这件事显形。
 *
 * **为什么两端共用一条循环**：同 `quic_stream_func.cpp` 文件头那条 —— 两个端点
 * 在同一个进程里，两条循环只会把"谁先跑"变成时序偶然。
 *
 * **第三段（`test_h3_web_client_over_one_loop`）** 是 **P4** 的判据：同一条
 * 裸 h3 服务端，客户端换成 `uvcpp_http_client` + `set_http3_enabled(true)`，
 * 钉的是 web 层这一侧的接线 —— `negotiated_alpn()` 从 QUIC 的 TLS 握手读、
 * 交付给用户回调的响应逐字段对得上、`send_wait()` 明说不支持、连接专属头被
 * 剥掉、以及析构次序让对端真看到断开。
 */
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_HTTP3_ENABLE

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <http3/uvcpp_h3_common.h>
#include <http3/uvcpp_h3_connection.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

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

/// 服务端给的响应体。`/` 那条是那个固定串（第四条第 4 项钉的就是"恰好等于"），
/// 其余按路径回声 —— 第二段要拿它区分两条并发请求各自的响应。
std::string body_for(const std::string& path) {
  if (path == "/") return std::string("hello-h3");
  if (path == "/empty") return std::string();  // 空体：见下面第 4 条请求那段
  return std::string("path=") + path;
}

/// 服务端收到的那份头里有没有这个名字。（`http_get_header()` 收的是
/// `http_headers`，h3 这一层是另一个结构体，所以另写一个。）
///
/// 比较大小写不敏感：h3 要求头名小写，所以实际收到的一定是小写 —— 但断言不该
/// 依赖"对端守规矩"这件事。
bool has_h3_header(const std::vector<h3_header>& hs, const char* name) {
  const size_t n = strlen(name);
  for (size_t i = 0; i < hs.size(); ++i) {
    if (hs[i].name.size() != n) continue;
    bool eq = true;
    for (size_t k = 0; k < n; ++k) {
      const unsigned char a = static_cast<unsigned char>(hs[i].name[k]);
      const unsigned char b = static_cast<unsigned char>(name[k]);
      if (tolower(a) != tolower(b)) {
        eq = false;
        break;
      }
    }
    if (eq) return true;
  }
  return false;
}

/// 同上，找一个头的值；找不到返回空串。
std::string h3_header_value(const std::vector<h3_header>& hs,
                            const char* name) {
  const size_t n = strlen(name);
  for (size_t i = 0; i < hs.size(); ++i) {
    if (hs[i].name.size() != n) continue;
    bool eq = true;
    for (size_t k = 0; k < n; ++k) {
      const unsigned char a = static_cast<unsigned char>(hs[i].name[k]);
      const unsigned char b = static_cast<unsigned char>(name[k]);
      if (tolower(a) != tolower(b)) {
        eq = false;
        break;
      }
    }
    if (eq) return hs[i].value;
  }
  return std::string();
}

void test_h3_get_over_one_loop() {
  uvcpp_loop loop;
  uvcpp_test::loop_drain drain_loop(&loop);

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    std::cerr << "  [FAIL] 生成自签证书失败" << std::endl;
    ++g_failures;
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  // -------------------------------------------------------------------
  // 服务端：一条 UDP 口
  // -------------------------------------------------------------------
  uvcpp_quic_server server(&loop);
  server.set_ssl_context(&server_ctx);
  check_eq_i(server.bind("127.0.0.1", 0), 0, "bind(127.0.0.1, 0)");

  std::vector<h3_request> server_reqs;
  int                     server_stream_closes = 0;
  int                     server_disconnects   = 0;

  // 两个端点对象先声明、h3 连接后声明 —— 析构顺序反过来，于是 h3 连接一定先于
  // 它脚下那条 QUIC 连接消失（本类**不拥有**那个连接，这是它自己的契约）。
  std::unique_ptr<uvcpp_h3_connection> srv_h3;

  const int listen_rc = server.listen([&](uvcpp_quic_connection* c) {
    srv_h3.reset(new uvcpp_h3_connection(c, /*server_side=*/true));
    uvcpp_h3_connection::callbacks cbs;

    cbs.on_request = [&](uvcpp_h3_connection& h3, const h3_request& req) {
      server_reqs.push_back(req);
      h3_response resp;
      resp.stream_id = req.stream_id;
      resp.status    = 200;
      h3_header ct;
      ct.name  = "content-type";
      ct.value = "text/plain";
      resp.headers.push_back(ct);
      resp.body = body_for(req.path);
      const int rc = h3.send_response(resp);
      if (rc != 0) {
        std::cerr << "  [FAIL] 服务端 send_response 返回 " << rc << std::endl;
        ++g_failures;
      }
    };
    cbs.on_stream_close = [&](uvcpp_h3_connection&,
                              const h3_stream_close_info&) {
      ++server_stream_closes;
    };
    cbs.on_error = [&](uvcpp_h3_connection&, int code) {
      std::cerr << "  [FAIL] 服务端 h3 报致命错误：" << code << std::endl;
      ++g_failures;
    };
    cbs.on_disconnect = [&](uvcpp_h3_connection&) { ++server_disconnects; };

    const int rc = srv_h3->start(cbs);
    if (rc != 0) {
      std::cerr << "  [FAIL] 服务端 h3 start() 返回 " << rc << std::endl;
      ++g_failures;
    }
  });
  check_eq_i(listen_rc, 0, "listen() 返回 0");

  const int port = server.configured_port();
  check(port > 0, "listen() 之后 configured_port() 报内核分配的端口");

  // -------------------------------------------------------------------
  // 客户端
  // -------------------------------------------------------------------
  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  std::unique_ptr<uvcpp_h3_connection> cli_h3;
  std::vector<h3_response>             client_done;
  int                                  client_stream_closes = 0;
  int                                  client_disconnects   = 0;
  bool                                 connected            = false;
  int                                  connect_status       = 12345;
  int64_t                              req1_id              = -1;
  int64_t                              req2_id              = -1;
  int64_t                              req3_id              = -1;

  // **完成队列是"拉"的，所以得有人来拉。** `uvcpp_h3_connection` 刻意不给
  // "响应到了"的回调：一条响应的收场在 h3 里是好几条路（收全、被 reset、连接
  // 断了）汇成的，做成回调就要在三条路上各报一次、还要自己保证不重不漏。队列
  // 把这层保证收在一处（`take_completed` 一次一条）。
  //
  // 代价是消费者要有个拉的地方 —— 这里就是它。放测试里而不是库里，是因为
  // "多久拉一次"是使用者的调度问题，本层不该替它定。
  auto pump_responses = [&]() {
    h3_response r;
    while (cli_h3 && cli_h3->take_completed(r)) client_done.push_back(r);
  };

  const int rc = client.connect("127.0.0.1", port, [&](int status) {
    connected      = true;
    connect_status = status;
    if (status != 0) return;
    // 这个回调是**握手完成**那一刻跑的，而 `on_alpn` 严格早于它（同一个包的
    // 处理里，ALPN 先报、`fire_connect_if_established` 在收包函数的尾巴上）——
    // 所以三条关键流此刻已经开出来了，可以直接提问。
    h3_request req;
    req.method    = "GET";
    req.scheme    = "https";
    req.authority = "localhost";
    req.path      = "/";
    req1_id       = cli_h3->send_request(req);
  });
  check_eq_i(rc, 0, "connect() 返回 0");
  if (rc != 0) return;

  // `connect()` 返回之后、下一次循环迭代之前建 h3 连接并装回调 —— 这是
  // `uvcpp_quic_client::connect()` 文件里那条 `@warning` 说的唯一安全窗口。
  cli_h3.reset(new uvcpp_h3_connection(client.connection(), /*server_side=*/false));
  {
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_stream_close = [&](uvcpp_h3_connection&, const h3_stream_close_info&) {
      ++client_stream_closes;
    };
    cbs.on_error = [&](uvcpp_h3_connection&, int code) {
      std::cerr << "  [FAIL] 客户端 h3 报致命错误：" << code << std::endl;
      ++g_failures;
    };
    cbs.on_disconnect = [&](uvcpp_h3_connection&) { ++client_disconnects; };
    check_eq_i(cli_h3->start(cbs), 0, "客户端 h3 start() 返回 0");
  }

  // -------------------------------------------------------------------
  // 第 1 条：握手
  // -------------------------------------------------------------------
  const bool up = uvcpp_test::wait_until(&loop, [&] { return connected; },
                                         kDeadlineMs);
  check(up, "握手在 deadline 内完成");
  if (!up) return;
  check_eq_i(connect_status, 0, "connect 的 cb 收到 0");

  // 第 6 条的前半：三条关键单向流。
  check(uvcpp_test::wait_until(&loop, [&] { return cli_h3->ready(); },
                               kDeadlineMs),
        "客户端的三条关键流开出来了（ready()）");
  check(cli_h3->control_stream_id() >= 0, "控制流有流号");
  check(cli_h3->qpack_encoder_stream_id() >= 0, "QPACK 编码流有流号");
  check(cli_h3->qpack_decoder_stream_id() >= 0, "QPACK 解码流有流号");
  check(cli_h3->control_stream_id() != cli_h3->qpack_encoder_stream_id() &&
            cli_h3->qpack_encoder_stream_id() !=
                cli_h3->qpack_decoder_stream_id() &&
            cli_h3->control_stream_id() != cli_h3->qpack_decoder_stream_id(),
        "三条关键流是三条不同的流");
  // RFC 9000 §2.1：客户端发起的单向流，最低两位是 0b10。
  check_eq_i(cli_h3->control_stream_id() & 0x3, 2, "控制流的流号低位是 0b10");
  check_eq_i(cli_h3->qpack_encoder_stream_id() & 0x3, 2,
             "QPACK 编码流的流号低位是 0b10");
  check_eq_i(cli_h3->qpack_decoder_stream_id() & 0x3, 2,
             "QPACK 解码流的流号低位是 0b10");

  check(req1_id >= 0, "客户端提交了第一条请求");
  if (req1_id < 0) return;

  // -------------------------------------------------------------------
  // 第 2 条：服务端 handler 真跑了，而且 method / path 都对
  // -------------------------------------------------------------------
  check(uvcpp_test::wait_until(&loop, [&] { return !server_reqs.empty(); },
                               kDeadlineMs),
        "服务端的 on_request 跑了");
  if (!server_reqs.empty()) {
    check_eq_s(server_reqs[0].method, "GET", "服务端看到的 :method");
    check_eq_s(server_reqs[0].path, "/", "服务端看到的 :path");
    check_eq_s(server_reqs[0].scheme, "https", "服务端看到的 :scheme");
    check_eq_s(server_reqs[0].authority, "localhost",
               "服务端看到的 :authority");
    check_eq_i(server_reqs[0].stream_id, req1_id,
               "服务端看到的流号就是客户端开的那个");
    check_eq_s(server_reqs[0].body, "", "这条 GET 没有请求体");
  }

  // -------------------------------------------------------------------
  // 第 3、4 条：status 与 body
  // -------------------------------------------------------------------
  check(uvcpp_test::wait_until(
            &loop,
            [&] {
              pump_responses();
              return !client_done.empty();
            },
            kDeadlineMs),
        "客户端拿到了一条响应");
  if (!client_done.empty()) {
    const h3_response& r = client_done[0];
    check_eq_i(r.error, 0, "响应是干净收场的");
    check_eq_i(r.status, 200, "客户端看到的 :status");
    check_eq_i(r.stream_id, req1_id, "响应挂在客户端开的那条流上");
    // **"恰好等于"**，不是"非空"：分片、截断、多发一次重传都能让"收到过字节"
    // 为真，而这条断言要钉的是 body 一个字节不多一个字节不少。
    check_eq_s(r.body, "hello-h3", "客户端收到的响应体");
  }
  // -------------------------------------------------------------------
  // 第 5 条：恰好一次
  // -------------------------------------------------------------------
  // 上面那次 `wait_until` 只说明"至少有一条"。再转几圈、再拉几轮，确认它没有
  // 第二条 —— 一条请求的完成队列里出现两次，会在调用方的每一次轮询里都多出
  // 一条假响应。
  uvcpp_test::pump_for(&loop, 50);
  pump_responses();
  check_eq_i(static_cast<int>(client_done.size()), 1, "那一条请求只给过一次");
  check_eq_i(cli_h3->completed_count(), 0, "取走之后队列里没有第二条");
  h3_response extra;
  // 这一句必须在 `completed_count() == 0` **之后**：调用会把它掏空，先调的话
  // 上面那条断言就永远为真了。
  check(!cli_h3->take_completed(extra),
        "take_completed() 对同一条流不会再给第二次");

  // -------------------------------------------------------------------
  // 第二段：两条并发请求
  // -------------------------------------------------------------------
  {
    h3_request a;
    a.method    = "GET";
    a.authority = "localhost";
    a.path      = "/a";
    req2_id     = cli_h3->send_request(a);
    h3_request b;
    b.method    = "GET";
    b.authority = "localhost";
    b.path      = "/b";
    req3_id     = cli_h3->send_request(b);
  }
  check(req2_id >= 0, "第二条请求提交成功");
  check(req3_id >= 0, "第三条请求提交成功");
  check(req2_id != req3_id, "两条并发请求各占一条流");

  check(uvcpp_test::wait_until(
            &loop,
            [&] {
              pump_responses();
              return client_done.size() >= 3;
            },
            kDeadlineMs),
        "三条响应到底都到了");
  if (client_done.size() >= 3) {
    // **按流号对号入座**，不是按到达顺序。哪一条先到是链路的偶然；而"记到别人
    // 头上"这件事只有对号入座才看得出来。
    std::map<int64_t, std::string> bodies;
    std::map<int64_t, int>         statuses;
    for (size_t i = 0; i < client_done.size(); ++i) {
      bodies[client_done[i].stream_id]   = client_done[i].body;
      statuses[client_done[i].stream_id] = client_done[i].status;
    }
    check_eq_s(bodies[req1_id], "hello-h3", "第 1 条请求的响应体");
    check_eq_s(bodies[req2_id], "path=/a", "第 2 条请求的响应体");
    check_eq_s(bodies[req3_id], "path=/b", "第 3 条请求的响应体");
    check_eq_i(statuses[req2_id], 200, "第 2 条请求的 :status");
    check_eq_i(statuses[req3_id], 200, "第 3 条请求的 :status");
  }
  check_eq_i(static_cast<int>(server_reqs.size()), 3, "服务端一共收到三条请求");

  // -------------------------------------------------------------------
  // 第四段：**空体响应**
  // -------------------------------------------------------------------
  // 上面那三段的服务端响应都带体。这一条走的是另一条路：`body` 为空且
  // `omit_body` 为假 —— 本层据此**不**给 nghttp3 挂 data reader（空 reader 会
  // 在线上多出一次零长 DATA 帧的机会，没有任何好处）。于是这条响应的形状是
  // "头块 + FIN"，一个 DATA 帧都没有，客户端拿到的体必须是**空**的。
  //
  // **它不钉 `add_write_offset(sid, 0)`，也不该被说成钉了它。** 那条规矩管的是
  // nghttp3 文档里那种 `veccnt == 0 && fin` 的块（"0 length data … and it is
  // the last data to the stream"），而**本层的 reader 形状产生不出那种块**：
  // `cb_read_data` 把剩下的字节和 EOF **在同一次调用里**一起交出去，于是
  // `nghttp3_stream_write_data()` 里那条"EOF 但 datalen == 0 且 outq 已吐空"
  // 的分支永远走不到（实测：把 reader 临时改成"先交数据、下一轮再交 EOF"的
  // 两段式之后，`drain()` 当场吐出 `vec=0 total=0 fin=1` 的块，那条路才出现）。
  //
  // 所以 `flush()` 里那句"无论多少字节都要调 `add_write_offset()`"是**防御性**
  // 的：它按上游文档把那个形状处理对了，功能用例压不到它 —— 这一条如实记在这里，
  // 别让下一个人以为它被覆盖了。
  h3_request empty;
  empty.method    = "GET";
  empty.authority = "localhost";
  empty.path      = "/empty";
  const int64_t req4_id = cli_h3->send_request(empty);
  check(req4_id >= 0, "空体那条请求提交成功");

  check(uvcpp_test::wait_until(
            &loop,
            [&] {
              pump_responses();
              return client_done.size() >= 4;
            },
            kDeadlineMs),
        "空体那条响应也到了（那个只有 FIN 的块发出去了）");
  {
    bool      seen = false;
    h3_response r4;
    for (size_t i = 0; i < client_done.size(); ++i) {
      if (client_done[i].stream_id == req4_id) {
        seen = true;
        r4   = client_done[i];
        break;
      }
    }
    check(seen, "空体那条响应挂在它自己那条流上");
    if (seen) {
      check_eq_i(r4.error, 0, "空体响应是干净收场的");
      check_eq_i(r4.status, 200, "空体响应的 :status");
      check(r4.body.empty(), "空体响应的体是空的（不是'少收了一段'）");
    }
  }
  check_eq_i(static_cast<int>(server_reqs.size()), 4, "服务端一共收到四条请求");

  // -------------------------------------------------------------------
  // 第 6 条：收尾
  // -------------------------------------------------------------------
  // 到这一刻三条响应都到齐了，服务端那三笔响应也早被确认 —— 两侧的流应该都
  // 已经收场。等它，而不是"睡一会儿再看"。
  check(uvcpp_test::wait_until(&loop, [&] { return server_stream_closes >= 4; },
                               kDeadlineMs),
        "服务端四条流都关了");
  check(uvcpp_test::wait_until(&loop, [&] { return client_stream_closes >= 4; },
                               kDeadlineMs),
        "客户端四条流都关了");

  check_eq_i(client.close(), 0, "close() 返回 0");

  check(uvcpp_test::wait_until(&loop, [&] { return client_disconnects == 1; },
                               kDeadlineMs),
        "客户端 h3 的 on_disconnect 恰好一次");
  check(uvcpp_test::wait_until(&loop, [&] { return server_disconnects == 1; },
                               kDeadlineMs),
        "服务端 h3 的 on_disconnect 恰好一次");
  // 再多转几圈，确认它没有第二次 —— "恰好一次"这四个字的后半截。
  uvcpp_test::pump_for(&loop, 50);
  check_eq_i(client_disconnects, 1, "客户端 on_disconnect 没有第二次");
  check_eq_i(server_disconnects, 1, "服务端 on_disconnect 没有第二次");
}

// =========================================================================
// 第二段：**web 层客户端**那条腿
// =========================================================================

/// P4 的判据：`uvcpp_http_client` 打开 `set_http3_enabled(true)` 之后，真的在
/// QUIC（UDP）上把一次 GET 跑完，并且交给用户回调的东西是**对端说过的**那一份。
///
/// 打的是上面那个裸 h3 服务端（`uvcpp_quic_server` + `uvcpp_h3_connection`），
/// **不是** web 服务端 —— 那一条是 `http3_web_func.cpp` 的事。分开是为了让
/// "客户端接线对不对"与"服务端接线对不对"各有一条独立的红。
///
/// **两个循环**：`uvcpp_http_client` 自带一条（`get_tcp_client()->get_loop()`），
/// 服务端在用例这条上。所以两边都要泵（`wait_until_pair` / `pump_for_pair`）——
/// 只泵一条会变成"客户端发得出去、服务端收不到"的超时。
void test_h3_web_client_over_one_loop() {
  uvcpp_loop loop;
  uvcpp_test::loop_drain drain_loop(&loop);

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    std::cerr << "  [FAIL] web: 生成自签证书失败" << std::endl;
    ++g_failures;
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  // -------------------------------------------------------------------
  // 服务端：与第一段同一套（裸 QUIC + h3），照路径回声
  // -------------------------------------------------------------------
  uvcpp_quic_server server(&loop);
  server.set_ssl_context(&server_ctx);
  check_eq_i(server.bind("127.0.0.1", 0), 0, "web: bind(127.0.0.1, 0)");

  std::vector<h3_request> server_reqs;
  int                     server_disconnects = 0;
  std::unique_ptr<uvcpp_h3_connection> srv_h3;

  const int listen_rc = server.listen([&](uvcpp_quic_connection* c) {
    srv_h3.reset(new uvcpp_h3_connection(c, /*server_side=*/true));
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_request = [&](uvcpp_h3_connection& h3, const h3_request& req) {
      server_reqs.push_back(req);
      h3_response resp;
      resp.stream_id = req.stream_id;
      resp.status    = 200;
      h3_header ct;
      ct.name  = "content-type";
      ct.value = "text/plain";
      resp.headers.push_back(ct);
      resp.body = body_for(req.path);
      h3.send_response(resp);
    };
    cbs.on_error = [&](uvcpp_h3_connection&, int code) {
      std::cerr << "  [FAIL] web: 服务端 h3 报致命错误：" << code << std::endl;
      ++g_failures;
    };
    cbs.on_disconnect = [&](uvcpp_h3_connection&) { ++server_disconnects; };
    srv_h3->start(cbs);
  });
  check_eq_i(listen_rc, 0, "web: listen() 返回 0");
  const int port = server.configured_port();
  if (port <= 0) return;

  {
    uvcpp_http_client client;
    uvcpp_loop*       cloop = client.get_tcp_client()->get_loop();

    client.set_http3_enabled(true);
    check(client.http3_enabled(),
          "web: set_http3_enabled(true) 之后 http3_enabled() 为真");

    // 同步那条路必须**明说**不支持。`send_wait()` 是对 socket fd 做阻塞收发，
    // 而 QUIC 底下没有那条 fd；悄悄退化成 h1 会把明文写进一条 UDP 连接，
    // 症状是"写成功了、响应永远不来"，一处报错都没有。
    //
    // **这一句必须排在 `set_ssl_context()` 之前。** 装了 TLS 之后 `send_wait()`
    // 还有另一道守卫（"异步 TLS 建起来的连接不能阻塞读"）报的还是同一个码，
    // 那一道会把 h3 这一道**掩住** —— 这时弄坏 h3 那道守卫这条用例也不会红，
    // 测到的就不是 h3 这条了。实测：把三道 h3 守卫全去掉、且先装了 TLS，
    // 这条断言照样绿（见本次提交的反空转表）。
    {
      uvcpp_http_response sync_resp;
      const int wrc =
          client.send_wait(uvcpp_http_request::make_get("/"), sync_resp, 500);
      check_eq_i(wrc, UV_ENOTSUP, "web: h3 上 send_wait() 报 UV_ENOTSUP");
    }

    // TLS **必须在 connect 之前**装：h3 那条腿要拿它去拼 QUIC 的 ClientHello
    // （QUIC 没有明文模式），装晚了 `connect()` 直接返回 `UV_EINVAL`。
    client.set_ssl_context(&client_ctx);

    int       connect_status = 12345;
    bool      connected      = false;
    const int rc = client.connect("127.0.0.1", port, [&](int status) {
      connect_status = status;
      connected      = true;
    });
    check_eq_i(rc, 0, "web: connect() 返回 0");
    check(uvcpp_test::wait_until_pair(&loop, cloop,
                                      [&] { return connected; }, kDeadlineMs),
          "web: 握手在 deadline 内完成");
    check_eq_i(connect_status, 0, "web: connect 的 cb 收到 0");

    // ALPN：h3 那份是从 **QUIC 自己的** TLS 握手里读的，不是从 `tcp_` 那份。
    // 空串（= 那一格没人写过）与 `"h3"` 在功能上是两回事，所以要钉。
    check_eq_s(client.negotiated_alpn(), "h3", "web: negotiated_alpn()");

    // -----------------------------------------------------------------
    // 第 1 条：`make_get` 拼出来的请求，**没有 host 头**
    // -----------------------------------------------------------------
    int         req1_calls   = 0;
    int         req1_err     = 12345;
    int         req1_status  = 0;
    int         req1_version = -1;
    long long   req1_sid     = 0;
    std::string req1_body;
    std::string req1_ct;

    uvcpp_http_request r1 = uvcpp_http_request::make_get("/");
    r1.set_header("x-mark", "h3web");
    // h1 的 `to_string()` 默认就会给每个请求加一个 `connection: keep-alive`，
    // 于是"一个为 h1 拼好的请求原样交给 h3"是常态而不是异常。它在 h3 里是
    // **畸形头**（RFC 9114 §4.2 禁掉 connection / keep-alive / transfer-encoding
    // / upgrade / proxy-connection），必须在会话层剥掉。这一条钉的就是它 ——
    // 手工加上去，比等某条 h1 路径顺手带一个更确定。
    r1.set_header("connection", "keep-alive");

    const int s1 = client.send(r1, [&](const uvcpp_http_response& r, int err) {
      ++req1_calls;
      req1_err     = err;
      req1_status  = static_cast<int>(r.status_code);
      req1_version = static_cast<int>(r.version);
      req1_sid     = r.stream_id;
      req1_body    = r.body.to_string();
      req1_ct      = r.get_header("content-type");
    });
    check_eq_i(s1, 0, "web: send() 返回 0");

    check(uvcpp_test::wait_until_pair(&loop, cloop,
                                      [&] { return req1_calls > 0; },
                                      kDeadlineMs),
          "web: 第 1 条响应到了");
    check_eq_i(req1_err, 0, "web: 第 1 条干净收场");
    check_eq_i(req1_status, 200, "web: 第 1 条的 :status");
    check_eq_s(req1_body, "hello-h3", "web: 第 1 条的 body");
    check_eq_s(req1_ct, "text/plain",
               "web: 第 1 条响应头里搬过来的字段（`content-type`）");
    check_eq_i(req1_version, static_cast<int>(uvcpp_http_version::HVER_30),
               "web: 响应报的版本是 HTTP/3，不是默认那个 1.1");

    check_eq_i(static_cast<int>(server_reqs.size()), 1,
               "web: 服务端收到第 1 条请求");
    if (!server_reqs.empty()) {
      const h3_request& sr = server_reqs[0];
      // 响应交付的那条流号 = 服务端看到的那条流。
      //
      // **这里不能写 `sid != 0`**：h3 上流号 0 是**合法**的（RFC 9000 §2.1：
      // 客户端发起的双向流从 0 起、每条 +4），而 `uvcpp_http_response::stream_id`
      // 那个"0 表示 HTTP/1.1"的约定是 h2/h1 那两条腿定的。所以 h3 上**分辨传输
      // 只能靠 `version`**（上面那条断言钉的就是它），流号这一栏只用来对号入座。
      check_eq_i(req1_sid, sr.stream_id,
                 "web: 响应交付的流号 = 服务端看到的那条流");
      check_eq_s(sr.method, "GET", "web: 服务端看到的 :method");
      check_eq_s(sr.path, "/", "web: 服务端看到的 :path");
      check_eq_s(sr.scheme, "https", "web: 服务端看到的 :scheme");
      // 请求里没写 host ⇒ 兜底用 `connect()` 的 host。钉的就是那条兜底：
      // 缺了它发出去的是一个**空** `:authority`，服务端一律判畸形。
      check_eq_s(sr.authority, "127.0.0.1",
                 "web: 缺 host 时 :authority 取 connect() 的 host");
      check_eq_s(h3_header_value(sr.headers, "x-mark"), "h3web",
                 "web: 自定义头原样到了对端");
      check(!has_h3_header(sr.headers, "connection"),
            "web: 连接专属头 `connection` 被剥掉了（剥不掉就是畸形请求）");
      check(!has_h3_header(sr.headers, "host"),
            "web: `host` 没有作为普通头再出现一次（它已经是 `:authority`）");
    }

    // 交付**恰好一次**：再转几圈 + 再泵两圈，计数不许动。
    uvcpp_test::pump_for_pair(&loop, cloop, 100);
    check_eq_i(req1_calls, 1, "web: 第 1 条的回调恰好一次");

    // -----------------------------------------------------------------
    // 第 2 条：显式带 host 与**三种**连接专属头 —— 钉另一条分支
    // -----------------------------------------------------------------
    int         req2_calls  = 0;
    int         req2_err    = 12345;
    int         req2_status = 0;
    std::string req2_body;

    uvcpp_http_request r2 = uvcpp_http_request::make_get("/second");
    r2.set_header("host", "example.test");  // 有 host ⇒ `:authority` 取它
    r2.set_header("connection", "keep-alive");
    r2.set_header("keep-alive", "timeout=5");
    r2.set_header("transfer-encoding", "chunked");

    const int s2 = client.send(r2, [&](const uvcpp_http_response& r, int err) {
      ++req2_calls;
      req2_err    = err;
      req2_status = static_cast<int>(r.status_code);
      req2_body   = r.body.to_string();
    });
    check_eq_i(s2, 0, "web: 第 2 条 send() 返回 0");
    check(uvcpp_test::wait_until_pair(&loop, cloop,
                                      [&] { return req2_calls > 0; },
                                      kDeadlineMs),
          "web: 第 2 条响应到了");
    check_eq_i(req2_err, 0, "web: 第 2 条干净收场");
    check_eq_i(req2_status, 200, "web: 第 2 条的 :status");
    check_eq_s(req2_body, "path=/second", "web: 第 2 条的 body");
    check_eq_i(req2_calls, 1, "web: 第 2 条的回调恰好一次");

    check_eq_i(static_cast<int>(server_reqs.size()), 2,
               "web: 服务端一共收到两条请求（同一条连接上跑两次 send）");
    if (server_reqs.size() >= 2) {
      const h3_request& sr = server_reqs[1];
      // 第二条客户端发起的双向流就是 4（0 + 4）—— 本层在这条连接上除请求以外
      // 不开任何别的**双向**流，而关键流走的是单向流号空间，不参与这个算术。
      check_eq_i(sr.stream_id, 4,
                 "web: 第 2 条请求是第二条客户端双向流（流号 4）");
      check_eq_s(sr.authority, "example.test",
                 "web: 写了 host 时 :authority 取请求里那个");
      check(!has_h3_header(sr.headers, "connection"),
            "web: 第 2 条的 `connection` 也剥了");
      check(!has_h3_header(sr.headers, "keep-alive"),
            "web: `keep-alive` 也剥了");
      check(!has_h3_header(sr.headers, "transfer-encoding"),
            "web: `transfer-encoding` 也剥了（在 h3 里 chunked 不是一个编码）");
    }

    // -----------------------------------------------------------------
    // 收尾：客户端析构 ⇒ QUIC 端点收掉 ⇒ 对端看到断开
    // -----------------------------------------------------------------
    // 析构里那句 `delete h3_` 必须排在 `delete quic_` **之前**（h3 层捕着那条
    // QUIC 连接却不拥有它）。次序倒了这里不一定会红，但 sanitizer 会 —— 而
    // "对端在 deadline 内看到断开"这条断言是那个次序在功能上的可见后果：
    // 连接没被真正收掉，服务端就一直挂着。
  }
  check(uvcpp_test::wait_until(&loop, [&] { return server_disconnects >= 1; },
                               kDeadlineMs),
        "web: 客户端析构之后，服务端看到 h3 断开");
  uvcpp_test::pump_for(&loop, 50);
  check_eq_i(server_disconnects, 1, "web: 服务端 on_disconnect 恰好一次");
}

}  // namespace

int main() {
  std::cout << "[functional http3_request] start" << std::endl;
  std::cout << "[functional http3_request] nghttp3="
            << uvcpp_h3_connection::nghttp3_version() << std::endl;

  check_eq_i(quic_crypto_backend_init(), 0, "quic_crypto_backend_init()");

  test_h3_get_over_one_loop();
  test_h3_web_client_over_one_loop();

  quic_crypto_backend_free();

  std::cout << "[functional http3_request] checks=" << g_checks
            << " failures=" << g_failures << std::endl;
  if (g_failures == 0) {
    std::cout << "[functional http3_request] done success=true" << std::endl;
    return 0;
  }
  std::cout << "[functional http3_request] done success=false" << std::endl;
  return 2;
}

#else  // UVCPP_HTTP3_ENABLE

// 关掉 http3 模块时这个文件不该被编译（`tests/functional/CMakeLists.txt` 的
// 过滤器按文件名摘掉它 —— 规则是 `http3`）。真编到了就是配置错，返回非零而不是
// 打一句 SKIP 再返回 0：后者会把"这个模块一次都没被跑过"伪装成"全绿"。
int main() {
  std::cerr << "[http3_request] UVCPP_HTTP3_ENABLE=0 —— 这个测试文件不该被编译进来"
            << std::endl;
  return 2;
}

#endif  // UVCPP_HTTP3_ENABLE
