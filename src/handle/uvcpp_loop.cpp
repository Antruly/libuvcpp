#include "uvcpp_loop.h"
namespace uvcpp {
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
int uvcpp_loop::run(uv_run_mode md) { return uv_run(UVCPP_LOOP_HANDLE, md); }

void uvcpp_loop::walk(::std::function<void(uvcpp_handle *, void *)> walk_cb,
                      void *arg) {
  this->handle_walk_cb = walk_cb;
  this->walk_arg_ = arg;
  uv_walk(UVCPP_LOOP_HANDLE, uvcpp_loop::callback_walk, this);
}

void uvcpp_loop::callback_walk(uv_handle_t *handle, void *arg) {
  if (reinterpret_cast<uvcpp_loop *>(arg)->handle_walk_cb)
    reinterpret_cast<uvcpp_loop *>(arg)->handle_walk_cb(
        reinterpret_cast<uvcpp_handle *>(handle->data),
        reinterpret_cast<uvcpp_loop *>(arg)->walk_arg_);
}

uvcpp_loop *uvcpp_loop::default_loop() {
  static uvcpp_loop default_loop;
  default_loop.init();
  return &default_loop;
}

int uvcpp_loop::init() {
  int ret = uv_loop_init(UVCPP_LOOP_HANDLE);
  closed_ = false;
  this->set_handle_data();
  return ret;
}

int uvcpp_loop::loop_alive() { return uv_loop_alive(UVCPP_LOOP_HANDLE); }

void uvcpp_loop::stop() {
  if (uv_loop_alive(UVCPP_LOOP_HANDLE)) {
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