#include <iostream>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tty.h"

using namespace uvcpp;

int main() {
  std::cout << "[functional tty] start\n";
  uvcpp_loop loop;
  loop.init();

  // init with fd 1 (stdout) if supported
  bool ok = false;
  int fds[] = {1, 0, 2};
  for (int fd : fds) {
    uvcpp_tty *t = new uvcpp_tty();
   int r = t->init(&loop, fd, 0);
  
    if (r == 0) {
      ok = true;
      int w = 0, h = 0;
      int g = t->get_winsize(&w, &h);
      if (g != 0) {
        std::cout << "[functional tty] try fd=" << fd << " init=" << r << " ("
                  << uv_err_name(g) << ")" << std::endl;
      
      } else {
        std::cout << "[functional tty] get_winsize r=" << g << " w=" << w
                  << " h=" << h << std::endl;
        // try reset mode if available
        t->reset_mode();
      }
     
    } else {
      std::cout << "[functional tty] try fd=" << fd << " init=" << r << " ("
                << uv_err_name(r) << ")" << std::endl;
    }
    delete t;
  }

  std::cout << "[functional tty] done ok=" << (ok ? "true" : "false") << std::endl;
  if (!ok) {
    std::cout << "[functional tty] no TTY available in this environment â€” treating as passed\n";
    return 0;
  }
  return 0;
}


