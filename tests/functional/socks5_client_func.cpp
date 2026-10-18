/**
 * @file tests/functional/socks5_client_func.cpp
 * @brief SOCKS5 客户端 ↔ 服务端回环：CONNECT / BIND / UDP ASSOCIATE。
 *
 * 结构：服务端（SOCKS5 代理、TCP echo、UDP echo）各自跑在后台线程自己的
 * 循环上；主线程用同步 API 驱动客户端。这样每条腿都是真的跨线程网络往返，
 * 不是把两端塞进一条循环里"自己跟自己说话"。
 *
 * 本用例不依赖外部网络，也不需要本机装任何代理。
 */

#include <net/uvcpp_socks5_client.h>
#include <net/uvcpp_socks5_server.h>
#include <net/uvcpp_socks5_udp.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <net/uvcpp_udp_client.h>

#include <handle/uvcpp_loop.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool pump_until(uvcpp::uvcpp_loop* loop, const std::function<bool()>& done,
                int timeout_ms) {
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  while (!done()) {
    loop->run(UV_RUN_NOWAIT);
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count() > timeout_ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  loop->run(UV_RUN_NOWAIT);
  return true;
}

void pump_for(uvcpp::uvcpp_loop* loop, int ms) {
  pump_until(loop, []() { return false; }, ms);
}

// -------------------------------------------------------------------------
// 后台 SOCKS5 代理
// -------------------------------------------------------------------------
struct SocksServer {
  uvcpp::uvcpp_socks5_server_options opts;
  std::thread thread;
  std::atomic<bool> ready;
  std::atomic<bool> stop;
  std::atomic<int>  sessions;
  std::atomic<int>  last_status;
  int               port;

  SocksServer() : ready(false), stop(false), sessions(0), last_status(0), port(0) {}

  void start() {
    thread = std::thread([this]() {
      uvcpp::uvcpp_socks5_server srv;
      srv.set_options(opts);
      srv.bind("127.0.0.1", 0);
      port = srv.port();
      srv.listen([this](const uvcpp::uvcpp_socks5_session_info& info) {
        sessions.fetch_add(1);
        last_status.store(info.status);
      });
      ready.store(true);

      while (!stop.load()) {
        srv.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      srv.close();
      for (int i = 0; i < 50; ++i) {
        srv.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    while (!ready.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  ~SocksServer() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// -------------------------------------------------------------------------
// 后台 TCP echo
// -------------------------------------------------------------------------
struct TcpEchoServer {
  std::thread thread;
  std::atomic<bool> ready;
  std::atomic<bool> stop;
  int               port;

  TcpEchoServer() : ready(false), stop(false), port(0) {}

  void start() {
    thread = std::thread([this]() {
      uvcpp::uvcpp_tcp_server server;
      server.bind("127.0.0.1", 0);

      struct sockaddr_in name;
      int namelen = static_cast<int>(sizeof(name));
      server.get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&name),
                                    &namelen);
      port = ntohs(name.sin_port);

      server.listen([](uvcpp::uvcpp_tcp_client* client) {
        // 每个连接一份写队列：tcp_client 同一时刻只允许一笔异步写在途，
        // 且在途时再写是 **拒收**（UV_EALREADY）而不是排队。
        //
        // 状态用 shared_ptr 持有：关闭观察者与写完成回调谁先跑都安全，
        // 观察者只把 closed 置真，晚到的写回调看到它就撒手。
        struct echo_state {
          uvcpp::uvcpp_tcp_client* client;
          std::deque<std::string>  queue;
          bool                     in_flight;
          bool                     closed;

          echo_state() : client(nullptr), in_flight(false), closed(false) {}

          void pump(const std::shared_ptr<echo_state>& self) {
            while (!closed && !in_flight && !queue.empty()) {
              const std::string chunk = queue.front();
              queue.pop_front();
              in_flight = true;
              const int rc = client->write(chunk.data(), chunk.size(),
                                           [self](int) {
                                             self->in_flight = false;
                                             self->pump(self);
                                           });
              if (rc != 0) {
                // 连接已经不可写：丢掉这一段，后续的读回调也很快会停。
                in_flight = false;
                queue.clear();
                return;
              }
            }
          }
        };

        std::shared_ptr<echo_state> st = std::make_shared<echo_state>();
        st->client = client;

        client->add_close_observer([st]() { st->closed = true; });
        client->read_start([st](uvcpp::uvcpp_buf* b) {
          if (b == nullptr || b->size() == 0) return;
          st->queue.push_back(std::string(b->get_const_data(), b->size()));
          st->pump(st);
        });
      });
      ready.store(true);
      while (!stop.load()) {
        server.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    while (!ready.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  ~TcpEchoServer() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// -------------------------------------------------------------------------
// 后台 UDP echo
// -------------------------------------------------------------------------
struct UdpEchoServer {
  std::thread thread;
  std::atomic<bool> ready;
  std::atomic<bool> stop;
  int               port;

  UdpEchoServer() : ready(false), stop(false), port(0) {}

  void start() {
    thread = std::thread([this]() {
      uvcpp::uvcpp_loop loop;
      uvcpp::uvcpp_udp_client udp(&loop);
      udp.bindIpv4("127.0.0.1", 0);
      std::string ip;
      int         p = 0;
      udp.getLocalAddrs(ip, p);
      port = p;

      uvcpp::uvcpp_udp_client* self = &udp;
      udp.recv_start([self](uvcpp::uvcpp_buf* b, const char* from_ip,
                            int from_port) {
        if (b == nullptr || from_ip == nullptr) return;
        self->send(from_ip, from_port, b->get_const_data(), b->size(),
                   [](int) {});
      });

      ready.store(true);
      while (!stop.load()) {
        loop.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    while (!ready.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  ~UdpEchoServer() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

bool test_connect_no_auth() {
  std::cout << "[socks5] connect_no_auth start\n";

  TcpEchoServer echo;
  echo.start();
  SocksServer proxy;
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_socks5_client client(&loop);

  int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5] connect_wait failed: " << uv_strerror(rc) << "\n";
    return false;
  }

  rc = client.connect_target_wait("127.0.0.1", echo.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5] connect_target_wait failed: " << uv_strerror(rc)
              << " (" << client.last_error() << ")\n";
    return false;
  }

  std::string got;
  client.read_start([&got](uvcpp::uvcpp_buf* b) {
    if (b != nullptr && b->size() > 0) {
      got.append(b->get_const_data(), b->size());
    }
  });

  rc = client.write_wait("ping", 4, 5000);
  if (rc != 0) {
    std::cout << "[socks5] write_wait failed: " << uv_strerror(rc) << "\n";
    return false;
  }

  if (!pump_until(&loop, [&got]() { return got == "ping"; }, 5000)) {
    std::cout << "[socks5] echo not received, got '" << got << "'\n";
    return false;
  }

  client.close();
  pump_for(&loop, 20);
  std::cout << "[socks5] connect_no_auth ok\n";
  return true;
}

bool test_connect_domain_target() {
  std::cout << "[socks5] connect_domain_target start\n";

  TcpEchoServer echo;
  echo.start();
  SocksServer proxy;
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_socks5_client client(&loop);

  int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc == 0) rc = client.connect_target_wait("localhost", echo.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5] domain connect failed: " << uv_strerror(rc) << " ("
              << client.last_error() << ")\n";
    return false;
  }

  std::string got;
  client.read_start([&got](uvcpp::uvcpp_buf* b) {
    if (b != nullptr && b->size() > 0) {
      got.append(b->get_const_data(), b->size());
    }
  });
  client.write_wait("dom", 3, 5000);
  if (!pump_until(&loop, [&got]() { return got == "dom"; }, 5000)) {
    std::cout << "[socks5] domain echo failed, got '" << got << "'\n";
    return false;
  }

  client.close();
  pump_for(&loop, 20);
  std::cout << "[socks5] connect_domain_target ok\n";
  return true;
}

bool test_connect_auth() {
  std::cout << "[socks5] connect_auth start\n";

  TcpEchoServer echo;
  echo.start();
  SocksServer proxy;
  proxy.opts.allow_no_auth           = false;
  proxy.opts.allow_username_password = true;
  proxy.opts.authenticator = [](const std::string& u, const std::string& p) {
    return u == "alice" && p == "secret";
  };
  proxy.start();

  // --- 正确口令 ---
  {
    uvcpp::uvcpp_loop loop;
    uvcpp::uvcpp_socks5_client client(&loop);
    client.set_credentials("alice", "secret");

    int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
    if (rc == 0) rc = client.connect_target_wait("127.0.0.1", echo.port, 5000);
    if (rc != 0) {
      std::cout << "[socks5] auth success path failed: " << uv_strerror(rc)
                << " (" << client.last_error() << ")\n";
      return false;
    }
    client.close();
    pump_for(&loop, 20);
  }

  // --- 错误口令 ---
  {
    uvcpp::uvcpp_loop loop;
    uvcpp::uvcpp_socks5_client client(&loop);
    client.set_credentials("alice", "wrong");

    const int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
    if (rc == 0) {
      std::cout << "[socks5] wrong password unexpectedly accepted\n";
      return false;
    }
    if (client.last_error().empty()) {
      std::cout << "[socks5] wrong password failed without last_error()\n";
      return false;
    }
    client.close();
    pump_for(&loop, 20);
  }

  // --- 不给口令（只有 no-auth 一种方法，服务器不接受） ---
  {
    uvcpp::uvcpp_loop loop;
    uvcpp::uvcpp_socks5_client client(&loop);
    const int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
    if (rc == 0) {
      std::cout << "[socks5] method negotiation unexpectedly succeeded\n";
      return false;
    }
    client.close();
    pump_for(&loop, 20);
  }

  std::cout << "[socks5] connect_auth ok\n";
  return true;
}

bool test_bind() {
  std::cout << "[socks5] bind start\n";

  SocksServer proxy;
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_socks5_client client(&loop);

  int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5] bind: connect failed: " << uv_strerror(rc) << "\n";
    return false;
  }

  uvcpp::uvcpp_socks5_endpoint bound;
  rc = client.bind_wait("127.0.0.1", 0, bound, 5000);
  if (rc != 0 || bound.port == 0) {
    std::cout << "[socks5] bind_wait failed: " << uv_strerror(rc) << " ("
              << client.last_error() << ")\n";
    return false;
  }

  std::string got;
  client.read_start([&got](uvcpp::uvcpp_buf* b) {
    if (b != nullptr && b->size() > 0) {
      got.append(b->get_const_data(), b->size());
    }
  });

  // 对端连到代理给的绑定地址。
  uvcpp::uvcpp_tcp_client peer(&loop);
  rc = peer.connect_wait(bound.host.c_str(), bound.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5] peer connect to " << bound.host << ":" << bound.port
              << " failed: " << uv_strerror(rc) << "\n";
    return false;
  }

  uvcpp::uvcpp_socks5_endpoint peer_ep;
  rc = client.wait_for_peer(peer_ep, 5000);
  if (rc != 0) {
    std::cout << "[socks5] wait_for_peer failed: " << uv_strerror(rc) << "\n";
    return false;
  }

  // 对端 → 客户端
  rc = peer.write_wait("hello", 5, 5000);
  if (rc != 0 || !pump_until(&loop, [&got]() { return got == "hello"; }, 5000)) {
    std::cout << "[socks5] bind peer->client relay failed, got '" << got
              << "'\n";
    return false;
  }

  // 客户端 → 对端
  std::string back;
  peer.read_start([&back](uvcpp::uvcpp_buf* b) {
    if (b != nullptr && b->size() > 0) {
      back.append(b->get_const_data(), b->size());
    }
  });
  rc = client.write_wait("world", 5, 5000);
  if (rc != 0 || !pump_until(&loop, [&back]() { return back == "world"; }, 5000)) {
    std::cout << "[socks5] bind client->peer relay failed, got '" << back
              << "'\n";
    return false;
  }

  client.close();
  pump_for(&loop, 20);
  std::cout << "[socks5] bind ok\n";
  return true;
}

bool test_udp_associate() {
  std::cout << "[socks5] udp_associate start\n";

  UdpEchoServer echo;
  echo.start();
  SocksServer proxy;
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_socks5_client client(&loop);

  int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5] udp: connect failed: " << uv_strerror(rc) << "\n";
    return false;
  }

  uvcpp::uvcpp_socks5_endpoint relay;
  rc = client.udp_associate_wait(relay, 5000);
  if (rc != 0 || client.udp() == nullptr) {
    std::cout << "[socks5] udp_associate_wait failed: " << uv_strerror(rc)
              << " (" << client.last_error() << ")\n";
    return false;
  }

  std::string got;
  rc = client.udp()->recv_start(
      [&got](int, const uvcpp::uvcpp_socks5_endpoint&,
             const void* data, std::size_t len) {
        if (data != nullptr && len > 0) {
          got.append(static_cast<const char*>(data), len);
        }
      });
  if (rc != 0) {
    std::cout << "[socks5] udp recv_start failed: " << uv_strerror(rc) << "\n";
    return false;
  }

  rc = client.udp()->send_to("127.0.0.1", echo.port, "udp-hello", 9);
  if (rc != 0) {
    std::cout << "[socks5] udp send_to failed: " << uv_strerror(rc) << "\n";
    return false;
  }
  if (!pump_until(&loop, [&got]() { return got == "udp-hello"; }, 5000)) {
    std::cout << "[socks5] udp echo failed, got '" << got << "'\n";
    return false;
  }

  client.close();
  pump_for(&loop, 20);
  std::cout << "[socks5] udp_associate ok\n";
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_connect_no_auth() && ok;
  ok = test_connect_domain_target() && ok;
  ok = test_connect_auth() && ok;
  ok = test_bind() && ok;
  ok = test_udp_associate() && ok;

  if (!ok) {
    std::cout << "[socks5] FAILED\n";
    return 1;
  }
  std::cout << "[socks5] ALL PASSED\n";
  return 0;
}
