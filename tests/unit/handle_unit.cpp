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

int main() {
  std::cout << "[unit][handle] start\n";
  try {
    uvcpp_loop loop;
    loop.init();

    // create-by-value tests
    uvcpp_timer timer(&loop); timer.init();
    uvcpp_tcp tcp(&loop); tcp.init();
    uvcpp_udp udp(&loop); udp.init();
    uvcpp_pipe pipe(&loop, true); pipe.init();
    uvcpp_stream stream; stream.init();
    uvcpp_idle idle(&loop); idle.init();
    uvcpp_prepare prepare(&loop); prepare.init();
    uvcpp_check check(&loop); check.init();
    uvcpp_async async(&loop); async.init();
    uvcpp_fs_event fs_event(&loop); fs_event.init();
    uvcpp_fs_poll fs_poll(&loop); fs_poll.init();
    uvcpp_poll poll(&loop, 0); poll.init();
    uvcpp_signal signal(&loop); signal.init();
    uvcpp_tty tty(&loop, 0, 0); tty.init();
    uvcpp_process process(&loop); process.init();

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