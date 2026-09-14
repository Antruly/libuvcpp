/**
 * @file tests/functional/web_static_server_func.cpp
 * @brief uvcpp_static_server 静态文件服务功能测试。
 *
 * 覆盖：
 *  - 构造与配置（无网络，验证构造器/setter 不崩溃）；
 *  - 静态文件返回（真实 HTTP 往返）；
 *  - SPA 回退（不存在的路由 → index.html）；
 *  - mtime 实时更新（文件改写后下一请求返回新内容，即「动态加载」）；
 *  - 禁用 SPA 回退时的 404；
 *  - 目录穿越防护（段级归一化，含"合法名字里带 .."的反例）；
 *  - **挂载前缀**：`/static` 与 `/static/` 都要落到索引文件；
 *    `/staticfoo` 不算命中（挂载点是路径段，不是字符串前缀）；
 *  - **目录请求**：`/sub` 与 `/sub/` 都落到 `/sub/index.html`；
 *  - **HEAD 不带 body**（但 Content-Length 反映 GET 应有的长度）；
 *  - **连接代次号**：单调递增、永不复用；半途断连不写错连接。
 *
 * 通过真实的 uvcpp_http_server（后台线程）+ uvcpp_http_client（阻塞同步）
 * 端到端验证，参考 tcp_server_func.cpp 的 server 线程模式。
 *
 * 为什么要裸 socket 发 HEAD
 * -------------------------
 * `uvcpp_http_client::send_wait` 是按 `Content-Length` 等 body 的，而 HEAD 的
 * body 本来就不该有 —— 它只会一路等到超时。所以 HEAD 那条用例自己拼报文、
 * 收原始字节（`raw_exchange`），断言的是**字节层面**"头之后什么都没有"，
 * 而不是某个被客户端库加工过的视图。
 */
#include <iostream>
#include <string>
#include <fstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <vector>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include "net/uvcpp_net_read.h"
#include "net/uvcpp_tcp_client.h"
#include "req/uvcpp_work.h"
#include <web/uvcpp_static_server.h>
#include <web/uvcpp_http_server.h>
#include <web/uvcpp_http_client.h>

#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>   // _mkdir / _rmdir
#endif

using namespace uvcpp;

// =========================================================================
// 临时目录与文件写入工具
// =========================================================================
static const char* kTmpDir = "uvcpp_static_test_tmp";

static std::string tmp_path(const std::string& rel) {
  return std::string(kTmpDir) + "/" + rel;
}

static bool write_text(const std::string& rel, const std::string& content) {
  std::ofstream f(tmp_path(rel).c_str(), std::ios::binary | std::ios::trunc);
  if (!f.good()) return false;
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
  f.close();
  return true;
}

/**
 * @brief 确保目录存在；**已存在也算成功**。
 *
 * `_mkdir` 在目录已存在时返回失败 —— 于是"上一次跑挂了、没清干净"会变成
 * "这一次所有用例都返回 false"，而且一个字都不打印。一个因为环境残留而
 * 静默报 FAIL 的夹具比没有夹具更坏：它让人分不清是产品坏了还是地没扫。
 */
static bool ensure_dir(const std::string& abs) {
  struct stat st;
  if (stat(abs.c_str(), &st) == 0) return (st.st_mode & S_IFMT) == S_IFDIR;
#ifdef _WIN32
  return _mkdir(abs.c_str()) == 0;
#else
  return mkdir(abs.c_str(), 0755) == 0;
#endif
}

/// 根目录下的内容；`kAppJs` 的长度在 HEAD 用例里被当成期望的 Content-Length。
static const char* kAppJs = "console.log('JS-V1');";

static bool prepare_fs() {
  if (!ensure_dir(kTmpDir) || !ensure_dir(tmp_path("sub"))) {
    std::cout << "  [err] 建不出临时目录 " << kTmpDir << "\n";
    return false;
  }

  // `lib..min.js` 是**合法文件名**，名字里带两个点。旧的 `find("..")` 子串
  // 匹配会把它拒掉 —— 那条用例正是用来盯住这个误杀的。
  if (!write_text("lib..min.js", "MINIFIED")) return false;
  if (!write_text("sub/index.html", "<html>SUB-INDEX</html>")) return false;

  // 旧连接完成回调那个用例用的大文件：读它要走线程池，我们才能把"读盘还没
  // 回来客户端就断了"这件事摆到台面上（见 `PoolBlocker`）。
  //
  // **尺寸是调过的，别顺手加大**：这个大小要同时满足两件事 —— 读盘要**快到**
  // 在断开被处理之前回来（否则那笔写根本排不上队，窗口不存在），响应又要
  // **大到**填不满 socket 缓冲区（否则写当场就完成了，照样没有在途回调）。
  // 4 MiB 就栽在前一条上：读盘慢过 `settle_ms`，写压根没发出去，把守卫拆掉
  // 跑 30 轮全绿 —— 用例看着更"狠"，实际覆盖归零。
  std::string big(256 * 1024, 'x');
  if (!write_text("big.bin", big)) return false;

  return write_text("index.html", "<html>INDEX-V1</html>") &&
         write_text("app.js", kAppJs);
}

static void cleanup_fs() {
  std::remove(tmp_path("index.html").c_str());
  std::remove(tmp_path("app.js").c_str());
  std::remove(tmp_path("lib..min.js").c_str());
  std::remove(tmp_path("big.bin").c_str());
  std::remove(tmp_path("sub/index.html").c_str());
#ifdef _WIN32
  _rmdir(tmp_path("sub").c_str());
  _rmdir(kTmpDir);
#else
  rmdir(tmp_path("sub").c_str());
  rmdir(kTmpDir);
#endif
}

// =========================================================================
// 后台静态服务：在独立线程跑 HTTP server，端口 0 自动分配
// （server + static_svr 均在后台线程内创建/析构，loop 只在同线程跑，安全）
// =========================================================================

/** @brief 连接建立时记录的 (指针, 代次号)，用来验证"永不复用"。 */
struct ConnObservation {
  uvcpp_tcp_client* client;
  uint64_t generation;
  uint64_t generation_at_close;
};

struct StaticTestServer {
  std::string root;
  std::string url_root;
  bool spa_fallback;

  std::promise<int> port_promise;
  std::atomic<bool> stop{false};
  std::thread thread;

  /// 只被 loop 线程写、测试线程在 shutdown() 之后读，所以用 mutex 护住。
  std::mutex obs_mutex;
  std::vector<ConnObservation> observations;
  int closes = 0;

  StaticTestServer(const std::string& r, bool spa,
                   const std::string& mount = std::string("/"))
      : root(r), url_root(mount), spa_fallback(spa) {}

  int start() {
    thread = std::thread([this]() {
      uvcpp_static_server static_svr(root, url_root);
      static_svr.set_spa_fallback(spa_fallback);
      uvcpp_http_server server;

      // 代次号只能从 loop 线程上问 —— 测试线程拿不到 server 对象。
      server.on_connection([this, &server](uvcpp_tcp_client* c) {
        ConnObservation o;
        o.client = c;
        o.generation = server.connection_generation(c);
        o.generation_at_close = 0;
        std::lock_guard<std::mutex> lk(obs_mutex);
        observations.push_back(o);
      });
      server.on_connection_close([this, &server](uvcpp_tcp_client* c) {
        std::lock_guard<std::mutex> lk(obs_mutex);
        ++closes;
        for (size_t i = 0; i < observations.size(); ++i) {
          if (observations[i].client == c) {
            observations[i].generation_at_close =
                server.connection_generation(c);
          }
        }
      });

      server.on_request(static_svr.handler(&server));

      server.bind("127.0.0.1", 0);
      // 读取系统分配的实际端口
      sockaddr_in name;
      int namelen = sizeof(name);
      server.get_tcp_server()->get_tcp()->getsockname(
          reinterpret_cast<sockaddr*>(&name), &namelen);
      const int port = ntohs(name.sin_port);

      // 先 listen 再发布端口：否则 start() 返回时套接字还没进入 LISTEN，
      // 调用方立刻 connect 会拿到 ECONNREFUSED。本文件目前靠客户端重连掩盖了
      // 这一点，但那是运气，不是保证（web_http_server_func.cpp 里没有重试的
      // 那几条测试正是因此间歇性失败）。
      server.listen();

      port_promise.set_value(port);
      while (!stop.load()) {
        server.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      // 循环停了、连接登记表也空了，这时再问一次代次号：**全部应当是 0**。
      // 这里把（可能已经悬垂的）旧指针当 map 的 key 用，不解引用 —— 这正是
      // 生产代码里 `connection_matches()` 做的事。
      std::lock_guard<std::mutex> lk(obs_mutex);
      for (size_t i = 0; i < observations.size(); ++i) {
        observations[i].generation_at_close =
            server.connection_generation(observations[i].client);
      }
    });
    return port_promise.get_future().get();
  }

  void shutdown() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }

  size_t observation_count() {
    std::lock_guard<std::mutex> lk(obs_mutex);
    return observations.size();
  }

  ConnObservation observation(size_t i) {
    std::lock_guard<std::mutex> lk(obs_mutex);
    return observations[i];
  }
};

// =========================================================================
// 客户端小工具
// =========================================================================

// 阻塞式 GET：每次新建 client + connect_wait + send_wait（带连接重试，容忍 server 启动延迟）
static bool http_get(int port, const std::string& path, int& status, std::string& body) {
  uvcpp_http_response resp;
  int rc = -1;
  for (int i = 0; i < 40; ++i) {
    uvcpp_http_client client;
    rc = client.connect_wait("127.0.0.1", port, 2000);
    if (rc != 0) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); continue; }
    rc = client.send_wait(uvcpp_http_request::make_get(path), resp, 3000);
    if (rc == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (rc != 0) return false;
  status = static_cast<int>(resp.status_code);
  body.assign(resp.body.get_const_data(), resp.body.size());
  return true;
}

/**
 * @brief 裸交换：自己拼请求报文、收到对端关闭为止的全部原始字节。
 *
 * HEAD 必须走这条路（原因见文件头）。请求里带上 `Connection: close`，
 * 这样响应之后服务端会关连接，我们就能用"读到 EOF"当作"收完了"。
 */
static bool raw_exchange(int port, const std::string& req, std::string& out) {
  // 注意声明顺序：`client` 放在最后，于是它**最先**析构 —— 读回调捕获的
  // `acc` / `ended` 一定比它活得久。
  std::atomic<bool> connected(false);
  std::atomic<bool> ended(false);
  std::string acc;
  uvcpp_tcp_client client;

  int rc = client.connect("127.0.0.1", port, [&](int st) {
    if (st != 0) return;
    connected.store(true);
    client.read_start_events(
        [&](uvcpp_tcp_client&, const net_read_result& r) {
          if (r.is_data() && r.data != nullptr && r.size > 0) {
            acc.append(r.data, r.size);
          } else if (r.is_end()) {
            ended.store(true);
          }
        });
    client.write(req.data(), req.size(), [](int) {});
  });
  if (rc != 0) return false;

  uvcpp_loop* loop = client.get_loop();
  for (int i = 0; i < 500 && !connected.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 4000 && !ended.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  out = acc;
  return ended.load();
}

/** @brief 发一个请求然后**立刻**断开，不等任何响应。 */
static bool fire_and_disconnect(int port, const std::string& path,
                                int settle_ms) {
  // 同上：`client` 最后声明 → 最先析构。
  std::atomic<bool> connected(false);
  std::atomic<bool> written(false);
  std::atomic<bool> closed(false);
  const std::string req = "GET " + path +
                          " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "Connection: close\r\n\r\n";
  uvcpp_tcp_client client;
  int rc = client.connect("127.0.0.1", port, [&](int st) {
    if (st != 0) return;
    connected.store(true);
    client.write(req.data(), req.size(), [&](int) { written.store(true); });
  });
  if (rc != 0) return false;

  uvcpp_loop* loop = client.get_loop();
  for (int i = 0; i < 500 && !connected.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 500 && !written.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  loop->run(UV_RUN_NOWAIT);
  std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));

  // **显式 close()，不是等析构。**
  //
  // 一开始这里靠客户端析构来断开，结果服务端那边一条关闭都没收到（`closes`
  // 计数是 0）：`~uvcpp_tcp_client()` 只在句柄还 `is_active()` 时才去关，
  // 而且它绕开框架的关闭通知 —— 句柄没关，对端自然等不到 FIN，"连接已经
  // 没了"这个前提就从来没成立过，整个用例形同虚设。
  // 走 close() 才是文档里说的"主动关闭的唯一正确入口"。
  client.close([&closed]() { closed.store(true); });
  for (int i = 0; i < 500 && !closed.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return written.load();
}

/**
 * @brief 发请求 → **等到服务端确实开始回包** → 停止收 → 立刻断开。
 *
 * 构造的是「对端在**回包回了一半**的时候死掉」这个场景：等到第一个响应字节
 * 到达（证明服务端那笔写已经排进 libuv 写队列），随即 `read_pause()` 不再
 * 排空 socket，让那笔写永远完不成，然后断开。
 *
 * **注意它撞的不是写完成回调的那个 UAF。** 这点是实测出来的，不是推的：
 * 最初以为「保证有在途写」就等于撞上了那个窗口，于是在这个形状下把
 * `alive_token_` 守卫拆掉跑了 30 轮 —— 一轮都没崩；给守卫加计数探针，在
 * 这个形状下更是**一次都没命中**。真正能命中的是上面 (a) 那个"发完就断"的
 * 形状（拆掉守卫 200 次崩 9 次）。
 *
 * 之所以还是留着：它测的是另一件事 —— 连接在响应写了一半时消失，服务端要
 * 能扛住并且不影响后续请求。这个场景本身值得盯，(a) 覆盖不到它。
 *
 * 返回 false 表示这次没能构造出窗口（没等到首字节），调用方应当视为失败而
 * 不是静默跳过。
 */
static bool fire_and_disconnect_midwrite(int port, const std::string& path) {
  std::atomic<bool> connected(false);
  std::atomic<bool> written(false);
  std::atomic<bool> got_first(false);
  std::atomic<bool> closed(false);
  const std::string req = "GET " + path +
                          " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "Connection: close\r\n\r\n";
  uvcpp_tcp_client client;  // 最后声明 → 最先析构
  int rc = client.connect("127.0.0.1", port, [&](int st) {
    if (st != 0) return;
    connected.store(true);
    client.read_start_events(
        [&](uvcpp_tcp_client& c, const net_read_result& r) {
          if (r.is_data() && r.size > 0 && !got_first.load()) {
            got_first.store(true);
            // 从这一刻起不再排空 socket：服务端在途的那笔写再无完成的可能。
            c.read_pause();
          }
        });
    client.write(req.data(), req.size(), [&](int) { written.store(true); });
  });
  if (rc != 0) return false;

  uvcpp_loop* loop = client.get_loop();
  for (int i = 0; i < 500 && !connected.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 2000 && !got_first.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!got_first.load()) return false;

  client.close([&closed]() { closed.store(true); });
  for (int i = 0; i < 500 && !closed.load(); ++i) {
    loop->run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

/**
 * @brief 把 libuv 线程池**占满**，直到 `release_all()`。
 *
 * 为什么需要它：要测「异步完成回来时连接已经没了」，就得让那个窗口稳定存在。
 * 第一次写这个用例时只是"发完立刻断开"，结果把守卫改成恒真、跑三遍**全绿**
 * —— 256KB 的本地读在断开被处理之前就回来了，那个窗口只有几十微秒，靠时序
 * 是撞不上的（那样的用例只是装饰）。
 *
 * 线程池是**进程级**的：服务器那边的 stat/读盘和我们这里的占位任务排的是
 * 同一条队。占住它，服务器侧的完成回调就必然排在断开事件之后 —— 窗口从
 * 微秒级变成我们说了算。
 */
struct PoolBlocker {
  std::atomic<int> started;
  std::atomic<int> done;
  std::atomic<bool> release;
  std::vector<uvcpp_work*> items;
  uvcpp_loop* loop;

  explicit PoolBlocker(uvcpp_loop* lp)
      : started(0), done(0), release(false), loop(lp) {}

  /// 塞入 n 个占位任务，等到"再没有新线程能起来"（= 每个池线程都被卡住）。
  void occupy(int n) {
    for (int i = 0; i < n; ++i) {
      uvcpp_work* w = new uvcpp_work();
      w->queue_work(
          loop,
          [this](uvcpp_work*) {
            started.fetch_add(1);
            while (!release.load()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
          },
          [this](uvcpp_work* self, int) {
            delete self;
            done.fetch_add(1);
          });
      items.push_back(w);
    }
    // started 停止增长 = 池子里每个线程都卡在上面那个循环里了。
    int last = -1;
    for (int i = 0; i < 400 && started.load() != last; ++i) {
      last = started.load();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  void release_all() { release.store(true); }

  /// 泵到所有占位任务都收尾（after_work 回调跑了、对象删了）。
  void drain() {
    for (int i = 0; i < 3000 && done.load() < static_cast<int>(items.size());
         ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    items.clear();
  }

  int peak_threads() const { return started.load(); }
};

/**
 * @brief 一个"发完请求先不急着收"的裸 GET，好在中间插别的事情。
 *
 * 需要它是因为 `http_get()` 是阻塞的：必须能在 B 的请求已经发出、但响应还没
 * 回来的时候**放行线程池**，否则 B 自己也会卡在池子后面，什么都测不到。
 */
struct PendingGet {
  std::atomic<bool> connected;
  std::atomic<bool> written;
  std::atomic<bool> ended;
  std::string acc;
  std::string req;
  uvcpp_tcp_client client;  // 最后声明 → 最先析构

  PendingGet() : connected(false), written(false), ended(false) {}

  bool start(int port, const std::string& path) {
    req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    PendingGet* self = this;
    return client.connect("127.0.0.1", port, [self](int st) {
             if (st != 0) return;
             self->connected.store(true);
             self->client.read_start_events(
                 [self](uvcpp_tcp_client&, const net_read_result& r) {
                   if (r.is_data() && r.data != nullptr && r.size > 0) {
                     self->acc.append(r.data, r.size);
                   } else if (r.is_end()) {
                     self->ended.store(true);
                   }
                 });
             self->client.write(self->req.data(), self->req.size(),
                                [self](int) { self->written.store(true); });
           }) == 0;
  }

  void pump(int ms) {
    uvcpp_loop* loop = client.get_loop();
    for (int i = 0; i < ms; ++i) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  /// 取 `\r\n\r\n` 之后的 body。
  std::string body() const {
    const size_t sep = acc.find("\r\n\r\n");
    return sep == std::string::npos ? std::string() : acc.substr(sep + 4);
  }
};

/** @brief 在 `hay` 里找 `needle`（大小写不敏感，只用于读响应头）。 */
static bool header_contains(const std::string& hay, const std::string& needle) {
  if (needle.empty() || hay.size() < needle.size()) return false;
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    size_t j = 0;
    while (j < needle.size()) {
      char a = hay[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) break;
      ++j;
    }
    if (j == needle.size()) return true;
  }
  return false;
}

// =========================================================================
// Test 1: 构造与配置（无网络）
// =========================================================================
static bool test_construct_and_config() {
  uvcpp_static_server s1("frontend/dist");
  uvcpp_static_server s2("frontend/dist", "/static", "home.html");
  uvcpp_static_server s3("", "", "index.html");
  if (s1.cache_size() != 0) return false;
  s1.set_spa_fallback(false);
  s1.set_cache_enabled(false);
  s1.clear_cache();
  return true;
}

// =========================================================================
// Test 2: 静态文件返回 + SPA 回退 + mtime 实时更新
// =========================================================================
static bool test_serve_and_update() {
  if (!prepare_fs()) return false;

  StaticTestServer runner(kTmpDir, /*spa_fallback=*/true);
  int port = runner.start();

  bool ok = true;
  int status = 0;
  std::string body;

  // 2.1 根路径 -> index.html
  if (!http_get(port, "/", status, body)) ok = false;
  else if (status != 200 || body != "<html>INDEX-V1</html>") {
    std::cout << "  [err] GET / -> " << status << " '" << body << "'\n";
    ok = false;
  }

  // 2.2 具体静态文件 -> app.js
  if (!http_get(port, "/app.js", status, body)) ok = false;
  else if (status != 200 || body != kAppJs) {
    std::cout << "  [err] GET /app.js -> " << status << " '" << body << "'\n";
    ok = false;
  }

  // 2.3 SPA 回退：不存在的路由 -> index.html
  if (!http_get(port, "/some/client/route", status, body)) ok = false;
  else if (status != 200 || body != "<html>INDEX-V1</html>") {
    std::cout << "  [err] GET /some/client/route -> " << status << " '" << body << "'\n";
    ok = false;
  }

  // 2.4 实时更新：改写 app.js（大小与内容均变）后，下一请求应返回新内容
  if (!write_text("app.js", "console.log('JS-V2-UPDATED');")) ok = false;
  else {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    if (!http_get(port, "/app.js", status, body)) ok = false;
    else if (status != 200 || body != "console.log('JS-V2-UPDATED');") {
      std::cout << "  [err] GET /app.js(after update) -> " << status << " '" << body << "'\n";
      ok = false;
    }
  }

  // 2.5 目录请求：`/sub` 与 `/sub/` 都要落到 `/sub/index.html`。
  //     之前这里会去 stat 目录本身，Windows 上 404、Linux 上把目录当文件读
  //     出一个**空 body 的 200** —— 两种都不对。
  if (!http_get(port, "/sub/", status, body)) ok = false;
  else if (status != 200 || body != "<html>SUB-INDEX</html>") {
    std::cout << "  [err] GET /sub/ -> " << status << " '" << body << "'\n";
    ok = false;
  }
  if (!http_get(port, "/sub", status, body)) ok = false;
  else if (status != 200 || body != "<html>SUB-INDEX</html>") {
    std::cout << "  [err] GET /sub -> " << status << " '" << body << "'\n";
    ok = false;
  }

  runner.shutdown();
  cleanup_fs();
  return ok;
}

// =========================================================================
// Test 3: 禁用 SPA 回退时：404 + 目录穿越防护
// =========================================================================
static bool test_not_found_and_traversal() {
  if (!prepare_fs()) return false;

  StaticTestServer runner(kTmpDir, /*spa_fallback=*/false);
  int port = runner.start();

  bool ok = true;
  int status = 0;
  std::string body;

  // 3.1 存在的文件 -> 200
  if (!http_get(port, "/index.html", status, body)) ok = false;
  else if (status != 200 || body != "<html>INDEX-V1</html>") {
    std::cout << "  [err] GET /index.html -> " << status << "\n";
    ok = false;
  }

  // 3.2 不存在的文件（回退已禁用）-> 404
  if (!http_get(port, "/missing.css", status, body)) ok = false;
  else if (status != 404) {
    std::cout << "  [err] GET /missing.css -> " << status << " (expect 404)\n";
    ok = false;
  }

  // 3.3 目录穿越 -> 404
  if (!http_get(port, "/../secret.txt", status, body)) ok = false;
  else if (status != 404) {
    std::cout << "  [err] GET /../secret.txt -> " << status << " (expect 404)\n";
    ok = false;
  }

  // 3.4 爬出根：归一化时栈会空，直接拒绝（不是静默截断成 /passwd）
  if (!http_get(port, "/a/../../etc/passwd", status, body)) ok = false;
  else if (status != 404) {
    std::cout << "  [err] GET /a/../../etc/passwd -> " << status
              << " (expect 404)\n";
    ok = false;
  }

  // 3.5 **反例**：名字里带 `..` 的合法文件必须能取到。
  //     旧的 `find("..")` 子串匹配会在这里误杀。
  if (!http_get(port, "/lib..min.js", status, body)) ok = false;
  else if (status != 200 || body != "MINIFIED") {
    std::cout << "  [err] GET /lib..min.js -> " << status << " '" << body
              << "' (expect 200, 合法文件名不该被当成穿越)\n";
    ok = false;
  }

  // 3.6 `..` 被归一化掉但没越界：`/sub/../app.js` == `/app.js`
  if (!http_get(port, "/sub/../app.js", status, body)) ok = false;
  else if (status != 200 || body != kAppJs) {
    std::cout << "  [err] GET /sub/../app.js -> " << status << "\n";
    ok = false;
  }

  runner.shutdown();
  cleanup_fs();
  return ok;
}

// =========================================================================
// Test 4: 挂载前缀
// =========================================================================
//
// `url_root` 这个参数以前在两处漏了：
//   - `/static` 与 `/static/` 都进不了索引分支（先把前缀剥掉再判"是不是根"
//     才成立），Windows 上 404、Linux 上空 body 的 200；
//   - 前缀是**字符串**比较而不是**路径段**比较，于是 `/staticfoo` 会被当成
//     "/static" + "foo" 服务出去 —— 等于从挂载点里溜出去。
static bool test_mount_prefix() {
  if (!prepare_fs()) return false;

  StaticTestServer runner(kTmpDir, /*spa_fallback=*/true, /*mount=*/"/static");
  int port = runner.start();

  bool ok = true;
  int status = 0;
  std::string body;

  // 4.1 挂载点本身 -> 索引文件（这条以前是 404 / 空 200）
  if (!http_get(port, "/static", status, body)) ok = false;
  else if (status != 200 || body != "<html>INDEX-V1</html>") {
    std::cout << "  [err] GET /static -> " << status << " '" << body
              << "' (expect index)\n";
    ok = false;
  }

  // 4.2 挂载点带尾斜杠 -> 同一个索引文件
  if (!http_get(port, "/static/", status, body)) ok = false;
  else if (status != 200 || body != "<html>INDEX-V1</html>") {
    std::cout << "  [err] GET /static/ -> " << status << " '" << body
              << "' (expect index)\n";
    ok = false;
  }

  // 4.3 挂载点下的文件
  if (!http_get(port, "/static/app.js", status, body)) ok = false;
  else if (status != 200 || body != kAppJs) {
    std::cout << "  [err] GET /static/app.js -> " << status << "\n";
    ok = false;
  }

  // 4.4 挂载点下的目录
  if (!http_get(port, "/static/sub/", status, body)) ok = false;
  else if (status != 200 || body != "<html>SUB-INDEX</html>") {
    std::cout << "  [err] GET /static/sub/ -> " << status << "\n";
    ok = false;
  }

  // 4.5 **反例**：`/staticfoo` 不是挂载点内的路径。
  //     SPA 回退开着，所以它会在挂载点里找不到 -> 回退到 index；
  //     关键是**不能**被当成 "/static" + "foo" 而取到 dist/foo。
  if (!http_get(port, "/staticfoo", status, body)) ok = false;
  else if (body == "MINIFIED" || body == kAppJs) {
    std::cout << "  [err] GET /staticfoo 取到了挂载点外的文件（'" << body
              << "'）\n";
    ok = false;
  }

  // 4.6 挂载点之外的路径一律不进这个处理器（on_request 兜底会回 404）
  if (!http_get(port, "/elsewhere.js", status, body)) ok = false;
  else if (body == kAppJs) {
    std::cout << "  [err] GET /elsewhere.js 取到了挂载点内的文件\n";
    ok = false;
  }

  runner.shutdown();
  cleanup_fs();
  return ok;
}

// =========================================================================
// Test 5: HEAD 不带 body
// =========================================================================
static bool test_head_no_body() {
  if (!prepare_fs()) return false;

  StaticTestServer runner(kTmpDir, /*spa_fallback=*/false);
  int port = runner.start();

  bool ok = true;
  std::string raw;
  const std::string req =
      "HEAD /app.js HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";

  if (!raw_exchange(port, req, raw)) {
    std::cout << "  [err] HEAD 没收完响应（读到 " << raw.size() << " 字节）\n";
    runner.shutdown();
    cleanup_fs();
    return false;
  }

  const size_t sep = raw.find("\r\n\r\n");
  if (sep == std::string::npos) {
    std::cout << "  [err] HEAD 响应没有头结束标记: '" << raw << "'\n";
    runner.shutdown();
    cleanup_fs();
    return false;
  }

  const std::string head = raw.substr(0, sep);
  const std::string body = raw.substr(sep + 4);

  if (!header_contains(head, "200")) {
    std::cout << "  [err] HEAD 状态不是 200:\n" << head << "\n";
    ok = false;
  }
  // 关键：Content-Length 要反映 **GET 应有的长度**，而不是 0。
  if (!header_contains(head, "content-length: " +
                                 std::to_string(std::strlen(kAppJs)))) {
    std::cout << "  [err] HEAD 的 Content-Length 不是 " << std::strlen(kAppJs)
              << ":\n" << head << "\n";
    ok = false;
  }
  // 关键：头之后**一个字节都没有**。
  if (!body.empty()) {
    std::cout << "  [err] HEAD 带了 " << body.size() << " 字节 body\n";
    ok = false;
  }

  runner.shutdown();
  cleanup_fs();
  return ok;
}

// =========================================================================
// Test 6: 连接代次号 + 半途断连
// =========================================================================
static bool test_generation_and_midflight_disconnect() {
  if (!prepare_fs()) return false;

  StaticTestServer runner(kTmpDir, /*spa_fallback=*/false);
  int port = runner.start();

  bool ok = true;
  int status = 0;
  std::string body;

  // 6.1 两次连接拿到两个**不同**的代次号，且都不是 0。
  if (!http_get(port, "/app.js", status, body) || status != 200) {
    std::cout << "  [err] 第一次请求失败\n";
    ok = false;
  }
  if (!http_get(port, "/index.html", status, body) || status != 200) {
    std::cout << "  [err] 第二次请求失败\n";
    ok = false;
  }
  if (runner.observation_count() < 2) {
    std::cout << "  [err] 只观察到 " << runner.observation_count()
              << " 条连接（期望 >= 2）\n";
    ok = false;
  } else {
    const ConnObservation a = runner.observation(0);
    const ConnObservation b = runner.observation(1);
    if (a.generation == 0 || b.generation == 0) {
      std::cout << "  [err] 代次号不该是 0（a=" << a.generation
                << " b=" << b.generation << "）\n";
      ok = false;
    }
    // **就是这一条**：代次号永不复用。它成立，`connection_matches()` 才能
    // 分辨"还是那条连接"和"地址被新连接占用了"。
    if (a.generation == b.generation) {
      std::cout << "  [err] 两条连接拿到同一个代次号 " << a.generation
                << " —— 复用就等于这个机制失效\n";
      ok = false;
    }
    if (b.generation < a.generation) {
      std::cout << "  [err] 代次号不单调：" << a.generation << " -> "
                << b.generation << "\n";
      ok = false;
    }
  }

  // 6.2 半途断连之后服务必须还活着。
  //
  // 两种断连形状都要跑，它们撞的不是同一个窗口：
  //
  // (a) 请求刚发完就断 —— **这一段才是 `alive_token_` 守卫真正拦下的场景**。
  //     窗口靠"读盘够快（写来得及排进 libuv 写队列）、断得够早（写完成回调
  //     还没送到）"撞出来，单轮命中率不高，所以轮数必须给够。轮数是有实测
  //     依据的：跑 3 轮时把守卫拆掉，200 次里才崩 3 次（≈1.4%），噪声级别；
  //     给到 40 轮之后，同样拆掉守卫 200 次崩 9 次（4.5%），而**装回去 200
  //     次崩 0 次** —— 这个差距才让这条用例算得上回归网。
  // (b) 确认服务端已经开始回包再断 —— 见 `fire_and_disconnect_midwrite()`，
  //     它撞不到上面那个 UAF，测的是"写了一半对端消失"这个别的场景。
  //
  // 两种都留着：它们各自都可能退化（比如把 big.bin 调大，(a) 的窗口就没了，
  // 而 (b) 照样通过），只有一起跑才看得出覆盖是不是还在。
  for (int i = 0; i < 40; ++i) {
    fire_and_disconnect(port, "/big.bin", 5);
  }
  for (int i = 0; i < 8; ++i) {
    if (!fire_and_disconnect_midwrite(port, "/big.bin")) {
      std::cout << "  [err] 第 " << i
                << " 轮没等到服务端回包就断了 —— 「回包中途断连」这个窗口没"
                   "构造出来\n";
      ok = false;
      break;
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  if (!http_get(port, "/index.html", status, body)) ok = false;
  else if (status != 200 || body != "<html>INDEX-V1</html>") {
    std::cout << "  [err] 断连之后服务不正常 -> " << status << " '" << body
              << "'\n";
    ok = false;
  }

  // 让服务端把那几条断连事件也处理掉再停：连接关闭是**对端**事件，循环
  // 需要跑一轮才知道，光把 stop 置真只是让下一轮不再开始。
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  runner.shutdown();

  // 6.4 停完之后（登记表已空），全部代次号都该是 0。
  //     这里查的是**悬垂指针**，只当 map 的 key 用，不解引用。
  for (size_t i = 0; i < runner.observation_count(); ++i) {
    const ConnObservation o = runner.observation(i);
    if (o.generation_at_close != 0) {
      std::cout << "  [err] 连接关掉之后代次号还查得到（" << o.generation_at_close
                << "）—— 说明它没从登记表里摘干净\n";
      ok = false;
      break;
    }
  }

  cleanup_fs();
  return ok;
}

// =========================================================================
// Test 7: 旧连接的异步完成不许写到新连接上（ABA）
// =========================================================================
//
// 这是代次号机制真正要挡的那件事，也是唯一能把"守卫拆掉"和"守卫在岗"区分开
// 的用例：
//
//   1. A 连上来请求 /big.bin，然后立刻断开 —— 服务端**已经收到请求**，异步
//      stat/读盘已排队；
//   2. 服务端处理 A 的断开，`uvcpp_tcp_client` 被 delete，堆地址空出来；
//   3. B 连上来，**极可能分到 A 刚释放的那个地址**；
//   4. A 的完成回调此时才跑。它手里只有一个（已悬垂的）指针，地址又和 B
//      一模一样 —— 如果只比指针，它就会把 big.bin 的响应写进 B 的连接。
//
// 第 4 步是唯一可能出错的时刻，我们需要它**必然**发生在第 2、3 步之后，
// 所以先把线程池占满（见 `PoolBlocker`）。没有这一步，A 的读盘在断开被处理
// 之前就回来了，窗口只有几十微秒 —— 实测把守卫改成恒真也照样全绿。
static bool test_stale_connection_crosstalk() {
  if (!prepare_fs()) return false;

  StaticTestServer runner(kTmpDir, /*spa_fallback=*/false);
  int port = runner.start();

  bool ok = true;

  // 不连接，只用它的 loop 来跑占位任务。
  uvcpp_tcp_client pump_client;
  PoolBlocker blocker(pump_client.get_loop());

  // 连开 N 条"请求完就断"的连接，**中间不放行池子** —— 于是 N 个完成回调
  // 全部悬在半空，它们手里的指针全都指向已经被 delete 的连接对象。
  //
  // 为什么要攒这么多：只开一条的话，那条连接的地址要过好几轮分配才会被复用
  // （Windows LFH 有延迟释放，实测复用发生在两轮之后），等它被复用时那个完成
  // 回调早就跑完了 —— 窗口对不上。攒一批，则"某个 A 的地址"必然落在
  // "后来某个还活着的连接"上。
  const int kStale = 16;
  blocker.occupy(32);

  for (int i = 0; i < kStale; ++i) {
    // A_i：请求大文件，写出去就断。它的 stat/读盘排在占位任务后面，
    //       回来时它自己那条连接早没了。
    fire_and_disconnect(port, "/big.bin", 40);
    // 等服务器把这条断开处理掉（关连接是 loop 线程上的事，不用线程池），
    // A_i 的 client 对象随之 delete，地址回到空闲链上。
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
  }

  // **前提自检二**：那 16 条连接必须真的**已经断开**。这是最容易假成立的一环
  // —— 最初这里靠客户端析构断开，服务端一条关闭都没收到（`closes` 恒为 0），
  // 于是"回来时连接已经没了"根本不成立，用例看着绿其实什么都没测。
  {
    std::lock_guard<std::mutex> lk(runner.obs_mutex);
    if (runner.closes < kStale) {
      std::cout << "  [err] 放行前只关掉了 " << runner.closes << "/" << kStale
                << " 条连接 —— 对端断开没被服务端发现，这个用例没测到东西\n";
      blocker.release_all();
      blocker.drain();
      runner.shutdown();
      cleanup_fs();
      return false;
    }
  }

  const size_t accepted_before = runner.observation_count();
  if (accepted_before < static_cast<size_t>(kStale)) {
    std::cout << "  [err] 只建起 " << accepted_before << " 条连接（期望 "
              << kStale << "）—— 用例前提没成立\n";
    blocker.release_all();
    blocker.drain();
    runner.shutdown();
    cleanup_fs();
    return false;
  }

  // B：最后一个连上来的，它的地址极可能就是某个 A 用过的那个。
  PendingGet b;
  if (!b.start(port, "/index.html")) {
    std::cout << "  [err] B 连接失败\n";
    blocker.release_all();
    blocker.drain();
    runner.shutdown();
    cleanup_fs();
    return false;
  }
  b.pump(250);  // 让服务器接受 B、解析并派发（B 的 stat 同样在排队）

  // **前提自检**：池子必须真的占住了 —— 放行之前 B 一个字节都不该收到。
  // 占不住的话这些完成回调早就跑完了，"回来时连接已经没了"这个前提根本不
  // 成立，用例就什么都没测到。与其静默通过，不如直接报出来。
  if (!b.acc.empty() || b.ended.load()) {
    std::cout << "  [err] 放行前 B 已收到 " << b.acc.size()
              << " 字节 —— 线程池没占住（只起来 " << blocker.peak_threads()
              << " 个占位线程），这个用例没测到东西\n";
    blocker.release_all();
    blocker.drain();
    runner.shutdown();
    cleanup_fs();
    return false;
  }
  if (blocker.peak_threads() < 1) {
    std::cout << "  [err] 占位任务一个都没跑起来，这个用例没测到东西\n";
    blocker.release_all();
    blocker.drain();
    runner.shutdown();
    cleanup_fs();
    return false;
  }

  // 放行：这 N 个完成回调现在才跑，而 B 是活的。
  blocker.release_all();
  b.pump(3000);
  blocker.drain();

  if (!b.ended.load()) {
    std::cout << "  [err] B 没收到完整响应\n";
    ok = false;
  } else {
    // B 必须拿到**它自己请求的那个文件**，一字节不多。任何一个旧连接的完成
    // 写进来（那些地址里有一个就是 B 的），这里都会看到 big.bin 的内容或者
    // 两段响应拼接在一起。
    const std::string got = b.body();
    if (got != "<html>INDEX-V1</html>") {
      std::cout << "  [err] B 收到了不属于它的响应（body 前 64 字节：'"
                << got.substr(0, 64) << "'，共 " << got.size() << " 字节）\n";
      ok = false;
    }
  }

  runner.shutdown();
  cleanup_fs();
  return ok;
}

int main(int argc, char** argv) {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"construct_and_config", test_construct_and_config},
    {"serve_and_update", test_serve_and_update},
    {"not_found_and_traversal", test_not_found_and_traversal},
    {"mount_prefix", test_mount_prefix},
    {"head_no_body", test_head_no_body},
    {"generation_and_midflight_disconnect", test_generation_and_midflight_disconnect},
    {"stale_connection_crosstalk", test_stale_connection_crosstalk},
  };
  // 可选过滤：`test_web_static_server_func.exe <name>` 只跑一条。
  // 间歇性故障的定位全靠它 —— 整跑到一次崩溃要几十秒，单条用例是秒级，
  // 而"哪条用例崩"是这类竞态唯一能立刻缩小范围的线索。
  const char* only = (argc > 1) ? argv[1] : nullptr;
  for (const auto& t : tests) {
    if (only != nullptr && std::strcmp(only, t.name) != 0) continue;
    std::cout << "[web_static_server] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_static_server] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}

#else
int main() {
  std::cout << "[web_static_server] SKIP (web module disabled)\n";
  return 0;
}
#endif
