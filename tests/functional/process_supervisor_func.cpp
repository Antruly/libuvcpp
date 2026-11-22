/**
 * @file tests/functional/process_supervisor_func.cpp
 * @brief 进程监管：输出、退出码、超时、进程组终止、取消、有界缓冲。
 *
 * 只在 POSIX 上跑（Windows 的 Job Object 是 issue #69 的后续；这一版在
 * Windows 上 `kill_tree=true` 会返回 ENOTSUP，直接 skip）。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#ifdef _WIN32

int main() {
  std::cout << "SKIP: process supervisor (POSIX) on Windows" << std::endl;
  return 0;
}

#else

#include <cerrno>
#include <chrono>
#include <csignal>
#include <sys/types.h>

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_process_supervisor.h>
#include <uvcpp/uvcpp_define.h>

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "  [FAIL] " << what << std::endl;
  }
}

bool run_until(uvcpp_loop& loop, const bool& done, int ms) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!done && std::chrono::steady_clock::now() < deadline) {
    loop.run(UV_RUN_ONCE);
  }
  for (int i = 0; i < 5; ++i) loop.run(UV_RUN_NOWAIT);
  return done;
}

void test_basic() {
  std::cout << "-- 输出 / 退出码 --" << std::endl;
  uvcpp_loop loop;
  bool       done = false;
  uvcpp_process_result result;
  std::string out;
  std::string err;
  size_t      chunks = 0;
  {
    uvcpp_process_supervisor sup(&loop);
    uvcpp_process_spec       spec;
    spec.file    = "/bin/sh";
    spec.args    = std::vector<std::string>{
        "-c", "printf OUT; printf ERR >&2; exit 3"};
    spec.timeout_ms = 5000;
    const uint64_t id = sup.spawn(
        spec,
        [&](const char* d, size_t n, bool is_err) {
          ++chunks;
          (is_err ? err : out).append(d, n);
        },
        [&](const uvcpp_process_result& r) {
          result = r;
          done   = true;
        });
    check(id != 0, "spawn 返回 id");
    if (id == 0) return;
    check(run_until(loop, done, 5000), "进程收尾");
    check(result.exit_status == 3, "退出码 3（实际 " +
                                        std::to_string(result.exit_status) + "）");
    check(result.term_signal == 0, "不是被信号杀死");
    check(result.stdout_tail == "OUT", "stdout 尾缓冲 = OUT（实际 '" +
                                           result.stdout_tail + "'）");
    check(result.stderr_tail == "ERR", "stderr 尾缓冲 = ERR");
    check(result.stdout_truncated == 0 && result.stderr_truncated == 0,
          "未截断");
    check(chunks >= 1, "output 回调被调用");
    check(sup.running() == 0, "running() 归零");
    check(sup.stats().failed == 1, "非零退出计入 stats.failed");
  }
}

void test_timeout_and_tree() {
  std::cout << "-- 超时 / 进程组终止 --" << std::endl;
  uvcpp_loop loop;
  bool       done = false;
  uvcpp_process_result result;
  std::string out;
  {
    uvcpp_process_supervisor sup(&loop);
    uvcpp_process_spec       spec;
    spec.file       = "/bin/sh";
    spec.args       = std::vector<std::string>{
        "-c", "sleep 30 & echo $!; wait"};
    spec.timeout_ms = 200;
    spec.stop_grace_ms = 200;
    spec.kill_tree  = true;
    const uint64_t id = sup.spawn(
        spec, [&](const char* d, size_t n, bool) { out.append(d, n); },
        [&](const uvcpp_process_result& r) { result = r; done = true; });
    check(id != 0, "spawn 返回 id");
    if (id == 0) return;
    check(run_until(loop, done, 5000), "超时后收尾");
    check(result.timed_out, "结果标记 timed_out");
    check(result.elapsed_ms < 4000, "没有被 30s 的 sleep 拖住（" +
                                        std::to_string(result.elapsed_ms) +
                                        " ms）");
    // 输出的第一行是那个 sleep 的 pid；进程组被杀后它应当不存在了。
    const size_t nl = out.find('\n');
    const bool   has_pid = nl != std::string::npos && nl > 0;
    check(has_pid, "拿到子进程 pid");
    if (has_pid) {
      const pid_t child = static_cast<pid_t>(std::strtol(out.c_str(), nullptr, 10));
      bool gone = false;
      for (int i = 0; i < 50 && !gone; ++i) {
        if (::kill(child, 0) != 0 && errno == ESRCH) gone = true;
        else { struct timespec ts = {0, 20 * 1000 * 1000}; nanosleep(&ts, nullptr); }
      }
      check(gone, "子进程（sleep）随进程组一起被杀");
    }
    check(sup.stats().timed_out == 1, "stats.timed_out = 1");
  }
}

void test_cancel() {
  std::cout << "-- 取消 --" << std::endl;
  uvcpp_loop loop;
  bool       done = false;
  int        exit_calls = 0;
  uvcpp_process_result result;
  {
    uvcpp_process_supervisor sup(&loop);
    uvcpp_process_spec       spec;
    spec.file = "/bin/sh";
    spec.args = std::vector<std::string>{"-c", "sleep 30"};
    spec.stop_grace_ms = 200;
    const uint64_t id = sup.spawn(
        spec, nullptr,
        [&](const uvcpp_process_result& r) { ++exit_calls; result = r; done = true; });
    check(id != 0, "spawn 返回 id");
    if (id == 0) return;
    check(sup.cancel(id) == 0, "cancel 成功");
    check(sup.cancel(id) == 0, "重复 cancel 是幂等成功（进程仍在收尾）");
    check(run_until(loop, done, 5000), "取消后收尾");
    check(result.cancelled, "结果标记 cancelled");
    check(exit_calls == 1, "退出回调恰好一次");
    check(sup.stats().cancelled == 1, "stats.cancelled = 1");
  }
}

void test_bounded_output() {
  std::cout << "-- 有界输出 --" << std::endl;
  uvcpp_loop loop;
  bool       done = false;
  uvcpp_process_result result;
  size_t total = 0;
  {
    uvcpp_process_supervisor sup(&loop);
    uvcpp_process_spec       spec;
    spec.file             = "/bin/sh";
    spec.args             = std::vector<std::string>{"-c", "yes 0123456789 | head -c 200000"};
    spec.max_output_bytes = 1024;
    spec.timeout_ms       = 5000;
    const uint64_t id = sup.spawn(
        spec, [&](const char*, size_t n, bool) { total += n; },
        [&](const uvcpp_process_result& r) { result = r; done = true; });
    check(id != 0, "spawn 返回 id");
    if (id == 0) return;
    check(run_until(loop, done, 8000), "大输出收尾");
    check(total > 100000, "回调确实收到了大流量（" +
                              std::to_string(total) + " 字节）");
    check(result.stdout_tail.size() <= 1024,
          "尾缓冲不超过上限（" + std::to_string(result.stdout_tail.size()) +
              "）");
    check(result.stdout_truncated > 0, "记录了截断字节数（" +
                                           std::to_string(result.stdout_truncated) +
                                           "）");
    check(sup.stats().succeeded == 1, "零退出计入 stats.succeeded");
  }
}

}  // namespace

int main() {
  std::cout << "[process_supervisor] start" << std::endl;
  test_basic();
  test_timeout_and_tree();
  test_cancel();
  test_bounded_output();
  if (g_fail == 0) {
    std::cout << "[process_supervisor] ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << "[process_supervisor] " << g_fail << " check(s) failed"
            << std::endl;
  return 1;
}

#endif  // _WIN32
