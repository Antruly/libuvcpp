/**
 * @file src/webapp/uvcpp_web_work_limit.h
 * @brief 工作池**在途任务上限** —— `uv_queue_work` 的背压闸门。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么需要它
 * ------------
 * `uv_queue_work` **本身没有任何背压**：线程池只有 `UV_THREADPOOL_SIZE` 个
 * 线程（libuv 默认 4），而排队**没有上限**。文件密集型负载可以把上千个任务
 * 堆在 4 个线程后面，后果是内存与延迟随队列长度线性膨胀 —— 而且从外部**看不
 * 出任何异常**，请求只是越来越慢，直到某一刻雪崩。
 *
 * 本类提供的是**计数器式**的准入控制：投递之前先 `acquire()`，拿不到名额就
 * 别投。它不是队列，不持有任务，也不参与调度 —— 只是"现在有几个活儿在池子
 * 里"这一个数字加一把闸。
 *
 * 默认上限
 * --------
 * 线程池线程数 × 4，下限 16。乘 4 的理由：线程池的线程数决定**并行度**，而队列
 * 允许略长一些才不会让每一次抖动都变成拒绝；但必须有界。下限 16 是为了让小池子
 * （默认 4 线程 → 16）不至于因为一两个慢请求就把服务打成 503。
 *
 * 那个"线程池线程数"取自 `uvcpp_threadpool.h` 的 `uvcpp_threadpool_size()`——
 * 本类**不再自己重读一遍环境变量**。那边多记了两样这里答不出来的事：
 *
 * - libuv 1.51 是**惰性**读的（第一次 `uv__work_submit` 才读，见该头文件），
 *   所以真正的先决条件是"**在本进程第一次往线程池投递之前**"设好，比"进程启动
 *   之前"松；
 * - 投过之后环境变量说什么都不算数了，那时它报的是**投递那一刻钉住**的数 ——
 *   也就是 libuv 真正在跑的那个数。
 *
 * 于是 `uvcpp_web_app::start()` 在"用户没显式设过上限"时会按**当时**的值重算
 * 一遍（`set_work_limit()` 之外另有一个标记）：`default_limit()` 若只在构造时
 * 快照一次，`uvcpp_set_threadpool_size()` 在构造与启动之间被调用就反映不到上限
 * 上。要提醒用户设池子，也在那同一个点上说 —— 见 `threadpool_size_is_set()`。
 *
 * 拿不到名额时怎么办，**按路径分工**（这不是一个可以统一的选择）
 * --------------------------------------------------------------
 * | 路径 | 处理 | 理由 |
 * |---|---|---|
 * | 静态文件 | **503 + `Retry-After`** | 请求已经整包读完了，没有退路：不投递就无事可做，而"堆着不投"只会把内存堆爆 |
 * | 上传（3b） | **`stream->pause()`** | 字节还在网上没读完，**能把压力退回去**：暂停读就是让对端等，这比拒绝好得多 |
 *
 * 所以本类不做"排队"也不做"拒绝"，它只回答"有没有名额"，怎么处理由调用方
 * 按自己的退路决定。
 *
 * 唤醒（`add_wakeup` / `remove_wakeup`）
 * -------------------------------------
 * "拿不到名额就暂停读"这条退路**必须有人来叫醒**，否则就是**永久卡死** ——
 * 而本类原本是个纯粹的原子计数器，没有任何等待队列。补上的就是最小的一套：
 * 注册一个回调（`std::function<void()>`），`release()` 在名额真的变松时逐个
 * 调用。
 *
 * 为什么是**回调**而不是"阻塞等待"或"内部队列"：
 *
 * - 调用方在 loop 线程上，阻塞等待会把整个事件循环停住 —— 那正是本项目
 *   第一条硬要求禁止的；
 * - 本类不持有任务、不参与调度，让它排队就等于把它变成第二个线程池，
 *   职责重叠且没有收益。
 *
 * 回调的契约：**在 `release()` 的调用栈上被同步调用**（也就是 loop 线程），
 * 里面应当重试 `acquire()`，拿到就开始干活、拿不到就**重新注册**（注册是
 * 一次性的：被唤醒即自动注销，不会重复投递）。
 *
 * 线程安全
 * --------
 * 计数是原子的，`acquire()`/`release()` 可以从任何线程调。**但实际用法是
 * 只在 loop 线程调**（投递前、写完成回调里），原子性只是为了让这个类在
 * 被误用时也不会静默错账。唤醒表由自身的一把互斥量保护，所以从别的线程
 * `release()` 也不会撕裂 —— 但回调仍然在被调用的那个线程上跑，调用方必须
 * 自己知道这一点（现有的两个使用方都只在 loop 线程）。
 *
 * `limit() == 0` 表示**不限**（`in_flight()` 照常计数，供观测用）。
 */
#ifndef SRC_WEBAPP_UVCPP_WEB_WORK_LIMIT_H
#define SRC_WEBAPP_UVCPP_WEB_WORK_LIMIT_H

#include <uvcpp/uvcpp_define.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>

namespace uvcpp {

/**
 * @brief 名额变松时的唤醒回调（一次性，触发后自动注销）。
 *
 * @warning 在 `release()` 的栈上被同步调用。里面**不要**做重活儿，也不要做
 *          任何假设"当前不在回调里"的事（比如就地释放某个正在跑的闭包）。
 */
using uvcpp_web_work_wakeup = ::std::function<void()>;

/**
 * @brief 工作池在途任务上限（计数器式准入控制）。
 *
 * 典型用法 —— 拿不到名额就**别投递**：
 * @code
 * if (limit->acquire()) {
 *   work->queue_work(loop, do_it, [limit](uvcpp_work* w, int) {
 *     limit->release();      // 与 acquire() 严格配对
 *     ...
 *   });
 * } else {
 *   resp.service_unavailable();
 *   resp.set_header("retry-after", "1");
 * }
 * @endcode
 *
 * **`release()` 必须与成功的 `acquire()` 一一配对。** 漏一次就是名额永久
 * 泄漏，最终表现为服务"无缘无故"开始回 503；多还一次由内部的钳位挡掉
 * （不会下溢成一堆虚假名额）。
 */
class UVCPP_API uvcpp_web_work_limit {
 public:
  /** @brief 按 `default_limit()` 初始化。 */
  uvcpp_web_work_limit();

  /** @param limit 上限；**0 = 不限**。 */
  explicit uvcpp_web_work_limit(size_t limit);

  /**
   * @brief 改上限。**不影响已经在途的任务** —— 调小只是让之后不再放新的进来
   *        （在途数可能暂时大于新上限，这是对的：那些活儿已经在池子里了，
   *        把它们判成"超限"没有任何可执行的后果）。
   */
  void set_limit(size_t limit);

  /** @brief 当前上限；0 = 不限。 */
  size_t limit() const;

  /** @brief 当前在途数（`limit() == 0` 时也照常累计，供观测）。 */
  size_t in_flight() const;

  /** @brief 是否已经到顶（`limit() == 0` 恒为 false）。 */
  bool full() const;

  /**
   * @brief 试着拿一个名额。
   * @return true = 拿到（在途数 +1，用完后**必须** `release()`）；
   *         false = 已满，**没有拿到任何东西**，不需要（也不能）`release()`。
   */
  bool acquire();

  /**
   * @brief 还回一个名额。
   *
   * 在途数为 0 时调用是**安全的空操作**（钳位而非下溢）：多还一次说明调用方
   * 记账错了，但让计数下溢成 `SIZE_MAX` 会把闸门彻底打开，比静默忽略坏得多。
   */
  void release();

  /** @brief 把在途数清零。**只在确认没有在途任务时调**（停机收尾）。 */
  void reset();

  /**
   * @brief 注册一个"名额变松时叫醒我"的回调。
   *
   * @param cb 唤醒回调；**空回调会被拒绝**（返回 `INVALID_WAKEUP`）—— 注册一个
   *           永远什么事都不做的条目只会让唤醒表无声地长胖。
   * @return 用于 `remove_wakeup()` 的 id；`INVALID_WAKEUP` 表示没注册上。
   *
   * **一次性**：被唤醒时该条目**已经**从表里摘掉了，回调里如果需要继续等
   * 就得重新 `add_wakeup()`。这一点是刻意的 —— "醒来一次就忘掉"让调用方
   * 在回调里做任何事（包括注销再注册）都不会与正在进行的投递打架。
   */
  size_t add_wakeup(uvcpp_web_work_wakeup cb);

  /**
   * @brief 注销一个唤醒回调（幂等：id 不存在或已被唤醒消费掉都是安全空操作）。
   *
   * **注销必须在对象析构前做**：回调按值捕获调用方的 `shared_ptr` 之类的
   * 东西时，留在表里就等于替它续命。用 `weak_ptr` 捕获当然更稳，但那是调用
   * 方的选择，本类不替它决定。
   */
  void remove_wakeup(size_t id);

  /** @brief 当前登记在册的唤醒回调数（观测/测试用）。 */
  size_t wakeup_count() const;

  /** @brief `add_wakeup()` 失败的返回值，也表示"没有注册"。 */
  static const size_t INVALID_WAKEUP = 0;

  /**
   * @brief 按当前线程池大小推导的默认上限：`threadpool_size() * 4`，下限 16。
   *
   * **每次调用都现算**（不是构造时算好存着）—— 所以 `uvcpp_web_app::start()`
   * 在用户没显式设过上限时能靠重算一次把"构造之后才调的
   * `uvcpp_set_threadpool_size()`"接上。
   */
  static size_t default_limit();

  /**
   * @brief libuv 的线程池线程数。
   *
   * 直通 `uvcpp_threadpool.h` 的 `uvcpp_threadpool_size()`（那条路会**逐字重演
   * libuv 的夹法**，并在投过活儿之后报**钉住**的实测值）。本类不另做一套。
   */
  static size_t threadpool_size();

  /**
   * @brief `UV_THREADPOOL_SIZE` 有没有被显式设成**一个 libuv 会原样采用的数**。
   *
   * 直通 `uvcpp_threadpool.h` 的 `uvcpp_threadpool_size_is_set()`：空串 / `abc` /
   * `0`（libuv 会给 1）、负数与超上限（会给 1024）都算没设好。比
   * `threadpool_size() != 4` 严格 —— 那个分不出"没设"与"设成了 4"。
   *
   * 给 `uvcpp_web_app::start()` 判断要不要打那条 WARN 用。那条 WARN 的时机是
   * 有意义的：libuv 是**惰性**读的，启动了但还没投过活儿时提醒**仍然来得及**
   * （真正过了那个村的是第一次往池子里投递，见 `uvcpp_threadpool.h`）。
   */
  static bool threadpool_size_is_set();

 private:
  /**
   * @brief 逐个唤醒登记在册的回调；派发期间又有 `release()` 进来就**再跑一遍**。
   *
   * 单趟的形状是「加锁快照 id → 逐个：加锁查表、拷出回调、解锁、**再查一次是否
   * 还在** → 调用」。三条都不能省：
   *
   * - **拷出来再调**：回调里会 `remove_wakeup()` 自己（甚至注销别的条目），
   *   拿着表里的引用去调等于让被调的函数拆掉自己脚下的地板。
   * - **调用前重新查一次是否还在**：上一轮回调可能已经把它注销了，而我们的
   *   快照是陈旧的。
   * - **锁在调用之前放掉**：回调里会 `acquire()` / `release()`，而 `release()`
   *   可能再次进这里 —— 抱着锁调就是自死锁。
   *
   * 而重入**不能只是"记一笔"**：快照是开始时取的，派发期间新注册的等待者不在
   * 里面，而"回调里 acquire() 失败就重新注册"恰恰是正常用法。所以外面套一层
   * `do/while`，由 `notify_again_` 决定要不要再跑一趟（完整论证见 `.cpp`）。
   */
  void notify_wakeups();

  std::atomic<size_t> limit_;
  std::atomic<size_t> in_flight_;

  mutable std::mutex wakeup_mu_;
  ::std::map<size_t, uvcpp_web_work_wakeup> wakeups_;
  size_t next_wakeup_id_;
  /**
   * @brief 是否正在派发唤醒。
   *
   * 被 `release()` 的重入撞上时，不是丢弃这次唤醒，而是置上 `notify_again_`
   * 让外层那一轮**跑完再补一遍**。
   */
  bool notifying_;
  /** @brief 派发期间又被要求唤醒一次（见 `notify_wakeups()`）。 */
  bool notify_again_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_WORK_LIMIT_H
