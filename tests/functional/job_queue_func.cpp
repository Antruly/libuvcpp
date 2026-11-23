/**
 * @file tests/functional/job_queue_func.cpp
 * @brief 作业队列：执行、并发键、重试退避、超时、取消、队列满、重启恢复。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_job_queue.h>

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "  [FAIL] " << what << std::endl;
  }
}

bool pump(uvcpp_loop& loop, const std::function<bool()>& pred, int ms) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  // 用 NOWAIT + 1ms 睡眠：作业在 worker 上跑时循环里可能**没有任何事件**，
  // `UV_RUN_ONCE` 会一直阻塞到 after_work（长任务就是永远），主线程看不到
  // worker 侧的进展。NOWAIT 轮询保证谓词（往往是原子量）能及时被观察到。
  while (!pred() && std::chrono::steady_clock::now() < deadline) {
    loop.run(UV_RUN_NOWAIT);
    if (!pred()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 5; ++i) loop.run(UV_RUN_NOWAIT);
  return pred();
}

void test_basic() {
  std::cout << "-- 执行 / 状态 / 统计 --" << std::endl;
  uvcpp_loop loop;
  std::atomic<int> runs(0);
  {
    uvcpp_job_queue q(&loop);
    q.register_handler("ok", [&](uvcpp_job_context&) { ++runs; });
    q.set_max_concurrent(2);
    q.start();
    std::string err;
    for (int i = 0; i < 3; ++i) {
      uvcpp_job_submit s;
      s.type = "ok";
      check(q.submit(s, &err) != 0, "提交成功");
    }
    check(pump(loop, [&] { return runs.load() == 3; }, 3000), "三个都跑了");
    check(pump(loop, [&] { return q.stats().success == 3; }, 3000),
          "stats.success = 3");
    check(q.stats().pending == 0 && q.stats().running == 0, "队列清空");
    check(q.clear_finished() == 3, "clear_finished 清掉 3 条");
    check(q.stats().success == 0, "清掉后统计归零");
  }
}

void test_concurrency_key() {
  std::cout << "-- concurrency_key 串行 --" << std::endl;
  uvcpp_loop loop;
  std::atomic<int> active(0);
  std::atomic<int> max_active(0);
  std::atomic<int> done(0);
  {
    uvcpp_job_queue q(&loop);
    q.set_max_concurrent(4);
    q.register_handler("work", [&](uvcpp_job_context&) {
      const int a = active.fetch_add(1) + 1;
      int m = max_active.load();
      while (a > m && !max_active.compare_exchange_weak(m, a)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      active.fetch_sub(1);
      ++done;
    });
    q.start();
    for (int i = 0; i < 5; ++i) {
      uvcpp_job_submit s;
      s.type            = "work";
      s.concurrency_key = "k";
      q.submit(s);
    }
    check(pump(loop, [&] { return done.load() == 5; }, 5000), "五个都完成");
    check(max_active.load() == 1, "同一 key 从不同时执行（实际最大 " +
                                      std::to_string(max_active.load()) + "）");
  }
}

void test_retry_and_fail() {
  std::cout << "-- 重试 / 耗尽 --" << std::endl;
  uvcpp_loop loop;
  std::atomic<int> attempts(0);
  std::atomic<int> good_done(0);
  {
    uvcpp_job_queue q(&loop);
    q.register_handler("flaky", [&](uvcpp_job_context& ctx) {
      ++attempts;
      if (ctx.job().attempt < 3) throw std::runtime_error("boom");
    });
    q.register_handler("always_fail", [](uvcpp_job_context&) {
      throw std::runtime_error("nope");
    });
    q.start();

    uvcpp_job_submit s;
    s.type        = "flaky";
    s.max_retry   = 2;
    s.backoff_ms  = 10;
    s.timeout_ms  = 0;
    const uint64_t id = q.submit(s);
    check(pump(loop, [&] {
            const uvcpp_job_record* r = q.find(id);
            return r != nullptr &&
                   r->state == static_cast<int>(uvcpp_job_state::SUCCESS);
          }, 5000),
          "失败两次后第三次成功");
    check(q.find(id)->attempt == 3, "attempt = 3");
    check(q.stats().retried == 2, "retried = 2");
    (void)good_done;

    uvcpp_job_submit f;
    f.type      = "always_fail";
    f.max_retry = 1;
    f.backoff_ms = 10;
    const uint64_t fid = q.submit(f);
    check(pump(loop, [&] {
            const uvcpp_job_record* r = q.find(fid);
            return r != nullptr &&
                   r->state == static_cast<int>(uvcpp_job_state::FAILED);
          }, 5000),
          "重试耗尽后 FAILED");
    check(q.find(fid)->attempt == 2, "失败作业尝试了 2 次");
  }
}

/// 异步 handler 用：`delay_ms` 之后调一次 `done(ok, error)`。
void schedule_done(uvcpp_loop& loop, int delay_ms, bool ok,
                   const std::string& error, uvcpp_job_done done) {
  uv_timer_t* t = new uv_timer_t();
  uv_timer_init(reinterpret_cast<uv_loop_t*>(loop.get_handle()), t);
  struct payload {
    uvcpp_job_done cb;
    bool           ok;
    std::string    error;
  };
  payload* p = new payload();
  p->cb      = done;
  p->ok      = ok;
  p->error   = error;
  t->data    = p;
  uv_timer_start(
      t,
      [](uv_timer_t* tt) {
        payload* pp = static_cast<payload*>(tt->data);
        tt->data    = nullptr;
        uvcpp_job_done cb = pp->cb;
        const bool ok      = pp->ok;
        const std::string err = pp->error;
        delete pp;
        uv_close(reinterpret_cast<uv_handle_t*>(tt),
                 [](uv_handle_t* h) {
                   delete reinterpret_cast<uv_timer_t*>(h);
                 });
        cb(ok, err);
      },
      static_cast<uint64_t>(delay_ms), 0);
}

void test_async_handlers() {
  std::cout << "-- 异步 handler --" << std::endl;

  // (1) 成功：done 之后 ctx 失效但对象还在。
  {
    uvcpp_loop loop;
    std::shared_ptr<uvcpp_job_context> seen;
    {
      uvcpp_job_queue q(&loop);
      q.register_async_handler(
          "async.ok", [&](std::shared_ptr<uvcpp_job_context> ctx,
                          uvcpp_job_done done) {
            seen = ctx;
            schedule_done(loop, 30, true, "", done);
          });
      q.start();
      uvcpp_job_submit req;
      req.type       = "async.ok";
      req.timeout_ms = 5000;
      std::string err;
      const uint64_t id = q.submit(req, &err);
      check(id != 0, "异步作业 submit");
      check(pump(loop,
                 [&] {
                   const uvcpp_job_record* r = q.find(id);
                   return r != nullptr &&
                          r->state ==
                              static_cast<int>(uvcpp_job_state::SUCCESS);
                 },
                 5000),
            "异步 handler 调 done(true) 后作业成功");
      check(q.stats().running == 0, "running 归零");
      check(seen && !seen->cancelled(), "done 之后 ctx 仍可安全读");
      q.stop();
    }
  }

  // (2) 失败一次后重试成功。
  {
    uvcpp_loop loop;
    std::atomic<int> attempts(0);
    {
      uvcpp_job_queue q(&loop);
      q.register_async_handler(
          "async.flaky", [&](std::shared_ptr<uvcpp_job_context>,
                             uvcpp_job_done done) {
            const int n = ++attempts;
            schedule_done(loop, 10, n > 1, n > 1 ? "" : "first-fail", done);
          });
      q.start();
      uvcpp_job_submit req;
      req.type       = "async.flaky";
      req.max_retry  = 2;
      req.backoff_ms = 10;
      req.timeout_ms = 5000;
      const uint64_t id = q.submit(req);
      check(id != 0, "异步重试作业 submit");
      check(pump(loop,
                 [&] {
                   const uvcpp_job_record* r = q.find(id);
                   return r != nullptr &&
                          r->state ==
                              static_cast<int>(uvcpp_job_state::SUCCESS);
                 },
                 5000),
            "第二次尝试成功");
      check(attempts.load() == 2, "handler 跑了两次");
      check(q.stats().retried >= 1, "retried 计数涨");
      q.stop();
    }
  }

  // (3) 运行中取消：handler 稍后 done(true)，队列仍判 CANCELLED。
  {
    uvcpp_loop loop;
    std::atomic<bool> started(false);
    {
      uvcpp_job_queue q(&loop);
      q.register_async_handler(
          "async.cancel", [&](std::shared_ptr<uvcpp_job_context>,
                              uvcpp_job_done done) {
            started.store(true);
            schedule_done(loop, 200, true, "", done);
          });
      q.start();
      uvcpp_job_submit req;
      req.type       = "async.cancel";
      req.timeout_ms = 5000;
      const uint64_t id = q.submit(req);
      check(id != 0, "异步取消作业 submit");
      check(pump(loop, [&] { return started.load(); }, 3000), "handler 已开始");
      check(q.cancel(id) == 0, "cancel 成功");
      check(pump(loop,
                 [&] { return q.stats().cancelled >= 1; }, 3000),
            "done 之后归为 CANCELLED");
      const uvcpp_job_record* r = q.find(id);
      check(r != nullptr &&
                r->state == static_cast<int>(uvcpp_job_state::CANCELLED),
            "记录状态 CANCELLED");
      q.stop();
    }
  }

  // (4) 超时：handler 拖到超时之后才 done，结果按 timeout 落地。
  {
    uvcpp_loop loop;
    std::atomic<bool> started(false);
    {
      uvcpp_job_queue q(&loop);
      q.register_async_handler(
          "async.timeout", [&](std::shared_ptr<uvcpp_job_context>,
                               uvcpp_job_done done) {
            started.store(true);
            schedule_done(loop, 300, true, "", done);
          });
      q.start();
      uvcpp_job_submit req;
      req.type       = "async.timeout";
      req.timeout_ms = 50;
      req.max_retry  = 0;
      const uint64_t id = q.submit(req);
      check(id != 0, "异步超时作业 submit");
      check(pump(loop, [&] { return started.load(); }, 3000), "handler 已开始");
      check(pump(loop,
                 [&] {
                   const uvcpp_job_record* r = q.find(id);
                   return r != nullptr &&
                          r->state ==
                              static_cast<int>(uvcpp_job_state::FAILED);
                 },
                 5000),
            "超时后落地 FAILED");
      const uvcpp_job_record* r = q.find(id);
      check(r != nullptr && r->error == "timeout", "错误是 timeout");
      check(q.stats().failed >= 1, "stats.failed 计数涨");
      q.stop();
    }
  }
}

void test_cancel_and_timeout() {
  std::cout << "-- 取消 / 超时 --" << std::endl;
  uvcpp_loop loop;
  std::atomic<int> slow_started(0);
  std::atomic<int> victim_ran(0);
  {
    uvcpp_job_queue q(&loop);
    q.set_max_concurrent(1);
    std::atomic<bool> block_release(false);
    q.register_handler("block", [&](uvcpp_job_context& ctx) {
      ++slow_started;
      while (!block_release.load() && !ctx.cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    q.register_handler("victim", [&](uvcpp_job_context&) { ++victim_ran; });
    q.register_handler("forever", [](uvcpp_job_context& ctx) {
      while (!ctx.cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    q.register_handler("slow", [](uvcpp_job_context& ctx) {
      while (!ctx.cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    q.start();

    uvcpp_job_submit b; b.type = "block";
    q.submit(b);
    check(pump(loop, [&] { return slow_started.load() == 1; }, 3000),
          "占位任务在跑");

    uvcpp_job_submit v; v.type = "victim";
    const uint64_t vid = q.submit(v);
    check(q.cancel(vid) == 0, "取消 PENDING 作业");
    check(q.find(vid)->state == static_cast<int>(uvcpp_job_state::CANCELLED),
          "PENDING → CANCELLED");

    // 放行占位任务，确认被取消的 victim 也不会再跑（它在 PENDING 时已被摘掉）。
    block_release.store(true);
    check(pump(loop, [&] { return q.stats().running == 0; }, 3000),
          "占位任务结束");

    uvcpp_job_submit f; f.type = "forever";
    const uint64_t fid = q.submit(f);
    check(pump(loop, [&] {
            const uvcpp_job_record* r = q.find(fid);
            return r != nullptr &&
                   r->state == static_cast<int>(uvcpp_job_state::RUNNING);
          }, 3000),
          "长任务开始");
    check(q.cancel(fid) == 0, "取消 RUNNING 作业");
    check(pump(loop, [&] {
            const uvcpp_job_record* r = q.find(fid);
            return r != nullptr &&
                   r->state == static_cast<int>(uvcpp_job_state::CANCELLED);
          }, 3000),
          "RUNNING → CANCELLED（协作式）");

    uvcpp_job_submit t; t.type = "slow"; t.timeout_ms = 30;
    const uint64_t tid = q.submit(t);
    check(pump(loop, [&] {
            const uvcpp_job_record* r = q.find(tid);
            return r != nullptr &&
                   r->state == static_cast<int>(uvcpp_job_state::FAILED);
          }, 3000),
          "超时后 FAILED");
    check(q.find(tid)->error == "timeout", "错误标成 timeout");
    check(q.stats().cancelled >= 2, "cancelled 统计 ≥ 2");
    check(victim_ran.load() == 0, "被取消的 PENDING 任务从未执行");
  }
}

void test_queue_full() {
  std::cout << "-- 队列满 --" << std::endl;
  uvcpp_loop loop;
  std::atomic<int> started(0);
  {
    uvcpp_job_queue q(&loop);
    q.set_max_concurrent(1);
    q.set_max_queue(1);
    std::atomic<bool> hold_release(false);
    q.register_handler("hold", [&](uvcpp_job_context& ctx) {
      ++started;
      while (!hold_release.load() && !ctx.cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    q.start();
    uvcpp_job_submit s; s.type = "hold";
    check(q.submit(s) != 0, "第一个进 RUNNING");
    check(pump(loop, [&] { return started.load() == 1; }, 3000), "它在跑");
    uvcpp_job_submit s2; s2.type = "hold";
    check(q.submit(s2) != 0, "第二个进 PENDING（队列上限 1）");
    uvcpp_job_submit s3; s3.type = "hold";
    std::string err;
    check(q.submit(s3, &err) == 0, "第三个被拒绝");
    check(err.find("满") != std::string::npos, "错误说明队列已满");
    check(q.stats().rejected == 1, "rejected = 1");
    hold_release.store(true);
    pump(loop, [&] { return q.stats().running == 0; }, 3000);
  }
}

void test_recovery() {
  std::cout << "-- 重启恢复 --" << std::endl;
  uvcpp_loop loop;
  std::shared_ptr<uvcpp_memory_job_store> store =
      std::make_shared<uvcpp_memory_job_store>();
  uvcpp_job_record crashed;
  crashed.id      = 7;
  crashed.type    = "recovered";
  crashed.state   = static_cast<int>(uvcpp_job_state::RUNNING);
  crashed.attempt = 1;
  store->save(crashed);

  std::atomic<int> recovered_done(0);
  std::atomic<bool> release(false);
  {
    uvcpp_job_queue q(&loop, store);
    q.set_max_concurrent(1);
    q.register_handler("block", [&](uvcpp_job_context&) {
      while (!release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    q.register_handler("recovered", [&](uvcpp_job_context&) {
      ++recovered_done;
    });
    uvcpp_job_submit b; b.type = "block";
    q.submit(b);
    q.start();
    check(pump(loop, [&] {
            const uvcpp_job_record* r = q.find(7);
            return r != nullptr &&
                   r->state == static_cast<int>(uvcpp_job_state::PENDING);
          }, 3000),
          "RUNNING 记录恢复成 PENDING");
    check(q.find(7)->error.find("recovered") != std::string::npos,
          "记录里标了 recovered after restart");
    release.store(true);
    check(pump(loop, [&] { return recovered_done.load() == 1; }, 3000),
          "恢复后的任务被执行");
    check(q.find(7)->state == static_cast<int>(uvcpp_job_state::SUCCESS),
          "恢复任务成功");
  }
}

}  // namespace

int main() {
  std::cout << "[job_queue] start" << std::endl;
  test_basic();
  test_concurrency_key();
  test_retry_and_fail();
  test_async_handlers();
  test_cancel_and_timeout();
  test_queue_full();
  test_recovery();
  if (g_fail == 0) {
    std::cout << "[job_queue] ALL PASS" << std::endl;
    return 0;
  }
  std::cerr << "[job_queue] " << g_fail << " check(s) failed" << std::endl;
  return 1;
}
