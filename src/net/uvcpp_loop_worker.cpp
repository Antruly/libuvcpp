/**
 * @file src/net/uvcpp_loop_worker.cpp
 * @brief Implementation of uvcpp_loop_worker.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <net/uvcpp_loop_worker.h>

#include <handle/uvcpp_async.h>
#include <handle/uvcpp_loop.h>

#include <cstdio>
#include <exception>
#include <utility>

namespace uvcpp {

namespace {

/** @brief 退出前把挂起的关闭回调放掉的有界轮数（抄 `~uvcpp_tcp_server` 第三步）。 */
const int kExitPumpRounds = 256;

/**
 * @brief 线程本地的"我在几号循环上"。
 *
 * 用 `thread_local` 而不是查表：问这个问题的位置**全在热路径上**（每来一条
 * 连接、每收一个请求都要按循环切容器），而查表要么带锁、要么按线程身份线性
 * 扫。线程本地量是一次 TLS 读。
 */
thread_local int t_loop_index = -1;

}  // namespace

int uvcpp_loop_index_of_this_thread() { return t_loop_index; }

void uvcpp_set_loop_index_of_this_thread(int index) { t_loop_index = index; }

uvcpp_loop_worker::uvcpp_loop_worker() {}

uvcpp_loop_worker::~uvcpp_loop_worker() {
  stop_and_join();

  // 线程正常退出时它自己已经把 loop_ 关掉并置空了；这里管的是"从来没起来过"
  // 那条路（`start()` 返回了错误码，或者压根没调过）。
  if (loop_ != nullptr) {
    loop_->loop_close();
    delete loop_;
    loop_ = nullptr;
  }
}

int uvcpp_loop_worker::start() {
  if (thread_.joinable()) {
    return UV_EALREADY;
  }
  if (loop_ == nullptr) {
    loop_ = new uvcpp_loop();
  }

  // 交握手：线程里把 async 装好之后才放行。`ready->set_value()` 是 release 边、
  // `fut.get()` 是 acquire 边，所以 `start()` 一返回，从别的线程读 `async_`
  // 就是安全的 —— 这也是 `post()` 敢直接碰它的理由。
  std::promise<int> ready;
  std::future<int> fut = ready.get_future();
  std::thread t(&uvcpp_loop_worker::thread_main, this, &ready);
  thread_ = std::move(t);

  return fut.get();
}

void uvcpp_loop_worker::thread_main(std::promise<int>* ready) {
  // 第一件事就自报家门：下面每一句（包括 `on_start_` 钩子）都可能在问
  // "我在哪条循环上"。放在这里而不是 `on_start_` 里，是因为钩子是**可选**的
  // （没装就没有），而线程身份不是。
  t_loop_index = loop_index_;

  // **必须走无参构造 + `init(cb, loop)`**：`callback_init` 是从 `handle->data`
  // 里把自己取回来的，而无参构造那次 `init()` 才会把 data 指到 this 上
  // （见 `uvcpp_handle::reset_handle_state`）。
  async_ = new uvcpp_async();
  const int rc = async_->init([this](uvcpp_async*) { drain(); }, loop_);
  if (rc != 0) {
    delete async_;
    async_ = nullptr;
    if (loop_ != nullptr) {
      loop_->loop_close();
      delete loop_;
      loop_ = nullptr;
    }
    ready->set_value(rc);
    return;
  }

  // 属主的"就绪"钩子：跑在放行 `start()` **之前**，所以 `start()` 返回时它已经
  // 跑完，属主可以立刻依赖它建好的东西（per-loop 的 async / timer）。
  //
  // 抛异常不能让 promise 悬着 —— 那会把 `start()` 永久挂住，而"挂住"看起来跟
  // "慢"一模一样。所以接住、翻成一个确定的错误码放行；异常本身跨线程没法安全
  // 重抛，只能吞掉（这点写在 `set_on_start()` 的契约里）。
  if (on_start_) {
    int hrc = 0;
    try {
      on_start_();
    } catch (...) {
      hrc = UV_ECANCELED;
    }
    if (hrc != 0) {
      // 与上面 async 建失败那条路**同一套收尾**：钩子可能已经建了东西，但它
      // 自己最清楚怎么收（属主的收尾在 `on_exit_` 里），所以这里只保证循环
      // 与邮箱不泄漏，然后走人。
      delete async_;
      async_ = nullptr;
      if (loop_ != nullptr) {
        loop_->loop_close();
        delete loop_;
        loop_ = nullptr;
      }
      ready->set_value(hrc);
      return;
    }
  }

  ready->set_value(0);
  loop_->run(UV_RUN_DEFAULT);

  // -------------------------------------------------------------------
  // 退出路径。顺序不能换。
  // -------------------------------------------------------------------

  // 1. 排空邮箱。走到这儿闸已经关了（是 `stop_and_join` 投进来的那条任务关的），
  //    所以队列里剩下的任务一律丢掉 —— 它们析构时把自己携带的资源（比如一个
  //    中转中的 socket）一起带走。
  drain();

  // 2. 让属主收尾（把挂在本循环上的东西关掉）。在**本线程**上跑：那些东西的
  //    关闭回调必须在它们自己的循环线程上执行。
  if (on_exit_) {
    on_exit_();
  }

  // 3. `delete async_` 必须在 `uv_run` 返回之后、**不能**在它自己的回调里
  //    （那会析构正在执行的 std::function）。先把指针摘掉，让 `post()` 立刻
  //    开始拒收，再删。
  uvcpp_async* async = nullptr;
  {
    std::lock_guard<std::mutex> lk(mu_);
    async = async_;
    async_ = nullptr;
  }
  delete async;

  // 4. 把挂起的关闭回调放掉，循环才关得掉（`uv_loop_close` 队列非空会返回
  //    `UV_EBUSY`，那时 `~uvcpp_loop` 只会泄漏那块内存而不是释放）。
  for (int i = 0; i < kExitPumpRounds; ++i) {
    if (loop_ == nullptr || loop_->loop_alive() == 0) break;
    loop_->run(UV_RUN_NOWAIT);
  }

  if (loop_ != nullptr) {
    loop_->loop_close();
    delete loop_;
    loop_ = nullptr;
  }

  // 5. 最后一次自报家门。**这一步今天不是必需的**：线程本地量随线程结束一起
  //    销毁，而这条线程体就是线程的入口。留着是因为"循环没了，我就不在几号
  //    循环上了"这条不变式值得显式写出来 —— 真出现"同一线程跑完一条循环再跑
  //    别的东西"的那天（比如线程体被搬到复用线程的池子里），它才不会静默说谎。
  t_loop_index = -1;
}

bool uvcpp_loop_worker::post(std::function<void()> fn) {
  if (!fn) return false;
  std::lock_guard<std::mutex> lk(mu_);
  if (async_ == nullptr || stopping_.load()) {
    return false;  // fn 在这里析构 —— 捕获得来的资源随之释放
  }
  q_.push_back(std::move(fn));
  // 在锁内发：保证异步唤醒一定发生在入队**之后**（在锁外发就可能出现
  // "worker 排空了个空队列、然后睡过去，而任务还在队列里"的丢唤醒）。
  async_->send();
  return true;
}

void uvcpp_loop_worker::stop_and_join() {
  if (!thread_.joinable()) return;

  {
    std::lock_guard<std::mutex> lk(mu_);
    if (async_ != nullptr && !stopping_.load()) {
      // 关闸这件事放在**任务里**做，不在调用线程上做：闸一关，队列里剩下的任务
      // 就都会被丢掉，而"丢掉"是靠 drain 看到闸关了才生效的 —— 闸必须在 worker
      // 线程上关，才有一个明确的先后：这一条任务之前的照跑，之后的照丢。
      q_.push_back([this]() {
        stopping_.store(true);
        if (loop_ != nullptr) loop_->stop();
      });
      async_->send();
    }
  }

  thread_.join();
}

void uvcpp_loop_worker::drain() {
  // swap 整个队列出来再跑：闭包里的自投递会落到新的空队列上，跑完这一批就
  // 回得到 poll（否则持续自投递会把循环饿死在这批里）。
  std::deque<std::function<void()>> batch;
  {
    std::lock_guard<std::mutex> lk(mu_);
    batch.swap(q_);
  }

  for (std::deque<std::function<void()>>::iterator it = batch.begin();
       it != batch.end(); ++it) {
    if (stopping_.load()) break;  // 剩下的在 batch 析构时释放（连资源一起）
    try {
      (*it)();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[uvcpp_loop_worker] posted task threw: %s\n",
                   e.what());
    } catch (...) {
      std::fprintf(stderr, "[uvcpp_loop_worker] posted task threw (unknown)\n");
    }
  }
}

}  // namespace uvcpp
