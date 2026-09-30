/**
 * @file src/capi/uvcpp_c_net.cpp
 * @brief `uvcpp_c_net.h` 的实现：TCP 客户端 / 服务端的 C 门面。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这个文件里有三件事值得先读一遍（其余都是逐条对着 C++ 方法搬的）
 * ----------------------------------------------------------------------
 * 1. **包装器的死期由"关闭观察者"决定，不由调用方决定。**
 *    服务端交出来的每条连接，它那个 `uvcpp_tcp_client` 对象是**框架**持有并
 *    `delete` 的（`uvcpp_tcp_server` 的管理槽）。所以我们在造包装器的时候挂一个
 *    `add_close_observer()`：按它的文档，观察者排在**用户槽之后、管理槽之前**
 *    —— 也就是"连接确实已经关了、客户端对象还活着"的那一刻。在那一点把包装器
 *    从活句柄表里摘掉，于是 C 侧永远不会拿到一个"底层对象已经被 delete"的句柄：
 *    它拿到的是 `UVCPP_C_E_STALE`，不是段错误。这是 `uvcpp_c_net.h` 开头那句
 *    "连接句柄在断开时会被本层回收"的实现。
 *
 * 2. **回调期间不许删任何人。** 用户回调里可能 `close()` 一条连接（观察者会在
 *    同一层栈里回调回来），也可能调 `free()`。所以每个跳板进用户代码前把
 *    `cb_depth` 加一、出来减一；`reap_client()` 看见任一计数非零就把包装器先
 *    放进"坟场"，等最外层跳板出栈时再 `delete`。`free()` 在回调里调则直接返回
 *    `UVCPP_C_E_STATE` —— 与头里那句"不要在回调里 free"是同一件事，只是这一版
 *    由代码兜住，不靠自觉。
 *
 * 3. **读回调不能装早。** `uv_read_start()` 在一条还没连上的流上返回
 *    `UV_ENOTCONN`（`_local_deps/libuv/src/unix/stream.c` 的
 *    `!(stream->flags & UV_HANDLE_READABLE)` 那一行），而"装读回调"正是这一层
 *    最自然会被写在 `connect()` **之前**的一步。所以 `set_events()` 只把回调
 *    记在包装器里，真正的 `read_start_events()` 由 `maybe_arm_read()` 在
 *    **连接建立之后**补上（异步 connect 的完成回调、`connect_wait()` 返回、
 *    以及服务端交出一条已经可读的连接时）。跳板是**按调用时刻**读
 *    `c->on_read` 的，所以"装一次、之后随便改"是对的，不需要跟着改回调重装。
 */

#include "capi/uvcpp_c_net.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>

#include "capi/uvcpp_c_internal.h"

// 两个枚举的编号必须逐值对上：C 侧把 `read_result.event` 当整数透传，错位就是
// "事件类型静默换了意思"这种最难查的错。
static_assert(static_cast<int>(uvcpp::net_read_event::DATA) == UVCPP_C_READ_DATA,
              "net_read_event::DATA 必须与 UVCPP_C_READ_DATA 同值");
static_assert(
    static_cast<int>(uvcpp::net_read_event::PEER_CLOSED) ==
        UVCPP_C_READ_PEER_CLOSED,
    "net_read_event::PEER_CLOSED 必须与 UVCPP_C_READ_PEER_CLOSED 同值");
static_assert(
    static_cast<int>(uvcpp::net_read_event::READ_ERROR) == UVCPP_C_READ_ERROR,
    "net_read_event::READ_ERROR 必须与 UVCPP_C_READ_ERROR 同值");

using uvcpp_c_detail::alive;
// `copy_out()` 从 `1.4.3` 起收在 `uvcpp_c_detail` 里（webapp 面也要用它，
// 而"返回长度 + 写不写结尾 NUL"这处算术抄成两份就是两份会各自漂的算术）。
using uvcpp_c_detail::copy_out;

// ---------------------------------------------------------------------------
// 句柄
// ---------------------------------------------------------------------------

/**
 * @brief 客户端句柄（不透明类型的真身）。
 *
 * 里面**只有标量、函数指针和一个指针**，没有任何带析构的成员 —— 这是刻意的：
 * `delete` 一个包装器因此不可能跑用户代码，也不可能抛异常。
 */
struct uvcpp_c_tcp_client {
  uvcpp_c_detail::handle_head head;  ///< 必须在偏移 0（`alive()` 按它读）

  uvcpp::uvcpp_tcp_client* cli;  ///< nullptr = 已经回收（句柄即将失效）
  int owns;      ///< 1 = 本层 `new` 的（`free()` 会删它）；0 = 服务端交出来的
  int reaped;    ///< 1 = 已经从活句柄表里摘掉
  int cb_depth;  ///< 正在跑的回调层数（> 0 时不许删）
  int read_armed;  ///< 1 = C++ 那侧的 `read_start_events()` 已经装上

  uvcpp_c_tcp_server* owner;  ///< 非 NULL = 服务端交出来的

  uvcpp_c_read_cb on_read;
  void* on_read_user_data;
  uvcpp_c_notify_cb on_close;
  void* on_close_user_data;
};

struct uvcpp_c_tcp_server {
  uvcpp_c_detail::handle_head head;

  uvcpp::uvcpp_tcp_server* srv;
  int cb_depth;

  uvcpp_c_connect_cb on_connection;
  void* on_connection_user_data;
  uvcpp_c_read_cb on_read;
  void* on_read_user_data;

  /// C++ 连接对象 → 包装器。服务端交出来的连接在这里有且只有一个包装器：
  /// 用户在 `on_connection` 里拿到的句柄，和之后读回调里拿到的**是同一个**。
  std::unordered_map<uvcpp::uvcpp_tcp_client*, uvcpp_c_tcp_client*> live;

  /// 回调期间被回收的包装器。见文件开头第 2 条。
  std::vector<uvcpp_c_tcp_client*> graveyard;
};

static_assert(offsetof(uvcpp_c_tcp_client, head) == 0,
              "handle_head 必须在偏移 0：alive() 会按它读任意句柄");
static_assert(offsetof(uvcpp_c_tcp_server, head) == 0,
              "handle_head 必须在偏移 0");

namespace {

/// @brief 回调期间挂上的"不许删"计数（构造 +1，析构 -1）。
class CallbackScope {
 public:
  CallbackScope(uvcpp_c_tcp_server* s, uvcpp_c_tcp_client* c) : s_(s), c_(c) {
    if (c_ != nullptr) ++c_->cb_depth;
    if (s_ != nullptr) ++s_->cb_depth;
  }
  ~CallbackScope() {
    if (c_ != nullptr) --c_->cb_depth;
    if (s_ != nullptr) {
      --s_->cb_depth;
      drain();
    }
  }
  CallbackScope(const CallbackScope&) = delete;
  CallbackScope& operator=(const CallbackScope&) = delete;

 private:
  void drain();

  uvcpp_c_tcp_server* s_;
  uvcpp_c_tcp_client* c_;
};

void reap_client(uvcpp_c_tcp_server* s, uvcpp_c_tcp_client* w);

void CallbackScope::drain() {
  if (s_ == nullptr || s_->graveyard.empty()) return;
  for (size_t i = 0; i < s_->graveyard.size();) {
    uvcpp_c_tcp_client* w = s_->graveyard[i];
    if (w->cb_depth != 0) {  // 它自己还在某一层回调里，等它出栈时再收
      ++i;
      continue;
    }
    s_->graveyard.erase(s_->graveyard.begin() + static_cast<long>(i));
    delete w;
  }
}

/**
 * @brief 把一次 C++ 读事件摊平成 C 的结构体。
 *
 * 逐字段赋值而不是"整块 memcpy"：两边的布局**没有**互相承诺（C 侧用 `int`
 * 表示 `bool` / `enum class` 就是为了让 C# 侧不必认识 C++ 的枚举底层类型）。
 */
void fill_result(const uvcpp::net_read_result& r, uvcpp_c_read_result* out) {
  out->event = static_cast<int>(r.event);
  out->data = r.data;
  out->size = r.size;
  out->error = r.error;
  out->fin = r.fin ? 1 : 0;
}

/**
 * @brief 回收一条服务端交出来的连接：摘表 + 毒化 + （能删就）删包装器。
 *
 * 由关闭观察者触发，所以此刻 C++ 对象**还活着**（`add_close_observer` 的契约
 * 是"观察者跑的时候客户端对象还活着，但连接确实已经关了"）—— 我们在这里做的事
 * 只有一件：让 C 侧的句柄从此失效。
 */
void reap_client(uvcpp_c_tcp_server* s, uvcpp_c_tcp_client* w) {
  if (w == nullptr || w->reaped) return;
  w->reaped = 1;
  uvcpp_c_detail::unregister_head(w);

  uvcpp::uvcpp_tcp_client* cli = w->cli;
  w->cli = nullptr;
  if (s == nullptr) {
    delete w;
    return;
  }
  if (cli != nullptr) s->live.erase(cli);
  // 正在任何一层回调里（包括本层）就先放坟场，出栈时收。
  if (s->cb_depth > 0 || w->cb_depth > 0) {
    s->graveyard.push_back(w);
    return;
  }
  delete w;
}

/** @brief 找（或造）一条 C++ 连接对应的包装器。 */
uvcpp_c_tcp_client* wrap_client(uvcpp_c_tcp_server* s,
                                uvcpp::uvcpp_tcp_client* cli) {
  if (s == nullptr || cli == nullptr) return nullptr;

  auto it = s->live.find(cli);
  if (it != s->live.end()) return it->second;

  auto* w = new (std::nothrow) uvcpp_c_tcp_client();
  if (w == nullptr) return nullptr;
  w->cli = cli;
  w->owns = 0;
  w->owner = s;
  uvcpp_c_detail::register_head(w, UVCPP_C_MAGIC_TCP_CLIENT);
  // 这个函数跑在**服务这条连接的那条循环线程**上（连接回调与读回调都在它上面），
  // 所以"此刻的线程"就是它的循环线程 —— 多循环扇出时每个包装器各记各的，
  // 不需要问服务端要（服务端自己那份记录在多循环下是不可信的，见 `_run`）。
  uvcpp_c_detail::arm_loop_thread(&w->head);

  s->live[cli] = w;
  // 「连接关了」的那一刻回收。这条注册是承重的：没有它，框架 delete 掉 C++
  // 对象之后，C 侧手里那个句柄就是一个悬空指针。
  cli->add_close_observer([s, w]() { reap_client(s, w); });
  return w;
}

/**
 * @brief 连接已经建立、而用户又关心读/关闭时，把 C++ 那侧的读回调补上。
 *
 * 幂等：`read_armed` 挡住重复安装（重复装会返回 `UV_EALREADY`）。
 * 返回 0 = 装好了或者本来不用装；非 0 = 装的时候失败了（原样透传）。
 */
int maybe_arm_read(uvcpp_c_tcp_client* c) {
  if (c == nullptr || c->cli == nullptr) return UVCPP_C_E_STALE;
  if (c->read_armed) return 0;
  if (c->on_read == nullptr && c->on_close == nullptr) return 0;
  if (!c->cli->has_status(uvcpp::TCP_CLIENT_CONNECTED)) return 0;

  const int rc = c->cli->read_start_events(
      [c](uvcpp::uvcpp_tcp_client&, const uvcpp::net_read_result& r) {
        uvcpp_c_read_cb cb = c->on_read;
        if (cb == nullptr) return;  // 只关心 on_close 时也是这条路
        void* ud = c->on_read_user_data;
        uvcpp_c_read_result out;
        fill_result(r, &out);
        CallbackScope guard(c->owner, c);
        cb(ud, c, &out);
      });
  if (rc == 0) c->read_armed = 1;
  return rc;
}

}  // namespace

// ---------------------------------------------------------------------------
// 客户端
// ---------------------------------------------------------------------------

extern "C" UVCPP_C_API uvcpp_c_tcp_client* uvcpp_c_tcp_client_new(void) {
  UVCPP_C_TRY
    // 先造 C++ 对象（它的构造函数会建一条循环，可能抛），再造包装器 ——
    // 顺序反过来的话，构造函数抛出去时包装器已经分配了，而唯一的句柄在
    // 栈上那个正在展开的帧里，没人能释放它。
    std::unique_ptr<uvcpp::uvcpp_tcp_client> cli(
        new uvcpp::uvcpp_tcp_client());

    auto* raw = new (std::nothrow) uvcpp_c_tcp_client();
    if (raw == nullptr) return nullptr;
    std::unique_ptr<uvcpp_c_tcp_client> c(raw);

    c->cli = cli.get();
    c->owns = 1;
    uvcpp_c_detail::register_head(c.get(), UVCPP_C_MAGIC_TCP_CLIENT);
    cli.release();  // 所有权交给包装器（`free()` 里 delete）
    return c.release();
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_free(uvcpp_c_tcp_client* c) {
  UVCPP_C_TRY
    if (c == nullptr) return UVCPP_C_E_STALE;
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (!c->owns) return UVCPP_C_E_STATE;      // 服务端交出来的，归框架
    if (c->cb_depth > 0) return UVCPP_C_E_STATE;  // 回调里不许删（见头）

    uvcpp_c_detail::unregister_head(c);
    uvcpp::uvcpp_tcp_client* cli = c->cli;
    c->cli = nullptr;
    delete cli;  // 连带它自带的那条循环
    delete c;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_set_events(
    uvcpp_c_tcp_client* c, const uvcpp_c_tcp_client_events* table) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;

    c->on_read = nullptr;
    c->on_read_user_data = nullptr;
    c->on_close = nullptr;
    c->on_close_user_data = nullptr;

    if (table != nullptr) {
      if (!uvcpp_c_detail::table_size_ok(table->size)) {
        uvcpp_c_detail::set_last_error(
            "uvcpp_c_tcp_client_events.size 连第一格都没盖住");
        return UVCPP_C_E_INVALID_ARG;
      }
      if (uvcpp_c_detail::field_present(
              table->size, &uvcpp_c_tcp_client_events::on_read)) {
        c->on_read = table->on_read;
      }
      if (uvcpp_c_detail::field_present(
              table->size, &uvcpp_c_tcp_client_events::on_read_user_data)) {
        c->on_read_user_data = table->on_read_user_data;
      }
      if (uvcpp_c_detail::field_present(
              table->size, &uvcpp_c_tcp_client_events::on_close)) {
        c->on_close = table->on_close;
      }
      if (uvcpp_c_detail::field_present(
              table->size, &uvcpp_c_tcp_client_events::on_close_user_data)) {
        c->on_close_user_data = table->on_close_user_data;
      }
    }

    // C++ 那侧的 on_close 是一个**普通槽**，不需要连接存在，随时可装。
    uvcpp::uvcpp_tcp_client* cli = c->cli;
    if (c->on_close != nullptr) {
      cli->set_on_close([c]() {
        uvcpp_c_notify_cb cb = c->on_close;
        if (cb == nullptr) return;
        void* ud = c->on_close_user_data;
        CallbackScope guard(c->owner, c);
        cb(ud);
      });
    } else {
      cli->clear_on_close();
    }

    // 读回调只在连接已经建立时装得上去（见文件开头第 3 条）。
    const int rc = maybe_arm_read(c);
    if (rc != 0) return rc;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_connect(uvcpp_c_tcp_client* c,
                                                      const char* ip, int port,
                                                      uvcpp_c_status_cb cb,
                                                      void* user_data) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    if (ip == nullptr || ip[0] == '\0') return UVCPP_C_E_INVALID_ARG;
    if (port < 1 || port > 65535) return UVCPP_C_E_INVALID_ARG;
    if (cb == nullptr) {
      uvcpp_c_detail::set_last_error(
          "connect() 必须给完成回调；要同步等请用 connect_wait()");
      return UVCPP_C_E_INVALID_ARG;
    }

    // 跳板只捕获**两个标量**（函数指针 + user_data），不捕获任何句柄 ——
    // 于是这次回调里发生什么删除都不会踩到跳板自己。
    return c->cli->connect(ip, port, [c, cb, user_data](int status) {
      if (status == 0) maybe_arm_read(c);  // 连上了：现在装读回调才合法
      cb(user_data, status);
    });
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_connect_wait(
    uvcpp_c_tcp_client* c, const char* ip, int port, int timeout_ms) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    if (ip == nullptr || ip[0] == '\0') return UVCPP_C_E_INVALID_ARG;
    if (port < 1 || port > 65535) return UVCPP_C_E_INVALID_ARG;

    const int rc = c->cli->connect_wait(ip, port, timeout_ms);
    if (rc != 0) return rc;
    // 同步路径同样要补读回调：用户完全可能"先 set_events 再 connect_wait"。
    const int arm = maybe_arm_read(c);
    return arm == 0 ? UVCPP_C_OK : arm;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_write(uvcpp_c_tcp_client* c,
                                                    const void* data,
                                                    size_t len,
                                                    uvcpp_c_status_cb cb,
                                                    void* user_data) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    if (data == nullptr && len != 0) return UVCPP_C_E_INVALID_ARG;

    if (cb == nullptr) {
      // C++ 那侧 `cb == nullptr` 表示"同步写"，那是另一条路（`write_wait`）。
      // 混着解释会变成"一次不经意的阻塞写"，所以这里要求必须给回调。
      uvcpp_c_detail::set_last_error(
          "write() 必须给完成回调；要同步等请用 write_wait()");
      return UVCPP_C_E_INVALID_ARG;
    }
    return c->cli->write(static_cast<const char*>(data), len,
                         [cb, user_data](int status) { cb(user_data, status); });
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_write_wait(uvcpp_c_tcp_client* c,
                                                         const void* data,
                                                         size_t len,
                                                         int timeout_ms) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    if (data == nullptr && len != 0) return UVCPP_C_E_INVALID_ARG;
    return c->cli->write_wait(static_cast<const char*>(data), len, timeout_ms);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_read_pause(
    uvcpp_c_tcp_client* c) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    return c->cli->read_pause();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_read_resume(
    uvcpp_c_tcp_client* c) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    return c->cli->read_resume();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_read_stop(uvcpp_c_tcp_client* c) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    const int rc = c->cli->read_stop();
    // 停读把 C++ 那侧的槽也清了，所以下次 `set_events()` 得重新装一遍 ——
    // 把这个标志清掉，让 `maybe_arm_read()` 认得出来。
    c->read_armed = 0;
    return rc;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_close(uvcpp_c_tcp_client* c) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    return c->cli->close();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_run(uvcpp_c_tcp_client* c) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    // 服务端交出来的连接跑在**服务端**那条循环上，它自己没有 loop 可跑。
    if (!c->owns) return UVCPP_C_E_STATE;
    uvcpp_c_detail::arm_loop_thread(&c->head);
    return c->cli->run(UV_RUN_DEFAULT);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_stop(uvcpp_c_tcp_client* c) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    if (!c->owns) return UVCPP_C_E_STATE;
    c->cli->stop();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_is_connected(
    uvcpp_c_tcp_client* c) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    // **不能用 `uvcpp_tcp_client::has_status(TCP_CLIENT_CONNECTED)` 直接答。**
    // 那一层的 `set_status()` 是**按位或**（`status_ |= flags`，见
    // `uvcpp_tcp_client.cpp` 里 `set_status` 的实现），所以关闭路径上
    // `set_status(TCP_CLIENT_CLOSED)` 之后 `CONNECTED` 这一位**还在**——
    // 一条已经关掉的连接会一直自称"已连上"。
    //
    // 对 C++ 侧那是有意的分层（调用方自己看 `CLOSED`），但对 C 面不行：
    // `uvcpp_c_tcp_client_is_connected()` 是 P/Invoke 那边唯一会被当成
    // "能不能写"来用的那个问句，答错就是一次注定失败（或写到别的连接上）的写。
    // 所以这里按"活着且能收发"来答：连上了、且不在关、也没关。
    const bool up = c->cli->has_status(uvcpp::TCP_CLIENT_CONNECTED) &&
                    !c->cli->has_status(uvcpp::TCP_CLIENT_CLOSING) &&
                    !c->cli->has_status(uvcpp::TCP_CLIENT_CLOSED);
    return up ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_last_error(uvcpp_c_tcp_client* c) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    return c->cli->get_last_error();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_is_tls(uvcpp_c_tcp_client* c) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    // 答 0 是**真话**，不是占位：这一层一个给 C 的 TLS 入口都没有（见头里的
    // "不提供"），所以从这里能拿到的每一条连接都真的没装 TLS。
    //
    // 刻意**不**写成 `c->cli->is_tls()`：那个成员整个被 `#if UVCPP_OPENSSL_ENABLE`
    // 围住，而本项目的 capi 格**故意不带 OpenSSL**（Linux / macOS / MSVC 三格，
    // 理由写在各格的注释里）。调它会让"C 面这一批能不能编出来"取决于一个 C 面
    // 根本够不着的特性 —— 而那三格正是本层唯一跑纯 C 用例的地方。等 ssl 的 C 面
    // 落地（后续批次），这里才该换成真正的那一句。
    return 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_client_alpn_selected(
    uvcpp_c_tcp_client* c, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_TCP_CLIENT)) return UVCPP_C_E_STALE;
    if (c->cli == nullptr) return UVCPP_C_E_STALE;
    // 同 `_is_tls()`：本批没有给 C 的 TLS 入口，协商出来的名字因此**恒为空串**。
    // 上一版这里调的是 `c->cli->tls_alpn_selected()`，而那个成员只在
    // `UVCPP_OPENSSL_ENABLE` 下存在 —— 于是就有一条腿（capi 格）编不过，见头里
    // 那条"不提供"的注释。
    //
    // 空串也走同一个 `copy_out()`：这一层"调用方给缓冲区"的约定只该有一个实现，
    // 这里另写一句 `buf[0] = '\0'` 就是第二处会写结尾 NUL 的地方。
    //
    // 线程检查一并去掉：它当初在这里的理由是 `tls_alpn_selected()` 返回引用、
    // 握手线程可能正在改写那块内存；现在既不读也不返回，就没有可被别的线程
    // 改坏的东西 —— 留一个查不出任何问题的检查只会让行为在两种构建里不一样。
    return copy_out(std::string(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ---------------------------------------------------------------------------
// 服务端
// ---------------------------------------------------------------------------

extern "C" UVCPP_C_API uvcpp_c_tcp_server* uvcpp_c_tcp_server_new(void) {
  UVCPP_C_TRY
    std::unique_ptr<uvcpp::uvcpp_tcp_server> srv(
        new uvcpp::uvcpp_tcp_server());

    auto* raw = new (std::nothrow) uvcpp_c_tcp_server();
    if (raw == nullptr) return nullptr;
    std::unique_ptr<uvcpp_c_tcp_server> s(raw);

    s->srv = srv.get();
    uvcpp_c_detail::register_head(s.get(), UVCPP_C_MAGIC_TCP_SERVER);
    srv.release();
    return s.release();
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_free(uvcpp_c_tcp_server* s) {
  UVCPP_C_TRY
    if (s == nullptr) return UVCPP_C_E_STALE;
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->cb_depth > 0) return UVCPP_C_E_STATE;  // 回调里不许删

    uvcpp_c_detail::unregister_head(s);
    uvcpp::uvcpp_tcp_server* srv = s->srv;
    s->srv = nullptr;
    // 析构会关监听、逐条 delete 连接、停工作线程。逐条 delete 时会跑上面挂的
    // 关闭观察者，于是 `s->live` 会被它们自己清干净 —— 此刻 `s` 还活着，
    // 观察者里那句 `s->live.erase(...)` 是安全的。这也是为什么 `s` 必须
    // **最后**才 delete。
    delete srv;

    for (auto& kv : s->live) {  // 兜底：万一有谁没走到观察者
      uvcpp_c_detail::unregister_head(kv.second);
      delete kv.second;
    }
    s->live.clear();
    for (auto* w : s->graveyard) delete w;
    s->graveyard.clear();
    delete s;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_set_events(
    uvcpp_c_tcp_server* s, const uvcpp_c_tcp_server_events* table) {
  UVCPP_C_TRY
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->srv == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&s->head)) return UVCPP_C_E_WRONG_THREAD;

    s->on_connection = nullptr;
    s->on_connection_user_data = nullptr;
    s->on_read = nullptr;
    s->on_read_user_data = nullptr;

    if (table != nullptr) {
      if (!uvcpp_c_detail::table_size_ok(table->size)) {
        uvcpp_c_detail::set_last_error(
            "uvcpp_c_tcp_server_events.size 连第一格都没盖住");
        return UVCPP_C_E_INVALID_ARG;
      }
      if (uvcpp_c_detail::field_present(
              table->size, &uvcpp_c_tcp_server_events::on_connection)) {
        s->on_connection = table->on_connection;
      }
      if (uvcpp_c_detail::field_present(
              table->size,
              &uvcpp_c_tcp_server_events::on_connection_user_data)) {
        s->on_connection_user_data = table->on_connection_user_data;
      }
      if (uvcpp_c_detail::field_present(
              table->size, &uvcpp_c_tcp_server_events::on_read)) {
        s->on_read = table->on_read;
      }
      if (uvcpp_c_detail::field_present(
              table->size, &uvcpp_c_tcp_server_events::on_read_user_data)) {
        s->on_read_user_data = table->on_read_user_data;
      }
    }

    // 所有连接**共用**的那一份读回调：对应 C++ 的 `set_read_callback()`。
    // 它是**兜底** —— 用户在 `on_connection` 里对某条连接单独 `set_events`（装
    // 上 `on_read`）会把这条顶掉，这正是 C++ 侧的语义（见 `read_start` 里的
    // `release_auto_read`）。
    if (s->on_read != nullptr) {
      s->srv->set_read_callback([s](uvcpp::uvcpp_tcp_client& cli,
                                    const uvcpp::net_read_result& r) {
        uvcpp_c_read_cb cb = s->on_read;
        if (cb == nullptr) return;
        void* ud = s->on_read_user_data;
        uvcpp_c_read_result out;
        fill_result(r, &out);
        // 懒包装：这条连接可能从来没进过 `on_connection`（或者用户压根没装
        // 那个回调），但读回调仍然要给它一个句柄。
        uvcpp_c_tcp_client* w = wrap_client(s, &cli);
        if (w == nullptr) return;
        CallbackScope guard(s, w);
        cb(ud, w, &out);
      });
    } else {
      s->srv->clear_read_callback();
    }
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_bind(uvcpp_c_tcp_server* s,
                                                   const char* ip, int port) {
  UVCPP_C_TRY
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->srv == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&s->head)) return UVCPP_C_E_WRONG_THREAD;
    if (ip == nullptr || ip[0] == '\0') return UVCPP_C_E_INVALID_ARG;
    if (port < 0 || port > 65535) return UVCPP_C_E_INVALID_ARG;
    return s->srv->bind(ip, port);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_local_port(uvcpp_c_tcp_server* s) {
  UVCPP_C_TRY
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->srv == nullptr) return UVCPP_C_E_STALE;
    uvcpp::uvcpp_tcp* tcp = s->srv->get_tcp();
    if (tcp == nullptr) return UVCPP_C_E_STATE;

    // `uvcpp_tcp_server` 自己没有 `getsockname()`（`uvcpp_tcp` 才有），所以
    // 这里先问底层的 `uvcpp_tcp`，再按地址族取端口 —— 服务端可能绑在 IPv4 上
    // （`0.0.0.0`）也可能绑在 IPv6 上（`::`），两种的端口字段不在同一个偏移。
    struct sockaddr_storage ss;
    std::memset(&ss, 0, sizeof(ss));
    int len = static_cast<int>(sizeof(ss));
    if (tcp->getsockname(reinterpret_cast<struct sockaddr*>(&ss), &len) != 0) {
      return UVCPP_C_E_STATE;
    }
    if (ss.ss_family == AF_INET) {
      const struct sockaddr_in* in =
          reinterpret_cast<const struct sockaddr_in*>(&ss);
      return static_cast<int>(ntohs(in->sin_port));
    }
    if (ss.ss_family == AF_INET6) {
      const struct sockaddr_in6* in6 =
          reinterpret_cast<const struct sockaddr_in6*>(&ss);
      return static_cast<int>(ntohs(in6->sin6_port));
    }
    return UVCPP_C_E_STATE;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_listen(uvcpp_c_tcp_server* s,
                                                     int backlog) {
  UVCPP_C_TRY
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->srv == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&s->head)) return UVCPP_C_E_WRONG_THREAD;

    const int effective = backlog > 0 ? backlog : 128;
    return s->srv->listen(
        [s](uvcpp::uvcpp_tcp_client* cli) {
          if (cli == nullptr) return;
          // 先包装：这样即使用户没给 `on_connection`，这条连接也已经有了
          // C 侧句柄，后续读回调（`on_read`）能直接找到它。
          uvcpp_c_tcp_client* w = wrap_client(s, cli);
          uvcpp_c_connect_cb cb = s->on_connection;
          if (cb == nullptr || w == nullptr) return;
          void* ud = s->on_connection_user_data;
          CallbackScope guard(s, w);
          cb(ud, w);
        },
        effective);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_set_loops(uvcpp_c_tcp_server* s,
                                                        int n) {
  UVCPP_C_TRY
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->srv == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&s->head)) return UVCPP_C_E_WRONG_THREAD;
    if (n < 1) return UVCPP_C_E_INVALID_ARG;
    return s->srv->set_loops(n);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_loop_count(uvcpp_c_tcp_server* s) {
  UVCPP_C_TRY
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->srv == nullptr) return UVCPP_C_E_STALE;
    return s->srv->loop_count();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_run(uvcpp_c_tcp_server* s) {
  UVCPP_C_TRY
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->srv == nullptr) return UVCPP_C_E_STALE;
    // 线程检查只在**单循环**时记：多循环扇出下回调跑在 n 条不同的线程上，
    // 记下其中一条只会让另外 n-1 条上的合法调用被误判成 `WRONG_THREAD`。
    // 那时候"同一句柄只在它那条线程上用"这条纪律仍然成立，只是要靠调用方守
    // （头里 `set_loops` 那段写的就是这件事）。
    if (!s->srv->is_fanout()) uvcpp_c_detail::arm_loop_thread(&s->head);
    return s->srv->run(UV_RUN_DEFAULT);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_stop(uvcpp_c_tcp_server* s) {
  UVCPP_C_TRY
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->srv == nullptr) return UVCPP_C_E_STALE;
    s->srv->stop();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_client_count(
    uvcpp_c_tcp_server* s) {
  UVCPP_C_TRY
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->srv == nullptr) return UVCPP_C_E_STALE;
    // 这一条**要**查线程：它不是在读一个标量，而是在**遍历库自己那张活连接表**
    // —— 循环线程正在接受/断开连接时改的就是那张表，跨线程读它读到的不是
    // "稍旧的条数"，而是一次真的数据竞争。所以这里比读标量那几条严一档，
    // 从别的线程问就明确给 `UVCPP_C_E_WRONG_THREAD`（而不是给一个可能上溢的
    // 条数）。`tests/capi/capi_net_func.c` 里那条断言钉的就是这一条。
    if (!uvcpp_c_detail::thread_ok(&s->head)) return UVCPP_C_E_WRONG_THREAD;
    return static_cast<int>(s->srv->client_count());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_close_all_clients(
    uvcpp_c_tcp_server* s) {
  UVCPP_C_TRY
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->srv == nullptr) return UVCPP_C_E_STALE;
    return static_cast<int>(s->srv->close_all_clients());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_tcp_server_last_error(uvcpp_c_tcp_server* s) {
  UVCPP_C_TRY
    if (!alive(s, UVCPP_C_MAGIC_TCP_SERVER)) return UVCPP_C_E_STALE;
    if (s->srv == nullptr) return UVCPP_C_E_STALE;
    return s->srv->get_last_error();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}
