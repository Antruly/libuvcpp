/**
 * @file tests/functional/handle_close_semantics_func.cpp
 * @brief 锁住 uvcpp_handle 的关闭回调语义（callback_close）。
 *
 * 背景：`callback_close` 以前是「先调用使用者的关闭回调，再回头读 wrapper
 * 上的 _owns_handle / _handle 并释放底层句柄内存」。于是**在使用者的关闭
 * 回调里 delete 掉 wrapper（或它的宿主对象）就是 use-after-free** ——
 * 而仓库里 poll_func / tcp_func / pipe_func / shutdown_func /
 * tcp_client_func 都是这么写的，只是当时恰好没崩。
 *
 * 现在的约定是：
 *   1. 进入使用者回调之前，wrapper 已经和底层句柄断开（get_handle() == nullptr），
 *      底层句柄内存也已经释放完毕；
 *   2. 使用者的回调里可以安全 delete wrapper —— 回调返回后框架不再碰它。
 *
 * 这个测试就是这个约定的可执行规格。
 */
#include <atomic>
#include <cstring>
#include <iostream>

#include "handle/uvcpp_idle.h"
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_timer.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

// =========================================================================
// 1. 关闭回调里 delete wrapper —— 这曾经是 use-after-free
// =========================================================================
void test_delete_wrapper_in_close_callback() {
  std::cout << "[handle_close] delete_wrapper_in_close_callback" << std::endl;

  uvcpp_loop loop;
  loop.init();

  // 故意堆分配：回调里要 delete 它。
  uvcpp_idle* idl = new uvcpp_idle(&loop);

  std::atomic<bool> cb_ran(false);
  std::atomic<bool> handle_detached(false);

  idl->start([](uvcpp_idle*) {});

  idl->close([&](uvcpp_handle* h) {
    // 约定 1：回调跑的时候，wrapper 已经和底层句柄断开了。
    //
    // 这里必须通过捕获的指针读，不能读 h —— 下面就会把 h 删掉。
    handle_detached.store(h->get_handle() == nullptr);
    cb_ran.store(true);
    // 约定 2：在关闭回调里删除 wrapper 是允许的。
    // 旧实现会在本回调返回后再去读 h->_owns_handle / h->_handle，
    // 也就是读这块刚刚被释放的内存。
    delete static_cast<uvcpp_idle*>(h);
  });

  // 回调最多等 2 秒；跑完就停循环。
  uvcpp_timer watchdog(&loop);
  watchdog.start(
      [&](uvcpp_timer*) {
        watchdog.stop();
        watchdog.close();
        loop.stop();
      },
      2000, 0);

  loop.run(UV_RUN_DEFAULT);

  check(cb_ran.load(), "关闭回调应当被调用");
  check(handle_detached.load(), "关闭回调里 get_handle() 应当已经是 nullptr");
}

// =========================================================================
// 2. 关闭完成后 wrapper 处于「已断开」状态，重复 close 不会重新触发回调
// =========================================================================
void test_close_callback_fires_once() {
  std::cout << "[handle_close] close_callback_fires_once" << std::endl;

  uvcpp_loop loop;
  loop.init();

  uvcpp_idle idl(&loop);
  std::atomic<int> cb_count(0);

  idl.start([](uvcpp_idle*) {});
  idl.close([&](uvcpp_handle*) { ++cb_count; });

  uvcpp_timer watchdog(&loop);
  watchdog.start(
      [&](uvcpp_timer*) {
        watchdog.stop();
        watchdog.close();
        loop.stop();
      },
      1000, 0);

  loop.run(UV_RUN_DEFAULT);

  check(cb_count.load() == 1, "关闭回调应当恰好触发一次");
  check(idl.get_handle() == nullptr, "关闭完成后 get_handle() 应为 nullptr");

  // 关闭已经完成，再调一次 close() 应当是安全的空操作，
  // 绝不能把那个已经用掉的回调再跑一遍。
  idl.close();
  check(cb_count.load() == 1, "重复 close() 不应再次触发关闭回调");
}

// =========================================================================
// 3. 多个句柄在同一轮里各自关闭、各自在回调里自删
// =========================================================================
void test_multiple_self_deleting_handles() {
  std::cout << "[handle_close] multiple_self_deleting_handles" << std::endl;

  uvcpp_loop loop;
  loop.init();

  const int kCount = 16;
  std::atomic<int> closed(0);

  for (int i = 0; i < kCount; ++i) {
    uvcpp_idle* idl = new uvcpp_idle(&loop);
    idl->start([](uvcpp_idle*) {});
    idl->close([&closed](uvcpp_handle* h) {
      delete static_cast<uvcpp_idle*>(h);
      ++closed;
    });
  }

  uvcpp_timer watchdog(&loop);
  watchdog.start(
      [&](uvcpp_timer*) {
        watchdog.stop();
        watchdog.close();
        loop.stop();
      },
      3000, 0);

  loop.run(UV_RUN_DEFAULT);

  check(closed.load() == kCount, "所有句柄都应当在各自的回调里被释放");
}

// =========================================================================
// 4. 关闭回调可以为空 —— 此时框架必须自己把底层句柄内存收回去
// =========================================================================
void test_close_without_callback() {
  std::cout << "[handle_close] close_without_callback" << std::endl;

  uvcpp_loop loop;
  loop.init();

  uvcpp_idle idl(&loop);
  idl.start([](uvcpp_idle*) {});
  idl.close();  // 无回调

  uvcpp_timer watchdog(&loop);
  watchdog.start(
      [&](uvcpp_timer*) {
        watchdog.stop();
        watchdog.close();
        loop.stop();
      },
      1000, 0);

  loop.run(UV_RUN_DEFAULT);

  check(idl.get_handle() == nullptr, "无回调关闭后 get_handle() 应为 nullptr");
}

}  // namespace

int main() {
  test_delete_wrapper_in_close_callback();
  test_close_callback_fires_once();
  test_multiple_self_deleting_handles();
  test_close_without_callback();

  if (g_failures == 0) {
    std::cout << "[handle_close] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[handle_close] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}
