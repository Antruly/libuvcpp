/**
 * @file tests/functional/web_app_context_func.cpp
 * @brief uvcpp_web_context（链的控制流 / 生命周期）与连接登记表的测试。
 *
 * 不起网络。用一个假的 `uvcpp_web_context_host` 把「loop 线程」和「发送」
 * 换成两个计数器，于是链的每一种结局都能被**精确断言**，不会被端口、时序、
 * TCP 缓冲干扰。真正的 App 只是把这个假实现换成真的（见 Phase 1.8）。
 *
 * 这里的断言按重要性排：
 *
 *  1. **`test_sync_next_is_not_recursive`** —— 5000 环同步链的最大栈深必须是
 *     1。这是 `advance()` 那个「索引 + 重入标志」存在的全部理由；改成递归
 *     调用栈（最自然的写法）这条立刻挂。
 *  2. **`test_async_resume_continues_at_right_index`** —— 异步续跑必须从
 *     `chain_index_` 接着跑，而不是从头。写错的话第 1 环会被重复执行，
 *     表现是「中间件跑了两次」（比如访问日志记两条、鉴权做两遍）。
 *  3. **`test_conn_id_never_resolves_to_reused_address`** —— id 永不复用。
 *     登记表只按 id 查，所以同一个地址被新连接复用**绝不会**让旧 id 又
 *     "活"过来。这是整个设计要挡的那个缺陷。
 *  4. **`test_handler_without_next_or_end_warns_and_sends`** —— handler 忘了
 *     结束响应时请求不能永远挂着。
 */
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_connection.h>
#include <webapp/uvcpp_web_context.h>
#include <webapp/uvcpp_web_middleware.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

// -------------------------------------------------------------------------
// 日志收集（断言 WARN/ERROR 真的打了）
// -------------------------------------------------------------------------

class capturing_sink : public uvcpp_log_sink {
 public:
  void write(const uvcpp_log_record& record) override {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back(record);
  }
  bool message_contains(const std::string& needle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < records_.size(); ++i) {
      if (records_[i].message.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  bool has_level(log_level lv) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < records_.size(); ++i) {
      if (records_[i].level == lv) return true;
    }
    return false;
  }
  size_t count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
  }

 private:
  mutable std::mutex               mutex_;
  std::vector<uvcpp_log_record>    records_;
};

class log_capture_guard {
 public:
  explicit log_capture_guard(capturing_sink* sink) : sink_(sink) {
    uvcpp_logger::instance().set_sink(sink);
    uvcpp_logger::instance().set_level(log_level::TRACE);
  }
  ~log_capture_guard() {
    uvcpp_logger::instance().set_sink(nullptr);
    uvcpp_logger::instance().set_level(log_level::INFO);
  }

 private:
  log_capture_guard(const log_capture_guard&);
  log_capture_guard& operator=(const log_capture_guard&);
  capturing_sink* sink_;
};

// -------------------------------------------------------------------------
// 假的 host
// -------------------------------------------------------------------------

class fake_host : public uvcpp_web_context_host {
 public:
  fake_host()
      : loop_thread_(std::this_thread::get_id()),
        sent_count(0),
        abort_count(0),
        finished_count(0),
        post_count(0),
        last_status(0) {}

  // --- uvcpp_web_context_host ---

  bool on_loop_thread() const override {
    return std::this_thread::get_id() == loop_thread_;
  }

  void post(std::function<void()> fn) override {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(fn);
    ++post_count;
  }

  uvcpp_loop* loop() const override { return nullptr; }

  uvcpp_tcp_client* connection(uvcpp_web_conn_id id) override {
    // 真实实现就是这一句 —— 登记表里没有 = 连接已经断了。
    return registry.client(id);
  }

  void send_response(uvcpp_web_context& ctx) override {
    ++sent_count;
    last_status = ctx.response().status_code();
    const char* p = ctx.response().body_data();
    last_body = (p == nullptr || ctx.response().body_size() == 0)
                    ? std::string()
                    : std::string(p, ctx.response().body_size());
  }

  void abort_request(uvcpp_web_context& ctx) override {
    (void)ctx;
    ++abort_count;
  }

  void context_finished(uvcpp_web_context& ctx) override {
    (void)ctx;
    ++finished_count;
  }

  // --- 测试辅助 ---

  /// 把投递进来的任务跑一遍（模拟 loop 线程的一次迭代）。
  size_t pump() {
    std::deque<std::function<void()> > tasks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks.swap(queue_);
    }
    for (size_t i = 0; i < tasks.size(); ++i) tasks[i]();
    return tasks.size();
  }

  uvcpp_web_connection_registry registry;

  int         sent_count;
  int         abort_count;
  int         finished_count;
  int         post_count;
  int         last_status;
  std::string last_body;

 private:
  std::thread::id                  loop_thread_;
  mutable std::mutex               mutex_;
  std::deque<std::function<void()> > queue_;
};

// -------------------------------------------------------------------------
// 假的 client 指针
//
// 登记表**从不解引用** client，只拿它当身份比较。所以造几个地址就够了 ——
// 比去起一条真连接干净得多，也不会因为时序而 flaky。
// 用真实的对齐缓冲区，避免 reinterpret_cast 一个任意整数。
// -------------------------------------------------------------------------

union fake_client_slot {
  void* align_;
  char  bytes[256];
};

fake_client_slot g_slots[4];

uvcpp_tcp_client* fake_client(int i) {
  return reinterpret_cast<uvcpp_tcp_client*>(g_slots[i].bytes);
}

// -------------------------------------------------------------------------
// 造请求
// -------------------------------------------------------------------------

void make_get(uvcpp_web_request& req, const std::string& path) {
  uvcpp_http_request raw;
  raw.method = http_method::HTTP_GET;
  raw.url    = path;
  req.take_from(raw);
}

/// 造一个已经在 loop 线程上、连接已登记的上下文。
std::shared_ptr<uvcpp_web_context> make_ctx(fake_host& host,
                                            uvcpp_web_conn_id id) {
  return uvcpp_web_context::create(host, id);
}

// =========================================================================
// 1. 顺序与基本收尾
// =========================================================================
void test_linear_chain() {
  std::cout << "[1] 线性链" << std::endl;

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0), "10.0.0.1", 5000);

  std::string order;
  std::vector<uvcpp_web_handler> chain;
  chain.push_back([&order](uvcpp_web_request&, uvcpp_web_response&,
                           uvcpp_web_next next) {
    order += 'a';
    next();
  });
  chain.push_back([&order](uvcpp_web_request&, uvcpp_web_response&,
                           uvcpp_web_next next) {
    order += 'b';
    next();
  });
  chain.push_back([&order](uvcpp_web_request& req, uvcpp_web_response& resp,
                           uvcpp_web_next) {
    order += 'c';
    // 最后一环：末环能看见请求（这里是 ctx 里那份 request 的 path）。
    resp.text(req.path());
    resp.end();
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  uvcpp_http_request raw;
  raw.method = http_method::HTTP_GET;
  raw.url    = "/hello/world";
  ctx->request().take_from(raw);

  check(ctx->chain_size() == 0, "run 之前 chain_size() 是 0");
  ctx->run(chain);

  check(order == "abc", "处理器按注册顺序执行（实际 \"" + order + "\"）");
  check(host.sent_count == 1, "响应只发一次");
  check(host.finished_count == 1, "收尾通知只来一次");
  check(host.abort_count == 0, "没有 abort");
  check(host.last_status == 200, "默认状态码 200");
  check(host.last_body == "/hello/world", "handler 拿到的请求是 ctx 里那一份");
  check(ctx->finished(), "ctx.finished() 为真");
  check(ctx->chain_index() == 3, "chain_index 停在链尾");
  check(ctx->chain_size() == 3, "chain_size 是 3");
  check(!ctx->aborted(), "没 abort");
}

// =========================================================================
// 2. 同步 next 不递归 —— 5000 环最大栈深 1
// =========================================================================
void test_sync_next_is_not_recursive() {
  std::cout << "[2] 同步 next 不递归（5000 环）" << std::endl;

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  const size_t kDepth = 5000;

  int  depth     = 0;
  int  max_depth = 0;
  size_t ran     = 0;

  std::vector<uvcpp_web_handler> chain;
  for (size_t i = 0; i < kDepth; ++i) {
    chain.push_back([&depth, &max_depth, &ran](
        uvcpp_web_request&, uvcpp_web_response& resp, uvcpp_web_next next) {
      ++depth;
      if (depth > max_depth) max_depth = depth;
      ++ran;
      next();
      --depth;
    });
  }
  // 末环结束响应（上面每一环都调了 next，所以最后一个必须收尾，否则会有一条
  // 「没结束响应」的 WARN —— 那不是本用例要测的东西）。
  chain.push_back([](uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next) {
    resp.text("done");
    resp.end();
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);

  check(ran == kDepth, "5000 环全跑了");
  check(max_depth == 1,
        "最大栈深是 1（递归实现会是 5000，实际 " +
            std::to_string(max_depth) + "）");
  check(host.sent_count == 1, "响应只发一次");
  check(host.finished_count == 1, "收尾一次");
}

// =========================================================================
// 3. 中间件短路
// =========================================================================
void test_middleware_short_circuit() {
  std::cout << "[3] 中间件短路" << std::endl;

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  bool biz_ran = false;
  std::vector<uvcpp_web_handler> chain;
  // 鉴权失败：结束响应、**不调 next()**。
  chain.push_back([](uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next) {
    resp.status(401).text("unauthorized");
    resp.end();
  });
  chain.push_back([&biz_ran](uvcpp_web_request&, uvcpp_web_response&,
                             uvcpp_web_next) { biz_ran = true; });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);

  check(!biz_ran, "短路之后业务 handler 没跑");
  check(host.sent_count == 1, "只发了一次响应（短路的那个）");
  check(host.last_status == 401, "发出的是 401");
  check(ctx->chain_index() == 1, "链停在第 1 环之后");
}

// =========================================================================
// 4. end() 优先于 next()
// =========================================================================
void test_end_wins_over_next() {
  std::cout << "[4] end() 优先于 next()" << std::endl;

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  bool second_ran = false;
  std::vector<uvcpp_web_handler> chain;
  // 既结束响应又调 next()：使用者的错，但框架必须选一个 —— 选 end()，
  // 否则后面的 handler 会往一个已经序列化好的响应里继续写。
  chain.push_back([](uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next next) {
    resp.status(204).end();
    next();
  });
  chain.push_back([&second_ran](uvcpp_web_request&, uvcpp_web_response&,
                                uvcpp_web_next) { second_ran = true; });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);

  check(!second_ran, "end() 之后链停了");
  check(host.sent_count == 1, "只发一次");
  check(host.last_status == 204, "发出的是 end() 时的那个响应");
}

// =========================================================================
// 5. 异步续跑：从正确的下标继续
// =========================================================================
void test_async_resume_continues_at_right_index() {
  std::cout << "[5] 异步续跑" << std::endl;

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  int           first_runs  = 0;
  int           second_runs = 0;
  uvcpp_web_next saved;

  std::vector<uvcpp_web_handler> chain;
  chain.push_back([&first_runs, &saved](uvcpp_web_request& req,
                                        uvcpp_web_response&,
                                        uvcpp_web_next next) {
    ++first_runs;
    (void)req;
    saved = next;  // **留副本** → 框架据此判定"它打算稍后自己调"，链挂起
  });
  chain.push_back([&second_runs](uvcpp_web_request&, uvcpp_web_response& resp,
                                 uvcpp_web_next) {
    ++second_runs;
    resp.text("async done");
    resp.end();
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);

  check(first_runs == 1, "第 1 环跑了一次");
  check(second_runs == 0, "留了 next 没调 → 链挂起，第 2 环还没跑");
  check(host.sent_count == 0, "挂起期间不发响应");
  check(host.finished_count == 0, "挂起期间不通知收尾");
  check(!ctx->finished(), "ctx 未结束");
  check(ctx->chain_index() == 1, "下标停在 1");

  // 异步工作完成 → 恢复。
  saved();

  check(first_runs == 1, "第 1 环**没有**被重跑");
  check(second_runs == 1, "第 2 环跑了");
  check(host.sent_count == 1, "恢复后正常发送");
  check(host.last_body == "async done", "响应内容正确");
  check(ctx->finished(), "收尾了");
}

// =========================================================================
// 6. 末环留 next：先挂起，恢复后正常收尾
// =========================================================================
void test_last_handler_keeps_next_then_resumes() {
  std::cout << "[6] 末环留 next" << std::endl;

  capturing_sink    sink;
  log_capture_guard guard(&sink);

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  uvcpp_web_next saved;
  std::vector<uvcpp_web_handler> chain;
  chain.push_back([&saved](uvcpp_web_request&, uvcpp_web_response& resp,
                           uvcpp_web_next next) {
    resp.text("late");       // 只写了 body，没 end()
    saved = next;            // 留了 next → 挂起
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);

  check(host.sent_count == 0, "末环留 next → 挂起，什么都不发");
  check(ctx->chain_index() == 1, "下标越过了唯一的环");

  saved();

  check(host.sent_count == 1, "恢复后收尾并发送");
  check(host.last_body == "late", "body 是 handler 写的那份");
  check(sink.message_contains("没有结束响应"),
        "链结束但没 end() → 有 WARN");
}

// =========================================================================
// 7. 既不调 next 也不结束响应
// =========================================================================
void test_handler_without_next_or_end_warns_and_sends() {
  std::cout << "[7] 忘了 next 也忘了 end" << std::endl;

  capturing_sink    sink;
  log_capture_guard guard(&sink);

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  bool second_ran = false;
  std::vector<uvcpp_web_handler> chain;
  chain.push_back([](uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next) {
    resp.status(200).text("partial");  // 忘了 end()，也没调 next()
  });
  chain.push_back([&second_ran](uvcpp_web_request&, uvcpp_web_response&,
                                uvcpp_web_next) { second_ran = true; });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);

  check(!second_ran, "链在第 1 环中断");
  check(sink.message_contains("既没调 next()"), "有「既没调 next()」WARN");
  check(sink.message_contains("没有结束响应"), "也有「没有结束响应」WARN");
  // **照样发** —— 响应里可能已经被填好了，丢掉是更坏的结果。
  check(host.sent_count == 1, "仍然把响应发出去了（请求不会永远挂着）");
  check(host.last_status == 200, "状态码是 handler 设的那个");
  check(host.last_body == "partial", "body 保住了");
}

// =========================================================================
// 8. 异常 → 错误处理器
// =========================================================================
void test_exception_with_error_handler() {
  std::cout << "[8] 异常走错误处理器" << std::endl;

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  std::string seen_what;
  bool        after_throw_ran = false;

  std::vector<uvcpp_web_handler> chain;
  // 装错误处理器（真实的 App 会把它放在链首）。
  chain.push_back(web_middleware_error_handler(
      [&seen_what](uvcpp_web_request&, uvcpp_web_response& resp,
                   const std::string& what) {
        seen_what = what;
        resp.status(500).text("handled: " + what);
        resp.end();
      }));
  chain.push_back([](uvcpp_web_request&, uvcpp_web_response&, uvcpp_web_next) {
    throw std::runtime_error("boom");
  });
  chain.push_back([&after_throw_ran](uvcpp_web_request&, uvcpp_web_response&,
                                     uvcpp_web_next) {
    after_throw_ran = true;
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);

  check(seen_what == "boom", "错误处理器收到了 what()");
  check(!after_throw_ran, "抛异常之后的环没跑");
  check(host.sent_count == 1, "只发一次");
  check(host.last_body == "handled: boom", "错误处理器写的响应发出去了");
}

// =========================================================================
// 9. 异常且没装错误处理器 → 兜底 500
// =========================================================================
void test_exception_without_error_handler_500() {
  std::cout << "[9] 异常兜底 500" << std::endl;

  capturing_sink    sink;
  log_capture_guard guard(&sink);

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  std::vector<uvcpp_web_handler> chain;
  chain.push_back([](uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next) {
    // 先写一半再抛：兜底必须把它换掉，不能把半截业务数据当 500 的 body。
    resp.status(200).text("half-written business data");
    throw std::runtime_error("kaboom");
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);

  check(host.sent_count == 1, "发了响应");
  check(host.last_status == 500, "状态码是 500");
  check(host.last_body == "Internal Server Error", "body 是兜底文案");
  check(sink.has_level(log_level::ERR), "记了一条 ERROR");
  check(!ctx->aborted(), "兜底不是 abort");
}

// =========================================================================
// 9b. 非 std::exception 的异常也要接住
// =========================================================================
void test_exception_non_std() {
  std::cout << "[9b] 非标准异常" << std::endl;

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  std::vector<uvcpp_web_handler> chain;
  chain.push_back([](uvcpp_web_request&, uvcpp_web_response&, uvcpp_web_next) {
    throw 42;  // 不是 std::exception —— 只 catch(...) 能接住
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);

  check(host.sent_count == 1, "发了响应");
  check(host.last_status == 500, "非标准异常也回 500");
}

// =========================================================================
// 10. abort：不发响应
// =========================================================================
void test_abort_sends_nothing() {
  std::cout << "[10] abort" << std::endl;

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  bool second_ran = false;
  uvcpp_web_context* raw_ctx = nullptr;

  std::vector<uvcpp_web_handler> chain;
  chain.push_back([&raw_ctx](uvcpp_web_request&, uvcpp_web_response&,
                             uvcpp_web_next) {
    // 协议层发现这个连接上的数据已经不可信：不发任何回显，直接断。
    raw_ctx->abort();
  });
  chain.push_back([&second_ran](uvcpp_web_request&, uvcpp_web_response&,
                                uvcpp_web_next) { second_ran = true; });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  raw_ctx = ctx.get();
  ctx->run(chain);

  check(!second_ran, "abort 之后链停了");
  check(host.sent_count == 0, "**不发响应**");
  check(host.abort_count == 1, "通知 host 关连接");
  check(host.finished_count == 1, "收尾通知还是来了一次");
  check(ctx->aborted(), "ctx.aborted() 为真");
  check(ctx->finished(), "ctx.finished() 为真");
}

// =========================================================================
// 11. 收尾之后的 next 是安全空操作
// =========================================================================
void test_next_after_finish_is_noop() {
  std::cout << "[11] 收尾之后的 next" << std::endl;

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  uvcpp_web_next saved;
  std::vector<uvcpp_web_handler> chain;
  chain.push_back([&saved](uvcpp_web_request&, uvcpp_web_response& resp,
                           uvcpp_web_next next) {
    resp.text("immediate");
    resp.end();
    saved = next;  // 留了副本，但已经 end() —— 链会在 end() 处停下
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);

  // 留了 next 就是承诺"我会调它"（哪怕响应已经 end()）—— 所以链挂起，
  // 不抢在 handler 之前把响应发出去。这对**延迟响应**是必须的：
  // 那种 handler 会先 end() 占位，稍后才把内容填完。
  check(host.sent_count == 0, "留了 next → 挂起，不抢先发送");

  // 晚到的 next（比如异步任务回来才发现响应早发完了）。不能崩、不能重发。
  saved();
  check(host.sent_count == 1, "恢复后发送了一次");
  saved();
  saved();

  check(host.sent_count == 1, "晚到的 next 没有重复发送");
  check(host.finished_count == 1, "收尾也只一次");
  check(host.last_body == "immediate", "第一次发送的内容没被后来的调用改掉");
}

// =========================================================================
// 12. hold / release
// =========================================================================
void test_hold_defers_context_finished() {
  std::cout << "[12] hold / release" << std::endl;

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  uvcpp_web_next saved;
  uvcpp_web_context* raw_ctx = nullptr;

  std::vector<uvcpp_web_handler> chain;
  chain.push_back([&saved, &raw_ctx](uvcpp_web_request&, uvcpp_web_response&,
                                     uvcpp_web_next next) {
    // 打算用 post() 而不是留 next 来恢复：那就必须先钉住上下文，
    // 否则链一跑完 host 就把它销毁了。
    raw_ctx->hold();
    saved = next;
  });
  chain.push_back([](uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next) {
    resp.text("ok");
    resp.end();
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  raw_ctx = ctx.get();
  ctx->run(chain);

  saved();

  check(host.sent_count == 1, "响应发出去了");
  check(ctx->finished(), "链结束了");
  check(ctx->hold_count() == 1, "还有 1 个 hold 挂着");
  check(host.finished_count == 0, "**收尾通知被推迟** —— 上下文还不能销毁");

  ctx->release();

  check(ctx->hold_count() == 0, "hold 归零");
  check(host.finished_count == 1, "release 之后才通知收尾");
}

void test_release_without_hold_warns() {
  std::cout << "[12b] 多余的 release" << std::endl;

  capturing_sink    sink;
  log_capture_guard guard(&sink);

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->release();
  ctx->release();

  check(ctx->hold_count() == 0, "计数没有变成负数");
  check(sink.message_contains("多于 hold()"), "报了 WARN");
}

// =========================================================================
// 13. run() 只能一次
// =========================================================================
void test_run_twice_ignored() {
  std::cout << "[13] run 两次" << std::endl;

  capturing_sink    sink;
  log_capture_guard guard(&sink);

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  int runs = 0;
  std::vector<uvcpp_web_handler> chain;
  chain.push_back([&runs](uvcpp_web_request&, uvcpp_web_response& resp,
                          uvcpp_web_next) {
    ++runs;
    resp.text("once");
    resp.end();
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);
  ctx->run(chain);  // 第二次必须被拒

  check(runs == 1, "handler 只跑了一次");
  check(host.sent_count == 1, "只发一次");
  check(sink.has_level(log_level::ERR), "第二次 run 记了 ERROR");
}

// =========================================================================
// 14. post()
// =========================================================================
void test_post_inline_on_loop_thread() {
  std::cout << "[14] post 在 loop 线程上就地执行" << std::endl;

  fake_host host;
  std::shared_ptr<uvcpp_web_context> ctx =
      make_ctx(host, host.registry.add(fake_client(0)));

  check(ctx->on_loop_thread(), "测试线程就是 loop 线程");

  bool ran = false;
  ctx->post([&ran]() { ran = true; });

  check(ran, "post 就地执行了");
  check(host.post_count == 0, "没有往队列里投（省了一次投递）");
}

void test_post_from_other_thread() {
  std::cout << "[14b] 从别的线程 post" << std::endl;

  fake_host host;
  const uvcpp_web_conn_id id = host.registry.add(fake_client(0));

  uvcpp_web_next saved;
  int            second_runs = 0;

  std::vector<uvcpp_web_handler> chain;
  chain.push_back([&saved](uvcpp_web_request&, uvcpp_web_response&,
                           uvcpp_web_next next) { saved = next; });
  chain.push_back([&second_runs](uvcpp_web_request&, uvcpp_web_response& resp,
                                 uvcpp_web_next) {
    ++second_runs;
    resp.text("from thread");
    resp.end();
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  ctx->run(chain);

  // 模拟"线程池完成回调"：在别的线程里把恢复动作送回 loop 线程。
  std::thread worker([ctx, &saved]() {
    check(!ctx->on_loop_thread(), "worker 线程不是 loop 线程");
    ctx->post([&saved]() { saved(); });
  });
  worker.join();

  check(host.post_count == 1, "投递了一次");
  check(second_runs == 0, "还没 pump，第 2 环没跑");

  const size_t pumped = host.pump();

  check(pumped == 1, "pump 出一个任务");
  check(second_runs == 1, "pump 之后第 2 环跑了");
  check(host.last_body == "from thread", "响应正确");
}

// =========================================================================
// 15. user_data
// =========================================================================
void test_user_data() {
  std::cout << "[15] user_data" << std::endl;

  fake_host host;
  std::shared_ptr<uvcpp_web_context> ctx =
      make_ctx(host, host.registry.add(fake_client(0)));

  check(ctx->user_data() == nullptr, "默认没有 user_data");

  std::shared_ptr<std::string> who(new std::string("alice"));
  ctx->set_user_data(who);

  check(ctx->user_data_as<std::string>() != nullptr, "按类型取得回来");
  check(ctx->user_data_as<std::string>()->compare("alice") == 0, "内容对");
  // 类型对不上是**静默**返回空 —— 文档里写明了别拿它当类型校验。
  check(ctx->user_data_as<int>() == nullptr, "类型不对返回空");
}

// =========================================================================
// 16. 连接查询
// =========================================================================
void test_connection_lookup() {
  std::cout << "[16] 连接查询" << std::endl;

  fake_host host;
  uvcpp_tcp_client* c = fake_client(1);
  const uvcpp_web_conn_id id = host.registry.add(c, "192.168.1.9", 4444);

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);

  check(ctx->connection_alive(), "连接活着");
  check(ctx->raw_client() == c, "raw_client() 拿到登记时那个指针");
  check(ctx->connection_id() == id, "connection_id 对得上");

  // 对端断开。
  host.registry.remove(id);

  check(!ctx->connection_alive(), "断开后 connection_alive() 为假");
  check(ctx->raw_client() == nullptr, "**断开后 raw_client() 返回 nullptr**");

  // 从没登记过的 id。
  std::shared_ptr<uvcpp_web_context> ghost = make_ctx(host, 999999);
  check(!ghost->connection_alive(), "未知 id 的连接视为不活");
  check(ghost->raw_client() == nullptr, "未知 id 也是 nullptr");
}

// =========================================================================
// 17. id 永不复用（登记表的核心保证）
// =========================================================================
void test_conn_id_never_resolves_to_reused_address() {
  std::cout << "[17] 地址复用不会让旧 id 复活" << std::endl;

  uvcpp_web_connection_registry reg;
  uvcpp_tcp_client* c = fake_client(2);

  const uvcpp_web_conn_id first = reg.add(c, "1.1.1.1", 1000);
  check(reg.client(first) == c, "第一次登记能查到");

  reg.remove_by_client(c);
  check(reg.client(first) == nullptr, "断开后查不到");

  // **同一个地址**被新连接复用（真实场景：client 对象被 delete，新连接
  // 拿到了同一块内存）。这是整个设计要挡的那个缺陷。
  const uvcpp_web_conn_id second = reg.add(c, "2.2.2.2", 2000);

  check(second != first, "新连接拿到的是新 id");
  check(reg.client(first) == nullptr,
        "**旧 id 依然查不到** —— 不会指到新连接上");
  check(reg.client(second) == c, "新 id 查到新连接");
  check(reg.find(first) == nullptr, "find(旧 id) 也是空");
  check(reg.find(second) != nullptr, "find(新 id) 有记录");
  check(reg.find(second)->peer_port == 2000, "记录里的对端端口是新的");

  // 区分「断过」和「从没发过」：靠 id 单调递增，不需要额外的死 id 表。
  check(reg.issued(first), "旧 id **曾经**发出过");
  check(!reg.alive(first), "但它现在不活");
  check(!reg.issued(999999), "没发过的 id issued() 为假");
}

// =========================================================================
// 18. 登记表的其余行为
// =========================================================================
void test_registry_clear_keeps_ids_monotonic() {
  std::cout << "[18] clear 不回收 id" << std::endl;

  uvcpp_web_connection_registry reg;
  const uvcpp_web_conn_id a = reg.add(fake_client(0));

  check(reg.issued_count() == 1, "发出过 1 个 id");

  reg.clear();
  check(reg.size() == 0, "清空了");
  check(!reg.alive(a), "旧 id 不活了");

  const uvcpp_web_conn_id b = reg.add(fake_client(1));
  check(b > a, "**清空之后 id 接着涨，不复用**");
  check(reg.issued_count() == 2, "累计发出过 2 个");
  // clear() 不清历史：a 仍然算"发出过"，只是不活。"清空登记表"不等于
  // "允许旧 id 复活"。
  check(reg.issued(a), "clear 之后 issued(a) 仍为真");
}

void test_registry_duplicate_add() {
  std::cout << "[18b] 同一 client 重复登记" << std::endl;

  uvcpp_web_connection_registry reg;
  uvcpp_tcp_client* c = fake_client(3);

  const uvcpp_web_conn_id a = reg.add(c, "1.1.1.1", 1000);
  const uvcpp_web_conn_id b = reg.add(c, "1.1.1.1", 1001);

  check(a != b, "两次登记两个 id");
  check(reg.size() == 1, "表里只留一条（新的）");
  check(reg.client(a) == nullptr, "旧记录被摘掉了");
  check(reg.client(b) == c, "新记录有效");

  // 反向索引必须指着**新**的 id，否则 remove_by_client 会摘错。
  uvcpp_web_conn_id got = 0;
  check(reg.remove_by_client(c, &got), "按 client 能摘除");
  check(got == b, "摘掉的是新记录");
  check(reg.size() == 0, "表空了");
}

// =========================================================================
// 18d. 多循环：每份登记表只认自己的号（id 的高段是循环号）
// =========================================================================
void test_registry_ids_are_scoped_to_their_loop() {
  std::cout << "[18d] 多循环下 id 归哪条循环" << std::endl;

  // 单循环时 id 就是递增号本身 —— 这一条是"默认逐字节不变"的判据：
  // 高段恒 0，编码是恒等变换。
  uvcpp_web_connection_registry solo;  // loop_index 默认 0
  check(solo.loop_index() == 0, "默认循环号是 0");
  const uvcpp_web_conn_id s1 = solo.add(fake_client(0));
  const uvcpp_web_conn_id s2 = solo.add(fake_client(1));
  check(s1 == 1 && s2 == 2, "循环 0 发出来的 id 就是 1、2（与多循环之前相同）");

  // 两份登记表 = 两条循环，各自发号。**同一个 fake client 指针**分别登记进
  // 两份表：两张 by_client_ 是各自独立的，所以这不算重复登记。
  uvcpp_web_connection_registry r0(0);
  uvcpp_web_connection_registry r1(1);
  uvcpp_tcp_client* c0 = fake_client(2);
  uvcpp_tcp_client* c1 = fake_client(3);
  const uvcpp_web_conn_id ia = r0.add(c0, "1.1.1.1", 1000);
  const uvcpp_web_conn_id ib = r1.add(c1, "1.1.1.1", 1000);

  check(ia != ib, "两条循环发出来的号不同");
  check(uvcpp_web_connection_registry::loop_of(ia) == 0, "ia 归循环 0");
  check(uvcpp_web_connection_registry::loop_of(ib) == 1, "ib 归循环 1");
  // 低段各自从 1 开始 ⇒ **两条循环会发出低段完全相同的号**。这正是"只比低段"
  // （也就是只比 `id < next_id_`）会误判的地方。
  check(uvcpp_web_connection_registry::seq_of(ia) ==
            uvcpp_web_connection_registry::seq_of(ib),
        "两条循环的低段相同（都从 1 开始）");

  // 核心判据：issued() **不能**对别的循环发出来的号返回真。
  check(r0.issued(ia), "r0 认自己的号");
  check(!r0.issued(ib), "**r0 不认循环 1 发出来的号**");
  check(!r1.issued(ia), "**r1 不认循环 0 发出来的号**");
  check(r1.issued(ib), "r1 认自己的号");

  // 配套的两条：那个号在本表里既查不到、也不活。少了高段那一条，"曾经发过"
  // 与"查不到任何记录"会同时为真 —— 调用方只能二选一地误判（把框架自己的 bug
  // 当正常断开，或者反过来把正常断开报成 bug）。
  check(r0.client(ib) == nullptr, "r0 查不到别的循环的连接");
  check(!r0.alive(ib), "r0 里它不是活的");
  check(r0.find(ib) == nullptr, "r0 的 find 也是空");
  check(r1.client(ia) == nullptr && !r1.alive(ia), "反向同理");

  // 越界的循环号夹回 0，而不是带着一个会溢进递增号那一段的值继续跑。
  uvcpp_web_connection_registry neg(-1);
  check(neg.loop_index() == 0, "负的循环号夹回 0");
  uvcpp_web_connection_registry over(1 << 20);
  check(over.loop_index() == 0, "超上界的循环号也夹回 0");
  check(uvcpp_web_connection_registry::loop_of(over.add(nullptr)) == 0,
        "夹回之后发出来的号高段为 0");

  // make_id / loop_of / seq_of 是一组互逆的拆装 —— 跨循环定位就靠它。
  const uvcpp_web_conn_id made = uvcpp_web_connection_registry::make_id(3, 7);
  check(made == (static_cast<uvcpp_web_conn_id>(3) << 44) + 7, "make_id 拼得对");
  check(uvcpp_web_connection_registry::loop_of(made) == 3 &&
            uvcpp_web_connection_registry::seq_of(made) == 7,
        "loop_of/seq_of 是 make_id 的逆运算");

  // 高段满了会溢进递增号那一段 —— 这条断言把上界钉住（20 位循环号 = 1048575）。
  check(uvcpp_web_connection_registry::loop_of(
            static_cast<uvcpp_web_conn_id>((1 << 20) - 1) << 44) == (1 << 20) - 1,
        "循环号上界 (2^20)-1 不溢出");
}

void test_registry_remove_paths() {
  std::cout << "[18c] 登记表的摘除路径" << std::endl;

  uvcpp_web_connection_registry reg;
  uvcpp_tcp_client* c = fake_client(0);

  check(!reg.remove_by_client(nullptr), "remove_by_client(nullptr) 为假");
  check(!reg.remove_by_client(fake_client(1)), "摘除没登记的 client 为假");
  check(!reg.remove(12345), "摘除不存在的 id 为假");

  const uvcpp_web_conn_id a = reg.add(c);
  const uvcpp_web_conn_id b = reg.add(fake_client(1));
  check(reg.size() == 2, "两条");

  std::vector<uvcpp_web_conn_id> ids = reg.ids();
  check(ids.size() == 2, "ids() 两个");
  check(ids[0] == a && ids[1] == b, "ids() 按 id 升序");

  // 摘掉一个，另一个不受影响（反向索引不能误伤）。
  check(reg.remove(a), "摘掉 a");
  check(reg.client(b) != nullptr, "b 还在");
  // a 被摘掉时，它对应的反向索引**必须一起清掉** —— 否则 by_client_ 会
  // 一直指着一条不存在的记录，之后的 remove_by_client 就会摘错东西。
  check(!reg.remove_by_client(c), "c 的反向索引已经跟着 a 一起清了");

  // add(nullptr) 是允许的：只登记一个身份，没有连接。
  const uvcpp_web_conn_id n = reg.add(nullptr, "0.0.0.0", 0);
  check(reg.alive(n), "空 client 也能登记");
  check(reg.client(n) == nullptr, "查出来是 nullptr");
  // 摘除一个 id **不等于**允许那个号被复用 —— 号只增不减，两条路径都是。
  // （只测 clear() 是不够的：`remove()` 是另一条会更新的路径。）
  check(n != a && n != b && n > b, "摘除之后发的 id 也不回头（实际 " +
                                        std::to_string(n) + "，b=" +
                                        std::to_string(b) + "）");
  check(reg.remove(n), "也能摘除");
}

// =========================================================================
// 19. 连接断开后 handler 仍然安全收尾
// =========================================================================
void test_async_handler_survives_disconnect() {
  std::cout << "[19] 断开后异步收尾" << std::endl;

  fake_host host;
  uvcpp_tcp_client* c = fake_client(1);
  const uvcpp_web_conn_id id = host.registry.add(c);

  uvcpp_web_next   saved;
  uvcpp_web_context* raw_ctx = nullptr;

  std::vector<uvcpp_web_handler> chain;
  chain.push_back([&saved, &raw_ctx](uvcpp_web_request&, uvcpp_web_response&,
                                     uvcpp_web_next next) {
    // 异步 handler 起手就该钉住上下文：连接可能在这期间断掉，那时框架
    // 会把它从 in-flight 表里摘掉，唯一撑住它的就是我们留的这份。
    raw_ctx->hold();
    saved = next;
  });
  chain.push_back([](uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next) {
    resp.text("too late");
    resp.end();
  });

  std::shared_ptr<uvcpp_web_context> ctx = make_ctx(host, id);
  raw_ctx = ctx.get();
  ctx->run(chain);

  // 异步工作还没回来，连接先断了。
  host.registry.remove(id);

  check(!ctx->connection_alive(), "连接已经断了");
  check(ctx->raw_client() == nullptr, "拿不到裸连了");

  // 现在异步工作回来了：链照样跑完，host 负责把响应丢掉并记一条。
  saved();

  check(ctx->finished(), "链仍然正常收尾");
  check(host.sent_count == 1,
        "host 仍然收到了 send_response（由它决定丢不丢）");
  check(host.last_body == "too late", "body 是完整的");

  ctx->release();
  check(host.finished_count == 1, "hold 释放后收尾");
}

}  // namespace

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  test_linear_chain();
  test_sync_next_is_not_recursive();
  test_middleware_short_circuit();
  test_end_wins_over_next();
  test_async_resume_continues_at_right_index();
  test_last_handler_keeps_next_then_resumes();
  test_handler_without_next_or_end_warns_and_sends();
  test_exception_with_error_handler();
  test_exception_without_error_handler_500();
  test_exception_non_std();
  test_abort_sends_nothing();
  test_next_after_finish_is_noop();
  test_hold_defers_context_finished();
  test_release_without_hold_warns();
  test_run_twice_ignored();
  test_post_inline_on_loop_thread();
  test_post_from_other_thread();
  test_user_data();
  test_connection_lookup();
  test_conn_id_never_resolves_to_reused_address();
  test_registry_clear_keeps_ids_monotonic();
  test_registry_duplicate_add();
  test_registry_ids_are_scoped_to_their_loop();
  test_registry_remove_paths();
  test_async_handler_survives_disconnect();

  if (g_failures == 0) {
    std::cout << "[context] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[context] FAIL (" << g_failures << " checks failed)"
            << std::endl;
  return 2;
}
