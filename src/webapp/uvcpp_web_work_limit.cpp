/**
 * @file src/webapp/uvcpp_web_work_limit.cpp
 * @brief uvcpp_web_work_limit 实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <webapp/uvcpp_web_work_limit.h>

#include <uvcpp/uvcpp_threadpool.h>

#include <vector>

namespace uvcpp {

const size_t uvcpp_web_work_limit::INVALID_WAKEUP;

namespace {

/** @brief 在途上限的下限：小池子也不至于被一两个慢请求打成 503。 */
const size_t kMinLimit = 16;

}  // namespace

uvcpp_web_work_limit::uvcpp_web_work_limit()
    : limit_(default_limit()),
      in_flight_(0),
      next_wakeup_id_(1),
      notifying_(false),
      notify_again_(false) {}

uvcpp_web_work_limit::uvcpp_web_work_limit(size_t limit)
    : limit_(limit),
      in_flight_(0),
      next_wakeup_id_(1),
      notifying_(false),
      notify_again_(false) {}

void uvcpp_web_work_limit::set_limit(size_t limit) {
  limit_.store(limit, std::memory_order_relaxed);
}

size_t uvcpp_web_work_limit::limit() const {
  return limit_.load(std::memory_order_relaxed);
}

size_t uvcpp_web_work_limit::in_flight() const {
  return in_flight_.load(std::memory_order_relaxed);
}

bool uvcpp_web_work_limit::full() const {
  const size_t lim = limit_.load(std::memory_order_relaxed);
  if (lim == 0) return false;
  return in_flight_.load(std::memory_order_relaxed) >= lim;
}

bool uvcpp_web_work_limit::acquire() {
  // CAS 循环而不是"先 load 判断再 ++"：后者在并发下会让多个调用方同时看到
  // "还差一个名额"而一起挤进去，上限就成了摆设。这里判断与自增是同一笔
  // 原子操作，超限绝不可能发生。
  size_t cur = in_flight_.load(std::memory_order_relaxed);
  for (;;) {
    const size_t lim = limit_.load(std::memory_order_relaxed);
    if (lim != 0 && cur >= lim) return false;
    if (in_flight_.compare_exchange_weak(cur, cur + 1,
                                         std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
      return true;
    }
    // CAS 失败时 `cur` 已被更新成当前值，直接重试。
  }
}

void uvcpp_web_work_limit::release() {
  size_t cur = in_flight_.load(std::memory_order_relaxed);
  for (;;) {
    // 钳位：多还一次说明调用方记账错了，但**不能**让它下溢 —— 下溢之后
    // `in_flight_` 会是个接近 SIZE_MAX 的巨数，闸门就此永久打开，而且
    // 唯一的症状是"限流突然不管用了"，极难定位。静默忽略一次多余归还，
    // 至少症状是良性的。
    if (cur == 0) return;
    if (in_flight_.compare_exchange_weak(cur, cur - 1,
                                         std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
      break;
    }
  }

  // 真的少了一个在途任务 —— 名额可能刚刚变松，去叫醒等着的人。
  //
  // **`!full()` 那个前置判断不是省事，是防止无效唤醒风暴**：满是满的说明
  // 谁来都拿不到名额，叫醒一遍只会让每个等待者白跑一次 acquire() 再重新注册。
  // 漏掉这一次唤醒也不会丢事 —— 名额在还被占着的时候就一定还有在途任务，
  // 那个任务完成时还会再调一次 `release()`，到那时再叫。
  if (full()) return;

  {
    std::lock_guard<std::mutex> lk(wakeup_mu_);
    if (wakeups_.empty()) return;
  }
  notify_wakeups();
}

size_t uvcpp_web_work_limit::add_wakeup(uvcpp_web_work_wakeup cb) {
  if (!cb) return INVALID_WAKEUP;
  std::lock_guard<std::mutex> lk(wakeup_mu_);
  const size_t id = next_wakeup_id_++;
  if (id == INVALID_WAKEUP) return INVALID_WAKEUP;  // 回绕到了保留值，理论上到不了
  wakeups_[id] = cb;
  return id;
}

void uvcpp_web_work_limit::remove_wakeup(size_t id) {
  if (id == INVALID_WAKEUP) return;
  std::lock_guard<std::mutex> lk(wakeup_mu_);
  wakeups_.erase(id);
}

size_t uvcpp_web_work_limit::wakeup_count() const {
  std::lock_guard<std::mutex> lk(wakeup_mu_);
  return wakeups_.size();
}

void uvcpp_web_work_limit::notify_wakeups() {
  // **重入不能直接丢弃这次唤醒。** 早先这里写的是 `if (notifying_) return;`
  // 配一段"不会丢"的论证，那段论证有个洞：
  //
  //   派发期间的**快照**是开始时取的，期间新注册的等待者不在里面。而
  //   "派发期间注册"恰恰是正常用法（回调里 acquire() 失败就重新注册）。若这次
  //   注册发生在一个**嵌套** release() 之后 —— 比如某个被唤醒的回调在同一栈上
  //   acquire() 成功、做完了又 release()，此时池子重新变空 —— 那么：快照里
  //   没有这个等待者，嵌套那次唤醒又被守卫挡掉，而池子里已经没有在途任务了，
  //   于是"它们完成时还会再调 release()"这句话**不成立**。这个等待者就再也
  //   等不到人叫它了。
  //
  // 光"记一笔"还不够，得让外层那一轮**跑完再补一遍** —— 这就是下面的
  // `do/while`。终止性：每一轮要么快照为空、要么已在派发中重新注册（且
  // `full()` 为真，说明池子里还有活儿，那一轮之后就靠未来的 release）、要么
  // 没人再触发重入。唯一可能转不停的是"回调自己在循环里 acquire+release"，
  // 那本身就是调用方的死循环。
  if (notifying_) {
    notify_again_ = true;
    return;
  }

  notifying_ = true;
  do {
    notify_again_ = false;

    ::std::vector<size_t> ids;
    {
      std::lock_guard<std::mutex> lk(wakeup_mu_);
      for (::std::map<size_t, uvcpp_web_work_wakeup>::const_iterator it =
               wakeups_.begin();
           it != wakeups_.end(); ++it) {
        ids.push_back(it->first);
      }
    }

    for (size_t i = 0; i < ids.size(); ++i) {
      uvcpp_web_work_wakeup cb;
      {
        std::lock_guard<std::mutex> lk(wakeup_mu_);
        ::std::map<size_t, uvcpp_web_work_wakeup>::iterator it =
            wakeups_.find(ids[i]);
        // 快照是陈旧的：上一轮回调可能已经把它注销了（或被它引发的嵌套
        // 派发消费掉了）。
        if (it == wakeups_.end()) continue;
        cb = it->second;
        // **一次性**：摘掉再调。回调里即使不主动注销也不会留下死条目，
        // 更不会有"同一个等待者被同一次 release 叫醒两遍"。
        wakeups_.erase(it);
      }
      if (cb) cb();
    }

    if (!notify_again_) break;
    // 池子又满了：再跑一遍谁也别想拿到名额，只会让每个等待者白跑一次
    // acquire() 再重新注册。留给他们的是"未来那次 release 会再叫一遍"
    // （满是满的时候池子里必有在途任务，它的结算就是那次 release）。
    if (full()) break;
    {
      std::lock_guard<std::mutex> lk(wakeup_mu_);
      if (wakeups_.empty()) break;
    }
  } while (true);

  notifying_ = false;
}

void uvcpp_web_work_limit::reset() {
  in_flight_.store(0, std::memory_order_relaxed);
}

size_t uvcpp_web_work_limit::threadpool_size() {
  // 直通核心模块，**不再自己重读环境变量**：那边多记了"libuv 是惰性读的"与
  // "投过之后环境变量不算数"这两件事（见 uvcpp_threadpool.h），本类重读一遍
  // 只会得到两个答案里更旧的那个。
  return static_cast<size_t>(uvcpp_threadpool_size());
}

bool uvcpp_web_work_limit::threadpool_size_is_set() {
  return uvcpp_threadpool_size_is_set();
}

size_t uvcpp_web_work_limit::default_limit() {
  size_t lim = threadpool_size() * 4;
  if (lim < kMinLimit) lim = kMinLimit;
  return lim;
}

}  // namespace uvcpp
