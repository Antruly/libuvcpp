/**
 * @file tests/functional/fs_async_write_func.cpp
 * @brief Phase 3b 步骤 1 —— 异步 `uvcpp_fs` **写**路径的风险退除探针。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么单独有这个文件
 * --------------------
 * Phase 3b（multipart 上传）要把收到的部件字节**边收边落盘**，载体选的是
 * 「在事件循环上直接提交异步 `uv_fs_write`」，而不是仓库既有的
 * 「`uvcpp_work` worker 里跑同步 `uv_fs_*`」。这是一个**有意的偏离**：
 * `uvcpp_work` 严格一次性（`queue_work` 在 `loop != nullptr` 时抛异常），
 * 每写一块就要新建一个对象加一套 retire/drain 回收流水，代价纯粹是为了迁就
 * 它的不可复用性。
 *
 * 但异步 `uvcpp_fs` **在全仓生产代码里零使用** —— 六个既有调用点（静态服务、
 * 旧静态服务）全都走同步形态（回调传 `nullptr`）在 worker 线程里调。所以
 * 「它到底能不能这么用」是一个必须先证出来的假设，而不是可以假设的前提。
 *
 * **本文件不做任何上传相关的事**，只回答一个问题：异步 `uvcpp_fs` 的写路径
 * 能不能支撑「一个对象、串行、多笔、边写边链」这个用法。所以它匹配不到任何
 * `web_*` 过滤规则，在所有构建配置里都被编译 —— 它测的是 core 能力，
 * 不是 webapp 能力。
 *
 * 每条断言对应一个具体风险，见各场景的注释。
 * 跑法：`test_fs_async_write_func.exe [子串过滤]`
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_timer.h"
#include "req/uvcpp_fs.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

/// 读回整个文件。用 `fopen` 而不是 `uv_fs_*`，因为本文件用的全是纯 ASCII
/// 路径（ANSI 代码页与 UTF-8 一致），这里不需要 `uv_fs_*` 的 UTF-8 转换。
/// 生产代码里读非 ASCII 路径**必须**用 `uv_fs_*`，理由见
/// `src/webapp/uvcpp_web_static.cpp` 的 `read_file()` 注释。
std::string read_all(const std::string& path) {
  std::string out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return out;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

/// 造一段**可校验**的字节：含 NUL、0xFF、CRLF，且每个位置的值与下标有关，
/// 于是「偏移算错」和「长度截断」都会表现为内容不等，而不是碰巧相等。
std::string make_payload(size_t n, unsigned seed) {
  std::string s;
  s.resize(n);
  for (size_t i = 0; i < n; ++i) {
    s[i] = static_cast<char>((i * 31u + seed * 17u + (i >> 8)) & 0xFF);
  }
  // 钉几个刻意挑的字节，确保边界值真的被覆盖到
  if (n > 0) s[0] = '\0';
  if (n > 1) s[1] = static_cast<char>(0xFF);
  if (n > 3) {
    s[2] = '\r';
    s[3] = '\n';
  }
  if (n > 5) s[n - 1] = static_cast<char>(0xFE);
  return s;
}

// ---------------------------------------------------------------------------
// 场景骨架：一个 loop + 一个看门狗。
//
// 看门狗的作用不是"测性能"，而是把「链没跑完」变成**确定的失败**而不是永久
// 挂起 —— `fs_async_reuse_func.cpp` 记过同一个教训：单飞守卫写错时 libuv
// 的线程池队列会失衡，症状是 `uv_run` 空转 100% CPU 且回调永不到达。
// ---------------------------------------------------------------------------
class scenario {
 public:
  scenario(const char* name, int timeout_ms)
      : name_(name), timed_out_(false), loop_() {
    loop_.init();
    watchdog_.reset(new uvcpp_timer(&loop_));
    watchdog_->start(
        [this, timeout_ms](uvcpp_timer*) {
          timed_out_.store(true);
          std::cerr << "  [FAIL] " << name_ << ": 看门狗超时（" << timeout_ms
                    << "ms）—— 异步链没有跑完" << std::endl;
          loop_.stop();
        },
        timeout_ms, 0);
  }

  uvcpp_loop* loop() { return &loop_; }
  bool timed_out() const { return timed_out_.load(); }

  /// 供「回调必须在 loop 线程」用：在**将要跑 loop 的那个线程**上记一次。
  void note_loop_thread() { loop_thread_ = std::this_thread::get_id(); }
  bool on_loop_thread() const {
    return std::this_thread::get_id() == loop_thread_;
  }

 private:
  std::string name_;
  std::atomic<bool> timed_out_;
  uvcpp_loop loop_;
  std::unique_ptr<uvcpp_timer> watchdog_;
  std::thread::id loop_thread_;
};

// ---------------------------------------------------------------------------
// 场景 1：write_readback —— 最短的正路
//
// 退休的风险：`uv_fs_open` 的 fd 到底从哪来（提交返回 0、fd 经 `req->result`
// 回来）、回调里 `get_result()` 是不是还能读（`req_cleanup()` 在回调**之前**
// 跑）、写进去的字节是不是真的按偏移落对位置。
// ---------------------------------------------------------------------------
void test_write_readback() {
  const std::string path = "uvcpp_fs_aw_1.bin";
  std::remove(path.c_str());

  scenario env("write_readback", 10000);
  env.note_loop_thread();

  const std::string payload = make_payload(4096, 1);
  // 缓冲区必须活到回调返回 —— `uv_fs_write` 只 memcpy 那个 `uv_buf_t`
  // **数组**，从不拷贝数据本身。用 shared_ptr 把这件事显式化。
  std::shared_ptr<std::string> buf(new std::string(payload));

  std::promise<int> done;
  std::future<int> done_future = done.get_future();

  std::atomic<int> open_rc(999), write_rc(999), fsync_rc(999), close_rc(999);
  std::atomic<bool> cb_on_loop_thread(true);

  uvcpp_fs fs;
  int rc = fs.open(
      env.loop(), path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600,
      [&](uvcpp_fs* o) {
        if (!env.on_loop_thread()) cb_on_loop_thread.store(false);
        open_rc.store(static_cast<int>(o->get_result()));
        if (o->get_result() < 0) {
          done.set_value(1);
          env.loop()->stop();
          return;
        }
        const uv_file fd = static_cast<uv_file>(o->get_result());
        uv_buf_t b = uv_buf_init(&(*buf)[0],
                                 static_cast<unsigned int>(buf->size()));
        // `fd` 是**本 lambda 的局部量**，必须在嵌套回调里按值捕获 ——
        // 它随本 lambda 返回就没了，而嵌套回调要晚得多才跑。
        // 这个坑第一版就踩了：`[&]` 捕到的是悬垂栈引用，fd 变成垃圾值，
        // fsync 报 UV_EBADF(-4083)。`uv_buf_t b` 反而**可以**是局部量 ——
        // `uv_fs_write` 会把 bufs **数组**memcpy 走，只有数据指针要活到回调。
        fs.write(env.loop(), fd, &b, 1, -1, [&, fd](uvcpp_fs* w) {
          if (!env.on_loop_thread()) cb_on_loop_thread.store(false);
          // 回调里 get_result() 必须是**已写入字节数**：
          // req_cleanup() 在回调之前跑，statbuf/ptr/path 都已失效，
          // 只有 result 还在。
          write_rc.store(static_cast<int>(w->get_result()));
          fs.fsync(env.loop(), fd, [&, fd](uvcpp_fs* s) {
            if (!env.on_loop_thread()) cb_on_loop_thread.store(false);
            fsync_rc.store(static_cast<int>(s->get_result()));
            fs.close(env.loop(), fd, [&](uvcpp_fs* c) {
              if (!env.on_loop_thread()) cb_on_loop_thread.store(false);
              close_rc.store(static_cast<int>(c->get_result()));
              done.set_value(0);
              env.loop()->stop();
            });
          });
        });
      });

  check_eq_i(rc, 0, "write_readback: 异步 open 的提交必须返回 0（fd 经 result 回来）");

  env.loop()->run(UV_RUN_DEFAULT);

  if (done_future.wait_for(std::chrono::milliseconds(0)) !=
      std::future_status::ready) {
    check(false, "write_readback: 异步链没有完成");
    return;
  }
  done_future.get();

  check(!env.timed_out(), "write_readback: 不能超时");
  check(open_rc.load() >= 0, "write_readback: 异步 open 必须拿到有效 fd");
  check_eq_i(write_rc.load(), static_cast<long long>(payload.size()),
             "write_readback: 一次写必须报告写了全部字节");
  check_eq_i(fsync_rc.load(), 0, "write_readback: 异步 fsync 必须成功");
  check_eq_i(close_rc.load(), 0, "write_readback: 异步 close 必须成功");
  // 回调跑在线程池线程上是错的：uv_fs_* 的工作在池里做，但**完成回调
  // 必须回到 loop 线程**，否则框架里所有"在回调里碰请求对象"的写法都不成立。
  check(cb_on_loop_thread.load(), "write_readback: 每个回调都必须在 loop 线程上");

  const std::string back = read_all(path);
  check_eq_i(static_cast<long long>(back.size()),
             static_cast<long long>(payload.size()),
             "write_readback: 读回长度必须一致");
  check(back == payload, "write_readback: 读回内容必须逐字节一致");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 场景 2：chained_writes —— **同一个对象上，从写回调里再链一笔写**
//
// 这是上传路径真正要用的形状（每收到一块就写一块，写完接着写下一块），
// 也是本文件最要紧的一条。
//
// 为什么它可能不成立：`uvcpp_fs::callback_write` 的写法是
//
//     self->async_pending_ = false;
//     self->req_cleanup();
//     if (self->fs_write_cb) self->fs_write_cb(self);   // ← 正在执行的就是它
//
// 而在它内部再调 `fs.write(...)` 会执行 `fs_write_cb = 新闭包;` —— 给
// **正在栈上执行的那个 std::function** 赋值。`std::function::operator=` 会
// 先销毁旧的目标对象，于是当前闭包的捕获（包括它持有的 shared_ptr）在
// 回调还没返回时就被释放了。
//
// 这与 `uvcpp_work` 那个「在 after-work 回调里 delete 自己」是同一族缺陷
// （见 `src/web/uvcpp_web_static.cpp` 的 retire 注释、`uvcpp_work.h:51-55`），
// 也与旧静态服务那两处 `delete work` 同族。既有用例之所以没暴露它：
// `fs_async_reuse_func.cpp` 链的是 **open→close→open→close→unlink**，
// 每次赋值的是**另一个**成员，而「同一个成员自我赋值」这条路径从没被走过。
// ---------------------------------------------------------------------------
void test_chained_writes() {
  const std::string path = "uvcpp_fs_aw_2.bin";
  std::remove(path.c_str());

  scenario env("chained_writes", 10000);
  env.note_loop_thread();

  // 三块，各自独立可校验；链起来后文件内容必须是三块的顺序拼接。
  const std::string c1 = make_payload(4096, 11);
  const std::string c2 = make_payload(8192, 22);
  const std::string c3 = make_payload(1024, 33);
  const std::string want = c1 + c2 + c3;

  std::vector<std::shared_ptr<std::string> > bufs;
  bufs.push_back(std::shared_ptr<std::string>(new std::string(c1)));
  bufs.push_back(std::shared_ptr<std::string>(new std::string(c2)));
  bufs.push_back(std::shared_ptr<std::string>(new std::string(c3)));

  std::promise<int> done;
  std::future<int> done_future = done.get_future();

  std::vector<long long> got(3, -1);
  std::atomic<int> chain_depth(0);

  // 用一个 shared_ptr<uvcpp_fs> 让"回调里还活着"这件事不依赖栈上对象 ——
  // 与上传会话用 shared_ptr 自持是同一个理由。
  std::shared_ptr<uvcpp_fs> fs(new uvcpp_fs());
  // 供回调递归引用的自持闭包。C++11 没有 init-capture，所以先声明再赋值。
  std::shared_ptr<std::function<void(uv_file, size_t)> > step(
      new std::function<void(uv_file, size_t)>());

  *step = [&, fs, step](uv_file fd, size_t idx) {
    if (idx >= bufs.size()) {
      fs->close(env.loop(), fd, [&](uvcpp_fs* c) {
        got.push_back(static_cast<long long>(c->get_result()));
        done.set_value(0);
        env.loop()->stop();
      });
      return;
    }
    // 在**写回调内部**再提交一笔写 —— 这才是被验的那件事。
    uv_buf_t b = uv_buf_init(&(*bufs[idx])[0],
                             static_cast<unsigned int>(bufs[idx]->size()));
    const size_t cur = idx;
    int wrc = fs->write(env.loop(), fd, &b, 1, -1, [&, fd, cur](uvcpp_fs* w) {
      got[cur] = static_cast<long long>(w->get_result());
      chain_depth.fetch_add(1);
      // 这一句之后**不再读任何捕获**（这正是既有代码侥幸不炸的原因），
      // 真正的工作交给下一层。
      (*step)(fd, cur + 1);
    });
    if (wrc != 0) {
      std::cerr << "  [FAIL] chained_writes: 链第 " << idx
                << " 笔写的提交失败 rc=" << wrc << std::endl;
      ++g_failures;
      done.set_value(2);
      env.loop()->stop();
    }
  };

  int rc = fs->open(env.loop(), path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600,
                    [&](uvcpp_fs* o) {
                      if (o->get_result() < 0) {
                        done.set_value(3);
                        env.loop()->stop();
                        return;
                      }
                      (*step)(static_cast<uv_file>(o->get_result()), 0);
                    });
  check_eq_i(rc, 0, "chained_writes: 异步 open 的提交必须返回 0");

  env.loop()->run(UV_RUN_DEFAULT);

  if (done_future.wait_for(std::chrono::milliseconds(0)) !=
      std::future_status::ready) {
    check(false, "chained_writes: 异步链没有完成");
    return;
  }
  done_future.get();

  check(!env.timed_out(), "chained_writes: 不能超时");
  check_eq_i(chain_depth.load(), 3, "chained_writes: 三段都必须写完成");
  check_eq_i(got[0], static_cast<long long>(c1.size()),
             "chained_writes: 第 1 块必须写全");
  check_eq_i(got[1], static_cast<long long>(c2.size()),
             "chained_writes: 第 2 块必须写全");
  check_eq_i(got[2], static_cast<long long>(c3.size()),
             "chained_writes: 第 3 块必须写全");

  const std::string back = read_all(path);
  check_eq_i(static_cast<long long>(back.size()),
             static_cast<long long>(want.size()),
             "chained_writes: 文件长度必须是三块之和");
  check(back == want, "chained_writes: 文件内容必须是三块的顺序拼接");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 场景 3：guard_reset —— 单飞守卫既**挡得住**也**放得开**
//
// 挡得住：在途时二次提交必须拿到 UV_EALREADY（否则共享的 uv_fs_t 会被
//         重新入队，线程池队列失衡 —— 见 fs_async_reuse_func.cpp 的记载）。
// 放得开：回调跑完之后**必须能再次提交**。上传路径整个建立在这一点上：
//         每一块的写都是"上一次写完成之后"才提交的。守卫若不在回调前复位，
//         第二块永远拿不到名额，上传会在第一个块之后静默停住。
// ---------------------------------------------------------------------------
void test_guard_reset() {
  const std::string path = "uvcpp_fs_aw_3.bin";
  std::remove(path.c_str());

  scenario env("guard_reset", 10000);
  env.note_loop_thread();

  const std::string payload = make_payload(2048, 7);
  std::shared_ptr<std::string> buf(new std::string(payload));

  std::promise<int> done;
  std::future<int> done_future = done.get_future();

  std::atomic<int> in_flight_rc(0), after_cb_rc(0);
  std::atomic<long long> first_wrote(-1), second_wrote(-1);

  uvcpp_fs fs;
  std::shared_ptr<uv_file> fd(new uv_file(-1));

  int rc = fs.open(env.loop(), path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600,
                   [&](uvcpp_fs* o) {
                     if (o->get_result() < 0) {
                       done.set_value(1);
                       env.loop()->stop();
                       return;
                     }
                     *fd = static_cast<uv_file>(o->get_result());
                     uv_buf_t b = uv_buf_init(&(*buf)[0], 1024);
                     fs.write(env.loop(), *fd, &b, 1, -1, [&](uvcpp_fs* w) {
                       first_wrote.store(static_cast<long long>(w->get_result()));
                       // 回调已经跑起来了 ⇒ 守卫必须已复位 ⇒ 这一笔能提交。
                       uv_buf_t b2 = uv_buf_init(&(*buf)[1024], 1024);
                       after_cb_rc.store(fs.write(
                           env.loop(), *fd, &b2, 1, -1, [&](uvcpp_fs* w2) {
                             second_wrote.store(
                                 static_cast<long long>(w2->get_result()));
                             fs.close(env.loop(), *fd, [&](uvcpp_fs*) {
                               done.set_value(0);
                               env.loop()->stop();
                             });
                           }));
                     });
                     // 紧接着的第一笔还在途 —— 这一笔必须被守卫挡掉。
                     uv_buf_t b3 = uv_buf_init(&(*buf)[0], 1024);
                     in_flight_rc.store(fs.write(env.loop(), *fd, &b3, 1, -1,
                                                 [](uvcpp_fs*) {}));
                   });
  check_eq_i(rc, 0, "guard_reset: 异步 open 的提交必须返回 0");

  env.loop()->run(UV_RUN_DEFAULT);

  if (done_future.wait_for(std::chrono::milliseconds(0)) !=
      std::future_status::ready) {
    check(false, "guard_reset: 异步链没有完成");
    return;
  }
  done_future.get();

  check(!env.timed_out(), "guard_reset: 不能超时");
  check_eq_i(in_flight_rc.load(), UV_EALREADY,
             "guard_reset: 在途时的二次提交必须被挡成 UV_EALREADY");
  check_eq_i(after_cb_rc.load(), 0,
             "guard_reset: 回调跑完之后必须能再次提交（守卫要复位）");
  check_eq_i(first_wrote.load(), 1024, "guard_reset: 第 1 笔必须写全");
  check_eq_i(second_wrote.load(), 1024, "guard_reset: 第 2 笔必须写全");

  const std::string back = read_all(path);
  check_eq_i(static_cast<long long>(back.size()), 2048,
             "guard_reset: 两笔写都落盘（文件 2048 字节）");
  check(back == payload, "guard_reset: 内容必须逐字节一致");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 场景 4：large_payload —— 短写到底会不会发生
//
// `uv_fs_write` 不保证写满：内核/驱动可以在任意位置返回短写。上传路径若
// 假设"提交多少就写多少"，短写会**静默丢数据**（内容长度对不上，而且只在
// 特定尺寸/平台上出现）。
//
// 本场景如实回答"在本平台上 4 MiB 会不会短写"，两种结论都要有据：
//   - 观察到短写 ⇒ 写循环是**必需**的，变异 M17（去掉重试循环）可被抓住；
//   - 没观察到   ⇒ 写循环仍然是必需的（契约不保证），但 M17 在本平台上
//                  是**等价变异** —— 这一点必须照实记，不许假装覆盖了。
// ---------------------------------------------------------------------------
void test_large_payload() {
  const std::string path = "uvcpp_fs_aw_4.bin";
  std::remove(path.c_str());

  const size_t kSize = 4u * 1024u * 1024u;  // 4 MiB
  scenario env("large_payload", 15000);
  env.note_loop_thread();

  const std::string payload = make_payload(kSize, 5);
  std::shared_ptr<std::string> buf(new std::string(payload));

  std::promise<int> done;
  std::future<int> done_future = done.get_future();

  std::atomic<long long> total_written(0);
  std::atomic<int> submits(0);
  std::atomic<bool> saw_short(false);

  uvcpp_fs fs;
  std::shared_ptr<uv_file> fd(new uv_file(-1));
  // 写循环：短写就接着写剩下的。offset 用 -1（顺序写，文件位置由驱动推进）。
  std::shared_ptr<std::function<void(size_t)> > pump(
      new std::function<void(size_t)>());

  *pump = [&, pump](size_t off) {
    if (off >= kSize) {
      fs.close(env.loop(), *fd, [&](uvcpp_fs*) {
        done.set_value(0);
        env.loop()->stop();
      });
      return;
    }
    const size_t want = kSize - off;
    uv_buf_t b =
        uv_buf_init(&(*buf)[off], static_cast<unsigned int>(want));
    int rc = fs.write(env.loop(), *fd, &b, 1, -1, [&, off](uvcpp_fs* w) {
      const long long n = static_cast<long long>(w->get_result());
      if (n < 0) {
        std::cerr << "  [FAIL] large_payload: 写失败 rc=" << n << std::endl;
        ++g_failures;
        done.set_value(1);
        env.loop()->stop();
        return;
      }
      if (n < static_cast<long long>(kSize - off)) saw_short.store(true);
      total_written.fetch_add(n);
      (*pump)(off + static_cast<size_t>(n));
    });
    if (rc != 0) {
      std::cerr << "  [FAIL] large_payload: 提交失败 rc=" << rc << std::endl;
      ++g_failures;
      done.set_value(2);
      env.loop()->stop();
    } else {
      submits.fetch_add(1);
    }
  };

  int rc = fs.open(env.loop(), path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600,
                   [&](uvcpp_fs* o) {
                     if (o->get_result() < 0) {
                       done.set_value(3);
                       env.loop()->stop();
                       return;
                     }
                     *fd = static_cast<uv_file>(o->get_result());
                     (*pump)(0);
                   });
  check_eq_i(rc, 0, "large_payload: 异步 open 的提交必须返回 0");

  env.loop()->run(UV_RUN_DEFAULT);

  if (done_future.wait_for(std::chrono::milliseconds(0)) !=
      std::future_status::ready) {
    check(false, "large_payload: 异步链没有完成");
    return;
  }
  done_future.get();

  check(!env.timed_out(), "large_payload: 不能超时");
  check_eq_i(total_written.load(), static_cast<long long>(kSize),
             "large_payload: 累计写入必须等于载荷长度（短写要被写循环接住）");

  const std::string back = read_all(path);
  check_eq_i(static_cast<long long>(back.size()),
             static_cast<long long>(kSize),
             "large_payload: 文件长度必须等于载荷长度");
  check(back == payload, "large_payload: 4 MiB 内容必须逐字节一致");

  // **如实记录，不是断言**：本平台到底会不会短写。两种结果都不算失败，
  // 但结论决定了变异 M17 是"可被抓住"还是"在本平台上等价"。
  std::cout << "  [note] large_payload: 提交次数=" << submits.load()
            << " 观察到短写=" << (saw_short.load() ? "是" : "否")
            << " —— 即使不短写，写循环仍是契约要求的（uv_fs_write 不保证写满）"
            << std::endl;

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 场景 6：capture_alive_after_chain —— 把"闭包被自己销毁"变成**可测量**的
//
// 场景 2 的链法是"链完就返回"（提交是最后一句），于是即使 `fs_write_cb` 在
// 回调内部被重新赋值、正在执行的那个闭包被析构，也读不到任何捕获了 ——
// 属于**侥幸不炸**，与 `uvcpp_static_server` 那两处"最后一句 delete"同形。
//
// 上传路径需要的形状比这更强：写完一块之后还要 `resume()`、还要还名额、
// 还要更新统计 —— 也就是**提交之后仍要读捕获**。所以这里把判定做成确定的：
// 在回调里按值捕一个 `shared_ptr`，先记引用计数，链下一笔，**再**记一次。
// 正在执行的闭包若被析构，它那份捕获随之释放，计数必然下降。
//
// 这个判据不依赖"崩溃与否"（那要碰运气），而是直接量"执行中的闭包还活着吗"。
// ---------------------------------------------------------------------------
void test_capture_alive_after_chain() {
  const std::string path = "uvcpp_fs_aw_6.bin";
  std::remove(path.c_str());

  scenario env("capture_alive_after_chain", 10000);
  env.note_loop_thread();

  // 外层也持一份，所以"闭包活着"时计数是 2。
  std::shared_ptr<std::string> probe(new std::string("probe-payload"));

  std::promise<int> done;
  std::future<int> done_future = done.get_future();

  std::atomic<long> count_before(0);
  std::atomic<long> count_after(0);
  std::atomic<long> count_in_second(0);

  std::shared_ptr<uvcpp_fs> fs(new uvcpp_fs());
  std::shared_ptr<uv_file> fd(new uv_file(-1));
  std::shared_ptr<std::function<void()> > second(
      new std::function<void()>());

  uvcpp_loop* loop = env.loop();

  *second = [&, fs, fd]() {
    uv_buf_t b = uv_buf_init(&(*probe)[0], 4);
    fs->write(loop, *fd, &b, 1, -1, [&](uvcpp_fs*) {
      // 第二笔是收尾，不再链，所以这里读捕获是安全的。
      count_in_second.store(static_cast<long>(probe.use_count()));
      fs->close(loop, *fd, [&](uvcpp_fs*) {
        done.set_value(0);
        env.loop()->stop();
      });
    });
  };

  int rc = fs->open(loop, path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600,
                    [&, fs, fd](uvcpp_fs* o) {
                      if (o->get_result() < 0) {
                        done.set_value(1);
                        env.loop()->stop();
                        return;
                      }
                      *fd = static_cast<uv_file>(o->get_result());
                      uv_buf_t b = uv_buf_init(&(*probe)[0], 4);
                      // probe 按值捕进来 —— 本闭包就是这个捕获的唯一持有者之一。
                      fs->write(loop, *fd, &b, 1, -1, [&, probe](uvcpp_fs*) {
                        count_before.store(
                            static_cast<long>(probe.use_count()));
                        // 链下一笔：这一步会执行 `fs_write_cb = 新闭包;`，
                        // 即给**正在执行的自己**赋值。
                        (*second)();
                        // 链完**仍然**读捕获 —— 这才是上传路径要的形状。
                        count_after.store(
                            static_cast<long>(probe.use_count()));
                      });
                    });
  check_eq_i(rc, 0, "capture_alive_after_chain: open 提交必须返回 0");

  env.loop()->run(UV_RUN_DEFAULT);

  if (done_future.wait_for(std::chrono::milliseconds(0)) !=
      std::future_status::ready) {
    check(false, "capture_alive_after_chain: 异步链没有完成");
    return;
  }
  done_future.get();

  check(!env.timed_out(), "capture_alive_after_chain: 不能超时");
  check_eq_i(count_before.load(), 2,
             "capture_alive_after_chain: 回调执行中，闭包与外层各持一份（=2）");
  check_eq_i(count_after.load(), count_before.load(),
             "capture_alive_after_chain: 链下一笔**不得**析构正在执行的闭包"
             "（计数掉了就说明 fs_write_cb 的自我赋值把执行中的闭包释放了）");
  // 第二笔的闭包按**引用**捕 probe（`[&]`），自己不留副本，所以此刻只有
  // 外层那一份持有着 —— 1 才是对的，不是 2。
  check_eq_i(count_in_second.load(), 1,
             "capture_alive_after_chain: 第二笔闭包按引用捕，只剩外层那一份");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 场景 5：two_objects —— 两个独立对象并发写，互不干扰
//
// 退休的风险：`uvcpp_fs` 是否有**共享的全局状态**（比如那个共用的
// `uv_fs_t` 是不是静态的）。上传路径每个会话一个对象、多个会话并发，
// 若有全局状态就会互相踩。
// ---------------------------------------------------------------------------
void test_two_objects() {
  const std::string pa = "uvcpp_fs_aw_5a.bin";
  const std::string pb = "uvcpp_fs_aw_5b.bin";
  std::remove(pa.c_str());
  std::remove(pb.c_str());

  scenario env("two_objects", 10000);
  env.note_loop_thread();

  const std::string ka = make_payload(3000, 101);
  const std::string kb = make_payload(5001, 202);
  std::shared_ptr<std::string> ba(new std::string(ka));
  std::shared_ptr<std::string> bb(new std::string(kb));

  std::promise<int> done;
  std::future<int> done_future = done.get_future();

  std::atomic<int> finished(0);
  std::atomic<int> rc_a(999), rc_b(999);

  // 两个对象各自一份，交叉提交（A 开→B 开→A 写→B 写），确保真的在途重叠。
  std::shared_ptr<uvcpp_fs> fa(new uvcpp_fs());
  std::shared_ptr<uvcpp_fs> fb(new uvcpp_fs());
  std::shared_ptr<uv_file> fda(new uv_file(-1));
  std::shared_ptr<uv_file> fdb(new uv_file(-1));

  uvcpp_loop* loop = env.loop();

  // 每一次 open/写 都立刻回来，让两个对象的在途段重叠。
  int ra = fa->open(loop, pa.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600,
                    [&, fa, fda](uvcpp_fs* o) {
                      if (o->get_result() < 0) {
                        rc_a.store(1);
                        if (finished.fetch_add(1) + 1 == 2) {
                          done.set_value(0);
                          env.loop()->stop();
                        }
                        return;
                      }
                      *fda = static_cast<uv_file>(o->get_result());
                      uv_buf_t b = uv_buf_init(&(*ba)[0],
                                               static_cast<unsigned int>(ba->size()));
                      fa->write(loop, *fda, &b, 1, -1, [&](uvcpp_fs* w) {
                        rc_a.store(static_cast<int>(w->get_result() >= 0 ? 0 : 1));
                        fa->close(loop, *fda, [&](uvcpp_fs*) {
                          if (finished.fetch_add(1) + 1 == 2) {
                            done.set_value(0);
                            env.loop()->stop();
                          }
                        });
                      });
                    });
  int rb = fb->open(loop, pb.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600,
                    [&, fb, fdb](uvcpp_fs* o) {
                      if (o->get_result() < 0) {
                        rc_b.store(1);
                        if (finished.fetch_add(1) + 1 == 2) {
                          done.set_value(0);
                          env.loop()->stop();
                        }
                        return;
                      }
                      *fdb = static_cast<uv_file>(o->get_result());
                      uv_buf_t b = uv_buf_init(&(*bb)[0],
                                               static_cast<unsigned int>(bb->size()));
                      fb->write(loop, *fdb, &b, 1, -1, [&](uvcpp_fs* w) {
                        rc_b.store(static_cast<int>(w->get_result() >= 0 ? 0 : 1));
                        fb->close(loop, *fdb, [&](uvcpp_fs*) {
                          if (finished.fetch_add(1) + 1 == 2) {
                            done.set_value(0);
                            env.loop()->stop();
                          }
                        });
                      });
                    });

  check(ra == 0 && rb == 0, "two_objects: 两个对象的提交都必须返回 0");

  env.loop()->run(UV_RUN_DEFAULT);

  if (done_future.wait_for(std::chrono::milliseconds(0)) !=
      std::future_status::ready) {
    check(false, "two_objects: 两个对象都没有跑完");
    return;
  }
  done_future.get();

  check(!env.timed_out(), "two_objects: 不能超时");
  check_eq_i(rc_a.load(), 0, "two_objects: A 必须写成功");
  check_eq_i(rc_b.load(), 0, "two_objects: B 必须写成功");
  check(read_all(pa) == ka, "two_objects: A 的内容必须正确（没有被 B 串到）");
  check(read_all(pb) == kb, "two_objects: B 的内容必须正确（没有被 A 串到）");

  std::remove(pa.c_str());
  std::remove(pb.c_str());
}

struct case_entry {
  const char* name;
  void (*fn)();
};

}  // namespace

int main(int argc, char** argv) {
  std::string only;
  if (argc > 1) only = argv[1];

  const case_entry cases[] = {
      {"write_readback", test_write_readback},
      {"chained_writes", test_chained_writes},
      {"capture_alive_after_chain", test_capture_alive_after_chain},
      {"guard_reset", test_guard_reset},
      {"large_payload", test_large_payload},
      {"two_objects", test_two_objects},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (!only.empty() &&
        std::string(cases[i].name).find(only) == std::string::npos) {
      continue;
    }
    // 每换一组就把计数清零：汇总只看"这一组有没有红"。
    const int before = g_failures;
    std::cout << "[fs_async_write] " << cases[i].name << std::endl;
    cases[i].fn();
    const bool ok = (g_failures == before);
    std::cout << (ok ? "  -> PASS" : "  -> FAIL") << std::endl;
    if (!ok) {
      std::cout << "[fs_async_write] FAIL" << std::endl;
      return 2;
    }
  }

  std::cout << "[fs_async_write] ALL PASS" << std::endl;
  return 0;
}
