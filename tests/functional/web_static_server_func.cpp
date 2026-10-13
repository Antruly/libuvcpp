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
 *  - **连接代次号**：单调递增、永不复用；半途断连不写错连接；
 *  - **多循环并发**（`set_loops(4)` + 8 路 × 120 次）：同一个服务对象被多条
 *    循环线程同时用，判据是"逐条响应都对且进程活着"；
 *  - **预压缩孪生**（`set_precompressed`）：关时一个孪生都不碰；开时按
 *    `Accept-Encoding` 挑 `.br`/`.gz`，含 `q=0` 是拒绝、`*` 兜底、无孪生退回、
 *    `Range` 不取孪生，以及**缓存命中那一次也要带上 `content-encoding`**；
 *  - **`cache_tag` 与缓存边界**（`1.6.44-dev`）：tag 认的是盘上那份文件（`/` 与
 *    `/index.html` 同 tag、两个文件不同 tag、内容变了 tag 跟着变、发了孪生就
 *    不设 tag），以及条目上限/LRU 次序、字节预算、`set_cache_enabled(false)`
 *    连**写**一起关掉 —— 这些都不进响应头，只能在进程内判。
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

#include "wait_util.h"

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
#else
#include <unistd.h>   // rmdir
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

/// `app.js` 的预压缩孪生，给 `test_precompressed` 用。
///
/// **它们不是真的 brotli/deflate 流，也不必是**：这一节钉的是"服务端挑了盘上
/// 哪一份、挂了哪个 `content-encoding`"，不是"压出来的字节能不能解开"。真去
/// 解压就要把用例绑到一个第三方解码器上，而那条往返里的服务端一个字节都没压。
static const char* kAppJsBr = "APP-JS-BR-TWIN";
static const char* kAppJsGz = "APP-JS-GZ-TWIN-BYTES";

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

  // 孪生文件只在 `set_precompressed(true)` 下才会被取到；默认关的那些用例
  // 因此不受它们影响（`index.html` 故意**不**给孪生，用来钉"没有孪生就退回
  // 原文件"）。
  if (!write_text("app.js.br", kAppJsBr)) return false;
  if (!write_text("app.js.gz", kAppJsGz)) return false;

  return write_text("index.html", "<html>INDEX-V1</html>") &&
         write_text("app.js", kAppJs);
}

static void cleanup_fs() {
  std::remove(tmp_path("index.html").c_str());
  std::remove(tmp_path("app.js").c_str());
  std::remove(tmp_path("app.js.br").c_str());
  std::remove(tmp_path("app.js.gz").c_str());
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
  /// 工作循环数（`set_loops()` 的实参）。1 = 今天的行为。
  int loops;
  /// 是否发盘上的预压缩孪生文件（`set_precompressed()`）。默认关。
  bool precompressed;
  /// 校验跑在哪条线程上（`set_revalidate()`）。默认 `POOL`。
  uvcpp_static_revalidate revalidate;
  /// 缓存开关与两个预算（`set_cache_enabled()` / `set_cache_max_entries()` /
  /// `set_cache_max_bytes()`）。默认值与类自己的默认值一致。
  bool cache_enabled = true;
  size_t cache_max_entries = 256;
  size_t cache_max_bytes = 32u * 1024u * 1024u;

  /// 每条请求在**处理函数返回之后**的 `resp.deferred` 与状态码 ——
  /// `LOOP_STAT` 档"命中就地回完"这件事只有在这里看得见（客户端那边
  /// "推迟"与"就地"是同一个响应）。
  struct ServeObservation {
    int status;
    bool deferred;
    /// `resp.body` 是不是一份**共享视图**（`uvcpp_buf::is_shared()`），是的话
    /// 记下它指向哪一个 `std::string`。
    ///
    /// 这一格是"命中路径零拷贝"的确定性判据：缓存里那份字节**借**给响应，
    /// 而不是拷一份 —— 同一份文件的两次命中必须指向**同一个对象**（指针相等），
    /// 拷贝实现做不到这一点。它在网络那头看不出来（两种实现发出去的字节
    /// 逐字节相同），所以只能在这里判。
    bool shared;
    const std::string* shared_body;
    /// 处理函数返回时 `resp.cache_tag` 的值（`1.6.44-dev` 起本类会填它）。
    ///
    /// 它在网络那头**完全看不出来**（不进任何响应头），却是"实时压缩会不会
    /// 每请求重来一遍"的唯一开关：`uvcpp_http_server` 只拿它当变体缓存的键。
    /// 所以判据只能在这里 —— 与上面那两格同一个道理。
    std::string cache_tag;
    /// 处理函数返回时的 `cache_size()` / `cache_bytes()`。同样是"网络那头看
    /// 不出来"的状态（上限是进程内的账），只能在这里判。
    size_t cache_size;
    size_t cache_bytes;
  };
  std::vector<ServeObservation> serves;

  std::promise<int> port_promise;
  std::atomic<bool> stop{false};
  std::thread thread;

  /// 只被 loop 线程写、测试线程在 shutdown() 之后读，所以用 mutex 护住。
  std::mutex obs_mutex;
  std::vector<ConnObservation> observations;
  int closes = 0;

  /// `connection_generation()` 拿一个野指针问出来的值。只能被 1 或 0 ——
  /// 见 `start()` 里那一句：这是"本 API 不许解引用传进来的指针"的确定性判据。
  uint64_t wild_generation = 0;

  StaticTestServer(const std::string& r, bool spa,
                   const std::string& mount = std::string("/"),
                   int nloops = 1, bool pc = false,
                   uvcpp_static_revalidate rv = uvcpp_static_revalidate::POOL)
      : root(r), url_root(mount), spa_fallback(spa), loops(nloops),
        precompressed(pc), revalidate(rv) {}

  int start() {
    thread = std::thread([this]() {
      uvcpp_static_server static_svr(root, url_root);
      static_svr.set_spa_fallback(spa_fallback);
      static_svr.set_precompressed(precompressed);
      static_svr.set_revalidate(revalidate);
      static_svr.set_cache_enabled(cache_enabled);
      static_svr.set_cache_max_entries(cache_max_entries);
      static_svr.set_cache_max_bytes(cache_max_bytes);
      uvcpp_http_server server;
      // 必须在 `listen()` 之前（见 `uvcpp_http_server::set_loops` 的 @warning）。
      server.set_loops(loops);

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

      // 不直接挂 `static_svr.handler(&server)`，而是包一层：处理函数**返回之后**
      // 把 `resp.deferred` 记下来。这是 `LOOP_STAT` 唯一与 `POOL` 不同的那件事，
      // 而它在网络那头看不出来（两种档客户端拿到的响应逐字节相同）。
      server.on_request([this, &static_svr, &server](uvcpp_http_request& req,
                                                     uvcpp_http_response& resp,
                                                     uvcpp_tcp_client* client) {
        static_svr.serve(req, resp, client, &server);
        ServeObservation o;
        o.status = static_cast<int>(resp.status_code);
        o.deferred = resp.deferred;
        o.shared = resp.body.is_shared();
        o.shared_body = resp.body.shared_ref().get();
        o.cache_tag = resp.cache_tag;
        o.cache_size = static_svr.cache_size();
        o.cache_bytes = static_svr.cache_bytes();
        std::lock_guard<std::mutex> lk(obs_mutex);
        serves.push_back(o);
      });

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
      // `connection_generation()` 文档里那条用法（"开始时记下、异步回来再问
      // 一次"），而 `uvcpp_static_server` 从前那个 `connection_matches()` 也是
      // 这么做的。
      std::lock_guard<std::mutex> lk(obs_mutex);
      for (size_t i = 0; i < observations.size(); ++i) {
        observations[i].generation_at_close =
            server.connection_generation(observations[i].client);
      }

      // **上面那句是"解不解引用"这件事的确定性判据。**
      //
      // 悬垂指针读出来的东西通常还是老值、还被 `ctxs_at()` 的越界兜底夹住，
      // 所以"跑完没崩"证明不了实现没读它。`set_loops(4)` 下这一点是能钉死的：
      // 传一个**必然未映射**的地址进去，只要实现里读了 `client->anything`
      // 就是当场 SIGSEGV（1.6.19 修之前，多循环的 `ctxs_of()` 会读
      // `client->loop_index()`，这一句就是段错误；Linux + ASan 报的是
      // `heap-buffer-overflow ... READ of size 8`）。返回值也必须是 0：
      // 没有哪张表会登记 `(uvcpp_tcp_client*)1`。
      wild_generation =
          server.connection_generation(reinterpret_cast<uvcpp_tcp_client*>(1));
      if (wild_generation != 0) {
        std::fprintf(stderr,
                     "[web_static_server] connection_generation(野指针) = %llu，"
                     "期望 0 —— 说明它把那个地址当成一条真连接了\n",
                     static_cast<unsigned long long>(wild_generation));
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

  size_t serve_count() {
    std::lock_guard<std::mutex> lk(obs_mutex);
    return serves.size();
  }

  ServeObservation serve(size_t i) {
    std::lock_guard<std::mutex> lk(obs_mutex);
    return serves[i];
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
 * @brief 带 `accept-encoding` / `range` 的 GET：额外问出响应的
 *        `content-encoding`。
 *
 * 空字符串表示**不发**那个头。`accept-encoding` 是原样发出去的 —— 包括
 * `gzip;q=0`、`*` 这些专门给那条挑选规则看的形状；它正是 `test_precompressed`
 * 要分辨的那一层，客户端这边一个字节都不该替它规范化。
 *
 * **它不能用来测"服务端不回话"**：POSIX 上这条路的阻塞式 socket I/O 里
 * `timeout_ms` 是被忽略的（`uvcpp_http_client.cpp` 的 `send_wait_ssl` 上写着这句），
 * 对端不回就永久阻塞。`test_loop_stat` 要抓的恰恰是"漏回话"，所以那边走
 * @ref raw_get（每次交换的等待是有界的）。
 */
static bool http_get_enc(int port, const std::string& path,
                         const std::string& accept_encoding,
                         const std::string& range,
                         int& status, std::string& encoding,
                         std::string& body) {
  uvcpp_http_response resp;
  int rc = -1;
  for (int i = 0; i < 40; ++i) {
    uvcpp_http_client client;
    rc = client.connect_wait("127.0.0.1", port, 2000);
    if (rc != 0) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); continue; }
    uvcpp_http_request req = uvcpp_http_request::make_get(path);
    if (!accept_encoding.empty()) req.set_header("accept-encoding", accept_encoding);
    if (!range.empty()) req.set_header("range", range);
    rc = client.send_wait(req, resp, 3000);
    if (rc == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (rc != 0) return false;
  status = static_cast<int>(resp.status_code);
  encoding = resp.get_header("content-encoding", "");
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
  uvcpp_test::wait_until(loop, [&] { return connected.load(); }, uvcpp_test::kWaitMs);
  uvcpp_test::wait_until(loop, [&] { return ended.load(); }, uvcpp_test::kWaitMs);
  out = acc;
  return ended.load();
}

/** @brief 从响应头块里取状态码；读不懂返回 -1。 */
static int raw_status(const std::string& head) {
  const size_t sp = head.find(' ');
  if (sp == std::string::npos || head.size() < sp + 4) return -1;
  for (size_t i = 1; i <= 3; ++i) {
    if (head[sp + i] < '0' || head[sp + i] > '9') return -1;
  }
  return (head[sp + 1] - '0') * 100 + (head[sp + 2] - '0') * 10 +
         (head[sp + 3] - '0');
}

/** @brief 从响应头块里取某个头的值（名字大小写不敏感）；没有这个头返回空串。 */
static std::string raw_header(const std::string& head, const std::string& name) {
  std::string lower = head;
  for (size_t i = 0; i < lower.size(); ++i) {
    if (lower[i] >= 'A' && lower[i] <= 'Z') {
      lower[i] = static_cast<char>(lower[i] - 'A' + 'a');
    }
  }
  const std::string key = "\r\n" + name + ":";
  const size_t pos = lower.find(key);
  if (pos == std::string::npos) return std::string();
  size_t v = pos + key.size();
  while (v < head.size() && (head[v] == ' ' || head[v] == '\t')) ++v;
  size_t e = head.find("\r\n", v);
  if (e == std::string::npos) e = head.size();
  return head.substr(v, e - v);
}

/**
 * @brief 一次有界等待的 GET：自己拼报文 + `Connection: close`，解析状态/编码/body。
 *
 * 与 @ref http_get_enc 的差别只有一处，但正是 `test_loop_stat` 需要的那一处：
 * 每次交换的等待**有界**（`raw_exchange()` 里 `kWaitMs`）。服务端不回话时
 * `http_get_enc` 会永久阻塞（POSIX 上那条路的 `timeout_ms` 被忽略），一次
 * 红会变成一次挂死；这里则是 2 秒后返回 false，用例照样打印 FAIL 并继续。
 */
static bool raw_get(int port, const std::string& path,
                    const std::string& accept_encoding, int& status,
                    std::string& encoding, std::string& body) {
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
  if (!accept_encoding.empty()) {
    req += "Accept-Encoding: " + accept_encoding + "\r\n";
  }
  req += "Connection: close\r\n\r\n";

  std::string raw;
  if (!raw_exchange(port, req, raw)) return false;
  const size_t sep = raw.find("\r\n\r\n");
  if (sep == std::string::npos) return false;

  const std::string head = raw.substr(0, sep);
  body = raw.substr(sep + 4);
  encoding = raw_header(head, "content-encoding");
  status = raw_status(head);
  return status > 0;
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
  uvcpp_test::wait_until(loop, [&] { return connected.load(); }, uvcpp_test::kWaitMs);
  uvcpp_test::wait_until(loop, [&] { return written.load(); }, uvcpp_test::kWaitMs);
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
  uvcpp_test::wait_until(loop, [&] { return closed.load(); }, uvcpp_test::kWaitMs);
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
  uvcpp_test::wait_until(loop, [&] { return connected.load(); }, uvcpp_test::kWaitMs);
  uvcpp_test::wait_until(loop, [&] { return got_first.load(); }, uvcpp_test::kWaitMs);
  if (!got_first.load()) return false;

  client.close([&closed]() { closed.store(true); });
  uvcpp_test::wait_until(loop, [&] { return closed.load(); }, uvcpp_test::kWaitMs);
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
    uvcpp_test::wait_until(
        loop, [this] { return done.load() >= static_cast<int>(items.size()); },
        uvcpp_test::kWaitMs);
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

  /// 上限是**墙钟**毫秒，不是圈数（见文件顶部 wait_util.h 的说明）。
  void pump(int ms) { uvcpp_test::pump_for(client.get_loop(), ms); }

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

  // 校验档：**默认是池往返那一档**，这是"改动前的行为逐字不变"这条承诺的可执行
  // 版本 —— 老调用方一行不改，拿到的就是 1.6.24 的行为。
  if (s1.revalidate() != uvcpp_static_revalidate::POOL) return false;
  s1.set_revalidate(uvcpp_static_revalidate::LOOP_STAT);
  if (s1.revalidate() != uvcpp_static_revalidate::LOOP_STAT) return false;
  s1.set_revalidate(uvcpp_static_revalidate::POOL);
  if (s1.revalidate() != uvcpp_static_revalidate::POOL) return false;
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
  std::cout << "  [phase] 起单循环服务端\n";
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

  std::cout << "  [phase] 攒 " << kStale << " 条半途断开的连接（线程池占住不放）\n";
  for (int i = 0; i < kStale; ++i) {
    // A_i：请求大文件，写出去就断。它的 stat/读盘排在占位任务后面，
    //       回来时它自己那条连接早没了。
    fire_and_disconnect(port, "/big.bin", 40);
    // 等服务器把这条断开处理掉（关连接是 loop 线程上的事，不用线程池），
    // A_i 的 client 对象随之 delete，地址回到空闲链上。
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
  }
  std::cout << "  [phase] A 全断开了，连 B 并等池子放行\n";

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
  std::cout << "  [phase] 放行，等 B 收完\n";
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

  std::cout << "  [phase] shutdown\n";
  runner.shutdown();
  cleanup_fs();
  return ok;
}

// =========================================================================
// Test 8: 多循环下的并发（`set_loops(n > 1)`）
// =========================================================================
/**
 * @brief 同一个 `uvcpp_static_server` 被 n 条循环线程同时用。
 *
 * 这一条钉的是**多循环下的数据正确性**：判据是"并发跑完之后每一个响应都还是它
 * 自己那个文件的内容，而且进程活着"。
 *
 * **它不是「修前必红」的那种用例 —— 别把它当成锁还在的证据。** 本类修的那三处
 * 竞态（`cache_` 无锁的 `std::map`、`retired_` 无锁、以及按循环分桶）在入口层
 * 是**立刻段错误**（见 `uvcpp_static_server.h` 顶上那张 A/B 表：h2load-h3 打
 * `/static/*`，修前 `-c 4` 起就是 `0 done`）。但同样的代码在下面这个形状下
 * **摘掉锁也照样全绿** —— 实测把 5 处加锁全摘掉重建，4 条循环 × 8 路 × 120 与
 * 4 条循环 × 16 路 × 300 各跑 3 次都是 PASS。原因是这里的并发写窗口窄（只有
 * 6 个缓存键、`retired_` 每轮 `serve()` 就排空一次），而数据竞争是 UB，不保证
 * 以段错误的形式现形。所以这条用例能抓的是**错数据与进程死亡**，不是"锁被删了"。
 *
 * **为什么必须多循环才现形**：单循环下 `serve()` 与 after_work 都在同一条
 * 线程上，两者天然互斥。这也是它一直没被本文件的前七条用例抓到的原因 ——
 * 它们全是单循环。
 *
 * **判据不去数"多少条循环各处理了多少请求"**：那是统计判据（与
 * `tcp_multiloop_func.cpp` 第 1 条同一个立场，文档 `doc/multiloop-design.md`
 * §6）。这里要的是"并发下数据不错、不崩"，所以判据是逐条比 body/状态码。
 *
 * 负载里**必须有走线程池的那条路**（`big.bin` 的首次读盘 ⇒ `load_async` ⇒
 * after_work 里 `retire()`），否则 `retired_` 根本不会被并发碰。所以这一节
 * 特意关掉 SPA、并且每个线程都混着请求 404 与缓存命中。
 */
static bool test_multiloop_concurrency() {
  if (!prepare_fs()) return false;

  // 4 条工作循环 + acceptor = 5 条线程。取值小是有意的：这条用例要能在 CI 的
  // 双核机器上跑。**不要因为"跑快点"往上加** —— 加循环数换不来更硬的判据
  // （见本函数上面那段：摘掉锁跑到 16 路 × 300 也照样绿），只会让 CI 更慢。
  StaticTestServer runner(kTmpDir, /*spa_fallback=*/false,
                          /*mount=*/"/", /*nloops=*/4);
  std::cout << "  [phase] 起 4 条工作循环 + acceptor\n";
  int port = runner.start();

  const int nthreads = 8;
  const int per_thread = 120;

  struct Case { const char* path; int status; size_t len; };
  static const Case kCases[] = {
    {"/index.html",     200, sizeof("<html>INDEX-V1</html>") - 1},
    {"/app.js",         200, 0},   // 长度下面单独算
    {"/lib..min.js",    200, sizeof("MINIFIED") - 1},
    {"/sub/index.html", 200, sizeof("<html>SUB-INDEX</html>") - 1},
    {"/big.bin",        200, 256 * 1024},  // 走线程池读盘那条路
    // 404 的 body 是框架给的错误页，长度不是这条用例要钉的东西 —— 只钉状态码。
    {"/missing.css",    404, static_cast<size_t>(-1)},
  };
  const size_t ncases = sizeof(kCases) / sizeof(kCases[0]);

  std::atomic<int> done(0);
  std::atomic<int> bad(0);
  std::mutex sample_mutex;
  std::string sample;

  std::vector<std::thread> workers;
  std::cout << "  [phase] 8 路 × 120 并发打（含 /big.bin 那条走线程池的路）\n";
  for (int t = 0; t < nthreads; ++t) {
    workers.push_back(std::thread([&, t]() {
      for (int i = 0; i < per_thread; ++i) {
        const Case& c = kCases[(t + i) % static_cast<int>(ncases)];
        int status = 0;
        std::string body;
        if (!http_get(port, c.path, status, body)) {
          ++bad;
          std::lock_guard<std::mutex> lk(sample_mutex);
          if (sample.empty()) sample = std::string(c.path) + " 请求没完成";
          continue;
        }
        size_t want = c.len;
        if (std::strcmp(c.path, "/app.js") == 0) want = std::strlen(kAppJs);
        const bool len_ok =
            want == static_cast<size_t>(-1) ? true : body.size() == want;
        if (status != c.status || !len_ok) {
          ++bad;
          std::lock_guard<std::mutex> lk(sample_mutex);
          if (sample.empty()) {
            sample = std::string(c.path) + " -> " +
                     std::to_string(status) + "，" +
                     std::to_string(body.size()) + " 字节（期望 " +
                     std::to_string(c.status) + "，" +
                     std::to_string(want) + " 字节）";
          }
        }
        ++done;
      }
    }));
  }
  for (size_t i = 0; i < workers.size(); ++i) workers[i].join();
  std::cout << "  [phase] 8 路全部跑完（" << done.load() << " 个请求）\n";

  bool ok = true;
  if (bad.load() != 0) {
    std::cout << "  [err] " << bad.load() << "/" << (nthreads * per_thread)
              << " 个响应不对（第一例：" << sample << "）\n";
    ok = false;
  }
  if (done.load() != nthreads * per_thread) {
    std::cout << "  [err] 只有 " << done.load() << " 个请求跑完\n";
    ok = false;
  }

  std::cout << "  [phase] shutdown\n";
  runner.shutdown();
  // 关机之后服务端线程已经 join，这时读它留下的那个数是安全的。
  if (runner.wild_generation != 0) {
    std::cout << "  [err] connection_generation(野指针) = "
              << runner.wild_generation << "，期望 0\n";
    ok = false;
  }
  cleanup_fs();
  return ok;
}

// =========================================================================
// Test 9: 预压缩孪生文件（`set_precompressed`）
// =========================================================================
//
// 判据是**盘上哪一份被发出去**，看的是 `content-encoding` 与 body 的逐字节比对。
// 开关本身在 `uvcpp_static_server` 上，挑选规则在 `http_pick_precompressed_variants()`
// （`web/uvcpp_http_common.h`），与技术层的 `uvcpp_web_static` 共用同一份实现 ——
// 所以这里钉的是那一份规则在这条腿上确实接上了，而不是另有一套。
//
// 关的那一档必须**一个孪生都不碰**：`.br`/`.gz` 就摆在盘上，它照样发原文件。
// 这是"默认关"这个承诺的可执行版本。
static bool test_precompressed() {
  if (!prepare_fs()) return false;

  bool ok = true;
  int status = 0;
  std::string enc, body;

  // --- 关 ---
  {
    StaticTestServer runner(kTmpDir, /*spa_fallback=*/false);
    const int port = runner.start();
    if (!http_get_enc(port, "/app.js", "br;q=1, gzip;q=0.8", "", status, enc, body)) {
      ok = false;
    } else if (status != 200 || !enc.empty() || body != kAppJs) {
      std::cout << "  [err] 关时 GET /app.js -> " << status << " enc='" << enc
                << "' '" << body << "'\n";
      ok = false;
    }
    runner.shutdown();
  }

  // --- 开 ---
  {
    StaticTestServer runner(kTmpDir, /*spa_fallback=*/false,
                            /*mount=*/"/", /*nloops=*/1, /*precompressed=*/true);
    const int port = runner.start();

    struct Case {
      const char* path;
      const char* accept_encoding;
      const char* want_enc;   // 期望的 content-encoding（"" = 一个都没有）
      const char* want_body;
    };
    const Case cases[] = {
      // br 与 gzip 都在、q 不同 ⇒ br 胜（本仓语料上它更小）。
      {"/app.js", "br;q=1, gzip;q=0.8", "br",   kAppJsBr},
      // 只认 gzip ⇒ 退到 .gz。
      {"/app.js", "gzip",               "gzip", kAppJsGz},
      // **同一条再来一次**：这次走缓存命中那条路。缓存键是拼了后缀的
      // `/app.js.br`，但发响应时手上只有条目本身 —— 缓存条目不带回
      // `variant` 的话，这一档就会变成"body 是孪生、头却说没编码"。
      {"/app.js", "br;q=1, gzip;q=0.8", "br",   kAppJsBr},
      // `q=0` 是**拒绝**，不是"最低优先"。
      {"/app.js", "br;q=0, gzip",       "gzip", kAppJsGz},
      // `*` 兜底：两份都收，同 q 时 br 在前。
      {"/app.js", "*",                  "br",   kAppJsBr},
      // 两份都不认 ⇒ 原文件。
      {"/app.js", "identity",           "",     kAppJs},
      // **没有孪生**的文件：退回原文件，与关着时逐字节相同。
      {"/index.html", "br;q=1, gzip;q=0.8", "", "<html>INDEX-V1</html>"},
    };
    const size_t ncases = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < ncases; ++i) {
      const Case& c = cases[i];
      if (!http_get_enc(port, c.path, c.accept_encoding, "", status, enc, body)) {
        std::cout << "  [err] #" << i << " " << c.path << " 请求没完成\n";
        ok = false;
        continue;
      }
      if (status != 200 || enc != c.want_enc || body != c.want_body) {
        std::cout << "  [err] #" << i << " " << c.path << " ae='"
                  << c.accept_encoding << "' -> " << status << " enc='" << enc
                  << "' body='" << body << "'（期望 200 enc='" << c.want_enc
                  << "' body='" << c.want_body << "'）\n";
        ok = false;
      }
    }

    // 带 `Range` 的请求**一律不取孪生**：范围说的是**原文件**的字节坐标，换一份
    // 表示之后那个坐标就不成立了。断言只钉"没拿到孪生"这一件事，不去钉本类
    // 面对 `Range` 时到底回 200 还是 206 —— 本类今天不做 Range，那是另一条
    // 能力边界，不是这条用例要管的东西。
    if (!http_get_enc(port, "/app.js", "br;q=1, gzip;q=0.8", "bytes=0-3",
                      status, enc, body)) {
      std::cout << "  [err] Range 请求没完成\n";
      ok = false;
    } else if (!enc.empty() ||
               body.find("TWIN") != std::string::npos) {
      std::cout << "  [err] Range 请求拿到了孪生：enc='" << enc << "' body='"
                << body << "'\n";
      ok = false;
    }

    runner.shutdown();
  }

  cleanup_fs();
  return ok;
}

// =========================================================================
// Test 10: `LOOP_STAT` 档（缓存校验就地做在循环线程上）
// =========================================================================
//
// `uvcpp_static_revalidate::LOOP_STAT` 把**唯一的池往返**（那次校验用的 stat）
// 搬回循环线程，于是一次命中完全不碰线程池。两条档在**网络上**逐字节同形 ——
// 客户端看不出区别 —— 所以这里把判据分两处钉：
//
//   1. **网络那头**：九种请求形状（孪生 / 目录 index / SPA 回退 / 实时更新 /
//      404）在 `LOOP_STAT` 下的响应必须与 `POOL` 档逐字相同。这一半用的是
//      与 `test_serve_and_update`、`test_precompressed` 同一批断言值；
//   2. **处理函数返回之后**的 `resp.deferred`：命中与 404 是 `false`（就地回完），
//      只有真的落到读盘那条路（缓存未命中）才是 `true`。这一格是两档**唯一**
//      不同的地方，也正是"搬回循环线程"这句话的确定性判据。
//
// 第 2 条还顺带钉住了同步档最容易错的那件事：`deliver()` 写回调用方的 `resp`
// 之后**不能再自己发一次**（那就成两条响应），而落到 `load_async()` 那一支时
// **必须**把 `deferred` 立起来（否则谁也不会回这条请求，客户端挂到超时）。
//
// 这条用例**一律走 `raw_get()`**（有界等待），不走 `http_get_enc()`：同步档漏
// 回话的症状就是"服务端一个字节都不发"，而阻塞式客户端在 POSIX 上会永久等下去
// —— 那样红了也是挂死，不是一条 FAIL。

static bool test_loop_stat() {
  if (!prepare_fs()) return false;

  bool ok = true;
  int status = 0;
  std::string enc, body;

  {
    StaticTestServer runner(kTmpDir, /*spa_fallback=*/true, /*mount=*/"/",
                            /*nloops=*/1, /*precompressed=*/true,
                            uvcpp_static_revalidate::LOOP_STAT);
    const int port = runner.start();

    struct Case {
      const char* path;
      const char* accept_encoding;
      int want_status;
      const char* want_enc;
      const char* want_body;
      bool want_deferred;   // 处理函数返回之后那一格
      /// `resp.body` 是不是共享视图（`1` 必须、`0` 必须不是、`-1` 不判）。
      int want_shared;
      /// 与本表里第几条**必须指向同一个 `std::string`**（-1 = 不比对）。
      int same_body_as;
      const char* why;
    };
    const Case cases[] = {
      // 1. 首次请求必然未命中（缓存是空的）⇒ 仍然要读盘 ⇒ 推迟。
      {"/app.js", "br;q=1, gzip;q=0.8", 200, "br", kAppJsBr, true, 0, -1,
       "未命中：读盘还在池里"},
      // 2. 同一条 URL 再来一次 ⇒ 命中 ⇒ 就地回完（本条与 POOL 档唯一的差别）。
      {"/app.js", "br;q=1, gzip;q=0.8", 200, "br", kAppJsBr, false, 1, -1,
       "命中：循环线程上回完"},
      // 3/4. 索引文件：未命中 → 推迟；再来一次 → 就地。
      {"/", "", 200, "", "<html>INDEX-V1</html>", true, 0, -1, "索引未命中"},
      {"/", "", 200, "", "<html>INDEX-V1</html>", false, 1, -1, "索引命中"},
      // 5/6. 目录请求要**递归**一次（`/sub` → `/sub/index.html`）：同步档下这一
      //      跳走的是 `stat_dispatch()` 里那份栈上拷贝，`sync_sent` 还得从拷贝
      //      传回原 task。第 6 条是命中，因此必须就地 —— 传不回来的话它会变成
      //      "既没回话也没推迟"，客户端超时。
      {"/sub", "", 200, "", "<html>SUB-INDEX</html>", true, 0, -1,
       "目录步进未命中"},
      {"/sub/", "", 200, "", "<html>SUB-INDEX</html>", false, 1, -1,
       "目录步进命中"},
      // 7. SPA 回退（step 2）同样是一跳，落到已经缓存好的 `index.html` ⇒ 就地。
      {"/some/client/route", "", 200, "", "<html>INDEX-V1</html>", false, 1, -1,
       "SPA 回退命中"},
      // 8. 再命中一次 `/app.js`：**必须与第 2 条指向同一份字节**。
      //
      //    判据是"缓存里那份字节是借出去的、不是拷出去的"：拷贝实现下两次命中
      //    各有各的 `std::string`，指针不可能相等。发出去的字节两种实现逐字节
      //    相同，所以这条**只能**在这里判 —— 网络那头看不出来。
      {"/app.js", "br;q=1, gzip;q=0.8", 200, "br", kAppJsBr, false, 1, 1,
       "命中：借的是缓存里同一个对象"},
    };

    size_t nreq = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      const Case& c = cases[i];
      if (!raw_get(port, c.path, c.accept_encoding, status, enc, body)) {
        std::cout << "  [err] #" << i << " " << c.path << " 请求没完成\n";
        ok = false;
        ++nreq;  // 请求发出去了但没读回来，后面的下标就别指望对得上
        continue;
      }
      if (status != c.want_status || enc != c.want_enc || body != c.want_body) {
        std::cout << "  [err] #" << i << " " << c.path << " -> " << status
                  << " enc='" << enc << "' body='" << body << "'（期望 "
                  << c.want_status << " enc='" << c.want_enc << "' body='"
                  << c.want_body << "'）\n";
        ok = false;
      }
      const size_t n = runner.serve_count();
      if (n != nreq + 1) {
        std::cout << "  [err] #" << i << " " << c.path << " 触发 " << (n - nreq)
                  << " 次处理函数（期望 1）—— 后面的 deferred 断言对不上号\n";
        ok = false;
      } else {
        const StaticTestServer::ServeObservation o = runner.serve(nreq);
        if (o.deferred != c.want_deferred) {
          std::cout << "  [err] #" << i << " " << c.path
                    << " 处理函数返回后 deferred=" << (o.deferred ? 1 : 0)
                    << "（期望 " << (c.want_deferred ? 1 : 0) << "，" << c.why
                    << "）\n";
          ok = false;
        } else if (c.want_deferred == false && o.status != c.want_status) {
          // 就地回完的那几档，状态码**在 `serve()` 返回前**就得是最终值 ——
          // 否则 `deferred` 那格是假的（置了 200 又推迟去改成 404）。
          std::cout << "  [err] #" << i << " " << c.path
                    << " 就地回完但状态是 " << o.status << "\n";
          ok = false;
        }
        // body 是借还是拷：命中那几档必须是缓存里那份字节的**共享视图**，
        // 推迟那几档还没填 body，因此不该有视图。
        if (c.want_shared >= 0 && o.shared != (c.want_shared == 1)) {
          std::cout << "  [err] #" << i << " " << c.path
                    << " body.is_shared()=" << (o.shared ? 1 : 0) << "（期望 "
                    << c.want_shared << "，" << c.why << "）\n";
          ok = false;
        }
        // 同一个文件的两次命中必须指向同一个对象（见第 8 条那一段）。
        if (c.same_body_as >= 0) {
          const size_t k = static_cast<size_t>(c.same_body_as);
          const StaticTestServer::ServeObservation prev = runner.serve(k);
          if (!o.shared || prev.shared_body == nullptr ||
              o.shared_body != prev.shared_body) {
            std::cout << "  [err] #" << i << " " << c.path
                      << " 的 body 与 #" << c.same_body_as
                      << " 不是同一份对象 —— 命中路径上又拷了一份\n";
            ok = false;
          }
        }
      }
      ++nreq;
    }

    // 8. 实时更新在这条档下必须一样灵：校验仍然真的做了（就地 stat），文件变
    //    了就要发新字节。这一条是"LOOP_STAT 不是 TTL 缓存"的判据。
    if (!write_text("app.js", "console.log('JS-V2-LOOPSTAT');")) {
      ok = false;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      if (!raw_get(port, "/app.js", "br;q=1, gzip;q=0.8", status, enc, body)) {
        std::cout << "  [err] 更新后的 /app.js 请求没完成\n";
        ok = false;
      } else if (status != 200 || enc != "br" || body != kAppJsBr) {
        // 改的是**原文件**，孪生没动 ⇒ 仍然发孪生那份（与 POOL 档逐字同策：
        // 缓存键带后缀，校验的是被发出去的那一份的 mtime/大小）。
        std::cout << "  [err] 更新后 /app.js -> " << status << " enc='" << enc
                  << "' body='" << body << "'\n";
        ok = false;
      }
      // 同一个原文件、不带 `accept-encoding` ⇒ 另一条缓存键（原文件那一份），
      // 所以这里是**未命中** → 推迟 → 拿到新字节。
      const size_t n0 = runner.serve_count();
      if (!raw_get(port, "/app.js", "", status, enc, body)) {
        std::cout << "  [err] 更新后的 /app.js（无 ae）请求没完成\n";
        ok = false;
      } else if (status != 200 || body != "console.log('JS-V2-LOOPSTAT');") {
        std::cout << "  [err] 更新后 /app.js（无 ae）-> " << status << " '"
                  << body << "'（期望新字节）\n";
        ok = false;
      } else if (runner.serve_count() != n0 + 1 ||
                 runner.serve(n0).deferred != true) {
        std::cout << "  [err] 原文件那一份换了内容，本该未命中去读盘\n";
        ok = false;
      }
    }

    runner.shutdown();
  }

  // --- 404：SPA 关掉、孪生关掉，只剩"文件不存在"这一条路 ---
  //
  // 终点的 404 也在同步档下就地回，且**不是**推迟 —— 推迟出去就没人回了。
  {
    StaticTestServer runner(kTmpDir, /*spa_fallback=*/false, /*mount=*/"/",
                            /*nloops=*/1, /*precompressed=*/false,
                            uvcpp_static_revalidate::LOOP_STAT);
    const int port = runner.start();
    if (!raw_get(port, "/missing.css", "", status, enc, body)) {
      std::cout << "  [err] /missing.css 请求没完成\n";
      ok = false;
    } else if (status != 404) {
      std::cout << "  [err] GET /missing.css -> " << status << "（期望 404）\n";
      ok = false;
    } else if (runner.serve_count() != 1 || runner.serve(0).deferred) {
      std::cout << "  [err] 同步档的 404 应当是就地回的（deferred=1 就没人回它了）\n";
      ok = false;
    }
    runner.shutdown();
  }

  cleanup_fs();
  return ok;
}

// =========================================================================
// `cache_tag` 与缓存的三个边界（`1.6.44-dev`）
// =========================================================================
//
// 这一节钉的全是**网络那头看不出来**的状态：
//
//   - `resp.cache_tag` 不进任何响应头，它是 `uvcpp_http_server` 那个**压缩变体
//     缓存**的唯一开关键（空 = 不参与）。本类从前不填它 —— 于是走本类的每一条
//     腿都**每个请求在循环线程上重压一遍整份文件**，而线上字节数与"压一次发
//     多次"**逐字节相同**，端到端比对发现不了。
//   - `cache_size()` / `cache_bytes()` 是进程内的账，同样看不出来。它们钉的
//     是"这张表有上限"：`1.6.44-dev` 之前它是**无上限也无淘汰**的 `std::map`。
//   - `set_cache_enabled(false)` 从前只关掉"读"、关不掉"写"（写入那一支根本没
//     看这个开关）—— 于是"关掉缓存"的表照样只增不减。
//
// **必须用 `LOOP_STAT` 才判得动**：`POOL` 档下命中是在 stat 任务的 after_work
// 里才判出来的，那时 `serve()` 早已返回，观测到的是"推迟 + 空 tag"。这不是
// 两档行为不同，只是观测点不同（两档发出去的响应逐字节相同）——
// `test_loop_stat` 已经钉过这件事。
//
// 未命中那一档在观测点上也**必须**是空 tag：填它的是读盘回来的那个回调，而
// `serve()` 在那之前就返回了。这一格不是凑数 —— 它是"tag 是**盘上那份字节**的
// 身份、不是 URL 的身份"这件事在时间上的证据。
static bool test_cache_tag_and_bounds() {
  std::cout << "--- cache_tag 与缓存边界 ---\n";
  if (!prepare_fs()) return false;

  bool ok = true;
  int status = 0;
  std::string enc, body;

  /// `index.html` 的 V1 内容；下列断言里的长度就是它的长度（别写死 21，
  /// 改内容时那个数会跟着漂）。
  const std::string kIndexV1 = "<html>INDEX-V1</html>";

  /// 把 tag 按 `\x1f` 切成字段，返回字段数（最多 4 段）。
  struct Split {
    static size_t run(const std::string& tag, std::string out[4]) {
      size_t n = 0;
      size_t i = 0;
      while (n < 4) {
        const size_t j = tag.find('\x1f', i);
        out[n++] = tag.substr(i, j == std::string::npos ? j : j - i);
        if (j == std::string::npos) break;
        i = j + 1;
      }
      return n;
    }
  };

  // --- A. tag 是**文件**的身份，不是 URL 的身份 ---
  {
    StaticTestServer runner(kTmpDir, /*spa_fallback=*/true, /*mount=*/"/",
                            /*nloops=*/1, /*precompressed=*/false,
                            uvcpp_static_revalidate::LOOP_STAT);
    const int port = runner.start();

    if (!raw_get(port, "/index.html", "", status, enc, body)) {
      std::cout << "  [err] /index.html 请求没完成\n";
      ok = false;
    } else if (status != 200 || body != kIndexV1) {
      std::cout << "  [err] /index.html -> " << status << " '" << body
                << "'（期望 200 / V1）\n";
      ok = false;
    } else if (runner.serve_count() != 1) {
      std::cout << "  [err] 一条请求触发了 " << runner.serve_count()
                << " 次处理函数（期望 1）—— 后面的下标对不上号\n";
      ok = false;
    } else if (!runner.serve(0).deferred) {
      std::cout << "  [err] 首次请求必然未命中（缓存是空的），应当推迟去读盘\n";
      ok = false;
    } else if (!runner.serve(0).cache_tag.empty()) {
      std::cout << "  [err] 未命中时不该已经有 tag —— 读盘还没回来，此刻还没有"
                   "「盘上那份字节」这个身份\n";
      ok = false;
    }

    // 第二次：命中，tag 必须就位。
    if (!raw_get(port, "/index.html", "", status, enc, body)) {
      std::cout << "  [err] /index.html 第二次请求没完成\n";
      ok = false;
    } else if (status != 200 || body != kIndexV1) {
      std::cout << "  [err] 第二次 /index.html -> " << status << " '" << body
                << "'（期望 200 / V1）\n";
      ok = false;
    } else if (runner.serve_count() != 2 || runner.serve(1).deferred) {
      std::cout << "  [err] 第二次 /index.html 本该命中\n";
      ok = false;
    } else {
      const std::string tag_a = runner.serve(1).cache_tag;
      std::string f[4];
      const size_t nf = Split::run(tag_a, f);
      if (nf != 4 || f[0].empty() || f[1].empty() || f[2].empty() ||
          f[3].empty()) {
        std::cout << "  [err] 命中的 tag 应当是四段（盘上路径 / 大小 / mtime 秒"
                     " / mtime 纳秒），实得 " << nf << " 段：'" << tag_a << "'\n";
        ok = false;
      } else {
        // 第一段是盘上的路径（结尾必须是那个文件名），第二段是文件长度，
        // 后两段是十进制的时间戳。**不比整串**：盘上路径取决于这里用的相对
        // 目录，写死了换个工作目录就红，而那与本节要钉的东西无关。
        const bool path_ok = f[0].size() >= 10 &&
                             f[0].compare(f[0].size() - 10, 10, "index.html") == 0;
        const bool size_ok = f[1] == std::to_string(kIndexV1.size());
        bool digits_ok = true;
        for (size_t k = 1; k < 4 && digits_ok; ++k) {
          if (f[k].empty()) digits_ok = false;
          for (size_t c = 0; c < f[k].size(); ++c) {
            if (f[k][c] < '0' || f[k][c] > '9') digits_ok = false;
          }
        }
        if (!path_ok) {
          std::cout << "  [err] tag 第一段不是 index.html 的盘上路径：'" << f[0]
                    << "'\n";
          ok = false;
        }
        if (!size_ok) {
          std::cout << "  [err] tag 第二段应当是文件长度 " << kIndexV1.size()
                    << "，实得 '" << f[1] << "'\n";
          ok = false;
        }
        if (!digits_ok) {
          std::cout << "  [err] tag 的后三段应当是十进制\n";
          ok = false;
        }
      }

      // 同一份文件、另一条 URL（`/` 经索引步进归到 `/index.html`）⇒
      // **同一个 tag**。不同就是把同一份文件的两条 URL 各压一遍、各存一份
      // 压好的字节 —— 变体缓存按 tag 分桶，它认不出这是同一个文件。
      if (!raw_get(port, "/", "", status, enc, body)) {
        std::cout << "  [err] / 请求没完成\n";
        ok = false;
      } else if (status != 200 || body != kIndexV1) {
        std::cout << "  [err] / -> " << status << " '" << body << "'（期望 200 / V1）\n";
        ok = false;
      } else if (runner.serve_count() != 3 || runner.serve(2).deferred) {
        std::cout << "  [err] / 应当命中 index.html 那一条\n";
        ok = false;
      } else if (runner.serve(2).cache_tag != tag_a) {
        std::cout << "  [err] `/` 与 `/index.html` 归到同一个文件，tag 必须相同"
                     " —— 不同就是把同一份文件压了两遍\n    '"
                  << runner.serve(2).cache_tag << "' vs '" << tag_a << "'\n";
        ok = false;
      }
    }

    // 另一个文件 ⇒ 另一个 tag。
    if (!raw_get(port, "/lib..min.js", "", status, enc, body)) {
      std::cout << "  [err] /lib..min.js 请求没完成\n";
      ok = false;
    } else if (status != 200 || body != "MINIFIED") {
      std::cout << "  [err] /lib..min.js -> " << status << " '" << body
                << "'（期望 200 / MINIFIED）\n";
      ok = false;
    } else if (runner.serve_count() != 4 || !runner.serve(3).deferred) {
      std::cout << "  [err] /lib..min.js 第一次本该未命中\n";
      ok = false;
    }
    if (!raw_get(port, "/lib..min.js", "", status, enc, body)) {
      std::cout << "  [err] /lib..min.js 第二次请求没完成\n";
      ok = false;
    } else if (runner.serve_count() != 5 || runner.serve(4).deferred) {
      std::cout << "  [err] /lib..min.js 第二次本该命中\n";
      ok = false;
    } else if (runner.serve(4).cache_tag == runner.serve(1).cache_tag) {
      std::cout << "  [err] 两个不同的文件不该算出同一个 tag\n";
      ok = false;
    }

    // 内容变了 ⇒ tag 必须变。这里让**长度**变（不赌纳秒级 mtime 的粒度）：
    // 缓存与 tag 的"变了没有"都同时看 mtime 与 size，长度变是那条判据里
    // 最不依赖文件系统计数精度的一半。
    const std::string kIndexV2 = "<html>INDEX-V2-LONGER</html>";
    if (!write_text("index.html", kIndexV2)) {
      std::cout << "  [err] 改 index.html 失败\n";
      ok = false;
    }
    if (!raw_get(port, "/index.html", "", status, enc, body)) {
      std::cout << "  [err] 改写后 /index.html 请求没完成\n";
      ok = false;
    } else if (status != 200 || body != kIndexV2) {
      std::cout << "  [err] 改写后 /index.html -> " << status << "（期望新字节）\n";
      ok = false;
    } else if (!runner.serve(5).deferred) {
      std::cout << "  [err] 改写后第一次本该未命中（长度变了）\n";
      ok = false;
    }
    if (!raw_get(port, "/index.html", "", status, enc, body)) {
      std::cout << "  [err] 改写后第二次请求没完成\n";
      ok = false;
    } else if (runner.serve(6).deferred) {
      std::cout << "  [err] 改写后第二次本该命中\n";
      ok = false;
    } else if (runner.serve(6).cache_tag == runner.serve(1).cache_tag) {
      std::cout << "  [err] 文件内容变了，tag 必须跟着变 —— 否则变体缓存会把"
                   "旧字节当成新文件的那一份发出去\n";
      ok = false;
    }
    runner.shutdown();
  }

  // --- B. 用了孪生文件 ⇒ **不设** tag ---
  //
  // 孪生命中时 body 已经是压缩产物，`apply_compression_for()` 见到
  // `content-encoding` 根本不会进来。设了它只会白占一份变体缓存的位置。
  {
    StaticTestServer runner(kTmpDir, /*spa_fallback=*/true, /*mount=*/"/",
                            /*nloops=*/1, /*precompressed=*/true,
                            uvcpp_static_revalidate::LOOP_STAT);
    const int port = runner.start();

    for (int i = 0; i < 2; ++i) {
      if (!raw_get(port, "/app.js", "br;q=1, gzip;q=0.8", status, enc, body)) {
        std::cout << "  [err] 孪生第 " << i << " 次请求没完成\n";
        ok = false;
      } else if (status != 200 || enc != "br" || body != kAppJsBr) {
        std::cout << "  [err] 孪生第 " << i << " 次 -> " << status << " enc='"
                  << enc << "' body='" << body << "'（期望 200 / br / 孪生那份）\n";
        ok = false;
      }
    }
    if (runner.serve_count() != 2 || runner.serve(1).deferred) {
      std::cout << "  [err] 孪生第二次本该命中（缓存键带 `.br` 后缀）\n";
      ok = false;
    } else if (!runner.serve(1).cache_tag.empty()) {
      std::cout << "  [err] 发的是盘上孪生（已经是压缩产物），不该设 cache_tag\n";
      ok = false;
    }
    runner.shutdown();
  }

  // --- C. 条目上限 + LRU 次序 ---
  //
  // 上限 2，然后故意让"最近用过"的次序与"插入"的次序不同：A、B 之后**再摸一次
  // A**，然后插入 C ⇒ 被淘汰的必须是 B。判据是最后再请求一次 B —— 只有真按
  // LRU 淘汰才成立（按插入顺序淘汰的话走的会是 A）。
  {
    StaticTestServer runner(kTmpDir, /*spa_fallback=*/false, /*mount=*/"/",
                            /*nloops=*/1, /*precompressed=*/false,
                            uvcpp_static_revalidate::LOOP_STAT);
    runner.cache_max_entries = 2;
    const int port = runner.start();

    struct Step {
      const char* path;
      bool want_deferred;
      const char* why;
    };
    const Step steps[] = {
      {"/app.js",      true,  "A 第一次：未命中"},
      {"/index.html",  true,  "B 第一次：未命中"},
      {"/app.js",      false, "A 命中（顺带把它提升到队首）"},
      {"/lib..min.js", true,  "C 第一次：未命中 —— 该把 B 挤出去"},
      {"/app.js",      false, "A 还在（它是最近用过的）"},
      {"/index.html",  true,  "B 已被淘汰 ⇒ 又是未命中"},
    };
    const size_t nstep = sizeof(steps) / sizeof(steps[0]);
    for (size_t i = 0; i < nstep; ++i) {
      const size_t before = runner.serve_count();
      if (!raw_get(port, steps[i].path, "", status, enc, body)) {
        std::cout << "  [err] #" << i << " " << steps[i].path << " 请求没完成\n";
        ok = false;
        continue;
      }
      if (runner.serve_count() != before + 1) {
        std::cout << "  [err] #" << i << " 触发 " << (runner.serve_count() - before)
                  << " 次处理函数（期望 1）\n";
        ok = false;
        continue;
      }
      const StaticTestServer::ServeObservation o = runner.serve(before);
      if (o.deferred != steps[i].want_deferred) {
        std::cout << "  [err] #" << i << " " << steps[i].path << " deferred="
                  << (o.deferred ? 1 : 0) << "（" << steps[i].why << "）\n";
        ok = false;
      }
      // 上限 2：**任何一个观测点上**都不许超过。观测点在插入之前，所以它量的是
      // "上一次请求留下的表"，正好覆盖"插入之后有没有当场把越界的那条淘汰掉"。
      if (o.cache_size > 2) {
        std::cout << "  [err] #" << i << " 缓存条目数 " << o.cache_size
                  << " 超过上限 2\n";
        ok = false;
      }
    }
    // 收尾再摸一次 A：此刻表里应当正好 2 条（A 与 B），字节账 > 0。
    if (!raw_get(port, "/app.js", "", status, enc, body)) {
      std::cout << "  [err] 收尾请求没完成\n";
      ok = false;
    } else if (runner.serve_count() != nstep + 1) {
      std::cout << "  [err] 收尾请求的处理函数没记上（观测数 "
                << runner.serve_count() << "）\n";
      ok = false;
    } else {
      const StaticTestServer::ServeObservation o = runner.serve(nstep);
      if (o.cache_size != 2) {
        std::cout << "  [err] 6 条请求、上限 2 之后表里应当正好 2 条，实得 "
                  << o.cache_size << "\n";
        ok = false;
      }
      if (o.cache_bytes == 0) {
        std::cout << "  [err] 表里有条目，字节账不该是 0\n";
        ok = false;
      }
    }
    runner.shutdown();
  }

  // --- D. `set_cache_enabled(false)` 必须连**写**一起关掉 ---
  //
  // 从前只关掉了命中那一支的"读"：表照样只增不减，"关了缓存"的进程内存还在涨，
  // 而且 `cache_size()` 也跟着涨 —— 那不是省一次判断，是个漏洞。
  {
    StaticTestServer runner(kTmpDir, /*spa_fallback=*/false, /*mount=*/"/",
                            /*nloops=*/1, /*precompressed=*/false,
                            uvcpp_static_revalidate::LOOP_STAT);
    runner.cache_enabled = false;
    const int port = runner.start();

    for (int i = 0; i < 2; ++i) {
      if (!raw_get(port, "/app.js", "", status, enc, body)) {
        std::cout << "  [err] 关缓存后第 " << i << " 次请求没完成\n";
        ok = false;
      } else if (status != 200 || body != kAppJs) {
        std::cout << "  [err] 关缓存后第 " << i << " 次 -> " << status << " '"
                  << body << "'（关缓存不影响正确性）\n";
        ok = false;
      }
    }
    if (runner.serve_count() != 2) {
      std::cout << "  [err] 关缓存时两条请求都该触发处理函数\n";
      ok = false;
    } else if (!runner.serve(1).deferred) {
      std::cout << "  [err] 关缓存时第二次仍该未命中\n";
      ok = false;
    } else if (runner.serve(1).cache_size != 0) {
      std::cout << "  [err] 关缓存时表必须一直是空的，实得 "
                << runner.serve(1).cache_size << " 条\n";
      ok = false;
    }
    runner.shutdown();
  }

  // --- E. 单份文件超过字节预算 ⇒ 干脆不缓存 ---
  //
  // 否则它一进来就把整个缓存冲空，然后下一次仍然装不下 —— 退化成"每次请求都
  // 把缓存冲一遍"，比不缓存更贵，还让别的条目全被误伤。
  {
    StaticTestServer runner(kTmpDir, /*spa_fallback=*/false, /*mount=*/"/",
                            /*nloops=*/1, /*precompressed=*/false,
                            uvcpp_static_revalidate::LOOP_STAT);
    // 预算 64：`index.html`（20 出头）装得下，`big.bin`（256 KiB）装不下。
    runner.cache_max_bytes = 64;
    const int port = runner.start();

    // 先让 index.html 占住一条，用来判"大文件没把它挤掉"。
    if (!raw_get(port, "/index.html", "", status, enc, body) ||
        !raw_get(port, "/index.html", "", status, enc, body)) {
      std::cout << "  [err] index.html 预热请求没完成\n";
      ok = false;
    } else if (status != 200) {
      std::cout << "  [err] index.html -> " << status << "（期望 200）\n";
      ok = false;
    } else if (runner.serve(1).deferred || runner.serve(1).cache_size != 1) {
      std::cout << "  [err] index.html 小于预算，本该被缓存（deferred="
                << (runner.serve(1).deferred ? 1 : 0) << " size="
                << runner.serve(1).cache_size << "）\n";
      ok = false;
    }

    const size_t base = runner.serve_count();
    for (int i = 0; i < 2; ++i) {
      if (!raw_get(port, "/big.bin", "", status, enc, body)) {
        std::cout << "  [err] big.bin 第 " << i << " 次请求没完成\n";
        ok = false;
      } else if (status != 200 || body.size() != 256u * 1024u) {
        std::cout << "  [err] big.bin 第 " << i << " 次 -> " << status << "，"
                  << body.size() << " 字节（期望 200 / 262144）\n";
        ok = false;
      }
    }
    if (runner.serve_count() != base + 2) {
      std::cout << "  [err] 超过预算的文件每次都该去读盘（实触发 "
                << (runner.serve_count() - base) << " 次）\n";
      ok = false;
    } else if (!runner.serve(base + 1).deferred) {
      std::cout << "  [err] big.bin 超过字节预算，第二次仍该未命中\n";
      ok = false;
    } else if (runner.serve(base + 1).cache_size != 1) {
      std::cout << "  [err] big.bin 不该进表，更不该把 index.html 挤掉（实得 "
                << runner.serve(base + 1).cache_size << " 条）\n";
      ok = false;
    }
    runner.shutdown();
  }

  cleanup_fs();
  return ok;
}

int main(int argc, char** argv) {
  // **整条输出不许缓冲**。这条文件里有 `set_loops(4)` + 线程池 + 半途断连，
  // 出问题是 SIGSEGV —— 而 ctest 只拿得到进程**真的写进管道**的那些字节。
  // libc 的缓冲区一旦还在它手里，崩溃现场之前打印的每一条进度都跟着进程一起
  // 没了，CI 上就只剩一行 `***Exception: SegFault`，连崩在哪条用例（更别说哪一
  // 步）都看不出来。所以这里关掉缓冲，让每条进度在打印的那一刻就落地。
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"construct_and_config", test_construct_and_config},
    {"serve_and_update", test_serve_and_update},
    {"not_found_and_traversal", test_not_found_and_traversal},
    {"mount_prefix", test_mount_prefix},
    {"head_no_body", test_head_no_body},
    {"generation_and_midflight_disconnect", test_generation_and_midflight_disconnect},
    {"stale_connection_crosstalk", test_stale_connection_crosstalk},
    {"multiloop_concurrency", test_multiloop_concurrency},
    {"precompressed", test_precompressed},
    {"loop_stat", test_loop_stat},
    {"cache_tag_and_bounds", test_cache_tag_and_bounds},
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
