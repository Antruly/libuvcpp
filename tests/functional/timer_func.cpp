#include <iostream>
#include <atomic>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_timer.h"

using namespace uvcpp;

int main() {
  std::cout << "[functional timer] start\n";

  std::atomic<bool> single_ok(false);
  std::atomic<int> repeat_count(0);
  std::atomic<bool> repeat_ok(false);
  std::atomic<bool> restart_ok(false);

  uvcpp_loop loop;
  loop.init();

  // Step 1: single-shot timer (50ms)
  uvcpp_timer t1(&loop);
  t1.start([&](uvcpp_timer* t) {
    single_ok.store(true);
    t->stop();

    // Step 2: repeat timer (3 times, 10ms repeat interval)
    uvcpp_timer* t2 = new uvcpp_timer(&loop);
    t2->start([t2, &repeat_count, &repeat_ok, &loop, &restart_ok](uvcpp_timer* t) {
      int c = repeat_count.fetch_add(1) + 1;
      if (c >= 3) {
        repeat_ok.store(true);
        t->stop();

        // Step 3: stop + restart same timer
        uvcpp_timer* t3 = new uvcpp_timer(&loop);
        t3->start([t3, &restart_ok, &loop](uvcpp_timer* tt) {
          restart_ok.store(true);
          tt->stop();
          loop.stop();
          delete t3;
        }, 30, 0);
        t3->stop();
        t3->start([t3, &restart_ok, &loop](uvcpp_timer* tt) {
          restart_ok.store(true);
          tt->stop();
          loop.stop();
          delete t3;
        }, 20, 0);

        delete t2;
      }
    }, 10, 10);

  }, 50, 0);

  // Watchdog
  uvcpp_timer watchdog(&loop);
  watchdog.start([&loop](uvcpp_timer* t) {
    std::cout << "[functional timer] watchdog timeout\n";
    loop.stop();
  }, 5000, 0);

  loop.run(UV_RUN_DEFAULT);

  bool all_ok = single_ok.load() && repeat_ok.load() && restart_ok.load();
  std::cout << "[functional timer] single=" << single_ok.load()
            << " repeat=" << repeat_ok.load()
            << " restart=" << restart_ok.load() << std::endl;
  std::cout << "[functional timer] done success=" << (all_ok ? "true" : "false") << std::endl;
  return all_ok ? 0 : 2;
}
