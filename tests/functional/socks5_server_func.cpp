/**
 * @file tests/functional/socks5_server_func.cpp
 * @brief SOCKS5 服务端的策略与失败路径：REP 映射、目标白名单、握手超时、
 *        并发会话上限。
 *
 * 回环（CONNECT / BIND / UDP）在 `socks5_client_func.cpp` 里；这里只钉
 * "被拒的时候回什么、什么时候关"。
 */

#include <net/uvcpp_socks5_client.h>
#include <net/uvcpp_socks5_server.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>

#include <handle/uvcpp_loop.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
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

bool wait_until(const std::function<bool()>& done, int timeout_ms) {
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  while (!done()) {
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count() > timeout_ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

struct SocksServer {
  uvcpp::uvcpp_socks5_server_options opts;
  std::thread                      thread;
  std::atomic<bool>                ready;
  std::atomic<bool>                stop;
  std::atomic<int>                 sessions;
  std::atomic<int>                 last_status;
  std::atomic<std::size_t>         active;
  int                              port;

  SocksServer()
      : ready(false), stop(false), sessions(0), last_status(0), active(0), port(0) {}

  void start() {
    thread = std::thread([this]() {
      uvcpp::uvcpp_socks5_server srv;
      srv.set_options(opts);
      srv.bind("127.0.0.1", 0);
      port = srv.port();
      srv.listen([this](const uvcpp::uvcpp_socks5_session_info& info) {
        last_status.store(info.status);
        sessions.fetch_add(1);
      });
      ready.store(true);

      while (!stop.load()) {
        srv.run(UV_RUN_NOWAIT);
        active.store(srv.connection_count());
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

// 拿一个"刚刚还开着、现在没人监听"的端口：绑 0 取端口，然后析构监听器。
int closed_port() {
  uvcpp::uvcpp_loop      loop;
  uvcpp::uvcpp_tcp_server server(&loop);
  server.bind("127.0.0.1", 0);

  struct sockaddr_in name;
  int namelen = static_cast<int>(sizeof(name));
  server.get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&name),
                                &namelen);
  const int port = ntohs(name.sin_port);

  server.stop();
  pump_for(&loop, 20);
  return port;
}

bool test_refused_mapping() {
  std::cout << "[socks5-server] refused_mapping start\n";

  const int dead = closed_port();
  if (dead <= 0) {
    std::cout << "[socks5-server] could not obtain a closed port\n";
    return false;
  }

  SocksServer proxy;
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_socks5_client client(&loop);

  int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc == 0) rc = client.connect_target_wait("127.0.0.1", dead, 5000);
  if (rc != UV_ECONNREFUSED) {
    std::cout << "[socks5-server] expected ECONNREFUSED, got "
              << uv_strerror(rc) << " (" << client.last_error() << ")\n";
    return false;
  }
  if (client.last_error().find("refused") == std::string::npos) {
    std::cout << "[socks5-server] last_error lacks 'refused': "
              << client.last_error() << "\n";
    return false;
  }

  client.close();
  pump_for(&loop, 20);
  std::cout << "[socks5-server] refused_mapping ok\n";
  return true;
}

bool test_disabled_commands() {
  std::cout << "[socks5-server] disabled_commands start\n";

  SocksServer proxy;
  proxy.opts.allow_connect = false;
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_socks5_client client(&loop);

  int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc == 0) rc = client.connect_target_wait("127.0.0.1", 80, 5000);
  if (rc != UV_ENOTSUP) {
    std::cout << "[socks5-server] expected ENOTSUP for disabled CONNECT, got "
              << uv_strerror(rc) << " (" << client.last_error() << ")\n";
    return false;
  }

  client.close();
  pump_for(&loop, 20);
  std::cout << "[socks5-server] disabled_commands ok\n";
  return true;
}

bool test_target_denied() {
  std::cout << "[socks5-server] target_denied start\n";

  SocksServer proxy;
  proxy.opts.allow_target = [](const uvcpp::uvcpp_socks5_endpoint& ep) {
    return ep.port != 9999;
  };
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_socks5_client client(&loop);

  int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc == 0) rc = client.connect_target_wait("127.0.0.1", 9999, 5000);
  if (rc != UV_EACCES) {
    std::cout << "[socks5-server] expected EACCES, got " << uv_strerror(rc)
              << " (" << client.last_error() << ")\n";
    return false;
  }
  if (!wait_until([&proxy]() { return proxy.sessions.load() > 0; }, 2000)) {
    std::cout << "[socks5-server] session callback never fired\n";
    return false;
  }
  if (proxy.last_status.load() != UV_EACCES) {
    std::cout << "[socks5-server] session status = " << proxy.last_status.load()
              << ", expected " << UV_EACCES << "\n";
    return false;
  }

  client.close();
  pump_for(&loop, 20);
  std::cout << "[socks5-server] target_denied ok\n";
  return true;
}

bool test_handshake_timeout() {
  std::cout << "[socks5-server] handshake_timeout start\n";

  SocksServer proxy;
  proxy.opts.handshake_timeout_ms = 200;
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_tcp_client raw(&loop);

  std::atomic<bool> closed(false);
  raw.add_close_observer([&closed]() { closed.store(true); });

  const int rc = raw.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5-server] raw connect failed: " << uv_strerror(rc)
              << "\n";
    return false;
  }
  // 必须有人读，libuv 才会把对端的 FIN 变成一次 `UV_EOF` 通知；不读的连接
  // 看不见对端关闭，关闭观察者也就不会响。
  raw.read_start([](uvcpp::uvcpp_buf*) {});
  // 一个字也不发：服务端应在 200 ms 后把控制连接关掉。
  if (!pump_until(&loop, [&closed]() { return closed.load(); }, 5000)) {
    std::cout << "[socks5-server] idle handshake was not closed\n";
    return false;
  }

  std::cout << "[socks5-server] handshake_timeout ok\n";
  return true;
}

bool test_slot_release_raw() {
  std::cout << "[socks5-server] slot_release_raw start\n";

  SocksServer proxy;
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_tcp_client raw(&loop);
  int rc = raw.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5-server] raw connect failed: " << uv_strerror(rc) << "\n";
    return false;
  }
  raw.read_start([](uvcpp::uvcpp_buf*) {});

  if (!wait_until([&proxy]() { return proxy.active.load() == 1; }, 5000)) {
    std::cout << "[socks5-server] session was not counted (active="
              << proxy.active.load() << ")\n";
    return false;
  }

  raw.close();
  if (!wait_until([&proxy]() { return proxy.active.load() == 0; }, 5000)) {
    std::cout << "[socks5-server] raw close left the slot busy (active="
              << proxy.active.load() << ")\n";
    return false;
  }

  std::cout << "[socks5-server] slot_release_raw ok\n";
  return true;
}

bool test_max_connections() {
  std::cout << "[socks5-server] max_connections start\n";

  SocksServer proxy;
  proxy.opts.max_connections = 1;
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_socks5_client first(&loop);

  int rc = first.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5-server] first session failed: " << uv_strerror(rc)
              << "\n";
    return false;
  }

  uvcpp::uvcpp_socks5_client second(&loop);
  rc = second.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc == 0) {
    std::cout << "[socks5-server] second session was accepted over the limit\n";
    return false;
  }

  // 第一条还活着：把它关掉之后第二条的槽位应该空出来。
  first.close();
  if (!pump_until(&loop, [&proxy]() { return proxy.active.load() == 0; }, 5000)) {
    std::cout << "[socks5-server] session slot never released (active="
              << proxy.active.load() << ")\n";
    return false;
  }

  uvcpp::uvcpp_socks5_client third(&loop);
  rc = third.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5-server] slot was not released: " << uv_strerror(rc)
              << "\n";
    return false;
  }

  third.close();
  pump_for(&loop, 20);
  std::cout << "[socks5-server] max_connections ok\n";
  return true;
}

// -------------------------------------------------------------------------
// 后台 TCP echo：链式转发用例的目标
// -------------------------------------------------------------------------
struct EchoTarget {
  std::thread thread;
  std::atomic<bool> ready;
  std::atomic<bool> stop;
  int port;

  EchoTarget() : ready(false), stop(false), port(0) {}

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
        struct echo_state {
          uvcpp::uvcpp_tcp_client* client;
          std::deque<std::string> queue;
          bool in_flight;
          bool closed;
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

  ~EchoTarget() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

bool test_route_target_upstream() {
  std::cout << "[socks5-server] route_target_upstream start\n";

  EchoTarget via_proxy;
  EchoTarget direct;
  via_proxy.start();
  direct.start();

  SocksServer upstream;  // plain SOCKS5 proxy used as the upstream hop
  upstream.start();

  std::atomic<int> route_calls{0};
  SocksServer proxy;
  proxy.opts.route_target =
      [&](const uvcpp::uvcpp_socks5_endpoint& target,
          uvcpp::uvcpp_socks5_upstream* out) {
        route_calls.fetch_add(1);
        if (target.port != via_proxy.port) return false;  // stay direct
        out->host = "127.0.0.1";
        out->port = upstream.port;
        return true;
      };
  proxy.start();

  uvcpp::uvcpp_loop loop;

  // 1) The hook selects the upstream: data must round-trip through the chain.
  {
    uvcpp::uvcpp_socks5_client client(&loop);
    int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
    if (rc == 0) {
      rc = client.connect_target_wait("127.0.0.1", via_proxy.port, 5000);
    }
    if (rc != 0) {
      std::cout << "[socks5-server] upstream chain connect failed: "
                << uv_strerror(rc) << "\n";
      return false;
    }
    std::string got;
    client.read_start([&got](uvcpp::uvcpp_buf* b) {
      if (b != nullptr && b->size() > 0) {
        got.append(b->get_const_data(), b->size());
      }
    });
    rc = client.write_wait("upstream", 8, 5000);
    if (rc != 0 ||
        !pump_until(&loop, [&got]() { return got == "upstream"; }, 5000)) {
      std::cout << "[socks5-server] upstream chain echo failed, got '" << got
                << "'\n";
      return false;
    }
    client.close();
    pump_for(&loop, 20);
  }

  // 2) The hook returns false: the same server still dials directly.
  {
    uvcpp::uvcpp_socks5_client client(&loop);
    int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
    if (rc == 0) rc = client.connect_target_wait("127.0.0.1", direct.port, 5000);
    if (rc != 0) {
      std::cout << "[socks5-server] direct fallback connect failed: "
                << uv_strerror(rc) << "\n";
      return false;
    }
    std::string got;
    client.read_start([&got](uvcpp::uvcpp_buf* b) {
      if (b != nullptr && b->size() > 0) {
        got.append(b->get_const_data(), b->size());
      }
    });
    rc = client.write_wait("direct", 6, 5000);
    if (rc != 0 || !pump_until(&loop, [&got]() { return got == "direct"; }, 5000)) {
      std::cout << "[socks5-server] direct fallback echo failed, got '" << got
                << "'\n";
      return false;
    }
    client.close();
    pump_for(&loop, 20);
  }

  if (route_calls.load() < 2) {
    std::cout << "[socks5-server] route hook was not consulted\n";
    return false;
  }
  std::cout << "[socks5-server] route_target_upstream ok\n";
  return true;
}

}  // namespace


int main() {
  bool ok = true;
  ok = test_refused_mapping() && ok;
  ok = test_disabled_commands() && ok;
  ok = test_target_denied() && ok;
  ok = test_handshake_timeout() && ok;
  ok = test_slot_release_raw() && ok;
  ok = test_max_connections() && ok;
  ok = test_route_target_upstream() && ok;

  if (!ok) {
    std::cout << "[socks5-server] FAILED\n";
    return 1;
  }
  std::cout << "[socks5-server] ALL PASSED\n";
  return 0;
}
