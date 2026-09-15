/**
 * @file src/webapp/uvcpp_web_stream.cpp
 * @brief uvcpp_web_stream 的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <webapp/uvcpp_web_stream.h>

#include <string>

#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_web_context.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

namespace uvcpp {

uvcpp_web_stream::uvcpp_web_stream(uvcpp_web_context& ctx)
    : ctx_(&ctx),
      data_cb_(),
      end_cb_(),
      abort_cb_(),
      progress_cb_(),
      hooks_(),
      resume_(),
      received_(0),
      total_(0),
      has_total_(false),
      delivered_end_(false),
      aborted_(false),
      paused_(false),
      end_deferred_(false),
      end_released_(false) {}

uvcpp_web_stream::~uvcpp_web_stream() {}

// =========================================================================
// 读状态 / 回调
// =========================================================================

void uvcpp_web_stream::on_data(uvcpp_web_stream_data_cb cb) {
  data_cb_ = std::move(cb);
}

void uvcpp_web_stream::on_end(uvcpp_web_stream_end_cb cb) {
  end_cb_ = std::move(cb);
}

void uvcpp_web_stream::on_abort(uvcpp_web_stream_abort_cb cb) {
  abort_cb_ = std::move(cb);
}

void uvcpp_web_stream::on_progress(uvcpp_web_stream_progress_cb cb) {
  progress_cb_ = std::move(cb);
}

void uvcpp_web_stream::set_framework_hooks(const uvcpp_web_stream_hooks& hooks) {
  hooks_ = hooks;
}

// =========================================================================
// 背压
// =========================================================================

bool uvcpp_web_stream::pause() {
  // 走 `raw_client()` 而不是缓存一个 `uvcpp_tcp_client*`：它每次都查登记表，
  // 连接断开后返回 nullptr —— 这正是"异步回调拿着旧指针往别人的连接上操作"
  // 那个缺陷的结构性解法，背压这条路同样需要它。
  uvcpp_tcp_client* c = ctx_->raw_client();
  if (c == nullptr) return false;
  c->read_pause();
  paused_ = true;
  return true;
}

bool uvcpp_web_stream::resume() {
  if (!paused_) return true;  // 没暂停过：空操作，别去动底层的读状态
  uvcpp_tcp_client* c = ctx_->raw_client();
  if (c == nullptr) return false;
  c->read_resume();
  paused_ = false;
  return true;
}

// =========================================================================
// 结束
// =========================================================================

void uvcpp_web_stream::abort(int status) {
  if (aborted_ || delivered_end_) return;
  aborted_ = true;

  // **两个中止槽都要跑，框架槽在前。** 这一步不是"顺手通知一下"，两条投递
  // 路径的收尾完全挂在它们身上：
  //
  //   - 框架槽（`uvcpp_web_upload.cpp` 的 `on_abort`）→ `notify_parse_done(false)`
  //     → `fail()` → 关掉 fd、**删掉本次创建的临时文件**。少了它，被上限拦下的
  //     那次上传会把半截文件留在盘上，直到上下文被析构时才由
  //     `~uvcpp_web_upload` 的兜底分支删掉 —— 而"上下文什么时候析构"不由这里
  //     决定，那是把一条契约的实现押在一个无关的时序上。
  //     `uvcpp_web_upload.cpp` 里"abort 会同步走到流层的 on_abort"那句注释
  //     写的就是这个契约，本句是它的落地。
  //   - 用户槽：`on_abort` 的文档是"对端断开 / **框架掐断**"，而上限拦截正是
  //     框架掐断。用户在那里清业务状态（半条数据库记录、计数……），不通知
  //     就是让它漏。
  //
  // 顺序与 `deliver_abort()` 一致：框架先清自己的状态，用户再清业务状态。
  // 由此 `on_data` 返回 false 与自己调 `abort(413)` 也变得**完全等价** ——
  // 头文件里那句"返回 false 等价于 abort(413)"本来就是这么承诺的。
  if (hooks_.on_abort) hooks_.on_abort();
  if (abort_cb_) abort_cb_();

  uvcpp_web_response& r = ctx_->response();
  r.status(status);
  // **必须显式要求关闭。** 剩下的 body 没人消费了，复用这条连接只会让下一个
  // 请求读到上一个上传的残留字节 —— 那是纯粹的数据损坏，而且只在"上传被拒"
  // 之后才发生，极难复现。内容长度由序列化层按 body 重算。
  r.set_header("connection", "close");

  const char* reason = http_status_reason(static_cast<http_status>(status));
  r.text(std::string(reason));

  r.end();
  ctx_->stream_resume_chain();
}

// =========================================================================
// 逃生口
// =========================================================================

const uvcpp_web_request& uvcpp_web_stream::request() const {
  return ctx_->request();
}

uvcpp_web_request& uvcpp_web_stream::request() { return ctx_->request(); }

// =========================================================================
// 框架内部
// =========================================================================

void uvcpp_web_stream::prime(uint64_t total, bool has_total) {
  total_     = total;
  has_total_ = has_total;
}

void uvcpp_web_stream::deliver_data(const char* data, size_t len) {
  if (aborted_ || delivered_end_) return;

  received_ += len;

  // 进度先报：用户可能在这里发现"超出我自己的上限了"，然后 abort。
  // 报完要重新查一次状态 —— 回调里 abort 之后不该再往下走。
  if (progress_cb_) progress_cb_(received_, has_total_ ? total_ : 0);
  if (aborted_) return;

  // 框架槽先吃（上传路由的 multipart 解析器在这里）。顺序不能反：解析器可能
  // 当场判出畸形并 abort，那这一块就不该再交给用户的 on_data —— 让用户看到
  // 一个框架已经拒收的字节流，他会以为上传还在正常进行。
  if (hooks_.on_data) {
    hooks_.on_data(data, len);
    if (aborted_) return;
  }

  if (!data_cb_) return;

  const bool keep_going = data_cb_(data, len);
  if (aborted_) return;  // data 回调里自己 abort 了，别覆盖它的状态码

  if (!keep_going) {
    // 回调说"到此为止"。按 413 收场：绝大多数场景就是超限，而框架没法知道
    // 用户心里想的是哪个码 —— 用户可以自己先调 abort(别的码)，那就走到了
    // 上面那个 return，不会被这里覆盖。
    abort(static_cast<int>(http_status::PAYLOAD_TOO_LARGE));
  }
}

void uvcpp_web_stream::deliver_end() {
  if (aborted_ || delivered_end_) return;
  delivered_end_ = true;

  // 框架槽先跑。它回 true 表示"用户的 on_end 我先扣着"：上传路由到达这里时
  // 落盘只写到了内存，`fsync` + `close` 还在线程池上，而用户在 `on_end` 里
  // 就要读 `req.upload()` —— 先放他进来会让他读到一个还没填的结果。
  //
  // **先扣住、再跑钩子**，顺序不能反：上传会话在"一个文件都没有"（纯字段
  // 表单、或者一个 0 字节部件都没有）时是**同步**跑完完成回调的，而那个回调
  // 会调 `release_end()`。如果那一刻 `end_deferred_` 还是假，这一放就什么都
  // 没放，用户的 `on_end` 永远不来 —— 请求挂到闲置超时才收场。
  end_deferred_ = true;
  if (!hooks_.on_end || !hooks_.on_end()) end_deferred_ = false;

  // 同步路径已经把用户槽放掉了（`end_released_` 为真）—— 别再放第二次，
  // 放两次就是发两个响应。
  //
  // 注意上面那句是 `||` 不是 `&&`：**没有框架槽时也必须清掉**（恒扣住 = 所有
  // 流式路由的 `on_end` 永不触发，而链会当场按空响应收尾）。这一处写错过一次，
  // `test_web_app_stream_func` 五组同时红。
  if (end_deferred_ || end_released_) return;

  if (end_cb_) end_cb_();
  // 这里**不**处理"回调里 abort"的情况：那是 `abort()` 自己的事，它已经把
  // 响应写完并续跑了链（见下面 take_resume 的注释）。
}

void uvcpp_web_stream::release_end() {
  if (!end_deferred_ || end_released_) return;  // 幂等：放行两次 = 发两个响应
  end_released_ = true;
  end_deferred_ = false;

  if (end_cb_) end_cb_();
  if (aborted_) return;  // on_end 里 abort 了：它自己已经续过跑

  ctx_->stream_resume_chain();
}

void uvcpp_web_stream::deliver_abort() {
  if (aborted_ || delivered_end_) return;
  aborted_ = true;

  // 框架槽先清自己的状态（把半截的临时文件删掉），用户槽再清业务状态。
  if (hooks_.on_abort) hooks_.on_abort();
  if (abort_cb_) abort_cb_();
}

void uvcpp_web_stream::bind_resume(const uvcpp_web_next& next) {
  resume_ = next;
}

bool uvcpp_web_stream::take_resume(uvcpp_web_next* out) {
  if (!resume_) return false;

  // 用 swap 而不是赋值：赋值会先销毁 `out` 里的旧值、再拷贝 —— 而拷贝意味着
  // **本对象仍然握着一份**，引用环没断。swap 之后 `resume_` 是空的。
  out->swap(resume_);
  resume_ = uvcpp_web_next();
  return true;
}

void uvcpp_web_stream::clear_resume() { resume_ = uvcpp_web_next(); }

}  // namespace uvcpp
