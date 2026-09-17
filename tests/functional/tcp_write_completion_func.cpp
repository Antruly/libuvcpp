/**
 * @file tests/functional/tcp_write_completion_func.cpp
 * @brief 写完成回调**执行的是什么存储**，以及写请求归谁释放。
 *
 * 缺陷（清单 #7）：`uvcpp_tcp_client` 的四处写完成回调里都写着 `delete wr`。
 * 那个 lambda 就存在 `wr->m_write_cb` 里 —— `delete wr` 删掉的正是**此刻正在
 * 执行的那个闭包本身**，C++ 里是未定义行为。它平时不表现为故障，只因为那句
 * 是回调的最后一句、之后再没读过捕获。
 *
 * 修法：释放挪进 `uvcpp_write::callback_write`。它先把闭包**搬到栈上**再调用
 * （于是回调里怎么删 wr 都与本次调用无关），再由 `set_self_free(true)` 让它在
 * 闭包返回**之后**自己 `delete`。这条路是 O(1) 的：不入队、不延迟、不额外分配，
 * 所以每次写都走的热路径没有变慢。
 *
 * 核心判据只有一条，但它是**确定性**的 —— 修复前必挂、修复后必过：
 *
 *  **完成回调里 `wr->m_write_cb` 必须为空。** 那个 lambda 的存储就在这个成员
 *  里；非空意味着此刻执行的闭包**与 `wr` 同生共死**，回调里任何一句 `delete wr`
 *  都是在抽自己脚下的存储。为空则说明它已经在栈上，`wr` 的生死与本次调用无关。
 *
 * 其余几条是防"改坏了"：四个调用点各跑一批、回调里接着发起下一次写、
 * 客户端先析构（令牌失效分支）、以及"不设 self_free 时旧写法仍然成立"。
 * 它们都不是内存探针 —— 见下面那段关于内存池计数器的说明。
 *
 * 判据 1 只能在 `uvcpp_write` 这一层看：`uvcpp_tcp_client` 的四个 write 重载
 * 都只把 `int status` 交给使用者，拿不到请求对象。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_tcp.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <req/uvcpp_write.h>
#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_define.h>
#include "loop_drain.h"

#if UVCPP_ENABLE_MEMORY_POOL
#include <expand/uvcpp_page_heap.h>
#endif

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// @brief 写完成回调里的观察点。
struct probe {
  std::atomic<int> completed{0};
  std::atomic<int> failed{0};
  /// @brief 回调里 `wr->m_write_cb` **仍非空**的次数 —— 核心判据，必须为 0。
  std::atomic<int> closure_still_inside{0};
  /// @brief 回调里 `wr->is_self_free()` 为假的次数，必须为 0。
  std::atomic<int> not_self_free{0};
};

const char kPayload[] = "0123456789abcdef";

}  // namespace

int main() {
  std::cout << "[tcp_write_completion] start" << std::endl;

  uvcpp_loop loop;

  uvcpp_test::loop_drain drain_loop(&loop);
  loop.init();

  uvcpp_tcp_server server;
  std::atomic<int>  accepted{0};

  // 监听端与连接端都在**同一个线程**上泵。跨线程的话，另一个线程的活动会
  // 混进任何进程级度量里。
  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    std::cerr << "  [FAIL] server bind rc=" << rc << std::endl;
    return 2;
  }

  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int port = ntohs(name.sin_port);

  rc = server.listen([&accepted](uvcpp_tcp_client*) { accepted.fetch_add(1); }, 128);
  if (rc != 0) {
    std::cerr << "  [FAIL] server listen rc=" << rc << std::endl;
    return 2;
  }

  uvcpp_tcp_client client(&loop);
  std::atomic<bool> connected{false};
  std::atomic<int>  connect_status{-99};

  rc = client.connect("127.0.0.1", port, [&](int st) {
    connect_status.store(st);
    connected.store(true);
  });
  if (rc != 0) {
    std::cerr << "  [FAIL] client connect rc=" << rc << std::endl;
    return 2;
  }

  // 连接阶段两边都要泵：accept 在服务端的 loop 上。
  for (int i = 0; i < 3000; ++i) {
    server.get_loop()->run(UV_RUN_NOWAIT);
    loop.run(UV_RUN_NOWAIT);
    if (connected.load() && accepted.load() > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(connect_status.load() == 0,
        "connect status = " + std::to_string(connect_status.load()));
  check(accepted.load() == 1,
        "server accepted " + std::to_string(accepted.load()) + " connections");
  if (connect_status.load() != 0) return 2;

  // 此后服务端不再被泵：它的自动读会分配缓冲区。这条连接上我们只需要
  // "写得出去"（服务端不回话，`kPayload` 那么大的量内核缓冲绰绰有余）。
  auto pump = [&](int ms) {
    for (int i = 0; i < ms; ++i) {
      loop.run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  auto wait_for = [&](const std::atomic<int>& c, int want, int ms) {
    for (int i = 0; i < ms && c.load() < want; ++i) {
      loop.run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };

  pump(200);  // 静置：让连接建立带来的一次性分配先落地

  // =====================================================================
  // 判据 1：闭包不在请求对象里执行
  // =====================================================================
  {
    probe p;
    // 直接在这一层的接口上写：`uvcpp_tcp_client` 的四个重载都不把请求对象
    // 交给使用者，只有这里能看到 `wr->m_write_cb`。
    // 这条连接没开 TLS，所以直接写 stream 与走客户端的 write() 是同一条路。
    uvcpp_buf payload(kPayload, sizeof(kPayload) - 1);
    uvcpp_write* w = new uvcpp_write();
    w->set_uv_buf(payload.out_uv_buf(), true);
    w->set_self_free(true);

    const int wrc = client.get_tcp()->write(
        w, w->get_uv_buf(), 1, [&p](uvcpp_write* wr, int status) {
          if (status != 0) p.failed.fetch_add(1);
          // **本文件的核心断言。** 非空 = 正在执行的就是这个对象里的闭包，
          // 此刻 `delete wr` 就是抽掉自己脚下的存储。
          if (wr->m_write_cb != nullptr) p.closure_still_inside.fetch_add(1);
          if (!wr->is_self_free()) p.not_self_free.fetch_add(1);
          p.completed.fetch_add(1);
        });
    check(wrc == 0, "mechanism: submit rc = " + std::to_string(wrc));
    if (wrc != 0) delete w;

    wait_for(p.completed, 1, 3000);
    pump(150);

    check(p.completed.load() == 1,
          "mechanism: completion fired " + std::to_string(p.completed.load()) +
              " times");
    check(p.failed.load() == 0, "mechanism: completion status != 0");
    check(p.closure_still_inside.load() == 0,
          "mechanism: the closure was STILL inside wr->m_write_cb while it ran"
          " — `delete wr` in a completion callback is self-deletion again");
    check(p.not_self_free.load() == 0, "mechanism: self_free was not set on wr");
  }

  // =====================================================================
  // 四个调用点：各跑一批，全部完成、全部成功
  // =====================================================================
  //
  // 关于"有没有漏释放"：本来打算用内存池的 in_use 计数做探针，实测**不成立**。
  // `uvcpp_memory_pool_enterprise::alloc()` 命中 thread cache 时直接返回，**不**
  // 递增 `g_in_use`，而每一次 free 都递减 —— 于是一个块被复用一次就多减一次，
  // 那个计数根本不是"活块数"。下面这段先现场量一遍，把这个结论留在用例里，
  // 免得以后有人再拿它当漏释放的判据。
  {
    const int kN = 16;
#if UVCPP_ENABLE_MEMORY_POOL
    // 先热一遍缓存，再量一个"同样大小、分配与释放次数相同"的窗口。
    // 真是一个活块计数的话，这个窗口的净变化必然是 0。
    {
      std::vector<void*> blocks;
      for (int i = 0; i < 64; ++i) blocks.push_back(uvcpp_alloc_bytes(64));
      for (void* b : blocks) uvcpp_free_bytes(b);
      blocks.clear();

      size_t t0 = 0, before = 0, f0 = 0;
      uvcpp_memory_pool_enterprise::instance().get_stats(t0, before, f0);
      for (int i = 0; i < 64; ++i) blocks.push_back(uvcpp_alloc_bytes(64));
      for (void* b : blocks) uvcpp_free_bytes(b);

      size_t t1 = 0, after = 0, f1 = 0;
      uvcpp_memory_pool_enterprise::instance().get_stats(t1, after, f1);
      std::cout << "  [note] pool in_use over a balanced 64/64 window: "
                << static_cast<long long>(after) - static_cast<long long>(before)
                << " (0 才说明它是个活块计数；实测非 0，故本文件不用它判泄漏)"
                << std::endl;
    }
#endif

    // ---- async write(const char*, size_t, cb) ----
    // 一次一个：框架的约定是"回调到了才允许下一次异步写"，并着提交拿 UV_EALREADY。
    {
      probe p;
      int submitted = 0;
      bool submit_ok = true;
      for (int i = 0; i < kN; ++i) {
        const int wrc = client.write(kPayload, sizeof(kPayload) - 1,
                                     [&p](int st) {
                                       if (st != 0) p.failed.fetch_add(1);
                                       p.completed.fetch_add(1);
                                     });
        if (wrc != 0) {
          submit_ok = false;
          check(false, "async const char*: submit #" + std::to_string(i) +
                           " rc = " + std::to_string(wrc));
          break;
        }
        ++submitted;
        wait_for(p.completed, submitted, 2000);
      }
      pump(150);
      check(submit_ok, "async const char*: all submits accepted");
      check(p.completed.load() == submitted,
            "async const char*: " + std::to_string(p.completed.load()) + "/" +
                std::to_string(submitted) + " completions");
      check(p.failed.load() == 0,
            "async const char*: " + std::to_string(p.failed.load()) +
                " failed completions");
    }

    // ---- async write(uvcpp_buf*, cb)（所有权转移那个重载）----
    {
      probe p;
      int submitted = 0;
      for (int i = 0; i < kN; ++i) {
        uvcpp_buf* buf = new uvcpp_buf(kPayload, sizeof(kPayload) - 1);
        const int wrc = client.write(buf, [&p](int st) {
          if (st != 0) p.failed.fetch_add(1);
          p.completed.fetch_add(1);
        });
        if (wrc != 0) {
          check(false, "async uvcpp_buf*: submit #" + std::to_string(i) +
                           " rc = " + std::to_string(wrc));
          delete buf;  // 没交出去，所有权还在自己手上
          break;
        }
        ++submitted;
        wait_for(p.completed, submitted, 2000);
      }
      pump(150);
      check(p.completed.load() == submitted,
            "async uvcpp_buf*: " + std::to_string(p.completed.load()) + "/" +
                std::to_string(submitted) + " completions");
      check(p.failed.load() == 0,
            "async uvcpp_buf*: " + std::to_string(p.failed.load()) +
                " failed completions");
    }

    // ---- write_wait(const char*, size_t) ----
    {
      int bad = 0;
      for (int i = 0; i < kN; ++i) {
        if (client.write_wait(kPayload, sizeof(kPayload) - 1, 5000) != 0) ++bad;
      }
      check(bad == 0, "sync const char*: " + std::to_string(bad) + "/" +
                          std::to_string(kN) + " failed");
    }

    // ---- write_wait(uvcpp_buf*) ----
    {
      int bad = 0;
      for (int i = 0; i < kN; ++i) {
        uvcpp_buf* buf = new uvcpp_buf(kPayload, sizeof(kPayload) - 1);
        const int wrc = client.write_wait(buf, 5000);
        if (wrc != 0) {
          ++bad;
          delete buf;  // 没交出去
        }
      }
      check(bad == 0, "sync uvcpp_buf*: " + std::to_string(bad) + "/" +
                          std::to_string(kN) + " failed");
    }

    // 四批跑完连接还得是活的 —— 释放改错了地方（提前放/放两次）在这里最先露头。
    check(client.has_status(TCP_CLIENT_CONNECTED),
          "connection still alive after the batches (status=" +
              std::to_string(client.get_status()) + ")");
  }

  // =====================================================================
  // 完成回调里接着发起下一次写（热路径上真实存在的用法）
  // =====================================================================
  {
    probe p;
    const int kN = 32;
    std::function<void(int)> chain;
    chain = [&](int st) {
      if (st != 0) p.failed.fetch_add(1);
      const int done = p.completed.fetch_add(1) + 1;
      if (done < kN) {
        if (client.write(kPayload, sizeof(kPayload) - 1, chain) != 0) {
          p.failed.fetch_add(1);
        }
      }
    };
    const int wrc = client.write(kPayload, sizeof(kPayload) - 1, chain);
    check(wrc == 0, "reentrant: first submit rc = " + std::to_string(wrc));
    wait_for(p.completed, kN, 5000);
    pump(150);
    check(p.completed.load() == kN,
          "reentrant: " + std::to_string(p.completed.load()) + "/" +
              std::to_string(kN) + " completions");
    check(p.failed.load() == 0,
          "reentrant: " + std::to_string(p.failed.load()) + " failures");
  }

  // =====================================================================
  // 旧写法仍然成立：不设 self_free 时，回调自己 delete
  // =====================================================================
  // 仓库里大量代码就是这么写的（例如 tcp_client_func 里的回显服务端）。
  // 修复之后它**从"侥幸不炸"变成"确实安全"**：闭包已经在栈上，删 wr 与本次
  // 调用无关。trampoline 也不会再补一次 delete（self_free 为假）。
  {
    probe p;
    uvcpp_buf payload(kPayload, sizeof(kPayload) - 1);
    uvcpp_write* w = new uvcpp_write();
    w->set_uv_buf(payload.out_uv_buf(), true);

    const int wrc = client.get_tcp()->write(
        w, w->get_uv_buf(), 1, [&p](uvcpp_write* wr, int status) {
          if (status != 0) p.failed.fetch_add(1);
          if (wr->is_self_free()) p.not_self_free.fetch_add(1);
          delete wr;  // 旧写法
          // 删完之后**还要读一次捕获** —— 修复前这一句读的就是已释放的存储。
          p.completed.fetch_add(1);
        });
    check(wrc == 0, "legacy_delete: submit rc = " + std::to_string(wrc));
    if (wrc != 0) delete w;

    wait_for(p.completed, 1, 3000);
    pump(200);
    check(p.completed.load() == 1,
          "legacy_delete: completion fired " + std::to_string(p.completed.load()) +
              " times");
    check(p.failed.load() == 0, "legacy_delete: completion status != 0");
    check(p.not_self_free.load() == 0,
          "legacy_delete: self_free must stay false when the callback frees");
  }

  // =====================================================================
  // 客户端先析构，在飞的写走的是令牌失效那条分支
  // =====================================================================
  // 修复前那里是显式 `delete wr`，修复后只剩一句 return —— **释放全靠
  // callback_write 兜底**。这条分支最容易"改坏了还看不出来"（漏了只是慢泄漏），
  // 所以至少要确认它还能把控制权交回来、循环还能干净关掉。
  {
    uvcpp_loop loop2;

    uvcpp_test::loop_drain drain_loop2(&loop2);
    loop2.init();
    {
      uvcpp_tcp_client* c2 = new uvcpp_tcp_client(&loop2);
      std::atomic<bool> up{false};
      const int crc =
          c2->connect("127.0.0.1", port, [&up](int st) { up.store(st == 0); });
      check(crc == 0, "tokendead: connect submit rc = " + std::to_string(crc));
      for (int i = 0; i < 3000 && !up.load(); ++i) {
        loop2.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      check(up.load(), "tokendead: second client never connected");
      for (int i = 0; i < 150; ++i) {
        loop2.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      // 写发出去，然后**在它完成之前**把客户端整个删掉。
      const int wrc = c2->write(kPayload, sizeof(kPayload) - 1, [](int) {});
      check(wrc == 0, "tokendead: submit rc = " + std::to_string(wrc));
      delete c2;
      // 把挂起的完成回调跑出来（libuv 会用 UV_ECANCELED 把在飞的写交回）。
      for (int i = 0; i < 400; ++i) {
        loop2.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    // 句柄全摘干净了才可能返回 0；这不能证明写请求被释放，但能证明这条分支
    // 没有把循环搅坏（句柄悬在 handle_queue 上的话这里必然是 UV_EBUSY）。
    const int lrc = loop2.loop_close();
    check(lrc == 0,
          "tokendead: loop_close rc = " + std::to_string(lrc) +
              " (UV_EBUSY = 还有句柄挂在 handle_queue 上)");
  }

  if (g_failures == 0) {
    std::cout << "[tcp_write_completion] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[tcp_write_completion] FAIL (" << g_failures << ")" << std::endl;
  return 2;
}
