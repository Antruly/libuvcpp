/**
 * @file tests/functional/tcp_two_buf_write_func.cpp
 * @brief 头/体分两块写（`uv_write` 的 `nbufs = 2`）与它必须持有的那块字节。
 *
 * 这个文件守的是 #17 第 3 步**新引入的那条所有权契约**，而不是"两块能跑通"：
 *
 *  1. **写请求必须把那块字节留到完成回调。** libuv 的契约是"缓冲在回调之前
 *     一直有效"，它拷 `uv_buf_t` **数组**但不拷数据。所以 `write(head, head_len,
 *     body, cb)` 必须自己把体接住（共享视图 → 引用计数、自有块 → 拿所有权）。
 *     缺了这一条，表现不是"偶尔少几个字节"，而是**悬空读**：调用方一释放，
 *     libuv 手里就是一个指向已释放内存的指针。
 *     本文件把这一点做成**可判的**，分两条腿：
 *       * `write()` 一返回就数引用计数 —— 请求必须在那份串上占一个持有者。
 *         这条在裸跑里当场说话，变异（`hold` 传 `nullptr`）跑不掉；
 *       * 然后把调用方那边的引用全放掉（`body.clear()` + `sp.reset()`）再跑完
 *         这次写 —— 少接一层，这里就是悬空读。裸跑未必咬得到（回环上内核常常
 *         已经把体收走了），决定性的是页面堆/ASAN 门禁。
 *
 *  2. **自有块是"所有权转移"，不是"借"**：交出去之后源必须变空，且总共只释放
 *     一次（两块都放 → double free；一块都不放 → 泄漏）。两条都靠"跑完之后源
 *     是空的" + 门禁来判。
 *
 *  3. **退化情况必须退对**：空体不能变成"两块里有一块长度 0"（libuv 收得下，
 *     但对端会看到一个多余的 0 长度段，且写请求会白持一份句柄）；`cb == nullptr`
 *     那条同步退路要真的把头体合并后发出去。
 *
 * 起的是真连接：这条契约整个都活在"回调之前那块内存是否还有效"上，不跑真连接
 * 根本触发不到。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <uv.h>

#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "handle/uvcpp_timer.h"
#include "net/uvcpp_tcp_client.h"
#include "uvcpp/uvcpp_buf.h"

#include "loop_drain.h"
#include "wait_util.h"

using namespace uvcpp;
using namespace uvcpp_test;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

// =========================================================================
// 收数据的服务端：只累积，不回显
// =========================================================================

struct sink {
  std::mutex m;
  std::string bytes;
  std::atomic<size_t> total;

  sink() : total(0) {}

  void add(const char* p, size_t n) {
    std::lock_guard<std::mutex> g(m);
    bytes.append(p, n);
    total.store(bytes.size());
  }

  std::string snapshot() {
    std::lock_guard<std::mutex> g(m);
    return bytes;
  }

  void reset() {
    std::lock_guard<std::mutex> g(m);
    bytes.clear();
    total.store(0);
  }
};

/// 等到 sink 收满 \p want 字节（墙钟上限，不是迭代次数 —— 见 wait_util.h）。
bool wait_sink(sink* sk, size_t want, int deadline_ms) {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  while (elapsed_ms(t0) < deadline_ms) {
    if (sk->total.load() >= want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return sk->total.load() >= want;
}

void run_sink_server(std::promise<int>& port_promise,
                     std::atomic<bool>& stop_server, sink* sk) {
  uvcpp_loop server_loop;
  loop_drain drain_server_loop(&server_loop);
  server_loop.init();

  uvcpp_tcp server(&server_loop);
  if (server.bindIpv4("127.0.0.1", 0) != 0) {
    port_promise.set_value(-1);
    return;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  server.getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  port_promise.set_value(ntohs(name.sin_port));

  server.listen(
      [&](uvcpp_stream* s, int status) {
        if (status < 0) return;
        uvcpp_tcp* peer = new uvcpp_tcp(&server_loop);
        if (s->accept(peer) != 0) {
          delete peer;
          return;
        }
        peer->read_start(
            [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
              uvcpp_buf::alloc_buf(buf, sz > 0 ? sz : 4096);
            },
            [peer, sk](uvcpp_stream* stream, ssize_t nread,
                       const uv_buf_t* buf) {
              if (nread > 0) {
                sk->add(buf->base, static_cast<size_t>(nread));
                uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
              } else if (nread == 0) {
                // 空读不是 EOF（等价于 EAGAIN）。
                if (buf->base != nullptr) {
                  uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
                }
              } else {
                if (buf->base != nullptr) {
                  uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
                }
                stream->close([peer](uvcpp_handle*) { delete peer; });
              }
            });
      },
      128);

  uvcpp_timer watchdog(&server_loop);
  watchdog.start(
      [&server_loop](uvcpp_timer*) { server_loop.stop(); }, 20000, 0);
  uvcpp_timer stop_poller(&server_loop);
  stop_poller.start(
      [&stop_server, &server_loop](uvcpp_timer*) {
        if (stop_server.load()) server_loop.stop();
      },
      50, 50);

  server_loop.run(UV_RUN_DEFAULT);
}

// =========================================================================
// 用例 A：共享视图 —— write() 返回之后调用方把引用全放掉
//
// 两个判据，各管一头：
//
//   A2 **引用计数**（裸跑里说话的那条）：`write()` 返回后数一下那份串上有几个
//      持有者。写请求必须自己占一个 —— 少了它，这里当场红。变异验证过：把
//      `hold` 传成 `nullptr`，红的就是这一条。
//
//   A3 **端到端字节**（语义判据）：放掉调用方的引用之后，对端收到的还得逐字节
//      正确 —— 管的是头体拼接、次序、长度这些**内容**上的事。悬空读它**不保证**
//      咬得到：回环上内核常常在 `write()` 返回前就把整份体收走了（4 MiB 都照样
//      收得下），此后指针悬不悬空都看不出来。悬空读的确定性判据是页面堆/ASAN
//      门禁（那边当场报），以及上面的 A2。
// =========================================================================
void case_shared_view(int port, sink* sk) {
  const std::string head = "A-head|";
  const std::string payload(1024 * 1024, 'A');
  const std::string expect = head + payload;

  sk->reset();

  std::shared_ptr<const std::string> sp =
      std::make_shared<const std::string>(payload);

  std::atomic<bool> write_done(false);
  std::atomic<int> write_rc(-1);

  {
    uvcpp_tcp_client client;
    int rc = client.connect_wait("127.0.0.1", port, 5000);
    check(rc == 0, "A: connect_wait 成功");
    if (rc != 0) return;

    uvcpp_buf body;
    body.share(sp);

    rc = client.write(head.data(), head.size(), &body,
                      [&](int ws) {
                        write_rc.store(ws);
                        write_done.store(true);
                        client.stop();
                      });
    check(rc == 0, "A: write(head, len, body, cb) 返回 0");
    check(body.is_shared(), "A: 共享视图这一边**仍然**是共享的（请求接的是引用计数，不是句柄）");

    // ---- A2：请求自己占了一个引用 ----
    // 4 = 调用方 1 + 体那份 1 + **写请求那份 1** + 这里取出来的 view 1。
    // 请求没接住的话是 3。
    {
      std::shared_ptr<const std::string> view = body.shared_ref();
      check(view.use_count() == 4,
            "A: 写请求在体的串上占了一个引用（4 份持有者，见注释）");
    }

    // ---- A3：把调用方的引用全放掉，写还得照样发对 ----
    // `write()` 已返回、写还没完成。放掉之后这块字节的唯一持有者只能是那个写
    // 请求；它要是没接住，页面堆门禁会在这里当场报悬空读。
    body.clear();
    sp.reset();

    client.run(UV_RUN_DEFAULT);
  }

  check(write_done.load(), "A: 完成回调被调用");
  check(write_rc.load() == 0, "A: 写成功（status == 0）");
  check(wait_sink(sk, expect.size(), 4000), "A: 对端收满 1 MiB + 头");
  check(sk->snapshot() == expect, "A: 收到的字节 == 头接体（逐字节）");
}

// =========================================================================
// 用例 B：自有块 —— 所有权转移，源必须变空
// =========================================================================
void case_owned_block(int port, sink* sk) {
  const std::string head = "B-head|";
  const std::string payload(2048, 'B');
  const std::string expect = head + payload;

  sk->reset();

  std::atomic<bool> write_done(false);
  std::atomic<int> write_rc(-1);

  {
    uvcpp_tcp_client client;
    int rc = client.connect_wait("127.0.0.1", port, 5000);
    check(rc == 0, "B: connect_wait 成功");
    if (rc != 0) return;

    uvcpp_buf body;
    body.clone_data(payload.data(), payload.size());
    const bool owned_before = (body.get_const_data() != nullptr);

    rc = client.write(head.data(), head.size(), &body,
                      [&](int ws) {
                        write_rc.store(ws);
                        write_done.store(true);
                        client.stop();
                      });
    check(rc == 0, "B: write(head, len, body, cb) 返回 0");
    check(owned_before, "B: 交接之前体里确有字节");
    // 所有权转移的**可判形状**：源被搬空（块归写请求了，它析构时释放一次）。
    // 源若还留着指针，两边都会释放 —— 那是 double free。
    check(body.size() == 0, "B: 交接之后源变空（块的所有权已经转移）");

    body.clear();

    client.run(UV_RUN_DEFAULT);
  }

  check(write_done.load(), "B: 完成回调被调用");
  check(write_rc.load() == 0, "B: 写成功（status == 0）");
  check(wait_sink(sk, expect.size(), 2000), "B: 对端收满");
  check(sk->snapshot() == expect, "B: 收到的字节 == 头接体（逐字节）");
}

// =========================================================================
// 用例 C：空体退化成一块（只头）
// =========================================================================
void case_empty_body(int port, sink* sk) {
  const std::string head = "C-head-only|";

  sk->reset();

  std::atomic<bool> write_done(false);
  std::atomic<int> write_rc(-1);

  {
    uvcpp_tcp_client client;
    int rc = client.connect_wait("127.0.0.1", port, 5000);
    check(rc == 0, "C: connect_wait 成功");
    if (rc != 0) return;

    uvcpp_buf body;  // 空
    rc = client.write(head.data(), head.size(), &body,
                      [&](int ws) {
                        write_rc.store(ws);
                        write_done.store(true);
                        client.stop();
                      });
    check(rc == 0, "C: write(head, len, 空 body, cb) 返回 0");

    client.run(UV_RUN_DEFAULT);
  }

  check(write_done.load(), "C: 完成回调被调用");
  check(write_rc.load() == 0, "C: 写成功（status == 0）");
  check(wait_sink(sk, head.size(), 2000), "C: 对端收到头");
  check(sk->snapshot() == head, "C: 收到的就是头，没有多出一个零长度段");
}

// =========================================================================
// 用例 D：cb == nullptr 的同步退路（头体合并后写）
// =========================================================================
void case_sync_fallback(int port, sink* sk) {
  const std::string head = "D-sync|";
  const std::string payload(1024, 'D');
  const std::string expect = head + payload;

  sk->reset();

  {
    uvcpp_tcp_client client;
    int rc = client.connect_wait("127.0.0.1", port, 5000);
    check(rc == 0, "D: connect_wait 成功");
    if (rc != 0) return;

    uvcpp_buf body;
    body.clone_data(payload.data(), payload.size());

    // 没有回调 = 同步写完再返回（合并那条退路）。
    rc = client.write(head.data(), head.size(), &body, nullptr);
    check(rc == 0, "D: write(..., nullptr) 同步返回 0");
  }

  check(wait_sink(sk, expect.size(), 2000), "D: 对端收满");
  check(sk->snapshot() == expect, "D: 同步退路收到的字节 == 头接体");
}

// =========================================================================
// 用例 E：`uvcpp_buf` 的移动语义（含共享手柄与自移动）
//
// 这条不属于"写路径"，但它是上面那条契约的底座：本类有用户声明的拷贝构造与
// 析构，编译器**不会**隐式生成移动 —— 少了显式移动，`std::move` 会**静默退化
// 成深拷贝**（源原封不动、接收方多一份字节）。写队列那条路正是"移出来"的形状，
// 所以这里在库这一层把它钉死。
// =========================================================================
void case_buf_move() {
  // 自有块
  {
    uvcpp_buf a;
    a.clone_data("abcdef", 6);
    const char* before = a.get_const_data();

    uvcpp_buf b(std::move(a));
    check(b.size() == 6 && b.to_string() == "abcdef", "E1: 移动构造后接收方拿到字节");
    check(b.get_const_data() == before, "E1: 移动的是**块本身**（指针不变），不是拷一份");
    check(a.size() == 0 && a.get_const_data() == nullptr, "E1: 源被搬空");
  }

  // 移动赋值（源非空 → 目标原有的块要放掉）
  {
    uvcpp_buf a;
    a.clone_data("12345", 5);
    uvcpp_buf b;
    b.clone_data("zzz", 3);

    b = std::move(a);
    check(b.size() == 5 && b.to_string() == "12345", "E2: 移动赋值后接收方拿到字节");
    check(a.size() == 0, "E2: 源被搬空");
  }

  // 自移动：不许把块放掉再交给自己（那会直接把数据清零）
  {
    uvcpp_buf a;
    a.clone_data("selfmove", 8);
    const char* before = a.get_const_data();
    a = std::move(a);
    check(a.get_const_data() == before && a.to_string() == "selfmove",
          "E3: 自移动之后内容完好（不是先 free 再赋给自己）");
  }

  // 共享视图：手柄必须**跟着块一起走**。留在源上的话，接手方拿到的就是一个
  // 指向"别人的 string"的裸指针，源一旦析构或改写，这边立刻悬空。
  {
    std::shared_ptr<const std::string> sp =
        std::make_shared<const std::string>("shared-payload");

    uvcpp_buf a;
    a.share(sp);
    check(a.is_shared(), "E4: share() 之后是共享视图");

    uvcpp_buf b(std::move(a));
    check(b.is_shared(), "E4: 手柄跟着移动到了接收方");
    check(b.to_string() == "shared-payload", "E4: 接收方读到的是同一份字节");
    check(!a.is_shared(), "E4: 源不再持有手柄");
    sp.reset();
    check(b.to_string() == "shared-payload", "E4: 外部引用放掉之后接收方仍然有效");
  }

  // 共享之后被写：物化成自有块，**不写坏**原来那份串（①②那条保证的判据）
  {
    std::shared_ptr<const std::string> sp =
        std::make_shared<const std::string>("original");
    uvcpp_buf a;
    a.share(sp);
    // 这一次改写就是所谓的"先共享又丢"：共享让位、拷一份自有的块。库**静默**
    // 处理（不断言、不抛 —— `clear()` 紧接 `clone()` 是响应对象复用的正常
    // 序列），但把它计进 `share_discard_count()`，所以我们这里能钉住"恰好一次"。
    const uint64_t before = uvcpp_buf::share_discard_count();
    a.clone_data("replaced!", 9);
    check(*sp == "original", "E5: 共享视图被改写不会动到那份串");
    check(a.to_string() == "replaced!", "E5: 改写之后是自有块");
    check(!a.is_shared(), "E5: 改写之后不再是共享视图");
    check(uvcpp_buf::share_discard_count() == before + 1,
          "E5: 这次改写被记成一次「共享又丢」（多一次少一次都是计数器自己错了）");
  }
}

}  // namespace

int main() {
  std::promise<int> port_promise;
  std::future<int> port_future = port_promise.get_future();
  std::atomic<bool> stop_server(false);
  sink sk;

  std::thread server_thread(run_sink_server, std::ref(port_promise),
                            std::ref(stop_server), &sk);

  const int port = port_future.get();
  if (port <= 0) {
    std::cerr << "[tcp_two_buf_write] 服务端起不来" << std::endl;
    stop_server.store(true);
    server_thread.join();
    return 1;
  }

  std::cout << "[tcp_two_buf_write] server port=" << port << std::endl;

  // "先共享又丢"的计数器：**热路径上必须是 0**。这四条都是"共享出去、只读地
  // 发走"，一次都不该物化 —— 物化意味着那块字节又被拷了一遍，共享白做了。
  uvcpp_buf::reset_share_discard_count();
  case_shared_view(port, &sk);
  case_owned_block(port, &sk);
  case_empty_body(port, &sk);
  case_sync_fallback(port, &sk);
  check(uvcpp_buf::share_discard_count() == 0,
        "写路径（含共享视图那两条）一次都没「共享又丢」");

  case_buf_move();

  stop_server.store(true);
  server_thread.join();

  if (g_failures == 0) {
    std::cout << "[tcp_two_buf_write] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[tcp_two_buf_write] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}
