// duplicate placeholder to ensure glob removal safe (no-op)

#include <iostream>
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

using namespace uvcpp;

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
