/**
 * @file tests/functional/web_app_multiloop_func.cpp
 * @brief `uvcpp_web_app::set_loops(n > 1)`：停机扇出、按循环分片、以及两张表。
 *
 * 这个文件回答的问题
 * ------------------
 * net 层那条路（`uvcpp_tcp_server::set_loops`）已经有自己的用例
 * （`tcp_multiloop_func.cpp`），web 层的按循环切容器也有
 * （`web_multiloop_func.cpp`）。缺的是**框架这一层**：`uvcpp_web_app::set_loops()`
 * 之前根本不存在，于是 n>1 在 webapp 层端到端不可达，设计稿 §6 里那些判据
 * 无处可落。本文件把它接上，并且把"接上了没有"落成可红的断言。
 *
 * 判据逐条（以及"未改的代码 / 坏实现会让它怎么红"）
 * -------------------------------------------------
 * 1. **对照组 n == 1 逐字不变** —— `loop_count() == 1`、请求照常答对、
 *    `connection_count_at(0) == connection_count()`、身份号高段为 0。
 *    没有这个对照组，"n>1 也答对了"证明不了是分片分对了。
 * 2. **停机扇出（本批的主判据，确定性红）** —— n=2 起服务，主线程 `stop()`
 *    之后 `join()` 必须在有界墙钟内返回，**而且不许留下"有循环没退出"的
 *    ERROR**。未改的代码：`stop()` 走 `post(fn)`，而 `post(fn)` 投给
 *    `slot_here()`，从非循环线程调时是 **0 号**，于是 `begin_shutdown()` 只在
 *    接受者那条循环上跑，工作循环既不进停机状态机、也不会被停 ⇒ 那几格的
 *    `async`/`timer` 没人删 ⇒ `uv_loop_close` 撞 `UV_EBUSY` ⇒ 整块循环泄漏。
 *
 *    **红法不是"挂住"，是那条 ERROR —— 这一点核过源码才敢这么写。**
 *    `join()` 的等待是**有界**的（`shutdown_grace_ms` 默认 3000 +
 *    `kJoinSlackMs` 5000 ⇒ 8 s 之后它报出来然后返回，见
 *    `uvcpp_web_app.cpp` 里那段注释）。所以"`join()` 返回了"这条断言在坏实现
 *    上**照样绿**（8 s < 用例给的 15 s）；唯一有牙的进程内观测面是它打出的
 *    「join() 已等 N ms，仍有 M 条循环没退出」—— 见 `capture_sink`。
 *    **"我原以为是挂住"必须写出来**：按错的机理写断言，判据 2 就会是一条
 *    恒绿的摆设。外部那只眼睛是 `run_loop_leak_probe.py`（看 `UV_EBUSY`
 *    的整块泄漏），两者不互相冒充。
 *
 *    墙钟兜底仍然要留（见 `require_within` / `stop_and_join_within`）：坏实现
 *    还可能有别的挂法，而 ctest 的 `Timeout` 看不出卡在哪一条判据上。
 * 3. **请求跑在工作循环上** —— n>1 时处理函数所在的那条线程必须**不是** 0 号
 *    循环的线程，而且必须**就是**该连接所属循环的线程。**哪条连接属哪条循环
 *    有两条形状**（`is_fanout()`，运行时探测）：转手那条路是 net 层
 *    `accept_and_handoff()` 里那条显式轮转定的（0 号一条都不留）；内核分流那条
 *    路上 0 号**自己也承载连接** ⇒ "处理函数不在 0 号线程上"在分流下必然为假，
 *    那一支只断言"是该连接所属循环的线程"。
 *
 *    **不夸大**：分流那一支**抓不到"每格都回落 0 号"**这类坏实现（0 号本来就在
 *    收），它在分流下改由**监听 socket 普查**负责（每个循环一个监听句柄、属主
 *    pid 相同）：`bench/bench_server.cpp` 的 `--loops N` 与 `/stats`。
 *
 *    **"哪条循环是哪条线程"是问出来的，不是猜的**：
 *    `uvcpp_loop_index_of_this_thread()` 不是导出符号（`src/net/uvcpp_loop_worker.h`
 *    里那三个声明都没有 `UVCPP_API`），用例链不到它 —— 现有那两个多循环用例
 *    （`tcp_multiloop_func` / `web_multiloop_func`）用的也是
 *    `std::this_thread::get_id()`。而 `post(fn, i)` 的语义恰好是"在 i 号循环的
 *    线程上跑 fn"，于是它就是一个现成的公开口子：逐格投一个"报出自己是谁"的
 *    任务，就拿到了"循环号 → 线程身份"这张表（见 `ask_loop_threads`）。
 * 4. **id 的高段就是它所属的循环号** —— 每个 `on_connection` 收到的 id 都要
 *    满足 `loop_of(id) == client->loop_index()` —— **这条两条形状都成立，是承重
 *    面**。至于"哪条循环分到几条"：转手路上是显式轮转（n=3 时 16 条分给 2 条
 *    必然 {8,8}、0 号一条都不留），分流路上由内核哈希定 ⇒ 那一支只断言"高段都
 *    落在 0..n-1 之内"。坏实现（id 不带高段）两条路上都红。
 * 5. **聚合量跑着读也安全** —— 循环还在跑的时候从主线程读
 *    `connection_count()` / `connection_count_at(i)` / `inflight_count()`，
 *    取值必须在界内，且各格之和等于总数。**这条是"单写者 + 原子读数"的
 *    论证，不是竞态判据** —— 本仓构建里没有 sanitizer，数据竞争没有确定性的
 *    观测方式（诚实说明见文末）。
 * 6. **每一条循环各发自己的 Close/GOAWAY** —— WS 会话挂在 1 号循环上时，
 *    停机必须让对端收到 **Close 帧（1001 GOING_AWAY）**，而不是连接被直接
 *    断掉。坏实现（只让 0 号发、或者干脆每格都发一遍全量会话）红。
 * 7. **静态缓存在两条循环下是共享一份的** —— n=3（两条工作循环）下两条连接
 *    并发打同一个静态路径：不崩、`cache_entries()` 收敛到 1、
 *    `hits + misses` 等于真正拿到正文的请求数、**`cache_bytes()` 恰好等于
 *    文件长度**（并发 `put()` 会让同一份数据被计两次）。
 * 8. **`set_loops` 的契约** —— 0 / -1 / 65 ⇒ `UV_EINVAL`；`start()` 之后 ⇒
 *    `UV_EBUSY`；先自己调 `tcp_server()->set_loops(2)` 再调 `app.set_loops(2)`
 *    ⇒ `UV_EBUSY`（**不是静默的 0** —— 那种情况下每循环就绪钩子装不上，
 *    工作循环那几格会永远空着）；n>1 时 `run(md)` ⇒ `UV_EINVAL`，而且**之后
 *    `start()` 仍然可用**。
 *
 * **不在本用例里的**：TLS 的多循环语义（每条循环一个 `SSL` 对象 —— 设计稿
 * §4.4，本批没做，`set_loops` 与 `enable_ssl()` 同时用时请自行确认）；
 * POSIX 那条 `dup()` 转手路 —— 本机是 Windows，那条路**编得到、跑不到**，
 * 证据在 CI 的 ubuntu/macOS 腿上。不要写成"两端都验过"。
 *
 * 诚实说明（两处竞态拿不到确定性的红）
 * ------------------------------------
 * 本仓的构建里**没有 sanitizer**（没有 ASan/TSan 的档位）。所以判据 5 与判据 7
 * 里那两处"共享容器跨线程访问"的修复，能给的只有：单写者论证 + 重复跑的
 * 压力形状 + 页面堆门禁（`tests/tools/run_pageheap_gate.py`）与循环泄漏探针
 * （`tests/tools/run_loop_leak_probe.py`）。**"跑了 100 遍没崩"不是判据** ——
 * 它是"没观察到"，别在记录里写成"验过了"。真正确定性的那两条是判据 2（挂住）
 * 与判据 6（Close 帧），停机扇出这条本批的主判据正是选在这里。
 *
 * 判据 2 里那句"红法是挂住"是**写的时候就想错了**，改对的过程记在这里：本条
 * 一开始把 `stop()+join()` 写成"在有界墙钟内返回"，指望坏实现挂住。核 `join()`
 * 的实现才发现它自己有 8 秒的界，坏实现照样返回 —— 于是一条本该是主判据的断言
 * 变成了恒绿。要不是先变异了一遍，这条会以"全绿"的形式交出去。
 *
 * 崩溃定位：每条用例先打名字再跑（`std::unitbuf`），进程中途挂掉也能从最后
 * 一行看出死在哪一条。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h>  // _mkdir / _rmdir
#else
#include <sys/stat.h>  // mkdir
#include <unistd.h>    // rmdir
#endif

#include <uv.h>

#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_ws_parser.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_connection.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_static.h>
#include <webapp/uvcpp_web_ws.h>

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
// 有界等待：超时 = 报失败 + 就地结束进程
// =========================================================================

/**
 * @brief 等到 \p pred 成立；到点还是不成立就打印**是哪一条**等不到，然后退出。
 *
 * 为什么不是"返回 false 让调用方记账"：本文件的主判据（判据 2）红起来就是
 * **挂住**。等不到的时候调用方**没有机会**去打印 —— 整条用例会一直挂到 ctest
 * 的超时上限，红的形状变成一条 Timeout，看不出卡在哪一条判据上。这与
 * `tcp_multiloop_func.cpp` 里那个同名帮手是同一条理由。
 *
 * @return 成立时返 true。**永不返回 false**：等不到的那条路直接 `_Exit(2)`，
 *         所以调用方可以把它当一个"必须成立"的断言来用（返回值只是为了能
 *         写成 `return require_within(...)` 这种形状）。
 */
bool require_within(const std::function<bool()>& pred, int ms,
                    const char* what) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (pred()) return true;
  std::cout << "  -> FAIL: 等不到「" << what << "」（" << ms
            << " ms 墙钟上限）" << std::endl;  // endl 会 flush，下面 _Exit 不丢字
  std::_Exit(2);
  return false;  // 到不了
}

/** @brief 测试用的 App 骨架：回环 + 端口 0 + 日志压到 WARN（免得刷屏）。 */
void configure_for_test(uvcpp_web_app& app) {
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);
}

// =========================================================================
// 日志捕获：判据 2 的**唯一确定性观测面**
// =========================================================================

/**
 * @brief 把 WARN/ERR 级别的日志收进内存。
 *
 * 为什么非要有它：`join()` 等的是**有界**的
 * （`shutdown_grace_ms`（默认 3000）+ `kJoinSlackMs`（5000）之后它就报出来然后
 * 返回，见 `uvcpp_web_app.cpp` 里那段注释）。所以"工作循环没被停掉"这条缺陷
 * **不会**表现为挂死 —— 它表现为一条 ERROR（「join() 已等 N ms，仍有 M 条循环
 * 没退出」）。
 *
 * 不看日志的话，「`join()` 在有界墙钟内返回」这条断言在**坏实现上照样绿**
 * （8 秒 < 用例给的 15 秒上限），判据 2 就成了本仓最反对的那种"跑了但恒绿"。
 * 这里把那条 ERROR 变成可断言的量。
 *
 * 外部量具（`run_loop_leak_probe.py` 看 `uv_loop_close` 撞 `UV_EBUSY` 的整块
 * 泄漏）仍然是**独立的第二只眼睛**，两者不互相冒充。
 */
class capture_sink : public uvcpp_log_sink {
 public:
  void write(const uvcpp_log_record& r) override {
    if (r.level != log_level::WARN && r.level != log_level::ERR) return;
    std::lock_guard<std::mutex> lk(mu_);
    lines_.push_back(r.message);
  }

  /** @brief 含 \p needle 的条数。 */
  size_t count_containing(const std::string& needle) const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (size_t i = 0; i < lines_.size(); ++i) {
      if (lines_[i].find(needle) != std::string::npos) ++n;
    }
    return n;
  }

  /** @brief 第一条含 \p needle 的原文（没有时返空串），失败时打出来用。 */
  std::string first_containing(const std::string& needle) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < lines_.size(); ++i) {
      if (lines_[i].find(needle) != std::string::npos) return lines_[i];
    }
    return std::string();
  }

  void clear() {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.clear();
  }

 private:
  mutable std::mutex mu_;
  std::vector<std::string> lines_;
};

capture_sink g_sink;

/** @brief `join()` 只在这条路上打 ERROR ⇒ 它出现就说明有循环没退出。 */
const char* kJoinIncomplete = "条循环没退出";

void install_capture_sink() {
  uvcpp_logger::instance().set_sink(&g_sink);
  // 等级不能压得太高：那条 ERROR 本身是 ERR，任何等级都留得住，但把它留住
  // 需要 sink 真的收到它 —— 全局阈值压到 WARN 即可（`configure_for_test`
  // 又把每个 App 的阈值设成 WARN，同一档）。
  uvcpp_logger::instance().set_level(log_level::WARN);
}

// =========================================================================
// HTTP 侧小工具（沿用 web_app_app_func.cpp 的写法）
// =========================================================================

bool roundtrip(int port, const uvcpp_http_request& req,
               uvcpp_http_response& resp) {
  for (int i = 0; i < 40; ++i) {
    uvcpp_http_client client;
    if (client.connect_wait("127.0.0.1", port, 2000) == 0 &&
        client.send_wait(req, resp, 3000) == 0) {
      return true;
    }
    // Windows 上 connect 偶尔会撞上内核的负缓存 —— 系统行为，不是被测代码
    // 的问题，不该让用例间歇性变红。
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

bool get(int port, const std::string& path, uvcpp_http_response& resp) {
  return roundtrip(port, uvcpp_http_request::make_get(path), resp);
}

std::string body_of(const uvcpp_http_response& r) {
  if (r.body.size() == 0) return std::string();
  return std::string(r.body.get_const_data(), r.body.size());
}

int status_of(const uvcpp_http_response& r) {
  return static_cast<int>(r.status_code);
}

/**
 * @brief 从**裸报头**里取状态码。
 *
 * WS 探针收的是原始字节，拿不到 `uvcpp_http_response`；它的 101 只能自己解。
 */
int status_of_raw(const std::string& resp) {
  if (resp.size() < 12) return -1;
  return std::atoi(resp.c_str() + 9);
}

/**
 * @brief 问出每一格循环的**线程身份**（只用公开 API）。
 *
 * `uvcpp_loop_index_of_this_thread()` 不是导出符号，用例链不到它。而
 * `post(fn, i)` 的语义恰好是"在 i 号循环的线程上跑 fn" —— 于是它就是那个
 * 现成的、公开的"问某条循环要身份"的口子（顺便，这条判据自己也在测
 * `post(fn, loop_index)` 这个重载：投错格的话这张表当场对不上）。
 *
 * @param out 调用前必须已经 resize 到 n。
 * @return true = 每一格都在墙钟内报出了身份。
 */
bool ask_loop_threads(uvcpp_web_app& app, int n,
                      std::vector<std::thread::id>& out) {
  std::atomic<int> done(0);
  for (int i = 0; i < n; ++i) {
    // `out` 的每个元素只被它自己那条循环写，`done` 的 seq_cst 递增/读取
    // 把"写完了"这件事交给主线程 —— 不需要额外的锁。
    app.post(
        [&out, &done, i] {
          out[static_cast<size_t>(i)] = std::this_thread::get_id();
          done.fetch_add(1);
        },
        i);
  }
  return require_within([&] { return done.load() == n; }, 5000,
                        "n 格循环都报出了线程身份");
}

/**
 * @brief 在**另一条线程**上跑 `stop()` + `join()`，主线程拿墙钟等它。
 *
 * 为什么不能直接在主线程调：判据 2（停机扇出）红起来就是**挂住**，直接调的话
 * 调用方永远没有机会打印 —— 红的形状会退化成一条看不出卡在哪的 ctest
 * `Timeout`。所以"会在哪一条上挂住"的那个动作必须放到能被打断的观察之下。
 *
 * @return true = 在 \p ms 之内收场了；false = 超时（**调用方必须判它**，
 *         就此退出进程，因为那条线程还卡在 `join()` 里、动不了 app）。
 */
bool stop_and_join_within(uvcpp_web_app& app, int ms, const char* what) {
  std::atomic<bool> done(false);
  std::thread t([&app, &done] {
    app.stop();
    app.join();
    done.store(true);
  });

  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  bool ok = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (done.load()) {
      ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (ok) {
    t.join();
    return true;
  }

  std::cout << "  -> FAIL: 等不到「" << what << "」（" << ms
            << " ms 墙钟上限）" << std::endl;
  std::_Exit(2);
  return false;  // 到不了
}

// =========================================================================
// WS 探针：裸 TCP + 手工字节，事件循环由测试线程拨
// =========================================================================

/**
 * @brief 一个能逐帧看的 WS 客户端。
 *
 * 为什么不用真 `uvcpp_ws_client`：判据 6 要区分「收到 Close 帧」与「连接被
 * 直接断掉」—— 那两件事在协议层客户端上都是 `on_close`，只差一个码，而
 * "没收到帧就被断掉"在某些路径上连回调都不来。手工解析帧能直接数 Close 帧
 * 的条数与码。
 *
 * 服务端跑在 `start_background()` 自己的线程上，测试拨不动它的事件循环，
 * 能拨的只有**本端**这条（`pump()` 里那句 `run(UV_RUN_NOWAIT)`）。
 */
struct ws_probe {
  uvcpp_tcp_client tcp;
  uvcpp_ws_parser parser;

  bool connected = false;
  bool handshake_done = false;
  bool upgraded = false;      ///< 服务端回的是 101
  bool peer_closed = false;   ///< 底层连接被关（FIN/RST）—— 与"收到 Close 帧"是两件事
  bool parse_error = false;
  std::string resp;           ///< 非 101 时：完整响应
  std::vector<uvcpp_ws_frame> frames;
  const char* where = "";

  bool connect(int port) {
    tcp.set_on_close([this]() { peer_closed = true; });
    if (tcp.connect("127.0.0.1", port,
                    [this](int st) { connected = (st == 0); }) != 0) {
      where = "connect call";
      return false;
    }
    if (!pump([this] { return connected; }, 3000)) {
      where = "connect timeout";
      return false;
    }
    return true;
  }

  /**
   * @brief 装读回调。**必须在连上之后**：socket 还没建立时装了会静默拿不到
   *        任何字节（真客户端的 `read_start` 也写在写完成回调里）。
   */
  bool start_reading() {
    const int rc = tcp.read_start([this](uvcpp_buf* b) {
      if (!b || b->size() == 0) return;
      const std::string chunk(b->get_const_data(), b->size());
      if (handshake_done) {
        if (upgraded) {
          feed(chunk);
        } else {
          resp += chunk;
        }
        return;
      }
      resp += chunk;
      const size_t e = resp.find("\r\n\r\n");
      if (e == std::string::npos) return;
      handshake_done = true;
      upgraded = (resp.compare(0, 12, "HTTP/1.1 101") == 0);
      if (!upgraded) return;  // 报文体留在 resp 里，不是 WS 帧
      const std::string tail = resp.substr(e + 4);
      resp.resize(e + 4);
      if (!tail.empty()) feed(tail);
    });
    if (rc != 0) {
      where = "read_start";
      return false;
    }
    return true;
  }

  /**
   * @brief 只喂**这一次新到的**字节。
   *
   * 解析器自己留着半帧状态，所以外面**绝不能再存一份半帧缓冲**并整段重喂。
   */
  void feed(const std::string& chunk) {
    const char* d = chunk.data();
    size_t n = chunk.size();
    while (n > 0) {
      const size_t used = parser.execute(d, n);
      const ws_parser_state st = parser.get_state();
      if (st == ws_parser_state::PARSE_ERROR) {
        parse_error = true;
        return;
      }
      if (st != ws_parser_state::COMPLETE) return;
      frames.push_back(parser.get_current_frame());
      parser.reset();
      if (used == 0) return;  // 防御：不前进就退出，绝不空转
      d += used;
      n -= used;
    }
  }

  bool write_bytes(const std::string& b) {
    // 用 shared_ptr 而不是栈上的 bool：写没完成时这个闭包由 TCP 客户端持有，
    // 引用局部变量就是悬垂。
    std::shared_ptr<bool> done(new bool(false));
    if (tcp.write(b.data(), b.size(), [done](int) { *done = true; }) != 0) {
      where = "write call";
      return false;
    }
    if (!pump([done] { return *done; }, 3000)) {
      where = "write timeout";
      return false;
    }
    return true;
  }

  /** @brief 拨自己的循环直到 done() 为真或超时。 */
  bool pump(const std::function<bool()>& done, int timeout_ms) {
    const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    for (;;) {
      tcp.get_loop()->run(UV_RUN_NOWAIT);
      if (done()) return true;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= timeout_ms) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  /** @brief 跑固定时长推进事件（不做断言）。 */
  void settle(int ms) {
    const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    for (;;) {
      tcp.get_loop()->run(UV_RUN_NOWAIT);
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count() >= ms) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  bool upgrade(const std::string& path) {
    const std::string req = "GET " + path + " HTTP/1.1\r\n"
                            "Host: 127.0.0.1\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n\r\n";
    return write_bytes(req);
  }

  bool wait_response(int timeout_ms = 3000) {
    return pump([this] { return handshake_done; }, timeout_ms);
  }

  /** @brief 收到的 Close 帧的码；一条都没收到时返回 -1。 */
  int close_frame_code() const {
    for (size_t i = 0; i < frames.size(); ++i) {
      if (frames[i].opcode == ws_opcode::CLOSE) {
        return static_cast<int>(frames[i].get_close_code());
      }
    }
    return -1;
  }

  /** @brief 收到过几条 Close 帧（判据 6 顺带盯住"重复发"这个反面）。 */
  int close_frame_count() const {
    int n = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
      if (frames[i].opcode == ws_opcode::CLOSE) ++n;
    }
    return n;
  }

  /** @brief 裸 TCP 关闭：一个 WS 字节都不发。 */
  void close_transport() {
    if (tcp.get_tcp() != nullptr) tcp.get_tcp()->close();
    settle(30);
  }
};

// =========================================================================
// 9. `set_loops` 的契约 + 对照组 n == 1
// =========================================================================

void test_set_loops_contract() {
  uvcpp_web_app app;
  configure_for_test(app);

  check(app.loop_count() == 1, "没调 set_loops() 时 loop_count() == 1");
  check(app.set_loops(0) == UV_EINVAL, "set_loops(0) ⇒ UV_EINVAL");
  check(app.set_loops(-1) == UV_EINVAL, "set_loops(-1) ⇒ UV_EINVAL");
  check(app.set_loops(65) == UV_EINVAL, "set_loops(65) ⇒ UV_EINVAL");
  check(app.loop_count() == 1, "被拒的调用不改 loop_count()");

  check(app.set_loops(1) == 0, "set_loops(1) ⇒ 0（逐字节等于没有这个 API）");
  check(app.loop_count() == 1, "set_loops(1) 之后 loop_count() 还是 1");

  // n>1 时 `run()` 不成立 —— 它是在**调用者线程**上就地跑一条循环，而工作
  // 循环那几条要各自有主。这一条与"之后 start() 仍然可用"必须成对出现：
  // 只断言前一半的话，实现里"顺手把状态置坏了"也能过。
  check(app.set_loops(2) == 0, "set_loops(2) 被接受（n == 1 的检查放行）");
  check(app.loop_count() == 2, "loop_count() 跟着变成 2");
  check(app.run(UV_RUN_DEFAULT) == UV_EINVAL, "n>1 时 run() ⇒ UV_EINVAL");

  app.get("/hi", [](uvcpp_web_request&, uvcpp_web_response& resp,
                    uvcpp_web_next) { resp.text("hi"); resp.end(); });

  check(app.start() == 0, "run() 被拒之后 start() 仍然可用（返回 0）");
  const int port = app.bound_port();
  check(port > 0, "bound_port() 是真端口");

  // 对照组：n>1 下请求也得答对，而且**不落在 0 号循环上**（0 号是接受者，
  // 转手之后一条连接都不留）。
  uvcpp_http_response r;
  check(get(port, "/hi", r) && status_of(r) == 200, "n=2 下 GET /hi 是 200");
  check(body_of(r) == "hi", "正文正确");

  // 挂住就自报家门，不让它变成一条只有 ctest Timeout 的红。
  check(stop_and_join_within(app, 15000, "n>1 下 stop()+join() 返回"),
        "n>1 下 stop()+join() 返回");

  check(!app.running(), "停机后 running() 为假");
  check(!app.loop_started(), "停机后 loop_started() 为假");

  // 已经启动过之后不能再改：格子是按 n 一次长成的，中途改 n 等于让已经建好的
  // 句柄没了归属。
  check(app.set_loops(1) == UV_EBUSY, "start() 之后 set_loops() ⇒ UV_EBUSY");
}

// =========================================================================
// 2'. 反向的那道门：底层自己已经是多循环
// =========================================================================

void test_tcp_already_multiloop_is_rejected() {
  uvcpp_web_app app;
  configure_for_test(app);

  // 调用者若先自己调了 `tcp_server()->set_loops(n>1)`，工作循环那时就已经起
  // 来了，而 `set_loop_start_hook()` 在 workers 非空时**只打一条 stderr 然后
  // 不装** —— 于是 1..n-1 那几格永远空着（`async` 为 nullptr），`slot_here()`
  // 把它们全部回落到 0 号：**静默退化**。这条断言就是钉住"宁可启动失败"。
  uvcpp_tcp_server* tcp = app.tcp_server();
  check(tcp != nullptr, "启动前就能拿到 tcp_server()（钩子要在它上面装）");
  if (tcp != nullptr) {
    check(tcp->set_loops(2) == 0, "底层先自己 set_loops(2) 成功");
    check(app.set_loops(2) == UV_EBUSY,
          "底层已经是多循环时 app.set_loops(2) ⇒ UV_EBUSY（**不是静默的 0**）");
  }
  app.stop();
}

// =========================================================================
// 2. 停机扇出 —— 本批的主判据（确定性红）
// =========================================================================

/**
 * @brief n=2 起服务，主线程 `stop()`，`join()` 必须在有界墙钟内返回。
 *
 * 未改的代码怎么红：`stop()` 走 `post(fn)`，`post(fn)` 投给 `slot_here()`，
 * 从非循环线程调时给 0 号 ⇒ `begin_shutdown()` 只在接受者那条循环上跑，工作
 * 循环既不进停机状态机（它的 `shutdown_timer` 从没被设过）、也不会被停 ⇒
 * 槽位里的 `async`/`timer` 没人删 ⇒ **挂住**。所以 `stop()+join()` 必须在
 * 另一条线程上跑，主线程拿墙钟等它 —— 直接在主线程调的话，红起来整条用例
 * 就只剩一条 ctest `Timeout`。
 */
void test_stop_fans_out_to_workers() {
  g_sink.clear();  // 只判本条留下的日志
  uvcpp_web_app app;
  configure_for_test(app);
  check(app.set_loops(2) == 0, "set_loops(2) 成功");

  app.get("/ping", [](uvcpp_web_request&, uvcpp_web_response& resp,
                      uvcpp_web_next) { resp.text("pong"); resp.end(); });

  check(app.start() == 0, "start() 成功");
  const int port = app.bound_port();

  // 先证明"确实有工作在 1 号循环上"—— 否则这一条可能在"一条工作循环都没起来"
  // 的形状下也能过（那时扇出没什么可扇的）。
  uvcpp_http_response r;
  check(get(port, "/ping", r) && status_of(r) == 200, "停机前请求正常");

  // 基线：走到这里不该有停机相关的 ERROR。**这一步是判据本身的对照组** ——
  // 没有它的话，"下面那条断言为真"也可能只是因为日志根本没进来（sink 装晚了、
  // 等级压过头），而那是"没测"冒充"通过"。
  check(g_sink.count_containing(kJoinIncomplete) == 0,
        "停机前没有「有循环没退出」的 ERROR");

  check(stop_and_join_within(app, 15000,
                             "主线程 stop() 之后 join() 返回（工作循环真的被停了）"),
        "主线程 stop() 之后 join() 在有界墙钟内返回");
  check(!app.running(), "停机后 running() 为假");
  check(!app.loop_started(), "两台都停下来之后 loop_started() 才为假");

  // ---- 判据 2 的确定性观测面 ------------------------------------------
  //
  // 拆掉 `stop()` 的逐槽位扇出（改成只投 0 号）之后，工作循环既不进停机状态机
  // 也不会被停 ⇒ `join()` 等满 `shutdown_grace_ms + 5 s` 之后打这条 ERROR。
  // 只看"join() 有没有返回"是**抓不住**的：它是有界等待，坏实现 8 秒就返回了，
  // 仍然落在上面给的 15 秒里 —— 那条断言在坏实现上照样绿。
  check(g_sink.count_containing(kJoinIncomplete) == 0,
        "停机没有留下「有循环没退出」的 ERROR（实际：" +
            g_sink.first_containing(kJoinIncomplete) + "）");
}

// =========================================================================
// 3 + 4. 请求落在哪条循环上、id 的高段
// =========================================================================

void test_loop_identity_and_ids() {
  // n=3 ⇒ 2 条工作循环。轮转是**显式**的（`1 + i % (n-1)`），所以 16 条连接
  // 必然 8/8 分给 1 号与 2 号 —— 这是可以断言的结构，不是均匀度阈值。
  uvcpp_web_app app;
  configure_for_test(app);
  check(app.set_loops(3) == 0, "set_loops(3) 成功");

  const int kConns = 16;

  // on_connection 里记下 (id 的高段, client->loop_index(), 本条连接线程身份)。
  std::mutex mu;
  std::vector<int> id_loop;      ///< `uvcpp_web_connection_registry::loop_of(id)`
  std::vector<int> client_loop;  ///< `client->loop_index()`
  std::map<int, std::thread::id> conn_tid;  ///< 循环号 → 该连接的回调所在线程

  app.on_connection([&](uvcpp_web_conn_id id, uvcpp_tcp_client* client) {
    const int a = uvcpp_web_connection_registry::loop_of(id);
    const int b = client != nullptr ? client->loop_index() : -1;
    std::lock_guard<std::mutex> lk(mu);
    id_loop.push_back(a);
    client_loop.push_back(b);
    conn_tid[b] = std::this_thread::get_id();
  });

  // 处理函数里记下自己的线程身份（判据 3 的正面）。
  std::mutex hmu;
  std::vector<std::thread::id> handler_tids;
  app.get("/who", [&](uvcpp_web_request&, uvcpp_web_response& resp,
                      uvcpp_web_next) {
    {
      std::lock_guard<std::mutex> lk(hmu);
      handler_tids.push_back(std::this_thread::get_id());
    }
    resp.text("ok");
    resp.end();
  });

  check(app.start() == 0, "start() 成功");
  const int port = app.bound_port();

  // **先问出"循环号 → 线程身份"**：3 格各投一个报身份的任务。这一步必须在
  // 发请求之前 —— 否则下面那些断言没有可比对的基准。
  std::vector<std::thread::id> slot_tid(3);
  require_within([&] { return ask_loop_threads(app, 3, slot_tid); }, 6000,
                 "3 格循环都报出了线程身份");
  check(slot_tid[0] != slot_tid[1] && slot_tid[1] != slot_tid[2] &&
            slot_tid[0] != slot_tid[2],
        "3 格循环各是一条**不同**的线程（post 到 0/1/2 落到三条线程上）");

  int ok = 0;
  for (int i = 0; i < kConns; ++i) {
    uvcpp_http_response r;
    if (get(port, "/who", r) && status_of(r) == 200) ++ok;
  }
  check(ok == kConns,
        "16 条连接全部答对（" + std::to_string(ok) + "/" +
            std::to_string(kConns) + "）");

  // 循环还在跑的时候读聚合量：只读每格的原子计数，不遍历任何 map。
  //
  // 下面那个和式**不能**当逐格计数对不对的判据用：它采样时手上有几条活连接
  // 是不受控的 —— 十六条请求已经发完，但回收是异步的，采样点上可能一条都不剩
  // （总数 0 ⇒ `0 == 0+0+0`，对坏实现照样成立），也可能还剩一条（那时才有牙）。
  // 实测两种都发生过：变异 M4 第一次跑**没抓住**（正好采到 0），第二次抓住了。
  // 判据**时红时绿**等于没判据。真正有牙的那条在
  // `test_aggregates_while_running()` 里 —— 采样点放进处理函数，连接必然活着，
  // 前提 `at1 >= 1` 自己先断言。
  const size_t total = app.connection_count();
  const size_t at0 = app.connection_count_at(0);
  const size_t at1 = app.connection_count_at(1);
  const size_t at2 = app.connection_count_at(2);
  check(at0 + at1 + at2 == total,
        "逐格之和 == connection_count()（" + std::to_string(at0) + "+" +
            std::to_string(at1) + "+" + std::to_string(at2) + " vs " +
            std::to_string(total) + "）");

  {
    std::lock_guard<std::mutex> lk(mu);

    int mismatch = 0;
    for (size_t i = 0; i < id_loop.size() && i < client_loop.size(); ++i) {
      if (id_loop[i] != client_loop[i]) ++mismatch;
    }
    check(mismatch == 0,
          "id 的高段 == client->loop_index()（不一致 " + std::to_string(mismatch) +
              " 条）");
    check(id_loop.size() == static_cast<size_t>(kConns),
          "16 条连接都进了 on_connection（" + std::to_string(id_loop.size()) +
              "）");

    int n1 = 0, n2 = 0, n0 = 0, nbad = 0;
    for (size_t i = 0; i < id_loop.size(); ++i) {
      if (id_loop[i] == 1) ++n1;
      else if (id_loop[i] == 2) ++n2;
      else if (id_loop[i] == 0) ++n0;
      else ++nbad;
    }
    // **形状有两条**（`is_fanout()`，运行时探测）：只有转手那条路上分布才是确定
    // 的（显式轮转 ⇒ {8,8}、0 号一条都不留）。内核分流下 0 号自己也收、分布由
    // 内核哈希定 ⇒ 那两条必然为假，换成的判据是"高段都落在 0..2 之内"（越界说明
    // 高段没带对）。至于"高段 == 连接自己那条循环"，上面已经逐条比过 —— **那条
    // 才是本体判据**，两条形状都成立。
    if (app.tcp_server() != nullptr && app.tcp_server()->is_fanout()) {
      check(nbad == 0, "分流下 16 条的高段都落在 0..2（越界 " +
                           std::to_string(nbad) + " 条；分布 " +
                           std::to_string(n0) + "/" + std::to_string(n1) + "/" +
                           std::to_string(n2) + "）");
    } else {
      check(n1 == kConns / 2 && n2 == kConns / 2,
            "显式轮转：16 条分给 2 条工作循环必然 {8,8}，实际 {" +
                std::to_string(n1) + "," + std::to_string(n2) + "}");
      check(n0 == 0, "0 号（接受者）一条都不留（转手是显式的）");
    }

    // 连接的回调必须就在它自己那条循环的线程上（与 post 问出来的那张表对齐）。
    for (std::map<int, std::thread::id>::const_iterator it = conn_tid.begin();
         it != conn_tid.end(); ++it) {
      const int idx = it->first;
      const bool in_range = idx >= 0 && idx < 3;
      check(in_range && it->second == slot_tid[static_cast<size_t>(idx)],
            "循环 " + std::to_string(idx) +
                " 的连接回调就在该循环的线程上（post 问出来的那张表）");
    }
  }

  {
    std::lock_guard<std::mutex> lk(hmu);
    size_t on1 = 0, on2 = 0, on0 = 0, other = 0;
    for (size_t i = 0; i < handler_tids.size(); ++i) {
      if (handler_tids[i] == slot_tid[1]) ++on1;
      else if (handler_tids[i] == slot_tid[2]) ++on2;
      else if (handler_tids[i] == slot_tid[0]) ++on0;
      else ++other;
    }
    check(handler_tids.size() == static_cast<size_t>(kConns),
          "16 次请求都记下了线程身份（" +
              std::to_string(handler_tids.size()) + "）");
    check(other == 0,
          "请求线程全部落在本服务端自己的循环上（不认识的线程有 " +
              std::to_string(other) + " 条）");
    if (app.tcp_server() != nullptr && app.tcp_server()->is_fanout()) {
      // 分流：0 号**自己也承载连接**，请求当然也跑在 0 号线程上 ⇒ "落在 0 号
      // 的必须是 0 条"与"必然 {8,8}"在分流下必然为假。承重的那条是上面那句
      // `other == 0`（请求没跑到不属于本服务端的线程上）＋每条连接"回调就在
      // 它自己那条循环的线程上"（上面逐格比过）。分布只打印出来看，不作判据。
      std::cout << "  [分流] 请求线程分布 0/1/2 = " << on0 << "/" << on1 << "/"
                << on2 << "（内核哈希定，不作判据）" << std::endl;
    } else {
      check(on0 == 0,
            "n>1 时请求**不**在 0 号循环的线程上处理（落在 0 号的有 " +
                std::to_string(on0) + " 条）");
      check(on1 == kConns / 2 && on2 == kConns / 2,
            "请求按连接的轮转分给两条工作循环，必然 {8,8}（实际 {" +
                std::to_string(on1) + "," + std::to_string(on2) + "}）");
    }
  }

  // 停机也走一遍扇出路径（3 条循环）：这条用例本身不判它，但"两条工作循环
  // 都能被停掉"是收尾不挂的前提。
  check(stop_and_join_within(app, 20000, "n=3 下 stop()+join() 返回"),
        "n=3 下 stop()+join() 在有界墙钟内返回");
}

// =========================================================================
// 5. 聚合量：跑着读，取值有界
// =========================================================================

void test_aggregates_while_running() {
  uvcpp_web_app app;
  configure_for_test(app);
  check(app.set_loops(2) == 0, "set_loops(2) 成功");

  // 异步收尾：中间件把 `next` **按值**带进工作线程，睡一会儿再放行 —— 于是
  // 请求真的在途，主线程才有机会在"跑着"的时候读数。
  //
  // **不**在工作线程里写 `resp`：写响应是 loop 线程的事（框架的契约，见
  // `web_app_app_func.cpp` 的 `test_async_from_worker_thread`）。线程用
  // `shared_ptr` 持有、发完请求 join 掉 —— 比 detach 干净，也不会出现"线程
  // 还活着但 App 已经析构"这种用例自身的竞态。
  std::mutex wmu;
  std::vector<std::shared_ptr<std::thread> > workers;
  std::atomic<int> resumed(0);

  app.use([&](uvcpp_web_request&, uvcpp_web_response&, uvcpp_web_next next) {
    std::shared_ptr<std::thread> t(new std::thread([next, &resumed]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      resumed.fetch_add(1);
      next();  // ← 在**别的线程**上放行：框架必须自己投回 loop 线程
    }));
    std::lock_guard<std::mutex> lk(wmu);
    workers.push_back(t);
  });

  app.get("/slow", [](uvcpp_web_request&, uvcpp_web_response& resp,
                      uvcpp_web_next) {
    resp.text("slow");
    resp.end();
  });

  // 逐格计数必须在**连接真的活着**的时刻读。原先那句「逐格之和 == 总数」写在
  // 16 条请求全跑完之后（另一条用例里），那时总数是 0 —— `0 == 0 + 0 + 0`
  // 对**坏实现**同样成立：把 `connection_count_at(i)` 改成一律返回 0 号分片的
  // 大小（不管 i），它照样绿。变异脚本 M4 就是这条，第一次跑**没抓住**。
  //
  // 采样点放进处理函数：那一刻本条连接一定已登记，而且就在它**自己那条**
  // 循环上（n=2 时工作循环只有 1 号，0 号是接受者、一条都不留）⇒ `at1 >= 1`
  // 是硬前提，那个和式于是不再恒真。
  std::mutex cmu;
  size_t c_total = 0, c_at0 = 0, c_at1 = 0;
  bool c_got = false;
  app.get("/counts", [&](uvcpp_web_request&, uvcpp_web_response& resp,
                         uvcpp_web_next) {
    const size_t t = app.connection_count();
    const size_t a0 = app.connection_count_at(0);
    const size_t a1 = app.connection_count_at(1);
    {
      std::lock_guard<std::mutex> lk(cmu);
      c_total = t;
      c_at0 = a0;
      c_at1 = a1;
      c_got = true;
    }
    resp.text("counts");
    resp.end();
  });

  check(app.start() == 0, "start() 成功");
  const int port = app.bound_port();

  // 一边发请求一边从主线程读：读的全是每格的原子计数。
  int ok = 0;
  size_t max_conn = 0;
  size_t max_inflight = 0;
  for (int i = 0; i < 8; ++i) {
    uvcpp_http_response r;
    if (get(port, "/slow", r) && status_of(r) == 200 && body_of(r) == "slow") {
      ++ok;
    }
    const size_t c = app.connection_count();
    const size_t f = app.inflight_count();
    if (c > max_conn) max_conn = c;
    if (f > max_inflight) max_inflight = f;
    // 顺手把逐格读也走一遍（越界下标不许炸）。
    (void)app.connection_count_at(0);
    (void)app.connection_count_at(1);
    (void)app.connection_count_at(9);
  }
  check(ok == 8, "8 条异步请求全部答对（" + std::to_string(ok) + "）");
  check(resumed.load() == 8,
        "8 次续跑都发生了（" + std::to_string(resumed.load()) + "）");
  check(max_conn <= 8, "读到的连接数在界内（峰值 " + std::to_string(max_conn) +
                           "，最多 8 条）");
  check(max_inflight <= 8, "读到的在途数在界内（峰值 " +
                               std::to_string(max_inflight) + "）");

  // 上面那两条是「跑着读不炸」的界，**不是**逐格计数对不对的判据。下面这条
  // 才是：它在连接活着的时刻采样，前提（总数 ≥ 1、1 号分片 ≥ 1）自己先断言。
  {
    uvcpp_http_response r;
    const bool okc = get(port, "/counts", r) && status_of(r) == 200 &&
                     body_of(r) == "counts";
    check(okc, "/counts 答对（逐格读数的采样点）");

    std::lock_guard<std::mutex> lk(cmu);
    check(c_got, "处理函数里取到了逐格读数（采样时连接是活的）");
    check(c_total >= 1, "采样时至少有 1 条连接活着（总数 " +
                            std::to_string(c_total) + "）");
    if (app.tcp_server() != nullptr && app.tcp_server()->is_fanout()) {
      // 分流：**采样那条连接自己可能就在 0 号**（0 号也承载连接）⇒ "一定记在
      // 1 号分片"不成立。承重的判据是"它被记进了**一**格"：逐格之和 == 总数，
      // 记两处（每格都回落 0 号那种）或漏记都会红 —— 下面那条。
      check(c_at0 + c_at1 == c_total && c_total >= 1,
            "采样的那条连接只被记在一格（0 号 " + std::to_string(c_at0) +
                " + 1 号 " + std::to_string(c_at1) + " == 总数 " +
                std::to_string(c_total) + "）");
    } else {
      check(c_at1 >= 1, "活着的连接记在 1 号分片上（实际 " +
                            std::to_string(c_at1) + "）");
    }
    check(c_at0 + c_at1 == c_total,
          "逐格之和 == 总数（" + std::to_string(c_at0) + "+" +
              std::to_string(c_at1) + " vs " + std::to_string(c_total) + "）");
  }

  // 先把工作线程收干净再停机：线程还活着的时候对象不能析构。
  {
    std::lock_guard<std::mutex> lk(wmu);
    for (size_t i = 0; i < workers.size(); ++i) {
      if (workers[i]->joinable()) workers[i]->join();
    }
  }

  check(stop_and_join_within(app, 15000, "stop()+join() 返回"),
        "stop()+join() 在有界墙钟内返回");
  check(app.connection_count() == 0, "停机后登记表回零");
  check(app.inflight_count() == 0, "停机后在途数回零");
}

// =========================================================================
// 6. WS：会话归 1 号循环，Close 帧从 1 号循环发出去
// =========================================================================

void test_ws_session_and_close_frame() {
  g_sink.clear();  // 只判本条留下的日志
  uvcpp_web_app app;
  configure_for_test(app);
  check(app.set_loops(2) == 0, "set_loops(2) 成功");

  std::atomic<int> opened(0);
  app.websocket("/ws", [&](uvcpp_web_ws_request& ws) {
    opened.fetch_add(1);
    uvcpp_ws_connection* c = ws.connection();
    if (c != nullptr) c->on_text([](const std::string&) {});
  });

  check(app.start() == 0, "start() 成功");
  const int port = app.bound_port();

  ws_probe p;
  // 消息要**先算好再断言**：`check(f(), msg)` 里两个实参的求值次序是未指定的，
  // 写成 `check(..., p.where)` 会在 `connect()` 跑之前就把 `where` 读走。
  const bool connected = p.connect(port);
  check(connected, std::string("WS 探针连上了（") + p.where + "）");
  check(p.start_reading(), "探针起了读");
  check(p.upgrade("/ws"), "升级请求写出去了");
  check(p.wait_response(), "等到了握手应答");
  check(p.upgraded, "服务端回了 101（实际 " + std::to_string(status_of_raw(p.resp)) +
                        "）");

  // 会话必须**落在连接自己那条循环**上：n=2 时工作循环只有 1 号，
  // 0 号是接受者、一条连接都不留。
  uvcpp_ws_server* wss = app.ws_server();
  check(wss != nullptr, "ws_server() 非空");
  if (wss != nullptr) {
    require_within([&] { return wss->session_count() == 1; }, 3000,
                   "服务端看到 1 个 WS 会话");
    if (app.tcp_server() != nullptr && app.tcp_server()->is_fanout()) {
      // 分流：WS 那条连接**可能落在 0 号** ⇒ "一定记在 1 号、0 号必须是空的"
      // 必然为假。承重的是"**只记在一格**"：逐格之和 == session_count()
      // （记两处、漏记都会红）。
      check(wss->session_count_at(0) + wss->session_count_at(1) ==
                wss->session_count(),
            "会话只记在一格（0 号 " + std::to_string(wss->session_count_at(0)) +
                " + 1 号 " + std::to_string(wss->session_count_at(1)) + " == " +
                std::to_string(wss->session_count()) + "）");
    } else {
      check(wss->session_count_at(1) == 1,
            "会话记在 **1 号**分片上（实际 1 号 = " +
                std::to_string(wss->session_count_at(1)) + "）");
      check(wss->session_count_at(0) == 0,
            "0 号分片是空的（实际 " + std::to_string(wss->session_count_at(0)) +
                "）");
    }
  }

  // ---- 停机：对端必须收到 Close 帧，而不是被直接断掉 ----
  app.stop();
  p.pump([&] { return p.close_frame_code() >= 0 || p.peer_closed; }, 5000);

  check(p.close_frame_code() >= 0,
        "对端收到的是 **Close 帧**，不是连接被直接断掉（RFC 6455 §7.1.4）");
  check(p.close_frame_code() == static_cast<int>(ws_close_code::GOING_AWAY),
        "Close 码是 1001 GOING_AWAY（实际 " +
            std::to_string(p.close_frame_code()) + "）");
  check(p.close_frame_count() == 1,
        "只收到 **1 条** Close 帧（重复发说明每进程动作没把门，实际 " +
            std::to_string(p.close_frame_count()) + "）");
  check(!p.parse_error, "帧格式合法（没有解析错误）");

  p.close_transport();

  check(stop_and_join_within(app, 15000, "stop() 之后 join() 返回"),
        "stop() 之后 join() 在有界墙钟内返回");

  check(g_sink.count_containing(kJoinIncomplete) == 0,
        "WS 停机也没留下「有循环没退出」的 ERROR（实际：" +
            g_sink.first_containing(kJoinIncomplete) + "）");

  // 会话确实被回收了（活动归零 + 已回收数至少 1）。
  check(wss != nullptr && wss->session_count() == 0, "停机后活动会话归零");
  check(wss != nullptr && wss->recycled_session_count() >= 1,
        "停机后已回收会话数 ≥ 1（实际 " +
            std::to_string(wss != nullptr ? wss->recycled_session_count() : 0) +
            "）");
  check(opened.load() == 1, "WS 路由被命中了 1 次");
}

// =========================================================================
// 7. 静态缓存：两条循环共享一份，且计数自洽
// =========================================================================

static const char* kStaticDir = "uvcpp_multiloop_tmp";
static const char* kStaticBody = "body-of-app-js-0123456789";

/// 把 `app.js` 写成 `body`。**整写**（`trunc`），所以长度变了短的那次也会真的变短。
static bool write_static_body(const std::string& body) {
  std::ofstream f((std::string(kStaticDir) + "/app.js").c_str(),
                  std::ios::binary | std::ios::trunc);
  if (!f.good()) return false;
  f.write(body.data(), static_cast<std::streamsize>(body.size()));
  f.close();
  return true;
}

static bool prepare_static_fs() {
#ifdef _WIN32
  _mkdir(kStaticDir);
#else
  mkdir(kStaticDir, 0755);
#endif
  return write_static_body(kStaticBody);
}

static void cleanup_static_fs() {
  std::remove((std::string(kStaticDir) + "/app.js").c_str());
#ifdef _WIN32
  _rmdir(kStaticDir);
#else
  rmdir(kStaticDir);
#endif
}

void test_static_cache_two_loops() {
  if (!prepare_static_fs()) {
    check(false, "建不出静态临时目录");
    return;
  }

  uvcpp_web_app app;
  configure_for_test(app);
  // n=3 ⇒ 两条工作循环：第 i 条连接落 `1 + i % 2`，所以偶数条落 1 号、奇数条
  // 落 2 号 —— 同一个静态路径真的会被**两条循环**同时服务。
  check(app.set_loops(3) == 0, "set_loops(3) 成功");

  uvcpp_web_static_options opts;
  opts.cache_max_entries = 8;
  opts.cache_max_bytes = 1u * 1024u * 1024u;
  opts.max_cached_file_size = 1u * 1024u * 1024u;
  std::shared_ptr<uvcpp_web_static> st =
      app.serve_static("/s", kStaticDir, opts);
  check(st != nullptr, "serve_static 拿到了对象");

  check(app.start() == 0, "start() 成功");
  const int port = app.bound_port();

  const int kReqs = 12;
  int ok = 0;
  for (int i = 0; i < kReqs; ++i) {
    uvcpp_http_response r;
    if (get(port, "/s/app.js", r) && status_of(r) == 200) {
      if (body_of(r) == kStaticBody) ++ok;
    }
  }
  check(ok == kReqs,
        "12 次静态请求全部拿到正文字节（" + std::to_string(ok) + "/" +
            std::to_string(kReqs) + "）");

  if (st) {
    // **缓存是共享一份的**：不管请求落在那条循环上，同一个键只有一条。
    check(st->cache_entries() == 1,
          "缓存只有 1 条（实际 " + std::to_string(st->cache_entries()) + "）");
    // 并发 `put()` 的签名：同一个键被插两次时 `bytes` 会被加两次。
    check(st->cache_bytes() == std::strlen(kStaticBody),
          "cache_bytes() 恰好等于文件长度（实际 " +
              std::to_string(st->cache_bytes()) + "，期望 " +
              std::to_string(std::strlen(kStaticBody)) + "）");
    // 命中/未命中覆盖了每一次真的拿到字节的请求：丢更新说明 `++` 没被护住。
    const unsigned long long sum = st->cache_hits() + st->cache_misses();
    check(sum == static_cast<unsigned long long>(kReqs),
          "hits + misses == 请求数（" + std::to_string(st->cache_hits()) +
              " + " + std::to_string(st->cache_misses()) + " vs " +
              std::to_string(kReqs) + "）");
    check(st->cache_hits() >= 1, "缓存真的命中过（hits = " +
                                     std::to_string(st->cache_hits()) + "）");
  }

  check(stop_and_join_within(app, 20000, "stop()+join() 返回"),
        "stop()+join() 在有界墙钟内返回");

  cleanup_static_fs();
}

// =========================================================================
// 7b. 静态缓存：同一个键**重插**时的字节账
// =========================================================================

/**
 * @brief 缓存里已经有一条同键的记录时，再 `put()` 一次，`cache_bytes()` 必须
 *        等于**当前**文件长度，不是两份之和。
 *
 * **为什么要单独一条**：`put()` 只在**未命中**那条分支上被调
 * （`src/webapp/uvcpp_web_static.cpp:1150-1152`），所以"键已在缓存里、又插一次"
 * 只有一条路走得到 —— **文件变了**（size/mtime 与快照对不上 ⇒ 再次未命中 ⇒
 * 带着同一个 key 再 `put()`）。上面那条 12 次请求的用例是"1 次未命中 + 11 次
 * 命中"，**一步都进不了** `put()` 里那段"先把旧字节扣掉"。
 *
 * 这不是推演出来的：`tests/tools/multiloop_mutation.py` 的 M5 把
 * `bytes -= old->second.data->size();` 整句删掉，那条 12 次请求的用例**照样全绿**
 * （同步 dll 3 处 → **没抓住**）。缺口是先被变异证明、才补的这条。
 */
void test_static_cache_reput_bytes() {
  if (!prepare_static_fs()) {
    check(false, "建不出静态临时目录");
    return;
  }

  uvcpp_web_app app;
  configure_for_test(app);
  check(app.set_loops(2) == 0, "set_loops(2) 成功");

  uvcpp_web_static_options opts;
  opts.cache_max_entries = 8;
  opts.cache_max_bytes = 1u * 1024u * 1024u;
  opts.max_cached_file_size = 1u * 1024u * 1024u;
  std::shared_ptr<uvcpp_web_static> st =
      app.serve_static("/s", kStaticDir, opts);
  check(st != nullptr, "serve_static 拿到了对象");

  check(app.start() == 0, "start() 成功");
  const int port = app.bound_port();

  // 第一轮：未命中 ⇒ 真的 `put()` 一次。
  {
    uvcpp_http_response r;
    check(get(port, "/s/app.js", r) && status_of(r) == 200 &&
              body_of(r) == kStaticBody,
          "第一轮拿到原正文");
  }
  if (st) {
    check(st->cache_bytes() == std::strlen(kStaticBody),
          "第一轮 cache_bytes() == 原文件长度（实际 " +
              std::to_string(st->cache_bytes()) + "，期望 " +
              std::to_string(std::strlen(kStaticBody)) + "）");
  }

  // 换一份**长度不同**的正文。长度不同是这条用例的前提：命中判定比的是
  // `size` + mtime（`src/webapp/uvcpp_web_static.cpp:892-900`），长度变了就一定
  // 对不上 ⇒ 必定再次未命中 —— **不依赖 mtime 的分辨率**（同一秒内改文件在
  // 有些文件系统上 mtime 不变，那样这条用例就会静默退化成"其实还是命中"）。
  const std::string kSecond = "second-body-of-app-js-and-it-is-longer-0123456789";
  check(kSecond.size() != std::strlen(kStaticBody),
        "两份正文长度不同（下面那条断言的前提）");
  check(write_static_body(kSecond), "改写静态文件成功");

  {
    uvcpp_http_response r;
    check(get(port, "/s/app.js", r) && status_of(r) == 200 &&
              body_of(r) == kSecond,
          "第二轮拿到**新**正文（证明真的又未命中、又 put 了一次）");
  }
  if (st) {
    check(st->cache_entries() == 1,
          "重插之后缓存仍然只有 1 条（实际 " +
              std::to_string(st->cache_entries()) + "）");
    check(st->cache_bytes() == kSecond.size(),
          "重插之后 cache_bytes() == **新**文件长度，不是两份之和（实际 " +
              std::to_string(st->cache_bytes()) + "，期望 " +
              std::to_string(kSecond.size()) + "）");
  }

  check(stop_and_join_within(app, 20000, "stop()+join() 返回"),
        "stop()+join() 在有界墙钟内返回");

  cleanup_static_fs();
}

// =========================================================================
// 8. 对照组：n == 1 逐字不变
// =========================================================================

void test_control_group_n1() {
  uvcpp_web_app app;
  configure_for_test(app);
  // **不调 set_loops()** —— 这就是"没有这个 API 时"的那条路。
  check(app.loop_count() == 1, "loop_count() == 1");

  std::mutex hmu;
  std::thread::id handler_tid;
  bool handler_ran = false;
  app.get("/c", [&](uvcpp_web_request&, uvcpp_web_response& resp,
                    uvcpp_web_next) {
    {
      std::lock_guard<std::mutex> lk(hmu);
      handler_tid = std::this_thread::get_id();
      handler_ran = true;
    }
    resp.text("c");
    resp.end();
  });

  check(app.start() == 0, "start() 成功");
  const int port = app.bound_port();

  // n=1 时"循环 0 是哪条线程"就是**调用 start() 的这条线程**（`run(md)` 不算，
  // 那条路就地跑）。这里仍走 `ask_loop_threads` 问一遍，免得把"恰好等于主线程"
  // 这个当前实现细节写成断言。
  std::vector<std::thread::id> slot_tid(1);
  require_within([&] { return ask_loop_threads(app, 1, slot_tid); }, 6000,
                 "n=1 时 0 号循环报出了线程身份");

  uvcpp_http_response r;
  check(get(port, "/c", r) && status_of(r) == 200, "n=1 下请求正常");
  {
    std::lock_guard<std::mutex> lk(hmu);
    check(handler_ran, "n=1 时处理函数跑过");
    check(handler_tid == slot_tid[0],
          "n=1 时请求就在 0 号循环的线程上（没有工作循环）");
  }
  check(app.connection_count_at(0) == app.connection_count(),
        "n=1 时全部连接都在 0 号分片上");
  check(app.connection_count_at(1) == 0, "n=1 时 1 号分片不存在/为空");

  check(stop_and_join_within(app, 15000, "n=1 下 stop()+join() 返回"),
        "n=1 下 stop()+join() 在有界墙钟内返回");
}

/** @brief 请求侧看一眼 id 的高段（判据 4 的另一半：id 真的带高段）。 */
void test_id_high_bits_from_loop() {
  uvcpp_web_app app;
  configure_for_test(app);
  check(app.set_loops(2) == 0, "set_loops(2) 成功");

  std::atomic<unsigned long long> seen_id(0);
  std::atomic<int> id_loop(-1);
  app.on_connection([&](uvcpp_web_conn_id id, uvcpp_tcp_client*) {
    seen_id.store(static_cast<unsigned long long>(id));
    id_loop.store(uvcpp_web_connection_registry::loop_of(id));
  });
  app.get("/z", [](uvcpp_web_request&, uvcpp_web_response& resp,
                   uvcpp_web_next) { resp.text("z"); resp.end(); });

  check(app.start() == 0, "start() 成功");
  const int port = app.bound_port();

  uvcpp_http_response r;
  check(get(port, "/z", r) && status_of(r) == 200, "请求正常");
  check(seen_id.load() != 0, "on_connection 拿到了 id");
  if (app.tcp_server() != nullptr && app.tcp_server()->is_fanout()) {
    // 分流：这一条连接可能被内核分给 0 号或 1 号 ⇒ 高段是 0 还是 1 都合法。
    // 能断言的是"高段是个合法循环号"（每循环一张表，越界就落到 0 号兜底那种
    // 静默错配 —— 那正是这条判据要挡的形状）。
    const int il = id_loop.load();
    check(il >= 0 && il < 2,
          "n=2 时 id 的高段是合法循环号（实际 " + std::to_string(il) + "）");
  } else {
    check(id_loop.load() == 1,
          "n=2 时 id 的高段是 1（实际 " + std::to_string(id_loop.load()) + "）");
  }
  // 低 44 位还是那份递增号：n=2 下第一条连接的递增号必须是 1。
  check((seen_id.load() & UVCPP_WEB_CONN_SEQ_MASK) == 1,
        "低 44 位是这条循环自己的递增号，从 1 起（实际 " +
            std::to_string(seen_id.load() & UVCPP_WEB_CONN_SEQ_MASK) + "）");

  check(stop_and_join_within(app, 15000, "stop()+join() 返回"),
        "stop()+join() 在有界墙钟内返回");
}

}  // namespace

/**
 * @brief 一个**绑不上**的地址：TEST-NET-1，永远不会配在网卡上。
 *
 * 失败用"地址绑不上"制造，**不用"端口被占"** —— 本仓实测 Windows 上
 * `SO_REUSEADDR` 两边都设时会**共存**（见 testing-guide 那条），拿端口占用
 * 去逼一个 bind 失败在这条腿上根本逼不出来，用例会假绿。
 */
static const char* kUnbindableHost = "192.0.2.1";

void test_start_failure_reclaims_workers() {
  // 判据 7：n>1 启动失败之后**不许留下在跑的工作循环**。
  //
  // 未改的代码怎么红：n-1 条工作循环在 `init_process_once()` 里就被放行了，
  // 钩子里已经 `note_loop_started()` 过；而 `bind()`/`listen()` 的失败分支
  // **只 `return rc`**，一条收尾都没有。于是 `loop_started_` 停在"真"、
  // 工作线程还在跑 —— 而 `start()` 的注释写着"失败时不会有线程残留，可以
  // 直接改配置重试"。用户手上剩下的是一句空话。
  {
    uvcpp_web_app app;
    configure_for_test(app);
    app.set_host(kUnbindableHost);
    check(app.set_loops(2) == 0, "n=2 被接受");

    const int rc = app.start();
    check(rc != 0, "绑不上的地址 ⇒ start() 返回非 0");
    check(rc != UV_EBUSY, "失败码是 bind 那个，不是 UV_EBUSY");
    std::cout << "     （bind 失败码 " << rc << "：" << uv_strerror(rc) << "）"
              << std::endl;

    // **本条用例真正的红面是下面这一句**：`loop_started_` 正是
    // `teardown_failed_start()` 回滚的那个量，未收尾的实现在这里为真。
    check(!app.loop_started(),
          "启动失败之后 loop_started() 为假 —— 工作循环已经收回");

    // 这一句在**改与不改上都绿**（`running_` 只在 `rc == 0` 之后才置位，见
    // `start_background()` 的线程尾巴），所以它不是判据，只是把"这个对象现在
    // 不该自称在服务"写下来。别把它的绿当成收尾生效的证据。
    check(!app.running(), "启动失败之后 running() 为假");

    // 终态契约：再 `start()` 要被**挡下来**，而不是"看起来能起来、实际把新
    // 句柄泄漏一路"。注意这一句**单独不足以**证明回滚做了事 —— 不回滚的实现
    // 同样返 `UV_EBUSY`（撞在 `loop_started_` 上，理由是"还在跑"）；上面那句
    // 才是把两条路分开的判据，这一句钉的是**报出来的原因**别退化成另一种。
    check(app.start() == UV_EBUSY, "失败之后本实例不能再 start()（终态）");
  }  // 析构在这里跑。收尾没做的话，它就是在几条**还活着**的循环上动句柄
     // （`delete slot.async` 会走 `uv_close`）—— 那正是 `tcp->stop()` 被钉在
     // 0 号时列出的同一条理由，只是从析构这条门进来。

  // 进程级不许有残留：同样 n=2 的新实例，换个能绑的地址必须起得来、停得掉。
  // 没有这一条的话，"失败之后什么都没坏"就只能靠上面那两条断言去代表，
  // 而它们只看得到这一个对象自己的账。
  uvcpp_web_app app2;
  configure_for_test(app2);
  app2.set_loops(2);
  app2.get("/hi", [](uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next) { resp.text("hi"); resp.end(); });
  check(app2.start() == 0, "失败一次之后，新实例照常起得来");
  const int port = app2.bound_port();
  check(port > 0, "新实例的 bound_port() 是真端口");
  uvcpp_http_response r;
  check(get(port, "/hi", r) && status_of(r) == 200, "新实例照常答 200");
  check(stop_and_join_within(app2, 15000, "失败一次之后新实例仍停得掉"),
        "新实例 stop()+join() 在有界墙钟内返回");
}

void test_worker_init_failure_is_not_silent() {
  // 判据 8（不变量）：**`start()` 返回 0 ⇒ 停机必须干净**。
  //
  // 它守的是"钩子把 `init_loop_state()` 的返回码丢了"那一条：某一格建 `async`
  // 失败时，那一格身份记了、`loop_started_` 也置真，但它的 `post()` 全被丢弃
  // （WARN，不是就地执行）⇒ 它**永远进不了停机状态机** ⇒ 停机会退化成 `join()`
  // 那条有界兜底：等满 grace+slack 之后报「仍有 N 条循环没退出」。而启动
  // **返回 0**，用户看不出任何异常。
  //
  // **诚实说明**：这条路径在进程内**没有自然触发器**（`uv_async_init` 只在
  // OOM / 非法循环上失败），所以本用例在**未变异**的树上恒真 —— 它是给变异用
  // 的观测面，不是"验过了"的证据。红面由 `multiloop_mutation.py` 的 M8
  // （注入 1 号格初始化失败 **+** 拿掉 `init_rc` 那道检查）制造，M7（只注入）
  // 是它的对照组：那时 `start()` 如实返非 0，本条走下面那条早退支。
  uvcpp_web_app app;
  configure_for_test(app);
  app.set_loops(2);
  app.get("/hi", [](uvcpp_web_request&, uvcpp_web_response& resp,
                    uvcpp_web_next) { resp.text("hi"); resp.end(); });

  g_sink.clear();
  const int rc = app.start();
  if (rc != 0) {
    // 初始化失败（注入的或真的）⇒ 如实报出来 + 收尾。这一支的正面判据在
    // `test_start_failure_reclaims_workers()` 里，这里只核收尾。
    check(!app.loop_started(), "某一格初始化失败 ⇒ start() 报错且循环已收回");
    return;
  }

  const int port = app.bound_port();
  uvcpp_http_response r;
  check(get(port, "/hi", r) && status_of(r) == 200, "n=2 下 GET /hi 是 200");
  check(stop_and_join_within(app, 15000, "启动成功的实例必须停得掉"),
        "start() 返 0 之后 stop()+join() 在有界墙钟内返回");

  // **有牙的是这一条**：`join()` 那条有界兜底只在"有循环没退出"时打，而
  // "join 在 15 秒内返回"在坏实现上照样绿（8 秒 < 15 秒上限）—— 与 M1 的红法
  // 是同一个道理。没有这一句，上面那句就是本仓最反对的"跑了但恒绿"。
  check(g_sink.count_containing(kJoinIncomplete) == 0,
        "启动成功 ⇒ 停机干净（日志里没有「条循环没退出」）");
}

int main() {
  std::cout << std::unitbuf;  // 崩溃时也能从最后一行看出死在哪一条
  std::cout << "== web_app_multiloop_func ==" << std::endl;
  install_capture_sink();  // 判据 2 的确定性观测面，必须在起任何 App 之前装

  std::cout << "-- set_loops 契约 --" << std::endl;
  test_set_loops_contract();
  std::cout << "-- 底层已是多循环时被拒 --" << std::endl;
  test_tcp_already_multiloop_is_rejected();
  std::cout << "-- 停机扇出（主判据）--" << std::endl;
  test_stop_fans_out_to_workers();
  std::cout << "-- 循环身份与 id 高段 --" << std::endl;
  test_loop_identity_and_ids();
  std::cout << "-- id 高段（单连接）--" << std::endl;
  test_id_high_bits_from_loop();
  std::cout << "-- 跑着读聚合量 --" << std::endl;
  test_aggregates_while_running();
  std::cout << "-- WS 会话归属与停机 Close 帧 --" << std::endl;
  test_ws_session_and_close_frame();
  std::cout << "-- 静态缓存（两条循环共享）--" << std::endl;
  test_static_cache_two_loops();
  std::cout << "-- 静态缓存：重插同一个键时的字节账 --" << std::endl;
  test_static_cache_reput_bytes();
  std::cout << "-- 启动失败要收回工作循环 --" << std::endl;
  test_start_failure_reclaims_workers();
  std::cout << "-- 启动成功 ⇒ 停机干净（初始化失败的观测面）--" << std::endl;
  test_worker_init_failure_is_not_silent();
  std::cout << "-- 对照组 n == 1 --" << std::endl;
  test_control_group_n1();

  if (g_failures != 0) {
    std::cerr << "web_app_multiloop_func: " << g_failures << " 条判据红了"
              << std::endl;
    return 1;
  }
  std::cout << "web_app_multiloop_func: 全绿" << std::endl;
  return 0;
}

#endif  // UVCPP_WEBAPP_ENABLE
