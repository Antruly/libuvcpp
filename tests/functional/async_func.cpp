#include <iostream>
#include <thread>
#include <future>
#include <atomic>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_async.h"

using namespace uvcpp;

int main() {
  std::cout << "[functional async] start\n";
  uvcpp_loop loop;
  loop.init();

  uvcpp_async async;

  std::promise<void> done;
  auto done_future = done.get_future();

  std::atomic<int> count{0};
  async.init([&](uvcpp_async * a) {
    int c = ++count;
    std::cout << "[functional async] callback " << c << std::endl;
    if (c >= 3) {
      a->close();
      loop.stop();
      done.set_value();
    }
  }, &loop);

  // send async signals from another thread (use uvcpp_async::send)
  std::thread t([&](){
    for (int i = 0; i < 3; ++i) {
      async.send();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  loop.run(UV_RUN_DEFAULT);

  // wait until callback signals completion
  done_future.get();
  t.join();
  std::cout << "[functional async] done\n";
  return 0;
}


