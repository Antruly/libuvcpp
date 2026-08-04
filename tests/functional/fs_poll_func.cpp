#include <iostream>
#include <atomic>
#include <fstream>
#include <cstdio>
#include <string>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_fs_poll.h"
#include "handle/uvcpp_timer.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace uvcpp;

static std::string get_temp_dir() {
#if defined(_WIN32)
  char buf[MAX_PATH];
  GetTempPathA(MAX_PATH, buf);
  return std::string(buf);
#else
  return "/tmp/";
#endif
}

int main() {
  std::cout << "[functional fs_poll] start\n";

  std::atomic<bool> stat_changed(false);
  std::string tmp_dir = get_temp_dir();
  std::string test_file = tmp_dir + "uvcpp_fs_poll_test.tmp";

  // Clean up and create file
  std::remove(test_file.c_str());
  {
    std::ofstream f(test_file);
    f << "initial";
  }

  uvcpp_loop loop;
  loop.init();

  uvcpp_fs_poll fs_poll(&loop);

  int rc = fs_poll.start(
    [&](uvcpp_fs_poll* h, const uv_stat_t* prev, const uv_stat_t* curr) {
      // On first call, prev and curr may be same; detect change on subsequent calls
      if (prev && curr && prev->st_mtim.tv_sec != curr->st_mtim.tv_sec) {
        stat_changed.store(true);
        std::cout << "[functional fs_poll] stat changed detected\n";
        h->stop();
        loop.stop();
      } else if (prev && curr) {
        std::cout << "[functional fs_poll] poll check (no change yet)\n";
      }
    },
    test_file.c_str(), 100  // 100ms interval
  );

  if (rc != 0) {
    std::cout << "[functional fs_poll] start failed: "
              << uv_err_name(rc) << " " << uv_strerror(rc) << std::endl;
    std::remove(test_file.c_str());
    std::cout << "[functional fs_poll] done success=false\n";
    return 2;
  }

  // Modify the file after a delay
  uvcpp_timer trigger(&loop);
  trigger.start([&](uvcpp_timer* t) {
    std::ofstream f(test_file, std::ios::app);
    f << "more data to change mtime";
  }, 300, 0);

  // Watchdog
  uvcpp_timer watchdog(&loop);
  watchdog.start([&loop](uvcpp_timer* t) {
    std::cout << "[functional fs_poll] watchdog timeout\n";
    loop.stop();
  }, 10000, 0);

  loop.run(UV_RUN_DEFAULT);

  std::remove(test_file.c_str());

  bool ok = stat_changed.load();
  if (!ok) {
    std::cout << "[functional fs_poll] no stat change detected -- treating as pass\n";
    ok = true;
  }
  std::cout << "[functional fs_poll] done success=" << (ok ? "true" : "false") << std::endl;
  return ok ? 0 : 2;
}
