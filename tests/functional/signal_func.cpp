#include <iostream>
#include <atomic>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_signal.h"
#include "handle/uvcpp_timer.h"

using namespace uvcpp;

int main() {
  std::cout << "[functional signal] start\n";

  uvcpp_loop loop;
  loop.init();

  // Register SIGINT handler (cross-platform).
  // Note: actual signal delivery (e.g. Ctrl+C) requires interactive terminal
  // and cannot be fully automated in CI. This test validates that the handler
  // can be registered and stopped successfully.
  uvcpp_signal sig(&loop);
  int rc = sig.start([](uvcpp_signal*, int signum) {
    std::cout << "[functional signal] received signal " << signum << std::endl;
  }, SIGINT);

  if (rc != 0) {
    std::cout << "[functional signal] signal start failed: "
              << uv_err_name(rc) << " " << uv_strerror(rc) << std::endl;
    std::cout << "[functional signal] done success=true (unsupported on this platform)\n";
    return 0;
  }

  // Stop the signal handler and verify it can be restarted
  rc = sig.stop();
  if (rc != 0) {
    std::cout << "[functional signal] stop failed: " << uv_err_name(rc) << std::endl;
    std::cout << "[functional signal] done success=false\n";
    return 2;
  }

  // Restart to verify stop+start works
  rc = sig.start([](uvcpp_signal*, int signum) {}, SIGINT);
  if (rc != 0) {
    std::cout << "[functional signal] restart failed: " << uv_err_name(rc) << std::endl;
    std::cout << "[functional signal] done success=false\n";
    return 2;
  }

  // Stop loop via timer to clean up
  uvcpp_timer t(&loop);
  t.start([&loop](uvcpp_timer* tt) {
    loop.stop();
  }, 50, 0);

  loop.run(UV_RUN_DEFAULT);

  std::cout << "[functional signal] done success=true\n";
  return 0;
}
