/**
 * @file tests/functional/socks5_udp_func.cpp
 * @brief SOCKS5 服务端的 UDP ASSOCIATE：中继、首个数据报锁定、FRAG 丢弃、
 *        控制连接关闭后的释放、IPv6。
 *
 * UDP 回环那一趟在 `socks5_client_func.cpp` 里已经走过；这里钉的是服务端
 * 的**行为边界** —— 锁定之前/之后的来源判别、分片位、以及关联的生命周期。
 */

#include <net/uvcpp_socks5_client.h>
#include <net/uvcpp_socks5_server.h>
#include <net/uvcpp_socks5_types.h>
#include <net/uvcpp_socks5_udp.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_udp_client.h>

#include <handle/uvcpp_loop.h>

#include <atomic>
#include <chrono>
#include <cstring>
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
  std::string                      bind_host;
  std::thread                      thread;
  std::atomic<bool>                ready;
  std::atomic<bool>                stop;
  std::atomic<std::size_t>         active;
  int                              port;

  SocksServer() : bind_host("127.0.0.1"), ready(false), stop(false), active(0), port(0) {}

  void start() {
    thread = std::thread([this]() {
      uvcpp::uvcpp_socks5_server srv;
      srv.set_options(opts);
      srv.bind(bind_host, 0);
      port = srv.port();
      srv.listen([](const uvcpp::uvcpp_socks5_session_info&) {});
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

// UDP echo：记录收到的数据报数（用来判断"有没有被转发过来"）。
struct UdpEchoServer {
  std::thread             thread;
  std::atomic<bool>       ready;
  std::atomic<bool>       stop;
  std::atomic<int>        received;
  std::atomic<int>        ipv6_rc;
  int                     port;

  UdpEchoServer() : ready(false), stop(false), received(0), ipv6_rc(0), port(0) {}

  void start(bool ipv6) {
    thread = std::thread([this, ipv6]() {
      uvcpp::uvcpp_loop loop;
      uvcpp::uvcpp_udp_client udp(&loop);
      const int rc = ipv6 ? udp.bindIpv6("::1", 0) : udp.bindIpv4("127.0.0.1", 0);
      ipv6_rc.store(rc);
      if (rc == 0) {
        std::string ip;
        int         p = 0;
        udp.getLocalAddrs(ip, p);
        port = p;

        uvcpp::uvcpp_udp_client* self = &udp;
        udp.recv_start([this, self](uvcpp::uvcpp_buf* b, const char* from_ip,
                                    int from_port) {
          if (b == nullptr || from_ip == nullptr) return;
          received.fetch_add(1);
          self->send(from_ip, from_port, b->get_const_data(), b->size(),
                     [](int) {});
        });
      }
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

// 手写一条 SOCKS5 的 UDP 帧：RSV(2) + FRAG + ATYP + ADDR + PORT + DATA。
std::string udp_frame(std::uint8_t frag, const std::string& ip, int port,
                      const std::string& payload) {
  std::string out;
  out.push_back(0);
  out.push_back(0);
  out.push_back(static_cast<char>(frag));
  out.push_back(1);  // ATYP = IPv4
  unsigned a = 0, b = 0, c = 0, d = 0;
  std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
  out.push_back(static_cast<char>(a));
  out.push_back(static_cast<char>(b));
  out.push_back(static_cast<char>(c));
  out.push_back(static_cast<char>(d));
  out.push_back(static_cast<char>((port >> 8) & 0xFF));
  out.push_back(static_cast<char>(port & 0xFF));
  out.append(payload);
  return out;
}

bool parse_udp_reply(const std::string& frame, std::string& payload) {
  if (frame.size() < 10) return false;
  if (frame[0] != 0 || frame[1] != 0) return false;
  if (frame[3] != 1) return false;  // 只支持 IPv4 应答
  payload.assign(frame.data() + 10, frame.size() - 10);
  return true;
}

bool test_echo_and_lock() {
  std::cout << "[socks5-udp] echo_and_lock start\n";

  UdpEchoServer echo;
  echo.start(false);
  SocksServer proxy;
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_socks5_client client(&loop);

  int rc = client.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5-udp] connect failed: " << uv_strerror(rc) << "\n";
    return false;
  }

  uvcpp::uvcpp_socks5_endpoint relay;
  rc = client.udp_associate_wait(relay, 5000);
  if (rc != 0 || client.udp() == nullptr) {
    std::cout << "[socks5-udp] associate failed: " << uv_strerror(rc) << " ("
              << client.last_error() << ")\n";
    return false;
  }

  std::string got;
  uvcpp::uvcpp_socks5_endpoint got_from;
  rc = client.udp()->recv_start(
      [&got, &got_from](int, const uvcpp::uvcpp_socks5_endpoint& from,
                        const void* data, std::size_t len) {
        got_from = from;
        got.assign(static_cast<const char*>(data), len);
      });
  if (rc != 0) {
    std::cout << "[socks5-udp] recv_start failed: " << uv_strerror(rc) << "\n";
    return false;
  }

  rc = client.udp()->send_to("127.0.0.1", echo.port, "hello", 5);
  if (rc != 0 || !pump_until(&loop, [&got]() { return got == "hello"; }, 5000)) {
    std::cout << "[socks5-udp] echo failed (rc=" << rc << ", got='" << got
              << "')\n";
    return false;
  }
  if (echo.received.load() != 1) {
    std::cout << "[socks5-udp] target saw " << echo.received.load()
              << " datagrams, expected 1\n";
    return false;
  }

  // 锁定之后：另一个来源打中继，服务端把它当成"目标的应答"转给客户端，
  // 而**不会**转发给目标。两条都验。
  uvcpp::uvcpp_loop spoof_loop;
  uvcpp::uvcpp_udp_client spoof(&spoof_loop);
  spoof.bindIpv4("127.0.0.1", 0);
  std::string spoof_ip;
  int         spoof_port = 0;
  spoof.getLocalAddrs(spoof_ip, spoof_port);

  got.clear();
  const std::string injected = udp_frame(0, "127.0.0.1", echo.port, "spoofed");
  rc = spoof.send(relay.host.c_str(), relay.port, injected.data(),
                  injected.size(), [](int) {});
  if (rc != 0) {
    std::cout << "[socks5-udp] spoof send failed: " << uv_strerror(rc) << "\n";
    return false;
  }
  // 锁定之后的"非客户端来源"被当成**目标的应答**：服务端把收到的整段字节
  // （包括我们手写的那个 SOCKS 头）原样当载荷，重新封一帧发给客户端。
  if (!pump_until(&loop, [&got, &injected]() { return got == injected; },
                  5000)) {
    std::cout << "[socks5-udp] spoofed datagram was not relayed verbatim to"
              << " the client\n";
    return false;
  }
  if (got_from.port != spoof_port) {
    std::cout << "[socks5-udp] relayed source port = " << got_from.port
              << ", expected " << spoof_port << "\n";
    return false;
  }
  pump_for(&spoof_loop, 50);
  if (echo.received.load() != 1) {
    std::cout << "[socks5-udp] spoofed datagram reached the target ("
              << echo.received.load() << " datagrams)\n";
    return false;
  }

  const std::size_t before = proxy.active.load();
  if (before == 0) {
    std::cout << "[socks5-udp] session was never counted\n";
    return false;
  }
  client.close();
  pump_for(&loop, 20);
  if (!wait_until([&proxy]() { return proxy.active.load() == 0; }, 5000)) {
    std::cout << "[socks5-udp] session not released after control close ("
              << proxy.active.load() << ")\n";
    return false;
  }
  if (client.udp() != nullptr) {
    std::cout << "[socks5-udp] udp() survived client.close()\n";
    return false;
  }

  std::cout << "[socks5-udp] echo_and_lock ok\n";
  return true;
}

// 手写一条关联（不用本仓客户端），专门验 FRAG≠0 的那个数据报被丢掉。
bool test_frag_dropped() {
  std::cout << "[socks5-udp] frag_dropped start\n";

  UdpEchoServer echo;
  echo.start(false);
  SocksServer proxy;
  proxy.start();

  uvcpp::uvcpp_loop loop;

  // --- 控制连接：GREETING → REQUEST(UDP ASSOCIATE) ---
  uvcpp::uvcpp_tcp_client ctl(&loop);
  int rc = ctl.connect_wait("127.0.0.1", proxy.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5-udp] raw ctl connect failed: " << uv_strerror(rc)
              << "\n";
    return false;
  }

  std::string in;
  ctl.read_start([&in](uvcpp::uvcpp_buf* b) {
    if (b != nullptr && b->size() > 0) in.append(b->get_const_data(), b->size());
  });

  const char greeting[3] = {0x05, 0x01, 0x00};
  rc = ctl.write_wait(greeting, 3, 5000);
  if (rc != 0 || !pump_until(&loop, [&in]() { return in.size() >= 2; }, 5000)) {
    std::cout << "[socks5-udp] greeting failed\n";
    return false;
  }
  if (in[0] != 0x05 || in[1] != 0x00) {
    std::cout << "[socks5-udp] unexpected method selection\n";
    return false;
  }
  in.clear();

  const char request[10] = {0x05, 0x03, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
  rc = ctl.write_wait(request, 10, 5000);
  if (rc != 0 || !pump_until(&loop, [&in]() { return in.size() >= 10; }, 5000)) {
    std::cout << "[socks5-udp] UDP ASSOCIATE request failed\n";
    return false;
  }
  if (in[0] != 0x05 || in[1] != 0x00) {
    std::cout << "[socks5-udp] UDP ASSOCIATE was refused\n";
    return false;
  }
  const int relay_port =
      (static_cast<unsigned char>(in[8]) << 8) | static_cast<unsigned char>(in[9]);
  if (relay_port == 0) {
    std::cout << "[socks5-udp] relay port is 0\n";
    return false;
  }

  // --- 数据面：自己绑一个 UDP socket，手写帧 ---
  uvcpp::uvcpp_udp_client udp(&loop);
  udp.bindIpv4("127.0.0.1", 0);

  std::string replies;
  udp.recv_start([&replies](uvcpp::uvcpp_buf* b, const char*, int) {
    if (b != nullptr && b->size() > 0) {
      replies.append(b->get_const_data(), b->size());
    }
  });

  // 第一个数据报带 FRAG=1：锁定来源，但**不转发**。
  const std::string frag = udp_frame(1, "127.0.0.1", echo.port, "frag");
  rc = udp.send("127.0.0.1", relay_port, frag.data(), frag.size(),
                [](int) {});
  if (rc != 0) {
    std::cout << "[socks5-udp] frag send failed: " << uv_strerror(rc) << "\n";
    return false;
  }
  pump_for(&loop, 100);
  if (echo.received.load() != 0) {
    std::cout << "[socks5-udp] FRAG!=0 datagram was forwarded\n";
    return false;
  }

  // 第二个数据报 FRAG=0：正常转发，目标的应答要回来。
  const std::string ok_frame = udp_frame(0, "127.0.0.1", echo.port, "ok");
  rc = udp.send("127.0.0.1", relay_port, ok_frame.data(), ok_frame.size(),
                [](int) {});
  if (rc != 0) {
    std::cout << "[socks5-udp] frame send failed: " << uv_strerror(rc) << "\n";
    return false;
  }
  if (!pump_until(&loop, [&replies]() { return !replies.empty(); }, 5000)) {
    std::cout << "[socks5-udp] no relayed reply\n";
    return false;
  }
  std::string payload;
  if (!parse_udp_reply(replies, payload) || payload != "ok") {
    std::cout << "[socks5-udp] bad relayed reply (" << replies.size()
              << " bytes)\n";
    return false;
  }
  if (echo.received.load() != 1) {
    std::cout << "[socks5-udp] target saw " << echo.received.load()
              << " datagrams, expected 1\n";
    return false;
  }

  ctl.close();
  pump_for(&loop, 20);
  std::cout << "[socks5-udp] frag_dropped ok\n";
  return true;
}

bool test_ipv6() {
  std::cout << "[socks5-udp] ipv6 start\n";

  UdpEchoServer echo;
  echo.start(true);
  if (echo.ipv6_rc.load() != 0) {
    std::cout << "[socks5-udp] ipv6 unavailable (" << uv_strerror(echo.ipv6_rc.load())
              << "), skipped\n";
    return true;
  }

  SocksServer proxy;
  proxy.bind_host = "::1";
  proxy.start();

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_socks5_client client(&loop);

  int rc = client.connect_wait("::1", proxy.port, 5000);
  if (rc != 0) {
    std::cout << "[socks5-udp] ipv6 connect failed: " << uv_strerror(rc) << "\n";
    return false;
  }

  uvcpp::uvcpp_socks5_endpoint relay;
  rc = client.udp_associate_wait(relay, 5000, "::1", 0);
  if (rc != 0 || client.udp() == nullptr) {
    std::cout << "[socks5-udp] ipv6 associate failed: " << uv_strerror(rc)
              << " (" << client.last_error() << ")\n";
    return false;
  }

  std::string got;
  rc = client.udp()->recv_start(
      [&got](int, const uvcpp::uvcpp_socks5_endpoint&, const void* data,
             std::size_t len) {
        got.assign(static_cast<const char*>(data), len);
      });
  if (rc != 0) {
    std::cout << "[socks5-udp] ipv6 recv_start failed: " << uv_strerror(rc)
              << "\n";
    return false;
  }

  rc = client.udp()->send_to("::1", echo.port, "v6", 2);
  if (rc != 0 || !pump_until(&loop, [&got]() { return got == "v6"; }, 5000)) {
    std::cout << "[socks5-udp] ipv6 echo failed (rc=" << rc << ", got='" << got
              << "')\n";
    return false;
  }

  client.close();
  pump_for(&loop, 20);
  std::cout << "[socks5-udp] ipv6 ok\n";
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_echo_and_lock() && ok;
  ok = test_frag_dropped() && ok;
  ok = test_ipv6() && ok;

  if (!ok) {
    std::cout << "[socks5-udp] FAILED\n";
    return 1;
  }
  std::cout << "[socks5-udp] ALL PASSED\n";
  return 0;
}
