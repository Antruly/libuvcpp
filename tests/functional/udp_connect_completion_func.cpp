/**
 * @file tests/functional/udp_connect_completion_func.cpp
 * @brief `uvcpp_udp_send` / `uvcpp_connect` 的完成回调**执行的是什么存储**。
 *
 * 清单 #7 的同族：`uvcpp_tcp_client` 的写完成回调里那句 `delete wr` 已经改由
 * 跳板事后释放，同样的形状还在两处 ——
 *
 *  - `uvcpp_udp_client` / `uvcpp_udp_server` 里 `uvcpp_udp_send` 的 `delete wr`
 *    （12 处），那个 lambda 就存在 `wr->udp_send_cb` 里；
 *  - `uvcpp_tcp_client` 里 `uvcpp_connect` 的 `delete r`（5 处），那个 lambda 就
 *    存在 `r->m_connect_cb` 里。
 *
 * 两处都是"回调删掉自己正在执行的闭包"，C++ 里是未定义行为，平时不炸只因为那
 * 是回调的最后一句、删完不再读捕获。
 *
 * 修法与写路径共用一套：`uvcpp_req::invoke_completion` 先把闭包**搬到栈上**再
 * 调用，`set_self_free(true)` 让对象在闭包返回**之后**自我 `delete`。机制整个
 * 挪到了基类，三个跳板只各剩一行。
 *
 * 核心判据只有一条，但**确定性** —— 修复前必挂、修复后必过：
 *
 *  **完成回调里 `udp_send_cb` / `m_connect_cb` 必须为空。** 那两个 lambda 的存储
 *  就在这两个成员里；非空意味着此刻执行的闭包与请求对象**同生共死**，回调里
 *  任何一句 `delete` 都是在抽自己脚下的存储。
 *
 * 判据只能在 `uvcpp_udp` / `uvcpp_tcp` 这一层看：上层的 `send()` / `connect()`
 * 都只把 `int status`（或 `uvcpp_udp_send*`）交给使用者，拿不到 `uvcpp_connect`。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_tcp.h>
#include <handle/uvcpp_udp.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <net/uvcpp_udp_client.h>
#include <net/uvcpp_udp_server.h>
#include <req/uvcpp_connect.h>
#include <req/uvcpp_udp_send.h>
#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_define.h>
#include "loop_drain.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

const char kPayload[] = "udp-payload-0123456789";

}  // namespace

int main() {
  std::cout << "[udp_connect_completion] start" << std::endl;

  // 接收端：一个真的绑上了的 UDP socket，让 send 有一个存在的目的地
  // （发给没人听的端口也照样会完成，但那时钻出来的是 ICMP 差错，不是我们要
  // 测的路径）。
  uvcpp_udp_server usrv;
  std::atomic<int>  received{0};
  int rc = usrv.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    std::cerr << "  [FAIL] udp server bind rc=" << rc << std::endl;
    return 2;
  }
  usrv.recv_start([&received](uvcpp_buf*, const char*, int) {
    received.fetch_add(1);
  });
  sockaddr_in uaddr;
  int ulen = sizeof(uaddr);
  rc = usrv.get_udp()->getsockname(reinterpret_cast<sockaddr*>(&uaddr), &ulen);
  check(rc == 0, "udp server getsockname rc = " + std::to_string(rc));
  if (rc != 0) return 2;
  const int uport = ntohs(uaddr.sin_port);

  struct sockaddr_in dest;
  rc = uv_ip4_addr("127.0.0.1", uport, &dest);
  check(rc == 0, "uv_ip4_addr rc = " + std::to_string(rc));

  // =====================================================================
  // 判据 1：udp_send 的闭包不在请求对象里执行
  // =====================================================================
  {
    uvcpp_udp_client client;
    uvcpp_loop* loop = client.get_loop();

    std::atomic<int> completed{0}, failed{0}, still_inside{0}, not_self_free{0};

    uvcpp_buf payload(kPayload, sizeof(kPayload) - 1);
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(payload.out_uv_buf(), true);
    w->set_self_free(true);

    const int wrc = client.get_udp()->send(
        w, w->get_uv_buf(), 1,
        reinterpret_cast<const struct sockaddr*>(&dest),
        [&](uvcpp_udp_send* wr, int status) {
          if (status != 0) failed.fetch_add(1);
          // **本文件的核心断言。** 非空 = 正在执行的就是这个对象里的闭包。
          if (wr->udp_send_cb != nullptr) still_inside.fetch_add(1);
          if (!wr->is_self_free()) not_self_free.fetch_add(1);
          completed.fetch_add(1);
        });
    check(wrc == 0, "udp mechanism: submit rc = " + std::to_string(wrc));
    if (wrc != 0) delete w;

    for (int i = 0; i < 3000 && completed.load() == 0; ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(completed.load() == 1,
          "udp mechanism: completion fired " + std::to_string(completed.load()) +
              " times");
    check(failed.load() == 0, "udp mechanism: completion status != 0");
    check(still_inside.load() == 0,
          "udp mechanism: the closure was STILL inside wr->udp_send_cb while it"
          " ran — `delete wr` in a completion callback is self-deletion again");
    check(not_self_free.load() == 0, "udp mechanism: self_free was not set");
  }

  // =====================================================================
  // 判据 2：connect 的闭包不在请求对象里执行
  // =====================================================================
  {
    // 真的连一个监听端，这样走的是成功分支（status == 0）。
    uvcpp_tcp_server tsrv;
    std::atomic<int> accepted{0};
    tsrv.set_read_callback([](uvcpp_tcp_client&, const net_read_result&) {});
    rc = tsrv.bindIpv4("127.0.0.1", 0);
    check(rc == 0, "tcp server bind rc = " + std::to_string(rc));
    sockaddr_in taddr;
    int tlen = sizeof(taddr);
    tsrv.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&taddr), &tlen);
    const int tport = ntohs(taddr.sin_port);
    rc = tsrv.listen([&accepted](uvcpp_tcp_client*) { accepted.fetch_add(1); }, 16);
    check(rc == 0, "tcp server listen rc = " + std::to_string(rc));

    uvcpp_loop loop2;

    uvcpp_test::loop_drain drain_loop2(&loop2);
    loop2.init();
    uvcpp_tcp tcp(&loop2);

    sockaddr_in peer;
    rc = uv_ip4_addr("127.0.0.1", tport, &peer);
    check(rc == 0, "uv_ip4_addr(tcp) rc = " + std::to_string(rc));

    std::atomic<int> completed{0}, failed{0}, still_inside{0}, not_self_free{0};
    uvcpp_connect* conn = new uvcpp_connect();
    conn->set_self_free(true);

    const int crc = tcp.connect(
        conn, reinterpret_cast<const struct sockaddr*>(&peer),
        [&](uvcpp_connect* r, int status) {
          if (status != 0) failed.fetch_add(1);
          // **核心断言。** 非空 = 正在执行的就是 m_connect_cb 里那个闭包。
          if (r->m_connect_cb != nullptr) still_inside.fetch_add(1);
          if (!r->is_self_free()) not_self_free.fetch_add(1);
          completed.fetch_add(1);
        });
    check(crc == 0, "connect mechanism: submit rc = " + std::to_string(crc));
    if (crc != 0) delete conn;

    for (int i = 0; i < 3000 && completed.load() == 0; ++i) {
      tsrv.get_loop()->run(UV_RUN_NOWAIT);
      loop2.run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(completed.load() == 1,
          "connect mechanism: completion fired " +
              std::to_string(completed.load()) + " times");
    check(failed.load() == 0, "connect mechanism: completion status != 0");
    check(still_inside.load() == 0,
          "connect mechanism: the closure was STILL inside r->m_connect_cb while"
          " it ran — `delete r` in a completion callback is self-deletion again");
    check(not_self_free.load() == 0, "connect mechanism: self_free was not set");
    check(accepted.load() == 1,
          "connect mechanism: server accepted " +
              std::to_string(accepted.load()) + " connections");

    // 收尾：把连接摘干净，别把句柄留给 loop2。
    bool closed = false;
    tcp.close([&closed](uvcpp_handle*) { closed = true; });
    for (int i = 0; i < 200 && !closed; ++i) {
      loop2.run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(closed, "connect mechanism: close callback never ran");
    const int lrc = loop2.loop_close();
    check(lrc == 0,
          "connect mechanism: loop_close rc = " + std::to_string(lrc) +
              " (UV_EBUSY = 还有句柄挂在 handle_queue 上)");
  }

  // =====================================================================
  // udp_client 的四个 send 重载：全部完成、全部成功
  // =====================================================================
  // 释放改错了地方（提前放 / 放两次）会在这里先露头。
  {
    uvcpp_udp_client client;
    uvcpp_loop* loop = client.get_loop();
    auto pump_until = [&](std::atomic<int>& c, int want, int ms) {
      for (int i = 0; i < ms && c.load() < want; ++i) {
        loop->run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    };

    const int kN = 8;

    // ---- send(ip, port, const char*, len, cb) ----
    {
      std::atomic<int> done{0}, bad{0};
      int submitted = 0;
      for (int i = 0; i < kN; ++i) {
        const int src = client.send("127.0.0.1", uport, kPayload,
                                    sizeof(kPayload) - 1, [&](int st) {
                                      if (st != 0) bad.fetch_add(1);
                                      done.fetch_add(1);
                                    });
        if (src != 0) {
          check(false, "udp client send(char*): #" + std::to_string(i) +
                           " rc = " + std::to_string(src));
          break;
        }
        ++submitted;
        pump_until(done, submitted, 2000);
      }
      check(done.load() == submitted,
            "udp client send(char*): " + std::to_string(done.load()) + "/" +
                std::to_string(submitted) + " completions");
      check(bad.load() == 0, "udp client send(char*): status != 0");
    }

    // ---- send(ip, port, uvcpp_buf*, cb)（所有权转移那个重载）----
    {
      std::atomic<int> done{0}, bad{0};
      int submitted = 0;
      for (int i = 0; i < kN; ++i) {
        uvcpp_buf* buf = new uvcpp_buf(kPayload, sizeof(kPayload) - 1);
        const int src = client.send("127.0.0.1", uport, buf, [&](int st) {
          if (st != 0) bad.fetch_add(1);
          done.fetch_add(1);
        });
        if (src != 0) {
          check(false, "udp client send(buf*): #" + std::to_string(i) +
                           " rc = " + std::to_string(src));
          delete buf;  // 没交出去
          break;
        }
        ++submitted;
        pump_until(done, submitted, 2000);
      }
      check(done.load() == submitted,
            "udp client send(buf*): " + std::to_string(done.load()) + "/" +
                std::to_string(submitted) + " completions");
      check(bad.load() == 0, "udp client send(buf*): status != 0");
    }

    // ---- write_wait 那一族的两个同步重载 ----
    {
      int bad = 0;
      for (int i = 0; i < kN; ++i) {
        if (client.send_wait("127.0.0.1", uport, kPayload, sizeof(kPayload) - 1,
                             5000) != 0) {
          ++bad;
        }
      }
      check(bad == 0, "udp client send_wait(char*): " + std::to_string(bad) +
                          "/" + std::to_string(kN) + " failed");

      bad = 0;
      for (int i = 0; i < kN; ++i) {
        uvcpp_buf* buf = new uvcpp_buf(kPayload, sizeof(kPayload) - 1);
        // 没有 `send_wait(uvcpp_buf*)` 重载：cb 传 nullptr 就是"同步 + 转移
        // 所有权"那条分支。
        if (client.send("127.0.0.1", uport, buf, nullptr) != 0) {
          ++bad;
          delete buf;  // 没交出去
        }
      }
      check(bad == 0, "udp client sync send(buf*): " + std::to_string(bad) +
                          "/" + std::to_string(kN) + " failed");
    }

    check(!client.has_status(UDP_CLIENT_ERROR),
          "udp client: handle went bad after the batches (status=" +
              std::to_string(client.get_status()) + ")");
  }

  // =====================================================================
  // udp_server 的两个 send 重载（走的是它自己那份 send_impl）
  // =====================================================================
  {
    std::atomic<int> done{0}, bad{0};
    const int kN = 4;
    int submitted = 0;
    for (int i = 0; i < kN; ++i) {
      const int src = usrv.send("127.0.0.1", uport, kPayload,
                                sizeof(kPayload) - 1, [&](int st) {
                                  if (st != 0) bad.fetch_add(1);
                                  done.fetch_add(1);
                                });
      if (src != 0) {
        check(false, "udp server send(char*): #" + std::to_string(i) +
                         " rc = " + std::to_string(src));
        break;
      }
      ++submitted;
      for (int k = 0; k < 2000 && done.load() < submitted; ++k) {
        usrv.get_loop()->run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    check(done.load() == submitted,
          "udp server send(char*): " + std::to_string(done.load()) + "/" +
              std::to_string(submitted) + " completions");
    check(bad.load() == 0, "udp server send(char*): status != 0");
    check(usrv.send_wait("127.0.0.1", uport, kPayload, sizeof(kPayload) - 1,
                         5000) == 0,
          "udp server send_wait(char*) failed");
  }

  // =====================================================================
  // 旧写法仍然成立：不设 self_free 时，回调自己 delete
  // =====================================================================
  // 仓库里 test/ 下大量代码就是这么写的（pipe_func / poll_func / tcp_func）。
  // 修复之后它**从"侥幸不炸"变成"确实安全"**：闭包已经在栈上。
  {
    uvcpp_udp_client client;
    uvcpp_loop* loop = client.get_loop();
    std::atomic<int> completed{0}, not_self_free{0};

    uvcpp_buf payload(kPayload, sizeof(kPayload) - 1);
    uvcpp_udp_send* w = new uvcpp_udp_send();
    w->set_uv_buf(payload.out_uv_buf(), true);

    const int wrc = client.get_udp()->send(
        w, w->get_uv_buf(), 1,
        reinterpret_cast<const struct sockaddr*>(&dest),
        [&](uvcpp_udp_send* wr, int status) {
          (void)status;
          if (wr->is_self_free()) not_self_free.fetch_add(1);
          delete wr;  // 旧写法
          // 删完之后**还要读一次捕获** —— 修复前这一句读的就是已释放的存储。
          completed.fetch_add(1);
        });
    check(wrc == 0, "legacy udp_delete: submit rc = " + std::to_string(wrc));
    if (wrc != 0) delete w;

    for (int i = 0; i < 3000 && completed.load() == 0; ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(completed.load() == 1,
          "legacy udp_delete: completion fired " +
              std::to_string(completed.load()) + " times");
    check(not_self_free.load() == 0,
          "legacy udp_delete: self_free must stay false when the callback frees");
  }

  // =====================================================================
  // 上层 tcp_client::connect 走的那条路（connect_fn_ + 令牌）
  // =====================================================================
  // 它内部创建的 `uvcpp_connect` 现在也交给跳板释放；这条既有用例覆盖不到
  // "闭包搬走之后 connect_fn_ 还能不能正常送达"。
  {
    uvcpp_tcp_server tsrv;
    std::atomic<int> accepted{0};
    tsrv.set_read_callback([](uvcpp_tcp_client&, const net_read_result&) {});
    rc = tsrv.bindIpv4("127.0.0.1", 0);
    check(rc == 0, "client connect: server bind rc = " + std::to_string(rc));
    sockaddr_in taddr;
    int tlen = sizeof(taddr);
    tsrv.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&taddr), &tlen);
    rc = tsrv.listen([&accepted](uvcpp_tcp_client*) { accepted.fetch_add(1); },
                     16);
    check(rc == 0, "client connect: server listen rc = " + std::to_string(rc));

    uvcpp_loop loop3;

    uvcpp_test::loop_drain drain_loop3(&loop3);
    loop3.init();
    uvcpp_tcp_client c(&loop3);
    std::atomic<bool> up{false};
    std::atomic<int>  st{-99};
    const int crc = c.connect("127.0.0.1", ntohs(taddr.sin_port), [&](int s) {
      st.store(s);
      up.store(true);
    });
    check(crc == 0, "client connect: submit rc = " + std::to_string(crc));
    for (int i = 0; i < 3000 && !up.load(); ++i) {
      tsrv.get_loop()->run(UV_RUN_NOWAIT);
      loop3.run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(up.load(), "client connect: user callback never fired");
    check(st.load() == 0,
          "client connect: status = " + std::to_string(st.load()));
    check(c.has_status(TCP_CLIENT_CONNECTED),
          "client connect: not CONNECTED (status=" +
              std::to_string(c.get_status()) + ")");
  }

  std::cout << "  [note] udp datagrams received by the peer: " << received.load()
            << std::endl;

  if (g_failures == 0) {
    std::cout << "[udp_connect_completion] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[udp_connect_completion] FAIL (" << g_failures << ")"
            << std::endl;
  return 2;
}
