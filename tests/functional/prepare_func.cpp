#include <iostream>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_prepare.h"
#include "handle/uvcpp_idle.h"

using namespace uvcpp;

int main() {
  std::cout << "[functional prepare] start\n";
  uvcpp_loop loop;
  loop.init();

  uvcpp_prepare prep(&loop);
  uvcpp_idle idle(&loop);
  idle.start([&](uvcpp_idle* i) {});

  int count = 0;
  prep.start([&](uvcpp_prepare* p){
    std::cout << "[functional prepare] callback " << ++count << std::endl;
    if (count >= 3) {
      p->stop();
      p->close();
      loop.stop();
    }
  });

  loop.run(UV_RUN_DEFAULT);
  std::cout << "[functional prepare] done\n";
  return 0;
}


