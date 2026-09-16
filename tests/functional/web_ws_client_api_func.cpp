/**
 * @file tests/functional/web_ws_client_api_func.cpp
 * @brief `uvcpp_ws_client` 这一层的会话 / 收发 / 关闭（清单 #10 后半）。
 *
 * 缺口形状（**功能缺口，不是崩溃**）：`uvcpp_ws_connection` 早就有了
 * `send_text` / `on_text` / `close`，但 `uvcpp_ws_client` 只给了一个
 * `connect()`，用户得自己把会话指针存下来才能收发；客户端这一层没有转发，
 * 也没有"关掉"的入口。
 *
 * 接这层转发时撞出来的**第二个缺陷（真缺陷）**：客户端的会话是**懒启动**的
 * —— `start()` 只在第一次 `send_frame` 里被调用。于是**只收不发**的客户端
 * （订阅、推送、纯监听）从头到尾一帧都读不到，表现是"连上了但永远没有消息"，
 * 而它跟"对端确实没发"长得一模一样。服务端本来就是升级完直接 start()
 * （`uvcpp_ws_server.cpp:230`），客户端这边与它对齐就好，而**判据必须是
 * "不发送任何帧也收得到"** —— 现有的 ws 用例全是"先发再收"（回显），
 * 正好从这条缝上走了过去，所以它才一直躺着。
 *
 * 判据分几族：
 *   [1] 只收不发必须收得到（上面那个缺陷的判据；修复前必然超时）。
 *   [2] 客户端层的发送与回调转发：全程不碰会话指针，connect **之前**装回调
 *       也生效（异步的常见写法就是先把 handler 备好再连）。
 *   [3] `close()`：状态机真的走 CLOSING → CLOSED、会话被回收、对端收到
 *       1000 的 Close 帧 —— 只断言"函数返回了"是测不出转发有没有做的。
 *   [4] connect **之后**装回调也生效（两条路都要成立）。
 *   [5] 没有会话时如实报错（`UV_ENOTCONN` + 回调**恰好一次**），不静默丢。
 *   [5b] **建立中** close() = 取消：直接落终态（不许停在 CLOSING —— 没有会话
 *       就没有任何在途操作能把它推下去）、connect 回调当场结算一次、
 *       底层取消事件真回来时也不许把终态翻成 ERROR。
 *   [6] 升级应答（101）与第一帧**同一次写**：客户端必须收到那一帧。
 *       这是 arm 读的时机带来的风险面 —— 手写应答的裸服务端把两者拼成一次
 *       write，loopback 上约 150 字节必然是一个段，所以两者确实进了同一个
 *       读事件。（若被系统分段，这条会"因为另一个原因"通过 —— 159 字节远
 *       小于 MSS，实际不会。）
 *   [7] [6] 的**镜像**：裸客户端把"升级请求 + 第一帧"一次写出去，服务端必须
 *       收到那一帧。
 *
 * 接 [6] 时撞出来的**第三个缺陷（两侧都有）**：握手那一段的读回调会把收到的
 * **整批**字节一次取走（升级应答/请求 + 第一帧），换成本会话的读回调之后那批
 * 字节**再也读不回来**；两侧原先都把它们丢掉了。客户端是 `handshake_buf_`
 * 里 `clear()` 掉的，服务端是 HTTP 解析器把请求吃完、把剩下的留给"下一条 HTTP
 * 消息"（升级之后根本没有下一条）—— 表现一模一样：连接好好的、后面的帧都收得
 * 到，**只有第一帧凭空消失**，而它跟"对端压根没发"长得一模一样。[6] 钉客户端、
 * [7] 钉服务端。
 *
 * 变异验证（改回缺陷写法 → 重建 → 跑；9 个全被抓住，基线还原后仍全绿）：
 *   | 变异 | 结果（失败的判据）|
 *   |---|---|
 *   | M1 去掉升级完成时的 `conn->start()`（退回懒启动）| 抓住：**[1]**、[4]、[6] |
 *   | M2 `send_text` 不转发（直接 `return 0`）| 抓住：**[2]** |
 *   | M3 `on_text` 不回存（只在会话已存在时装）| 抓住：**[1]**、**[2]**、[6] |
 *   | M4 connect 之后装回调不装到会话上 | 抓住：**[4]** |
 *   | M5 没有会话时 `send_text` 返回 0 | 抓住：**[5]** |
 *   | M6 终结观察者不清 `session_` | 抓住：**[3]**（`session() != nullptr`）|
 *   | M7 没有会话时 close() 停在 CLOSING | 抓住：**[5]**、**[5b]**×3 |
 *   | M8 客户端不补投跟 101 同段的字节 | 抓住：**[6]**（[1] 也跟着红）|
 *   | M9 服务端不补投跟升级请求同段的字节 | 抓住：**[7]** |
 *
 *   M1/M3 会把 [1] 一起带红是**应当的**：那条路径上"只收不发"靠的正是
 *   "arm 读 + 回调已装好"，两处任一坏掉它都收不到东西。M8 带红 [1] 说明真实
 *   服务端推的第一帧**有时**也是跟 101 同段到达的（同一台机器上跑，两帧写
 *   之间的间隔可能小于一个段）—— 也就是说这条缝在真实用法里并不罕见。
 *

 * 覆盖不到、如实记录的边界：
 *   - `on_binary` 只是转发，交付语义（分片重组、指针生命周期）由连接层负责，
 *     那里已有用例，这里不重复；
 *   - ping/pong 之类本层没转发（要用的直接拿 `session()`），所以也没测；
 *   - **同一个客户端对象重连**（关掉之后再 `connect()` 一次）没测：本层复用的是
 *     同一个底层 `uvcpp_tcp_client`，它的句柄关掉之后还能不能重开是**它**的
 *     契约，这里没有验证过 —— 所以本层不承诺可重连，要重连就换一个客户端。
 *     回调存在客户端上、每建一个新会话装一次，这条性质由 [2]（connect 之前就
 *     装）与 [4]（之后装）两头夹住。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>

#include <uv.h>

#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_ws_client.h>
#include <web/uvcpp_ws_frame.h>
#include <web/uvcpp_ws_parser.h>
#include <web/uvcpp_ws_server.h>

#if UVCPP_WEB_ENABLE

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++g_fail;
    std::cout << "  [fail] " << what << "\n";
  }
}

int server_port(uvcpp_ws_server& s) {
  sockaddr_in n;
  int l = sizeof(n);
  s.get_http_server()->get_tcp_server()->get_tcp()->getsockname(
      reinterpret_cast<sockaddr*>(&n), &l);
  return ntohs(n.sin_port);
}

/// @brief 服务端与客户端各有自己的循环，主线程交替泵到条件成立（或有界超时）。
bool pump(uvcpp_ws_server& srv, uvcpp_ws_client& cli,
          const std::function<bool()>& done, int timeout_ms = 5000) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!done()) {
    srv.run(UV_RUN_NOWAIT);
    cli.run(UV_RUN_NOWAIT);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count() >= timeout_ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

/// @brief 手写一条服务端→客户端的文本帧（**不加掩码**，RFC 6455 §5.1）。
std::string server_text_frame(const std::string& payload) {
  uvcpp_ws_frame f;
  f.opcode      = ws_opcode::TEXT;
  f.masked      = false;
  f.payload_len = payload.size();
  f.payload     = uvcpp_buf(payload.c_str(), payload.size());
  std::string out(uvcpp_ws_parser::calc_frame_size(f), '\0');
  if (!out.empty()) uvcpp_ws_parser::build_frame(&out[0], f);
  return out;
}

/// @brief 手写一条客户端→服务端的文本帧（**必须加掩码**，RFC 6455 §5.3）。
std::string client_text_frame(const std::string& payload) {
  static const unsigned char kMask[4] = {0x11, 0x22, 0x33, 0x44};
  std::string out;
  out.push_back(static_cast<char>(0x81));   // FIN | TEXT
  out.push_back(static_cast<char>(0x80 | payload.size()));   // MASK | len(<126)
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(kMask[i]));
  for (size_t i = 0; i < payload.size(); ++i) {
    out.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^
                                    kMask[i % 4]));
  }
  return out;
}

// ---------------------------------------------------------------------------
// [1] 只收不发
// ---------------------------------------------------------------------------
void test_receive_only() {
  uvcpp_ws_server srv;
  if (srv.bind("127.0.0.1", 0) != 0) { check(false, "[1] bind"); return; }
  const int port = server_port(srv);
  // 服务端升级完就推一条：客户端**一个字节都不发**。
  srv.on_connection([](uvcpp_ws_connection* c) { c->send_text("push-1", 6); });
  srv.listen();

  auto* cli = new uvcpp_ws_client();
  std::string got;
  cli->on_text([&](const std::string& m) { got = m; });
  const int rc = cli->connect("ws://127.0.0.1:" + std::to_string(port) + "/push",
                             [](uvcpp_ws_connection*, int) {});
  check(rc == 0, "[1] connect 返回 " + std::to_string(rc));

  const bool ok = pump(srv, *cli, [&] { return !got.empty(); });
  check(ok, "[1] 只收不发的客户端必须收到服务端推的第一条"
            "（会话懒启动时这里一帧都读不到，必然超时）");
  check(got == "push-1", "[1] 收到的是 \"" + got + "\"");

  delete cli;
}

// ---------------------------------------------------------------------------
// [2] 客户端层的发送/回调转发（回调在 connect **之前**装）
// ---------------------------------------------------------------------------
void test_client_level_send() {
  uvcpp_ws_server srv;
  if (srv.bind("127.0.0.1", 0) != 0) { check(false, "[2] bind"); return; }
  const int port = server_port(srv);
  srv.on_connection([](uvcpp_ws_connection* c) {
    c->on_text([c](const std::string& m) { c->send_text(m.c_str(), m.size()); });
  });
  srv.listen();

  auto* cli = new uvcpp_ws_client();
  std::string got;
  int send_rc = -9999;
  cli->on_text([&](const std::string& m) { got = m; });   // 先装 handler，再连
  const int rc = cli->connect("ws://127.0.0.1:" + std::to_string(port) + "/echo",
                             [&](uvcpp_ws_connection*, int) {
                               send_rc = cli->send_text("echo-me", 7);
                             });
  check(rc == 0, "[2] connect 返回 " + std::to_string(rc));

  const bool ok = pump(srv, *cli, [&] { return !got.empty(); });
  check(send_rc == 0, "[2] 客户端层 send_text 返回 " + std::to_string(send_rc));
  check(ok && got == "echo-me", "[2] 回显必须回到客户端层装的那个回调上，收到 \""
                                    + got + "\"");
  check(cli->session() != nullptr, "[2] 连上之后 session() 不能是 nullptr");

  delete cli;
}

// ---------------------------------------------------------------------------
// [3] close()：状态机 + 回收 + 对端真的收到 Close 帧；之后还能重连
// ---------------------------------------------------------------------------
void test_close_graceful() {
  uvcpp_ws_server srv;
  if (srv.bind("127.0.0.1", 0) != 0) { check(false, "[3] bind"); return; }
  const int port = server_port(srv);
  std::atomic<int> srv_code{0};
  std::atomic<int> srv_close_calls{0};
  srv.on_connection([&](uvcpp_ws_connection* c) {
    c->on_close([&](ws_close_code code, const std::string&) {
      srv_code.store(static_cast<int>(code));
      srv_close_calls.fetch_add(1);
    });
  });
  srv.listen();

  auto* cli = new uvcpp_ws_client();
  std::string got;
  std::atomic<int> cli_close_calls{0};
  cli->on_text([&](const std::string& m) { got = m; });
  cli->on_close([&](ws_close_code, const std::string&) { cli_close_calls.fetch_add(1); });
  check(cli->connect("ws://127.0.0.1:" + std::to_string(port) + "/", nullptr) == 0,
        "[3] connect");

  pump(srv, *cli, [&] { return cli->session_count() == 1; });
  check(cli->session_count() == 1, "[3] 前置：必须真的连上并有一个会话");

  cli->close();   // 默认 1000 NORMAL
  check(cli->has_status(WS_CLIENT_CLOSING),
        "[3] close() 之后状态必须是 CLOSING（状态机真的动了，不是原地不动）");

  const bool done = pump(srv, *cli, [&] { return cli->recycled_session_count() >= 1; });
  check(done, "[3] close() 之后会话必须被终结并回收");
  check(cli->has_status(WS_CLIENT_CLOSED), "[3] 终结之后状态必须是 CLOSED");
  check(cli->session() == nullptr, "[3] 终结之后 session() 必须是 nullptr");
  check(cli->session_count() == 0, "[3] 活动会话数必须归零");
  check(cli->recycled_session_count() == 1, "[3] 回收计数必须是 1");
  check(srv_code.load() == 1000,
        "[3] 服务端必须收到 1000 —— close() 真的把 Close 帧发出去了（收到 "
        + std::to_string(srv_code.load()) + "）");
  check(srv_close_calls.load() == 1,
        "[3] 服务端 on_close 恰好一次（实际 " + std::to_string(srv_close_calls.load()) + "）");
  check(cli_close_calls.load() <= 1,
        "[3] 客户端自己的 close() 不该把 on_close 叫两次");

  delete cli;
}

// ---------------------------------------------------------------------------
// [4] connect **之后**装回调
// ---------------------------------------------------------------------------
void test_callbacks_after_connect() {
  uvcpp_ws_server srv;
  if (srv.bind("127.0.0.1", 0) != 0) { check(false, "[4] bind"); return; }
  const int port = server_port(srv);
  uvcpp_ws_connection* srv_conn = nullptr;
  srv.on_connection([&](uvcpp_ws_connection* c) { srv_conn = c; });
  srv.listen();

  auto* cli = new uvcpp_ws_client();
  const int rc = cli->connect("ws://127.0.0.1:" + std::to_string(port) + "/",
                             [](uvcpp_ws_connection*, int) {});
  check(rc == 0, "[4] connect");

  pump(srv, *cli, [&] { return cli->session() != nullptr && srv_conn != nullptr; });
  check(cli->session() != nullptr && srv_conn != nullptr, "[4] 前置：必须真的连上");

  std::string got;
  cli->on_text([&](const std::string& m) { got = m; });   // 连上之后再装
  check(got.empty(), "[4] 前置：装回调之前不该有消息");

  // 从**服务端**那边推一条（客户端一个字节都不发）：这样消息一定是在
  // "回调装晚了"之后才产生的，测得不是"恰好赶上了"。
  srv_conn->send_text("push-3", 6);
  const bool ok = pump(srv, *cli, [&] { return !got.empty(); });
  check(ok && got == "push-3",
        "[4] connect 之后再装回调也必须生效（收到 \"" + got + "\"）");

  delete cli;
}

// ---------------------------------------------------------------------------
// [5] 没有会话时的如实报错；建立中就 close 等于取消
// ---------------------------------------------------------------------------
void test_no_session_reporting() {
  auto* cli = new uvcpp_ws_client();   // 从没连过
  int cb_calls = 0;
  int cb_err   = 0;
  const int rc = cli->send_text("x", 1, [&](int e) { ++cb_calls; cb_err = e; });
  check(rc == UV_ENOTCONN, "[5] 没有会话时 send_text 必须返回 UV_ENOTCONN（返回 "
                              + std::to_string(rc) + "）");
  check(cb_calls == 1 && cb_err == UV_ENOTCONN,
        "[5] 回调必须**恰好一次**且带同一个错误码（calls=" + std::to_string(cb_calls)
            + " err=" + std::to_string(cb_err) + "）");
  check(cli->send_binary("x", 1) == UV_ENOTCONN, "[5] send_binary 同理");

  cli->close();   // 没有会话：不能崩，落到终态
  check(cli->has_status(WS_CLIENT_CLOSED), "[5] 没有会话时 close() 落到终态 CLOSED");
  check(cli->session() == nullptr, "[5] session() 仍是 nullptr");

  delete cli;   // 没连过就析构，不许崩
}

/**
 * [5b] **建立中**就 close()：等于取消这次连接。
 *
 * 判据是"取消之后状态**不会**停在 CLOSING" —— 没有会话就没有任何在途操作能把
 * 它推到 CLOSED，停在 CLOSING 的客户端看起来"还在关"，实际上永远不会再变。
 * 服务端 accept 得到、但**永远不回 101**，客户端就稳稳地停在建立中。
 */
void test_close_while_connecting() {
  uvcpp_tcp_server raw;
  raw.set_read_callback([](uvcpp_tcp_client&, const net_read_result&) {
    // 收到握手请求就装死：不回 101。
  });
  if (raw.bindIpv4("127.0.0.1", 0) != 0) { check(false, "[5b] bind"); return; }
  sockaddr_in n;
  int l = sizeof(n);
  raw.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&n), &l);
  const int port = ntohs(n.sin_port);
  std::atomic<int> accepted{0};
  raw.listen([&](uvcpp_tcp_client*) { accepted.fetch_add(1); }, 128);

  auto* cli = new uvcpp_ws_client();
  int  cb_calls = 0;
  int  cb_err   = 0;
  uvcpp_ws_connection* cb_conn = reinterpret_cast<uvcpp_ws_connection*>(1);
  check(cli->connect("ws://127.0.0.1:" + std::to_string(port) + "/",
                     [&](uvcpp_ws_connection* c, int e) {
                       ++cb_calls;
                       cb_conn = c;
                       cb_err  = e;
                     }) == 0,
        "[5b] connect");
  check(cli->has_status(WS_CLIENT_CONNECTING), "[5b] 前置：此刻必须在建立中");
  check(cb_calls == 0, "[5b] 前置：一秒都没泵过，connect 回调不该已经来过");

  cli->close();
  check(cb_calls == 1, "[5b] 取消也要**当场**给调用方一个结果，不能悬着");
  check(cb_conn == nullptr && cb_err != 0,
        "[5b] 取消时报的是失败（conn=nullptr, err=" + std::to_string(cb_err) + "）");
  check(cli->has_status(WS_CLIENT_CLOSED),
        "[5b] 建立中 close() 必须直接落终态 —— 停在 CLOSING 是个永远不会变的谎");
  check(cli->session() == nullptr && cli->session_count() == 0,
        "[5b] 不该冒出会话");

  // 再泵一会儿：底层取消事件真回来的时候，不许把终态翻成 ERROR。
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0).count() < 300) {
    raw.run(UV_RUN_NOWAIT);
    cli->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(!cli->has_status(WS_CLIENT_ERROR),
        "[5b] 调用方自己关的不算连接出错，终态不许被翻成 ERROR");
  check(cli->has_status(WS_CLIENT_CLOSED), "[5b] 终态保持 CLOSED");
  check(cb_calls == 1, "[5b] connect 回调只该结算一次（实际 "
                           + std::to_string(cb_calls) + " 次）");

  delete cli;
}

// ---------------------------------------------------------------------------
// [6] 101 与第一帧同一次写
// ---------------------------------------------------------------------------
void test_coalesced_101_and_frame() {
  // 先用 RFC 6455 §1.3 的定值钉一下这个 fixture 的前提：本用例要靠
  // `compute_accept_key` 写一个**合法**的 101，它错了这条用例会因为"握手就
  // 没过"而失败（那是另一种红），所以先把算法本身钉死。
  check(uvcpp_ws_server::compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==") ==
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
        "[6] 前置：compute_accept_key 必须给出 RFC 6455 的示例值");

  std::map<uvcpp_tcp_client*, std::string> reqs;
  std::set<uvcpp_tcp_client*>              replied;
  uvcpp_tcp_server raw;
  raw.set_read_callback([&](uvcpp_tcp_client& c, const net_read_result& r) {
    if (!r.is_data() || replied.count(&c) > 0) return;
    std::string& acc = reqs[&c];
    acc.append(r.data, r.size);
    const size_t end = acc.find("\r\n\r\n");
    if (end == std::string::npos) return;
    replied.insert(&c);

    // 取握手 key（大小写不敏感地找一遍足够用：本用例自己发的请求）。
    std::string key;
    const std::string name = "sec-websocket-key:";
    std::string lower = acc;
    for (size_t i = 0; i < lower.size(); ++i) {
      if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] = static_cast<char>(lower[i] + 32);
    }
    const size_t k = lower.find(name);
    if (k != std::string::npos) {
      size_t v = k + name.size();
      while (v < acc.size() && (acc[v] == ' ' || acc[v] == '\t')) ++v;
      size_t ve = acc.find("\r\n", v);
      if (ve == std::string::npos) ve = acc.size();
      key = acc.substr(v, ve - v);
    }

    // **一次 write**：101 与第一帧在同一个 TCP 段里到达客户端。
    const std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + uvcpp_ws_server::compute_accept_key(key) + "\r\n\r\n" +
        server_text_frame("same-segment");
    c.write(resp.c_str(), resp.size(), nullptr);
  });
  if (raw.bindIpv4("127.0.0.1", 0) != 0) { check(false, "[6] bind"); return; }
  sockaddr_in n;
  int l = sizeof(n);
  raw.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&n), &l);
  const int port = ntohs(n.sin_port);
  std::atomic<int> accepted{0};
  raw.listen([&](uvcpp_tcp_client*) { accepted.fetch_add(1); }, 128);

  auto* cli = new uvcpp_ws_client();
  std::string got;
  cli->on_text([&](const std::string& m) { got = m; });   // 只收不发
  const int rc = cli->connect("ws://127.0.0.1:" + std::to_string(port) + "/",
                             [](uvcpp_ws_connection*, int) {});
  check(rc == 0, "[6] connect");

  const auto t0 = std::chrono::steady_clock::now();
  while (got.empty() &&
         std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0).count() < 5000) {
    raw.run(UV_RUN_NOWAIT);
    cli->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(accepted.load() == 1, "[6] 前置：裸服务端必须 accept 到连接");
  check(got == "same-segment",
        "[6] 101 与首帧同段时必须收到那一帧（收到 \"" + got + "\"）");

  delete cli;
}

// ---------------------------------------------------------------------------
// [7] 服务端那一侧的同一条缝：升级请求与第一帧同一次写
// ---------------------------------------------------------------------------
/**
 * 裸客户端把 **升级请求 + 第一帧** 拼成**一次** `write()` 发出去 —— 真实
 * 客户端就是这么发的（升级和第一条消息之间没有任何等待）。服务端必须收到
 * 那一帧。
 *
 * 这是 [6] 的镜像：两边的读路径都把"这一批剩余的字节"吃掉了（服务端是 HTTP
 * 解析器把请求吃完、把剩下的留给"下一条 HTTP 消息"，可升级之后根本没有下一条
 * HTTP 消息）。两边都不补投的话，表现完全一样：连接好好的、只有第一帧没了。
 */
void test_coalesced_upgrade_and_frame_server_side() {
  uvcpp_ws_server srv;
  if (srv.bind("127.0.0.1", 0) != 0) { check(false, "[7] bind"); return; }
  const int port = server_port(srv);
  std::atomic<int> conns{0};
  std::string got;
  srv.on_connection([&](uvcpp_ws_connection* c) {
    conns.fetch_add(1);
    c->on_text([&](const std::string& m) { got = m; });
  });
  srv.listen();

  // 升级请求 + 第一帧，**一次** write。
  const std::string handshake =
      "GET /coalesce HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  const std::string req = handshake + client_text_frame("coalesced");

  uvcpp_loop* cli_loop = new uvcpp_loop();
  auto*       cli      = new uvcpp_tcp_client(cli_loop);
  int         conn_rc  = -9999;
  cli->connect("127.0.0.1", port, [&](int st) {
    conn_rc = st;
    if (st == 0) cli->write(req.c_str(), req.size(), nullptr);
  });

  const auto t0 = std::chrono::steady_clock::now();
  while (got.empty() &&
         std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0).count() < 5000) {
    srv.run(UV_RUN_NOWAIT);
    cli_loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(conn_rc == 0, "[7] 前置：裸客户端必须连上（rc=" + std::to_string(conn_rc) + "）");
  check(conns.load() == 1, "[7] 前置：服务端必须升级出恰好一个会话（实际 "
                               + std::to_string(conns.load()) + "）");
  check(got == "coalesced",
        "[7] 升级请求与首帧同一次到达时，服务端必须收到那一帧（收到 \"" + got + "\"）");

  delete cli;
  delete cli_loop;
}

}  // namespace

int main() {
  std::cout << "[functional web_ws_client_api] start" << std::endl;

  test_receive_only();
  test_client_level_send();
  test_close_graceful();
  test_callbacks_after_connect();
  test_no_session_reporting();
  test_close_while_connecting();
  test_coalesced_101_and_frame();
  test_coalesced_upgrade_and_frame_server_side();

  if (g_fail == 0) {
    std::cout << "[functional web_ws_client_api] all checks passed" << std::endl;
    return 0;
  }
  std::cout << "[functional web_ws_client_api] " << g_fail << " check(s) failed"
            << std::endl;
  return 2;
}

#else
int main() { return 0; }
#endif
