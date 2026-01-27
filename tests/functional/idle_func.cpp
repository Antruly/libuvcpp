#include <iostream>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_idle.h"
#include "handle/uvcpp_timer.h"
#include <atomic>
#include <thread>

using namespace uvcpp;

int main() {
  std::cout << "[functional idle] start\n";
  uvcpp_loop loop;
  loop.init();

  uvcpp_idle idl(&loop);
  uvcpp_timer timer(&loop);

  std::atomic<int> idle_count(0);
  std::atomic<int> timer_count(0);
  std::atomic<int> result(2);

  // idle: count busy-loop invocations (lightweight)
  idl.start([&](uvcpp_idle* i) {
    ++idle_count;
    ::std::this_thread::sleep_for(::std::chrono::milliseconds(1));
  });

  // timer: emit three times, then stop idle and verify counts
  timer.start([&](uvcpp_timer* t) {
    int tc = ++timer_count;
    std::cout << "[functional idle] timer tick " << tc << std::endl;
    if (tc > 0) {
      result.store(0);
      // stop timer and idle, validate
      t->stop();
      t->close();
      // stop prepare/check/idle before closing to avoid races
      idl.stop();
      int ic = idle_count.load();
      std::cout << "[functional idle] timer done, idle_count=" << ic << std::endl;
      // close handles
      loop.stop();
    }
    else {
      result.store(2);
    }
  }, 100, 100);

  loop.run(UV_RUN_DEFAULT);
  std::cout << "[functional idle] done\n";
  return result.load();
}


