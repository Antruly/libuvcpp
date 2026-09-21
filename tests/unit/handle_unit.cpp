// duplicate placeholder to ensure glob removal safe (no-op)

#include <iostream>
#include <string>
#include <type_traits>
#include <vector>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_timer.h"
#include "handle/uvcpp_tcp.h"
#include "handle/uvcpp_udp.h"
#include "handle/uvcpp_pipe.h"
#include "handle/uvcpp_stream.h"
#include "handle/uvcpp_idle.h"
#include "handle/uvcpp_prepare.h"
#include "handle/uvcpp_check.h"
#include "handle/uvcpp_async.h"
#include "handle/uvcpp_fs_event.h"
#include "handle/uvcpp_fs_poll.h"
#include "handle/uvcpp_poll.h"
#include "handle/uvcpp_signal.h"
#include "handle/uvcpp_tty.h"
#include "handle/uvcpp_process.h"
#include "../functional/loop_drain.h"
#include "uvcpp/uvcpp_buf.h"

using namespace uvcpp;

// ---------------------------------------------------------------------------
// 拷贝语义（编译期判据）
// ---------------------------------------------------------------------------
// 句柄**不可拷贝**，基类与 16 个派生类一致。
//
// 这不是风格洁癖：`uv_handle_t` 不是一个值，它是挂在某个 `uv_loop_t` 的
// `handle_queue` 上的一个节点。`memcpy` 一个**活着的**它，既得不到合法句柄，
// 也分不出该由谁来还那块内存 —— 不还就是纯泄漏（改动前的实况），还了则会走
// `uv_close`，顺着**源句柄邻居**的地址把**还活着的源句柄**从它自己的队列上摘掉。
// 整段推理见 doc/lowlevel-guide.md §10。
//
// 这两条在改动前的树上是**编译失败**的（基类当时还是可拷贝的），所以判据有牙。
#define UVCPP_ASSERT_NOT_COPYABLE(T)                   \
  static_assert(!std::is_copy_constructible<T>::value, \
                #T " 不应可拷贝构造");                 \
  static_assert(!std::is_copy_assignable<T>::value,    \
                #T " 不应可拷贝赋值")

UVCPP_ASSERT_NOT_COPYABLE(uvcpp_handle);

UVCPP_ASSERT_NOT_COPYABLE(uvcpp_loop);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_timer);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_tcp);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_udp);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_pipe);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_stream);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_idle);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_prepare);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_check);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_async);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_fs_event);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_fs_poll);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_poll);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_signal);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_tty);
UVCPP_ASSERT_NOT_COPYABLE(uvcpp_process);

#undef UVCPP_ASSERT_NOT_COPYABLE

// 正控：判据对**可拷贝**的类型必须答 true。少了这两条，上面那一片"全绿"也可能
// 只是因为这个谓词对什么都答 false —— 那就等于一条判据都没有。
static_assert(std::is_copy_constructible<std::string>::value,
              "正控：std::string 可拷贝构造");
static_assert(std::is_copy_constructible<uvcpp_buf>::value,
              "正控：uvcpp_buf 可拷贝构造");

#define TRY(expr, name) \
  try { expr; std::cout << "  OK: " << name << std::endl; } \
  catch(const std::exception &ex) { \
    std::cerr << "  FAIL: " << name << " - " << ex.what() << std::endl; \
    return 2; \
  }

int main() {
  std::cout << "[unit][handle] start\n";
  try {
    std::cout << "constructing loop..." << std::endl;
    uvcpp_loop loop;

    uvcpp_test::loop_drain drain_loop(&loop);
    std::cout << "loop.init..." << std::endl;
    loop.init();
    std::cout << "  OK: loop" << std::endl;

    // create-by-value tests
    TRY(uvcpp_timer timer(&loop); timer.init();, "timer")
    TRY(uvcpp_tcp tcp(&loop); tcp.init();, "tcp")
    TRY(uvcpp_udp udp(&loop); udp.init();, "udp")
    TRY(uvcpp_pipe pipe(&loop, true); pipe.init();, "pipe")
    TRY(uvcpp_stream stream; stream.init();, "stream")
    TRY(uvcpp_idle idle(&loop); idle.init();, "idle")
    TRY(uvcpp_prepare prepare(&loop); prepare.init();, "prepare")
    TRY(uvcpp_check check(&loop); check.init();, "check")
    TRY(uvcpp_async async(&loop); async.init();, "async")
    TRY(uvcpp_fs_event fs_event(&loop); fs_event.init();, "fs_event")
    TRY(uvcpp_fs_poll fs_poll(&loop); fs_poll.init();, "fs_poll")
    TRY(uvcpp_poll poll(&loop, 0); poll.init();, "poll")
    TRY(uvcpp_signal signal(&loop); signal.init();, "signal")
    TRY(uvcpp_tty tty(&loop, 0, 0); tty.init();, "tty")
    TRY(uvcpp_process process(&loop); process.init();, "process")

    // heap allocation tests
    auto *p = new uvcpp_timer(&loop);
    delete p;

    std::cout << "[unit][handle] done\n";
    return 0;
  } catch(const std::exception &ex) {
    std::cerr << "exception: " << ex.what() << std::endl;
    return 2;
  }
}

// end of file
