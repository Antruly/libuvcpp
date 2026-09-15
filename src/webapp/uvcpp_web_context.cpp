/**
 * @file src/webapp/uvcpp_web_context.cpp
 * @brief uvcpp_web_context 的实现 —— 链的推进与收尾。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <webapp/uvcpp_web_context.h>

#include <exception>

#include <webapp/uvcpp_log.h>

namespace uvcpp {

uvcpp_web_context_host::~uvcpp_web_context_host() {}

uvcpp_web_context::uvcpp_web_context(uvcpp_web_context_host& host,
                                     uvcpp_web_conn_id conn_id)
    : host_(host),
      conn_id_(conn_id),
      chain_(nullptr),
      chain_index_(0),
      advancing_(false),
      next_pending_(false),
      chain_finished_(false),
      finished_(false),
      aborted_(false),
      hold_count_(0),
      pending_release_(false),
      user_data_type_(nullptr) {}

std::shared_ptr<uvcpp_web_context> uvcpp_web_context::create(
    uvcpp_web_context_host& host, uvcpp_web_conn_id conn_id) {
  // 不能用 std::make_shared：构造函数是私有的，而 create() 是成员所以能 new。
  return std::shared_ptr<uvcpp_web_context>(
      new uvcpp_web_context(host, conn_id));
}

// =========================================================================
// 连接
// =========================================================================

bool uvcpp_web_context::connection_alive() const {
  return host_.connection(conn_id_) != nullptr;
}

uvcpp_tcp_client* uvcpp_web_context::raw_client() {
  return host_.connection(conn_id_);
}

// =========================================================================
// 线程
// =========================================================================

bool uvcpp_web_context::on_loop_thread() const {
  return host_.on_loop_thread();
}

void uvcpp_web_context::post(const std::function<void()>& fn) {
  if (!fn) return;
  // 已经在 loop 线程上就地跑：省一次投递。
  // 副作用是 fn 可能**同步**执行到这里面来，调用方不能假设自己在下一个
  // 循环迭代才被调用。
  if (host_.on_loop_thread()) {
    fn();
    return;
  }
  host_.post(fn);
}

uvcpp_loop* uvcpp_web_context::loop() const { return host_.loop(); }

// =========================================================================
// 生命周期
// =========================================================================

void uvcpp_web_context::hold() { ++hold_count_; }

void uvcpp_web_context::release() {
  if (hold_count_ == 0) {
    // 没有 hold 过却 release：调用方的计数错了。静默减成负数会让上下文
    // 永远不被回收，所以这里直接拒掉并报出来。
    UVCPP_LOG_WARN(log_category::CORE)
        << "context::release() 调用次数多于 hold()，已忽略";
    return;
  }

  --hold_count_;
  if (hold_count_ > 0 || !pending_release_) return;

  pending_release_ = false;

  UVCPP_LOG_DEBUG(log_category::CORE) << "context 最后一个 hold 已释放，收尾";
  // **之后不要再碰任何成员** —— host 很可能就在这一句里把最后一份
  // shared_ptr 还掉，对象随即析构。
  host_.context_finished(*this);
}

// =========================================================================
// 控制流
// =========================================================================

void uvcpp_web_context::run(const std::vector<uvcpp_web_handler>& chain) {
  if (chain_ != nullptr) {
    // 一个上下文只跑一条链。重复 run 会让 chain_index_ 这类状态对不上，
    // 而且第二次的响应根本没地方发。
    UVCPP_LOG_ERROR(log_category::CORE)
        << "context::run() 被调用了两次，第二次已忽略";
    return;
  }

  chain_       = &chain;
  chain_index_ = 0;

  UVCPP_LOG_DEBUG(log_category::REQUEST)
      << "开始派发：连接 " << conn_id_ << "，共 " << chain.size() << " 环";

  advance();
}

void uvcpp_web_context::advance() {
  // 链已经收尾之后再来调是**安全空操作**：异步 handler 里留着的 next 副本
  // 可能晚于响应发出才被调用，那是使用者的时序问题，不该把框架带崩。
  if (finished_) {
    UVCPP_LOG_DEBUG(log_category::REQUEST)
        << "链已结束，忽略这次 next()（连接 " << conn_id_ << "）";
    return;
  }

  // **同步调 next() 的落点**：已经在循环里了，只记账，由外层循环继续。
  // 这就是"不递归"的全部实现 —— 于是 50 层甚至 5000 层中间件也只吃一层栈。
  if (advancing_) {
    next_pending_ = true;
    return;
  }

  // 自己先取一份，保证整个循环（含 finish() 里 host 松手的那一刻）对象都活着。
  std::shared_ptr<uvcpp_web_context> self = shared_from_this();

  advancing_ = true;

  do {
    next_pending_ = false;

    // abort() / fail() 已经定了结局，别再往下跑。
    if (chain_finished_) break;

    if (chain_ == nullptr || chain_index_ >= chain_->size()) {
      chain_finished_ = true;
      break;
    }

    // 响应已经结束 → 后面的处理器不该再跑。
    // 这条比"没调 next"更硬：handler 既 end() 了又调 next() 是它自己的错，
    // 不能让后面的 handler 往一个已经序列化好的响应里继续写。
    if (resp_.ended()) {
      chain_finished_ = true;
      break;
    }

    bool kept_next = false;

    try {
      // 「handler 留没留 next」= 这次调用**前后** `self` 的引用数有没有变多。
      //
      // 留了 next 就等于留了一份 `self`（闭包里捕获着它），所以"变多"就是
      // "留了"。两个测量点的持有者集合完全一致（调用方自己的引用、局部
      // self、局部 next 闭包里那份），handler 的**按值参数**副本在它返回时
      // 就销毁了，不在任何一个测量点里 —— 所以差值只来自 handler 留下的副本。
      //
      // 为什么是**差值**而不是绝对数：留了 next 的 handler 恢复时**仍然握着**
      // 那份副本（它就是靠它恢复的），绝对数里一直含着它，于是"恢复之后的
      // 这一环"会被永远误判成"又留了一份"，链再也走不到收尾。差值把它抵消掉。
      //
      // next 走 `post()` 而不是直接 `advance()`：这样**从工作线程调也成立**。
      // 在 loop 线程上 post() 就是就地调用（多一层函数调用而已），行为与
      // 直接调完全一致；不在 loop 线程上则自动投回去。少了这一层，"从线程池
      // 回调里调 next()"就是跨线程跑链 —— 轻则数据竞争，重则在别的线程上
      // 把响应写出去。异步续跑是本框架的核心用法，这条不该是个坑。
      uvcpp_web_next next = uvcpp_web_next([self]() {
        self->post([self]() { self->advance(); });
      });

      const long   before = self.use_count();
      const size_t idx    = chain_index_++;
      (*chain_)[idx](req_, resp_, next);
      const long after = self.use_count();

      // handler 返回了：它留 next 了吗？
      //
      // 留了 → 它打算稍后自己调（异步续跑），链挂起，等它。
      // 没留 → 这个 handler 之后没有任何东西能让链动起来：
      //        要么它已经结束了响应，要么它忘了 —— 两种情况都该收尾。
      //
      // 用 `>` 而不是 `!=`：万一异步协作让引用数**变少**（比如 handler 里
      // 触发了 host 松手），宁可判定"没留"而收尾，也不要挂起一个已经没人
      // 能唤醒的请求。
      kept_next = (after > before);
    } catch (const std::exception& e) {
      fail(e.what());
      break;
    } catch (...) {
      fail("unknown exception");
      break;
    }

    if (next_pending_ || kept_next) continue;

    // 还有下一环，但这一环既不放行也没结束响应 —— 请求就此被吞掉。
    //
    // `!resp_.ended()` 这个条件不能少：**"结束响应并且不调 next()"正是
    // 短路中间件的标准写法**（鉴权失败回 401、缓存命中直接回），那种情况
    // 是链的正常终点、不是 bug。少了这个判断，每一次正常的短路都会收到
    // 一条"既没调 next() 也没结束响应"的警告 —— 而它说的两件事里有一件
    // 明明是做过的，这种警告比不打更坏。
    if (chain_index_ < chain_->size() && !resp_.ended()) {
      // 响应照样发（下面 finish 里那条 WARN 也会打）。
      UVCPP_LOG_WARN(log_category::REQUEST)
          << "第 " << chain_index_ << " 环既没调 next() 也没结束响应，"
          << "链在此中断（连接 " << conn_id_ << "）";
    }
    chain_finished_ = true;
  } while (next_pending_);

  advancing_ = false;

  // finish() 放在最后：它可能触发 host 松手，而 self 还在我们手里兜着。
  if (chain_finished_) finish();
}

// =========================================================================
// 流式请求体
// =========================================================================

uvcpp_web_stream* uvcpp_web_context::attach_stream(uint64_t total,
                                                   bool has_total) {
  if (stream_ != nullptr) return stream_.get();  // 幂等：重复挂不该换对象

  stream_.reset(new uvcpp_web_stream(*this));
  stream_->prime(total, has_total);
  req_.set_stream(stream_.get());
  return stream_.get();
}

void uvcpp_web_context::stream_deliver(const char* data, size_t len) {
  if (stream_ == nullptr) return;
  if (stream_->delivered_end() || stream_->aborted()) return;
  stream_->deliver_data(data, len);
}

void uvcpp_web_context::stream_end() {
  if (stream_ == nullptr) return;
  if (stream_->delivered_end() || stream_->aborted()) return;

  // 顺序不能反：先让用户把响应填好，再续跑链（链收尾会立刻把响应发出去）。
  stream_->deliver_end();
  if (stream_->aborted()) return;  // on_end 里 abort 了：它自己已经续过跑

  // 框架槽把用户的 on_end 扣住了（上传路由的落盘还没完）：**链继续挂着**，
  // 由 `release_end()` 收口。这里绝不能续跑 —— 链一收尾就按当前（还是空的）
  // 响应发出去，用户后面填的那个就再也发不出来了。
  if (stream_->end_deferred()) return;

  stream_resume_chain();
}

void uvcpp_web_context::stream_abort() {
  if (stream_ == nullptr) return;
  if (stream_->delivered_end() || stream_->aborted()) return;

  stream_->deliver_abort();
  stream_resume_chain();
}

void uvcpp_web_context::stream_resume_chain() {
  if (stream_ == nullptr) return;

  // **取走**，不是拷一份：`resume_` 捕获着本上下文（`self`），而本上下文持有
  // stream_ —— 留下副本就是 ctx → stream → resume_ → ctx 的环，链跑完了上下文
  // 也永远不会析构。取走之后环断开，`finish()` 里 host 一松手就真回收了。
  uvcpp_web_next resume;
  if (!stream_->take_resume(&resume)) return;

  // resume() 内部走的是 `post()`：在 loop 线程上就地执行 `advance()`，
  // 于是链从挂起处续跑、发现没有下一环、收尾并发送响应。本对象的所有权在
  // 调用方手里（HTTP 层的流式 handler 捕获着 shared_ptr），所以即便
  // `context_finished()` 把 inflight_ 里那份摘了，这里也不会悬垂。
  resume();
}

void uvcpp_web_context::abort() {
  if (finished_) return;

  aborted_        = true;
  chain_finished_ = true;

  // 在 handler 里调的：交给外层循环收尾（不能在这里 finish，那样会在链的
  // 循环还没退栈的时候就把对象交出去）。
  if (advancing_) return;

  finish();
}

void uvcpp_web_context::fail(const std::string& what) {
  // 先定结局，再处理 —— 错误处理器自己也可能抛。
  chain_finished_ = true;

  if (resp_.has_error_handler()) {
    try {
      resp_.error_handler()(req_, resp_, what);
      return;
    } catch (const std::exception& e) {
      UVCPP_LOG_ERROR(log_category::REQUEST)
          << "错误处理器自己抛了异常: " << e.what();
    } catch (...) {
      UVCPP_LOG_ERROR(log_category::REQUEST) << "错误处理器自己抛了异常";
    }
  }

  UVCPP_LOG_ERROR(log_category::REQUEST)
      << "处理器抛出异常，已回 500（连接 " << conn_id_ << "）: " << what;

  // 默认兜底：500。这里**不吞异常**（异常已经在这一层被接住了），但也不
  // 往上抛 —— 抛出去就是 libuv 回调里逃逸的异常，那是整个进程的事。
  //
  // 不用先 clear_body()：下面 text() 里 body() 的第一步就是 clear()，写了一半
  // 的业务数据不会留在 500 的正文里。
  resp_.status(500);
  resp_.text("Internal Server Error");
  resp_.end();
}

void uvcpp_web_context::finish() {
  // 幂等保护。**调用方已经各自挡住了重复调用**（`advance()` 认 `finished_`、
  // `abort()` 也认），所以这一句目前是够不着的 —— 留着是因为 Phase 1.9 的
  // 闲置超时会成为第三个调用方。改这里之前先确认新调用方也做了判断。
  if (finished_) return;
  finished_ = true;

  if (aborted_) {
    UVCPP_LOG_DEBUG(log_category::REQUEST)
        << "请求已 abort，不发响应，交由 host 关闭连接 " << conn_id_;
    host_.abort_request(*this);
  } else {
    if (!resp_.ended()) {
      // 链跑完了却没人结束响应。**照样发** —— 响应对象里可能已经被填好了
      // （比如 handler 只设了 body 忘了 end()），丢了它反而是更坏的结果。
      UVCPP_LOG_WARN(log_category::REQUEST)
          << "处理器链结束但没有结束响应，按现状发送（status "
          << resp_.status_code() << "，连接 " << conn_id_ << "）";
    }
    host_.send_response(*this);
  }

  if (hold_count_ > 0) {
    // 还有人钉着这个上下文（多半是异步工作还没回来）。等 release()。
    pending_release_ = true;
    UVCPP_LOG_DEBUG(log_category::CORE)
        << "链已收尾但还有 " << hold_count_ << " 个 hold，暂不回收上下文";
    return;
  }

  // **之后不要再碰任何成员。**
  host_.context_finished(*this);
}

}  // namespace uvcpp
