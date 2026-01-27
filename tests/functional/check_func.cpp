#include <iostream>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_check.h"
#include "handle/uvcpp_idle.h"
#include <atomic>

using namespace uvcpp;

int main() {
  std::cout << "[functional check] start\n";
  uvcpp_loop loop;
  loop.init();

  uvcpp_check chk(&loop);
  uvcpp_idle idl(&loop);

  std::atomic<int> idle_count(0);
  std::atomic<int> result(2);

  // idle increments
  idl.start([&](uvcpp_idle* i) {
    int c = ++idle_count;
    std::cout << "[functional check] idle callback " << c << std::endl;
  });

  // check observes when timer finished and validates idle count
  chk.start([&](uvcpp_check* c) {
    int ic = idle_count.load();
    std::cout << "[functional check] check validating idle_count=" << ic
              << std::endl;
    if (ic == 1) {
      std::cout << "[functional check] success\n";
      result.store(0);
    } else {
      std::cout << "[functional check] failed (idle_count != 3)\n";
      result.store(2);
    }
    // cleanup
    c->stop();
    c->close();
    idl.stop();
    loop.stop();
  });

  loop.run(UV_RUN_DEFAULT);
  std::cout << "[functional check] done\n";
  return result.load();
}


