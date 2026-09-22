/**
 * @file src/net/uvcpp_loop_worker.h
 * @brief 一条工作循环：循环 + 专用线程 + 跨线程邮箱。
 * @author zhuweiye
 * @version 1.0.0
 *
 * `uvcpp_loop` 本身是个裸壳（无线程、无邮箱、无线程身份），所以"一条工作循环"
 * 需要一个所有者把这几个收在一起。这就是它。
 *
 * 形状（与 `uvcpp_web_app` 的 post 同一套）：
 *
 * - 线程体里建 **一个** `uvcpp_async` 当邮箱入口，然后 `run(UV_RUN_DEFAULT)`；
 * - `post()` 任意线程可调：加锁入队 + `uv_async_send` 叫醒；
 * - 排空时把整个 `deque` **swap 出来**再逐条跑（照 `uvcpp_web_app` 的写法）——
 *   否则闭包里的自投递会让循环一直排空、永远回不到 poll，把那批 I/O 饿死。
 * - async **不 unref**：它要保住 `UV_RUN_DEFAULT` 不自己返回。退出只由
 *   `stop_and_join()` 驱动。
 * - 叫停**只投任务、绝不从别的线程 `uv_stop`**：`uv_stop` 既非线程安全，也叫不醒
 *   一个正 parked 在 `epoll_wait`/`GetQueuedCompletionStatus` 的循环。
 *
 * 本类型**不认识连接**（不认识 server、也不认识 client），谁用谁通过
 * `set_on_exit()` 挂一个"退出前收尾"的钩子。这样 webapp 那一批的 per-loop
 * 容器可以原样坐在它上面。
 */

#pragma once
#ifndef SRC_NET_UVCPP_LOOP_WORKER_H
#define SRC_NET_UVCPP_LOOP_WORKER_H

#include <uvcpp/uvcpp_config.h>

#include <uv.h>

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

namespace uvcpp {

class uvcpp_async;
class uvcpp_loop;

class uvcpp_loop_worker {
 public:
  uvcpp_loop_worker();
  ~uvcpp_loop_worker();

  uvcpp_loop_worker(const uvcpp_loop_worker&) = delete;
  uvcpp_loop_worker& operator=(const uvcpp_loop_worker&) = delete;

  /**
   * @brief 建循环 + 起线程，等到线程里的 async 装好才返回。
   *
   * 返回之后 `post()` 从任何线程调都是安全的，`loop()` 也已经可用。
   *
   * @return 0 成功；非 0 为 libuv 错误码（此时线程已退干净、循环也已还掉）。
   */
  int start();

  /**
   * @brief 投一个任务到这条循环的线程上跑。
   *
   * **被拒 = 任务当场析构**（参数按值进来，拒收时随函数返回一起销毁），所以携带
   * RAII 资源的任务不需要自己写失败清理：投递失败时那些资源已经随任务释放了。
   *
   * @return true = 已受理；false = 没受理（线程已经不在受理了）。
   */
  bool post(std::function<void()> fn);

  /**
   * @brief 让循环停下来并 join。幂等。
   *
   * 返回之后：线程已退出、邮箱里没跑的任务已经析构掉、`on_exit` 钩子已经跑完、
   * 循环已 `loop_close` 并释放。
   */
  void stop_and_join();

  /**
   * @brief 退出前收尾（在 worker 线程上、关循环之前、邮箱排空之后跑一次）。
   *
   * 必须**在 `start()` 之前**设好：它是在 worker 线程上跑的，写它的人和读它的
   * 人之间要靠 `start()` 的交握手建立先后关系。
   */
  void set_on_exit(std::function<void()> fn) { on_exit_ = std::move(fn); }

  /** @brief 循环指针。**只在 worker 线程上有意义**（线程退出后会被置空）。 */
  uvcpp_loop* loop() const { return loop_; }

 private:
  void thread_main(std::promise<int>* ready);
  /** @brief 把邮箱 swap 出来逐条跑。worker 线程调用。 */
  void drain();

  uvcpp_loop* loop_ = nullptr;
  std::thread thread_;
  /** @brief 邮箱入口；**在 worker 线程上**建。nullptr = 不再受理投递。 */
  uvcpp_async* async_ = nullptr;
  std::function<void()> on_exit_;

  /** @brief 护 `q_`、`async_`。 */
  mutable std::mutex mu_;
  std::deque<std::function<void()>> q_;

  /** @brief 停机中：不再受理投递，邮箱里剩下的任务一律丢掉。 */
  std::atomic<bool> stopping_{false};
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_LOOP_WORKER_H
