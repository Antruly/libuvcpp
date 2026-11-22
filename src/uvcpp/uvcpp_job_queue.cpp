/**
 * @file src/uvcpp/uvcpp_job_queue.cpp
 * @brief 作业队列的实现（内存记录 + uv 线程池执行）。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "uvcpp/uvcpp_job_queue.h"

#include <handle/uvcpp_loop.h>
#include <req/uvcpp_work.h>

#include <uv.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace uvcpp {

uvcpp_job_record::uvcpp_job_record()
    : id(0),
      state(static_cast<int>(uvcpp_job_state::PENDING)),
      attempt(0),
      max_retry(0),
      timeout_ms(0),
      backoff_ms(1000),
      not_before_ms(0) {}

uvcpp_job_submit::uvcpp_job_submit()
    : max_retry(0), timeout_ms(0), backoff_ms(1000) {}

uvcpp_job_stats::uvcpp_job_stats()
    : pending(0),
      running(0),
      success(0),
      failed(0),
      cancelled(0),
      retried(0),
      rejected(0) {}

// ===========================================================================
// 内存 store
// ===========================================================================

struct uvcpp_memory_job_store::impl {
  mutable std::mutex                        mu;
  std::map<uint64_t, uvcpp_job_record>      rows;
};

uvcpp_memory_job_store::uvcpp_memory_job_store() : impl_(new impl()) {}
uvcpp_memory_job_store::~uvcpp_memory_job_store() {}

bool uvcpp_memory_job_store::load_all(std::vector<uvcpp_job_record>& out) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  out.clear();
  out.reserve(impl_->rows.size());
  for (std::map<uint64_t, uvcpp_job_record>::const_iterator it =
           impl_->rows.begin();
       it != impl_->rows.end(); ++it) {
    out.push_back(it->second);
  }
  return true;
}

bool uvcpp_memory_job_store::save(const uvcpp_job_record& rec) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->rows[rec.id] = rec;
  return true;
}

bool uvcpp_memory_job_store::erase(uint64_t id) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->rows.erase(id);
  return true;
}

// ===========================================================================
// 上下文
// ===========================================================================

namespace {

struct job_impl {
  uvcpp_job_record    rec;
  std::atomic<bool>   cancel_requested;
  std::atomic<bool>   timed_out;
  std::string         handler_error;  ///< 仅 worker 写、after_work 读
  bool                has_handler_error;
  uvcpp_work*         work;
  uv_timer_t*         timeout_timer;

  job_impl() : cancel_requested(false), timed_out(false),
               has_handler_error(false), work(nullptr),
               timeout_timer(nullptr) {}
};

}  // namespace

struct uvcpp_job_context::impl {
  job_impl* j;
};

namespace {

}  // namespace

uvcpp_job_context::uvcpp_job_context() : impl_(new impl()) {
  impl_->j = nullptr;
}
uvcpp_job_context::~uvcpp_job_context() { delete impl_; }

bool uvcpp_job_context::cancelled() const {
  return impl_ != nullptr && impl_->j != nullptr &&
         impl_->j->cancel_requested.load();
}

const uvcpp_job_record& uvcpp_job_context::job() const {
  static const uvcpp_job_record empty;
  return impl_ != nullptr && impl_->j != nullptr ? impl_->j->rec : empty;
}

// ===========================================================================
// 队列
// ===========================================================================

struct uvcpp_job_queue::impl {
  explicit impl(uvcpp_loop* l, std::shared_ptr<uvcpp_job_store> st)
      : loop(l),
        store(std::move(st)),
        max_concurrent(4),
        max_queue(0),
        running(0),
        next_id(1),
        started(false),
        backoff_timer(nullptr),
        backoff_init(false),
        retried(0),
        rejected(0) {}

  uvcpp_loop*                                   loop;
  std::shared_ptr<uvcpp_job_store>              store;
  std::map<uint64_t, std::shared_ptr<job_impl> > jobs;
  std::map<std::string, uvcpp_job_handler>      handlers;
  std::set<std::string>                         running_keys;
  size_t                                        max_concurrent;
  size_t                                        max_queue;
  size_t                                        running;
  uint64_t                                      next_id;
  bool                                          started;
  uv_timer_t*                                   backoff_timer;
  bool                                          backoff_init;
  uint64_t                                      retried;
  uint64_t                                      rejected;
  uvcpp_job_state_cb                            state_cb;

  static int64_t now_ms() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  void notify(const uvcpp_job_record& rec) {
    if (state_cb) state_cb(rec);
  }

  void persist(job_impl* j) {
    if (store) store->save(j->rec);
  }

  static void on_backoff(uv_timer_t* t) {
    impl* self = static_cast<impl*>(t->data);
    if (self != nullptr) self->dispatch();
  }

  void schedule_backoff() {
    if (!backoff_init) return;
    int64_t earliest = 0;
    for (std::map<uint64_t, std::shared_ptr<job_impl> >::iterator it =
             jobs.begin();
         it != jobs.end(); ++it) {
      if (it->second->rec.state !=
          static_cast<int>(uvcpp_job_state::PENDING))
        continue;
      if (it->second->rec.not_before_ms <= 0) continue;
      if (earliest == 0 || it->second->rec.not_before_ms < earliest)
        earliest = it->second->rec.not_before_ms;
    }
    if (earliest == 0) return;
    int64_t delay = earliest - now_ms();
    if (delay < 1) delay = 1;
    uv_timer_start(backoff_timer, on_backoff, static_cast<uint64_t>(delay), 0);
  }

  void dispatch();
  void run_job(const std::shared_ptr<job_impl>& j);
  void finish_job(const std::shared_ptr<job_impl>& j, bool ok);
  void release_running(job_impl* j);
};

void uvcpp_job_queue::impl::release_running(job_impl* j) {
  if (running > 0) --running;
  if (!j->rec.concurrency_key.empty()) {
    running_keys.erase(j->rec.concurrency_key);
  }
}

void uvcpp_job_queue::impl::dispatch() {
  if (!started) return;
  bool progressed = true;
  while (progressed && running < max_concurrent) {
    progressed = false;
    const int64_t now = now_ms();
    for (std::map<uint64_t, std::shared_ptr<job_impl> >::iterator it =
             jobs.begin();
         it != jobs.end(); ++it) {
      job_impl* j = it->second.get();
      if (j->rec.state != static_cast<int>(uvcpp_job_state::PENDING)) continue;
      if (j->rec.not_before_ms > 0 && j->rec.not_before_ms > now) continue;
      if (!j->rec.concurrency_key.empty() &&
          running_keys.find(j->rec.concurrency_key) != running_keys.end()) {
        continue;
      }
      run_job(it->second);
      progressed = true;
      break;
    }
  }
  schedule_backoff();
}

void uvcpp_job_queue::impl::run_job(const std::shared_ptr<job_impl>& sp) {
  job_impl* j = sp.get();
  j->rec.state   = static_cast<int>(uvcpp_job_state::RUNNING);
  j->rec.attempt += 1;
  j->rec.error.clear();
  j->has_handler_error = false;
  j->cancel_requested.store(false);
  j->timed_out.store(false);
  persist(j);
  notify(j->rec);
  ++running;
  if (!j->rec.concurrency_key.empty()) {
    running_keys.insert(j->rec.concurrency_key);
  }

  if (j->rec.timeout_ms > 0) {
    j->timeout_timer = new uv_timer_t();
    uv_timer_init(reinterpret_cast<uv_loop_t*>(loop->get_handle()),
                  j->timeout_timer);
    j->timeout_timer->data = j;
    uv_timer_start(
        j->timeout_timer,
        [](uv_timer_t* t) {
          job_impl* jj = static_cast<job_impl*>(t->data);
          if (jj == nullptr) return;
          jj->timed_out.store(true);
          jj->cancel_requested.store(true);
        },
        j->rec.timeout_ms, 0);
  }

  uvcpp_job_handler handler;
  std::map<std::string, uvcpp_job_handler>::iterator hit =
      handlers.find(j->rec.type);
  if (hit != handlers.end()) handler = hit->second;

  uvcpp_work* work = new uvcpp_work();
  j->work = work;
  impl* self = this;
  const int rc = work->queue_work(
      loop,
      [sp, handler](uvcpp_work*) {
        job_impl* jj = sp.get();
        if (!handler) {
          jj->has_handler_error = true;
          jj->handler_error      = "没有注册该类型的 handler";
          return;
        }
        uvcpp_job_context ctx;
        ctx.impl_->j = jj;
        try {
          handler(ctx);
        } catch (const std::exception& e) {
          jj->has_handler_error = true;
          jj->handler_error      = e.what();
        } catch (...) {
          jj->has_handler_error = true;
          jj->handler_error      = "handler 抛出未知异常";
        }
        ctx.impl_->j = nullptr;
      },
      [sp, self](uvcpp_work* w, int status) {
        job_impl* jj = sp.get();
        delete w;
        if (jj->timeout_timer != nullptr) {
          uv_timer_stop(jj->timeout_timer);
          uv_close(reinterpret_cast<uv_handle_t*>(jj->timeout_timer),
                   [](uv_handle_t* h) {
                     delete reinterpret_cast<uv_timer_t*>(h);
                   });
          jj->timeout_timer = nullptr;
        }
        jj->work = nullptr;
        self->release_running(jj);
        const bool ok = status == 0 && !jj->has_handler_error &&
                        !jj->timed_out.load() &&
                        !jj->cancel_requested.load();
        if (!ok && jj->has_handler_error) jj->rec.error = jj->handler_error;
        self->finish_job(sp, ok);
        self->dispatch();
      });
  if (rc != 0) {
    delete work;
    j->work = nullptr;
    if (j->timeout_timer != nullptr) {
      uv_timer_stop(j->timeout_timer);
      uv_close(reinterpret_cast<uv_handle_t*>(j->timeout_timer),
               [](uv_handle_t* h) {
                 delete reinterpret_cast<uv_timer_t*>(h);
               });
      j->timeout_timer = nullptr;
    }
    release_running(j);
    j->rec.error = "投递到线程池失败";
    finish_job(sp, false);
  }
}

void uvcpp_job_queue::impl::finish_job(const std::shared_ptr<job_impl>& sp,
                                       bool ok) {
  job_impl* j = sp.get();
  if (ok) {
    j->rec.state = static_cast<int>(uvcpp_job_state::SUCCESS);
  } else if (j->cancel_requested.load() && !j->timed_out.load()) {
    j->rec.state = static_cast<int>(uvcpp_job_state::CANCELLED);
  } else {
    const bool retryable = j->rec.attempt <= j->rec.max_retry;
    if (retryable) {
      // 指数退避，封顶 1 小时。
      uint64_t backoff = j->rec.backoff_ms;
      for (int k = 1; k < j->rec.attempt && backoff < 3600ull * 1000; ++k) {
        backoff *= 2;
      }
      if (backoff > 3600ull * 1000) backoff = 3600ull * 1000;
      j->rec.state         = static_cast<int>(uvcpp_job_state::PENDING);
      j->rec.not_before_ms = now_ms() + static_cast<int64_t>(backoff);
      if (j->rec.error.empty()) {
        j->rec.error = j->timed_out.load() ? "timeout" : "failed";
      }
      ++retried;
    } else {
      j->rec.state = static_cast<int>(uvcpp_job_state::FAILED);
      if (j->rec.error.empty()) {
        j->rec.error = j->timed_out.load() ? "timeout" : "failed";
      }
    }
  }
  persist(j);
  notify(j->rec);
}

uvcpp_job_queue::uvcpp_job_queue(uvcpp_loop* loop,
                                 std::shared_ptr<uvcpp_job_store> store)
    : impl_(new impl(loop, store ? store : std::make_shared<uvcpp_memory_job_store>())) {
  if (impl_->loop != nullptr) {
    impl_->backoff_timer = new uv_timer_t();
    uv_timer_init(reinterpret_cast<uv_loop_t*>(impl_->loop->get_handle()),
                  impl_->backoff_timer);
    impl_->backoff_timer->data = impl_;
    impl_->backoff_init        = true;
  }
}

uvcpp_job_queue::~uvcpp_job_queue() {
  // 契约：析构前任务的 running 必须已经归零（work 回调持有 impl_）。
  if (impl_->backoff_init) {
    uv_timer_stop(impl_->backoff_timer);
    uv_close(reinterpret_cast<uv_handle_t*>(impl_->backoff_timer),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_timer_t*>(h); });
    impl_->backoff_timer = nullptr;
  }
  delete impl_;
}

void uvcpp_job_queue::register_handler(const std::string& type,
                                       const uvcpp_job_handler& handler) {
  impl_->handlers[type] = handler;
}

void uvcpp_job_queue::set_max_concurrent(size_t n) {
  impl_->max_concurrent = n == 0 ? 1 : n;
}

void uvcpp_job_queue::set_max_queue(size_t n) { impl_->max_queue = n; }

uint64_t uvcpp_job_queue::submit(const uvcpp_job_submit& req,
                                 std::string* err) {
  if (impl_->loop == nullptr || req.type.empty()) {
    if (err) *err = "没有循环或 type 为空";
    return 0;
  }
  size_t pending = 0;
  for (std::map<uint64_t, std::shared_ptr<job_impl> >::iterator it =
           impl_->jobs.begin();
       it != impl_->jobs.end(); ++it) {
    if (it->second->rec.state == static_cast<int>(uvcpp_job_state::PENDING))
      ++pending;
  }
  if (impl_->max_queue > 0 && pending >= impl_->max_queue) {
    ++impl_->rejected;
    if (err) *err = "队列已满";
    return 0;
  }

  std::shared_ptr<job_impl> j(new job_impl());
  j->rec.id              = impl_->next_id++;
  j->rec.type            = req.type;
  j->rec.payload         = req.payload;
  j->rec.concurrency_key = req.concurrency_key;
  j->rec.state           = static_cast<int>(uvcpp_job_state::PENDING);
  j->rec.attempt         = 0;
  j->rec.max_retry       = req.max_retry < 0 ? 0 : req.max_retry;
  j->rec.timeout_ms      = req.timeout_ms;
  j->rec.backoff_ms      = req.backoff_ms;
  j->rec.not_before_ms   = 0;
  impl_->jobs[j->rec.id] = j;
  if (impl_->store) impl_->store->save(j->rec);
  impl_->notify(j->rec);
  impl_->dispatch();
  return j->rec.id;
}

int uvcpp_job_queue::cancel(uint64_t id) {
  std::map<uint64_t, std::shared_ptr<job_impl> >::iterator it =
      impl_->jobs.find(id);
  if (it == impl_->jobs.end()) return -1;
  job_impl* j = it->second.get();
  if (j->rec.state == static_cast<int>(uvcpp_job_state::PENDING)) {
    j->rec.state = static_cast<int>(uvcpp_job_state::CANCELLED);
    impl_->persist(j);
    impl_->notify(j->rec);
    impl_->dispatch();
    return 0;
  }
  if (j->rec.state == static_cast<int>(uvcpp_job_state::RUNNING)) {
    // 协作式：只能让 handler 自己尽早退出；状态在它返回后变 CANCELLED。
    j->cancel_requested.store(true);
    return 0;
  }
  return -1;
}

void uvcpp_job_queue::start() {
  if (impl_->started) return;
  impl_->started = true;
  if (impl_->store) {
    std::vector<uvcpp_job_record> rows;
    impl_->store->load_all(rows);
    for (size_t i = 0; i < rows.size(); ++i) {
      uvcpp_job_record rec = rows[i];
      if (rec.id == 0 || impl_->jobs.find(rec.id) != impl_->jobs.end()) {
        if (rec.id >= impl_->next_id) impl_->next_id = rec.id + 1;
        continue;
      }
      if (rec.state == static_cast<int>(uvcpp_job_state::RUNNING)) {
        // 上次进程在执行中退出：回到 PENDING，由业务决定重试/放弃。
        rec.state = static_cast<int>(uvcpp_job_state::PENDING);
        rec.error = "recovered after restart";
      }
      std::shared_ptr<job_impl> j(new job_impl());
      j->rec = rec;
      impl_->jobs[rec.id] = j;
      if (rec.id >= impl_->next_id) impl_->next_id = rec.id + 1;
      impl_->store->save(j->rec);
    }
  }
  impl_->dispatch();
}

void uvcpp_job_queue::stop() { impl_->started = false; }

void uvcpp_job_queue::on_state(const uvcpp_job_state_cb& cb) {
  impl_->state_cb = cb;
}

const uvcpp_job_record* uvcpp_job_queue::find(uint64_t id) const {
  std::map<uint64_t, std::shared_ptr<job_impl> >::const_iterator it =
      impl_->jobs.find(id);
  return it == impl_->jobs.end() ? nullptr : &it->second->rec;
}

size_t uvcpp_job_queue::clear_finished() {
  size_t n = 0;
  for (std::map<uint64_t, std::shared_ptr<job_impl> >::iterator it =
           impl_->jobs.begin();
       it != impl_->jobs.end();) {
    const int st = it->second->rec.state;
    if (st == static_cast<int>(uvcpp_job_state::SUCCESS) ||
        st == static_cast<int>(uvcpp_job_state::FAILED) ||
        st == static_cast<int>(uvcpp_job_state::CANCELLED)) {
      if (impl_->store) impl_->store->erase(it->first);
      it = impl_->jobs.erase(it);
      ++n;
    } else {
      ++it;
    }
  }
  return n;
}

uvcpp_job_stats uvcpp_job_queue::stats() const {
  uvcpp_job_stats s;
  for (std::map<uint64_t, std::shared_ptr<job_impl> >::const_iterator it =
           impl_->jobs.begin();
       it != impl_->jobs.end(); ++it) {
    switch (it->second->rec.state) {
      case static_cast<int>(uvcpp_job_state::PENDING):   ++s.pending; break;
      case static_cast<int>(uvcpp_job_state::RUNNING):   ++s.running; break;
      case static_cast<int>(uvcpp_job_state::SUCCESS):   ++s.success; break;
      case static_cast<int>(uvcpp_job_state::FAILED):    ++s.failed; break;
      case static_cast<int>(uvcpp_job_state::CANCELLED): ++s.cancelled; break;
      default: break;
    }
  }
  s.retried  = impl_->retried;
  s.rejected = impl_->rejected;
  return s;
}

}  // namespace uvcpp
