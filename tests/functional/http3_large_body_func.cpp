/**
 * @file tests/functional/http3_large_body_func.cpp
 * @brief 大响应体的 h3 端到端逐字节校验 —— 打的是"一个包里装几条流"那条路。
 * @author zhuweiye
 * @version 1.7.2
 *
 * **这条用例补的是一个真实的洞。** 在此之前，本仓九个 h3/quic 用例的响应体
 * 全是 `"hello-h3"` / `"path=/a"` 这种一个 chunk 都装不满的串，而发送侧
 * （`quic_session::do_flush()`）在 1.7.2 引入了 ngtcp2 官方的手法
 * `NGTCP2_WRITE_STREAM_FLAG_MORE` —— 让一个 QUIC 数据报里装**多条流**的
 * STREAM 帧。开了它之后，库会把一个包**攥着不封口**，跨很多轮往里加帧，
 * 于是 `sent_off` 的每一步记账都落在"包还开着"那一支上，FIN 也落在
 * "最后一段刚好编完"那一轮。**小体走不到这些分支**：包还没满就已经收尾了。
 *
 * 代价是当时 83 条用例**全绿**，而在任何一个体超过一个数据报的响应上，
 * 服务端会在下一次 `ngtcp2_conn_read_pkt` 上撞断言
 * （`assert(!(conn->flags & NGTCP2_CONN_FLAG_PPE_PENDING))`）当场死掉。
 * 这正是本文件存在的理由：**判据必须是"大的也对"，而不是"没写坏小的"。**
 *
 * 判据取**逐字节相等**，不是长度相等。漏发一段、多发一段、段序错位都会改变
 * 字节内容；只有长度对得上这一个条件太弱 —— 本仓历史上那次静默字节损坏
 * （见 `uvcpp_quic_session.h` 里 `stream_send` 的注释）就是"长度对、内容错"。
 * 体也是**确定性伪随机**的，不是全同字节：全同字节下"段错位"与"段丢失"
 * 都看不出来。
 *
 * 两段各自的靶子：
 *
 * 1. **第一批在 `connect` 回调里一次全发**（9 条、合计约 1.8 MiB）。这一条同时
 *    覆盖两件事：其一，体长凑齐"整数倍 / 非整数倍 / 极小 / 一个 chunk 装得下"
 *    四种形状（`300000` = 64 KiB × 4 + 38944 跨 5 个 chunk、`131072` **正好**
 *    落在 chunk 边界上、`1`、`37`）；其二，`send_q_` 里同时挂着多条**都有大块
 *    数据**的流 —— 写侧那本账如果退化成"一个全局偏移"，并发的两条大体才会让
 *    它显形（与大响应体那条用例头里说的"记账按流分开"同源）。
 *    **"一次全发"不是风格选择**：见下面那段注释，分批发在坏版本上不复现。
 * 2. **发完之后连接还活着**：大响应之后再打一轮小的，确认队列没被大包卡死
 *    （"FIN 落了但队尾没清"这类错会让第二轮永远不来）。
 */
#include <cstdint>
#include <cstring>
#include <iostream>
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

const int kDeadlineMs = 20000;

/// 确定性伪随机体。**不能用全同字节** —— 那样"段错位"与"段丢失"都看不出来。
std::string make_body(size_t n, unsigned seed) {
  std::string s;
  s.resize(n);
  std::uint32_t x = seed * 2654435761u + 12345u;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    s[i] = static_cast<char>((x >> 24) & 0xff);
  }
  return s;
}

/// 路由表。长度刻意取三种形状（见文件头第 1 条）。
struct route {
  const char* path;
  std::string body;
};

std::vector<route>& routes() {
  static std::vector<route> r;
  if (r.empty()) {
    route a;
    a.path = "/big";  // 64 KiB × 4 + 38944 —— 跨 5 个 chunk
    a.body = make_body(300000, 1);
    r.push_back(a);
    route b;
    b.path = "/exact";  // 正好两个 chunk，边界上
    b.body = make_body(131072, 2);
    r.push_back(b);
    route c;
    c.path = "/odd";  // 1 字节
    c.body = make_body(1, 3);
    r.push_back(c);
    route d;
    d.path = "/small";  // 对照：一个 chunk 装得下
    d.body = make_body(37, 4);
    r.push_back(d);
  }
  return r;
}

const std::string* body_for(const std::string& path) {
  std::vector<route>& r = routes();
  for (size_t i = 0; i < r.size(); ++i) {
    if (path == r[i].path) return &r[i].body;
  }
  return nullptr;
}

/// 两条体的第一处不同 —— 把"内容错了"变成一条能定位的读数，而不是只报一句
/// "不相等"。段错位这一类错从这里能一眼看出错在第几字节。
std::string first_diff(const std::string& got, const std::string& want) {
  const size_t n = got.size() < want.size() ? got.size() : want.size();
  for (size_t i = 0; i < n; ++i) {
    if (got[i] != want[i]) {
      return "第 " + std::to_string(i) + " 字节起不同（收 " +
             std::to_string(static_cast<unsigned char>(got[i])) + " / 期望 " +
             std::to_string(static_cast<unsigned char>(want[i])) + "）";
    }
  }
  return "前 " + std::to_string(n) + " 字节相同，长度不同（收 " +
         std::to_string(got.size()) + " / 期望 " + std::to_string(want.size()) +
         "）";
}

/// 响应头里取一个值；没有返回空串。
std::string header_value(const std::vector<h3_header>& hs, const char* name) {
  const size_t n = strlen(name);
  for (size_t i = 0; i < hs.size(); ++i) {
    if (hs[i].name.size() != n) continue;
    if (hs[i].name.compare(0, n, name, n) == 0) return hs[i].value;
  }
  return std::string();
}

/// 等 `want_total` 条响应到齐，再从**进入时**的计数起逐字节比对。返回 false
/// 表示这一段已经失败。
///
/// 完成队列是"拉"的（`uvcpp_h3_connection` 刻意不给"响应到了"的回调，理由见
/// `http3_request_func.cpp` 里那段），所以**拉的地方必须在等待的谓词里面** ——
/// 只等不拉会一直等到 deadline。
///
/// 与 `run_round` 分开，是为了那条"在 `connect` 回调里一次全发"的路：请求不是
/// 这个函数发的（发了就等不到流控那个水位了，见下面那段），它只管等与判。
bool await_and_check(uvcpp_loop* loop,
                     uvcpp_h3_connection& cli,
                     std::vector<h3_response>& done,
                     size_t want_total,
                     const std::string& label) {
  auto pump = [&]() {
    h3_response r;
    while (cli.take_completed(r)) done.push_back(r);
  };

  const size_t before = done.size();
  const size_t want_n = want_total;
  const bool got_all = uvcpp_test::wait_until(
      loop,
      [&] {
        pump();
        return done.size() >= want_n;
      },
      kDeadlineMs);
  pump();
  check(got_all, label + "：所有响应都回来了（期望 " + std::to_string(want_n) +
                     "，实得 " + std::to_string(done.size()) + "）");
  if (!got_all) return false;

  bool ok = true;
  for (size_t i = before; i < done.size(); ++i) {
    const h3_response& r = done[i];
    check(r.error == 0, label + "：响应干净收场");
    check(r.status == 200, label + "：状态 200");

    // 路由由服务端**原样回带**的 `x-req-path` 认 —— 不靠"体长恰好互不相同"
    // 这种一改就塌的巧合，也不靠到达顺序（本来就不保证）。
    const std::string path = header_value(r.headers, "x-req-path");
    if (path.empty()) {
      std::cerr << "  [FAIL] " << label << "：响应没带回 x-req-path" << std::endl;
      ++g_failures;
      ++g_checks;
      ok = false;
      continue;
    }
    const std::string* want = body_for(path);
    if (want == nullptr) {
      std::cerr << "  [FAIL] " << label << "：认不出路由 \"" << path << "\""
                << std::endl;
      ++g_failures;
      ++g_checks;
      ok = false;
      continue;
    }
    // **逐字节相等**，不是长度相等。
    const bool same = r.body == *want;
    check(same, label + "：体逐字节相等（" + path + "，长 " +
                    std::to_string(want->size()) + "，收 " +
                    std::to_string(r.body.size()) + "）" +
                    (same ? std::string() : "：" + first_diff(r.body, *want)));
    if (!same) ok = false;
  }
  return ok;
}

/// 发一批请求、等齐、逐字节比对。适用于"发完这批、再看结果"的形状。
/// **第一批不走这里** —— 理由见 `await_and_check` 的注释与下面 test 里那段。
bool run_round(uvcpp_loop* loop,
               uvcpp_h3_connection& cli,
               std::vector<h3_response>& done,
               const std::vector<std::string>& plan,
               const std::string& label) {
  const size_t before = done.size();
  for (size_t i = 0; i < plan.size(); ++i) {
    h3_request req;
    req.method    = "GET";
    req.scheme    = "https";
    req.authority = "localhost";
    req.path      = plan[i];
    const int64_t sid = cli.send_request(req);
    if (sid < 0) {
      std::cerr << "  [FAIL] " << label << " send_request(" << plan[i] << ")"
                << std::endl;
      ++g_failures;
      ++g_checks;
      return false;
    }
  }
  return await_and_check(loop, cli, done, before + plan.size(), label);
}

void test_h3_large_body_over_one_loop() {
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

  uvcpp_quic_server server(&loop);
  server.set_ssl_context(&server_ctx);
  check_eq_i(server.bind("127.0.0.1", 0), 0, "bind(127.0.0.1, 0)");

  std::unique_ptr<uvcpp_h3_connection> srv_h3;
  int server_sent = 0;

  const int listen_rc = server.listen([&](uvcpp_quic_connection* c) {
    srv_h3.reset(new uvcpp_h3_connection(c, /*server_side=*/true));
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_request = [&](uvcpp_h3_connection& h3, const h3_request& req) {
      h3_response resp;
      resp.stream_id = req.stream_id;
      resp.status    = 200;
      h3_header ct;
      ct.name  = "content-type";
      ct.value = "application/octet-stream";
      resp.headers.push_back(ct);
      // 路径原样回带，客户端据此精确配对。
      h3_header px;
      px.name  = "x-req-path";
      px.value = req.path;
      resp.headers.push_back(px);
      const std::string* b = body_for(req.path);
      resp.body = b != nullptr ? *b : std::string("path=") + req.path;
      if (h3.send_response(resp) != 0) {
        std::cerr << "  [FAIL] 服务端 send_response 返回非 0" << std::endl;
        ++g_failures;
      }
      ++server_sent;
    };
    cbs.on_error = [&](uvcpp_h3_connection&, int code) {
      std::cerr << "  [FAIL] 服务端 h3 报致命错误：" << code << std::endl;
      ++g_failures;
    };
    if (srv_h3->start(cbs) != 0) {
      std::cerr << "  [FAIL] 服务端 h3 start() 返回非 0" << std::endl;
      ++g_failures;
    }
  });
  check_eq_i(listen_rc, 0, "listen() 返回 0");
  const int port = server.configured_port();
  check(port > 0, "listen() 之后 configured_port() 报内核分配的端口");

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  std::unique_ptr<uvcpp_h3_connection> cli_h3;
  std::vector<h3_response>             done;
  bool                                 connected = false;

  // **第一批必须在 `connect` 回调里一次全发出去**，这不是风格问题。
  //
  // 崩溃的触发条件是**连接级流控把发送挡住**：多条大流同时排进 `send_q_`，
  // 加起来超过对端在当前 MAX_DATA 下允许的量，ngtcp2 就会对某条流回
  // `NGTCP2_ERR_STREAM_DATA_BLOCKED`。一条一条发、发完等一等，永远堆不到那个
  // 水位 —— 于是"分批发"的写法在坏版本上照样全绿（这不是推测：本用例最初就是
  // 分三段发的，在坏版本上 3/3 通过；改成下面这种一次全发之后 3/3 复现）。
  //
  // 9 条加起来约 1.8 MiB，明显高过客户端初始 MAX_DATA，所以这个水位是**必然**
  // 到的，不靠时序凑。顺序也照抄了那个能复现的形状。
  std::vector<std::string> burst;
  for (int i = 0; i < 3; ++i) burst.push_back("/big");
  for (int i = 0; i < 2; ++i) burst.push_back("/exact");
  burst.push_back("/odd");
  burst.push_back("/small");
  for (int i = 0; i < 2; ++i) burst.push_back("/big");
  size_t burst_sent = 0;

  if (client.connect("127.0.0.1", port, [&](int status) {
        connected = true;
        if (status != 0) return;
        // 这个回调是**握手完成**那一刻跑的，三条关键流此刻已经开出来了，
        // 可以直接提问（同 `http3_request_func.cpp` 里那段注释）。
        for (size_t i = 0; i < burst.size(); ++i) {
          h3_request req;
          req.method    = "GET";
          req.scheme    = "https";
          req.authority = "localhost";
          req.path      = burst[i];
          if (cli_h3->send_request(req) < 0) {
            std::cerr << "  [FAIL] 第一批 send_request(" << burst[i] << ")"
                      << std::endl;
            ++g_failures;
            return;
          }
          ++burst_sent;
        }
      }) != 0) {
    std::cerr << "  [FAIL] connect() 返回非 0" << std::endl;
    ++g_failures;
    return;
  }

  // `connect()` 返回之后、下一次循环迭代之前建 h3 连接并装回调 —— 这是
  // `uvcpp_quic_client::connect()` 文件里那条 `@warning` 说的唯一安全窗口。
  cli_h3.reset(new uvcpp_h3_connection(client.connection(), /*server_side=*/false));
  {
    uvcpp_h3_connection::callbacks cbs;
    cbs.on_error = [&](uvcpp_h3_connection&, int code) {
      std::cerr << "  [FAIL] 客户端 h3 报致命错误：" << code << std::endl;
      ++g_failures;
    };
    if (cli_h3->start(cbs) != 0) {
      std::cerr << "  [FAIL] 客户端 h3 start() 返回非 0" << std::endl;
      ++g_failures;
      return;
    }
  }

  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "握手在 deadline 内完成");
  if (!connected) return;

  // ---------------------------------------------------------------
  // 第 1 段：第一批（在 `connect` 回调里一次全发的 9 条）—— 等齐、逐字节对
  // ---------------------------------------------------------------
  check_eq_i(static_cast<int>(burst_sent), static_cast<int>(burst.size()),
             "第一批全部发出去了");
  await_and_check(&loop, *cli_h3, done, burst.size(), "第一批");

  // ---------------------------------------------------------------
  // 第 2 段：大响应之后连接还活着（队列没被大包卡死）
  // ---------------------------------------------------------------
  {
    std::vector<std::string> plan;
    plan.push_back("/small");
    plan.push_back("/big");
    plan.push_back("/small");
    run_round(&loop, *cli_h3, done, plan, "收尾");
  }

  // 完成条数恰好等于请求条数：多一条是"重发/重报"，少一条是"丢了一条"。
  // 再转几圈、再拉一轮，确认没有**迟到的**那条（大包之后的收场如果慢了，
  // 上一条断言会假绿）。
  uvcpp_test::pump_for(&loop, 200);
  {
    h3_response r;
    while (cli_h3->take_completed(r)) done.push_back(r);
  }
  check_eq_i(static_cast<int>(done.size()), static_cast<int>(server_sent),
             "完成条数 == 服务端发出的条数");
  check_eq_i(cli_h3->completed_count(), 0, "取走之后队列里没有剩的");
}

}  // namespace

int main() {
  std::cout << "[functional http3_large_body] start" << std::endl;

  check_eq_i(quic_crypto_backend_init(), 0, "quic_crypto_backend_init()");

  test_h3_large_body_over_one_loop();

  quic_crypto_backend_free();

  std::cout << "[functional http3_large_body] checks=" << g_checks
            << " failures=" << g_failures << std::endl;
  if (g_failures == 0) {
    std::cout << "[functional http3_large_body] done success=true" << std::endl;
    return 0;
  }
  std::cout << "[functional http3_large_body] done success=false" << std::endl;
  return 2;
}

#else  // UVCPP_HTTP3_ENABLE

// 关掉 http3 模块时这个文件不该被编译（`tests/functional/CMakeLists.txt` 的
// 过滤器按文件名摘掉它 —— 规则是 `http3`）。真编到了就是配置错，返回非零而不是
// 打一句 SKIP 再返回 0：后者会把"这个模块一次都没被跑过"伪装成"全绿"。
int main() {
  std::cerr << "[http3_large_body] UVCPP_HTTP3_ENABLE=0 —— 这个测试文件不该被编译进来"
            << std::endl;
  return 2;
}

#endif  // UVCPP_HTTP3_ENABLE
