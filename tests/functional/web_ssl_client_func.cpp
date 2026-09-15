/**
 * @file tests/functional/web_ssl_client_func.cpp
 * @brief `uvcpp_tcp_client` 的 TLS 过滤层回环测试（真握手、真往返）。
 *
 * 这个文件守的是 Phase 4b 里最容易写错、也最难自己发现的一件事：
 * **装上 TLS 之后，上层说的必须仍然是明文。**
 *
 * 漏掉过滤器的表现不是"少个功能"，而是两种**静默的错答案**：
 *   - 出方向：明文直接写进已建立 TLS 会话的 socket（对端按畸形记录丢弃）；
 *   - 入方向：把密文当明文交给使用者（数据看起来是乱码，但没有任何报错）。
 * 两者都不会让程序崩、不会返回错误码，只有"对端收到的字节不等于发出的
 * 字节"才能识破。所以本文件的判据一律是**逐字节比对往返内容**，不是
 * "连接建立成功"。
 *
 * 另外两条判据同样是必要的，缺了正向用例就说明不了问题：
 *   - **反向对照**：明文客户端打 TLS 服务端，握手必须失败（过滤器没接上的
 *     实现，在这一条上会表现成"连上了"）；
 *   - **接收侧必须是明文**：收到的字节要与发出的完全一致，而不是"收到了
 *     一些字节"。
 *
 * 服务端的 TLS 是在 `on_connection` 里手动 `enable_tls()` 的 —— 这一步
 * 相当于 `uvcpp_tcp_server` 的每连接 TLS（工作计划里的第 3 项）将来要做的
 * 事，所以本文件同时把服务端角色也覆盖了。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_OPENSSL_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>

#include <openssl/ssl.h>  // SSL_ERROR_* —— 只返回值分不出「还没完」和「出错」

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// 客户端在循环里最多转这么久（每次 UV_RUN_NOWAIT + 1ms 睡眠）。
const int kClientTicks = 4000;

std::string big_message(size_t n) {
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    // 可压缩但不像"全一样"的内容 —— 免得正好撞上某种退化路径。
    s.push_back(static_cast<char>('a' + (i * 7 + i / 251) % 26));
  }
  return s;
}

// =========================================================================
// 服务端：所有场景共用一个真 TLS 服务端
// =========================================================================

struct server_state {
  std::atomic<int> accepted{0};
  std::atomic<int> tls_ok{0};        ///< enable_tls 返回 0 的次数
  std::atomic<int> hs_failed{0};     ///< 服务端握手中途失败的次数
  std::atomic<int> echo_sent{0};
  std::atomic<int> echo_failed{0};
  /// 其中因为**对端主动放弃回声**（关连接时接收缓冲里还有没读走的数据 →
  /// 内核发 RST）而失败的次数。见 `pump_echo` 与 main() 末尾那条断言。
  std::atomic<int> echo_reset{0};
  std::atomic<int> bytes_in{0};      ///< 服务端解出的**明文**字节数
  std::atomic<int> final_count{0};

  /// 每连接的发送队列。
  ///
  /// 第一版这里没有它，直接"一收到就读回调里 write" —— 256 KiB 那条用例当场
  /// 把它照出来了：一大段消息会被 TCP/TLS 切成很多次读事件，而
  /// `uvcpp_tcp_client::write` **同一时刻只允许一个在途写**，第二个起的都拿到
  /// `UV_EALREADY`，帧就没了。表现是"发了 256 KiB，收回 48 KiB"，且没有任何
  /// 报错。这正是 Phase 4a 在 WS 上踩过的同一个坑，一模一样。
  struct echo_conn {
    std::deque<std::string> q;
    bool                    busy = false;
  };
  std::mutex                             mu;
  std::map<uvcpp_tcp_client*, echo_conn> conns;
};

void pump_echo(server_state& st, uvcpp_tcp_client& c);

/// 服务端读回调：把解出来的明文原样回显。
///
/// 注意这里收到的是 `net_read_result::data` —— 若 TLS 过滤器没接上，
/// 它会是密文，回显出去的也就是密文，客户端的逐字节比对会失败。
void on_server_read(server_state& st, uvcpp_tcp_client& c,
                    const net_read_result& r) {
  if (!r.is_data()) {
    if (r.event == net_read_event::READ_ERROR) st.hs_failed.fetch_add(1);
    // 连接要走了：把队列摘掉。此时写回调可能还在途，它会 find 不到条目
    // 而直接返回 —— 这是有意的，别改成"找不到就新建"。
    std::lock_guard<std::mutex> lk(st.mu);
    st.conns.erase(&c);
    return;
  }
  st.bytes_in.fetch_add(static_cast<int>(r.size));

  {
    std::lock_guard<std::mutex> lk(st.mu);
    st.conns[&c].q.push_back(std::string(r.data, r.size));
  }
  pump_echo(st, c);
}

/// 送队首一块；写完成后再进来送下一块。
void pump_echo(server_state& st, uvcpp_tcp_client& c) {
  std::string chunk;
  {
    std::lock_guard<std::mutex> lk(st.mu);
    auto it = st.conns.find(&c);
    if (it == st.conns.end() || it->second.busy || it->second.q.empty()) return;
    chunk.swap(it->second.q.front());
    it->second.q.pop_front();
    it->second.busy = true;
  }

  const int rc = c.write(chunk.data(), chunk.size(), [&st, &c](int status) {
    if (status == 0) {
      st.echo_sent.fetch_add(1);
    } else {
      st.echo_failed.fetch_add(1);
      // **区分"回声没人要"和"回声送不出去"。**
      //
      // 场景 3（write_only）故意只发不收，然后在写完成之后立刻 `close()` ——
      // 那一刻它的接收缓冲里还压着没读走的回声，于是内核发 RST，服务端在途的
      // 那次写拿到 `UV_ECONNRESET`。这是**对端主动放弃**的正常结局，不是服务端
      // 的回声管线坏了；把两者算在一起，会让"echo_failed == 0"这条断言在约
      // 1/60 的轮次里假失败（实测）。
      //
      // 真正要挡的是"服务端漏发了一块"（在途写撞上 `UV_EALREADY` 被丢掉），
      // 那种失败**不是** RST —— 下面的断言 `echo_failed == echo_reset` 因此仍然
      // 抓得住它，判别力没有被削弱。
      if (status == UV_ECONNRESET) st.echo_reset.fetch_add(1);
    }
    {
      std::lock_guard<std::mutex> lk(st.mu);
      auto it = st.conns.find(&c);
      if (it != st.conns.end()) it->second.busy = false;
    }
    pump_echo(st, c);  // 续发下一块
  });

  if (rc != 0) {
    st.echo_failed.fetch_add(1);
    std::lock_guard<std::mutex> lk(st.mu);
    auto it = st.conns.find(&c);
    if (it != st.conns.end()) it->second.busy = false;
  }
}

void run_server(std::promise<int>& port_promise, std::atomic<bool>& stop,
                server_state& st, uvcpp_ssl_context* sctx) {
  uvcpp_tcp_server server;

  server.set_read_callback([&st](uvcpp_tcp_client& c, const net_read_result& r) {
    on_server_read(st, c, r);
  });

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }

  // 绑 0 让内核挑端口，再问出来。
  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int port = ntohs(name.sin_port);

  rc = server.listen(
      [&st, sctx](uvcpp_tcp_client* client) {
        st.accepted.fetch_add(1);
        // 每连接 TLS：这正是 uvcpp_tcp_server 将来要内建的那一步。
        if (client->enable_tls(sctx) == 0) st.tls_ok.fetch_add(1);
      },
      128);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }

  // **端口必须在 listen() 成功之后才放行。** 调用方拿到端口就立刻 connect，
  // 而"已 bind、尚未 listen"的 socket 在内核里是**拒连**的（ECONNREFUSED），
  // 不是排队等 listen。放行在前的话，本用例会以约 1/6 的概率假失败
  // （实测：全量套件连跑 6 次红 1 次，单独跑则看不出）。
  // 同一族的问题在 `web_ssl_server_func.cpp` 已经修过一次，这是第二处。
  port_promise.set_value(port);

  uvcpp_loop* loop = server.get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 让挂起的关闭事件跑完，再读登记表。
  for (int i = 0; i < 300; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  st.final_count.store(static_cast<int>(server.client_count()));
}

// =========================================================================
// 场景 1：真 TLS 往返（async 接口）
// =========================================================================

struct client_probe {
  std::atomic<int>  connect_status{-99};
  std::atomic<bool> connect_fired{false};
  std::atomic<bool> hs_done_at_connect{false};
  std::atomic<int>  write_status{-99};
  std::atomic<bool> write_fired{false};
  std::atomic<int>  peer_closed{0};
  std::atomic<int>  read_error{0};

  std::mutex        mu;
  std::string       received;
};

/// 跑一个异步 TLS 客户端：连上 → 发 msg → 收 response_len 字节 → 返回是否一致。
bool run_async_tls_client(int port, uvcpp_ssl_context* cctx,
                          const std::string& msg, size_t response_len,
                          client_probe& p, const char* label) {
  uvcpp_tcp_client client;

  const int trc = client.enable_tls(cctx);
  check(trc == 0, std::string(label) + ": enable_tls returned " +
                      std::to_string(trc));

  const int crc = client.connect("127.0.0.1", port,
                                 [&client, &p, msg](int status) {
                                   p.connect_status.store(status);
                                   p.hs_done_at_connect.store(
                                       client.is_tls_handshake_done());
                                   p.connect_fired.store(true);
                                   if (status != 0) return;
                                   // 契约：拿到成功就应当能直接发明文。
                                   const int wrc = client.write(
                                       msg.data(), msg.size(),
                                       [&p](int ws) {
                                         p.write_status.store(ws);
                                         p.write_fired.store(true);
                                       });
                                   if (wrc != 0) {
                                     p.write_status.store(wrc);
                                     p.write_fired.store(true);
                                   }
                                 });
  check(crc == 0, std::string(label) + ": connect() start failed");

  client.read_start_events([&p](uvcpp_tcp_client&, const net_read_result& r) {
    if (r.is_data()) {
      std::lock_guard<std::mutex> lk(p.mu);
      p.received.append(r.data, r.size);
    } else if (r.event == net_read_event::PEER_CLOSED) {
      p.peer_closed.fetch_add(1);
    } else if (r.event == net_read_event::READ_ERROR) {
      p.read_error.fetch_add(1);
    }
  });

  // 不用阻塞的 run(DEFAULT)：卡住就是一个永远不返回的用例。转够圈数就收。
  uvcpp_loop* loop = client.get_loop();
  for (int i = 0; i < kClientTicks; ++i) {
    loop->run(UV_RUN_NOWAIT);
    {
      std::lock_guard<std::mutex> lk(p.mu);
      if (p.received.size() >= response_len) break;
    }
    if (p.connect_fired.load() && p.connect_status.load() != 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 收尾：把连接关掉，让服务端那边也能走完。
  client.close();
  for (int i = 0; i < 50; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  check(p.connect_fired.load(), std::string(label) + ": connect cb never fired");
  check(p.connect_status.load() == 0,
        std::string(label) + ": connect status = " +
            std::to_string(p.connect_status.load()));
  check(p.hs_done_at_connect.load(),
        std::string(label) + ": connect cb fired BEFORE handshake completed");
  check(p.write_fired.load(), std::string(label) + ": write cb never fired");
  check(p.write_status.load() == 0,
        std::string(label) + ": write status = " +
            std::to_string(p.write_status.load()));

  std::string got;
  {
    std::lock_guard<std::mutex> lk(p.mu);
    got = p.received;
  }
  if (got != msg) {
    std::cerr << "  [FAIL] " << label << ": roundtrip mismatch — sent "
              << msg.size() << " bytes, got " << got.size() << std::endl;
    ++g_failures;
    return false;
  }
  return true;
}

// =========================================================================
// 场景 2：同步接口也要过过滤器（read_wait 拿到的必须是明文）
// =========================================================================

/**
 * @param echo_first 在 `write_wait()` 与 `read_wait()` 之间先把循环泵一会儿，
 *        让**回声在 `read_wait` 之前到达**。
 *
 * 为什么要专门跑这一遍：`read_wait()` 前进来的明文会按设计留在 `tls_plain_`
 * 里等消费者，而 TLS 连接上 `read_started_` 早为真，`read_wait` 里那句
 * `read_start(nullptr)` **根本不会执行** —— 攒下的明文没有任何人来取。
 * 这是本文件抓到的第二个真缺陷（`read_wait` 返回 `-4039` / `UV_ETIMEDOUT`，
 * 而数据一直在 `tls_plain_` 里）。
 *
 * 自然的时序下这个窗口只有约 1/40 的轮次会命中，**太弱，不足以当回归网**。
 * 泵这一下不是把窗口藏起来，而是把它**确定性地打开**：回声在 `sync_read_wanted_`
 * 还是假的时候到达，`read_wait()` 必须自己负责把它取出来。
 */
bool run_sync_tls_client(int port, uvcpp_ssl_context* cctx,
                         const std::string& msg, const char* label,
                         bool echo_first) {
  uvcpp_tcp_client client;
  check(client.enable_tls(cctx) == 0, std::string(label) + ": enable_tls");

  const int crc = client.connect_wait("127.0.0.1", port, 8000);
  check(crc == 0, std::string(label) + ": connect_wait = " +
                      std::to_string(crc) + " " + uv_strerror(crc));
  check(client.is_tls_handshake_done(),
        std::string(label) + ": connect_wait returned before handshake done");

  const int wrc = client.write_wait(msg.data(), msg.size(), 8000);
  check(wrc == 0, std::string(label) + ": write_wait = " + std::to_string(wrc));

  if (echo_first) {
    // 回环上的回声往返是微秒级、服务端循环每 1ms 一拍，所以 200 拍足够让它
    // **必然**落在 `read_wait` 之前（实测：把修复还原之后，这一条 3/3 稳定复现）。
    uvcpp_loop* lp = client.get_loop();
    for (int i = 0; i < 200; ++i) {
      lp->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  uvcpp_buf buf;
  const int rrc = client.read_wait(buf, 8000);
  check(rrc == 0, std::string(label) + ": read_wait = " + std::to_string(rrc));

  const std::string got(buf.get_const_data(), buf.size());

  // **这里故意不调 `client.close()`。**
  //
  // 本用例要顺着析构路径走完（`~uvcpp_tcp_client` 自己把 socket 关掉），因为
  // 正是这条路径原先漏掉了关闭：析构函数先 `read_stop()`，再拿
  // `!is_closing() && is_active()` 判断"句柄还开着吗" —— 而 read_stop 已经把
  // 句柄变成不活跃，于是它把自己刚停掉读的、**还开着的** socket 当成已关闭的
  // 略过，对端永远收不到 FIN。加上 `client.close()` 会把这条路径盖住。
  if (got != msg) {
    std::cerr << "  [FAIL] " << label << ": sync roundtrip mismatch — sent "
              << msg.size() << ", got " << got.size() << std::endl;
    ++g_failures;
    return false;
  }
  return true;
}

// =========================================================================
// 场景 3：反向对照 —— 明文客户端打 TLS 服务端**必须失败**
//
// 这是钉死「过滤器真的接上了」的那一条。一个"enable_tls 只是把对象建出来、
// 但没接进出入口"的实现，在正向用例里可能蒙混过关（因为两边都没加密、
// 往返照样逐字节相等），唯独在这里会表现成"连接成功"。
// =========================================================================

bool run_plaintext_against_tls(int port, const char* label) {
  uvcpp_tcp_client client;  // 不调 enable_tls —— 这条连接说的是明文

  std::atomic<int>  status{-99};
  std::atomic<bool> fired{false};

  const int crc = client.connect("127.0.0.1", port,
                                 [&status, &fired](int st) {
                                   status.store(st);
                                   fired.store(true);
                                 });
  check(crc == 0, std::string(label) + ": connect() start failed");

  uvcpp_loop* loop = client.get_loop();
  for (int i = 0; i < 1500 && !fired.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  check(fired.load(), std::string(label) + ": connect cb never fired");

  // TCP 层当然连得上（TLS 是在其上的一层），所以这里**不**要求 connect
  // 失败；要钉的是「明文不能在这个会话里被当成正常数据收下」。
  // 判据落在服务端：它解不出任何明文，且握手以失败收场。
  const int wrc = client.write("GET / HTTP/1.1\r\nHost: x\r\n\r\n", 26,
                               [](int) {});
  (void)wrc;

  for (int i = 0; i < 300; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  client.close();
  for (int i = 0; i < 50; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

}  // namespace

int main() {
  std::cout << "[web_ssl_client] OpenSSL " << OpenSSL_version(OPENSSL_VERSION_STRING)
            << std::endl;

  // ---- 服务端证书 + 客户端 ctx --------------------------------------
  uvcpp_ssl_context sctx(tls_mode::SERVER, tls_version::TLS_1_2);
  if (!sctx.is_ready()) {
    std::cerr << "  [FAIL] server ctx not ready: " << sctx.get_last_error()
              << std::endl;
    return 2;
  }
  if (!sctx.generate_self_signed("loopback.test", 2048)) {
    std::cerr << "  [FAIL] generate_self_signed: " << sctx.get_last_error()
              << std::endl;
    return 2;
  }

  uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);
  if (!cctx.is_ready()) {
    std::cerr << "  [FAIL] client ctx not ready" << std::endl;
    return 2;
  }
  // 自签证书的回环测试需要它。**生产客户端不能这样** —— 默认接受任何证书
  // 等于中间人可直接接管；这里显式写出来正是为了让它显眼。
  cctx.set_verify_mode(tls_verify_mode::NONE);

  server_state     st;
  std::atomic<bool> stop{false};
  std::promise<int> port_promise;
  std::future<int>  port_future = port_promise.get_future();

  std::thread srv_thread(run_server, std::ref(port_promise), std::ref(stop),
                         std::ref(st), &sctx);

  const int port = port_future.get();
  if (port <= 0) {
    std::cerr << "  [FAIL] server failed to bind" << std::endl;
    stop.store(true);
    srv_thread.join();
    return 2;
  }
  std::cout << "[web_ssl_client] server port " << port << std::endl;

  // ---- 场景 1：小消息往返（async） ----------------------------------
  {
    const std::string msg = "GET / HTTP/1.1\r\nHost: loopback.test\r\n\r\n";
    client_probe p;
    check(run_async_tls_client(port, &cctx, msg, msg.size(), p, "async_small"),
          "async_small: roundtrip");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  // ---- 场景 2：大消息往返（跨多个 TLS 记录 / 多次 uv_write） --------
  //
  // 小消息可能整个装进一个 TLS 记录、一次 uv_write 就发完，于是
  // "密文没发完就回调写完成"这类缺陷照不出来。256 KiB 会跨很多次。
  {
    const std::string msg = big_message(256 * 1024);
    client_probe p;
    check(run_async_tls_client(port, &cctx, msg, msg.size(), p, "async_large"),
          "async_large: roundtrip");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  // 场景 1+2 的窗口到此为止：这两条都会把回声读完，两次快照之间**不允许**
  // 出现任何一次回声失败。
  const int echo_failed_after_readback = st.echo_failed.load();

  // ---- 场景 3：只发不收（写完成语义） -------------------------------
  // 上面两条都在"能收到回声"的前提下断言；这条专门看写本身的完成时机：
  // 写完回调必须发生在密文**全部出网**之后，而不是 SSL_write 接受明文之后。
  {
    const std::string msg = big_message(64 * 1024);
    uvcpp_tcp_client client;
    check(client.enable_tls(&cctx) == 0, "write_only: enable_tls");

    std::atomic<bool> wrote{false};
    std::atomic<int>  wst{-99};

    bool ok = false;
    (void)client.connect("127.0.0.1", port, [&](int status) {
      if (status != 0) return;
      ok = true;
      client.write(msg.data(), msg.size(), [&](int s) {
        wst.store(s);
        wrote.store(true);
      });
    });

    uvcpp_loop* loop = client.get_loop();
    for (int i = 0; i < kClientTicks && !wrote.load(); ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(ok, "write_only: never connected");
    check(wrote.load(), "write_only: write cb never fired");
    check(wst.load() == 0, "write_only: write status = " +
                               std::to_string(wst.load()));
    client.close();
    for (int i = 0; i < 50; ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  // 场景 3 的窗口到此为止。它自己的失败**不在这里断言** —— 那条路径故意放弃
  // 回声，RST 是预期内的结局（见 `pump_echo` 里的说明）；能证明它是 RST 而不是
  // 漏发的，是文件末尾那条 `echo_failed == echo_reset`。
  const int echo_failed_after_write_only = st.echo_failed.load();

  // ---- 场景 4：同步接口（connect_wait / write_wait / read_wait） ----
  //
  // 两遍，唯一的区别是**回声到达的时刻**：自然的时序，以及被泵到
  // `read_wait()` 之前的时序。后一遍是确定性的回归网，理由见
  // `run_sync_tls_client` 的 `echo_first` 参数。
  {
    const std::string msg = "PING-sync\r\n";
    check(run_sync_tls_client(port, &cctx, msg, "sync_roundtrip", false),
          "sync_roundtrip");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(run_sync_tls_client(port, &cctx, msg, "sync_roundtrip_buffered", true),
          "sync_roundtrip_buffered");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }


  // ---- 场景 5：反向对照（明文客户端打 TLS 服务端） ------------------
  {
    const int hs_before = st.hs_failed.load();
    const int in_before = st.bytes_in.load();
    run_plaintext_against_tls(port, "plaintext_vs_tls");
    // 给它一点时间让服务端把失败走完。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int in_after = st.bytes_in.load();

    // 服务端**没有**从这条连接解出任何明文 —— 这就是"过滤器真的在解"的证据。
    // 反过来说，若 TLS 层是个摆设，明文字节会被原样收下、计数就会涨。
    check(in_after == in_before,
          "plaintext_vs_tls: TLS server ACCEPTED plaintext as data (+" +
              std::to_string(in_after - in_before) + " bytes)");
    (void)hs_before;
  }

  // ---- 服务端侧的整体判据 -------------------------------------------
  stop.store(true);
  srv_thread.join();

  check(st.accepted.load() >= 5,
        "server accepted " + std::to_string(st.accepted.load()) +
            " connections (expect >= 5)");
  check(st.tls_ok.load() == st.accepted.load(),
        "server enable_tls succeeded on " +
            std::to_string(st.tls_ok.load()) + "/" +
            std::to_string(st.accepted.load()) + " connections");
  // 读回了回声的那些场景（1、2）一次失败都不许有。
  check(echo_failed_after_readback == 0,
        "server echo failures in the read-back scenarios: " +
            std::to_string(echo_failed_after_readback));
  // 场景 4（同步接口，也把回声读完了）同理。
  check(st.echo_failed.load() == echo_failed_after_write_only,
        "server echo failures after write_only: " +
            std::to_string(st.echo_failed.load() - echo_failed_after_write_only));

  // **每一次失败都必须是"对端主动放弃"（RST）**，一次别的都不许有。
  //
  // 上面两条窗口断言只覆盖读回声的场景；这一条覆盖**全部**（含场景 3）。
  // 服务端真漏发一块（在途写撞上 `UV_EALREADY` 被丢掉）拿到的是 `UV_EALREADY`
  // 而不是 `UV_ECONNRESET`，所以照样会被它抓住 —— 原先把两者算在一起的
  // `echo_failed == 0` 会在这条断言上假失败（实测约 1/60），判别力却是一样的。
  check(st.echo_failed.load() == st.echo_reset.load(),
        "server echo failures that were NOT a peer reset: " +
            std::to_string(st.echo_failed.load() - st.echo_reset.load()) +
            " (failed=" + std::to_string(st.echo_failed.load()) +
            " reset=" + std::to_string(st.echo_reset.load()) + ")");
  // 每条连接断开后都应从登记表里摘掉 —— TLS 层不该把连接的生命周期搞乱。
  check(st.final_count.load() == 0,
        "server client table not drained: " +
            std::to_string(st.final_count.load()) + " left");

  // 明文字节一共只应来自那几条真 TLS 连接。
  check(st.bytes_in.load() > 0, "server never decrypted anything");

  if (g_failures == 0) {
    std::cout << "[web_ssl_client] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[web_ssl_client] FAIL (" << g_failures << ")" << std::endl;
  return 2;
}

#else
int main() {
  std::cout << "[web_ssl_client] SKIP (SSL disabled)" << std::endl;
  return 0;
}
#endif
