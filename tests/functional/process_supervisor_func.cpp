/**
 * @file tests/functional/process_supervisor_func.cpp
 * @brief 进程监管：输出、退出码、超时、进程树终止、取消、有界缓冲。
 *
 * 跨平台，但"进程树"那一条两平台各用各的机制，判据也不同：
 *
 * - **POSIX**：`kill_tree` 让子进程自成进程组（`UV_PROCESS_DETACHED`），超时/取消
 *   时对 `-pid` 发信号。判据是"孙进程（`sleep`）真的没了"（`kill(pid, 0)` 报
 *   `ESRCH`）。
 * - **Windows**：`kill_tree` 给子进程建 Job Object
 *   （`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`），超时/取消时整棵树一起收。判据是
 *   "孙进程本该写的标记文件**没有**出现"—— 孙进程是 PowerShell 里
 *   `Start-Process` 起来的另一个 PowerShell，4 秒后写文件；监管在 0.5 秒时
 *   终止整棵树，文件就永远不该落地。
 *
 * 其余三组（输出/退出码、取消、有界输出）两平台同一份脚本形状，只是 shell 不同
 * （`/bin/sh -c` 与 `cmd.exe /c` 或直接 PowerShell）。Windows 上刻意用
 * PowerShell 而不是 `cmd /c`：cmd 的嵌套引号规则会把脚本拼坏，而 PowerShell 的
 * `-ArgumentList` 是数组，不需要转义。
 */

#include <chrono>
#include <cstdio>
#include <thread>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#endif

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

std::string self_exe() {
  char buf[4096];
  std::size_t n = sizeof(buf);
  if (uv_exepath(buf, &n) != 0) return std::string();
  return std::string(buf, n);
}

std::string temp_file(const std::string& leaf) {
  char dir[1024];
  std::size_t n = sizeof(dir);
  std::string base = ".";
  if (uv_os_tmpdir(dir, &n) == 0) base.assign(dir, n);
  char pid[32];
  std::snprintf(pid, sizeof(pid), "%ld", static_cast<long>(uv_os_getpid()));
  return base + "/uvcpp_restart_" + pid + "_" + leaf;
}

/// 一条 `/bin/sh -c` 脚本的 spec（POSIX）。
#ifndef _WIN32
uvcpp_process_spec make_sh(const std::string& script) {
  uvcpp_process_spec s;
  s.file = "/bin/sh";
  s.args = std::vector<std::string>{"-c", script};
  return s;
}
#else
uvcpp_process_spec make_powershell(const std::string& script) {
  uvcpp_process_spec s;
  s.file = "powershell.exe";
  s.args = std::vector<std::string>{"-NoProfile", "-NonInteractive", "-Command",
                                    script};
  return s;
}

std::string temp_dir() {
  char  buf[MAX_PATH + 1];
  std::memset(buf, 0, sizeof(buf));
  const DWORD n = ::GetTempPathA(MAX_PATH, buf);
  if (n == 0 || n > MAX_PATH) return std::string();
  return std::string(buf, n);
}

std::string base64(const std::string& in) {
  static const char* kTab =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  while (i + 2 < in.size()) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                       (static_cast<unsigned char>(in[i + 1]) << 8) |
                       static_cast<unsigned char>(in[i + 2]);
    out += kTab[(v >> 18) & 0x3F];
    out += kTab[(v >> 12) & 0x3F];
    out += kTab[(v >> 6) & 0x3F];
    out += kTab[v & 0x3F];
    i += 3;
  }
  if (i + 1 == in.size()) {
    const unsigned v = static_cast<unsigned char>(in[i]) << 16;
    out += kTab[(v >> 18) & 0x3F];
    out += kTab[(v >> 12) & 0x3F];
    out += "==";
  } else if (i + 2 == in.size()) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                       (static_cast<unsigned char>(in[i + 1]) << 8);
    out += kTab[(v >> 18) & 0x3F];
    out += kTab[(v >> 12) & 0x3F];
    out += kTab[(v >> 6) & 0x3F];
    out += '=';
  }
  return out;
}

/// 本可执行文件的完整路径（树测试用它把自己拉起当子进程）。
std::string self_path() {
  char  buf[MAX_PATH + 1];
  std::memset(buf, 0, sizeof(buf));
  const DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n > MAX_PATH) return std::string();
  return std::string(buf, n);
}

/// PowerShell `-EncodedCommand` 的载荷：UTF-16LE 再 base64。
/// 用它而不是把脚本塞进 `-Command`，是为了绕开嵌套引号：脚本里本来就有
/// 单引号路径，再经 cmd/PowerShell 两层解析很容易拼坏。
std::string ps_encoded(const std::string& script) {
  std::string utf16;
  utf16.reserve(script.size() * 2);
  for (size_t i = 0; i < script.size(); ++i) {
    utf16 += script[i];
    utf16 += '\0';
  }
  return base64(utf16);
}
#endif

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
#ifdef _WIN32
    uvcpp_process_spec spec = make_powershell(
        "[Console]::Out.Write('OUT'); [Console]::Error.Write('ERR'); exit 3");
#else
    uvcpp_process_spec spec = make_sh("printf OUT; printf ERR >&2; exit 3");
#endif
    spec.timeout_ms = 8000;
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
    check(run_until(loop, done, 8000), "进程收尾");
    check(result.exit_status == 3,
          "退出码 3（实际 " + std::to_string(result.exit_status) + "）");
    check(result.term_signal == 0, "不是被信号杀死");
    check(result.stdout_tail.find("OUT") != std::string::npos,
          "stdout 尾缓冲含 OUT（实际 '" + result.stdout_tail + "'）");
    check(result.stderr_tail.find("ERR") != std::string::npos,
          "stderr 尾缓冲含 ERR");
    check(result.stdout_truncated == 0 && result.stderr_truncated == 0,
          "未截断");
    check(chunks >= 1, "output 回调被调用");
    check(sup.running() == 0, "running() 归零");
    check(sup.stats().failed == 1, "非零退出计入 stats.failed");
  }
}

void test_timeout_and_tree() {
  std::cout << "-- 超时 / 进程树终止 --" << std::endl;
  uvcpp_loop loop;
  bool       done = false;
  uvcpp_process_result result;
  std::string out;
  {
    uvcpp_process_supervisor sup(&loop);
    uvcpp_process_spec       spec;
#ifdef _WIN32
    const std::string marker =
        temp_dir() + "uvcpp_tree_" +
        std::to_string(static_cast<unsigned long long>(::GetCurrentProcessId())) +
        "_" +
        std::to_string(static_cast<unsigned long long>(::GetTickCount64())) +
        ".txt";
    ::DeleteFileA(marker.c_str());
    // **用测试程序自己当父/孙进程**，不走 PowerShell：PowerShell 在
    // arm64 runner 上启动慢且 stdout 缓冲不可控，会让"父进程打印 SPAWNED"
    // 这一格在 4 秒内收不到（v1.10.0 的发布 workflow 就是这么红的）。
    // 自派生的路径没有 shell 引号、没有外部 shell 启动开销。
    //
    // 父模式（`--tree-parent <marker>`）：再拉起一个自己（`--write-marker`），
    // 打印 SPAWNED，然后睡 30 秒。孙模式：睡 4 秒后写标记文件。
    spec.file = self_path();
    spec.args = std::vector<std::string>{"--tree-parent", marker};
#else
    spec = make_sh("sleep 30 & echo $!; wait");
#endif
#ifndef _WIN32
    spec.timeout_ms    = 500;
#endif
    spec.stop_grace_ms = 300;
    spec.kill_tree     = true;
    const uint64_t id = sup.spawn(
        spec, [&](const char* d, size_t n, bool) { out.append(d, n); },
        [&](const uvcpp_process_result& r) { result = r; done = true; });
    check(id != 0, "spawn 返回 id（kill_tree 被接受）");
    if (id == 0) {
#ifdef _WIN32
      ::DeleteFileA(marker.c_str());
#endif
      return;
    }
#ifdef _WIN32
    // 等父进程把孙进程拉起来（打印 SPAWNED），再取消。
    bool spawned_seen = false;
    for (int i = 0; i < 400 && !spawned_seen && !done; ++i) {
      loop.run(UV_RUN_NOWAIT);
      spawned_seen = out.find("SPAWNED") != std::string::npos;
      if (!spawned_seen) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(spawned_seen, "孙进程已经创建（父进程打印了 SPAWNED）");
    check(sup.cancel(id) == 0, "cancel 成功");
    check(run_until(loop, done, 8000), "取消后收尾");
    check(result.cancelled, "结果标记 cancelled");
    check(result.elapsed_ms < 6000,
          "没有被 30s 的睡眠拖住（" + std::to_string(result.elapsed_ms) +
              " ms）");
    // 等过孙进程原本要写标记的时刻（4s），看它有没有活下来。
    std::this_thread::sleep_for(std::chrono::seconds(6));
    const bool leaked = ::GetFileAttributesA(marker.c_str()) !=
                        INVALID_FILE_ATTRIBUTES;
    check(!leaked, "孙进程随 Job Object 一起被杀（标记文件没有出现）");
    ::DeleteFileA(marker.c_str());
    check(sup.stats().cancelled == 1, "stats.cancelled = 1");
#else
    check(run_until(loop, done, 8000), "超时后收尾");
    check(result.timed_out, "结果标记 timed_out");
    check(result.elapsed_ms < 6000,
          "没有被 30s 的睡眠拖住（" + std::to_string(result.elapsed_ms) +
              " ms）");
    const size_t nl     = out.find('\n');
    const bool   has_pid = nl != std::string::npos && nl > 0;
    check(has_pid, "拿到子进程 pid");
    if (has_pid) {
      const pid_t child = static_cast<pid_t>(std::strtol(out.c_str(), nullptr, 10));
      bool gone = false;
      for (int i = 0; i < 50 && !gone; ++i) {
        if (::kill(child, 0) != 0 && errno == ESRCH) {
          gone = true;
        } else {
          struct timespec ts = {0, 20 * 1000 * 1000};
          nanosleep(&ts, nullptr);
        }
      }
      check(gone, "子进程（sleep）随进程组一起被杀");
    }
#endif
#ifndef _WIN32
    check(sup.stats().timed_out == 1, "stats.timed_out = 1");
#endif
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
#ifdef _WIN32
    uvcpp_process_spec spec =
        make_powershell("Start-Sleep -Seconds 30");
#else
    uvcpp_process_spec spec = make_sh("sleep 30");
#endif
    spec.stop_grace_ms = 300;
    const uint64_t id = sup.spawn(
        spec, nullptr,
        [&](const uvcpp_process_result& r) {
          ++exit_calls;
          result = r;
          done   = true;
        });
    check(id != 0, "spawn 返回 id");
    if (id == 0) return;
    check(sup.cancel(id) == 0, "cancel 成功");
    check(sup.cancel(id) == 0, "重复 cancel 是幂等成功（进程仍在收尾）");
    check(run_until(loop, done, 8000), "取消后收尾");
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
#ifdef _WIN32
    uvcpp_process_spec spec = make_powershell(
        "[Console]::Out.Write(('0123456789' * 20000))");
#else
    uvcpp_process_spec spec =
        make_sh("yes 0123456789 | head -c 200000");
#endif
    spec.max_output_bytes = 1024;
    spec.timeout_ms       = 15000;
    const uint64_t id = sup.spawn(
        spec, [&](const char*, size_t n, bool) { total += n; },
        [&](const uvcpp_process_result& r) { result = r; done = true; });
    check(id != 0, "spawn 返回 id");
    if (id == 0) return;
    check(run_until(loop, done, 20000), "大输出收尾");
    check(total > 100000,
          "回调确实收到了大流量（" + std::to_string(total) + " 字节）");
    check(result.stdout_tail.size() <= 1024,
          "尾缓冲不超过上限（" + std::to_string(result.stdout_tail.size()) +
              "）");
    check(result.stdout_truncated > 0,
          "记录了截断字节数（" + std::to_string(result.stdout_truncated) + "）");
    check(sup.stats().succeeded == 1, "零退出计入 stats.succeeded");
  }
}

}  // namespace


void test_restart_policy() {
  std::cout << "-- 重启策略 --" << std::endl;

  // (1) 第一次以 7 退出，第二次看到标记文件后以 0 退出。
  {
    uvcpp_loop loop;
    bool done = false;
    bool seen_restart = false;
    int  exit_calls = 0;
    int  restart_calls = 0;
    uint64_t last_delay = 0;
    uvcpp_process_result result;
    const std::string marker = temp_file("once");
    std::remove(marker.c_str());
    {
      uvcpp_process_supervisor sup(&loop);
      uvcpp_process_spec spec;
      spec.file = self_exe();
      spec.args.push_back("--exit-once");
      spec.args.push_back(marker);
      spec.timeout_ms           = 8000;
      spec.restart_max          = 2;
      spec.restart_backoff_ms   = 40;
      spec.restart_backoff_max_ms = 200;

      const uint64_t id = sup.spawn(
          spec,
          [](const char*, size_t, bool) {},
          [&](const uvcpp_process_result& r) {
            result = r;
            done   = true;
            ++exit_calls;
          },
          nullptr,
          [&](const uvcpp_process_result&, int, uint64_t delay) {
            seen_restart = true;
            ++restart_calls;
            last_delay = delay;
          });
      check(id != 0, "重启链 spawn 返回 id");
      if (id != 0) {
        check(run_until(loop, done, 12000), "链在失败一次后收尾");
        check(result.exit_status == 0, "最终退出码 0（第二次成功）");
        check(result.restarts == 1,
              "最终结果 restarts=1（实际 " +
                  std::to_string(result.restarts) + "）");
        check(restart_calls == 1 && last_delay >= 1,
              "on_restart 恰好一次并带退避");
        check(exit_calls == 1, "on_exit 恰好一次");
        const uvcpp_process_supervisor_stats st = sup.stats();
        check(st.restarted == 1, "stats.restarted=1");
        check(st.spawned == 2, "stats.spawned=2（两次尝试）");
        check(sup.running() == 0, "running() 归零");
      }
    }
    std::remove(marker.c_str());
  }

  // (2) 一直失败：重启次数用完就交最终结果。
  {
    uvcpp_loop loop;
    bool done = false;
    int  exit_calls = 0;
    int  restart_calls = 0;
    uvcpp_process_result result;
    {
      uvcpp_process_supervisor sup(&loop);
      uvcpp_process_spec spec;
      spec.file = self_exe();
      spec.args.push_back("--exit-code");
      spec.args.push_back("3");
      spec.timeout_ms             = 8000;
      spec.restart_max            = 2;
      spec.restart_backoff_ms     = 20;
      spec.restart_backoff_max_ms = 40;
      const uint64_t id = sup.spawn(
          spec, nullptr,
          [&](const uvcpp_process_result& r) {
            result = r;
            done   = true;
            ++exit_calls;
          },
          nullptr,
          [&](const uvcpp_process_result&, int, uint64_t) { ++restart_calls; });
      check(id != 0, "一直失败的链 spawn 返回 id");
      if (id != 0) {
        check(run_until(loop, done, 8000), "重启次数用完后收尾");
        check(result.exit_status == 3, "最终退出码 3");
        check(result.restarts == 2, "最终结果 restarts=2");
        check(restart_calls == 2, "on_restart 两次");
        check(exit_calls == 1, "on_exit 仍恰好一次");
        const uvcpp_process_supervisor_stats st = sup.stats();
        check(st.restarted == 2, "stats.restarted=2");
        check(st.failed == 1, "只把最终结果计入 failed");
      }
    }
  }

  // (3) 退避等待中取消：没有进程可杀，仍要交恰好一次 cancelled。
  {
    uvcpp_loop loop;
    bool restarted_once = false;
    bool done = false;
    int  exit_calls = 0;
    uvcpp_process_result result;
    {
      uvcpp_process_supervisor sup(&loop);
      uvcpp_process_spec spec;
      spec.file = self_exe();
      spec.args.push_back("--exit-code");
      spec.args.push_back("4");
      spec.timeout_ms         = 8000;
      spec.restart_max        = 5;
      spec.restart_backoff_ms = 2000;  // 留足取消窗口
      const uint64_t id = sup.spawn(
          spec, nullptr,
          [&](const uvcpp_process_result& r) {
            result = r;
            done   = true;
            ++exit_calls;
          },
          nullptr,
          [&](const uvcpp_process_result&, int, uint64_t) {
            restarted_once = true;
          });
      check(id != 0, "退避取消组 spawn 返回 id");
      if (id != 0) {
        check(run_until(loop, restarted_once, 5000),
              "第一次失败进入退避等待");
        check(sup.cancel(id) == 0, "退避等待中 cancel 成功");
        check(run_until(loop, done, 5000), "取消后链立即收尾");
        check(result.cancelled, "最终结果 cancelled");
        check(result.restarts == 1, "已重启 1 次后取消");
        check(exit_calls == 1, "on_exit 恰好一次");
        check(sup.stats().cancelled == 1, "stats.cancelled=1");
        check(sup.running() == 0, "running() 归零");
      }
    }
  }

  // (4) 超时不是"非正常退出"，不重启。
  {
    uvcpp_loop loop;
    bool done = false;
    int  restart_calls = 0;
    uvcpp_process_result result;
    {
      uvcpp_process_supervisor sup(&loop);
      uvcpp_process_spec spec;
      spec.file = self_exe();
      spec.args.push_back("--sleep");
      spec.args.push_back("3");
      spec.timeout_ms     = 150;
      spec.stop_grace_ms  = 1000;
      spec.restart_max    = 3;
      spec.restart_backoff_ms = 20;
      const uint64_t id = sup.spawn(
          spec, nullptr,
          [&](const uvcpp_process_result& r) {
            result = r;
            done   = true;
          },
          nullptr,
          [&](const uvcpp_process_result&, int, uint64_t) { ++restart_calls; });
      check(id != 0, "超时组 spawn 返回 id");
      if (id != 0) {
        check(run_until(loop, done, 10000), "超时后收尾");
        check(result.timed_out, "结果标记 timed_out");
        check(result.restarts == 0, "超时不重启");
        check(restart_calls == 0, "on_restart 一次都没被调");
        check(sup.stats().restarted == 0, "stats.restarted=0");
      }
    }
  }
}

int main(int argc, char** argv) {
  // —— 给"重启策略"那一组用的子模式（跨平台）——
  if (argc >= 3 && std::strcmp(argv[1], "--exit-once") == 0) {
    FILE* f = std::fopen(argv[2], "rb");
    if (f != nullptr) {
      std::fclose(f);
      return 0;  // 第二次：成功
    }
    f = std::fopen(argv[2], "wb");
    if (f != nullptr) {
      std::fputs("x", f);
      std::fclose(f);
    }
    return 7;  // 第一次：失败
  }
  if (argc >= 3 && std::strcmp(argv[1], "--exit-code") == 0) {
    return std::atoi(argv[2]);
  }
  if (argc >= 3 && std::strcmp(argv[1], "--sleep") == 0) {
    std::this_thread::sleep_for(
        std::chrono::seconds(std::atoi(argv[2])));
    return 0;
  }

#ifdef _WIN32
  // —— 只给"进程树"那一格用的两个子模式（见 test_timeout_and_tree）——
  if (argc >= 3 && std::strcmp(argv[1], "--tree-parent") == 0) {
    const std::string marker = argv[2];
    // 让监管器先把我们分配进 Job 再派生孙进程（分配发生在 `uv_spawn` 返回
    // 之后的一瞬；这条 200ms 把那个窗口让开，孙进程因此必然在 Job 里）。
    ::Sleep(200);
    const std::string self   = self_path();
    std::string cmdline = "\"" + self + "\" --write-marker 4 \"" + marker +
                          "\"";
    std::vector<char> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back('\0');
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    std::memset(&si, 0, sizeof(si));
    std::memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    const BOOL ok = ::CreateProcessA(nullptr, cmd.data(), nullptr, nullptr,
                                     TRUE, 0, nullptr, nullptr, &si, &pi);
    if (ok) {
      ::CloseHandle(pi.hProcess);
      ::CloseHandle(pi.hThread);
    }
    std::printf("SPAWNED");
    std::fflush(stdout);
    ::Sleep(30000);
    return 0;
  }
  if (argc >= 4 && std::strcmp(argv[1], "--write-marker") == 0) {
    ::Sleep(static_cast<DWORD>(std::strtoul(argv[2], nullptr, 10)) * 1000);
    FILE* f = std::fopen(argv[3], "wb");
    if (f != nullptr) {
      std::fputs("x", f);
      std::fclose(f);
    }
    return 0;
  }
#endif

  std::cout << "[process_supervisor] start" << std::endl;
  test_basic();
  test_timeout_and_tree();
  test_cancel();
  test_bounded_output();
  test_restart_policy();
  if (g_fail == 0) {
    std::cout << "[process_supervisor] ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << "[process_supervisor] " << g_fail << " check(s) failed"
            << std::endl;
  return 1;
}
