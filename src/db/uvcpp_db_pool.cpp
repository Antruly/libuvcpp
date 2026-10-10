/**
 * @file src/db/uvcpp_db_pool.cpp
 * @brief `uvcpp_db_pool.h` 的实现。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 一把互斥锁 + 一个条件变量管三样东西：
 *
 * ```
 * owned_   池子拥有的每一条连接（在借的 + 空闲的都在这里，所有权在这）
 * idle_    空闲表（存的是 owned_ 里的裸指针 + 各自的空闲起始时刻）
 * opening_ 正在开的条数（名额先占住，开的过程在锁外）
 * ```
 *
 * `size() = owned_.size()`、`in_use() = owned_.size() - idle_.size()`。**在借的
 * 连接留在 `owned_` 里** —— 正因为如此，`release()` 才能认出"这条指针不是我的"。
 *
 * 开连接（1.9~2.4 ms）**在锁外**做：先占一个名额（`opening_`）再解锁去开，开完
 * 回来填表。名额**先占后开**，所以两条线程同时借也开不出超过 `max` 条。
 *
 * 析构那一段的顺序是承重的，且**分在 `~uvcpp_db_pool()` 与 `~impl()` 两处** ——
 * 「等 work 相归零」必须在 `impl_` 还活着的时候做，见那两个析构函数的注释。
 */

#include "db/uvcpp_db_pool.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_value.h>
#include <handle/uvcpp_loop.h>
#include <req/uvcpp_work.h>

namespace uvcpp {

using steady_tp = std::chrono::steady_clock::time_point;

// ---------------------------------------------------------------------------
// 异步那一套
// ---------------------------------------------------------------------------
//
// 与 `uvcpp_db_async.cpp` 同形，但**不共用实现**：那边的 job 绑一条固定的 client，
// 这边的 job **自己去池子里借一条**、在池线程里还掉。共用的话那个 job 就得同时
// 认识"固定连接"与"从池里借"两种来源，反而更难读。

namespace {

/// 池子上在途的活。**只数 work 相**（碰池子的那一半）：它归零之后池子才敢拆。
/// 回调相（跑在调用方自己的循环上）可能一直不来，不能等 —— 所以不在这里记账。
struct pool_counters {
  std::atomic<size_t> work_in_flight{0};
};

struct pool_job;
using pool_job_ptr = std::shared_ptr<pool_job>;

struct pool_job {
  enum kind_t { K_QUERY, K_EXECUTE, K_INSERT };

  kind_t kind = K_QUERY;
  /// **只在 work 相里有值**。work 跑完就置空 —— 从那一刻起，回调相与池子再无
  /// 关系，池子也就可以先走一步。
  uvcpp_db_pool* pool = nullptr;
  std::string sql;
  uvcpp_db_params params;

  uvcpp_db_status status = uvcpp_db_status::MISUSE;
  uvcpp_db_table table;
  int64_t affected = -1;
  uvcpp_db_value last_id;
  std::string error;

  std::weak_ptr<char> alive;  ///< 池子的存活凭证
  std::shared_ptr<pool_counters> counters;

  uvcpp_db_pool::table_cb table_cb_fn;
  uvcpp_db_pool::exec_cb exec_cb_fn;
};

/// 池线程：借 → 干活 → 还。**只碰 job 自己的字段和池子的公开接口**。
void run_pool_job(const pool_job_ptr& job) {
  uvcpp_db_pool* p = job->pool;
  if (p != nullptr) {
    uvcpp_db_client* c = p->acquire();
    if (c == nullptr) {
      // 借不出（满了超时 / 新开失败）。原因池子已经写进 `last_error()`。
      job->status = uvcpp_db_status::NO_CONNECTION;
      job->error = p->last_error();
      if (job->error.empty()) job->error = "池子借不出连接";
    } else {
      switch (job->kind) {
        case pool_job::K_QUERY:
          job->status = c->query(job->sql, job->params, &job->table);
          break;
        case pool_job::K_EXECUTE:
          job->status = c->execute(job->sql, job->params, &job->affected);
          break;
        case pool_job::K_INSERT: {
          uvcpp_db_value id;
          job->status = c->insert(job->sql, job->params, &id);
          if (uvcpp_db_ok(job->status)) {
            job->affected = 1;
            job->last_id = std::move(id);
          }
          break;
        }
      }
      if (!uvcpp_db_ok(job->status)) {
        job->error = c->last_error();
        if (job->error.empty()) job->error = uvcpp_db_status_name(job->status);
      }
      // 与代借代还那组同一条规矩：这条连接要是死了就**不还回池子** —— 交给下一个
      // 借的人等于交一颗雷。
      if (job->status == uvcpp_db_status::NOT_CONNECTED) {
        p->discard(c);
      } else {
        p->release(c);
      }
    }
  }
  // 摘掉池子指针 + 还 work 相的那笔账：从这一刻起，这个 job 与那个池子再无关系。
  job->pool = nullptr;
  if (job->counters) job->counters->work_in_flight.fetch_sub(1);
}

/// 循环线程：交付。池子没了（或析构了）就不调用户回调 —— 与门面同一条契约。
void complete_pool_job(const pool_job_ptr& job, int libuv_status) {
  if (libuv_status != 0) {
    job->status = uvcpp_db_status::MISUSE;
    job->error = std::string("异步工作被 libuv 取消（status=") +
                 std::to_string(libuv_status) + "）";
  }
  std::shared_ptr<char> alive = job->alive.lock();
  if (alive) {
    switch (job->kind) {
      case pool_job::K_QUERY:
        if (job->table_cb_fn) job->table_cb_fn(job->status, job->table);
        break;
      case pool_job::K_EXECUTE:
      case pool_job::K_INSERT:
        if (job->exec_cb_fn) job->exec_cb_fn(job->status, job->affected);
        break;
    }
  }
}

/// 投递失败那一路：回调不来（契约），work 相的账要还。
void fail_pool_job(const pool_job_ptr& job, std::string msg) {
  job->status = uvcpp_db_status::MISUSE;
  job->error = std::move(msg);
  job->pool = nullptr;
  if (job->counters) job->counters->work_in_flight.fetch_sub(1);
}

}  // namespace

// ---------------------------------------------------------------------------
// impl
// ---------------------------------------------------------------------------

struct uvcpp_db_pool::impl {
  struct idle_entry {
    uvcpp_db_client* c = nullptr;
    steady_tp since;
  };

  /// 外层的门面指针。异步 job 要的是一个 `uvcpp_db_pool*`（它调的是**公开**接口 ——
  /// `acquire`/`release`/`discard` 都自己加锁），而 `this` 是 impl。构造时由外层填。
  uvcpp_db_pool* owner_ = nullptr;

  mutable std::mutex mu_;
  std::condition_variable cv_;

  std::string url_;
  std::vector<std::unique_ptr<uvcpp_db_client>> owned_;  ///< 池子拥有的全部连接
  std::vector<idle_entry> idle_;                         ///< owned_ 里的空闲子集
  size_t opening_ = 0;                                   ///< 正在开的条数（占着名额）

  size_t min_ = 1;
  size_t max_ = 8;
  int acquire_timeout_ms_ = 5000;
  int idle_timeout_ms_ = 60000;
  bool check_on_acquire_ = false;
  int client_timeout_ms_ = 0;
  bool closed_ = true;

  std::string last_error_;
  /// 最近一次 `acquire` 失败该报的返回码：借不出到底是"满了"还是"开不起来"。
  uvcpp_db_status acquire_status_ = uvcpp_db_status::NO_CONNECTION;

  uint64_t created_total_ = 0;
  uint64_t reused_total_ = 0;

  // ---- 异步 ----
  std::shared_ptr<char> alive_ = std::make_shared<char>(0);
  std::shared_ptr<pool_counters> counters_ = std::make_shared<pool_counters>();

  /// 收尾第一段：断回调、叫醒在等的 `acquire`、**等 work 相归零**。
  ///
  /// **调用点是承重的：必须由 `~uvcpp_db_pool` 在 `impl_` 还活着的时候调，
  /// 不能放回 `~impl()` 里。** 理由见 `uvcpp_db_pool::~uvcpp_db_pool`
  /// 那段注释（`std::unique_ptr` 的析构顺序两个标准库正好相反，放错地方
  /// macOS 上必崩）。这里三步顺序也不能换：
  ///
  /// 1. 断回调 —— 之后跑完的活不再调用户闭包；
  /// 2. 置 `closed_` 并叫醒在等的 `acquire` —— 否则要等到各自的借出超时
  ///    （最长 5 s 一条）；
  /// 3. **等 work 相归零**：还在池线程上的活可能正捏着一条连接，池子不能先拆。
  ///    回调相**不等** —— 那只在调用方自己的循环上，可能永远不来，等它就是死等
  ///    （那也正是 job 在 work 相结束时把 `pool` 置空的原因）。
  ///
  /// 返回之后 `closed_` 为真且 work 相为 0，此后不会再有工作线程碰到本对象。
  void wind_down() {
    alive_.reset();
    {
      std::lock_guard<std::mutex> lk(mu_);
      closed_ = true;
    }
    cv_.notify_all();

    if (counters_) {
      while (counters_->work_in_flight.load() != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }

  /// 收尾第二段：把连接全放掉（在借的也一起 —— 那些指针当场变成野指针，
  /// 头文件里写明了）。走到这里 work 相已经归零（`wind_down` 的账），
  /// 不会再有人经 `p->acquire()` 那一串摸进来。
  ///
  /// **这里不做那段等待**：`~impl()` 是 `impl_` 那个 `unique_ptr` 的 deleter，
  /// 在 libc++（macOS）上它被调用的时刻成员指针**已经**是 nullptr 了 —— 详见
  /// `uvcpp_db_pool::~uvcpp_db_pool`。
  ~impl() {
    std::lock_guard<std::mutex> lk(mu_);
    idle_.clear();
    owned_.clear();
  }

  // -- 琐碎 --

  static std::string size_desc(size_t total, size_t max) {
    return std::to_string(total) + "/" + std::to_string(max);
  }

  bool owns_locked(uvcpp_db_client* c) const {
    for (const std::unique_ptr<uvcpp_db_client>& p : owned_) {
      if (p.get() == c) return true;
    }
    return false;
  }

  bool is_idle_locked(uvcpp_db_client* c) const {
    for (const idle_entry& e : idle_) {
      if (e.c == c) return true;
    }
    return false;
  }

  /// 从 owned_ 里摘掉并析构（`unique_ptr` 一 erase，连接就关了）。**调用方持锁**
  /// —— 关连接本身是一次 `sqlite3_close` / `mysql_close` / `PQfinish`，快操作，
  /// 不值得为它解锁。
  void erase_owned_locked(uvcpp_db_client* c) {
    for (auto it = owned_.begin(); it != owned_.end(); ++it) {
      if (it->get() == c) {
        owned_.erase(it);
        return;
      }
    }
  }

  /// 回收空闲连接。`force` 为真则连没歇够的也收（仍受 `min_` 保底）。
  /// **调用方持锁**。返回关掉几条。
  size_t prune_idle_locked(bool force) {
    if (!force && idle_timeout_ms_ <= 0) return 0;
    size_t closed = 0;
    const steady_tp now = std::chrono::steady_clock::now();
    for (auto it = idle_.begin(); it != idle_.end();) {
      if (owned_.size() <= min_) break;  // 保底：到 min 就不再收了
      const bool old =
          (idle_timeout_ms_ > 0) &&
          (now - it->since >= std::chrono::milliseconds(idle_timeout_ms_));
      if (force || old) {
        uvcpp_db_client* c = it->c;
        it = idle_.erase(it);
        erase_owned_locked(c);
        ++closed;
      } else {
        ++it;
      }
    }
    return closed;
  }

  /// 开一条新的塞进 `owned_`。**返回时 `lk` 仍然是锁着的。** 开的过程在锁外。
  uvcpp_db_status open_into_locked(std::unique_lock<std::mutex>& lk) {
    ++opening_;  // 名额先占住，别的线程据此算"总数"
    const std::string url = url_;
    const int tmo = client_timeout_ms_;
    lk.unlock();

    std::unique_ptr<uvcpp_db_client> c(new uvcpp_db_client());
    if (tmo > 0) c->set_timeout_ms(tmo);  // 必须在 open 之前，才管得到连接阶段
    const uvcpp_db_status st = c->open(url);
    const std::string err = c->last_error();

    lk.lock();
    --opening_;
    if (!uvcpp_db_ok(st)) {
      last_error_ = err.empty() ? uvcpp_db_status_name(st) : err;
      return st;
    }
    owned_.push_back(std::move(c));
    ++created_total_;
    return uvcpp_db_status::OK;
  }

  /// 借一条。返回 nullptr 时 `acquire_status_` 与 `last_error_` 已经填好。
  /// **调用方持锁**（内部会按需解锁再锁）。
  uvcpp_db_client* acquire_locked(std::unique_lock<std::mutex>& lk, int timeout_ms) {
    if (closed_) {
      acquire_status_ = uvcpp_db_status::MISUSE;
      last_error_ = "池子没有 init（或已 close）";
      return nullptr;
    }
    const bool forever = (timeout_ms < 0);
    const steady_tp deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);

    for (;;) {
      prune_idle_locked(false);

      if (!idle_.empty()) {
        uvcpp_db_client* c = idle_.back().c;
        idle_.pop_back();
        ++reused_total_;
        const bool check = check_on_acquire_;
        lk.unlock();
        // 健康检查按需：默认关着（ping 一次约 20 µs，每次借出都付会把池子省下的
        // 那部分吃回去）。开着的时候宁可多等这一次 ping，也不把死连接交出去。
        const bool alive = !check || uvcpp_db_ok(c->ping());
        lk.lock();
        if (alive) {
          acquire_status_ = uvcpp_db_status::OK;
          return c;
        }
        // 死了：**丢掉、不回空闲表**，把名额让出来。
        put_back_locked(c, true);
        continue;  // 再走一遍：也许还有别的空闲的，也许能新开一条
      }

      if (owned_.size() + opening_ < max_) {
        if (uvcpp_db_ok(open_into_locked(lk))) {
          acquire_status_ = uvcpp_db_status::OK;
          return owned_.back().get();  // 刚开出来的直接算借出，不进空闲表
        }
        acquire_status_ = uvcpp_db_status::OPEN_FAILED;
        return nullptr;
      }

      // 到上限了，等别人还。
      if (timeout_ms == 0) {
        acquire_status_ = uvcpp_db_status::NO_CONNECTION;
        last_error_ =
            "池子已满（" + size_desc(owned_.size(), max_) + "）且借出超时为 0";
        return nullptr;
      }
      const bool timed_out =
          forever ? (cv_.wait(lk), false)
                  : (cv_.wait_until(lk, deadline) == std::cv_status::timeout);
      if (timed_out) {
        acquire_status_ = uvcpp_db_status::NO_CONNECTION;
        last_error_ = "借出超时 " + std::to_string(timeout_ms) + " ms：池子 " +
                      size_desc(owned_.size(), max_) + " 全在外借";
        return nullptr;
      }
      // 被叫醒之后整个重判一遍：closed_ 可能变了，也可能空出一条。
    }
  }

  /// 借一条、并**在同一次持锁里**把它失败时的返回码带出来。代借代还那组用它 ——
  /// 读 `acquire_status_` 必须与 `acquire_locked` 同锁，否则别的线程刚好又借了一次
  /// 就会把它改掉，这里报出来的码就成了别人的。
  uvcpp_db_status acquire_guarded(uvcpp_db_client** out) {
    std::unique_lock<std::mutex> lk(mu_);
    *out = acquire_locked(lk, acquire_timeout_ms_);
    return *out != nullptr ? uvcpp_db_status::OK : acquire_status_;
  }

  /// 归还 / 丢弃的公共部分。**调用方持锁**。
  void put_back_locked(uvcpp_db_client* c, bool bad) {
    if (c == nullptr) return;
    if (!owns_locked(c)) {
      last_error_ = "release/discard 了一个不属于本池的指针（no-op）";
      return;
    }
    if (is_idle_locked(c)) {
      last_error_ = "重复 release（no-op）";
      return;
    }
    if (bad || closed_) {
      // 坏掉的连接，或者池子已经关了：直接释放，把名额让出来。
      erase_owned_locked(c);
      cv_.notify_all();  // 名额腾出来了，等在超时边上的线程可以新开一条
      return;
    }
    idle_entry e;
    e.c = c;
    e.since = std::chrono::steady_clock::now();
    idle_.push_back(e);
    cv_.notify_one();  // **先入表、再唤醒**：反了就是丢唤醒
    prune_idle_locked(false);
  }

  // -- 异步投递 --

  int launch(const pool_job_ptr& job, uvcpp_loop* loop) {
    uvcpp_work* w = nullptr;
    try {
      w = new uvcpp_work();
    } catch (...) {
      fail_pool_job(job, "分配 uvcpp_work 失败");
      return UV_ENOMEM;
    }
    w->set_self_free(true);

    int rc = 0;
    try {
      rc = w->queue_work(loop, [job](uvcpp_work*) { run_pool_job(job); },
                         [job](uvcpp_work*, int status) {
                           complete_pool_job(job, status);
                         });
    } catch (...) {
      // `queue_work` 抛的是裸 `const char*`（loop 为空、或同一个 work 被复用）。
      rc = UV_EINVAL;
    }
    if (rc != 0) {
      w->set_self_free(false);
      delete w;
      fail_pool_job(job, std::string("投递异步工作失败（libuv ") +
                                 std::to_string(rc) + "）");
    }
    return rc;
  }

  /// 记 work 相那一笔账、**与 closed_ 的检查在同一把锁里**（否则"检查过 closed_
  /// 但还没记账"的窗口正好撞上析构，析构会在活儿还没跑时就拆掉池子），然后投出去。
  /// job 由两个闭包共同持有（shared_ptr），调用方不用管它的生死。
  int submit_job(const pool_job_ptr& job, uvcpp_loop* loop) {
    if (loop == nullptr) return UV_EINVAL;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (closed_) return UV_EINVAL;
      job->pool = owner_;
      job->counters = counters_;
      job->alive = alive_;
      counters_->work_in_flight.fetch_add(1);
    }
    return launch(job, loop);
  }
};

// ---------------------------------------------------------------------------
// uvcpp_db_pool
// ---------------------------------------------------------------------------

uvcpp_db_pool::uvcpp_db_pool() : impl_(new impl()) { impl_->owner_ = this; }

uvcpp_db_pool::~uvcpp_db_pool() {
  // **顺序承重：这句必须在 `impl_` 这个成员被销毁之前跑。**
  //
  // 在途的活（池线程上的 `run_pool_job`）会经**公开对象指针**回头调
  // `p->acquire()` / `p->release()` —— 那都是 `impl_->…`。而 `impl_` 是个
  // `std::unique_ptr`，两个标准库的析构顺序**正好相反**：
  //
  //   libstdc++（Linux）：先 `get_deleter()(ptr)`（跑 `~impl`），**之后**才置空；
  //   libc++（macOS）   ：`~unique_ptr(){ reset(); }`，而 `reset()` 是
  //                       **先把成员指针置成 nullptr，再调 deleter**。
  //
  // 于是只要那段「等 work 相归零」留在 `~impl()` 里，macOS 上等的就是
  // 「`impl_` 已经空了」的那扇窗：工作线程撞在 `impl_->mu_` 上直接 SEGV。
  // 现场指纹是**出错地址 `0x18`**（`mu_` 在 `impl` 里偏移 8，glibc 的
  // `pthread_mutex_lock` 头一件事是读 `pthread_mutex_t::__kind`，那又在
  // 互斥量内偏移 16 —— 8 + 16 = 0x18），栈顶是
  // `pthread_mutex_lock` ← `uvcpp_db_pool::acquire()` ← `callback_work`。
  // Linux 上永远看不见 —— libstdc++ 恰好把指针留到 deleter 返回之后；MSVC 也
  // 不置空。
  //
  // **这个 bug 在本机（Linux）也能验，办法是把 libc++ 的时序搬过来**：把
  // `uvcpp_db_pool::~uvcpp_db_pool` 临时代码写成
  // `impl* raw = impl_.release(); delete raw;`（等价于 libc++ 的"先置空再跑
  // deleter"），出事那版会在第 4 节那条用例上稳定 SEGV 在 0x18；把 drain 提到
  // 析构体里（就是现在这样）则 5/5 绿。改这种顺序时值得照这个法子先复现一遍。
  //
  // 放到析构体里（成员析构**之前**）就够：`wind_down()` 返回时 work 相已经归零，
  // 此后没有任何工作线程还会碰这个对象，`impl_` 怎么销毁都安全。
  impl_->wind_down();
}

uvcpp_db_status uvcpp_db_pool::init(const std::string& url, size_t min, size_t max) {
  if (max == 0) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->last_error_ = "max 不能是 0";
    return uvcpp_db_status::MISUSE;
  }
  if (min > max) min = max;

  close();  // 再 init 就是重来一遍

  {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->url_ = url;
    impl_->min_ = min;
    impl_->max_ = max;
    impl_->closed_ = false;
    impl_->last_error_.clear();
    impl_->created_total_ = 0;
    impl_->reused_total_ = 0;
  }

  // min 条**立刻**开：连不上就在这里报出来，不留到第一次查询 —— `open` 一次
  // 1.9~2.4 ms，摊到第一次查询上只是把那个延迟藏起来，不是消掉。
  for (size_t i = 0; i < min; ++i) {
    std::unique_lock<std::mutex> lk(impl_->mu_);
    const uvcpp_db_status st = impl_->open_into_locked(lk);
    if (!uvcpp_db_ok(st)) {
      // 起不来就退回"空的、没 init"：修好问题可以再 init 一次（此时池子是空的，
      // 不会留下半开的连接）。
      impl_->closed_ = true;
      return st;
    }
    impl_->put_back_locked(impl_->owned_.back().get(), false);
  }
  return uvcpp_db_status::OK;
}

void uvcpp_db_pool::close() {
  std::unique_lock<std::mutex> lk(impl_->mu_);
  impl_->closed_ = true;
  impl_->url_.clear();
  // 空闲的连接全部释放；**在借的一个都不动** —— 调用方手上还捏着那个指针，池子
  // 不能在它不知情的时候把连接抽掉。在借的仍然归池子所有，`release()` 或析构时
  // 才释放（见头里那条"析构前请确认 in_use() == 0"）。
  for (const impl::idle_entry& e : impl_->idle_) {
    impl_->erase_owned_locked(e.c);
  }
  impl_->idle_.clear();
  lk.unlock();
  impl_->cv_.notify_all();
}

uvcpp_db_client* uvcpp_db_pool::acquire() {
  std::unique_lock<std::mutex> lk(impl_->mu_);
  return impl_->acquire_locked(lk, impl_->acquire_timeout_ms_);
}

uvcpp_db_client* uvcpp_db_pool::acquire(int timeout_ms) {
  std::unique_lock<std::mutex> lk(impl_->mu_);
  return impl_->acquire_locked(lk, timeout_ms);
}

void uvcpp_db_pool::release(uvcpp_db_client* client) {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  impl_->put_back_locked(client, false);
}

void uvcpp_db_pool::discard(uvcpp_db_client* client) {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  impl_->put_back_locked(client, true);
}

// ---- 代借代还 ----
//
// 每一条都是同一个形状：借 → 干 → （死了就 discard，否则 release）→ 失败时把
// 连接给的错误文案记到池子的 `last_error_` 上。`NOT_CONNECTED` 一律不还回池子。

uvcpp_db_status uvcpp_db_pool::query(const std::string& sql, uvcpp_db_table* out) {
  return query(sql, uvcpp_db_params(), out);
}

uvcpp_db_status uvcpp_db_pool::query(const std::string& sql,
                                     const uvcpp_db_params& params,
                                     uvcpp_db_table* out) {
  if (out == nullptr) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->last_error_ = "结果集指针是空的";
    return uvcpp_db_status::MISUSE;
  }
  uvcpp_db_client* c = nullptr;
  const uvcpp_db_status acq = impl_->acquire_guarded(&c);
  if (c == nullptr) return acq;
  const uvcpp_db_status st = c->query(sql, params, out);
  const std::string err = c->last_error();
  if (st == uvcpp_db_status::NOT_CONNECTED) {
    discard(c);
  } else {
    release(c);
  }
  if (!uvcpp_db_ok(st)) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->last_error_ = err;
  }
  return st;
}

uvcpp_db_status uvcpp_db_pool::execute(const std::string& sql,
                                       const uvcpp_db_params& params,
                                       int64_t* affected) {
  uvcpp_db_client* c = nullptr;
  const uvcpp_db_status acq = impl_->acquire_guarded(&c);
  if (c == nullptr) return acq;
  const uvcpp_db_status st = c->execute(sql, params, affected);
  const std::string err = c->last_error();
  if (st == uvcpp_db_status::NOT_CONNECTED) {
    discard(c);
  } else {
    release(c);
  }
  if (!uvcpp_db_ok(st)) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->last_error_ = err;
  }
  return st;
}

uvcpp_db_status uvcpp_db_pool::insert(const std::string& sql,
                                      const uvcpp_db_params& params,
                                      uvcpp_db_value* new_id) {
  uvcpp_db_client* c = nullptr;
  const uvcpp_db_status acq = impl_->acquire_guarded(&c);
  if (c == nullptr) return acq;
  const uvcpp_db_status st = c->insert(sql, params, new_id);
  const std::string err = c->last_error();
  if (st == uvcpp_db_status::NOT_CONNECTED) {
    discard(c);
  } else {
    release(c);
  }
  if (!uvcpp_db_ok(st)) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->last_error_ = err;
  }
  return st;
}

uvcpp_db_status uvcpp_db_pool::ping() {
  uvcpp_db_client* c = nullptr;
  const uvcpp_db_status acq = impl_->acquire_guarded(&c);
  if (c == nullptr) return acq;
  const uvcpp_db_status st = c->ping();
  const std::string err = c->last_error();
  if (st == uvcpp_db_status::NOT_CONNECTED) {
    discard(c);
  } else {
    release(c);
  }
  if (!uvcpp_db_ok(st)) {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->last_error_ = err;
  }
  return st;
}

// ---- 异步 ----

namespace {

pool_job_ptr make_pool_job(pool_job::kind_t kind, const std::string& sql,
                           const uvcpp_db_params& params) {
  pool_job_ptr job(new pool_job());
  job->kind = kind;
  job->sql = sql;
  job->params = params;
  return job;
}

}  // namespace

int uvcpp_db_pool::query(uvcpp_loop* loop, const std::string& sql, const table_cb& cb) {
  return query(loop, sql, uvcpp_db_params(), cb);
}

int uvcpp_db_pool::query(uvcpp_loop* loop, const std::string& sql,
                         const uvcpp_db_params& params, const table_cb& cb) {
  pool_job_ptr job = make_pool_job(pool_job::K_QUERY, sql, params);
  job->table_cb_fn = cb;  // **先填回调再投递**：投出去之后那条线程随时可能跑完
  return impl_->submit_job(job, loop);
}

int uvcpp_db_pool::execute(uvcpp_loop* loop, const std::string& sql,
                           const uvcpp_db_params& params, const exec_cb& cb) {
  pool_job_ptr job = make_pool_job(pool_job::K_EXECUTE, sql, params);
  job->exec_cb_fn = cb;
  return impl_->submit_job(job, loop);
}

int uvcpp_db_pool::insert(uvcpp_loop* loop, const std::string& sql,
                          const uvcpp_db_params& params, const exec_cb& cb) {
  pool_job_ptr job = make_pool_job(pool_job::K_INSERT, sql, params);
  job->exec_cb_fn = cb;
  return impl_->submit_job(job, loop);
}

// ---- 调参与观测 ----

void uvcpp_db_pool::set_acquire_timeout_ms(int ms) {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  impl_->acquire_timeout_ms_ = ms < 0 ? 0 : ms;
}

void uvcpp_db_pool::set_idle_timeout_ms(int ms) {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  impl_->idle_timeout_ms_ = ms < 0 ? 0 : ms;
}

void uvcpp_db_pool::set_check_on_acquire(bool on) {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  impl_->check_on_acquire_ = on;
}

void uvcpp_db_pool::set_timeout_ms(int ms) {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  impl_->client_timeout_ms_ = ms < 0 ? 0 : ms;
}

size_t uvcpp_db_pool::size() const {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  return impl_->owned_.size();
}

size_t uvcpp_db_pool::in_use() const {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  return impl_->owned_.size() - impl_->idle_.size();
}

size_t uvcpp_db_pool::idle() const {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  return impl_->idle_.size();
}

size_t uvcpp_db_pool::max_size() const {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  return impl_->max_;
}

size_t uvcpp_db_pool::min_size() const {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  return impl_->min_;
}

uint64_t uvcpp_db_pool::created_total() const {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  return impl_->created_total_;
}

uint64_t uvcpp_db_pool::reused_total() const {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  return impl_->reused_total_;
}

std::string uvcpp_db_pool::last_error() const {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  return impl_->last_error_;
}

size_t uvcpp_db_pool::close_idle() {
  std::lock_guard<std::mutex> lk(impl_->mu_);
  return impl_->prune_idle_locked(true);
}

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
