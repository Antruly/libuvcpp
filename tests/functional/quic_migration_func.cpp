/**
 * @file tests/functional/quic_migration_func.cpp
 * @brief QUIC 路径变化通知（#53）：NAT rebinding 之后服务端会报新路径。
 *
 * 装置：客户端连到本机一个 **UDP 中继**，中继用两个源端口把包转给服务端。
 * 握手完成后把中继切到第二个源端口 —— 对服务端来说，这就是一次 NAT
 * rebinding（同一个连接的包忽然从一个新的源地址来了）。
 *
 * 判据：
 * 1. 切换前数据报双向可达；
 * 2. 切换后服务端的 `on_path_validation(success=true)` 与 `on_path_changed`
 *    都会响，且报的端口是中继第二个端口的端口；
 * 3. 迁移之后同一条连接上的数据报仍然可达（服务端已经按新路径发包）。
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_QUIC_ENABLE

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_udp.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_common.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <req/uvcpp_udp_send.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>
#include <uvcpp/uvcpp_buf.h>

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

// ---------------------------------------------------------------------------
// 两个源端口的 UDP 中继：`a` 同时是客户端看到的那条路径；切换后客户端→服务端
// 的包改从 `b` 发出（服务端因此看到 NAT rebinding）。服务端→客户端的包永远
// 从 `a` 发回（客户端的对端地址不能变）。
// ---------------------------------------------------------------------------
struct udp_relay {
  uvcpp_udp a;
  uvcpp_udp b;
  struct sockaddr_in server;
  struct sockaddr_in client;
  bool have_client = false;
  bool use_b = false;
  int  port_a = 0;
  int  port_b = 0;

  explicit udp_relay(uvcpp_loop* loop) : a(loop), b(loop) {}

  bool start(int server_port) {
    std::memset(&server, 0, sizeof(server));
    std::memset(&client, 0, sizeof(client));
    struct sockaddr_in bind_addr;
    if (uv_ip4_addr("127.0.0.1", 0, &bind_addr) != 0) return false;
    if (a.bind(reinterpret_cast<const sockaddr*>(&bind_addr), 0) != 0) {
      return false;
    }
    if (b.bind(reinterpret_cast<const sockaddr*>(&bind_addr), 0) != 0) {
      return false;
    }
    struct sockaddr_in n;
    int l = sizeof(n);
    if (a.getsockname(reinterpret_cast<sockaddr*>(&n), &l) != 0) return false;
    port_a = ntohs(n.sin_port);
    l = sizeof(n);
    if (b.getsockname(reinterpret_cast<sockaddr*>(&n), &l) != 0) return false;
    port_b = ntohs(n.sin_port);
    uv_ip4_addr("127.0.0.1", server_port, &server);

    auto on_recv = [this](uvcpp_udp* u, ssize_t nread, const uv_buf_t* buf,
                          const sockaddr* from, unsigned) {
      if (nread <= 0 || buf == nullptr || buf->base == nullptr) return;
      const struct sockaddr_in* src =
          reinterpret_cast<const struct sockaddr_in*>(from);
      if (src != nullptr && src->sin_port == server.sin_port &&
          src->sin_addr.s_addr == server.sin_addr.s_addr) {
        if (have_client) {
          forward(a, buf->base, static_cast<size_t>(nread),
                  reinterpret_cast<const sockaddr*>(&client));
        }
        return;
      }
      if (src != nullptr) {
        client = *src;
        have_client = true;
      }
      forward(use_b ? b : a, buf->base, static_cast<size_t>(nread),
              reinterpret_cast<const sockaddr*>(&server));
      (void)u;
    };
    auto alloc = [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
      uvcpp_buf::alloc_buf(buf, sz > 0 ? sz : 4096);
    };
    a.recv_start(alloc, on_recv);
    b.recv_start(alloc, on_recv);
    return true;
  }

  void forward(uvcpp_udp& u, const char* data, size_t n,
               const struct sockaddr* dst) {
    uvcpp_buf copy(data, n);
    uv_buf_t* raw = copy.out_uv_buf();
    uvcpp_udp_send* req = new uvcpp_udp_send();
    req->set_uv_buf(raw, true);
    u.send(req, raw, 1, dst, [](uvcpp_udp_send* r, int) { delete r; });
  }
};

void test_migration() {
  uvcpp_loop loop;

  uvcpp_ssl_context server_ctx(tls_mode::SERVER, tls_version::TLS_1_3);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    check(false, "server cert");
    return;
  }
  uvcpp_ssl_context client_ctx(tls_mode::CLIENT, tls_version::TLS_1_3);
  client_ctx.set_verify_mode(tls_verify_mode::NONE);

  uvcpp_quic_server server(&loop);
  server.set_ssl_context(&server_ctx);
  if (server.bind("127.0.0.1", 0) != 0) {
    check(false, "server bind");
    return;
  }

  std::string server_got;
  int  server_dgram_count = 0;
  int  validation_ok = 0;
  int  validation_bad = 0;
  int  path_changed = 0;
  int  changed_port = 0;
  std::string changed_ip;
  uvcpp_quic_connection* server_conn = nullptr;

  const int lrc = server.listen([&](uvcpp_quic_connection* c) {
    server_conn = c;
    uvcpp_quic_connection::callbacks cbs;
    cbs.on_datagram = [&](uvcpp_quic_connection& conn, const uint8_t* data,
                          size_t len) {
      ++server_dgram_count;
      server_got.assign(reinterpret_cast<const char*>(data), len);
      conn.send_datagram(reinterpret_cast<const char*>(data), len);
    };
    cbs.on_path_validation = [&](uvcpp_quic_connection&, const std::string&,
                                 int, bool success) {
      if (success) {
        ++validation_ok;
      } else {
        ++validation_bad;
      }
    };
    cbs.on_path_changed = [&](uvcpp_quic_connection&, const std::string& ip,
                              int port) {
      ++path_changed;
      changed_ip   = ip;
      changed_port = port;
    };
    c->set_callbacks(cbs);
  });
  check(lrc == 0, "server listen");
  if (lrc != 0) return;
  const int server_port = server.configured_port();
  check(server_port > 0, "server port");

  udp_relay relay(&loop);
  check(relay.start(server_port), "relay start");
  if (relay.port_a <= 0 || relay.port_b <= 0) {
    check(false, "relay ports");
    return;
  }
  check(relay.port_a != relay.port_b, "relay 两个源端口不同");

  uvcpp_quic_client client(&loop);
  client.set_ssl_context(&client_ctx);

  std::string client_got;
  int  client_dgram_count = 0;
  bool connected = false;
  int  connect_status = -999;

  const int crc = client.connect("127.0.0.1", relay.port_a, [&](int status) {
    connected = true;
    connect_status = status;
  });
  check(crc == 0, "client connect 受理");
  if (crc != 0) return;

  uvcpp_quic_connection* cli_conn = client.connection();
  check(cli_conn != nullptr, "client.connection()");
  if (cli_conn == nullptr) return;
  uvcpp_quic_connection::callbacks cc;
  cc.on_datagram = [&](uvcpp_quic_connection&, const uint8_t* data,
                       size_t len) {
    ++client_dgram_count;
    client_got.assign(reinterpret_cast<const char*>(data), len);
  };
  cli_conn->set_callbacks(cc);

  check(uvcpp_test::wait_until(&loop, [&] { return connected; }, kDeadlineMs),
        "握手完成");
  check(connect_status == 0, "connect status = 0");

  // 切换前：双向数据报。
  check(cli_conn->send_datagram("before", 6) == 0, "发送 before");
  check(uvcpp_test::wait_until(
            &loop,
            [&] { return server_dgram_count > 0 && client_dgram_count > 0; },
            kDeadlineMs),
        "切换前数据报双向可达");
  check(client_got == "before", "切换前回声 \"" + client_got + "\"");

  // 切到第二个源端口：服务端看到新的源地址（NAT rebinding）。
  relay.use_b = true;
  check(cli_conn->send_datagram("after", 5) == 0, "发送 after");
  check(uvcpp_test::wait_until(
            &loop,
            [&] { return path_changed > 0 && validation_ok > 0; },
            kDeadlineMs),
        "服务端报出路径校验成功与路径变化");
  check(validation_bad == 0, "没有失败的路径校验");
  check(path_changed >= 1, "on_path_changed 至少一次");
  check(changed_ip == "127.0.0.1", "新路径 ip = " + changed_ip);
  check(changed_port == relay.port_b,
        "新路径端口 = " + std::to_string(changed_port) + "（应为 " +
            std::to_string(relay.port_b) + "）");
  check(client_got == "after", "迁移后回声 \"" + client_got + "\"");

  // 迁移之后继续可用。
  const int before = client_dgram_count;
  check(cli_conn->send_datagram("post", 4) == 0, "发送 post");
  check(uvcpp_test::wait_until(&loop,
                               [&] { return client_dgram_count > before; },
                               kDeadlineMs),
        "迁移后数据报仍可达");
}

}  // namespace

int main() {
  std::cout << "QUIC path migration (#53)" << std::endl;
  test_migration();
  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "all checks passed" << std::endl;
  return 0;
}

#else  // !UVCPP_QUIC_ENABLE

int main() {
  std::cout << "SKIP: quic disabled" << std::endl;
  return 0;
}

#endif
