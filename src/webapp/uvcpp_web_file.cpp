/**
 * @file src/webapp/uvcpp_web_file.cpp
 * @brief `uvcpp_web_file_transfer` 的实现：`open → read → … → close` 的串行状态机。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 形状与 `uvcpp_web_upload` 完全同族，只是方向相反：那边是"来一块写一块"，
 * 这边是"要一块读一块"。全部并发都在**一个 loop 线程**上，所以本文件没有
 * 任何锁，也没有原子量 —— `uvcpp_fs` 的单笔在途守卫（`async_pending_`）保证
 * 了同一时刻只有一笔 fs 操作，状态机因此只需要一个状态变量。
 *
 * 阅读顺序：`start()` → `on_open_done()` → `on_read_done()` →（必要时 `resume()`）
 * → `submit_close()` → `on_close_done()` → `finish()`。
 */

#include <webapp/uvcpp_web_file.h>

#include <fcntl.h>

// uvcpp_fs.h 自己会把 <uv.h> 拉进来。
#include <req/uvcpp_fs.h>

#include <uv.h>

#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_work_limit.h>

// POSIX 上没有 O_BINARY（那是 Windows 的"不做 CRLF 翻译"标志）。
#ifndef O_BINARY
#define O_BINARY 0
#endif

namespace uvcpp {

namespace {

/// 读文件用 O_RDONLY；**O_BINARY 不能省** —— 少了它在 Windows 上会把
/// CRLF 翻译成 LF，于是"逐字节相等"这条断言在含 `\r\n` 的文件上必红，
/// 而失败信息看起来像"读错了几个字节"，与真正的原因（文本模式翻译）
/// 差得很远。这个坑在 `uvcpp_web_upload` 的写侧是对称的。
const int k_read_flags = O_RDONLY | O_BINARY;

/// 本头的策略是**不拉 libuv**（见头文件的前向声明说明），fd 因此用 `int`
/// 存 —— `uvcpp_web_upload` 同样这么做。这里补上调用点需要的转换。
///
/// 顺带记一句：本机 libuv 的 `uv_file` 就是 `typedef int uv_file`
/// （`_local_deps/libuv/include/uv/win.h:231`），所以这个转换在两个平台上
/// 都是恒等的。写成函数而不是散落的 `static_cast`，是为了让"这里是从
/// `int` 到 `uv_file` 的边界"只出现一次。
uv_file as_uv_file(int fd) { return static_cast<uv_file>(fd); }

}  // namespace

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_web_file_transfer::uvcpp_web_file_transfer(uvcpp_loop* loop,
                                                 uvcpp_web_file_sink* sink)
    : loop_(loop),
      sink_(sink),
      fs_(new uvcpp_fs()),
      slice_bytes_(k_default_slice_bytes),
      high_water_bytes_(k_default_high_water_bytes),
      stop_at_eof_(false),
      first_(0),
      last_(0),
      offset_(0),
      bytes_sent_(0),
      peak_window_(0),
      read_submits_(0),
      pause_events_(0),
      state_(st_idle),
      abort_requested_(false),
      close_status_(0),
      fd_(-1),
      gate_(nullptr),
      gate_held_(false),
      gate_wakeup_(0),
      gate_waits_(0) {
  // 切片缓冲按最大值一次性备好，之后**永不重分配** —— 每一笔 `uv_fs_read`
  // 都写进同一块内存，这正是"一片缓冲就够"的落地方式。
  slice_buf_.resize(slice_bytes_);
}

uvcpp_web_file_transfer::~uvcpp_web_file_transfer() {
  if (state_ != st_idle && state_ != st_done) {
    // 正常走不到这里：每一笔在途操作的完成回调都按值捕了一份
    // `shared_from_this()`，所以只要 I/O 还没跑完，transfer 就一定活着。
    // 真到了这里说明不变式破了（有人手工 delete、或状态机漏了一条收尾路径），
    // 而后果是 fd 泄漏 —— 值得吵一句，但不值得断言崩溃。
    UVCPP_LOG_WARN(log_category::DOWNLOAD)
        << "分片下发对象在 I/O 未结束时被析构（fd=" << fd_ << "，state=" << state_
        << "）—— fd 会随进程泄漏";
  }
}

// =========================================================================
// 配置与观测
// =========================================================================

void uvcpp_web_file_transfer::set_chunk_gate(uvcpp_web_work_limit* limit) {
  // **只在还没起跑时可以换闸门。** 跑起来之后 `gate_held_` 可能正代表"手里攥着
  // 一个旧闸门的名额"，换掉它就没有对象可还了（新闸门的 release 不是它的账）。
  // 调用点的次序本来就是"挂好再 start()"（`uvcpp_web_response` 那侧），所以
  // 这里把违约挡掉而不是去救它。
  if (state_ != st_idle) return;
  gate_ = limit;
}

size_t uvcpp_web_file_transfer::gate_waits() const { return gate_waits_; }

bool uvcpp_web_file_transfer::chunk_slot_held() const { return gate_held_; }

void uvcpp_web_file_transfer::set_slice_bytes(size_t n) {
  // 0 = "用默认值"（与 `set_max_upload_size` 那一族的约定一致：0 不是"零字节"，
  // 而是"没配"）。切片为 0 会让状态机原地打转，所以必须挡在这里。
  slice_bytes_ = (n == 0) ? k_default_slice_bytes : n;
  slice_buf_.resize(slice_bytes_);
}

void uvcpp_web_file_transfer::set_high_water_bytes(size_t n) {
  high_water_bytes_ = (n == 0) ? k_default_high_water_bytes : n;
}

size_t uvcpp_web_file_transfer::slice_bytes() const { return slice_bytes_; }

size_t uvcpp_web_file_transfer::high_water_bytes() const {
  return high_water_bytes_;
}

void uvcpp_web_file_transfer::set_stop_at_eof(bool on) { stop_at_eof_ = on; }

bool uvcpp_web_file_transfer::stop_at_eof() const { return stop_at_eof_; }

bool uvcpp_web_file_transfer::done() const { return state_ == st_done; }

bool uvcpp_web_file_transfer::cancelled() const { return abort_requested_; }

uint64_t uvcpp_web_file_transfer::bytes_sent() const { return bytes_sent_; }

size_t uvcpp_web_file_transfer::read_submits() const { return read_submits_; }

size_t uvcpp_web_file_transfer::pause_events() const { return pause_events_; }

size_t uvcpp_web_file_transfer::peak_window_bytes() const {
  return peak_window_;
}

size_t uvcpp_web_file_transfer::window_bytes() const {
  return slice_bytes_ + (sink_ == nullptr ? 0u : sink_->backlog());
}

void uvcpp_web_file_transfer::note_window() {
  const size_t w = window_bytes();
  if (w > peak_window_) peak_window_ = w;
}

// =========================================================================
// 启动
// =========================================================================

int uvcpp_web_file_transfer::start(const std::string& path, uint64_t first,
                                   uint64_t last) {
  if (state_ != st_idle) return UV_EINVAL;
  if (loop_ == nullptr || sink_ == nullptr) return UV_EINVAL;

  path_ = path;
  first_ = first;
  last_ = last;
  offset_ = first;
  state_ = st_opening;
  note_window();

  std::shared_ptr<uvcpp_web_file_transfer> self = shared_from_this();
  const int rc = fs_->open(loop_, path_.c_str(), k_read_flags, 0,
                           [self](uvcpp_fs* f) {
                             self->on_open_done(
                                 static_cast<long long>(f->get_result()));
                           });
  if (rc != 0) {
    // **同步失败：libuv 根本没排队，完成回调永远不来。**
    // 按头文件的契约，这条路 `return` 而不回调 —— 与 libuv「同步失败不排队」
    // 的约定一致，免得同一个失败被通知两次。调用方看到非 0 就丢掉自己那份
    // `shared_ptr` 即可。
    //
    // 状态置 `st_done` 有两个作用：让析构不再打"未结束就析构"的警告（这是
    // 一条**已知且已说明**的结束路径），也让后续的 `cancel()`/`resume()` 变成
    // 空操作。
    //
    // 如实记一处代价：`uvcpp_fs::open` 在同步失败时虽然清了 `async_pending_`，
    // 但**没有清 `fs_open_cb`**（`uvcpp_fs.cpp:255-263`），而那个闭包按值捕了
    // `self` —— 于是 `transfer → fs_ → fs_open_cb → transfer` 成环，这一份
    // `shared_ptr` 再也回不来（`uvcpp_web_upload.cpp` 里记的是同一个取舍：
    // 用一个内存泄漏换一次 use-after-free 不划算）。**实际上走不到**：本函数
    // 已经挡掉了 state/loop/sink 三种非法输入，而 `path_.c_str()` 永远有效，
    // 剩下的同步失败源只有 `UV_EALREADY` —— 本模块从不并发提交，也拿不到。
    state_ = st_done;
    return rc;
  }
  return 0;
}

void uvcpp_web_file_transfer::release_chunk_slot() {
  if (gate_ == nullptr) return;
  if (gate_wakeup_ != 0) {
    gate_->remove_wakeup(gate_wakeup_);
    gate_wakeup_ = 0;
  }
  if (!gate_held_) return;
  gate_held_ = false;
  // 这一句可能**同步**跑别的传输的唤醒回调（它们会在这条栈上 acquire + 起读）。
  // 本对象此刻的成员已经收拾干净（名额已还、唤醒已注销），所以它们是安全的。
  gate_->release();
}

void uvcpp_web_file_transfer::resume() {
  if (state_ != st_paused) return;
  if (abort_requested_) return;  // 已经决定不发了，再读就是白读
  // `submit_read()` 必须是最后一句：它可能同步失败并当场把 `on_done` 发出去，
  // 而那时对象可能已经不存在了。
  submit_read();
}

void uvcpp_web_file_transfer::cancel() {
  if (state_ == st_idle || state_ == st_done) return;
  abort_requested_ = true;
  if (state_ == st_paused) {
    // **这一支不能省。** 因背压停下时**没有任何 fs 操作在途**，所以"等当前
    // 这一笔跑完再关"这个说法在这里没有对象 —— 不自己推进，状态机就永远停在
    // `st_paused`，`on_done` 一次都不来，调用方等一个永不到来的通知，fd 也
    // 一直开着。其余状态都有在途操作，回调回来时自然会看到 `abort_requested_`。
    submit_close(UV_ECANCELED);
  }
}

// =========================================================================
// 各状态的推进
// =========================================================================

void uvcpp_web_file_transfer::on_open_done(long long rc) {
  if (rc < 0) {
    // 打开就失败：**没有 fd 可关**，直接收尾。这里不单独看 `abort_requested_`
    // —— 两者都以非 0 结束，而"文件根本没打开"是更准确的原因。
    finish(static_cast<int>(rc));
    return;
  }

  fd_ = static_cast<int>(rc);
  note_window();

  if (abort_requested_) {
    // 打开期间被取消 —— 但 **fd 已经到手了**，不走 `finish()` 就会漏一个 fd。
    // 这一支是 `cancel()` 与 `start()` 之间那个窗口的唯一出口。
    submit_close(UV_ECANCELED);
    return;
  }
  if (offset_ > last_) {
    // 空区间（`first > last`）。头文件承诺过它照常走 open（好让 ENOENT 之类的
    // 错误还能上报），所以到这里要**立刻收尾**，一个字节都不读。
    submit_close(0);
    return;
  }
  submit_read();
}

void uvcpp_web_file_transfer::submit_read() {
  // ---- 块级名额：读盘之前拿，读完立刻还 ----
  //
  // 位置就在"真要提交一笔 `uv_fs_read`"的前一句 —— 这是**唯一**一处提交读的
  // 地方，所以名额的粒度严格等于"一次读盘"，不多不少。
  //
  // **拿不到不是失败**：停在块边界上（`st_paused`，此刻没有任何 fs 操作在途），
  // 向闸门登记一次唤醒，等它叫。这条路上没有 503。
  if (gate_ != nullptr && !gate_held_) {
    if (!gate_->acquire()) {
      ++gate_waits_;
      state_ = st_paused;
      if (gate_wakeup_ == 0) {
        // `weak_ptr` 而不是 `shared_ptr`：闸门的唤醒表**替持有者续命**，
        // 用 `shared_ptr` 就是一条 transfer → 闸门 → transfer 的环。
        std::weak_ptr<uvcpp_web_file_transfer> w = shared_from_this();
        gate_wakeup_ = gate_->add_wakeup([w]() {
          std::shared_ptr<uvcpp_web_file_transfer> s = w.lock();
          if (!s) return;
          // 唤醒是**一次性**的（条目在回调前就被摘了）⇒ 先清 id 再续做；
          // 续做里若又拿不到名额，会重新登记一条。
          s->gate_wakeup_ = 0;
          s->resume();
        });
      }
      return;
    }
    gate_held_ = true;
  }

  // 进到这里 `offset_ <= last_` 已经成立（每一处调用点都判过），所以
  // `last_ - offset_` 不会下溢，`+ 1` 是闭区间的"还差多少字节"。
  const uint64_t remain = last_ - offset_ + 1u;
  uint64_t want = static_cast<uint64_t>(slice_bytes_);
  if (remain < want) want = remain;
  // `want` 已经 <= slice_bytes_ == slice_buf_.size()，这句是纵深防御：
  // 有人改了 `set_slice_bytes` 却没同步缓冲大小时，宁可少读也不能越界写。
  if (want > static_cast<uint64_t>(slice_buf_.size())) {
    want = static_cast<uint64_t>(slice_buf_.size());
  }
  if (want == 0) {
    // 到不了：slice_bytes_ 恒 >= 1，remain 恒 >= 1。
    submit_close(0);
    return;
  }

  state_ = st_reading;
  ++read_submits_;

  // `&slice_buf_[0]` 而不是 `slice_buf_.data()`：C++11 下只有前者是形式可写的。
  uv_buf_t b = uv_buf_init(&slice_buf_[0], static_cast<unsigned int>(want));

  std::shared_ptr<uvcpp_web_file_transfer> self = shared_from_this();
  const int rc =
      fs_->read(loop_, as_uv_file(fd_), &b, 1, static_cast<int64_t>(offset_),
                [self](uvcpp_fs* f) {
                  self->on_read_done(static_cast<long long>(f->get_result()));
                });
  if (rc != 0) {
    // 与 `start()` 同一条道理：同步失败不排队，得自己收尾。
    // 这一支**必须**回调（不像 `start()`）—— 调用方已经在等 `on_done` 了，
    // 静默丢掉会让它永远等下去。`submit_close` 是最后一句。
    submit_close(rc);
  }
}

void uvcpp_web_file_transfer::on_read_done(long long nread) {
  // **第一句就还名额**：这一笔读已经跑完，闸门封的是"并行的读盘"，不是"在跑的
  // 传输"。还在这儿而不是等到收尾：短读/出错/取消都走本函数，放第一句就没有
  // 任何一条分支能漏掉它。
  //
  // 这一句可能同步叫醒别的传输（在那条栈上 acquire + 起读）。`this` 在本函数
  // 期间一定活着 —— 读的完成回调按值捕了一份 `shared_from_this()`。
  release_chunk_slot();
  note_window();

  if (abort_requested_) {
    submit_close(UV_ECANCELED);
    return;
  }
  if (nread < 0) {
    submit_close(static_cast<int>(nread));
    return;
  }
  if (nread == 0) {
    // 读到文件尾了。**它算不算"正常结束"，取决于有没有人声明过长度**：
    //
    // - `stop_at_eof_` 关（默认）：调用方声明过 `[first, last]`，而区间还没
    //   走完 —— 文件比声明时短了（被截断、或被换掉了）。**这不是"正常结束"**：
    //   头部已经带着 `Content-Length` 上线了，悄悄少发几个字节等于交给客户端
    //   一个长度对不上的 body，而客户端要到读完才发现。明确报错，让上层截断
    //   并关连接。
    // - `stop_at_eof_` 开：**没有任何长度被声明过**（上层走的是 chunked），
    //   EOF 就是这条流的天然终点，正常收尾。
    //
    // 两支里 `submit_close` 都必须是最后一句：它可能同步把 `on_done` 发出去，
    // 而那时本对象可能已经不存在了。
    submit_close(stop_at_eof_ ? 0 : static_cast<int>(UV_EOF));
    return;
  }

  const size_t n = static_cast<size_t>(nread);
  offset_ += n;
  bytes_sent_ += n;

  // 交付。**这一句里 sink 可能调 `cancel()`**（对端在这一句里断了是常态），
  // 所以返回之后必须重新判一次，不能假设状态没变。
  sink_->on_data(slice_buf_.data(), n);
  note_window();

  if (abort_requested_) {
    submit_close(UV_ECANCELED);
    return;
  }
  if (offset_ > last_) {
    submit_close(0);
    return;
  }
  if (sink_->backlog() >= high_water_bytes_) {
    // 停读。**这一步之后没有任何 fs 操作在途** —— 唤醒只有 `resume()` 一条路，
    // 所以 `cancel()` 也必须自己处理这个状态（见那里的注释）。
    ++pause_events_;
    state_ = st_paused;
    return;
  }
  // `submit_read()` 必须是最后一句，理由同 `resume()`。
  submit_read();
}

void uvcpp_web_file_transfer::submit_close(int status) {
  // 纵深防御：走到关闭路径时名额**本该**已经还了（`on_read_done` 第一句）。
  // 留着这一句是因为"漏还"的后果（闸门越来越紧、最后全 503）和一处忘掉不成
  // 比例 —— 幂等，重复调用无副作用。
  release_chunk_slot();
  close_status_ = status;
  state_ = st_closing;

  if (fd_ < 0) {
    // 没有 fd 可关（open 失败那条路）。
    finish(status);
    return;
  }

  const int fd = fd_;
  // fd 已经在关的路上了，先作废成员：这样"关完之后又有人读 `fd_`"这种错
  // 不会悄悄地读到过期值，析构里的警告也不会把一次正常关闭报成泄漏。
  fd_ = -1;

  std::shared_ptr<uvcpp_web_file_transfer> self = shared_from_this();
  const int rc = fs_->close(loop_, as_uv_file(fd), [self](uvcpp_fs* f) {
    (void)f;  // `close` 的回调里只认我们自己记下的 `close_status_`
    self->on_close_done(self->close_status_);
  });
  if (rc != 0) {
    // 关不上也要收尾（否则调用方永远等不到 `on_done`）。fd 会漏，但这是
    // "close 提交都失败了"这种已经没什么可做的局面。
    on_close_done(status);
  }
}

void uvcpp_web_file_transfer::on_close_done(int status) {
  // `finish()` 必须是最后一句：`on_done` 很可能就是最后一次 `shared_ptr`
  // 释放的地方，之后本对象已经不存在。
  finish(status);
}

void uvcpp_web_file_transfer::finish(int status) {
  if (state_ == st_done) return;
  // 同样必须在"不得再碰任何成员"那句之前（它要碰 `gate_*`）。
  release_chunk_slot();
  state_ = st_done;
  // **这一句之后不得再碰任何成员。**
  sink_->on_done(status, bytes_sent_);
}

}  // namespace uvcpp
