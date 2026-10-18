/**
 * @file tests/functional/redis_client_func.cpp
 * @brief `uvcpp_redis_client` ↔ 进程内假 Redis 服务端：协商、命令、管线、订阅。
 *
 * 服务端跑在后台线程自己的循环上，用 `uvcpp_tcp_server` 接受连接、用
 * **同一个** `uvcpp_redis_parser` 解客户端发来的命令数组 —— 解析器同时当
 * 请求解码器用，是因为 RESP 的请求与应答本来就共用一套编码。
 *
 * 这里不依赖本机装 redis-server；连真服务器的用例在 `redis_real_func.cpp`。
 * 假服务端的好处是可以把"应答晚到一半""命令永远不回"这类时序钉死，这些在
 * 真服务器上没法安排。
 */

#include <redis/uvcpp_redis.h>
#include <redis/uvcpp_redis_resp.h>

#include <handle/uvcpp_loop.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <uvcpp/uvcpp_buf.h>

#include <uv.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++g_fail;
    std::cout << "[redis_client] FAIL: " << what << "\n";
  }
}

bool pump_until(uvcpp::uvcpp_loop* loop, const std::function<bool()>& done,
                int timeout_ms) {
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  while (!done()) {
    loop->run(UV_RUN_NOWAIT);
    if (done()) break;
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

}  // namespace

// =========================================================================
// 假 Redis 服务端
// =========================================================================

namespace {

struct FakeServer;

/// 一条已接受的连接。所有成员只在服务端线程上访问。
struct FakeConn : std::enable_shared_from_this<FakeConn> {
  uvcpp::uvcpp_tcp_client* client;
  uvcpp::uvcpp_redis_parser parser;
  FakeServer* owner;
  std::string in;
  std::size_t off;
  std::deque<std::string> out;
  struct late_chunk {
    std::chrono::steady_clock::time_point due;
    std::string data;
  };
  std::deque<late_chunk> late;
  bool in_flight;
  bool closed;

  FakeConn() : client(nullptr), owner(nullptr), off(0),
               in_flight(false), closed(false) {}

  void push(const std::string& bytes);
  void push_late(const std::string& bytes, int delay_ms);
  void push_bulk(const std::string& s);
  void pump();
  void on_command(const std::vector<std::string>& args);
  void flush_late();
};

struct FakeServer {
  std::thread thread;
  std::promise<int> port_promise;
  std::atomic<bool> ready;
  std::atomic<bool> stop;
  int port;

  mutable std::mutex mu;
  std::vector<std::vector<std::string>> seen;

  // 只在服务端线程上访问。
  std::vector<std::weak_ptr<FakeConn>> live;

  FakeServer() : ready(false), stop(false), port(0) {}

  void record(const std::vector<std::string>& args) {
    std::lock_guard<std::mutex> lk(mu);
    seen.push_back(args);
  }

  std::vector<std::vector<std::string>> commands() const {
    std::lock_guard<std::mutex> lk(mu);
    return seen;
  }

  void start() {
    thread = std::thread([this]() {
      uvcpp::uvcpp_loop loop;
      uvcpp::uvcpp_tcp_server server(&loop);
      server.bind("127.0.0.1", 0);

      struct sockaddr_in name;
      int namelen = static_cast<int>(sizeof(name));
      server.get_tcp()->getsockname(reinterpret_cast<struct sockaddr*>(&name),
                                    &namelen);
      port_promise.set_value(ntohs(name.sin_port));

      server.listen(
          [this](uvcpp::uvcpp_tcp_client* client) {
            std::shared_ptr<FakeConn> conn(new FakeConn());
            conn->client = client;
            conn->owner = this;

            client->add_close_observer([conn]() { conn->closed = true; });
            std::weak_ptr<FakeConn> weak = conn;
            client->read_start([weak](uvcpp::uvcpp_buf* b) {
              std::shared_ptr<FakeConn> c = weak.lock();
              if (!c || c->closed) return;
              if (b == nullptr || b->size() == 0 ||
                  b->get_const_data() == nullptr) {
                return;
              }
              c->in.append(b->get_const_data(), b->size());

              while (!c->closed) {
                if (c->off >= c->in.size()) {
                  c->in.clear();
                  c->off = 0;
                  break;
                }
                uvcpp::uvcpp_redis_value v;
                const uvcpp::uvcpp_redis_parser::result r =
                    c->parser.next(c->in.data(), c->in.size(), c->off, v,
                                   nullptr);
                if (r == uvcpp::uvcpp_redis_parser::NEED_MORE) break;
                if (r == uvcpp::uvcpp_redis_parser::BAD) {
                  c->in.clear();
                  c->off = 0;
                  c->client->close();
                  return;
                }
                std::vector<std::string> args;
                for (std::size_t i = 0; i < v.size(); ++i) {
                  args.push_back(v.at(i).as_string());
                }
                c->on_command(args);
                if (c->off >= c->in.size()) {
                  c->in.clear();
                  c->off = 0;
                  break;
                }
              }
              if (!c->closed && c->off > 0) {
                c->in.erase(0, c->off);
                c->off = 0;
              }
            });

            live.push_back(conn);
          },
          128);

      ready.store(true);

      while (!stop.load()) {
        server.run(UV_RUN_NOWAIT);
        // 晚到的应答段在这里投递（假服务端不用定时器：1 ms 的轮询粒度
        // 对"分成两段到达"这条判据绰绰有余，还省掉了句柄的生命周期问题）。
        for (std::size_t i = 0; i < live.size(); ++i) {
          std::shared_ptr<FakeConn> c = live[i].lock();
          if (c) c->flush_late();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      // 让 `~uvcpp_tcp_server` 的收尾在循环还活着时做完。
      for (int i = 0; i < 50; ++i) {
        server.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });

    port = port_promise.get_future().get();
    while (!ready.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  ~FakeServer() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

void FakeConn::push(const std::string& bytes) {
  out.push_back(bytes);
  pump();
}

void FakeConn::push_late(const std::string& bytes, int delay_ms) {
  late_chunk ch;
  ch.due = std::chrono::steady_clock::now() +
           std::chrono::milliseconds(delay_ms);
  ch.data = bytes;
  late.push_back(ch);
}

void FakeConn::push_bulk(const std::string& s) {
  char head[32];
  std::snprintf(head, sizeof(head), "$%lu\r\n",
                static_cast<unsigned long>(s.size()));
  push(std::string(head) + s + "\r\n");
}

void FakeConn::pump() {
  while (!closed && !in_flight && !out.empty()) {
    // 拷贝一份再投：`write()` 的完成回调里会回填 `in_flight`，而那一笔的
    // 字节必须活到写完（`write()` 内部会拷贝，但把局部量交出去更直观）。
    std::shared_ptr<FakeConn> self = shared_from_this();
    const std::string chunk = out.front();
    out.pop_front();
    in_flight = true;
    const int rc = client->write(chunk.data(), chunk.size(), [self](int) {
      self->in_flight = false;
      self->pump();
    });
    if (rc != 0) {
      in_flight = false;
      out.clear();
      return;
    }
  }
}

void FakeConn::flush_late() {
  const std::chrono::steady_clock::time_point now =
      std::chrono::steady_clock::now();
  bool moved = false;
  while (!late.empty() && late.front().due <= now) {
    out.push_back(late.front().data);
    late.pop_front();
    moved = true;
  }
  if (moved) pump();
}

void FakeConn::on_command(const std::vector<std::string>& args) {
  if (args.empty()) return;
  owner->record(args);

  const std::string& cmd = args[0];
  if (cmd == "AUTH") {
    const std::string& pass = args.back();
    push(pass == "secret" ? "+OK\r\n"
                          : "-WRONGPASS invalid username-password pair\r\n");
    return;
  }
  if (cmd == "HELLO") {
    // `HELLO 3 [AUTH user pass]`
    if (args.size() >= 5 && args[2] == "AUTH") {
      if (args[4] != "secret") {
        push("-WRONGPASS invalid username-password pair\r\n");
        return;
      }
    }
    push(
        "%3\r\n$6\r\nserver\r\n$5\r\nredis\r\n$7\r\nversion\r\n$5\r\n7.2.0\r\n"
        "$5\r\nproto\r\n:3\r\n");
    return;
  }
  if (cmd == "SELECT") {
    push("+OK\r\n");
    return;
  }
  if (cmd == "PING") {
    push("+PONG\r\n");
    return;
  }
  if (cmd == "ECHO") {
    push_bulk(args.size() > 1 ? args[1] : std::string());
    return;
  }
  if (cmd == "GET") {
    if (args.size() > 1 && args[1] == "missing") {
      push("$-1\r\n");
    } else {
      push_bulk("vv");
    }
    return;
  }
  if (cmd == "SET") {
    push("+OK\r\n");
    return;
  }
  if (cmd == "FAIL") {
    push("-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
    return;
  }
  if (cmd == "MAP") {
    push("%2\r\n$1\r\na\r\n:1\r\n$1\r\nb\r\n:2\r\n");
    return;
  }
  if (cmd == "PARTIAL") {
    // 同一个值拆成两段写出去：第一段不是完整帧，解析器必须等第二段。
    push("$11\r\nhel");
    push_late("lo world\r\n", 30);
    return;
  }
  if (cmd == "SLOW") {
    return;  // 永不回：留给客户端的命令超时
  }
  if (cmd == "SUBSCRIBE") {
    const std::string ch = args.size() > 1 ? args[1] : std::string();
    char head[32];
    std::snprintf(head, sizeof(head), "*3\r\n$9\r\nsubscribe\r\n$%lu\r\n",
                  static_cast<unsigned long>(ch.size()));
    push(std::string(head) + ch + "\r\n:1\r\n");
    // 订阅确认之后再推一条消息（RESP2 下它与命令应答同一种编码）。
    char msg[64];
    std::snprintf(msg, sizeof(msg), "*3\r\n$7\r\nmessage\r\n$%lu\r\n",
                  static_cast<unsigned long>(ch.size()));
    push_late(std::string(msg) + ch + "\r\n$5\r\nhello\r\n", 30);
    return;
  }
  if (cmd == "UNSUBSCRIBE") {
    const std::string ch = args.size() > 1 ? args[1] : std::string();
    char head[32];
    std::snprintf(head, sizeof(head), "*3\r\n$11\r\nunsubscribe\r\n$%lu\r\n",
                  static_cast<unsigned long>(ch.size()));
    push(std::string(head) + ch + "\r\n:0\r\n");
    return;
  }
  push("-ERR unknown command '" + cmd + "'\r\n");
}

}  // namespace

// =========================================================================
// 用例
// =========================================================================

namespace {

std::string url_for(int port, const std::string& tail) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "redis://127.0.0.1:%d", port);
  return std::string(buf) + tail;
}

// -------------------------------------------------------------------------
// URL / 状态错误
// -------------------------------------------------------------------------
void test_errors_before_connect(int port) {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_redis_client c(&loop);

  std::function<void(int)> cb = [](int) {};

  check(c.connect("127.0.0.1:6379", cb) == UV_EINVAL,
        "URL without scheme is EINVAL");
  check(c.connect("http://127.0.0.1:6379", cb) == UV_ENOTSUP,
        "http:// scheme is ENOTSUP");
  check(c.connect("redis://", cb) == UV_EINVAL, "empty host is EINVAL");
  check(c.connect("redis://u:%zz@127.0.0.1:6379", cb) == UV_EINVAL,
        "bad percent escape is EINVAL");
  check(c.connect("redis://127.0.0.1:99999", cb) == UV_EINVAL,
        "bad port is EINVAL");

  bool called = false;
  check(c.command({"PING"}, [&called](int, const uvcpp::uvcpp_redis_value&) {
          called = true;
        }) == UV_ENOTCONN,
        "command before connect is ENOTCONN");
  pump_for(&loop, 10);
  check(!called, "command before connect does not fire");

  (void)port;
}

// -------------------------------------------------------------------------
// AUTH + SELECT 协商
// -------------------------------------------------------------------------
void test_handshake(int port, FakeServer& srv) {
  const std::size_t before = srv.commands().size();

  char url[128];
  std::snprintf(url, sizeof(url), "redis://:secret@127.0.0.1:%d/3", port);

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_redis_client c(&loop);
  const int rc = c.connect_wait(url);
  check(rc == 0, "connect_wait with AUTH + SELECT succeeds");
  check(c.is_connected(), "is_connected() after handshake");

  const std::vector<std::vector<std::string>> cmds = srv.commands();
  check(cmds.size() >= before + 2, "server saw both handshake commands");
  if (cmds.size() >= before + 2) {
    check(cmds[before].size() == 2 && cmds[before][0] == "AUTH" &&
              cmds[before][1] == "secret",
          "handshake order: AUTH secret first");
    check(cmds[before + 1].size() == 2 && cmds[before + 1][0] == "SELECT" &&
              cmds[before + 1][1] == "3",
          "handshake order: SELECT 3 second");
  }

  uvcpp::uvcpp_redis_value v;
  check(c.command_wait({"PING"}, v, 3000) == 0 && v.as_string() == "PONG",
        "PING after handshake");

  c.close();
  pump_for(&loop, 20);
}

// -------------------------------------------------------------------------
// 认证失败
// -------------------------------------------------------------------------
void test_auth_failure(int port) {
  char url[128];
  std::snprintf(url, sizeof(url), "redis://:wrong@127.0.0.1:%d", port);

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_redis_client c(&loop);
  const int rc = c.connect_wait(url);
  check(rc == UV_EACCES, "wrong password maps to EACCES");
  check(c.last_error().find("WRONGPASS") != std::string::npos,
        "last_error carries the server's text");
  check(!c.is_connected(), "not connected after auth failure");
  pump_for(&loop, 20);
}

// -------------------------------------------------------------------------
// 普通应答：空、bulk、错误应答、二进制参数
// -------------------------------------------------------------------------
void test_replies(int port) {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_redis_client c(&loop);
  check(c.connect_wait(url_for(port, "")) == 0, "connect for replies");

  uvcpp::uvcpp_redis_value v;
  check(c.command_wait({"GET", "missing"}, v, 3000) == 0 && v.is_null(),
        "missing key is null");

  check(c.command_wait({"GET", "k"}, v, 3000) == 0 &&
            v.type() == uvcpp::uvcpp_redis_type::BULK &&
            v.as_string() == "vv",
        "GET returns bulk");

  check(c.command_wait({"FAIL"}, v, 3000) == 0 && v.is_error() &&
            v.as_string().find("WRONGTYPE") == 0,
        "server error reply: status 0 + is_error()");

  const std::string binary("a\0b", 3);
  check(c.command_wait({"ECHO", binary}, v, 3000) == 0 &&
            v.as_string() == binary,
        "binary-safe command argument");

  c.close();
  pump_for(&loop, 20);
}

// -------------------------------------------------------------------------
// pipeline：一次写出去，应答逐条配对
// -------------------------------------------------------------------------
void test_pipeline(int port) {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_redis_client c(&loop);
  check(c.connect_wait(url_for(port, "")) == 0, "connect for pipeline");

  std::vector<std::vector<std::string>> cmds;
  cmds.push_back(std::vector<std::string>{"PING"});
  cmds.push_back(std::vector<std::string>{"ECHO", "a"});
  cmds.push_back(std::vector<std::string>{"ECHO", "b"});

  std::vector<uvcpp::uvcpp_redis_value> out;
  check(c.pipeline_wait(cmds, out, 3000) == 0 && out.size() == 3 &&
            out[0].as_string() == "PONG" && out[1].as_string() == "a" &&
            out[2].as_string() == "b",
        "pipeline keeps request order");

  c.close();
  pump_for(&loop, 20);
}

// -------------------------------------------------------------------------
// 应答分两段到达
// -------------------------------------------------------------------------
void test_partial(int port) {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_redis_client c(&loop);
  check(c.connect_wait(url_for(port, "")) == 0, "connect for partial");

  uvcpp::uvcpp_redis_value v;
  check(c.command_wait({"PARTIAL"}, v, 3000) == 0 &&
            v.as_string() == "hello world",
        "reply split across two reads");

  c.close();
  pump_for(&loop, 20);
}

// -------------------------------------------------------------------------
// 订阅：确认 + 消息分发
// -------------------------------------------------------------------------
void test_subscribe(int port) {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_redis_client c(&loop);
  check(c.connect_wait(url_for(port, "")) == 0, "connect for subscribe");

  int sub_status = -1;
  std::string got_channel;
  std::string got_message;
  check(c.subscribe({"news"},
                    [&](const std::string& channel,
                        const uvcpp::uvcpp_redis_value& m) {
                      got_channel = channel;
                      got_message = m.as_string();
                    },
                    [&](int st) { sub_status = st; }) == 0,
        "SUBSCRIBE accepted");

  check(pump_until(&loop,
                   [&]() { return sub_status >= 0 && !got_message.empty(); },
                   3000),
        "subscribe ack and message both arrive");
  check(sub_status == 0, "subscribe acknowledged with status 0");
  check(got_channel == "news" && got_message == "hello",
        "message carries channel + payload");

  c.close();
  pump_for(&loop, 20);
}

// -------------------------------------------------------------------------
// RESP3：HELLO 3 + 映射应答
// -------------------------------------------------------------------------
void test_resp3(int port, FakeServer& srv) {
  const std::size_t before = srv.commands().size();
  char url[128];
  std::snprintf(url, sizeof(url), "redis://:secret@127.0.0.1:%d", port);

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_redis_client c(&loop);
  c.set_resp_version(3);
  check(c.connect_wait(url) == 0, "HELLO 3 handshake");

  const std::vector<std::vector<std::string>> cmds = srv.commands();
  check(cmds.size() >= before + 1, "server saw HELLO");
  if (cmds.size() >= before + 1) {
    const std::vector<std::string>& hello = cmds[before];
    check(hello.size() == 5 && hello[0] == "HELLO" && hello[1] == "3" &&
              hello[2] == "AUTH" && hello[3] == "default" &&
              hello[4] == "secret",
          "HELLO 3 carries AUTH default <pass>");
  }

  uvcpp::uvcpp_redis_value v;
  check(c.command_wait({"MAP"}, v, 3000) == 0 &&
            v.type() == uvcpp::uvcpp_redis_type::MAP &&
            v.items().size() == 2 && v.items()[0].first.as_string() == "a" &&
            v.items()[0].second.as_integer() == 1 &&
            v.items()[1].first.as_string() == "b" &&
            v.items()[1].second.as_integer() == 2,
        "RESP3 map reply");

  c.close();
  pump_for(&loop, 20);
}

// -------------------------------------------------------------------------
// 命令超时：服务器永不回
// -------------------------------------------------------------------------
void test_command_timeout(int port) {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_redis_client c(&loop);
  c.set_command_timeout_ms(100);
  check(c.connect_wait(url_for(port, "")) == 0, "connect for timeout");

  bool done = false;
  int  status = 0;
  check(c.command({"SLOW"}, [&](int st, const uvcpp::uvcpp_redis_value&) {
          status = st;
          done = true;
        }) == 0,
        "SLOW accepted");

  check(pump_until(&loop, [&]() { return done; }, 3000),
        "command timeout fires");
  check(status == UV_ETIMEDOUT, "timeout status is ETIMEDOUT");
  check(!c.is_connected(), "connection is closed after a command timeout");

  c.close();
  pump_for(&loop, 20);
}

}  // namespace

int main() {
  FakeServer srv;
  srv.start();

  test_errors_before_connect(srv.port);
  test_handshake(srv.port, srv);
  test_auth_failure(srv.port);
  test_replies(srv.port);
  test_pipeline(srv.port);
  test_partial(srv.port);
  test_subscribe(srv.port);
  test_resp3(srv.port, srv);
  test_command_timeout(srv.port);

  if (g_fail != 0) {
    std::cout << "[redis_client] " << g_fail << " check(s) FAILED\n";
    return 1;
  }
  std::cout << "[redis_client] ALL PASSED\n";
  return 0;
}
