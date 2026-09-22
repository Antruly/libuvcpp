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
  if (abandoned_) {
    // 属主在自己的回调里析构，会话已经交出去了（见 abandon()）。这里**连
    // `drain_async_` 都不能删**：它挂在那个正跑着的循环上，`delete` 会去
    // 碰一个正在执行的句柄。一起留给循环。
    drain_async_ = nullptr;
    return;
  }
  recycle_all();

  // `delete` 一个 async 句柄只是**发起**释放（走 uv_close，底层内存在完成
  // 回调里还回去），而且绝不能在自己的回调里做 —— 那会析构正在执行的
  // `std::function`。
  delete drain_async_;
  drain_async_ = nullptr;
}

void uvcpp_ws_sessions::abandon() {
  abandoned_ = true;
  sessions_.clear();
  retired_.clear();
  live_.store(0);
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
  // 起始状态是 **unref**：此刻退休表是空的，这个句柄没有任何事要办，而它默认
  // 会把循环算成"还活着"（`uv_run` 的存活判据是 `active_handles > 0`，libuv
  // `uv-common.h`）—— 于是一个**没事可做**的循环也永远不返回。
  //
  // 实测（2026-09-17，改前）：框架层客户端不开重连、对端走掉之后，循环上
  // **只剩这一个** async 句柄（uv_walk 普查：`async 活跃`），
  // `uv_run(UV_RUN_NOWAIT)` 连拨 200 轮全部返回非 0、`uv_loop_alive() == 1`，
  // 于是 `uvcpp_web_ws_client::run(UV_RUN_DEFAULT)` 那个"只在 rc == 0 时才
  // break"的泵循环永远出不来 —— 而它的头文件示例写着"循环在没有活句柄时自然
  // 返回"。
  //
  // **不是一直 unref**：退休表里有东西时它必须重新 **ref**（见 `on_retired()`），
  // 否则"醒来把待回收的会话删掉"这件事可能永远不再发生 —— `uv_run` 是在进
  // while 体**之前**算存活的，存活为 0 时那一轮连 poll 都不做，挂起的唤醒请求
  // 就没人取。实测（同一天，只 unref 不 ref 的版本）：`web_ws_ownership_func`
  // 的「client-side session recycled」与 `web_ws_client_api_func` 的「[3] close()
  // 之后会话必须被终结并回收」双双变红 —— 拨 3000 轮也没人回收。
  //
  // 所以规则是一句话：**这个句柄只在自己手上有活时才算数**。唤醒本身与 ref
  // 无关（`uv_async_send()` 该踢还是踢，libuv 的 unref 只清 `UV_HANDLE_REF`
  // 并减 `active_handles`）。服务器那边不受影响 —— 那个循环由监听句柄保活。
  a->unref();
  drain_async_ = a;
}

void uvcpp_ws_sessions::adopt(uvcpp_ws_connection* c) {
  if (c == nullptr) return;
  // 必须**在 start() 之前**装好：start() 会把关闭观察者挂到 TCP 客户端上，
  // 而对端如果已经关了连接，观察者可能立刻就回调 —— 那时会话需要知道该把
  // 自己交给谁。
  c->set_retire_callback([this](uvcpp_ws_connection* s) { on_retired(s); });
  sessions_.push_back(c);
  live_.store(sessions_.size());
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
  live_.store(sessions_.size());

  // 观察者在**账已经记好之后**调用：它看到的表必须是一致的（活动表里没有它、
  // pending 里有它），否则属主按 `size()` 对账会算错。会话这时候还在（回收是
  // 延迟的），但已经 `is_open() == false` —— 观察者只该更新自己的状态。
  if (retire_observer_) retire_observer_(c);

  // 唤醒循环去回收。没有句柄（循环不可用 / 装不上）就等 recycle_all()。
  if (drain_async_ != nullptr) {
    // 先 **ref** 再 send：手上有活了，这一轮必须真的转起来。只 send 不 ref
    // 的话，循环上要是别的什么都不剩，`uv_run` 会因为存活为 0 **根本不进
    // while 体**（它在进循环之前就算一次），挂起的唤醒请求没人取，回收就永远
    // 不发生 —— 见 `set_loop()` 里那段实测记录。
    drain_async_->ref();
    drain_async_->send();
  }
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
    // 原子自增：`recycled()` 是跨线程读数（与 `live_` 同一条边界）。
    recycled_.fetch_add(1);
  }

  // 活干完了就把 ref 还回去 —— 于是"这个句柄保活"与"退休表非空"是同一件事。
  // 放在**删完之后**问，是因为上面那些 `delete` 会跑用户回调，回调里还可能
  // 再关一个会话（那条路又 `ref()` + `send()` 了），此时就该让它继续活着。
  if (retired_.empty() && drain_async_ != nullptr) drain_async_->unref();
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
  live_.store(sessions_.size());
}

void uvcpp_ws_sessions::recycle_all() {
  // 当场终结：把会话从活动表摘出来（`terminate()` 会经 on_retired 送进
  // retired_，所以先摘出来再终结，避免边遍历边改 sessions_）。
  std::vector<uvcpp_ws_connection*> live;
  live.swap(sessions_);
  for (size_t i = 0; i < live.size(); ++i) live[i]->terminate();
  live_.store(sessions_.size());
  drain();
}

size_t uvcpp_ws_sessions::size() const { return live_.load(); }
size_t uvcpp_ws_sessions::pending() const { return retired_.size(); }
size_t uvcpp_ws_sessions::recycled() const { return recycled_.load(); }

const std::vector<uvcpp_ws_connection*>& uvcpp_ws_sessions::all() const {
  return sessions_;
}

}  // namespace uvcpp
#endif  // UVCPP_WEB_ENABLE
