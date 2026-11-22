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

#ifndef _WIN32
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
      kill_tree(true) {}

uvcpp_process_result::uvcpp_process_result()
    : exit_status(0),
      term_signal(0),
      error(0),
      timed_out(false),
      cancelled(false),
      elapsed_ms(0),
      stdout_truncated(0),
      stderr_truncated(0) {}

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

  std::vector<std::string> arg_store;
  std::vector<std::string> env_store;
  std::vector<char*>       argv;
  std::vector<char*>       envp;

  std::chrono::steady_clock::time_point started_at;
};

struct uvcpp_process_supervisor::impl {
  explicit impl(uvcpp_loop* l) : loop(l) {}

  uvcpp_loop*                       loop;
  std::map<uint64_t, job*>          jobs;
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
    if (j->pending_closes == 0) delete j;
  }

  static void close_handle(uv_handle_t* h, job* j, bool& flag) {
    if (flag) return;
    flag = true;
    uv_close(h, on_close_any);
  }

  void signal_tree(job* j, bool force) {
    if (!j->proc_started || j->proc_exited) return;
#ifdef _WIN32
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

    if (!j->exit_fired && j->on_exit) {
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
      uv_process_kill(&j->proc, kForceSignal);
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
  impl_->jobs.clear();
  delete impl_;
}

uint64_t uvcpp_process_supervisor::spawn(
    const uvcpp_process_spec& spec, uvcpp_process_output_cb on_output,
    uvcpp_process_exit_cb on_exit, std::string* err) {
  if (impl_->loop == nullptr || spec.file.empty()) {
    if (err) *err = "没有循环或可执行文件为空";
    return 0;
  }
  if (!spec.capture_stdout && !spec.capture_stderr) {
    // 允许，但输出回调永远收不到东西 —— 不拦。
  }
#ifndef _WIN32
  if (spec.kill_tree && false) {
    // 保留给未来平台分支；POSIX 的 detached 已经处理。
  }
#else
  if (spec.kill_tree) {
    if (err) *err = "Windows 进程树终止（Job Object）尚未实现";
    return 0;
  }
#endif

  job* j = new job();
  j->owner      = impl_;
  j->id         = impl_->next_id++;
  j->spec       = spec;
  j->on_output  = std::move(on_output);
  j->on_exit    = std::move(on_exit);
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

  uv_loop_t* loop = reinterpret_cast<uv_loop_t*>(impl_->loop->get_handle());
  uv_timer_init(loop, &j->timeout_timer);
  j->timeout_timer.data = j;
  uv_timer_init(loop, &j->kill_timer);
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
    uv_pipe_init(loop, &j->out_pipe, 0);
    j->out_pipe.data = j;
    stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&j->out_pipe);
  }
  if (spec.capture_stderr) {
    uv_pipe_init(loop, &j->err_pipe, 0);
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
  const int rc = uv_spawn(loop, &j->proc, &opts);
  if (rc != 0) {
    j->spec.capture_stdout = spec.capture_stdout;
    j->spec.capture_stderr = spec.capture_stderr;
    impl_->fail_spawn(j, rc);
    if (err) *err = uv_strerror(rc);
    return 0;
  }

  j->proc_started = true;
  ++j->pending_closes;  // 进程句柄从这一刻起也会在退出时关一次
  ++impl_->s.spawned;
  impl_->jobs[j->id] = j;

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
  return j->id;
}

int uvcpp_process_supervisor::cancel(uint64_t id) {
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
