/**
 * @file tests/functional/h2_ws_func.cpp
 * @brief WebSocket over HTTP/2（RFC 8441，extended CONNECT，#58）。
 *
 * 一条 h2c（明文、先验知识）连接上开一条 `:protocol = websocket` 的 CONNECT
 * 流，双方在流上跑 RFC 6455 帧：文本回显、PING→PONG、Close。
 *
 * **为什么要真 socket**：h2 的会话层与帧层在这里是同一段代码的两侧，而本用例
 * 要判的恰好是"帧有没有穿过 h2 的 DATA 帧走到对面"。两条 `uvcpp_h2_session`
 * 面对面的装置（`h2_session_func.cpp`）搬的是会话字节，判不到 `uvcpp_h2_ws_connection`
 * 与 `uvcpp_h2_connection` 之间那条接缝（流落点、发送方向保持开着、END_STREAM
 * 的时机）。所以这里起一条真的回环连接，两端都走完整的 `uvcpp_h2_connection`。
 *
 * 明文而不是 TLS：这条腿与 ALPN 无关（h2c 同样建服务端会话），TLS 那一层由
 * `web_ssl_h2_*` 覆盖 —— 在这里加一层握手只会把"帧没到"和"握手没成"搅在一起。
 */

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_NGHTTP2_ENABLE

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <handle/uvcpp_loop.h>
#include <http2/uvcpp_h2_connection.h>
#include <http2/uvcpp_h2_ws_connection.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

#include "loop_drain.h"
#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++g_fail;
    std::cerr << "FAIL: " << what << std::endl;
  } else {
    std::cout << "ok: " << what << std::endl;
  }
}

const int kDeadlineMs = 8000;

int server_port(uvcpp_tcp_server& server) {
  struct sockaddr_in name;
  std::memset(&name, 0, sizeof(name));
  int namelen = static_cast<int>(sizeof(name));
  if (server.get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&name),
                                    &namelen) != 0) {
    return 0;
  }
  return ntohs(name.sin_port);
}

// =========================================================================
// 纯会话层的两条闸（不开 socket）
// =========================================================================

/// `:protocol` + 请求体是互斥的：extended CONNECT 的载荷是流上的帧。
void test_session_guards() {
  uvcpp_h2_session client(false);
  uvcpp_h2_session::callbacks cbs;
  check(client.init(cbs) == 0, "会话 init");

  uvcpp_http_request req;
  req.method   = http_method::HTTP_CONNECT;
  req.protocol = "websocket";
  req.url      = "/chat";
  // 对端 SETTINGS 还没来 ⇒ 不许发 extended CONNECT（RFC 8441 §3）。
  check(client.submit_request(req, std::string()) == UV_EPROTONOSUPPORT,
        "对端未宣告 ENABLE_CONNECT_PROTOCOL 时同步拒发");

  // 带请求体的 extended CONNECT 是参数错误，**先于**协议闸判：CONNECT 的
  // 载荷是流上的帧，请求体那些字节没有地方可去。
  const int32_t r = client.submit_request(req, std::string("payload"));
  check(r == UV_EINVAL,
        "带请求体的 extended CONNECT 返回 UV_EINVAL（实际 " +
            std::to_string(r) + "）");
}

// =========================================================================
// 端到端
// =========================================================================

void test_ws() {
  uvcpp_loop loop;
  // 句柄都在本对象后面声明 ⇒ 析构顺序「句柄 → 本守卫 → 循环」，
  // 补上最后那一次把关闭回调拨完的 `uv_run`（否则整个循环对象漏掉）。
  uvcpp_test::loop_drain drain(&loop);

  uvcpp_tcp_server server(&loop);
  check(server.bind("127.0.0.1", 0) == 0, "server bind");
  const int port = server_port(server);
  check(port > 0, "server port");

  std::unique_ptr<uvcpp_h2_connection>    srv_h2;
  std::unique_ptr<uvcpp_h2_ws_connection> srv_ws;
  std::string server_got;
  std::string server_protocol;
  std::string server_path;
  bool        server_accept_failed = false;
  int         server_close_count   = 0;
  ws_close_code server_close_code  = ws_close_code::NORMAL;

  const int lrc = server.listen([&](uvcpp_tcp_client* client) {
    srv_h2.reset(new uvcpp_h2_connection(client, /*server_side=*/true));
    uvcpp_h2_session::callbacks h2cbs;
    h2cbs.on_request = [&](uvcpp_h2_session&, uvcpp_h2_stream& st, bool) {
      // 普通请求在这条用例里不该出现；出现就记一笔，让判据红在哪一句可查。
      if (st.request.protocol != "websocket") {
        server_accept_failed = true;
        return;
      }
      server_protocol = st.request.protocol;
      server_path     = st.request.url;

      uvcpp_h2_ws_connection::callbacks wc;
      wc.on_text = [&](const std::string& m) {
        server_got = m;
        if (srv_ws) srv_ws->send_text(m.data(), m.size());
      };
      // 对端（客户端）主动发 Close：这里必须**恰好一次**，而且不是 1006。
      wc.on_close = [&](ws_close_code code, const std::string&) {
        ++server_close_count;
        server_close_code = code;
      };
      srv_ws = uvcpp_h2_ws_connection::accept(*srv_h2, st.stream_id, st.request, wc);
      if (srv_ws == nullptr) server_accept_failed = true;
    };
    uvcpp_h2_connection::callbacks cc;
    cc.on_disconnect = [&](uvcpp_h2_connection&) {
      // 次序是契约：适配器先摘落点，h2 连接后销毁（`uvcpp_h2_ws_connection`
      // 不拥有 h2 连接，反向销毁会留下一个指向已释放对象的 `h2_`）。
      srv_ws.reset();
      srv_h2.reset();
    };
    const int rv = srv_h2->start(h2cbs, cc);
    if (rv != 0) {
      std::cerr << "server h2 start failed: " << rv << std::endl;
      ++g_fail;
      srv_h2.reset();
    }
  });
  check(lrc == 0, "server listen");
  if (lrc != 0) return;

  uvcpp_tcp_client client(&loop);
  std::unique_ptr<uvcpp_h2_connection>    cli_h2;
  std::unique_ptr<uvcpp_h2_ws_connection> cli_ws;
  bool connected = false;

  const int crc = client.connect("127.0.0.1", port, [&](int status) {
    connected = status == 0;
    if (!connected) return;
    cli_h2.reset(new uvcpp_h2_connection(&client, /*server_side=*/false));
    uvcpp_h2_session::callbacks h2cbs;
    uvcpp_h2_connection::callbacks cc;
    cc.on_disconnect = [&](uvcpp_h2_connection&) {};
    const int rv = cli_h2->start(h2cbs, cc);
    if (rv != 0) {
      std::cerr << "client h2 start failed: " << rv << std::endl;
      ++g_fail;
      cli_h2.reset();
    }
  });
  check(crc == 0, "client connect 受理");
  if (crc != 0) return;

  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "TCP 连上");

  uvcpp_h2_ws_connection::callbacks wc;
  std::string client_got;
  std::string client_pong;
  int         close_count = 0;
  ws_close_code close_code = ws_close_code::NORMAL;
  wc.on_text  = [&](const std::string& m) { client_got = m; };
  wc.on_pong  = [&](const uint8_t* d, size_t n) {
    client_pong.assign(reinterpret_cast<const char*>(d), n);
  };
  wc.on_close = [&](ws_close_code code, const std::string&) {
    ++close_count;
    close_code = code;
  };

  // 对端 SETTINGS 到达之前 `connect()` 同步返回 nullptr —— 这里像真实客户端
  // 那样等它：每次失败都发生在提交之前，所以重试不产生任何流。
  check(uvcpp_test::wait_until(
            &loop,
            [&] {
              cli_ws = uvcpp_h2_ws_connection::connect(*cli_h2, "/chat",
                                                       "localhost", wc);
              return cli_ws != nullptr;
            },
            kDeadlineMs),
        "客户端 connect websocket（等对端 SETTINGS 后成功）");

  if (cli_ws == nullptr) {
    // 下面每一条都依赖它；提前收摊，别把空指针解引用演成崩溃。
    return;
  }
  check(cli_ws->stream_id() > 0, "ws stream id");

  check(uvcpp_test::wait_until(
            &loop, [&] { return server_protocol == "websocket"; }, kDeadlineMs),
        "服务端看到 :protocol = websocket");
  check(server_path == "/chat",
        "服务端看到 :path = /chat（实际 \"" + server_path + "\"）");

  {
    const int rc = cli_ws->send_text("hello-h2", 8);
    check(rc == 0, "发送文本（rc=" + std::to_string(rc) + "）");
  }
  const bool echo_ok = uvcpp_test::wait_until(
      &loop, [&] { return client_got == "hello-h2"; }, kDeadlineMs);
  check(echo_ok, "文本回显（实际 \"" + client_got + "\"）");
  check(server_got == "hello-h2", "服务端收到文本");

  check(cli_ws->send_text("second", 6) == 0, "发送第二段文本");
  // 判据先落到 bool 上再拼消息：`check(f(), "..." + s)` 里两个实参的求值次序
  // 是**未指定**的，拼消息那一半可能读到等待之前的值（本用例踩过：结果是对的，
  // 失败消息却打印上一段文本）。
  const bool second_ok = uvcpp_test::wait_until(
      &loop, [&] { return server_got == "second"; }, kDeadlineMs);
  check(second_ok, "第二段文本到达服务端（实际 \"" + server_got + "\"）");

  check(cli_ws->send_ping("p", 1) == 0, "发送 PING");
  const bool pong_ok = uvcpp_test::wait_until(
      &loop, [&] { return client_pong == "p"; }, kDeadlineMs);
  check(pong_ok, "收到 PONG（实际 \"" + client_pong + "\"）");

  check(cli_ws->close(ws_close_code::NORMAL, "bye") == 0, "发送 Close");
  check(!cli_ws->is_open(), "本地关闭后 is_open = false");
  // **本端主动关不补 `on_close`**（与 h1 的 `uvcpp_ws_connection` 同一条语义：
  // 调用方自己发起的，它已经知道了）。所以客户端这一侧恰好是 0。
  check(close_count == 0, "本端主动关闭不补 on_close（实际 " +
                              std::to_string(close_count) + "）");
  // 对端那侧相反：Close 是**收到的**，必须恰好一次、且是原样的 1000。
  check(uvcpp_test::wait_until(&loop, [&] { return server_close_count == 1; },
                               kDeadlineMs),
        "服务端 on_close 恰好一次（实际 " +
            std::to_string(server_close_count) + "）");
  check(server_close_code == ws_close_code::NORMAL, "服务端收到的 Close 码是 1000");

  check(!server_accept_failed, "服务端 accept 没走错路");

  // 次序：适配器 → h2 连接 → 底层连接。
  cli_ws.reset();
  cli_h2.reset();
  server.stop();
  loop.run(UV_RUN_NOWAIT);
}

/// 与 h3 那条腿同形：帧回调里把会话删掉是合法用法（应用收到一条错误帧就把整条
/// 会话收摊）。两帧躺在同一批数据里时，旧实现会拿已析构的解析器解第二帧。
void test_destroy_in_frame_callback() {
  uvcpp_loop loop;
  uvcpp_test::loop_drain drain(&loop);

  uvcpp_tcp_server server(&loop);
  check(server.bind("127.0.0.1", 0) == 0, "destroy: server bind");
  const int port = server_port(server);
  check(port > 0, "destroy: server port");

  std::unique_ptr<uvcpp_h2_connection>    srv_h2;
  std::unique_ptr<uvcpp_h2_ws_connection> srv_ws;
  std::vector<std::string> server_texts;

  const int lrc = server.listen([&](uvcpp_tcp_client* client) {
    srv_h2.reset(new uvcpp_h2_connection(client, /*server_side=*/true));
    uvcpp_h2_session::callbacks h2cbs;
    h2cbs.on_request = [&](uvcpp_h2_session&, uvcpp_h2_stream& st, bool) {
      uvcpp_h2_ws_connection::callbacks wc;
      wc.on_text = [&](const std::string& message) {
        server_texts.push_back(message);
        // The application hangs up from inside the frame callback.
        srv_ws.reset();
      };
      wc.on_error = [](int, const std::string&) {};
      srv_ws = uvcpp_h2_ws_connection::accept(*srv_h2, st.stream_id,
                                              st.request, wc);
      check(srv_ws != nullptr, "destroy: server accept websocket");
    };
    uvcpp_h2_connection::callbacks cc;
    cc.on_disconnect = [&](uvcpp_h2_connection&) {
      srv_ws.reset();
      srv_h2.reset();
    };
    if (srv_h2->start(h2cbs, cc) != 0) {
      std::cerr << "destroy: server h2 start failed" << std::endl;
      ++g_fail;
    }
  });
  check(lrc == 0, "destroy: server listen");
  if (lrc != 0) return;

  uvcpp_tcp_client client(&loop);
  std::unique_ptr<uvcpp_h2_connection>    cli_h2;
  std::unique_ptr<uvcpp_h2_ws_connection> cli_ws;
  bool connected = false;

  const int crc = client.connect("127.0.0.1", port, [&](int status) {
    connected = status == 0;
    if (!connected) return;
    cli_h2.reset(new uvcpp_h2_connection(&client, /*server_side=*/false));
    uvcpp_h2_session::callbacks h2cbs;
    uvcpp_h2_connection::callbacks cc;
    cc.on_disconnect = [](uvcpp_h2_connection&) {};
    if (cli_h2->start(h2cbs, cc) != 0) {
      std::cerr << "destroy: client h2 start failed" << std::endl;
      ++g_fail;
      cli_h2.reset();
    }
  });
  check(crc == 0, "destroy: client connect accepted");
  if (crc != 0) return;
  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "destroy: tcp connected");

  uvcpp_h2_ws_connection::callbacks wc;
  wc.on_close = [](ws_close_code, const std::string&) {};
  wc.on_error = [](int, const std::string&) {};
  check(uvcpp_test::wait_until(
            &loop,
            [&] {
              cli_ws = uvcpp_h2_ws_connection::connect(*cli_h2, "/chat",
                                                       "localhost", wc);
              return cli_ws != nullptr;
            },
            kDeadlineMs),
        "destroy: client websocket");
  if (cli_ws == nullptr) return;
  check(uvcpp_test::wait_until(&loop, [&] { return srv_ws != nullptr; },
                               kDeadlineMs),
        "destroy: server side is up");

  // Two text frames in one stream write: they arrive in one `on_data` call,
  // which is the shape that crashed.
  std::string frames;
  for (const char* message : {"one", "two"}) {
    uvcpp_ws_frame frame;
    frame.opcode  = ws_opcode::TEXT;
    frame.fin     = true;
    frame.masked  = false;
    frame.payload = uvcpp_buf(message, std::strlen(message));
    const std::size_t size = uvcpp_ws_parser::calc_frame_size(frame);
    std::string bytes(size, '\0');
    uvcpp_ws_parser::build_frame(&bytes[0], frame);
    frames += bytes;
  }
  check(cli_h2->send_data(cli_ws->stream_id(), frames.data(), frames.size(),
                          /*end_stream=*/false, nullptr) == 0,
        "destroy: two frames written at once");
  check(uvcpp_test::wait_until(&loop, [&] { return !server_texts.empty(); },
                               kDeadlineMs),
        "destroy: the first frame arrives");
  // The second frame must not reach the session the first one destroyed.
  check(server_texts.size() == 1 && server_texts[0] == "one",
        "destroy: the frame after the close is dropped");
  check(srv_ws == nullptr, "destroy: the server session is gone");

  cli_ws.reset();
  cli_h2.reset();
  server.stop();
  loop.run(UV_RUN_NOWAIT);
}

}  // namespace

int main() {
  std::cout << "WebSocket over HTTP/2 (RFC 8441, #58)" << std::endl;
  test_session_guards();
  test_ws();
  test_destroy_in_frame_callback();
  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "all checks passed" << std::endl;
  return 0;
}

#else  // !UVCPP_NGHTTP2_ENABLE

int main() {
  std::cout << "SKIP: nghttp2 disabled" << std::endl;
  return 0;
}

#endif
