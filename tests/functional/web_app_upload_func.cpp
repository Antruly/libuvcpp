/**
 * @file tests/functional/web_app_upload_func.cpp
 * @brief Phase 3b 步骤 3 —— `uvcpp_web_upload` 磁盘 sink 的功能测试。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么是"真循环 + 真磁盘"，而不是打桩
 * -------------------------------------
 * 这一步要证的只有一件事：**收到的部件字节逐字节、不多不少地落在盘上**。
 * 而这条链上每一环都可能悄悄改字节 —— `uv_buf_t` 指向的内存被写盘期间改动、
 * 短写没接住、偏移算错、双缓冲换错槽。这些**没有一个**能在打桩的 fs 上暴露：
 * 桩里 `write` 是立即"成功"的，根本没有"在途"这个状态。
 *
 * 所以这里跑的是真 `uvcpp_loop`、真线程池、真文件系统，收尾时用**独立的**
 * 读路径（`std::ifstream`）把文件读回来跟原文逐字节比对。
 *
 * 本步**不做**的事（都有意留给后面的步骤）
 * ----------------------------------------
 * 没有服务器、没有路由、没有背压、没有上限、没有文件名清洗。所以这里直接用
 * `uvcpp_web_multipart` 喂 `uvcpp_web_upload`，**不经过任何 web 层**：
 * 这一步的断言只该被"落盘"这件事影响，混进路由或 HTTP 的失败会让定位变难。
 *
 * 最有价值的三条
 * --------------
 *   - `async_offload`：喂完整个 body、**一次循环都不跑**时，盘上必须一个字节
 *     都没有。这是"耗时操作不在主循环上做"唯一可判定的形式 —— 若实现里任何
 *     一处偷偷用了同步 `uv_fs_*`，这条会立刻红。
 *   - `binary_body`：1 MiB 含 NUL/0xFF/CRLF 的载荷，分块喂、边喂边泵。
 *   - `abort_deletes_completed_file`：**已经写完并已交给调用方**的文件，在
 *     整次上传失败后也必须被删掉 —— 那是「失败时框架删」这条决策的唯一证据。
 *
 * 命名同时命中 `web_.*\.cpp$` 与 `web_app_.*\.cpp$` 两条过滤器，所以
 * 「web 关」和「webapp 关」两种配置下都会被正确摘掉（详见
 * `tests/functional/CMakeLists.txt` 里那段注释：名字起错会让用例在
 * `UVCPP_BUILD_WEBAPP=OFF` 时留下来，然后**链接失败**）。
 *
 * 跑法：`test_web_app_upload_func.exe [子串过滤]`
 */

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEBAPP_ENABLE

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

// 目录操作要用系统调用：`std::filesystem` 是 C++17，而本库锁在 C++11。
// 这两个头必须放在**文件顶部**，不能塞进匿名命名空间里 —— 在 namespace 里
// include 系统头会把它们的声明拖进那个 namespace（今天能编过纯粹是因为
// include guard 已经被上游拉过一次了，属于运气）。
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>   // rmdir（`sys/stat.h` 只给 mkdir）
#endif

#include "handle/uvcpp_check.h"
#include "handle/uvcpp_handle.h"
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_timer.h"

// 只为 `http_status::PAYLOAD_TOO_LARGE` —— 越界那条断言要钉的是"**413**"这个
// 具体数字，而不是"某个非零状态码"（回错码的实现必须被抓住）。
#include <web/uvcpp_http_common.h>

#include <webapp/uvcpp_web_multipart.h>
#include <webapp/uvcpp_web_upload.h>
#include <webapp/uvcpp_web_util.h>
#include <webapp/uvcpp_web_work_limit.h>

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

/// 比对两个可能很长的字节串，失败时只报"第几个字节不同"，不把整个载荷
/// 打到输出里（1 MiB 的失败信息会让 ctest 日志没法看）。
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
                << " 个字节不同：期望 " << static_cast<int>(
                       static_cast<unsigned char>(want[i]))
                << "，实际 "
                << static_cast<int>(static_cast<unsigned char>(got[i]))
                << std::endl;
      ++g_failures;
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// 目录工具
//
// 用 **同步** 的 `uv_fs_scandir` / `std::remove`，而且只在**循环没在跑**的时候
// 调（用例的准备与收尾阶段）。生产代码里这么写是不行的 —— 这里可以，因为它
// 是**判据**的一部分：只有用一条跟被测路径完全无关的读路径去数文件，才能证明
// "框架真的删了"，而不是"框架说它删了"。
// ---------------------------------------------------------------------------

uv_loop_t* raw_loop(uvcpp_loop* l) { return OBJ_UVCPP_LOOP_HANDLE(*l); }

std::vector<std::string> list_dir(uvcpp_loop* l, const std::string& path) {
  std::vector<std::string> out;
  uv_fs_t req;
  const int n = uv_fs_scandir(raw_loop(l), &req, path.c_str(), 0, nullptr);
  if (n >= 0) {
    uv_dirent_t ent;
    while (uv_fs_scandir_next(&req, &ent) != UV_EOF) {
      out.push_back(std::string(ent.name));
    }
  }
  uv_fs_req_cleanup(&req);
  return out;
}

/// 清空目录内容（不删目录本身）。用例跑挂了就不会走到收尾，所以下一次运行
/// 必须先能自愈 —— 与 `web_app_static_func.cpp` 的 `ensure_dir()` 同一个理由。
void purge_dir(uvcpp_loop* l, const std::string& path) {
  std::vector<std::string> names = list_dir(l, path);
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
#define UP_MKDIR(p) _mkdir(p)
#define UP_RMDIR(p) _rmdir(p)
#else
#define UP_MKDIR(p) mkdir((p), 0755)
#define UP_RMDIR(p) rmdir(p)
#endif

bool ensure_dir(const std::string& p) {
  if (UP_MKDIR(p.c_str()) == 0) return true;
  // 已存在也算成功：跑挂一次留下的旧目录不该让后续每一次运行都起不来。
  //
  // 判据用 `errno == EEXIST` 而不是"列一下看能不能列出来" —— 后者要
  // `uv_fs_scandir`，而那是**同步**调用，需要一个 loop。为一个建目录的动作
  // 拉一个 `uv_default_loop()` 进来，等于让进程多一个永远不关的 loop。
  return errno == EEXIST;
}

/// 目录**确实不存在**吗。
///
/// 用"删一下看报不报 ENOENT"来判，而不是 `list_dir()` 是否为空 ——
/// `uv_fs_scandir` 对"不存在"和"存在但为空"返回的都是空列表，那个判据
/// 恒真，当前置用等于没断言。
bool dir_absent(const std::string& p) {
  if (UP_RMDIR(p.c_str()) == 0) {
    UP_MKDIR(p.c_str());  // 居然删掉了 = 它本来就在，复原后如实返回假
    return false;
  }
  return errno == ENOENT;
}

// ---------------------------------------------------------------------------
// 载荷：每个位置的值与下标有关，于是"偏移错"与"长度截断"都会表现为内容不等，
// 而不是碰巧相等。刻意钉上 NUL / 0xFF / CRLF。
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// 落盘叶子名的形状
//
// `16 位小写十六进制` + 可选的 `.<白名单过滤后的扩展名>`。这个形状**本身就是
// 一条安全断言**：它证明落盘名是框架摇的，与客户端给的名字一个字节的关系都
// 没有（客户端给的名字不可能恰好是这个形状）。所以它是"客户端决定不了自己
// 落在盘上的名字"这句话里可判定的那一半。
//
// 扩展名那一段不是装饰：`pick_extension()` 的白名单与长度上限要有判据，否则
// 一个"把客户端给的整个名字当扩展名接上去"的实现也能过 —— 而那个实现会让
// 落盘名重新受客户端控制。
// ---------------------------------------------------------------------------
bool leaf_shape_ok(const std::string& leaf) {
  if (leaf.size() < 16) return false;
  for (size_t i = 0; i < 16; ++i) {
    const char c = leaf[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  if (leaf.size() == 16) return true;
  if (leaf[16] != '.') return false;
  const size_t ext_len = leaf.size() - 17;
  if (ext_len == 0 || ext_len > 10) return false;  // 空扩展名与超长都要挡住
  for (size_t i = 17; i < leaf.size(); ++i) {
    const char c = leaf[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9'))) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// 报文构造
// ---------------------------------------------------------------------------

const char* k_boundary = "----uvcppUploadBoundary7f3a91";

std::string mp_file_part(const std::string& name, const std::string& filename,
                         const std::string& mime, const std::string& data) {
  return "Content-Disposition: form-data; name=\"" + name +
         "\"; filename=\"" + filename + "\"\r\nContent-Type: " + mime +
         "\r\n\r\n" + data;
}

std::string mp_field_part(const std::string& name, const std::string& value) {
  return "Content-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" +
         value;
}

std::string mp_body(const std::string& boundary,
                    const std::vector<std::string>& parts, bool terminate) {
  std::string b;
  for (size_t i = 0; i < parts.size(); ++i) {
    b += "--" + boundary + "\r\n";
    b += parts[i];
    b += "\r\n";
  }
  if (terminate) b += "--" + boundary + "--\r\n";
  return b;
}

// ---------------------------------------------------------------------------
// 场景骨架：一个 loop + 一个看门狗。
//
// 看门狗把"落盘链没跑完"变成**确定的失败**而不是永久挂起 —— 这一步里最可能
// 的挂法恰恰是"提交了 fs 操作但回调永远不来"（比如把单笔在途守卫弄错），
// 那种情况下 `uv_run` 会空转、ctest 到超时才报，定位成本极高。
//
// **但它是一次性的，而且 `uv_run` 退出时会清掉 `stop_flag`。** 这两件事合起来
// 有一个很坏的后果：看门狗如果在**中途**（喂数据那一圈、或下面等落盘那一圈）
// 烧掉了，它那个 `loop_.stop()` 就被那一拍的 `UV_RUN_NOWAIT` **消费并清空**了，
// 收尾那一圈的 `run(UV_RUN_DEFAULT)` 于是**没有任何人来叫停**。而收尾那一圈
// 挂着一个**一直活跃**的采样句柄（定时器也好、`uv_check` 也好），
// `uv__loop_alive` 恒为真 —— 于是不是"看门狗把它抓住"，而是**永久挂住**。
//
// 实测（U8：把异步提交的回调换成空函数，链永远不推进）：scoped 跑 `single_chunk`
// 是 15s 后正常 FAIL（那一圈看门狗还活着），**全量跑却挂了 14 分钟**，一路挂到
// 我手动把进程杀了 —— 因为全量里排在前面的是 `abort_deletes_completed_file`，
// 它开着 `settle_before_finish`（见下，整整 78 秒），15s 的看门狗在那里面就烧掉了。
//
// 所以"收尾这一圈必须**自己重新装一次**看门狗"，见 `arm_watchdog()`。
// ---------------------------------------------------------------------------
class upload_env {
 public:
  upload_env(const char* name, int timeout_ms)
      : name_(name), timeout_ms_(timeout_ms), timed_out_(false), loop_() {
    loop_.init();
    watchdog_.reset(new uvcpp_timer(&loop_));
    arm_watchdog();
  }

  /**
   * @brief 装（或重新装）看门狗，`timeout_ms_` 之后叫停循环。
   *
   * 从**回调外面**调是安全的：`uvcpp_timer::start` 会赋值 `timer_start_cb`，
   * 而仓库里那个 `std::function` 自赋值陷阱只发生在**从自己的回调里**再次
   * `start()`（那时正在执行的闭包被 `operator=` 析构）。这里不是那种形状。
   */
  void arm_watchdog() {
    watchdog_->start(
        [this](uvcpp_timer*) {
          timed_out_.store(true);
          std::cerr << "  [FAIL] " << name_ << ": 看门狗超时（" << timeout_ms_
                    << "ms）—— 落盘链没有跑完" << std::endl;
          loop_.stop();
        },
        static_cast<uint64_t>(timeout_ms_), 0);
  }

  uvcpp_loop* loop() { return &loop_; }
  bool timed_out() const { return timed_out_.load(); }

 private:
  std::string name_;
  int timeout_ms_;
  std::atomic<bool> timed_out_;
  uvcpp_loop loop_;
  std::unique_ptr<uvcpp_timer> watchdog_;
};

// ---------------------------------------------------------------------------
// 驱动器
// ---------------------------------------------------------------------------

/**
 * @brief 假的流控通道：只数数，不碰任何真流对象。
 *
 * 这是 `uvcpp_web_upload_flow` 存在的**全部理由**的落地 —— 会话要能脱开服务器
 * 单独测，而"背压有没有真的下发"必须是个可观测的事实，不能靠推理。
 */
struct flow_probe {
  flow_probe()
      : pause(0), resume(0), abort_calls(0), abort_status(0), done(false) {}

  int pause;
  int resume;
  int abort_calls;
  int abort_status;
  bool done;  ///< 由完成回调置位（`done` 之后不能再等它）
};

struct upload_plan {
  upload_plan()
      : chunk(0),
        ticks(0),
        fsync_on(true),
        force_fail(false),
        settle_before_finish(false),
        probe(nullptr),
        max_pending(0),
        limit(nullptr),
        after_feed() {}

  std::string body;
  size_t chunk;       ///< 0 = 整包喂；否则每块这么多字节
  int ticks;          ///< 每喂一块之后空转循环几拍（每拍 1ms）
  bool fsync_on;
  /// 收尾时故意喊 `notify_parse_done(false)` —— 模拟对端断开/解析失败。
  bool force_fail;
  /// 收尾**之前**把每个已开始的部件推到"写完并已交给调用方"，最多等 5 秒。
  ///
  /// 只有需要"文件已经落盘、`result_` 里已经有了"这个前提的用例才打开它。
  /// 注意它**不能**默认开：`async_offload` 的全部意义就是"一次循环都不跑"。
  bool settle_before_finish;

  /// 非空则装一个计数用的 `uvcpp_web_upload_flow`（背压用例）。
  ///
  /// **裸指针是有意的，也是安全的**：会话只在本函数内被泵，而本函数返回后
  /// 调用方手上那个 `out.session` 会先在探针之前析构（`out` 声明在探针之后）。
  flow_probe* probe;
  /// 非 0 则 `set_max_pending_bytes(plan.max_pending)`。
  size_t max_pending;
  /// 非空则 `set_work_limit(plan.limit)`。
  uvcpp_web_work_limit* limit;
  /// 非空则 `set_name_generator(plan.name_gen)`。
  ///
  /// **存在的唯一理由是让 `O_EXCL` 可测**：不注入生成器，就没有任何办法让两次
  /// `open` 撞上同一个名字（真实生成器摇的是 2^-64 量级的随机名）。默认空 =
  /// 用框架自己的生成器，也就是生产路径。
  ::std::function<std::string(const std::string&)> name_gen;
  /// 喂完所有块之后、`ticks`/`settle` 之前跑一次。名额耗尽那类用例在这里
  /// 空转几拍（断言"停住了"）再松开名额（断言"被叫醒了"）。
  ::std::function<void()> after_feed;
};

struct upload_outcome {
  upload_outcome()
      : called(false),
        ok(false),
        received(0),
        open_files(99),
        peak_open_files(0),
        kept_before_finish(0),
        session() {}

  bool called;      ///< 完成回调跑过没有
  bool ok;
  std::string error;
  std::vector<uvcpp_web_upload_file> files;
  std::vector<uvcpp_web_upload_field> fields;
  uint64_t received;
  size_t open_files;  ///< 回调里那一刻还开着的 fd 数
  /// **每一次泵循环之后**采到的 `open_file_count()` 的峰值。
  ///
  /// `open_files` 只是**完成回调那一刻**的快照，它对"全程有没有多出来的 fd"
  /// 一无所知 —— 而 `fd_valid` 那道守卫要钉的恰恰是**中途**：open 从未成功过的
  /// 部件，清理路径一个 syscall 都不该发。变异验证证实了这一点：把
  /// `on_open_done` 改成"open 失败也算 fd 有效"，`open_files` 终值仍是 0
  /// （那次多余的 close 早就跑完了），只有**逐拍采样的峰值**能看到中途那 1。
  size_t peak_open_files;
  /// 收尾那一刻**已经写进 `result_`** 的文件数 ——
  /// `abort_deletes_completed_file` 拿它当前置断言。
  size_t kept_before_finish;
  /// 会话本体，供用例读状态。按值留着即可：`out` 声明在 `env` **之后**，
  /// 所以它先析构，会话不会活得比 loop 长。
  std::shared_ptr<uvcpp_web_upload> session;
};

/**
 * @brief 记下"泵了一拍之后还开着几个 fd"的峰值。
 *
 * **为什么要逐拍采样，而不是等结束再读一次。** `open_files`（完成回调那一刻的
 * 快照）只能看出"收工时有没有漏关的 fd"。而 `fd_valid` 那道守卫要钉的是**中途**：
 * 一个 open 从未成功过的部件，清理路径不该对它发任何 syscall。变异验证证实了
 * 这个区别是真的 —— 把 `on_open_done` 改成"open 失败也算 fd 有效"，终值照样是
 * 0（那次多余的 `close(-4058)` 早在回调之前就跑完了），逐拍采样的峰值才看得见
 * 中途那一拍是 1。**快照测不出过程，而这条不变式是过程的性质。**
 */
void sample_peak_open_files(const std::shared_ptr<uvcpp_web_upload>& up,
                            const std::shared_ptr<upload_outcome>& sp) {
  const size_t n = up->open_file_count();
  if (n > sp->peak_open_files) sp->peak_open_files = n;
}

/**
 * @brief 跑一次完整的上传：建会话 → 分块喂 → 收尾 → 把循环跑到结束。
 * @param done_after_finish [out] `notify_parse_done` 返回时完成回调是否**已经**
 *                          跑过了（`async_offload` 用它钉"不阻塞"）。
 */
upload_outcome run_upload(upload_env& env, const std::string& dir,
                          const upload_plan& plan, bool* done_after_finish) {
  upload_outcome out;
  if (done_after_finish != nullptr) *done_after_finish = false;

  // 结果放在**堆上**、由闭包按值持有一份 shared_ptr，而不是捕 `out` 的引用。
  //
  // 理由不是洁癖：超时那条路径上完成回调**可能一次都不跑**，而闭包活在与
  // `out` 无关的地方（它由会话持有）。捕引用的话，`run_upload` 一返回，
  // 闭包里的指针就指向一个已经不存在的栈帧 —— 一个只在失败路径上才成立的
  // 悬垂引用，偏偏失败路径正是最需要看清的时候。
  std::shared_ptr<upload_outcome> sp(new upload_outcome());
  std::shared_ptr<uvcpp_web_upload> up =
      uvcpp_web_upload::create(env.loop(), dir);
  up->set_fsync(plan.fsync_on);
  sp->session = up;

  // 背压三通道：本模块**只**通过这三个回调与"流"打交道，所以计数器就等于
  // "流层真的被告知了"。空的话会话照常落盘，只是没有背压。
  flow_probe* const probe = plan.probe;
  if (probe != nullptr) {
    uvcpp_web_upload_flow fl;
    fl.pause = [probe]() { ++probe->pause; };
    fl.resume = [probe]() { ++probe->resume; };
    fl.abort = [probe](int status) {
      ++probe->abort_calls;
      probe->abort_status = status;
    };
    up->set_flow(fl);
  }
  if (plan.max_pending != 0) up->set_max_pending_bytes(plan.max_pending);
  if (plan.limit != nullptr) up->set_work_limit(plan.limit);
  if (plan.name_gen) up->set_name_generator(plan.name_gen);

  // 按值捕 `up`：会话由"每个在途回调各持一份"自持，这里多一份不影响 ——
  // 真正打破引用环的是 `invoke_done_callback()` 里那次 swap（见头文件）。
  up->set_done_callback([sp, &env, up, probe](
                            const uvcpp_web_upload_result& r, bool ok,
                            const std::string& err) {
    sp->called = true;
    sp->ok = ok;
    sp->error = err;
    sp->files = r.files();
    sp->fields = r.fields();
    sp->received = up->received_bytes();
    sp->open_files = up->open_file_count();
    if (probe != nullptr) probe->done = true;
    env.loop()->stop();
  });

  uvcpp_web_multipart mp;
  mp.set_sink(up.get());
  if (!mp.set_boundary(k_boundary)) {
    check(false, "set_boundary 必须成功");
    return *sp;
  }
  check_eq_s(mp.boundary(), k_boundary, "boundary 必须原样存下来");

  size_t pos = 0;
  while (pos < plan.body.size()) {
    const size_t step =
        (plan.chunk == 0) ? plan.body.size() : plan.chunk;
    const size_t n = (step < plan.body.size() - pos) ? step
                                                     : (plan.body.size() - pos);
    mp.feed(plan.body.data() + pos, n);
    pos += n;
    for (int i = 0; i < plan.ticks; ++i) {
      env.loop()->run(UV_RUN_NOWAIT);
      sample_peak_open_files(up, sp);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // 喂完之后的钩子：名额耗尽那类用例在这里先空转几拍（断言"会话确实停住了"），
  // 再松开名额（断言"唤醒真的把它叫起来"）。次序是判据的一部分，所以给它一个
  // 明确的位置，而不是让用例自己去猜 `ticks` 该给多少。
  if (plan.after_feed) plan.after_feed();

  // 收尾**之前**把已开始的部件推到"写完并已交给调用方"。
  //
  // 这里用一个**有界等待**而不是固定拍数：固定拍数在慢机器上会变成假失败，
  // 而"等它真的到了"在任何机器上都是同一个事实。前置一旦不成立，用例会
  // 用它自己的断言报出来（`kept_before_finish`），而不是悄悄少考一半。
  if (plan.settle_before_finish) {
    // **按墙钟设上限，不按拍数。** 原先写的是 `for (i < 5000)` 配
    // `sleep_for(1ms)`，上面的注释说"最多等 5 秒" —— 但 Windows 的定时器粒度
    // 约 15.6ms，5000 拍实际是**约 78 秒**。注释与实现差了 15 倍，而后果不止是慢：
    // 那个 15s 的一次性看门狗会**在这一圈里就烧掉**，收尾那一圈随后没人叫停
    // （见 `arm_watchdog()` 上面那段）。判据本身是对的（要"等它真的到了"而不是
    // 数固定拍数），错的只是"上限"这件事没有真的成立。
    const ::std::chrono::steady_clock::time_point deadline =
        ::std::chrono::steady_clock::now() + ::std::chrono::seconds(5);
    while (::std::chrono::steady_clock::now() < deadline) {
      if (up->open_file_count() == 0 &&
          up->result().files().size() == up->file_count()) {
        break;
      }
      env.loop()->run(UV_RUN_NOWAIT);
      sample_peak_open_files(up, sp);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  const uvcpp_web_multipart_result fin = mp.finish();
  const bool parse_ok =
      plan.force_fail ? false : (fin == uvcpp_web_multipart_result::DONE);
  sp->kept_before_finish = up->result().files().size();
  up->notify_parse_done(parse_ok);

  if (done_after_finish != nullptr) *done_after_finish = sp->called;

  // 收尾仍然交给 `UV_RUN_DEFAULT`，只是往循环里**插一个采样点**。
  //
  // **不要自己用 `UV_RUN_NOWAIT` 逐拍泵。** 第一版就是这么写的，结果把落盘链
  // 活活饿死：`single_chunk` 看门狗 15s 超时，一次都没推进。（喂数据那一圈的
  // `UV_RUN_NOWAIT` 是另一回事 —— 那边每次 `mp.feed()` 都同步提交了新工作，
  // 循环里始终有事可做；收尾这一圈没有，靠 NOWAIT 空转让 libuv 走到线程池完成，
  // 走不通。）采样点不改变"谁在推进循环"，只是**在推进过程中多几个观察点**。
  //
  // 观察点确实是必需的：`open_failure` 那条路上的 open 失败就发生在这一步，
  // 而要钉的中间态（`fd_valid` 被置真、close 还没回来）只在这一点上。一把跑完
  // 的话中途**没有任何采样点**，峰值恒等于终值 —— 那样的断言看着在钉过程，
  // 实际只钉了终点（第一版就是如此，而变异验证当场证明它抓不住 U3）。
  //
  // **采样点必须是 `uv_check`，不是 `uv_timer`。** 第一版用 `repeat=1` 的定时器，
  // 采到的节拍由**墙钟**决定：`uv__run_timers` 每轮循环都跑，但只触发"已经到期"
  // 的定时器，于是最快也就 1ms 一次。而这里要抓的窗口 —— `fd_valid` 刚置真、
  // 线程池那边的 close 完成还没回来 —— 是**微秒级**的一次往返。结果"抓不抓得住"
  // 变成了抛硬币：驱动跑出 U3「没抓住」，探针 C 却抓住，唯一的差别是探针那行
  // `fprintf` 把窗口撑过了 1ms。**靠运气通过的断言不算覆盖**，两边都不可信。
  //
  // `uv_check` 每轮循环**必然**被调用一次，而 `uv_run` 里的次序是
  // `uv__poll` → 处理 pending reqs（fs 完成回调在这里跑）→ `uv__check_invoke`
  // → timers。也就是它正好落在窗口**之内**：`fd_valid` 已置真，而 close 的完成
  // 回调这一轮还送不到。确定性由此而来 —— 与"谁快谁慢"无关。
  //
  // 顺带：活跃的 check 句柄**不会**把 `uv_backend_timeout` 压成 0（那是 idle 句柄
  // 的行为），所以循环照旧在 `uv__poll` 里阻塞等待，`single_chunk` 那次
  // "NOWAIT 空转让落盘链饿死"的回归不可能重演。
  //
  // 这里也不涉及"在回调里重新 arm 自己"那种形状（`uvcpp_timer::start` 会在自身
  // 回调里再次赋值 `timer_start_cb`，那才是仓库里那个 `std::function` 自赋值陷阱），
  // 因为 check 的回调只采样、不重新 start。
  // **收尾这一圈必须自己重新装一次看门狗。**
  //
  // 上面那个是一次的，而且可能在等落盘那一圈就已经烧掉了 —— 它调的那个
  // `loop_.stop()` 被那一圈最后一次 `UV_RUN_NOWAIT` 消费并清空（`uv_run` 退出时
  // 会清 `stop_flag`）。而采样句柄是**一直活跃**的，`uv__loop_alive` 恒真，
  // 于是没有任何人能叫停这个 `run(UV_RUN_DEFAULT)`。实测（U8：异步提交的回调被
  // 换成空函数）全量跑挂了 14 分钟，而 scoped 跑同一组只是 15s 后正常 FAIL ——
  // 差别就在于全量里排在前面的 `abort_deletes_completed_file` 会在等落盘那一圈
  // 把看门狗烧掉。**没有这一句，"看门狗该抓住它"这句话就是假的。**
  env.arm_watchdog();
  ::std::unique_ptr<uvcpp_check> sampler(new uvcpp_check(env.loop()));
  sampler->start([up, sp](uvcpp_check*) { sample_peak_open_files(up, sp); });
  env.loop()->run(UV_RUN_DEFAULT);
  sampler->stop();

  return *sp;
}

/// 上传目录固定在工作目录下（ctest 的工作目录就是构建目录）。
///
/// **`main()` 会把它换成真实路径**（`web_real_path`），而这里刻意存真实路径
/// 而不是那个相对名：框架交出来的 `file.path()` 是以**目录的真实路径**为基底
/// 的（见 `uvcpp_web_upload_file::path()`），拿相对名去比前缀只会得到一串
/// "路径不在上传目录内"的假失败。用例与实现用同一个基准，这条断言才有意义 ——
/// 否则它测的是"两串字符像不像"，而不是"文件落在哪"。
std::string g_dir;

/// 从落盘全路径里取出叶子名（上传目录是固定的 `g_dir`）。
std::string leaf_of(const std::string& path) {
  if (path.size() <= g_dir.size() + 1) return std::string();
  return path.substr(g_dir.size() + 1);
}

/// 路径是否落在上传目录**内**（前缀 + 恰好一层叶子）。
///
/// 比"前缀相等"强一档：前缀相等拦不住 `g_dir/../outside.txt`，而这一条能 ——
/// 叶子里的 `..` 会让 `leaf_shape_ok` 直接失败。
bool path_is_inside(const std::string& path) {
  if (path.compare(0, g_dir.size(), g_dir) != 0) return false;
  return leaf_shape_ok(leaf_of(path));
}

// ---------------------------------------------------------------------------
// 1. single_chunk —— 最短的正路
//
// 退休的风险：`on_part_begin` 里提交的 open 有没有把 fd 拿回来、`size()` 报的
// 是不是**真正落盘**的字节数、结束之后 fd 有没有关掉。
// ---------------------------------------------------------------------------
void test_single_chunk() {
  upload_env env("single_chunk", 15000);
  purge_dir(env.loop(), g_dir);

  const std::string data = make_payload(4096, 1);
  upload_plan plan;
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "avatar", "a.bin",
                                                      "application/octet-stream",
                                                      data)),
                      true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "single_chunk: 不能超时");
  check(out.called, "single_chunk: 完成回调必须被调用");
  check(out.ok, "single_chunk: 必须成功");
  check_eq_s(out.error, "", "single_chunk: 成功时 error 必须是空串");
  check_eq_i(static_cast<long long>(out.files.size()), 1,
             "single_chunk: 恰好一个文件");
  check_eq_i(static_cast<long long>(out.open_files), 0,
             "single_chunk: 回调里不该还有开着的 fd");
  check_eq_i(static_cast<long long>(out.received), 4096,
             "single_chunk: received_bytes 必须等于部件 body 长度");

  if (out.files.size() == 1) {
    check_eq_i(static_cast<long long>(out.files[0].size()), 4096,
               "single_chunk: size() 必须等于落盘字节数");
    check_eq_s(out.files[0].field_name(), "avatar", "single_chunk: 字段名");
    check_eq_s(out.files[0].original_filename(), "a.bin",
               "single_chunk: 客户端文件名（未清洗原值）");
    check_eq_s(out.files[0].content_type(), "application/octet-stream",
               "single_chunk: content-type");
    check(!out.files[0].truncated(), "single_chunk: 本步不该有截断");
    check_bytes(read_all(out.files[0].path()), data,
                "single_chunk: 盘上内容必须逐字节等于原文");
  }

  const std::vector<std::string> entries = list_dir(env.loop(), g_dir);
  check_eq_i(static_cast<long long>(entries.size()), 1,
             "single_chunk: 目录里恰好一个文件（不多不少）");
}

// ---------------------------------------------------------------------------
// 2. async_offload —— **非阻塞的证据**
//
// 喂完整个 body、`uv_run` 一次都没跑过，此时：
//   - 盘上必须**一个文件都没有**（open 还在线程池里排队）；
//   - 完成回调必须没跑过。
//
// 这是"耗时操作不在主循环上做"唯一可判定的形式。实现里任何一处偷偷用了同步
// `uv_fs_*`，第二次断言就会红。反过来，若有人把 open 改成"提交时同步打开"，
// 第一条会红。
// ---------------------------------------------------------------------------
void test_async_offload() {
  upload_env env("async_offload", 15000);
  purge_dir(env.loop(), g_dir);

  const std::string data = make_payload(200000, 2);
  upload_plan plan;
  plan.ticks = 0;  // **一次循环都不跑**
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "big", "big.bin",
                                                      "application/octet-stream",
                                                      data)),
                      true);

  bool done_after_finish = true;
  const upload_outcome out = run_upload(env, g_dir, plan, &done_after_finish);

  check(!env.timed_out(), "async_offload: 不能超时");
  check(!done_after_finish,
        "async_offload: notify_parse_done 返回时完成回调**不该**已经跑过");
  check(out.called, "async_offload: 跑完循环后完成回调必须被调用");
  check(out.ok, "async_offload: 必须成功");
  check_eq_i(static_cast<long long>(out.files.size()), 1,
             "async_offload: 恰好一个文件");
  if (out.files.size() == 1) {
    check_bytes(read_all(out.files[0].path()), data,
                "async_offload: 20 万字节必须逐字节正确（整包喂 = 单笔大写）");
  }
}

// ---------------------------------------------------------------------------
// 3. chunked_feed —— 边收边写
//
// 每块 **37 字节**（刻意与 2 的幂不整除，跨块相位不会对齐），每块之后泵 2 拍。
// 于是"写与收"真的交错：写还在途时新块继续到，走的正是单槽双缓冲那条路。
// ---------------------------------------------------------------------------
void test_chunked_feed() {
  upload_env env("chunked_feed", 20000);
  purge_dir(env.loop(), g_dir);

  // 载荷里塞一个**近似但不完整**的边界串：它绝不能把部件切断。
  std::string data = make_payload(65536, 3);
  data.insert(32768, "\r\n--" + std::string(k_boundary) + "\r");  // 缺尾字节

  upload_plan plan;
  plan.chunk = 37;
  plan.ticks = 2;
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "f", "chunk.bin",
                                                      "application/octet-stream",
                                                      data)),
                      true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "chunked_feed: 不能超时");
  check(out.ok, "chunked_feed: 必须成功");
  check_eq_i(static_cast<long long>(out.files.size()), 1,
             "chunked_feed: 恰好一个文件（近似边界串不得被当成边界）");
  if (out.files.size() == 1) {
    check_bytes(read_all(out.files[0].path()), data,
                "chunked_feed: 37 字节一块喂进来的内容必须逐字节正确");
    check_eq_i(static_cast<long long>(out.files[0].size()),
               static_cast<long long>(data.size()),
               "chunked_feed: size() 必须等于落盘字节数");
  }
}

// ---------------------------------------------------------------------------
// 4. binary_body —— 1 MiB，含 NUL / 0xFF / CRLF
//
// 这是本文件里唯一会**多次**触发写链的场景（64 KiB 一块、每块泵 3 拍），
// 于是 `pending`/`incoming` 的换槽会真的发生若干次。
// ---------------------------------------------------------------------------
void test_binary_body() {
  upload_env env("binary_body", 30000);
  purge_dir(env.loop(), g_dir);

  const std::string data = make_payload(1024 * 1024, 4);

  upload_plan plan;
  plan.chunk = 64 * 1024;
  plan.ticks = 3;
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "blob", "blob.bin",
                                                      "application/octet-stream",
                                                      data)),
                      true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "binary_body: 不能超时");
  check(out.ok, "binary_body: 必须成功");
  check_eq_i(static_cast<long long>(out.files.size()), 1,
             "binary_body: 恰好一个文件");
  if (out.files.size() == 1) {
    check_eq_i(static_cast<long long>(out.files[0].size()),
               static_cast<long long>(data.size()),
               "binary_body: 1 MiB 必须一个字节不少");
    check_bytes(read_all(out.files[0].path()), data,
                "binary_body: 二进制内容必须逐字节正确（含 NUL / 0xFF / CRLF）");
  }
}

// ---------------------------------------------------------------------------
// 5. multi_part —— 多文件 + 混字段，顺序与归属
// ---------------------------------------------------------------------------
void test_multi_part() {
  upload_env env("multi_part", 20000);
  purge_dir(env.loop(), g_dir);

  const std::string a = make_payload(300, 5);
  const std::string b = make_payload(7, 6);

  std::vector<std::string> parts;
  parts.push_back(mp_file_part("avatar", "a.png", "image/png", a));
  parts.push_back(mp_field_part("desc", "hello world"));
  parts.push_back(mp_file_part("doc", "b.txt", "text/plain", b));

  upload_plan plan;
  plan.chunk = 13;
  plan.ticks = 1;
  plan.body = mp_body(k_boundary, parts, true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "multi_part: 不能超时");
  check(out.ok, "multi_part: 必须成功");
  check_eq_i(static_cast<long long>(out.files.size()), 2,
             "multi_part: 恰好两个文件");
  check_eq_i(static_cast<long long>(out.fields.size()), 1,
             "multi_part: 恰好一个字段");

  if (out.fields.size() == 1) {
    check_eq_s(out.fields[0].name(), "desc", "multi_part: 字段名");
    check_eq_s(out.fields[0].value(), "hello world", "multi_part: 字段值");
  }
  if (out.files.size() == 2) {
    check_eq_s(out.files[0].field_name(), "avatar",
               "multi_part: 文件按出现顺序（第 1 个是 avatar）");
    check_eq_s(out.files[1].field_name(), "doc",
               "multi_part: 文件按出现顺序（第 2 个是 doc）");
    check_bytes(read_all(out.files[0].path()), a, "multi_part: avatar 内容");
    check_bytes(read_all(out.files[1].path()), b, "multi_part: doc 内容");
  }

  // 两个文件名必须**不同** —— 同一时刻两个部件共用一个落盘名的话，后一个会把
  // 前一个截断，而"内容不对"这个断言只能抓到一个。所以这里直接钉名字互异。
  if (out.files.size() == 2) {
    check(out.files[0].path() != out.files[1].path(),
          "multi_part: 两个部件的落盘路径必须互不相同");
  }

  const std::vector<std::string> entries = list_dir(env.loop(), g_dir);
  check_eq_i(static_cast<long long>(entries.size()), 2,
             "multi_part: 目录里恰好两个文件");
}

// ---------------------------------------------------------------------------
// 6. empty_file —— 0 字节部件
//
// 这是最容易漏的一支：`on_part_data` 一次都不会被调用，若 close 链依赖
// "写过至少一块"才会被推进，它就会永远挂住（而不是报错）。
// ---------------------------------------------------------------------------
void test_empty_file() {
  upload_env env("empty_file", 15000);
  purge_dir(env.loop(), g_dir);

  upload_plan plan;
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "empty", "e.bin",
                                                      "application/octet-stream",
                                                      "")),
                      true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "empty_file: 不能超时（0 字节部件也必须走到 close）");
  check(out.called, "empty_file: 完成回调必须被调用");
  check(out.ok, "empty_file: 必须成功");
  check_eq_i(static_cast<long long>(out.files.size()), 1,
             "empty_file: 0 字节部件也是一个文件");
  if (out.files.size() == 1) {
    check_eq_i(static_cast<long long>(out.files[0].size()), 0,
               "empty_file: size() 必须是 0");
    check_eq_i(static_cast<long long>(read_all(out.files[0].path()).size()), 0,
               "empty_file: 盘上必须是 0 字节");
  }
  check_eq_i(static_cast<long long>(list_dir(env.loop(), g_dir).size()), 1,
             "empty_file: 目录里恰好一个空文件");
}

// ---------------------------------------------------------------------------
// 7. filename_metadata —— 落盘名由框架生成，与客户端给的名字无关
//
// 步骤 7 之后 `original_filename()` 是**清洗过**的（`web_sanitize_filename`），
// 而落盘名**从来都**是框架生成的随机名。这一组钉的是后者这条最基本的前提。
//
// 断言分三层，各钉各的
// -------------------
// - **解析器**给出什么，由 `web_app_multipart_func.cpp` 钉（它有 `quoted_filename`
//   那组）。这里**不复制**那份知识 —— 做法是拿同一份 body 喂一个只做记录的
//   解析器当前置，再拿它跟会话交出来的比。
// - **清洗规则**由 `web_app_util_func.cpp` 的 `sanitize_filename` 用硬编码表钉
//   （那张表是规格，这里不重复）。本组只钉**接线**：上传层在解析器的值之上
//   恰好做了清洗这一件事，没漏、也没自己另搞一套。
// - **落盘名**的形状由 `path_is_inside` / `leaf_shape_ok` 钉（见上面的注释）。
//
// 有一处**反直觉但确实是解析器的既定行为**，写在这里免得下次又当成 bug：
// `uvcpp_web_multipart.cpp:108` 对引号内的值实现了 RFC 2616 的 quoted-pair
// 反转义（`\X` → `X`），所以 `..\..\windows\x.txt` 到这一步已经变成
// `....windowsx.txt`、`C:\z\w.txt` 变成 `C:zw.txt` —— **反斜杠没了**。
// 对安全而言这是往好的方向偏（穿越串反而失去了分隔符），而且步骤 7 的
// `web_sanitize_filename()` 对正反斜杠都按分隔符取叶子名，不依赖这里。
// ---------------------------------------------------------------------------

/// 只记录解析器交出来的文件名，不碰盘 —— 用来取"解析器到底给了什么"这个基准。
class filename_recorder : public uvcpp_web_multipart_sink {
 public:
  filename_recorder() : saw_file(false) {}

  virtual bool on_part_begin(const uvcpp_web_part_info& info) {
    if (info.is_file && !saw_file) {
      first_filename = info.filename;
      saw_file = true;
    }
    return true;
  }
  virtual bool on_part_data(const char*, size_t) { return true; }
  virtual void on_part_end(bool) {}

  std::string first_filename;
  bool saw_file;
};

std::string parser_filename_of(const std::string& body) {
  filename_recorder rec;
  uvcpp_web_multipart mp;
  mp.set_sink(&rec);
  mp.set_boundary(k_boundary);
  mp.feed(body.data(), body.size());
  mp.finish();
  return rec.first_filename;
}

void test_filename_metadata() {
  upload_env env("filename_metadata", 15000);
  purge_dir(env.loop(), g_dir);

  // 典型的穿越试图 + 一个正斜杠绝对路径 + 两个 Windows 反斜杠路径。
  const char* nasty[] = {"../../etc/passwd", "..\\..\\windows\\x.txt",
                         "/abs/dir/y.txt", "C:\\z\\w.txt"};
  for (size_t k = 0; k < sizeof(nasty) / sizeof(nasty[0]); ++k) {
    purge_dir(env.loop(), g_dir);

    upload_plan plan;
    plan.body = mp_body(k_boundary,
                        std::vector<std::string>(1, mp_file_part(
                                                        "f", nasty[k],
                                                        "text/plain", "X")),
                        true);

    const std::string from_parser = parser_filename_of(plan.body);
    const upload_outcome out = run_upload(env, g_dir, plan, nullptr);
    const std::string tag = std::string("filename_metadata[") + nasty[k] + "]";

    // 一条**硬编码**的对照，防"两边一起错"：不含反斜杠的那个输入，解析器必须
    // 原样保留（含前面的 `../`）。反斜杠那两条不做硬编码 —— 它们经过
    // quoted-pair 反转义，那是解析器的地界，由 multipart 那个文件钉。
    if (k == 0) {
      check_eq_s(from_parser, nasty[0],
                 tag + ": 解析器对不含反斜杠的名字必须原样保留");
    }

    check(out.ok, tag + ": 必须成功");
    if (out.files.size() != 1) {
      check(false, tag + ": 恰好一个文件");
      continue;
    }

    const std::string p = out.files[0].path();
    // 落盘路径必须**恰好**是上传目录下的一层合法叶子。`path_is_inside` 里的
    // 形状检查同时排掉了 `..` 与任何分隔符 —— 比"前缀相等"强一档：前缀相等
    // 拦不住 `g_dir/../outside.txt`。
    check(path_is_inside(p),
          tag + ": 落盘路径必须恰好是上传目录下的一层随机名");

    // 叶子名必须是**框架摇的 16 位十六进制**（可选带白名单扩展名），与客户端
    // 给的名字毫无关系。
    const std::string leaf = leaf_of(p);
    check(leaf_shape_ok(leaf), tag + ": 落盘叶子名必须是框架摇的随机名");
    check(leaf != nasty[k], tag + ": 落盘名绝不能等于客户端给的名字");

    // **本文件真正要钉的那一条**：上传层在解析器给的值**之上**只做了一件事
    // —— 清洗。清洗规则本身由 `web_app_util_func.cpp` 的硬编码表钉住；这里钉
    // 的是接线：这一层没有自己另搞一套变换，也没有把清洗漏掉。
    check_eq_s(out.files[0].original_filename(),
               web_sanitize_filename(from_parser),
               tag + ": 上传层转发的必须是清洗后的解析器值");

    // 真实内容仍然正确 —— 证明上面那些断言不是靠"文件根本没写成"蒙过去的。
    check_eq_s(read_all(p), "X", tag + ": 内容仍然正确");
  }
}

// ---------------------------------------------------------------------------
// 7.5 filename_traversal —— 客户端给的名字**不能**决定文件落在哪
//
// 这是步骤 7 的核心断言，三件事缺一不可：
//   1. **元数据**：`original_filename()` 是清洗后的叶子（表里逐条硬编码）；
//   2. **落盘**：路径在上传目录内、叶子是框架摇的随机名；
//   3. **内容**：字节仍然正确 —— 少了它，一个"根本没写成"的实现能让前两条全绿。
//
// 期望值为什么长这样（读表之前必须先看这一段）
// -------------------------------------------
// `uvcpp_web_multipart` 对引号内的值做 RFC 2616 的 quoted-pair 反转义
// （`\X` → `X`），所以 `..\..\x.txt` 到上传层已经是 `....x.txt` —— 反斜杠
// **全没了**。表里第二列因此**看起来**不像"取叶子"（它根本没被取），但那是
// 这条链上真实发生的转换，第三列才是清洗的产物。
//
// 把期望写成"我以为的"会让这一组变成在测我的想象，而不是在测实现。所以这里
// 保留两列：`parsed`（解析器交出来的）与 `sanitized`（上传层交出来的），
// 中间那一步由 `web_app_multipart_func.cpp` 的 `quoted_filename` 单独钉。
// **反斜杠当分隔符**那条规则不经过解析器，由 `web_app_util_func.cpp` 的
// `sanitize_filename` 直接钉（那里直接喂字符串）。
// ---------------------------------------------------------------------------
void test_filename_traversal() {
  upload_env env("filename_traversal", 20000);

  struct trav_case {
    const char* raw;        ///< 写进 `filename=` 的原始值
    const char* parsed;     ///< 解析器（quoted-pair 反转义之后）交出来的
    const char* sanitized;  ///< 上传层写进 `original_filename()` 的
  };
  // 头两条各钉一件事：`/` 是分隔符（穿越串被削成叶子）；`.` 段本身被削掉。
  // 第三条与第四条是**反例**——它们证明前两条不是"见到什么都没收"。
  static const trav_case k_cases[] = {
      {"../../etc/passwd", "../../etc/passwd", "passwd"},
      {"a/../../b", "a/../../b", "b"},
      {"/abs/x", "/abs/x", "x"},
      {"..\\..\\x.txt", "....x.txt", "....x.txt"},
      {"C:\\z\\w.txt", "C:zw.txt", "C:zw.txt"},
      {"..", "..", "file"},
      {"../../../", "../../../", "file"},
  };

  const std::string content = "PAYLOAD";
  const size_t n = sizeof(k_cases) / sizeof(k_cases[0]);
  for (size_t k = 0; k < n; ++k) {
    purge_dir(env.loop(), g_dir);

    upload_plan plan;
    plan.body = mp_body(k_boundary,
                        std::vector<std::string>(1, mp_file_part(
                                                        "f", k_cases[k].raw,
                                                        "text/plain",
                                                        content)),
                        true);

    const std::string from_parser = parser_filename_of(plan.body);
    const upload_outcome out = run_upload(env, g_dir, plan, nullptr);
    const std::string tag =
        std::string("filename_traversal[") + k_cases[k].raw + "]";

    // 前置：解析器这一步确实做了那件事（不然上面那段解释就是错的，而这组会以
    // "看起来抓住了"的方式误导人）。
    check_eq_s(from_parser, k_cases[k].parsed,
               tag + ": 前置 —— 解析器交出来的值（quoted-pair 反转义后）");

    check(!env.timed_out(), tag + ": 不能超时");
    check(out.ok, tag + ": 必须成功");
    if (out.files.size() != 1) {
      check(false, tag + ": 恰好一个文件");
      continue;
    }

    check_eq_s(out.files[0].original_filename(), k_cases[k].sanitized,
               tag + ": original_filename 必须是清洗后的叶子");
    // 元数据里**不能**留下任何分隔符 —— 这是"它是个叶子"的直接形式。
    const std::string meta = out.files[0].original_filename();
    check(meta.find('/') == std::string::npos,
          tag + ": 元数据里不该有正斜杠");
    check(meta.find('\\') == std::string::npos,
          tag + ": 元数据里不该有反斜杠");

    const std::string p = out.files[0].path();
    check(path_is_inside(p), tag + ": 落盘路径必须在上传目录内且只有一层");
    check(leaf_of(p) != meta, tag + ": 落盘名绝不能等于元数据里的名字");
    check_eq_s(read_all(p), content, tag + ": 内容仍然正确");
  }

  // 目录里不许有**任何**多出来的东西（比如某个实现把客户端给的名字又落了一份）。
  const std::vector<std::string> entries = list_dir(env.loop(), g_dir);
  check_eq_i(static_cast<long long>(entries.size()), 1,
             "filename_traversal: 目录里只该有最后一次上传的那一个文件");
}

// ---------------------------------------------------------------------------
// 7.6 filename_reserved —— Windows 保留设备名、结尾的点/空格、超长、退化
//
// 与上一组的分工：那个钉"名字里带路径"，这个钉"名字本身是个特殊的东西"。
// 两条都**不会**让文件落到别的目录，但都会让**展示出来的名字与实际使用的不一致**
// —— 设备名会让 `open` 打到设备上去，结尾的点/空格会被 Win32 静默削掉。
//
// 判据仍然是那三条：元数据 == 清洗结果、落盘名是框架摇的、内容正确。
// 另外补了一条**负例对照**（`NULL`、`COM0`、`COM10`、`console.txt`）：少了它，
// 一个"见到 con/nul/com/lpt 就加下划线"的实现也能全过。
// ---------------------------------------------------------------------------
void test_filename_reserved() {
  upload_env env("filename_reserved", 20000);

  struct res_case {
    const char* raw;
    const char* sanitized;
  };
  static const res_case k_cases[] = {
      // 保留设备名（大小写无关、带扩展名也算、含上标变体）
      {"CON", "CON_"},
      {"nul.txt", "nul.txt_"},
      {"aux.txt", "aux.txt_"},
      {"COM1", "COM1_"},
      {"LPT\xC2\xB9", "LPT\xC2\xB9_"},  // 上标 ¹
      // 结尾的点与空格（Win32 会静默截掉 ⇒ 展示名与落盘名不一致）
      {"a.txt.", "a.txt"},
      {"a.txt   ", "a.txt"},
      // 退化
      {"..", "file"},
      {".", "file"},
      // 负例：**不**该被改的那些
      {"NULL", "NULL"},
      {"COM0", "COM0"},
      {"COM10", "COM10"},
      {"console.txt", "console.txt"},
      // 普通名字照旧
      {"report.pdf", "report.pdf"},
  };

  const std::string content = "R";
  const size_t n = sizeof(k_cases) / sizeof(k_cases[0]);
  for (size_t k = 0; k < n; ++k) {
    purge_dir(env.loop(), g_dir);

    upload_plan plan;
    plan.body = mp_body(k_boundary,
                        std::vector<std::string>(1, mp_file_part(
                                                        "f", k_cases[k].raw,
                                                        "text/plain",
                                                        content)),
                        true);

    const upload_outcome out = run_upload(env, g_dir, plan, nullptr);
    const std::string tag =
        std::string("filename_reserved[") + k_cases[k].raw + "]";

    check(!env.timed_out(), tag + ": 不能超时");
    check(out.ok, tag + ": 必须成功（保留设备名是**清洗**，不是拒绝）");
    if (out.files.size() != 1) {
      check(false, tag + ": 恰好一个文件");
      continue;
    }

    check_eq_s(out.files[0].original_filename(), k_cases[k].sanitized,
               tag + ": original_filename 必须是清洗结果");
    const std::string p = out.files[0].path();
    check(path_is_inside(p), tag + ": 落盘路径必须是上传目录下的一层随机名");
    check_eq_s(read_all(p), content, tag + ": 内容仍然正确");
  }

  // 超长名字：截断到 255 字节，且**不切出半个码点**。
  //
  // 用 3 字节的 `中` 而不是 ASCII：ASCII 截断在任何实现下都对，只有多字节才
  // 能区分"截断"与"按字节乱切"。判据是"结果本身是合法 UTF-8 且没有截断的
  // 尾巴"，而不是"长度是某个数" —— 后者在切出半个码点时同样成立。
  {
    purge_dir(env.loop(), g_dir);
    std::string long_cn;
    for (int i = 0; i < 200; ++i) long_cn += "\xE4\xB8\xAD";  // 600 字节

    upload_plan plan;
    plan.body = mp_body(k_boundary,
                        std::vector<std::string>(1, mp_file_part(
                                                        "f", long_cn,
                                                        "text/plain",
                                                        content)),
                        true);

    const upload_outcome out = run_upload(env, g_dir, plan, nullptr);
    check(out.ok, "filename_reserved[超长]: 必须成功");
    if (out.files.size() == 1) {
      const std::string meta = out.files[0].original_filename();
      check_eq_i(static_cast<long long>(meta.size()), 255,
                 "filename_reserved[超长]: 必须截断到 255 字节");
      check(meta.size() % 3 == 0,
            "filename_reserved[超长]: 截断必须落在码点边界上（每字 3 字节）");
      check(path_is_inside(out.files[0].path()),
            "filename_reserved[超长]: 落盘路径照旧只有一层");
    }
  }
}

// ---------------------------------------------------------------------------
// 7.7 disk_name_shape —— 落盘名的构成（`pick_extension` 的规格）
//
// 落盘名 = `16 位随机十六进制` + `可选的白名单扩展名`。扩展名是客户端的信息里
// **唯一**还留在落盘名上的东西，所以它的三条规则（只取最后一段、白名单过滤、
// 空就丢掉）各要有判据 —— 少了它们，一个"把客户端给的名字整个当扩展名接上去"
// 的实现能让落盘名重新受客户端控制，而那正是本模块要断掉的东西。
// ---------------------------------------------------------------------------
void test_disk_name_shape() {
  upload_env env("disk_name_shape", 20000);

  struct ext_case {
    const char* raw;
    const char* want_ext;  ///< 期望的扩展名（含点），空串 = 不该带扩展名
  };
  static const ext_case k_cases[] = {
      {"a.bin", ".bin"},
      {"a.tar.gz", ".gz"},            // 只取**最后一段**：多段对落盘名无意义
      {"report.PDF", ".PDF"},         // 大小写原样保留
      {".bashrc", ""},                // 点在第 0 位是隐藏文件，不是扩展名
      {"a.", ""},                     // 清洗把结尾的点削掉了 ⇒ 没有扩展名
      {"noext", ""},
      {"a.b!n", ".b"},                // 白名单在 `!` 处停下
      {"a.verylongextensionname", ".verylongex"},  // 上限 10 字节
  };

  const std::string content = "S";
  const size_t n = sizeof(k_cases) / sizeof(k_cases[0]);
  for (size_t k = 0; k < n; ++k) {
    purge_dir(env.loop(), g_dir);

    upload_plan plan;
    plan.body = mp_body(k_boundary,
                        std::vector<std::string>(1, mp_file_part(
                                                        "f", k_cases[k].raw,
                                                        "text/plain",
                                                        content)),
                        true);

    const upload_outcome out = run_upload(env, g_dir, plan, nullptr);
    const std::string tag =
        std::string("disk_name_shape[") + k_cases[k].raw + "]";

    check(out.ok, tag + ": 必须成功");
    if (out.files.size() != 1) {
      check(false, tag + ": 恰好一个文件");
      continue;
    }

    const std::string leaf = leaf_of(out.files[0].path());
    check(leaf_shape_ok(leaf), tag + ": 叶子名必须是合法形状");
    check_eq_s(leaf.size() >= 16 ? leaf.substr(16) : std::string(),
               k_cases[k].want_ext, tag + ": 扩展名必须是白名单过滤后的");
    check_eq_s(read_all(out.files[0].path()), content, tag + ": 内容正确");
  }
}

// ---------------------------------------------------------------------------
// 7.8 excl_no_clobber —— `O_EXCL`：撞名时重摇，**不是**顺着已有文件写下去
//
// 这是本模块最重要的一条安全性质，也是**唯一**需要注入生成器才能测的一条：
// 真实生成器摇的是 2^-64 量级的随机名，两次 `open` 撞上同一个名字在物理上
// 不会发生。所以 `set_name_generator()` 这个接缝存在的全部意义就是让这一组
// 可写（见 `uvcpp_web_upload.h` 里那段说明）。
//
// 攻击的形状：在目标路径上预置一个东西（真实攻击里是指向别处的**符号链接**，
// 效果与这里预置一个普通文件完全相同），期望实现顺着它写下去。`O_EXCL` 让
// `open` 拿到 `UV_EEXIST`、会话重摇一个名字，于是**预置的东西一个字节都没动**。
//
// 为什么用普通文件而不是真符号链接：建符号链接在 Windows 上要管理员权限
// （`mklink` 不带 `/J` 时的默认行为），CI 上会静默跳过 —— 那种用例是"看起来
// 覆盖了、其实没跑"。而 `O_CREAT` 对"已存在的普通文件"与"已存在的符号链接"
// 行为一致（都会命中），所以普通文件能把同一条不变式钉死，且不需要特权。
//
// 判据有四条，缺哪一条都留一个洞：
//   - 生成器被调用了 ≥ 2 次（重摇**真的**发生了）；
//   - 落盘名是重摇之后的那个（而不是预置名）；
//   - **预置文件逐字节原封不动**（这是核心那条）；
//   - 目录里恰好两个文件（预置的 + 上传的）。
// 去掉 `O_EXCL` 的实现会让后三条同时红，且失败信息直接指向"哨兵被写坏了"。
// ---------------------------------------------------------------------------
const char* const k_preset_leaf = "preset_sentinel.bin";

void test_excl_no_clobber() {
  upload_env env("excl_no_clobber", 20000);
  purge_dir(env.loop(), g_dir);

  // 1. 预置哨兵。
  const std::string preset = g_dir + "/" + k_preset_leaf;
  const std::string sentinel = "SENTINEL-MUST-SURVIVE";
  {
    std::ofstream f(preset.c_str(), std::ios::binary | std::ios::trunc);
    f << sentinel;
  }
  check_eq_s(read_all(preset), sentinel, "excl: 前置 —— 哨兵必须已种好");

  // 2. 生成器：第一次给预置名（必然撞上 UV_EEXIST），之后给一个固定的新名。
  int gen_calls = 0;
  upload_plan plan;
  plan.name_gen = [&gen_calls](const std::string&) -> std::string {
    ++gen_calls;
    if (gen_calls == 1) return std::string(k_preset_leaf);
    return std::string("fresh_name_after_retry");
  };

  const std::string data = make_payload(2048, 9);
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "f", "inner.bin",
                                                      "application/octet-stream",
                                                      data)),
                      true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "excl: 不能超时");
  check(out.ok,
        "excl: 撞名之后重摇必须成功。**若这里红了且 error 是「打开上传文件失败」，"
        "先查本平台 open 撞名回的是不是 UV_EEXIST，别急着改断言**");
  check(gen_calls >= 2,
        "excl: 生成器必须被调用 ≥2 次（第一次撞名、第二次重摇）");
  check_eq_i(static_cast<long long>(out.files.size()), 1,
             "excl: 恰好一个文件");

  if (out.files.size() == 1) {
    check_eq_s(out.files[0].path(), g_dir + "/fresh_name_after_retry",
               "excl: 落盘名必须是**重摇之后**的那个，不是预置名");
    check_bytes(read_all(out.files[0].path()), data, "excl: 内容必须正确");
  }

  // 3. **核心那条**：预置文件逐字节原封不动。
  check_eq_s(read_all(preset), sentinel,
             "excl: 预置文件必须原封不动 —— O_EXCL 让 open 失败并重摇，"
             "而不是顺着它写下去");

  // 4. 目录里恰好两个：哨兵 + 重摇出来的那个。
  check_eq_i(static_cast<long long>(list_dir(env.loop(), g_dir).size()), 2,
             "excl: 目录里恰好两个文件（哨兵 + 上传的）");
}

// ---------------------------------------------------------------------------
// 8. truncated_body —— 报文被截断 ⇒ 一个文件都不留
//
// 判据是**目录为空**，不是"`ok()==false`"。理由：一个把错误状态报对了、
// 却把半截文件留在盘上的实现，用前者能过、用后者不能。
// ---------------------------------------------------------------------------
void test_truncated_body() {
  upload_env env("truncated_body", 15000);
  purge_dir(env.loop(), g_dir);

  std::string body =
      mp_body(k_boundary,
              std::vector<std::string>(1, mp_file_part(
                                              "f", "t.bin",
                                              "application/octet-stream",
                                              make_payload(50000, 7))),
              false);  // **不发**终边界
  check(body.size() > 50000, "truncated_body: 前提 —— body 里确实有内容");

  upload_plan plan;
  plan.chunk = 4096;
  plan.ticks = 2;
  plan.body = body;

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "truncated_body: 不能超时");
  check(out.called, "truncated_body: 完成回调必须被调用（失败也要调）");
  check(!out.ok, "truncated_body: 截断的报文必须报失败");
  check(!out.error.empty(), "truncated_body: 失败必须给出原因");
  check_eq_i(static_cast<long long>(out.files.size()), 0,
             "truncated_body: 失败时 result 必须为空");
  check_eq_i(static_cast<long long>(out.fields.size()), 0,
             "truncated_body: 失败时字段也必须为空");
  check_eq_i(static_cast<long long>(out.open_files), 0,
             "truncated_body: 失败路径也必须把 fd 关掉");

  const std::vector<std::string> entries = list_dir(env.loop(), g_dir);
  check_eq_i(static_cast<long long>(entries.size()), 0,
             "truncated_body: 目录必须被清空（半截文件不许留在盘上）");
}

// ---------------------------------------------------------------------------
// 9. abort_deletes_completed_file —— 「失败时框架删」的唯一证据
//
// 报文本身是**完整的**（两个部件都正常结束，终边界也在），但收尾时故意喊
// `notify_parse_done(false)`，模拟"body 收完了、可连接在对端那边已经断了"这类
// 中止。此时第一个文件**早已写完并已经被放进 result** —— 它同样必须被删掉。
//
// 少了这条，"失败时框架删"这个决策就没有任何东西在守：只删"还没写完的"
// 实现能让第 8 条照样绿。
// ---------------------------------------------------------------------------
void test_abort_deletes_completed_file() {
  upload_env env("abort_deletes_completed_file", 15000);
  purge_dir(env.loop(), g_dir);

  std::vector<std::string> parts;
  parts.push_back(mp_file_part("a", "a.bin", "application/octet-stream",
                               make_payload(20000, 8)));
  parts.push_back(mp_file_part("b", "b.bin", "application/octet-stream",
                               make_payload(20000, 9)));

  upload_plan plan;
  plan.chunk = 8192;
  plan.ticks = 0;
  plan.force_fail = true;
  plan.settle_before_finish = true;  // **先让两个文件都写完并进 result_**
  plan.body = mp_body(k_boundary, parts, true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  // **前置** —— 少了它这个用例什么都测不到。
  //
  // 若 settle 没等到（慢机器、或实现里根本没把文件交出去），`kept_before_finish`
  // 就是 0，下面那条"目录必须为空"会**因为文件压根没写出来**而通过 —— 一个
  // 恒真的绿。所以先把它钉成事实再谈结论。
  check_eq_i(static_cast<long long>(out.kept_before_finish), 2,
             "abort: 前置 —— 收尾前两个文件必须都已写完并已进 result");

  check(!env.timed_out(), "abort: 不能超时");
  check(out.called, "abort: 完成回调必须被调用");
  check(!out.ok, "abort: 必须报失败");
  check_eq_i(static_cast<long long>(out.files.size()), 0,
             "abort: 失败时 result 必须为空");
  check_eq_i(static_cast<long long>(out.open_files), 0,
             "abort: 必须把 fd 都关掉（不能只删文件不关 fd）");

  const std::vector<std::string> entries = list_dir(env.loop(), g_dir);
  check_eq_i(static_cast<long long>(entries.size()), 0,
             "abort: **已经写完的**文件也必须被删掉");
}

// ---------------------------------------------------------------------------
// 10. open_failure_keeps_dir_clean —— 落盘目录不存在时的失败路径
//
// 这一条打的不是"报错"，而是**报错之后盘上不许留下任何东西**。它同时钉住了
// `fd_valid` 的语义：open 从未成功过，所以清理路径**一个多余的 syscall 都不该
// 发**（`close(-1)` 与 `unlink` 一个不存在的文件都是纯粹的噪声，而且前者在某些
// 平台上会误关掉调用方自己的 fd）。
// ---------------------------------------------------------------------------
void test_open_failure_keeps_dir_clean() {
  upload_env env("open_failure", 15000);
  purge_dir(env.loop(), g_dir);

  // 一个**不存在**的目录：open 必然失败。
  const std::string missing = g_dir + "/no_such_subdir_9f21";
  check(dir_absent(missing), "open_failure: 前置 —— 该目录必须不存在");

  upload_plan plan;
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "f", "x.bin",
                                                      "application/octet-stream",
                                                      make_payload(4096, 11))),
                      true);

  const upload_outcome out = run_upload(env, missing, plan, nullptr);

  check(!env.timed_out(), "open_failure: 不能超时（open 失败必须走完成路径）");
  check(out.called, "open_failure: 完成回调必须被调用");
  check(!out.ok, "open_failure: 必须报失败");
  check(!out.error.empty(), "open_failure: 必须给出原因");
  check_eq_i(static_cast<long long>(out.files.size()), 0,
             "open_failure: 失败时 result 必须为空");
  check_eq_i(static_cast<long long>(out.open_files), 0,
             "open_failure: 不该有 fd 被算成开着");
  // **全程**都必须是 0，不只是收工那一刻。open 从未成功过，所以清理路径不该
  // 对任何 fd 动手 —— 对负 fd 发 close 那种事会在这里露出来，而终值快照看不见
  // 它（那次多余的 close 早在回调之前就跑完了）。
  check_eq_i(static_cast<long long>(out.peak_open_files), 0,
             "open_failure: 全程都不该有 fd 被算成开着（对无效 fd 发 close 会在此露馅）");
  check(dir_absent(missing), "open_failure: 失败路径不该把那个目录建出来");

  // 上传目录本身必须干净 —— 这次失败不该在任何地方留下文件。
  check_eq_i(static_cast<long long>(list_dir(env.loop(), g_dir).size()), 0,
             "open_failure: 上传目录必须仍然为空");
}

// ---------------------------------------------------------------------------
// 11. fsync_off —— 关掉 fsync 之后依然落盘、依然关 fd
//
// 这一条不是为了"测掉电"（测不了），而是为了钉住 `set_fsync(false)` 那条
// 短路分支：`submit_fsync` 直接转 `submit_close`。若把它写成"什么都不做"，
// close 永远不会被提交，会话会挂到看门狗超时。
// ---------------------------------------------------------------------------
void test_fsync_off() {
  upload_env env("fsync_off", 15000);
  purge_dir(env.loop(), g_dir);

  const std::string data = make_payload(40000, 10);

  upload_plan plan;
  plan.chunk = 7000;
  plan.ticks = 1;
  plan.fsync_on = false;
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "f", "n.bin",
                                                      "application/octet-stream",
                                                      data)),
                      true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "fsync_off: 不能超时（关掉 fsync 也必须走到 close）");
  check(out.ok, "fsync_off: 必须成功");
  check_eq_i(static_cast<long long>(out.files.size()), 1,
             "fsync_off: 恰好一个文件");
  if (out.files.size() == 1) {
    check_bytes(read_all(out.files[0].path()), data,
                "fsync_off: 关掉 fsync 也必须内容正确");
  }
  check_eq_i(static_cast<long long>(out.open_files), 0,
             "fsync_off: fd 必须关掉");
}

// ---------------------------------------------------------------------------
// 12. backpressure_engages —— **pause 真的被下发了，而且成对**
//
// `ticks = 0` 是刻意的：喂数据那一圈**一次循环都不跑**，于是第一块数据提交的
// open 永远不会在喂完之前完成，`busy_` 从头到尾为真，`pause()` 必然在**第一块**
// 就被下发。这不是"凑巧能过"——是把时序钉死，让"pause 有没有被调用"变成一个
// 与机器快慢无关的事实。
//
// 判据里**最要紧的一条是 `pause == resume`**。`read_pause()` 是**连接**级的
// （不是请求级的），停下的读不会因为请求结束就自己恢复 —— 少下发一次
// `resume()`，这条 keep-alive 连接上后来的请求就永远收不到字节。这正是本项目
// 早先真实踩过的那个回归（`keepalive_after_upload`），而"pause 被调用过"这个
// 断言**单独是抓不住它的**。
// ---------------------------------------------------------------------------
void test_backpressure_engages() {
  upload_env env("backpressure", 20000);
  purge_dir(env.loop(), g_dir);

  const std::string data = make_payload(65536, 21);

  flow_probe probe;
  upload_plan plan;
  plan.chunk = 4096;
  plan.ticks = 0;  // 见上：把"第一块就提交 open"钉死
  plan.probe = &probe;
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "f", "bp.bin",
                                                      "application/octet-stream",
                                                      data)),
                      true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "backpressure: 不能超时");
  check(out.called, "backpressure: 完成回调必须被调用");
  check(out.ok, "backpressure: 必须成功");
  check(probe.pause >= 1,
        "backpressure: 慢消费下 pause() 必须真的被下发过（这是背压的全部证据）");
  check(probe.resume >= 1, "backpressure: resume() 必须被下发过");
  check_eq_i(static_cast<long long>(probe.pause),
             static_cast<long long>(probe.resume),
             "backpressure: pause 与 resume 必须**成对**（少一次 resume 会"
             "把 keep-alive 连接的读永久停住）");
  check_eq_i(static_cast<long long>(probe.abort_calls), 0,
             "backpressure: 正常的背压不该走到 abort");
  check_eq_i(static_cast<long long>(out.session->pause_events()),
             static_cast<long long>(probe.pause),
             "backpressure: 会话自己记的 pause 次数必须与流层收到的一致");
  check_eq_i(static_cast<long long>(out.session->overflow_events()), 0,
             "backpressure: 64 KiB 远在默认上限之内，不该越界");
  check(!out.session->flow_paused(),
        "backpressure: 收工之后不许还要求着暂停");
  check_eq_i(static_cast<long long>(out.session->buffered_bytes()), 0,
             "backpressure: 收工之后缓冲必须清空");

  if (out.files.size() == 1) {
    check_eq_i(static_cast<long long>(out.files[0].size()), 65536,
               "backpressure: 背压不该改变落盘字节数");
    check_bytes(read_all(out.files[0].path()), data,
                "backpressure: 边收边写也必须逐字节正确");
  } else {
    check(false, "backpressure: 恰好一个文件");
  }
  check_eq_i(static_cast<long long>(list_dir(env.loop(), g_dir).size()), 1,
             "backpressure: 目录里恰好一个文件");
}

// ---------------------------------------------------------------------------
// 13. pending_overflow_aborts —— 攒过头 ⇒ 413 掐断，且**不留半截文件**
//
// 这一条走的是"暂停了还在灌"那条**不该发生**的分支，但必须行为确定。
// `max_pending = 4096` 配 `chunk = 4096`：第二块 body 一进来就够到上限，第三块
// 越界。`ticks = 0` 让 open 一直在途（`busy_` 恒真），所以这不是碰运气 ——
// 上限判定的那个前置条件**按构造成立**。
//
// 断言里 `overflow_events() == 1` 与 `abort_status == 413` 缺一不可：前者证明
// "判到了"，后者证明"判到之后回的是 413 而不是别的什么"。少任何一个，一个
// "判到了但默不作声"或"回错码"的实现都能蒙混过关。
// ---------------------------------------------------------------------------
void test_pending_overflow_aborts() {
  upload_env env("overflow", 20000);
  purge_dir(env.loop(), g_dir);

  flow_probe probe;
  upload_plan plan;
  plan.chunk = 4096;
  plan.ticks = 0;
  plan.max_pending = 4096;
  plan.probe = &probe;
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "f", "ovf.bin",
                                                      "application/octet-stream",
                                                      make_payload(65536, 22))),
                      true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "overflow: 不能超时（越界必须有确定行为）");
  check(out.called, "overflow: 完成回调必须被调用");
  check(!out.ok, "overflow: 越界必须报失败");
  check(probe.pause >= 1, "overflow: 越界之前应当先背压过（顺序是可观测的）");
  check_eq_i(static_cast<long long>(probe.abort_calls), 1,
             "overflow: 必须**恰好**掐断一次");
  check_eq_i(static_cast<long long>(probe.abort_status),
             static_cast<long long>(http_status::PAYLOAD_TOO_LARGE),
             "overflow: 掐断时回的必须是 413");
  check_eq_i(static_cast<long long>(out.session->overflow_events()), 1,
             "overflow: 越界计数必须恰好一次");
  check_eq_i(static_cast<long long>(out.files.size()), 0,
             "overflow: 失败时 result 必须为空");
  check_eq_i(static_cast<long long>(list_dir(env.loop(), g_dir).size()), 0,
             "overflow: 半截文件必须被删掉（这是'失败时框架删'那个决策）");
}

// ---------------------------------------------------------------------------
// 14. no_flow_fails_not_hangs —— 没有背压通道时越界必须**失败**，不能挂
//
// 与上一条同一份数据、同一个上限，唯一差别是**不注入流控**。于是走到
// `fail("上传缓冲超出上限（没有可用的背压通道）")` 那一支。
//
// 为什么要单独一条：这一支是 `abort` 回调为空时的唯一出口。若它被写成
// "只是记一笔然后继续收"，`incoming` 会无界增长（本模块存在的意义当场作废）；
// 若被写成"返回 false 但不收尾"，会话就永久停在那里 —— 而两种情况都**不会**
// 让别的用例变红，只有这一条能分辨。
// ---------------------------------------------------------------------------
void test_no_flow_fails_not_hangs() {
  upload_env env("no_flow", 20000);
  purge_dir(env.loop(), g_dir);

  upload_plan plan;
  plan.chunk = 4096;
  plan.ticks = 0;
  plan.max_pending = 4096;  // 没有 probe ⇒ 没有 flow ⇒ abort 回调为空
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "f", "nf.bin",
                                                      "application/octet-stream",
                                                      make_payload(65536, 23))),
                      true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "no_flow: 越界不能把会话挂住（必须走到完成回调）");
  check(out.called, "no_flow: 完成回调必须被调用");
  check(!out.ok, "no_flow: 必须报失败");
  check(!out.error.empty(), "no_flow: 必须给出原因");
  check_eq_i(static_cast<long long>(out.session->overflow_events()), 1,
             "no_flow: 越界计数必须恰好一次");
  check_eq_i(static_cast<long long>(list_dir(env.loop(), g_dir).size()), 0,
             "no_flow: 失败路径同样不许留下半截文件");
}

// ---------------------------------------------------------------------------
// 15. slot_exhaustion_pauses —— 名额耗尽时**停住**，名额一松**被叫醒**
//
// 这一条是 `work_limit` 那套唤醒 API 的**唯一**判据，而它是本阶段的必需项：
// `acquire()`/`release()` 本身没有任何等待队列，没有唤醒就是**永久卡死**
// （会话停在等名额上，连接既不关也不回）。
//
// 时序是判据的一部分，所以 `after_feed` 的次序是写死的：
//   ① 空转若干拍 —— 断言"确实停住了"（`!probe.done`、还在等名额、唤醒登记着）；
//   ② `lim.release()` —— 断言"唤醒被消费"（一次性，`wakeup_count() == 0`）。
// 少了 ①，一个"根本没暂停"的实现也能过；少了 ②，一个"暂停了但没人叫醒"的
// 实现反而会挂到看门狗超时 —— 两种都必须在**同一个**用例里被区分开。
// ---------------------------------------------------------------------------
void test_slot_exhaustion_pauses() {
  upload_env env("slot_exhaust", 20000);
  purge_dir(env.loop(), g_dir);

  uvcpp_web_work_limit lim(1);
  check(lim.acquire(), "slot: 前置 —— 测试自己占住那唯一的名额");
  check(lim.full(), "slot: 前置 —— 占住之后池子必须是满的");

  const std::string data = make_payload(20000, 24);
  flow_probe probe;

  upload_plan plan;
  plan.ticks = 0;
  plan.probe = &probe;
  plan.limit = &lim;
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "f", "slot.bin",
                                                      "application/octet-stream",
                                                      data)),
                      true);
  plan.after_feed = [&env, &probe, &lim]() {
    // ① 名额还被测试占着 —— 会话必须**停在**这里，一拍都不许推进。
    for (int i = 0; i < 30; ++i) {
      env.loop()->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(!probe.done, "slot: 拿不到名额时会话不能完成");
    check(probe.pause >= 1, "slot: 拿不到名额必须先背压");
    check(lim.wakeup_count() == 1,
          "slot: 等名额期间必须登记了**一个**唤醒回调（没有它就是永久卡死）");

    // ② 松开名额 —— 唤醒是同步下发的，所以这些断言在 release() 返回时就成立。
    lim.release();
    check(lim.wakeup_count() == 0,
          "slot: 唤醒是**一次性**的，被叫醒之后表里不该还留着它");
    // **这一条才是"唤醒真的接上了"的判据**，上面那条只证明"登记了"。
    //
    // 唤醒是在 `release()` 的栈上同步跑的，所以会话当场就会把名额接回去（`in_flight`
    // 立刻从 0 变 1）。少了唤醒，它会**一直停在 0**。
    //
    // 为什么必须在这一刻断言、不能靠后面的结果去推：本装置的收尾会无条件调
    // `notify_parse_done(true)`，而那一次 re-pump 在"名额此刻恰好空着"时**也能
    // 把会话救活** —— 于是"没有唤醒"这件事在最终结果上看不出来。真服务器上
    // 没有这一救：连接已经被 `pause()` 停住了，END 永远不会到，就是**永久卡死**。
    check_eq_i(static_cast<long long>(lim.in_flight()), 1,
               "slot: 唤醒必须在 release() 的栈上就把名额接过去");
  };

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "slot: 不能超时（名额松开之后必须自己跑完）");
  check(out.called, "slot: 完成回调必须被调用");
  check(out.ok, "slot: 名额恢复之后必须成功");
  check(probe.abort_calls == 0, "slot: 等名额不是错误，不该掐断");
  check(probe.resume >= 1, "slot: 收工前必须把读放开");
  check_eq_i(static_cast<long long>(probe.pause),
             static_cast<long long>(probe.resume),
             "slot: pause 与 resume 必须成对");
  check_eq_i(static_cast<long long>(lim.in_flight()), 0,
             "slot: 每一笔 fs 操作借的名额都必须还回来（泄漏会表现为服务"
             "无缘无故开始 503）");
  check_eq_i(static_cast<long long>(lim.wakeup_count()), 0,
             "slot: 收工之后唤醒表必须是空的");

  if (out.files.size() == 1) {
    check_bytes(read_all(out.files[0].path()), data,
                "slot: 等过名额的文件也必须逐字节正确");
  } else {
    check(false, "slot: 恰好一个文件");
  }
}

// ---------------------------------------------------------------------------
// 16. idle_delivery_not_capped —— 空闲会话上的**单块大写**不该被判成越界
//
// 这一条钉的是 `set_max_pending_bytes()` 的**语义**，而不是某个数字。
// 头文件写的是"在途写期间**允许攒下**的字节上限"，并且明确注明峰值可以是
// `max_pending + 一块`。而"一次交付"的粒度是解析器决定的（一次完整 feed 里
// 的部件 body 会**一次性**交付），所以：
//
//   - 越界判定的前置条件必须是"这一块真的会被攒下来"（有在途写、或在等名额）。
//     空闲会话上来的那一块会被 `pump()` 立刻挪进 `pending` 发出去 —— 它根本
//     不是"攒"，拿它去撞上限是**判错了对象**。
//   - 判的必须是**已经攒下**的量，而不是"加上这一块会不会超"。后者等价于要求
//     单块不得大于上限，与头文件承诺的峰值自相矛盾。
//
// 少了这一条，`set_max_pending_bytes(4096)` 会让**每一次正常上传**都被判越界
// （第一块的 body 通常就有几 KB），而那正是这个旋钮最容易被误用的取值。
// ---------------------------------------------------------------------------
void test_idle_delivery_not_capped() {
  upload_env env("idle_delivery", 20000);
  purge_dir(env.loop(), g_dir);

  const std::string data = make_payload(8192, 25);

  flow_probe probe;
  upload_plan plan;
  plan.chunk = 0;      // 整包喂 ⇒ 部件 body 一次性交付
  plan.ticks = 0;      // 交付时**没有任何在途写**（会话是空闲的）
  plan.max_pending = 4096;  // 上限**小于**这一块
  plan.probe = &probe;
  plan.body = mp_body(k_boundary,
                      std::vector<std::string>(1, mp_file_part(
                                                      "f", "idle.bin",
                                                      "application/octet-stream",
                                                      data)),
                      true);

  const upload_outcome out = run_upload(env, g_dir, plan, nullptr);

  check(!env.timed_out(), "idle_delivery: 不能超时");
  check(out.ok, "idle_delivery: 空闲会话上的单块大写必须被照常接受");
  check_eq_i(static_cast<long long>(out.session->overflow_events()), 0,
             "idle_delivery: 不该越界（这一块根本不会被'攒'下来）");
  check_eq_i(static_cast<long long>(probe.abort_calls), 0,
             "idle_delivery: 更不该掐断连接");
  if (out.files.size() == 1) {
    check_eq_i(static_cast<long long>(out.files[0].size()), 8192,
               "idle_delivery: 8 KiB 必须一个字节不少地落盘");
    check_bytes(read_all(out.files[0].path()), data,
                "idle_delivery: 内容必须逐字节正确");
  } else {
    check(false, "idle_delivery: 恰好一个文件");
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string only;
  if (argc > 1) only = argv[1];

  g_dir = "uvcpp_upload_test_dir";
  if (!ensure_dir(g_dir)) {
    std::cerr << "[upload] 无法建立上传目录: " << g_dir << std::endl;
    return 2;
  }
  // 建好之后换成**真实路径**：框架的 `file.path()` 以目录真实路径为基底，用例
  // 必须用同一个基准比对（见 `g_dir` 上面那段注释）。换不成也照常跑 —— 那时
  // 两边都是这个相对名，前缀照样相等，只是不再覆盖"相对配置"这一层。
  {
    std::string real;
    if (web_real_path(g_dir, real, /*allow_missing=*/false) && !real.empty())
      g_dir = real;
  }

  struct case_entry {
    const char* name;
    void (*fn)();
  };
  const case_entry cases[] = {
      {"single_chunk", test_single_chunk},
      {"async_offload", test_async_offload},
      {"chunked_feed", test_chunked_feed},
      {"binary_body", test_binary_body},
      {"multi_part", test_multi_part},
      {"empty_file", test_empty_file},
      {"filename_metadata", test_filename_metadata},
      {"filename_traversal", test_filename_traversal},
      {"filename_reserved", test_filename_reserved},
      {"disk_name_shape", test_disk_name_shape},
      {"excl_no_clobber", test_excl_no_clobber},
      {"truncated_body", test_truncated_body},
      {"abort_deletes_completed_file", test_abort_deletes_completed_file},
      {"open_failure_keeps_dir_clean", test_open_failure_keeps_dir_clean},
      {"fsync_off", test_fsync_off},
      {"backpressure_engages", test_backpressure_engages},
      {"pending_overflow_aborts", test_pending_overflow_aborts},
      {"no_flow_fails_not_hangs", test_no_flow_fails_not_hangs},
      {"slot_exhaustion_pauses", test_slot_exhaustion_pauses},
      {"idle_delivery_not_capped", test_idle_delivery_not_capped},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (!only.empty() &&
        std::string(cases[i].name).find(only) == std::string::npos) {
      continue;
    }
    const int before = g_failures;
    std::cout << "[upload] " << cases[i].name << std::endl;
    cases[i].fn();
    const bool ok = (g_failures == before);
    std::cout << (ok ? "  -> PASS" : "  -> FAIL") << std::endl;
    if (!ok) {
      std::cout << "[upload] FAIL" << std::endl;
      return 2;
    }
  }

  std::cout << "[upload] ALL PASS" << std::endl;
  return 0;
}

#else  // !UVCPP_WEBAPP_ENABLE

int main() {
  std::cout << "[upload] SKIP (UVCPP_WEBAPP_ENABLE=0)" << std::endl;
  return 0;
}

#endif
