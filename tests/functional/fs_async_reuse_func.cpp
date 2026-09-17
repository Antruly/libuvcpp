// Regression test for the uvcpp_fs async re-submission fix (2026-08).
//
// Two defects in uvcpp_fs async (callback) ops were fixed together:
//   1. Re-submitting an async op on the SAME uvcpp_fs object while a prior op
//      is still in flight used to re-post the same embedded uv_fs_t
//      work_req.wq queue node into libuv's thread pool, corrupting the queue
//      (lost work items -> active_reqs imbalance -> uv_run spins at 100% CPU).
//      Now a second submit is rejected with UV_EALREADY (async_pending_ guard).
//   2. The callbacks never called req_cleanup(), leaking the uv_fs_t path buffer
//      and leaving the request dirty. Now every callback auto-cleans up, so a
//      single uvcpp_fs object can be reused across a sequence of async ops.
//
// This test:
//   - Reuses ONE uvcpp_fs across open/close/open/close/unlink and expects every
//     callback to fire (no spin). A watchdog timer fails the test if the loop
//     spins (the original regression symptom).
//   - Submits two async stats back-to-back on one uvcpp_fs and asserts the
//     second returns UV_EALREADY.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <iostream>

#include <uv.h>

#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_timer.h"
#include "req/uvcpp_fs.h"
#include "loop_drain.h"

using namespace uvcpp;

int main() {
  std::cout << "[fs async reuse] start\n";

  const char* fname = "uvcpp_fs_reuse_test.txt";
  {
    FILE* f = std::fopen(fname, "wb");
    if (!f) {
      std::cerr << "[fs async reuse] cannot create temp file\n";
      return 1;
    }
    std::fputs("reuse", f);
    std::fclose(f);
  }

  // ---- scenario 1: sequential reuse of ONE fs object ----
  {
    uvcpp_loop loop;

    uvcpp_test::loop_drain drain_loop(&loop);
    loop.init();

    std::promise<int> done;
    auto done_future = done.get_future();
    std::atomic<bool> timed_out(false);

    uvcpp_timer watchdog(&loop);
    watchdog.start(
        [&](uvcpp_timer* t) {
          timed_out.store(true);
          std::cerr
              << "[fs async reuse] WATCHDOG TIMEOUT (uv_run spin regression)\n";
          loop.stop();
        },
        10000, 0);

    uvcpp_fs fs;  // one object reused for the whole chain

    // open(write) -> close -> open(read) -> close -> unlink
    int rc = fs.open(
        &loop, fname, O_WRONLY, 0,
        [&](uvcpp_fs* req) {
          if (req->get_result() < 0) {
            done.set_value(2);
            loop.stop();
            return;
          }
          uv_file fd = (uv_file)req->get_result();
          fs.close(&loop, fd, [&](uvcpp_fs* creq) {
            fs.open(&loop, fname, O_RDONLY, 0, [&](uvcpp_fs* o2) {
              if (o2->get_result() < 0) {
                done.set_value(3);
                loop.stop();
                return;
              }
              uv_file fd2 = (uv_file)o2->get_result();
              fs.close(&loop, fd2, [&](uvcpp_fs* c2) {
                fs.unlink(&loop, fname, [&](uvcpp_fs* u) {
                  done.set_value(u->get_result() < 0 ? 4 : 0);
                  loop.stop();
                });
              });
            });
          });
        });

    if (rc < 0) {
      std::cerr << "[fs async reuse] open schedule failed rc=" << rc << "\n";
      return 1;
    }

    loop.run(UV_RUN_DEFAULT);

    // Never call get() on an unfulfilled promise (watchdog path leaves it unset).
    if (done_future.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready) {
      std::cerr << "[fs async reuse] reuse chain did not complete (timed_out="
                << (timed_out.load() ? "true" : "false") << ")\n";
      return 2;
    }
    int reuse_rc = done_future.get();
    std::cout << "[fs async reuse] reuse_rc=" << reuse_rc
              << " timed_out=" << (timed_out.load() ? "true" : "false") << "\n";
    if (reuse_rc != 0 || timed_out.load()) {
      std::cerr << "[fs async reuse] reuse chain FAILED\n";
      return 2;
    }
  }

  // ---- scenario 2: guard rejects re-submit while in flight ----
  {
    uvcpp_loop loop;

    uvcpp_test::loop_drain drain_loop(&loop);
    loop.init();

    uvcpp_fs fs;
    std::promise<int> gdone;
    auto gdone_future = gdone.get_future();

    int r1 = fs.stat(&loop, ".", [&](uvcpp_fs* req) {
      gdone.set_value(static_cast<int>(req->get_result()));
    });
    // second async op on the same fs object while the first is in flight
    int r2 = fs.stat(&loop, ".", [](uvcpp_fs* req) { (void)req; });

    loop.run(UV_RUN_DEFAULT);
    int stat_rc = gdone_future.get();

    bool guard_ok = (r2 == UV_EALREADY);
    std::cout << "[fs async reuse] guard r1=" << r1 << " r2=" << r2
              << " stat_rc=" << stat_rc
              << " ealready=" << (guard_ok ? "true" : "false") << "\n";
    if (!guard_ok) {
      std::cerr << "[fs async reuse] guard FAILED (expected r2 == UV_EALREADY)\n";
      return 3;
    }
  }

  // best-effort cleanup if a failure path left the temp file behind
  std::remove(fname);

  std::cout << "[fs async reuse] done success=true\n";
  return 0;
}
