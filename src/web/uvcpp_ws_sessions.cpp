#include <web/uvcpp_ws_sessions.h>

#if UVCPP_WEB_ENABLE

#include <handle/uvcpp_async.h>

namespace uvcpp {

uvcpp_ws_sessions::uvcpp_ws_sessions() {}

uvcpp_ws_sessions::~uvcpp_ws_sessions() {
  // 兜底：属主正常路径上已经在关 loop 之前调过 shutdown()，这里是最后一道。
  shutdown();
}

void uvcpp_ws_sessions::shutdown() {
  recycle_all();

  // `delete` 一个 async 句柄只是**发起**释放（走 uv_close，底层内存在完成
  // 回调里还回去），而且绝不能在自己的回调里做 —— 那会析构正在执行的
  // `std::function`。
  delete drain_async_;
  drain_async_ = nullptr;
}

void uvcpp_ws_sessions::set_loop(uvcpp_loop* loop) {
  if (drain_async_ != nullptr || loop == nullptr) return;
  uvcpp_async* a = new uvcpp_async();
  if (a->init([this](uvcpp_async*) { drain(); }, loop) != 0) {
    // 装不上就退回同步路径：会话照旧不丢，只是回收要等 recycle_all()。
    // （与 uvcpp_web_app 对 init 失败的处理一致。）
    delete a;
    return;
  }
  drain_async_ = a;
}

void uvcpp_ws_sessions::adopt(uvcpp_ws_connection* c) {
  if (c == nullptr) return;
  // 必须**在 start() 之前**装好：start() 会把关闭观察者挂到 TCP 客户端上，
  // 而对端如果已经关了连接，观察者可能立刻就回调 —— 那时会话需要知道该把
  // 自己交给谁。
  c->set_retire_callback([this](uvcpp_ws_connection* s) { on_retired(s); });
  sessions_.push_back(c);
}

void uvcpp_ws_sessions::set_retire_observer(
    std::function<void(uvcpp_ws_connection*)> cb) {
  retire_observer_ = std::move(cb);
}

void uvcpp_ws_sessions::on_retired(uvcpp_ws_connection* c) {
  for (size_t i = 0; i < sessions_.size(); ++i) {
    if (sessions_[i] == c) {
      sessions_.erase(sessions_.begin() + static_cast<long>(i));
      break;
    }
  }
  retired_.push_back(c);

  // 观察者在**账已经记好之后**调用：它看到的表必须是一致的（活动表里没有它、
  // pending 里有它），否则属主按 `size()` 对账会算错。会话这时候还在（回收是
  // 延迟的），但已经 `is_open() == false` —— 观察者只该更新自己的状态。
  if (retire_observer_) retire_observer_(c);

  // 唤醒循环去回收。没有句柄（循环不可用 / 装不上）就等 recycle_all()。
  if (drain_async_ != nullptr) drain_async_->send();
}

void uvcpp_ws_sessions::drain() {
  // 先整体换出来再删：`delete` 会跑用户那些发送完成回调（在
  // `notify_retired()` 里以错误结算），那些回调可能又建会话、又关会话。
  // 边遍历边被改会出事，而且新终结的会话留给下一轮更安全 —— 否则一个
  // "关掉会话又立刻建新会话"的回调就能让这个循环永远转下去。
  std::vector<uvcpp_ws_connection*> batch;
  batch.swap(retired_);
  for (size_t i = 0; i < batch.size(); ++i) {
    delete batch[i];
    ++recycled_;
  }
}

void uvcpp_ws_sessions::close_all(ws_close_code code) {
  std::vector<uvcpp_ws_connection*> live;
  live.swap(sessions_);
  for (size_t i = 0; i < live.size(); ++i) {
    live[i]->close(code);
    // `close()` 只是**发起**：帧要发出去、连接要关掉，终结回调才会来。
    // 还没终结的放回表里 —— 放回去才不会漏，等终结回调或 recycle_all()。
    if (live[i]->is_open()) sessions_.push_back(live[i]);
  }
}

void uvcpp_ws_sessions::recycle_all() {
  // 当场终结：把会话从活动表摘出来（`terminate()` 会经 on_retired 送进
  // retired_，所以先摘出来再终结，避免边遍历边改 sessions_）。
  std::vector<uvcpp_ws_connection*> live;
  live.swap(sessions_);
  for (size_t i = 0; i < live.size(); ++i) live[i]->terminate();
  drain();
}

size_t uvcpp_ws_sessions::size() const { return sessions_.size(); }
size_t uvcpp_ws_sessions::pending() const { return retired_.size(); }
size_t uvcpp_ws_sessions::recycled() const { return recycled_; }

const std::vector<uvcpp_ws_connection*>& uvcpp_ws_sessions::all() const {
  return sessions_;
}

}  // namespace uvcpp
#endif  // UVCPP_WEB_ENABLE
