/**
 * @file tests/functional/web_app_read_boundary_func.cpp
 * @brief 一条连接上连灌多条请求时，**读边界不得吃掉消息**。
 *
 * 与 `web_app_pipeline_func.cpp` 打的是同一条连接上的多条消息，但判据不同：那边
 * 判的是**响应顺序 / 头部串味 / 同连接上限**（消息都解析出来了之后的事），本文件
 * 判的是**消息有没有被解析出来**。
 *
 * 解析器的状态是按**读**清的（`on_connection_data` 开头那个 `if (ctx.msg_done)`），
 * 而"消息在哪结束"和"读在哪结束"是两件互不相干的事。错位的坏法有两条，各自独立：
 *
 *  1. **读结束在消息中间、而这个读里已经有消息完成过**（`msg_done` 因此为真）⇒ 下一次
 *     读开头照着 `msg_done` 对一条解析到一半的报文调 `llhttp_init`，半个状态被抹掉。
 *     断在报文末尾那个空行上 ⇒ 那条请求**静默消失**（不报错、不回、也不进任何表 ——
 *     `llhttp_init` 落到的 `n_start` 把收尾空行当空行吞掉）；断在头部中间 ⇒
 *     `HPE_INVALID_METHOD` ⇒ 400 关连接 ⇒ 它之后整段不回。这个现象最早是在
 *     `web_app_static_func.cpp` 的 `raw_batch` 里撞见的
 *     （本机 100% 复现：1100 条固定丢第 745 条，它的字节区间 65450..65538 正好盖住
 *     65536），当时记成"另一个问题"、靠"每批只灌两三 KB"绕开 —— 这个文件就是那个
 *     "另一个问题"的判据，那边现在只留一行指向这里。
 *  2. **同一读里有两条消息** ⇒ 单消息暂存（`body_buf`/`body_bytes`…）属于**正在
 *     解析的那条**消息，却只在读开头清 ⇒ 第二条请求的 body 变成"上一条 + 这一条"。
 *
 * 两条都是**静默**的正确性问题（没有错误码、没有告警），所以判据钉的是"一条都不许
 * 少""每个 body 都是它自己的"，不是"没崩"。另有两条**对照**：单条消息、以及 body
 * 跨读的消息 —— 它们在修之前之后都必须一直是绿的，否则上面两条红说明不了是边界的错。
 */
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>

#include "wait_util.h"

#if UVCPP_WEBAPP_ENABLE

#include "net/uvcpp_net_read.h"
#include "net/uvcpp_tcp_client.h"
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "（得到 " << got << "，期望 " << want << "）"
              << std::endl;
    ++g_failures;
  }
}

size_t count_of(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  size_t p = 0;
  while ((p = hay.find(needle, p)) != std::string::npos) {
    ++n;
    p += needle.size();
  }
  return n;
}

std::string body_of(const std::string& raw) {
  const size_t p = raw.find("\r\n\r\n");
  return (p == std::string::npos) ? std::string() : raw.substr(p + 4);
}

/// 连上去，把 `parts` 依次写出去（每次写之间**跑一会儿 loop**，逼服务端分读），
/// 读到对端关闭为止，返回全部原始字节。
///
/// 请求的最后一条必须带 `Connection: close`：靠"服务端关连接"判定这批处理完了，
/// 否则要白等一整个 `wait_ms`。
std::string send_parts(int port, const std::string* parts, size_t nparts,
                       int gap_ms, int wait_ms) {
  uvcpp_tcp_client client;
  std::atomic<bool> connected(false);
  std::atomic<bool> ended(false);
  std::string got;

  int rc = client.connect("127.0.0.1", port, [&](int st) {
    if (st != 0) return;
    connected.store(true);
    client.read_start_events([&](uvcpp_tcp_client&, const net_read_result& r) {
      if (r.is_data()) {
        got.append(r.data, r.size);
      } else {
        ended.store(true);
      }
    });
  });
  if (rc != 0) return got;

  uvcpp_loop* loop = client.get_loop();
  uvcpp_test::wait_until(loop, [&] { return connected.load(); }, uvcpp_test::kWaitMs);
  if (!connected.load()) return got;

  const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < nparts && !ended.load(); ++i) {
    client.write(parts[i].data(), parts[i].size(), [](int) {});
    if (i + 1 == nparts) break;
    // 两次写之间给服务端一段时间把它读到 —— 不分读的话"body 跨读"这条对照就
    // 退化成"两段挤在一次读里"，测的就不是它了。
    const std::chrono::steady_clock::time_point g0 = std::chrono::steady_clock::now();
    while (!ended.load()) {
      loop->run(UV_RUN_NOWAIT);
      const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - g0)
                               .count();
      if (ms >= gap_ms) break;
      std::this_thread::yield();
    }
  }

  while (!ended.load()) {
    loop->run(UV_RUN_NOWAIT);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    if (ms >= wait_ms) break;
    std::this_thread::yield();
  }

  std::atomic<bool> closed(false);
  if (client.get_tcp() != nullptr) {
    client.get_tcp()->close([&](uvcpp_handle*) { closed.store(true); });
  }
  uvcpp_test::wait_until(loop, [&] { return closed.load(); }, uvcpp_test::kWaitMs);
  for (int i = 0; i < 30; ++i) loop->run(UV_RUN_NOWAIT);

  return got;
}

std::string send_one(int port, const std::string& all, int wait_ms) {
  const std::string parts[1] = {all};
  return send_parts(port, parts, 1, 0, wait_ms);
}

/// `/p/:i` 的响应体固定，判据只数状态行 —— 1100 条里数 body 反而容易被别的东西
/// 撞上（`content-length` 的数字串就散落在头里）。
std::string burst_request(int i, int last, size_t fill) {
  std::string r = "GET /p/" + std::to_string(i) + " HTTP/1.1\r\n";
  r += "Host: 127.0.0.1\r\n";
  r += "Accept-Encoding: identity\r\n";
  if (fill > 0) r += "X-Fill: " + std::string(fill, 'f') + "\r\n";
  // 只让最后一条关连接：中间任何一条关掉都会把"这批都处理完了"提前成立，
  // 后面丢多少条都看不出来。
  r += (i == last) ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
  r += "\r\n";
  return r;
}

// =========================================================================
// 1. 读在消息中间结束 —— 一条都不许少
// =========================================================================
void case_burst_across_read_boundary() {
  std::cout << "[pipeline] 跨读边界的请求不许丢" << std::endl;

  // 1100 条 × ~86 B ≈ 94 KiB。服务端每次读的缓冲是 libuv 建议的尺寸（64 KiB，
  // `uvcpp_tcp_client::internal_alloc_cb` 直接用 `sz`），所以这批**必定**至少
  // 有一次读结束在报文中间。填充长度换个值，边界就落到另一条请求上 —— 判据不是
  // "某个特定的第 N 条"，而是"一条都不许少"。
  const int kBurst = 1100;
  const size_t kFill[2] = {0, 19};

  for (int variant = 0; variant < 2; ++variant) {
    std::atomic<int> handled(0);

    uvcpp_web_app app;
    app.set_port(0);
    app.set_log_level(log_level::WARN);
    app.get("/p/:i", [&handled](uvcpp_web_request&, uvcpp_web_response& resp,
                                uvcpp_web_next) {
      handled.fetch_add(1);
      resp.text(std::string("ok"));
      resp.end();
    });
    if (app.start_background() != 0) {
      check(false, "边界：服务启动失败");
      return;
    }
    const int port = app.bound_port();
    check(port > 0, "边界：端口有效");

    std::string burst;
    burst.reserve(static_cast<size_t>(kBurst) * 96);
    for (int i = 0; i < kBurst; ++i) burst += burst_request(i, kBurst - 1, kFill[variant]);

    // 前置：这批字节必须真的越过一次读的缓冲，否则这条用例只是"又跑了一遍
    // 正常路径"，红了绿了都跟边界无关。
    check(burst.size() > 65536, "前置：灌入的字节要越过服务端 64 KiB 的读缓冲");

    const std::string got = send_one(port, burst, 15000);
    app.stop();
    app.join();

    const std::string tag = "（填充 " + std::to_string(kFill[variant]) + "）";
    check_eq_i(static_cast<long long>(handled.load()), kBurst,
               "边界：每一条都要**被处理**，一条都不许静默消失 " + tag);
    check_eq_i(static_cast<long long>(count_of(got, "HTTP/1.1 200")), kBurst,
               "边界：每一条都要有响应 " + tag);
    check_eq_i(static_cast<long long>(count_of(got, "HTTP/1.1 400")), 0,
               "边界：不许把半条报文当成畸形请求回 400 " + tag);
  }
}

// =========================================================================
// 2. 同一读里两条消息 —— 后一条的 body 不许带上前一条的
// =========================================================================
void case_two_posts_one_read() {
  std::cout << "[pipeline] 同读两条请求的 body 各归各" << std::endl;

  std::atomic<int> handled(0);

  uvcpp_web_app app;
  app.set_port(0);
  app.set_log_level(log_level::WARN);
  app.post("/echo", [&handled](uvcpp_web_request& req, uvcpp_web_response& resp,
                               uvcpp_web_next) {
    handled.fetch_add(1);
    resp.text(req.body_str());
    resp.end();
  });
  if (app.start_background() != 0) {
    check(false, "同读两条：服务启动失败");
    return;
  }
  const int port = app.bound_port();

  // 两条一起写出去：**很小**，小到服务端一次就读完 —— 这条用例要的正是"两条消息
  // 落在同一次读里"，不是边界。上一条用例才是边界。
  std::string all =
      "POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 4\r\n"
      "Connection: keep-alive\r\n\r\nAAAA"
      "POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 4\r\n"
      "Connection: close\r\n\r\nBBBB";

  const std::string got = send_one(port, all, 5000);
  app.stop();
  app.join();

  check_eq_i(static_cast<long long>(handled.load()), 2, "同读两条：两条都要被处理");
  check_eq_i(static_cast<long long>(count_of(got, "AAAA")), 1,
             "同读两条：第一条回显的就是它自己的 body");
  check_eq_i(static_cast<long long>(count_of(got, "BBBB")), 1,
             "同读两条：第二条回显的也是它自己的 body");
  // 这条是主判据：暂存按消息清的话，第二条的 body 会变成 "AAAA"+"BBBB"。
  check_eq_i(static_cast<long long>(count_of(got, "AAAABBBB")), 0,
             "同读两条：第二条不许把第一条的 body 一起带上");
}

// =========================================================================
// 3. 对照甲：单条消息（边界条件不成立时，判据本来就该是绿的）
// =========================================================================
void case_control_single_post() {
  std::cout << "[pipeline] 对照：单条 POST" << std::endl;

  uvcpp_web_app app;
  app.set_port(0);
  app.set_log_level(log_level::WARN);
  app.post("/echo", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next) {
    resp.text(req.body_str());
    resp.end();
  });
  if (app.start_background() != 0) {
    check(false, "对照甲：服务启动失败");
    return;
  }
  const int port = app.bound_port();

  const std::string all =
      "POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 4\r\n"
      "Connection: close\r\n\r\nZZZZ";
  const std::string got = send_one(port, all, 5000);
  app.stop();
  app.join();

  check_eq_i(static_cast<long long>(body_of(got).size()), 4, "对照甲：body 长度");
  check(body_of(got) == "ZZZZ", "对照甲：body 逐字节回显");
}

// =========================================================================
// 4. 对照乙：body 跨读 —— 一次读只拿到半个 body 时，前半个不许被清掉
// =========================================================================
void case_control_body_across_reads() {
  std::cout << "[pipeline] 对照：body 跨读" << std::endl;

  uvcpp_web_app app;
  app.set_port(0);
  app.set_log_level(log_level::WARN);
  app.post("/echo", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next) {
    resp.text(req.body_str());
    resp.end();
  });
  if (app.start_background() != 0) {
    check(false, "对照乙：服务启动失败");
    return;
  }
  const int port = app.bound_port();

  const std::string body(2000, 'q');
  std::string head = "POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2000\r\n"
                     "Connection: close\r\n\r\n";
  // 恰好断在 body 中间：前半段先写、等服务端读走，再写后半段。
  const std::string parts[2] = {head + body.substr(0, 1000), body.substr(1000)};

  const std::string got = send_parts(port, parts, 2, 50, 5000);
  app.stop();
  app.join();

  check_eq_i(static_cast<long long>(body_of(got).size()), 2000,
             "对照乙：跨读的 body 一个字节都不许丢");
  check(body_of(got) == body, "对照乙：跨读的 body 逐字节相同");
}

}  // namespace

int main() {
  std::cout << std::unitbuf;
  case_burst_across_read_boundary();
  case_two_posts_one_read();
  case_control_single_post();
  case_control_body_across_reads();

  if (g_failures != 0) {
    std::cerr << "[pipeline] 失败 " << g_failures << " 条" << std::endl;
    return 1;
  }
  std::cout << "[pipeline] 全部通过" << std::endl;
  return 0;
}

#else

int main() {
  std::cout << "[pipeline] WEBAPP 未启用，跳过" << std::endl;
  return 0;
}

#endif
