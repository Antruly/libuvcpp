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
#include "loop_drain.h"

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

  uvcpp_test::loop_drain drain_loop(&loop);
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

  uvcpp_test::loop_drain drain_loop(&loop);
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

  uvcpp_test::loop_drain drain_loop(&loop);
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

  uvcpp_test::loop_drain drain_loop(&loop);
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

// =========================================================================
// 5. 句柄释放之后（get_handle() == nullptr），整族入口都必须安全
//
// `_handle == nullptr` 的含义是"底层 uv_handle_t 已经被 callback_close 还回
// 去了"，不是"这个 wrapper 不能用了"。原先 `ref`/`unref`/`has_ref`/`fileno`/
// `handle_size`/`handle_get_type`/`handle_type_name`/`handle_get_data`/
// `handle_get_loop`/`handle_set_data` 这一族**直接把 `_handle` 交给 libuv**，
// 于是在已释放的句柄上调用就是空指针解引用（libuv 读 `handle->flags`）。
// 更别扭的是同一个类里 `close()` 判空而 `close(cb)` 不判 —— 前者安全后者崩。
//
// 判据分两半，缺一不可：
//   (a) 已释放的句柄上，整族调用都必须安全返回"空"（而不是崩）；
//   (b) **活句柄上这一族仍然返回真值** —— 只有 (a) 的话，"把每个入口都改成
//       直接 return 0"也能通过，那不是修好，是把功能删掉。
// =========================================================================
void test_null_handle_guards() {
  std::cout << "[handle_close] null_handle_guards" << std::endl;

  uvcpp_loop loop;

  uvcpp_test::loop_drain drain_loop(&loop);
  loop.init();

  uvcpp_idle idl(&loop);
  idl.start([](uvcpp_idle*) {});

  // (b) 活句柄：这一族必须给出真值，守卫生效不能以牺牲功能为代价。
  check(idl.get_handle() != nullptr, "活句柄 get_handle() 非空");
  check(idl.handle_size() > 0, "活句柄 handle_size() > 0");
  check(idl.has_ref() == 1, "活句柄默认是 ref 状态");
  check(idl.is_active() == 1, "start 之后是 active");
#if UV_VERSION_MAJOR >= 1 && UV_VERSION_MINOR >= 18
  check(idl.handle_get_type() == UV_IDLE, "活句柄 handle_get_type() == UV_IDLE");
  check(idl.handle_type_name() != nullptr, "活句柄 handle_type_name() 非空");
  check(idl.handle_get_loop() != nullptr, "活句柄 handle_get_loop() 非空");
#endif

  std::atomic<bool> closed(false);
  idl.close([&closed](uvcpp_handle*) { closed.store(true); });

  uvcpp_timer watchdog(&loop);
  watchdog.start(
      [&](uvcpp_timer*) {
        watchdog.stop();
        watchdog.close();
        loop.stop();
      },
      1000, 0);
  loop.run(UV_RUN_DEFAULT);

  check(closed.load(), "关闭回调跑过了");
  // 前提断言：没有这一条，下面测的就不是"已释放的句柄"。
  check(idl.get_handle() == nullptr, "关闭后 get_handle() == nullptr");

  // (a) 已释放：整族都必须在 nullptr 上安全返回"空"。
  idl.ref();
  idl.unref();
  check(idl.has_ref() == 0, "已释放 has_ref() == 0");
  check(idl.is_active() == 0, "已释放 is_active() == 0");
  check(idl.is_closing() == 0, "已释放 is_closing() == 0");
  check(idl.handle_size() == 0, "已释放 handle_size() == 0");
  uv_os_sock_t sock = 0;
  check(idl.fileno(sock) == UV_EBADF, "已释放 fileno() == UV_EBADF");
#if UV_VERSION_MAJOR >= 1 && UV_VERSION_MINOR >= 18
  check(idl.handle_get_type() == UV_UNKNOWN_HANDLE,
        "已释放 handle_get_type() == UV_UNKNOWN_HANDLE");
  check(idl.handle_type_name() == nullptr, "已释放 handle_type_name() == nullptr");
  check(idl.handle_get_data() == nullptr, "已释放 handle_get_data() == nullptr");
  check(idl.handle_get_loop() == nullptr, "已释放 handle_get_loop() == nullptr");
  idl.handle_set_data(nullptr);
#endif
  // 同一族里原先最不一致的一对：无参版判空、带回调版不判。
  idl.close();
  idl.close([](uvcpp_handle*) {});

  // 静态重载走的是另一份实现（`uvcpp_handle::ref(uvcpp_handle*)` 等），
  // 同一个判据要各自钉一遍。
  uvcpp_handle::ref(&idl);
  uvcpp_handle::unref(&idl);
  check(uvcpp_handle::has_ref(&idl) == 0, "静态 has_ref() == 0");
  check(uvcpp_handle::is_active(&idl) == 0, "静态 is_active() == 0");
  check(uvcpp_handle::is_closing(&idl) == 0, "静态 is_closing() == 0");
  check(uvcpp_handle::handle_size(&idl) == 0, "静态 handle_size() == 0");
  uv_os_sock_t sock2 = 0;
  check(uvcpp_handle::fileno(&idl, sock2) == UV_EBADF,
        "静态 fileno() == UV_EBADF");
  uvcpp_handle::close(&idl, [](uvcpp_handle*) {});
#if UV_VERSION_MAJOR >= 1 && UV_VERSION_MINOR >= 18
  check(uvcpp_handle::handle_get_type(&idl) == UV_UNKNOWN_HANDLE,
        "静态 handle_get_type() == UV_UNKNOWN_HANDLE");
  check(uvcpp_handle::handle_type_name(&idl) == nullptr,
        "静态 handle_type_name() == nullptr");
  check(uvcpp_handle::handle_get_data(&idl) == nullptr,
        "静态 handle_get_data() == nullptr");
  check(uvcpp_handle::handle_get_loop(&idl) == nullptr,
        "静态 handle_get_loop() == nullptr");
  uvcpp_handle::handle_set_data(&idl, nullptr);
#endif

  // 空指针本身也不能崩（同一族的边界）。
  uvcpp_handle::ref(nullptr);
  uvcpp_handle::unref(nullptr);
  check(uvcpp_handle::has_ref(nullptr) == 0, "nullptr has_ref() == 0");
  check(uvcpp_handle::is_active(nullptr) == 0, "nullptr is_active() == 0");
  check(uvcpp_handle::is_closing(nullptr) == 0, "nullptr is_closing() == 0");
  check(uvcpp_handle::handle_size(nullptr) == 0, "nullptr handle_size() == 0");
  uvcpp_handle::close(nullptr, [](uvcpp_handle*) {});
}

// =========================================================================
// 6. 初始化过、但从未启动的句柄：析构必须把它从 loop 的句柄队列上摘下来
// =========================================================================
//
// `free_handle()` 原来把"既不在跑也没在关"一律当成"这块内存没人要了"，
// 直接还给分配器。但那一个分支里其实是两种东西：
//
//   (a) `uv_*_init` **过**的句柄 —— 底层是活着的 libuv 句柄，而且
//       `uv__handle_init` 已经把它链进了 `loop->handle_queue`。仅有
//       `uv_close` 会摘链。直接 free 的结果是队列里那一格指向已释放内存。
//   (b) 从来没 init 过的裸缓冲 —— `loop` 还是空，free 才对。
//
// 判据用 libuv 自己的话来讲最准：`uv_loop_close()` 会遍历 handle_queue，
// 只要还挂着一个非 internal 的句柄就返回 UV_EBUSY。也就是说 ——
// **队列没摘干净，这个循环就永远关不掉**。这比"有没有崩"稳得多：
// 悬垂的那一格在 Release 堆上通常还留着原字节，队列本身仍然自洽可走。
void test_inited_but_never_started_handle() {
  std::cout << "[handle_close] inited_but_never_started_handle" << std::endl;

  uvcpp_loop loop;

  uvcpp_test::loop_drain drain_loop(&loop);
  check(loop.init() == 0, "loop.init()");

  {
    // 只构造、不 start：一个"初始化过但从未启动"的句柄。
    // uvcpp_idle 的构造函数会 uvcpp_alloc + set_handle(owned) + uv_idle_init，
    // 析构函数是空的 —— 于是它正好落在 free_handle() 的第三个分支上。
    uvcpp_idle idl(&loop);
    check(idl.get_handle() != nullptr, "前提：句柄非空");
    check(idl.is_active() == 0, "前提：从未 start，所以不 active");
    check(idl.is_closing() == 0, "前提：也没在关 —— 落进第三个分支");
  }  // ← 析构

  // uv_close 是异步的：完成回调（callback_close，负责还底层内存）要靠
  // 一次循环迭代驱动。两轮保险 —— 第一轮跑完成回调，第二轮确认没有残留。
  loop.run(UV_RUN_NOWAIT);
  loop.run(UV_RUN_NOWAIT);

  const int rc = loop.loop_close();
  check(rc == 0,
        "loop 关不掉：句柄没从 handle_queue 上摘下来（uv_loop_close != 0）");
  if (rc != 0) {
    std::cerr << "      uv_loop_close rc=" << rc
              << "（UV_EBUSY 表示队列里还挂着句柄）" << std::endl;
  }
}

}  // namespace

int main() {
  test_delete_wrapper_in_close_callback();
  test_close_callback_fires_once();
  test_multiple_self_deleting_handles();
  test_close_without_callback();
  test_null_handle_guards();
  test_inited_but_never_started_handle();

  if (g_failures == 0) {
    std::cout << "[handle_close] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[handle_close] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}
