/**
 * @file tests/functional/web_ws_frame_func.cpp
 * @brief WebSocket 逐帧接口（#50）：`on_frame` / `send_frame`。
 *
 * uvscope 0.4 的 WebSocket 代理/调试器要看到**线上原样**的帧：opcode、FIN、
 * RSV1（permessage-deflate）、masked 与原始 payload。1.8.2 之前只有消息级
 * 回调，分片已经重组、拿不到帧边界。这里钉住：
 *
 * 1. `send_text` 发一条普通消息：对端 `on_frame` 看到一帧 TEXT/fin/masked，
 *    `on_text` 仍然拿到完整消息（两级回调可同时注册）；
 * 2. 分片：`send_frame` 发 TEXT(fin=false) + CONTINUATION(fin=true)，对端
 *    逐帧看到两帧，`on_text` 只回调一次且内容是重组后的；
 * 3. PING/PONG：`on_frame` 能看到控制帧，自动 PONG 仍然执行；
 * 4. 掩码方向：客户端发的是 masked、服务端发的是 unmasked（由连接按角色盖）；
 * 5. permessage-deflate：分片的压缩文本在逐帧接口上 `rsv1` 可见，`on_text`
 *    拿到解压后的文本。
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_ws_client.h>
#include <web/uvcpp_ws_connection.h>
#include <web/uvcpp_ws_ext.h>
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
  } else {
    std::cout << "  [ok] " << what << "\n";
  }
}

int server_port(uvcpp_ws_server& s) {
  sockaddr_in n;
  int l = sizeof(n);
  s.get_http_server()->get_tcp_server()->get_tcp()->getsockname(
      reinterpret_cast<sockaddr*>(&n), &l);
  return ntohs(n.sin_port);
}

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

/// 回调里 frame 的 payload 只在回调期间有效 —— 这里把要判的字段抄一份。
struct frame_rec {
  bool      fin    = true;
  bool      rsv1   = false;
  bool      rsv2   = false;
  bool      rsv3   = false;
  bool      masked = false;
  ws_opcode opcode = ws_opcode::TEXT;
  std::string payload;
};

struct frame_log {
  std::mutex             mu;
  std::vector<frame_rec> recs;

  void add(const uvcpp_ws_frame& f) {
    frame_rec r;
    r.fin    = f.fin;
    r.rsv1   = f.rsv1;
    r.rsv2   = f.rsv2;
    r.rsv3   = f.rsv3;
    r.masked = f.masked;
    r.opcode = f.opcode;
    if (f.payload.size() > 0) {
      r.payload.assign(f.payload.get_const_data(), f.payload.size());
    }
    std::lock_guard<std::mutex> lk(mu);
    recs.push_back(std::move(r));
  }

  std::size_t size() {
    std::lock_guard<std::mutex> lk(mu);
    return recs.size();
  }

  frame_rec at(std::size_t i) {
    std::lock_guard<std::mutex> lk(mu);
    return recs.at(i);
  }
};

#if UVCPP_ZLIB_ENABLE
// 默认配置里 permessage-deflate 是**开**的；1–4 要看未压缩的原始帧。
void disable_compression(uvcpp_ws_server& srv) {
  uvcpp_ws_deflate_config off;
  off.enabled = false;
  srv.set_compression(off);
}
void disable_compression(uvcpp_ws_client& cli) {
  uvcpp_ws_deflate_config off;
  off.enabled = false;
  cli.set_compression(off);
}
#else
void disable_compression(uvcpp_ws_server&) {}
void disable_compression(uvcpp_ws_client&) {}
#endif

uvcpp_buf make_payload(const std::string& s) {
  return uvcpp_buf(s.data(), s.size());
}

// ---------------------------------------------------------------------------
// 1. 普通消息：两级回调同时注册
// ---------------------------------------------------------------------------
void test_plain_message() {
  uvcpp_ws_server srv;
  disable_compression(srv);
  if (srv.bind("127.0.0.1", 0) != 0) {
    check(false, "[1] bind");
    return;
  }
  const int port = server_port(srv);
  frame_log srv_frames;
  std::string srv_text;
  srv.on_connection([&](uvcpp_ws_connection* c) {
    c->on_frame([&](const uvcpp_ws_frame& f) { srv_frames.add(f); });
    c->on_text([&](const std::string& m) { srv_text = m; });
  });
  srv.listen();

  uvcpp_ws_client cli;
  disable_compression(cli);
  check(cli.connect("ws://127.0.0.1:" + std::to_string(port) + "/", nullptr) == 0,
        "[1] connect");
  check(pump(srv, cli, [&] { return cli.session() != nullptr; }),
        "[1] 会话建立");
  check(cli.session() != nullptr, "[1] session()");
  if (cli.session() == nullptr) return;

  check(cli.session()->send_text("plain", 5) == 0, "[1] send_text 受理");
  check(pump(srv, cli, [&] { return srv_text == "plain"; }), "[1] 对端收到消息");
  check(srv_text == "plain", "[1] on_text 内容");
  check(srv_frames.size() == 1, "[1] 逐帧回调恰好一次");
  if (srv_frames.size() == 1) {
    const frame_rec r = srv_frames.at(0);
    check(r.opcode == ws_opcode::TEXT, "[1] opcode = TEXT");
    check(r.fin, "[1] fin = true");
    check(r.masked, "[1] 客户端发出的帧 masked = true");
    check(!r.rsv1 && !r.rsv2 && !r.rsv3,
          "[1] 未压缩时 RSV 全 0 (rsv=" + std::to_string(r.rsv1) + "/" +
              std::to_string(r.rsv2) + "/" + std::to_string(r.rsv3) + ")");
    check(r.payload == "plain",
          "[1] 逐帧 payload 原样 (len=" + std::to_string(r.payload.size()) +
              " data=\"" + r.payload + "\")");
  }

  cli.close();
  pump(srv, cli, [&] { return cli.session() == nullptr; });
}

// ---------------------------------------------------------------------------
// 2. 分片：send_frame(TEXT, fin=false) + CONTINUATION(fin=true)
// ---------------------------------------------------------------------------
void test_fragmented() {
  uvcpp_ws_server srv;
  disable_compression(srv);
  if (srv.bind("127.0.0.1", 0) != 0) {
    check(false, "[2] bind");
    return;
  }
  const int port = server_port(srv);
  frame_log srv_frames;
  std::string srv_text;
  srv.on_connection([&](uvcpp_ws_connection* c) {
    c->on_frame([&](const uvcpp_ws_frame& f) { srv_frames.add(f); });
    c->on_text([&](const std::string& m) { srv_text = m; });
  });
  srv.listen();

  uvcpp_ws_client cli;
  disable_compression(cli);
  check(cli.connect("ws://127.0.0.1:" + std::to_string(port) + "/", nullptr) == 0,
        "[2] connect");
  check(pump(srv, cli, [&] { return cli.session() != nullptr; }), "[2] 会话建立");
  if (cli.session() == nullptr) return;

  uvcpp_ws_frame f1;
  f1.opcode  = ws_opcode::TEXT;
  f1.fin     = false;
  f1.payload = make_payload("hel");
  check(cli.session()->send_frame(f1) == 0, "[2] 首片受理");

  uvcpp_ws_frame f2;
  f2.opcode  = ws_opcode::CONTINUATION;
  f2.fin     = true;
  f2.payload = make_payload("lo");
  check(cli.session()->send_frame(f2) == 0, "[2] 续片受理");

  check(pump(srv, cli, [&] { return srv_text == "hello"; }), "[2] 重组消息到达");
  check(srv_text == "hello", "[2] on_text 只交付重组后的完整消息");
  check(srv_frames.size() == 2, "[2] 逐帧回调两次（不是一次）");
  if (srv_frames.size() == 2) {
    const frame_rec a = srv_frames.at(0);
    const frame_rec b = srv_frames.at(1);
    check(a.opcode == ws_opcode::TEXT && !a.fin && a.payload == "hel",
          "[2] 第 1 帧 TEXT/fin=false/\"hel\"");
    check(b.opcode == ws_opcode::CONTINUATION && b.fin && b.payload == "lo",
          "[2] 第 2 帧 CONTINUATION/fin=true/\"lo\"");
    check(a.masked && b.masked, "[2] 客户端分片的每一片都 masked");
  }

  cli.close();
  pump(srv, cli, [&] { return cli.session() == nullptr; });
}

// ---------------------------------------------------------------------------
// 3. PING / PONG：逐帧可见，自动 PONG 仍然执行
// ---------------------------------------------------------------------------
void test_ping_pong() {
  uvcpp_ws_server srv;
  disable_compression(srv);
  if (srv.bind("127.0.0.1", 0) != 0) {
    check(false, "[3] bind");
    return;
  }
  const int port = server_port(srv);
  frame_log srv_frames;
  frame_log cli_frames;
  std::string srv_ping;
  std::string cli_pong;
  srv.on_connection([&](uvcpp_ws_connection* c) {
    c->on_frame([&](const uvcpp_ws_frame& f) { srv_frames.add(f); });
    c->on_ping([&](const uint8_t* d, size_t n) {
      srv_ping.assign(reinterpret_cast<const char*>(d), n);
    });
  });
  srv.listen();

  uvcpp_ws_client cli;
  disable_compression(cli);
  check(cli.connect("ws://127.0.0.1:" + std::to_string(port) + "/", nullptr) == 0,
        "[3] connect");
  check(pump(srv, cli, [&] { return cli.session() != nullptr; }), "[3] 会话建立");
  if (cli.session() == nullptr) return;
  cli.session()->on_frame(
      [&](const uvcpp_ws_frame& f) { cli_frames.add(f); });
  cli.session()->on_pong([&](const uint8_t* d, size_t n) {
    cli_pong.assign(reinterpret_cast<const char*>(d), n);
  });

  check(cli.session()->send_ping("p", 1) == 0, "[3] send_ping 受理");
  check(pump(srv, cli, [&] { return cli_pong == "p"; }), "[3] 自动 PONG 回到客户端");
  check(srv_ping == "p", "[3] 服务端 on_ping 负载");
  check(cli_pong == "p", "[3] 客户端 on_pong 负载");

  check(srv_frames.size() == 1, "[3] 服务端逐帧看到 1 帧");
  if (srv_frames.size() == 1) {
    const frame_rec r = srv_frames.at(0);
    check(r.opcode == ws_opcode::PING && r.fin, "[3] PING/fin=true");
    check(r.payload == "p" && r.masked, "[3] PING 负载与掩码");
  }
  check(cli_frames.size() == 1, "[3] 客户端逐帧看到 1 帧");
  if (cli_frames.size() == 1) {
    const frame_rec r = cli_frames.at(0);
    check(r.opcode == ws_opcode::PONG && r.fin, "[3] PONG/fin=true");
    check(r.payload == "p" && !r.masked, "[3] 服务端 PONG 不掩码");
  }

  cli.close();
  pump(srv, cli, [&] { return cli.session() == nullptr; });
}

// ---------------------------------------------------------------------------
// 4. 服务端 → 客户端：不掩码
// ---------------------------------------------------------------------------
void test_server_to_client() {
  uvcpp_ws_server srv;
  disable_compression(srv);
  if (srv.bind("127.0.0.1", 0) != 0) {
    check(false, "[4] bind");
    return;
  }
  const int port = server_port(srv);
  // 等服务端收到客户端的 "go" 再回 —— 客户端的 on_frame 必须先装好，
  // 否则服务器的第一帧可能赶在安装之前到达（会话建立与数据到达是两拍）。
  srv.on_connection([](uvcpp_ws_connection* c) {
    c->on_text([c](const std::string& m) {
      if (m == "go") c->send_text("srv", 3);
    });
  });
  srv.listen();

  uvcpp_ws_client cli;
  disable_compression(cli);
  frame_log cli_frames;
  std::string cli_text;
  cli.on_text([&](const std::string& m) { cli_text = m; });
  // connect **之前**装逐帧回调：客户端层会把它装到新会话上，不会漏掉
  // 对端一升级就推来的帧（这正是 #50 要的、而 session() 拿不到的能力）。
  cli.on_frame([&](const uvcpp_ws_frame& f) { cli_frames.add(f); });
  check(cli.connect("ws://127.0.0.1:" + std::to_string(port) + "/", nullptr) == 0,
        "[4] connect");
  check(pump(srv, cli, [&] { return cli.session() != nullptr; }), "[4] 会话建立");
  if (cli.session() == nullptr) return;
  check(cli.send_text("go", 2) == 0, "[4] 触发服务端回应");

  check(pump(srv, cli, [&] { return cli_text == "srv"; }), "[4] 收到服务端消息");
  check(cli_frames.size() == 1, "[4] 逐帧回调一次");
  if (cli_frames.size() == 1) {
    const frame_rec r = cli_frames.at(0);
    check(r.opcode == ws_opcode::TEXT && r.fin, "[4] TEXT/fin=true");
    check(!r.masked, "[4] 服务端发出的帧 masked = false");
    check(r.payload == "srv", "[4] 负载");
  }

  cli.close();
  pump(srv, cli, [&] { return cli.session() == nullptr; });
}

#if UVCPP_ZLIB_ENABLE
// ---------------------------------------------------------------------------
// 5. permessage-deflate + 分片：RSV1 只在首帧、逐帧可见、消息仍被解开
// ---------------------------------------------------------------------------
void test_compressed_fragmented() {
  uvcpp_ws_server srv;
  if (srv.bind("127.0.0.1", 0) != 0) {
    check(false, "[5] bind");
    return;
  }
  const int port = server_port(srv);
  uvcpp_ws_deflate_config cfg;   // 默认：15 bits、两端 context takeover
  srv.set_compression(cfg);

  frame_log srv_frames;
  std::string srv_text;
  srv.on_connection([&](uvcpp_ws_connection* c) {
    c->on_frame([&](const uvcpp_ws_frame& f) { srv_frames.add(f); });
    c->on_text([&](const std::string& m) { srv_text = m; });
  });
  srv.listen();

  uvcpp_ws_client cli;
  cli.set_compression(cfg);
  check(cli.connect("ws://127.0.0.1:" + std::to_string(port) + "/", nullptr) == 0,
        "[5] connect");
  check(pump(srv, cli, [&] { return cli.session() != nullptr; }), "[5] 会话建立");
  if (cli.session() == nullptr) return;
  check(cli.session()->is_compression_enabled(), "[5] 协商结果真的接进了连接层");

  // 用同一套默认参数（客户端方向）自己压一份 —— 这正是 send_frame 的用法：
  // RSV1 与压缩边界由调用方负责。
  uvcpp_ws_parser compressor;
  compressor.enable_compression(false, false, false, 15, 15);
  const std::string plain = "deflate-me";
  uvcpp_buf compressed;
  check(compressor.compress(reinterpret_cast<const uint8_t*>(plain.data()),
                            plain.size(), compressed) == 0,
        "[5] 构造压缩负载");
  check(compressed.size() > 0, "[5] 压缩负载非空");
  if (compressed.size() == 0) return;

  const std::size_t half = compressed.size() / 2;
  uvcpp_ws_frame f1;
  f1.opcode  = ws_opcode::TEXT;
  f1.fin     = false;
  f1.rsv1    = true;   // 消息首帧带压缩标志
  f1.payload = uvcpp_buf(compressed.get_const_data(), half);
  check(cli.session()->send_frame(f1) == 0, "[5] 压缩首片受理");

  uvcpp_ws_frame f2;
  f2.opcode  = ws_opcode::CONTINUATION;
  f2.fin     = true;
  f2.rsv1    = false;  // 只有首帧带 RSV1（RFC 7692 §7.2.2）
  f2.payload = uvcpp_buf(compressed.get_const_data() + half,
                         compressed.size() - half);
  check(cli.session()->send_frame(f2) == 0, "[5] 压缩续片受理");

  check(pump(srv, cli, [&] { return srv_text == plain; }), "[5] 解压后的消息到达");
  check(srv_text == plain, "[5] on_text 拿到解压后的文本");
  check(srv_frames.size() == 2, "[5] 逐帧回调两次");
  if (srv_frames.size() == 2) {
    const frame_rec a = srv_frames.at(0);
    const frame_rec b = srv_frames.at(1);
    check(a.opcode == ws_opcode::TEXT && a.rsv1 && !a.fin,
          "[5] 首帧 TEXT / RSV1 / fin=false");
    check(b.opcode == ws_opcode::CONTINUATION && !b.rsv1 && b.fin,
          "[5] 续帧 CONTINUATION / RSV1=0 / fin=true");
    check(a.payload.size() == half &&
              b.payload.size() == compressed.size() - half,
          "[5] 逐帧看到的是压缩字节（未被本回调解开）");
  }

  cli.close();
  pump(srv, cli, [&] { return cli.session() == nullptr; });
}
#endif  // UVCPP_ZLIB_ENABLE

}  // namespace

int main() {
  std::cout << "WebSocket raw frame API (#50)\n";
  test_plain_message();
  test_fragmented();
  test_ping_pong();
  test_server_to_client();
#if UVCPP_ZLIB_ENABLE
  test_compressed_fragmented();
#endif
  if (g_fail != 0) {
    std::cout << g_fail << " check(s) failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}

#else  // !UVCPP_WEB_ENABLE

int main() {
  std::cout << "SKIP: web disabled\n";
  return 0;
}

#endif  // UVCPP_WEB_ENABLE
