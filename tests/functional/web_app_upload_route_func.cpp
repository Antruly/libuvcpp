/**
 * @file tests/functional/web_app_upload_route_func.cpp
 * @brief Phase 3b 步骤 4 —— `app.upload_route()` 接线 + `req.upload()` 端到端。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一步测的**不是**落盘本身（那是 `web_app_upload_func.cpp` 的地界，它直接喂
 * `uvcpp_web_multipart` → `uvcpp_web_upload`，完全不经过 web 层），而是**接线**：
 * 一条 HTTP 报文从真端口进来之后，框架有没有把 multipart 解析器和落盘会话挂到
 * 这条流上，以及 `req.upload()` 在用户读到它的那一刻**是不是已经填好了**。
 *
 * 为什么必须真端口真连接
 * ----------------------
 * 本步的两个机制在"直接调函数"的形状下根本不存在：
 *
 *   1. **框架槽先于用户槽**（`uvcpp_web_stream_hooks`）。用户在 handler 里调
 *      `stream->on_end(...)` 会不会把框架那一份顶掉，只有真的走一遍
 *      `on_end` 才知道。
 *   2. **用户 `on_end` 被扣住到落盘完成**。落盘是异步的（线程池），所以
 *      "用户在 `on_end` 里读 `req.upload()`"这件事天然是竞态的 —— 而这正是
 *      本文件每一条断言的前提。打桩的 fs 里没有"在途"这个状态，测不出来。
 *
 * 判据的形状：**线上报文 + 盘上文件**，两条独立的读路径
 * ---------------------------------------------------
 * 每个用例都同时看两边：
 *   - 线上：状态码、响应个数、响应体（响应体是 handler 从 `req.upload()` 拼出来的
 *     摘要，所以它非空就等于"用户在 `on_end` 里读到了完整结果"）；
 *   - 盘上：用 `std::ifstream`（与被测路径无关的读路径）把文件读回来逐字节比对。
 * 只看线上会漏掉"响应对了但盘上什么都没写"，只看盘上会漏掉"文件写对了但用户
 * 读不到结果"。两边都看，才排得掉"文件根本没写成"这类蒙混过关。
 *
 * 命名同时命中 `web_.*\.cpp$` 与 `web_app_.*\.cpp$` 两条过滤器，所以「web 关」
 * 和「webapp 关」两种配置下都会被正确摘掉（`tests/functional/CMakeLists.txt`
 * 里那段注释讲了：名字起错会让用例在 `UVCPP_BUILD_WEBAPP=OFF` 时留下来，然后
 * **链接失败**）。
 *
 * 本步**不做**的事（都留给后面的步骤）
 * -----------------------------------
 * 尺寸上限与截断（步骤 6）、文件名清洗（步骤 7）、普通路由上的缓冲 multipart
 * （步骤 8）。所以这里**没有**一条断言依赖那些行为 —— `filename_metadata`
 * 反而钉的是"此刻原样转发、不清洗"。
 *
 * 步骤 5（背压接线）的用例也加在这个文件里（`flow_backpressure_wired`），因为
 * 它是同一件东西的另一半：`web_app_upload_func.cpp` 证明会话本身会背压，而
 * "框架在真端口上把背压通道接到了这条流上"只有走一遍 `wire_upload()` 才知道。
 *
 * 跑法：`test_web_app_upload_route_func.exe [子串过滤]`
 */

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

// 目录操作要用系统调用：`std::filesystem` 是 C++17，而本库锁在 C++11。
// 这两个头必须放在**文件顶部**，不能塞进匿名命名空间里（在 namespace 里
// include 系统头会把它们的声明拖进那个 namespace）。
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "net/uvcpp_net_read.h"
#include "net/uvcpp_tcp_client.h"
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_handler.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_stream.h>
#include <webapp/uvcpp_web_upload.h>
// 只为 `web_real_path()` —— `main()` 要把上传目录换成真实路径，好和框架交出来的
// `file.path()` 用同一个基准比对（见那里的注释）。
#include <webapp/uvcpp_web_util.h>

#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_eq_s(const std::string& got, const std::string& want,
                const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: [" << want
              << "]\n         实际: [" << got << "]" << std::endl;
    ++g_failures;
  }
}

/// 失败时只报"第几个字节不同"，不把整个载荷打进日志（1 MiB 的失败信息会让
/// ctest 的输出没法看）。
void check_bytes(const std::string& got, const std::string& want,
                 const std::string& what) {
  if (got.size() != want.size()) {
    std::cerr << "  [FAIL] " << what << "\n         期望长度: " << want.size()
              << "\n         实际长度: " << got.size() << std::endl;
    ++g_failures;
    return;
  }
  for (size_t i = 0; i < got.size(); ++i) {
    if (got[i] != want[i]) {
      std::cerr << "  [FAIL] " << what << "\n         第 " << i
                << " 个字节不同：期望 "
                << static_cast<int>(static_cast<unsigned char>(want[i]))
                << "，实际 "
                << static_cast<int>(static_cast<unsigned char>(got[i]))
                << std::endl;
      ++g_failures;
      return;
    }
  }
}

void check_contains(const std::string& hay, const std::string& needle,
                    const std::string& what) {
  if (hay.find(needle) == std::string::npos) {
    std::cerr << "  [FAIL] " << what << "\n         应在其中找到: [" << needle
              << "]\n         实际: [" << hay << "]" << std::endl;
    ++g_failures;
  }
}

/** @brief 轮询等待一个条件成立，最长 timeout_ms 毫秒。 */
template <typename Pred>
bool wait_for(Pred pred, int timeout_ms) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// =========================================================================
// 目录工具
//
// 用**同步**的 `uv_fs_scandir` 和 `std::ifstream`，而且只在循环没在跑的时候调。
// 生产代码里不能这么写 —— 这里可以，而且是**判据的一部分**：只有用一条跟被测
// 路径完全无关的读路径去数文件、去读字节，才能证明"框架真的写/删了"，
// 而不是"框架说它写了"。
//
// 循环用 `uv_default_loop()`：同步 fs 调用只需要一个"句柄容器"，不需要它跑起来。
// 用 App 的循环做不到（它在后台线程上转），而为此单独建一个 `uvcpp_loop` 等于
// 让进程多一个永远不关的 loop。
// =========================================================================

std::vector<std::string> list_dir(const std::string& path) {
  std::vector<std::string> out;
  uv_fs_t req;
  const int n = uv_fs_scandir(uv_default_loop(), &req, path.c_str(), 0, nullptr);
  if (n >= 0) {
    uv_dirent_t ent;
    while (uv_fs_scandir_next(&req, &ent) != UV_EOF) {
      out.push_back(std::string(ent.name));
    }
  }
  uv_fs_req_cleanup(&req);
  return out;
}

void purge_dir(const std::string& path) {
  const std::vector<std::string> names = list_dir(path);
  for (size_t i = 0; i < names.size(); ++i) {
    std::remove((path + "/" + names[i]).c_str());
  }
}

std::string read_all(const std::string& path) {
  std::string out;
  std::ifstream f(path.c_str(), std::ios::binary);
  if (!f) return out;
  out.assign((std::istreambuf_iterator<char>(f)),
             std::istreambuf_iterator<char>());
  return out;
}

#ifdef _WIN32
#define UPR_MKDIR(p) _mkdir(p)
#else
#define UPR_MKDIR(p) mkdir((p), 0755)
#endif

bool ensure_dir(const std::string& p) {
  if (UPR_MKDIR(p.c_str()) == 0) return true;
  // 已存在也算成功：跑挂一次留下的旧目录不该让后续每一次运行都起不来。
  return errno == EEXIST;
}

// =========================================================================
// 载荷与报文构造
// =========================================================================

/// 每个位置的值与下标有关，于是"偏移错"和"长度截断"都会表现为内容不等，
/// 而不是碰巧相等。刻意钉上 NUL / 0xFF / CRLF。
std::string make_payload(size_t n, unsigned seed) {
  std::string s;
  s.resize(n);
  for (size_t i = 0; i < n; ++i) {
    s[i] = static_cast<char>((i * 31u + seed * 17u + (i >> 8)) & 0xFF);
  }
  if (n > 0) s[0] = '\0';
  if (n > 1) s[1] = static_cast<char>(0xFF);
  if (n > 3) {
    s[2] = '\r';
    s[3] = '\n';
  }
  if (n > 5) s[n - 1] = static_cast<char>(0xFE);
  return s;
}

const char* kBoundary = "uvcppRouteBoundary4c81";

std::string file_part(const std::string& name, const std::string& filename,
                      const std::string& mime, const std::string& data) {
  return "Content-Disposition: form-data; name=\"" + name +
         "\"; filename=\"" + filename + "\"\r\nContent-Type: " + mime +
         "\r\n\r\n" + data;
}

std::string field_part(const std::string& name, const std::string& value) {
  return "Content-Disposition: form-data; name=\"" + name +
         "\"\r\n\r\n" + value;
}

std::string mp_body(const std::string& boundary,
                    const std::vector<std::string>& parts) {
  std::string b;
  for (size_t i = 0; i < parts.size(); ++i) {
    b += "--" + boundary + "\r\n";
    b += parts[i];
    b += "\r\n";
  }
  b += "--" + boundary + "--\r\n";
  return b;
}

/// 一次 POST 的报文头。`quoted` 控制 boundary 参数带不带引号 —— 两条路都要走，
/// 因为 `web_multipart_boundary()` 对引号的处理是单独一段代码。
std::string mp_head(const std::string& path, const std::string& boundary,
                    size_t len, const std::string& content_type,
                    bool quoted = false) {
  std::string ct = content_type;
  if (content_type == "multipart/form-data") {
    ct += "; boundary=";
    ct += quoted ? ("\"" + boundary + "\"") : boundary;
  }
  std::string h = "POST " + path + " HTTP/1.1\r\nHost: t\r\n";
  h += "content-length: " + std::to_string(len) + "\r\n";
  h += "content-type: " + ct + "\r\n";
  h += "\r\n";
  return h;
}

/** @brief 收到的字节里 "HTTP/1.1 " 出现了几次（用来数响应个数）。 */
int count_responses(const std::string& wire) {
  int n = 0;
  size_t p = 0;
  while ((p = wire.find("HTTP/1.1 ", p)) != std::string::npos) {
    ++n;
    p += 9;
  }
  return n;
}

// =========================================================================
// 分阶段裸客户端 —— 从 `web_app_stream_func.cpp` 沿用同一形状
//
// 连接与它的 loop 都在**本线程**上（App 的循环在它自己的后台线程），
// 所以下面的标志不需要原子：回调都是 `loop_->run()` 在我们自己的栈上跑出来的。
// =========================================================================

class staged_conn {
 public:
  staged_conn()
      : loop_(nullptr), connected_(false), written_(false), closed_(false) {}

  bool open(int port, int timeout_ms = 3000) {
    int rc = c_.connect("127.0.0.1", port, [this](int st) {
      if (st != 0) return;
      connected_ = true;
      // 起读必须在连上之后（socket 还没建立时装回调会静默收不到任何字节）。
      c_.read_start_events([this](uvcpp_tcp_client&, const net_read_result& r) {
        if (r.event == net_read_event::DATA && r.size > 0 && r.data != nullptr) {
          rx_.append(r.data, r.size);
        } else if (r.event == net_read_event::PEER_CLOSED ||
                   r.event == net_read_event::READ_ERROR) {
          closed_ = true;
        }
      });
    });
    if (rc != 0) return false;
    loop_ = c_.get_loop();
    return pump_until([this]() { return connected_; }, timeout_ms);
  }

  /**
   * @brief 写一段字节，泵到写完成。
   *
   * **必须等写完成，且失败要当回事**：`uvcpp_tcp_client` 同一时刻只允许一个
   * 异步写，撞上在途写会返回 `UV_EALREADY` 并**把这一块丢掉**。不检查的话
   * "分 5 次写入"实际会变成"只写了第一次"，而用例照样可能绿。
   */
  bool write(const std::string& s, int timeout_ms = 3000) {
    if (s.empty()) return true;
    written_ = false;
    int rc = c_.write(s.data(), s.size(), [this](int) { written_ = true; });
    if (rc != 0) return false;
    return pump_until([this]() { return written_; }, timeout_ms);
  }

  /// 上限是**墙钟**毫秒，不是圈数：一圈的代价就是系统定时器粒度（Windows 无
  /// 请求者时默认 15.625 ms），按圈数计时在粗粒度机器上会整体放大约 8 倍。
  void pump(int ms) { uvcpp_test::pump_for(loop_, ms); }

  template <typename Pred>
  bool pump_until(Pred pred, int timeout_ms) {
    return uvcpp_test::wait_until(loop_, pred, timeout_ms);
  }

  const std::string& rx() const { return rx_; }
  bool peer_closed() const { return closed_; }

  ~staged_conn() {
    if (loop_ == nullptr) return;
    if (c_.get_tcp() == nullptr) return;
    bool done = false;
    c_.get_tcp()->close([&done](uvcpp_handle*) { done = true; });
    for (int i = 0; i < 200 && !done; ++i) loop_->run(UV_RUN_NOWAIT);
  }

 private:
  uvcpp_tcp_client c_;
  uvcpp_loop*      loop_;
  std::string      rx_;
  bool             connected_;
  bool             written_;
  bool             closed_;
};

// =========================================================================
// 探针：handler 里能观察到的一切
//
// 全部字段由 handler / `on_end` 在 **loop 线程**上写，由测试线程在
// `pump_until()` 之后读 —— 中间隔着 `loop_->run()` 的墙，所以不需要原子。
// =========================================================================

struct up_probe {
  up_probe()
      : handler_called(0),
        upload_at_handler(false),
        ended(0),
        aborted(0),
        upload_null(true),
        files(0),
        fields(0),
        file_list(),
        field_list(),
        summary() {}

  int         handler_called;
  /// handler **当场**（headers 时机）读 `req.upload()` 是不是非空。
  /// 契约说必须是假 —— 结果要到 `on_end` 才存在。
  bool        upload_at_handler;
  int         ended;
  int         aborted;
  bool        upload_null;
  size_t      files;
  size_t      fields;
  /// **拷出来**的副本：`req.upload()` 返回的指针生存期只到 `on_end` 返回为止。
  std::vector<uvcpp_web_upload_file> file_list;
  std::vector<uvcpp_web_upload_field> field_list;
  /// handler 从 `req.upload()` 拼出来的摘要，作为响应体发回去。
  std::string summary;
};

/** @brief 把结果拼成一行一项的摘要 —— 测试直接对这个串做子串断言。 */
std::string describe_upload(const uvcpp_web_upload_result* up) {
  if (up == nullptr) return "null";
  std::string s;
  for (size_t i = 0; i < up->files().size(); ++i) {
    const uvcpp_web_upload_file& f = up->files()[i];
    s += "F|" + f.field_name() + "|" + f.original_filename() + "|" +
         f.content_type() + "|" + std::to_string(f.size()) + "|" +
         (f.truncated() ? "1" : "0") + "\n";
  }
  for (size_t i = 0; i < up->fields().size(); ++i) {
    s += "D|" + up->fields()[i].name() + "|" + up->fields()[i].value() + "\n";
  }
  return s;
}

void configure_for_test(uvcpp_web_app& app) {
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);
}

/**
 * @brief 注册一条上传路由，并把过程记进 `p`。
 *
 * 形状**刻意就是文档里的用法**：handler 里拿 `req.stream()`、在其中注册
 * `on_end`、在 `on_end` 里读 `req.upload()`。如果框架把用户槽顶掉了，或者
 * 链在落盘完成之前就收尾了，这里的 `ended` 或 `summary` 会对不上 —— 这正是
 * 本文件要钉的接线。
 */
void add_upload_route(uvcpp_web_app& app, const std::string& pattern,
                      const std::shared_ptr<up_probe>& p) {
  app.post_upload(
      pattern,
      [p](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next) {
        p->handler_called += 1;

        uvcpp_web_stream* s = req.stream();
        if (s == nullptr) {
          // 非流式 = 接线错了（这条路由必须走认领路径）。
          resp.server_error().text("not streaming");
          resp.end();
          return;
        }
        p->upload_at_handler = (req.upload() != nullptr);

        s->on_end([&req, &resp, p]() {
          p->ended += 1;
          const uvcpp_web_upload_result* up = req.upload();
          p->upload_null = (up == nullptr);
          if (up != nullptr) {
            p->files = up->files().size();
            p->fields = up->fields().size();
            p->file_list = up->files();
            p->field_list = up->fields();
          }
          p->summary = describe_upload(up);
          resp.text(p->summary);
          resp.end();
        });

        s->on_abort([p]() { p->aborted += 1; });
      });
}

/// 上传目录固定在工作目录下（ctest 的工作目录就是构建目录）。
std::string g_dir;

// =========================================================================
// 背压探针：**用户槽**能观察到什么（步骤 5）
//
// 要证明的是"框架把背压通道装上去了"，而它对外**唯一**可观测的面是
// `stream->paused()` —— `uvcpp_web_upload_flow` 是会话与框架之间的内部结构，
// 用户碰不到它。
//
// 用户为什么在这里看得到：`uvcpp_web_stream::deliver_data()` 里**框架槽先跑、
// 用户槽后跑**，而框架槽（`mp->feed()`）解析到文件部件的头时会当场把异步
// open 提交出去（`busy_` 为真），紧随其后的 `sync_flow()` 就下发 `pause()`。
// 于是"用户的数据回调被执行到"与"这一块数据让会话决定暂停"落在**同一个同步
// 栈**上 —— 用户槽读到的 `paused()` 必然为真。这不是竞态，是同一次调用里定死
// 的先后（`hooks_.on_data` 与 `data_cb_` 之间没有任何 `uv_run`）。
//
// 反过来，如果 `wire_upload()` 少了 `up->set_flow(fl)`，`flow_.pause` 就是个空
// `std::function`，`sync_flow()` 在第一句 `if (!fn) return;` 上返回（连
// `pause_events_` 都不加），`paused()` 永远是假 —— 这正是本组要钉的实现错误。
// =========================================================================

struct flow_probe {
  flow_probe()
      : handler_called(0),
        data_calls(0),
        paused_samples(0),
        raw_bytes(0),
        paused_at_end(false),
        ended(0),
        aborted(0),
        file_path(),
        file_size(0) {}

  int      handler_called;
  int      data_calls;
  /// 用户 `on_data` 里 `stream->paused()` 为真的次数。
  int      paused_samples;
  /// 用户 `on_data` 收到的原始 body 字节总数 —— **加上**框架槽自己吃的那一份，
  /// 所以它应当等于整个 body 的长度（两个槽是并列的，不是二选一）。
  uint64_t raw_bytes;
  /// `on_end` 那一刻读到的 `paused()`。
  bool     paused_at_end;
  int      ended;
  int      aborted;
  std::string file_path;
  uint64_t    file_size;
};

/**
 * @brief 上传路由，用户槽**额外**挂一个 `on_data` 采样背压状态。
 *
 * 这同时钉住另一件容易写错的事：`upload_route()` 的框架槽与用户槽是**两个独立
 * 的槽**，用户装 `on_data` 不会把 multipart 的喂入顶掉（真顶掉了的话，文件一
 * 个字节都不会落盘，下面的逐字节比对会当场红）。
 */
void add_flow_route(uvcpp_web_app& app, const std::string& pattern,
                    const std::shared_ptr<flow_probe>& p) {
  app.post_upload(
      pattern,
      [p](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next) {
        p->handler_called += 1;

        uvcpp_web_stream* s = req.stream();
        if (s == nullptr) {
          resp.server_error().text("not streaming");
          resp.end();
          return;
        }

        s->on_data([p, s](const char*, size_t n) {
          p->data_calls += 1;
          p->raw_bytes += n;
          if (s->paused()) p->paused_samples += 1;
          // 返回 false 会被框架当成"超限"，直接 `abort(413)`。
          return true;
        });

        s->on_end([&req, &resp, p]() {
          p->ended += 1;
          uvcpp_web_stream* st = req.stream();
          p->paused_at_end = (st != nullptr) && st->paused();
          const uvcpp_web_upload_result* up = req.upload();
          if (up != nullptr && up->files().size() == 1) {
            p->file_path = up->files()[0].path();
            p->file_size = up->files()[0].size();
          }
          resp.text(describe_upload(up));
          resp.end();
        });
      });
}

/**
 * @brief 对照组：**普通**流式路由（没有落盘会话，拿不到 `req.upload()`）。
 *
 * 少了它，"只要走流式就先把读停一下"的实现能让上面那组断言全绿 —— 而那说明不
 * 了任何"背压通道被装上了"。这一条要求 `paused()` **一次都不为真**。
 */
void add_plain_stream_route(uvcpp_web_app& app, const std::string& pattern,
                           const std::shared_ptr<flow_probe>& p) {
  app.post_stream(
      pattern,
      [p](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next) {
        p->handler_called += 1;

        uvcpp_web_stream* s = req.stream();
        if (s == nullptr) {
          resp.server_error().text("not streaming");
          resp.end();
          return;
        }

        s->on_data([p, s](const char*, size_t n) {
          p->data_calls += 1;
          p->raw_bytes += n;
          if (s->paused()) p->paused_samples += 1;
          return true;
        });

        s->on_end([&resp, p]() {
          p->ended += 1;
          resp.text("plain-ok");
          resp.end();
        });
      });
}

// =========================================================================
// 1. single_file —— 最短的正路
//
// 一条报文、一个文件部件，两边都看：响应体是 handler 读到的结果，
// 盘上是独立读路径拿回的字节。
// =========================================================================
void test_single_file() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  const std::string data = make_payload(4096, 7);

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_upload_route(app, "/upload", p);

  check(app.start_background() == 0, "single: 服务启动");
  const int port = app.bound_port();

  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(1, file_part("avatar", "a.bin",
                                            "application/octet-stream", data)));

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/upload", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "single: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(200);

    check_eq_i(p->handler_called, 1, "single: handler 恰好一次");
    check(!p->upload_at_handler,
          "single: handler 里 `req.upload()` 必须还是 nullptr"
          "（结果在 on_end 才存在）");
    check_eq_i(p->ended, 1, "single: on_end 恰好一次");
    check_eq_i(p->aborted, 0, "single: 不该走 abort");
    check(!p->upload_null, "single: on_end 里 `req.upload()` 必须非空");
    check_eq_i(static_cast<long long>(p->files), 1, "single: 恰好一个文件");
    check_eq_i(static_cast<long long>(p->fields), 0, "single: 不该有字段");

    check_eq_i(count_responses(c.rx()), 1, "single: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 200", "single: 状态码 200");
    check_contains(c.rx(),
                   "F|avatar|a.bin|application/octet-stream|4096|0",
                   "single: 用户在 on_end 里读到了完整结果");

    const std::vector<std::string> entries = list_dir(g_dir);
    check_eq_i(static_cast<long long>(entries.size()), 1,
               "single: 目录里恰好一个文件（不多不少）");
    if (p->file_list.size() == 1) {
      check(p->file_list[0].path().compare(0, g_dir.size(), g_dir) == 0,
            "single: 落盘路径必须在上传目录内");
      check_bytes(read_all(p->file_list[0].path()), data,
                  "single: 盘上内容必须逐字节等于原文");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 2. quoted_boundary —— boundary 参数带引号
//
// `web_multipart_boundary()` 对引号的处理是单独一段代码，而这一步是它**第一次**
// 被接进真实请求路径。少了这条，一个"只认不带引号的 boundary"的实现在别处
// 全绿 —— 而带引号是 RFC 2046 允许的、且真实客户端会发的形状。
// =========================================================================
void test_quoted_boundary() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  const std::string data = "quoted-boundary-payload";

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_upload_route(app, "/q", p);

  check(app.start_background() == 0, "quoted: 服务启动");
  const int port = app.bound_port();

  const std::string body = mp_body(
      kBoundary, std::vector<std::string>(1, file_part("f", "q.txt",
                                                       "text/plain", data)));

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/q", kBoundary, body.size(), "multipart/form-data",
                          /*quoted=*/true) +
                  body),
          "quoted: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(200);

    check_eq_i(p->ended, 1, "quoted: on_end 恰好一次");
    check(!p->upload_null, "quoted: 带引号的 boundary 必须能被解析出来");
    check_contains(c.rx(), "HTTP/1.1 200", "quoted: 状态码 200");
    if (p->file_list.size() == 1) {
      check_eq_s(read_all(p->file_list[0].path()), data,
                 "quoted: 内容必须正确");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 3. multi_part_fields —— 多文件 + 混字段，顺序与内容
//
// 文件与**字段**在同一个报文里交替出现。字段不进盘，所以这条同时钉住了
// "两种部件走的是两条路"：少一个文件、或者少一个字段，这里都会红。
// =========================================================================
void test_multi_part_fields() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  const std::string d1 = make_payload(1000, 11);
  const std::string d2 = make_payload(2000, 13);

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_upload_route(app, "/multi", p);

  check(app.start_background() == 0, "multi: 服务启动");
  const int port = app.bound_port();

  std::vector<std::string> parts;
  parts.push_back(field_part("room", "kitchen"));
  parts.push_back(file_part("one", "1.bin", "application/octet-stream", d1));
  parts.push_back(field_part("note", "hi there"));
  parts.push_back(file_part("two", "2.bin", "application/octet-stream", d2));
  const std::string body = mp_body(kBoundary, parts);

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/multi", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "multi: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(200);

    check_eq_i(p->ended, 1, "multi: on_end 恰好一次");
    check_eq_i(static_cast<long long>(p->files), 2, "multi: 两个文件");
    check_eq_i(static_cast<long long>(p->fields), 2, "multi: 两个字段");
    check_eq_i(count_responses(c.rx()), 1, "multi: 线上恰好一个响应");

    if (p->file_list.size() == 2) {
      check_eq_s(p->file_list[0].field_name(), "one", "multi: 第一个文件的字段名");
      check_eq_s(p->file_list[1].field_name(), "two", "multi: 第二个文件的字段名");
      check_bytes(read_all(p->file_list[0].path()), d1,
                  "multi: 第一个文件内容");
      check_bytes(read_all(p->file_list[1].path()), d2,
                  "multi: 第二个文件内容");
    }
    if (p->field_list.size() == 2) {
      check_eq_s(p->field_list[0].name(), "room", "multi: 第一个字段名");
      check_eq_s(p->field_list[0].value(), "kitchen", "multi: 第一个字段值");
      check_eq_s(p->field_list[1].name(), "note", "multi: 第二个字段名");
      check_eq_s(p->field_list[1].value(), "hi there", "multi: 第二个字段值");
    }

    check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 2,
               "multi: 目录里恰好两个文件（字段不落盘）");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 4. binary_body —— NUL / 0xFF / CRLF 混合的大载荷
//
// 走的是 socket → llhttp → 框架流 → multipart → 线程池 → 磁盘这一整条链。
// 任何一处按 C 字符串处理、或者按文本模式读写，都会在这里断。
// =========================================================================
void test_binary_body() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  // 1 MiB：够大，能跨多个读块和多个写块，但又不至于让用例变慢。
  std::string data;
  data.reserve(1024 * 1024);
  for (size_t i = 0; i < 1024 * 1024; ++i) {
    switch (i % 8) {
      case 0: data.push_back('\0'); break;
      case 1: data.push_back(static_cast<char>(0xFF)); break;
      case 2: data.push_back('\r'); break;
      case 3: data.push_back('\n'); break;
      default: data.push_back(static_cast<char>('a' + (i % 26))); break;
    }
  }

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_upload_route(app, "/bin", p);

  check(app.start_background() == 0, "binary: 服务启动");
  const int port = app.bound_port();

  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(1, file_part("blob", "b.bin",
                                            "application/octet-stream", data)));

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/bin", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "binary: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 15000);
    c.pump(300);

    check_eq_i(p->ended, 1, "binary: on_end 恰好一次");
    check(!p->upload_null, "binary: `req.upload()` 必须非空");
    if (p->file_list.size() == 1) {
      check_eq_i(static_cast<long long>(p->file_list[0].size()),
                 static_cast<long long>(data.size()),
                 "binary: size() 必须等于载荷长度");
      check_bytes(read_all(p->file_list[0].path()), data,
                  "binary: 1 MiB 二进制载荷必须逐字节一致");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 5. empty_file —— 0 字节**文件**部件
//
// 0 字节部件仍然是**一个文件**（沿用步骤 3 的语义）。
//
// 注意它**不是**"同步收口"那一支：一旦有文件部件，会话就要提交一次异步 open
// （哪怕零字节），所以 `on_end` 那一刻 `busy_` 必为真、完成回调必然异步。
// 同步收口的是下面第 14 条（纯字段、一个文件都没有）—— 两者必须分开，混为一谈
// 会让"`end_deferred_` 先扣后跑"这条顺序失去唯一的判据（我一开始就写错了，
// 把这条注释安在了 empty_file 上）。
// =========================================================================
void test_empty_file() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_upload_route(app, "/empty", p);

  check(app.start_background() == 0, "empty: 服务启动");
  const int port = app.bound_port();

  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(1, file_part("e", "e.bin",
                                            "application/octet-stream", "")));

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/empty", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "empty: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(200);

    check_eq_i(p->ended, 1,
               "empty: on_end 恰好一次（同步收口那条路必须也放行）");
    check_eq_i(count_responses(c.rx()), 1, "empty: 线上恰好一个响应");
    check_eq_i(static_cast<long long>(p->files), 1, "empty: 0 字节部件也是文件");
    if (p->file_list.size() == 1) {
      check_eq_i(static_cast<long long>(p->file_list[0].size()), 0,
                 "empty: size() 必须是 0");
      check_eq_i(static_cast<long long>(read_all(p->file_list[0].path()).size()),
                 0, "empty: 盘上必须是 0 字节");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 6. split_writes —— 报文分多次写出去
//
// 解析器的跨块状态（保留缓冲、边界相位）在真实路径上第一次被用到。整包写出去
// 永远测不出"保留长度算错"这类 bug。
// =========================================================================
void test_split_writes() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  const std::string data = make_payload(3000, 17);

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_upload_route(app, "/split", p);

  check(app.start_background() == 0, "split: 服务启动");
  const int port = app.bound_port();

  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(1, file_part("s", "s.bin",
                                            "application/octet-stream", data)));
  const std::string head =
      mp_head("/split", kBoundary, body.size(), "multipart/form-data");
  const std::string wire = head + body;

  staged_conn c;
  if (c.open(port)) {
    // 切成 7 块。这个数字不是随手挑的：报文里最长的"必须整体匹配"的记号是
    // 边界行，切成 7 块必然把它切开好几次。
    const size_t kChunks = 7;
    const size_t per = (wire.size() + kChunks - 1) / kChunks;
    bool all_written = true;
    for (size_t off = 0; off < wire.size(); off += per) {
      const size_t n =
          (per < wire.size() - off) ? per : (wire.size() - off);
      if (!c.write(wire.substr(off, n))) {
        all_written = false;
        break;
      }
      c.pump(20);
    }
    check(all_written, "split: 每一块都必须写成功（EALREADY 会静默丢块）");

    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 8000);
    c.pump(200);

    check_eq_i(p->ended, 1, "split: on_end 恰好一次");
    check(!p->upload_null, "split: `req.upload()` 必须非空");
    if (p->file_list.size() == 1) {
      check_bytes(read_all(p->file_list[0].path()), data,
                  "split: 跨块拼出来的内容必须逐字节正确");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 7. filename_metadata —— 客户端给的名字**此刻原样转发**，落盘名与它无关
//
// 清洗是步骤 7 的事，本步**刻意不做**。这条把"此刻的行为"钉下来，免得步骤 7
// 之前的某个"顺手加个清洗"悄悄改变语义；反过来，步骤 7 落地时这条会红 ——
// 那是**预期的**，届时按新语义改写。
//
// 真正与安全有关的那半（落盘名与客户端输入无关）在这里就已经成立，所以现在就
// 钉住：穿越路径不该让文件落到上传目录之外。
// =========================================================================
void test_filename_metadata() {
  const char* nasty[] = {"../../etc/passwd", "..\\..\\windows\\x.txt",
                         "/abs/dir/y.txt", "C:\\z\\w.txt"};

  for (size_t k = 0; k < sizeof(nasty) / sizeof(nasty[0]); ++k) {
    purge_dir(g_dir);
    std::shared_ptr<up_probe> p(new up_probe());

    uvcpp_web_app app;
    configure_for_test(app);
    app.set_upload_dir(g_dir);
    add_upload_route(app, "/name", p);

    check(app.start_background() == 0, "name: 服务启动");
    const int port = app.bound_port();

    const std::string body = mp_body(
        kBoundary,
        std::vector<std::string>(1, file_part("f", nasty[k], "text/plain", "X")));
    const std::string tag = std::string("name[") + nasty[k] + "]";

    staged_conn c;
    if (c.open(port)) {
      check(c.write(mp_head("/name", kBoundary, body.size(),
                            "multipart/form-data") +
                    body),
            tag + ": 请求必须写出去");
      c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
      c.pump(200);

      check_eq_i(p->ended, 1, tag + ": on_end 恰好一次");
      if (p->file_list.size() == 1) {
        const std::string path = p->file_list[0].path();
        check(path.compare(0, g_dir.size(), g_dir) == 0,
              tag + ": 落盘路径必须在上传目录内");
        check(path.find("..") == std::string::npos,
              tag + ": 落盘路径里不该出现 ..");
        check_eq_s(read_all(path), "X", tag + ": 内容仍然正确");

        // 落盘叶子名必须是框架摇的 16 位十六进制 —— 与客户端给的名字无关。
        //
        // 只钉**前 16 位是十六进制**而不钉"总长恰好 16"：步骤 7 会给它补一个
        // 白名单过滤后的扩展名，那时总长会变成 16 + 1 + n。现在钉死长度等于
        // 给步骤 7 埋一个必然要改的断言；钉住"框架名"这件事本身才是本步的判据。
        const std::string leaf = path.substr(g_dir.size() + 1);
        bool hex16 = leaf.size() >= 16;
        for (size_t i = 0; hex16 && i < 16; ++i) {
          const char c = leaf[i];
          const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
          if (!is_hex) hex16 = false;
        }
        check(hex16, tag + ": 落盘叶子名的前 16 位必须是框架摇的十六进制");
        check(leaf != nasty[k], tag + ": 落盘名绝不能等于客户端给的名字");
      }
      // 目录里恰好一个文件：证明没有别的地方被写出去。
      check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 1,
                 tag + ": 上传目录里恰好一个文件");
    }

    app.stop();
    app.join();
  }
}

// =========================================================================
// 8. reject_415 —— 不是 multipart ⇒ 415，且**用户的 handler 一次都不跑**
//
// 框架在 body 之前就回了，所以用户的 `on_end` 也不该被调用（`ended == 0`）。
// 少了后半个断言，一个"先跑用户链再覆盖响应"的实现能蒙混过关 —— 而那个实现
// 会让用户看到一个自己已经拒收的请求。
// =========================================================================
void test_reject_415() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_upload_route(app, "/json", p);

  check(app.start_background() == 0, "415: 服务启动");
  const int port = app.bound_port();

  const std::string payload = "{\"not\":\"multipart\"}";

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/json", kBoundary, payload.size(),
                          "application/json") +
                  payload),
          "415: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(200);

    check_eq_i(p->handler_called, 0,
               "415: 用户的 handler 一次都不该跑（格式判断框架已经做过了）");
    check_eq_i(p->ended, 0, "415: 用户的 on_end 不该被调用");
    check_eq_i(count_responses(c.rx()), 1, "415: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 415", "415: 状态码是 415");
    check_contains(c.rx(), "application/json",
                   "415: 错误信息里要说清收到的是什么");
    check_contains(c.rx(), "connection: close",
                   "415: 剩下的 body 没人消费，必须要求关闭连接");
    check(wait_for([&]() { return c.peer_closed(); }, 3000),
          "415: 服务端必须真的把连接关掉");
    check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 0,
               "415: 上传目录必须还是空的");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 9. reject_400_no_boundary —— 是 multipart，但 boundary 参数缺失
//
// 与 415 分开：媒体类型对而 boundary 缺失是**报文写坏了**，不是"换个格式"。
// 合成一个 400（或一个 415）会让排障的人往错的方向找。
// =========================================================================
void test_reject_400_no_boundary() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_upload_route(app, "/nb", p);

  check(app.start_background() == 0, "400nb: 服务启动");
  const int port = app.bound_port();

  const std::string payload = "whatever";

  staged_conn c;
  if (c.open(port)) {
    // 刻意用 `mp_head` 的 multipart 分支会被补上 boundary，所以这里手拼一个
    // **没有** boundary 参数的 Content-Type。
    std::string head = "POST /nb HTTP/1.1\r\nHost: t\r\n";
    head += "content-length: " + std::to_string(payload.size()) + "\r\n";
    head += "content-type: multipart/form-data\r\n\r\n";
    check(c.write(head + payload), "400nb: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(200);

    check_eq_i(p->handler_called, 0, "400nb: 用户的 handler 不该跑");
    check_eq_i(count_responses(c.rx()), 1, "400nb: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 400", "400nb: 状态码是 400（不是 415）");
    check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 0,
               "400nb: 上传目录必须还是空的");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 10. reject_500_no_dir —— 忘了配 `set_upload_dir()`
//
// 这是**配置错**而不是请求错，所以必须是 5xx。用户不该因为自己漏配了一个
// setter 而让客户端收到 4xx（那会把排障的人引到报文上去）。
// =========================================================================
void test_reject_500_no_dir() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  // **刻意不调 `set_upload_dir()`。**
  add_upload_route(app, "/nodir", p);

  check(app.start_background() == 0, "500: 服务启动");
  const int port = app.bound_port();

  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(1, file_part("f", "f.bin", "text/plain", "Y")));

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/nodir", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "500: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(200);

    check_eq_i(p->handler_called, 0, "500: 用户的 handler 不该跑");
    check_eq_i(count_responses(c.rx()), 1, "500: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 500", "500: 配置错必须是 5xx");
    check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 0,
               "500: 上传目录必须还是空的");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 11. plain_route_untouched —— 普通路由**不**走 multipart（回归网）
//
// 本步只给 `upload_route()` 接线，普通路由必须**一个字节都不变**：body 照旧
// 整包攒进 `req.body_*()`，`req.upload()` 恒为空。
//
// 这条同时是步骤 8 的前置判据 —— 步骤 8 落地后它会被改写（普通路由也要解析
// multipart 进内存），届时 `req.upload()` 之外的东西仍然必须成立。
// =========================================================================
void test_plain_route_untouched() {
  const std::string data = "plain-body";
  size_t seen_size = 0;
  std::string seen_body;
  bool seen_multipart = false;
  bool seen_upload_null = false;
  bool seen_stream_null = false;
  int calls = 0;

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);

  // **普通** `post()`，不是 `post_upload()`。
  app.post("/raw", [&](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next) {
    calls += 1;
    seen_size = req.body_size();
    seen_body = req.body_str();
    seen_multipart = req.is_multipart();
    seen_upload_null = (req.upload() == nullptr);
    seen_stream_null = (req.stream() == nullptr);
    resp.text("raw-ok");
    resp.end();
  });

  check(app.start_background() == 0, "plain: 服务启动");
  const int port = app.bound_port();

  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(1, file_part("f", "f.bin", "text/plain", data)));

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/raw", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "plain: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(200);

    check_eq_i(calls, 1, "plain: handler 恰好一次");
    check_eq_i(static_cast<long long>(seen_size),
               static_cast<long long>(body.size()),
               "plain: body 必须整包攒进 req（不多不少）");
    check_eq_s(seen_body, body, "plain: body 内容逐字节一致");
    check(seen_multipart, "plain: `is_multipart()` 仍按 Content-Type 判定");
    check(seen_upload_null,
          "plain: 普通路由的 `req.upload()` 必须恒为 nullptr");
    check(seen_stream_null, "plain: 普通路由不该有流对象");
    check_contains(c.rx(), "raw-ok", "plain: 用户填的响应照常发出");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 12. disconnect_deletes_temp —— 传到一半断开
//
// 「失败时框架删」这条决策的唯一证据。判据是**盘上**（上传目录变空），
// 而不是某个标志位 —— 因为这里要证的是文件真的不在了。
//
// 时序上有一处刻意不钉：断开时链已经收尾、`inflight_` 里那条已经摘掉了，
// 所以落盘完成回调会走"上下文已收场"那一支（只会打一条 WARN）。文件仍然
// 必须被删 —— 这个分支下的删除**由会话自己负责**，与上下文在不在无关。
// =========================================================================
void test_disconnect_deletes_temp() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_upload_route(app, "/cut", p);

  check(app.start_background() == 0, "cut: 服务启动");
  const int port = app.bound_port();

  const std::string data = make_payload(64 * 1024, 23);
  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(1, file_part("f", "f.bin",
                                            "application/octet-stream", data)));
  const std::string head =
      mp_head("/cut", kBoundary, body.size(), "multipart/form-data");

  {
    staged_conn c;
    if (c.open(port)) {
      // 头 + 前三分之一 —— 足够让部件开始、有字节落盘，但远远没到边界。
      check(c.write(head + body.substr(0, body.size() / 3)),
            "cut: 部分报文必须写出去");
      // 让服务端真的开始落盘（给线程池几个来回）。
      c.pump(400);
      // 析构 `c` 会关掉连接 —— 对端从这里断开。
    }
  }

  const bool drained = wait_for(
      [&]() { return list_dir(g_dir).empty(); }, 5000);

  check(drained,
        "cut: 断开之后上传目录必须变空（本次创建的临时文件全部删掉）");
  check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 0,
             "cut: 目录里一个文件都不该留");
  check_eq_i(p->ended, 0, "cut: 没收到完整 body，on_end 不该被调用");
  check_eq_i(p->aborted, 1, "cut: on_abort 恰好一次");

  app.stop();
  app.join();
}

// =========================================================================
// 13. keepalive_after_upload —— 上传之后同一条连接上的第二次上传
//
// 这条钉的是**上下文有没有真的收尾**：`inflight_` 里那一条要是在落盘完成之后
// 没被摘掉，第二次请求会撞上"已有在途请求"并把前一个顶掉（框架会打 WARN），
// 症状是第二个响应永远不来。
// =========================================================================
void test_keepalive_after_upload() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_upload_route(app, "/ka", p);

  check(app.start_background() == 0, "ka: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    for (int i = 0; i < 2; ++i) {
      const std::string data = make_payload(1024, static_cast<unsigned>(31 + i));
      const std::string body =
          mp_body(kBoundary,
                  std::vector<std::string>(
                      1, file_part("f", "f.bin", "application/octet-stream",
                                   data)));
      const std::string tag = "ka[" + std::to_string(i) + "]";

      // 第二次要在第一个响应回来之后才发 —— 本用例钉的是"上一次上传收尾了
      // 没有"，流水线（两条同时在途）是另一条路径，见
      // `web_app_pipeline_func.cpp`。
      if (i > 0) {
        c.pump_until([&]() { return count_responses(c.rx()) >= i; }, 5000);
      }
      check(c.write(mp_head("/ka", kBoundary, body.size(),
                            "multipart/form-data") +
                    body),
            tag + ": 请求必须写出去");
      c.pump_until([&]() { return count_responses(c.rx()) >= i + 1; }, 8000);
      c.pump(150);
    }

    check_eq_i(p->ended, 2, "ka: on_end 两次");
    check_eq_i(p->aborted, 0, "ka: 不该走 abort");
    check_eq_i(count_responses(c.rx()), 2,
               "ka: 线上恰好两个响应（前一个真的收尾了）");
    check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 2,
               "ka: 两次上传各留一个文件");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 14. fields_only_no_files —— 纯字段表单，一个文件部件都没有
//
// 这条是**唯一**打在"同步收口"上的用例，而那条路是 `deliver_end()` 里
// "**先扣住、再跑钩子**"这个顺序存在的全部理由：
//
//   会话在 `notify_parse_done()` 里调 `pump()`；没有文件部件 ⇒ `queue_` 空、
//   `busy_` 假 ⇒ `complete_if_ready()` **当场**调完成回调，而那个回调会调
//   `release_end()`。于是"放行用户 on_end"这件事发生在**框架钩子还没返回**的
//   时候。如果那一刻 `end_deferred_` 还是假，这一放就等于什么都没放（
//   `release_end()` 第一句就早返回），用户的 `on_end` 永远不来 —— 请求一直挂到
//   闲置超时，线上什么响应都没有。
//
// 用例的形状因此必须与 `empty_file` 那种"有文件、只是 0 字节"分开：后者提交了
// 一次异步 open，收口必然异步，走不到这条路上。
// =========================================================================
void test_fields_only_no_files() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_upload_route(app, "/fields", p);

  check(app.start_background() == 0, "fields: 服务启动");
  const int port = app.bound_port();

  std::vector<std::string> parts;
  parts.push_back(field_part("user", "alice"));
  parts.push_back(field_part("note", "no files here"));
  const std::string body = mp_body(kBoundary, parts);

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/fields", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "fields: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(200);

    check_eq_i(p->ended, 1,
               "fields: on_end 恰好一次（同步收口那条路放行的就是它）");
    check(!p->upload_null,
          "fields: 纯字段表单也算一次成功的上传，`req.upload()` 必须非空");
    check_eq_i(static_cast<long long>(p->files), 0, "fields: 不该有文件");
    check_eq_i(static_cast<long long>(p->fields), 2, "fields: 两个字段");
    check_eq_i(count_responses(c.rx()), 1, "fields: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 200", "fields: 状态码 200");
    check_contains(c.rx(), "D|user|alice", "fields: 用户在 on_end 里读到了字段");
    if (p->field_list.size() == 2) {
      check_eq_s(p->field_list[1].name(), "note", "fields: 第二个字段名");
      check_eq_s(p->field_list[1].value(), "no files here", "fields: 第二个字段值");
    }
    check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 0,
               "fields: 字段不进盘 —— 上传目录必须还是空的");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 15. flow_backpressure_wired —— `wire_upload()` 真的把背压通道接到了这条流上
//
// 这是步骤 5 在**接线层**的唯一证据。`web_app_upload_func.cpp` 里的
// `backpressure_engages` 测的是会话本身（直接喂字节、手工注入 flow），它证明
// 不了"框架在真端口上把 flow 装上了"—— 而这两件事之间隔着一整个
// `uvcpp_web_app::wire_upload()`。
//
// 三条断言各钉一件事，缺一条都留一个洞：
//
//   1. `paused_samples >= 1`：背压真的下发了（见 `flow_probe` 那段注释里为什么
//      这一步是确定的，而不是撞运气）。
//   2. `!paused_at_end`：收尾前**必须松开**读。`complete_if_ready()` 在跑完成
//      回调之前调了一次 `sync_flow()`，就是为了这个 —— 它是"暂停没有配对的
//      resume"那条 keep-alive 事故（本文件第 14 组覆盖的是同一件事的另一半：
//      松开之后下一个请求照常工作）。
//   3. 盘上内容逐字节相等：整个暂停/恢复过程中**一块字节都不能少、不能乱序**。
//      只断言"暂停过"是不够的 —— 一个丢掉暂停期间数据的实现照样能通过前两条。
//
// 外加一组对照（普通流式路由 `paused()` 必须恒假），否则"见流式就 pause"的
// 实现也能蒙混过关。
// =========================================================================
void test_flow_backpressure_wired() {
  purge_dir(g_dir);
  std::shared_ptr<flow_probe> up_p(new flow_probe());
  std::shared_ptr<flow_probe> plain_p(new flow_probe());

  // 256 KiB：小到不拖时间，大到**必然**跨多次读 —— 于是"暂停之后对端还在灌"
  // 的那几块真的会被攒下来，而 resume 之后它们必须一块不少地落盘。
  const std::string data = make_payload(256 * 1024, 23);

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  add_flow_route(app, "/flow", up_p);
  add_plain_stream_route(app, "/plain", plain_p);

  check(app.start_background() == 0, "flow: 服务启动");
  const int port = app.bound_port();

  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(1, file_part("doc", "big.bin",
                                            "application/octet-stream", data)));

  {
    staged_conn c;
    if (c.open(port)) {
      // **整包一次写**。分块写（`split_writes` 那种）会让框架每块之间有机会
      // 把 open 和 write 都跑完 —— 那正好把要考的窗口关掉了。
      check(c.write(mp_head("/flow", kBoundary, body.size(),
                            "multipart/form-data") + body, 8000),
            "flow: 请求必须写出去");
      c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 15000);
      c.pump(300);

      check_eq_i(up_p->handler_called, 1, "flow: handler 恰好一次");
      check(up_p->data_calls >= 1,
            "flow: 用户槽照常收到 body 字节（框架槽没有把数据独占掉）");
      check_eq_i(static_cast<long long>(up_p->raw_bytes),
                 static_cast<long long>(body.size()),
                 "flow: 用户 on_data 收到的字节总数必须等于整个 body 的长度");
      check(up_p->paused_samples >= 1,
            "flow: 背压真的装上了 —— 用户槽观察到 stream 已暂停");
      check(!up_p->paused_at_end,
            "flow: on_end 时必须已经松开读"
            "（漏一次 resume 会把 keep-alive 连接的读永久停住）");
      check_eq_i(up_p->ended, 1, "flow: on_end 恰好一次");
      check_eq_i(up_p->aborted, 0, "flow: 不该走 abort");
      check_eq_i(count_responses(c.rx()), 1, "flow: 线上恰好一个响应");
      check_contains(c.rx(), "HTTP/1.1 200", "flow: 状态码 200");
      check_contains(c.rx(), "F|doc|big.bin|application/octet-stream|262144|0",
                     "flow: 用户在 on_end 里读到了完整结果");

      check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 1,
                 "flow: 目录里恰好一个文件");
      check_eq_i(static_cast<long long>(up_p->file_size),
                 static_cast<long long>(data.size()),
                 "flow: 元数据里的 size 必须等于原文长度");
      if (!up_p->file_path.empty()) {
        check_bytes(read_all(up_p->file_path), data,
                    "flow: 背压期间攒下的字节一块不能少、一格不能错位");
      }
    }
  }

  {
    // 对照组：同样的客户端形状、同样的用户槽代码，只是路由是**普通**流式路由。
    const std::string pdata = make_payload(4096, 29);

    staged_conn c2;
    if (c2.open(port)) {
      // `mp_head()` 在 content-type 不是 multipart 时原样照发，所以这条报文
      // 是一条普通的 `text/plain` POST。
      check(c2.write(mp_head("/plain", kBoundary, pdata.size(), "text/plain") +
                     pdata, 8000),
            "flow/plain: 对照请求必须写出去");
      c2.pump_until([&]() { return count_responses(c2.rx()) > 0; }, 8000);
      c2.pump(200);

      check_eq_i(plain_p->handler_called, 1, "flow/plain: handler 恰好一次");
      check(plain_p->data_calls >= 1, "flow/plain: 对照组也要收到字节");
      check_eq_i(static_cast<long long>(plain_p->raw_bytes),
                 static_cast<long long>(pdata.size()),
                 "flow/plain: 收到的字节总数等于 body 长度");
      check_eq_i(plain_p->paused_samples, 0,
                 "flow/plain: 没有落盘会话的流式路由，没人有权调 pause");
      check_eq_i(plain_p->ended, 1, "flow/plain: on_end 恰好一次");
      check_contains(c2.rx(), "plain-ok", "flow/plain: 用户填的响应照常发出");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 步骤 6 的报文构造：chunked 请求
//
// 这几组要的上限都**不能**靠 `content-length` 表达 —— 那条路上有 `body_limit`
// 中间件和 http 层的 `max_body_size_` 挡着，测出来的是它们而不是框架自己的
// 计数。chunked 把这两道闸门一起绕开（没有声明值可判），于是"413 是谁回的"
// 这个问题才只有一个答案。
// =========================================================================

/** @brief 一个 HTTP chunk 的线上形态（含长度行与结尾 CRLF）。 */
std::string chunk_of(const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  size_t n = s.size();
  std::string h;
  if (n == 0) {
    h = "0";
  } else {
    while (n > 0) {
      h.insert(h.begin(), kHex[n & 0xFu]);
      n >>= 4;
    }
  }
  return h + "\r\n" + s + "\r\n";
}

/** @brief 不带 `content-length` 的 multipart POST 头：长度全靠 chunked 表达。 */
std::string chunked_head(const std::string& path, const std::string& boundary) {
  std::string h = "POST " + path + " HTTP/1.1\r\nHost: t\r\n";
  h += "transfer-encoding: chunked\r\n";
  h += "content-type: multipart/form-data; boundary=" + boundary + "\r\n";
  h += "\r\n";
  return h;
}

// =========================================================================
// 16. limits_total —— 总长上限：框架**自己的字节计数**拦下来的
//
// 组形状的三个要点，缺一个这条上限就证明不了：
//
//   1. **chunked**（见上）。没有它，一个"压根没有总长上限"的实现照样会被
//      http 层的 `max_body_size_` 拦成 413，用例照绿。
//   2. **两个文件部件**，第一个完整、第二个跨越上限。这条形状把"删"的契约变成
//      可观测的：框架的掐断路径会把**已经交收的** part1 也删掉，而
//      `~uvcpp_web_upload` 那条兜底反而**特意保留** `kept` 的文件
//      （`if (j.kept && !discarding_) continue;`）。两条路在盘上留下**不同**的
//      结果，所以"目录空了"这个断言真的能分辨实现。
//   3. **前置断言**（`assert-test-preconditions`）：part1 必须真的成了一个完整的、
//      落在盘上的文件。少了它，"目录空了"在"part1 压根没落过盘"时也成立 ——
//      那这条用例什么都没证明，而且会是**永远绿**的那种什么都没证明。
// =========================================================================
void test_limits_total() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  const std::string part1 = make_payload(4096, 31);
  const std::string part2 = make_payload(32 * 1024, 37);

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  // 20 KiB：**大于** part1（4096）、**小于** part1 + part2（≈36 KiB）。
  // 于是掐断必然发生在 part2 的中段，part1 那边早已尘埃落定。
  app.set_max_upload_size(20 * 1024);
  add_upload_route(app, "/upload", p);

  check(app.start_background() == 0, "limittotal: 服务启动");
  const int port = app.bound_port();

  staged_conn c;
  if (c.open(port)) {
    const std::string b = kBoundary;

    // ---- 第一段：头 + part1 + 边界 + 边界后那个 CRLF，**到此为止**。
    //
    // 停在"part2 的头还没开始"这个位置上是有意的：此刻 part1 的边界已经完整
    // （`\r\n--<b>\r\n` 四个字节都在），它的写 / fsync / close 会全部跑完；
    // 而 part2 的头一个字节都还没到，`on_part_begin` 没被调用过 —— 目录里
    // 此时**只该有 part1 那一个文件**。
    std::string seg1 = chunked_head("/upload", b);
    seg1 += chunk_of("--" + b + "\r\n" +
                     file_part("a", "a.bin", "application/octet-stream", part1) +
                     "\r\n--" + b + "\r\n");
    check(c.write(seg1, 8000), "limittotal: 第一段必须写出去");

    const bool settled = c.pump_until(
        [&]() {
          const std::vector<std::string> e = list_dir(g_dir);
          return e.size() == 1 &&
                 read_all(g_dir + "/" + e[0]).size() == part1.size();
        },
        5000);
    check(settled, "limittotal: 前置 —— part1 必须已经完整落盘");
    // 再给一拍：落盘完成到 `kept` 之间还隔着 fsync + close 两次线程池往返，
    // 而"part1 是不是 `kept`"正是下面那条判别力的前提。
    c.pump(400);
    check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 1,
               "limittotal: 前置 —— 此刻目录里恰好一个文件（part2 还没开始）");

    // ---- 第二段：part2 的头 + 整个 part2 + 终边界，一次写出去。
    // 这一次的写可能因为服务端已经关连接而失败 —— 那是预期内的，不当断言。
    std::string seg2 =
        chunk_of(file_part("b", "b.bin", "application/octet-stream", part2) +
                 "\r\n--" + b + "--\r\n");
    seg2 += chunk_of("");
    c.write(seg2, 8000);

    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 8000);
    c.pump(300);

    check_eq_i(p->handler_called, 1, "limittotal: handler 恰好一次");
    check_eq_i(p->aborted, 1,
               "limittotal: 用户 on_abort 恰好一次（框架掐断了这次上传）");
    check_eq_i(p->ended, 0,
               "limittotal: 用户的 on_end **不能**触发（响应已经由框架回了）");
    check_eq_i(count_responses(c.rx()), 1, "limittotal: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 413", "limittotal: 状态码 413");
    check_contains(c.rx(), "connection: close",
                   "limittotal: 剩下的 body 没人消费，必须要求关闭连接");

    // **本组的核心断言。** 它同时钉住三件事：
    //   - 总长上限真的在框架里判（否则这条 chunked 上传会一路 200 收完）；
    //   - 掐断走的是流层 `abort()` → 框架钩子 `on_abort()` → `fail()`；
    //   - `fail()` 会把 `kept` 的文件也排进删除队列 —— 少了那一句钩子通知，
    //     删除就只剩析构兜底，而兜底**保留** `kept` 文件，part1 会留在盘上。
    check(wait_for([&]() { return list_dir(g_dir).empty(); }, 3000),
          "limittotal: 413 之后上传目录必须清空（连已交收的 part1 也要删）");
    check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 0,
               "limittotal: 目录里一个文件都不许剩");
    check(wait_for([&]() { return c.peer_closed(); }, 3000),
          "limittotal: 服务端必须真的把连接关掉");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 17. limits_file_truncate —— 单文件上限：**截断**而不是失败
//
// 这是六条上限里唯一的例外，所以它必须与其余五组**分开**测：截断不失败、不回
// 413、文件留着交给 handler，只有 `truncated()` 置位。合成一组的话，"截断"和
// "拒绝"这两件相反的事就会互相掩盖。
//
// 组里放了**第二个部件**（一个普通字段）是有意的：它证明 `PART_BODY_SKIP` 的
// "跳过"是"跳过交付、继续扫边界"，而不是"把后面的部件全吃掉"。少了它，一个
// 转 SKIP 之后就不再看边界的实现照样能让前两条断言全绿。
//
// **报文分两笔写，切点落在 part1 body 里、且已经在 4096 上限之后** —— 这不是
// 为了"更真实"，而是这一组能不能钉住那条不变式的**前提**：
//
//   整包喂（一笔写完）时，"交付 body（越限 → 截断 → 转 SKIP）"与"命中紧随其后
//   的那个边界（→ PART_HEADERS）"落在 `scan()` 的**同一次循环迭代**里 —— SKIP
//   当场被 PART_HEADERS 覆盖，`scan()` 顶部那处 SKIP 判定一次都走不到。于是
//   "SKIP 阶段还要不要扫边界"这件事**根本不可观测**：实测变异 L4（SKIP 阶段
//   直接 return）在整包喂的形状下全绿；探针给出该次请求只有**一次** feed
//   （`len=16624 state=0`），SKIP 判定命中 **0** 次。那是**真实覆盖缺口**，
//   不是等价变异 —— 已按缺口修（这一组改成两笔写），不是记成"等价"了事。
//
//   切开之后 SKIP 跨过了一次 feed 边界：第二笔进来时状态机正停在
//   `PART_BODY_SKIP`，`scan()` 必须在这个状态下继续扫边界，才能找到 part1 的
//   收尾边界、进而解析出后面的字段部件。这正是 `PART_BODY_SKIP` 存在的全部
//   理由。切完之后 L4 被这一组稳定抓住。
//
// 前置是**断言**出来的、不是假设的：第一笔之后盘上必须正好 4096 字节（说明
// 截断真的发生了）且报文还没结束；再 `pump(400)` 把第一笔剩下的字节读完，
// 第二笔才发出去 —— 这样"第二笔进来时停在 SKIP"就是确定的，而不用猜 TCP
// 怎么切。少了这个前置，整组就退化成"碰运气撞窗口"。
// =========================================================================
void test_limits_file_truncate() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  const std::string data = make_payload(16 * 1024, 41);

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  app.set_max_file_size(4096);

  add_upload_route(app, "/upload", p);

  check(app.start_background() == 0, "limittrunc: 服务启动");
  const int port = app.bound_port();

  // 部件头的字节数从 `file_part()` 自己量出来，不另抄一份 —— 抄一份必然会与
  // 它漂移，而下面的切点是按字节偏移算的。
  const std::string fp =
      file_part("doc", "big.bin", "application/octet-stream", data);
  const std::string part_head = fp.substr(0, fp.size() - data.size());

  const std::string body = mp_body(
      kBoundary, std::vector<std::string>{fp, field_part("after", "ok")});
  const std::string head =
      mp_head("/upload", kBoundary, body.size(), "multipart/form-data");

  // 切点 = `--<boundary>\r\n` + 部件头 + data 的前 6144 字节。
  // **越过 4096**：越过了才有截断；**不含任何边界**：不含才停在 SKIP。
  const size_t prefix_len =
      2 + std::string(kBoundary).size() + 2 + part_head.size();
  const size_t cut = prefix_len + 6144;
  check(cut > prefix_len + 4096 && cut < prefix_len + data.size(),
        "limittrunc: 前置 —— 切点落在 part1 的 body 里、且越过 4096 截断点");

  staged_conn c;
  if (c.open(port)) {
    check(c.write(head + body.substr(0, cut)),
          "limittrunc: 第一笔（止于 part1 body 中间）必须写出去");

    // 前置：截断已经发生（盘上正好 4096 字节），而报文还没结束。
    const bool cut_seen = c.pump_until(
        [&]() {
          const std::vector<std::string> e = list_dir(g_dir);
          return e.size() == 1 && read_all(g_dir + "/" + e[0]).size() == 4096;
        },
        5000);
    check(cut_seen,
          "limittrunc: 前置 —— 第一笔必须已被消费到截断点（盘上正好 4096 字节）");
    check_eq_i(count_responses(c.rx()), 0,
               "limittrunc: 前置 —— 报文还没结束，此刻不该有响应");

    // 把第一笔剩下的字节读完：第一笔带了 6144 > 4096 字节 body 且不含边界，
    // 读完之后状态机就**确定**停在 `PART_BODY_SKIP`（不是"大概在"）。
    c.pump(400);

    check(c.write(body.substr(cut)),
          "limittrunc: 第二笔（其余的 body）必须写出去");

    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 8000);
    c.pump(300);

    check_eq_i(p->handler_called, 1, "limittrunc: handler 恰好一次");
    check_eq_i(p->ended, 1, "limittrunc: on_end 恰好一次");
    check_eq_i(p->aborted, 0, "limittrunc: 截断**不是**失败，不该走 abort");
    check_eq_i(count_responses(c.rx()), 1, "limittrunc: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 200", "limittrunc: 截断照常 200");
    check_contains(c.rx(), "F|doc|big.bin|application/octet-stream|4096|1",
                   "limittrunc: 元数据里的 size 是截断后的长度，且 truncated=1");
    check_contains(c.rx(), "D|after|ok",
                   "limittrunc: 转 SKIP 之后仍要扫边界 —— 后面的部件照常解析");

    check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 1,
               "limittrunc: 截断的文件必须留着交给 handler");
    if (p->file_list.size() == 1) {
      check_bytes(read_all(p->file_list[0].path()), data.substr(0, 4096),
                  "limittrunc: 盘上必须是原文的**前 4096 字节**，不多不少");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 18. limits_file_count —— 文件部件数超限 ⇒ 413 + close
//
// 上限是 2，发 3 个：**第 3 个**才越界。判在 `on_part_begin` 之后（数量检查在
// `sink_->on_part_begin()` **之前**），所以第 3 个部件的 body 一个字节都不会落盘。
// =========================================================================
void test_limits_file_count() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  app.set_max_upload_files(2);
  add_upload_route(app, "/upload", p);

  check(app.start_background() == 0, "limitfiles: 服务启动");
  const int port = app.bound_port();

  std::vector<std::string> parts;
  for (int i = 0; i < 3; ++i) {
    parts.push_back(file_part("f" + std::to_string(i), "x.bin",
                              "application/octet-stream",
                              make_payload(512, 50 + i)));
  }
  const std::string body = mp_body(kBoundary, parts);

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/upload", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "limitfiles: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 8000);
    c.pump(300);

    check_eq_i(p->aborted, 1, "limitfiles: 用户 on_abort 恰好一次");
    check_eq_i(p->ended, 0, "limitfiles: 用户的 on_end 不能触发");
    check_eq_i(count_responses(c.rx()), 1, "limitfiles: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 413", "limitfiles: 状态码 413");
    check_contains(c.rx(), "connection: close", "limitfiles: 必须要求关闭连接");
    check(wait_for([&]() { return list_dir(g_dir).empty(); }, 3000),
          "limitfiles: 超限后前两个已经建了的文件也要删掉");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 19. limits_field_count —— 字段部件数超限 ⇒ 413 + close
//
// 与上面分开：字段部件**不建文件**，走的是内存累积那条路。合成一组的话，
// "文件数判了、字段数没判"的实现会被文件数那条断言掩盖过去。
// =========================================================================
void test_limits_field_count() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  app.set_max_form_fields(2);
  add_upload_route(app, "/upload", p);

  check(app.start_background() == 0, "limitfields: 服务启动");
  const int port = app.bound_port();

  std::vector<std::string> parts;
  for (int i = 0; i < 3; ++i) {
    parts.push_back(field_part("k" + std::to_string(i), "v"));
  }
  const std::string body = mp_body(kBoundary, parts);

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/upload", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "limitfields: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 8000);
    c.pump(300);

    check_eq_i(p->aborted, 1, "limitfields: 用户 on_abort 恰好一次");
    check_eq_i(p->ended, 0, "limitfields: 用户的 on_end 不能触发");
    check_eq_i(count_responses(c.rx()), 1, "limitfields: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 413", "limitfields: 状态码 413");
    check_contains(c.rx(), "connection: close",
                   "limitfields: 必须要求关闭连接");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 20. limits_field_size —— 单字段超限 ⇒ 413 + close
//
// 字段与文件的处理**故意不同**：文件能截断（半截文件仍然有业务价值），字段
// 是攒在内存里的一个值，半截的值是**错的**而不是"少一点" —— 所以字段直接拒。
// 这一组就是钉这个区别的。
// =========================================================================
void test_limits_field_size() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  app.set_max_field_size(64);
  add_upload_route(app, "/upload", p);

  check(app.start_background() == 0, "limitfieldsize: 服务启动");
  const int port = app.bound_port();

  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(1, field_part("note", make_payload(4096, 53))));

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/upload", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "limitfieldsize: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 8000);
    c.pump(300);

    check_eq_i(p->aborted, 1, "limitfieldsize: 用户 on_abort 恰好一次");
    check_eq_i(p->ended, 0, "limitfieldsize: 用户的 on_end 不能触发");
    check_eq_i(count_responses(c.rx()), 1, "limitfieldsize: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 413", "limitfieldsize: 状态码 413");
    check_contains(c.rx(), "connection: close",
                   "limitfieldsize: 必须要求关闭连接");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 21. limits_part_header —— 部件头超长 ⇒ **400**（不是 413）+ close
//
// 与上面四组的状态码**故意不同**：文件数 / 字段数 / 单字段 / 总长都是"你给的
// 东西太大"，是 413；而部件头超长是"你的报文本身不成形"，是 400。把两张表
// 合成一张，排障的人就会往错的方向找。
// =========================================================================
void test_limits_part_header() {
  purge_dir(g_dir);
  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  app.set_upload_dir(g_dir);
  app.set_max_part_header_bytes(256);
  add_upload_route(app, "/upload", p);

  check(app.start_background() == 0, "limitphdr: 服务启动");
  const int port = app.bound_port();

  // 一个 1 KiB 的文件名 —— 整个部件头因此远超 256 字节的限额，但**报文本身
  // 是合法的**：这正是"超长"与"畸形"要分开的地方。
  const std::string big_name(1024, 'n');
  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(
          1, file_part("doc", big_name, "application/octet-stream", "hi")));

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/upload", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "limitphdr: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 8000);
    c.pump(300);

    check_eq_i(p->aborted, 1, "limitphdr: 用户 on_abort 恰好一次");
    check_eq_i(p->ended, 0, "limitphdr: 用户的 on_end 不能触发");
    check_eq_i(count_responses(c.rx()), 1, "limitphdr: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 400",
                   "limitphdr: 部件头超长是报文不成形，必须是 400 而不是 413");
    check_contains(c.rx(), "connection: close",
                   "limitphdr: 必须要求关闭连接");
    check_eq_i(static_cast<long long>(list_dir(g_dir).size()), 0,
               "limitphdr: 头都没解析出来，盘上不该有任何文件");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 22. upload_dir_in_static_root —— 上传目录落在静态文档根**内** ⇒ 500 拒绝
//
// 这条守的是配置期的安全错误里最坏的一种：上传目录是静态文档根的子目录，
// 于是**上传成功即可被任意 GET 拿到**（"用户上传一个 .html/.svg 就能挂到
// 站点上"是同一族的经典攻击）。框架在 `set_upload_dir()` / `serve_static()` /
// `start()` 三处任一"两者都已可知"的时刻做一次包含判断，命中就置
// `upload_dir_unsafe_`，请求进来时整条上传路径被拒。
//
// 为什么断言里必须带上**那条消息的原文**
// --------------------------------------
// `wire_upload()` 里有**两个** 500 分支：`upload_dir_.empty()`（用户忘了配）
// 和本组要打的 `upload_dir_unsafe_`。两者的线上形状完全一样 —— 500 +
// `connection: close` + handler 不跑 + 盘上无文件。只断那些的话，一个
// "`set_upload_dir()` 根本没生效"的实现照样全绿，而它测的根本不是本组的
// 那条不变式。所以这里额外断言响应体里有"静态文档根"这段文字 ——
// `reject_upload()` 把消息放进 body，它就是两个分支之间唯一可观测的区别。
//
// 前置断言（`GET /assets/public.txt` 必须 200）为什么也在
// ------------------------------------------------------
// 静态根没挂上 ⇒ `static_roots_real_` 为空 ⇒ 包含判断无从谈起。这时 **500 不会
// 出现**（上传会成功），所以本组不会因此假绿。但那句前置断言把"根真的在提供
// 服务"这件事钉死，从而说明下面拒绝掉的是**一次真实的暴露**，而不是一句空话。
// =========================================================================
void test_upload_dir_in_static_root() {
  const std::string root = "uvcpp_rt_static_root";
  const std::string inner = root + "/uploads";
  const std::string marker = "PUBLIC-MARKER";

  check(ensure_dir(root), "instatic: 建文档根");
  check(ensure_dir(inner), "instatic: 建根内的上传目录");
  purge_dir(inner);
  {
    std::ofstream f((root + "/public.txt").c_str(), std::ios::binary);
    f << marker;
  }

  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  // 次序刻意如此：先挂静态根、再配上传目录。反过来的次序走的是
  // `serve_static()` 那一侧的检查，两条路都要有人走（第二次改这组时把次序
  // 换一下即可覆盖另一半）。
  app.serve_static("/assets", root);
  app.set_upload_dir(inner);
  add_upload_route(app, "/upload", p);

  check(app.start_background() == 0, "instatic: 服务启动");
  const int port = app.bound_port();

  // ---- 前置：静态根真的在服务（否则下面的 500 与这条配置错误无关）----
  {
    staged_conn g;
    if (g.open(port)) {
      check(g.write("GET /assets/public.txt HTTP/1.1\r\nHost: t\r\n\r\n"),
            "instatic: 静态请求必须写出去");
      g.pump_until([&]() { return count_responses(g.rx()) > 0; }, 5000);
      g.pump(200);
      check_contains(g.rx(), "HTTP/1.1 200",
                     "instatic: 前置 —— 文档根必须在服务（否则本组测不到东西）");
      check_contains(g.rx(), marker,
                     "instatic: 前置 —— 服务的就是我们放的那个文件");
    }
  }

  // ---- 正题：往根内的上传目录上传，必须被拒 ----
  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(
          1, file_part("f", "evil.html", "text/html", "<h1>pwned</h1>")));

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/upload", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "instatic: 上传请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(300);

    check_eq_i(p->handler_called, 0, "instatic: 用户的 handler 不该跑");
    check_eq_i(p->ended, 0, "instatic: 用户的 on_end 不该跑");
    check_eq_i(count_responses(c.rx()), 1, "instatic: 线上恰好一个响应");
    check_contains(c.rx(), "HTTP/1.1 500",
                   "instatic: 配置错必须是 5xx，不是请求错");
    check_contains(c.rx(), "connection: close", "instatic: 必须要求关闭连接");
    // **这一条是判别性的**：把本组与"忘了配 upload_dir"那个同形状的 500
    // 分开。少了它，本组会被另一个分支蒙过去。
    check_contains(c.rx(), "静态文档根",
                   "instatic: 拒绝的理由必须是'落在静态文档根里'，"
                   "而不是另一个同形状的 500（upload_dir 为空）");
    check_eq_i(static_cast<long long>(list_dir(inner).size()), 0,
               "instatic: 上传目录必须还是空的");
    // 顺带看一眼：拒绝之后那个目录**照样是静态可读的** —— 正是这条检查要防的
    // 那个后果。上传目录在根内，所以 `/assets/uploads/...` 是可访问路径。
    check_eq_i(static_cast<long long>(list_dir(root).size()), 2,
               "instatic: 文档根里应当只有 public.txt 与 uploads 两项");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 23. upload_dir_outside_root_ok —— **对照**：上传目录挪出根 ⇒ 照常 200
//
// 没有这一组，一个"见到 `serve_static()` 就把上传一律拒掉"的实现能让上面那组
// 全绿 —— 那测的就不是包含判断，而是"挂过静态根就不许上传"。两组合起来才钉住
// 「**在根内**才拒绝」这个不等式。
//
// 对照组自己的前置同样必需：静态根必须真的在服务。否则"根压根没挂上"的实现
// 也能让本组通过，而它并没有证明包含判断在**不该触发**的时候不触发。
// =========================================================================
void test_upload_dir_outside_root_ok() {
  const std::string root = "uvcpp_rt_static_root";
  const std::string outside = "uvcpp_rt_outside_uploads";
  const std::string data = make_payload(4096, 23u);

  check(ensure_dir(root), "outside: 建文档根");
  check(ensure_dir(outside), "outside: 建根外的上传目录");
  purge_dir(outside);
  {
    std::ofstream f((root + "/public.txt").c_str(), std::ios::binary);
    f << "PUBLIC-MARKER";
  }

  std::shared_ptr<up_probe> p(new up_probe());

  uvcpp_web_app app;
  configure_for_test(app);
  app.serve_static("/assets", root);
  app.set_upload_dir(outside);
  add_upload_route(app, "/upload", p);

  check(app.start_background() == 0, "outside: 服务启动");
  const int port = app.bound_port();

  {
    staged_conn g;
    if (g.open(port)) {
      check(g.write("GET /assets/public.txt HTTP/1.1\r\nHost: t\r\n\r\n"),
            "outside: 静态请求必须写出去");
      g.pump_until([&]() { return count_responses(g.rx()) > 0; }, 5000);
      g.pump(200);
      check_contains(g.rx(), "HTTP/1.1 200",
                     "outside: 前置 —— 文档根必须在服务");
    }
  }

  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>(
          1, file_part("f", "ok.bin", "application/octet-stream", data)));

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/upload", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "outside: 上传请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 8000);
    c.pump(300);

    check_eq_i(p->handler_called, 1, "outside: handler 恰好一次");
    check_contains(c.rx(), "HTTP/1.1 200",
                   "outside: 上传目录在根外时必须照常接受");
    check_eq_i(static_cast<long long>(list_dir(outside).size()), 1,
               "outside: 恰好一个文件落盘");
    const std::vector<std::string> e = list_dir(outside);
    if (e.size() == 1) {
      check_bytes(read_all(outside + "/" + e[0]), data,
                  "outside: 盘上内容必须逐字节等于原文");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 24. buffered_multipart_form —— 普通路由上的 multipart **进内存**
//
// 步骤 8 补的窟窿：`req.form("x")` 对一份完全合法的 multipart 表单原本**静默
// 返回空** —— 不是"没这个字段"，而是"框架装作它是个空表单"。这一组钉的是
// 它现在真的有值，而且文本字段与文件部件走的是**同一次**解析。
//
// 这一组同时钉住一条**不该变**的事：普通路由**不**因为报文是 multipart 就
// 变成流式路由。`req.stream()` 与 `req.upload()` 必须照旧为空 —— 少了这几条，
// 一个"见到 multipart 就认领"的实现会让上面的断言全绿，而它已经把这条路由
// 从"整包攒 body"改成了另一套生命周期（用户的 `handler` 会在 headers 时被调用，
// `body_str()` 恒空）。那是步骤 4 那条 `plain_route_untouched` 的不变式，这里
// 用 multipart 报文再钉一遍。
//
// 文件名要**逐字节**断言成清洗后的叶子 —— 与 `uvcpp_web_upload_file::
// original_filename()` 同一把尺子（`web_sanitize_filename()`）。同一个概念
// 在两条路径上给两种语义是个陷阱，这一组就是钉它。
//
// **两个部件、两种线上形状，因为它们走的是两段不同的代码**
// ------------------------------------------------------
// 第一版我只发了 `filename="..\..\pic.PNG"`，断言叶子是 `pic.PNG`，跑出来是
// `....pic.PNG`。**这不是 `web_sanitize_filename()` 的 bug**：部件头里的
// 引号串要按 RFC 2616 的 quoted-pair 解转义（`\X` → `X`），所以那一串在
// **解析器眼里**本来就是 `....pic.PNG` —— 四个点、没有分隔符，清洗函数原样
// 返回才是对的。真正想表达"客户端绕了一圈"必须把反斜杠**在线上写成 `\\`**。
//
// 于是这一组拆成两个部件，各自钉住一半：
//   - `avatar` 用正斜杠 `../../pic.PNG` —— 不经过 quoted-pair，直接考清洗；
//   - `doc` 在线上发 `..\\..\\notes.TXT`（C++ 源里是四个反斜杠）——
//     **先考解转义、再考清洗**，两段接起来才得到 `notes.TXT`。
// 只发前者的话，"解析器不做解转义"或"sink 忘了清洗"各自都能蒙混过去一半。
// =========================================================================
void test_buffered_multipart_form() {
  const std::string data = make_payload(2048, 41u);

  int calls = 0;
  bool ok_flag = false;
  bool is_mp = false;
  bool upload_null = false;
  bool stream_null = false;
  size_t field_count = 0;
  size_t file_count = 0;
  std::string got_name;
  std::string got_note;
  std::string got_data;
  std::string got_filename;
  std::string got_ctype;
  std::string got_file_field;
  std::string got_doc_filename;
  std::string got_doc_data;

  uvcpp_web_app app;
  configure_for_test(app);

  // **普通** `post()`。步骤 8 改的正是这条路径上的请求对象，不是 upload_route。
  app.post("/form", [&](uvcpp_web_request& req, uvcpp_web_response& resp,
                        uvcpp_web_next) {
    calls += 1;
    is_mp = req.is_multipart();
    ok_flag = req.multipart_ok();
    upload_null = (req.upload() == nullptr);
    stream_null = (req.stream() == nullptr);
    field_count = req.form_params().size();
    file_count = req.files().size();

    const std::string* n = req.form("name");
    if (n != nullptr) got_name = *n;
    const std::string* t = req.form("note");
    if (t != nullptr) got_note = *t;

    const uvcpp_web_form_file* f = req.file("avatar");
    if (f != nullptr) {
      got_data = f->data;
      got_filename = f->filename;
      got_ctype = f->content_type;
      got_file_field = f->name;
    }
    const uvcpp_web_form_file* d = req.file("doc");
    if (d != nullptr) {
      got_doc_filename = d->filename;
      got_doc_data = d->data;
    }

    // 不存在的字段名必须是 nullptr（不是空串）—— 与 `form()` 的既有约定一致。
    if (req.file("nope") == nullptr && req.form("nope") == nullptr) {
      resp.text("form-ok");
    } else {
      resp.text("form-BAD");
    }
    resp.end();
  });

  check(app.start_background() == 0, "buffered: 服务启动");
  const int port = app.bound_port();

  const std::string body = mp_body(
      kBoundary,
      std::vector<std::string>{
          field_part("name", "alice"),
          file_part("avatar", "../../pic.PNG", "image/png", data),
          // 线上是 `filename="..\\..\\notes.TXT"` —— 解转义后 `..\..\notes.TXT`。
          file_part("doc", "..\\\\..\\\\notes.TXT", "text/plain", "NOTE"),
          field_part("note", "hello")});

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/form", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "buffered: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(200);

    check_eq_i(calls, 1, "buffered: 用户的 handler 恰好一次");
    check_contains(c.rx(), "HTTP/1.1 200", "buffered: 必须是 200");
    check_contains(c.rx(), "form-ok",
                   "buffered: 不存在的字段名必须回 nullptr");

    check(is_mp, "buffered: 请求必须被识别为 multipart");
    check(ok_flag, "buffered: 报文完好，multipart_ok() 必须为真");
    check(upload_null, "buffered: 普通路由上 req.upload() 必须仍为空");
    check(stream_null,
          "buffered: 普通路由不得因为报文是 multipart 就变成流式路由");
    check_eq_i(static_cast<long long>(field_count), 2,
               "buffered: 两个文本字段");
    check_eq_i(static_cast<long long>(file_count), 2, "buffered: 两个文件部件");

    check_eq_s(got_name, "alice", "buffered: req.form(\"name\")");
    check_eq_s(got_note, "hello", "buffered: req.form(\"note\")");
    check_eq_s(got_file_field, "avatar", "buffered: 文件部件的字段名");
    check_eq_s(got_ctype, "image/png", "buffered: 文件部件的 Content-Type");
    check_eq_s(got_filename, "pic.PNG",
               "buffered: 客户端给的 `../../pic.PNG` 必须是清洗后的叶子");
    // 线上 `..\\..\\notes.TXT` → 解转义 `..\..\notes.TXT` → 清洗成叶子。
    // 断的是**两段接起来的结果**，所以"sink 忘了清洗"会在这里现形。
    check_eq_s(got_doc_filename, "notes.TXT",
               "buffered: 线上转义过的反斜杠路径也必须落到叶子上");
    check_eq_s(got_doc_data, "NOTE",
               "buffered: 第二个文件部件的内容也要跟着字段名对齐（不是串位）");
    check_bytes(got_data, data,
                "buffered: 文件部件的内容必须逐字节等于原文（二进制安全）");
  }

  app.stop();
  app.join();
}

// =========================================================================
// 25. buffered_multipart_bad —— **对照**：报文坏了 ⇒ `multipart_ok()` 为假
//
// 没有这一组，一个"解析失败就交半份结果"的实现能让上面那组全绿 —— 而半份结果
// 是这一族里最危险的形状：handler 拿到的值**看起来完全合法**，只是被截断了。
//
// 判据是**三条一起**：`multipart_ok()` 为假、两个容器都空、`form()` 返回
// nullptr。只断第一条的话，"置个假标志但照样把半份数据交出去"能过；只断后两条
// 的话，"压根没解析（本来就全空）"能过 —— 而那正是步骤 8 要修的那个窟窿。
// =========================================================================
void test_buffered_multipart_bad() {
  int calls = 0;
  bool ok_flag = true;
  size_t field_count = 9;
  size_t file_count = 9;
  bool form_null = false;

  uvcpp_web_app app;
  configure_for_test(app);

  app.post("/badform", [&](uvcpp_web_request& req, uvcpp_web_response& resp,
                           uvcpp_web_next) {
    calls += 1;
    ok_flag = req.multipart_ok();
    field_count = req.form_params().size();
    file_count = req.files().size();
    form_null = (req.form("name") == nullptr);
    resp.text("bad-form-seen");
    resp.end();
  });

  check(app.start_background() == 0, "badform: 服务启动");
  const int port = app.bound_port();

  // boundary 声明了，但 body 里**没有终边界** —— 报文被掐断。这是
  // `finish()` 的 `ERROR_BOUNDARY_NEVER_FOUND` 那一支，也是"上传被中断"在
  // 缓冲路径上的对应物。
  const std::string body =
      "--" + std::string(kBoundary) +
      "\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nalice\r\n";

  staged_conn c;
  if (c.open(port)) {
    check(c.write(mp_head("/badform", kBoundary, body.size(),
                          "multipart/form-data") +
                  body),
          "badform: 请求必须写出去");
    c.pump_until([&]() { return count_responses(c.rx()) > 0; }, 5000);
    c.pump(200);

    check_eq_i(calls, 1, "badform: handler 恰好一次");
    check_contains(c.rx(), "HTTP/1.1 200",
                   "badform: 缓冲路径上报文坏了不改变 HTTP 状态码 —— "
                   "请求早就整包收下了，判不判由 handler 决定");
    check(!ok_flag, "badform: 报文被掐断 ⇒ multipart_ok() 必须为假");
    check_eq_i(static_cast<long long>(field_count), 0,
               "badform: 解析失败时字段容器必须是空的（不能交半份）");
    check_eq_i(static_cast<long long>(file_count), 0,
               "badform: 解析失败时文件容器必须是空的");
    check(form_null,
          "badform: 那个'看起来合法'的截断值必须取不到 —— "
          "它正是这一族里最危险的形状");
  }

  app.stop();
  app.join();
}

}  // namespace

int main(int argc, char** argv) {
  std::string only;
  if (argc > 1) only = argv[1];

  g_dir = "uvcpp_upload_route_dir";
  if (!ensure_dir(g_dir)) {
    std::cerr << "[upload_route] 无法建立上传目录: " << g_dir << std::endl;
    return 2;
  }
  // 建好之后换成**真实路径**。框架把 `file.path()` 建在目录的真实路径上
  // （`uvcpp_web_upload_file::path()` 与 `uvcpp_web_app::set_upload_dir()` 都把
  // 解析结果当唯一基准），而这里好几处断言是"路径以 `g_dir` 开头 + 只有一层
  // 叶子"。拿相对名去比前缀会得到一串假失败，而且**看起来像穿越成功了** ——
  // 那是最坏的一种假失败（安全断言报错，问题却在用例自己）。
  //
  // 顺带这也让这一组真正覆盖了"配置里给的是相对路径"这个常见写法：真实路径是
  // 框架自己解析出来的，用例用同一份结果比对。
  {
    std::string real;
    if (web_real_path(g_dir, real, /*allow_missing=*/false) && !real.empty())
      g_dir = real;
  }
  purge_dir(g_dir);

  struct case_entry {
    const char* name;
    void (*fn)();
  };
  const case_entry cases[] = {
      {"single_file", test_single_file},
      {"quoted_boundary", test_quoted_boundary},
      {"multi_part_fields", test_multi_part_fields},
      {"binary_body", test_binary_body},
      {"empty_file", test_empty_file},
      {"fields_only_no_files", test_fields_only_no_files},
      {"split_writes", test_split_writes},
      {"filename_metadata", test_filename_metadata},
      {"reject_415", test_reject_415},
      {"reject_400_no_boundary", test_reject_400_no_boundary},
      {"reject_500_no_dir", test_reject_500_no_dir},
      {"plain_route_untouched", test_plain_route_untouched},
      {"disconnect_deletes_temp", test_disconnect_deletes_temp},
      {"keepalive_after_upload", test_keepalive_after_upload},
      {"flow_backpressure_wired", test_flow_backpressure_wired},
      {"limits_total", test_limits_total},
      {"limits_file_truncate", test_limits_file_truncate},
      {"limits_file_count", test_limits_file_count},
      {"limits_field_count", test_limits_field_count},
      {"limits_field_size", test_limits_field_size},
      {"limits_part_header", test_limits_part_header},
      {"upload_dir_in_static_root", test_upload_dir_in_static_root},
      {"upload_dir_outside_root_ok", test_upload_dir_outside_root_ok},
      {"buffered_multipart_form", test_buffered_multipart_form},
      {"buffered_multipart_bad", test_buffered_multipart_bad},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (!only.empty() &&
        std::string(cases[i].name).find(only) == std::string::npos) {
      continue;
    }
    const int before = g_failures;
    std::cout << "[upload_route] " << cases[i].name << std::endl;
    cases[i].fn();
    const bool ok = (g_failures == before);
    std::cout << (ok ? "  -> PASS" : "  -> FAIL") << std::endl;
    if (!ok) {
      std::cout << "[upload_route] FAIL" << std::endl;
      return 2;
    }
  }

  std::cout << "[upload_route] ALL PASS" << std::endl;
  return 0;
}

#else  // !UVCPP_WEBAPP_ENABLE

int main() {
  std::cout << "[upload_route] SKIP (UVCPP_WEBAPP_ENABLE=0)" << std::endl;
  return 0;
}

#endif
