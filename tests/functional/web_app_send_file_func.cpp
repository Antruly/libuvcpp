/**
 * @file tests/functional/web_app_send_file_func.cpp
 * @brief Phase 3c 步骤 5 —— `uvcpp_web_file_transfer` 分片读下发（**不接 HTTP**）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这个文件为什么先于 HTTP 存在
 * ---------------------------
 * 滑动窗口 + 背压 + 中途取消约 250 行，与 HTTP 一点关系都没有。把它单独测，
 * 才能把「分片读本身错了」和「接线接错了」分开 —— 步骤 6 的
 * `send_file`/静态服务分流会复用本文件里的同一批载体。
 *
 * 所以本文件**不建服务器、不发请求**：一个 `uvcpp_loop`、一个把字节攒进
 * `std::string` 的 sink，就是全部。步骤 6 会往同一个文件的 `cases[]` 里追加
 * 应用层的那几组。
 *
 * 判据的形状
 * ----------
 * 本模块唯一的**不变式**是「内存占用与文件大小无关」。所以 `mem_bounded`
 * 那一组不是"顺手测一下内存"，而是整个模块存在的理由；它的上界
 * `high_water + 2 * slice` 直接由导出的静态常量算出来，**不在用例里另抄一个
 * 数**（抄一份必然漂移，而漂移了这条断言就变成在测我的想象）。
 *
 * 跑法：`test_web_app_send_file_func.exe [子串过滤]`
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_file.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_static.h>
#include "loop_drain.h"

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

void check_eq_u64(uint64_t got, uint64_t want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

/// 造一段**可校验**的字节：每个位置的值与下标有关，于是「偏移算错」和
/// 「长度截断」都会表现为内容不等，而不是碰巧相等；另外钉几个边界值
/// （NUL / 0xFF / CRLF），让「文本模式翻译」这类缺陷也躲不掉。
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

bool write_file(const std::string& path, const std::string& data) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  if (!data.empty()) {
    const size_t n = std::fwrite(data.data(), 1, data.size(), f);
    if (n != data.size()) {
      std::fclose(f);
      return false;
    }
  }
  std::fclose(f);
  return true;
}

/// 用例用的 sink。三件事各自可配，因为不同的组要用不同的"接收方行为"：
/// - `hold_at` —— 从多少字节起开始报背压（0 = 永远报 0，也就是不背压）；
/// - `drained` —— 测试手工增加的"已消费"量，`backlog()` 扣掉它；
/// - `cancel_on_call` —— 在第几次交付里调 `cancel()`（对端断开的等价物）。
class test_sink : public uvcpp_web_file_sink {
 public:
  test_sink()
      : transfer(nullptr),
        hold_at(0),
        drained(0),
        cancel_on_call(0),
        data_calls(0),
        done_count(0),
        status(0),
        bytes(0) {}

  void on_data(const char* d, size_t len) override {
    ++data_calls;
    // 契约：这一句之内必须拷走。
    collected.append(d, len);
    if (cancel_on_call != 0 && data_calls == cancel_on_call &&
        transfer != nullptr) {
      transfer->cancel();
    }
  }

  size_t backlog() const override {
    if (hold_at == 0) return 0;
    const size_t live = collected.size() - drained;
    return (live >= hold_at) ? live : 0;
  }

  void on_done(int st, uint64_t n) override {
    ++done_count;
    status = st;
    bytes = n;
    if (finish_hook) finish_hook();
  }

  uvcpp_web_file_transfer* transfer;
  size_t hold_at;
  size_t drained;
  int cancel_on_call;
  size_t data_calls;
  int done_count;
  int status;
  uint64_t bytes;
  std::string collected;
  std::function<void()> finish_hook;
};

// ---------------------------------------------------------------------------
// 场景骨架：一个 loop + 一个看门狗（把「链没跑完」变成确定的失败，而不是
// 永久挂起）+ 一个可选的 1ms 轮询器。
//
// 轮询器是必需的，不是偷懒：**背压暂停时没有任何 fs 操作在途**，状态机不会
// 自己往前走，唯一的唤醒源 `resume()` 只能从外部来。而"外部"在真实系统里是
// 写完成回调，在这里就只能是定时器。
// ---------------------------------------------------------------------------
class scenario {
 public:
  scenario(const char* name, int timeout_ms)
      : name_(name), timed_out_(false), loop_() {
    loop_.init();
    watchdog_.reset(new uvcpp_timer(&loop_));
    watchdog_->start(
        [this, timeout_ms](uvcpp_timer*) {
          timed_out_.store(true);
          std::cerr << "  [FAIL] " << name_ << ": 看门狗超时（" << timeout_ms
                    << "ms）—— 异步链没有跑完" << std::endl;
          loop_.stop();
        },
        timeout_ms, 0);
  }

  uvcpp_loop* loop() { return &loop_; }
  bool timed_out() const { return timed_out_.load(); }

  void note_loop_thread() { loop_thread_ = std::this_thread::get_id(); }
  bool on_loop_thread() const {
    return std::this_thread::get_id() == loop_thread_;
  }

  /// 装一个每 `interval_ms` 跑一次的轮询器。
  void poll(int interval_ms, std::function<void()> fn) {
    poller_.reset(new uvcpp_timer(&loop_));
    poller_->start([fn](uvcpp_timer*) { fn(); }, interval_ms, interval_ms);
  }

  /// 收尾：把两个定时器都停掉再停循环。停定时器是必要的 —— 循环停掉之后
  /// 还留着一个活跃的 repeat 定时器没有害处，但会让人误以为"还没结束"。
  void stop() {
    if (watchdog_) watchdog_->stop();
    if (poller_) poller_->stop();
    loop_.stop();
  }

 private:
  std::string name_;
  std::atomic<bool> timed_out_;
  uvcpp_loop loop_;
  uvcpp_test::loop_drain drain_loop_{&loop_};
  std::unique_ptr<uvcpp_timer> watchdog_;
  std::unique_ptr<uvcpp_timer> poller_;
  std::thread::id loop_thread_;
};

/// 一次传输的全部可观测量。
struct xfer_result {
  xfer_result()
      : start_rc(0),
        done_count(0),
        status(0),
        bytes(0),
        data_calls(0),
        slice(0),
        high_water(0),
        peak_window(0),
        read_submits(0),
        pause_events(0),
        cancelled(false),
        timed_out(false),
        start_called_back(false),
        data("") {}

  int start_rc;
  int done_count;
  int status;
  uint64_t bytes;
  size_t data_calls;
  size_t slice;
  size_t high_water;
  size_t peak_window;
  size_t read_submits;
  size_t pause_events;
  bool cancelled;
  bool timed_out;
  /// 只对 `start_defers_callback` 有意义：`start()` 返回的那一刻有没有回调。
  bool start_called_back;
  std::string data;
};

/// 跑一次 `[first, last]` 的下发，收工后把可观测量填进 `out`。
/// 返回 `start()` 是否提交成功。
bool run_transfer(const std::string& path, uint64_t first, uint64_t last,
                  xfer_result* out, size_t slice_bytes = 0,
                  size_t high_water = 0, int cancel_on_call = 0,
                  size_t hold_at = 0, bool drain_on_pause = false) {
  scenario env("transfer", 30000);
  env.note_loop_thread();

  test_sink sink;
  sink.cancel_on_call = cancel_on_call;
  sink.hold_at = hold_at;

  std::shared_ptr<uvcpp_web_file_transfer> t(
      new uvcpp_web_file_transfer(env.loop(), &sink));
  if (slice_bytes != 0) t->set_slice_bytes(slice_bytes);
  if (high_water != 0) t->set_high_water_bytes(high_water);
  sink.transfer = t.get();
  sink.finish_hook = [&env]() { env.stop(); };

  const int rc = t->start(path, first, last);
  out->start_rc = rc;
  if (rc != 0) {
    // 头文件的契约：提交失败时**不会有任何回调**。
    out->timed_out = false;
    out->start_called_back = false;
    return false;
  }

  // `seen_pauses` 必须在**外面**声明：它由下面的 lambda 按引用捕获，而 poller
  // 活到 `env` 析构时为止。写在 `if` 块里会在 `run()` 之前就出作用域。
  size_t seen_pauses = 0;
  if (drain_on_pause) {
    // 「消费者腾出空间」的等价物：**只在刚发生过一次暂停时**才把已收下的量
    // 整体标记为已消费，然后 resume。
    //
    // 这个"边沿触发"不是讲究，是必需的：libuv 的重复定时器在每轮循环的**开头**
    // 触发，而 fs 完成回调在后面的 poll 阶段才跑。于是"每一拍都排空"会让每次交付
    // 之前 backlog 刚被清零，交付之后 backlog 永远只剩一片 —— 高水位**一次都到
    // 不了**，`pause_events >= 1` 必红，而失败信息会指向背压，与真正的原因
    // （消费者的节奏被我自己写错了）差得很远。
    env.poll(1, [&sink, &t, &seen_pauses]() {
      if (t->done()) return;
      if (t->pause_events() == seen_pauses) return;  // 没有新的暂停，不动
      seen_pauses = t->pause_events();
      sink.drained = sink.collected.size();
      t->resume();
    });
  }

  env.loop()->run(UV_RUN_DEFAULT);

  // 收工之后再泵几拍：`on_done` 的契约是**恰好一次**，而"多来一次"这种事
  // 只会发生在传输已经结束之后的下一轮循环里。
  for (int i = 0; i < 5; ++i) {
    if (env.loop()->run(UV_RUN_NOWAIT) == 0) break;
  }

  out->timed_out = env.timed_out();
  out->done_count = sink.done_count;
  out->status = sink.status;
  out->bytes = sink.bytes;
  out->data_calls = sink.data_calls;
  out->slice = t->slice_bytes();
  out->high_water = t->high_water_bytes();
  out->peak_window = t->peak_window_bytes();
  out->read_submits = t->read_submits();
  out->pause_events = t->pause_events();
  out->cancelled = t->cancelled();
  out->data = sink.collected;
  return true;
}

// ---------------------------------------------------------------------------
// 1. start_defers_callback —— `start()` 绝不同步回调
//
// 契约写在头文件里：每一次 `on_done` 都来自某笔 fs 操作的完成回调，所以调用方
// 可以放心地在 `start()` 返回之后继续设置自己的状态。这条契约一旦破，调用方
// 在 `on_done` 里访问的成员就是**还没初始化**的 —— 而症状是随机的野值，
// 不是崩溃，极难定位。
// ---------------------------------------------------------------------------
void test_start_defers_callback() {
  const std::string path = "uvcpp_sf_defer.bin";
  const std::string payload = make_payload(8192, 11);
  check(write_file(path, payload), "前置：测试文件必须写成功");

  scenario env("start_defers_callback", 30000);
  test_sink sink;
  std::shared_ptr<uvcpp_web_file_transfer> t(
      new uvcpp_web_file_transfer(env.loop(), &sink));
  sink.transfer = t.get();
  sink.finish_hook = [&env]() { env.stop(); };

  const int rc = t->start(path, 0, payload.size() - 1);
  check_eq_i(rc, 0, "start() 必须提交成功");

  // **返回之后、跑循环之前**：一个回调都不许有。
  check_eq_i(sink.data_calls, 0, "start() 返回时不得交付任何数据");
  check_eq_i(sink.done_count, 0, "start() 返回时不得已经结束");
  check(!t->done(), "start() 返回时 done() 必须为假");

  env.loop()->run(UV_RUN_DEFAULT);
  check(!env.timed_out(), "看门狗不得超时");
  check_eq_i(sink.done_count, 1, "跑完之后恰好一次 on_done");
  check_eq_i(sink.status, 0, "正常结束的状态码必须是 0");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 2. whole_file —— 最短的正路
// ---------------------------------------------------------------------------
void test_whole_file() {
  const std::string path = "uvcpp_sf_whole.bin";
  const std::string payload = make_payload(65536, 2);
  check(write_file(path, payload), "前置：测试文件必须写成功");

  xfer_result r;
  const bool started = run_transfer(path, 0, payload.size() - 1, &r);
  check(started, "start() 必须提交成功");
  check(!r.timed_out, "看门狗不得超时");
  check_eq_i(r.done_count, 1, "恰好一次 on_done");
  check_eq_i(r.status, 0, "正常结束的状态码必须是 0");
  check_eq_u64(r.bytes, payload.size(), "交付字节数必须等于文件大小");
  check_eq_u64(r.data.size(), payload.size(), "收到的字节数必须等于文件大小");
  check(r.data == payload, "内容必须逐字节相等");
  check(!r.cancelled, "正常结束不得被标记为已取消");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 3. slice_boundaries —— 闭区间四个角
//
// 闭区间最容易错的地方是 `last`：写成半开区间的话，`[0, size-1]` 会少一个
// 字节，而 `[999,999]`（单字节）会一个字节都读不出来。
// ---------------------------------------------------------------------------
void test_slice_boundaries() {
  const std::string path = "uvcpp_sf_bound.bin";
  const std::string payload = make_payload(1000, 3);
  check(write_file(path, payload), "前置：测试文件必须写成功");

  struct boundary_case {
    const char* label;
    uint64_t first;
    uint64_t last;
    size_t expect_len;
    size_t expect_off;
  };
  const boundary_case cases[] = {
      {"首字节 [0,0]", 0, 0, 1, 0},
      {"末字节 [999,999]", 999, 999, 1, 999},
      {"中段 [100,199]", 100, 199, 100, 100},
      {"整文件 [0,999]", 0, 999, 1000, 0},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    xfer_result r;
    const bool started =
        run_transfer(path, cases[i].first, cases[i].last, &r);
    check(started, std::string("start() 必须提交成功：") + cases[i].label);
    check(!r.timed_out, std::string("看门狗不得超时：") + cases[i].label);
    check_eq_i(r.status, 0, std::string("状态码必须是 0：") + cases[i].label);
    check_eq_u64(r.bytes, cases[i].expect_len,
                 std::string("交付字节数：") + cases[i].label);
    check(r.data == payload.substr(cases[i].expect_off, cases[i].expect_len),
          std::string("内容必须是被请求的那一段：") + cases[i].label);
  }

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 4. multi_slice_exact —— 跨片，且片数可数
//
// 文件取 slice 的整数倍，是为了让"少读一片/多读一片"这件事**当场**表现为
// 长度不符，而不是要等到内容比对才看得出来。
// ---------------------------------------------------------------------------
void test_multi_slice_exact() {
  const std::string path = "uvcpp_sf_multi.bin";
  const size_t k_slice = 4096;
  const size_t k_slices = 16;
  const std::string payload = make_payload(k_slice * k_slices, 4);
  check(write_file(path, payload), "前置：测试文件必须写成功");

  xfer_result r;
  const bool started =
      run_transfer(path, 0, payload.size() - 1, &r, k_slice);
  check(started, "start() 必须提交成功");
  check(!r.timed_out, "看门狗不得超时");
  check_eq_i(r.status, 0, "状态码必须是 0");
  check(r.data == payload, "跨片之后内容必须逐字节相等");
  check_eq_u64(r.bytes, payload.size(), "交付字节数必须等于文件大小");
  // `>=` 而不是 `==`：`uv_fs_read` 不保证读满，短读会让提交次数变多。
  // **如实记**：本平台很可能走不到短读（与 3b 步骤 1 实测的短写同族），
  // 所以这里不断言"恰好 N 片"，只在真的多了的时候打一行出来。
  check(r.read_submits >= k_slices, "片数不得少于「文件大小 / 切片」");
  if (r.read_submits != k_slices) {
    std::cout << "  [note] 提交了 " << r.read_submits << " 笔读，期望 "
              << k_slices << " 笔 —— 观察到了短读" << std::endl;
  }

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 5. backpressure_pauses —— 背压真的起过作用，且不丢字节
//
// 判据是两条缺一不可：`pause_events >= 1`（停了）**且** 内容逐字节相等
// （停/恢复期间一块字节都没少、没乱序）。只测前者的话，一个"暂停了就再也
// 不恢复、然后靠别的路径结束"的实现能过。
// ---------------------------------------------------------------------------
void test_backpressure_pauses() {
  const std::string path = "uvcpp_sf_bp.bin";
  const size_t k_slice = 4096;
  const size_t k_high_water = 8192;
  const std::string payload = make_payload(65536, 5);
  check(write_file(path, payload), "前置：测试文件必须写成功");

  xfer_result r;
  const bool started = run_transfer(path, 0, payload.size() - 1, &r, k_slice,
                                    k_high_water, 0, k_high_water, true);
  check(started, "start() 必须提交成功");
  check(!r.timed_out, "看门狗不得超时");
  check_eq_i(r.status, 0, "状态码必须是 0");
  check(r.pause_events >= 1, "背压必须真的起过作用（否则这一组什么都没测）");
  check(r.data == payload, "经过暂停/恢复之后内容必须仍然逐字节相等");
  check_eq_u64(r.bytes, payload.size(), "暂停不得丢掉任何一个字节");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 6. mem_bounded —— 本模块存在的全部意义
//
// 上界由**导出的常量**算出来：窗口 = slice + backlog，而 backlog 在交付时
// 最多是「刚过一点高水位」，于是峰值 <= high_water + 2 * slice。
// 同时断言它远小于文件大小 —— 否则这条断言在一个"整读"实现上也能过。
// 还断言 `pause_events >= 1`：没有暂停就意味着没走到界上，那测的是别的东西。
// ---------------------------------------------------------------------------
void test_mem_bounded() {
  const std::string path = "uvcpp_sf_mem.bin";
  // 8 MiB 是**按下面那条前置断言算出来的**，不是随手取的大数：
  // 默认配置下 bound = 1 MiB + 2 * 256 KiB = 1.5 MiB，而前置要求
  // `k_file > 4 * bound` ≈ 6 MiB。4 MiB 会让前置**当场红**（第一次就是这么写的）。
  // 取 8 MiB 是为了留出余量，而不是踩在 6 MiB 上。
  const size_t k_file = 8u * 1024u * 1024u;
  const std::string payload = make_payload(k_file, 6);
  check(write_file(path, payload), "前置：测试文件必须写成功");

  xfer_result r;
  // 用**默认**切片与高水位：这条断言要钉的是"出厂配置就是有界的"。
  const bool started = run_transfer(path, 0, payload.size() - 1, &r, 0, 0, 0,
                                    uvcpp_web_file_transfer::
                                            k_default_high_water_bytes,
                                    true);
  check(started, "start() 必须提交成功");
  check(!r.timed_out, "看门狗不得超时");
  check_eq_i(r.status, 0, "状态码必须是 0");
  check(r.data == payload, "内容必须逐字节相等");

  const size_t bound = r.high_water + 2 * r.slice;
  // 前置：文件必须**远大于**上界，否则"峰值小于文件大小"是同义反复。
  check(k_file > 4 * bound,
        "前置：文件大小必须远大于上界，这一组才有意义");
  check(r.pause_events >= 1, "背压必须真的起过作用");
  check(r.peak_window <= bound,
        "峰值在途字节不得超过 high_water + 2 * slice");
  check(r.peak_window * 2 < k_file, "峰值必须远小于文件大小");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 7. cancel_from_on_data —— 对端在交付回调里断开的等价物
//
// 这是真实系统里**最常见**的取消时机：`on_data` 里发现写不出去了。
// 所以取消必须能在这一句里发出来，且 `on_done` 仍然恰好一次。
// ---------------------------------------------------------------------------
void test_cancel_from_on_data() {
  const std::string path = "uvcpp_sf_cancel.bin";
  const size_t k_slice = 4096;
  const std::string payload = make_payload(65536, 7);
  check(write_file(path, payload), "前置：测试文件必须写成功");

  xfer_result r;
  const bool started = run_transfer(path, 0, payload.size() - 1, &r, k_slice,
                                    0, /*cancel_on_call=*/2);
  check(started, "start() 必须提交成功");
  check(!r.timed_out, "看门狗不得超时");
  check_eq_i(r.done_count, 1, "取消之后 on_done 仍然恰好一次");
  check_eq_i(r.status, UV_ECANCELED, "取消必须回报 UV_ECANCELED");
  check(r.cancelled, "cancelled() 必须为真");
  check_eq_u64(r.bytes, k_slice * 2, "只应交付取消前的那两片");
  check(r.bytes < payload.size(), "取消必须真的少发了一些字节");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 8. cancel_while_paused —— 因背压停下时取消
//
// **这一支不能省。** 因背压停下时没有任何 fs 操作在途，所以「等当前这一笔跑完
// 再关」这个说法在这里没有对象 —— 不自己推进，状态机就永远停在暂停态，
// `on_done` 一次都不来，调用方等一个永不到来的通知。所以这条用例的判据是
// 「看门狗没超时**且**恰好一次 on_done」—— 少了任何一个，实现都能蒙混过关。
// ---------------------------------------------------------------------------
void test_cancel_while_paused() {
  const std::string path = "uvcpp_sf_pcancel.bin";
  const size_t k_slice = 4096;
  const size_t k_high_water = 8192;
  const std::string payload = make_payload(65536, 8);
  check(write_file(path, payload), "前置：测试文件必须写成功");

  scenario env("cancel_while_paused", 30000);
  test_sink sink;
  sink.hold_at = k_high_water;  // 永远报背压 —— 不会自己恢复

  std::shared_ptr<uvcpp_web_file_transfer> t(
      new uvcpp_web_file_transfer(env.loop(), &sink));
  t->set_slice_bytes(k_slice);
  t->set_high_water_bytes(k_high_water);
  sink.transfer = t.get();
  sink.finish_hook = [&env]() { env.stop(); };

  bool cancelled_from_pause = false;
  env.poll(1, [&t, &cancelled_from_pause]() {
    if (t->done()) return;
    if (t->pause_events() > 0) {
      cancelled_from_pause = true;
      t->cancel();  // 这一句必须自己把状态机推走
    }
  });

  check_eq_i(t->start(path, 0, payload.size() - 1), 0, "start() 必须提交成功");
  env.loop()->run(UV_RUN_DEFAULT);

  check(cancelled_from_pause, "前置：必须在暂停态里取消（否则测的不是这一支）");
  check(!env.timed_out(), "暂停态取消不得挂起（这正是本组要钉的那条）");
  check_eq_i(sink.done_count, 1, "暂停态取消后 on_done 恰好一次");
  check_eq_i(sink.status, UV_ECANCELED, "暂停态取消必须回报 UV_ECANCELED");
  check(sink.bytes < payload.size(), "取消必须真的少发了一些字节");
  check(t->pause_events() >= 1, "前置：必须真的进过暂停态");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 9. cancel_before_start —— 幂等且无声
// ---------------------------------------------------------------------------
void test_cancel_before_start() {
  const std::string path = "uvcpp_sf_pre.bin";
  const std::string payload = make_payload(4096, 9);
  check(write_file(path, payload), "前置：测试文件必须写成功");

  scenario env("cancel_before_start", 30000);
  test_sink sink;
  std::shared_ptr<uvcpp_web_file_transfer> t(
      new uvcpp_web_file_transfer(env.loop(), &sink));
  sink.transfer = t.get();
  sink.finish_hook = [&env]() { env.stop(); };

  // `start()` 之前取消：还没开始的东西，`on_done` 对它没有承诺。
  t->cancel();
  check(!t->done(), "start() 之前取消不得让 done() 变真");
  check_eq_i(sink.done_count, 0, "start() 之前取消不得触发 on_done");

  // 取消过之后，`start()` 仍然必须走得通（幂等，不是"一次性开关"）。
  check_eq_i(t->start(path, 0, payload.size() - 1), 0,
             "取消过之后 start() 仍须成功");
  env.loop()->run(UV_RUN_DEFAULT);

  check(!env.timed_out(), "看门狗不得超时");
  check_eq_i(sink.done_count, 1, "恰好一次 on_done");
  check_eq_i(sink.status, 0, "start() 之前那次取消不得影响本次的状态码");
  check(sink.collected == payload, "内容必须逐字节相等");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 10. missing_file —— open 失败仍然要一个 `on_done`
//
// 与 `start()` 的同步失败不同：那条路**不回调**（libuv 根本没排队）；
// 这一条是异步失败（提交成功、open 报错），必须回调 —— 否则调用方永远等下去。
// 两者的分界正是"提交有没有成功"，这一组和上一组各钉一半。
// ---------------------------------------------------------------------------
void test_missing_file() {
  const std::string path = "uvcpp_sf_no_such_file_9f2a.bin";
  std::remove(path.c_str());

  xfer_result r;
  const bool started = run_transfer(path, 0, 1023, &r);
  check(started, "start() 提交必须成功（异步失败不在这里报）");
  check(!r.timed_out, "看门狗不得超时 —— 异步 open 失败必须回调");
  check_eq_i(r.done_count, 1, "恰好一次 on_done");
  check(r.status < 0, "打开不存在的文件必须回报负的错误码");
  check_eq_u64(r.bytes, 0, "一个字节都不该交付");
  check_eq_i(r.data_calls, 0, "on_data 一次都不该被调用");
}

// ---------------------------------------------------------------------------
// 11. empty_range —— `first > last` 是合法的空区间
//
// 头文件承诺它**仍然走 open**，好让 ENOENT 之类的错误照常上报。所以这一组是
// **一对**：存在的文件上给空区间 ⇒ 成功且 0 字节；不存在的文件上给空区间 ⇒
// 仍然报错。少了后半句，一个"空区间直接短路返回成功"的实现照样全绿。
// ---------------------------------------------------------------------------
void test_empty_range() {
  const std::string path = "uvcpp_sf_empty.bin";
  const std::string payload = make_payload(512, 10);
  check(write_file(path, payload), "前置：测试文件必须写成功");

  xfer_result r;
  check(run_transfer(path, 5, 4, &r), "start() 必须提交成功");
  check(!r.timed_out, "看门狗不得超时");
  check_eq_i(r.status, 0, "存在的文件上给空区间必须成功");
  check_eq_u64(r.bytes, 0, "空区间不得交付任何字节");
  check_eq_i(r.data_calls, 0, "空区间不得调用 on_data");
  check_eq_i(r.read_submits, 0, "空区间不得提交任何读");

  // 对照：同一个空区间，文件不存在时必须仍然报错。
  const std::string missing = "uvcpp_sf_empty_missing_7c1d.bin";
  std::remove(missing.c_str());
  xfer_result r2;
  check(run_transfer(missing, 5, 4, &r2), "start() 必须提交成功");
  check(!r2.timed_out, "看门狗不得超时");
  check(r2.status < 0, "空区间也必须走 open，所以缺文件要报错");
  check_eq_u64(r2.bytes, 0, "错误路径不得交付任何字节");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 12. on_done_exactly_once —— 三种收尾路径各跑 5 轮，每轮恰好一次
//
// 三种收尾是三条不同的代码路径：正常读完、取消、异步 open 失败。它们各错一次
// 的表现不一样，但共同的不变式只有一个：「恰好一次」。跑 5 轮是为了让
// "第二轮的收尾被漏掉"这类只在特定计数下出现的问题也有机会露面。
// ---------------------------------------------------------------------------
void test_on_done_exactly_once() {
  const std::string path = "uvcpp_sf_once.bin";
  const std::string payload = make_payload(32768, 12);
  check(write_file(path, payload), "前置：测试文件必须写成功");
  const std::string missing = "uvcpp_sf_once_missing_4b8e.bin";
  std::remove(missing.c_str());

  for (int round = 0; round < 5; ++round) {
    xfer_result normal;
    check(run_transfer(path, 0, payload.size() - 1, &normal, 4096),
          "正常路径 start() 必须提交成功");
    check_eq_i(normal.done_count, 1, "正常路径：恰好一次 on_done");
    check_eq_i(normal.status, 0, "正常路径：状态码必须是 0");

    xfer_result cancelled;
    check(run_transfer(path, 0, payload.size() - 1, &cancelled, 4096, 0,
                       /*cancel_on_call=*/1),
          "取消路径 start() 必须提交成功");
    check_eq_i(cancelled.done_count, 1, "取消路径：恰好一次 on_done");
    check_eq_i(cancelled.status, UV_ECANCELED, "取消路径：状态码必须是取消");

    xfer_result failed;
    check(run_transfer(missing, 0, 1023, &failed),
          "失败路径 start() 必须提交成功");
    check_eq_i(failed.done_count, 1, "失败路径：恰好一次 on_done");
    check(failed.status < 0, "失败路径：状态码必须是负值");
  }

  std::remove(path.c_str());
}

// ===========================================================================
// 应用层：真端口、真连接（步骤 6）
//
// 与上面那批的分工：上面测「分片读本身」，这里测「接线」——`send_file*` 的
// 头部形状、失败映射、静态服务的阈值分流。判据尽量取**线上字节**，而不是
// 内部计数，因为内部计数对「框架压根没调它」是无感的。
// ===========================================================================

bool ensure_dir(const std::string& path) {
  uv_fs_t req;
  const int rc = uv_fs_mkdir(nullptr, &req, path.c_str(), 0755, nullptr);
  uv_fs_req_cleanup(&req);
  return rc == 0 || rc == UV_EEXIST;
}

void remove_dir(const std::string& path) {
  uv_fs_t req;
  uv_fs_rmdir(nullptr, &req, path.c_str(), nullptr);
  uv_fs_req_cleanup(&req);
}

std::string hex_of(size_t n) {
  static const char* k = "0123456789abcdef";
  if (n == 0) return "0";
  std::string s;
  while (n != 0) {
    s.insert(s.begin(), k[n & 0xf]);
    n >>= 4;
  }
  return s;
}

std::string chunk_frame(const std::string& d) {
  return hex_of(d.size()) + "\r\n" + d + "\r\n";
}

std::string get_request(const std::string& path) {
  return "GET " + path + " HTTP/1.1\r\nHost: t\r\n\r\n";
}

std::string head_request(const std::string& path) {
  return "HEAD " + path + " HTTP/1.1\r\nHost: t\r\n\r\n";
}

std::string lower_copy(const std::string& s) {
  std::string r(s);
  for (size_t i = 0; i < r.size(); ++i) {
    if (r[i] >= 'A' && r[i] <= 'Z') r[i] = static_cast<char>(r[i] + 32);
  }
  return r;
}

bool split_head_body(const std::string& raw, std::string& head, std::string& body) {
  const size_t pos = raw.find("\r\n\r\n");
  if (pos == std::string::npos) return false;
  head = raw.substr(0, pos);
  body = raw.substr(pos + 4);
  return true;
}

int status_of(const std::string& head) {
  if (head.size() < 12) return -1;
  return std::atoi(head.c_str() + 9);
}

long long number_header(const std::string& low_head, const char* name) {
  const std::string key = std::string("\r\n") + name + ":";
  size_t p = low_head.find(key);
  if (p == std::string::npos) return -1;
  p += key.size();
  while (p < low_head.size() && low_head[p] == ' ') ++p;
  return std::atoll(low_head.c_str() + p);
}

struct app_reply {
  app_reply()
      : status(-1), content_length(-1), chunked(false), closed(false) {}
  std::string head_low;
  std::string body;
  int         status;
  long long   content_length;
  bool        chunked;
  bool        closed;
};

class app_conn {
 public:
  app_conn() : loop_(nullptr), connected_(false), written_(false), closed_(false) {}

  bool open(int port, int timeout_ms = 5000) {
    const int rc = c_.connect("127.0.0.1", port, [this](int st) {
      if (st != 0) return;
      connected_ = true;
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

  bool write(const std::string& s, int timeout_ms = 5000) {
    if (s.empty()) return true;
    written_ = false;
    const int rc = c_.write(s.data(), s.size(), [this](int) { written_ = true; });
    if (rc != 0) return false;
    return pump_until([this]() { return written_; }, timeout_ms);
  }

  template <typename Pred>
  bool pump_until(Pred pred, int timeout_ms) {
    return uvcpp_test::wait_until(loop_, pred, timeout_ms);
  }

  const std::string& rx() const { return rx_; }
  bool peer_closed() const { return closed_; }

  ~app_conn() {
    if (loop_ == nullptr || c_.get_tcp() == nullptr) return;
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

/**
 * @param wait_close 真的等到对端关连接（失败截断、404 那类用）；
 *                   否则按 CL / chunked 终止块判定"这条报文收完了"。
 *
 * HEAD 自动只等头部 —— 它的 CL 描述的是 GET 该发多少，body 永远不来，
 * 按 CL 等会白等到超时。
 */
bool do_request(int port, const std::string& req, bool wait_close, int timeout_ms,
                app_reply* out) {
  const bool is_head = req.compare(0, 5, "HEAD ") == 0;
  app_conn c;
  if (!c.open(port, timeout_ms)) return false;
  if (!c.write(req, timeout_ms)) return false;

  if (wait_close) {
    c.pump_until([&c]() { return c.peer_closed(); }, timeout_ms);
  } else if (is_head) {
    c.pump_until(
        [&c]() {
          std::string h, b;
          return split_head_body(c.rx(), h, b);
        },
        timeout_ms);
  } else {
    c.pump_until(
        [&c]() {
          std::string h, b;
          if (!split_head_body(c.rx(), h, b)) return false;
          const std::string low = lower_copy(h);
          if (low.find("transfer-encoding: chunked") != std::string::npos) {
            return b.compare(0, 5, "0\r\n\r\n") == 0 ||
                   b.find("\r\n0\r\n\r\n") != std::string::npos;
          }
          const long long n = number_header(low, "content-length");
          if (n < 0) return false;
          return static_cast<long long>(b.size()) >= n;
        },
        timeout_ms);
  }

  std::string head, body;
  if (!split_head_body(c.rx(), head, body)) return false;
  out->head_low = lower_copy(head);
  out->body = body;
  out->status = status_of(head);
  out->content_length = number_header(out->head_low, "content-length");
  out->chunked =
      out->head_low.find("transfer-encoding: chunked") != std::string::npos;
  out->closed = c.peer_closed();
  return true;
}

void configure_app(uvcpp_web_app& app) {
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);
}

// --- small_file：两种长度模式各走一次 ---------------------------------------
void test_small_file() {
  const std::string path = "uvcpp_sf_app_small.bin";
  const std::string payload = make_payload(4096, 11);
  check(write_file(path, payload), "前置：写文件");
  const int64_t size = static_cast<int64_t>(payload.size());

  uvcpp_web_app app;
  configure_app(app);
  app.get("/small", [path, size](uvcpp_web_request&, uvcpp_web_response& resp,
                                 uvcpp_web_next) { resp.send_file(path, size); });
  app.get("/small_chunked", [path](uvcpp_web_request&, uvcpp_web_response& resp,
                                   uvcpp_web_next) { resp.send_file(path); });
  check_eq_i(app.start_background(), 0, "前置：启动");
  const int port = app.bound_port();
  check(port > 0, "前置：端口有效");

  app_reply r;
  check(do_request(port, get_request("/small"), false, 8000, &r), "GET /small 有响应");
  check_eq_i(r.status, 200, "CL 模式：200");
  check_eq_i(r.content_length, static_cast<long long>(size), "CL 模式：CL 等于文件大小");
  check(!r.chunked, "CL 模式：不带 transfer-encoding");
  check(r.body == payload, "CL 模式：字节逐字节相同");

  app_reply c;
  check(do_request(port, get_request("/small_chunked"), false, 8000, &c),
        "GET /small_chunked 有响应");
  check_eq_i(c.status, 200, "chunked 模式：200");
  check(c.chunked, "chunked 模式：带 transfer-encoding: chunked");
  check_eq_i(c.content_length, -1, "chunked 模式：不带 content-length");
  // 前提：文件 4 KiB < 默认切片 256 KiB ⇒ 一次读、一块 body。
  check(c.body == chunk_frame(payload) + "0\r\n\r\n",
        "chunked 模式：线上字节逐字节相同（含终止块）");

  app.stop();
  app.join();
  std::remove(path.c_str());
}

// --- large_file：文件 > 切片，走多片 ----------------------------------------
void test_large_file() {
  const std::string path = "uvcpp_sf_app_large.bin";
  const std::string payload = make_payload(1024 * 1024 + 777, 23);
  check(write_file(path, payload), "前置：写文件");
  const int64_t size = static_cast<int64_t>(payload.size());

  uvcpp_web_app app;
  configure_app(app);
  app.get("/large", [path, size](uvcpp_web_request&, uvcpp_web_response& resp,
                                 uvcpp_web_next) { resp.send_file(path, size); });
  check_eq_i(app.start_background(), 0, "前置：启动");
  const int port = app.bound_port();

  app_reply r;
  check(do_request(port, get_request("/large"), false, 30000, &r), "GET /large 有响应");
  check_eq_i(r.status, 200, "多片：200");
  check_eq_i(r.content_length, static_cast<long long>(size), "多片：CL");
  check_eq_i(static_cast<long long>(r.body.size()), static_cast<long long>(size),
             "多片：收到全部字节");
  check(r.body == payload, "多片：字节逐字节相同");

  app.stop();
  app.join();
  std::remove(path.c_str());
}

// --- range_206：闭区间边界 ---------------------------------------------------
void test_range_206() {
  const std::string path = "uvcpp_sf_app_range.bin";
  const std::string payload = make_payload(65536, 31);
  check(write_file(path, payload), "前置：写文件");

  uvcpp_web_app app;
  configure_app(app);
  app.get("/range/:first/:last",
          [path](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next) {
            const std::string* raw_first = req.param("first");
            const std::string* raw_last = req.param("last");
            if (raw_first == nullptr || raw_last == nullptr) {
              resp.bad_request().end();
              return;
            }
            const uint64_t f = std::strtoull(raw_first->c_str(), nullptr, 10);
            const uint64_t l = std::strtoull(raw_last->c_str(), nullptr, 10);
            resp.send_file_range(path, f, l);
          });
  check_eq_i(app.start_background(), 0, "前置：启动");
  const int port = app.bound_port();

  // 状态码不在这里判：`send_file_range` 只负责产出那一段字节，206 与
  // Content-Range 由**调用方**（静态服务）设 —— 下面 static_streams_large
  // 那一组打的就是那一步。
  static const uint64_t kRanges[][2] = {
      {0, 0}, {0, 63}, {100, 199}, {0, 65535}, {65535, 65535}, {65534, 65535}};
  for (size_t i = 0; i < sizeof(kRanges) / sizeof(kRanges[0]); ++i) {
    const uint64_t f = kRanges[i][0];
    const uint64_t l = kRanges[i][1];
    const std::string url = "/range/" + std::to_string(f) + "/" + std::to_string(l);
    app_reply r;
    check(do_request(port, get_request(url), false, 8000, &r), "区间 " + url + " 有响应");
    check_eq_i(r.content_length, static_cast<long long>(l - f + 1),
               "区间 " + url + "：CL 等于闭区间长度");
    check(r.body == payload.substr(static_cast<size_t>(f), static_cast<size_t>(l - f + 1)),
          "区间 " + url + "：字节逐字节相同");
  }

  app.stop();
  app.join();
  std::remove(path.c_str());
}

// --- 静态服务：阈值分流的那一对 ---------------------------------------------
//
// 两组的判据都是**缓存账目**，不是内存峰值：阈值分支在缓存探测**之前**，
// 所以走流式的文件既不命中也不未命中。
//
// `static_caches_small` 与 `static_streams_large` **是一对**，缺任一半都不成立：
// 只有前者，「一律走流式」的实现能过；只有后者，「阈值形同虚设」的实现能过。

std::shared_ptr<uvcpp_web_static> mount_assets(uvcpp_web_app& app,
                                                const std::string& dir) {
  uvcpp_web_static_options opts;
  opts.max_cached_file_size = 4096;
  return app.serve_static("/assets", dir, opts);
}

void test_static_caches_small() {
  const std::string dir = "uvcpp_sf_app_assets";
  const std::string path = dir + "/small.bin";
  const std::string payload = make_payload(1024, 61);
  check(ensure_dir(dir), "前置：建目录");
  check(write_file(path, payload), "前置：写文件");

  uvcpp_web_app app;
  configure_app(app);
  std::shared_ptr<uvcpp_web_static> st = mount_assets(app, dir);
  check(st != nullptr, "前置：静态服务已挂载");
  check_eq_i(app.start_background(), 0, "前置：启动");
  const int port = app.bound_port();

  app_reply a;
  check(do_request(port, get_request("/assets/small.bin"), false, 8000, &a),
        "第一次：有响应");
  check_eq_i(a.status, 200, "第一次：200");
  check(a.body == payload, "第一次：字节相同");

  app_reply b;
  check(do_request(port, get_request("/assets/small.bin"), false, 8000, &b),
        "第二次：有响应");
  check(b.body == payload, "第二次：字节相同");
  check_eq_i(static_cast<long long>(st->cache_hits()), 1,
             "≤ 阈值：第二次必须命中 LRU");
  check_eq_i(static_cast<long long>(st->cache_entries()), 1,
             "≤ 阈值：文件在缓存里");

  app.stop();
  app.join();
  std::remove(path.c_str());
  remove_dir(dir);
}

void test_static_streams_large() {
  const std::string dir = "uvcpp_sf_app_assets";
  const std::string path = dir + "/big.bin";
  const std::string payload = make_payload(32768, 71);
  check(ensure_dir(dir), "前置：建目录");
  check(write_file(path, payload), "前置：写文件");

  uvcpp_web_app app;
  configure_app(app);
  std::shared_ptr<uvcpp_web_static> st = mount_assets(app, dir);
  check(st != nullptr, "前置：静态服务已挂载");
  check_eq_i(app.start_background(), 0, "前置：启动");
  const int port = app.bound_port();

  app_reply a;
  check(do_request(port, get_request("/assets/big.bin"), false, 8000, &a),
        "第一次：有响应");
  check_eq_i(a.status, 200, "第一次：200");
  check(a.body == payload, "第一次：字节相同");

  app_reply b;
  check(do_request(port, get_request("/assets/big.bin"), false, 8000, &b),
        "第二次：有响应");
  check(b.body == payload, "第二次：字节相同");
  check_eq_i(static_cast<long long>(st->cache_hits()), 0,
             "> 阈值：走流式，一次都不该命中");
  check_eq_i(static_cast<long long>(st->cache_entries()), 0,
             "> 阈值：文件不进 LRU");

  // 分流不得改坏 Range —— 静态服务自己设 206 与 Content-Range，分片只产出那段。
  const std::string rreq =
      "GET /assets/big.bin HTTP/1.1\r\nHost: t\r\nRange: bytes=100-199\r\n\r\n";
  app_reply rg;
  check(do_request(port, rreq, false, 8000, &rg), "Range：有响应");
  check_eq_i(rg.status, 206, "Range：206");
  check(rg.head_low.find("content-range: bytes 100-199/32768") != std::string::npos,
        "Range：Content-Range 正确");
  check(rg.body == payload.substr(100, 100), "Range：字节是那一段");

  app.stop();
  app.join();
  std::remove(path.c_str());
  remove_dir(dir);
}

// --- head_no_read ------------------------------------------------------------
void test_head_no_read() {
  const std::string dir = "uvcpp_sf_app_assets";
  const std::string path = dir + "/big.bin";
  const std::string payload = make_payload(32768, 71);
  check(ensure_dir(dir), "前置：建目录");
  check(write_file(path, payload), "前置：写文件");

  uvcpp_web_app app;
  configure_app(app);
  check(mount_assets(app, dir) != nullptr, "前置：静态服务已挂载");
  check_eq_i(app.start_background(), 0, "前置：启动");
  const int port = app.bound_port();

  app_reply g;
  check(do_request(port, get_request("/assets/big.bin"), false, 8000, &g), "GET 有响应");
  check_eq_i(g.content_length, static_cast<long long>(payload.size()), "GET：CL");

  app_reply h;
  check(do_request(port, head_request("/assets/big.bin"), false, 8000, &h), "HEAD 有响应");
  check_eq_i(h.status, 200, "HEAD：200");
  check_eq_i(h.content_length, g.content_length, "HEAD：CL 与 GET 一致");
  check(h.body.empty(), "HEAD：body 一个字节都没有");

  // 这里的 32768 字节超过本用例的 `max_cached_file_size`（4096），走的是
  // `send_file_range()` 那条**流式**路 —— 那条路上 HEAD 确实不读盘
  // （`start_file_transfer()` 见到 `head_only_` 就只发头、一个字节不读）。
  //
  // **别把这句读成"HEAD 一律不读盘"**：可缓存的小文件那条路上，HEAD 与 GET
  // 走的是同一条路（一样读盘、一样进缓存），因为 `Content-Length` 与
  // `Content-Encoding` 都是 HTTP 层压缩**之后**才算出来的，不读盘就算不出来
  // —— 报错了长度，拿 HEAD 探长度再按长度读满的客户端会一直等到超时
  // （`web_app_static_func.cpp` 的 `head-compressed` 钉着这一点）。
  //
  // 「不读盘」的可观测形态仍然只有上面那条（CL 照给、body 全无）。缺失文件的
  // 404 就是它与 GET 共用的那道 stat 闸门，一并钉在这里。
  app_reply m;
  check(do_request(port, get_request("/assets/nope.bin"), false, 8000, &m),
        "缺失文件：有响应");
  check_eq_i(m.status, 404, "缺失文件：GET 404");
  app_reply mh;
  check(do_request(port, head_request("/assets/nope.bin"), false, 8000, &mh),
        "缺失文件 HEAD：有响应");
  check_eq_i(mh.status, 404, "缺失文件：HEAD 也是 404");

  app.stop();
  app.join();
  std::remove(path.c_str());
  remove_dir(dir);
}

// --- not_found_404：首字节之前失败 ------------------------------------------
void test_not_found_404() {
  const std::string missing = "uvcpp_sf_app_absent_3c7a.bin";

  uvcpp_web_app app;
  configure_app(app);
  app.get("/missing", [missing](uvcpp_web_request&, uvcpp_web_response& resp,
                                uvcpp_web_next) { resp.send_file(missing, 4096); });
  app.get("/missing_chunked",
          [missing](uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next) {
            resp.send_file(missing);
          });
  check_eq_i(app.start_background(), 0, "前置：启动");
  const int port = app.bound_port();

  app_reply r;
  check(do_request(port, get_request("/missing"), true, 8000, &r), "CL 模式：有响应");
  check_eq_i(r.status, 404, "CL 模式：404");
  check(r.head_low.find("connection: close") != std::string::npos,
        "CL 模式：带 connection: close");
  check(!r.body.empty(), "CL 模式：有错误正文");
  check(r.closed, "CL 模式：连接真被服务端关掉");

  app_reply c;
  check(do_request(port, get_request("/missing_chunked"), true, 8000, &c),
        "chunked 模式：有响应");
  check_eq_i(c.status, 404, "chunked 模式：404");

  app.stop();
  app.join();
}

// --- read_error_midstream：头部已上线之后失败 -------------------------------
void test_read_error_midstream() {
  const std::string path = "uvcpp_sf_app_short.bin";
  const std::string payload = make_payload(8192, 53);
  check(write_file(path, payload), "前置：写文件");
  // 声明一个比文件长的区间：第一片读回全部 8192 字节，第二片撞上真 EOF，
  // 而区间还没走完 ⇒ `stop_at_eof_` 为假 ⇒ 传输以 UV_EOF 收尾。
  const uint64_t declared = payload.size() + 4096;

  uvcpp_web_app app;
  configure_app(app);
  app.get("/short", [path, declared](uvcpp_web_request&, uvcpp_web_response& resp,
                                     uvcpp_web_next) {
    resp.send_file_range(path, 0, declared - 1);
  });
  check_eq_i(app.start_background(), 0, "前置：启动");
  const int port = app.bound_port();

  app_reply r;
  check(do_request(port, get_request("/short"), true, 8000, &r), "有响应");
  check_eq_i(r.status, 200, "头部已上线：状态码仍是 200，改不了");
  check_eq_i(r.content_length, static_cast<long long>(declared), "CL 是声明的长度");
  check_eq_i(static_cast<long long>(r.body.size()),
             static_cast<long long>(payload.size()), "body 是真实文件长度");
  check(static_cast<long long>(r.body.size()) < r.content_length,
        "长度对不上 —— 这正是客户端看得见的失败信号");
  check(r.body == payload, "收到的那一段本身是对的");
  check(r.closed, "失败后连接被关闭");

  app.stop();
  app.join();
  std::remove(path.c_str());
}

// --- mem_bounded_regression：64 MiB 端到端 ----------------------------------
//
// **内存上界本身在传输层那一组（`mem_bounded`）里断言**，不在这一组：
// 传输对象在响应内部，应用层拿不到 `peak_window_bytes()`，所以这一组能给的
// 是"64 MiB 也是逐字节正确的"（切片循环跑 256 轮），而上界由那一组用导出的
// 常量钉住。两者合起来才是完整的判据。
void test_mem_bounded_regression() {
  const std::string path = "uvcpp_sf_app_mem.bin";
  const size_t kBig = 64u * 1024u * 1024u;
  const std::string payload = make_payload(kBig, 83);
  check(payload.size() == kBig, "前置：载荷大小");
  check(write_file(path, payload), "前置：写文件");

  uvcpp_web_app app;
  configure_app(app);
  app.get("/mem", [path, kBig](uvcpp_web_request&, uvcpp_web_response& resp,
                               uvcpp_web_next) {
    resp.send_file(path, static_cast<int64_t>(kBig));
  });
  check_eq_i(app.start_background(), 0, "前置：启动");
  const int port = app.bound_port();

  app_reply r;
  check(do_request(port, get_request("/mem"), false, 120000, &r), "64 MiB：有响应");
  check_eq_i(r.status, 200, "64 MiB：200");
  check_eq_i(r.content_length, static_cast<long long>(kBig), "64 MiB：CL");
  check_eq_i(static_cast<long long>(r.body.size()), static_cast<long long>(kBig),
             "64 MiB：全部到达");
  check(r.body == payload, "64 MiB：字节逐字节相同");

  app.stop();
  app.join();
  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 用例表
// ---------------------------------------------------------------------------
struct case_entry {
  const char* name;
  void (*fn)();
};

}  // namespace

int main(int argc, char** argv) {
  std::string only;
  if (argc > 1) only = argv[1];

  const case_entry cases[] = {
      {"start_defers_callback", test_start_defers_callback},
      {"whole_file", test_whole_file},
      {"slice_boundaries", test_slice_boundaries},
      {"multi_slice_exact", test_multi_slice_exact},
      {"backpressure_pauses", test_backpressure_pauses},
      {"mem_bounded", test_mem_bounded},
      {"cancel_from_on_data", test_cancel_from_on_data},
      {"cancel_while_paused", test_cancel_while_paused},
      {"cancel_before_start", test_cancel_before_start},
      {"missing_file", test_missing_file},
      {"empty_range", test_empty_range},
      {"on_done_exactly_once", test_on_done_exactly_once},
      // ---- 应用层（真端口、真连接）----
      {"small_file", test_small_file},
      {"large_file", test_large_file},
      {"range_206", test_range_206},
      {"static_caches_small", test_static_caches_small},
      {"static_streams_large", test_static_streams_large},
      {"head_no_read", test_head_no_read},
      {"not_found_404", test_not_found_404},
      {"read_error_midstream", test_read_error_midstream},
      {"mem_bounded_regression", test_mem_bounded_regression},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (!only.empty() &&
        std::string(cases[i].name).find(only) == std::string::npos) {
      continue;
    }
    // 每换一组就把计数清零：汇总只看「这一组有没有红」。
    const int before = g_failures;
    std::cout << "[web_app_send_file] " << cases[i].name << std::endl;
    cases[i].fn();
    const bool ok = (g_failures == before);
    std::cout << (ok ? "  -> PASS" : "  -> FAIL") << std::endl;
    if (!ok) {
      std::cout << "[web_app_send_file] FAIL" << std::endl;
      return 2;
    }
  }

  std::cout << "[web_app_send_file] ALL PASS" << std::endl;
  return 0;
}
