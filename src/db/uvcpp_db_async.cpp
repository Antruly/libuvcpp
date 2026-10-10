/**
 * @file src/db/uvcpp_db_async.cpp
 * @brief `uvcpp_db_async.h` 的实现。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 形状（一条在途操作的一生）：
 *
 * ```
 * 调用线程   launch()：记一笔在途账 → new uvcpp_work → queue_work(loop, ...)
 * 池线程     run_job()：client->query(...)，只写 job 自己的字段，不碰循环
 * 循环线程   complete_job()：兑现 promise、调用户回调、把在途账还掉
 * ```
 *
 * **中间那一格一定在池线程上、头尾两格的回调一定在循环线程上**，是 libuv 的
 * 保证（`uv_queue_work` 的 after-work 从 `uv__work_done` 里发），不是这里拼的
 * —— 所以没有 `uv_async` 邮箱。
 */

#include "db/uvcpp_db_async.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <db/uvcpp_db.h>
#include <handle/uvcpp_async.h>
#include <handle/uvcpp_loop.h>
#include <req/uvcpp_work.h>

namespace uvcpp {

namespace {

/// 在途账。**用 shared_ptr 而不是放在门面里**：门面可以比在途的活先没
/// （回调里 `delete` 门面是合法用法），而那时活还得把自己的那一笔还掉。
/// 账本跟着活走，才不会去写已经释放的计数器。
struct db_async_counters {
  std::atomic<size_t> in_flight{0};
  /// 只数投到**私有循环**上的活。析构要等它归零才敢停那条循环 ——
  /// 循环一停，那些 after-work 就永远发不出来，`get()` 会永久挂住。
  std::atomic<size_t> loop_in_flight{0};
};

struct db_async_job;
using job_ptr = std::shared_ptr<db_async_job>;

/// 一次异步操作的全部状态。
struct db_async_job {
  enum kind_t { K_QUERY, K_EXECUTE, K_INSERT, K_PING };

  kind_t kind = K_QUERY;
  /// **投递时**快照下来的连接。门面之后 `bind()` 别的连接也不影响这一笔。
  uvcpp_db_client* client = nullptr;
  std::string sql;
  uvcpp_db_params params;

  // ---- 结果：池线程写，循环线程读 ----
  uvcpp_db_status status = uvcpp_db_status::MISUSE;
  uvcpp_db_table table;
  int64_t affected = -1;
  uvcpp_db_value last_id;
  std::string error;

  // ---- 交付 ----
  /// 门面的存活凭证。锁不上 ⇒ 门面已经没了 ⇒ **这个回调不再来**
  /// （这正是「在回调里 delete 门面」能安全成立的那一条）。
  std::weak_ptr<char> alive;
  uvcpp_db_async::table_cb table_cb_fn;
  uvcpp_db_async::exec_cb exec_cb_fn;
  uvcpp_db_async::ping_cb ping_cb_fn;

  // ---- future 糖 ----
  std::unique_ptr<std::promise<uvcpp_db_result>> promise;

  // ---- 在途账 ----
  std::shared_ptr<db_async_counters> counters;
  bool on_private_loop = false;

  void release_slot() {
    if (on_private_loop && counters) counters->loop_in_flight.fetch_sub(1);
    if (counters) counters->in_flight.fetch_sub(1);
    counters.reset();
  }
};

/// 池线程：跑那一次数据库调用。**只碰 job 自己的字段**。
void run_job(const job_ptr& job) {
  uvcpp_db_client* c = job->client;
  if (c == nullptr) {
    job->status = uvcpp_db_status::MISUSE;
    job->error = "门面没有绑定连接";
    return;
  }
  switch (job->kind) {
    case db_async_job::K_QUERY:
      job->status = c->query(job->sql, job->params, &job->table);
      break;
    case db_async_job::K_EXECUTE:
      job->status = c->execute(job->sql, job->params, &job->affected);
      break;
    case db_async_job::K_INSERT: {
      uvcpp_db_value id;
      job->status = c->insert(job->sql, job->params, &id);
      if (uvcpp_db_ok(job->status)) {
        job->affected = 1;
        job->last_id = std::move(id);
      }
      break;
    }
    case db_async_job::K_PING:
      job->status = c->ping();
      break;
  }
  if (!uvcpp_db_ok(job->status)) {
    // 当场抓一次文案：`last_error()` 是「最近一次失败」的槽，之后被谁覆盖
    // 都有可能，而调用方是在**稍后**的循环线程上读结果的。
    job->error = c->last_error();
    if (job->error.empty()) job->error = uvcpp_db_status_name(job->status);
  }
}

/// 交付并还账。跑在循环线程上（投递失败那一路跑在调用线程上）。
void complete_job(const job_ptr& job, int libuv_status) {
  if (libuv_status != 0) {
    // 走到这里只有一种来路：`uv_cancel`。本门面从不取消，所以这是一个
    // 「按理不会发生」的兜底 —— 但不填的话，结果会带着默认的 MISUSE 和一句
    // 空文案出去，比说实话更难查。
    job->status = uvcpp_db_status::MISUSE;
    job->error = std::string("异步工作被 libuv 取消（status=") +
                 std::to_string(libuv_status) + "）";
  }

  // 1. future 糖先兑现：它没有循环可言，门面没了也得有结果。
  if (job->promise) {
    uvcpp_db_result r;
    r.status = job->status;
    r.table = std::move(job->table);
    r.affected = job->affected;
    r.last_id = std::move(job->last_id);
    r.error = job->error;
    try {
      job->promise->set_value(std::move(r));
    } catch (...) {
      // promise 已经兑现过（只可能是重复交付）。吞掉：这个模块不抛异常，
      // 而这里除了多发一次没有别的可说。
    }
    job->promise.reset();
  }

  // 2. 用户回调。门面没了就不调 —— 这是契约，不是遗漏。
  std::shared_ptr<char> alive = job->alive.lock();
  if (alive) {
    switch (job->kind) {
      case db_async_job::K_QUERY:
        if (job->table_cb_fn) job->table_cb_fn(job->status, job->table);
        break;
      case db_async_job::K_EXECUTE:
      case db_async_job::K_INSERT:
        if (job->exec_cb_fn) job->exec_cb_fn(job->status, job->affected);
        break;
      case db_async_job::K_PING:
        if (job->ping_cb_fn) job->ping_cb_fn(job->status);
        break;
    }
    // 回调里 `delete` 门面是合法的：上面这一份 `alive` 是我们自己拿住的，
    // 它保证在**这次**回调跑完之前凭证不会被释放。之后再回调也不会来了。
  }

  job->release_slot();
}

/// 投递失败那一路：**兑现 promise，但绝不调用户回调**。
///
/// 回调的契约是「在循环线程上被调**一次**」——投递都没成功，那条线程压根不会
/// 跑这个活，硬调就是在调用方自己的线程上、在 `queue_work` 的栈里调，两头
/// 都说不通。所以约定是：投递失败看**返回值**，回调不来。
void fail_before_queue(const job_ptr& job, std::string msg) {
  job->status = uvcpp_db_status::MISUSE;
  job->error = std::move(msg);
  if (job->promise) {
    uvcpp_db_result r;
    r.status = job->status;
    r.error = job->error;
    try {
      job->promise->set_value(std::move(r));
    } catch (...) {
    }
    job->promise.reset();
  }
  job->release_slot();
}

}  // namespace

// ---------------------------------------------------------------------------
// impl
// ---------------------------------------------------------------------------

struct uvcpp_db_async::impl {
  uvcpp_db_client* client = nullptr;
  /// 门面的存活凭证。析构第一件事就是 reset 它，在途的活据此知道自己成了孤儿。
  std::shared_ptr<char> alive = std::make_shared<char>(0);
  std::shared_ptr<db_async_counters> counters =
      std::make_shared<db_async_counters>();

  // ---- future 糖用的私有循环 ----
  //
  // 用 `atomic` 指针而不是加锁的成员：线程在 `ready->set_value()` **之前**
  // 存、调用方在 `future::get()` **之后**读，那对 promise/future 就是一对
  // release/acquire 边。用原子指针是让这条边在代码里显式可见 —— 不靠"读的人
  // 记得这里已经同步过了"。
  std::atomic<uvcpp_loop*> loop_{nullptr};
  std::atomic<uvcpp_async*> wake_{nullptr};
  std::atomic<bool> stopping_{false};

  std::mutex start_mu_;
  std::thread thread_;
  bool start_done_ = false;
  int start_rc_ = 0;

  ~impl() {
    // 先断回调：之后跑完的活都不再调用户闭包（`complete_job` 里 lock 失败）。
    alive.reset();
    stop_private_loop();
  }

  // -- 投递 --

  int launch(const job_ptr& job, uvcpp_loop* loop, bool private_loop) {
    job->counters = counters;
    job->on_private_loop = private_loop;
    if (private_loop) counters->loop_in_flight.fetch_add(1);
    counters->in_flight.fetch_add(1);

    uvcpp_work* w = nullptr;
    try {
      w = new uvcpp_work();
    } catch (...) {
      fail_before_queue(job, "分配 uvcpp_work 失败");
      return UV_ENOMEM;
    }
    w->set_self_free(true);

    int rc = 0;
    try {
      rc = w->queue_work(loop, [job](uvcpp_work*) { run_job(job); },
                         [job](uvcpp_work*, int status) {
                           complete_job(job, status);
                         });
    } catch (...) {
      // `uvcpp_work::queue_work` 抛的是裸 `const char*`（loop 为空、或同一个
      // work 被复用）。本模块不抛异常，这里把它翻成返回码。
      rc = UV_EINVAL;
    }

    if (rc != 0) {
      // 没投出去：libuv 两个回调都不会跑，work 得自己收。先摘掉 self_free，
      // 免得将来某条路上被第二次删除。
      w->set_self_free(false);
      delete w;
      fail_before_queue(job, std::string("投递异步工作失败（libuv ") +
                                 std::to_string(rc) + "）");
    }
    return rc;
  }

  // -- future 糖 --

  int ensure_private_loop() {
    std::lock_guard<std::mutex> lk(start_mu_);
    if (start_done_) return start_rc_;

    std::promise<int> ready;
    std::future<int> fut = ready.get_future();
    std::thread t(&impl::thread_main, this, &ready);
    const int rc = fut.get();
    thread_ = std::move(t);
    start_done_ = true;
    start_rc_ = rc;
    return rc;
  }

  void thread_main(std::promise<int>* ready) {
    uvcpp_loop* lp = nullptr;
    uvcpp_async* wk = nullptr;
    try {
      lp = new uvcpp_loop();
      wk = new uvcpp_async();
    } catch (...) {
      delete wk;
      delete lp;
      ready->set_value(UV_ENOMEM);
      return;
    }

    // **必须走无参构造 + `init(cb, loop)`**：跳板是拿 `handle->data` 找回自己
    // 的，而无参构造那次 `init()` 才把它指到 this 上。理由与
    // `uvcpp_loop_worker::thread_main` 同一处。
    //
    // 这条 async 有两个用处，缺一不可：
    //   1. 把循环**撑住** —— 没有它，两次 future 调用之间的空档里循环没有活跃
    //      句柄，`UV_RUN_DEFAULT` 会当场返回、线程就退了；
    //   2. 当**叫停通道** —— `uv_stop` 既非线程安全，也叫不醒一条正 parked 在
    //      `epoll_wait` 里的循环，所以停只能靠往这条 async 上发一次。
    const int rc = wk->init(
        [this](uvcpp_async*) {
          if (stopping_.load()) {
            uvcpp_loop* l = loop_.load();
            if (l != nullptr) l->stop();
          }
        },
        lp);
    if (rc != 0) {
      delete wk;
      lp->loop_close();
      delete lp;
      ready->set_value(rc);
      return;
    }

    wake_.store(wk);
    loop_.store(lp);
    ready->set_value(0);

    lp->run(UV_RUN_DEFAULT);

    // 收尾，顺序不能换：先把两个指针摘掉（此后 `stop_private_loop` 不会再往
    // 一条已经在拆的循环上发唤醒），再删 async（**必须**在 `uv_run` 返回之后、
    // 不能在它自己的回调里 —— 那会析构正在执行的 std::function），然后泵掉
    // 挂起的关闭回调，最后才关得掉循环。
    wake_.store(nullptr);
    loop_.store(nullptr);
    delete wk;
    for (int i = 0; i < 256 && lp->loop_alive() != 0; ++i) {
      lp->run(UV_RUN_NOWAIT);
    }
    lp->loop_close();
    delete lp;
  }

  void stop_private_loop() {
    // 等**私有循环上**的在途活收完。为什么非等不可：循环一停，那些
    // after-work 就永远不会发出来，对应的 promise 永远不兑现 ⇒ 调用方的
    // `get()` 永久挂住。等待是有界的 —— 那些活得先跑完，而数据库调用自己有
    // 超时兜底。
    //
    // 只等私有循环的那一笔账，**不等回调式**：回调式投在调用方自己的循环上，
    // 那条循环此刻可能压根没在跑（甚至永远不会跑），等它就是死等。
    if (counters) {
      while (counters->loop_in_flight.load() != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    std::thread t;
    {
      std::lock_guard<std::mutex> lk(start_mu_);
      if (!thread_.joinable()) return;
      t = std::move(thread_);
    }
    // 叫停**只投一次唤醒**，绝不在别的线程上调 `uv_stop`。
    stopping_.store(true);
    uvcpp_async* w = wake_.load();
    if (w != nullptr) w->send();
    t.join();
  }

  job_ptr make_job(db_async_job::kind_t kind, const std::string& sql,
                   const uvcpp_db_params& params) {
    job_ptr job(new db_async_job());
    job->kind = kind;
    job->client = client;
    job->sql = sql;
    job->params = params;
    job->alive = alive;
    return job;
  }

  int submit_callback(const job_ptr& job, uvcpp_loop* loop) {
    if (loop == nullptr || job->client == nullptr) return UV_EINVAL;
    return launch(job, loop, false);
  }

  uvcpp_db_async::result_fut submit_future(db_async_job::kind_t kind,
                                           const std::string& sql,
                                           const uvcpp_db_params& params) {
    job_ptr job = make_job(kind, sql, params);
    job->promise.reset(new std::promise<uvcpp_db_result>());
    // `get_future()` 与 `set_value()` 的先后无所谓：值存在共享状态里，谁先谁后
    // 都读得到。先拿 future 只是为了让下面每条失败路都能直接返回它。
    uvcpp_db_async::result_fut fut = job->promise->get_future();

    if (job->client == nullptr) {
      fail_before_queue(job, "门面没有绑定连接");
      return fut;
    }
    const int rc = ensure_private_loop();
    if (rc != 0) {
      fail_before_queue(job, std::string("起私有循环线程失败（libuv ") +
                                 std::to_string(rc) + "）");
      return fut;
    }
    launch(job, loop_.load(), true);
    return fut;
  }
};

// ---------------------------------------------------------------------------
// uvcpp_db_async
// ---------------------------------------------------------------------------

uvcpp_db_async::uvcpp_db_async() : impl_(new impl()) {}

uvcpp_db_async::uvcpp_db_async(uvcpp_db_client* client) : impl_(new impl()) {
  impl_->client = client;
}

uvcpp_db_async::~uvcpp_db_async() = default;

void uvcpp_db_async::bind(uvcpp_db_client* client) { impl_->client = client; }

uvcpp_db_client* uvcpp_db_async::client() const { return impl_->client; }

int uvcpp_db_async::query(uvcpp_loop* loop, const std::string& sql,
                          const table_cb& cb) {
  return query(loop, sql, uvcpp_db_params(), cb);
}

int uvcpp_db_async::query(uvcpp_loop* loop, const std::string& sql,
                          const uvcpp_db_params& params, const table_cb& cb) {
  job_ptr job = impl_->make_job(db_async_job::K_QUERY, sql, params);
  job->table_cb_fn = cb;
  return impl_->submit_callback(job, loop);
}

int uvcpp_db_async::execute(uvcpp_loop* loop, const std::string& sql,
                            const uvcpp_db_params& params, const exec_cb& cb) {
  job_ptr job = impl_->make_job(db_async_job::K_EXECUTE, sql, params);
  job->exec_cb_fn = cb;
  return impl_->submit_callback(job, loop);
}

int uvcpp_db_async::insert(uvcpp_loop* loop, const std::string& sql,
                           const uvcpp_db_params& params, const exec_cb& cb) {
  job_ptr job = impl_->make_job(db_async_job::K_INSERT, sql, params);
  job->exec_cb_fn = cb;
  return impl_->submit_callback(job, loop);
}

int uvcpp_db_async::ping(uvcpp_loop* loop, const ping_cb& cb) {
  job_ptr job = impl_->make_job(db_async_job::K_PING, std::string(),
                                uvcpp_db_params());
  job->ping_cb_fn = cb;
  return impl_->submit_callback(job, loop);
}

uvcpp_db_async::result_fut uvcpp_db_async::query(
    const std::string& sql, const uvcpp_db_params& params) {
  return impl_->submit_future(db_async_job::K_QUERY, sql, params);
}

uvcpp_db_async::result_fut uvcpp_db_async::execute(
    const std::string& sql, const uvcpp_db_params& params) {
  return impl_->submit_future(db_async_job::K_EXECUTE, sql, params);
}

uvcpp_db_async::result_fut uvcpp_db_async::insert(
    const std::string& sql, const uvcpp_db_params& params) {
  return impl_->submit_future(db_async_job::K_INSERT, sql, params);
}

size_t uvcpp_db_async::in_flight() const {
  return impl_->counters->in_flight.load();
}

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
