#include "uvcpp_loop.h"

#if !defined(_WIN32)
#include <signal.h>
#endif

namespace uvcpp {

#if !defined(_WIN32)
namespace {

/**
 * @brief 每进程做一次：把 SIGPIPE 的处置设成**忽略**。
 *
 * **为什么非做不可。** 往一个已被对端关掉的 socket 上写，Linux 会给本进程发
 * SIGPIPE，而默认处置是**结束进程** —— 对端什么时候走是网络事件，不是本进程
 * 的错，一个网络库让使用者因此丧命说不过去。libuv 只在有 `SO_NOSIGPIPE` 的
 * 平台挡这一下（BSD/macOS，见 `_local_deps/libuv/src/unix/core.c` 里那处
 * `setsockopt`），**Linux 的写路径用的是不带 `MSG_NOSIGNAL` 的
 * `sendmsg`/`uv__writev`**，所以只有 Linux 上会真的把进程打死
 * （CI 上 `test_web_ssl_client_func` / `test_web_stream_response_func` 报
 * `(SIGPIPE)` 就是这个 —— 那两条用例都是**故意**中途丢下对端的）。
 *
 * 忽略之后那次写以 `EPIPE` 失败，libuv 把它变成 `UV_EPIPE` 交给写完成回调：
 * 库本来就有这条送达路径，缺的只是"别先死"。
 *
 * **只在当前处置是 `SIG_DFL` 时才动。** 应用自己装过（包括它自己设成
 * `SIG_IGN`）就说明是有意为之，不改。这是**进程级**副作用，而构造函数里那次
 * `init()` 是每一条环路都必然走到的唯一入口，所以放在这里。
 */
void ignore_sigpipe_once() {
  // 函数局部静态的初始化自 C++11 起是线程安全的，不必额外加锁。
  static const bool done = []() {
    struct sigaction cur;
    if (::sigaction(SIGPIPE, nullptr, &cur) != 0) return true;
    if (cur.sa_handler != SIG_DFL) return true;
    struct sigaction ign;
    ign.sa_handler = SIG_IGN;
    ign.sa_flags = 0;
    // **这里不能写 `::sigemptyset`。** macOS 的 sigemptyset 是**宏**而不是函数
    // （`Libc/include/signal.h`：`#define sigemptyset(set) (*(set) = 0, 0)`，
    // 和 sigaction 的声明同在 `#ifndef _ANSI_SOURCE` 那一块里），加了 `::` 就会
    // 展开成 `::(*(&ign.sa_mask) = 0, 0)` —— 语法错误。同一段东西在 MSVC 上被
    // `#if !defined(_WIN32)` 整段跳过，本机看不见，改回 `::` 之前先看这条。
    // `::sigaction` 可以带 `::`：它是真函数，不是宏。
    sigemptyset(&ign.sa_mask);
    // 设不上（极罕见）也只能这样：它不是本函数的职责，不影响其余逻辑。
    ::sigaction(SIGPIPE, &ign, nullptr);
    return true;
  }();
  (void)done;
}

}  // namespace
#endif  // !_WIN32

uvcpp_loop::uvcpp_loop() : uvcpp_handle() {
  uv_loop_t *loop = uvcpp::uvcpp_alloc<uv_loop_t>();
  if (loop == nullptr)
    throw std::bad_alloc();
  this->set_handle(loop, true);
  this->init();
}
uvcpp_loop::~uvcpp_loop() {
  // uv_loop_t is NOT a uv_handle_t: the base free_handle() would call
  // uv_is_closing()/uv_is_active() on the loop pointer (reading the wrong
  // memory, UB) and could uv_close() the loop or double uv_loop_close it.
  // The loop is closed by its owner via loop_close() before delete; here we
  // only free the uv_loop_t storage we own, then detach so free_handle() no-ops.
  uv_handle_t *h = this->get_handle();
  if (h == nullptr)
    return;
  uv_loop_t *loop = reinterpret_cast<uv_loop_t *>(h);

  // 属主可能没关成过（`loop_close()` 返回 UV_EBUSY —— 队列里还有句柄或请求
  // 时就会这样），也可能压根没调 `loop_close()`。这里补一次：`uv_loop_close()`
  // **只看队列、不跑任何回调**，所以不会把已经失效的应用回调拉起来。
  //
  // 之所以不能改成"顺手拨几轮 `uv_run` 把队列收干净再关"：那会把**已经到期的
  // 应用回调**在属主析构到一半时拉起来，而回调里的对象往往已经销毁；而且它会
  // 先跑掉句柄的关闭回调、让 wrapper 手里的指针变成已释放内存。实测
  // （2026-09-16）这条路把 tty/udp 那批用例直接打崩（0xC0000409 / 0xC0000005）。
  // 要收干净只能由属主在**自己还活着**的时候关句柄、拨循环 —— 见
  // `uvcpp_udp_server` / `uvcpp_udp_client` 两处析构的写法。
  //
  // 关不掉就**不释放**这块内存：它上面还挂着句柄队列，释放之后那些句柄的
  // `handle->loop` 就是悬垂，而 `free_handle()` 分支 (1) 里的 `uv_close()`
  // 正会往队列里写（见 `uvcpp_handle.cpp` 对该分支的说明）—— use-after-free
  // 写。泄漏一块仍然有效的内存，换掉一个悬垂。
  if (!closed_ && uv_loop_close(loop) != 0) {
    // 即便泄漏也**必须** detach：不然基类 `~uvcpp_handle` 会拿着这个
    // `uv_loop_t*` 当 `uv_handle_t*` 用（`uv_is_closing` 读错内存，
    // `uv_close` 直接往循环里写）。
    this->detach_handle();
    return;
  }
  uvcpp_free_bytes(h);
  this->detach_handle();
}
int uvcpp_loop::run(uv_run_mode md) {
  // 关过的 loop 不能再拨：`uv_loop_close()` 之后这块 `uv_loop_t` 已经还给 libuv
  // （debug 构建里更是被整块填成 -1），`uv_run` 会拿 `pending_reqs_tail` 之类的
  // 野指针去解引用 —— 这正是 Ubuntu CI 上那批 SEGFAULT 的来源：用例自己
  // `loop_close()` 了，收尾用的 `loop_drain` 析构里还会再拨一次。
  if (closed_) return 0;
  // 计数包住整段 `uv_run`：进出的路可能不止一条（见 `is_running()`），
  // 中途任何一层里问都得答"在跑"。
  ++run_depth_;
  const int rc = uv_run(UVCPP_LOOP_HANDLE, md);
  --run_depth_;
  return rc;
}

void uvcpp_loop::walk(::std::function<void(uvcpp_handle *, void *)> walk_cb,
                      void *arg) {
  if (closed_) return;  // `uv_walk` 走的就是被投毒的 handle_queue
  this->handle_walk_cb = walk_cb;
  this->walk_arg_ = arg;
  uv_walk(UVCPP_LOOP_HANDLE, uvcpp_loop::callback_walk, this);
}

void uvcpp_loop::callback_walk(uv_handle_t *handle, void *arg) {
  uvcpp_loop *self = reinterpret_cast<uvcpp_loop *>(arg);
  if (self == nullptr) {
    return;
  }
  // 拷一份再调用：回调里 `delete self` 是合法用法（见 uvcpp_handle::
  // callback_close 的说明），就地调用等于在正在执行的闭包上删对象。
  // 每次 uv_walk 会为每个句柄各触发一次，所以是拷不是搬。
  auto cb = self->handle_walk_cb;
  if (cb) {
    cb(reinterpret_cast<uvcpp_handle *>(handle->data), self->walk_arg_);
  }
}

uvcpp_loop *uvcpp_loop::default_loop() {
  static uvcpp_loop default_loop;
  default_loop.init();
  return &default_loop;
}

int uvcpp_loop::init() {
#if !defined(_WIN32)
  ignore_sigpipe_once();
#endif
  int ret = uv_loop_init(UVCPP_LOOP_HANDLE);
  closed_ = false;
  this->set_handle_data();
  return ret;
}

int uvcpp_loop::loop_alive() {
  // `uv_loop_close()` 不清这几个计数（debug 下反而是 -1），光问 libuv 会一直
  // 答"活着" —— 于是任何拿它当上界的等待（`loop_drain`）都不会退出。
  if (closed_) return 0;
  return uv_loop_alive(UVCPP_LOOP_HANDLE);
}

void uvcpp_loop::stop() {
  // 经 `loop_alive()` 走，别直接问 libuv：同上，关过的 loop 那里是野计数。
  if (this->loop_alive()) {
    uv_stop(UVCPP_LOOP_HANDLE);
  }
}

int uvcpp_loop::loop_close() {
  if (closed_) return 0;  // 幂等：再调一次会让 libuv 二次释放内部资源
  stop();
  const int rc = uv_loop_close(UVCPP_LOOP_HANDLE);
  if (rc == 0) closed_ = true;
  return rc;
}

int uvcpp_loop::close() { return loop_close(); }

} // namespace uvcpp