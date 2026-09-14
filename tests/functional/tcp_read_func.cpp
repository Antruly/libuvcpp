/**
 * @file tests/functional/tcp_read_func.cpp
 * @brief 框架层读（`net_read_result` / 自动读 / 背压）的功能测试。
 *
 * 这个文件守的是**读路径上一个已经确认的缺陷和一个已知的坑**：
 *
 *  1. **不读就永远发现不了断开**（缺陷）。断开只在读回调的 `nread < 0`
 *     分支里看得见。不注册读回调的连接，服务端既不会知道对端走了、也不会
 *     走关闭回调、也不会把自己从登记表里摘掉 —— 每连接静默泄漏一个
 *     `uvcpp_tcp_client`。修法是把自动读变成**默认行为**（`set_auto_read`
 *     默认 true），于是"忘记起读"这个失败模式从根上没有了。
 *
 *  2. **原始读回调把语义丢了**（坑）。`read_start(cb)` 给的是
 *     `void(uvcpp_buf*)`，底层那个 `ssize_t nread` 的符号信息在中间那层就
 *     没了：`nread > 0` 有 buf，`nread == 0`（EAGAIN）什么都没有，
 *     `nread < 0`（对端关闭 / 读错误）**也什么都没有**。于是"对端正常关了"
 *     和"连接被重置了"在使用者看来是同一件事 —— 而这两者要用完全不同的
 *     方式处理。`net_read_result` 把它们拆成 DATA / PEER_CLOSED /
 *     READ_ERROR 三个明确的事件，READ_ERROR 还带 libuv 错误码。
 *
 * 起真连接、真发数据、真断（含 RST），因为这些行为全都在真实的 socket
 * 事件路径上，构造对象直接调是测不到的。
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <uv.h>

#ifdef _WIN32
#include <io.h>
#define uvtest_fileno _fileno
#define uvtest_dup _dup
#define uvtest_dup2 _dup2
#define uvtest_close _close
#else
#include <unistd.h>
#define uvtest_fileno fileno
#define uvtest_dup dup
#define uvtest_dup2 dup2
#define uvtest_close close
#endif

#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "net/uvcpp_net_read.h"
#include "net/uvcpp_tcp_client.h"
#include "net/uvcpp_tcp_server.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

// =========================================================================
// 服务端形态
// =========================================================================

enum server_mode {
  /// 什么都不设：全靠默认自动读（场景 A/H —— 缺陷 1 的回归护栏）。
  MODE_AUTO_READ = 0,
  /// 服务端共用读回调（上层框架的用法）。
  MODE_SET_READ_CB,
  /// 在 on_connection 里用户自己注册读，应当顶掉自动读。
  MODE_USER_READ_START,
  /// 在 on_connection 里暂停读，稍后由服务端自己恢复（背压）。
  MODE_PAUSE_RESUME,
};

struct probe_state {
  std::atomic<int> accepted;
  std::atomic<int> data_events;
  std::atomic<int> data_bytes;
  std::atomic<int> peer_closed;
  std::atomic<int> read_error;
  std::atomic<int> error_code;
  std::atomic<int> final_count;
  /// 首次收到数据时 `is_auto_read()` 的取值（应当是 0）。
  std::atomic<int> auto_read_flag;
  /// 首次收到数据时 `has_read_callback()` 的取值（应当是 1）。
  std::atomic<int> user_read_flag;
  /// `pause_read()` 是否返回成功。
  std::atomic<int> pause_ok;
  /// 恢复读**之前**累计的数据事件数（背压的判据）。
  std::atomic<int> data_before_resume;
  /// 服务端最后一次见到的客户端（用于 pause/resume 操作）。
  std::atomic<uvcpp_tcp_client*> last_client;

  std::mutex mu;
  std::string blob;  ///< 收到过的全部字节（二进制安全，含 NUL）

  probe_state()
      : accepted(0),
        data_events(0),
        data_bytes(0),
        peer_closed(0),
        read_error(0),
        error_code(0),
        final_count(-1),
        auto_read_flag(-1),
        user_read_flag(-1),
        pause_ok(-1),
        data_before_resume(-1),
        last_client(nullptr) {}

  void append(const char* d, size_t n) {
    std::lock_guard<std::mutex> lk(mu);
    blob.append(d, n);
  }
  std::string blob_copy() {
    std::lock_guard<std::mutex> lk(mu);
    return blob;
  }
};

/// 读事件的处理。三种事件分别计数 —— 这正是被守护的语义。
void handle_read(probe_state* st, uvcpp_tcp_client& c,
                 const net_read_result& r) {
  if (r.is_data()) {
    st->data_events.fetch_add(1);
    st->data_bytes.fetch_add(static_cast<int>(r.size));
    st->append(r.data, r.size);
    if (st->data_events.load() == 1) {
      st->auto_read_flag.store(c.is_auto_read() ? 1 : 0);
      st->user_read_flag.store(c.has_read_callback() ? 1 : 0);
    }
    return;
  }
  if (r.event == net_read_event::PEER_CLOSED) {
    // 干净收尾：对端发了 FIN。**不是错误**。
    st->peer_closed.fetch_add(1);
    return;
  }
  st->read_error.fetch_add(1);
  st->error_code.store(r.error);
}

// =========================================================================
// 服务端线程
// =========================================================================

void run_server(std::promise<int>& port_promise, std::atomic<bool>& ready,
                std::atomic<bool>& stop, probe_state* st, int mode) {
  uvcpp_tcp_server server;

  // 背压场景也要有真的消费者 —— 否则跑的是自动读（数据被丢弃），
  // "恢复之后收到数据"就永远不成立，测的也就不是背压了。
  if (mode == MODE_SET_READ_CB || mode == MODE_PAUSE_RESUME) {
    server.set_read_callback(
        [st](uvcpp_tcp_client& c, const net_read_result& r) {
          handle_read(st, c, r);
        });
  }

  int rc = server.bindIpv4("127.0.0.1", 0);
  if (rc != 0) {
    port_promise.set_value(-1);
    return;
  }

  // 绑 0 让内核挑端口，再问出来 —— 固定端口会跟别的测试撞。
  sockaddr_in name;
  int namelen = sizeof(name);
  server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  port_promise.set_value(ntohs(name.sin_port));

  rc = server.listen(
      [&](uvcpp_tcp_client* client) {
        st->accepted.fetch_add(1);
        st->last_client.store(client);

        if (mode == MODE_USER_READ_START) {
          // 使用者自己注册读。这一句应当把服务端装的自动读顶掉 ——
          // 如果顶不掉，用户会拿到 UV_EALREADY 并且一个字节都收不到。
          client->read_start_events([st](uvcpp_tcp_client& c,
                                        const net_read_result& r) {
            handle_read(st, c, r);
          });
        } else if (mode == MODE_PAUSE_RESUME) {
          st->pause_ok.store(server.pause_read(client) == 0 ? 1 : 0);
        } else {
          // 自动读场景：在 accept 这一瞬间把客户端的状态记下来。
          // 这是唯一能观察到"框架装的是自动读、不是用户回调"的位置。
          st->auto_read_flag.store(client->is_auto_read() ? 1 : 0);
          st->user_read_flag.store(client->has_read_callback() ? 1 : 0);
        }
      },
      128);

  if (rc != 0) return;
  ready.store(true);

  // 背压场景用**迭代计数**而不是时钟来定序：循环每圈大约 1ms，
  // 150 圈足够让对端的数据到达内核缓冲区（读已经停了，数据就停在那里）。
  bool resumed = false;
  int  ticks   = 0;

  uvcpp_loop* loop = server.get_loop();
  while (!stop.load()) {
    loop->run(UV_RUN_NOWAIT);

    if (mode == MODE_PAUSE_RESUME && !resumed && st->accepted.load() > 0) {
      if (++ticks >= 150) {
        // 记录恢复前收到过多少 —— 判据是"暂停期间一个字节都没投递"。
        st->data_before_resume.store(st->data_events.load());

        uvcpp_tcp_client* c = st->last_client.load();
        if (c != nullptr) server.resume_read(c);
        resumed = true;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 再转几圈，让挂起的关闭事件跑完。
  for (int i = 0; i < 200; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 判据：所有连接都断开之后，登记表必须回到 0。
  // 缺陷 1（没自动读）没修的话这里会等于连接数。
  st->final_count.store(static_cast<int>(server.client_count()));
}

// =========================================================================
// 客户端侧
// =========================================================================

/// 连上去、发一段数据、停 `settle_ms` 毫秒、再关掉（FIN）。
bool run_client(int port, const std::string& payload, int settle_ms) {
  uvcpp_tcp_client client;

  std::atomic<bool> connected(false);
  std::atomic<bool> written(false);
  std::atomic<bool> closed(false);

  int rc = client.connect("127.0.0.1", port, [&](int status) {
    if (status != 0) return;
    connected.store(true);
    if (!payload.empty()) {
      // 用 data()+size() 而不是 c_str()：载荷里有 NUL，二进制安全。
      client.write(payload.data(), payload.size(),
                   [&](int ws) { (void)ws; written.store(true); });
    } else {
      written.store(true);
    }
  });
  if (rc != 0) return false;

  uvcpp_loop* loop = client.get_loop();

  for (int i = 0; i < 500 && !connected.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!connected.load()) return false;

  for (int i = 0; i < 500 && !written.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 留时间让对端把数据读走/处理完。
  for (int i = 0; i < settle_ms; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // close() 发的是 FIN —— 对端会看到 UV_EOF（PEER_CLOSED），不是错误。
  client.get_tcp()->close([&](uvcpp_handle*) { closed.store(true); });
  for (int i = 0; i < 500 && !closed.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 50; ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

/**
 * @brief 用一个**裸 libuv 句柄**连上去，然后带 `SO_LINGER {1,0}` 关闭。
 *
 * 这样发出去的是 RST 而不是 FIN，所以对端读到的是读**错误**
 * （UV_ECONNRESET）而不是 UV_EOF —— 场景 D 需要区分这两者，而框架自己的
 * 客户端没有发 RST 的接口（它的 close 走正常的 FIN 路径）。
 */
bool connect_and_reset(int port) {
  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) return false;

  uv_tcp_t sock;
  if (uv_tcp_init(&loop, &sock) != 0) {
    uv_loop_close(&loop);
    return false;
  }

  sockaddr_in addr;
  if (uv_ip4_addr("127.0.0.1", port, &addr) != 0) {
    uv_loop_close(&loop);
    return false;
  }

  bool connected = false;
  uv_connect_t req;
  req.data = &connected;
  int rc = uv_tcp_connect(&req, &sock, reinterpret_cast<const sockaddr*>(&addr),
                          [](uv_connect_t* r, int status) {
                            if (status == 0) {
                              *static_cast<bool*>(r->data) = true;
                            }
                          });
  if (rc != 0) {
    uv_loop_close(&loop);
    return false;
  }

  for (int i = 0; i < 500 && !connected; ++i) {
    uv_run(&loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!connected) {
    uv_close(reinterpret_cast<uv_handle_t*>(&sock), nullptr);
    uv_run(&loop, UV_RUN_NOWAIT);
    uv_loop_close(&loop);
    return false;
  }

  // 让服务端先 accept 完，再重置 —— 否则 RST 可能在 accept 之前就到，
  // 那时连接还没进登记表，看到的错误也就不是这个场景要测的那个。
  for (int i = 0; i < 100; ++i) {
    uv_run(&loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  linger lg;
  lg.l_onoff  = 1;
  lg.l_linger = 0;
  setsockopt(sock.socket, SOL_SOCKET, SO_LINGER,
             reinterpret_cast<const char*>(&lg), sizeof(lg));

  uv_close(reinterpret_cast<uv_handle_t*>(&sock), nullptr);
  for (int i = 0; i < 100; ++i) {
    uv_run(&loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  uv_loop_close(&loop);
  return true;
}

// =========================================================================
// 场景脚手架
// =========================================================================

struct scenario_result {
  int accepted;
  int data_events;
  int data_bytes;
  int peer_closed;
  int read_error;
  int error_code;
  int final_count;
  int auto_read_flag;
  int user_read_flag;
  int pause_ok;
  int data_before_resume;
  std::string blob;
};

scenario_result run_scenario(int mode, const std::string& payload, int settle_ms,
                            bool use_reset) {
  std::promise<int> port_promise;
  std::future<int> port_future = port_promise.get_future();
  std::atomic<bool> ready(false);
  std::atomic<bool> stop(false);
  probe_state st;

  std::thread server_thread(run_server, std::ref(port_promise),
                            std::ref(ready), std::ref(stop), &st, mode);

  const int port = port_future.get();

  scenario_result r;
  r.accepted = r.data_events = r.data_bytes = 0;
  r.peer_closed = r.read_error = r.error_code = 0;
  r.final_count = 0;
  r.auto_read_flag = r.user_read_flag = 0;
  r.pause_ok = r.data_before_resume = 0;

  if (port <= 0) {
    stop.store(true);
    server_thread.join();
    std::cerr << "  [FAIL] 服务端没起来" << std::endl;
    ++g_failures;
    return r;
  }

  while (!ready.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  if (use_reset) {
    if (!connect_and_reset(port)) {
      std::cerr << "  [FAIL] 重置客户端没连上" << std::endl;
      ++g_failures;
    }
  } else {
    run_client(port, payload, settle_ms);
  }

  // 留时间让服务端处理完所有事件。
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  stop.store(true);
  server_thread.join();

  r.accepted = st.accepted.load();
  r.data_events = st.data_events.load();
  r.data_bytes = st.data_bytes.load();
  r.peer_closed = st.peer_closed.load();
  r.read_error = st.read_error.load();
  r.error_code = st.error_code.load();
  r.final_count = st.final_count.load();
  r.auto_read_flag = st.auto_read_flag.load();
  r.user_read_flag = st.user_read_flag.load();
  r.pause_ok = st.pause_ok.load();
  r.data_before_resume = st.data_before_resume.load();
  r.blob = st.blob_copy();
  return r;
}

// =========================================================================
// stderr 捕获（场景 H 要看那条"数据被丢弃"的警告）
// =========================================================================

class stderr_capture {
 public:
  explicit stderr_capture(const char* path) : path_(path), saved_(-1) {
    std::fflush(stderr);
    saved_ = uvtest_dup(uvtest_fileno(stderr));
    if (freopen(path_.c_str(), "w", stderr) == nullptr) {
      saved_ = -1;  // 退化成不捕获，测试会因此看到 0 行文本并失败
    }
  }

  ~stderr_capture() { restore(); }

  void restore() {
    if (saved_ < 0) return;
    std::fflush(stderr);
    uvtest_dup2(saved_, uvtest_fileno(stderr));
    uvtest_close(saved_);
    saved_ = -1;
  }

  std::string read_all() {
    std::fflush(stderr);
    std::ifstream f(path_.c_str(), std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    return s;
  }

 private:
  std::string path_;
  int saved_;
};

/// 统计 `needle` 在 `hay` 里出现的次数。
int count_occurrences(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return 0;
  int n = 0;
  size_t pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    ++n;
    pos += needle.size();
  }
  return n;
}

}  // namespace

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // ---------------------------------------------------------------------
  // 场景 A：默认自动读 —— **用户完全不需要参与**。
  //
  // 这是缺陷 1 的回归护栏。护栏是"断开能被发现、客户端能被释放"，而发现
  // 断开的前提是有人读；这里的服务端一个字都没设，全靠默认的自动读。
  // 去掉自动读（或把它默认关掉）之后 final_count 就会等于连接数。
  // ---------------------------------------------------------------------
  {
    const int n = 5;
    std::promise<int> port_promise;
    std::future<int> port_future = port_promise.get_future();
    std::atomic<bool> ready(false);
    std::atomic<bool> stop(false);
    probe_state st;
    std::thread th(run_server, std::ref(port_promise), std::ref(ready),
                   std::ref(stop), &st, static_cast<int>(MODE_AUTO_READ));
    const int port = port_future.get();
    if (port > 0) {
      while (!ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      for (int i = 0; i < n; ++i) {
        run_client(port, "", 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      stop.store(true);
      th.join();
      check(st.accepted.load() == n, "A: 5 条连接都被接受了");
      // **核心断言**
      check(st.final_count.load() == 0,
            "A: 用户没设任何读回调，连接断开后仍然被释放（client_count 归零）");
      check(st.read_error.load() == 0, "A: 正常断开不算读错误");
      // 被接受的连接确实处于自动读状态，而且**不算**"用户设了读回调"——
      // 后者要是被判成 true，"用户忘了设回调"这件事就再也报不出来了。
      check(st.auto_read_flag.load() == 1, "A: 连接处于自动读状态");
      check(st.user_read_flag.load() == 0,
            "A: 自动读不算用户读回调（has_read_callback() 为假）");
    } else {
      stop.store(true);
      th.join();
      check(false, "A: 服务端没起来");
    }
  }

  // ---------------------------------------------------------------------
  // 场景 B：框架层读的数据投递，二进制安全。
  //
  // 载荷里带 NUL —— 如果哪一层把它当 C 字符串处理（strlen/append(const
  // char*)），长度就会在 NUL 处截断，这条会立刻失败。
  // ---------------------------------------------------------------------
  {
    const std::string payload("ab\0cd\0\0ef", 9);
    scenario_result r = run_scenario(MODE_SET_READ_CB, payload, 150, false);

    check(r.accepted == 1, "B: 连接被接受");
    check(r.data_events >= 1, "B: 收到了数据事件");
    check(r.data_bytes == 9, "B: 字节数与发送的一致（NUL 没把它截断）");
    check(r.blob.size() == 9 && r.blob == payload,
          "B: 收到的内容逐字节一致（含内嵌 NUL）");
    // 服务端装了共用读回调，就不该再是"自动读"状态 —— 否则说明自动读
    // 没被顶掉，两个消费者会抢同一个 stream。
    check(r.auto_read_flag == 0, "B: 不是自动读（共用读回调已接管）");
    check(r.user_read_flag == 1, "B: 读回调已注册");
    check(r.peer_closed == 1, "B: 对端关闭被识别为 PEER_CLOSED");
    check(r.read_error == 0, "B: 干净关闭不产生 READ_ERROR");
    check(r.final_count == 0, "B: 连接被释放");
  }

  // ---------------------------------------------------------------------
  // 场景 C：单发一条再关 —— 事件序列是 DATA 然后 PEER_CLOSED。
  // ---------------------------------------------------------------------
  {
    const std::string payload("hello");
    scenario_result r = run_scenario(MODE_SET_READ_CB, payload, 100, false);

    check(r.data_bytes == 5, "C: 收到 5 字节");
    check(r.peer_closed == 1, "C: 收到 1 次 PEER_CLOSED");
    check(r.read_error == 0, "C: 没有读错误");
  }

  // ---------------------------------------------------------------------
  // 场景 D：RST（连接被重置）必须报成 READ_ERROR 并带上错误码。
  //
  // 这正是原始读回调做不到的事：FIN 和 RST 在用户看来都是"没有 buf"，
  // 于是"对端正常收工"和"连接炸了"分不出来。
  // ---------------------------------------------------------------------
  {
    scenario_result r = run_scenario(MODE_SET_READ_CB, "", 0, /*use_reset=*/true);

    check(r.accepted == 1, "D: 连接被接受");
    check(r.peer_closed == 0, "D: RST 不是 PEER_CLOSED（不能被当成正常关闭）");
    check(r.read_error == 1, "D: RST 报成 READ_ERROR");
    check(r.error_code != 0, "D: READ_ERROR 带上了错误码");
    std::cerr << "  [info] RST 的错误码 = " << r.error_code
              << " (UV_ECONNRESET = " << UV_ECONNRESET << ")" << std::endl;
    check(r.error_code == UV_ECONNRESET,
          "D: 错误码就是 UV_ECONNRESET（不是别的什么错）");
    check(r.final_count == 0, "D: 出错的连接同样被释放");
  }

  // ---------------------------------------------------------------------
  // 场景 E：使用者自己注册读，必须顶掉框架的自动读。
  //
  // 顶不掉的后果很隐蔽：read_start_events 返回 UV_EALREADY（用户多半不
  // 检查返回值），自动读继续把数据丢掉，用户则一个字节都收不到。
  // ---------------------------------------------------------------------
  {
    const std::string payload("user-owned-read");
    scenario_result r = run_scenario(MODE_USER_READ_START, payload, 150, false);

    check(r.accepted == 1, "E: 连接被接受");
    check(r.data_bytes == 15, "E: 用户拿到全部 15 字节");
    check(r.blob == payload, "E: 内容一致（没有被自动读抢走）");
    check(r.auto_read_flag == 0,
          "E: 用户注册之后不再是自动读（自动读被顶掉了）");
    check(r.final_count == 0, "E: 连接被释放");
  }

  // ---------------------------------------------------------------------
  // 场景 F：背压 —— pause_read 期间一个字节都不投递，resume 之后接着投。
  //
  // 服务端在 accept 时就暂停，等 ~150 圈（≈150ms）之后再恢复。对端在这
  // 期间已经把数据发了过来，它就停在内核缓冲区里（TCP 窗口会让对端自然
  // 减速）—— 这正是背压想要的效果。
  // ---------------------------------------------------------------------
  {
    const std::string payload("backpressure-payload");
    scenario_result r = run_scenario(MODE_PAUSE_RESUME, payload, 400, false);

    check(r.accepted == 1, "F: 连接被接受");
    check(r.pause_ok == 1, "F: pause_read 返回成功");
    check(r.data_before_resume == 0,
          "F: 暂停期间一个数据事件都没有（数据留在内核缓冲区）");
    check(r.data_bytes == 20, "F: 恢复之后数据全部到达");
    check(r.blob == payload, "F: 内容一致");
  }

  // ---------------------------------------------------------------------
  // 场景 G：API 边界（不建连接）。
  // ---------------------------------------------------------------------
  {
    uvcpp_tcp_server server;
    check(server.auto_read(), "G: 自动读默认是开的");
    check(!server.has_read_callback(), "G: 默认没有共用读回调");

    server.set_auto_read(false);
    check(!server.auto_read(), "G: set_auto_read(false) 生效");

    server.set_read_callback(
        [](uvcpp_tcp_client& c, const net_read_result& r) {
          (void)c;
          (void)r;
        });
    check(server.has_read_callback(), "G: set_read_callback 之后报告已设");

    server.clear_read_callback();
    check(!server.has_read_callback(), "G: clear_read_callback 之后报告未设");

    check(server.pause_read(nullptr) != 0, "G: pause_read(nullptr) 被拒");
    check(server.resume_read(nullptr) != 0, "G: resume_read(nullptr) 被拒");

    uvcpp_tcp_client client;
    check(client.read_start_events(nullptr) == UV_EINVAL,
          "G: read_start_events(nullptr) 返回 UV_EINVAL");
    check(!client.is_auto_read(), "G: 新客户端不是自动读");

    // net_read_result 的默认值与判定。
    net_read_result d;
    check(d.event == net_read_event::DATA, "G: net_read_result 默认是 DATA");
    check(d.is_data() && !d.is_end(), "G: 默认值 is_data() 为真");
    net_read_result e;
    e.event = net_read_event::PEER_CLOSED;
    check(!e.is_data() && e.is_end(), "G: PEER_CLOSED 时 is_end() 为真");
  }

  // ---------------------------------------------------------------------
  // 场景 H：自动读把数据丢了的时候，警告**只打一次**。
  //
  // 自动读是为了"发现断开"而存在的兜底，它没有数据消费者。如果使用者
  // 其实指望收数据却忘了注册回调，那以前是彻底无声的（对端说"我发了"，
  // 这边说什么都没发生）。现在至少有一行输出指向那个连接。
  //
  // 一次就够：自动读是默认行为，每个连接、每一块数据都警告会变成噪音。
  // ---------------------------------------------------------------------
  {
    const char* path = "tcp_read_stderr_capture.txt";
    std::string captured;
    int accepted = 0;
    int final_count = -1;

    {
      stderr_capture cap(path);

      std::promise<int> port_promise;
      std::future<int> port_future = port_promise.get_future();
      std::atomic<bool> ready(false);
      std::atomic<bool> stop(false);
      probe_state st;
      std::thread th(run_server, std::ref(port_promise), std::ref(ready),
                     std::ref(stop), &st, static_cast<int>(MODE_AUTO_READ));
      const int port = port_future.get();

      if (port > 0) {
        while (!ready.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // 分三次发：TCP 可能把它们合并成一次读事件，也可能不合并 ——
        // 无论哪种，警告都只该有一条。
        uvcpp_tcp_client client;
        std::atomic<bool> connected(false);
        std::atomic<bool> closed(false);
        int rc = client.connect("127.0.0.1", port, [&](int status) {
          if (status == 0) connected.store(true);
        });
        if (rc == 0) {
          uvcpp_loop* loop = client.get_loop();
          for (int i = 0; i < 500 && !connected.load(); ++i) {
            loop->run(UV_RUN_NOWAIT);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          const char* chunks[3] = {"AAAA", "BBBBBB", "CC"};
          for (int i = 0; i < 3; ++i) {
            client.write(chunks[i], std::strlen(chunks[i]), nullptr);
            for (int k = 0; k < 20; ++k) {
              loop->run(UV_RUN_NOWAIT);
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
          }
          for (int i = 0; i < 100; ++i) {
            loop->run(UV_RUN_NOWAIT);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          client.get_tcp()->close([&](uvcpp_handle*) { closed.store(true); });
          for (int i = 0; i < 500 && !closed.load(); ++i) {
            loop->run(UV_RUN_NOWAIT);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          for (int i = 0; i < 50; ++i) {
            loop->run(UV_RUN_NOWAIT);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        stop.store(true);
        th.join();
        accepted = st.accepted.load();
        final_count = st.final_count.load();
        // 自动读没有消费者，所以数据事件必然是 0。
        check(st.data_events.load() == 0,
              "H: 自动读没有消费者，不产生数据事件");
      } else {
        stop.store(true);
        th.join();
      }

      captured = cap.read_all();
      cap.restore();
    }

    std::remove(path);

    check(accepted == 1, "H: 连接被接受");
    check(final_count == 0, "H: 即使没人读数据，断开后仍被释放");
    check(captured.find("DISCARDED") != std::string::npos,
          "H: 丢弃数据时打出了警告");
    check(count_occurrences(captured, "DISCARDED") == 1,
          "H: 警告只打一次（不是每块数据一次）");
  }

  if (g_failures == 0) {
    std::cout << "[tcp_read] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[tcp_read] FAIL (" << g_failures << " checks failed)" << std::endl;
  return 2;
}
