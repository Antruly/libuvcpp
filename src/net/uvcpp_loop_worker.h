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

/**
 * @brief 本线程正在跑的是**几号循环**；不在任何一条上时返回 -1。
 *
 * 为什么要这个线程本地量：多循环下上层要按循环切容器（`uvcpp_http_server` 的
 * `contexts_`、webapp 的 `inflight_` 都是），而"我该动哪一份"这个问题只有
 * **当前在哪条循环上**答得了。拿连接对象去问是另一条路（`uvcpp_tcp_client::
 * loop_index()`），但有些入口手上根本没有连接 —— 比如停机时"把本循环上的
 * h2 连接都道别"。
 *
 * 谁写它：每条工作循环的线程体在开跑前自报家门（`uvcpp_loop_worker`）；
 * 接受者那条循环由 `uvcpp_tcp_server::run()` 在入口处认领为 0（那条线程是
 * 调用者的，服务端只能在自己被调用的那一刻写）。
 *
 * 线程退出时会被清回 -1，所以"线程复用后读到上一条循环的号"不会发生。
 */
int uvcpp_loop_index_of_this_thread();

/** @brief 由循环线程自己写 @ref uvcpp_loop_index_of_this_thread。 */
void uvcpp_set_loop_index_of_this_thread(int index);

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

  /**
   * @brief 循环就绪后、**开始跑之前**，在 worker 线程上跑一次（`start()` 里跑）。
   *
   * 同 `set_on_exit`：必须**在 `start()` 之前**设好（靠 `start()` 的交握手建立
   * 先后关系）。区别是时机 —— 这个跑在 `ready->set_value()` **之前**，所以
   * `start()` 返回时它已经跑完了；属主因此可以在返回前就依赖它留下的状态。
   *
   * 用途：属主在这条循环上建自己的句柄（`uv_async` / `uv_timer`）。那些只能在
   * 循环线程上建，而 `start()` 返回时循环已经在跑了 —— 这是唯一插得进去的地方。
   *
   * **钩子里抛异常 = `start()` 拿到 `UV_ECANCELED`**（不会静默挂住，也不会被
   * 当成成功）。异常本身不外传，因为跨线程没法安全地重抛。
   *
   * 抛了异常也**一样会跑 `on_exit_`**：钩子可能已经建了一半东西（`uvcpp_web_app`
   * 就是——它按顺序建 `async` / `loop` / `shutdown_timer`，中途分配失败就抛），
   * 而那些东西只能由属主自己收。所以属主的收尾**不能只挂在成功路径上**，它必须
   * 对"建到一半"也是幂等的（见 `release_loop_handles()` 那一族的空值容忍写法）。
   */
  void set_on_start(std::function<void()> fn) { on_start_ = std::move(fn); }

  /** @brief 循环指针。**只在 worker 线程上有意义**（线程退出后会被置空）。 */
  uvcpp_loop* loop() const { return loop_; }

  /**
   * @brief 这条工作循环是**几号**（0 号留给接受者，所以 worker 从 1 起）。
   *
   * 必须在 `start()` 之前装。线程体开跑的第一件事就是把它写进线程本地的
   * "我在几号循环上"（见 @ref uvcpp_loop_index_of_this_thread），于是这条线程
   * 上任何位置的代码都能问出自己在哪条循环上 —— 上层按循环切容器全靠它。
   */
  void set_loop_index(int index) { loop_index_ = index; }
  int loop_index() const { return loop_index_; }

 private:
  void thread_main(std::promise<int>* ready);

  /** @brief 把邮箱 swap 出来逐条跑。worker 线程调用。 */
  void drain();

  /**
   * @brief 摘邮箱句柄 → 泵掉挂起的关闭回调 → `loop_close` + 释放循环。worker 线程调用。
   *
   * **正常退出与"就绪钩子抛异常"两条路共用这一份**：顺序（先摘 `async_` 再泵、
   * 泵完才能 `loop_close`）不能换，两处各写一遍迟早会走岔。
   *
   * 调用者负责在它**之前**跑 `on_exit_` —— 那是属主句柄唯一的回收点，它里面
   * `uv_close` 出来的关闭回调正是靠这里的泵放掉的。
   */
  void teardown_loop();

  uvcpp_loop* loop_ = nullptr;
  int loop_index_ = 0;
  std::thread thread_;
  /** @brief 邮箱入口；**在 worker 线程上**建。nullptr = 不再受理投递。 */
  uvcpp_async* async_ = nullptr;
  std::function<void()> on_exit_;
  /** @brief 见 `set_on_start()`。与 `on_exit_` 相反的一头：跑在开跑之前。 */
  std::function<void()> on_start_;

  /** @brief 护 `q_`、`async_`。 */
  mutable std::mutex mu_;
  std::deque<std::function<void()>> q_;

  /** @brief 停机中：不再受理投递，邮箱里剩下的任务一律丢掉。 */
  std::atomic<bool> stopping_{false};
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_LOOP_WORKER_H
