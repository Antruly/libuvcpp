/**
 * @file src/handle/uvcpp_process_supervisor.h
 * @brief 进程监管：有界输出、超时、进程组终止、取消与统计（issue #69）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * uvcpp::uvcpp_process_supervisor sup(&loop);
 * uvcpp::uvcpp_process_spec spec;
 * spec.file = "/usr/bin/ffmpeg";
 * spec.args = {"-i", in, out};
 * spec.timeout_ms = 10 * 60 * 1000;
 * spec.kill_tree = true;
 * sup.spawn(spec, on_output, on_exit);
 * ```
 *
 * ## 范围
 *
 * 跨平台（libuv `uv_spawn`）：
 *
 * - 启动 / 取消 / 超时 / 退出结果 / 有界输出 / 输出回调：全平台；
 * - **进程树终止**：POSIX 上 `UV_PROCESS_DETACHED`（子进程自成会话/进程组）+
 *   对 `-pid` 发信号；Windows 上给子进程建 **Job Object**
 *   （`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`），超时/取消/主进程退出时整棵树一起
 *   收掉。**Windows 的分配发生在 `uv_spawn` 返回之后的一瞬** —— 子进程在这条
 *   缝里再派生的后代不在 Job 里（libuv 不暴露 `CREATE_SUSPENDED`）；机器不允许
 *   嵌套 Job 时会退回"只杀主进程"，不会拒绝 spawn。
 * - `kill_tree=false` 时只对进程本身发信号（两平台一致）。
 * - 不绑定任何具体程序、不维护业务状态机；重启策略/任务队列留给上层
 *   （#67 作业队列）。
 *
 * ## 输出
 *
 * - `capture_stdout/stderr` 打开对应管道；数据同时交给 `output_cb()` 与
 *   **有界尾缓冲**（`max_output_bytes`，默认 64 KiB/流）。超限时丢最旧的字节并
 *   在结果里累计 `stdout_truncated` / `stderr_truncated` —— 不会无限吃内存。
 * - 退出回调在**进程退出且两条管道都读到 EOF**之后触发一次；结果里带退出码、
 *   信号、耗时、错误、两条流的尾缓冲。
 * - 回调都在 loop 线程上；`output_cb` 里的指针只在回调期间有效。
 *
 * ## 终止语义
 *
 * - 超时或 `cancel()`：POSIX 对进程组发 `SIGTERM`，等 `stop_grace_ms`
 *   （默认 2000）再 `SIGKILL`；`kill_tree=false` 时只对进程本身发信号。
 * - 退出回调**恰好一次**：取消之后再超时、进程又自己退出，都不会重复。
 * - 结果里的 `cancelled/timed_out` 说明这次是谁先动手。
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_PROCESS_SUPERVISOR_H
#define SRC_HANDLE_UVCPP_PROCESS_SUPERVISOR_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace uvcpp {

class uvcpp_loop;

/// 启动一个进程的完整描述。显式构造函数（C++11 聚合初始化资格）。
struct UVCPP_API uvcpp_process_spec {
  uvcpp_process_spec();

  std::string              file;   ///< 可执行文件（绝对路径或 PATH 里找）
  std::vector<std::string> args;   ///< argv[1..]；argv[0] 固定为 file
  std::vector<std::string> env;    ///< "K=V"；空 = 继承父进程环境
  std::string              cwd;    ///< 空 = 继承

  uint64_t timeout_ms;       ///< 0 = 不设超时
  uint64_t stop_grace_ms;    ///< SIGTERM 之后等多久再 SIGKILL
  size_t   max_output_bytes; ///< **每条流**保留的尾字节上限
  bool     capture_stdout;
  bool     capture_stderr;
  bool     kill_tree;  ///< 原样下发给整棵进程树（POSIX 进程组 / Windows Job Object）
};

/// 一次监管的最终结果。
struct UVCPP_API uvcpp_process_result {
  uvcpp_process_result();

  int64_t  exit_status;   ///< 退出码（被信号杀死时一般为 0）
  int      term_signal;   ///< 终止信号（0 = 正常退出）
  int      error;         ///< 启动/监管错误；0 = 无
  bool     timed_out;     ///< 超时先动手
  bool     cancelled;     ///< `cancel()` 先动手
  uint64_t elapsed_ms;    ///< 从 spawn 到完全收尾
  size_t   stdout_truncated; ///< 因上限丢掉的字节数
  size_t   stderr_truncated;
  std::string stdout_tail;  ///< 最多 max_output_bytes 的尾部
  std::string stderr_tail;
};

/// 输出回调：字节只在回调期间有效；`is_stderr` 区分两条流。
using uvcpp_process_output_cb =
    std::function<void(const char* data, size_t len, bool is_stderr)>;
using uvcpp_process_exit_cb =
    std::function<void(const uvcpp_process_result& result)>;

struct UVCPP_API uvcpp_process_supervisor_stats {
  uint64_t spawned = 0;
  uint64_t succeeded = 0;
  uint64_t failed = 0;
  uint64_t timed_out = 0;
  uint64_t cancelled = 0;
  size_t   running = 0;
};

/**
 * @brief 监管器。一个对象可以同时跑多个进程；`id` 用来取消。
 *
 * 生命周期：句柄都挂在构造时给的循环上；析构时会取消所有在跑的进程并等
 * 它们收尾（同步取消后不阻塞循环，但句柄在循环下一次迭代才真正关闭）。
 * 请在 `loop.run()` 之后、循环仍在的时候析构（与其它句柄一致）。
 */
class UVCPP_API uvcpp_process_supervisor {
 public:
  explicit uvcpp_process_supervisor(uvcpp_loop* loop);
  ~uvcpp_process_supervisor();

  uvcpp_process_supervisor(const uvcpp_process_supervisor&) = delete;
  uvcpp_process_supervisor& operator=(const uvcpp_process_supervisor&) = delete;

  /**
   * @brief 启动。成功返回非 0 id；失败返回 0 并在 @p err 里写原因。
   */
  uint64_t spawn(const uvcpp_process_spec& spec,
                 uvcpp_process_output_cb    on_output = nullptr,
                 uvcpp_process_exit_cb      on_exit   = nullptr,
                 std::string*               err       = nullptr);

  /// 取消一个在跑的进程（SIGTERM → 宽限 → SIGKILL）。不存在/已结束返回 -1。
  int cancel(uint64_t id);

  size_t running() const;
  uvcpp_process_supervisor_stats stats() const;

 private:
  struct job;
  struct impl;
  impl* impl_;
};

}  // namespace uvcpp

#endif  // SRC_HANDLE_UVCPP_PROCESS_SUPERVISOR_H
