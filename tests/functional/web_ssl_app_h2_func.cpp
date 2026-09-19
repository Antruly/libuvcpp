/**
 * @file tests/functional/web_ssl_app_h2_func.cpp
 * @brief 框架层 HTTP/2：`uvcpp_web_app` **默认**就该支持 h2，零配置。
 *
 * 分工与前两个 h2 文件不同，缺一不可：
 *   - `web_h2_session_func` 把两条会话面对面摆好、手工搬字节 —— 考**转换层**；
 *   - `web_ssl_h2_server_func` 真 TLS + 真 socket 直连 `uvcpp_http_server` ——
 *     考**传输层 + 路由表**；
 *   - 本文件考的是**框架**：`uvcpp_web_app` 自己有没有把 h2 接上。这三层里
 *     只有这一层是"用户实际会用的那个入口"。
 *
 * 分层上的关键差别是**默认值方向相反**，这正是本文件存在的理由：
 * `uvcpp_http_server::set_http2_enabled` **默认 false**（协议实现，"我没说要 h2"
 * 就该是 h1），而框架默认**开着** —— 用户不该为了拿到现代协议去读 ALPN 文档。
 * 所以场景 1 里 `app` **一次都没碰** h2 相关的 API，客户端却必须协商出 h2。
 *
 * 两个场景互为对照，缺一个都说明不了问题。**客户端是同一个客户端**（同一份
 * ALPN 名单 `kClientAlpn`、同一个请求），唯一的变量是 `set_http2_enabled`：
 *   1. **默认**：一次都不调 h2 API → 协商出 `h2`，请求走框架路由（含带路径
 *      参数的路由）、响应落在对的流上；
 *   2. **可关**：`set_http2_enabled(false)` → 同一个客户端被服务端挡回
 *      `http/1.1`，同一个请求照样跑通。
 *
 * 只测场景 1 的话，"关掉"这个概念没被验证过；只测场景 2 的话，"默认开"没被
 * 验证过。而这两件事在实现上是**两个开关**（`http2_requested_` 与 ALPN 名单），
 * 任何一个接错都能让另一个看起来正常。
 *
 * 场景 2 里客户端**照样宣告 h2**：要证的是"服务端拒绝了它"，而不是"客户端
 * 没要"。后者只要客户端少报一个名字就能做到，什么也证明不了。
 *
 * 判据同样分两处：服务端处理函数看到了什么（路径参数解出来没有、流号对不对），
 * 以及客户端在哪条流上收到了什么。只看客户端的话，"响应落在别的流上"与
 * "服务端压根没路由"分不开。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_OPENSSL_ENABLE && UVCPP_NGHTTP2_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_handler.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

#include <http2/uvcpp_h2_connection.h>

#include <openssl/ssl.h>

#include "loop_drain.h"
#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// 单个事件的墙钟上限。`wait_util.h` 的原语按墙钟走，不是按圈数。
const int kWaitMs = 6000;

const char kHelloBody[]    = "hello-over-h2-app";
const char kParamEchoPre[] = "param=";

/// 场景 3 用的三块 SSE 载荷。**故意都够短**（hex 长度是一位数），这样"组了
/// h1 的帧"在线上的形状是可预测的：`6\r\ndata: 1\n\n\r\n` 之类。
const char kSse1[] = "data: 1\n\n";
const char kSse2[] = "data: 2\n\n";
const char kSse3[] = "data: 3\n\n";

/// 场景 5/7：一块的大小与块数。**总和必须显著大于 65535** —— 那是 h2 默认的
/// 连接级流控窗口，超出窗口的部分会一直排在服务端的发送队列里，这正是这条
/// 用例要的那种"还没上线的块"。
///
/// 倍数还得**够多**：客户端读一次就是一轮"发 WINDOW_UPDATE → 服务端再发一个
/// 窗口 → 客户端把它们全读掉"，而 `uv__read` 是循环到 EAGAIN 的，这一整轮可以
/// 落在同一个 `uv_run(NOWAIT)` 里。16 个窗口（1 MiB）才让"一遍读完全部"成为
/// 可以忽略的概率 —— 2 个窗口时实测能偶发整条收完，用例于是什么也没测到。
const size_t kBigChunkSize = 16 * 1024;
const int    kBigChunks    = 64;  // 1 MiB ≈ 16 个流控窗口

/// 场景 4：文档根里那个**该**取得出来的文件，以及根目录**外面**那个**绝不该**
/// 取得出来的文件。后者是"h2 上静态服务的目录穿越防护还灵不灵"的唯一判据。
const char kStaticBody[] = "static-over-h2-ok";
const char kSecretBody[] = "SECRET-MUST-NOT-LEAK";

/// 客户端宣告的 ALPN 名单，**两个场景共用**。
///
/// 像真客户端那样两个都报（浏览器、curl 都是这么发的），而不是只报 `h2`。
/// 只报 `h2` 会有个陷阱：服务端关掉 h2 之后名单里只剩 `http/1.1`，两边
/// **没有交集**，OpenSSL 的选择回调返回 `NOACK`，握手照样成功但 ALPN 是
/// **空**的 —— 于是"服务端拒绝 h2"和"服务端不支持 ALPN"在客户端看起来
/// 一模一样。报两个就没有这个歧义：服务端必须**明确挑一个**答复。
const std::vector<std::string> kClientAlpn{"h2", "http/1.1"};

// =========================================================================
// 场景 4 用的临时目录：一个文档根，以及它**外面**的一个"秘密"文件
// =========================================================================

/// 临时目录。**故意不走 `<windows.h>` 的 `GetTempPathA`**：那个头会把 Winsock 1.1
/// (`winsock.h`) 拉进来，而本文件经由 libuv 的公开头已经吃进了 `winsock2.h` ——
/// 两个头在 `sockaddr`/`timeval`/`accept` 上一路重定义，报出来的是一屏
/// `error C2011`，而它们全都指向 Windows SDK 自己的头，看着和本文件毫无关系。
/// 环境变量拿的是一个东西，代价是零。
std::string temp_dir() {
#if defined(_WIN32)
  const char* e = std::getenv("TEMP");
  if (e == nullptr || *e == '\0') e = std::getenv("TMP");
  if (e == nullptr || *e == '\0') return ".\\";
  std::string d(e);
  // 统一成反斜杠 + 尾随分隔符，免得下面的拼接出现 `C:\x\y/z` 这种混合形状。
  for (size_t i = 0; i < d.size(); ++i) {
    if (d[i] == '/') d[i] = '\\';
  }
  if (d[d.size() - 1] != '\\') d += '\\';
  return d;
#else
  return "/tmp/";
#endif
}

/// 场景 4 的路径。根目录带一个尾随分隔符，好让拼接不出错。
struct static_fixture {
  std::string root;    ///< 文档根（挂到 `/pub`）
  std::string secret;  ///< 根目录**外面**的文件 —— 穿越成功就等于它漏了

  bool make() {
    const std::string base = temp_dir() + "uvcpp_h2_static_";
    root   = base + "root";
    secret = base + "secret.txt";

#if defined(_WIN32)
    const std::string cmd = "if not exist \"" + root + "\" mkdir \"" + root + "\"";
    if (std::system(cmd.c_str()) != 0) return false;
#else
    const std::string cmd = "mkdir -p '" + root + "'";
    if (std::system(cmd.c_str()) != 0) return false;
#endif
    std::ofstream f1((root + "/ok.txt").c_str(), std::ios::binary);
    f1 << kStaticBody;
    f1.close();
    std::ofstream f2(secret.c_str(), std::ios::binary);
    f2 << kSecretBody;
    f2.close();
    // 写没写成功不在这里下结论 —— `on_disk_ok()` 会把字节读回来比对。
    return true;
  }

  /// 判据要落在**磁盘上真的有什么**，而不是"我以为写了什么"。
  bool on_disk_ok() const {
    std::ifstream a((root + "/ok.txt").c_str(), std::ios::binary);
    std::ifstream b(secret.c_str(), std::ios::binary);
    std::string   sa((std::istreambuf_iterator<char>(a)),
                     std::istreambuf_iterator<char>());
    std::string   sb((std::istreambuf_iterator<char>(b)),
                     std::istreambuf_iterator<char>());
    return sa == kStaticBody && sb == kSecretBody;
  }
};

/// 路径参数取不到时返回一个显眼的哨兵，避免"空串也算过"的假绿。
/// （`param()` 返回的是 `const std::string*`，取不到就是 nullptr。）
std::string param_of(uvcpp_web_request& req, const char* name) {
  const std::string* p = req.param(name);
  return p != nullptr ? *p : std::string("<missing>");
}

// =========================================================================
// 服务端侧观测：框架的处理函数到底被叫到没有
// =========================================================================

struct seen_request {
  std::string url;
  std::string param;  ///< 路由模板 `:name` 解出来的值
  int32_t     stream_id = 0;
};

struct app_probe {
  std::atomic<int> bound_port{-1};
  std::atomic<int> start_rc{-999};
  /// 框架自己答的 `http2_enabled()`（-1 = 还没读）。
  std::atomic<int> h2_flag{-1};

  std::mutex                mu;
  std::vector<seen_request> seen;

  /// 场景 5/7：`/big` 的处理函数被跑到过几次（-1 不用，0 就是"没跑到"）。
  std::atomic<int> big_count{0};

  /// 场景 3：`/hold` 把自己那个响应对象存这里（由 `ctx.hold()` 钉着，活得比
  /// 处理函数久），收尾的 `end()` 由测试线程 `post()` 回去跑。
  std::atomic<uvcpp_web_response*> held{nullptr};
  /// `/hold` 的响应真的被 `end()` 过。—— `-1` = 那个 post 压根没跑。
  std::atomic<int>                 held_ended{-1};

  /// 场景 8：`/hello` 上按方法分开记 `on_sent` 的 `body_bytes`。
  ///
  /// "HEAD 的 `body_bytes` 为 0"在 h1 上是**服务端层**顺手满足的（`uvcpp_http_server`
  /// 按压缩后的长度钉完 content-length 就 `resp.body.clear()`），h2 那条路
  /// **不清** body、只把 `omit_body` 交给会话层。所以框架层那个换算只有在 h2 上
  /// 才有可观测差别 —— 也只有在 h2 树上才验得出来。
  std::atomic<int>       hello_sent_get{0};
  std::atomic<int>       hello_sent_head{0};
  std::atomic<long long> hello_bytes_get{-1};
  std::atomic<long long> hello_bytes_head{-1};
};

/// 注册场景 3 需要的三条路由。由 `build_app()` 在 `start_background()` 之前调 ——
/// 路由表在监听开始之后就不再变了，所以三个场景共用同一个 `build_app` 时，
/// 这三条对场景 1/2 只是**多挂了几条没被请求的路由**，不影响它们的判据。
void add_stream_routes(uvcpp_web_app& app, app_probe& st) {
  // 整包流式：三块，处理函数里一次写完。**这里没有异步** —— 要考的是组帧，
  // 而组帧发生在 `write_chunk()` 那一刻（处理函数体内），与是否异步无关。
  app.get("/sse", [](uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next) {
    resp.begin_chunked("text/event-stream");
    resp.write_chunk(kSse1);
    resp.write_chunk(kSse2);
    resp.write_chunk(kSse3);
    resp.end();
  });

  // **不收尾**的流：头部与第一块发出去之后挂住，收尾由外部 `post()` 决定。
  // 场景 3 靠它把"另一条流能不能先回"这件事变成可控的（而不是靠猜时序）。
  app.get("/hold", [&st](uvcpp_web_request&, uvcpp_web_response& resp,
                         uvcpp_web_next) {
    resp.begin_chunked("text/event-stream");
    resp.write_chunk(kSse1);
    st.held.store(&resp);
  });

  app.get("/plain", [](uvcpp_web_request&, uvcpp_web_response& resp,
                       uvcpp_web_next) {
    resp.text(kHelloBody);
    resp.end();
  });

  // 场景 5/7：一次写出**超过默认流控窗口**（65535）的流式 body。超出窗口的那部分
  // 会一直排在服务端的发送队列里 —— 对端一 RST（场景 5）或把整条连接扔掉
  // （场景 7），它们就再也发不出去了，而它们的 `done` 必须照样跑（那是"每一块
  // 恰好结算一次"的契约）。
  app.get("/big", [&st](uvcpp_web_request&, uvcpp_web_response& resp,
                        uvcpp_web_next) {
    st.big_count.fetch_add(1);
    resp.begin_chunked("application/octet-stream");
    for (int i = 0; i < kBigChunks; ++i) resp.write_chunk(std::string(kBigChunkSize, 'x'));
    resp.end();
  });
}

/// 建一个**只配了 TLS 和路由**的 app —— 场景 1 里 h2 相关的 API 一次都不碰。
///
/// `set_flag` 为真时才显式调 `set_http2_enabled(enable_h2)`，那是场景 2 的全部差别。
int build_app(uvcpp_web_app& app, app_probe& st, bool set_flag, bool enable_h2,
              const std::string* static_root = nullptr) {
  app.set_port(0);
  app.set_host("127.0.0.1");
  app.set_access_log(false);
  app.enable_self_signed("loopback.test", 2048);
  if (!app.ssl_enabled()) {
    std::cerr << "  [FAIL] enable_self_signed 没有生效: " << app.ssl_error()
              << std::endl;
    return -1;
  }
  if (set_flag) app.set_http2_enabled(enable_h2);
  // 读的是**框架**的答案（内部会 `&&` 一次 UVCPP_NGHTTP2_ENABLE），不是用户意图。
  st.h2_flag.store(app.http2_enabled() ? 1 : 0);

  app.get("/hello", [&st](uvcpp_web_request& req, uvcpp_web_response& resp,
                          uvcpp_web_next next) {
    (void)next;
    seen_request r;
    r.url       = req.path();
    r.stream_id = req.raw().stream_id;
    {
      std::lock_guard<std::mutex> lk(st.mu);
      st.seen.push_back(r);
    }
    // 场景 8：HEAD 与 GET 各记一份 `on_sent`。HEAD 是被框架映射到这条 GET 路由
    // 上的（`head_as_get_`），所以这里按方法分岔。
    const bool head_req = (req.method() == http_method::HTTP_HEAD);
    resp.on_sent([&st, head_req](const uvcpp_web_sent_info& i) {
      if (head_req) {
        st.hello_sent_head.fetch_add(1);
        st.hello_bytes_head.store(static_cast<long long>(i.body_bytes));
      } else {
        st.hello_sent_get.fetch_add(1);
        st.hello_bytes_get.store(static_cast<long long>(i.body_bytes));
      }
    });
    resp.text(kHelloBody);
    resp.end();
  });

  // 带路径参数的路由：证明 h2 上走的是**同一套**路由（模板解得出参数），而不是
  // "h2 有一条自己的、只会精确匹配的旁路"。
  app.get("/echo/:what", [&st](uvcpp_web_request& req, uvcpp_web_response& resp,
                               uvcpp_web_next next) {
    (void)next;
    seen_request r;
    r.url       = req.path();
    r.param     = param_of(req, "what");
    r.stream_id = req.raw().stream_id;
    {
      std::lock_guard<std::mutex> lk(st.mu);
      st.seen.push_back(r);
    }
    resp.text(kParamEchoPre + r.param);
    resp.end();
  });

  add_stream_routes(app, st);

  // 场景 4 才挂静态；路由表在 `start_background()` 之后不再变，所以另外几个场景
  // 共用这个函数时 `/pub` 是不存在的 —— 不去请求它就毫无影响。
  if (static_root != nullptr) app.serve_static("/pub", *static_root);

  const int rc = app.start_background();
  st.start_rc.store(rc);
  if (rc != 0) return rc;
  st.bound_port.store(app.bound_port());
  return 0;
}

std::vector<seen_request> snapshot_seen(app_probe& st) {
  std::lock_guard<std::mutex> lk(st.mu);
  return st.seen;
}

// =========================================================================
// 客户端：真 TLS + 真 ALPN + 真 h2
// =========================================================================

struct seen_response {
  int32_t     sid                   = 0;
  int         status                = 0;
  bool        end_stream_at_headers = false;
  bool        end                   = false;
  std::string body;
};

struct client_probe {
  std::atomic<int>  connect_status{-99};
  std::atomic<bool> connect_fired{false};
  std::atomic<int>  start_rc{-999};
  std::atomic<int>  fatal_count{0};

  std::mutex                     mu;
  std::string                    client_alpn;
  std::vector<int32_t>           submitted;
  std::vector<seen_response>     responses;
  std::map<int32_t, std::string> partial;
};

/// 一条 h2 客户端连接。
///
/// **成员的声明次序是有意的**（析构逆序）：
///   - `p` 先声明 ⇒ 最后析构：回调可能在客户端析构的过程中还被叫到，那会儿
///     它碰的正是 `p`；
///   - `conn` 在 `client` 之前声明 ⇒ 在 `client` 之后析构：h2 层把自己的读
///     回调挂在客户端上（`start()` 里的 `read_start_events`），客户端还没拆
///     干净就先删 h2 层，那之后的任何一次读都会进已释放的对象；
///   - `drain_` 最后声明 ⇒ 最先析构，在循环还活着的时候把收尾队列拨干净。
struct h2_client {
  client_probe                         p;
  std::unique_ptr<uvcpp_h2_connection> conn;
  uvcpp_tcp_client                     client;
  uvcpp_test::loop_drain               drain_;
  bool                                 began = false;

  h2_client() : drain_(client.get_loop()) {}

  uvcpp_loop* loop() { return client.get_loop(); }

  /// 客户端宣告的名单。两个场景用**同一份**（见 `kClientAlpn`）—— 这样
  /// "协商出什么"只由服务端配置决定，变量只有一个。
  bool begin(int port, uvcpp_ssl_context* cctx, const char* label) {
    const int trc = client.enable_tls(cctx);
    check(trc == 0, std::string(label) + ": enable_tls returned " +
                        std::to_string(trc));
    // enable_tls 之后设：握手要到 connect 完成回调里才起。
    check(client.set_tls_alpn_protos(kClientAlpn),
          std::string(label) + ": set_tls_alpn_protos failed");

    const int crc = client.connect("127.0.0.1", port, [this, label](int status) {
      p.connect_status.store(status);
      p.connect_fired.store(true);
      if (status != 0) return;
      {
        std::lock_guard<std::mutex> lk(p.mu);
        p.client_alpn = client.tls_alpn_selected();
      }
      // 协商结果不是 h2 就别建 h2 层 —— 场景 2 走的正是这条路（回落 h1）。
      if (client.tls_alpn_selected() != "h2") return;

      conn.reset(new uvcpp_h2_connection(&client, /*server_side=*/false));

      uvcpp_h2_session::callbacks h2c;
      h2c.on_response = [this](uvcpp_h2_session&, uvcpp_h2_stream& st,
                               bool end_stream) {
        std::lock_guard<std::mutex> lk(p.mu);
        seen_response& r        = open_response(st.stream_id);
        r.status                = static_cast<int>(st.response.status_code);
        r.end_stream_at_headers = end_stream;
      };
      h2c.on_body = [this](uvcpp_h2_session&, uvcpp_h2_stream& st, const char* d,
                           size_t n) {
        std::lock_guard<std::mutex> lk(p.mu);
        p.partial[st.stream_id].append(d, n);
      };
      h2c.on_response_end = [this](uvcpp_h2_session&, uvcpp_h2_stream& st) {
        std::lock_guard<std::mutex> lk(p.mu);
        seen_response& r = open_response(st.stream_id);
        r.body           = p.partial[st.stream_id];
        p.partial.erase(st.stream_id);
        r.end = true;
      };
      h2c.on_fatal = [this, label](uvcpp_h2_session&, int code) {
        std::cerr << "  [note] " << label << ": client h2 fatal " << code
                  << std::endl;
        p.fatal_count.fetch_add(1);
      };

      uvcpp_h2_connection::callbacks cc;
      cc.on_disconnect = [](uvcpp_h2_connection&) {};

      const int rv = conn->start(h2c, cc);
      p.start_rc.store(rv);
      check(rv == 0, std::string(label) + ": h2 conn start returned " +
                         std::to_string(rv));
      began = true;
    });
    check(crc == 0, std::string(label) + ": connect() start failed");
    return crc == 0;
  }

  /// 提交一个请求并把待发字节冲出去。`submit_request` 只把帧排进会话，
  /// `drain()` 才吐字节，而 `uvcpp_h2_connection` 只有响应侧的合并入口 ——
  /// 所以 `flush()` 必须自己调，忘了就是"请求提交了但一个字节都没发"。
  int32_t submit(http_method method, const std::string& url) {
    if (conn == nullptr) return -1;
    uvcpp_http_request req;
    req.method = method;
    req.url    = url;
    req.set_header("host", "loopback.test");
    const int32_t sid = conn->session().submit_request(req, std::string());
    if (sid <= 0) return sid;
    conn->flush();
    {
      std::lock_guard<std::mutex> lk(p.mu);
      p.submitted.push_back(sid);
    }
    return sid;
  }

  size_t response_count() {
    std::lock_guard<std::mutex> lk(p.mu);
    return p.responses.size();
  }

  bool wait_responses(size_t want) {
    return uvcpp_test::wait_until(
        loop(), [this, want] { return response_count() >= want; }, kWaitMs);
  }

  bool find_response(int32_t sid, seen_response& out) {
    std::lock_guard<std::mutex> lk(p.mu);
    for (size_t i = 0; i < p.responses.size(); ++i) {
      if (p.responses[i].sid == sid) {
        out = p.responses[i];
        return true;
      }
    }
    return false;
  }

  /// 等 `connect` 回调跑过。
  ///
  /// 判据必须是 `connect_fired` 本身，**不能**是"`client_alpn` 非空" ——
  /// 那是它的**推论**，而推论在协商结果为空时不成立（服务端没有共同协议，
  /// 或者压根没装 ALPN 回调）。拿推论当同步点，会把"握手成功但没协商出
  /// ALPN"错报成"握手根本没完成"，把诊断指向完全错误的方向。
  bool wait_connect() {
    return uvcpp_test::wait_until(
        loop(), [this] { return p.connect_fired.load(); }, kWaitMs);
  }

  std::string alpn() {
    std::lock_guard<std::mutex> lk(p.mu);
    return p.client_alpn;
  }

  /// 关连接并等关闭**完成**。关完之后再不拆 h2 层也不会漏掉什么 —— 真正的
  /// 拆除次序由成员声明次序保证（见本结构的说明）。
  bool finish() {
    bool closed = false;
    client.close([&closed] { closed = true; });
    const bool ok =
        uvcpp_test::wait_until(loop(), [&closed] { return closed; }, kWaitMs);
    uvcpp_test::pump_for(loop(), 60);
    return ok;
  }

 private:
  /// 找这条流的响应槽；没有就补一个，保持"按到达序"。
  seen_response& open_response(int32_t sid) {
    for (size_t i = 0; i < p.responses.size(); ++i) {
      if (p.responses[i].sid == sid) return p.responses[i];
    }
    seen_response r;
    r.sid = sid;
    p.responses.push_back(r);
    return p.responses.back();
  }
};

/// 客户端用的 TLS 上下文（自签证书 → 不校验，只为回环测试）。
std::shared_ptr<uvcpp_ssl_context> make_client_ctx() {
  std::shared_ptr<uvcpp_ssl_context> ctx(
      new uvcpp_ssl_context(tls_mode::CLIENT, tls_version::TLS_1_2));
  ctx->set_verify_mode(tls_verify_mode::NONE);
  return ctx->is_ready() ? ctx : std::shared_ptr<uvcpp_ssl_context>();
}

/// `/big` 的两条前置，缺一不可：
///
///   - **第一批字节到了**：少了它，"上下文归零"与"这条流压根没起来"在判据上
///     长得一模一样 —— 而后者永远归零；
///   - **还没整条收完**：少了它，一条已经跑完的流也会让判据通过，而那时服务端
///     队列里一个字节都没有，这条用例什么也没测到。
///
/// 第二半不是洁癖，是实测踩出来的：客户端这边 `uv__read` 是**循环读到 EAGAIN
/// 为止**的，而服务端在另一个线程上全速跑 —— 每开一次窗口就是一次完整的往返
/// （几十微秒），body 只有两个窗口那么长时，两次往返可以整个塞进**同一个**
/// `uv_run(NOWAIT)` 里。于是 `partial` 从填上到被 `on_response_end` 清掉，中间
/// 没有任何一次判据求值的机会 —— 用例静默地测了个寂寞。所以 body 做成 16 个
/// 窗口那么长（16 次往返塞进一圈的概率可以忽略），并且把"没收完"明写出来。
void dump_big_diag(const char* tag, h2_client& c, app_probe& st, int32_t sid);

/// `/big` 的两条前置，缺一不可：
///
///   - **第一批字节到了**：少了它，"上下文归零"与"这条流压根没起来"在判据上
///     长得一模一样 —— 而后者永远归零；
///   - **还没整条收完**：少了它，一条已经跑完的流也会让判据通过，而那时服务端
///     队列里一个字节都没有，这条用例什么也没测到。
///
/// 第二半不是洁癖，是实测踩出来的：客户端这边 `uv__read` 是**循环读到 EAGAIN
/// 为止**的，而服务端在另一个线程上全速跑 —— 每开一次窗口就是一次完整的往返
/// （几十微秒），body 只有两个窗口那么长时，两次往返可以整个塞进**同一个**
/// `uv_run(NOWAIT)` 里。于是 `partial` 从填上到被 `on_response_end` 清掉，中间
/// 没有任何一次判据求值的机会 —— 用例静默地测了个寂寞。所以 body 做成 16 个
/// 窗口那么长（16 次往返塞进一圈的概率可以忽略），并且把"没收完"明写出来。
bool big_started_and_open(h2_client& c, app_probe& st, int32_t sid,
                          const char* tag) {
  const bool started = uvcpp_test::wait_until(
      c.loop(),
      [&c, sid] {
        std::lock_guard<std::mutex> lk(c.p.mu);
        std::map<int32_t, std::string>::iterator it = c.p.partial.find(sid);
        return it != c.p.partial.end() && !it->second.empty();
      },
      kWaitMs);
  if (!started) {
    check(false, std::string(tag) + "[前置]: /big 的第一批字节在 " +
                     std::to_string(kWaitMs) + "ms 内没到");
    dump_big_diag(tag, c, st, sid);
    return false;
  }
  seen_response r;
  if (c.find_response(sid, r) && r.end) {
    check(false, std::string(tag) +
                     "[前置]: /big 在作废之前就整条收完了 —— 服务端队列里已经"
                     "没有可作废的块，这条用例什么也没测到");
    dump_big_diag(tag, c, st, sid);
    return false;
  }
  return true;
}

/// "第一批字节没到"这条前置失败时，把两侧的状态各自报一遍 —— 否则"服务端没跑
/// 那条路由"和"跑了但字节没出去"在判据上长得一模一样。
void dump_big_diag(const char* tag, h2_client& c, app_probe& st, int32_t sid) {
  std::cerr << "  [diag] " << tag << ": /big 处理函数跑了 " << st.big_count.load()
            << " 次；客户端 ALPN=\"" << c.alpn() << "\" start_rc="
            << c.p.start_rc.load() << " fatal=" << c.p.fatal_count.load()
            << " 响应条数=" << c.response_count() << std::endl;
  seen_response rd;
  if (c.find_response(sid, rd)) {
    std::cerr << "  [diag] " << tag << ": 流 " << sid << " status=" << rd.status
              << " end=" << rd.end << " body=" << rd.body.size() << " 字节"
              << std::endl;
  } else {
    std::cerr << "  [diag] " << tag << ": 流 " << sid << " 没有任何响应"
              << std::endl;
  }
  {
    std::lock_guard<std::mutex> lk(c.p.mu);
    std::cerr << "  [diag] " << tag << ": 已提交流数=" << c.p.submitted.size()
              << " partial 条目=" << c.p.partial.size() << std::endl;
  }
}

/// h2 上跑两条请求（一条精确匹配、一条带路径参数），两条都要对号入座。
void run_h2_requests(h2_client& c, app_probe& st, const char* tag) {
  const int32_t s1 = c.submit(http_method::HTTP_GET, "/hello");
  check(s1 > 0, std::string(tag) + ": /hello 没提交上");
  const int32_t s2 = c.submit(http_method::HTTP_GET, "/echo/abc");
  check(s2 > 0, std::string(tag) + ": /echo/abc 没提交上");

  check(c.wait_responses(2), std::string(tag) + ": 两条响应没在 " +
                                 std::to_string(kWaitMs) + "ms 内收齐");
  uvcpp_test::pump_for(c.loop(), 80);

  seen_response r1;
  const bool has1 = c.find_response(s1, r1);
  check(has1, std::string(tag) + ": 流 " + std::to_string(s1) + " 没有响应");
  if (has1) {
    check(r1.end, std::string(tag) + ": /hello 的响应没收完");
    check(r1.status == 200,
          std::string(tag) + ": /hello 状态 " + std::to_string(r1.status));
    check(r1.body == kHelloBody,
          std::string(tag) + ": /hello body = \"" + r1.body + "\"");
  }

  seen_response r2;
  const bool has2 = c.find_response(s2, r2);
  check(has2, std::string(tag) + ": 流 " + std::to_string(s2) + " 没有响应");
  if (has2) {
    check(r2.end, std::string(tag) + ": /echo 的响应没收完");
    check(r2.status == 200,
          std::string(tag) + ": /echo 状态 " + std::to_string(r2.status));
    // **路由模板真的解出了参数** —— 这条把"h2 有一条只会精确匹配的旁路"与
    // "h2 走的是同一套路由"分开。
    check(r2.body == std::string(kParamEchoPre) + "abc",
          std::string(tag) + ": /echo/abc body = \"" + r2.body + "\"");
  }

  // 服务端侧同号对座：框架处理函数看到的 `stream_id` 必须就是客户端发出去的那个。
  // 少了这一半，"响应体对"可能只是路由恰好命中，而流身份其实是错的。
  // 对不上时必须**自报家门**：把两边看到的流号都打出来。只报"没匹配上"
  // 会让"框架收到的流号是 0"和"流号对但路由串了"看起来一样。
  std::vector<seen_request> seen = snapshot_seen(st);
  bool saw1 = false, saw2 = false;
  for (size_t i = 0; i < seen.size(); ++i) {
    if (seen[i].stream_id == s1 && seen[i].url == "/hello") saw1 = true;
    if (seen[i].stream_id == s2 && seen[i].param == "abc") saw2 = true;
  }
  if (!saw1 || !saw2) {
    std::cerr << "  [diag] " << tag << ": 客户端提交 s1=" << s1 << " s2=" << s2
              << "；服务端 handler 看到 " << seen.size() << " 条:";
    for (size_t i = 0; i < seen.size(); ++i) {
      std::cerr << " {sid=" << seen[i].stream_id << " url=" << seen[i].url
                << " param=" << seen[i].param << "}";
    }
    std::cerr << std::endl;
  }
  check(saw1, std::string(tag) + ": 服务端没有在流 " + std::to_string(s1) +
                  " 上看到 /hello（框架拿到的 stream_id 不对？）");
  check(saw2, std::string(tag) + ": 服务端没有在流 " + std::to_string(s2) +
                  " 上解出 param=abc");
}

}  // namespace

int main() {
  std::cout << "[web_ssl_app_h2] OpenSSL "
            << OpenSSL_version(OPENSSL_VERSION_STRING) << std::endl;

  std::shared_ptr<uvcpp_ssl_context> cctx = make_client_ctx();
  if (!cctx) {
    std::cerr << "  [FAIL] 客户端 TLS 上下文建不出来" << std::endl;
    return 2;
  }

  // =====================================================================
  // 场景 1：**默认**就支持 h2 —— 一次都不碰 h2 相关的 API
  // =====================================================================
  {
    uvcpp_web_app app;
    app_probe      st;
    // `set_flag = false`：连 `set_http2_enabled(true)` 都不调。默认值必须自己
    // 站得住 —— 这正是"框架默认支持、用户只管关"那条约定的一半。
    if (build_app(app, st, /*set_flag=*/false, /*enable_h2=*/true) != 0) {
      std::cerr << "  [FAIL] 场景 1：app 起不来 (rc=" << st.start_rc.load()
                << ")" << std::endl;
      return 2;
    }
    check(st.h2_flag.load() == 1,
          "h2_default: 框架默认 `http2_enabled()` 读到 false —— 默认值反了");

    h2_client c;
    if (c.begin(st.bound_port.load(), cctx.get(), "h2_default")) {
      check(c.wait_connect(), "h2_default: connect 回调没等到");
      const std::string alpn = c.alpn();
      check(alpn == "h2",
            "h2_default: 客户端拿到 ALPN \"" + alpn +
                "\" —— 框架没有默认宣告 h2（服务端名单里没有 h2？）");
      check(c.began, "h2_default: h2 层没起来（ALPN 是 \"" + alpn + "\"）");
      if (c.began) run_h2_requests(c, st, "h2_default");
      check(c.finish(), "h2_default: client close never completed");
    } else {
      check(false, "h2_default: connect() 就没起来");
    }
    app.stop();
  }

  // =====================================================================
  // 场景 2：`set_http2_enabled(false)` 退到纯 HTTP/1.1
  // =====================================================================
  //
  // 客户端**照样宣告 h2**：要证的是"服务端拒绝了 h2"，而不是"客户端没要"。
  {
    uvcpp_web_app app;
    app_probe      st;
    if (build_app(app, st, /*set_flag=*/true, /*enable_h2=*/false) != 0) {
      std::cerr << "  [FAIL] 场景 2：app 起不来 (rc=" << st.start_rc.load()
                << ")" << std::endl;
      return 2;
    }
    check(st.h2_flag.load() == 0,
          "h2_off: set_http2_enabled(false) 之后 `http2_enabled()` 仍是 true");

    h2_client c;
    if (c.begin(st.bound_port.load(), cctx.get(), "h2_off")) {
      check(c.wait_connect(), "h2_off: connect 回调没等到");
      const std::string alpn = c.alpn();
      check(alpn == "http/1.1",
            "h2_off: 客户端拿到 ALPN \"" + alpn +
                "\" —— 关掉 h2 之后服务端还在宣告它");
      check(!c.began, "h2_off: 协商不是 h2 却仍然建起了 h2 层");

      // **降级必须是能用的，而不只是协商结果不同。** 同一条 TLS 连接上发一个
      // 普通 HTTP/1.1 请求，框架必须照常路由、照常回。
      std::string got;
      std::mutex  gm;
      // 先挂读再写：响应可能比 `read_start_events` 还快。
      const int rrc = c.client.read_start_events(
          [&got, &gm](uvcpp_tcp_client&, const net_read_result& r) {
            if (!r.is_data()) return;
            std::lock_guard<std::mutex> lk(gm);
            got.append(r.data, r.size);
          });
      check(rrc == 0, "h2_off: read_start_events returned " +
                          std::to_string(rrc));

      const std::string req =
          "GET /hello HTTP/1.1\r\nHost: loopback.test\r\n"
          "Connection: close\r\n\r\n";
      const int wrc = c.client.write(req.data(), req.size(), [](int) {});
      check(wrc == 0, "h2_off: h1 请求没写出去 (" + std::to_string(wrc) + ")");

      uvcpp_test::wait_until(
          c.loop(),
          [&got, &gm] {
            std::lock_guard<std::mutex> lk(gm);
            return got.find(kHelloBody) != std::string::npos;
          },
          kWaitMs);
      {
        std::lock_guard<std::mutex> lk(gm);
        check(got.find(" 200 ") != std::string::npos,
              "h2_off: 回落之后没有拿到 200，收到: " + got.substr(0, 120));
        check(got.find(kHelloBody) != std::string::npos,
              "h2_off: 回落之后响应体不对，收到: " + got.substr(0, 200));
      }
      c.finish();
    } else {
      check(false, "h2_off: connect() 就没起来");
    }
    app.stop();
  }

  // =====================================================================
  // 场景 3：流式响应走 h2
  // =====================================================================
  //
  // 两件事，缺一个就漏掉一半：
  //
  //   3a. **组帧**。h1 的流式响应在线上是 chunked 帧（`6\r\ndata: 1\n\n\r\n`
  //       …`0\r\n\r\n`），h2 上**没有这层帧** —— DATA 帧由会话层组，框架这一
  //       层该给的是裸字节。判据因此是"收到的 body 逐字节等于三块载荷拼起来"，
  //       而不是"包含"：`包含` 对 `6\r\ndata: 1\n\n\r\n…` 也成立。
  //
  //   3b. **一条慢流不许扣住别的流**。`/hold` 发完头部就挂住不收尾，随后提交的
  //       `/plain` 必须**照常在它之前回完**。这条钉的是响应顺序闸门（h1 的
  //       流水线要求"只有队首能发"）在 h2 上被绕过 —— 闸门没绕过的话，`/plain`
  //       会一直排到 `/hold` 收尾为止，而这个时长由测试决定，于是表现为超时。
  {
    uvcpp_web_app app;
    app_probe      st;
    if (build_app(app, st, /*set_flag=*/false, /*enable_h2=*/true) != 0) {
      std::cerr << "  [FAIL] 场景 3：app 起不来 (rc=" << st.start_rc.load()
                << ")" << std::endl;
      return 2;
    }

    h2_client c;
    if (c.begin(st.bound_port.load(), cctx.get(), "h2_stream")) {
      check(c.wait_connect(), "h2_stream: connect 回调没等到");
      check(c.began, "h2_stream: h2 层没起来");

      if (c.began) {
        // --- 3a：整包流式 ---
        const int32_t sse = c.submit(http_method::HTTP_GET, "/sse");
        check(sse > 0, "h2_stream: /sse 没提交上");
        check(c.wait_responses(1), "h2_stream: /sse 的响应没在 " +
                                       std::to_string(kWaitMs) + "ms 内收到");
        uvcpp_test::pump_for(c.loop(), 80);

        seen_response rs;
        const bool has_sse = c.find_response(sse, rs);
        check(has_sse, "h2_stream: 流 " + std::to_string(sse) + " 没有响应");
        if (has_sse) {
          check(rs.status == 200, "h2_stream: /sse 状态 " +
                                      std::to_string(rs.status));
          check(rs.end, "h2_stream: /sse 的响应没收完（END_STREAM 没到）");
          const std::string want = std::string(kSse1) + kSse2 + kSse3;
          check(rs.body == want,
                "h2_stream: /sse body = \"" + rs.body + "\"，期望 \"" + want +
                    "\" —— h2 上不该有 chunked 帧（hex 长度 / CRLF / 终止块）");
          // 单独再点一次终止块：上面那条失败时，它把"少了终止块"和"组了帧"分成
          // 两件可读的事。
          check(rs.body.find("0\r\n\r\n") == std::string::npos,
                "h2_stream: /sse 的 body 里出现了 chunked 终止块");
        }

        // --- 3b：慢流不许扣住别的流 ---
        const int32_t slow = c.submit(http_method::HTTP_GET, "/hold");
        check(slow > 0, "h2_stream: /hold 没提交上");
        // **前置**：`/hold` 自己得先发出去。少了它，"`/plain` 没被挡住"与
        // "`/hold` 压根没提交成功"在后面的断言里长得一样。
        check(c.wait_responses(1), "h2_stream: /hold 的头部没在 " +
                                       std::to_string(kWaitMs) + "ms 内到达");

        const int32_t fast = c.submit(http_method::HTTP_GET, "/plain");
        check(fast > 0, "h2_stream: /plain 没提交上");

        seen_response rf;
        const bool got_fast = uvcpp_test::wait_until(
            c.loop(),
            [&c, fast, &rf] {
              return c.find_response(fast, rf) && rf.end;
            },
            kWaitMs);
        if (!got_fast) {
          std::cerr << "  [diag] h2_stream: /hold(流 " << slow
                    << ") 还没收尾，/plain(流 " << fast << ") 在 "
                    << kWaitMs << "ms 内没回完；已收到 " << c.response_count()
                    << " 条响应" << std::endl;
        }
        check(got_fast,
              "h2_stream: /plain 被未收尾的 /hold 挡住了（h2 的响应顺序闸门"
              "没绕过？）");
        check(rf.body == kHelloBody,
              "h2_stream: /plain body = \"" + rf.body + "\"");

        // --- 收尾：让 `/hold` 结束，整条流必须完整地走完 ---
        app.post([&st]() {
          uvcpp_web_response* rp = st.held.load();
          if (rp == nullptr) return;
          st.held_ended.store(1);
          rp->end();
        });
        seen_response rh;
        const bool got_hold = uvcpp_test::wait_until(
            c.loop(),
            [&c, slow, &rh] {
              return c.find_response(slow, rh) && rh.end;
            },
            kWaitMs);
        check(got_hold, "h2_stream: 收尾之后 /hold 仍然没收完");
        check(st.held_ended.load() == 1,
              "h2_stream: 收尾那一句根本没跑（handler 没存下响应对象？）");
        if (got_hold) {
          check(rh.body == kSse1,
                "h2_stream: /hold body = \"" + rh.body + "\"，期望 \"" + kSse1 +
                    "\"");
        }
      }
      check(c.finish(), "h2_stream: client close never completed");
    } else {
      check(false, "h2_stream: connect() 就没起来");
    }

    // 三条流全部收尾之后框架必须把上下文放干净（`/hold` 那条走过 `hold()`）。
    //
    // **不能泵 app 的循环**：它在后台线程上跑着，测试线程再 `run()` 一次就是
    // 两个线程同时驱动同一个 loop。传空循环 = 只等，不泵 —— 服务端的事由它
    // 自己那个线程推进。显式写类型是为了避开 `uv_loop_t*` 那个重载。
    uvcpp_loop* const none = nullptr;
    check(uvcpp_test::wait_until(
              none, [&app] { return app.inflight_count() == 0; }, kWaitMs),
          "h2_stream: 三条流都收完了，在途上下文没有归零（实测 " +
              std::to_string(app.inflight_count()) + "）");
    app.stop();
  }

  // =====================================================================
  // 场景 4：h2 上的静态服务与**目录穿越**
  // =====================================================================
  //
  // 计划里点是名要做的一条：现有防护"看着走同一套"不算数，必须在 h2 上实测。
  // 这条路确实和 h1 不同 —— `:path` 由**对端**原样送来，没有 llhttp 那层
  // 请求行解析（`req.url = :path` 是逐字节直通），所以"h1 那层顺带做掉的
  // 归一化"在 h2 上不存在。
  //
  // **前置断言不能省**（A 段）：没有它，"穿越被挡住"和"静态在 h2 上压根不工作"
  // 在下面的判据里长得一模一样 —— 后者会让整个场景假绿。
  {
    static_fixture fx;
    const bool     fixture_ok = fx.make() && fx.on_disk_ok();
    check(fixture_ok,
          "h2_static: 临时目录/文件没建出来（" + fx.root + " / " + fx.secret + "）");
    if (fixture_ok) {
      uvcpp_web_app app;
      app_probe      st;
      if (build_app(app, st, /*set_flag=*/false, /*enable_h2=*/true,
                    &fx.root) != 0) {
        std::cerr << "  [FAIL] 场景 4：app 起不来 (rc=" << st.start_rc.load()
                  << ")" << std::endl;
        return 2;
      }

      h2_client c;
      if (c.begin(st.bound_port.load(), cctx.get(), "h2_static")) {
        check(c.wait_connect(), "h2_static: connect 回调没等到");
        check(c.began, "h2_static: h2 层没起来");

        if (c.began) {
          // --- A：前置 —— 静态服务在 h2 上真的工作 ---
          const int32_t ok_sid = c.submit(http_method::HTTP_GET, "/pub/ok.txt");
          check(ok_sid > 0, "h2_static: /pub/ok.txt 没提交上");

          // **按流号等，不按"响应条数 +1"等。** 前者是这一条流自己的事实；后者
          // 在"响应比这一行跑得还快"时算出来的目标永远追不上（回环 + 小文件，
          // 这不是罕见时序而是常态），于是一个正确的实现被报成超时。
          seen_response rok;
          const bool has_ok = uvcpp_test::wait_until(
              c.loop(),
              [&c, ok_sid, &rok] {
                return c.find_response(ok_sid, rok) && rok.end;
              },
              kWaitMs);
          if (!has_ok) {
            std::cerr << "  [diag] h2_static: 流 " << ok_sid
                      << " 在 " << kWaitMs << "ms 内没回完；已收到 "
                      << c.response_count() << " 条响应" << std::endl;
          }
          check(has_ok, "h2_static: 流 " + std::to_string(ok_sid) + " 没有响应");
          if (has_ok) {
            // 这一条失败就说明"静态没接上 h2"，下面两条穿越判据随之失去意义。
            check(rok.status == 200 && rok.body == kStaticBody,
                  "h2_static[前置]: /pub/ok.txt 拿到 status=" +
                      std::to_string(rok.status) + " body=\"" + rok.body +
                      "\" —— 静态服务根本没跑在 h2 上，穿越用例无意义");
          }

          // --- B/C：两种穿越写法，一条都不许漏 ---
          const char* kEvil[] = {"/pub/../uvcpp_h2_static_secret.txt",
                                 "/pub/%2e%2e/uvcpp_h2_static_secret.txt"};
          for (int i = 0; i < 2; ++i) {
            const std::string url = kEvil[i];
            const int32_t sid     = c.submit(http_method::HTTP_GET, url);
            const std::string tag = "h2_static[trav " + std::to_string(i) + "] " + url;
            check(sid > 0, tag + " 没提交上");
            if (sid <= 0) continue;

            seen_response r;
            const bool got = uvcpp_test::wait_until(
                c.loop(), [&c, sid, &r] { return c.find_response(sid, r) && r.end; },
                kWaitMs);
            if (!got) {
              std::cerr << "  [diag] " << tag << ": 流 " << sid << " 在 "
                        << kWaitMs << "ms 内没回完；已收到 " << c.response_count()
                        << " 条响应" << std::endl;
            }
            if (!got || !c.find_response(sid, r)) {
              // **没收到响应不等于挡住了穿越** —— 这一条必须自己说出来，
              // 否则它会伪装成"防护生效"溜过去。
              check(false, tag + " 没有响应（超时/连接断了都不算挡住，得看状态码）");
              continue;
            }
            // **判据是"秘密没漏"，不是"状态码是 404"。** 归一化成 404、在静态层
            // 被挡成 403、被路由当不存在 —— 哪种都对；唯一不能接受的是把根目录
            // 外面的字节送出去。
            check(r.body.find(kSecretBody) == std::string::npos,
                  tag + " 把根目录外面的文件发出去了！status=" +
                      std::to_string(r.status) + " body=\"" + r.body + "\"");
            check(r.status != 200,
                  tag + " 回了 200（status=" + std::to_string(r.status) +
                      " body=\"" + r.body + "\"）");
          }
        }
        check(c.finish(), "h2_static: client close never completed");
      } else {
        check(false, "h2_static: connect() 就没起来");
      }
      app.stop();
    }
    std::remove(fx.secret.c_str());
    std::remove((fx.root + "/ok.txt").c_str());
  }

  // =====================================================================
  // 场景 5：被 RST 掉的流式响应，框架的上下文必须照样放干净
  // =====================================================================
  //
  // 这是"每一块的 `done` 恰好跑一次"那条契约在**收方向**上的落点。
  //
  // 服务端一次写出 128 KiB，而 h2 默认的连接级流控窗口只有 65535 —— 超出的
  // 部分排在服务端的发送队列里，客户端不发 WINDOW_UPDATE 就永远发不出去。
  // 客户端这时 RST 掉这条流，那些排队的块就整体作废，它们的 `done` 必须被
  // 结算（`UV_ECANCELED`）。不结算的话 `uvcpp_web_response::pending_bytes_`
  // 永远减不回 0，`stream_finish_ready()` 恒假，这条流的上下文**永远不释放**
  // —— 停机时 `inflight_` 排不空，宽限期白等满。
  //
  // 判据只有一条，但它是端到端的：`inflight_count()` 必须归零。
  {
    uvcpp_web_app app;
    app_probe      st;
    if (build_app(app, st, /*set_flag=*/false, /*enable_h2=*/true) != 0) {
      std::cerr << "  [FAIL] 场景 5：app 起不来 (rc=" << st.start_rc.load()
                << ")" << std::endl;
      return 2;
    }

    h2_client c;
    if (c.begin(st.bound_port.load(), cctx.get(), "h2_rst")) {
      check(c.wait_connect(), "h2_rst: connect 回调没等到");
      check(c.began, "h2_rst: h2 层没起来");

      if (c.began) {
        const int32_t sid = c.submit(http_method::HTTP_GET, "/big");
        check(sid > 0, "h2_rst: /big 没提交上");

        // **前置**：这条流起来了、**而且还没收完**。见 `big_started_and_open`
        // 上面那段 —— 少了它，"上下文归零"与"这条流压根没起来 / 早就跑完了"
        // 在下面的判据里长得一模一样，而后者永远归零。
        if (big_started_and_open(c, st, sid, "h2_rst")) {
          // 8 = CANCEL。此刻服务端至少还有 64 KiB 被窗口挡在队列里。
          const int rrc = c.conn->session().submit_rst(sid, 8 /* CANCEL */);
          check(rrc == 0, "h2_rst: submit_rst returned " + std::to_string(rrc));
          c.conn->flush();

          // 服务端在**另一个线程**上跑，所以这里只等不泵（传空循环）。
          uvcpp_loop* const none = nullptr;
          const bool drained = uvcpp_test::wait_until(
              none, [&app] { return app.inflight_count() == 0; }, kWaitMs);
          check(drained,
                "h2_rst: RST 之后在途上下文没有归零（实测 " +
                    std::to_string(app.inflight_count()) +
                    "）—— 被作废的那些块的 done 没人跑，"
                    "`pending_bytes_` 减不回 0，这条流永远不收尾");
        }
      }
      c.finish();
    } else {
      check(false, "h2_rst: connect() 就没起来");
    }
    app.stop();
  }

  // =====================================================================
  // 场景 6：框架停机时，h2 连接必须**道别**，不能只看到连接断了
  // =====================================================================
  //
  // 停机时"发 GOAWAY"和"关连接"的分工是场景的全部内容：前者排在停机第 0 拍，
  // 后者在第 1 拍（中间隔一拍排水）。少了第 0 拍，对端拿到的就是一条被断开的
  // 连接 —— 与拔网线长得一模一样，在飞的请求只能一律按"结果未知"处理，而
  // GOAWAY 里的 `last_stream_id` 本来能告诉它哪几条可以安全重试。
  //
  // 判据落在客户端侧：**收到了 GOAWAY**（而不是"连接断了"）+ 三个字段都对 +
  // 新流被拒。第三条尤其重要 —— 它证明 GOAWAY 是被**解析**过的，而不是仅仅
  // 有几个字节到过。
  {
    uvcpp_web_app app;
    app_probe      st;
    if (build_app(app, st, /*set_flag=*/false, /*enable_h2=*/true) != 0) {
      std::cerr << "  [FAIL] 场景 6：app 起不来 (rc=" << st.start_rc.load()
                << ")" << std::endl;
      return 2;
    }

    h2_client c;
    if (c.begin(st.bound_port.load(), cctx.get(), "h2_bye")) {
      check(c.wait_connect(), "h2_bye: connect 回调没等到");
      check(c.began, "h2_bye: h2 层没起来");

      if (c.began) {
        const int32_t s1 = c.submit(http_method::HTTP_GET, "/hello");
        check(s1 > 0, "h2_bye: /hello 没提交上");

        // **前置**：先有一条真正跑完的流。没有它，`last_stream_id` 该是多少
        // 就没有基准，而"连接空着时收到 GOAWAY"和"跑过一条之后收到"在下面
        // 的判据里长得一样。
        seen_response r1;
        const bool done1 = uvcpp_test::wait_until(
            c.loop(), [&c, s1, &r1] { return c.find_response(s1, r1) && r1.end; },
            kWaitMs);
        check(done1, "h2_bye[前置]: /hello 在 " + std::to_string(kWaitMs) +
                         "ms 内没跑完");
        if (done1) {
          check(r1.status == 200, "h2_bye[前置]: /hello 回了 " +
                                      std::to_string(r1.status));
        }

        // 停机是**另一个线程**上的事，这里只等不碰它的状态。
        app.stop();

        // GOAWAY 在第 0 拍发出，第 1 拍（隔 10ms）就关连接 —— 所以这个等待
        // 窗口很短，等到就必须是"数据先到"，不能靠重试。
        const bool goodbye = uvcpp_test::wait_until(
            c.loop(),
            [&c] {
              return c.conn != nullptr &&
                     c.conn->session().peer_goaway_received();
            },
            kWaitMs);
        check(goodbye,
              "h2_bye: 停机后没收到 GOAWAY —— 对端只看到连接断了，"
              "分不清这是停机还是断线");

        if (goodbye) {
          check(c.conn->session().peer_goaway_error_code() == 0,
                "h2_bye: GOAWAY 错误码是 " +
                    std::to_string(c.conn->session().peer_goaway_error_code()) +
                    "（正常停机该是 0/NO_ERROR）");
          // 这一条同时钉住"服务端填的是**已处理**的最大流号"：填 0 或者
          // 填一条我们没发过的号，对端就没法区分"没处理"和"处理完了"。
          check(c.conn->session().peer_goaway_last_stream_id() == s1,
                "h2_bye: GOAWAY 的 last_stream_id 是 " +
                    std::to_string(
                        c.conn->session().peer_goaway_last_stream_id()) +
                    "，期望 " + std::to_string(s1));
          check(static_cast<int32_t>(c.submit(http_method::HTTP_GET, "/hello")) ==
                    UV_ENOTCONN,
                "h2_bye: GOAWAY 之后还能开新流 —— 说明它只是被收到了，"
                "没进到会话状态里");
        }

        // 连接随后要真的被服务端关掉（第 1 拍）。这一条是"排水那一拍真的存在"
        // 的落点：如果服务端把 GOAWAY 和 `uv_close` 挤在同一拍里，上面那条
        // 断言就会在**某些机器上**偶尔失败 —— 而现在它是确定的。
        const bool closed = uvcpp_test::wait_until(
            c.loop(), [&c] { return c.conn != nullptr && c.conn->closed(); },
            kWaitMs);
        check(closed, "h2_bye: 服务端停机后连接没有关掉");
      }
      c.finish();
    } else {
      check(false, "h2_bye: connect() 就没起来");
    }
    app.stop();
  }

  // =====================================================================
  // 场景 7：**整条连接**被丢掉（不是 RST 单条流），队列里的块照样要结算
  // =====================================================================
  //
  // 场景 5 走的是"对端 RST 掉这条流"这条路 —— 会话层收到 RST 就会把那条流
  // 还没上线的块整体作废。这一条走**另一条路**：对端什么都不说，直接把连接
  // 扔掉。此时会话层收不到任何帧（`recv()` 压根不会被叫到），那些块是
  // **传输层**拆掉的 —— 结算它们的责任在 `remove_ctx`。
  //
  // 少了那一句，`done` 一辈子不响：`pending_bytes_` 减不回 0，上下文永远扣在
  // `inflight_` 里，停机时要白等满整个宽限期（场景 5 的注释里说的同一件事，
  // 只是触发口不同）。所以判据还是那一条：`inflight_count()` 必须归零。
  {
    uvcpp_web_app app;
    app_probe      st;
    if (build_app(app, st, /*set_flag=*/false, /*enable_h2=*/true) != 0) {
      std::cerr << "  [FAIL] 场景 7：app 起不来 (rc=" << st.start_rc.load()
                << ")" << std::endl;
      return 2;
    }

    h2_client c;
    if (c.begin(st.bound_port.load(), cctx.get(), "h2_drop")) {
      check(c.wait_connect(), "h2_drop: connect 回调没等到");
      check(c.began, "h2_drop: h2 层没起来");

      if (c.began) {
        const int32_t sid = c.submit(http_method::HTTP_GET, "/big");
        check(sid > 0, "h2_drop: /big 没提交上");

        // **前置**：这条流起来了、**而且还没收完**（见 `big_started_and_open`）。
        // 少了后半句，一条已经跑完的流同样能让下面"归零"的判据通过 —— 而那时
        // 服务端队列里一个字节都没有，这条用例什么也没测到。
        const bool open = big_started_and_open(c, st, sid, "h2_drop");
        if (open) {
          // 再钉一层：此刻这条流确实还扣在框架的 `inflight_` 里。
          check(app.inflight_count() >= 1,
                "h2_drop[前置]: /big 还没跑完，inflight_count() 却是 " +
                    std::to_string(app.inflight_count()));
        }

        // 整个连接扔掉：不发 RST、不说再见。**前置没成立也一样扔** —— 留着
        // 这条连接只会让后面那次 `app.stop()` 白等满宽限期。
        check(c.finish(), "h2_drop: client close never completed");

        if (open) {
          // 服务端在**另一个线程**上跑，所以这里只等不泵（传空循环）。
          uvcpp_loop* const none = nullptr;
          const bool drained = uvcpp_test::wait_until(
              none, [&app] { return app.inflight_count() == 0; }, kWaitMs);
          check(drained,
                "h2_drop: 连接被丢掉之后在途上下文没有归零（实测 " +
                    std::to_string(app.inflight_count()) +
                    "）—— 队列里那些块的 done 没人跑，`pending_bytes_` "
                    "减不回 0，这条流永远不收尾");
        }
      } else {
        c.finish();
      }
    } else {
      check(false, "h2_drop: connect() 就没起来");
    }
    app.stop();
  }

  // =====================================================================
  // 场景 8：h2 上 HEAD 的 `body_bytes` 必须是 0
  //
  // 这条腿**只能在 h2 上验**：h1 那条路上 `uvcpp_http_server` 自己会
  // `resp.body.clear()`（按压缩后的长度钉完 content-length 之后），到了采集点
  // body 已经是空的 —— 框架层那个换算写不写都一样。h2 走的是 `omit_body`，
  // body 一直留着，只有框架层的换算能把它记成 0。
  // =====================================================================
  {
    uvcpp_web_app app;
    app_probe      st;
    if (build_app(app, st, /*set_flag=*/false, /*enable_h2=*/true) != 0) {
      std::cerr << "  [FAIL] 场景 8：app 起不来 (rc=" << st.start_rc.load()
                << ")" << std::endl;
      return 2;
    }

    h2_client c;
    if (c.begin(st.bound_port.load(), cctx.get(), "h2_head_bytes")) {
      check(c.wait_connect(), "h2_head_bytes: connect 回调没等到");
      check(c.began, "h2_head_bytes: h2 层没起来");
      if (c.began) {
        const int32_t g = c.submit(http_method::HTTP_GET, "/hello");
        check(g > 0,
              "h2_head_bytes: GET 提交失败 (sid=" + std::to_string(g) + ")");
        check(c.wait_responses(1), "h2_head_bytes: GET 的响应没到");

        const int32_t h = c.submit(http_method::HTTP_HEAD, "/hello");
        check(h > 0,
              "h2_head_bytes: HEAD 提交失败 (sid=" + std::to_string(h) + ")");
        check(c.wait_responses(2), "h2_head_bytes: HEAD 的响应没到");

        // 前置：两个回调都真的来过。没有这两条，下面的断言在"处理函数压根没跑"
        // 时也会绿 —— 那测的是别的东西。
        check(st.hello_sent_get.load() == 1,
              "h2_head_bytes: GET 的 on_sent 被触发了 " +
                  std::to_string(st.hello_sent_get.load()) + " 次（应为 1）");
        check(st.hello_sent_head.load() == 1,
              "h2_head_bytes: HEAD 的 on_sent 被触发了 " +
                  std::to_string(st.hello_sent_head.load()) + " 次（应为 1）");

        check(st.hello_bytes_get.load() ==
                  static_cast<long long>(sizeof(kHelloBody) - 1),
              "h2_head_bytes: GET 的 body_bytes 是 " +
                  std::to_string(st.hello_bytes_get.load()) + "，应为 " +
                  std::to_string(sizeof(kHelloBody) - 1));
        check(st.hello_bytes_head.load() == 0,
              "h2_head_bytes: HEAD 的 body_bytes 是 " +
                  std::to_string(st.hello_bytes_head.load()) +
                  "，应为 0 —— h2 那条路不清 body，只有框架层的换算能记成 0");
      }
      check(c.finish(), "h2_head_bytes: client close never completed");
    } else {
      check(false, "h2_head_bytes: connect() 就没起来");
    }
    app.stop();
  }

  if (g_failures != 0) {
    std::cerr << "[web_ssl_app_h2] " << g_failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "[web_ssl_app_h2] all scenarios OK" << std::endl;
  return 0;
}

#else

int main() {
  std::cout << "[web_ssl_app_h2] skipped (needs UVCPP_WEBAPP_ENABLE && "
               "UVCPP_OPENSSL_ENABLE && UVCPP_NGHTTP2_ENABLE)"
            << std::endl;
  return 0;
}

#endif
