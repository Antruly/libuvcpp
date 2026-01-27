#include <iostream>
#include <vector>
#include <string>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_process.h"
#include <future>

using namespace uvcpp;

int main() {
  std::cout << "[functional process] start\n";
  uvcpp_loop loop;
  loop.init();

  // This functional test spawns a short-lived child process and verifies the exit callback.
  // Cross-platform: on POSIX use /bin/sh -c "exit 0"; on Windows use cmd /C exit 0.
  uvcpp_process proc(&loop);

  std::promise<int> exit_promise;
  auto exit_future = exit_promise.get_future();

#if defined(_WIN32)
  const char* file = "cmd";
  const char* args_arr[] = { "cmd", "/C", "exit", "0", nullptr };
#else
  const char* file = "/bin/sh";
  const char* args_arr[] = { "/bin/sh", "-c", "exit 0", nullptr };
#endif

  // set options and start process
  std::cout << "[functional process] setting options\n";
  proc.set_options(file, args_arr, nullptr, nullptr, 0);
  std::cout << "[functional process] starting process\n";
  int rc = proc.start([&exit_promise](uvcpp_process* p, int64_t exit_status, int term_signal){
    std::cout << "[functional process] exit_status=" << exit_status
              << " term_signal=" << term_signal << std::endl;
    exit_promise.set_value((int)exit_status);
  });
  std::cout << "[functional process] start rc=" << rc << std::endl;

  // run loop until child exits
  loop.run(UV_RUN_DEFAULT);
  int status = exit_future.get();
  std::cout << "[functional process] done status=" << status << std::endl;
  return status == 0 ? 0 : 2;
}


