/**
 * @file src/handle/uvcpp_process_supervisor.cpp
 * @brief 进程监管的实现（POSIX 进程组 + 有界输出 + 超时）。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "handle/uvcpp_process_supervisor.h"

#include <handle/uvcpp_loop.h>

#include <uv.h>

#include <chrono>
#include <cstring>
#include <map>

#ifdef _WIN32
// Windows 头会定义 min/max 与 `small` 宏，先挡住前两个（`small` 在本文件里
// 不用，但同一棵树上别的 TU 吃过它的亏）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <sys/types.h>
#endif

namespace uvcpp {

uvcpp_process_spec::uvcpp_process_spec()
    : timeout_ms(0),
      stop_grace_ms(2000),
      max_output_bytes(64 * 1024),
      capture_stdout(true),
      capture_stderr(true),
      kill_tree(true),
      restart_max(0),
      restart_backoff_ms(1000),
      restart_backoff_max_ms(60000) {}

uvcpp_process_result::uvcpp_process_result()
    : exit_status(0),
      term_signal(0),
      error(0),
      timed_out(false),
      cancelled(false),
      elapsed_ms(0),
      stdout_truncated(0),
      stderr_truncated(0),
      restarts(0) {}

namespace {

#ifdef _WIN32
// MSVC 的 <csignal> 没有 SIGKILL；Windows 上强杀也只走 uv_process_kill(SIGTERM)。
const int kForceSignal = SIGTERM;
#else
const int kForceSignal = SIGKILL;
#endif

void bounded_append(std::string& buf, const char* data, size_t len,
                    size_t cap, size_t& truncated) {
  if (cap == 0) {
    truncated += len;
    return;
  }
  buf.append(data, len);
  if (buf.size() > cap) {
    const size_t drop = buf.size() - cap;
    buf.erase(0, drop);
    truncated += drop;
  }
}

}  // namespace

struct uvcpp_process_supervisor::job {
  impl*                     owner = nullptr;
  chain*                    c = nullptr;  ///< 有重启策略时指向所属的重启链
  uint64_t                  id = 0;
  uvcpp_process_spec        spec;
  uvcpp_process_output_cb   on_output;
  uvcpp_process_exit_cb     on_exit;
  uvcpp_process_result      result;

  uv_process_t proc;
  uv_pipe_t    out_pipe;
  uv_pipe_t    err_pipe;
  uv_timer_t   timeout_timer;
  uv_timer_t   kill_timer;

  bool proc_started = false;
  bool proc_exited  = false;
  bool proc_closed  = false;
  bool out_eof      = false;
  bool err_eof      = false;
  bool out_closed   = false;
  bool err_closed   = false;
  bool time_closed  = false;
  bool kill_closed  = false;
  bool term_sent    = false;
  bool exit_fired   = false;
  bool finalized    = false;

  int pending_closes = 0;

#ifdef _WIN32
  /// 进程所属的 Job Object（`kill_tree` 且分配成功时非空）。关掉它时
  /// `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 会把整棵树一起收掉。
  HANDLE win_job = nullptr;
#endif

  std::vector<std::string> arg_store;
  std::vector<std::string> env_store;
  std::vector<char*>       argv;
  std::vector<char*>       envp;

  std::chrono::steady_clock::time_point started_at;
};

/**
 * @brief 一条"会重启"的监管链。id 跨尝试保持不变。
 *
 * 引用计数三份：`chains` 表一份、当前尝试的 job 一份（在 `note_closed` 里放）、
 * 退避定时器一份（在它的关闭完成回调里放）。都为 0 时释放 —— 这样析构、取消、
 * 重启三条路都不需要猜"还有谁在碰它"。
 */
struct uvcpp_process_supervisor::chain {
  impl*                    owner = nullptr;
  uint64_t                 id = 0;
  uvcpp_process_spec       spec;
  uvcpp_process_output_cb  on_output;
  uvcpp_process_exit_cb    on_exit;
  uvcpp_process_restart_cb on_restart;

  int  restarts_done = 0;
  int  refs = 1;  // 表里那一份
  bool finished = false;
  bool cancelled = false;
  bool timer_init = false;
  bool timer_open = false;  // init 过且还没 `uv_close`

  uv_timer_t            timer;
  job*                  current = nullptr;
  uvcpp_process_result  last_result;

  static void release(chain* c) {
    if (c == nullptr) return;
    if (--c->refs <= 0) delete c;
  }
};

struct uvcpp_process_supervisor::impl {
  explicit impl(uvcpp_loop* l) : loop(l) {}

  uvcpp_loop*                       loop;
  std::map<uint64_t, job*>          jobs;
  std::map<uint64_t, chain*>        chains;
  uint64_t                          next_id = 1;
  uvcpp_process_supervisor_stats    s;

  static void on_exit(uv_process_t* p, int64_t exit_status, int term_signal) {
    job* j = static_cast<job*>(p->data);
    if (j == nullptr || j->proc_exited) return;
    j->proc_exited      = true;
    j->result.exit_status = exit_status;
    j->result.term_signal = term_signal;
    if (j->owner != nullptr) {
      uv_timer_stop(&j->timeout_timer);
      uv_timer_stop(&j->kill_timer);
    }
#ifdef _WIN32
    // 主进程退出 = 这次监管的终点：`kill_tree` 的语义是"树跟着走"，后代的
    // 寿命不该超过它。`TerminateJobObject` 对已经死掉的主进程是空操作。
    if (j->win_job != nullptr) {
      TerminateJobObject(j->win_job, 1);
    }
#endif
    if (!j->proc_closed) {
      j->proc_closed = true;
      uv_close(reinterpret_cast<uv_handle_t*>(&j->proc), on_close_proc);
    }
    if (j->owner != nullptr) j->owner->maybe_finish(j);
  }

  static void on_close_proc(uv_handle_t* h) {
    job* j = static_cast<job*>(h->data);
    if (j == nullptr) return;
    j->proc_closed = true;
    note_closed(j);
  }
  /// 非进程句柄（管道 / 定时器）的关闭：只记账，**不要**碰 proc_closed
  /// —— 把管道关闭误标成"进程句柄已关"会让 on_exit 跳过 uv_close(proc)，
  /// 留下仍活跃的进程句柄，最后在 close 回调里访问已被释放的 job。
  static void on_close_any(uv_handle_t* h) {
    job* j = static_cast<job*>(h->data);
    if (j == nullptr) return;
    note_closed(j);
  }
  static void on_closed(uv_handle_t* h) { on_close_any(h); }

  static void on_read(uv_stream_t* stream, ssize_t nread,
                      const uv_buf_t* buf) {
    job* j = static_cast<job*>(stream->data);
    if (j == nullptr) {
      if (buf != nullptr && buf->base != nullptr) delete[] buf->base;
      return;
    }
    const bool is_err =
        stream == reinterpret_cast<uv_stream_t*>(&j->err_pipe);
    if (nread > 0) {
      if (j->on_output) j->on_output(buf->base, static_cast<size_t>(nread), is_err);
      size_t& trunc = is_err ? j->result.stderr_truncated
                             : j->result.stdout_truncated;
      std::string& tail =
          is_err ? j->result.stderr_tail : j->result.stdout_tail;
      bounded_append(tail, buf->base, static_cast<size_t>(nread),
                     j->spec.max_output_bytes, trunc);
    } else if (nread == UV_EOF || nread < 0) {
      if (is_err) {
        j->err_eof = true;
        uv_read_stop(stream);
        close_handle(reinterpret_cast<uv_handle_t*>(&j->err_pipe), j,
                     j->err_closed);
      } else {
        j->out_eof = true;
        uv_read_stop(stream);
        close_handle(reinterpret_cast<uv_handle_t*>(&j->out_pipe), j,
                     j->out_closed);
      }
      if (j->owner != nullptr) j->owner->maybe_finish(j);
    }
    if (buf != nullptr && buf->base != nullptr) delete[] buf->base;
  }

  static void alloc_cb(uv_handle_t*, size_t suggested, uv_buf_t* buf) {
    const size_t n = suggested == 0 ? 4096 : suggested;
    buf->base = new char[n];
    buf->len  = n;
  }

  static void on_timeout(uv_timer_t* t) {
    job* j = static_cast<job*>(t->data);
    if (j == nullptr || j->proc_exited) return;
    j->result.timed_out = true;
    if (j->owner != nullptr) j->owner->terminate(j);
  }

  static void on_kill(uv_timer_t* t) {
    job* j = static_cast<job*>(t->data);
    if (j == nullptr || j->proc_exited) return;
    if (j->owner != nullptr) j->owner->signal_tree(j, true);
  }

  static void note_closed(job* j) {
    if (j->pending_closes > 0) --j->pending_closes;
    if (j->pending_closes == 0) {
#ifdef _WIN32
      if (j->win_job != nullptr) {
        CloseHandle(j->win_job);
        j->win_job = nullptr;
      }
#endif
      chain* c = j->c;
      delete j;
      // job 那一份引用还掉；可能是最后一个引用（正常结束之后）。
      chain::release(c);
    }
  }

  static void close_handle(uv_handle_t* h, job* j, bool& flag) {
    if (flag) return;
    flag = true;
    uv_close(h, on_close_any);
  }

  void signal_tree(job* j, bool force) {
    if (!j->proc_started || j->proc_exited) return;
#ifdef _WIN32
    if (j->spec.kill_tree && j->win_job != nullptr) {
      if (force) {
        // 强杀阶段：整棵树一起收。主进程可能已经退出，这对它是空操作。
        TerminateJobObject(j->win_job, 1);
        return;
      }
      // Windows 没有 SIGTERM 的"礼貌"语义（uv_process_kill 就是
      // TerminateProcess）；先杀主进程，树留到强杀阶段/收尾时统一收。
      uv_process_kill(&j->proc, SIGTERM);
      return;
    }
    uv_process_kill(&j->proc, force ? kForceSignal : SIGTERM);
#else
    if (j->spec.kill_tree) {
      const int pgid = j->proc.pid;
      ::kill(-pgid, force ? SIGKILL : SIGTERM);
    } else {
      uv_process_kill(&j->proc, force ? kForceSignal : SIGTERM);
    }
#endif
  }

  void terminate(job* j) {
    if (j->term_sent || j->proc_exited) return;
    j->term_sent = true;
    signal_tree(j, false);
    uv_timer_start(&j->kill_timer, on_kill, j->spec.stop_grace_ms, 0);
  }

  void maybe_finish(job* j) {
    if (j->finalized || !j->proc_exited) return;
    const bool out_done = !j->spec.capture_stdout || j->out_eof;
    const bool err_done = !j->spec.capture_stderr || j->err_eof;
    if (!out_done || !err_done) return;
    j->finalized = true;
    j->result.elapsed_ms =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() -
                                  j->started_at)
                                  .count());

    uv_timer_stop(&j->timeout_timer);
    uv_timer_stop(&j->kill_timer);

    // 重启优先于"交最终结果"：这条 job 只是链上的一次尝试。
    if (j->c != nullptr && should_restart(j)) {
      begin_restart(j);
      return;
    }

    // 统计 + 回调（回调里新 spawn/cancel 是合法的：这里不持锁、不遍历表）。
    if (j->result.timed_out) {
      ++s.timed_out;
    } else if (j->result.cancelled) {
      ++s.cancelled;
    } else if (j->result.exit_status == 0 && j->result.term_signal == 0 &&
               j->result.error == 0) {
      ++s.succeeded;
    } else {
      ++s.failed;
    }
    if (j->id != 0) jobs.erase(j->id);

    if (j->c != nullptr) {
      chain* c = j->c;
      c->current = nullptr;
      j->result.restarts = c->restarts_done;  // 最终结果带上总重启次数
      // 链上的最终尝试：由这里交唯一一次 on_exit 并把链收掉。
      finish_chain(c, j->result, true);
    } else if (!j->exit_fired && j->on_exit) {
      j->exit_fired = true;
      uvcpp_process_exit_cb cb = j->on_exit;
      if (cb) cb(j->result);
    }

    // 收尾：关闭还没关的句柄。`proc` 在退出回调里已经 close 过。
    close_handle(reinterpret_cast<uv_handle_t*>(&j->timeout_timer), j,
                 j->time_closed);
    close_handle(reinterpret_cast<uv_handle_t*>(&j->kill_timer), j,
                 j->kill_closed);
    if (j->spec.capture_stdout) {
      close_handle(reinterpret_cast<uv_handle_t*>(&j->out_pipe), j,
                   j->out_closed);
    }
    if (j->spec.capture_stderr) {
      close_handle(reinterpret_cast<uv_handle_t*>(&j->err_pipe), j,
                   j->err_closed);
    }
  }

  void fail_spawn(job* j, int err) {
    j->result.error = err;
    j->result.elapsed_ms = 0;
    ++s.spawned;
    ++s.failed;
    if (j->on_exit) j->on_exit(j->result);
    if (j->c != nullptr) {
      // 启动失败不重试：直接把链收掉（on_exit 上面已经交过）。
      chain* c = j->c;
      c->current = nullptr;
      finish_chain(c, j->result, false);
    }
    // 关闭已经 init 的句柄
    close_handle(reinterpret_cast<uv_handle_t*>(&j->timeout_timer), j,
                 j->time_closed);
    close_handle(reinterpret_cast<uv_handle_t*>(&j->kill_timer), j,
                 j->kill_closed);
    if (j->spec.capture_stdout) {
      close_handle(reinterpret_cast<uv_handle_t*>(&j->out_pipe), j,
                   j->out_closed);
    }
    if (j->spec.capture_stderr) {
      close_handle(reinterpret_cast<uv_handle_t*>(&j->err_pipe), j,
                   j->err_closed);
    }
  }

  /// 真正的"建 job 并 `uv_spawn`"。`c == nullptr` 就是旧的一次性路径。
  job* launch(chain* c, const uvcpp_process_spec& spec,
              uvcpp_process_output_cb on_output,
              uvcpp_process_exit_cb on_exit, std::string* err, int& rc);

  // ---- 重启链 --------------------------------------------------------

  static void on_retry(uv_timer_t* t) {
    chain* c = static_cast<chain*>(t->data);
    if (c == nullptr || c->owner == nullptr) return;
    c->owner->retry_now(c);
  }

  static void on_retry_close(uv_handle_t* h) {
    chain* c = static_cast<chain*>(h->data);
    if (c == nullptr) return;
    c->timer_open = false;
    chain::release(c);
  }

  void close_chain_timer(chain* c) {
    if (c == nullptr || !c->timer_init || !c->timer_open) return;
    uv_timer_stop(&c->timer);
    c->timer_open = false;
    uv_close(reinterpret_cast<uv_handle_t*>(&c->timer), on_retry_close);
  }

  uint64_t retry_delay(const chain* c) const {
    uint64_t d =
        c->spec.restart_backoff_ms > 0 ? c->spec.restart_backoff_ms : 1;
    for (int i = 1; i < c->restarts_done; ++i) {
      if (d > 3600000ull) break;
      d *= 2;
    }
    if (c->spec.restart_backoff_max_ms > 0 &&
        d > c->spec.restart_backoff_max_ms) {
      d = c->spec.restart_backoff_max_ms;
    }
    return d;
  }

  bool should_restart(const job* j) const {
    const chain* c = j->c;
    if (c == nullptr || c->finished || c->cancelled) return false;
    if (c->restarts_done >= c->spec.restart_max) return false;
    if (j->result.timed_out || j->result.cancelled || j->result.error != 0) {
      return false;
    }
    return j->result.exit_status != 0 || j->result.term_signal != 0;
  }

  void schedule_retry(chain* c, uint64_t delay_ms) {
    if (c->owner == nullptr || c->finished || c->cancelled) return;
    if (!c->timer_init) {
      uv_loop_t* loop =
          reinterpret_cast<uv_loop_t*>(c->owner->loop->get_handle());
      uv_timer_init(loop, &c->timer);
      c->timer.data = c;
      c->timer_init = true;
      c->timer_open = true;
      ++c->refs;  // 定时器那一份
    }
    uv_timer_start(&c->timer, on_retry, delay_ms == 0 ? 1 : delay_ms, 0);
  }

  void retry_now(chain* c) {
    if (c->finished || c->cancelled || c->owner == nullptr) return;
    int         rc = 0;
    std::string err;
    job* j = launch(c, c->spec, c->on_output, c->on_exit, &err, rc);
    if (j != nullptr) c->current = j;
    // 启动失败不重试：`launch` 里的 `fail_spawn` 已经把链收掉了。
  }

  void begin_restart(job* j) {
    chain* c = j->c;
    ++c->restarts_done;
    ++s.restarted;
    j->result.restarts = c->restarts_done;
    c->last_result     = j->result;
    if (j->id != 0) jobs.erase(j->id);
    c->current = nullptr;

    const uint64_t delay = retry_delay(c);

    close_handle(reinterpret_cast<uv_handle_t*>(&j->timeout_timer), j,
                 j->time_closed);
    close_handle(reinterpret_cast<uv_handle_t*>(&j->kill_timer), j,
                 j->kill_closed);
    if (j->spec.capture_stdout) {
      close_handle(reinterpret_cast<uv_handle_t*>(&j->out_pipe), j,
                   j->out_closed);
    }
    if (j->spec.capture_stderr) {
      close_handle(reinterpret_cast<uv_handle_t*>(&j->err_pipe), j,
                   j->err_closed);
    }

    if (c->on_restart) {
      uvcpp_process_restart_cb cb = c->on_restart;
      cb(j->result, c->restarts_done, delay);
    }
    schedule_retry(c, delay);
  }

  /// 收掉整条链：注销 id、关退避定时器、按需交最终结果。
  void finish_chain(chain* c, const uvcpp_process_result& r, bool deliver) {
    if (c == nullptr || c->finished) return;
    c->finished    = true;
    c->last_result = r;
    if (c->id != 0) {
      jobs.erase(c->id);
      chains.erase(c->id);
    }
    close_chain_timer(c);
    if (deliver && c->on_exit) {
      uvcpp_process_exit_cb cb = c->on_exit;
      cb(r);
    }
    chain::release(c);
  }

  int cancel_chain(chain* c) {
    c->cancelled = true;
    if (c->current != nullptr) {
      c->current->result.cancelled = true;
      if (!c->current->proc_exited) terminate(c->current);
      return 0;
    }
    // 退避等待中：没有进程可杀，直接把这条链收掉（on_exit 仍恰好一次）。
    uvcpp_process_result r = c->last_result;
    r.cancelled  = true;
    r.restarts   = c->restarts_done;
    ++s.cancelled;
    finish_chain(c, r, true);
    return 0;
  }
};

uvcpp_process_supervisor::uvcpp_process_supervisor(uvcpp_loop* loop)
    : impl_(new impl(loop)) {}

uvcpp_process_supervisor::~uvcpp_process_supervisor() {
  // 尽力取消：句柄的真正关闭要等循环下一轮；这里只发信号并断开 owner，
  // 让 close 回调不再访问本对象。析构应在循环仍在时调用。
  for (std::map<uint64_t, job*>::iterator it = impl_->jobs.begin();
       it != impl_->jobs.end(); ++it) {
    job* j = it->second;
    j->owner = nullptr;
    if (!j->proc_exited) {
#ifdef _WIN32
      if (j->win_job != nullptr) {
        TerminateJobObject(j->win_job, 1);
      } else {
        uv_process_kill(&j->proc, kForceSignal);
      }
#else
      uv_process_kill(&j->proc, kForceSignal);
#endif
    }
    uv_timer_stop(&j->timeout_timer);
    uv_timer_stop(&j->kill_timer);
    if (!j->proc_closed && j->proc_started) {
      j->proc_closed = true;
      uv_close(reinterpret_cast<uv_handle_t*>(&j->proc), impl::on_closed);
    }
    if (j->spec.capture_stdout && !j->out_closed) {
      j->out_closed = true;
      uv_close(reinterpret_cast<uv_handle_t*>(&j->out_pipe), impl::on_closed);
    }
    if (j->spec.capture_stderr && !j->err_closed) {
      j->err_closed = true;
      uv_close(reinterpret_cast<uv_handle_t*>(&j->err_pipe), impl::on_closed);
    }
    if (!j->time_closed) {
      j->time_closed = true;
      uv_close(reinterpret_cast<uv_handle_t*>(&j->timeout_timer), impl::on_closed);
    }
    if (!j->kill_closed) {
      j->kill_closed = true;
      uv_close(reinterpret_cast<uv_handle_t*>(&j->kill_timer), impl::on_closed);
    }
  }
  // 重启链：定时器停掉、按引用计数收尾（在飞的 job 自己那一份由
  // `note_closed` 还，定时器那一份由关闭完成回调还）。
  for (std::map<uint64_t, chain*>::iterator it = impl_->chains.begin();
       it != impl_->chains.end(); ++it) {
    chain* c   = it->second;
    c->owner   = nullptr;
    c->finished = true;
    impl_->close_chain_timer(c);
  }
  for (std::map<uint64_t, chain*>::iterator it = impl_->chains.begin();
       it != impl_->chains.end(); ++it) {
    chain::release(it->second);
  }
  impl_->chains.clear();
  impl_->jobs.clear();
  delete impl_;
}

auto uvcpp_process_supervisor::impl::launch(
    chain* c, const uvcpp_process_spec& spec,
    uvcpp_process_output_cb on_output, uvcpp_process_exit_cb exit_cb,
    std::string* err, int& rc) -> job* {
  rc = 0;
  if (loop == nullptr || spec.file.empty()) {
    if (err) *err = "没有循环或可执行文件为空";
    return nullptr;
  }
  if (!spec.capture_stdout && !spec.capture_stderr) {
    // 允许，但输出回调永远收不到东西 —— 不拦。
  }

  job* j = new job();
  j->owner      = this;
  j->c          = c;
  j->id         = (c != nullptr) ? c->id : next_id++;
  if (c != nullptr) ++c->refs;  // 当前尝试那一份
  j->spec       = spec;
  j->on_output  = std::move(on_output);
  j->on_exit    = std::move(exit_cb);
  j->started_at = std::chrono::steady_clock::now();

  j->arg_store.push_back(spec.file);
  for (size_t i = 0; i < spec.args.size(); ++i) j->arg_store.push_back(spec.args[i]);
  for (size_t i = 0; i < j->arg_store.size(); ++i) {
    j->argv.push_back(const_cast<char*>(j->arg_store[i].c_str()));
  }
  j->argv.push_back(nullptr);
  for (size_t i = 0; i < spec.env.size(); ++i) j->env_store.push_back(spec.env[i]);
  for (size_t i = 0; i < j->env_store.size(); ++i) {
    j->envp.push_back(const_cast<char*>(j->env_store[i].c_str()));
  }
  if (!j->envp.empty()) j->envp.push_back(nullptr);

  uv_loop_t* uvloop = reinterpret_cast<uv_loop_t*>(loop->get_handle());
  uv_timer_init(uvloop, &j->timeout_timer);
  j->timeout_timer.data = j;
  uv_timer_init(uvloop, &j->kill_timer);
  j->kill_timer.data = j;

  uv_process_options_t opts;
  std::memset(&opts, 0, sizeof(opts));
  opts.exit_cb = impl::on_exit;
  opts.file    = j->arg_store[0].c_str();
  opts.args    = j->argv.data();
  opts.env     = j->envp.empty() ? nullptr : j->envp.data();
  opts.cwd     = spec.cwd.empty() ? nullptr : spec.cwd.c_str();
#ifdef _WIN32
  opts.flags = 0;
#else
  opts.flags = spec.kill_tree ? UV_PROCESS_DETACHED : 0;
#endif

  uv_stdio_container_t stdio[3];
  std::memset(stdio, 0, sizeof(stdio));
  stdio[0].flags = UV_IGNORE;
  stdio[1].flags = UV_IGNORE;
  stdio[2].flags = UV_IGNORE;
  if (spec.capture_stdout) {
    uv_pipe_init(uvloop, &j->out_pipe, 0);
    j->out_pipe.data = j;
    stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&j->out_pipe);
  }
  if (spec.capture_stderr) {
    uv_pipe_init(uvloop, &j->err_pipe, 0);
    j->err_pipe.data = j;
    stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(&j->err_pipe);
  }
  opts.stdio    = stdio;
  opts.stdio_count = 3;

  std::memset(&j->proc, 0, sizeof(j->proc));
  j->proc.data = j;
  j->pending_closes = 2 + (spec.capture_stdout ? 1 : 0) +
                      (spec.capture_stderr ? 1 : 0);
  const int spawn_rc = uv_spawn(uvloop, &j->proc, &opts);
  if (spawn_rc != 0) {
    j->spec.capture_stdout = spec.capture_stdout;
    j->spec.capture_stderr = spec.capture_stderr;
    fail_spawn(j, spawn_rc);
    if (err) *err = uv_strerror(spawn_rc);
    rc = spawn_rc;
    return nullptr;
  }

  j->proc_started = true;
  ++j->pending_closes;  // 进程句柄从这一刻起也会在退出时关一次
  ++s.spawned;
  jobs[j->id] = j;

#ifdef _WIN32
  // `kill_tree`：给子进程建一个 Job Object 并把它分配进去。
  //
  // `JOBOBJECT_LIMIT_KILL_ON_JOB_CLOSE` 让"关掉句柄"等价于"杀掉整棵树" ——
  // 超时/取消时主进程被 TerminateProcess 之后，后代在收尾那一刻也会跟着收掉。
  //
  // **分配发生在 `uv_spawn` 返回之后的一瞬**：在此之前子进程已经跑起来了，
  // 它在这条缝里再派生的后代不在 Job 里。libuv 不暴露 `CREATE_SUSPENDED`，
  // 这个窗口无法彻底消掉；对"监管自己拉起的进程"这个用法，后缀漏网的概率与
  // 后果都可接受，写在这里免得被当成完备保证。
  if (spec.kill_tree) {
    HANDLE hp = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                            static_cast<DWORD>(j->proc.pid));
    if (hp != nullptr) {
      HANDLE jo = CreateJobObjectW(nullptr, nullptr);
      if (jo != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        std::memset(&li, 0, sizeof(li));
        li.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(jo, JobObjectExtendedLimitInformation, &li,
                                sizeof(li));
        if (AssignProcessToJobObject(jo, hp)) {
          j->win_job = jo;
        } else {
          // 当前进程自己在一个不允许嵌套的 Job 里（老 Windows / 受限容器）：
          // 退回单进程终止语义，spawn 本身照样成功。
          CloseHandle(jo);
        }
      }
      CloseHandle(hp);
    }
  }
#endif

  if (spec.capture_stdout) {
    if (uv_read_start(reinterpret_cast<uv_stream_t*>(&j->out_pipe),
                      impl::alloc_cb, impl::on_read) != 0) {
      j->out_eof = true;
      impl::close_handle(reinterpret_cast<uv_handle_t*>(&j->out_pipe), j,
                         j->out_closed);
    }
  }
  if (spec.capture_stderr) {
    if (uv_read_start(reinterpret_cast<uv_stream_t*>(&j->err_pipe),
                      impl::alloc_cb, impl::on_read) != 0) {
      j->err_eof = true;
      impl::close_handle(reinterpret_cast<uv_handle_t*>(&j->err_pipe), j,
                         j->err_closed);
    }
  }
  if (spec.timeout_ms > 0) {
    uv_timer_start(&j->timeout_timer, impl::on_timeout, spec.timeout_ms, 0);
  }
  return j;
}


uint64_t uvcpp_process_supervisor::spawn(
    const uvcpp_process_spec& spec, uvcpp_process_output_cb on_output,
    uvcpp_process_exit_cb on_exit, std::string* err) {
  int  rc = 0;
  job* j  = impl_->launch(nullptr, spec, std::move(on_output),
                          std::move(on_exit), err, rc);
  return j != nullptr ? j->id : 0;
}

uint64_t uvcpp_process_supervisor::spawn(
    const uvcpp_process_spec& spec, uvcpp_process_output_cb on_output,
    uvcpp_process_exit_cb on_exit, std::string* err,
    uvcpp_process_restart_cb on_restart) {
  if (spec.restart_max <= 0) {
    // 没有重启策略时走旧的一次性路径，行为逐字节不变。
    return spawn(spec, std::move(on_output), std::move(on_exit), err);
  }
  chain* c      = new chain();
  c->owner      = impl_;
  c->id         = impl_->next_id++;
  c->spec       = spec;
  c->on_output  = std::move(on_output);
  c->on_exit    = std::move(on_exit);
  c->on_restart = std::move(on_restart);
  impl_->chains[c->id] = c;

  int  rc = 0;
  job* j  = impl_->launch(c, c->spec, c->on_output, c->on_exit, err, rc);
  if (j == nullptr) {
    // `fail_spawn` 已经把链收掉并交过 on_exit；与旧路径一致，失败返回 0。
    return 0;
  }
  c->current = j;
  return c->id;
}

int uvcpp_process_supervisor::cancel(uint64_t id) {
  std::map<uint64_t, chain*>::iterator cit = impl_->chains.find(id);
  if (cit != impl_->chains.end()) return impl_->cancel_chain(cit->second);

  std::map<uint64_t, job*>::iterator it = impl_->jobs.find(id);
  if (it == impl_->jobs.end()) return -1;
  job* j = it->second;
  if (j->proc_exited) return -1;
  j->result.cancelled = true;
  impl_->terminate(j);
  return 0;
}

size_t uvcpp_process_supervisor::running() const { return impl_->jobs.size(); }

uvcpp_process_supervisor_stats uvcpp_process_supervisor::stats() const {
  uvcpp_process_supervisor_stats out = impl_->s;
  out.running = impl_->jobs.size();
  return out;
}

}  // namespace uvcpp
