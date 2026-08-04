#include <iostream>
#include <atomic>
#include <thread>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_timer.h"
#include "req/uvcpp_work.h"

using namespace uvcpp;

int main() {
  std::cout << "[functional work] start\n";

  std::atomic<bool> work_called(false);
  std::atomic<bool> after_work_called(false);
  std::atomic<bool> data_passed(false);

  uvcpp_loop loop;
  loop.init();

  // Create work request with user data
  uvcpp_work* work = new uvcpp_work();
  work->init();
  int test_data = 42;
  work->set_data(&test_data);

  work->queue_work(&loop,
    // work_cb: runs in libuv thread pool worker thread
    [](uvcpp_work* w) {
      int* data = static_cast<int*>(w->get_data());
      if (data && *data == 42) {
        // Simulate some work
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    },
    // after_work_cb: runs on the loop thread
    [&](uvcpp_work* w, int status) {
      after_work_called.store(true);
      int* data = static_cast<int*>(w->get_data());
      if (data && *data == 42) {
        data_passed.store(true);
      }
      std::cout << "[functional work] after_work status=" << status << std::endl;
      delete w;
      loop.stop();
    }
  );

  // Watchdog
  uvcpp_timer watchdog(&loop);
  watchdog.start([&loop](uvcpp_timer* t) {
    std::cout << "[functional work] watchdog timeout\n";
    loop.stop();
  }, 5000, 0);

  loop.run(UV_RUN_DEFAULT);

  bool ok = after_work_called.load() && data_passed.load();
  std::cout << "[functional work] after_work=" << after_work_called.load()
            << " data_passed=" << data_passed.load() << std::endl;
  std::cout << "[functional work] done success=" << (ok ? "true" : "false") << std::endl;
  return ok ? 0 : 2;
}
